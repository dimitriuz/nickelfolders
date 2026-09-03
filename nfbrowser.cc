// Rung 2: Nickel's own list controller on Nickel's own window stack.
//
// This file used to build a raw AbstractController-shaped object over a
// hand-copied vtable, the way every one of Nickel's own controllers builds
// itself -- and that was the right pattern for a controller Nickel has NO
// concrete implementation of. But QuickAccessLibraryController IS a concrete
// Nickel implementation: a real QObject, with real RTTI Nickel's own
// compiler generated, whose inherited loadView() builds its own view and its
// own QWeakPointer<QWidget> -- see nfnickel.h and
// folder-stack-archaeology.md Part 2 for the full account of why this
// replaces the earlier plan rather than sitting alongside it. There is
// nothing here to fabricate: this file just constructs and pushes Nickel's
// own object.

#include "nfbrowser.h"
#include "nfnickel.h"

#include <QStringList>

#include <string.h> // memset

#include <NickelHook.h>

bool nf_browser_show_volumes(QStringList const& contentIds) {
    if (!nf_browser_resolve()) {
        nh_log("browser: a required libnickel symbol never resolved, refusing");
        return false;
    }

    // NULL only if Device::getCurrentDevice() itself failed -- see
    // nf_db_name's own comment -- in which case "" (this device's own
    // internal-storage value) is the least surprising fallback, same
    // default nf_open_book uses.
    QString const *db = nf_db_name();
    static QString const empty;

    NFSharedPtr source;
    if (!nf_build_volume_source(contentIds, db ? *db : empty, &source)) {
        nh_log("browser: could not build a data source, refusing");
        return false;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("browser: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    // sizeof(QuickAccessLibraryController) == 72 on 4.38.23684, read at the
    // `movs r0, #72` inside QuickAccessMenuView::QuickAccessMenuView's own
    // `operator new` call site (0xf499a4) -- Nickel's OWN allocation for
    // this exact class (nfnickel.h has the full derivation). Over-allocated
    // to 512 (roughly 7x), the same margin nf_open_book_staged already uses
    // for ReadBookActionProxy's own measured-but-padded allocation.
    void *controller = ::operator new(512);
    memset(controller, 0, 512);

    // QuickAccessLibraryController's ctor makes a virtual call THROUGH the
    // data source we just built (archaeology part 2, P2.2) as part of its
    // own construction, so `source` must already be a fully valid,
    // Nickel-constructed object at this point -- which it is: every object
    // nf_build_volume_source returned was constructed by Nickel's own code,
    // never by us reaching into its layout.
    QuickAccessLibraryController__ctor(controller, &source);

    nh_log("browser: pushing a list of %d ContentIDs", static_cast<int>(contentIds.size()));
    // `true`: animate the transition in, matching a user-tap-driven push --
    // not independently established what this bool controls beyond being
    // passed straight through to whatever was on top before (NOTES.md); a
    // cosmetic flag, not a correctness-affecting one, going by every other
    // use of push() in its own disassembly.
    MainWindowController__push(mwc, static_cast<AbstractController*>(controller), true);
    return true;
}
