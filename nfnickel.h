// The mod's libnickel call surface, and the trigger-watch infrastructure that
// drives it. Every symbol pulled out of libnickel lives in nfnickel.cc, in one
// place, so a firmware's layout assumptions are never split across files --
// see CLAUDE.md, "Nickel's classes stay opaque". NOTES.md has the disassembly
// behind every signature declared here.
#ifndef NFNICKEL_H
#define NFNICKEL_H

#include <QString>
#include <QStringList>

#include <NickelHook.h>

// The dlsym table nfnickel.cc's symbols resolve through. NickelHook processes
// exactly one such array per mod (struct nh's `dlsym` field), so nfolders.cc's
// NickelHook(...) invocation wires this one in directly -- there is nowhere
// else for it to live once every entry has moved out of nfolders.cc.
extern struct nh_dlsym NFNickelDlsym[];

// True once every symbol above has resolved. Every NFNickelDlsym entry is
// .optional = true (see nfnickel.cc for why -- the shared NickelHook
// failsafe), so a missing one no longer stops nf_init from running at all;
// it resolves to NULL instead, and THIS is what catches that: nf_open_book
// and nf_open_book_staged both refuse to run rather than call through a null
// pointer. A firmware that renames or drops one of these symbols makes
// book-opening inert -- logged, not a crash, and not a failsafe trip that
// could take the owner's other NickelHook mods down with it.
bool nf_nickel_resolve(void);

// The same idea as nf_nickel_resolve() above, but for the symbols rung 2's
// screen needs (QuickAccessLibraryController's ctor, the data-source chain
// underneath it, and MainWindowController's sharedInstance/push) and NONE of
// the seven above -- kept as a separate bool on purpose, so a firmware that
// breaks one feature's symbols does not also disable the other's. Note that
// nf_build_volume_source (below) ADDITIONALLY needs nf_nickel_resolve() to
// be true too (it calls VolumeManager::getById over each ContentID, the same
// as book-opening does) and checks that itself -- this bool alone is not
// sufficient to call nf_browser_show_volumes safely, which is why
// nfbrowser.cc's own gate checks both. See nfnickel.cc.
bool nf_browser_resolve(void);

// The dbName VolumeManager::getById wants for THIS device -- see nfnickel.cc
// for the derivation. NULL only if Device::getCurrentDevice() itself returned
// NULL, which the disassembly gives no reason to expect on any firmware this
// has been checked against, but nothing here can rule out on one it hasn't.
QString const *nf_db_name(void);

// The mod's product entry point: opens contentId in the stock reader using
// this device's own dbName (nf_db_name), running the full getById -> proxy ->
// onSelected sequence. Returns false, without navigating anything, if the
// symbols never resolved, if getById found no such book, or if the device's
// own dbName lookup unexpectedly failed.
bool nf_open_book(QString const& contentId);

// The staged form the spike exposed directly, kept as CLAUDE.md's fixed
// method for adding a new libnickel call requires ("advance one call at a
// time" -- see "Method: adding a new libnickel call"). NOT dead code: the
// trigger protocol in nfolders.cc still drives this at an explicit stage and
// an explicit dbName, which is how each of the next five rungs will localise
// a crash to the one new call it adds rather than to all of them at once.
// nf_open_book (above) is what the rest of the mod calls; it delegates here
// with stage=4 and the correct dbName.
bool nf_open_book_staged(QString const& contentId, QString const& dbName, int stage);

// MainWindowController is Nickel's window-stack singleton. Opaque per the
// house rule ("Nickel's classes stay opaque") -- typedef + explicitly
// written call signatures, never a real C++ class.
//
// AbstractController USED to be typedef'd void here too, for exactly the
// same reason -- but nfview.cc's shim controller now needs a REAL class
// literally NAMED AbstractController, at global scope, so that
// MainWindowController::push's internal `__dynamic_cast(controller,
// &_ZTI18AbstractController, &_ZTI7QObject, -2)` cross-cast finds a
// matching type NAME in our object's own RTTI (Itanium ABI: the src_type
// lookup is a `type_info::operator==`, which falls back to a byte-for-byte
// `strcmp` of the mangled name when pointer identity fails across DSOs --
// see nfview.cc's own header comment for the full derivation, and the
// review that caught this: a shim class named anything else compiles,
// links, and pushes, but the cast returns NULL, `topController()` stays
// NULL, and Nickel never calls QObject::setParent on it either). That real
// class can only live where it is actually defined (nfview.cc), so this
// header no longer names a type here at all: every opaque use below that
// used to say `AbstractController*` now says `void*` -- these call sites
// (MainWindowController::push, QuickAccessLibraryController__ctor,
// ArticleListLibraryController__ctor) never dereferenced an
// AbstractController* as anything but an opaque address anyway, so nothing
// about the ACTUAL signature changes, only the type NAME this header uses
// to spell it.
typedef void MainWindowController;

