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

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMargins>
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

// Items shown per page. PROVEN, not merely conservative -- review finding
// F3 deliberately gave up some headroom for this: NOTES.md's Task 10
// records the one row count actually seen fitting this panel, with clear
// blank space still below it -- BACK + a truncation notice + 15 item rows,
// 17 rows total. That screenshot's own margin READS as room for roughly
// 20, but 20 was never itself measured, only estimated from the same
// image -- and this constant now has to answer for MORE fixed chrome per
// page than that screenshot had (BACK, a position indicator, and now BOTH
// PREV and NEXT, all four together on a middle page of a multi-page
// listing, not just BACK and one notice). 14 kept the same 17-row total
// (BACK + PREV + NEXT + 14 items) that is the one number this project can
// actually cite a screenshot for, rather than betting the extra chrome on
// the estimated, unmeasured margin above it.
//
// LOWERED to 12, UNMEASURED, when the sort/filter chrome rows were added
// below: those two rows are NOT conditional the way the position indicator/
// PREV/NEXT are -- they show on every listing, multi-page or not -- so the
// worst-case page's fixed-chrome count grew from 4 (BACK, indicator, PREV,
// NEXT) to 6 (those four plus sort, filter). 12 keeps BACK + PREV + NEXT +
// sort + filter + 12 items at 17, the same proven total the original 14 was
// keyed to, treating the position indicator the same way the original
// comment already did -- as an accepted, unproven 18th row, "the smallest
// addition available" -- rather than compounding two unproven guesses (the
// indicator AND the two new rows) on top of each other. This has NOT been
// confirmed on hardware; see the task report's device checklist.
//
// 27 entries -- the largest listing measured on this card (Fullmetal
// Alchemist, same screenshot) -- is comfortably ABOVE this page size, not
// under it: it needs three pages at 12 per page (ceil(27/12) = 3), which is
// the multi-page case this whole feature exists to reach -- volume 26 is
// on page 3, and multi-page is precisely what nothing before pagination
// existed ever rendered. A single-page listing (fewer than 12 entries) is
// still the common case elsewhere on the card and drops the indicator/
// PREV/NEXT rows entirely; 27 is not an example of that case.
//
// Trivially raised once a fuller worst-case page -- indicator, PREV, sort,
// filter, 12 items, AND NEXT together -- has actually been seen on
// hardware; see the task report's device checklist.
#define NF_ITEMS_PER_PAGE 12

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
// previous directory happened to be showing. The PREV/NEXT PAGE rows are
// the one caller that passes false: they change the page WITHIN the same
// directory nf_browser_cwd already names, so resetting here would make
// NEXT PAGE always land back on page 0.
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
    row->hasRow = nf_volume_exists(contentId, c->dbName, &row->percentRead, &row->readState);
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

