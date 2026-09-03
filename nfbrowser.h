// Rung 2: Nickel's own list controller on Nickel's own window stack, showing
// books we chose. REPLACES the original rung 2 plan (a hand-built
// AbstractController subclass over a copied vtable): that plan was found
// unbuildable in review (it needed a real QObject base, a real
// QWeakPointer<QWidget>, destructor slots Nickel's own vtable leaves zero,
// and fabricated RTTI for an internal dynamic_cast<QObject*>). Archaeology
// (.superpowers/sdd/2026-09-03-nickelfolders-v1/folder-stack-archaeology.md,
// Part 2) found a concrete Nickel controller -- QuickAccessLibraryController
// -- that supplies every one of those itself, so none of it needs faking.
#ifndef NFBROWSER_H
#define NFBROWSER_H

#include <QStringList>

// Looks up each ContentID in contentIds (nf_build_volume_source, nfnickel.cc),
// constructs a QuickAccessLibraryController over the resulting data source,
// and pushes it via MainWindowController::push -- Nickel's own row widgets,
// own cover art, and own Read action (ReadBookActionProxy::onSelected,
// already hardware-proven in rung 1) do the rest; this mod hooks nothing.
// Logs and returns false, without navigating anything, if a required symbol
// never resolved or the data source could not be built. Safe to call more
// than once -- each call pushes a NEW controller; what Nickel's own stack
// then does with the one underneath is Nickel's own back-navigation logic,
// not this mod's.
bool nf_browser_show_volumes(QStringList const& contentIds);

#endif