// MainWindowController::sharedInstance() -- a lazily-constructed singleton
// accessor, not further disassembled (out of this rung's stated scope; see
// NOTES.md). MainWindowController::push(AbstractController*, bool) puts a
// controller onto Nickel's own window stack; NOTES.md has its complete PLT
// stub resolution. Rung 2's replacement plan (folder-stack-archaeology.md,
// Part 2) is what makes push's internal dynamic_cast<QObject*> succeed for
// real, rather than merely tolerate failing: QuickAccessLibraryController IS
// a QObject, with real RTTI Nickel's own compiler generated -- nothing here
// fabricates that any more.
extern void  *(*MainWindowController__sharedInstance)(void);
extern void   (*MainWindowController__push)(MainWindowController *_this, void *controller, bool animate);

// Which controller nf_browser_show_volumes (nfbrowser.cc) actually builds.
// A device run pushed QuickAccessLibraryController successfully -- full-
// screen, covers/titles/authors/format/size, reading progress, and tap-to-
// open all worked exactly as archaeology part 2 traced, with no hook from
// this mod -- but found it has NO header and NO back arrow at all: it is
// the home page's own quick-access WIDGET, not a page, so it carries no
// navigation chrome. On a device with no hardware back button that is a
// dead end, not a cosmetic gap, so ArticleListLibraryController (the
// brief's own named hedge for exactly this failure) is now the default.
//
// Both constructors stay resolved UNCONDITIONALLY below -- one more dlsym
// lookup at init is negligible, and it means flipping this single macro
// to 0 is enough to rebuild the proven QuickAccessLibraryController path
// for a side-by-side comparison, with no other code change anywhere.
// nf_browser_resolve()'s gate (nfnickel.cc) and nf_browser_show_volumes's
// own construction branch (nfbrowser.cc) both key off this same macro, so
// they can never disagree about which one is "the" controller. Do not
// ship this flipped to 0: it has no way out of the screen on hardware with
// no hardware back button.
#define NF_BROWSER_USE_ARTICLE_LIST 1

// QuickAccessLibraryController::QuickAccessLibraryController(QSharedPointer<LibraryDataSource<Volume> >)
// at 0xf44fb4 on 4.38.23684. sizeof == 72, read at the `movs r0, #72` inside
// QuickAccessMenuView::QuickAccessMenuView's own `operator new` call site
// (0xf499a4) -- Nickel's OWN allocation for this exact class, re-confirmed
// directly against this binary for this rung (archaeology part 2, P2.2).
// Proven on hardware (see NF_BROWSER_USE_ARTICLE_LIST above); kept only as
// the comparison path now, not the one the trigger uses.
extern void (*QuickAccessLibraryController__ctor)(void *_this, void const *source /* QSharedPointer<LibraryDataSource<Volume> > const& */);

// ArticleListLibraryController::ArticleListLibraryController(QSharedPointer
// <LibraryDataSource<Volume> >) at 0xdcbe70 on 4.38.23684. sizeof == 92,
// re-measured for THIS rung (the report's own number was explicitly
// distrusted and re-derived, per instruction, using objdump -R rather than
// readelf -r -- see NOTES.md's truncation-trap entry for why that
// distinction matters): `movs r0, #92` at 0xdc8d16, inside
// `ArticleLibraryBuilder::newController`'s ELSE branch (its IF branch,
// 0xdc8b98, builds the same-sized ArticleGridLibraryController instead --
// same function, same 92, a different class), immediately before
// `blx operator new` then a call `tools/plt.sh` resolves to this exact
// constructor. Confirmed, not merely trusted, that its constructor needs
// nothing new: same NickelGridLibraryControllerBase(int,int,src) base as
// QuickAccessLibraryController (0x6a2fd4, same stub), plus exactly one
// more thing, a FRESH `BrowserWorkflowManager(QObject*)` member
// (0xcfb298) constructed with a null parent -- confirmed by reading that
// constructor itself: it calls no `sharedInstance()`, touches no shared
// state, only `QObject::QObject` and its own field writes. No title
// string, no extra QSharedPointer, no singleton read or write. `_this` is
// `void*` for the same reason as QuickAccessLibraryController__ctor,
// above -- see this header's own comment on why AbstractController is no
// longer a type name declared here.
extern void (*ArticleListLibraryController__ctor)(void *_this, void const *source /* QSharedPointer<LibraryDataSource<Volume> > const& */);

