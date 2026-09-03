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

// AbstractController is Nickel's base UI-controller class. Opaque per the
// house rule ("Nickel's classes stay opaque") -- typedef + explicitly
// written call signatures, never a real C++ class. Kept here (unlike rung
// 2's original version of this file) purely as the STATIC TYPE
// MainWindowController::push's second parameter takes: this rung builds no
// AbstractController-shaped object by hand at all. QuickAccessLibraryController
// -- the real controller nfbrowser.cc constructs -- has AbstractController
// as its PRIMARY base at offset 0 (confirmed: NickelGridLibraryControllerBase's
// own ctor calls AbstractController::AbstractController() with `this` passed
// straight through, no offset adjustment -- archaeology part 2, P2.2), so a
// pointer to one IS, bit-for-bit, a valid AbstractController* with no cast
// arithmetic needed.
typedef void AbstractController;
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
extern void   (*MainWindowController__push)(MainWindowController *_this, AbstractController *controller, bool animate);

// QuickAccessLibraryController::QuickAccessLibraryController(QSharedPointer<LibraryDataSource<Volume> >)
// at 0xf44fb4 on 4.38.23684. sizeof == 72, read at the `movs r0, #72` inside
// QuickAccessMenuView::QuickAccessMenuView's own `operator new` call site
// (0xf499a4) -- Nickel's OWN allocation for this exact class, re-confirmed
// directly against this binary for this rung (archaeology part 2, P2.2).
// NOT static: nfbrowser.cc calls this directly, the same way it directly
// called AbstractController__ctor before this rung replaced that plan.
// `_this` is typed AbstractController* rather than a distinct opaque type
// for the controller, on purpose -- see AbstractController's own comment
// above: they are the same address, and this saves nfbrowser.cc a cast
// between "freshly allocated" and "ready to push".
extern void (*QuickAccessLibraryController__ctor)(AbstractController *_this, void const *source /* QSharedPointer<LibraryDataSource<Volume> > const& */);

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
