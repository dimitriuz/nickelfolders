// Rung 2: Nickel's own list controller on Nickel's own window stack.
//
// This file used to build a raw AbstractController-shaped object over a
// hand-copied vtable, the way every one of Nickel's own controllers builds
// itself -- and that was the right pattern for a controller Nickel has NO
// concrete implementation of. But both QuickAccessLibraryController and
// ArticleListLibraryController are concrete Nickel implementations: real
// QObjects, with real RTTI Nickel's own compiler generated, whose inherited
// loadView() builds their own view and their own QWeakPointer<QWidget> --
// see nfnickel.h and folder-stack-archaeology.md Part 2 for the full
// account of why this replaces the earlier plan rather than sitting
// alongside it. There is nothing here to fabricate: this file just
// constructs and pushes Nickel's own object.
//
// A DEVICE RUN pushed QuickAccessLibraryController and it worked -- full-
// screen, real covers/titles/authors/format/size, reading progress for
// free, and a tap opened the book with no hook from this mod -- but the
// screen had no header and no back arrow: QuickAccessLibraryView is the
// home page's own quick-access WIDGET, not a page, and on a device with no
// hardware back button there was literally no way off the screen. See
// NF_BROWSER_USE_ARTICLE_LIST (nfnickel.h) for the swap this drove, and
// keep it that way -- QuickAccessLibraryController stays resolved and
// buildable only for a side-by-side comparison, not as the default.

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

    // Checked BEFORE building the data source, not after: nf_build_volume_
    // source allocates real memory (the provider, the source, two refcount
    // headers) and hands it nowhere else if this function stops before
    // constructing the controller. Rather than building that, discovering
    // sharedInstance() failed, and then having to free four separate
    // objects correctly (including reasoning about the by-value refcount
    // chain nf_build_volume_source's own comment describes, which is not
    // something to redo casually on a rarely-exercised failure path), this
    // just fails fast before anything is allocated -- sharedInstance()
    // returning null this early in Nickel's life is implausible anyway, so
    // this reordering costs nothing on the success path.
    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("browser: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    // NULL only if Device::getCurrentDevice() itself failed -- see
    // nf_db_name's own comment -- in which case a fresh, empty QString (this
    // device's own internal-storage value) is the least surprising
    // fallback, same default nf_open_book uses. A plain local, not a
    // function-local static like nf_open_book's own `empty` (nfnickel.cc):
    // a second static-local-with-nontrivial-type in this mod would pull in
    // its own __cxa_guard_acquire/__cxa_guard_release pair, which brushes
    // against CLAUDE.md's "no C++ standard library runtime" rule -- avoided
    // here since a default-constructed QString costs nothing (Qt5's own
    // shared-null-sentinel pattern, the same one nf_qvector_shared_null
    // relies on for QVector) and this call site has no hot-path reason to
    // dodge that cost the way nf_open_book's, called once per book-open,
    // arguably does.
    QString const *db = nf_db_name();
    QString const dbName = db ? *db : QString();

    NFSharedPtr source;
    int kept = 0;
    if (!nf_build_volume_source(contentIds, dbName, &source, &kept)) {
        nh_log("browser: could not build a data source, refusing");
        return false;
    }

    // sizeof(ArticleListLibraryController) == 92, sizeof(QuickAccessLibrary
    // Controller) == 72 -- both on 4.38.23684, both read at a genuine
    // Nickel `operator new` call site (nfnickel.h has both derivations, with
    // addresses). 512 comfortably over-allocates either, so the same buffer
    // size is used regardless of which branch below is compiled in --
    // nothing here depends on the exact 92-vs-72 difference.
    void *controller = ::operator new(512);
    memset(controller, 0, 512);

    // Both constructors make a virtual call THROUGH the data source we just
    // built (archaeology part 2, P2.2, confirmed independently for
    // ArticleListLibraryController's own base ctor when the hedge was
    // adopted) as part of their own construction, so `source` must already
    // be a fully valid, Nickel-constructed object at this point -- which it
    // is: every object nf_build_volume_source returned was constructed by
    // Nickel's own code, never by us reaching into its layout.
#if NF_BROWSER_USE_ARTICLE_LIST
    ArticleListLibraryController__ctor(controller, &source);
#else
    QuickAccessLibraryController__ctor(controller, &source);
#endif

    // `kept`, not contentIds.size(): the input list may include ContentIDs
    // that resolved to nothing (nf_build_volume_source's own negative-
    // control handling), so the count that matters here is how many rows
    // will actually be on screen, not how many were asked for.
    nh_log("browser: pushing a list of %d ContentIDs", kept);
    // `true`: animate the transition in, matching a user-tap-driven push --
    // not independently established what this bool controls beyond being
    // passed straight through to whatever was on top before (NOTES.md); a
    // cosmetic flag, not a correctness-affecting one, going by every other
    // use of push() in its own disassembly. `controller` is already
    // `void*` (nfnickel.h no longer names an AbstractController type at
    // all -- see its own comment on why -- these opaque call sites just
    // spell the address as `void*` now), so no cast is needed here.
    MainWindowController__push(mwc, controller, true);
    return true;
}
