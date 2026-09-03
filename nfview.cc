// Our own controller and our own view on Nickel's window stack.
//
// ============================================================================
// SANCTIONED EXCEPTION to CLAUDE.md's "Nickel's classes stay opaque" rule --
// read this before changing anything below.
// ============================================================================
//
// WHY a real C++ class, here, when nowhere else in this project has one:
// Every other approach tried or considered for a controller of our own
// needed something worse. The original rung-2 plan (NOTES.md, "Task 7,
// rung 2: a screen on the window stack", superseded) hand-copied
// AbstractController's live vtable and manually wrote the QWeakPointer at
// +4/+8 -- review found that plan unbuildable: it needed a real QObject
// base (so MainWindowController::push's internal
// dynamic_cast<AbstractController*, QObject*> succeeds), which means real
// RTTI, which means a real vtable inheritance chain -- none of which a
// hand-copied 11-word array can produce, because RTTI and multiple-
// inheritance thunks are not data you can copy from one object to another,
// they are a relationship the COMPILER encodes between types. Borrowing one
// of Nickel's own concrete controllers (nfbrowser.cc, the current default)
// sidesteps all of that, but at the cost this whole milestone exists to pay
// off: it renders every row from a Volume's own metadata, so folders, our
// own label-shortening, and a greyed "not in the library" row are all
// unreachable through it (README.md's STATUS, NOTES.md's Task 7 rung-2
// results).
//
// A real, compiler-generated C++ class gets the three hard things right BY
// CONSTRUCTION, the same way any ordinary C++ program does: a correct
// vtable (with real destructor slots, not slots a hand-copy would find
// zeroed -- NOTES.md, "The paragraph above was wrong, and the mistake is
// left in on purpose"), real RTTI (so the internal dynamic_cast succeeds
// for real, not by accident), and real inheritance thunks for the
// secondary QObject base. None of those three is something this project's
// usual opacity discipline (typedef + explicit call signature) can express
// at all -- they are relationships between TYPES, not values a function
// signature can carry.
//
// WHAT STAYS OPAQUE: everything else. The two raw AbstractController
// symbols this file calls (nfnickel.h/.cc: AbstractController__ctor,
// AbstractController__dtor1) are resolved and called exactly like every
// other libnickel entry point in this project -- an explicit signature,
// never a redeclared method, NULL-gated, .optional = true in the shared
// dlsym table. It is ONLY NFAbstractControllerShim/NFController -- a class
// this file itself declares, not a redeclaration of any real Nickel class
// -- that bends the rule, and only for the one property that rule cannot
// express (a type relationship, not a call signature).
//
// THE THREE MITIGATIONS this task's brief requires, because a bent rule
// still owes the purpose it was protecting:
//   1. static_assert(sizeof(NFAbstractControllerShim) == 12, ...) below --
//      compile-time proof this shim's OWN layout is what it claims to be.
//   2. nf_view_layout_check() below -- a RUNTIME proof that the REAL,
//      resolved AbstractController::AbstractController() -- not this
//      comment, not the measurement it is based on -- still writes
//      exactly the 12 bytes this shim depends on, on THIS firmware build,
//      before the shim is ever pushed onto Nickel's window stack. A
//      mismatch disarms nf_browser_show(): logs loudly, refuses to push,
//      rather than trusting a possibly-stale measurement. This is the
//      "loud failure instead of a silent miscompile" CLAUDE.md's opacity
//      rule exists to guarantee, reconstructed by other means for the one
//      case the rule itself cannot cover.
//   3. This comment.
//
// RE-MEASURE ON A FIRMWARE BUMP: sizeof(AbstractController) (NOTES.md's
// three independent readings -- the constructor's own writes, and two
// independent derived-controller offsets that agree with them), whether
// _ZN18AbstractControllerC2Ev / D1Ev still exist and still do what
// NOTES.md describes, and the vtable slot order (9 slots: 2 compiler-
// managed destructors, then size()/viewWillAppear()/viewWillDisappear()/
// viewWillBeDestroyed()/allowedOrientations()/navSection(), then the one
// pure virtual AbstractController::ensureViewLoaded (0xad1408) actually
// calls). nf_view_layout_check() catches a changed CTOR-WRITE PATTERN
// automatically, every time nf_browser_show() first runs; it cannot catch
// a changed VTABLE SLOT COUNT or ORDER, which is a device question a
// firmware bump would raise (see this task's report for what a device run
// alone can settle).

