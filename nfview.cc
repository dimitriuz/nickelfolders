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

#include <QBrush>
#include <QDateTime>
#include <QDir>
#include <QFile>       // QFile::exists -- a stat, never an open; see nf_cover_path_for_row
#include <QFileInfo>
#include <QFileInfoList>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMargins>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPushButton>
#include <QRectF>
#include <QSize>
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

// Items shown per page, and the one constant in this file that is pure
// arithmetic over device-measured geometry rather than a judgement call. Every
// term below was measured on this panel (Kobo Libra 2, firmware 4.38.23684);
// none of it is estimated, because every layout number this project ever
// guessed turned out wrong -- NF_COVER_H_PX was 76 by eyeball and clipped its
// own rows, and the page size was 14 and then 12 against a row count read off
// a screenshot's margin rather than counted.
//
//   1330 px   the content area between the first row and the bottom margin
//             (NOTES.md; 1680 visible panel px less Nickel's own chrome)
//     75 px   a text-or-icon row
//     99 px   a row carrying a COVER. An inline <img> sits on the TEXT
//             BASELINE, so such a row is max(ascent, coverHeight) + descent
//             tall: ascent 46, descent ~29, NF_COVER_H_PX 70 -> 70 + 29.
//
// THE WORST CASE IS A PAGE OF NOTHING BUT COVERS, because that is the tallest
// a page of N items can be -- an icon-only page is shorter and simply leaves
// white space, which is the deliberate trade (a page size that varied with how
// many covers happened to land on it would make the row count jump around as
// you page through one folder).
//
// The chrome is TWO rows now, not five: one command bar across the top
// (BACK | sort | filter) and one page bar pinned to the bottom (PREV | page
// N/M | NEXT), each a single row of independently tappable TouchLabels in a
// horizontal layout rather than a full-width row apiece. Both are
// unconditional -- see the page bar's own comment for why its ends stay
// present-but-inert rather than disappearing -- so there is no
// fewer-chrome-rows case to make this number conditional on.
//
//   N * 99 + 2 * 75 <= 1330   ->   N <= (1330 - 150) / 99 = 11.92   ->   11
//
// 11 * 99 + 150 = 1239, with 91 px to spare -- less than one row of either
// height, so 11 is the real ceiling here and not a conservative pick. 12
// would need 1338 and overflow by 8.
//
// THAT IS TWO ROWS BACK, NOT THREE. Dropping three full-width chrome rows
// frees 3 * 75 = 225 px, which is three more ITEM rows only if items are text
// rows; against the 99 px cover rows that bound this number it is 2.27, and
// the fraction is not spendable. The brief's "about three" is right for the
// wrong page.
//
// COUPLED TO NF_COVER_H_PX (nffmt.h) -- change either one and you must redo
// the arithmetic above; its comment carries the same numbers from the cover
// side. A first device screenshot must COUNT the item rows on a full page: if
// it shows fewer than 11, the 1330/75/99 terms are what to re-measure, not
// this quotient.
#define NF_ITEMS_PER_PAGE 11

// PAGINATION, not scrolling -- a deliberate choice, not a shortcut, and
// the reasoning is load-bearing enough to spell out here so nobody
// "upgrades" this to a QScrollArea later and quietly loses touch input.
// NOTES.md's "Task 8: touch input archaeology" is why: Nickel does not
// deliver QMouseEvents at all, it recognises gestures itself, through
// machinery that needs THREE things registered per widget -- a
// grabGesture() call against a registration-time token, an event()
// override that routes touch/gesture events (QWidget::event() does
// neither), and RTTI-based GestureDelegate dispatch -- and this project
// has all three proven on hardware for exactly one gesture: TouchLabel's
// own tap, which self-registers in its own constructor. A QScrollArea's
// viewport is a bare QWidget with none of that wired up for a drag/pan; it
// would render correctly and then sit there as dead to a finger as the
// original AbstractController shim's QPushButton did (this file's own
// header comment, and NOTES.md) -- except SILENTLY, because a tall static
// list simply looks scrollable where a dead button at least looked like a
// button. Paging instead reuses the one gesture already proven end to end:
// a tap on a TouchLabel. Do not replace this with a QScrollArea without
// first doing the same grabGesture/event()/GestureDelegate archaeology
// Task 8 did for tapping -- this comment is that archaeology's citation,
// not a substitute for redoing it if scrolling is ever attempted.

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

// The content widget nf_browser_go most recently handed to
// N3Dialog::setContent -- i.e. the one currently ON SCREEN inside the
// dialog's own chrome. Same file-scope POD discipline as the dialog pointer
// above, and it exists for exactly one reason: it is the only way to MEASURE
// the width N3Dialog actually gives its content, which is the dialog's width
// LESS Nickel's own chrome inset. That inset is not readable from any API
// this mod has -- nf_row_width_px used to say so and over-estimate by it,
// which is one of the two terms that left rows clipping at the right edge on
// the 2026-09-04 device run.
//
// It works because of the navigation model (nfview.h): the content widget is
// replaced, never the dialog. By the time a navigation builds row N+1, the
// widget from navigation N has been laid out inside the real dialog on the
// real panel, and its width() is that measurement. Only ever READ for its
// width, never dereferenced as anything else.
//
// Cleared by the widget's own destroyed() signal, and the clear is guarded on
// the pointer still BEING this widget: setContent deleteLater()s the previous
// content, so an OLD widget's destroyed() fires after its successor has
// already been recorded here, and an unguarded clear would wipe the live
// pointer instead of the dead one.
static void *nf_browser_active_content = NULL;

// The one directory nf_browser_go (below) most recently rebuilt content
// for -- i.e. what BACK steps up from. Set at the top of every call to
// nf_browser_go, including the initial root call from nf_browser_show, so
// it is always valid by the time any row's tap handler or the BACK row can
// possibly fire (both are wired only after the content that reads this has
// already been built for the CURRENT directory, and nothing re-enters this
// file's own code from another thread -- every callback here runs on the
// GUI thread, same as nf_on_trigger_view that calls nf_browser_show).
static char nf_browser_cwd[PATH_MAX];

// The page currently shown within nf_browser_cwd's own listing (0-based).
// Same file-scope POD discipline as the two statics above -- a plain `int`
// is .bss-initialised, same as a `void*`/`char[]`, with no constructor to
// race NickelHook's own init ordering (see nf_browser_active_dialog's own
// comment for the full account of why that race matters here). Reset to 0
// by nf_browser_go itself whenever its `resetPage` argument is true --
// every descend, every BACK/ascend step, and the initial root call all
// pass true, because all three move nf_browser_cwd to a DIFFERENT
// directory, whose page 0 has no relationship to whatever page the
// previous directory happened to be showing. The page bar's PREV/NEXT are
// the one caller that passes false: they change the page WITHIN the same
// directory nf_browser_cwd already names, so resetting here would make
// NEXT always land back on page 0.
static int nf_browser_page = 0;

// Sort key/direction and type filter -- the state the two new chrome rows
// below cycle through. Enum/bool file-scope statics, same POD discipline as
// nf_browser_page above: `= NF_SORT_NAME`/`= false`/`= NF_FILTER_ALL` are
// CONSTANT initialisers the compiler folds into .data at link time, not a
// constructor call needing a runtime _GLOBAL__sub_I entry -- unlike a
// QString/QByteArray at file scope, which is exactly the failure mode
// nf_browser_active_dialog's own comment (above) documents. Confirmed empty
// with the same `nm ... GLOBAL__sub_I` check the task report cites.
//
// PERSIST across navigation -- deliberately NOT reset by nf_browser_go the
// way nf_browser_page is. A reader who just sorted a folder by date, or
// filtered it to PDF, is almost always trying to do the same thing one
// level up or down, not starting over in every new directory; resetting
// these on every descend/ascend would undo the reader's own last tap on
// every single navigation, which is more surprising than carrying it
// forward. nf_browser_page still resets on every navigation (its own
// comment, above) because a page NUMBER has no relationship to a different
// directory's listing, whereas a sort key or a format filter is a
// preference about how ANY listing is read, not a fact about one specific
// directory's contents.
static nf_sort_key    nf_browser_sort_key  = NF_SORT_NAME;
static bool           nf_browser_sort_desc = false;
static nf_filter_kind nf_browser_filter    = NF_FILTER_ALL;

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
// pass. Deliberately does NOT reach for Volume::getDbValues -- see
// nf_volume_exists's own declaration (nfnickel.h) for why that call was
// rejected on its own terms (a displaced sret+this shape this project has
// crashed on once already, a ReadStatus value that reads back as 0 through
// the only exported unwrap path, and an operator[] that inserts rather
// than fails) rather than merely deferred as unestablished archaeology.
// This fills nf_row::hasRow, ::percentRead and ::readState in ONE call --
// nf_volume_exists's own comment has the full derivation for the three
// narrower symbols it reads instead (Content::getReadStatus()/isFinished()
// and a guarded offset off Volume::d()) and NOTES.md's "reading progress
// on folder rows" section has the archaeology behind them. Deliberately does
// NOT touch nf_row::finished: nf_build_listing derives that from ::readState
// once this callback returns (nflist.cc), so there is one source of truth for
// it rather than two that can disagree.
struct NFMetaCtx {
    QString dirPath; // the directory `name` (below) is relative to; ABSOLUTE, no trailing slash
    QString dbName;  // this device's own getById partition key -- nf_db_name(), read once per directory, not per file
};

static void nf_row_meta(void *ctx, QString const& name, nf_row *row) {
    NFMetaCtx const *c = static_cast<NFMetaCtx const*>(ctx);
    QString contentId = QStringLiteral("file://") + c->dirPath + QLatin1Char('/') + name;
    // One call fills hasRow, percentRead, readState, the two raw date sort
    // keys AND the cover ImageId -- deliberately not a second lookup for any
    // of them: they all come off the same Volume, inside the same isValid()
    // branch, before the same Volume__dtor. nf_volume_exists' own declaration
    // (nfnickel.h) has the derivation for each, why the dates are not parsed
    // into a QDateTime, where an empty one sorts, and why an empty ImageId
    // means "no cover can be named" rather than an error.
    row->hasRow = nf_volume_exists(contentId, c->dbName, &row->percentRead, &row->readState,
                                   &row->dateAdded, &row->dateLastRead, &row->imageId);
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
        // size()/lastModified() are read off the SAME QFileInfo the loop
        // already built for name()/isDir() -- no extra stat() call, and no
        // new libnickel symbol: both are what nf_sort_entries' NF_SORT_SIZE/
        // NF_SORT_DATE keys read (nffmt.h). toMSecsSinceEpoch() rather than
        // the now-deprecated toTime_t(), and available since Qt 4.7 -- well
        // inside both the host's 5.15 and the device's 5.2.1, the same
        // cross-version floor every pure source in this project already
        // holds itself to (Makefile's own comment).
        e.size  = infos.at(i).size();
        e.mtime = infos.at(i).lastModified().toMSecsSinceEpoch();
        entries << e;
    }
    return entries;
}

// Cycles nf_browser_sort_key/nf_browser_sort_desc as ONE combined ten-state
// sequence on a single tap -- name-ascending, name-descending, size-
// ascending, size-descending, date-ascending, date-descending, added-
// ascending, added-descending, read-ascending, read-descending, back to
// name-ascending -- rather than needing two separate rows for what the task
// brief frames as two orthogonal choices (key, direction). Direction flips
// first and key advances only every second tap, so a reader sees both
// directions of whichever key they just picked before it moves on.
//
// The two library-date keys come AFTER name/size/date, in that order, so the
// tap sequence the owner has already learned on hardware is unchanged and the
// new keys are appended past the end of it rather than inserted into the
// middle -- the same rule the read-state filters followed onto
// nf_browser_cycle_filter below.
static void nf_browser_cycle_sort(void) {
    if (!nf_browser_sort_desc) {
        nf_browser_sort_desc = true;
        return;
    }
    nf_browser_sort_desc = false;
    switch (nf_browser_sort_key) {
        case NF_SORT_NAME:  nf_browser_sort_key = NF_SORT_SIZE;  break;
        case NF_SORT_SIZE:  nf_browser_sort_key = NF_SORT_DATE;  break;
        case NF_SORT_DATE:  nf_browser_sort_key = NF_SORT_ADDED; break;
        case NF_SORT_ADDED: nf_browser_sort_key = NF_SORT_READ;  break;
        case NF_SORT_READ:
        default:            nf_browser_sort_key = NF_SORT_NAME;  break;
    }
}