// --- the shim controller's two raw AbstractController symbols -------------
//
// nfview.cc builds our OWN controller and view -- a SANCTIONED, narrow
// exception to "Nickel's classes stay opaque" (see nfview.cc's own header
// comment for the full argument and the three mitigations it carries). The
// two symbols below are still resolved and called the project's usual way
// -- an opaque, explicitly-written call signature, never a redeclared
// method -- it is only nfview.cc's shim CLASS that is real C++, not these.
//
// AbstractController::AbstractController(), the "base object constructor"
// (C2Ev) variant -- what Nickel's OWN derived-controller code calls when
// building an AbstractController AS A BASE SUBOBJECT (NOTES.md: `blx
// 6a3708 -> _ZN18AbstractControllerC2Ev`, PasswordController/
// HelpDialogController). C1Ev aliases the IDENTICAL address (0xad1334 on
// 4.38.23684, confirmed nm -D --defined-only), so either name resolves the
// same function; C2Ev is used because it is what real Nickel code calls for
// this exact purpose. Measured to call nothing and write exactly 3 words --
// this[+0]=vptr, this[+4]=0, this[+8]=0 (NOTES.md, "sizeof(AbstractController)
// == 12 bytes") -- but resolved and called anyway, never assumed a no-op:
// nf_view_layout_check() (nfview.cc) verifies those exact writes at RUNTIME,
// against THIS firmware build, every time nf_browser_show() is first called,
// rather than trusting this comment to still be true.
extern void (*AbstractController__ctor)(void *_this);

// AbstractController::~AbstractController(), the COMPLETE OBJECT destructor
// (D1Ev, 0xad1358 on 4.38.23684) -- deliberately NOT the deleting destructor
// (D0Ev, 0xad1398), which additionally calls operator delete on `this` and
// would double-free memory nfview.cc's shim manages itself (one
// ::operator new(...) block backing the WHOLE shim object, not just its
// AbstractController-shaped base).
//
// Unlike the ctor above, this IS NOT a no-op -- disassembled for this task,
// exactly per "resolve the real destructors by name too -- do not rely on
// the zero slots" the earlier, abandoned hand-copied-vtable plan found
// (NOTES.md, "The paragraph above was wrong, and the mistake is left in on
// purpose"). D1Ev reads this[+4] (nfview.cc's nf_weak_d), atomically
// decrements its first word (an ldrex/strex CAS loop -- the weakref
// QBasicAtomicInt), and if that reaches zero, calls `operator delete` on it
// (`blx 672404`, resolved with tools/plt.sh to `_ZdlPv`) -- EXACTLY
// QWeakPointer<T>::~QWeakPointer()'s own documented logic
// (qsharedpointer_impl.h: "if (d && !d->weakref.deref()) delete d"). So
// this symbol is not merely defensive insurance -- it is the ONLY correct
// way to tear down the this[+4]/this[+8] pair nf_load_view() (nfview.cc)
// builds, and nfview.cc's own destructor calls it exactly once, never
// duplicating the decrement/delete by hand when this resolved (doing both
// would double-decrement, and potentially double-free, the same weakref).
extern void (*AbstractController__dtor1)(void *_this);

// --- the shim controller's six OTHER AbstractController symbols -----------
//
// A compiler-generated shim (nfview.cc) means the COMPILER emits the
// vtable, not a copy of Nickel's own -- so every slot AbstractController's
// vtable carries is now OURS, and a slot this shim does not forward is
// behaviour a real derived controller had that this one silently drops,
// not a harmless placeholder. This is the direct consequence of moving off
// the "hand-copy Nickel's vtable, only slot 8 is ours" plan the original
// rung-2 attempt used: that plan got these six for free BY COPYING;
// nothing here copies anything, so nothing here is free. Each was
// disassembled independently for this task -- resolved by exact address
// with `nm -D --defined-only`, then read instruction-by-instruction, per
// CLAUDE.md's "Method: adding a new libnickel call" ("do not infer a
// calling convention from the name" -- VolumeManager::getById's missing
// `this` is what guessing here has cost before, NOTES.md).
//
// AbstractController::size() -- 0xad12ec. Writes QSize(-1,-1) -- Qt's own
// "invalid size" sentinel, confirmed against this ARM sysroot's qsize.h
// ("Q_DECL_CONSTEXPR inline QSize::QSize() : wd(-1), ht(-1) {}") -- through
// r0, with `this` (r1) loaded but never dereferenced. r0 is a HIDDEN
// RETURN BUFFER, not `this` doing double duty: this[+0]/this[+4] are the
// vptr and the QWeakPointer `d` field this shim's own layout depends on
// (NFAbstractControllerShim, nfview.cc), and a write there would corrupt
// every controller of this shape on its very first size() call, which no
// real base-class accessor would do. QSize has user-declared constructors
// (qsize.h), which is what makes the ARM C++ ABI classify it as "not POD
// for the purposes of calls" and return it indirectly regardless of its
// 8-byte size -- stated here as the REASON the measurement makes sense,
// not as the basis for the signature: the signature below is written from
// the disassembly, the same discipline as VolumeManager::getById's own
// hidden-buffer signature (nfnickel.cc).
extern void (*AbstractController__size)(void *sretQSize, void const *_this);

