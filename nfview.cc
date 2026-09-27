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
// The file operations and the pure guards under them. nfops.h does the
// syscalls; nfpath.h decides whether any of them may happen at all. Nothing
// in this file re-implements either -- every destructive call goes through
// nfops.h and every path question is nfpath.h's answer.
#include "nfops.h"
#include "nfpath.h"

#include <QBrush>
#include <QCoreApplication> // processEvents -- the yield that keeps the panel alive mid-copy
#include <QEventLoop>       // QEventLoop::AllEvents, for that same yield
#include <QStringList>
#include <QTimer>       // the zero-delay hop that gets a destructive run OFF the tap's own call stack
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
#include <stdarg.h>        // va_list -- nf_op_say's one formatted message buffer
#include <stdio.h>         // snprintf, vsnprintf
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

// ITEMS SHOWN PER PAGE now comes from nf_items_per_page (nffmt.h), which
// COMPUTES it from the measured panel geometry for the mode `covers` selects
// -- 11 with covers on, 15 with them off. It used to be an NF_ITEMS_PER_PAGE
// macro right here, with the arithmetic written out in this comment and
// NF_COVER_H_PX's own comment (nffmt.h) carrying the same numbers from the
// cover side; the `covers` view toggle needed a per-mode answer anyway, and
// moving the quotient next to the constant it depends on made it
// host-testable at the same time. Both arithmetics, both modes, and the
// reason the page size depends on the MODE and never on what happened to land
// on a given PAGE, are in nf_items_per_page's own comment.
//
// A first device screenshot must still COUNT the item rows on a full page, in
// BOTH modes. If either shows fewer than nf_items_per_page returns, the
// 1330/75/99 terms in nffmt.h are what to re-measure, not this file.
//
// IT ALSO DEPENDS ON THE COMMAND BAR NOW. That bar wraps to a second row when
// its controls will not fit side by side (nf_bar_plan_layout, nffmt.h), and a
// bar row is the same 75 px as a text row -- so a wrapped bar costs an item
// off a covers-OFF page (15 -> 14) and, because the covers-ON page already had
// 91 px of slack, nothing at all off a covers-ON one (11 -> 11). The row count
// is measured in nf_browser_go and handed to nf_items_per_page; both
// arithmetics are written out beside it in nffmt.h, and the per-build log line
// prints which count this page was sized against.

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
// previous directory happened to be showing.
//
// Everything that stays in the SAME directory passes false, and there are
// now four such callers: the page bar's PREV/NEXT (a different slice of the
// same listing), opening a submenu, closing one with BACK, and selecting an
// option in one. The last three are the "return to the page you were on"
// requirement, and they need nothing but this: the page is simply not
// touched while a menu is up. The ONE case where a selection invalidates it
// -- a filter that shrinks the listing past the current page -- is handled
// by nf_browser_go's existing clamp (see there), which is reused rather than
// duplicated precisely because it is already the one place this is bounded.
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

// The five view toggles (nf_view_flags, nffmt.h), and the same POD-with-a-
// constant-initialiser discipline as the three statics above, for the same
// load-bearing reason: NF_VIEW_FLAGS_DEFAULT is a brace-enclosed list of
// `false`s that the compiler folds at link time, not a constructor call
// needing a runtime _GLOBAL__sub_I entry that NickelHook's nh_init would race
// (see nf_browser_active_dialog's own comment for the crash that established
// the rule). It is a MACRO and not nf_view_flags_default() for exactly that
// reason -- a function call here would be a dynamic initialiser -- and
// nffmt.cc's own nf_view_flags_default is defined in terms of the same macro
// so the two cannot drift. Every field being `false` also means a .bss-zeroed
// copy reads as today's behaviour rather than as a mode nobody asked for.
//
// PERSISTS across navigation, exactly like the sort key and the filter, and
// for the same reason: a reader who turned covers off or asked for full
// filenames means it for the browser, not for one folder. And, unlike
// nf_browser_menu, it is NOT reset by nf_browser_show either -- a trigger
// means "show me the listing", not "undo the way I set this up".
static nf_view_flags nf_browser_view = NF_VIEW_FLAGS_DEFAULT;

// THE BROWSER'S MODE: browsing the item listing (NF_MENU_NONE), or showing
// one of the two submenus in place of it. Tapping `sort:`/`filter:` in the
// command bar used to CYCLE to the next value, which took up to eight taps to
// reach a specific one; it now opens a menu, and this is the whole of the
// extra state that needs.
//
// Same POD-with-a-constant-initialiser discipline as the three statics above
// and for the same load-bearing reason: `= NF_MENU_NONE` is a constant the
// compiler folds into .data at link time, not a constructor call needing a
// runtime _GLOBAL__sub_I entry that NickelHook's nh_init would race (see
// nf_browser_active_dialog's own comment for the crash that established the
// rule). NF_MENU_NONE is nf_menu_kind's ZERO value (nffmt.h), so even a
// .bss-zeroed copy of this reads as "browsing" rather than as a menu nobody
// opened.
//
// DOES NOT PERSIST the way the sort key and filter do: it is reset to
// NF_MENU_NONE by nf_browser_show on every trigger, because "show me the
// browser" means the listing, never whichever menu happened to be up when
// Nickel last navigated away from a dialog it did not destroy.
static nf_menu_kind   nf_browser_menu      = NF_MENU_NONE;

// --- select mode, the clipboard, and the operation guard ----------------
//
// Six more pieces of file-scope state, and every one of them is POD for the
// same load-bearing reason as the eight above: a file-scope object with a
// dynamically initialised constructor runs from this translation unit's own
// _GLOBAL__sub_I, whose ordering against NickelHook's nh_init/nf_init is NOT
// guaranteed, and a QByteArray in exactly this position previously segfaulted
// Nickel on every boot (nf_browser_active_dialog's own comment). A `bool`, an
// `int` and a POINTER to a QStringList are all .bss-initialised with no
// constructor to race; the QStringLists themselves are heap-allocated on
// first use, well after boot, inside a Qt signal handler.
// `nm libnfolders.so | grep GLOBAL__sub_I` must stay empty.

// SELECT MODE. While it is on, a tap TICKS a row instead of opening it.
// Reset to false by nf_browser_show on every trigger (the trigger means "show
// me the listing"), by leaving it from the bar or BACK, and by any navigation
// to a different directory.
static bool nf_browser_select = false;

// The ticked rows, as BARE NAMES within nf_browser_cwd -- never paths.
//
// Names, not paths, because the selection is scoped to one directory by
// construction and a name is what the row loop has to match against anyway.
// Scoped to one directory because a selection that survived a directory
// change would let a later `delete` act on rows the owner can no longer see,
// which is exactly the shape of an accident -- so nf_browser_go clears this
// whenever the path it is asked for differs from the one already showing.
static QStringList *nf_browser_selection = NULL;

// THE CLIPBOARD: absolute source paths, plus which verb put them there.
//
// ABSOLUTE PATHS here, unlike the selection above, because the whole point of
// a clipboard is that it outlives the directory it was filled in -- the
// reader cuts in one folder and pastes in another, and a bare name would have
// lost the only thing that says where it came from. The source folder is also
// kept, purely so the paste confirmation can name it.
//
// PERSISTS across navigation and across a re-trigger, deliberately: a pending
// cut is a job the reader is half way through, and dropping it on a refresh
// would silently undo their last action. It is cleared by pasting, and by the
// `clear the clipboard` row on the paste confirmation -- which exists
// precisely so a pending cut can be put down (nffmt.cc).
static QStringList *nf_browser_clipboard = NULL;
static bool         nf_browser_clip_cut  = false;
static char         nf_browser_clip_dir[PATH_MAX];

// THE RE-ENTRANCY GUARD, and it is the most important four bytes in this
// file after the path checks.
//
// A chunked copy yields to the event loop between chunks (nf_op_copy's tick,
// below) so the panel stays alive and the cancel row can be tapped. That
// means Qt can deliver ANY tap in the middle of an operation -- a row, a bar
// item, the back arrow -- each of which would re-enter this file's own
// handlers while a copy is half done, rebuild the content widget the progress
// label lives in, and in the worst case start a second operation over the
// same files.
//
// So every tap handler in this file returns immediately while this is set,
// and the only control that does anything during an operation is the cancel
// row, which sets nf_op_cancel_requested and returns. Nothing else runs, and
// nothing navigates, until the operation has finished and rebuilt the screen
// itself.
static bool nf_op_busy = false;
static bool nf_op_cancel_requested = false;

// The progress screen's counter label, tracked the same way the dialog and
// the content widget are (a file-scope POD `void*` cleared off the widget's
// own destroyed() signal) rather than being held as a captured pointer.
//
// It has to be, because of what a copy does: it yields to the event loop
// between chunks, and ANYTHING can happen in that yield -- the device can go
// to sleep, Nickel can pop our dialog for a system dialog of its own -- any
// of which destroys this label while the copy is still running. A captured
// QLabel* would then be written to after it was freed, once per percent, for
// the rest of an 820 MB copy. Reading it back through a pointer that Qt
// itself nulls means the worst case is a copy that finishes with no visible
// progress.
static void *nf_progress_label = NULL;

// What the next listing build will show at the top of the screen, once.
// A char[] and not a QString for the reason every other piece of file-scope
// state in this file is POD: no dynamic initialiser may exist in this
// translation unit (`nm libnfolders.so | grep GLOBAL__sub_I` must print
// nothing -- CLAUDE.md). 256 bytes because that is also nh_log's own silent
// truncation point, so a message that fits here fits the log too.
static char nf_op_message[256];

// --- the rescan's own deferred refresh -----------------------------------
//
// Three more file-scope PODs, same discipline and same reason as every one
// above (`nm libnfolders.so | grep GLOBAL__sub_I` must print nothing).
//
// WHAT THEY ARE FOR: PlugWorkflowManager::sync() starts a QThread and returns
// immediately (rescan-archaeology.md section 5), so the rebuild that follows
// it runs BEFORE the scan has found anything -- which is exactly what the
// owner reported: "after rescan i have to open other folder and return back to
// see changes (if not i still see 'not in library')". The fix is to rebuild
// when Nickel says the scan has FINISHED, and these are what that arriving
// signal is checked against, because an arbitrary amount of time passes in
// between and the owner can do anything at all in it.

// The zero-delay QTimer that is BOTH the signal adaptor and the deferred hop.
// See nf_rescan_arm_refresh for why one object does both jobs.
static void *nf_rescan_refresh_timer = NULL;

// The dialog a rescan was last started from, and the folder that dialog was
// showing at the time. ONLY ever COMPARED, never dereferenced as a dialog --
// so a dialog destroyed since is not a use-after-free here, it is a pointer
// that no longer equals nf_browser_active_dialog (which Qt's own destroyed()
// signal nulls). Even the pathological case where a NEW dialog lands on the
// freed one's address is safe: the comparison then succeeds against the LIVE
// dialog, which is the one a rebuild would go through anyway.
static void *nf_rescan_dialog = NULL;
static char  nf_rescan_cwd[PATH_MAX];

// Lazily-allocated accessors for the two lists. NULL is returned, rather than
// a reference to something, if the allocation fails -- every caller treats
// that as "the selection is empty", which degrades select mode to doing
// nothing rather than taking the mod down (CLAUDE.md: the NickelHook failsafe
// is SHARED infrastructure).
// Exported for nfolders.cc's two other trigger handlers -- see nfview.h.
// Everything inside this file reads nf_op_busy directly.
bool nf_ops_busy(void) {
    return nf_op_busy;
}

static QStringList *nf_selection(void) {
    if (!nf_browser_selection)
        nf_browser_selection = new QStringList();
    return nf_browser_selection;
}

static QStringList *nf_clipboard(void) {
    if (!nf_browser_clipboard)
        nf_browser_clipboard = new QStringList();
    return nf_browser_clipboard;
}

static int nf_selection_count(void) {
    return nf_browser_selection ? nf_browser_selection->size() : 0;
}

static int nf_clipboard_count(void) {
    return nf_browser_clipboard ? nf_browser_clipboard->size() : 0;
}

// Empties the selection and says how many rows it dropped. Called from every
// place a selection must not survive -- leaving select mode, a directory
// change, and the end of any operation that acted on it -- so the rule has
// one implementation rather than four.
static void nf_selection_clear(char const *why) {
    int n = nf_selection_count();
    if (nf_browser_selection)
        nf_browser_selection->clear();
    if (n)
        nh_log("select: cleared %d ticked row(s) -- %s", n, why);
}

static void nf_clipboard_clear(char const *why) {
    int n = nf_clipboard_count();
    if (nf_browser_clipboard)
        nf_browser_clipboard->clear();
    nf_browser_clip_cut = false;
    nf_browser_clip_dir[0] = '\0';
    if (n)
        nh_log("clipboard: cleared %d pending item(s) -- %s", n, why);
}

// The mode's name for the one log line every content build carries. This is
// the cheap check whose FAILURE MODE IS SILENCE, which is the kind this
// project adds in advance: "the submenu opened but the rows are the
// listing's" looks like an ordinary rendering bug on a screenshot and like
// one wrong word in the log. Same idea as the MEASURED/PRE-LAYOUT marker on
// the bar-geometry lines, one level down.
static char const *nf_menu_name(nf_menu_kind menu) {
    switch (menu) {
        case NF_MENU_SORT:           return "SORT MENU";
        case NF_MENU_FILTER:         return "FILTER MENU";
        case NF_MENU_VIEW:           return "VIEW MENU";
        case NF_MENU_CONFIRM_DELETE: return "CONFIRM DELETE";
        case NF_MENU_CONFIRM_PASTE:  return "CONFIRM PASTE";
        case NF_MENU_CONFIRM_RESCAN: return "CONFIRM RESCAN";
        case NF_MENU_NONE:
        default:                     return nf_browser_select ? "BROWSE (SELECT)" : "BROWSE";
    }
}

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

// The command bar's two labels, for the CURRENT state. The strings, and the
// vocabulary both they and the submenu rows spell, live in nffmt.cc
// (nf_sort_bar_label/nf_filter_bar_label) -- a menu row reading "date" has to
// be the same word the bar reads back as "sort: date ^", and one place to
// spell it is the only way that holds. These two wrappers remain only to bind
// the file-scope state, so no call site has to repeat it.
//
// The two CYCLE functions that used to sit here are gone with the cycling
// itself: tapping `sort:` or `filter:` opens a submenu now (nf_browser_menu,
// above; nf_build_menu_content, below), and the order those menus list their
// options in is the order these cycles used to step through -- pinned in
// nffmt.cc's own NF_MENU_SORT_ROWS/NF_MENU_FILTER_ROWS tables and tested
// there, so the tap sequence the owner learned on hardware survives as a
// reading order.
static QString nf_sort_row_label(void) {
    return nf_sort_bar_label(nf_browser_sort_key, nf_browser_sort_desc);
}