// Cycles nf_browser_filter through all -> cbz -> cbr -> pdf -> epub ->
// finished -> in progress -> not started -> all.
//
// The three read-state options come AFTER the five format ones, in that order,
// so the tap sequence the owner has already learned on hardware (all -> cbz ->
// cbr -> pdf -> epub) is unchanged and the new options are appended past the
// end of it rather than inserted into the middle. Eight states on one row is a
// long cycle -- but a second chrome row would cost an item row out of the
// panel's measured budget of 17, which is a worse trade than a few extra taps
// (nffmt.h, nf_filter_kind).
static void nf_browser_cycle_filter(void) {
    switch (nf_browser_filter) {
        case NF_FILTER_ALL:         nf_browser_filter = NF_FILTER_CBZ;         break;
        case NF_FILTER_CBZ:         nf_browser_filter = NF_FILTER_CBR;         break;
        case NF_FILTER_CBR:         nf_browser_filter = NF_FILTER_PDF;         break;
        case NF_FILTER_PDF:         nf_browser_filter = NF_FILTER_EPUB;        break;
        case NF_FILTER_EPUB:        nf_browser_filter = NF_FILTER_FINISHED;    break;
        case NF_FILTER_FINISHED:    nf_browser_filter = NF_FILTER_IN_PROGRESS; break;
        case NF_FILTER_IN_PROGRESS: nf_browser_filter = NF_FILTER_NOT_STARTED; break;
        case NF_FILTER_NOT_STARTED:
        default:                     nf_browser_filter = NF_FILTER_ALL;         break;
    }
}

// Plain ASCII, e-ink-safe, matching the "^"/"v"-style affordance the task
// brief itself suggests ("sort: name ^") and the same convention as this
// file's other ASCII chrome ("< BACK", "< PREV"). "^" reads as
// ascending (smallest/oldest/A first, pointing at the top of the list) and
// "v" as descending, without needing a real glyph this panel may not have.
// "date" is the FILE's own mtime and "added"/"read" are the LIBRARY's two
// dates; three one-word names for three genuinely different questions, all
// short enough not to eat the row's width budget the way "date added" and
// "date last read" would. "read" is the reading date, not the read STATE --
// the filter row is where read state lives ("filter: finished"), and the two
// rows are never both showing a word from the other's vocabulary.
static QString nf_sort_row_label(void) {
    QString keyName;
    switch (nf_browser_sort_key) {
        case NF_SORT_SIZE:  keyName = QStringLiteral("size");  break;
        case NF_SORT_DATE:  keyName = QStringLiteral("date");  break;
        case NF_SORT_ADDED: keyName = QStringLiteral("added"); break;
        case NF_SORT_READ:  keyName = QStringLiteral("read");  break;
        case NF_SORT_NAME:
        default:            keyName = QStringLiteral("name");  break;
    }
    return QStringLiteral("sort: %1 %2").arg(keyName,
        nf_browser_sort_desc ? QStringLiteral("v") : QStringLiteral("^"));
}

// The read-state names are spelled out in words ("finished", "in progress",
// "not started") rather than shortened to match the four lowercase format
// abbreviations above them: the abbreviations are the formats' own file
// extensions, which a reader already knows, whereas an abbreviated read state
// would be this mod inventing a vocabulary. This string is also what the
// "everything here was filtered out" message quotes back (below), so it has to
// read as a sentence fragment, not a code.
static QString nf_filter_row_label(void) {
    QString filterName;
    switch (nf_browser_filter) {
        case NF_FILTER_CBZ:         filterName = QStringLiteral("cbz");         break;
        case NF_FILTER_CBR:         filterName = QStringLiteral("cbr");         break;
        case NF_FILTER_PDF:         filterName = QStringLiteral("pdf");         break;
        case NF_FILTER_EPUB:        filterName = QStringLiteral("epub");        break;
        case NF_FILTER_FINISHED:    filterName = QStringLiteral("finished");    break;
        case NF_FILTER_IN_PROGRESS: filterName = QStringLiteral("in progress"); break;
        case NF_FILTER_NOT_STARTED: filterName = QStringLiteral("not started"); break;
        case NF_FILTER_ALL:
        default:                     filterName = QStringLiteral("all");         break;
    }
    return QStringLiteral("filter: %1").arg(filterName);
}

static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path, bool resetPage);

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
    nf_browser_go(mwc, dialog, parent, true); // ascend -- a different directory, page resets
}

// --- row icons ---------------------------------------------------------
//
// A LEADING icon on every row. The folder/file distinction used to live only
// in a TRAILING "/" appended to the label below, which is the worst available
// place for it: it is the first thing lost when a long label runs off the
// right edge, and nf_strip_common (nffmt.cc) leaves plenty of long labels.
// The reference card's
// "The Road - A Graphic Novel Adaptation (2024) (Digital) (phillywilly-Empire).cbr"
// -- a FILE, and the reason this feature exists -- was read as a folder for
// exactly that reason. A LEADING marker cannot be elided away.
//
// THE ICONS ARE OURS, DRAWN BY THIS FILE. They used to be Nickel's own Qt
// resources, borrowed out of the process we are injected into -- free, but
// never a SET. Their device-measured dimensions are the whole argument:
//
//   :/images/menu/label_arrow_right.png     15x26   folder rows
//   :/images/home/main_nav_books.png        50x50   .epub / .kepub.epub
//   :/images/reading/reading_image_view.png 80x80   .cbz / .cbr
//   (nothing at all)                                .pdf, unknown
//
// Three aspect ratios, three visual weights, and two kinds with no art
// anywhere in Nickel's 373 :/images/... resources (swept, not assumed), which
// is why .pdf and unknown fell back to "[PDF]" and "[ ? ]" text badges. The
// owner asked for "icons for folders and different known file types"; the
// right arrow in particular was only ever a STOPGAP, picked because Nickel
// ships no folder pictogram at all, not because a right arrow is what was
// wanted. A folder icon was the original request and is now what a folder row
// gets.
//
// Two findings from the borrowed-resource era are worth keeping, because both
// cost a device cycle:
//   - `:/images/widgets/folder.png`, the obvious folder candidate, is NOT a
//     folder pictogram: the probe logged it at 250x350 and the screenshot
//     showed an empty light-grey rectangle -- COVER-ART-SHAPED PLACEHOLDER
//     art (5:7, the aspect of every cover slot on Nickel's own shelves),
//     i.e. presumably the artwork behind a Nickel COLLECTION. The resource
//     NAME did not predict its CONTENT.
//   - The DIMENSIONS were the tell, and were read past the first time: 250x350
//     next to 50x50 and 80x80 already said "this is not an icon" before the
//     screenshot confirmed it. Which is why the generator below still logs a
//     width and height per icon even though we drew them and therefore
//     already "know" -- what it logs is what the FILE says when read back,
//     not what we intended.
//
// HOW OUR OWN ART REACHES A QLabel, since the two obvious routes are closed:
//   - A QPixmap/QImage we drew cannot be handed to the row's RICH TEXT.
//     QLabel exposes no public QTextDocument, so QTextDocument::addResource
//     -- the documented way to bind a name to an in-memory image -- is
//     unreachable from outside the widget.
//   - rcc (a compiled-in :/ bundle of our own) is ruled out outright: it
//     registers its bundle from a FILE-SCOPE STATIC INITIALISER, the exact
//     construct that once boot-looped this mod into NickelHook's SHARED
//     failsafe, which can uninstall the owner's OTHER mods (nfnickel.cc's own
//     QByteArray account, and nf_browser_active_dialog's comment above).
// What IS reachable is a FILE PATH: QTextDocument::loadResource opens a local
// file for an <img src> it cannot otherwise resolve. So each icon is painted
// into a QImage and written out as a PNG, and the row markup points at it by
// absolute path.
//
// WHY /tmp, AND NOTHING ON THE CARD -- two of CLAUDE.md's hard constraints,
// not a convenience:
//   - /tmp is TMPFS, so writing there never opens a file handle on
//     /mnt/onboard. A handle held there across a USB session risks corrupting
//     the owner's card, which is why this project's own trigger files live on
//     /tmp too. Nothing here ever writes to the user's card.
//   - tmpfs CLEARS ON REBOOT, and that is the feature rather than the cost:
//     the icons are regenerated every time Nickel starts, so a changed
//     drawing can never be shadowed by a stale PNG from an older build, and
//     the install stays exactly one .so the owner deletes to uninstall.
//     Nothing is shipped and nothing has to be cleaned up.
// They are also rewritten UNCONDITIONALLY rather than reused when already
// present, for that same staleness reason: tools/restart-nickel.sh restarts
// Nickel without clearing tmpfs, so "the file is there" is not evidence it
// came from this build.
#define NF_ICON_DIR "/tmp/nfolders-icons"

// A path this file NEVER writes, loaded alongside the five real ones. Without
// it, "the icon loaded back" is not evidence of anything -- CLAUDE.md's "a
// negative control is what makes a check non-vacuous", the same discipline as
// the isValid=false ContentID that made rung 1's isValid=true mean something.
// The previous, resource-based probe's control caught nothing, and that was
// the point: it is what established that Nickel's resources were reachable
// from this library at all rather than that four lines had been printed.
#define NF_ICON_FILE_CONTROL NF_ICON_DIR "/nfolders-never-written.png"

// Every icon is square and every icon is this size -- the consistency that
// the borrowed set (15x26 / 50x50 / 80x80) could not have.
//
// 40 is the SAFE BUDGET, not an aesthetic pick: the panel's measured capacity
// is 17 rows on 1680 visible px, NF_ITEMS_PER_PAGE is keyed to that, and every
// px of row height risks it. 40 was already the forced <img> height of the
// borrowed set and the device did NOT grow rows at it, which is the whole
// reason it is reused here rather than raised. Do not raise it without a
// screenshot that counts rows.
#define NF_ICON_PX 40

// Odd, so a stroke centred on a half-pixel coordinate covers whole pixels
// (see nf_icon_draw). 3 px at 40 px is deliberately heavy: a 40 px icon drawn
// with hairlines is muddy after this panel's own dithering, and
// distinguishability at 40 px matters far more here than detail does.
#define NF_ICON_STROKE_PX 3

// One row per icon kind. POD only, with constant initialisers -- string
// literal addresses and integers, no constructor to run -- so this lives in
// .data with nothing that could race NickelHook's nh_init the way a
// file-scope QString/QByteArray would (nfnickel.cc's own account of that
// crash). `nm libnfolders.so | grep GLOBAL__sub_I` must stay empty.
//
// `kind` is stored rather than implied by the row's POSITION, and looked up
// by search (nf_icon_entry): reordering nf_icon_kind in nffmt.h, or inserting
// an enumerator, then silently shifts every icon by one if the table is
// indexed positionally, and a WRONG icon is exactly the defect this feature
// exists to end. A kind with no row here degrades to its text badge, the same
// as a kind whose PNG failed to write.
//
// `ok`, `w` and `h` are filled in by nf_icons_generate from the file READ
// BACK, never from what was drawn.
struct nf_icon_file {
    int         kind;    // an nf_icon_kind, held as int so this stays POD-plain
    char const *path;
    bool        ok;      // written AND loaded back -- the only thing that
                         // licenses emitting an <img> for this kind
    int         w;
    int         h;
};

static nf_icon_file nf_icon_files[] = {
    { NF_ICON_FOLDER,  NF_ICON_DIR "/folder.png",  false, 0, 0 },
    { NF_ICON_BOOK,    NF_ICON_DIR "/book.png",    false, 0, 0 },
    { NF_ICON_COMIC,   NF_ICON_DIR "/comic.png",   false, 0, 0 },
    { NF_ICON_PDF,     NF_ICON_DIR "/pdf.png",     false, 0, 0 },
    { NF_ICON_UNKNOWN, NF_ICON_DIR "/unknown.png", false, 0, 0 },
};

#define NF_ICON_FILE_COUNT ((int)(sizeof nf_icon_files / sizeof nf_icon_files[0]))

static nf_icon_file *nf_icon_entry(nf_icon_kind kind) {
    for (int i = 0; i < NF_ICON_FILE_COUNT; i++)
        if (nf_icon_files[i].kind == (int)kind)
            return &nf_icon_files[i];
    return NULL;
}

// Generated ONCE, lazily, on first use. A plain file-scope bool rather than a
// function-local static for TWO reasons, both load-bearing: a function-local
// static of non-POD type compiles to a __cxa_guard_acquire/release pair, i.e.
// libstdc++ runtime, which CLAUDE.md forbids outright; and a POD bool lives
// in .bss with no constructor to race NickelHook's own nh_init ordering, the
// same discipline as every other file-scope datum in this file. Every caller
// runs on the GUI THREAD (a row build, reached from a tap handler or from the
// trigger that opens the screen), which is both where Nickel's UI may be
// touched from and the only place this needs to be correct -- so there is no
// thread to race either, and no need for a guard even if one were allowed.
static bool nf_icons_generated = false;