#include "nfview.h"
#include "nfnickel.h"

#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QSharedPointer> // QtSharedPointer::ExternalRefCountData -- see the class comment below
#include <QVBoxLayout>
#include <QWidget>

#include <string.h> // memset

#include <NickelHook.h>

// --- the shim class ---------------------------------------------------
//
// Layout, top to bottom, mirrors AbstractController's own measured 12
// bytes EXACTLY (NOTES.md, "sizeof(AbstractController) == 12 bytes --
// three independent readings", 0xad1334):
//   +0  vptr                    (compiler-generated: this class has virtuals)
//   +4  ExternalRefCountData *d \_ the SAME two words, SAME order,
//   +8  QWidget *value          /  AbstractController::viewLoaded() (0xad13ac)
//                                  reads (this->d, then this->d->strongref,
//                                  then this->value)
//
// NOT declared as a real QWeakPointer<QWidget> member, even though that
// would seem like the obvious way to get the field order right for free:
// tried first, and it does not compile against THIS ARM Qt 5.2.1 sysroot.
// qsharedpointer_impl.h's own QWeakPointer<T> has exactly ONE public way to
// build one from a raw T* (`QWeakPointer(X *ptr)` / `operator=(X *ptr)`),
// and BOTH are guarded by `#if QT_DEPRECATED_SINCE(5, 0)` -- which, in this
// build, is FALSE (QT_DISABLE_DEPRECATED_BEFORE defaults to 5.0.0, and
// something deprecated exactly AT 5.0 is disabled by that default), so
// neither is visible; the one non-deprecated constructor that also builds
// this pair from a raw pointer (`QWeakPointer(X *ptr, bool)`) is PRIVATE,
// friended only to QSharedPointer/QPointer. Confirmed directly against a
// real ./nickeltc build failure, not assumed from reading the header.
//
// So this shim builds the SAME two words the SAME way
// NotebookGridController::loadView() does (folder-stack-archaeology.md,
// P1.5(b)): call QtSharedPointer::ExternalRefCountData::getAndRef
// EXPLICITLY. That function is `Q_CORE_EXPORT` and PUBLIC (unlike the
// constructors above), so this is an ORDINARY Qt5Core call this file
// already links against (Makefile: PKGCONF += Qt5Widgets, which pulls in
// Qt5Core) -- not a resolved-by-name libnickel symbol, and not the kind of
// guess CLAUDE.md's opacity rule exists to guard against: it is the exact
// function whose disassembly, inside Nickel's OWN loadView(), is what
// established this field order and read-order in the first place.
class NFAbstractControllerShim {
public:
    NFAbstractControllerShim() : nf_weak_d(0), nf_weak_widget(0) {}
    virtual ~NFAbstractControllerShim() {}

    // Slots 2-7 of AbstractController's own vtable (NOTES.md: "+8 size(),
    // +12 viewWillAppear(), +16 viewWillDisappear(), +20
    // viewWillBeDestroyed(), +24 allowedOrientations() const, +28
    // navSection() const"). NOTHING traced in this project calls any of
    // these -- only AbstractController::ensureViewLoaded (0xad1408)
    // calling slot 8 (nf_load_view, below) is measured -- so their exact
    // argument/return contracts are UNKNOWN. Given harmless, inert
    // defaults rather than guessed-at real behaviour: virtual dispatch
    // through OUR OWN vtable lands correctly in OUR OWN code regardless of
    // what we return here -- the SHAPE (9 slots, this order) is what
    // Nickel's machine code depends on, not any particular value -- but an
    // untraced Nickel code path (window-stack lifecycle, orientation lock,
    // back-navigation bookkeeping) calling one of these with a contract
    // this rung never measured is a real, open possibility. Flagged in
    // this task's own report as a "what only a device run can settle"
    // item, not silently assumed safe.
    virtual int  size() { return 0; }
    virtual void viewWillAppear() {}
    virtual void viewWillDisappear() {}
    virtual void viewWillBeDestroyed() {}
    virtual int  allowedOrientations() const { return 0; }
    virtual int  navSection() const { return 0; }