static QString nf_filter_row_label(void) {
    return nf_filter_bar_label(nf_browser_filter);
}

// There is no nf_view_row_label wrapper to match the two above, and its
// absence is deliberate rather than an omission: the view bar item is now
// STATIC TEXT (nf_view_bar_label, nffmt.cc -- the owner asked for the
// "default"/"custom" suffix gone), so it binds no file-scope state and a
// wrapper would exist only to forward an argument the function ignores.
// nf_cmd_label calls it directly, like the other two.

static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path, bool resetPage);

// Shared by the guaranteed BACK row and N3Dialog's own backTapped() signal
// -- same "one function, not two forks to audit for drift" reasoning as
// nf_pop_native_view, except this one does NOT always pop: which action it
// takes depends on nf_browser_menu and nf_browser_cwd, BOTH read fresh on
// every call, so it is always asking "where am I NOW", never a value
// captured at some earlier row-build time.
//
// THE SUBMENU CASE IS A CASE INSIDE THIS FUNCTION, not a second path, and
// that is deliberate: the whole reason the screen's two independent exits
// share one function is that there is then one place to read "where am I"
// from, rather than two near-duplicates to audit for drift (nfview.h, review
// finding I-3). An open submenu is one more answer to that question, so it
// belongs here with the others.
static void nf_browser_back(void *mwc, N3Dialog *dialog) {
    // CASE 0: an operation is running. BACK is INERT, and this is the same
    // re-entrancy guard every tap handler in this file carries (see
    // nf_op_busy's own comment): a chunked copy yields to the event loop
    // between chunks, so the back arrow really can be delivered mid-copy, and
    // honouring it would pop the screen out from under a running operation
    // whose progress label lives on it.
    //
    // FIRST of the cases, ahead of everything, because it is the only one
    // that is about whether this function may act at all rather than about
    // where the reader is.
    if (nf_op_busy) {
        nh_log("browser: BACK ignored -- a file operation is running (the cancel row is the way out of it)");
        return;
    }

    // CASE 1: a submenu or a CONFIRMATION SCREEN is open. Close it, change
    // NOTHING else, and do NOT go up a directory -- the reader opened it and
    // changed their mind, which is not a request to leave the folder they are
    // in.
    //
    // Nothing is selected and nothing is applied: the sort key, the direction,
    // the filter, the directory and the page are all exactly what they were
    // when the menu opened (nf_browser_page in particular is untouched while a
    // menu is up), so the rebuild below lands on the same screen the menu
    // replaced. resetPage is false for that reason.
    //
    // FOR A CONFIRMATION THIS IS THE CANCEL PATH, and it is the same code as
    // the `cancel` row's -- the two must be identical, because a reader who
    // uses the back arrow instead of the row must not get a different answer
    // than "nothing happened". Nothing is deleted, moved, copied or scanned;
    // the selection and the clipboard are both left exactly as they were, so
    // the reader lands back on the screen they were about to act from.
    //
    // AHEAD OF THE SELECT-MODE CASE BELOW, deliberately: the confirm-delete
    // screen is reached FROM select mode, so both conditions are true at once
    // there, and the reader backing out of a confirmation means the
    // confirmation -- not their whole selection, which they would then have
    // to tick all over again.
    if (nf_browser_menu != NF_MENU_NONE) {
        nh_log("browser: BACK closes the %s -- nothing was selected, applied, deleted, moved, copied or scanned",
               nf_menu_name(nf_browser_menu));
        nf_browser_menu = NF_MENU_NONE;
        nf_browser_go(mwc, dialog, QString::fromUtf8(nf_browser_cwd), false);
        return;
    }

    // CASE 1b: select mode is on. Leaving it is what BACK means here, and it
    // is a CASE IN THIS FUNCTION rather than a second path for exactly the
    // reason the submenu case above is: the two independent exits (the bar
    // control and N3Dialog's own backTapped()) share one place to read "where
    // am I" from, and select mode is one more answer to that question.
    //
    // It does NOT go up a directory, for the same reason a submenu does not:
    // the reader is backing out of a MODE, not leaving the folder. The
    // selection is cleared on the way out -- a tick that survived select mode
    // would be invisible and would still act on a later `delete`.
    if (nf_browser_select) {
        nh_log("browser: BACK leaves select mode -- nothing is deleted, moved or copied");
        nf_browser_select = false;
        nf_selection_clear("left select mode");
        nf_browser_go(mwc, dialog, QString::fromUtf8(nf_browser_cwd), false);
        return;
    }

    // CASE 2, unchanged: up one level, popping the dialog only at the root.
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
// is 17 rows on 1680 visible px, nf_items_per_page is keyed to that, and every
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
        // the cost in items per page (12 -> 9, and 11 after the two chrome
        // bars landed -- nf_items_per_page, nffmt.h).
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
        // every page (nf_items_per_page's own comment, and NF_COVER_H_PX's in
        // nffmt.h, for why that number is the owner's and what to do if a
        // screenshot shows fewer rows than it returns).
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
// "<< BACK", "page N/M", "NEXT PAGE >", "sort: name (asc)", "filter: all" -- one
// TouchLabel each, one per line of the panel. It is now two horizontal bars:
// a command bar across the top (BACK | sort | filter | view) and a page bar pinned
// to the bottom (PREV | page N/M | NEXT). Three rows of panel come back,
// which is what paid for the page size going 9 -> 11 (see nf_items_per_page's own
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
//
// THE TWO BARS ARE LAID OUT DIFFERENTLY, and the asymmetry is deliberate.
// The PAGE bar is three short, fixed labels whose middle one is a centred
// counter, so equal slots are exactly right for it (nf_bar_add). The COMMAND
// bar is six or seven labels of wildly different widths, where an equal slot
// elided a 357 px "sort: name (asc)" into "sort: n..." on the device while
// "view" sat in an identical slot it needed a third of -- so it lays out at
// natural widths and wraps to a SECOND bar row rather than eliding
// (nf_bar_plan_layout, nffmt.h, and nf_bar_add_natural below). A second row
// costs one item off each page, which nf_items_per_page is told about.

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

// NF_BAR_MAX_ITEMS (8, for a worst case of seven) used to be defined here and
// now lives in nffmt.h, next to nf_bar_plan, which is sized by it: the plan is
// what this file reads its layout out of, and two spellings of the same bound
// is one of them being wrong later. Overflow past it is dropped from the LOG
// only, never from the bar -- see nf_bar_record.
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
    // the prefix below spends some of that. An item costs ~23 characters, so
    // the command bar's four fit with room; NF_BAR_MAX_ITEMS' six would too.
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
// would quietly break nf_items_per_page's arithmetic, since that counts each
// bar as one 75 px row. The bars sit inside the outer QVBoxLayout, which
// already pays the horizontal margins for them.
//
// ZERO SPACING for a different reason: the separation between items is decided
// per bar and never here -- the PAGE bar gets it from the equal-width slots
// each of its three items is given (addWidget(w, 1)), the COMMAND bar from the
// explicit stretch spacers nf_bar_add_natural puts between its items. A
// spacing set here would be an invented layout constant on top of both, and
// every invented layout constant in this project has so far been wrong.
static QHBoxLayout *nf_new_bar_layout(void) {
    QHBoxLayout *bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(0);
    return bar;
}

// Puts one finished item into a bar in EQUAL SLOTS: stretch 1, so the slots do
// not move when a label's text changes -- "< PREV" becoming "no prev" must not
// shift the page counter beside it -- with the item filling its slot so the
// TAP TARGET is the whole third rather than just the glyphs, and the text
// aligned within it to give the bar its left/centre/right reading.
//
// THE PAGE BAR'S ROUTE, and now only the page bar's. Equal slots are right
// there and wrong for the command bar: the page bar's three items are a fixed
// set of short, stable labels whose middle one is a CENTRED counter, and
// centring it in the bar is exactly what an equal middle slot does. The
// command bar's are six or seven labels of wildly different widths, which is
// what the equal slot elided into uselessness -- see nf_bar_plan_layout
// (nffmt.h) and nf_bar_add_natural below.
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

// Puts one finished item into a bar at its NATURAL width, with the leftover
// width of the bar spread between the items as spacing.
//
// `first` says whether this is the first item on ITS row; every later item
// gets a stretch spacer in front of it, so a row of k items has k-1 spacers
// sharing whatever the labels did not use. That puts the first item hard
// against the left edge and the last hard against the right, which is the
// reading the equal-slot version got from its end alignments -- without the
// equal slots.
//
// STRETCH 0 ON THE WIDGET is what makes it natural-width: with the spacers
// carrying all the stretch, a QLabel's own Preferred policy has nothing to
// grow into. The one exception is a row holding a SINGLE item, which gets the
// old full-width fill (stretch 1) instead -- that row is the confirmation
// screens' `< cancel`, whose whole job is to be the one big tap target on a
// screen asking a destructive question, and shrinking it to the width of six
// glyphs would be a regression this task was not asked for.
//
// The tap target is otherwise the label's own box: 75 px tall (a full chrome
// bar row -- the widget fills the bar vertically) by 68-357 px wide for the
// labels this bar carries, which is a finger-sized target in both directions.
static void nf_bar_add_natural(QHBoxLayout *bar, nf_bar_item *items, int *n,
                               QLabel *item, char const *name, bool first, bool alone) {
    // Alignment is moot for a widget that is exactly its sizeHint, and set
    // anyway so that an ELIDED label (rule 3, nffmt.h) still reads from its
    // start rather than from wherever Qt's default put it.
    item->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (!first)
        bar->addStretch(1);
    bar->addWidget(item, alone ? 1 : 0);
    nf_bar_record(items, n, item, name);
}

// --- the command bar's item set ------------------------------------------
//
// EVERY tappable control the command bar can hold, in one enum, so the bar is
// a TABLE rather than a run of near-identical blocks. That restructuring is
// this task's, and it is not tidying: the bar's contents now depend on the
// mode (browse / select / a confirmation), on whether anything is on the
// clipboard, and -- for `rescan` -- on whether a libnickel symbol resolved.
// Spelled as six or seven independent `if` blocks each with its own lambda,
// the ALIGNMENT alone would be wrong in half of them, because "first item
// left, last item right, everything else centred" is a property of the SET
// and cannot be decided by a block that does not know what follows it.
enum nf_cmd {
    NF_CMD_BACK,    // browse: up one level / leave at the root. The guaranteed exit.
    NF_CMD_SORT,
    NF_CMD_FILTER,
    NF_CMD_VIEW,
    NF_CMD_SELECT,  // browse: turn select mode on
    NF_CMD_RESCAN,  // browse: ask Nickel to re-import the card (confirms first)
    NF_CMD_PASTE,   // either mode, only while the clipboard is non-empty
    NF_CMD_DONE,    // select: leave select mode. THIS MODE'S guaranteed exit.
    NF_CMD_DELETE,  // select: confirm, then delete the ticked rows
    NF_CMD_CUT,     // select: put the ticked rows on the clipboard as a move
    NF_CMD_COPY,    // select: put the ticked rows on the clipboard as a copy
    NF_CMD_CANCEL,  // a confirmation screen, and the running-operation screen
};

// Fills `out` with the item set for the current mode and returns how many.
// THE FIRST ITEM IS ALWAYS THE EXIT, in every mode, without exception --
// BACK when browsing, `done (N)` in select mode, `cancel` on a confirmation
// -- because the one thing this screen may never become is a place the owner
// cannot leave, on their daily-use device, with getDialog's own X already
// dead on this route (nfview.h, review finding I-3).
static int nf_bar_commands(nf_cmd *out) {
    int n = 0;

    if (nf_menu_is_confirm(nf_browser_menu)) {
        // A confirmation gets ONE control, and deliberately not the browsing
        // bar: `sort:`/`select`/`rescan` sitting next to "delete 3 items"
        // would all be live tap targets on a screen whose entire job is to
        // ask one question. The action rows are in the content below it.
        out[n++] = NF_CMD_CANCEL;
        return n;
    }

    if (nf_browser_select) {
        out[n++] = NF_CMD_DONE;
        out[n++] = NF_CMD_DELETE;
        out[n++] = NF_CMD_CUT;
        out[n++] = NF_CMD_COPY;
        if (nf_clipboard_count() > 0)
            out[n++] = NF_CMD_PASTE;
        return n;
    }

    out[n++] = NF_CMD_BACK;
    out[n++] = NF_CMD_SORT;
    out[n++] = NF_CMD_FILTER;
    out[n++] = NF_CMD_VIEW;
    out[n++] = NF_CMD_SELECT;
    // `rescan` is only offered when its two symbols resolved. A control that
    // silently does nothing is worse than an absent one, and CLAUDE.md's rule
    // is that a missing symbol degrades the feature and never the mod: a
    // firmware that renamed PlugWorkflowManager::sync loses this one item and
    // nothing else (nfnickel.h).
    if (nf_rescan_available())
        out[n++] = NF_CMD_RESCAN;
    // Paste is available OUTSIDE select mode too, because it acts on the
    // clipboard and the current folder rather than on a selection -- cutting
    // in one folder and pasting in another is the whole point of having a
    // clipboard, and requiring select mode to put it down would be a mode
    // with nothing to select in it.
    if (nf_clipboard_count() > 0)
        out[n++] = NF_CMD_PASTE;
    return n;
}

// What each control SAYS. One place, so the bar and the log agree about which
// control a tap landed on.
static QString nf_cmd_label(nf_cmd cmd) {
    switch (cmd) {
        case NF_CMD_BACK:   return QStringLiteral("< BACK");
        case NF_CMD_SORT:   return nf_sort_bar_label(nf_browser_sort_key, nf_browser_sort_desc);
        case NF_CMD_FILTER: return nf_filter_bar_label(nf_browser_filter);
        case NF_CMD_VIEW:   return nf_view_bar_label(nf_browser_view);
        case NF_CMD_SELECT: return QStringLiteral("select");
        case NF_CMD_RESCAN: return QStringLiteral("rescan");
        case NF_CMD_PASTE:  return nf_paste_bar_label(nf_clipboard_count());
        case NF_CMD_DONE:   return nf_select_bar_label(nf_selection_count());
        case NF_CMD_DELETE: return QStringLiteral("delete");
        case NF_CMD_CUT:    return QStringLiteral("cut");
        case NF_CMD_COPY:   return QStringLiteral("copy");
        case NF_CMD_CANCEL: return QStringLiteral("< cancel");
    }
    return QString(); // unreachable under -Wswitch -Werror
}

// The name the geometry log and any failed-allocation line use. A STRING
// LITERAL, because nf_new_touch_row's `what` is documented to be one.
static char const *nf_cmd_name(nf_cmd cmd) {
    switch (cmd) {
        case NF_CMD_BACK:   return "BACK";
        case NF_CMD_SORT:   return "sort";
        case NF_CMD_FILTER: return "filter";
        case NF_CMD_VIEW:   return "view";
        case NF_CMD_SELECT: return "select";
        case NF_CMD_RESCAN: return "rescan";
        case NF_CMD_PASTE:  return "paste";
        case NF_CMD_DONE:   return "done";
        case NF_CMD_DELETE: return "delete";
        case NF_CMD_CUT:    return "cut";
        case NF_CMD_COPY:   return "copy";
        case NF_CMD_CANCEL: return "cancel";
    }
    return "bar item";
}

// nf_bar_slot_px -- rowWidth/n, the equal share every command-bar item used to
// be elided against -- is GONE, not moved. It was the defect: a 357 px "sort:
// name (asc)" against the 190 px sixth of the bar it was handed, elided to
// "sort: n...", while "view" sat in an identical 190 px slot it needed a third
// of. The command bar now lays out at natural widths and wraps rather than
// elides (nf_bar_plan_layout, nffmt.h); the page bar keeps its equal slots,
// and has no elision arithmetic because its three labels are short and fixed.

// BROWSE mode's half of the content build: the directory listing, its item
// rows and the page bar, added to the `layout` nf_browser_go has already put
// the command bar into.
//
// SPLIT OUT OF nf_browser_go, verbatim, when the submenus landed. The two
// modes share the command bar and the row-width arithmetic and share nothing
// else, so the alternative was one function with the whole listing build
// wrapped in an `if` -- 300 lines of measured, device-verified code re-
// indented for no reason other than the brace. Moving it into a function of
// its own left every line, every log and every step order exactly as the
// 2026-09-27 device run measured them; the only thing that changed is the
// page bar (see there), and that change is this task's own.
//
// `pageItems`/`nPageItems` are the CALLER'S array, not locals, because the
// page bar's geometry may only be READ after setContent -- which happens
// after this function has returned. nf_browser_go's own comment at the end
// has why that ordering is the whole point of those two log lines.
//
// `chromeBarRows` is how many 75 px bar rows the chrome has already taken off
// this page: the page bar plus however many rows the command bar wrapped to
// (nf_bar_plan_layout, nffmt.h). It is PASSED IN rather than recomputed here
// because the command bar is built by the caller, before the mode dispatch,
// and its wrap is decided there from the real label widths.
static void nf_build_listing_content(void *mwc, N3Dialog *dialog, QString const &path,
                                     QWidget *content, QVBoxLayout *layout, int rowWidth,
                                     int chromeBarRows,
                                     nf_bar_item *pageItems, int *nPageItems) {
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
                     &filteredToNothing, nf_browser_view);

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
    //
    // SKIPPED ENTIRELY when the reader has turned covers off: every entry
    // stays empty, so every row falls back to its type icon through
    // nf_row_leading_markup's existing no-cover branch, and not one stat()
    // happens. The tally is not printed either -- over a listing with covers
    // off it would balance trivially (0 shown) and say nothing, which is
    // worse than silence because a partition that always adds up is not a
    // check. The one line below says why it is missing, so a log with no
    // cover tally is never mistaken for a cover tally that failed to run.
    QVector<QString> coverPaths(rows.size());
    if (nf_browser_view.hideCovers) {
        nh_log("covers: OFF by the view setting -- no cover paths resolved, no stat() made, every row draws its type icon");
    } else {
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
    }

    // Pagination bounds. totalPages is at least 1 even for an empty listing,
    // so "page 1/1" (below) is always a sensible thing to compute, never a
    // divide-by-zero. nf_browser_page is clamped defensively -- it should
    // already be in range by construction (resetPage zeroes it on every
    // directory change, and PREV/NEXT below never step it out of range),
    // but a stale value surviving some path this file does not currently
    // have is a clamp, not a crash, which is cheap insurance to keep.
    //
    // THE PAGE SIZE DEPENDS ON THE MODE AND ON THE CHROME (nf_items_per_page,
    // nffmt.h): 11 with covers on and 15 with them off against a two-row
    // chrome, because an icon row is 75 px where a cover row is 99 -- and 11
    // and 14 when the command bar has wrapped to a second row, which takes
    // another 75 px off the page. Read ONCE, here, into a local -- every use
    // below is the same number for this build, and calling the function three
    // times would invite a future version of it that could answer differently
    // mid-build.
    //
    // Turning covers ON shrinks the page, and so does a wrap; either can leave
    // nf_browser_page past the end -- the clamp two lines down already handles
    // it, and is the same clamp a filter that shrinks the listing relies on
    // (nf_menu_select's own comment). Nothing extra is needed for either.
    int itemsPerPage = nf_items_per_page(!nf_browser_view.hideCovers, chromeBarRows);
    int totalPages = (rows.size() + itemsPerPage - 1) / itemsPerPage;
    if (totalPages < 1)
        totalPages = 1;
    if (nf_browser_page >= totalPages)
        nf_browser_page = totalPages - 1;
    if (nf_browser_page < 0)
        nf_browser_page = 0;
    int startIdx = nf_browser_page * itemsPerPage;
    int endIdx   = qMin(startIdx + itemsPerPage, rows.size());
    // hasPrev/hasNext are NOT computed here any more: the page bar asks
    // nf_page_bar_labels (nffmt.h) for both the labels and the two active
    // flags in one call, so "is this end live" has one answer, made in the
    // one place a host test can reach it.

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

    // WHAT THE LAST OPERATION DID, shown ONCE and then forgotten. An
    // operation that refused half of what it was given must say so somewhere
    // the owner will actually look, and the log is not that place on a device
    // driven by a thumb -- "deleted 2, refused 1, failed 0 of 3" is the only
    // thing on this screen that distinguishes a partial refusal from a clean
    // success, because both leave a listing with fewer rows in it.
    //
    // Cleared as it is rendered, so it belongs to the build that follows the
    // operation and not to every build afterwards: a stale summary sitting
    // above an unrelated folder would be read as that folder's own result.
    // A plain QLabel -- informational, never a tap target, like the page
    // counter and the empty-folder message.
    if (nf_op_message[0]) {
        QLabel *opMsg = new QLabel(content);
        opMsg->setTextFormat(Qt::PlainText);
        opMsg->setWordWrap(true);
        opMsg->setText(QString::fromUtf8(nf_op_message));
        layout->addWidget(opMsg);
        nf_op_message[0] = '\0';
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
        nf_row_suffix(r, nf_browser_view, &suffixMarkup, &suffixPlain);

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

        // THE SELECT-MODE TICK, appended to the leading markup and PAID FOR
        // out of the same budget, which is the whole reason it goes through
        // the two-form nf_select_marker (nffmt.cc) rather than being tacked
        // onto the label: a fragment that is drawn without being measured is
        // a fragment that pushes one or two characters off the right edge,
        // which is exactly what NOTES.md Task 13 records at length.
        //
        // IN THE ROW'S TEXT, not in a style, because this panel has four grey
        // levels and "slightly lighter" reads as "the same" -- the finding
        // that already put "[not in library]" into a row's words. Both states
        // are marked ("[x]" and "[ ]"), so the tick column is visible before
        // anything is in it.
        bool rowSelected = false;
        if (nf_browser_select) {
            rowSelected = nf_browser_selection && nf_browser_selection->contains(r.name);
            QString selMarkup, selPlain;
            nf_select_marker(rowSelected, &selMarkup, &selPlain);
            leading  += selMarkup;
            leadWidth += fm.width(selPlain);
        }

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
            // The busy guard, as in every tap handler here: a chunked copy
            // yields to the event loop, so a row tap really can be delivered
            // mid-operation, and descending into a folder while one is
            // running would rebuild the content the progress label lives on.
            if (nf_op_busy) {
                nh_log("browser: row tap ignored -- a file operation is running");
                return;
            }

            // SELECT MODE: a tap TICKS, it does not open. Read off the
            // file-scope flag rather than captured at build time so this can
            // never act on a mode the screen has since left -- the content is
            // rebuilt when the mode changes, so the two agree either way, and
            // reading it live is the one that stays true if that ever stops
            // being so.
            //
            // A folder ticks like anything else: it can be cut (rename(2)
            // moves a directory whole) and it can be deleted if it is empty.
            // A file with no library row ticks too -- it is a file on the
            // card, and the reason it cannot be OPENED has nothing to do with
            // whether it can be moved or deleted.
            if (nf_browser_select) {
                QStringList *sel = nf_selection();
                if (!sel) {
                    nh_log("select: no selection list could be allocated -- the tap does nothing");
                    return;
                }
                int at = sel->indexOf(rowName);
                if (at >= 0)
                    sel->removeAt(at);
                else
                    *sel << rowName;
                // The COUNT first, then the name and its length: nh_log
                // truncates at 256 bytes silently and names on this card run
                // past 230 characters, so the number has to survive even when
                // the name does not (CLAUDE.md).
                nh_log("select: %d ticked after %s a %d-char name",
                       sel->size(), at >= 0 ? "unticking" : "ticking", rowName.length());
                // Same directory, same page -- only the ticks changed.
                nf_browser_go(mwc, dialog, QString::fromUtf8(nf_browser_cwd), false);
                return;
            }

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
    // also what makes nf_items_per_page's 75/99 px terms mean one thing
    // rather than two.
    layout->addStretch(1);

    // --- THE PAGE BAR, one row pinned to the bottom ---------------------
    //
    //     < PREV        page 2/4        NEXT >
    //
    // The three labels come from nf_page_bar_labels (nffmt.h) -- pure and
    // host-tested, because the one real DECISION here (what the ends do when
    // there is no such page) is the part that can be tested off-device, and
    // the layout is the part that cannot.
    //
    // AN UNAVAILABLE END IS NOT SHOWN AT ALL. It used to read "no prev"/"no
    // next", and those words were there for exactly one reason: to stop the
    // bar's layout jumping as a reader pages through a folder, which would
    // leave the page counter sliding around under their thumb. The owner
    // asked for the words gone. THE PROPERTY THEY EXISTED FOR IS KEPT -- and
    // it was never the text holding it up.
    //
    // WIDTH IS RESERVED, BY THE SLOT, and that is the more robust of the two
    // ways to satisfy it:
    //   - This bar is three slots of EQUAL STRETCH (nf_bar_add's
    //     addWidget(w, 1)) and nothing else, so Qt's layout engine hands each
    //     one a third of the bar and the counter's slot is the middle third
    //     whatever the other two contain -- its geometry does not read their
    //     text at all. The one condition is that no item's minimum size
    //     exceeds its own third, and at ~399 px a slot against labels of
    //     60-150 px that is not close; it is stated because it is the only
    //     way this could stop being true.
    //   - A `[prev] [stretch] [page] [stretch] [next]` arrangement is the
    //     alternative, and it is the WEAKER one: working the geometry through,
    //     the middle item's centre lands at `W/2 + (prev - next)/2`, i.e. it
    //     is centred only while the two ENDS ARE THE SAME WIDTH AS EACH OTHER.
    //     "< PREV" on the left against an empty right-hand end is precisely
    //     the case this change creates, so the arrangement that sounds like it
    //     solves this is the one that fails on the very page it has to work
    //     on. Equal stretch has no such term in it.
    //
    // INVISIBLE MUST NOT MEAN REMOVED FROM THE LAYOUT. That is the trap on
    // the route that looks obvious: a hidden widget's QWidgetItem reports
    // isEmpty(), and what a box layout then does with its slot is a Qt
    // internal this project would be betting a layout guarantee on, sight
    // unseen, on a Qt (5.2.1) it cannot run a host test against. An empty
    // TEXT needs none of that answered -- the widget is present, visible and
    // laid out exactly as a widget with text is; it simply draws nothing. So
    // setVisible(false) is not used here, deliberately, and this paragraph is
    // why rather than an oversight.
    //
    // A plain QLabel for an unavailable end, never an unconnected TouchLabel:
    // an end that cannot act must not be a tap target at all, rather than one
    // that silently does nothing. It also skips a 256-byte allocation and a
    // gesture registration for a control with nothing to do.
    //
    // The whole bar is still unconditional, including on a single-page
    // listing, where it renders as the counter alone with blank slots either
    // side. A bar that was sometimes absent would make the height of the item
    // area depend on the folder, and nf_items_per_page's arithmetic counts
    // exactly two chrome rows on every page.
    //
    // `path` is captured by value in both handlers, and nf_browser_go is
    // called with resetPage=FALSE: this is a page change WITHIN the current
    // directory, not a navigation to a different one, so nf_browser_page must
    // survive the rebuild it triggers.
    QString prevLabel, pageLabel, nextLabel;
    bool    prevActive = false, nextActive = false;
    nf_page_bar_labels(nf_browser_page, totalPages,
                       &prevLabel, &prevActive, &pageLabel, &nextLabel, &nextActive);

    QHBoxLayout *pageBar = nf_new_bar_layout();
    *nPageItems = 0;

    {
        QLabel *item = NULL;
        bool    live = false;
        if (prevActive) {
            QPushButton *shim = NULL;
            item = nf_new_touch_row(content, "PREV", &shim);
            if (item) {
                live = true;
                QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path] {
                    if (nf_op_busy) { // see nf_op_busy -- every tap handler carries this
                        nh_log("browser: PREV ignored -- a file operation is running");
                        return;
                    }
                    nf_browser_page--;
                    nh_log("browser: page -- prev, now %d in '%s'", nf_browser_page, qPrintable(path));
                    nf_browser_go(mwc, dialog, path, false);
                });
            }
        }
        // Two ways to get here without a live control: there IS no previous
        // page (the ordinary case -- nf_page_bar_labels hands back an empty
        // label for it), or the TouchLabel allocation failed (already logged
        // by nf_new_touch_row). A plain QLabel covers both and the SLOT is
        // held open either way, so neither one slides the counter out from
        // under the reader's eye. BLANK in both cases: a label that looks like
        // a control and cannot receive a tap is worse than a gap.
        if (!item)
            item = new QLabel(content);
        item->setText(live ? prevLabel : QString());
        nf_bar_add(pageBar, pageItems, nPageItems, item,
                   live ? "PREV" : (prevActive ? "prev(alloc failed)" : "prev(blank)"),
                   Qt::AlignLeft);
    }

    // The counter: informational only, never a tap target, so a plain QLabel
    // needs none of TouchLabel's gesture machinery -- same as the stacked
    // chrome's own page indicator. It is also the one item in this bar that
    // always has text, which is what keeps the bar a full row tall now that
    // the ends can be empty.
    {
        QLabel *item = new QLabel(content);
        item->setText(pageLabel);
        nf_bar_add(pageBar, pageItems, nPageItems, item, "page", Qt::AlignHCenter);
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
                    if (nf_op_busy) { // see nf_op_busy -- every tap handler carries this
                        nh_log("browser: NEXT ignored -- a file operation is running");
                        return;
                    }
                    nf_browser_page++;
                    nh_log("browser: page -- next, now %d in '%s'", nf_browser_page, qPrintable(path));
                    nf_browser_go(mwc, dialog, path, false);
                });
            }
        }
        if (!item) // blank, or a blank held-open slot -- see PREV's own comment
            item = new QLabel(content);
        item->setText(live ? nextLabel : QString());
        nf_bar_add(pageBar, pageItems, nPageItems, item,
                   live ? "NEXT" : (nextActive ? "next(alloc failed)" : "next(blank)"),
                   Qt::AlignRight);
    }

    layout->addLayout(pageBar);

    nh_log("browser: showing '%s' (%d row(s), page %d/%d, %d shown)",
           qPrintable(path), rows.size(), nf_browser_page + 1, totalPages, endIdx - startIdx);
}

