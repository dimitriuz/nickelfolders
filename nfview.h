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

// Builds a deliberately trivial screen -- three static TouchLabel rows and
// nothing else, no listing logic, no folder navigation, no book opening --
// wrapped in an N3Dialog with a title and a working back button, and pushes
// it via MainWindowController::pushView. This milestone exists to prove taps
// and back navigation on hardware, on this route, before anything real rides
// on it -- the same one-thing-at-a-time staging CLAUDE.md's "Method: adding
// a new libnickel call" already asks for.
//
// Returns false, without pushing anything, if a required symbol never
// resolved (every symbol this needs is looked up through the usual
// nh_dlsym table, nfnickel.cc, and is .optional -- see nf_native_view_resolve
// in nfnickel.h) or if MainWindowController::sharedInstance() or
// N3DialogFactory::getDialog() themselves returned null.
bool nf_browser_show(void);

#endif