    // Slot 8, offset +32 -- the ONLY pure virtual in AbstractController's
    // real vtable (__cxa_pure_virtual there, NOTES.md's vtable-header
    // reading). This one IS measured: AbstractController::ensureViewLoaded
    // (0xad1408) calls exactly this slot, with `this` in r0, discards the
    // return value, and expects the nf_weak_d/nf_weak_widget pair above to
    // hold a real widget afterward (viewLoaded()'s own read order, same
    // source). NFController (below) supplies the only concrete override;
    // this base is never instantiated on its own.
    virtual void nf_load_view() = 0;

protected:
    QtSharedPointer::ExternalRefCountData *nf_weak_d;
    QWidget                               *nf_weak_widget;
};

// Mitigation (1): build-time proof this shim's OWN layout matches the
// measured AbstractController layout. Catches a hand-edit that adds a
// field to NFAbstractControllerShim, or a QWeakPointer<T> that somehow
// stops being 8 bytes on some future Qt, at COMPILE time, on every build.
// Does NOT catch a firmware that changes AbstractController's OWN, real
// size -- that is what nf_view_layout_check() (below) is for; the two
// checks are deliberately complementary, not redundant.
static_assert(sizeof(NFAbstractControllerShim) == 12,
              "NFAbstractControllerShim must match AbstractController's "
              "measured 12-byte layout (NOTES.md, Task 7 rung 2, 0xad1334) "
              "-- re-measure before changing this");

// Nickel's own layout convention for a controller of this shape
// (NOTES.md / this task's brief: "AbstractController at offset 0, QObject
// at +12, with a secondary vtable group carrying _ZThn12_ QObject
// thunks") -- mirrored here as ordinary C++ multiple inheritance, so the
// COMPILER builds the equivalent thunks itself; nothing here reaches for
// offset arithmetic to find QObject's subobject. AbstractControllerShim is
// listed FIRST so it is the primary base: a pointer to an NFController IS,
// bit for bit, a valid pointer to its AbstractControllerShim subobject --
// the same "no cast arithmetic needed" property nfnickel.h already
// documents for QuickAccessLibraryController's real base.
//
// No Q_OBJECT and no moc, matching this project's existing house style
// (nfnickel.cc's own comment on why the inotify notifier's connect doesn't
// use one either): this class needs a real QObject base so
// MainWindowController::push's internal dynamic_cast<QObject*> succeeds --
// that is ordinary C++ RTTI, generated for any polymorphic type, and needs
// nothing from QObject's metaobject/signal-slot machinery, which nothing
// here uses.
class NFController : public NFAbstractControllerShim, public QObject {
public:
    NFController() : NFAbstractControllerShim(), QObject(0) {}

    ~NFController() {
        // Disassembled for this task (0xad1358) -- and this is NOT a
        // no-op, unlike the ctor: D1Ev reads this[+4] (our nf_weak_d),
        // atomically decrements its FIRST word (an ldrex/strex CAS loop --
        // the weakref QBasicAtomicInt), and if that reaches zero, calls
        // `operator delete` on it (`blx 672404`, resolved with
        // tools/plt.sh to `_ZdlPv`). That is EXACTLY
        // QWeakPointer<T>::~QWeakPointer()'s own documented logic
        // (qsharedpointer_impl.h, this ARM Qt 5.2.1 sysroot: "if (d &&
        // !d->weakref.deref()) delete d"), against the SAME field
        // nf_load_view() populated via getAndRef.
        //
        // So D1Ev is not "defensive, in case it does something" -- it IS
        // the correct teardown for this[+4]/this[+8], measured, and
        // calling it is the ONLY place this shim tears that pair down:
        // doing so a second time by hand (an earlier draft of this
        // function did exactly that) would double-decrement -- and
        // potentially double-`delete` -- the same weakref D1Ev just
        // freed. D1Ev, never D0Ev (the deleting destructor, which would
        // additionally call `operator delete` on `this` itself, an
        // address this shim did not allocate on its own -- see
        // nfnickel.h's own comment on AbstractController__dtor1).
        if (AbstractController__dtor1) {
            AbstractController__dtor1(static_cast<void*>(this));
        } else if (nf_weak_d) {
            // Fallback ONLY if the symbol above did not resolve on this
            // firmware: reproduce D1Ev's own measured logic by hand
            // (Qt's own QWeakPointer<T>::~QWeakPointer(), quoted above)
            // so a firmware that drops just this one symbol still tears
            // the weak pointer down correctly instead of leaking the
            // ExternalRefCountData block outright.
            if (!nf_weak_d->weakref.deref())
                delete nf_weak_d;
        }
    }

