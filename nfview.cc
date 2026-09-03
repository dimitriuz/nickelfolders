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
// WHAT STAYS OPAQUE: everything else. The nine raw AbstractController
// symbols this file calls (nfnickel.h/.cc: AbstractController__ctor,
// __dtor1, __size, __viewWillAppear, __viewWillDisappear,
// __viewWillBeDestroyed, __allowedOrientations, __navSection, __vtable)
// are resolved and called/read exactly like every other libnickel entry
// point in this project -- an explicit signature, never a redeclared
// method, NULL-gated, .optional = true in the shared dlsym table. It is
// ONLY AbstractController/NFController -- classes this file itself
// declares, not a redeclaration of any real Nickel class's INTERNALS --
// that bend the rule, and only for the one property that rule cannot
// express (a type relationship, not a call signature).
//
// CRITICAL: THE SHIM'S PRIMARY BASE MUST BE NAMED EXACTLY
// `AbstractController`, AT GLOBAL (NOT ANONYMOUS-NAMESPACE) SCOPE -- found
// in review, and the reason is worth carrying in full because it is easy
// to "simplify" back to a private, differently-named class without
// noticing anything broke. MainWindowController::push does not merely
// null-check its cast to QObject* -- it is a genuine Itanium ABI
// CROSS-CAST: `__dynamic_cast(controller, &_ZTI18AbstractController,
// &_ZTI7QObject, -2)` (0xeaacc4-0xeaacde). The src2dst hint of -2 means
// "no downcast fallback" -- the algorithm MUST find a base in the
// object's own RTTI hierarchy whose TYPE NAME matches `src_type`
// (`_ZTI18AbstractController`, Nickel's OWN, already-compiled type_info
// object) via `type_info::operator==`, which falls back to a byte-for-byte
// `strcmp` of the mangled NAME STRING when pointer identity fails (as it
// always will here -- our RTTI and Nickel's live in two different shared
// libraries, at two different addresses, no matter what the class is
// named). A class named anything else -- NFAbstractControllerShim, in an
// earlier draft -- COMPILES, LINKS, and PUSHES onto the window stack
// (slot 8/`+32` still gets called, the widget still gets built), but the
// cross-cast returns NULL: `push` appends `{d=0, value=0}` to its window
// stack, so `topController()` returns NULL and back has nothing to pop --
// and the SAME failed cast, at a second call site (0xea9174), skips
// `QObject::setParent(controller, view)`, which is how Nickel actually
// takes ownership of a pushed controller (see nf_browser_show()'s own
// ownership comment for what that means for this file). The screen would
// have appeared, unowned, with a dead back arrow -- a failure mode
// invisible in a screenshot and easy to blame on something else entirely.
// The class below is named `AbstractController`, matching Nickel's own
// name byte-for-byte, at plain global scope (an anonymous namespace would
// make GCC prefix the mangled name with `*`, per the Itanium ABI's own
// convention for internal-linkage types, which forces POINTER comparison
// -- exactly the comparison guaranteed to fail across two separately
// compiled shared libraries -- so anonymous-namespace wrapping would
// silently reintroduce this exact bug). `-fvisibility=hidden` (this
// project's own default, Makefile) keeps this class's own vtable/RTTI/
// constructor/destructor SYMBOLS out of this .so's dynamic symbol table,
// so there is no risk of colliding with Nickel's OWN, separately exported
// `_ZN18AbstractControllerC1Ev` and friends at the DYNAMIC LINKER level --
// hidden visibility only affects whether another shared library could
// look this symbol UP by name; it has no bearing on the NAME STRING
// baked into our own RTTI data, which is the only thing the cross-cast
// above ever reads.
//
// WHY SIX OF THOSE NINE ARE FORWARDED, NOT STUBBED -- read this before
// "simplifying" size()/viewWillAppear()/viewWillDisappear()/
// viewWillBeDestroyed()/allowedOrientations()/navSection() back into
// placeholders. The ENTIRE reason the earlier, abandoned plan copied
// Nickel's live vtable rather than hand-building one was that
// AbstractController's own base implementations of those six are real
// behaviour Nickel's window-stack machinery may depend on -- that plan
// got them for free, by copying. A COMPILER-GENERATED shim does not: the
// compiler emits OUR OWN vtable, so every slot in it is now ours to fill
// in, and a slot this class does not forward is not a harmless
// placeholder -- it is a real base-class behaviour silently DROPPED. The
// clearest instance is size(): if Nickel ever asks this controller how
// big it is and gets a stub's default answer instead of the real
// implementation's, the view can lay out to nothing, which looks
// identical on a screenshot to "the screen never appeared" -- exactly the
// kind of wrong-place hunting this project's verification culture exists
// to prevent. So each of the six is resolved and forwarded to the real,
// disassembled base implementation (nfnickel.h has the full derivation
// and address for each), with a fallback constant used ONLY if its symbol
// failed to resolve -- and even then, nf_view_resolve() (nfnickel.cc)
// refuses to push the controller at all in that case, so the fallback is
// belt-and-braces, not the expected path.
//
// THE MITIGATIONS this task's brief requires, because a bent rule still
// owes the purpose it was protecting -- REVISED IN REVIEW, because the
// original three overstated what the compile-time check alone proves:
//   1. static_assert(sizeof(AbstractController) == 12, ...) below --
//      compile-time proof this shim's OWN layout is what it claims to be.
//      WEAK, and known to be weak: sizeof(vptr + two pointers) is 12 on
//      this ABI unconditionally, so this can only ever catch a LOCAL
//      HAND-EDIT (an extra field added to this class) -- it CANNOT catch
//      a firmware where Nickel's real AbstractController changed size,
//      because it never reads anything from libnickel.so at all. Kept
//      because it is free and does catch its one real case, but it is
//      NOT the layout mitigation -- (2) is.
//   2. nf_view_layout_check() below -- THE real, load-bearing runtime
//      mitigation, doing TWO independent things against THIS firmware's
//      actual, live libnickel.so, neither trusted from a comment: (a)
//      the real constructor's write pattern, compared EXACTLY against the
//      real vtable's own address point (not merely "changed from a
//      poison byte" -- an earlier draft's version of this check was
//      exactly that vacuous, caught in review); and (b) the real vtable's
//      own SLOT CONTENTS at [0] and [2..7], compared against the six
//      forwarded symbols resolved independently by NAME -- the check that
//      actually validates the SLOT ORDER this shim's hand-written virtual
//      function order assumes, which (a) and the six individual dlsym
//      resolutions cannot: a firmware that inserted or removed one
//      virtual before size() would still resolve every symbol by name
//      (their addresses would not change), while silently shifting every
//      slot this shim depends on. A mismatch in either (a) or (b) disarms
//      nf_browser_show(): logs loudly, refuses to push, rather than
//      trusting a possibly-stale measurement. See nf_view_layout_check()'s
//      own comment for the full account of both.
//   3. This comment.
//
// RE-MEASURE ON A FIRMWARE BUMP: sizeof(AbstractController) (NOTES.md's
// three independent readings -- the constructor's own writes, and two
// independent derived-controller offsets that agree with them), whether
// _ZN18AbstractControllerC2Ev / D1Ev / the six forwarded symbols / the
// vtable data symbol itself (nfnickel.h has every address) still exist
// and still do what this file and NOTES.md describe, and whether
// `_ZTS18AbstractController`/`_ZTI18AbstractController` still exist and
// still describe a class with QObject reachable as a PUBLIC base (the
// cross-cast this whole file exists to satisfy). nf_view_layout_check()
// catches a changed CTOR-WRITE PATTERN and a changed VTABLE SLOT
// ORDER/CONTENT for the six forwards automatically, every time
// nf_browser_show() first runs; it cannot catch a changed CALLING
// CONVENTION for one of the six (e.g. size() no longer needing a hidden
// return buffer) or a changed RTTI/inheritance shape for the cross-cast
// itself, which are device/re-disassembly questions a firmware bump would
// raise (see this task's report for what a device run alone can settle).