// Draws one icon into a fresh NF_ICON_PX-square QImage. Pure QPainter on a
// QImage: no libnickel, no window, no platform plugin, nothing that has to be
// on-screen -- which is also why this is the one part of the icon work that
// could not have been host-tested anyway (the mapping that CAN be is
// nf_icon_kind_for, in nffmt.cc, with its own tests).
//
// THE SHAPE LANGUAGE, so the five read as one set: a 3 px black outline
// silhouette plus exactly ONE solid black accent each, on transparency. No
// grey fills anywhere. koboy's measurements on this exact panel
// (../koboy/CLAUDE.md, ../koboy/TESTED.md) are why: four-level content is
// what smears under the fast waveforms and genuinely two-valued content does
// not, and four grey levels is all this panel has to spend in the first place
// -- so the set spends none of them. What that finding does NOT forbid is
// antialiasing, which is enabled below; it was measured against a 60 fps
// emulator repainting the whole screen through DU, not against a static list
// Nickel repaints once, and at ~300 ppi the aliasing on the folder tab's
// diagonal and the comic mountain is coarse enough to see. So: antialiased
// EDGES, every fill strictly black or fully transparent, and every stroke
// 3 px wide, so no shape depends on a partially covered pixel to be visible.
static QImage nf_icon_draw(nf_icon_kind kind) {
    // ARGB32 and a fully TRANSPARENT ground, not white: the row background is
    // Nickel's, not ours, and a white tile would show as a visible box around
    // every icon the moment Nickel draws a row on anything but pure white (a
    // tap highlight, a themed list). Transparency costs nothing -- PNG
    // carries alpha and Qt's own rich-text image handler composites it.
    QImage img(NF_ICON_PX, NF_ICON_PX, QImage::Format_ARGB32);
    if (img.isNull())
        return img;
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    // HALF-PIXEL coordinates throughout, with the odd NF_ICON_STROKE_PX: a
    // 3 px pen centred on x=8.5 covers exactly 7..10, where the same pen
    // centred on x=8 covers 6.5..9.5 and leaves two half-intensity columns.
    // On a reflective panel that is the difference between a crisp line and a
    // smudged one, and it is the reason every number below ends in .5.
    QPen pen(Qt::black);
    pen.setWidth(NF_ICON_STROKE_PX);
    pen.setJoinStyle(Qt::MiterJoin);
    pen.setCapStyle(Qt::FlatCap);

    switch (kind) {
        case NF_ICON_FOLDER: {
            // The classic folder: a body with a raised TAB on the left, the
            // tab filled solid as this icon's one black accent. Outlined
            // rather than filled solid throughout so it carries the same
            // visual weight as the four file icons beside it.
            QPointF body[6] = {
                QPointF( 3.5, 34.5), QPointF( 3.5,  7.5), QPointF(15.5,  7.5),
                QPointF(19.5, 12.5), QPointF(36.5, 12.5), QPointF(36.5, 34.5),
            };
            // The tab's own BOTTOM edge lands on y=13.0, an integer, and
            // not on the body's 12.5 like every other coordinate here: for
            // x below 19.5 that edge is EXPOSED (the body outline runs
            // vertically at x=3.5 there, not along y=12.5), so a half-pixel
            // bottom left a visible grey seam under the tab -- caught on a
            // host render before it ever reached the panel. Half-pixels are
            // for STROKE centres; an exposed FILL boundary wants an integer.
            QPointF tab[4] = {
                QPointF( 3.5, 13.0), QPointF( 3.5,  7.5),
                QPointF(15.5,  7.5), QPointF(19.5, 13.0),
            };
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPolygon(tab, 4);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(body, 6);
            break;
        }
        case NF_ICON_BOOK: {
            // An OPEN book -- two pages leaning into a solid black gutter --
            // and this is the one shape here that was chosen by rendering
            // the alternatives rather than by reasoning about them. A CLOSED
            // book (a portrait outline with a black spine bar, tried two
            // ways) is a rectangle, and at 40 px it read as a domino or a
            // battery, not as a book: it was also barely distinguishable
            // from the PDF sheet below, which is the other portrait
            // rectangle in the set. The open book's V silhouette is the only
            // one of the four candidates that cannot be mistaken for any
            // other icon here.
            //
            // One kind for .epub and .kepub.epub, intentionally: the same
            // book format with and without Kobo's own preprocessing, which
            // is not a distinction a reader makes (nffmt.h says the same on
            // its own side of the boundary).
            QPointF leftPage[4] = {
                QPointF( 3.5, 11.5), QPointF(19.5, 15.5),
                QPointF(19.5, 34.5), QPointF( 3.5, 30.5),
            };
            QPointF rightPage[4] = {
                QPointF(36.5, 11.5), QPointF(20.5, 15.5),
                QPointF(20.5, 34.5), QPointF(36.5, 30.5),
            };
            // The gutter, on integers and deliberately WIDER than the two
            // page strokes it sits under (18..22 against 18..21 and 19..22):
            // drawn as two coincident 3 px strokes instead, the pair
            // antialiased against each other and rendered as a GREY bar --
            // seen on a host render of exactly that variant. A solid fill
            // underneath them has no seam to grey.
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawRect(QRectF(18.0, 14.0, 4.0, 21.0));
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(leftPage, 4);
            p.drawPolygon(rightPage, 4);
            break;
        }
        case NF_ICON_COMIC: {
            // ONE icon for .cbz and .cbr, intentionally: they are the same
            // comic archive with a different compressor inside, and a reader
            // does not distinguish them (nffmt.h says the same on its own
            // side of the boundary). A framed picture -- mountain and sun --
            // rather than a stack of pages: the LANDSCAPE frame is what makes
            // it unmistakable against the PDF sheet beside it, and "the
            // archive is full of images" is what a comic is.
            QPointF peak[3] = { QPointF(9.0, 30.0), QPointF(19.0, 15.0), QPointF(29.0, 30.0) };
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPolygon(peak, 3);
            p.drawEllipse(QRectF(25.5, 11.5, 7.0, 7.0));
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(4.5, 8.5, 31.0, 23.0));
            break;
        }
        case NF_ICON_PDF: {
            // A document sheet with a dog-eared corner and a solid black band
            // across the bottom. The band, not the fold, is what carries at
            // 40 px -- the fold is 8 px of detail and is there for the
            // silhouette. Letters were considered and rejected: "PDF" inside
            // a 21 px band is ~6 px per glyph, which this panel's dithering
            // turns to mush; the row's own filename still ends in ".pdf".
            QPointF sheet[5] = {
                QPointF(10.5,  4.5), QPointF(23.5,  4.5), QPointF(31.5, 12.5),
                QPointF(31.5, 35.5), QPointF(10.5, 35.5),
            };
            QPointF fold[3] = { QPointF(23.5, 4.5), QPointF(23.5, 12.5), QPointF(31.5, 12.5) };
            // The band's TOP edge is the only one of its four not hidden
            // under the sheet outline, so it lands on y=25.0 rather than on
            // a half-pixel -- same seam, same host render, same reason as
            // the folder tab above.
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawRect(QRectF(10.5, 25.0, 21.0, 10.5));
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(sheet, 5);
            p.drawPolyline(fold, 3);
            break;
        }
        case NF_ICON_UNKNOWN:
        default: {
            // A rounded square with a bold "?" -- the replacement for the
            // "[ ? ]" badge, and the one icon whose accent is a GLYPH rather
            // than a shape we drew, because "?" is the meaning and drawing an
            // arc-and-dot by hand would be a worse question mark than the
            // font's own. Unreachable while nf_is_book_name gates every file
            // row on the same allowlist nf_icon_kind_for reads, and answered
            // anyway: a format added to NF_EXTS but not to the icon map must
            // look like a question, not like a missing icon.
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(5.5, 5.5, 29.0, 29.0), 6.0, 6.0);

            QFont f = p.font();
            f.setBold(true);
            f.setPixelSize(24); // pixels, not points: this is a 40 px canvas,
                                // and a point size would depend on whatever
                                // DPI the paint device claims.
            p.setFont(f);
            p.setPen(Qt::black); // a plain pen -- glyphs are FILLED with the
                                 // pen colour, so the 3 px width above would
                                 // do nothing here but is confusing to leave.
            p.drawText(QRectF(0.0, 0.0, (qreal)NF_ICON_PX, (qreal)NF_ICON_PX),
                       Qt::AlignCenter, QStringLiteral("?"));
            break;
        }
    }

    p.end();
    return img;
}

// Draws all five, writes them under NF_ICON_DIR, and VERIFIES each one by
// loading the written file back. Nothing here is fatal: every failure path
// leaves that kind's `ok` false, which nf_icon_markup renders as the text
// badge it used to render for .pdf and unknown. CLAUDE.md is explicit that
// NickelHook's failsafe is SHARED infrastructure -- a mod that fails hard can
// make the owner's OTHER mods uninstall themselves -- and an icon is about as
// non-essential as this project gets.
static void nf_icons_generate(void) {
    if (nf_icons_generated)
        return;
    // Set BEFORE the work, not after: a failure below must not be retried on
    // every row of every navigation for the rest of the session.
    nf_icons_generated = true;

    // mkpath, not mkdir: it creates NF_ICON_DIR's parents too and returns
    // true when the directory already exists, so there is no "already there"
    // case to special-case. It returns false if something that is NOT a
    // directory occupies the path, which is a real (if odd) way for this to
    // fail and is why the result is checked at all.
    QDir dir(QStringLiteral(NF_ICON_DIR));
    if (!dir.mkpath(QStringLiteral("."))) {
        nh_log("icons: mkpath '%s' failed -- every row falls back to a text badge", NF_ICON_DIR);
        return;
    }

    int ok = 0;
    for (int i = 0; i < NF_ICON_FILE_COUNT; i++) {
        nf_icon_file *e = &nf_icon_files[i];
        QString path = QString::fromLatin1(e->path);

        QImage img = nf_icon_draw((nf_icon_kind)e->kind);
        if (img.isNull()) {
            nh_log("icons: '%s' -> could not allocate the %dx%d canvas, falls back to its text badge",
                   e->path, NF_ICON_PX, NF_ICON_PX);
            continue;
        }
        // "PNG" spelled out rather than inferred from the extension: the
        // extension route asks Qt to guess, and a guess that lands on a
        // handler this build of Qt does not have fails with the same null
        // result as a missing file. Qt's PNG handler is built into QtGui, but
        // this library is loaded BY Qt's own image-format plugin scan, so
        // "which handlers exist" is not a thing to assume from in here -- if
        // it is missing, this is where it shows up, in one log line.
        if (!img.save(path, "PNG")) {
            nh_log("icons: '%s' -> QImage::save FAILED, falls back to its text badge", e->path);
            continue;
        }
        // READ BACK. The point is not that save() returned true -- it is that
        // the bytes on disk decode as an image, because decoding it is
        // precisely what Qt's rich-text image handler will have to do from
        // inside the QLabel, and a file that does not decode there renders as
        // nothing at all with no diagnostic anywhere.
        QImage back(path);
        if (back.isNull()) {
            nh_log("icons: '%s' -> written but does NOT load back, falls back to its text badge", e->path);
            continue;
        }
        // The ACTUAL dimensions, from the file, not NF_ICON_PX assumed -- see
        // nf_icon_width_px for why the elision below depends on this being
        // measured rather than believed.
        e->w  = back.width();
        e->h  = back.height();
        e->ok = true;
        ok++;
        nh_log("icons: '%s' -> wrote and loaded back %dx%d", e->path, e->w, e->h);
    }

    // The negative control, read LAST so it sits at the end of the block in
    // the log. If this ever says LOADED, the five lines above prove nothing:
    // something is resolving paths we never wrote, and "it loaded back" would
    // then be a statement about that something rather than about our PNGs.
    QImage control(QStringLiteral(NF_ICON_FILE_CONTROL));
    nh_log("icons: control '%s' -> %s", NF_ICON_FILE_CONTROL,
           control.isNull() ? "NULL, as required" : "LOADED -- the read-backs above prove nothing");

    nh_log("icons: %d of %d drawn and verified (%dpx square, %dpx stroke, in %s)",
           ok, NF_ICON_FILE_COUNT, NF_ICON_PX, NF_ICON_STROKE_PX, NF_ICON_DIR);
}

// The DEGRADED text-badge fallback -- what a row shows when its PNG could not
// be written or would not load back -- MOVED to nffmt.cc (nf_icon_badge),
// with its two-form contract and the reason both forms are built together.
// It is a pure function of a kind, so it belongs on the host-testable side of
// the boundary, and the invariant Fix 2 leans on (the measured form is
// character-for-character what the rendered form renders as) now has tests.

