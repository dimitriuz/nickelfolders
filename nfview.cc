// Our own screen, built out of Nickel's own dialog chrome (N3Dialog) and
// Nickel's own tappable row widget (TouchLabel) -- see nfview.h and
// NOTES.md's "Task 8: touch input archaeology" for why this replaces the
// earlier AbstractController shim.
//
// THE SHORT VERSION of that archaeology, because it explains every choice
// below: Nickel does not deliver QMouseEvents. It reads the touch panel
// itself, injects QTouchEvents, and recognises its own gestures through six
// custom QGestureRecognizer subclasses. A bare QWidget/QPushButton needs
// THREE things to receive a tap -- a grabGesture() call against a
// registration-time token, an event() override that routes touch/gesture
// events (QWidget::event() does neither), and RTTI-based GestureDelegate
// dispatch -- and the earlier shim's QPushButton, a stock Qt widget, had
// none of them: it rendered correctly and never once received a tap. Rather
// than hand-build any of that (or hand-build the AbstractController-shaped
// object the previous rung needed just to get onto the window stack at
// all), this route uses Nickel's OWN widgets, which already do it:
// TouchLabel self-registers for gestures in its own constructor, and
// N3Dialog is the screen chrome Nickel's own library search/settings/etc.
// dialogs use. Nothing here fabricates a vtable, a cross-cast, or RTTI for
// a class of our own -- every Nickel type stays opaque (typedef void +
// explicit call signature, nfnickel.h), the same discipline as every other
// libnickel entry point in this project.
//
// Prior art, read but not disassembled (both MIT): NickelHardcover
// (codeberg.org/StrayRose/NickelHardcover, HEAD 2026-07-21) is where the
// getDialog/pushView shape and the `true` argument to getDialog come from --
// its own comment does not explain that bool either, and neither does this
// one; it is copied working behaviour, not an understood value. NickelMenu
// (pgaskin/NickelMenu) is where the hidden-QPushButton signal-adaptor trick
// (below) comes from, for reaching a lambda without this project's having
// any moc step of its own.
//
// THIS FILE used to build a deliberately trivial, hardcoded three-row
// screen, to prove taps and back navigation on hardware before anything
// real rode on this route -- that milestone is done (task-dialog-view-
// report.md) and is superseded here: this is the payoff task, wiring
// nflist.h/nffmt.h's pure, fully host-tested listing pipeline
// (nf_build_listing) to this screen for the first time. See nfview.h for
// the navigation model.

#include "nfview.h"
#include "nfnickel.h"
#include "nflist.h"

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <linux/limits.h> // PATH_MAX -- same header nfnickel.cc's own nf_watch_dir uses
#include <stdio.h>         // snprintf
#include <stdlib.h>        // calloc -- see the row-allocation comment below for why not ::operator new

#include <NickelHook.h>

// --- casting an opaque Nickel pointer to a REAL, linked Qt base class -----
//
// TouchLabel (via FontSizeAdjustingLabel) inherits QLabel, and N3Dialog
// (via QDialog) inherits QWidget/QObject -- NOTES.md. Both chains are
// ordinary Qt single inheritance (no virtual bases ahead of QObject in any
// Qt widget hierarchy), so a TouchLabel* or N3Dialog* IS, bit for bit, a
// valid pointer to that base subobject -- the same "no cast arithmetic
// needed" property nfnickel.h already documents for
// QuickAccessLibraryController's own base. Calling QLabel::setText,
// QWidget::addWidget-compatible use, or QObject::setObjectName/connect
// through such a cast is NOT a redeclaration of anything Nickel's own
// compiler generated for TouchLabel/N3Dialog specifically -- it is calling
// an ordinary, ABI-stable, already-linked Qt5Widgets function (this
// project already links Qt5Widgets, Makefile) on a subobject whose
// position is guaranteed by Qt's own inheritance conventions, not by
// anything measured about Nickel's internals. What stays strictly opaque
// is everything Nickel-specific: TouchLabel's own constructor, N3Dialog's
// own setTitle/enableBackButton/setContent/getDialog, all resolved and
// called through nfnickel.h's usual function pointers, never assumed or
// redeclared.