#include "nfview.h"
#include "nfnickel.h"

#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QSharedPointer> // QtSharedPointer::ExternalRefCountData -- see the class comment below
#include <QSize>          // AbstractController::size()'s real return type -- see nfnickel.h
#include <QVBoxLayout>
#include <QWidget>

#include <string.h> // memset

#include <NickelHook.h>

// --- the shim class ---------------------------------------------------
//
// Named `AbstractController`, at global scope, ON PURPOSE -- not a style
// choice. See this file's own "CRITICAL" header comment above for the
// full derivation: MainWindowController::push performs a genuine Itanium
// ABI cross-cast whose src_type lookup is a mangled-NAME comparison
// (`strcmp`-based `type_info::operator==`), so this class's RTTI must
// carry the exact string "AbstractController" for that cast to succeed.
// This is NOT a redeclaration of Nickel's real AbstractController class
// (its actual members, beyond the 12 bytes measured here, are unknown and
// irrelevant) -- it is a class this file defines from scratch that
// happens to share Nickel's own class's NAME, because the name itself,
// not any internal layout beyond what NOTES.md measured, is what the
// cross-cast reads.
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
class AbstractController {
public:
    AbstractController() : nf_weak_d(0), nf_weak_widget(0) {}
    virtual ~AbstractController() {}