// Opens `menu`, or -- if it is the one already open -- closes it. All three
// command-bar menu items run this, so "tap sort: again to put it away" and
// "tap filter: while the sort menu is up to switch to it" are ONE rule rather
// than three, and there is no second place for the mode to be set from a bar
// tap.
//
// Nothing about the listing changes here: the sort key, the direction, the
// filter, the directory and the page are all untouched, so closing a menu
// this way lands on exactly the screen that was showing before it opened --
// the same guarantee BACK gives (nf_browser_back's own first case).
// resetPage is false for that reason.
static void nf_browser_open_menu(void *mwc, N3Dialog *dialog, QString const &path,
                                 nf_menu_kind menu) {
    if (nf_op_busy) { // see nf_op_busy -- every tap handler carries this
        nh_log("browser: opening the %s ignored -- a file operation is running", nf_menu_name(menu));
        return;
    }
    nf_browser_menu = (nf_browser_menu == menu) ? NF_MENU_NONE : menu;
    nh_log("browser: command bar tap -- mode is now %s", nf_menu_name(nf_browser_menu));
    nf_browser_go(mwc, dialog, path, false); // same directory, same page
}

// What tapping option `index` of `menu` does. Split out of the row loop's
// lambda so the three menus' rules sit next to each other and the asymmetries
// between them are visible in one place: the sort menu's ALREADY-ACTIVE row
// toggles direction, which is the only way direction is reachable now that
// tapping `sort:` opens a menu instead of cycling -- and which the active
// row's own text promises ("* date (asc) (tap for desc)", nffmt.cc). The
// filter menu has no second axis, so its already-active row just closes. The
// VIEW menu has no active row at all: every one of its rows is a toggle, so
// every tap flips something and there is no "select the one already selected"
// case to answer.
//
// Both paths end in the same two steps: back to BROWSE, then rebuild with
// resetPage=FALSE. FALSE, not true, is the whole "return to the page you were
// on" requirement -- nf_browser_page is untouched while a menu is open, so it
// is still the page the reader left. The one case where the change invalidates
// it (a filter that shrinks the listing past the current page) is handled by
// nf_browser_go's OWN clamp, which is reused rather than duplicated here
// precisely because it already exists and is already the one place the page
// is bounded. The old behaviour -- a sort or filter tap resetting to page 0 --
// is what this replaces.
static void nf_menu_select(void *mwc, N3Dialog *dialog, QString const &path,
                           nf_menu_kind menu, int index) {
    if (nf_op_busy) { // see nf_op_busy -- every tap handler carries this
        nh_log("browser: menu row %d ignored -- a file operation is running", index);
        return;
    }
    if (menu == NF_MENU_SORT) {
        nf_sort_key key = NF_SORT_NAME;
        if (!nf_menu_sort_key_at(index, &key)) {
            // Unreachable while the row loop below only wires indices it
            // built, and answered anyway rather than acted on: a nonsense
            // index must not become a confident wrong sort key.
            nh_log("browser: sort menu row %d is out of range -- ignoring the tap", index);
            return;
        }
        if (key == nf_browser_sort_key)
            nf_browser_sort_desc = !nf_browser_sort_desc;
        else
            nf_browser_sort_key = key; // direction KEPT -- what the row's label promises
        nh_log("browser: sort -- now %s", qPrintable(nf_sort_row_label()));
    } else if (menu == NF_MENU_FILTER) {
        nf_filter_kind filter = NF_FILTER_ALL;
        if (!nf_menu_filter_at(index, &filter)) {
            nh_log("browser: filter menu row %d is out of range -- ignoring the tap", index);
            return;
        }
        nf_browser_filter = filter;
        nh_log("browser: filter -- now %s", qPrintable(nf_filter_row_label()));
    } else if (menu == NF_MENU_VIEW) {
        nf_view_toggle toggle = NF_VIEW_FILENAMES;
        if (!nf_menu_view_toggle_at(index, &toggle)) {
            nh_log("browser: view menu row %d is out of range -- ignoring the tap", index);
            return;
        }
        // nf_view_flag (nffmt.cc) is the ONE place a toggle maps onto a field,
        // so this file has no switch of its own to fall out of step with the
        // menu's labels. A NULL is the same refusal the index check above is:
        // a toggle this build does not know must flip nothing rather than
        // flip something.
        bool *flag = nf_view_flag(&nf_browser_view, toggle);
        if (!flag) {
            nh_log("browser: view menu row %d maps to no flag -- ignoring the tap", index);
            return;
        }
        *flag = !*flag;
        // The WHOLE flag set, not just the one that moved: the point of this
        // line is that the next content build's rows can be attributed to a
        // mode, and one flag's new value does not say what mode that is.
        nh_log("browser: view -- %s", qPrintable(nf_view_flags_summary(nf_browser_view)));
    } else {
        nh_log("browser: a menu row fired with no menu open -- ignoring it");
        return;
    }

    nf_browser_menu = NF_MENU_NONE;
    nf_browser_go(mwc, dialog, path, false); // same directory, same page -- see above
}

