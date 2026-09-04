// The mod's libnickel call surface, and the trigger-watch infrastructure that
// drives it. Every symbol pulled out of libnickel lives in nfnickel.cc, in one
// place, so a firmware's layout assumptions are never split across files --
// see CLAUDE.md, "Nickel's classes stay opaque". NOTES.md has the disassembly
// behind every signature declared here.
#ifndef NFNICKEL_H
#define NFNICKEL_H

#include <QString>
#include <QStringList>
#include <QWidget> // TouchLabel's parent parameter, and QFlags<Qt::WindowType> (qnamespace.h, pulled in transitively) for its ctor's own signature

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
// AbstractController was, briefly, typedef'd as a REAL, compiler-generated
// C++ class here instead of void (nfview.cc's now-deleted shim), the
// project's one SANCTIONED exception to this rule -- needed because
// MainWindowController::push(AbstractController*, bool) performs a genuine
// Itanium ABI cross-cast whose src_type lookup is a mangled-NAME comparison,
// which only a real, compiler-generated RTTI hierarchy can satisfy. Task 8's
// touch-input archaeology (NOTES.md) replaced that whole route:
// N3DialogFactory::getDialog + MainWindowController::pushView (below) needs
// no controller, no cross-cast, and no class sharing a Nickel name at all --
// so the exception is retired along with the code it excused, and every
// opaque use below is a plain `void*`/typedef, same as everywhere else in
// this project.
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
// above -- neither ever dereferences its controller as anything but an
// opaque address.
extern void (*ArticleListLibraryController__ctor)(void *_this, void const *source /* QSharedPointer<LibraryDataSource<Volume> > const& */);

// --- the native-dialog route: N3Dialog, TouchLabel, MainWindowController's
// pushView/popView ----------------------------------------------------------
//
// Task 8 (NOTES.md, "touch input archaeology, and a measured route to a
// custom interactive screen") replaced the AbstractController shim with
// this: Nickel's OWN dialog chrome and OWN tappable row widget, reached the
// project's usual way -- opaque typedef, explicit call signature, every
// entry .optional and NULL-gated. No cross-cast, no fabricated RTTI, no
// class of ours sharing a Nickel name.

typedef void N3Dialog;

// N3DialogFactory::getDialog(QWidget *content, bool) -- STATIC (r0=content,
// r1=bool, no `this`; NOTES.md, 0xead698, every PLT stub resolved). Builds a
// FRESH N3Dialog with its own `operator new(68)` (measured at this exact
// call site, confirmed independently for this task against the local
// libnickel.so.1.0.0 -- we never allocate an N3Dialog ourselves), calls
// N3Dialog::setContent(content) -- which REPARENTS content into the
// dialog's own layout and calls content->show() -- and wires the dialog's
// closeTapped() signal to MainWindowController::closeActiveN3Dialogs().
// Returns the new N3Dialog* in r0, not by value.
//
// What the `bool` means is NOT established (NOTES.md): it is forwarded
// verbatim into N3Dialog's own constructor and never otherwise examined
// inside getDialog. `true` is what NickelHardcover (MIT,
// codeberg.org/StrayRose/NickelHardcover) passes, unexplained there too --
// followed here as working prior art, not as an understood value.
extern N3Dialog *(*N3DialogFactory__getDialog)(QWidget *content, bool fullScreenIdk);
extern void      (*N3Dialog__setTitle)(N3Dialog *_this, QString const &title);
extern void      (*N3Dialog__enableBackButton)(N3Dialog *_this, bool enable);

// MainWindowController::pushView(QWidget*) -- 0xea968c (NOTES.md). Distinct
// from MainWindowController::push(AbstractController*, bool), above: this
// sets NO objectName, does NOT touch the controller stack
// (MainWindowController+60) at all, and just does `stack->addWidget(v);
// stack->setCurrentWidget(v)` after closing any open touch menus -- which
// is exactly why this route needs no controller and no cross-cast: nothing
// here ever asks the pushed widget to BE one.
//
// popView(QWidget*) -- 0xea91e0 -- is the exact counterpart:
// setVisible(false), deleteLater(), stack->removeWidget(v). It DESTROYS the
// widget, so nothing pushed this way may be touched again after a pop.
// N3Dialog's own backTapped() signal is not pre-wired to this (only
// closeTapped() is, to closeActiveN3Dialogs() -- see getDialog above) --
// wiring backTapped() to a popView call is this file's own job (nfview.cc).
extern void (*MainWindowController__pushView)(MainWindowController *_this, QWidget *view);
extern void (*MainWindowController__popView)(MainWindowController *_this, QWidget *view);

// TouchLabel -- the row widget this route uses for every tap target.
// Opaque per house rule: `void`, not a redeclaration of TouchLabel's own
// class. 132 bytes measured (NOTES.md, cross-checked against multiple
// `operator new` call sites, not merely this constructor's own writes) at
// `_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE`, 0xbbae3c on
// 4.38.23684 -- confirmed present at that exact address against the local
// libnickel.so.1.0.0 for this task. NickelHardcover calloc(1,128)s this
// exact class on this exact firmware -- a live 4-byte heap overflow there,
// per NOTES.md -- which is the concrete argument for re-measuring rather
// than trusting even a working, shipped mod's own number: nfview.cc
// allocates 256, not 132 and not 128.
//
// Self-registers for Nickel's own tap-gesture pipeline INSIDE this
// constructor (TouchLabel::initialize(), 0xbba540, NOTES.md) -- the entire
// reason this route uses TouchLabel rather than a bare QWidget/QPushButton:
// constructing one is the whole opt-in, nothing else to call.
typedef void TouchLabel;
extern void (*TouchLabel__ctor)(TouchLabel *_this, QWidget *parent, QFlags<Qt::WindowType> flags);

// True once every symbol this route needs has resolved -- a FOURTH,
// independent gate from nf_nickel_resolve()/nf_browser_resolve() (the same
// independence those two already keep from each other): a firmware that
// renames one of these six does not disable book-opening or the borrowed-
// controller browser screen, and vice versa. nf_browser_show() (nfview.cc)
// refuses to build anything at all if this is false, rather than pushing a
// half-wired dialog with some calls silently skipped.
bool nf_native_view_resolve(void);

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