// --- browser state ----------------------------------------------------
//
// Root of the tree this screen shows -- NOTES.md: content.ContentID is
// file:///mnt/onboard/<relative path> verbatim, so this is also the prefix
// every ContentID this file builds starts with.
#define NF_ROOT "/mnt/onboard"

// The largest directory measured on the reference card is 27 entries
// (NOTES.md, the Fullmetal Alchemist volume run). Whether a plain
// QVBoxLayout with no scroll area actually fits that many TouchLabel rows
// on this panel's real pixel height is UNTESTED -- this cap is a
// deliberately conservative judgement call, not a measurement, so that a
// directory this large shows a visible "...and N more" line instead of
// silently overflowing off the bottom of the screen. Real scrolling (a
// QScrollArea, or a windowed view into `rows`) is a follow-up, not
// attempted here -- see the task report's device checklist for what
// confirming or replacing this number needs.
#define NF_MAX_VISIBLE_ROWS 15

// Two independent pieces of file-scope, mutable browser state, and BOTH are
// POD for the same load-bearing reason nf_watch_targets/nf_watch_dir
// (nfnickel.cc) are: a file-scope object with a non-trivial (dynamically
// initialised) constructor runs from this translation unit's own
// _GLOBAL__sub_I, whose ordering relative to NickelHook's nh_init/nf_init
// is NOT guaranteed -- a QByteArray in exactly this position previously
// segfaulted Nickel on every boot (nfnickel.cc's own comment has the full
// account). A `void*` and a `char[]` are zero-initialised by .bss alone,
// with no constructor to race, so neither can ever be the cause of that
// failure mode. `nm libnfolders.so | grep GLOBAL__sub_I` must stay empty --
// see the task report for the check.
//
// nf_browser_active_dialog: tracks the one live dialog (CLAUDE.md's task
// brief, Part 2). A second `touch /tmp/nfolders-native` while a screen was
// already up used to leak a dialog and leave BACK landing on a stale
// duplicate -- nf_browser_show, below, now RE-PUSHES this same dialog
// instead of building a second one when it is non-NULL (review finding
// L5: refusing outright left a dead end if Nickel's own navigation ever
// left the dialog alive but off-screen, e.g. tapping Home while browsing,
// since destroyed() -- the only thing that clears this -- does not fire
// for mere abandonment). Cleared by the dialog's own destroyed() signal,
// so it self-heals no matter which of this file's several pop paths (or,
// in principle, some path outside this file's control) is what actually
// tears the dialog down -- a later nf_browser_show then builds a fresh
// one rather than re-pushing a dead pointer.
static void *nf_browser_active_dialog = NULL;

// The one directory nf_browser_go (below) most recently rebuilt content
// for -- i.e. what BACK steps up from. Set at the top of every call to
// nf_browser_go, including the initial root call from nf_browser_show, so
// it is always valid by the time any row's tap handler or the BACK row can
// possibly fire (both are wired only after the content that reads this has
// already been built for the CURRENT directory, and nothing re-enters this
// file's own code from another thread -- every callback here runs on the
// GUI thread, same as nf_on_trigger_view that calls nf_browser_show).
static char nf_browser_cwd[PATH_MAX];

// --- construction ------------------------------------------------------

// Pops this screen and logs why. Shared by both of the ROOT-level exit
// paths (the guaranteed BACK row and N3Dialog's own backTapped() signal,
// both wired through nf_browser_back below) so a reader can see in one
// place that they really do the same thing, rather than auditing two
// near-duplicate lambdas for drift. `why` is always a string literal from
// the call site, never built here.
//
// popView DESTROYS the widget (setVisible(false), deleteLater(),
// stack->removeWidget() -- NOTES.md) -- including, eventually, the
// dialog's current content and everything under it. Safe from inside a
// signal handler on that same content because deleteLater() only POSTS the
// deletion for later in the event loop rather than deleting synchronously
// -- the standard Qt "a slot may schedule its own object's death" pattern,
// not a hazard specific to this call.
static void nf_pop_native_view(void *mwc, N3Dialog *dialog, char const *why) {
    nh_log("view: %s, popping", why);
    MainWindowController__popView(mwc, reinterpret_cast<QWidget*>(dialog));
}