    void nf_load_view() /* overrides the pure virtual above */ {
        // A REAL QWidget from the REAL Qt5Widgets this file links against
        // (Makefile: PKGCONF += Qt5Widgets) -- no libnickel call needed to
        // build the view itself, only to hand it to Nickel's window stack
        // (nf_browser_show, below). Deliberately trivial per this
        // milestone's brief: three static rows and a labelled affordance,
        // nothing that lists a folder or opens a book -- this milestone's
        // whole job is proving the CONTROLLER shim on hardware before
        // anything real (nffmt.h/nflist.h's tested listing logic) rides on
        // it.
        QWidget *w = new QWidget();
        QVBoxLayout *layout = new QVBoxLayout(w);
        layout->addWidget(new QLabel(QStringLiteral("NickelFolders (native view)")));
        layout->addWidget(new QLabel(QStringLiteral("Row 1")));
        layout->addWidget(new QLabel(QStringLiteral("Row 2")));
        layout->addWidget(new QLabel(QStringLiteral("Row 3")));

        // "Whatever back affordance you can give it" (this task's brief).
        // Deliberately NOT wired to any navigation call: no
        // MainWindowController::pop (or equivalent) has been resolved, or
        // even located by name -- guessing one here would be exactly the
        // unstaged, unverified libnickel call CLAUDE.md's "Method: adding
        // a new libnickel call" exists to prevent, and it is explicitly
        // out of THIS milestone's scope ("No listing logic, no folder
        // navigation, no book opening"). This button exists so the owner
        // has something concrete to tap while finding out, on device,
        // what Nickel actually does with a controller pushed THIS way --
        // NOTES.md's own open item #1 ("Where back goes is still open")
        // was measured only for ReadBookActionProxy::onSelected() and for
        // a BORROWED library controller (nfbrowser.cc); nothing has
        // measured it for a controller built like this one.
        QPushButton *back = new QPushButton(QStringLiteral("Back (tap to test)"));
        QObject::connect(back, &QPushButton::clicked, [] {
            nh_log("view: back tapped -- no pop call wired, this is a device test point");
        });
        layout->addWidget(back);

        // The one write this whole shim exists to get right: the SAME
        // ExternalRefCountData::getAndRef call Nickel's own
        // NotebookGridController::loadView() makes (folder-stack-
        // archaeology.md, P1.5(b)), landing at this+4/this+8 -- exactly
        // where AbstractController::viewLoaded() (0xad13ac) reads it. See
        // the class comment above for why this is a direct Qt5Core call,
        // not a QWeakPointer<QWidget> assignment.
        nf_weak_d      = QtSharedPointer::ExternalRefCountData::getAndRef(w);
        nf_weak_widget = w;
    }
};

// --- mitigation (2): the runtime layout check -----------------------------
//
// Proves, at runtime, against THIS firmware's actual
// AbstractController::AbstractController() -- not the measurement above --
// that it still writes exactly the 12 bytes this shim's layout depends on,
// before the shim is ever pushed onto Nickel's real window stack. Run
// LAZILY (on nf_browser_show()'s first call, not at nf_init()), matching
// nf_nickel_resolve()/nf_browser_resolve()'s own pattern of being cheap,
// side-effect-free gates nf_init only LOGS the status of -- the deeper
// write-pattern exercise below is reserved for the one feature it guards.
static bool nf_view_layout_checked = false;
static bool nf_view_layout_ok      = false;