// The leading markup for one row, separator included, so the row loop never
// has to know which kinds render as an image and which as text.
static QString nf_icon_markup(nf_icon_kind kind) {
    nf_icons_generate();

    nf_icon_file const *e = nf_icon_entry(kind);
    if (e && e->ok) {
        // A BARE ABSOLUTE PATH -- no scheme. DEVICE-MEASURED 2026-09-04 on
        // firmware 4.38.23684 (Qt 5.2.1): this form renders all five icons,
        // and `file:///tmp/...` renders NONE of them -- every row drew Qt's
        // own broken-image placeholder (a faint page with a folded corner)
        // instead. Verified by screenshot, with the two forms selected at
        // runtime and the build logging which one it used, so the images
        // cannot be misattributed.
        //
        // The PNGs were never the problem, which is the other half of the
        // measurement: all five wrote and loaded back at 40x40 with distinct
        // file sizes, and the never-written control path reported null. Only
        // the reference form was wrong.
        //
        // THE REASONING THIS REPLACES, kept because the next reader will
        // have exactly the same instinct: this used to emit "file://" + the
        // path, arguing that QTextDocument::loadResource's scheme-less
        // branch depends on the document's base URL staying empty, which is
        // "a QLabel internal we do not own", where an explicit scheme needs
        // none of it. That was reasonable and it was WRONG on 5.2.1 -- the
        // explicit-scheme path is the one that fails there. Do not "fix"
        // this bare path back into a URL; it is the measured form, and the
        // QLabel internal it leans on is measured working on the firmware
        // this mod runs against. (It was verified on HOST Qt 5.15, where
        // file:// does work -- which is exactly the 5.15-versus-5.2.1 skew
        // the Makefile's two build paths are kept apart to expose, showing
        // up here as a runtime difference rather than a build failure.)
        //
        // WIDTH AND HEIGHT are the dimensions read back off the FILE
        // (nf_icons_generate), so the <img> box is a number this code
        // measured rather than one it assumed -- and it is the same number
        // nf_icon_width_px charges the elision below, which is the only way
        // those two can agree.
        //
        // &nbsp; rather than a plain space for the separator, here and in
        // nf_icon_badge's fallback: this string is rich text by the time
        // QLabel sees it, and HTML collapses runs of whitespace. It is also
        // why nf_icon_width_px charges a U+00A0 and not a U+0020 -- the two
        // are different characters with different widths.
        return QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\">&nbsp;")
                   .arg(QString::fromLatin1(e->path)).arg(e->w).arg(e->h);
    }

    QString badge;
    nf_icon_badge(kind, &badge, NULL);
    return badge;
}

// --- row label elision -------------------------------------------------
//
// Long labels ran off the right edge of the panel, and what fell off it was
// the part that told two rows apart. The measured case, from the reference
// card's root:
//
//   steven l. kent - the ultimate history of video games, volume 1 - 2001.kepub.epub
//   steven l. kent - the ultimate history of video games, volume 2 - 2021.kepub.epub
//
// rendered as TWO VISUALLY IDENTICAL ROWS: everything on screen was the shared
// prefix, and "volume 1 - 2001" versus "volume 2 - 2021" was past the edge.
//
// nf_strip_common (nffmt.cc) did not save them, and correctly so: stripping
// the common run leaves "1 - 2001" and "2 - 2021", which contain no letter,
// and its letter guard rejects that whole set rather than hand a reader a
// column of bare numbers. That guard is deliberate and separately recorded --
// it is not what gets changed here.
//
// So the fix is Qt::ElideMiddle, which keeps the head AND the tail.
// Qt::ElideRight would be actively useless on exactly this case: it removes
// precisely the characters that distinguish the two rows, i.e. it produces the
// same two identical rows the panel already showed, only with an ellipsis on
// them. ElideMiddle is the only mode that keeps enough of both ends for the
// pair above to read as two different books.
//
// Qt picks the ellipsis itself -- U+2026 when the row's own font can render
// it, "..." otherwise (QTextEngine's elidedText) -- so there is nothing to
// choose here and no glyph to risk on this panel; whichever it uses is one the
// font already has.
//
// ELIDING TO THE RIGHT NUMBER is the harder half, and the 2026-09-04 device
// run is why it is now done in two passes rather than one. Rows still clipped
// at the right edge -- "... - 2016.pd", "(40%" without its "%)" -- with the
// suffix ALREADY measured and reserved before the name was elided. So the
// order was not the bug (it was already right); the WIDTHS were, and this file
// could not tell which of them off-device:
//
//   - the separators were measured as ASCII spaces and rendered as U+00A0,
//   - the row width was the dialog's, with Nickel's own chrome inset only
//     estimated (nf_row_width_px's old comment admitted the over-estimate),
//   - the TouchLabel's own inset was not subtracted at all,
//   - and FontSizeAdjustingLabel may change its point size when the text is
//     set, i.e. after the QFontMetrics used to measure it was read.
//
// The first three are now measured terms (see the budget in the row loop).
// The fourth cannot be, so the row loop ends with a pass that measures the
// ASSEMBLED widget (QLabel::sizeHint) and re-elides if it overflows -- which
// covers all four and anything else of the same shape, and logs the shortfall
// instead of hiding it.

// The panel's own visible width, MEASURED and already recorded in CLAUDE.md:
// the framebuffer is padded to 1280x1792 against a visible panel of 1264x1680,
// which is why this is 1264 and specifically not 1280 -- 16 px of that
// framebuffer is off the glass, and treating it as usable would elide 16 px
// too late on every row.
#define NF_PANEL_VISIBLE_WIDTH_PX 1264

// The floor below which a QWidget's own width() is read as "not laid out yet"
// rather than as a measurement. DERIVED, not picked: Qt gives a top-level
// widget that has never been shown or sized a default 640x480, and this
// panel's real width is 1264 (above), so 800 sits between the two with ~160 px
// of margin below and ~460 above -- there is no plausible real width for a
// full-screen N3Dialog on this device anywhere near it. This matters on the
// FIRST listing specifically: nf_browser_go builds the root screen BEFORE
// nf_browser_show calls pushView (see the call order there), so on that one
// pass the dialog genuinely has not been sized to the screen yet and its
// width() is that Qt default, not a truth about this panel.
#define NF_WIDTH_PLAUSIBLE_MIN_PX 800

// Width in px available to ONE row, icon and suffix not yet deducted (the row
// loop does both, per row, because both vary per row).
//
// THREE sources, best first, and *source tells the caller which one it got so
// the log line can say so: a silently wrong width would either elide text that
// fits or fail to elide text that does not, and on a screenshot both of those
// look like "the elision is broken" with no way to tell them apart.
//
//   "CONTENT"  the width the PREVIOUS navigation's content widget was
//              actually given inside the dialog -- see
//              nf_browser_active_content. This is the only one of the three
//              that has Nickel's own N3Dialog chrome inset already taken out
//              of it, because it is a widget that really was laid out inside
//              that chrome. Unavailable on the very first listing (there is
//              no previous content yet), which is why the other two remain.
//   "DIALOG"   N3Dialog::width(), device-measured at 1264 on 4.38.23684. An
//              OVER-estimate by however wide the chrome inset is -- which is
//              one of the two terms that left rows clipping on the
//              2026-09-04 run, and precisely what the CONTENT source above
//              was added to stop guessing at.
//   "FALLBACK" the panel constant. The FIRST listing specifically needs it:
//              nf_browser_go builds the root screen BEFORE nf_browser_show
//              calls pushView (see the call order there), so on that one
//              pass the dialog genuinely has not been sized to the screen
//              yet and its width() is Qt's 640x480 default.
//
// Both measured sources are CLAMPED to the visible panel width, and the clamp
// is a measurement rather than a pad: the framebuffer is 1280 wide against
// 1264 visible px (CLAUDE.md), so any widget sized to the padded buffer would
// hand back 16 px that are not on the glass. N3Dialog::width() reads 1264 on
// this firmware, so the clamp is a no-op here and a floor under a firmware
// that sizes its top-level widgets differently.
static int nf_row_width_px(N3Dialog *dialog, QLayout *layout,
                           char const **source, int *rawDialogWidth) {
    int contentW = 0;
    if (nf_browser_active_content)
        contentW = reinterpret_cast<QWidget*>(nf_browser_active_content)->width();
    int dialogW = reinterpret_cast<QWidget*>(dialog)->width();
    *rawDialogWidth = dialogW;

    int w;
    if (contentW >= NF_WIDTH_PLAUSIBLE_MIN_PX) {
        *source = "CONTENT, measured off the widget the dialog last laid out";
        w = contentW;
    } else if (dialogW >= NF_WIDTH_PLAUSIBLE_MIN_PX) {
        *source = "DIALOG, measured but not less N3Dialog's own chrome inset";
        w = dialogW;
    } else {
        *source = "FALLBACK, nothing is laid out yet";
        w = NF_PANEL_VISIBLE_WIDTH_PX;
    }
    if (w > NF_PANEL_VISIBLE_WIDTH_PX)
        w = NF_PANEL_VISIBLE_WIDTH_PX;

    // OUR OWN layout margins, queried rather than guessed -- 34+34 on this
    // firmware's default QVBoxLayout, i.e. the 1264 -> 1196 step in the log
    // line below.
    QMargins m = layout->contentsMargins();
    w -= m.left() + m.right();

    // A deliberate floor, not dead code: QFontMetrics::elidedText with a
    // width at or below the ellipsis' own width returns the ellipsis alone (or
    // nothing), i.e. a screen of rows reading "..." and no names at all. No
    // path above can currently produce a number that low -- every branch
    // starts from at least 800 -- but this is the one place where a bad width
    // erases the entire listing rather than degrading it, so the floor is
    // cheap insurance worth keeping.
    if (w < 200)
        w = 200;
    return w;
}

// The px a TouchLabel spends on ITSELF before any of our text is drawn, and
// the second of the two terms the 2026-09-04 device run showed missing: the
// row width above is the width of the WIDGET, not of the text area inside it,
// and eliding against the former over-runs the latter by exactly this much.
//
// Every term is a property read off the widget Nickel's own TouchLabel
// constructor just finished initialising -- none of it is guessed, and none of
// it needs the widget to have been laid out yet (contentsMargins/margin/
// indent/alignment are all set-values, not geometry). It mirrors, term for
// term, what QLabelPrivate::documentRect() itself subtracts before handing the
// remainder to QTextDocument::setTextWidth, which is the number that actually
// decides where rich text gets cut:
//
//   contentsMargins()   QFrame folds its own frameWidth into these
//                       (QFramePrivate::updateFrameWidth calls
//                       setContentsMargins), so reading frameWidth() as a
//                       separate term would double-count it.
//   margin()            QLabel::margin, applied on both sides.
//   indent()            QLabel::indent, applied on the aligned side(s) only.
//                       A NEGATIVE indent means "default", which QLabel then
//                       computes as fm.width('x')/2 - margin, and only when
//                       the label has a frame -- both conditions reproduced
//                       here rather than assumed away, because a styled
//                       TouchLabel may well have a frame.
//
// Expected to be 0 on a plain QLabel with no frame and no stylesheet, which
// is why it is LOGGED: a zero here says the clipping came from somewhere else,
// and that is a measurement rather than a silence.
static int nf_row_label_inset_px(QLabel *label, QFontMetrics const &fm) {
    QMargins cm = label->contentsMargins();
    int inset = cm.left() + cm.right();

    int margin = label->margin();
    inset += 2 * margin;

    int indent = label->indent();
    if (indent < 0 && label->frameWidth())
        indent = fm.width(QLatin1Char('x')) / 2 - margin;
    if (indent > 0) {
        Qt::Alignment a = label->alignment();
        if (a & Qt::AlignLeft)
            inset += indent;
        if (a & Qt::AlignRight)
            inset += indent;
    }
    return inset;
}

// The px this row's LEADING icon markup costs, so the name can be elided to
// what is actually left. Without this the elision is off by the icon on every
// single row.
//
// The image icons report their ACTUAL width, read back off the PNG this file
// wrote (nf_icons_generate), and no longer NF_ICON_PX standing in for it. The
// two happen to be equal today -- every icon in the set is drawn square at
// NF_ICON_PX -- and the point is that nothing here DEPENDS on that: change a
// shape's canvas and the elision follows it, with no second place to remember
// to update. That stand-in was measurably wrong before this set existed: the
// borrowed folder arrow was 15x26, so at height=40 it rendered ~23 px wide
// and this function charged 40, over-reserving ~17 px on every folder row.
//
// The two-form badge (nf_icon_badge, nffmt.cc) is measured EXACTLY, in this
// row's own font, because it is text -- which is also why the plain form
// exists at all.
static int nf_icon_width_px(nf_icon_kind kind, QFontMetrics const &fm) {
    nf_icons_generate();

    nf_icon_file const *e = nf_icon_entry(kind);
    if (e && e->ok)
        // Plus nf_icon_markup's own trailing separator, measured as the
        // NON-BREAKING SPACE the markup really emits (`&nbsp;`, U+00A0) and
        // not as the ASCII space this line used to charge. Two different
        // characters with two different advances in the same font: charging
        // the wrong one is a small, per-row, always-in-the-same-direction
        // shortfall, and it is the kind of error that adds to exactly the
        // one-or-two-characters-of-clipping seen on 2026-09-04. The row's
        // suffixes have the same fix on their own side (nf_row_suffix,
        // nffmt.cc, spells its separators with nf_nbsp() for this reason).
        return e->w + fm.width(nf_nbsp());

    QString badge;
    nf_icon_badge(kind, NULL, &badge);
    return fm.width(badge);
}