// SORT/FILTER mode's half of the content build: one tappable row per option,
// in place of the item listing, added to the `layout` nf_browser_go has
// already put the command bar into.
//
// THE COMMAND BAR STAYS AND THE PAGE BAR GOES. The bar stays because BACK
// lives in it and BACK is this screen's guaranteed exit -- a menu that could
// be entered and not left would be the same dead end the X button already is
// on this route (nfview.h, review finding I-3). The page bar goes because
// paging through a five- or eight-row menu is meaningless. That leaves one
// chrome row instead of two here, i.e. MORE vertical room than the listing
// has, so the longest menu (eight filter rows against nf_items_per_page's
// eleven) fits with room to spare and there is no pagination to build. If a
// menu ever grows past what one screen holds, the log line at the end is what
// says so -- it prints both the count built and the count wanted, rather than
// silently cutting the last option off the bottom.
//
// PLAIN TEXT, not rich text, and that is the one real difference from an item
// row: a menu label carries no <img>, no suffix and no card data, so nothing
// here needs escaping, nothing needs a non-breaking space, and the two-form
// measure/render split the item rows need (nffmt.h) has nothing to keep in
// step. setTextFormat is still set EXPLICITLY, for the same reason the item
// rows set theirs: Qt::AutoText decides by INSPECTING THE STRING, and no
// row's rendering may depend on what the string happens to contain.
static void nf_build_menu_content(void *mwc, N3Dialog *dialog, QString const &path,
                                  QWidget *content, QVBoxLayout *layout, int rowWidth) {
    nf_menu_kind menu = nf_browser_menu;
    int          want = nf_menu_row_count(menu);
    int          built = 0;

    // The increment is in the for-header, NOT the last statement of the body,
    // which is why the `continue` below is safe. A `continue` in a loop whose
    // increment is the last body statement hung Nickel's GUI thread once in
    // this project -- no crash, PID unchanged, the device needed a power
    // cycle -- so the shape is worth naming wherever a `continue` appears.
    for (int i = 0; i < want; i++) {
        QPushButton *shim = NULL;
        // `what` is only ever a string literal (nf_new_touch_row), which is
        // why this is a nested ternary rather than a built string -- and the
        // three names are what a failed-allocation log line says went missing.
        QLabel *item = nf_new_touch_row(content,
                                        menu == NF_MENU_SORT   ? "sort menu" :
                                        menu == NF_MENU_FILTER ? "filter menu" :
                                                                 "view menu",
                                        &shim);
        if (!item) {
            // Already logged by nf_new_touch_row. NOT fatal and NOT a break:
            // one missing option is better than a menu that stops halfway,
            // and the tally below is what says one is gone.
            continue;
        }

        QString label = nf_menu_row_label(menu, i, nf_browser_sort_key,
                                          nf_browser_sort_desc, nf_browser_filter,
                                          nf_browser_view);

        // THE SAME width arithmetic every item row uses, less the two terms a
        // menu row does not have (no leading image, no suffix) -- so the
        // MEASURED/FALLBACK marker nf_browser_go has already logged for this
        // build describes this half too, rather than describing a number
        // nothing here spends. nf_name_budget_px is reused for its floor
        // alone, which is the only part of it that can bite here.
        //
        // ElideRight rather than the item rows' ElideMiddle: a head-and-tail
        // elision exists to tell two long FILENAMES apart, where these are
        // this mod's own short words and the head is what identifies them.
        // Unreachable today at ~20 characters against a ~1196 px row, and
        // kept so a firmware with a much larger row font degrades instead of
        // overflowing. There is no second sizeHint pass like the item rows'
        // either: the four terms it corrects for (an <img> box, a suffix
        // twin, an italic stylesheet, a `&nbsp;` separator) are all things a
        // menu row does not have.
        QFontMetrics fm(item->font());
        int textWidth = rowWidth - nf_row_label_inset_px(item, fm);
        item->setTextFormat(Qt::PlainText);
        item->setText(fm.elidedText(label, Qt::ElideRight,
                                    nf_name_budget_px(textWidth, 0, 0)));
        item->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        // Captured by VALUE, same as the item rows' handlers and for the same
        // reason: these lambdas only run off a LATER tap, i.e. a separate
        // invocation of the Qt event loop, long after this function's locals
        // are gone.
        int index = i;
        QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path, menu, index] {
            nf_menu_select(mwc, dialog, path, menu, index);
        });

        layout->addWidget(item);
        built++;
    }

    // Pins the menu to the TOP the same way the listing's own stretch pins
    // its page bar to the bottom: without it a QVBoxLayout hands the spare
    // vertical space to the widgets themselves (QLabel's vertical size policy
    // can grow), so a five-row sort menu would render as five rows stretched
    // down the whole panel.
    layout->addStretch(1);

    nh_log("browser: showing the %s -- %d of %d option row(s) built, over '%s'",
           nf_menu_name(menu), built, want, qPrintable(path));
}

// --- the file operations -------------------------------------------------
//
// Everything below runs on Nickel's GUI thread, inside a Qt signal handler,
// which is one of the two windows CLAUDE.md names as safe for touching
// /mnt/onboard at all. The destructive work is nfops.cc's and the rules are
// nfpath.cc's; what lives here is the SCREEN around them -- the confirmation,
// the progress, the cancel, and the summary line the next listing carries.
//
// THE RE-ENTRANCY DISCIPLINE, in one place so it is not re-derived at each
// call site: every tap handler in this file begins by returning if
// nf_op_busy is set, and the ONLY control exempt is the cancel row on the
// progress screen, which sets a flag and returns without navigating. A
// chunked copy yields to the event loop between chunks, so Qt really can
// deliver a tap in the middle of one, and the alternative to this guard is a
// second operation starting over files the first is half way through.