// Implements nf_meta_fn (nflist.h) for the folder browser's own listing
// pass. Deliberately does NOT reach for Volume::getDbValues -- CLAUDE.md's
// task brief rules that out for this milestone (its calling convention is
// unestablished archaeology, and the ABI risk of guessing it wrong is
// exactly the class of mistake VolumeManager::getById's missing `this`
// once cost this project: a crashed Nickel, on the first device run,
// from code that compiled and linked cleanly -- NOTES.md). So this only
// ever answers "does a row exist for this file" (nf_row::hasRow), via
// nf_volume_exists (nfnickel.h, itself just getById + isValid, the same
// discipline nf_open_book_staged already uses) -- nf_row::percentRead and
// nf_row::finished are left at nf_build_listing's own defaults (-1,
// false), UNCHANGED here. Reading progress is a later task, not this one.
struct NFMetaCtx {
    QString dirPath; // the directory `name` (below) is relative to; ABSOLUTE, no trailing slash
    QString dbName;  // this device's own getById partition key -- nf_db_name(), read once per directory, not per file
};

static void nf_row_meta(void *ctx, QString const& name, nf_row *row) {
    NFMetaCtx const *c = static_cast<NFMetaCtx const*>(ctx);
    QString contentId = QStringLiteral("file://") + c->dirPath + QLatin1Char('/') + name;
    row->hasRow = nf_volume_exists(contentId, c->dbName);
}

// QDir::entryInfoList against ONE directory, never recursive -- the spec's
// own words, and also what keeps this a single, bounded syscall burst: no
// QDir/QFileInfo object here is held past this function returning, which
// is what CLAUDE.md's "never hold a file handle on /mnt/onboard for more
// than a few hundred milliseconds" needs -- this runs inside a Qt signal
// handler (a tap, or the initial trigger), one of the two windows CLAUDE.md
// names as safe.
//
// QDir::Hidden is passed so dot-directories/dot-files are actually IN this
// raw list -- without it Qt's own default filtering drops them before
// nf_build_listing (nflist.cc) ever sees them, which would make its own
// nf_is_hidden_dir dot-prefix check redundant in the case that matters
// (".kobo", ".adds" etc.) and silently untested in the case that doesn't
// (a dotfile that is NOT one v1 wants hidden -- there is none today, but
// nf_is_hidden_dir is the one place that decision belongs, not a QDir
// filter flag this file would otherwise be making unilaterally).
// QDir::NoSort: nf_sort_entries (nffmt.cc) is the real ordering, and asking
// Qt to sort first would just be discarded work.
static QVector<nf_entry> nf_browser_scan_dir(QString const &path) {
    QDir dir(path);
    QFileInfoList infos = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);

    QVector<nf_entry> entries;
    entries.reserve(infos.size());
    for (int i = 0; i < infos.size(); i++) {
        nf_entry e;
        e.name  = infos.at(i).fileName();
        e.isDir = infos.at(i).isDir();
        entries << e;
    }
    return entries;
}

static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path);

// Shared by the guaranteed BACK row and N3Dialog's own backTapped() signal
// -- same "one function, not two forks to audit for drift" reasoning as
// nf_pop_native_view, except this one does NOT always pop: which action it
// takes depends on nf_browser_cwd, read fresh on every call, so it is
// always asking "where am I NOW", never a value captured at some earlier
// row-build time.
static void nf_browser_back(void *mwc, N3Dialog *dialog) {
    QString cwd = QString::fromUtf8(nf_browser_cwd);
    if (cwd == QStringLiteral(NF_ROOT)) {
        nh_log("browser: BACK at the root -- leaving");
        nf_pop_native_view(mwc, dialog, "BACK at root");
        return;
    }

    int slash = cwd.lastIndexOf(QLatin1Char('/'));
    QString parent = (slash > 0) ? cwd.left(slash) : QStringLiteral(NF_ROOT);
    // A genuine path-boundary check, not a length check -- review finding
    // L2: a length-only floor (`parent.length() < strlen(NF_ROOT)`) passes
    // "/mnt/onboardX/a" straight through, since that string is LONGER than
    // NF_ROOT despite not being under it at all. `parent` must be NF_ROOT
    // itself, or begin with NF_ROOT followed by a real "/" (not just share
    // its characters), to count as still being inside the tree. Not
    // reachable from this file's own navigation today (every path
    // nf_browser_go is called with is either NF_ROOT or NF_ROOT + a real
    // child name it read off disk) -- this is a defensive floor for a
    // caller that changes later, and now the comment matches what the code
    // actually enforces.
    QString const rootPrefix = QStringLiteral(NF_ROOT) + QLatin1Char('/');
    if (parent != QStringLiteral(NF_ROOT) && !parent.startsWith(rootPrefix))
        parent = QStringLiteral(NF_ROOT);

    nh_log("browser: BACK -- up from '%s' to '%s'", qPrintable(cwd), qPrintable(parent));
    nf_browser_go(mwc, dialog, parent);
}