// --- book covers -------------------------------------------------------
//
// A row shows the book's OWN cover where Nickel has already rendered one, in
// place of the type icon, at the same height the type icon's row already has.
// Everything about naming the file is pure and host-tested (nffmt.h:
// nf_clean_image_id / nf_bucket_hash / nf_cover_path, pinned against the one
// real device-measured path this project has); what is left here is the two
// things that cannot be tested off-device -- the stat that decides whether
// the file is really there, and the markup Qt 5.2.1 will actually resolve.
//
// COVERAGE IS PARTIAL AND THAT IS NORMAL, not a bug to be reported in the
// row: Nickel renders a cover only once a book has been seen in one of its
// own library views, and the reference card's Fullmetal Alchemist folder has
// 27 volumes and 16 covers. So a missing cover falls back to the type icon
// SILENTLY -- no placeholder, no per-row log line. A row that said "cover
// missing" would say it eleven times out of twenty-seven on a perfectly
// healthy device and would train the owner to ignore it. The diagnosis lives
// in the per-listing tally instead (nf_browser_go).
//
// STAT, NEVER OPEN. QFile::exists is one stat and holds nothing; CLAUDE.md
// forbids holding a file handle on /mnt/onboard for more than a few hundred
// milliseconds, because a USB session while one is open risks corrupting the
// owner's card. Nothing here ever WRITES to the card either -- which is one
// of the two reasons VolumeManager::imagePathsForVolume was rejected as the
// route (its not-found branch does ScopedFSWrite + QDir::mkpath, i.e. a write
// to /mnt/onboard from a render loop; the other reason is 13 QDir round trips
// per row). The full comparison of the four candidate routes is in
// .superpowers/sdd/v2-features/cover-path-archaeology.md.

// One line per run for the FIRST cover path this process computes: the bucket
// it landed in, and whether the file was there. That is the whole scheme in
// one line and it is reproducible off-device, which is why it logs the bucket
// and the LENGTH rather than the path -- nh_log truncates at 256 bytes
// silently and these paths run past 200 characters (CLAUDE.md).
static bool nf_cover_first_logged = false;

// Set if the RAW ImageId named nothing and the MANGLED form named a real file.
// See nf_cover_path_for_row for why the second attempt exists at all. Logged
// loudly and once, because it would mean the archaeology's central inference
// (that the ImageId column already holds the mangled ContentID) is wrong --
// the covers would still render, but the reason recorded everywhere in this
// project would need correcting.
static bool nf_cover_mangle_fallback_logged = false;

// The absolute path to this row's cover, or an EMPTY QString if it has none.
// Empty is the ordinary answer, not a failure -- see the block comment above.
static QString nf_cover_path_for_row(nf_row const& r) {
    // A folder has no Volume and therefore no ImageId; a file with no library
    // row has no ImageId either, and the archaeology is explicit that no cover
    // path should even be BUILT for one -- the row already says
    // "[not in library]", which is the more useful thing to show.
    if (r.isDir || !r.hasRow || r.imageId.isEmpty())
        return QString();

    // fromUtf8, not fromLatin1 and not the raw bytes: the column is UTF-8
    // (Content::getImageId converts this same field with
    // QString::fromUtf8_helper) and nf_bucket_hash runs over UTF-16 code
    // units. Hashing the bytes would give the identical answer for every
    // ASCII name on the card and a wrong one for every Cyrillic name --
    // silently, with no error anywhere. tests/test_nffmt.cc has the vector
    // that tells the two apart.
    QString id   = QString::fromUtf8(r.imageId);
    QString path = nf_cover_path(id);

    bool exists = !path.isEmpty() && QFile::exists(path);

    if (!nf_cover_first_logged) {
        nf_cover_first_logged = true;
        unsigned h = nf_bucket_hash(id);
        nh_log("covers: imageId len=%d bucket=%u/%u type=%s exists=%d (path not logged -- nh_log truncates at 256 and this one is longer)",
               r.imageId.size(), h & 0xffu, (h >> 8) & 0xffu, NF_COVER_TYPE, exists ? 1 : 0);
    }

    if (exists)
        return path;

    // SECOND ATTEMPT, and the only reason it exists: the archaeology's single
    // unmeasured assumption is that ATTRIBUTE_IMAGE_ID already holds the
    // MANGLED ContentID (Image::fileNameForType memcpys the id in verbatim,
    // and Image::cleanId is called only from the import/parse paths). If that
    // is wrong for some row, the raw form above names nothing and the cleaned
    // form is what Nickel's own filename would have been. Both forms are
    // existence-checked, so this can only ever turn a MISSING cover into a
    // found one -- never a wrong image.
    //
    // It costs nothing in the expected case: an already-mangled id is
    // unchanged by cleaning (nf_clean_image_id is idempotent, host-tested),
    // the `!=` below is then false, and no second stat happens at all.
    QString cleaned = nf_clean_image_id(id);
    if (cleaned != id) {
        QString alt = nf_cover_path(cleaned);
        if (!alt.isEmpty() && QFile::exists(alt)) {
            if (!nf_cover_mangle_fallback_logged) {
                nf_cover_mangle_fallback_logged = true;
                nh_log("covers: the RAW ImageId named no file but its MANGLED form did -- ATTRIBUTE_IMAGE_ID does NOT already hold the mangled ContentID on this firmware, contrary to the archaeology's inference; covers still render, the record needs correcting");
            }
            return alt;
        }
    }

    return QString();
}

// THE ROW'S LEADING IMAGE, decided ONCE: the markup that will be emitted and
// the width the name budget must be charged, out of one function so the two
// can never be measured off different things.
//
// That is not tidiness. NOTES.md Task 13 records at length what happens when
// a width term is measured in units the row does not render in -- rows clipped
// their last character or two, and the cause took a full push-and-restart to
// find, because the arithmetic and the markup were assembled in different
// places. A cover is 51 px wide where a type icon is 40, so a reserve that
// still charged the icon would clip every cover row by 11 px plus the
// separator.
static QString nf_row_leading_markup(nf_icon_kind kind, QString const& coverPath,
                                     QFontMetrics const& fm,
                                     int *outWidth, bool *outIsCover) {
    if (!coverPath.isEmpty()) {
        // CAP THE HEIGHT AT THE FONT'S ASCENT, measured here rather than
        // assumed. An inline <img> sits on the TEXT BASELINE, so the line it
        // is on grows to `max(ascent, imageHeight) + descent`. While the image
        // is no taller than the ascent the row keeps exactly the height a
        // text-only row has; one pixel beyond it and every cover row grows,
        // which is not a cosmetic difference:
        //
        //   - device-measured 2026-09-27 at NF_COVER_H_PX = 76, cover rows ran
        //     ~105 px against ~75 px for icon rows, and the label's own
        //     descenders were CLIPPED by the row below -- v01..v04 rendered
        //     with the bottom half of the text sheared off while the icon rows
        //     beside them were fine.
        //   - NF_COVER_H_PX (76) was never a measurement. It came from
        //     eyeballing row spacing in a screenshot, and the implementer said
        //     so at the time; this is the correction.
        //
        // The cap is deliberately NOT a smaller hardcoded constant: the font
        // is FontSizeAdjustingLabel's and can differ from the one this code
        // would guess, so the only number that cannot drift is the one read
        // off the metrics actually in use.
        // NOT capped at fm.ascent() any more, deliberately. Capping was the
        // first fix for cover rows clipping their own text, and it worked --
        // but ascent is 46 px on this device, which made the cover 31x46, so
        // close to the type icon it replaced that the feature stopped earning
        // its row. The owner chose the other fix: let the row grow and take
        // the cost in items per page (12 -> 9, NF_ITEMS_PER_PAGE).
        //
        // So the row IS taller than a text row now, by design, and the page
        // size is what absorbs it. `fm` stays a parameter because the charged
        // width below still measures the separator with it.
        int h = NF_COVER_H_PX;
        int w = nf_cover_width_px(h);
        *outWidth   = w + fm.width(nf_nbsp());
        *outIsCover = true;
        // A BARE ABSOLUTE PATH, no scheme -- device-measured on this exact
        // firmware for the icon PNGs and identical here: Qt 5.2.1 renders
        // `<img src="/mnt/...">` and draws its own broken-image placeholder
        // for `file:///mnt/...`. Host Qt 5.15 renders both, so the host build
        // cannot catch a regression here (NOTES.md, Task 13). Do not "fix"
        // this into a URL.
        //
        // ESCAPED, unlike the icon paths, and this is the one real difference
        // between the two: an icon path is a literal this file wrote, while a
        // cover path contains the BOOK'S OWN NAME off the card. Image::cleanId
        // replaces only '/', ':', '.' and space, so a '"' or '&' in a filename
        // survives into the ImageId and would either terminate the attribute
        // early or be eaten as an entity. toHtmlEscaped covers exactly those
        // characters, and Qt's own parser resolves them back when it opens the
        // file.
        //
        // WIDTH AND HEIGHT ARE FORCED, and that is what keeps the 12-rows-per-
        // page budget: an unsized <img> lays out at the JPEG's native 149x223
        // and would nearly triple the row height, silently costing items off
        // every page (NF_ITEMS_PER_PAGE's own comment, and NF_COVER_H_PX's in
        // nffmt.h, for why that number is the owner's and what to do if a
        // screenshot shows fewer than 12).
        return QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\">&nbsp;")
                   .arg(coverPath.toHtmlEscaped()).arg(w).arg(h);
    }

    // No cover: the type icon, exactly as before this feature existed. The
    // kind is passed in rather than derived here because the caller has
    // already computed it off r.name (and needs it for nothing else), and
    // because a cover row must still HAVE a kind -- if this row's cover file
    // ever stops existing, the fallback is a correctly-typed icon and not a
    // question this function has to re-answer.
    *outWidth   = nf_icon_width_px(kind, fm);
    *outIsCover = false;
    return nf_icon_markup(kind);
}

// --- the two chrome bars -----------------------------------------------
//
// The chrome used to be FIVE full-width rows stacked above the items --
// "<< BACK", "page N/M", "NEXT PAGE >", "sort: name ^", "filter: all" -- one
// TouchLabel each, one per line of the panel. It is now two horizontal bars:
// a command bar across the top (BACK | sort | filter) and a page bar pinned
// to the bottom (PREV | page N/M | NEXT). Three rows of panel come back,
// which is what paid for NF_ITEMS_PER_PAGE going 9 -> 11 (see its own
// arithmetic above; against 99 px cover rows those 225 px buy two items, not
// three).
//
// EACH BAR ITEM IS ITS OWN TouchLabel, never one label with tappable regions,
// and that is not a style preference: Nickel does not deliver touch as Qt
// mouse events at all, so there is no coordinate to test a region against.
// It reads the panel itself and dispatches its own gestures, and a widget
// needs all three of grabGesture(), an event() override routing QEvent
// 194-196/209/198, and GestureDelegate-named RTTI to be in that path.
// TouchLabel self-registers for exactly that in its own constructor, which
// is why every tappable thing in this file is one and why "one label, three
// hot zones" is not an option here (this file's header comment, and
// NOTES.md's "Task 8: touch input archaeology").
//
// A HORIZONTAL LAYOUT IS THE ONE GENUINELY NEW MECHANISM on this screen --
// every widget this project has put on the panel so far has been a
// full-width row in a QVBoxLayout -- so nf_log_bar_geometry (below) logs
// each item's x/width/height once per listing build. Its failure modes are
// all silent ones: items stacked at x=0, items at zero width, a bar
// collapsed to nothing. On a screenshot those are indistinguishable from
// "the bar did not render"; in the log they are three different lines.

// Allocates, constructs and wires ONE tappable TouchLabel, returning it as
// the QLabel* every caller here needs anyway (setText/setAlignment are
// QLabel's OWN, ABI-stable, already-linked functions -- see this file's
// header comment on casting an opaque Nickel pointer to a real Qt base).
//
// One copy of the allocation size, the NULL check and the old-style signal
// connect, rather than the five the two bars would otherwise need. The item
// row loop in nf_browser_go deliberately keeps its own copy of this shape:
// both of its failure logs name the row by index AND filename, which a
// shared `char const *what` cannot carry, and those two lines are how a
// device run says WHICH row went missing.
//
// `what` is always a string literal from the call site. Returns NULL, having
// logged, if the allocation failed -- every caller treats that as "this one
// control is missing", never as fatal (CLAUDE.md: NickelHook's failsafe is
// SHARED infrastructure, and a mod that fails hard can make the owner's
// other mods uninstall themselves). *outShim receives the hidden QPushButton
// whose clicked() the caller connects its own lambda to.
static QLabel *nf_new_touch_row(QWidget *parent, char const *what, QPushButton **outShim) {
    // 132 bytes measured at TouchLabel's own construction call sites
    // (NOTES.md); 256 is this project's usual over-allocation margin for a
    // Nickel object whose own size we cannot ask -- and NickelHardcover's
    // shipped calloc(1, 128) for this same class is a live 4-byte overflow,
    // which is why the number here is measured rather than borrowed.
    // calloc, not ::operator new: Qt eventually deletes this widget itself
    // through its own real vtable, and glibc's calloc/malloc and libstdc++'s
    // default operator new/delete share the same underlying allocator -- the
    // same assumption every TouchLabel allocation in this project ships on.
    void *row = calloc(1, 256);
    if (!row) {
        nh_log("browser: calloc(1,256) failed for the %s control, skipping it", what);
        return NULL;
    }
    TouchLabel__ctor(row, parent, 0);

    // The signal-adaptor trick (NickelMenu, src/nickelmenu.cc): a hidden
    // QPushButton relays TouchLabel's own, real, old-style tapped(bool)
    // signal to a plain capturing lambda, so this project needs no moc step.
    // Old-style string connects are not compile-checked, so a failure is
    // logged loudly rather than silently doing nothing.
    QPushButton *shim = new QPushButton(parent);
    shim->setVisible(false);
    if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
        nh_log("browser: connecting the %s control's tapped(bool) failed -- it will silently do nothing", what);

    *outShim = shim;
    return reinterpret_cast<QLabel*>(row);
}

