// Rung 2: a screen of ours on Nickel's own window stack. Draws nothing on
// purpose -- the whole deliverable is that a screen appears, reports itself
// to ndbCurrentView (NickelDBus's oracle), and pops on back, with touch,
// e-ink refresh, fonts and the back gesture all coming free from
// MainWindowController's own machinery. NOTES.md, "Task 7, rung 2", has the
// archaeology this file's implementation rests on.
#ifndef NFBROWSER_H
#define NFBROWSER_H

// Builds an AbstractController-shaped object over Nickel's own vtable (slot
// 8 replaced) and pushes it via MainWindowController::push. Logs and returns
// without navigating anything if nf_browser_resolve() says the symbols this
// needs never resolved. Safe to call more than once -- each call pushes a
// NEW controller; what Nickel's own stack then does with the one
// underneath is Nickel's own back-navigation logic, not this mod's.
void nf_browser_show(void);

#endif