// Builds a fresh content widget (rows for `path`'s own directory listing)
// and swaps it into the ALREADY-EXISTING `dialog` via N3Dialog::setContent
// -- this is the whole navigation model (nfview.h): one N3Dialog for the
// lifetime of a browse session, rows rebuilt in place, never a second
// dialog pushed per level. setContent itself deleteLater()s whatever
// content was there before (nfnickel.h), so the previous screen's rows and
// their shim buttons are cleaned up by Qt, not by this function.
static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path) {
    // Recorded BEFORE anything below can fail, so BACK's own "where am I"
    // read is always this directory once this function has been entered --
    // matching every row/BACK handler being wired only after the listing
    // for THIS path has been built, never before.
    snprintf(nf_browser_cwd, sizeof nf_browser_cwd, "%s", qPrintable(path));

    QString const *db = nf_db_name();
    NFMetaCtx ctx;
    ctx.dirPath = path;
    ctx.dbName  = db ? *db : QString();

    QVector<nf_entry> raw = nf_browser_scan_dir(path);
    QVector<nf_row> rows;
    nf_build_listing(raw, &nf_row_meta, &ctx, &rows);
    int shown = qMin(rows.size(), static_cast<int>(NF_MAX_VISIBLE_ROWS));

    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    // Row 0: a GUARANTEED exit, independent of N3Dialog's own backTapped()
    // signal (wired once, in nf_browser_show, to this exact same
    // nf_browser_back) -- review finding I-3, carried over from the
    // trivial-screen milestone this replaces: getDialog wires the dialog's
    // X (closeTapped()) to a controller-stack call pushView never
    // populates, so the X does nothing on this route (see
    // N3Dialog__disableCloseButton below, which removes it). If
    // backTapped() ALSO failed to fire for any reason, this screen would
    // have no way off it short of a power cycle, on the owner's daily-use
    // device -- this row does not depend on N3Dialog's own signal at all,
    // so it is the one most worth trusting if anything else here is wrong.
    // Labelled unmistakably, placed first, rebuilt fresh on every call
    // (calloc'd here, not hoisted -- it is a child of `content`, which gets
    // deleteLater()'d wholesale on the next navigation, same as every real
    // listing row below).
    {
        // 132 bytes measured at TouchLabel's own construction call sites
        // (NOTES.md); 256 is this project's usual over-allocation margin
        // for a Nickel object whose own size we cannot ask. calloc, not
        // ::operator new: Qt eventually deletes this widget itself through
        // its own real vtable, and glibc's calloc/malloc and libstdc++'s
        // default operator new/delete share the same underlying allocator
        // -- the same assumption every TouchLabel allocation in this
        // project already ships on. Checked for NULL before use (CLAUDE.md
        // task brief, Part 2) -- calloc failing here is not fatal to the
        // rest of the screen, just to this one guaranteed-exit row, which
        // is exactly why it is worth logging loudly rather than silently
        // skipping.
        void *row = calloc(1, 256);
        if (row) {
            TouchLabel__ctor(row, content, 0);
            reinterpret_cast<QLabel*>(row)->setText(QStringLiteral("<< BACK"));

            // The signal-adaptor trick (NickelMenu, src/nickelmenu.cc): a
            // hidden QPushButton relays TouchLabel's own, real, old-style
            // tapped(bool) signal to a plain capturing lambda -- see this
            // file's header comment for the full derivation. Old-style
            // string connects are not compile-checked, so a failure here
            // is logged loudly rather than silently doing nothing.
            QPushButton *shim = new QPushButton(content);
            shim->setVisible(false);
            if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
                nh_log("browser: connecting the BACK row's tapped(bool) failed -- this row will silently do nothing (backTapped()/the back arrow is this screen's other, independent exit)");
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog] {
                nf_browser_back(mwc, dialog);
            });

            layout->addWidget(reinterpret_cast<QWidget*>(row));
        } else {
            nh_log("browser: calloc(1,256) failed for the BACK row -- this screen has no BACK row this time (backTapped()/the back arrow is still wired)");
        }
    }

    // Do NOT silently truncate (CLAUDE.md's task brief) -- a plain QLabel,
    // not a TouchLabel: this line is informational only, not a tap target,
    // so it needs none of TouchLabel's gesture machinery.
    //
    // Placed HERE -- immediately after the BACK row, ABOVE the listing rows
    // it is warning about -- not after them. Review finding L1:
    // NF_MAX_VISIBLE_ROWS exists because whether that many TouchLabel rows
    // actually fit this panel's real height is untested, and a notice
    // placed after the rows is exactly what a layout that overflows the
    // screen would clip first -- degrading exactly as silently as having
    // no cap at all. Placed first, it is pushed off screen last, if
    // anything is.
    if (rows.size() > shown) {
        QLabel *more = new QLabel(content);
        more->setText(QStringLiteral("...and %1 more (scrolling not implemented yet)").arg(rows.size() - shown));
        layout->addWidget(more);
    }

    for (int i = 0; i < shown; i++) {
        nf_row const &r = rows.at(i);

        void *row = calloc(1, 256);
        if (!row) {
            nh_log("browser: calloc(1,256) failed for row %d ('%s'), skipping it", i, qPrintable(r.name));
            continue;
        }
        TouchLabel__ctor(row, content, 0);

        // Labels are OURS -- nf_strip_common (nffmt.cc) already did the
        // work; this only adds a per-row suffix that nf_build_listing does
        // not itself carry an opinion about:
        //   - a folder gets a trailing "/", a plain, ASCII, e-ink-safe
        //     affordance that this row navigates rather than opens.
        //   - a file with NO library row gets its reason spelled out in
        //     the label TEXT itself, not left to colour/style alone: this
        //     panel gives four grey levels, and "slightly lighter" reads
        //     as "the same", not "different" (CLAUDE.md's task brief).
        //     The reference card's own example is exactly one row --
        //     Fullmetal Alchemist v26, a truncated file Nickel's own
        //     import rejected (NOTES.md) -- and it must render as clearly
        //     wrong, not silently vanish and leave a reader wondering
        //     where volume 26 went.
        QString label = r.label;
        if (r.isDir)
            label += QLatin1Char('/');
        else if (!r.hasRow)
            label += QStringLiteral("  [not in library]");
        reinterpret_cast<QLabel*>(row)->setText(label);

        if (!r.isDir && !r.hasRow) {
            // A SECONDARY visual cue, best-effort and UNVERIFIED on this
            // panel's own Qt/QStyle (Nickel's custom style may or may not
            // honour a plain stylesheet the way desktop Qt does) -- the
            // bracketed text above is what actually carries the meaning,
            // and is applied UNCONDITIONALLY (it is plain QLabel::setText,
            // above, not gated on this call succeeding), which is what
            // makes the signal survive regardless of what this line does.
            // Review finding L4: "costs nothing if it is a no-op" was not
            // established and has been removed -- setStyleSheet installs a
            // QStyleSheetStyle for this widget, which can change its
            // layout metrics relative to its neighbours, so it is not
            // necessarily inert even when its own visual effect does
            // nothing. Left in anyway because the label text does not
            // depend on it; confirming or dropping it is one of the
            // report's own device-checklist items.
            reinterpret_cast<QWidget*>(row)->setStyleSheet(QStringLiteral("font-style: italic; color: gray;"));
        }

        QPushButton *shim = new QPushButton(content);
        shim->setVisible(false);
        if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
            nh_log("browser: connecting row %d's ('%s') tapped(bool) failed -- taps on this row will silently do nothing", i, qPrintable(r.name));

        // Captured by VALUE, not by reference to `r`/`rows` -- `rows` is a
        // local QVector that goes out of scope when this function returns,
        // well before any of these lambdas can possibly run (they only run
        // off a LATER tap, i.e. a later, separate invocation of the Qt
        // event loop). QString is Qt's own implicitly-shared, refcounted
        // value type, so capturing one by value is cheap, not a copy of
        // the underlying character data.
        QString childPath = path + QLatin1Char('/') + r.name;
        QString rowName   = r.name;
        bool    isDir     = r.isDir;
        bool    hasRow    = r.hasRow;
        QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, childPath, rowName, isDir, hasRow] {
            if (isDir) {
                nh_log("browser: descending into '%s'", qPrintable(childPath));
                nf_browser_go(mwc, dialog, childPath);
            } else if (hasRow) {
                QString contentId = QStringLiteral("file://") + childPath;
                nh_log("browser: opening '%s'", qPrintable(contentId));
                nf_open_book(contentId);
            } else {
                // Spec's own words: log why and do nothing. No navigation,
                // no attempted open -- getById already said no book has
                // this ContentID (nf_row_meta, above), and calling
                // nf_open_book on it would just repeat that lookup for a
                // second, identical "no" this file already has the answer
                // to.
                nh_log("browser: '%s' has no library row -- ignoring the tap", qPrintable(rowName));
            }
        });

        layout->addWidget(reinterpret_cast<QWidget*>(row));
    }

    QString title = (path == QStringLiteral(NF_ROOT))
        ? QStringLiteral("NickelFolders")
        : QFileInfo(path).fileName();
    N3Dialog__setTitle(dialog, title);

    nh_log("browser: showing '%s' (%d row(s), %d shown)", qPrintable(path), rows.size(), shown);

    // Reparents `content` into the dialog's own layout and shows it;
    // deleteLater()s whatever content was there before (nfnickel.h) --
    // which is what actually tears down the PREVIOUS screen's rows and
    // shim buttons. Must be the last thing this function does with
    // `content`/`layout`/the rows just built: nothing here may be touched
    // again afterward.
    N3Dialog__setContent(dialog, content);
}