// One bar item's identity for the geometry log. POD, and only ever a LOCAL of
// nf_browser_go -- nothing at file scope, per this file's no-dynamic-
// initialiser rule.
struct nf_bar_item {
    QWidget    *w;
    char const *name;
};

// Four is what the command bar will want once file operations land (BACK,
// sort, filter, plus whatever they add); six leaves both bars room to grow
// without this becoming the thing that has to be edited. Overflow is dropped
// from the LOG only, never from the bar -- see nf_bar_record.
#define NF_BAR_MAX_ITEMS 6

static void nf_bar_record(nf_bar_item *items, int *n, QWidget *w, char const *name) {
    if (*n >= NF_BAR_MAX_ITEMS)
        return; // logging is best-effort; the widget is already in the layout
    items[*n].w    = w;
    items[*n].name = name;
    (*n)++;
}

// One line per bar per listing build: every item's x, width and height, as
// the layout actually resolved them.
//
// WHICH LAYOUT PASS these numbers came from is the whole reason the line
// carries a marker, and the precedent is exact: N3Dialog::width() returns 600
// before the dialog is laid out and 1264 after, and 600 is "a plausible-
// looking number that announces nothing" (CLAUDE.md). The same trap is here,
// one level down -- a bar read before layout hands back Qt's defaults, which
// look like measurements. So the content widget's own width decides the
// marker, on the same NF_WIDTH_PLAUSIBLE_MIN_PX floor nf_row_width_px uses.
//
// The FIRST listing of a session is always PRE-LAYOUT: nf_browser_show calls
// nf_browser_go before pushView, so nothing has been sized to the panel yet.
// Every later navigation is MEASURED. A run whose lines are all PRE-LAYOUT
// means setContent is not laying the content out, which is itself the finding.
static void nf_log_bar_geometry(char const *bar, nf_bar_item const *items, int n, int contentW) {
    // 160, not 256: nh_log truncates at 256 bytes SILENTLY (CLAUDE.md), and
    // the prefix below spends some of that. Three items cost ~75 characters.
    char line[160];
    line[0] = '\0';
    int off = 0;
    for (int i = 0; i < n; i++) {
        int room = (int)sizeof line - off;
        if (room <= 1)
            break;
        int wrote = snprintf(line + off, (size_t)room, "%s%s x=%d w=%d h=%d",
                             i ? " | " : "", items[i].name,
                             items[i].w->x(), items[i].w->width(), items[i].w->height());
        // snprintf returns what it WOULD have written, which can exceed the
        // room it had -- advancing by that would run `off` past the buffer.
        if (wrote < 0 || wrote >= room)
            break;
        off += wrote;
    }
    nh_log("browser: %s bar -- %s (content widget %d px): %s", bar,
           contentW >= NF_WIDTH_PLAUSIBLE_MIN_PX ? "MEASURED" : "PRE-LAYOUT",
           contentW, n ? line : "(no items -- every allocation failed)");
}

// A bar's own QHBoxLayout, with the two settings that decide whether it is
// ONE row tall.
//
// ZERO CONTENTS MARGINS, deliberately and not for tidiness: this firmware's
// default layout margins are 34 px a side (queried, and printed in the row-
// width log line below -- 1264 -> 1196), and a NESTED layout gets its own
// copy of them. Left at the default, each bar would be inset by another 34
// px top and bottom, i.e. ~68 px taller than the row it contains -- which
// would quietly break NF_ITEMS_PER_PAGE's arithmetic, since that counts each
// bar as one 75 px row. The bars sit inside the outer QVBoxLayout, which
// already pays the horizontal margins for them.
//
// ZERO SPACING for a different reason: the separation between items comes
// from the equal-width slots each item is given (addWidget(w, 1)) and from
// each item's own text alignment, so a spacing here would be an invented
// layout constant doing nothing the slots do not already do -- and every
// invented layout constant in this project has so far been wrong.
static QHBoxLayout *nf_new_bar_layout(void) {
    QHBoxLayout *bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(0);
    return bar;
}

// Puts one finished item into a bar: an equal-width slot (stretch 1, so the
// slots do not move when a label's text changes -- "< PREV" becoming "no
// prev" must not shift the page counter beside it), with the item filling its
// slot so the TAP TARGET is the whole third rather than just the glyphs, and
// the text aligned within it to give the bar its left/centre/right reading.
//
// Alignment is set on the LABEL, not passed to addWidget: passing it to
// addWidget shrinks the widget to its sizeHint inside the slot, which would
// make every bar control a small target on a panel operated with a finger.
static void nf_bar_add(QHBoxLayout *bar, nf_bar_item *items, int *n,
                       QLabel *item, char const *name, Qt::Alignment align) {
    item->setAlignment(align | Qt::AlignVCenter);
    bar->addWidget(item, 1);
    nf_bar_record(items, n, item, name);
}