// AbstractController::viewWillAppear/viewWillDisappear/viewWillBeDestroyed
// -- 0xad1300, 0xad130c, 0xad1318. Measured, all three: a bare prologue
// and epilogue with NOTHING between them -- true no-ops in the BASE
// implementation, on THIS firmware. Forwarded anyway, not left as inert
// stubs matching that measurement: a firmware where these stop being
// no-ops would silently start dropping behaviour again if this shim
// assumed today's measurement holds forever.
extern void (*AbstractController__viewWillAppear)(void *_this);
extern void (*AbstractController__viewWillDisappear)(void *_this);
extern void (*AbstractController__viewWillBeDestroyed)(void *_this);

// AbstractController::allowedOrientations() const -- 0xad1324. `this`
// loaded, never dereferenced; unconditionally returns the constant 5 in
// r0. A plain SCALAR return -- unlike size(), no hidden buffer: an `int`
// has no constructor to trip the "not POD for calls" ARM ABI rule above,
// so it returns the ordinary way (this in r0/r1 slot per the calling
// convention, result in r0).
extern int (*AbstractController__allowedOrientations)(void const *_this);

// AbstractController::navSection() const -- 0xad15b0 (a WEAK symbol,
// unlike every other symbol in this file -- confirmed with `nm -D`, not
// significant to how it is called, just noted because it was unexpected).
// Same shape as allowedOrientations(): `this` loaded, unused, constant 0
// returned in r0.
extern int (*AbstractController__navSection)(void const *_this);

// _ZTV18AbstractController itself -- 0x163ff70 on 4.38.23684, `nm -D` type
// D (a DATA symbol, the only one this whole project resolves by name --
// every other entry, here and in every other dlsym table, is a function).
// dlsym works identically for data and function symbols, so this resolves
// the SAME way as the nine function pointers above; what differs is only
// how nfview.cc's nf_view_layout_check() USES it: reading the LIVE
// vtable's own slots is what turns "these six symbols individually
// resolved" into "these six symbols sit at the SLOT this shim's vtable
// layout assumes" -- a firmware that shifted the slot order (inserted or
// removed a virtual before size(), say) would still resolve all six
// symbols individually by name, so the six dlsym entries ALONE cannot
// catch that; comparing each against the live table's own slot can.
// Points at the START of the vtable's Itanium ABI header (offset-to-top
// word) -- NOT the "address point" (the vptr value every
// AbstractController-shaped object actually stores at +0), which is this
// address PLUS 8 (two header words) -- nf_view_layout_check() applies
// that same +8 adjustment the real constructor's own disassembly does
// (NOTES.md: `adds r3, #8` after loading this exact symbol).
extern void *AbstractController__vtable;

// True once every AbstractController symbol this shim needs has resolved
// -- the original two (ctor, dtor1), the six forwarded slots, AND the
// vtable data symbol above (nine total). Deliberately ONE gate, not nine
// independent ones: none of these is "nice to have" the way most
// .optional entries in this project are -- a firmware missing just
// size(), for instance, would silently fall back to a default-constructed
// QSize(-1,-1) everywhere Nickel expects a real size, which can lay a
// view out to nothing -- indistinguishable, on a screenshot, from "the
// screen never appeared," which is exactly the kind of wrong-place
// hunting this gate exists to prevent. So nf_browser_show() (nfview.cc)
// refuses to push AT ALL if any of these nine is missing, rather than
// pushing a controller with some slots silently reverted to placeholder
// behaviour, or skipping the vtable-slot cross-check that is this file's
// real runtime layout mitigation (see nf_view_layout_check(), nfview.cc).
//
// A THIRD, independent gate from nf_nickel_resolve()/nf_browser_resolve()
// (book-opening; the borrowed-controller route) -- a firmware that breaks
// just one of these three features does not silently disable the other
// two. See nf_browser_resolve()'s own comment for why this independence is
// deliberate, not merely convenient.
bool nf_view_resolve(void);

