// Our own controller and our own view on Nickel's window stack -- the first
// milestone that does NOT borrow one of Nickel's own list controllers
// (nfbrowser.cc/.h, kept and unchanged). See nfview.cc's own header comment
// for the full account of why a real, compiler-generated C++ class is used
// here, as a SANCTIONED, narrow exception to CLAUDE.md's "Nickel's classes
// stay opaque" rule -- read that comment before touching anything in
// nfview.cc.
#ifndef NFVIEW_H
#define NFVIEW_H

// Constructs the shim controller -- a deliberately trivial screen (a few
// hardcoded rows, no folder navigation, no book opening -- see nfview.cc)
// -- and pushes it via MainWindowController::push, the same entry point
// nfbrowser.cc's borrowed-controller route already uses. This milestone
// exists to prove the controller shim itself on hardware before anything
// real rides on it, so it takes no arguments and lists nothing from the
// database.
//
// Returns false, without pushing anything, if a required symbol never
// resolved (nf_view_resolve() for the shim's own two AbstractController
// symbols, nf_nickel_resolve() for MainWindowController's) or if the
// runtime layout check (nf_view_layout_check, nfview.cc) found
// AbstractController's measured 12-byte layout does not hold on THIS
// firmware build -- a loud, logged refusal rather than trusting a
// possibly-stale measurement to still be true.
bool nf_browser_show(void);

#endif