static bool nf_view_layout_check() {
    if (nf_view_layout_checked)
        return nf_view_layout_ok;
    nf_view_layout_checked = true;

    if (!nf_view_resolve()) {
        nh_log("view: AbstractController ctor/dtor symbols did not resolve, refusing the layout check");
        return false;
    }

    // 64 bytes: the SAME over-allocation margin CLAUDE.md asks for every
    // Nickel constructor call (nf_open_book_staged's volbuf, this file's
    // own controller allocation below) -- the real ctor is measured to
    // write only 12 bytes, but this being throwaway scratch memory rather
    // than a live, pushed object is exactly why the margin costs nothing
    // here either.
    unsigned char scratch[64] __attribute__((aligned(8)));
    // Poison, not zero: the ctor's own measured writes are this[+4]=0 and
    // this[+8]=0 (NOTES.md), which is INDISTINGUISHABLE from "never wrote
    // there" if the buffer already reads zero. 0xCD makes every byte the
    // ctor does NOT touch stay conspicuously non-zero, so a mismatch (the
    // ctor writing past byte 12, or never writing +4/+8 at all) is visible
    // rather than looking like a pass by accident.
    memset(scratch, 0xCD, sizeof scratch);

    AbstractController__ctor(scratch);

    unsigned int *words = reinterpret_cast<unsigned int*>(scratch);
    bool vptrWritten     = words[0] != 0xCDCDCDCDu; // this[+0]: some non-poison vptr
    bool plus4Zero       = words[1] == 0u;          // this[+4]: measured zero
    bool plus8Zero       = words[2] == 0u;          // this[+8]: measured zero
    bool plus12Untouched = words[3] == 0xCDCDCDCDu; // this[+12]: must NOT be written --
                                                     // if it is, AbstractController is
                                                     // bigger than 12 bytes on this
                                                     // firmware and this shim's QObject
                                                     // base would land on top of live data.

    // Best-effort: also exercises the resolved destructor on the same
    // scratch memory the ctor just initialised, so a firmware where D1Ev
    // itself is broken is caught here too, rather than the first time a
    // real, pushed controller is destroyed.
    if (AbstractController__dtor1)
        AbstractController__dtor1(scratch);

    if (!vptrWritten || !plus4Zero || !plus8Zero || !plus12Untouched) {
        nh_log("view: AbstractController's real ctor wrote scratch[0..15] = "
               "%08x %08x %08x %08x, not the measured vptr/0/0/untouched "
               "pattern -- the 12-byte layout assumption is STALE on this "
               "firmware, refusing to build the shim controller",
               words[0], words[1], words[2], words[3]);
        return false;
    }

    nh_log("view: AbstractController layout check passed (12 bytes: vptr, 0, 0, untouched+12)");
    nf_view_layout_ok = true;
    return true;
}

// --- construction ----------------------------------------------------------

bool nf_browser_show(void) {
    if (!MainWindowController__sharedInstance || !MainWindowController__push) {
        nh_log("view: MainWindowController::sharedInstance/push did not resolve, refusing");
        return false;
    }
    if (!nf_view_layout_check()) {
        nh_log("view: refusing to build the shim controller (see the layout-check line above)");
        return false;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("view: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    // sizeof(NFController) is whatever THIS build's own compiler produces
    // -- unlike nfbrowser.cc's controller allocation, this is not a Nickel
    // constructor whose size this rung has to read off a disassembly, so
    // no measurement-derived margin is needed for correctness. 512 bytes
    // is used anyway, matching nfbrowser.cc's own allocation size, so a
    // future field added to NFController does not silently need a second,
    // separate change here.
    void *mem = ::operator new(512);
    memset(mem, 0, 512);

    // The pattern this task's brief describes: call the REAL, resolved
    // AbstractController ctor on the raw memory FIRST, so Nickel's own
    // initialisation genuinely runs (measured -- and, moments ago,
    // RE-verified on this boot by nf_view_layout_check() above -- to write
    // exactly this[+0]=Nickel's OWN transient vptr, this[+4]=0, this[+8]=0)
    // -- THEN placement-new the real C++ object on top, which immediately
    // overwrites this[+0] with OUR OWN vtable pointer as an ordinary side
    // effect of construction (the compiler's own constructor prologue sets
    // the vptr for the class currently under construction, before running
    // any user-written body). This is exactly "the two vtable writes"
    // NOTES.md documents as standard for every one of Nickel's own
    // controllers -- base ctor sets the base vtable, derived ctor resets
    // it -- the mechanism is identical; only who performs the second write
    // differs (the C++ compiler here, Nickel's own derived-controller code
    // there).
    AbstractController__ctor(mem);
    NFController *ctrl = new (mem) NFController();

    nh_log("view: pushing the shim controller");
    // ctrl's AbstractControllerShim base is its FIRST base (no virtual
    // inheritance anywhere in this hierarchy), so this address equals
    // `ctrl` itself -- no cast arithmetic, the same "no cast arithmetic
    // needed" property nfnickel.h already documents for
    // QuickAccessLibraryController's real base.
    MainWindowController__push(mwc, static_cast<AbstractController*>(static_cast<void*>(ctrl)), true);
    return true;
}