static void nf_op_say(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nf_op_say(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(nf_op_message, sizeof nf_op_message, fmt, ap);
    va_end(ap);
    // Logged as well as shown: the panel line is gone the moment the reader
    // navigates, and the log is what a device run reads afterwards.
    nh_log("fileops: %s", nf_op_message);
}

// Records `content` as the widget currently inside the dialog, and arranges
// for that record to clear when Qt destroys it. Lifted out of nf_browser_go
// verbatim when the progress screen (below) became a second place that calls
// N3Dialog::setContent -- two copies of a guarded clear is two chances to
// drop the guard, and the guard is the whole subtlety here (see below).
static void nf_track_content(QWidget *content) {
    nf_browser_active_content = content;
    // The destroyed() clear is guarded on the pointer still being THIS
    // widget: setContent deleteLater()s the previous content, so the previous
    // widget's destroyed() fires later in the event loop, i.e. after this
    // assignment -- an unguarded clear would then null out the LIVE pointer
    // and quietly send the next navigation back to the dialog-width estimate.
    QWidget *tracked = content;
    QObject::connect(content, &QObject::destroyed, [tracked] {
        if (nf_browser_active_content == tracked)
            nf_browser_active_content = NULL;
    });
}

// The progress screen a paste runs behind: a heading, a line that counts
// bytes, and ONE tappable control, which cancels. No command bar at all --
// every other control on this screen would be a live tap target during an
// operation, and the busy guard would make each of them do nothing, which is
// a worse screen than not offering them.
struct NFProgress {
    int     lastPct;   // so the label (and therefore the panel) is only repainted when the number changes
    int     index;     // 1-based item number, for "copying 2 of 5"
    int     total;     // how many items this paste is moving or copying
    QString name;      // the item being worked on
};

// The counter label, or NULL if it has been destroyed under us (see
// nf_progress_label). Every write to the progress line goes through this --
// there is deliberately no second, captured pointer to it anywhere.
static QLabel *nf_progress_line(void) {
    return reinterpret_cast<QLabel*>(nf_progress_label);
}

// nf_op_tick_fn (nfops.h): called between chunks, never during one, with both
// file handles closed. Three jobs, in this order:
//
//   1. update the counter, but only when the whole-number percentage has
//      MOVED -- this is an e-ink panel, and a repaint per 1 MiB chunk on an
//      820 MB file would be 820 refreshes of a label that mostly says the
//      same thing;
//   2. hand the event loop back, which is what keeps the panel alive and is
//      the only way the cancel row below can ever be tapped;
//   3. report whether the reader asked to stop.
static bool nf_copy_tick(void *ctx, qint64 done, qint64 total) {
    NFProgress *p = static_cast<NFProgress*>(ctx);

    int pct = (total > 0) ? (int)((done * 100) / total) : 100;
    QLabel *line = nf_progress_line();
    if (p && line && pct != p->lastPct) {
        p->lastPct = pct;
        line->setText(QStringLiteral("%1 of %2: %3 -- %4%")
                          .arg(p->index).arg(p->total).arg(p->name).arg(pct));
    }

    // THE YIELD. Bounded to 50 ms so a flood of events cannot turn one
    // between-chunks pause into an unbounded one; AllEvents rather than
    // ExcludeUserInputEvents because the cancel row is a TOUCH target and
    // excluding input would make it undeliverable -- i.e. would make the
    // cancel this screen promises impossible.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    return !nf_op_cancel_requested;
}

// Builds and shows the progress screen. The counter label it creates is
// recorded in nf_progress_label and read back through nf_progress_line() for
// the rest of the run -- see that variable's own comment for why a captured
// pointer would be a use-after-free waiting for the device to go to sleep.
static void nf_show_progress_screen(N3Dialog *dialog, QString const& heading) {
    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    QLabel *head = new QLabel(content);
    head->setTextFormat(Qt::PlainText); // never AutoText: a filename must not decide the render mode
    head->setWordWrap(true);
    head->setText(heading);
    layout->addWidget(head);

    QLabel *line = new QLabel(content);
    line->setTextFormat(Qt::PlainText);
    line->setText(QStringLiteral("starting..."));
    layout->addWidget(line);

    // The ONE control. Its handler is the single exemption from the busy
    // guard in this whole file: it sets a flag and returns, touching no
    // widget and navigating nowhere, so it cannot re-enter anything.
    QPushButton *shim = NULL;
    QLabel *cancelRow = nf_new_touch_row(content, "cancel operation", &shim);
    if (cancelRow) {
        cancelRow->setTextFormat(Qt::PlainText);
        cancelRow->setText(QStringLiteral("cancel"));
        cancelRow->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        QObject::connect(shim, &QPushButton::clicked, [] {
            nh_log("fileops: cancel tapped -- the copy will stop at the next chunk boundary and remove its temp file");
            nf_op_cancel_requested = true;
        });
        layout->addWidget(cancelRow);
    } else {
        // Already logged by nf_new_touch_row. Worth its own line because the
        // consequence is specific: an 820 MB copy with no way to stop it.
        nh_log("fileops: the cancel control could not be allocated -- this operation cannot be cancelled");
    }

    layout->addStretch(1);
    N3Dialog__setContent(dialog, content);
    nf_track_content(content);

    // Tracked, and cleared by Qt itself if anything destroys it mid-copy.
    // Guarded on the pointer still being THIS label, the same guard
    // nf_track_content carries and for the same reason: an older progress
    // label's destroyed() can fire after a newer one has been recorded.
    nf_progress_label = line;
    QLabel *trackedLine = line;
    QObject::connect(line, &QObject::destroyed, [trackedLine] {
        if (nf_progress_label == trackedLine)
            nf_progress_label = NULL;
    });
}

// The one place a selection name becomes a path. Refuses a name that is not a
// single safe component before it is ever joined -- a name containing a '/'
// would make `dir + "/" + name` a path two levels down with no ".."
// anywhere, which is the one way a source can escape the folder it was
// ticked in. Unreachable from QDir::entryInfoList's own output, and checked
// because "unreachable from today's caller" is not the same as "impossible".
static bool nf_join_in_dir(QString const& dir, QString const& name, QString *out) {
    if (!nf_path_name_is_safe(name)) {
        nh_log("fileops: refused an UNSAFE entry name (%d chars) -- not a single path component",
               name.length());
        return false;
    }
    *out = dir + QLatin1Char('/') + name;
    return true;
}

// DELETE the ticked rows, after the confirmation screen has already asked.
// Files and EMPTY directories only -- nfops.cc refuses the rest and says why,
// per row, and the tally below is what makes a partial refusal visible rather
// than silent.
static void nf_run_delete(void *mwc, N3Dialog *dialog, QString const &path) {
    QStringList names = nf_browser_selection ? *nf_browser_selection : QStringList();
    nf_browser_menu = NF_MENU_NONE;

    if (names.isEmpty()) {
        nf_op_say("nothing was ticked, so nothing was deleted");
        nf_browser_go(mwc, dialog, path, false);
        return;
    }

    nf_op_busy = true;
    nh_log("fileops: DELETE run starting -- %d ticked row(s) in a %d-char folder path",
           names.size(), path.length());

    int ok = 0, refused = 0, failed = 0;
    // The increment is in the for-header, NOT the last statement of the body,
    // which is what makes any early `continue` safe here. A `continue` in a
    // loop whose increment is the last body statement hung Nickel's GUI
    // thread once in this project -- no crash, PID unchanged, the device
    // needed a power cycle.
    for (int i = 0; i < names.size(); i++) {
        QString full;
        if (!nf_join_in_dir(path, names.at(i), &full)) {
            refused++;
            continue;
        }
        nf_path_verdict why = NF_PATH_OK;
        nf_op_result r = nf_op_delete(full, path, &why);
        if (r == NF_OP_OK)
            ok++;
        else if (r == NF_OP_FAILED)
            failed++;
        else
            refused++;
    }

    nf_op_busy = false;
    // The ticks are gone whatever happened: the rows they pointed at have
    // either been deleted or been refused, and a tick left over from a
    // finished operation is a tick the next `delete` would act on.
    nf_selection_clear("the delete run finished");
    nf_op_say("deleted %d, refused %d, failed %d of %d", ok, refused, failed, names.size());
    // Select mode STAYS ON, with nothing ticked: the reader came here to
    // tidy a folder and is probably not finished. `done (0)` in the bar says
    // plainly that nothing is ticked any more.
    nf_browser_go(mwc, dialog, path, false);
}

// PASTE the clipboard into `path`: a move (rename(2), instant even for the
// 820 MB .cbr) or a chunked, cancellable copy, depending on which verb filled
// it. Never overwrites -- nfops.cc refuses a destination that exists.
static void nf_run_paste(void *mwc, N3Dialog *dialog, QString const &path) {
    QStringList items = nf_browser_clipboard ? *nf_browser_clipboard : QStringList();
    bool cut = nf_browser_clip_cut;
    nf_browser_menu = NF_MENU_NONE;

    if (items.isEmpty()) {
        nf_op_say("the clipboard is empty, so nothing was pasted");
        nf_browser_go(mwc, dialog, path, false);
        return;
    }

    nf_op_busy = true;
    nf_op_cancel_requested = false;
    nh_log("fileops: PASTE run starting -- %s, %d item(s), into a %d-char folder path",
           cut ? "MOVE" : "COPY", items.size(), path.length());

    nf_show_progress_screen(
        dialog, cut ? QStringLiteral("Moving files. This is a rename -- it should be instant.")
                    : QStringLiteral("Copying files. Tap cancel to stop; a part-copied file is never left behind."));

    NFProgress prog;
    prog.lastPct = -1;
    prog.index   = 0;
    prog.total   = items.size();

    int ok = 0, refused = 0, failed = 0, cancelled = 0;
    QStringList remaining; // clipboard entries a cut did NOT manage to move

    for (int i = 0; i < items.size(); i++) {
        prog.index   = i + 1;
        prog.lastPct = -1;
        prog.name    = QFileInfo(items.at(i)).fileName();
        if (QLabel *line = nf_progress_line())
            line->setText(QStringLiteral("%1 of %2: %3 -- starting")
                              .arg(prog.index).arg(prog.total).arg(prog.name));
        // One yield per ITEM as well as one per chunk, so a run of instant
        // renames still lets the panel draw and still lets cancel land.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        // THE DIALOG MAY NOT HAVE SURVIVED THAT YIELD. Nickel can pop our
        // screen for one of its own, and the device can go to sleep, both of
        // which destroy the dialog -- after which `dialog` is a dangling
        // pointer and nf_browser_go below would call setContent through it.
        // nf_browser_active_dialog is cleared by the dialog's own destroyed()
        // signal, so comparing against it is the one honest way to ask.
        //
        // Nothing is undone: the items already moved stay moved, the current
        // item has not started, and the rest stay on the clipboard for a cut.
        if (nf_browser_active_dialog != dialog) {
            for (int k = i; k < items.size(); k++)
                remaining << items.at(k);
            nh_log("fileops: PASTE stopped at item %d of %d -- the browser screen was destroyed while it ran; %d done, %d still pending",
                   prog.index, prog.total, ok, remaining.size());
            nf_op_busy = false;
            nf_op_cancel_requested = false;
            if (cut && !remaining.isEmpty())
                *nf_clipboard() = remaining;
            return; // nothing below may touch `dialog`
        }

        if (nf_op_cancel_requested) {
            // Everything not yet attempted stays on the clipboard for a cut,
            // so a cancelled move can be resumed rather than half-lost.
            cancelled += items.size() - i;
            for (int k = i; k < items.size(); k++)
                remaining << items.at(k);
            nh_log("fileops: PASTE stopped before item %d of %d -- the reader cancelled",
                   prog.index, prog.total);
            break;
        }

        nf_path_verdict why = NF_PATH_OK;
        nf_op_result r = cut
            ? nf_op_move(items.at(i), path, path, &nf_copy_tick, &prog, &why)
            : nf_op_copy(items.at(i), path, path, &nf_copy_tick, &prog, &why);

        if (r == NF_OP_OK) {
            ok++;
        } else if (r == NF_OP_CANCELLED) {
            cancelled++;
            remaining << items.at(i);
        } else if (r == NF_OP_FAILED) {
            failed++;
            remaining << items.at(i);
        } else {
            refused++;
            remaining << items.at(i);
        }
    }

    nf_op_busy = false;
    nf_op_cancel_requested = false;

    // THE CLIPBOARD'S FATE DIFFERS BY VERB, and the asymmetry is deliberate:
    //
    //   a CUT that succeeded has moved the file, so its clipboard entry now
    //   names a path that does not exist -- keeping it would offer to move a
    //   ghost. Only the entries that did NOT move are kept, so a partially
    //   refused or cancelled move can be retried without re-ticking anything.
    //
    //   a COPY leaves every source exactly where it was, so the whole
    //   clipboard stays valid and pasting the same set into a second folder
    //   is a legitimate next action. It is put down with the `clear the
    //   clipboard` row on the paste confirmation (nffmt.cc), which exists for
    //   precisely this.
    if (cut) {
        if (remaining.isEmpty()) {
            nf_clipboard_clear("every cut item moved");
        } else {
            *nf_clipboard() = remaining;
            nh_log("clipboard: %d of %d cut item(s) did not move and stay on the clipboard",
                   remaining.size(), items.size());
        }
    }

    nf_op_say("%s: %d done, %d refused, %d failed, %d cancelled of %d",
              cut ? "move" : "copy", ok, refused, failed, cancelled, items.size());

    // The same check once more, for the yield that happened inside the last
    // item's own copy rather than before it.
    if (nf_browser_active_dialog != dialog) {
        nh_log("fileops: the browser screen was destroyed while the paste ran -- not rebuilding it");
        return;
    }
    nf_browser_go(mwc, dialog, path, false);
}

// THE SCAN HAS FINISHED: rebuild the listing, if and only if that is still
// the right thing to do.
//
// Reached from PlugWorkflowManager::doneProcessing() via the timer hop in
// nf_rescan_arm_refresh below, so by the time this runs the tap that started
// the rescan has fully unwound and an arbitrary amount of time has passed --
// seconds for a real scan, and the owner can have done anything at all in it.
// Every check below but the last is about that gap, and every one of them LOGS
// the decision it made: a refresh that silently does not happen is the exact
// shape that sent the owner navigating away and back to find out whether it
// had.
//
// CAPTURES NOTHING. It reads the dialog, the folder and the mode out of
// file-scope state instead, because a captured pointer would be a promise
// about lifetime that nothing here can keep.
static void nf_rescan_refresh_now(void) {
    if (nf_op_busy) {
        nh_log("rescan: doneProcessing() arrived while a file operation is running -- NOT rebuilding; the operation rebuilds the screen itself when it finishes");
        return;
    }
    if (!nf_rescan_dialog) {
        // Nickel emits this as part of its own post-USB workflow, so it can
        // fire when our button was never pressed -- see the comment in
        // nf_rescan_arm_refresh about why that is CORRECT and must not be
        // "fixed" by gating on our own button. It is unreachable today only
        // because the connection is not made until the first rescan.
        nh_log("rescan: doneProcessing() arrived but no rescan has been started from a screen of ours -- nothing to rebuild");
        return;
    }
    if (nf_browser_active_dialog != nf_rescan_dialog) {
        nh_log("rescan: doneProcessing() -- NOT rebuilding: the browser screen the rescan was started from is gone");
        return;
    }
    if (nf_browser_menu != NF_MENU_NONE) {
        // A menu or a confirmation is on screen, so the listing is not, and
        // rebuilding it now would repaint a screen nobody is looking at (and,
        // on a confirmation, repaint a question mid-decision). Nothing is
        // lost: closing either one calls nf_browser_go, which re-reads the
        // directory and the library rows from scratch.
        nh_log("rescan: doneProcessing() -- NOT rebuilding: the %s is open, and closing it rebuilds the listing anyway",
               nf_menu_name(nf_browser_menu));
        return;
    }
    QString cwd = QString::fromUtf8(nf_rescan_cwd);
    if (cwd != QString::fromUtf8(nf_browser_cwd)) {
        // A refresh that MOVES you is worse than one that does not happen.
        // Both sides go through fromUtf8 because nf_browser_cwd is written
        // with toUtf8 (nf_browser_go's own comment on why the codec has to
        // match), and this card has Cyrillic-named folders under books/.
        nh_log("rescan: doneProcessing() -- NOT rebuilding: the reader has navigated away from the folder the rescan was started in");
        return;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("rescan: doneProcessing() -- NOT rebuilding: MainWindowController::sharedInstance() returned null");
        return;
    }

    nh_log("rescan: doneProcessing() -- REBUILDING '%s' (the scan has finished, so the library rows are now current)",
           qPrintable(cwd));
    // Says so on the panel as well as in the log, because the visible change
    // may be nothing at all -- a scan that found no new files leaves every row
    // exactly as it was, and "nothing happened" and "the refresh never
    // happened" are otherwise the same screen.
    nf_op_say("the library rescan finished -- this folder was refreshed");
    nf_browser_go(mwc, static_cast<N3Dialog*>(nf_rescan_dialog), cwd, false);
}

// Wires doneProcessing() up, ONCE for the life of the process, and records
// which screen and which folder the rescan about to start belongs to.
//
// ONE OBJECT DOES BOTH JOBS. nfview.cc already needs two mechanisms here --
// the hidden-signal-adaptor trick (NickelMenu, src/nickelmenu.cc) to reach a
// lambda from an old-style SIGNAL() without a moc step of its own, and the
// zero-delay QTimer hop parented to the APPLICATION that every destructive
// runner on this screen goes through -- and a QTimer is both: `start()` is one
// of its own public slots, so `doneProcessing() -> SLOT(start())` is the
// adaptor, and its `timeout()` is the hop. The alternative, a hidden
// QPushButton whose clicked() then starts a timer, needs a second object and a
// QWidget parent that outlives every dialog, which there is none of on this
// route. The hop itself is not optional: doneProcessing() can be emitted
// INLINE inside nf_rescan_start() on the "Device is not signed" branch
// (rescan-archaeology.md section 5.1), and a rebuild from inside that emission
// would deleteLater() the content widget whose own shim button is still
// emitting underneath it -- the same use-after-free shape nf_confirm_select's
// own hop exists to prevent.
//
// PARENTED TO THE APPLICATION, never to the dialog or the content, for that
// same reason one level out: the thing this timer fires into can destroy the
// dialog, and a timer owned by the dialog would be destroyed while its own
// timeout() was still being emitted. It is never deleted -- one QTimer for the
// life of the process is a deliberate, permanent allocation, not a leak
// somebody should tidy up.
//
// CONNECTED ONCE, not once per rescan, so N rescans cannot stack N handlers
// and rebuild N times; Qt::UniqueConnection (nfnickel.cc) is the belt to this
// flag's braces. A failed connect deletes the timer and leaves the pointer
// NULL, so the next rescan RETRIES rather than being silently refresh-less
// forever.
//
// IT WILL ALSO FIRE WHEN WE DID NOT ASK: this is Nickel's own post-USB
// workflow signal, so a real USB disconnect emits it too. That is harmless and
// in fact correct -- a rebuild showing fresh data is the right response to a
// finished scan whoever started it -- and it is deliberately NOT gated on our
// own button having been pressed. Do not "fix" that; the guards in
// nf_rescan_refresh_now are what make it safe.
static void nf_rescan_arm_refresh(N3Dialog *dialog, QString const &path) {
    nf_rescan_dialog = dialog;
    QByteArray cwdUtf8 = path.toUtf8(); // toUtf8, matching nf_browser_cwd's own write
    snprintf(nf_rescan_cwd, sizeof nf_rescan_cwd, "%s", cwdUtf8.constData());

    if (nf_rescan_refresh_timer)
        return; // already wired for the life of this process

    QTimer *hop = new QTimer(QCoreApplication::instance());
    hop->setSingleShot(true);
    hop->setInterval(0); // start() with no argument uses this
    if (!nf_rescan_connect_done(hop, SLOT(start()))) {
        // nfnickel.cc has already logged which of the two causes it was.
        // Deleted rather than kept, so a later rescan tries again instead of
        // this one failure making the refresh permanently unavailable.
        nh_log("rescan: the listing will NOT refresh itself when this scan finishes -- reopen the folder to see the result");
        delete hop;
        return;
    }
    QObject::connect(hop, &QTimer::timeout, [] {
        nf_rescan_refresh_now();
    });
    nf_rescan_refresh_timer = hop;
}

// RESCAN, after the confirmation screen has named the Wi-Fi consequence.
// No progress screen and nothing to cancel: what the owner sees next is
// Nickel's own post-USB workflow, not this mod.
//
// IT IS GUARDED LIKE THE OTHER TWO EVEN THOUGH IT "RETURNS AT ONCE", and the
// qualification is the whole point. sync() starting a QThread and returning
// is ONE of its two branches. The other -- rescan-archaeology.md section 5.1,
// the "Device is not signed, it will not sync FS." path -- emits finished()
// INLINE, which runs onDoneProcessing synchronously inside nf_rescan_start():
// it pops controllers, may push a QuiltedViewController, and can open modal
// dialogs, i.e. a NESTED EVENT LOOP, inside this call.
//
// So the two things that shape carries are exactly the two nf_run_paste
// already carries, for exactly the same reason:
//
//   nf_op_busy around the call, so a tap delivered inside that nested loop
//   cannot schedule a second destructive run underneath this frame; and
//
//   a liveness re-check afterwards, because the controllers that branch pops
//   can include our own dialog -- after which `dialog` is dangling and
//   nf_browser_go would call setContent through it.
static void nf_run_rescan(void *mwc, N3Dialog *dialog, QString const &path) {
    nf_browser_menu = NF_MENU_NONE;

    // BEFORE sync(), not after, and that ordering is load-bearing: on the
    // "Device is not signed" branch doneProcessing() is emitted INLINE inside
    // nf_rescan_start() below, so a connection made afterwards would miss the
    // only notification that scan is ever going to send. It also records which
    // screen and which folder this rescan belongs to, which is what the
    // handler checks the world against when it eventually fires.
    nf_rescan_arm_refresh(dialog, path);

    nf_op_busy = true;
    bool started = nf_rescan_start();
    nf_op_busy = false;

    if (started)
        // Says the listing refreshes ITSELF, because the alternative reading
        // -- "it is done, and my rows still say [not in library]" -- is the
        // exact misunderstanding this whole change exists to remove. Nothing
        // is left waiting on the signal, though: this message and the rebuild
        // below stand on their own if doneProcessing() never arrives
        // (rescan-archaeology.md section 5.1's silent-no-op branch).
        nf_op_say("rescan started -- it runs in the background; this folder refreshes itself when it finishes, and Wi-Fi comes on");
    else
        nf_op_say("rescan unavailable on this firmware -- nothing was run");

    if (nf_browser_active_dialog != dialog) {
        nh_log("rescan: the browser screen was destroyed while sync() ran -- not rebuilding it (the inline finished() branch pops controllers)");
        return;
    }
    nf_browser_go(mwc, dialog, path, false);
}

// What tapping action row `index` of the current confirmation does. The
// sibling of nf_menu_select, and split out for the same reason: the three
// screens' rules sit next to each other where the asymmetries between them
// are visible in one place.
//
// INDEX 0 IS CANCEL ON ALL THREE (nffmt.cc), and cancel is the same code path
// as BACK's -- close the screen, change nothing. A reader who uses the back
// arrow instead of the row must not get a different answer.
static void nf_confirm_select(void *mwc, N3Dialog *dialog, QString const &path,
                              nf_menu_kind menu, int index) {
    if (nf_op_busy) {
        nh_log("browser: confirmation row %d ignored -- a file operation is running", index);
        return;
    }
    if (index < 0 || index >= nf_confirm_row_count(menu)) {
        // Unreachable while the row loop only wires indices it built, and
        // answered anyway rather than acted on: on THIS screen a nonsense
        // index that fell through to the action row would delete something.
        nh_log("browser: confirmation row %d is out of range -- ignoring the tap", index);
        return;
    }

    if (index == 0) {
        nh_log("browser: %s cancelled -- nothing was deleted, moved, copied or scanned",
               nf_menu_name(menu));
        nf_browser_menu = NF_MENU_NONE;
        nf_browser_go(mwc, dialog, path, false);
        return;
    }
    if (menu == NF_MENU_CONFIRM_PASTE && index == 2) {
        // Putting the clipboard down touches no file at all, so it runs
        // inline like the cancel above rather than taking the deferred route
        // below -- there is nothing here that could outlive this tap.
        nf_clipboard_clear("the reader tapped 'clear the clipboard'");
        nf_op_say("clipboard cleared -- nothing was moved or copied");
        nf_browser_menu = NF_MENU_NONE;
        nf_browser_go(mwc, dialog, path, false);
        return;
    }

    // EVERY DESTRUCTIVE RUN IS DEFERRED OFF THIS TAP'S OWN CALL STACK, by a
    // zero-delay single-shot QTimer, and this is a correctness requirement
    // rather than tidiness. The chain that makes it one:
    //
    //   this function runs inside a QPushButton::clicked emission, from a
    //   shim button that is a CHILD of the confirmation screen's content
    //   widget -> the run replaces that content (N3Dialog::setContent), which
    //   deleteLater()s it -> the chunked copy then calls processEvents()
    //   between chunks, and a DeferredDelete posted at this same event-loop
    //   level is exactly what processEvents() will deliver -> the shim
    //   button, and the TouchLabel whose tapped() is still being emitted
    //   beneath it, are destroyed while their own emission frames are still
    //   on the stack.
    //
    // "A slot may schedule its own object's death" is safe only because
    // deleteLater defers past the end of the emission; a copy loop that
    // re-enters the event loop DURING the emission is the one shape that
    // breaks that, and this is the only place in this project that does it.
    // Hopping through the event loop first means the tap has fully unwound
    // before anything is deleted or written.
    //
    // THE TIMER IS PARENTED TO THE APPLICATION, not to the dialog and not to
    // the content, and that is the same argument one level further out: the
    // copy's processEvents() can deliver whatever destroys our dialog (Nickel
    // popping it for a system dialog of its own is the plausible one), and a
    // timer parented to the dialog would then be destroyed WHILE ITS OWN
    // timeout() was still being emitted -- the very frame the copy is running
    // inside. Owned by the application, it cannot be destroyed by anything
    // except this callback's own last line.
    //
    // It is deleteLater()d at the END of that callback and deliberately not
    // at the start, for the same reason again: a deletion posted before the
    // copy would be delivered by the copy's own processEvents().
    QTimer *hop = new QTimer(QCoreApplication::instance());
    hop->setSingleShot(true);
    QObject::connect(hop, &QTimer::timeout, [mwc, dialog, path, menu, index, hop] {
        // The dialog could in principle have gone between the tap and this
        // callback. Practically it cannot at a zero delay, and it is checked
        // because "practically cannot" is not the standard this file holds
        // its destructive paths to.
        if (nf_browser_active_dialog != dialog) {
            nh_log("browser: the confirmed action was dropped -- the browser screen went away before it could run");
            hop->deleteLater();
            return;
        }
        // AND THE CONFIRMATION MUST STILL BE OPEN. `nf_op_busy` is not set
        // until the runner itself starts, so between the confirming tap and
        // this callback every handler is still live -- and a tap already
        // queued behind the first one can land on `cancel`, which sets
        // nf_browser_menu to NF_MENU_NONE and logs "nothing was deleted".
        // Without this the timer would then fire and delete the files anyway,
        // with both lines in the log and the reader having watched the cancel
        // take effect.
        //
        // Read FRESH here rather than trusted from the capture, which is the
        // same "ask where am I now, never where I was" discipline
        // nf_browser_back follows -- `menu` is the capture and
        // nf_browser_menu is the answer, so they have to agree.
        if (nf_browser_menu != menu) {
            nh_log("browser: the confirmed action was dropped -- the %s was closed before it could run (a cancel or a BACK got there first)",
                   nf_menu_name(menu));
            hop->deleteLater();
            return;
        }
        if (menu == NF_MENU_CONFIRM_DELETE)
            nf_run_delete(mwc, dialog, path);
        else if (menu == NF_MENU_CONFIRM_PASTE)
            nf_run_paste(mwc, dialog, path);
        else if (menu == NF_MENU_CONFIRM_RESCAN)
            nf_run_rescan(mwc, dialog, path);
        else
            nh_log("browser: a confirmation row fired with no confirmation open -- ignoring it");
        hop->deleteLater(); // LAST, never first -- see above
    });
    nh_log("browser: %s confirmed -- running it from the event loop, not from the tap",
           nf_menu_name(menu));
    hop->start(0);
}

// How many of the affected names a confirmation screen lists before it gives
// up and counts the rest. Six fits beside the header and the action rows in
// every mode (the listing has room for 11-15 rows and a confirmation has one
// chrome bar instead of two), and a confirmation that filled the panel with
// names would push its own action rows off the bottom -- which is the one
// failure this screen cannot afford.
#define NF_CONFIRM_LIST_MAX 6

// CONFIRMATION mode's half of the content build: the sentence, the names it
// is about, and the action rows -- added to the `layout` nf_browser_go has
// already put the (single-item) command bar into.
//
// Built like the submenus, on purpose: same TouchLabel rows, same plain text,
// same BACK routing, same elision arithmetic. What differs is that the rows
// are nf_confirm_row_label's rather than nf_menu_row_label's, because a
// confirmation row has to name a COUNT that only this file knows.
static void nf_build_confirm_content(void *mwc, N3Dialog *dialog, QString const &path,
                                     QWidget *content, QVBoxLayout *layout, int rowWidth) {
    nf_menu_kind menu = nf_browser_menu;
    bool cut = nf_browser_clip_cut;

    QStringList names;
    if (menu == NF_MENU_CONFIRM_DELETE) {
        if (nf_browser_selection)
            names = *nf_browser_selection;
    } else if (menu == NF_MENU_CONFIRM_PASTE) {
        if (nf_browser_clipboard) {
            for (int i = 0; i < nf_browser_clipboard->size(); i++)
                names << QFileInfo(nf_browser_clipboard->at(i)).fileName();
        }
    }
    int count = names.size();

    // THE SENTENCE. Word-wrapped and plain text: it is this mod's own words,
    // never card data, and it is the one thing on the screen that says what
    // the action row will do in full.
    {
        QLabel *head = new QLabel(content);
        head->setTextFormat(Qt::PlainText);
        head->setWordWrap(true);
        head->setText(nf_confirm_header(menu, count, cut));
        layout->addWidget(head);
    }

    // For a paste, WHERE the items are coming from. A cut made three folders
    // ago is otherwise a set of bare names with no context at all. The folder
    // NAME only, not its path -- a path would not fit and would not be read.
    if (menu == NF_MENU_CONFIRM_PASTE && nf_browser_clip_dir[0]) {
        QLabel *from = new QLabel(content);
        from->setTextFormat(Qt::PlainText);
        from->setWordWrap(true);
        QString dir = QString::fromUtf8(nf_browser_clip_dir);
        from->setText(QStringLiteral("From: %1").arg(QFileInfo(dir).fileName()));
        layout->addWidget(from);
    }

    // THE NAMES, so "delete 3 items" is checkable against what the reader
    // actually ticked rather than being taken on trust. Elided right, in the
    // row's own font, against the same width arithmetic every other row here
    // uses -- ElideRight and not ElideMiddle because these are being read to
    // confirm a decision already made about them, where a listing's
    // middle-elision exists to tell two similar names apart.
    for (int i = 0; i < names.size() && i < NF_CONFIRM_LIST_MAX; i++) {
        QLabel *nameRow = new QLabel(content);
        nameRow->setTextFormat(Qt::PlainText);
        QFontMetrics fm(nameRow->font());
        int textWidth = rowWidth - nf_row_label_inset_px(nameRow, fm);
        nameRow->setText(fm.elidedText(names.at(i), Qt::ElideRight,
                                       nf_name_budget_px(textWidth, 0, 0)));
        layout->addWidget(nameRow);
    }
    if (names.size() > NF_CONFIRM_LIST_MAX) {
        QLabel *more = new QLabel(content);
        more->setTextFormat(Qt::PlainText);
        more->setText(QStringLiteral("...and %1 more").arg(names.size() - NF_CONFIRM_LIST_MAX));
        layout->addWidget(more);
    }

    // THE ACTION ROWS. Tappable TouchLabels, exactly like the submenus', with
    // `cancel` first -- see nf_confirm_row_label (nffmt.cc) for why the
    // harmless row is the one a mis-tap lands on.
    int want = nf_confirm_row_count(menu), built = 0;
    for (int i = 0; i < want; i++) {
        QPushButton *shim = NULL;
        QLabel *item = nf_new_touch_row(content, "confirmation", &shim);
        if (!item)
            continue; // already logged; the tally below says one is gone
        QString label = nf_confirm_row_label(menu, i, count, cut);
        QFontMetrics fm(item->font());
        int textWidth = rowWidth - nf_row_label_inset_px(item, fm);
        item->setTextFormat(Qt::PlainText);
        item->setText(fm.elidedText(label, Qt::ElideRight, nf_name_budget_px(textWidth, 0, 0)));
        item->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        int index = i;
        QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path, menu, index] {
            nf_confirm_select(mwc, dialog, path, menu, index);
        });
        layout->addWidget(item);
        built++;
    }

    layout->addStretch(1);
    nh_log("browser: showing the %s -- %d of %d action row(s) built over %d name(s), in '%s'",
           nf_menu_name(menu), built, want, count, qPrintable(path));
}