// Cycles nf_browser_sort_key/nf_browser_sort_desc as ONE combined six-state
// sequence on a single tap -- name-ascending, name-descending, size-
// ascending, size-descending, date-ascending, date-descending, back to
// name-ascending -- rather than needing two separate rows for what the task
// brief frames as two orthogonal choices (key, direction). Direction flips
// first and key advances only every second tap, so a reader sees both
// directions of whichever key they just picked before it moves on.
static void nf_browser_cycle_sort(void) {
    if (!nf_browser_sort_desc) {
        nf_browser_sort_desc = true;
        return;
    }
    nf_browser_sort_desc = false;
    switch (nf_browser_sort_key) {
        case NF_SORT_NAME: nf_browser_sort_key = NF_SORT_SIZE; break;
        case NF_SORT_SIZE: nf_browser_sort_key = NF_SORT_DATE; break;
        case NF_SORT_DATE:
        default:            nf_browser_sort_key = NF_SORT_NAME; break;
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
// file's other ASCII chrome ("<< BACK", "< PREV PAGE"). "^" reads as
// ascending (smallest/oldest/A first, pointing at the top of the list) and
// "v" as descending, without needing a real glyph this panel may not have.
static QString nf_sort_row_label(void) {
    QString keyName;
    switch (nf_browser_sort_key) {
        case NF_SORT_SIZE: keyName = QStringLiteral("size"); break;
        case NF_SORT_DATE: keyName = QStringLiteral("date"); break;
        case NF_SORT_NAME:
        default:            keyName = QStringLiteral("name"); break;
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
// The images are NICKEL'S OWN Qt resources, not ours. This library runs inside
// Nickel's process, so Nickel's compiled-in resources are already registered
// and resolve from our code for free: no new assets, no rcc, nothing shipped,
// no archaeology. rcc is specifically NOT an option here even if we wanted our
// own artwork -- it registers a bundle from a FILE-SCOPE STATIC INITIALISER,
// the exact construct that once boot-looped this mod into NickelHook's SHARED
// failsafe, which can uninstall the owner's OTHER mods (nfnickel.cc's own
// QByteArray account, and nf_browser_active_dialog's comment above).
//
// All three paths are present in 4.38.23684 as :/-prefixed literals in
// Nickel's own code (`strings libnickel.so.1.0.0 | grep -F :/images/`, one hit
// each). That they are present is NOT the same claim as that they RESOLVE from
// this library, which is what nf_probe_icon_resources below exists to settle
// on the device rather than leave to inference.
//
// THE FOLDER ROW HAS NO FOLDER PICTOGRAM, and this is measured, not a
// preference. `:/images/widgets/folder.png` -- the obvious candidate, and what
// this line used to say -- is NOT a folder pictogram at all: the device probe
// logged it at 250x350, and the screenshot showed it rendering as an empty
// light-grey rectangle. It is COVER-ART-SHAPED PLACEHOLDER ART (a 5:7 book
// cover, the same aspect as every cover slot on Nickel's own shelves), which
// is presumably what "folder" means to whoever named it -- the artwork behind
// a Nickel COLLECTION, not a filesystem folder. Do not reach for it again on
// the strength of its filename. There is no folder pictogram anywhere in
// Nickel's 373 :/images/... resources; this was swept, not assumed.
//
// THE GENERAL LESSON, because it cost a device cycle: the resource NAME did
// not predict its CONTENT, and the dimensions this file's own probe logs were
// the tell -- 250x350 next to the other two at 50x50 and 80x80 already said
// "this is not an icon" before the screenshot confirmed it, and that log line
// was read past the first time. Judge a resource by the dimensions the probe
// reports, never by what it is called.
//
// The replacement is Nickel's own RIGHT-ARROW glyph, on the owner's reasoning
// that a right arrow conventionally means "drills down" -- which is exactly
// what tapping a folder row does here. It is also the one candidate with prior
// art in Nickel itself for THIS use: the firmware contains the literal
// `<img src=":/images/menu/label_arrow_right.png">` (one hit, alongside
// showSearchOptions()), i.e. Nickel embeds this same resource in rich text in
// a label, bare and at its intrinsic size, the same construct nf_icon_markup
// builds below. Its dimensions are still UNMEASURED here, which is why it
// stays in the probe list: if it turns out to load at some awkward size, that
// is for the owner to judge from the probe log and the next screenshot, NOT
// for this file to silently compensate for.
#define NF_ICON_FOLDER_RES ":/images/menu/label_arrow_right.png"
#define NF_ICON_BOOK_RES   ":/images/home/main_nav_books.png"
#define NF_ICON_COMIC_RES  ":/images/reading/reading_image_view.png"

// A path no resource has, probed alongside the three real ones. Without it,
// "the real one loaded" is not evidence of anything -- CLAUDE.md's "a negative
// control is what makes a check non-vacuous", the same discipline as the
// isValid=false ContentID that made rung 1's isValid=true mean something.
// Confirmed absent from the firmware by the same grep that found the three
// above (zero hits).
#define NF_ICON_CONTROL_RES ":/images/widgets/nfolders_does_not_exist.png"

// Rendered height in px, forced on every <img> rather than left at whatever
// size the resource happens to be. UNMEASURED and deliberately conservative:
// these are Nickel's own chrome images at whatever size Nickel's own screens
// wanted them, and an icon taller than the row's text would grow the row and
// eat into a page budget that is already spoken for -- NF_ITEMS_PER_PAGE
// (above) is keyed to a MEASURED 17-row page on a 1680px-visible panel, i.e. a
// row pitch near 100px, so 40 cannot be what pushes the 12th item off the
// screen. Raise it from the dimensions nf_probe_icon_resources logs, once a
// screenshot shows how these actually render.
//
// MEASURED on the device 2026-09-04, from that probe: the book icon is 50x50
// and the comic icon 80x80, so 40 is a modest downscale for both rather than
// an upscale, and both rendered correctly at it. Left at 40 anyway -- the
// arrow above is newly swapped in and its own dimensions are still unlogged,
// so there is nothing yet to tune all three against together. The number this
// constant is worth revisiting from is the NEXT probe log, with the arrow in
// it.
#define NF_ICON_PX 40

// Probed ONCE, lazily, on first use. A plain file-scope bool rather than a
// function-local static for TWO reasons, both load-bearing here: a
// function-local static of non-POD type compiles to a __cxa_guard_acquire/
// release pair, i.e. libstdc++ runtime, which CLAUDE.md forbids outright; and
// a POD bool lives in .bss with no constructor to race NickelHook's own
// nh_init ordering, the same discipline as every other file-scope datum in
// this file. Every caller runs on the GUI thread (a tap handler, or the
// trigger that opens the screen), so there is no thread to race either.
static bool nf_icons_probed = false;

// QImage, not QPixmap: the open question is whether Nickel's RESOURCE TABLE
// and the PNG decoder are reachable from this library at all, and QImage
// answers exactly that with no QGuiApplication/platform dependency of its own
// to confuse a null result with. It is also the type Qt's own rich-text image
// handler loads through (QTextImageHandler, which is what actually renders the
// <img> below), so a null here is a null there.
//
// Logged rather than acted on: there is nothing useful to do about a missing
// resource except tell whoever reads the log, and the row's TEXT carries the
// meaning regardless (see the label comment in the row loop). If the icons
// simply do not appear on the panel, these four lines are the only thing that
// can tell "no icon" apart from "the resource system is unreachable from a
// plugin" -- symptoms that are otherwise identical.
static void nf_probe_icon_resources(void) {
    if (nf_icons_probed)
        return;
    nf_icons_probed = true;

    char const *const paths[] = {
        NF_ICON_FOLDER_RES, NF_ICON_BOOK_RES, NF_ICON_COMIC_RES, NULL,
    };
    for (int i = 0; paths[i]; i++) {
        QImage img(QString::fromLatin1(paths[i]));
        nh_log("icons: '%s' -> %s %dx%d", paths[i],
               img.isNull() ? "NULL, did not load" : "loaded",
               img.width(), img.height());
    }

    QImage control(QStringLiteral(NF_ICON_CONTROL_RES));
    nh_log("icons: control '%s' -> %s", NF_ICON_CONTROL_RES,
           control.isNull() ? "NULL, as required" : "LOADED -- probe is meaningless");
}

// The leading markup for one row, separator included, so the row loop never
// has to know which kinds render as an image and which as text.
//
// pdf and unknown are TEXT BADGES, not images: Nickel's resource table holds
// no PDF icon and no generic-document icon anywhere (checked against the
// firmware, not assumed), and borrowing an unrelated pictogram for a PDF is
// worse than three unmistakable letters. The resulting mixed look -- images on
// some rows, letters on others -- is accepted for this pass; the alternative
// is drawing every row into a QPixmap ourselves, which is a bigger job than
// the defect warrants. Both badges are five characters wide so the labels
// after them still line up with each other.
static QString nf_icon_markup(nf_icon_kind kind) {
    nf_probe_icon_resources();

    char const *res = NULL;
    switch (kind) {
        case NF_ICON_FOLDER: res = NF_ICON_FOLDER_RES; break;
        case NF_ICON_BOOK:   res = NF_ICON_BOOK_RES;   break;
        case NF_ICON_COMIC:  res = NF_ICON_COMIC_RES;  break;
        case NF_ICON_PDF:    return QStringLiteral("[PDF]&nbsp;");
        // Unreachable while nf_is_book_name gates every file row on the same
        // allowlist nf_icon_kind_for reads (nffmt.h says so on its own side of
        // the boundary too), and answered anyway rather than left to fall off
        // the end: a "?" badge is what a format added to NF_EXTS but not to
        // the icon map should look like, not a missing icon.
        case NF_ICON_UNKNOWN:
        default:             return QStringLiteral("[&nbsp;?&nbsp;]&nbsp;");
    }
    // &nbsp; rather than a plain space for the separator, here and in the two
    // badges above: this string is rich text by the time QLabel sees it, and
    // HTML collapses runs of whitespace.
    return QStringLiteral("<img src=\"%1\" height=\"%2\">&nbsp;")
               .arg(QString::fromLatin1(res)).arg(NF_ICON_PX);
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
// Two paths, and *measured tells the caller which one it got so the log line
// can say so: a silently wrong width would either elide text that fits or fail
// to elide text that does not, and on a screenshot both of those look like
// "the elision is broken" with no way to tell them apart.
//
// The dialog, not the content widget, is what gets read: `content` is built
// fresh on every navigation and handed to setContent at the very END of
// nf_browser_go, so at row-build time it has never been laid out and its
// width() is always the Qt default -- it can never be the measured path. The
// dialog IS laid out on every navigation after the first.
//
// What this still cannot subtract is N3Dialog's own content-area inset, which
// is Nickel's chrome and not readable from here; the layout margins below are
// ours and are queried rather than guessed. So this is an OVER-estimate by
// however wide that inset is, which shows up as a little residual clipping
// rather than as over-eager elision -- deliberately that way round, and one
// for the screenshot to settle rather than for this file to pad by a guess.
//
// The measured path can also over-report for a second reason worth naming:
// if Nickel sizes its own top-level widgets to the PADDED framebuffer (1280)
// rather than to the visible panel (1264), width() hands back 16 px that are
// not on the glass. That is exactly why the log line below prints the raw
// width() as well as the number actually used -- the two together say which
// of the two the firmware thinks the screen is, which is a device measurement
// nobody has taken yet, not something to pre-compensate for here.
static int nf_row_width_px(N3Dialog *dialog, QLayout *layout, bool *measured, int *rawDialogWidth) {
    int w = reinterpret_cast<QWidget*>(dialog)->width();
    *rawDialogWidth = w;
    *measured = (w >= NF_WIDTH_PLAUSIBLE_MIN_PX);
    if (!*measured)
        w = NF_PANEL_VISIBLE_WIDTH_PX;

    QMargins m = layout->contentsMargins();
    w -= m.left() + m.right();

    // A deliberate floor, not dead code: QFontMetrics::elidedText with a
    // width at or below the ellipsis' own width returns the ellipsis alone (or
    // nothing), i.e. a screen of rows reading "..." and no names at all. No
    // path above can currently produce a number that low -- both branches
    // start from at least 800 -- but this is the one place where a bad width
    // erases the entire listing rather than degrading it, so the floor is
    // cheap insurance worth keeping.
    if (w < 200)
        w = 200;
    return w;
}

// The px this row's LEADING icon markup costs, so the name can be elided to
// what is actually left. Without this the elision is off by the icon on every
// single row.
//
// The two text badges are measured exactly -- they are text, in this row's own
// font, so QFontMetrics answers precisely. The image icons are ESTIMATED at
// NF_ICON_PX, i.e. their forced height used as a stand-in for their rendered
// width: <img height=N> with no width scales by the resource's own aspect
// ratio, and those aspect ratios are exactly what is not yet known for the
// arrow (nf_probe_icon_resources logs them; the book and comic icons measured
// square, 50x50 and 80x80, for which this stand-in is exact). Square or
// taller-than-wide makes it an over-estimate, which errs toward eliding a few
// characters early rather than toward running off the edge again.
static int nf_icon_width_px(nf_icon_kind kind, QFontMetrics const &fm) {
    switch (kind) {
        case NF_ICON_PDF:     return fm.width(QStringLiteral("[PDF] "));
        case NF_ICON_UNKNOWN: return fm.width(QStringLiteral("[ ? ] "));
        // Image icons plus nf_icon_markup's own trailing &nbsp; separator.
        default:              return NF_ICON_PX + fm.width(QLatin1Char(' '));
    }
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
    // PREV/NEXT PAGE rows below pass false, because they call back into
    // this SAME function for the SAME path just to render a different
    // slice of the same listing.
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
    bool hasPrev = nf_browser_page > 0;
    bool hasNext = nf_browser_page < totalPages - 1;

    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    // The width every item row's label is elided against -- see
    // nf_row_width_px for both paths and why the dialog is what gets read.
    //
    // Logged ONCE PER NAVIGATION rather than once per process (the way
    // nf_probe_icon_resources is) precisely BECAUSE the answer changes: the
    // first listing is built before pushView has sized the dialog, so it is
    // always the fallback, and a once-per-process log would therefore only
    // ever record the fallback and never the real measurement. One line per
    // navigation is still one line, not one per row, and the transition from
    // fallback to measured is visible in the log rather than invisible.
    bool widthMeasured   = false;
    int  rawDialogWidth  = 0;
    int  rowWidth        = nf_row_width_px(dialog, layout, &widthMeasured, &rawDialogWidth);
    QMargins layoutMargins = layout->contentsMargins();
    nh_log("browser: row width %d px -- %s (N3Dialog::width() read back %d, our layout margins %d+%d, panel fallback %d)",
           rowWidth,
           widthMeasured ? "MEASURED from the dialog"
                         : "FALLBACK, the dialog is not laid out yet",
           rawDialogWidth, layoutMargins.left(), layoutMargins.right(),
           NF_PANEL_VISIBLE_WIDTH_PX);

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

    // Position indicator -- "page 2/2" -- a plain QLabel, not a TouchLabel:
    // informational only, not a tap target, so it needs none of
    // TouchLabel's gesture machinery. Shown only when there is more than
    // one page: on a single-page listing -- fewer than NF_ITEMS_PER_PAGE
    // entries, still the common case elsewhere on this card even though
    // 27 (Fullmetal Alchemist, the largest listing measured) is NOT an
    // example of it -- "page 1/1" says nothing a reader does not already
    // know from PREV/NEXT both being absent.
    //
    // Placed HERE -- immediately after the BACK row, ABOVE the listing rows
    // -- for the same reason review finding L1 placed the old truncation
    // notice here rather than after the rows, and the same reason PREV/NEXT
    // (below) were moved here too (review finding F3): this panel's real
    // capacity for this many rows at once is still not device-measured
    // beyond the 17-row screenshot NF_ITEMS_PER_PAGE's own comment cites, so
    // whatever gets clipped first should be the least useful row -- and
    // "where I already am" (this label) is the least useful of the three
    // pieces of page-navigation chrome, which is why it sits above PREV/NEXT
    // rather than below them.
    if (totalPages > 1) {
        QLabel *pageInfo = new QLabel(content);
        pageInfo->setText(QStringLiteral("page %1/%2").arg(nf_browser_page + 1).arg(totalPages));
        layout->addWidget(pageInfo);
    }

    // PREV PAGE and NEXT PAGE rows -- TouchLabels, same construction/shim
    // pattern as every other tappable row in this function (see the BACK
    // row's own comment for the allocation-size and signal-adaptor
    // derivation, not repeated per row). Each is built only when that
    // direction actually exists -- an always-present, sometimes-disabled
    // row was rejected because this panel gives no reliable "disabled"
    // visual state (CLAUDE.md's task brief on the four grey levels applies
    // here too), so absence is the only unambiguous way to say "no such
    // page" on this hardware.
    //
    // BOTH placed HERE, directly under the position indicator and ABOVE
    // every item row -- review finding F3, reversing this file's own
    // earlier placement of NEXT PAGE after the items. That placement had
    // NEXT PAGE clipped FIRST if this page's row count ever exceeds the
    // panel's real height, and NEXT PAGE is the only route to any page
    // past the first -- concretely, the only route to Fullmetal Alchemist
    // volume 26, which is the entire reason pagination exists. A user
    // could see "page 1/2" (the indicator, above) with genuinely no way to
    // reach page 2 -- worse than the truncation notice ever being clipped,
    // because L1's old finding was about a MISSING clue, not a VISIBLE
    // clue to a control that isn't there. This file's own
    // NF_ITEMS_PER_PAGE comment already states the principle ("whatever
    // gets clipped first should be the least useful row"); this placement
    // is what makes the code match it -- PREV/NEXT are both more useful
    // than any single item row below them, not less.
    //
    // `path` (this directory) is captured by value in both, and
    // nf_browser_go is called with resetPage=FALSE in both -- this is a
    // page change WITHIN the current directory, not a navigation to a
    // different one, so nf_browser_page must survive the rebuild this
    // triggers.
    if (hasPrev) {
        void *row = calloc(1, 256); // 132 measured, 256 over-allocated -- see the BACK row's comment
        if (row) {
            TouchLabel__ctor(row, content, 0);
            reinterpret_cast<QLabel*>(row)->setText(QStringLiteral("< PREV PAGE"));

            QPushButton *shim = new QPushButton(content);
            shim->setVisible(false);
            if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
                nh_log("browser: connecting the PREV PAGE row's tapped(bool) failed -- this row will silently do nothing");
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_page--;
                nh_log("browser: page -- prev, now %d in '%s'", nf_browser_page, qPrintable(path));
                nf_browser_go(mwc, dialog, path, false);
            });

            layout->addWidget(reinterpret_cast<QWidget*>(row));
        } else {
            nh_log("browser: calloc(1,256) failed for the PREV PAGE row, skipping it");
        }
    }

    if (hasNext) {
        void *row = calloc(1, 256); // 132 measured, 256 over-allocated -- see the BACK row's comment
        if (row) {
            TouchLabel__ctor(row, content, 0);
            reinterpret_cast<QLabel*>(row)->setText(QStringLiteral("NEXT PAGE >"));

            QPushButton *shim = new QPushButton(content);
            shim->setVisible(false);
            if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
                nh_log("browser: connecting the NEXT PAGE row's tapped(bool) failed -- this row will silently do nothing");
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_page++;
                nh_log("browser: page -- next, now %d in '%s'", nf_browser_page, qPrintable(path));
                nf_browser_go(mwc, dialog, path, false);
            });

            layout->addWidget(reinterpret_cast<QWidget*>(row));
        } else {
            nh_log("browser: calloc(1,256) failed for the NEXT PAGE row, skipping it");
        }
    }

    // Sort and filter chrome -- ALWAYS shown (unlike the indicator/PREV/NEXT
    // above, which are conditional on more than one page), same TouchLabel/
    // shim construction as every other tappable row in this function.
    // Deliberately placed BELOW PREV/NEXT rather than above them: PREV/NEXT
    // are the ONLY route to a page past the first (this file's own NEXT
    // PAGE placement comment, above, already established that principle for
    // moving them ahead of the item rows), so if this panel's real capacity
    // is ever tight enough that something here gets clipped, it must be
    // these two rather than PREV/NEXT -- losing them costs a reader the
    // CONVENIENCE of changing sort/filter in this one directory (the default
    // state, or whatever was carried in from wherever they navigated from,
    // still works), where losing NEXT PAGE would cost outright reachability
    // of whatever is on page 2 and beyond. Still placed ABOVE every item
    // row, per the task brief: these are chrome, not content.
    //
    // Tapping either one changes what THIS directory shows, which is a
    // bigger change to the row set than a mere page turn -- unlike PREV/
    // NEXT (resetPage=false, same directory, different slice), both of
    // these pass resetPage=TRUE: the total row/page count can shrink or
    // grow arbitrarily (a filter can turn a 3-page listing into a 1-page
    // one), and landing on whatever page NUMBER happened to be current
    // would be an arbitrary slice of a now-different listing, not a
    // meaningful "same place" the way it is for BACK/descend's own
    // resetPage=true callers.
    {
        void *row = calloc(1, 256); // 132 measured, 256 over-allocated -- see the BACK row's comment
        if (row) {
            TouchLabel__ctor(row, content, 0);
            reinterpret_cast<QLabel*>(row)->setText(nf_sort_row_label());

            QPushButton *shim = new QPushButton(content);
            shim->setVisible(false);
            if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
                nh_log("browser: connecting the sort row's tapped(bool) failed -- this row will silently do nothing");
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_cycle_sort();
                nh_log("browser: sort -- now %s", qPrintable(nf_sort_row_label()));
                nf_browser_go(mwc, dialog, path, true); // resetPage -- see this block's own comment
            });

            layout->addWidget(reinterpret_cast<QWidget*>(row));
        } else {
            nh_log("browser: calloc(1,256) failed for the sort row, skipping it");
        }
    }
    {
        void *row = calloc(1, 256); // 132 measured, 256 over-allocated -- see the BACK row's comment
        if (row) {
            TouchLabel__ctor(row, content, 0);
            reinterpret_cast<QLabel*>(row)->setText(nf_filter_row_label());

            QPushButton *shim = new QPushButton(content);
            shim->setVisible(false);
            if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), shim, SLOT(click())))
                nh_log("browser: connecting the filter row's tapped(bool) failed -- this row will silently do nothing");
            QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                nf_browser_cycle_filter();
                nh_log("browser: filter -- now %s", qPrintable(nf_filter_row_label()));
                nf_browser_go(mwc, dialog, path, true); // resetPage -- see this block's own comment
            });

            layout->addWidget(reinterpret_cast<QWidget*>(row));
        } else {
            nh_log("browser: calloc(1,256) failed for the filter row, skipping it");
        }
    }

    // Spec sections 3.6/6.3: an empty ROW SET reads one of two ways, and
    // conflating them tells a reader who filtered to PDF and got nothing
    // that their books are gone rather than that their filter matched
    // nothing. A plain QLabel, like the page indicator above -- informational
    // only, not a tap target. Says WHICH filter is active so the fix (tap
    // "filter: ..." until it reads "all") is discoverable from this message
    // alone, without hunting for the filter row above it.
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
        // The suffix, built in TWO forms before either one is used: the
        // markup that actually gets appended, and a plain-text twin whose
        // only job is to be MEASURED (nf_row_width_px's own comment, and the
        // elision below). The twin exists because the suffixes are what the
        // row MEANS -- "[not in library]" on Fullmetal Alchemist v26 is the
        // whole point of that row -- so they must never be what elision
        // spends its budget on: the name is elided to what is left AFTER the
        // suffix is paid for, so the suffix survives by construction rather
        // than by hoping the name was short enough.
        //
        // The separator is the literal markup "&nbsp;&nbsp;" in the appended
        // form and two plain spaces in the measured one: rich text collapses
        // runs of whitespace (which is why the markup form cannot just use
        // spaces), and QFontMetrics measures plain text (which is why the
        // measured form cannot just use the entities).
        QString suffixMarkup;
        QString suffixPlain;
        if (r.isDir) {
            // A folder gets a trailing "/" as well as the folder icon. KEPT
            // rather than replaced by the icon, even though the two now say
            // the same thing: the icon depends on Nickel's resource table
            // resolving from inside this library -- that is the whole reason
            // nf_probe_icon_resources exists -- and if it does not resolve,
            // this one plain ASCII character is the only folder/file marker
            // left. Same reasoning as the [not in library] text below: the
            // text carries the meaning and the picture is the addition, never
            // the other way round. A folder never carries a progress marker
            // either -- there is no Volume for one, so r.percentRead/
            // r.finished are always -1/false for it (nf_build_listing,
            // nflist.cc: metadata is never fetched for a directory row).
            suffixMarkup = QStringLiteral("/");
            suffixPlain  = QStringLiteral("/");
        } else if (!r.hasRow) {
            // A file with NO library row gets its reason spelled out in the
            // label TEXT itself, not left to colour/style alone: this panel
            // gives four grey levels, and "slightly lighter" reads as "the
            // same", not "different" (CLAUDE.md's task brief). The reference
            // card's own example is exactly one row -- Fullmetal Alchemist
            // v26, a truncated file Nickel's own import rejected (NOTES.md)
            // -- and it must render as clearly wrong, not silently vanish and
            // leave a reader wondering where volume 26 went. Which is also
            // why it must not be elided away: see the suffix budget above.
            suffixMarkup = QStringLiteral("&nbsp;&nbsp;[not in library]");
            suffixPlain  = QStringLiteral("  [not in library]");
        } else if (r.finished) {
            // A file WITH a library row gets a progress marker: spec's own
            // wording is a percentage for in-progress books, a marker for
            // finished, and NOTHING for unread. "Finished" takes priority
            // over any number sitting in percentRead -- a re-read that
            // stopped partway through leaves a lower value there, and the
            // word is the more informative answer regardless of what that
            // number is.
            //
            // r.finished itself is DERIVED (nf_build_listing, nflist.cc) from
            // r.readState, whose primary source is Content::getReadStatus()
            // -- measured 0 = not started, 1 = in progress, 2 = finished
            // (NOTES.md) -- with Content::isFinished() demoted to the
            // cross-check it always was, because a bool cannot carry three
            // states and the read-state filters need all three
            // (nf_volume_exists, nfnickel.cc, has the full account).
            suffixMarkup = QStringLiteral("&nbsp;&nbsp;[finished]");
            suffixPlain  = QStringLiteral("  [finished]");
        } else if (r.percentRead > 0) {
            // 0% and -1 (unknown, including a firmware that moved the +140
            // offset -- nf_volume_exists's own guard) both render as nothing,
            // deliberately: Nickel's own BookWidget::getPercentReadString
            // clamps display to [1,99] for the same reason an untouched
            // book's own stored percentage is 0, not a real progress value
            // (NOTES.md).
            suffixMarkup = QStringLiteral("&nbsp;&nbsp;(%1%)").arg(r.percentRead);
            suffixPlain  = QStringLiteral("  (%1%)").arg(r.percentRead);
        }

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

        // The name's own budget: the row, less the leading icon, less the
        // suffix that must survive. Floored for the same reason
        // nf_row_width_px floors its own result -- elidedText at or below the
        // ellipsis' width yields the ellipsis alone, which would erase the
        // name entirely. 60 px is roughly a few characters at this panel's
        // row font, so the floor degrades a pathological row to "a stub plus
        // its suffix" rather than to "no name at all".
        int nameWidth = rowWidth - nf_icon_width_px(kind, fm) - fm.width(suffixPlain);
        if (nameWidth < 60)
            nameWidth = 60;

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
        rowLabel->setText(nf_icon_markup(kind) + label);

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

    QString title = (path == QStringLiteral(NF_ROOT))
        ? QStringLiteral("NickelFolders")
        : QFileInfo(path).fileName();
    N3Dialog__setTitle(dialog, title);

    nh_log("browser: showing '%s' (%d row(s), page %d/%d, %d shown)",
           qPrintable(path), rows.size(), nf_browser_page + 1, totalPages, endIdx - startIdx);

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
    nf_browser_go(mwc, dialog, QStringLiteral(NF_ROOT), true); // fresh dialog -- page 0

    MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(dialog));
    return true;
}
