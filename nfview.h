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
// guaranteed "<< BACK" row wired straight to that logic, and N3Dialog's own
// backTapped() signal wired to the exact same function -- review finding
// I-3, because getDialog's own X button is wired to a controller-stack call
// this route's pushView never populates, so it does nothing here (see
// nfview.cc/nfnickel.h).
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

#endif