// Builds a fresh content widget (rows for `path`'s own directory listing)
// and swaps it into the ALREADY-EXISTING `dialog` via N3Dialog::setContent
// -- this is the whole navigation model (nfview.h): one N3Dialog for the
// lifetime of a browse session, rows rebuilt in place, never a second
// dialog pushed per level. setContent itself deleteLater()s whatever
// content was there before (nfnickel.h), so the previous screen's rows and
// their shim buttons are cleaned up by Qt, not by this function.
static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path, bool resetPage) {
    // Recorded BEFORE anything below can fail, so BACK's own "where am I"
    // read is always this directory once this function has been entered --
    // matching every row/BACK handler being wired only after the listing
    // for THIS path has been built, never before.
    snprintf(nf_browser_cwd, sizeof nf_browser_cwd, "%s", qPrintable(path));

    // See nf_browser_page's own comment: every real navigation (descend,
    // ascend, the initial root call) passes resetPage=true here; only the
    // page bar's PREV/NEXT pass false, because they call back into this
    // SAME function for the SAME path just to render a different slice of
    // the same listing.
    if (resetPage)
        nf_browser_page = 0;

    QString const *db = nf_db_name();
    NFMetaCtx ctx;
    ctx.dirPath = path;
    ctx.dbName  = db ? *db : QString();

    QVector<nf_entry> raw = nf_browser_scan_dir(path);
    QVector<nf_row> rows;
    // filteredToNothing distinguishes spec section 6.3/3.6's third empty
    // state -- see nf_build_listing's own derivation (nflist.cc) and the
    // message built below, right before the item-row loop, for how it is
    // shown: a folder that HAD books before nf_browser_filter ran must not
    // read the same as one that never had anything.
    bool filteredToNothing = false;
    nf_build_listing(raw, &nf_row_meta, &ctx, &rows,
                     nf_browser_filter, nf_browser_sort_key, nf_browser_sort_desc,
                     &filteredToNothing);

    // COVER PATHS, resolved for the WHOLE listing and not just for the twelve
    // rows this page will draw. Two reasons, and the second is the important
    // one:
    //
    //   - the row loop below then does no stat of its own, so what the tally
    //     counted and what the rows render are the same decision rather than
    //     two that could drift;
    //   - the TALLY only means something over the whole listing. The
    //     non-vacuous check for this feature is arithmetic, the same shape as
    //     the read-state filters' (NOTES.md, Task 13): in the reference card's
    //     Fullmetal Alchemist folder, 16 covers + 10 files with a library row
    //     and no cover file + 1 file with no library row = 27 entries. A
    //     per-page tally could never balance against a number like that, and
    //     "some rows have covers" is an impression rather than a measurement.
    //
    // One stat per FILE row per navigation (folders and rows with no library
    // row are skipped before any path is built). That is cheap next to the
    // getById round trip each of those rows already made, and it holds no
    // handle -- see nf_cover_path_for_row.
    QVector<QString> coverPaths(rows.size());
    int nCover = 0, nNoCoverFile = 0, nNoRow = 0, nDirs = 0, nBlankImageId = 0;
    for (int i = 0; i < rows.size(); i++) {
        nf_row const &cr = rows.at(i);
        if (cr.isDir) {
            nDirs++;
            continue;
        }
        if (!cr.hasRow) {
            nNoRow++;
            continue;
        }
        coverPaths[i] = nf_cover_path_for_row(cr);
        if (coverPaths.at(i).isEmpty()) {
            nNoCoverFile++;
            // Counted separately because it is a DIFFERENT fact: a blank
            // ImageId means the library row itself names no image, where an
            // empty path with a non-blank id means the JPEG has not been
            // rendered yet. If this number ever equals nNoCoverFile for a
            // whole card, the ImageId read is what to look at, not the path
            // scheme.
            if (cr.imageId.isEmpty())
                nBlankImageId++;
        } else {
            nCover++;
        }
    }
    // The four buckets partition the listing exactly -- nCover +
    // nNoCoverFile + nNoRow + nDirs == rows.size() -- which is what makes
    // this checkable at a glance instead of merely informative. The total is
    // printed alongside them so a partition that stops adding up is visible
    // in the same line rather than needing a second one.
    nh_log("covers: %d shown, %d file(s) with a row but no cover file (%d of those with a blank ImageId), %d file(s) with no library row, %d folder(s) -- %d row(s) total",
           nCover, nNoCoverFile, nBlankImageId, nNoRow, nDirs, rows.size());

    // Pagination bounds. totalPages is at least 1 even for an empty listing,
    // so "page 1/1" (below) is always a sensible thing to compute, never a
    // divide-by-zero. nf_browser_page is clamped defensively -- it should
    // already be in range by construction (resetPage zeroes it on every
    // directory change, and PREV/NEXT below never step it out of range),
    // but a stale value surviving some path this file does not currently
    // have is a clamp, not a crash, which is cheap insurance to keep.
    int totalPages = (rows.size() + NF_ITEMS_PER_PAGE - 1) / NF_ITEMS_PER_PAGE;
    if (totalPages < 1)
        totalPages = 1;
    if (nf_browser_page >= totalPages)
        nf_browser_page = totalPages - 1;
    if (nf_browser_page < 0)
        nf_browser_page = 0;
    int startIdx = nf_browser_page * NF_ITEMS_PER_PAGE;
    int endIdx   = qMin(startIdx + NF_ITEMS_PER_PAGE, rows.size());
    // hasPrev/hasNext are NOT computed here any more: the page bar asks
    // nf_page_bar_labels (nffmt.h) for both the labels and the two active
    // flags in one call, so "is this end live" has one answer, made in the
    // one place a host test can reach it.

    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    // The width every item row's label is elided against -- see
    // nf_row_width_px for both paths and why the dialog is what gets read.
    //
    // Logged ONCE PER NAVIGATION rather than once per process (the way
    // nf_icons_generate is) precisely BECAUSE the answer changes: the
    // first listing is built before pushView has sized the dialog, so it is
    // always the fallback, and a once-per-process log would therefore only
    // ever record the fallback and never the real measurement. One line per
    // navigation is still one line, not one per row, and the transition from
    // fallback to measured is visible in the log rather than invisible.
    char const *widthSource = "unset";
    int  rawDialogWidth     = 0;
    int  rowWidth           = nf_row_width_px(dialog, layout, &widthSource, &rawDialogWidth);
    QMargins layoutMargins  = layout->contentsMargins();
    nh_log("browser: row width %d px -- %s (N3Dialog::width() read back %d, last content widget %d, our layout margins %d+%d, panel clamp %d)",
           rowWidth, widthSource, rawDialogWidth,
           nf_browser_active_content
               ? reinterpret_cast<QWidget*>(nf_browser_active_content)->width() : -1,
           layoutMargins.left(), layoutMargins.right(),
           NF_PANEL_VISIBLE_WIDTH_PX);

    // --- THE COMMAND BAR, one row across the top ------------------------
    //
    //     < BACK        sort: name ^        filter: all
    //
    // Three independently tappable TouchLabels in one horizontal layout,
    // where there used to be three full-width rows (plus the page indicator
    // and NEXT PAGE, now in the bottom bar). See the "two chrome bars"
    // comment above nf_new_touch_row for why each item must be its own
    // TouchLabel rather than one label with hot zones.
    //
    // The `|` separators in the brief's sketch are NOT drawn: a literal "|"
    // would either be its own TouchLabel (a tap target that does nothing) or
    // live inside a neighbour's text (widening that control's label for no
    // reason). The separation is the three equal-width slots and the
    // left/centre/right text alignment instead.
    //
    // ORDER AND LABELS ARE UNCHANGED from the stacked rows this replaces --
    // BACK, then sort, then filter, with the same strings and the same cycle
    // on each tap. The owner has learned those tap sequences on hardware;
    // this task moves where the controls sit, not what they do.
    //
    // ROOM FOR MORE, conceptually: file operations are the next task, and a
    // fourth item drops into this bar as another equal slot with no
    // arithmetic to redo (NF_ITEMS_PER_PAGE counts bars, not bar items).
    // Nothing is reserved for them here -- an empty placeholder control would
    // be a tap target that does nothing.
    QHBoxLayout *cmdBar   = nf_new_bar_layout();
    nf_bar_item  cmdItems[NF_BAR_MAX_ITEMS];
    int          nCmdItems = 0;

    // BACK: the GUARANTEED exit, independent of N3Dialog's own backTapped()
    // signal (wired once, in nf_browser_show, to this exact same
    // nf_browser_back) -- review finding I-3, carried over from the
    // trivial-screen milestone: getDialog wires the dialog's X
    // (closeTapped()) to a controller-stack call pushView never populates, so
    // the X does nothing on this route (see N3Dialog__disableCloseButton
    // below, which removes it). If backTapped() ALSO failed to fire for any
    // reason, this screen would have no way off it short of a power cycle, on
    // the owner's daily-use device -- this control does not depend on
    // N3Dialog's own signal at all, so it is the one most worth trusting if
    // anything else here is wrong.
    //
    // FIRST in the bar, and still the first thing built, for that reason.
    // Routed through nf_browser_back, the SAME function backTapped() calls --
    // up one level, popping the dialog only at the root -- so there are not
    // two forks of that logic to audit for drift.
    {
        QPushButton *shim = NULL;
        QLabel *item = nf_new_touch_row(content, "BACK", &shim);
        if (item) {
            item->setText(QStringLiteral("< BACK"));
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog] {
                nf_browser_back(mwc, dialog);
            });
            nf_bar_add(cmdBar, cmdItems, &nCmdItems, item, "BACK", Qt::AlignLeft);
        } else {
            nh_log("browser: no BACK control this time (backTapped()/the back arrow is still wired)");
        }
    }

    // Sort and filter. Tapping either changes what THIS directory shows,
    // which is a bigger change to the row set than a page turn -- so unlike
    // PREV/NEXT (resetPage=false, same directory, different slice), both of
    // these pass resetPage=TRUE: a filter can turn a 3-page listing into a
    // 1-page one, and landing on whatever page NUMBER happened to be current
    // would be an arbitrary slice of a now-different listing rather than the
    // meaningful "same place" it is for BACK/descend.
    {
        QPushButton *shim = NULL;
        QLabel *item = nf_new_touch_row(content, "sort", &shim);
        if (item) {
            item->setText(nf_sort_row_label());
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_cycle_sort();
                nh_log("browser: sort -- now %s", qPrintable(nf_sort_row_label()));
                nf_browser_go(mwc, dialog, path, true); // resetPage -- see this block's own comment
            });
            nf_bar_add(cmdBar, cmdItems, &nCmdItems, item, "sort", Qt::AlignHCenter);
        }
    }
    {
        QPushButton *shim = NULL;
        QLabel *item = nf_new_touch_row(content, "filter", &shim);
        if (item) {
            item->setText(nf_filter_row_label());
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_cycle_filter();
                nh_log("browser: filter -- now %s", qPrintable(nf_filter_row_label()));
                nf_browser_go(mwc, dialog, path, true); // resetPage -- see this block's own comment
            });
            nf_bar_add(cmdBar, cmdItems, &nCmdItems, item, "filter", Qt::AlignRight);
        }
    }

    // ABOVE every item row, never interleaved with them -- the same principle
    // the stacked chrome followed ("whatever gets clipped first should be the
    // least useful row"), now with only two things that could ever be clipped.
    layout->addLayout(cmdBar);

    // Spec sections 3.6/6.3: an empty ROW SET reads one of two ways, and
    // conflating them tells a reader who filtered to PDF and got nothing
    // that their books are gone rather than that their filter matched
    // nothing. A plain QLabel, like the page counter in the bar below --
    // informational only, not a tap target. Says WHICH filter is active so
    // the fix (tap "filter: ..." in the command bar until it reads "all") is
    // discoverable from this message alone.
    //
    // Spec section 3.6's OTHER distinction -- "empty" versus "cannot be listed at all"
    // (a read failure) -- is NOT built here: nf_browser_scan_dir (above)
    // has no readability check of its own, so a genuinely unreadable
    // directory today looks identical to an empty one, same as before this
    // task. Not addressed here because it is a pre-existing gap this task
    // was not asked to close, not a regression this task introduced.
    if (rows.isEmpty()) {
        QLabel *emptyMsg = new QLabel(content);
        emptyMsg->setText(filteredToNothing
            ? QStringLiteral("Everything here was filtered out (%1).").arg(nf_filter_row_label())
            : QStringLiteral("This folder is empty."));
        layout->addWidget(emptyMsg);
    }

    // One log line per listing for the width-correction pass at the end of
    // the loop, not one per row -- see there.
    bool loggedOverflow = false;

    for (int i = startIdx; i < endIdx; i++) {
        nf_row const &r = rows.at(i);

        void *row = calloc(1, 256);
        if (!row) {
            nh_log("browser: calloc(1,256) failed for row %d ('%s'), skipping it", i, qPrintable(r.name));
            continue;
        }
        TouchLabel__ctor(row, content, 0);

        // Labels are OURS -- nf_strip_common (nffmt.cc) already did the
        // work; this only adds a leading icon (nf_icon_markup, above), a
        // per-row suffix that nf_build_listing does not itself carry an
        // opinion about, and the middle-elision that keeps a long name from
        // running off the right edge of the panel (see the row-label elision
        // block above for why the middle and not the right).
        //
        // The suffix is built by nf_row_suffix (nffmt.cc), in TWO forms
        // before either one is used: the markup that actually gets appended,
        // and a plain-text twin whose only job is to be MEASURED. The twin
        // exists because the suffixes are what the row MEANS -- "[not in
        // library]" on Fullmetal Alchemist v26 is the whole point of that row
        // -- so they must never be what elision spends its budget on: the
        // suffix is measured and paid for FIRST, and the name is elided into
        // whatever is left, so the suffix survives by construction rather
        // than by hoping the name was short enough.
        //
        // Both forms, and the reason they are the same characters rather than
        // merely the same words, are in nffmt.cc -- that is also where the
        // 2026-09-04 measurement that moved them there is recorded.
        QString suffixMarkup;
        QString suffixPlain;
        nf_row_suffix(r, &suffixMarkup, &suffixPlain);

        QLabel *rowLabel = reinterpret_cast<QLabel*>(row);

        // The icon is keyed off r.name, the on-disk name -- NOT r.label,
        // which nf_strip_common may have stripped the extension clean off
        // (it removes a common one deliberately, nffmt.cc), leaving nothing
        // for nf_icon_kind_for to read.
        nf_icon_kind kind = nf_icon_kind_for(r.name, r.isDir);

        // THE ROW'S OWN FONT, read back off the widget Nickel's own
        // TouchLabel constructor just finished initialising -- not a
        // default-constructed QFont, and not the application font, either of
        // which would measure text in a size this row does not render in. The
        // one caveat, recorded rather than papered over: TouchLabel derives
        // from FontSizeAdjustingLabel, which may adjust its own point size
        // when text is set, i.e. AFTER this read. There is no way to ask it
        // for the post-adjustment font before giving it the text, so this
        // measures the pre-adjustment one; a screenshot is what says whether
        // that gap matters at all on this panel.
        QFontMetrics fm(rowLabel->font());

        // THE NAME'S BUDGET, and every term subtracted from it, in order:
        //
        //   rowWidth      the width of the row WIDGET (nf_row_width_px):
        //                 the content area the dialog laid out, or the
        //                 dialog, or the 1264 px panel constant -- less our
        //                 own QVBoxLayout's queried 34+34 margins.
        //   labelInset    what the TouchLabel spends on itself before any of
        //                 our text is drawn (nf_row_label_inset_px): its
        //                 contents margins, its QLabel margin, its indent.
        //                 Read once per row rather than hoisted, because it
        //                 depends on the widget and on fm, and a row is not
        //                 required to be styled like its neighbours.
        //   icon          the <img> box, at the width READ BACK off the PNG
        //                 this file wrote, plus the U+00A0 separator that
        //                 follows it, measured in this row's own font
        //                 (nf_icon_width_px).
        //   suffixPlain   the twin of the suffix that will really be
        //                 appended, measured in this row's own font --
        //                 character for character what gets rendered
        //                 (nf_row_suffix, nffmt.cc).
        //
        // The floor and the reason for it are in nf_name_budget_px (nffmt.cc)
        // -- pure, so both are host-tested.
        int labelInset  = nf_row_label_inset_px(rowLabel, fm);
        int textWidth   = rowWidth - labelInset; // the label's own text area
        // The leading image and the width it costs, from ONE call -- a cover
        // is 51 px wide where a type icon is 40, and charging the wrong one
        // is exactly the class of mismeasurement NOTES.md Task 13 records
        // (see nf_row_leading_markup).
        int  leadWidth = 0;
        bool leadIsCover = false;
        QString leading = nf_row_leading_markup(kind, coverPaths.at(i), fm,
                                                &leadWidth, &leadIsCover);
        int suffixWidth = fm.width(suffixPlain);
        int nameWidth   = nf_name_budget_px(textWidth, leadWidth, suffixWidth);

        // ONE line per navigation, not per row: every term above except the
        // suffix is the same on every row of a listing, and the first row's
        // arithmetic is what says whether a clipped screenshot means a wrong
        // width or a broken elision. Logged for the FIRST item row of the
        // page (i == startIdx), where the alternative -- logging every row --
        // would be 12 lines a navigation and, at nh_log's silent 256-byte
        // truncation, would push the lines that matter out of view.
        //
        // The leading term now NAMES which of the two it is: a cover and a
        // type icon are different widths, so a budget line that only said
        // "icon" could not be checked against what the row actually drew.
        if (i == startIdx)
            nh_log("browser: name budget %d px = row %d - label inset %d - %s %d - suffix %d ('%s')",
                   nameWidth, rowWidth, labelInset,
                   leadIsCover ? "cover" : "icon", leadWidth, suffixWidth,
                   qPrintable(suffixPlain));

        // ELIDED FIRST, THEN ESCAPED, THEN the markup joins it, and every
        // step of that order is load-bearing:
        //
        //   - Elide before escape, because escaping turns one "&" into five
        //     characters ("&amp;") that Qt would then both measure and elide
        //     as five. Names on this card do contain "&", so eliding the
        //     escaped form would cut those rows in the wrong place and could
        //     even split an entity in half, emitting "&am" into the markup.
        //   - Elide before the markup is appended, because elidedText knows
        //     nothing about tags: given the <img> the icon adds, it would
        //     happily cut through the middle of the tag itself.
        //   - Escape before the markup is appended, because r.label is a
        //     filename off the card. Escape afterwards instead and the
        //     escaping eats our OWN tags, turning every icon into visible
        //     source text.
        //
        // This project has the exact precedent for getting an ordering like
        // this wrong -- nf_strip_common's letter guard once ran AFTER the
        // extension was re-appended, which made it pass vacuously (nffmt.cc)
        // -- so it is spelled out rather than left to be re-derived.
        //
        // The suffixes are appended LAST and are safe to append after the
        // escape because they are OUR OWN literals plus one int
        // (r.percentRead), never card data.
        QString label = fm.elidedText(r.label, Qt::ElideMiddle, nameWidth).toHtmlEscaped();
        label += suffixMarkup;

        // Set EXPLICITLY rather than left at Qt::AutoText, which decides
        // text-vs-rich-text by INSPECTING THE STRING (Qt::mightBeRichText).
        // Nothing about how a row renders may depend on what a book happens
        // to be called: a name containing something tag-shaped would
        // otherwise flip the mode, in either direction, for that one row.
        rowLabel->setTextFormat(Qt::RichText);
        rowLabel->setText(leading + label);

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

        // THE SECOND PASS, AND THE ONLY ONE THAT MEASURES WHAT WILL ACTUALLY
        // BE DRAWN. Everything above is a budget assembled out of terms this
        // file can name -- the row width, the label's own inset, the <img>
        // box, the suffix twin. This asks the finished widget instead:
        // QLabel::sizeHint() on a rich-text label lays the QTextDocument out
        // at its natural width and hands back that width plus the label's own
        // insets, i.e. the number the panel will really try to draw. If it
        // exceeds the row, the difference is a term the budget did not know
        // about, and the name is re-elided by exactly that much.
        //
        // Why this exists at all: on 2026-09-04 the panel clipped rows whose
        // suffix HAD already been measured and paid for before the name was
        // elided (that reservation predates this task -- what was wrong was
        // never the ORDER, contrary to the first reading of the screenshot).
        // So at least one term was being measured differently from how it
        // renders, and the candidates were not distinguishable off-device:
        // the U+00A0 separators charged as U+0020 (fixed above), the
        // N3Dialog chrome inset that nf_row_width_px could only over-estimate
        // (now measured), the TouchLabel's own inset (now measured), the
        // <img> box, the italic stylesheet applied just above, and
        // FontSizeAdjustingLabel adjusting its own point size on setText --
        // AFTER the QFontMetrics above was read off it, which no arithmetic
        // here can anticipate. This pass does not care which one it was: it
        // measures the assembled result, so it corrects for all of them, and
        // it logs when it fires so the device run says how much was missing.
        //
        // The NAME is what gets shortened, never the suffix -- same rule as
        // the budget above. ONE retry, not a loop: a second correction would
        // be measuring the correction rather than the row, and a row that is
        // a few px short is invisible where a loop that does not converge is
        // a hung GUI thread.
        //
        // A CEILING on the correction, and it is a guard rather than a fudge
        // factor: the shortfall this pass exists to absorb is a term or two of
        // chrome, i.e. tens of px on a ~1196 px row. If sizeHint ever comes
        // back wildly larger -- a stylesheet minimum width, a hint computed on
        // some basis other than this text, a firmware whose QLabel differs --
        // then honouring it would elide EVERY row down to nf_name_budget_px's
        // 60 px floor, which is a far worse screen than the residual clipping
        // this is trying to remove. So an implausible overflow is logged and
        // NOT applied: the row keeps the first pass's label, i.e. exactly
        // today's behaviour, and the log says why.
        int hintWidth = rowLabel->sizeHint().width();
        int overflow  = hintWidth - rowWidth;
        if (overflow > rowWidth / 4) {
            if (!loggedOverflow) {
                loggedOverflow = true;
                nh_log("browser: row %d's sizeHint is %d px against a %d px row -- an implausible %d px overflow, NOT corrected (the label's hint is not measuring this text)",
                       i, hintWidth, rowWidth, overflow);
            }
        } else if (overflow > 0) {
            int corrected = nf_name_budget_px(textWidth - overflow, leadWidth, suffixWidth);
            QString reflowed = fm.elidedText(r.label, Qt::ElideMiddle, corrected).toHtmlEscaped();
            reflowed += suffixMarkup;
            rowLabel->setText(leading + reflowed);
            // Logged for the FIRST row that needs it only. The terms are the
            // same on every row of a listing, so the first one names the
            // shortfall; 12 identical lines would only push it out of the log
            // (nh_log truncates at 256 bytes, silently -- CLAUDE.md).
            if (!loggedOverflow) {
                loggedOverflow = true;
                nh_log("browser: row %d overflowed by %d px (sizeHint %d vs row %d) -- name budget %d -> %d px; a term the arithmetic does not know about",
                       i, overflow, hintWidth, rowWidth, nameWidth, corrected);
            }
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
                nf_browser_go(mwc, dialog, childPath, true); // descend -- a different directory, page resets
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

    // A STRETCH, and it is what PINS the page bar to the bottom of the
    // content area rather than letting it float under the last item row. A
    // folder with three items must still show the bar at the bottom, so the
    // reader's eye finds it in the same place in every folder -- the slack
    // goes here, between the items and the bar, instead of being shared out
    // among the rows.
    //
    // It also fixes the item rows at their natural height: a QVBoxLayout with
    // nothing expanding in it hands the spare vertical space to the widgets
    // themselves (QLabel's vertical size policy can grow), so before this the
    // rows on a short page were stretched taller than a full page's rows. Now
    // every page's rows are the same height whatever the page holds, which is
    // also what makes NF_ITEMS_PER_PAGE's 75/99 px terms mean one thing
    // rather than two.
    layout->addStretch(1);

    // --- THE PAGE BAR, one row pinned to the bottom ---------------------
    //
    //     < PREV        page 2/4        NEXT >
    //
    // The three labels come from nf_page_bar_labels (nffmt.h) -- pure and
    // host-tested, because the one real DECISION here (what the ends say when
    // there is no such page) is the part that can be tested off-device, and
    // the layout is the part that cannot.
    //
    // BOTH ENDS ARE ALWAYS PRESENT. A control that disappears on the first
    // and last page makes the bar's own layout jump as a reader pages through
    // a folder, and it leaves the page counter sliding around under their
    // thumb. So an unavailable end is rendered INERT rather than omitted:
    //   - the label loses its arrow and reads "no prev"/"no next", which is
    //     how inert is conveyed -- in the CHARACTERS, because this panel has
    //     four grey levels and "slightly lighter" does not read as
    //     "different" on it (the same finding that puts "[not in library]" in
    //     a row's text rather than leaving it to colour);
    //   - and it is built as a PLAIN QLabel, not as an unconnected
    //     TouchLabel, so it is not a tap target at all rather than one that
    //     silently does nothing. That also skips a 256-byte allocation and a
    //     gesture registration for a control that cannot act.
    //
    // The whole bar is unconditional, including on a single-page listing
    // (where it reads "no prev | page 1/1 | no next"). The stacked chrome
    // used to hide its page indicator in that case, on the grounds that
    // "page 1/1" says nothing -- but a bar that is sometimes absent makes the
    // height of the item area depend on the folder, and NF_ITEMS_PER_PAGE's
    // arithmetic counts exactly two chrome rows on every page.
    //
    // `path` is captured by value in both handlers, and nf_browser_go is
    // called with resetPage=FALSE: this is a page change WITHIN the current
    // directory, not a navigation to a different one, so nf_browser_page must
    // survive the rebuild it triggers.
    QString prevLabel, pageLabel, nextLabel;
    bool    prevActive = false, nextActive = false;
    nf_page_bar_labels(nf_browser_page, totalPages,
                       &prevLabel, &prevActive, &pageLabel, &nextLabel, &nextActive);

    QHBoxLayout *pageBar   = nf_new_bar_layout();
    nf_bar_item  pageItems[NF_BAR_MAX_ITEMS];
    int          nPageItems = 0;

    {
        QLabel *item = NULL;
        bool    live = false;
        if (prevActive) {
            QPushButton *shim = NULL;
            item = nf_new_touch_row(content, "PREV", &shim);
            if (item) {
                live = true;
                QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                    nf_browser_page--;
                    nh_log("browser: page -- prev, now %d in '%s'", nf_browser_page, qPrintable(path));
                    nf_browser_go(mwc, dialog, path, false);
                });
            }
        }
        // Two ways to get here without a live control: there IS no previous
        // page (the ordinary case -- the inert label, see this bar's own
        // comment), or the TouchLabel allocation failed (already logged by
        // nf_new_touch_row). A plain QLabel covers both, and the SLOT is held
        // open either way so a missing control never slides the counter out
        // from under the reader's eye. The failed-allocation slot is left
        // BLANK rather than labelled "< PREV": a label that looks like a
        // control and cannot receive a tap is worse than a gap.
        if (!item)
            item = new QLabel(content);
        item->setText((prevActive && !live) ? QString() : prevLabel);
        nf_bar_add(pageBar, pageItems, &nPageItems, item,
                   live ? "PREV" : (prevActive ? "prev(alloc failed)" : "prev(inert)"),
                   Qt::AlignLeft);
    }

    // The counter: informational only, never a tap target, so a plain QLabel
    // needs none of TouchLabel's gesture machinery -- same as the stacked
    // chrome's own page indicator.
    {
        QLabel *item = new QLabel(content);
        item->setText(pageLabel);
        nf_bar_add(pageBar, pageItems, &nPageItems, item, "page", Qt::AlignHCenter);
    }

    {
        QLabel *item = NULL;
        bool    live = false;
        if (nextActive) {
            QPushButton *shim = NULL;
            item = nf_new_touch_row(content, "NEXT", &shim);
            if (item) {
                live = true;
                QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                    nf_browser_page++;
                    nh_log("browser: page -- next, now %d in '%s'", nf_browser_page, qPrintable(path));
                    nf_browser_go(mwc, dialog, path, false);
                });
            }
        }
        if (!item) // inert, or a blank held-open slot -- see PREV's own comment
            item = new QLabel(content);
        item->setText((nextActive && !live) ? QString() : nextLabel);
        nf_bar_add(pageBar, pageItems, &nPageItems, item,
                   live ? "NEXT" : (nextActive ? "next(alloc failed)" : "next(inert)"),
                   Qt::AlignRight);
    }

    layout->addLayout(pageBar);

    QString title = (path == QStringLiteral(NF_ROOT))
        ? QStringLiteral("NickelFolders")
        : QFileInfo(path).fileName();
    N3Dialog__setTitle(dialog, title);

    nh_log("browser: showing '%s' (%d row(s), page %d/%d, %d shown)",
           qPrintable(path), rows.size(), nf_browser_page + 1, totalPages, endIdx - startIdx);

    // Reparents `content` into the dialog's own layout and shows it;
    // deleteLater()s whatever content was there before (nfnickel.h) --
    // which is what actually tears down the PREVIOUS screen's rows and
    // shim buttons. Must be the last thing this function MODIFIES about
    // `content`/`layout`/the rows just built: nothing here may be added to,
    // reparented or re-set afterward. What follows is read-only -- a pointer
    // remembered, a destroyed() connect, and the two bar-geometry lines,
    // which only READ x()/width()/height() off widgets the dialog now owns.
    N3Dialog__setContent(dialog, content);

    // Recorded AFTER setContent, so this only ever names a widget the dialog
    // has actually taken -- see nf_browser_active_content for what its width()
    // is for. `content` is not touched here beyond being remembered as a
    // pointer, which the comment above allows.
    //
    // The destroyed() clear is guarded on the pointer still being THIS
    // widget: setContent deleteLater()s the previous content, so the previous
    // widget's destroyed() fires later in the event loop, i.e. after this
    // assignment -- an unguarded clear would then null out the LIVE pointer
    // and quietly send the next navigation back to the dialog-width estimate.
    nf_browser_active_content = content;
    QWidget *tracked = content;
    QObject::connect(content, &QObject::destroyed, [tracked] {
        if (nf_browser_active_content == tracked)
            nf_browser_active_content = NULL;
    });

    // THE BARS' GEOMETRY, logged here and only here, because this is the
    // first point in the build where the numbers are real: QWidget::
    // setVisible(true) activates a widget's own layout before showing its
    // children, so the content widget the dialog has just taken and shown has
    // been laid out by the time these run. Read before setContent they would
    // be Qt's pre-layout defaults dressed up as measurements -- the same trap
    // N3Dialog::width()'s 600-versus-1264 sets one level up, which is why the
    // line carries a marker saying which it got (nf_log_bar_geometry).
    //
    // This is the cheap check whose ABSENCE is the problem: a horizontal
    // layout is new on this screen, and all three of its plausible failures
    // (items stacked at x=0, zero-width items, a collapsed bar) look
    // identical on a screenshot and different in these two lines.
    int laidOutWidth = content->width();
    nf_log_bar_geometry("command", cmdItems,  nCmdItems,  laidOutWidth);
    nf_log_bar_geometry("page",    pageItems, nPageItems, laidOutWidth);
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
        //
        // The rows are also REBUILT, not just re-pushed, which makes a
        // re-trigger a genuine REFRESH. Two reasons, and the first is what
        // asked for it: every change to how a row renders otherwise costs the
        // owner a tap on the device to see, and a `touch /tmp/nfolders-native`
        // over ssh that re-pushed WITHOUT rebuilding is actively misleading --
        // it produced a screenshot of stale rows built under the previous
        // setting, read as "the fix did not work", on 2026-09-04. The second
        // is ordinary: the card can change under us (a USB session, a sideload),
        // and a refresh is the obvious thing a reader would expect a
        // re-trigger to do.
        //
        // Rooted at nf_browser_cwd, the directory that is already showing --
        // NOT at NF_ROOT. Losing the reader's place on a refresh would be
        // worse than not refreshing at all. resetPage is false for the same
        // reason: the page number is part of "where I am", and nf_browser_go
        // clamps it if the listing has since shrunk under it.
        //
        // The recovery case above still works exactly as before: rebuilding
        // content is what an abandoned-but-alive dialog needs anyway, and the
        // pushView that follows is unchanged.
        QString cwd = QString::fromUtf8(nf_browser_cwd);
        if (cwd.isEmpty())
            cwd = QStringLiteral(NF_ROOT); // unreachable while a dialog exists
                                           // (nf_browser_go sets cwd before it
                                           // can), and a defined answer anyway
        nh_log("browser: a screen already exists -- rebuilding '%s' in it and re-pushing rather than building a new one",
               qPrintable(cwd));
        N3Dialog *existing = static_cast<N3Dialog*>(nf_browser_active_dialog);
        nf_browser_go(mwc, existing, cwd, false); // refresh -- same directory, same page
        MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(existing));
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
    nf_browser_go(mwc, dialog, QStringLiteral(NF_ROOT), true); // fresh dialog -- page 0

    MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(dialog));
    return true;
}