    // Slots 2-7 of AbstractController's own vtable (NOTES.md: "+8 size(),
    // +12 viewWillAppear(), +16 viewWillDisappear(), +20
    // viewWillBeDestroyed(), +24 allowedOrientations() const, +28
    // navSection() const"). FORWARDED to the real, resolved
    // AbstractController implementations, never left as inert stubs --
    // see nfnickel.h's own comment on each of the six symbols below for
    // the full disassembly each forward is based on, and the "why
    // forwarding exists" paragraph in this file's own header comment for
    // why an inert stub here is not a harmless placeholder: this shim
    // replaced Nickel's OWN vtable wholesale, so every slot NOT forwarded
    // is real base-class behaviour silently dropped, not a value nobody
    // reads. Each fallback below (used only if the matching symbol did
    // not resolve) reproduces the SAME constant the measured base
    // implementation returns on 4.38.23684, so a missing symbol degrades
    // to an IDENTICAL observable value rather than a different one --
    // this is defence in depth: nf_view_resolve() (nfnickel.cc) already
    // refuses to push the controller at all unless every one of these
    // nine symbols resolved (the ctor, dtor1, these six, and __vtable),
    // so the fallback path is not expected to run in practice.
    virtual QSize size() {
        QSize s; // QSize()'s own constexpr ctor already yields (-1,-1) --
                  // the SAME sentinel the real implementation writes, so
                  // the fallback below (symbol unresolved) and the
                  // forwarded call (symbol resolved, but Nickel's real
                  // AbstractController::size() genuinely returns this
                  // constant on 4.38.23684) produce the same value.
        if (AbstractController__size)
            AbstractController__size(&s, this);
        return s;
    }
    virtual void viewWillAppear() {
        if (AbstractController__viewWillAppear)
            AbstractController__viewWillAppear(this);
    }
    virtual void viewWillDisappear() {
        if (AbstractController__viewWillDisappear)
            AbstractController__viewWillDisappear(this);
    }
    virtual void viewWillBeDestroyed() {
        if (AbstractController__viewWillBeDestroyed)
            AbstractController__viewWillBeDestroyed(this);
    }
    virtual int allowedOrientations() const {
        return AbstractController__allowedOrientations
             ? AbstractController__allowedOrientations(this)
             : 5; // the measured constant (0xad1324), not an arbitrary guess
    }
    virtual int navSection() const {
        return AbstractController__navSection
             ? AbstractController__navSection(this)
             : 0; // the measured constant (0xad15b0), not an arbitrary guess
    }

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

// Mitigation (1) -- WEAK, and known to be weak (see the header comment's
// "THE MITIGATIONS" section for the full account of why this alone is not
// the layout mitigation): build-time proof this shim's OWN layout matches
// the measured AbstractController layout. Catches a hand-edit that adds a
// field to this class, or a QWeakPointer<T> that somehow stops being 8
// bytes on some future Qt, at COMPILE time. Cannot catch, and was never
// able to catch, a firmware that changes AbstractController's OWN, real
// size -- sizeof(vptr + two pointers) is 12 on this ABI unconditionally,
// so this assertion is checking THIS FILE against ITSELF, not against
// libnickel.so at all. nf_view_layout_check() (below) is the real,
// firmware-facing mitigation.
static_assert(sizeof(AbstractController) == 12,
              "AbstractController (this file's own shim class) must match "
              "the REAL AbstractController's measured 12-byte layout "
              "(NOTES.md, Task 7 rung 2, 0xad1334) -- re-measure before "
              "changing this");

// Nickel's own layout convention for a controller of this shape
// (NOTES.md / this task's brief: "AbstractController at offset 0, QObject
// at +12, with a secondary vtable group carrying _ZThn12_ QObject
// thunks") -- mirrored here as ordinary C++ multiple inheritance, so the
// COMPILER builds the equivalent thunks itself; nothing here reaches for
// offset arithmetic to find QObject's subobject. AbstractController is
// listed FIRST so it is the primary base: a pointer to an NFController IS,
// bit for bit, a valid pointer to its AbstractController subobject --
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
class NFController : public AbstractController, public QObject {
public:
    NFController() : AbstractController(), QObject(0) {}