// QSharedPointer<T>'s complete runtime layout, for every T, per Qt 5.2's
// public qsharedpointer_impl.h: a value pointer, then an
// ExternalRefCountData*. Two pointers, 8 bytes -- nothing here depends on
// T's own fields, which is why ONE struct describes both distinct
// QSharedPointer<...> instantiations nf_build_volume_source builds
// (QSharedPointer<LibraryDataProvider<Volume> > and
// QSharedPointer<LibraryDataSource<Volume> >). Confirmed against this
// binary's own calling convention, not merely assumed: every constructor
// that takes one of these receives a plain pointer in r1 and reads value at
// +0, d at +4 -- archaeology part 2, P2.2. NOTE: both
// LinearLibraryDataSource<Volume>'s and QuickAccessLibraryController's own
// ctors take this BY VALUE, not by const reference -- neither mangled name
// carries an `RK` -- so the CALLEE destroys whatever is passed at `r1`.
// This does not change how nf_build_volume_source/nfbrowser.cc pass one
// (still the address of a local NFSharedPtr; the ABI for a non-trivial
// by-value parameter is the same "pass an address" shape as a reference),
// but it does mean the passed-in object's underlying refcount is
// deliberately consumed by the call, not left alone -- see
// nf_build_volume_source's own comment on why strongref/weakref start at 2,
// not 1.
struct NFSharedPtr { void *value; void *d; };

// Looks up every ContentID in contentIds via VolumeManager::getById (the
// SAME getById/isValid/dtor discipline nf_open_book_staged already uses --
// see nf_open_book_staged's own comments for the measurements behind it; an
// invalid ContentID's Volume is simply never appended, which IS the negative
// control the brief's device checklist asks for, with no special-casing
// needed), builds a QVector<Volume> from what is found, and wraps it in an
// InMemoryDataProvider<Volume> and a LinearLibraryDataSource<Volume> --
// Nickel's own classes throughout, resolved by name. *outSource is left
// untouched and false is returned if a required symbol never resolved
// (checks BOTH nf_nickel_resolve(), for getById itself, and the data-source
// chain's own symbols -- see nf_browser_resolve's comment for why these are
// two independent gates) OR if not one ContentID resolved to a real book --
// a caller cannot otherwise tell "the chain is broken" from "the reference
// list is stale," so an empty result is treated as failure, not as an
// empty-but-valid screen. *outKept receives how many rows were actually
// found (<= contentIds.size()), for the caller to log; unlike *outSource it
// is written even on a false return (0, in that case), so a caller can log
// it unconditionally. See nfnickel.cc for the one hand-built structure this
// needs (the QSharedPointer control block) and why it is acceptable.
bool nf_build_volume_source(QStringList const& contentIds, QString const& dbName, NFSharedPtr *outSource, int *outKept);

// Sets up an inotify watch on the DIRECTORY containing `path` -- never on
// `path` itself, because a watch cannot be established on a file that does
// not exist yet, and the trigger file is created fresh on every use (see
// nfnickel.cc) -- for IN_CLOSE_WRITE/IN_MOVED_TO events whose name matches
// path's basename. `cb` runs with no arguments on whatever thread calls
// nf_watch_init (the GUI thread, in this mod), once per matching event.
// Returns 0 on success, -1 on failure OR if the watch was set up but is
// known dead on arrival (no QAbstractEventDispatcher for this thread yet --
// see nfnickel.cc); either way the reason is already logged, and the caller
// must not report readiness on a non-zero return.
//
// Callable more than once, up to a small fixed cap (nfnickel.cc), to watch
// several distinctly-named trigger files -- rung 2 added a second one
// (nfolders.cc: /tmp/nfolders-show, alongside rung 1's /tmp/nfolders-open).
// Every call after the first REUSES the one inotify fd/notifier rather than
// creating another, and only when `path` resolves to the SAME directory as
// the first call -- every trigger this mod uses lives directly in /tmp, so
// that is not a real restriction here, but a second call for a path in a
// DIFFERENT directory fails (logged, -1) rather than silently watching the
// wrong place.
int nf_watch_init(char const *path, void (*cb)(void));

#endif