// What tapping one command-bar control does. The single dispatch for the bar,
// the sibling of nf_menu_select and nf_confirm_select, and the one place the
// busy guard is applied to bar taps.
static void nf_bar_command(void *mwc, N3Dialog *dialog, QString const &path, nf_cmd cmd) {
    if (nf_op_busy) {
        nh_log("browser: '%s' ignored -- a file operation is running", nf_cmd_name(cmd));
        return;
    }

    switch (cmd) {
        case NF_CMD_BACK:
        case NF_CMD_DONE:
        case NF_CMD_CANCEL:
            // ALL THREE ARE THE SAME ROUTE. `done` and `cancel` are not
            // second exits with their own logic -- they are the BACK control
            // wearing the word that fits the mode, and they go through
            // nf_browser_back exactly as the guaranteed BACK row and
            // N3Dialog's own backTapped() do. One place reads "where am I"
            // (nfview.h, review finding I-3); this is what keeps it one.
            nf_browser_back(mwc, dialog);
            return;

        case NF_CMD_SORT:   nf_browser_open_menu(mwc, dialog, path, NF_MENU_SORT);   return;
        case NF_CMD_FILTER: nf_browser_open_menu(mwc, dialog, path, NF_MENU_FILTER); return;
        case NF_CMD_VIEW:   nf_browser_open_menu(mwc, dialog, path, NF_MENU_VIEW);   return;

        case NF_CMD_SELECT:
            nf_browser_select = true;
            nf_selection_clear("entering select mode");
            nh_log("browser: select mode ON -- taps tick rows instead of opening them");
            nf_browser_go(mwc, dialog, path, false); // same directory, same page
            return;

        case NF_CMD_RESCAN:
            // CONFIRMS FIRST, always. sync() is not a bare rescan: it is the
            // front of Nickel's whole post-USB workflow and it turns the
            // Wi-Fi on when it finishes (nfnickel.h). The owner asked for a
            // manual button because of that, so the consequence has to be on
            // a screen before anything runs.
            nf_browser_menu = NF_MENU_CONFIRM_RESCAN;
            nf_browser_go(mwc, dialog, path, false);
            return;

        case NF_CMD_PASTE:
            if (nf_clipboard_count() <= 0) {
                // Unreachable while nf_bar_commands only offers this control
                // with a non-empty clipboard; answered rather than acted on.
                nh_log("browser: paste tapped with an empty clipboard -- ignoring it");
                return;
            }
            nf_browser_menu = NF_MENU_CONFIRM_PASTE;
            nf_browser_go(mwc, dialog, path, false);
            return;

        case NF_CMD_DELETE:
            if (nf_selection_count() <= 0) {
                nf_op_say("nothing is ticked, so there is nothing to delete");
                nf_browser_go(mwc, dialog, path, false);
                return;
            }
            // CONFIRMS FIRST. There is no undo anywhere in this design and no
            // recycle bin on this device.
            nf_browser_menu = NF_MENU_CONFIRM_DELETE;
            nf_browser_go(mwc, dialog, path, false);
            return;

        case NF_CMD_CUT:
        case NF_CMD_COPY: {
            bool cut = (cmd == NF_CMD_CUT);
            if (nf_selection_count() <= 0) {
                nf_op_say("nothing is ticked, so there is nothing to %s", cut ? "cut" : "copy");
                nf_browser_go(mwc, dialog, path, false);
                return;
            }
            // The clipboard holds ABSOLUTE paths, because it outlives the
            // folder it was filled in -- that is the whole point of it.
            QStringList *clip = nf_clipboard();
            if (!clip) {
                nf_op_say("could not allocate a clipboard -- nothing was %s", cut ? "cut" : "copied");
                nf_browser_go(mwc, dialog, path, false);
                return;
            }
            clip->clear();
            QStringList names = nf_browser_selection ? *nf_browser_selection : QStringList();
            for (int i = 0; i < names.size(); i++) {
                QString full;
                if (nf_join_in_dir(path, names.at(i), &full))
                    *clip << full;
            }
            nf_browser_clip_cut = cut;
            // toUtf8() to match the QString::fromUtf8() that reads it back
            // on the paste confirmation -- the same write/read codec pairing
            // as nf_browser_cwd's, and wrong for the same reason if it drifts.
            QByteArray clipUtf8 = path.toUtf8();
            snprintf(nf_browser_clip_dir, sizeof nf_browser_clip_dir, "%s", clipUtf8.constData());
            // LEAVES SELECT MODE on the way out: the job has moved to the
            // destination folder, and the reader's next action is to navigate
            // there and paste. Staying ticked would leave a selection that a
            // later `delete` could act on, in a folder they are on their way
            // out of.
            nf_browser_select = false;
            nf_selection_clear(cut ? "cut to the clipboard" : "copied to the clipboard");
            nf_op_say("%d item(s) on the clipboard to %s -- open a folder and tap paste",
                      clip->size(), cut ? "move" : "copy");
            nf_browser_go(mwc, dialog, path, false);
            return;
        }
    }

    nh_log("browser: an unknown command-bar control fired -- ignoring it");
}