    ~NFController() {
        // WHEN does this run? Not from anything in this file -- see
        // nf_browser_show()'s own "OWNERSHIP" comment for the full
        // account. Short version: once Critical 1's fix (this class being
        // named exactly `AbstractController`) makes MainWindowController::
        // push's internal cross-cast succeed, Nickel's own
        // QObject::setParent call makes this object a real child in Qt's
        // parent/child tree, and Qt's own `delete` of that parent tree,
        // whenever it happens, is what invokes this destructor -- through
        // this object's REAL vtable, which is why it correctly reaches
        // HERE rather than some default QObject teardown.
        //
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

// --- mitigation (2): the runtime layout check -- THE real one ------------
//
// Two independent, non-vacuous checks, both against THIS firmware's
// actual, live libnickel.so -- not trusted from the measurements and
// comments elsewhere in this file. Run LAZILY (on nf_browser_show()'s
// first call, not at nf_init()), matching nf_nickel_resolve()/
// nf_browser_resolve()'s own pattern of being cheap, side-effect-free
// gates nf_init only LOGS the status of.
//
// (a) THE CONSTRUCTOR'S WRITE PATTERN, EXACTLY -- not merely "wrote
//     something". An earlier draft of this function accepted ANY write
//     to this[+0] as proof the vptr was set ("words[0] !=
//     0xCDCDCDCD"), which is near-vacuous: a constructor that wrote
//     garbage, or the WRONG vtable's address, would pass identically to
//     one that wrote the right one. Caught in review. Fixed by comparing
//     against the REAL vtable's own address point (realSlots itself,
//     see below) -- the EXACT value the real constructor's own
//     disassembly writes into this[+0] (NOTES.md) -- so this is now a
//     genuine equality check, not a "did anything happen" one.
// (b) THE VTABLE'S OWN SLOT CONTENTS, independently of (a): the six
//     symbols this file forwards to (AbstractController__size and
//     friends) were each resolved by NAME (nh_dlsym, nfnickel.cc). That
//     proves each symbol EXISTS somewhere in libnickel.so -- it does NOT
//     prove any of them sits at the VTABLE SLOT this shim's own
//     hand-written virtual function ORDER (and therefore Nickel's own
//     calling convention for it -- e.g. ensureViewLoaded's `ldr
//     r3,[r3,#32]` for slot 8) assumes it does. A firmware that inserted
//     or removed one virtual function anywhere before size(), for
//     instance, would still resolve all six symbols individually by
//     name -- their addresses would not change -- while silently
//     shifting every SLOT this shim's layout depends on, and neither (a)
//     nor the six dlsym resolutions alone could tell the difference.
//     THIS is the check that can: it reads the LIVE, already-relocated
//     `_ZTV18AbstractController` table (AbstractController__vtable,
//     resolved by nfnickel.cc -- a genuine exported DATA symbol, `nm -D`
//     type D, at 0x163ff70 on 4.38.23684) and compares each slot's
//     CONTENT against the same six resolved-by-name pointers --
//     cross-validating two independent ways of finding "the address of
//     AbstractController::size()" (etc.) that have no reason to agree
//     with each other unless the slot this shim assumes really is the
//     slot Nickel's own live vtable puts it at.
static bool nf_view_layout_checked = false;
static bool nf_view_layout_ok      = false;

static bool nf_view_layout_check() {
    if (nf_view_layout_checked)
        return nf_view_layout_ok;
    nf_view_layout_checked = true;

    // nf_view_resolve() (nfnickel.cc) already requires all NINE symbols
    // this function touches -- the ctor, dtor1, the six forwards, and
    // __vtable -- so every pointer used below this point is guaranteed
    // non-NULL; no further per-call NULL-gating is needed inside this
    // function specifically (contrast the shim class's OWN methods,
    // which NULL-gate independently because they can be reached even if
    // this check were somehow bypassed).
    if (!nf_view_resolve()) {
        nh_log("view: an AbstractController symbol did not resolve, refusing the layout check");
        return false;
    }

    // _ZTV18AbstractController resolves to the START of the vtable's
    // Itanium ABI header (offset-to-top word), NOT the "address point"
    // every AbstractController-shaped object actually stores at this[+0]
    // -- the header is TWO words (offset-to-top, RTTI pointer) before the
    // address point. +8 mirrors the SAME adjustment the real
    // constructor's own disassembly makes (`adds r3, #8` after loading
    // this exact symbol's GOT-relocated address, NOTES.md) -- not a
    // separately guessed offset.
    //
    // realSlots[0] and realSlots[1] (D1/D0) are BOTH ZERO IN THE FILE, and
    // that is correct, not a gap: AbstractController is abstract (slot 8
    // is `__cxa_pure_virtual`), and `_ZTV18AbstractController`'s own
    // relocations (checked directly: `readelf -r` / `objdump -R` show
    // entries at 0x163ff74 (the RTTI pointer) and 0x163ff80-0x163ff98
    // (slots 2-8), and NOTHING at 0x163ff78/0x163ff7c) confirm GCC never
    // emitted a destructor into either slot for an abstract class --
    // there is no relocation to fill them with an address, so they read
    // as the zeroed bytes the file already has. This is WHY both real
    // destructors (D1Ev, D0Ev) have to be resolved BY NAME
    // (AbstractController__dtor1, nfnickel.h) rather than read out of the
    // table the way the six slots below are cross-checked against it --
    // the table has nothing there to read. (Our OWN emitted
    // `_ZTV18AbstractController`, in this file's own object code, has the
    // identical two zero words, for the identical reason: NFController is
    // the only CONCRETE class in this hierarchy, so the ABSTRACT base's
    // own vtable-if-it-had-one is never what gets instantiated.)
    // realSlots[2..7] are the six forwarded slots; realSlots[8] is the
    // pure virtual nf_load_view's real counterpart occupies.
    void * const *realSlots = reinterpret_cast<void* const*>(
        reinterpret_cast<char const*>(AbstractController__vtable) + 8);

    // 64 bytes: the SAME over-allocation margin CLAUDE.md asks for every
    // Nickel constructor call (nf_open_book_staged's volbuf) -- the real
    // ctor is measured to write only 12 bytes, but this being throwaway
    // scratch memory rather than a live, pushed object is exactly why the
    // margin costs nothing here either.
    unsigned char scratch[64] __attribute__((aligned(8)));
    // Poison, not zero: the ctor's own measured writes are this[+4]=0 and
    // this[+8]=0 (NOTES.md), which is INDISTINGUISHABLE from "never wrote
    // there" if the buffer already reads zero. 0xCD makes every byte the
    // ctor does NOT touch stay conspicuously non-zero, so a mismatch (the
    // ctor writing past byte 12, or never writing +4/+8 at all) is visible
    // rather than looking like a pass by accident.
    memset(scratch, 0xCD, sizeof scratch);

    AbstractController__ctor(scratch);

    // (a), exact -- see this function's own header comment for what the
    // earlier, near-vacuous version of this specific check missed.
    void * const *scratchPtrs = reinterpret_cast<void* const*>(scratch);
    bool vptrCorrect     = scratchPtrs[0] == reinterpret_cast<void const*>(realSlots);
    unsigned int const *words = reinterpret_cast<unsigned int const*>(scratch);
    bool plus4Zero       = words[1] == 0u;          // this[+4]: measured zero
    bool plus8Zero       = words[2] == 0u;          // this[+8]: measured zero
    bool plus12Untouched = words[3] == 0xCDCDCDCDu; // this[+12]: must NOT be written --
                                                     // if it is, AbstractController is
                                                     // bigger than 12 bytes on this
                                                     // firmware and this shim's QObject
                                                     // base would land on top of live data.

    // (b), the check that actually validates the SLOT ORDER -- see this
    // function's own header comment for the full argument, and realSlots'
    // own comment above for why [0]/[1] are checked for ZERO rather than
    // against a resolved pointer. `realSlots[0] == NULL && realSlots[1]
    // == NULL` is a genuine assertion, not a placeholder: it is the
    // positive form of "AbstractController is still abstract" -- it
    // passes today for a real, relocation-backed reason (see above), and
    // it CATCHES a firmware where AbstractController stopped being
    // abstract (gained a real destructor there), which would invalidate
    // this whole shim's premise that slot 8 is the one and only pure
    // virtual. Slot [8] can only be checked for non-nullness -- it is
    // `__cxa_pure_virtual`, an external libstdc++ symbol this file has no
    // reason to resolve just to compare against itself.
    bool slotsMatch =
        realSlots[0] == 0 && realSlots[1] == 0 &&
        realSlots[2] == reinterpret_cast<void*>(AbstractController__size) &&
        realSlots[3] == reinterpret_cast<void*>(AbstractController__viewWillAppear) &&
        realSlots[4] == reinterpret_cast<void*>(AbstractController__viewWillDisappear) &&
        realSlots[5] == reinterpret_cast<void*>(AbstractController__viewWillBeDestroyed) &&
        realSlots[6] == reinterpret_cast<void*>(AbstractController__allowedOrientations) &&
        realSlots[7] == reinterpret_cast<void*>(AbstractController__navSection) &&
        realSlots[8] != 0;

    // Best-effort, and deliberately LAST: exercises the resolved
    // destructor on the same scratch memory the ctor just initialised, so
    // a firmware where D1Ev itself is broken is caught here too, rather
    // than the first time a real, pushed controller is destroyed. Run
    // AFTER every scratch word above has already been read, not before --
    // harmless either way today (D1Ev on a zeroed this[+4] is a no-op,
    // per its own disassembly), but reading scratch and then mutating it
    // is the less fragile order to keep.
    AbstractController__dtor1(scratch);

    if (!vptrCorrect || !plus4Zero || !plus8Zero || !plus12Untouched || !slotsMatch) {
        nh_log("view: AbstractController layout check FAILED -- vptrCorrect=%d plus4Zero=%d "
               "plus8Zero=%d plus12Untouched=%d slotsMatch=%d -- the layout or vtable-slot "
               "assumption is STALE on this firmware, refusing to build the shim controller",
               vptrCorrect, plus4Zero, plus8Zero, plus12Untouched, slotsMatch);
        return false;
    }

    nh_log("view: AbstractController layout check passed (12-byte ctor write pattern, "
           "exact vptr, AND all six forwarded vtable slots confirmed)");
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

    // sizeof(NFController), exactly -- NOT an over-allocated margin like
    // nfbrowser.cc's 512-byte controller buffer or nf_open_book_staged's
    // volbuf. Those exist because THEIR size is read off a Nickel
    // constructor this project cannot ask, so the margin absorbs a
    // firmware that grows the real object underneath an unchanged
    // measurement. NFController is not that: it is THIS FILE'S OWN class,
    // compiled by THIS SAME build, so `sizeof(NFController)` is never a
    // guess -- allocating exactly that (rather than a round number that
    // LOOKS like CLAUDE.md's over-allocation convention but isn't one) is
    // the correct amount, not slack pretending to be a measurement.
    void *mem = ::operator new(sizeof(NFController));
    memset(mem, 0, sizeof(NFController));

    // AbstractController__ctor is NOT called here, UNLIKE an earlier
    // draft of this function -- that draft's comment claimed calling it
    // meant "Nickel's own initialisation genuinely runs", which was
    // false, caught in review (this project has now hit that exact class
    // of mistake -- a comment asserting something the code does not do --
    // three times; see the review that flagged it for the tally). The
    // call would be INERT here: the placement-new immediately below
    // overwrites this[0]/[4]/[8] as an ordinary part of NFController's
    // own construction regardless -- the compiler sets the vptr;
    // AbstractController's own member-initialiser list zeroes nf_weak_d/
    // nf_weak_widget to the same zero the real ctor would have written --
    // and the real ctor is MEASURED (NOTES.md) to do NOTHING beyond
    // writing those same three words. So calling it here first would
    // write values placement-new immediately discards a moment later,
    // achieving nothing observable. It IS still called, separately,
    // inside nf_view_layout_check() above -- but that is a DIAGNOSTIC use
    // against THROWAWAY scratch memory (does the real ctor still write
    // what this shim assumes, on THIS firmware?), not an initialisation
    // one, and the two purposes should not be conflated the way the
    // earlier comment did.
    NFController *ctrl = new (mem) NFController();

    nh_log("view: pushing the shim controller");
    // ctrl's AbstractController base is its FIRST base (no virtual
    // inheritance anywhere in this hierarchy), so this address equals
    // `ctrl` itself -- no cast arithmetic would technically be required.
    // The two-step cast (through AbstractController* explicitly, THEN to
    // void*) is kept anyway to spell out WHICH subobject is meant, rather
    // than relying on that coincidence of layout -- the same "no cast
    // arithmetic needed" property nfnickel.h already documents for
    // QuickAccessLibraryController's real base, made explicit instead of
    // implicit now that AbstractController is a real, nameable type in
    // this file.
    void *controllerBase = static_cast<void*>(static_cast<AbstractController*>(ctrl));

    // OWNERSHIP -- read this before "fixing" what looks like a leak.
    // `mem` is never freed by this function, and `ctrl` is never
    // `delete`d anywhere in this file. Before Critical 1 (this class
    // being named exactly `AbstractController`, at global scope -- see
    // this file's own header comment for the full derivation),
    // MainWindowController::push's internal cross-cast to QObject*
    // FAILED, which skips `QObject::setParent(controller, view)` --
    // Nickel's own mechanism for taking ownership of a pushed controller
    // -- so this object genuinely WAS a permanent leak (sizeof(NFController)
    // bytes plus its refcount header, per call). WITH Critical 1's fix,
    // the cross-cast succeeds, `setParent` runs, and this object becomes a
    // real child in Qt's own parent/child tree: Qt deletes a child
    // whenever ITS parent is destroyed, and that `delete` reaches THIS
    // object through its REAL vtable, correctly running `~NFController()`
    // (which correctly tears down nf_weak_d/nf_weak_widget and calls the
    // resolved AbstractController__dtor1 -- see that destructor's own
    // comment) and then `operator delete` on the SAME address
    // `::operator new(sizeof(NFController))` returned above (a deleting
    // destructor always recovers the complete object's own start address
    // before deallocating, regardless of which base subobject pointer was
    // used to reach it).
    //
    // So: this object is NOT a leak once Critical 1 is in effect (i.e.
    // right now, in this build) -- and NOTHING in this file should ever
    // call `delete ctrl` (or `delete controllerBase`) directly. Doing so
    // would race, or outright double-free, against Qt's own
    // parent-driven deletion, which is now the ONLY thing responsible for
    // this object's lifetime once `push` hands it off below.
    MainWindowController__push(mwc, controllerBase, true);
    return true;
}
