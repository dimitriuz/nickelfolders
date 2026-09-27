// Our own screen on Nickel's window stack, built entirely out of Nickel's
// own dialog chrome and Nickel's own tappable row widget -- N3DialogFactory,
// N3Dialog and TouchLabel, all real Nickel classes, all reached the way
// every other libnickel entry point in this project is (nfnickel.h: opaque
// typedef + explicitly written call signature). This REPLACES the earlier
// AbstractController shim (a compiler-generated class deliberately named to
// match Nickel's own, sanctioned in review as a narrow, one-time exception
// to CLAUDE.md's "Nickel's classes stay opaque" rule): that route rendered
// correctly on hardware but its QPushButton never received a tap, because
// Nickel does not deliver QMouseEvents at all -- it injects QTouchEvents
// and recognises its own gestures through machinery only Nickel's own
// widgets (TouchLabel among them) are wired into. See NOTES.md, "Task 8:
// touch input archaeology", for the full derivation and
// N3DialogFactory::getDialog / MainWindowController::pushView's own
// disassembly.
//
// With this route there is no controller to fabricate, no cross-cast to
// satisfy, and no compiler-generated class sharing a Nickel name at all --
// the sanctioned exception is retired along with the code it excused; see
// nfnickel.h for what stayed opaque instead.
#ifndef NFVIEW_H
#define NFVIEW_H

// The payoff milestone: this is what makes nflist.h/nffmt.h's pure, fully
// host-tested listing pipeline (nf_build_listing) render on hardware for the
// first time. Builds ONE N3Dialog, rooted at /mnt/onboard, and rebuilds its
// content IN PLACE (N3Dialog::setContent -- nfnickel.h) every time a folder
// row is tapped, rather than pushing a second dialog per level: MainWindow
// Controller::popView's own semantics (NOTES.md) make a second push's back
// destination unpredictable, and it would also leak. So this mod owns the
// current-path state itself (nf_browser_cwd, nfview.cc, file-scope POD --
// see its own comment for why POD is load-bearing here) and BACK is this
// mod's own logic, not Nickel's: it steps up one directory, and only pops
// the dialog off the stack once the root is reached. The screen offers TWO
// independent exits at every level, deliberately not just one -- a
// guaranteed "< BACK" control, first in the command bar, wired straight to
// that logic, and N3Dialog's own
// backTapped() signal wired to the exact same function -- review finding
// I-3, because getDialog's own X button is wired to a controller-stack call
// this route's pushView never populates, so it does nothing here (see
// nfview.cc/nfnickel.h).
//
// The screen has FOUR MODES, not four screens: the item listing, and the
// three submenus that tapping `sort:`/`filter:`/`view:` in the command bar
// opens over it (sort and filter used to CYCLE to the next value, which took
// up to eight taps to reach a specific one; `view:` never had a cycle,
// because five INDEPENDENT toggles do not form a sequence). A submenu is the
// same dialog with different content swapped in -- no second dialog, no
// second push, nothing on the window stack -- so the reader's directory and
// page survive it untouched, and BACK out of one returns to exactly the
// screen it replaced. BACK disambiguates in ONE function (nf_browser_back):
// a submenu is a case inside it, not a second path, because the whole reason
// the screen's two independent exits share that function is that there is
// then one place to read "where am I" from.
//
// THE VIEW MENU'S five toggles (nf_view_flags, nffmt.h) are DISPLAY concerns
// with one deliberate exception -- `hidden files` is a genuine filter,
// because an entry the listing dropped cannot be shown differently. Each row
// states its own state in its own TEXT ("covers: on"), never by styling:
// this panel has four grey levels and "slightly lighter" reads as "the
// same", the same finding that puts "[not in library]" into a row's words.
// `covers` also changes the PAGE SIZE -- 11 rows with covers, 15 without,
// computed per MODE by nf_items_per_page and never per page. Every one of
// those decisions is pure and host-tested; what stays here is only the
// rendering. Every content build logs the whole flag set, so a screenshot
// can never be misattributed to the wrong mode.
//
// Directory listing is QDir::entryInfoList against ONE directory, never
// recursive, never held past the single Qt signal handler that runs it
// (CLAUDE.md's /mnt/onboard file-handle constraint). Per-file metadata
// (nf_row::hasRow, ::percentRead, ::readState -- with ::finished derived
// from the last of those by nf_build_listing, nflist.cc) comes from VolumeManager::
// getById + Volume::isValid + Content::getReadStatus()/isFinished() + a
// guarded offset off Volume::d() (nf_volume_exists, nfnickel.h) -- NOT
// Volume::getDbValues, whose calling convention IS now fully resolved
// (NOTES.md, "reading progress on folder rows") but which was rejected on
// its own terms even so: a displaced sret+this shape this project has
// crashed Nickel on once already, a ReadStatus value that silently reads
// back as 0 through the only exported unwrap path, and an operator[] that
// inserts on a missing key rather than failing. A file with no library row
// is shown, not hidden, with its reason in the label text itself (not
// colour alone -- this panel gives four grey levels, and "slightly
// lighter" does not read as "different"), and a tap on it logs why and
// does nothing. A file WITH a row gets a percentage, a finished marker, or
// nothing for unread -- see nfview.cc's own row-label comment.
//
// Tracks its one live dialog in nf_browser_active_dialog (nfview.cc,
// file-scope POD `void*` -- a second trigger while a screen was already up
// used to leak a dialog and leave BACK landing on a stale duplicate). If a
// dialog already exists, this RE-PUSHES that same dialog rather than
// building a second one or refusing outright -- refusing was the original
// fix, but the guard clearing only on the dialog's own destroyed() signal
// left a dead end if Nickel's own navigation ever left the dialog alive
// but off-screen (tapping Home while browsing, e.g.) without destroying
// it: every later trigger would have done nothing until a reboot. Re-
// pushing recovers instead, and costs nothing extra if the dialog is
// already the current view (MainWindowController::pushView's own
// early-return, nfnickel.h). The guard itself still clears off the
// dialog's own destroyed() signal for the case that IS destruction (BACK
// at the root, e.g.), so a later call builds a fresh dialog rather than
// re-pushing a dead pointer.
//
// Returns false, without pushing anything, if a required symbol never
// resolved (every symbol this needs is looked up through the usual
// nh_dlsym table, nfnickel.cc, and is .optional -- see
// nf_native_view_resolve in nfnickel.h), or if MainWindowController::
// sharedInstance() or N3DialogFactory::getDialog() themselves returned
// null. Returns true on either a fresh push or a re-push.
bool nf_browser_show(void);

// True while a file operation (a delete, a move or a chunked copy) is running
// on the GUI thread.
//
// It exists because a chunked copy YIELDS to the event loop between chunks --
// it has to, so the panel stays alive and `cancel` stays tappable -- and that
// yield will deliver anything queued, including this mod's OWN inotify
// triggers (/tmp/nfolders-open, /tmp/nfolders-show; nfolders.cc). Those two
// push Nickel UI of their own, on top of a running operation, from inside the
// operation's own call stack.
//
// nf_browser_show guards itself, so this is exported only for the two trigger
// handlers that have no other way to ask. Every tap handler inside nfview.cc
// checks the same flag directly.
bool nf_ops_busy(void);

#endif