// Builds a fresh content widget for `path` and swaps it into the ALREADY-
// EXISTING `dialog` via N3Dialog::setContent -- this is the whole navigation
// model (nfview.h): one N3Dialog for the lifetime of a browse session, rows
// rebuilt in place, never a second dialog pushed per level. setContent itself
// deleteLater()s whatever content was there before (nfnickel.h), so the
// previous screen's rows and their shim buttons are cleaned up by Qt, not by
// this function.
//
// WHAT the content is depends on nf_browser_menu: the directory listing
// (nf_build_listing_content) or one of the two submenus
// (nf_build_menu_content). Both halves get the SAME command bar and the SAME
// row-width arithmetic, and both are the same content swap into the same
// dialog -- a submenu is not a second screen, not a second dialog and not a
// second navigation stack, which is exactly why BACK out of one changes
// nothing about where the reader is (nf_browser_back's own first case).
static void nf_browser_go(void *mwc, N3Dialog *dialog, QString const &path, bool resetPage) {
    // A SELECTION MAY NOT SURVIVE A DIRECTORY CHANGE. Checked here, against
    // the path this call is FOR versus the one already showing, rather than
    // off `resetPage`: resetPage is about the page NUMBER and today happens
    // to be true for exactly the same calls, but the two answer different
    // questions and a future caller that got one right and the other wrong
    // would leave ticks pointing at rows in a folder the reader has left.
    //
    // That is precisely the shape of the accident this rule exists to
    // prevent: a stale tick is invisible -- it is in another folder's listing
    // -- and a later `delete` would act on it anyway.
    //
    // Compared as strings because both are this file's own canonical spelling
    // of a path it built (NF_ROOT, or a parent, or a child name read off
    // disk); nothing here has to resolve a symlink to know whether the reader
    // stayed put.
    if (path != QString::fromUtf8(nf_browser_cwd)) {
        nf_selection_clear("the browser moved to a different folder");
        if (nf_browser_select) {
            // Select mode itself goes too. A mode whose whole content has
            // just been thrown away is a mode that says "4 ticked" and means
            // nothing; the reader turns it back on where they meant to use it.
            nh_log("select: mode OFF -- the browser moved to a different folder");
            nf_browser_select = false;
        }
    }

    // Recorded BEFORE anything below can fail, so BACK's own "where am I"
    // read is always this directory once this function has been entered --
    // matching every row/BACK handler being wired only after the listing
    // for THIS path has been built, never before.
    // toUtf8(), NOT qPrintable(). THE WRITE AND THE READ MUST USE THE SAME
    // CODEC, and until this task nothing compared the two closely enough for
    // the mismatch to show: qPrintable() is toLocal8Bit(), every read of this
    // buffer is QString::fromUtf8(), and those are inverses only when the
    // locale codec happens to be UTF-8 -- Qt 5.2 falls back to Latin-1 under
    // a C/POSIX locale, which is what an init-spawned Nickel is likely to
    // have.
    //
    // What made it matter is three lines up: the selection-clear compares
    // `path` against the value read back out of here, so in a folder whose
    // path is not ASCII the round trip would differ from the original and a
    // mere rebuild would look like a folder change -- the tick would clear
    // and select mode would switch itself off. THIS CARD HAS CYRILLIC-NAMED
    // PDFs under /mnt/onboard/books/, so that is reachable rather than
    // theoretical.
    QByteArray cwdUtf8 = path.toUtf8();
    snprintf(nf_browser_cwd, sizeof nf_browser_cwd, "%s", cwdUtf8.constData());

    // See nf_browser_page's own comment: every real navigation (descend,
    // ascend, the initial root call) passes resetPage=true here, because all
    // three move to a DIFFERENT directory. Everything that stays in the same
    // directory passes false -- the page bar's PREV/NEXT, opening or closing
    // a submenu, and selecting an option in one -- because none of those is a
    // navigation and the reader's page is part of where they are.
    if (resetPage)
        nf_browser_page = 0;

    // ONE LINE PER CONTENT BUILD, naming the mode. The failure this exists to
    // catch is a silent one: "the submenu opened but the rows are the
    // listing's", or the reverse, renders as a perfectly plausible screen and
    // shows up nowhere else at all. Logged here, before anything below can
    // fail, so the line is present even for a build that then goes wrong.
    nh_log("browser: building %s content for '%s' (page %d)",
           nf_menu_name(nf_browser_menu), qPrintable(path), nf_browser_page);
    // THE VIEW FLAGS, once per content build, for exactly the reason the mode
    // line above exists and then some: a screenshot taken under a changed flag
    // is indistinguishable from a rendering bug, and this project has already
    // discarded a WORKING fix on precisely that mistake -- a flag was set, the
    // browser re-triggered, and the screenshot showed rows built under the
    // previous setting (which is also why a re-trigger now rebuilds rather
    // than merely re-pushing, nf_browser_show). With five independent toggles
    // there are 32 modes, so "which one was this screenshot?" is a question
    // that has to be answerable from the log and cannot be answered from the
    // picture. The items-per-page number rides along because it is DERIVED
    // from one of them and is the single easiest thing to check a screenshot
    // against: count the rows.
    //
    // LOGGED AFTER THE COMMAND BAR, not here with the mode line, and that
    // ordering is this task's: the page size now depends on how many rows the
    // command bar wrapped to, which is not known until the bar's labels have
    // been measured. Logged here it would print the unwrapped answer and be
    // off by one item on exactly the screens where the number matters.

    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    // The width every row's label is elided against -- item rows and submenu
    // option rows alike -- see nf_row_width_px for all three sources and why
    // the dialog is what gets read.
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

    // --- THE COMMAND BAR, one row across the top (two if it does not fit) --
    //
    //     < BACK     sort: name (asc)     filter: all     view     select     rescan
    //
    // Independently tappable TouchLabels in one horizontal layout, where
    // there used to be full-width rows. See the "two chrome bars" comment
    // above nf_new_touch_row for why each item must be its own TouchLabel
    // rather than one label with hot zones.
    //
    // THE ITEM SET IS A TABLE (nf_bar_commands, above), not a run of blocks,
    // because the bar's contents depend on the mode (browse / select / a
    // confirmation), on whether anything is on the clipboard, and -- for
    // `rescan` -- on whether a libnickel symbol resolved.
    //
    // LAID OUT AT NATURAL WIDTHS, wrapping to a second row rather than
    // eliding, and that is this task's correction to a device-measured defect:
    // the bar used to hand every item an equal share (one stretch-1 slot each)
    // and elide anything wider, which turned "sort: name (asc)" -- 357 px
    // against a 190 px sixth of the bar -- into "sort: n...", i.e. hid the one
    // piece of state the owner had just asked to have spelled out. The whole
    // decision is nf_bar_plan_layout (nffmt.h), which is pure and host-tested;
    // this loop only supplies the measured widths and renders its answer.
    //
    // The `|` separators in the brief's sketch are NOT drawn: a literal "|"
    // would either be its own TouchLabel (a tap target that does nothing) or
    // live inside a neighbour's text (widening that control's label for no
    // reason). The separation is the stretch spacers between the items.
    //
    // BUILT IN EVERY MODE, and that is a requirement rather than a
    // convenience: the exit lives in this bar, and a screen that could be
    // entered and not left would be a dead end on the owner's daily-use
    // device -- the same dead end getDialog's own X button already is on this
    // route. The FIRST item is always that exit, whatever it is called in the
    // mode it appears in (nf_bar_commands), and greedy row filling keeps it on
    // the FIRST bar row even when the bar wraps (nf_bar_plan_layout).
    nf_cmd cmds[NF_BAR_MAX_ITEMS];
    int    nCmds = nf_bar_commands(cmds);

    // EVERY ITEM IS BUILT AND MEASURED BEFORE ANY OF THEM IS PLACED, which is
    // the ordering the natural-width layout forces: a label's width comes from
    // QFontMetrics on the widget's own font, so the bar cannot know whether it
    // fits until every widget exists. The four parallel arrays are locals of
    // this one function and are indexed by `nBar`, which counts the items that
    // were actually ALLOCATED -- an item whose calloc failed is skipped here
    // and must not leave a hole for the plan to trip over.
    QLabel  *barItem[NF_BAR_MAX_ITEMS];
    QString  barLabel[NF_BAR_MAX_ITEMS];
    int      barNatural[NF_BAR_MAX_ITEMS];
    nf_cmd   barCmd[NF_BAR_MAX_ITEMS];
    int      nBar = 0;

    for (int i = 0; i < nCmds; i++) {
        nf_cmd cmd = cmds[i];
        QPushButton *shim = NULL;
        QLabel *item = nf_new_touch_row(content, nf_cmd_name(cmd), &shim);
        if (!item) {
            // Already logged by nf_new_touch_row. NOT fatal and NOT a break:
            // one missing control is better than half a bar. The exit is the
            // first item built, so if anything survives, it does -- and
            // N3Dialog's own backTapped() is wired independently of this bar
            // regardless (nf_browser_show).
            if (i == 0)
                nh_log("browser: no '%s' control this time (backTapped()/the back arrow is still wired)",
                       nf_cmd_name(cmd));
            continue;
        }
        item->setTextFormat(Qt::PlainText); // never AutoText -- a label must not pick its own render mode

        QObject::connect(shim, &QPushButton::clicked, [mwc, dialog, path, cmd] {
            nf_bar_command(mwc, dialog, path, cmd);
        });

        barItem[nBar]    = item;
        barLabel[nBar]   = nf_cmd_label(cmd);
        barNatural[nBar] = QFontMetrics(item->font()).width(barLabel[nBar]);
        barCmd[nBar]     = cmd;
        nBar++;
    }

    nf_bar_plan plan;
    nf_bar_plan_layout(barNatural, nBar, rowWidth, &plan);

    // THE LINE THAT DIAGNOSED THE DEFECT, kept and extended. It used to fire
    // only when an item elided, which said which control had been cut without
    // ever saying that the SET fitted comfortably and the slot arithmetic was
    // what did not -- so this now prints, every build, the natural total, how
    // much of it is gap budget, the width it was measured against, and which
    // of the three outcomes the plan chose.
    nh_log("browser: command bar -- %d item(s), natural total %d px (labels + %d px of minimum gaps) against %d px: %s%s",
           nBar, plan.naturalTotalPx,
           nBar > 1 ? (nBar - 1) * NF_BAR_MIN_GAP_PX : 0, rowWidth,
           plan.rows > 1 ? "WRAPPED to 2 rows" : "fits on ONE row",
           plan.anyElided ? " -- and STILL had to elide (see the per-item lines)" : "");

    // A WRAPPED COMMAND BAR COSTS ONE ITEM ROW, and the page size has to know:
    // the page bar plus however many rows this bar took. Computed here, where
    // the plan is, and handed to the listing build -- nf_items_per_page's own
    // comment (nffmt.h) has both modes' arithmetic for two rows and for three.
    int chromeBarRows = plan.rows + 1;

    // The view-flags line, moved down from the top of this function so that
    // its items/page number is the one this build will actually use (see the
    // comment up there for why it exists at all). The bar-row count is the
    // LISTING'S charge -- a submenu has no page bar, and no page size either,
    // so the number is only ever spent in browse mode.
    nh_log("browser: view -- %s (%d items/page against %d chrome bar row(s))",
           qPrintable(nf_view_flags_summary(nf_browser_view)),
           nf_items_per_page(!nf_browser_view.hideCovers, chromeBarRows),
           chromeBarRows);

    // ONE QHBoxLayout PER BAR ROW. Both go into the outer QVBoxLayout in
    // order, so a wrapped bar reads top-to-bottom the way the item set does.
    QHBoxLayout *cmdBar[NF_BAR_MAX_ROWS];
    nf_bar_item  cmdItems[NF_BAR_MAX_ROWS][NF_BAR_MAX_ITEMS];
    int          nCmdItems[NF_BAR_MAX_ROWS];
    int          nOnRow[NF_BAR_MAX_ROWS];
    int          nPlacedOnRow[NF_BAR_MAX_ROWS];
    for (int r = 0; r < NF_BAR_MAX_ROWS; r++) {
        cmdBar[r]       = NULL;
        nCmdItems[r]    = 0;
        nOnRow[r]       = 0;
        nPlacedOnRow[r] = 0;
    }
    for (int r = 0; r < plan.rows; r++)
        cmdBar[r] = nf_new_bar_layout();
    // How many land on each row, known before any is placed, because
    // nf_bar_add_natural needs to know whether an item is ALONE on its row
    // (the confirmation screens' single `< cancel`, which keeps the old
    // full-width fill so the one control on a destructive question stays a
    // big target).
    //
    // BOUNDS-CHECKED against plan.rows, which nf_bar_plan_layout guarantees
    // every rowOf falls inside. Checked anyway because the cost of being wrong
    // here is a NULL cmdBar[] dereference on the owner's daily-use device, and
    // an item quietly dropped from the bar is a survivable failure where that
    // is not. Both loops must agree about which items they skip, so the same
    // test is written once, here, and the placement loop below re-reads it.
    for (int i = 0; i < nBar; i++) {
        if (plan.rowOf[i] < 0 || plan.rowOf[i] >= plan.rows) {
            nh_log("browser: bar item '%s' was planned onto row %d of %d -- dropping it rather than following a bad index",
                   nf_cmd_name(barCmd[i]), plan.rowOf[i], plan.rows);
            continue;
        }
        nOnRow[plan.rowOf[i]]++;
    }

    for (int i = 0; i < nBar; i++) {
        int r = plan.rowOf[i];
        if (r < 0 || r >= plan.rows)
            continue; // already logged above
        QString label = barLabel[i];
        if (plan.elided[i]) {
            // RULE 3 ONLY -- the fallback, not the normal case any more. It is
            // reachable when even a wrapped row overflows, i.e. when a single
            // label is wider than the whole bar. Logged per item, as before,
            // because "the bar elided" and "the last control vanished" look
            // identical on a screenshot.
            QFontMetrics fm(barItem[i]->font());
            nh_log("browser: bar item '%s' is %d px against a %d px budget on bar row %d of %d -- eliding it",
                   nf_cmd_name(barCmd[i]), barNatural[i], plan.budgetPx[i],
                   r + 1, plan.rows);
            label = fm.elidedText(label, Qt::ElideRight, plan.budgetPx[i]);
        }
        barItem[i]->setText(label);
        nf_bar_add_natural(cmdBar[r], cmdItems[r], &nCmdItems[r], barItem[i],
                           nf_cmd_name(barCmd[i]),
                           nPlacedOnRow[r] == 0, nOnRow[r] == 1);
        nPlacedOnRow[r]++;
    }

    for (int r = 0; r < plan.rows; r++)
        layout->addLayout(cmdBar[r]);

    // THE MODE DISPATCH. Everything above is shared by every mode; below,
    // exactly one of them runs. `pageItems` is declared out here rather than
    // inside the listing half because that bar's geometry is logged at the
    // very END of this function, after setContent -- which is the only point
    // where those numbers are real (see there).
    nf_bar_item pageItems[NF_BAR_MAX_ITEMS];
    int         nPageItems = 0;
    if (nf_browser_menu == NF_MENU_NONE)
        nf_build_listing_content(mwc, dialog, path, content, layout, rowWidth,
                                 chromeBarRows, pageItems, &nPageItems);
    else if (nf_menu_is_confirm(nf_browser_menu))
        nf_build_confirm_content(mwc, dialog, path, content, layout, rowWidth);
    else
        nf_build_menu_content(mwc, dialog, path, content, layout, rowWidth);

    // THE TITLE is the only thing on screen that says which mode this is: the
    // command bar looks the same in all of them, because it IS the same bar.
    // A reader who taps `sort:` sees the dialog's own title change from the
    // folder's name to "Sort by", i.e. Nickel's own chrome doing the
    // announcing rather than a row of ours pretending to be a heading.
    QString title;
    if (nf_browser_menu == NF_MENU_SORT)
        title = QStringLiteral("Sort by");
    else if (nf_browser_menu == NF_MENU_FILTER)
        title = QStringLiteral("Filter by");
    else if (nf_browser_menu == NF_MENU_VIEW)
        // "View", not "View by": the other two titles complete the sentence
        // their rows start ("Sort by" + "date"), and a view row is not the
        // object of anything -- it is a setting with its own state written on
        // it.
        title = QStringLiteral("View");
    // The three confirmations name the ACTION, with the question mark, so the
    // chrome itself says a decision is being asked for. "Delete", not "Delete
    // files" -- the count and the names are on the screen below it, and a
    // title that restated them would be the second spelling of a number.
    else if (nf_browser_menu == NF_MENU_CONFIRM_DELETE)
        title = QStringLiteral("Delete?");
    else if (nf_browser_menu == NF_MENU_CONFIRM_PASTE)
        title = nf_browser_clip_cut ? QStringLiteral("Move here?") : QStringLiteral("Copy here?");
    else if (nf_browser_menu == NF_MENU_CONFIRM_RESCAN)
        title = QStringLiteral("Rescan library?");
    else
        // SELECT MODE says so in the title, because the command bar is the
        // only other thing that does and a reader who paged down is looking
        // at rows, not at the bar. The folder's own name stays, because
        // "which folder am I in" does not stop mattering in select mode.
        title = (path == QStringLiteral(NF_ROOT))
            ? QStringLiteral("NickelFolders")
            : QFileInfo(path).fileName();
    if (nf_browser_menu == NF_MENU_NONE && nf_browser_select)
        title = QStringLiteral("Select: %1").arg(title);
    N3Dialog__setTitle(dialog, title);

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
    // pointer, which the comment above allows. The guarded destroyed() clear
    // that goes with it is in nf_track_content, which the progress screen
    // also calls -- one implementation, because the guard is the whole
    // subtlety and two copies of it is two chances to drop it.
    nf_track_content(content);

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
    //
    // ONE LINE PER COMMAND-BAR ROW, not one line for the whole bar: the
    // per-item format carries x/width/height but not y, so a wrapped bar
    // logged as a single line would show two items at the same x and read as
    // the "items stacked" failure above. The row is in the line's own name
    // instead -- which is also the second, independent confirmation that the
    // wrap the plan decided is the wrap Qt actually built.
    int laidOutWidth = content->width();
    nf_log_bar_geometry("command", cmdItems[0], nCmdItems[0], laidOutWidth);
    if (plan.rows > 1)
        nf_log_bar_geometry("command row 2", cmdItems[1], nCmdItems[1], laidOutWidth);
    // No page bar in a submenu, so no line for one -- nf_log_bar_geometry's
    // own zero-item text reads "every allocation failed", which would be a
    // false accusation here rather than a missing measurement.
    if (nf_browser_menu == NF_MENU_NONE)
        nf_log_bar_geometry("page", pageItems, nPageItems, laidOutWidth);
}