bool nf_browser_show(void) {
    if (!nf_native_view_resolve()) {
        nh_log("browser: a required libnickel symbol did not resolve, refusing");
        return false;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("browser: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    if (nf_browser_active_dialog) {
        // Review finding L5: the guard used to refuse outright here, and
        // its own destroyed() clear (below) covers outright destruction
        // but NOT abandonment -- Nickel's own navigation (tapping Home
        // while browsing is the obvious way) can leave our dialog alive
        // but off the window stack, with nothing in this file positioned
        // to notice. Refusing in that state would be a dead end: every
        // later trigger would do nothing at all until a reboot, with no
        // way back to the browser. Re-pushing the SAME dialog instead
        // turns that into a recovery -- nothing tore its content down, so
        // whatever directory it was last showing is still there. If the
        // dialog is genuinely still the current view, pushView's own
        // early-return-on-already-current-widget check (nfnickel.h) makes
        // this a harmless no-op rather than a double push.
        nh_log("browser: a screen already exists -- re-pushing it rather than building a new one");
        MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(nf_browser_active_dialog));
        return true;
    }

    // A placeholder, ONLY to satisfy getDialog's signature -- nf_browser_go
    // (below) replaces it via setContent with the real root listing before
    // this function returns, so it is never shown and never has a chance
    // to be tapped. This is the restructuring CLAUDE.md's task brief (Part
    // 2) asks for: the EXPENSIVE allocation (a whole directory's worth of
    // TouchLabel rows and shim buttons) happens only after getDialog has
    // already succeeded, in nf_browser_go -- the trivial-screen milestone
    // this replaces allocated all of that BEFORE checking getDialog's
    // return, which leaked everything on a null return. The one allocation
    // that still happens before the check is this single, row-less,
    // unparented QWidget, freed explicitly below if getDialog fails.
    QWidget *placeholder = new QWidget();
    N3Dialog *dialog = N3DialogFactory__getDialog(placeholder, true);
    if (!dialog) {
        nh_log("browser: N3DialogFactory::getDialog returned null, refusing");
        delete placeholder;
        return false;
    }

    nf_browser_active_dialog = dialog;
    // QObject::destroyed() is Qt's OWN compiled signal (QObject's, not
    // ours), emitted synchronously from ~QObject regardless of WHY the
    // object is being destroyed -- so this clears the guard however this
    // dialog eventually goes away, not only through this file's own
    // popView call sites. New-style connect needs no moc step of ours for
    // the same reason the TouchLabel/N3Dialog SIGNAL() connects elsewhere
    // in this file don't: QObject's metaobject is Qt5Core's own, already
    // compiled and linked.
    QObject::connect(reinterpret_cast<QObject*>(dialog), &QObject::destroyed, [] {
        nh_log("browser: the dialog was destroyed -- clearing the re-trigger guard");
        nf_browser_active_dialog = NULL;
    });

    // Restores the ndbCurrentView oracle (NOTES.md's "cosmetic" correction:
    // NDB::ndbCurrentView() reads MainWindowController::currentView()->
    // objectName(), and a bare, unnamed widget reads back empty -- not
    // evidence of a failed push, just an anonymous one). Set on the
    // DIALOG, which is what pushView (below) makes the "current view", not
    // on `placeholder`/`content`, which are never pushed themselves.
    reinterpret_cast<QObject*>(dialog)->setObjectName(QStringLiteral("NFBrowserView"));

    N3Dialog__enableBackButton(dialog, true);

    // Review finding I-3, carried over from the trivial-screen milestone:
    // the X (closeTapped(), pre-wired by getDialog to MainWindowController
    // ::closeActiveN3Dialogs()) does nothing on this route -- see
    // N3Dialog__disableCloseButton's own comment (nfnickel.h) for why.
    // Deliberately NOT part of nf_native_view_resolve()'s hard gate: a
    // firmware missing just this symbol should still show the screen, with
    // the X's cosmetic problem left unfixed, not refuse to build the whole
    // thing.
    if (N3Dialog__disableCloseButton)
        N3Dialog__disableCloseButton(dialog);
    else
        nh_log("browser: N3Dialog::disableCloseButton did not resolve -- the X button will remain visible and will do nothing on this route (the BACK row and back gesture are this screen's real exits)");

    // N3Dialog's closeTapped() is already wired, by getDialog itself, to
    // MainWindowController::closeActiveN3Dialogs() (NOTES.md) -- but
    // backTapped() is NOT pre-wired to anything, which is what makes this
    // route's "back" our own responsibility. Wired ONCE, here, to the
    // dialog itself (parented to `dialog`, NOT to `content` -- `content`
    // gets deleteLater()'d on every navigation, via setContent, and this
    // shim must survive every one of those, all the way to the dialog's
    // own eventual destruction). Same signal-adaptor trick as every
    // TouchLabel row, this time relaying the DIALOG's own real signal
    // (built by getDialog's own N3Dialog constructor, not by this file) to
    // nf_browser_back -- the SAME function the guaranteed BACK row calls,
    // so both exits share one place to read "where am I" from.
    QPushButton *backShim = new QPushButton(reinterpret_cast<QWidget*>(dialog));
    backShim->setVisible(false);
    if (!QObject::connect(reinterpret_cast<QObject*>(dialog), SIGNAL(backTapped()), backShim, SLOT(click())))
        nh_log("browser: connecting N3Dialog::backTapped() failed -- the back arrow/gesture will do nothing (the BACK row is this screen's other, independent exit)");
    QObject::connect(backShim, &QPushButton::clicked, [mwc, dialog] {
        nf_browser_back(mwc, dialog);
    });

    nh_log("browser: pushing the folder browser, rooted at %s", NF_ROOT);
    // Builds the real root listing and swaps it in over `placeholder` --
    // see nf_browser_go's own comment for why this is safe to do before
    // pushView (nothing about setContent requires the dialog to already be
    // on screen).
    nf_browser_go(mwc, dialog, QStringLiteral(NF_ROOT));

    MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(dialog));
    return true;
}