bool nf_browser_show(void) {
    // THE BUSY GUARD REACHES THE TRIGGER TOO. `touch /tmp/nfolders-native`
    // arrives on the GUI thread through the inotify notifier (nfnickel.cc),
    // i.e. through the same event loop a chunked copy is yielding to -- so a
    // trigger fired mid-copy would rebuild the content widget the progress
    // label lives on, out from under a running operation. Refused rather than
    // queued: the operation finishes in seconds to a minute and rebuilds the
    // screen itself when it does.
    if (nf_op_busy) {
        nh_log("browser: trigger ignored -- a file operation is running; it will rebuild the screen when it finishes");
        return false;
    }

    if (!nf_native_view_resolve()) {
        nh_log("browser: a required libnickel symbol did not resolve, refusing");
        return false;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("browser: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    // THE TRIGGER ALWAYS MEANS "SHOW ME THE LISTING", never whichever submenu
    // happened to be open. Reset here rather than in either branch below, so
    // both the fresh-dialog and the re-push paths get it from one line.
    //
    // This is not only tidiness: nf_browser_menu is file-scope state that
    // outlives a dialog, and the one thing that clears it in the ordinary
    // course -- BACK, or an option tap -- cannot run if Nickel's own
    // navigation abandons the dialog while a menu is up (tapping Home while
    // browsing, the same case nf_browser_active_dialog's re-push exists for).
    // Without this line the next trigger would build a sort menu at the root
    // and the reader would have no idea why. The sort key, the direction, the
    // filter and the five view flags deliberately do NOT reset with it --
    // they are preferences about how any listing is read (see their own
    // comments), where the mode is a transient answer to "what is on screen
    // right now".
    nf_browser_menu = NF_MENU_NONE;

    // SELECT MODE RESETS WITH IT, and its ticks go too, for the same reason
    // and one more: the trigger means "show me the listing", and a set of
    // ticks that survived a trigger would be a set the owner made before
    // whatever they did in between -- possibly in a folder the refresh below
    // is about to re-read from disk, where the ticked names may no longer
    // exist. The CLIPBOARD deliberately does NOT reset with them: a pending
    // cut is a job half done, and a refresh is not a request to abandon it
    // (the paste confirmation's `clear the clipboard` row is).
    if (nf_browser_select || nf_selection_count()) {
        nh_log("browser: the trigger ends select mode -- the listing is what a trigger means");
        nf_browser_select = false;
        nf_selection_clear("a trigger arrived");
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
