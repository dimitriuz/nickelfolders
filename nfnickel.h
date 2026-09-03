// The mod's libnickel call surface, and the trigger-watch infrastructure that
// drives it. Every symbol pulled out of libnickel lives in nfnickel.cc, in one
// place, so a firmware's layout assumptions are never split across files --
// see CLAUDE.md, "Nickel's classes stay opaque". NOTES.md has the disassembly
// behind every signature declared here.
#ifndef NFNICKEL_H
#define NFNICKEL_H

#include <QString>

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

// The same idea as nf_nickel_resolve() above, but for the four symbols
// nfbrowser.cc needs (AbstractController's ctor and vtable,
// MainWindowController's sharedInstance and push) and NONE of the seven
// above -- kept as a separate bool on purpose, so a firmware that breaks one
// feature's symbols does not also disable the other's. See nfnickel.cc.
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
// house rule ("Nickel's classes stay opaque") even though nfbrowser.cc, not
// this file, is what actually builds an object shaped like one -- the
// typedef stays here because it is the type the four symbols below are
// resolved AS or against, and every libnickel symbol's resolved type lives
// in one place per this header's own opening comment. NOT a QObject: its typeinfo
// (0x163ff68 on 4.38.23684) is a plain __class_type_info with no base --
// given, device-verified fact, not re-derived here.
typedef void AbstractController;
typedef void MainWindowController;

// AbstractController::AbstractController() writes exactly 3 words (a vptr,
// then zero at +4 and +8) and calls nothing else -- confirmed by its own
// disassembly containing no bl/blx at all, so calling it through this
// pointer has no effect beyond those three writes. sizeof(AbstractController)
// == 12 on 4.38.23684: those same three writes are the lower bound, and two
// independent derived controllers each place their OWN next field exactly
// 12 bytes after their AbstractController base subobject starts, with no
// gap. NOTES.md, "Task 7, rung 2", has the full derivation. nfbrowser.cc
// over-allocates well past 12 bytes before calling this, per CLAUDE.md's
// "over-allocate for every Nickel constructor" -- the constructor cannot be
// told how much room it has.
extern void (*AbstractController__ctor)(AbstractController *_this);

// The resolved ADDRESS of Nickel's own _ZTV18AbstractController -- an
// 11-word array (offset-to-top, RTTI pointer, then the 9 slots CLAUDE.md's
// measured table lists) that nfbrowser.cc copies wholesale at runtime rather
// than hand-transcribing byte-for-byte, because two of those 9 slots (the
// destructors) carry no relocation this project's tools could decode
// statically (NOTES.md). Reading it live sidesteps needing to know how
// Nickel's own loader populates them -- only that by the time nf_init runs,
// the table in memory is correct, the same trust every other dlsym'd
// pointer in this project already rests on. A plain `void**` , not a
// pointer-to-array-of-11, because nfbrowser.cc indexes it with a runtime
// loop, not a compile-time struct.
extern void **AbstractController__vtable;

// MainWindowController::sharedInstance() -- a lazily-constructed singleton
// accessor, not further disassembled (out of this rung's stated scope; see
// NOTES.md). MainWindowController::push(AbstractController*, bool) puts a
// controller onto Nickel's own window stack; NOTES.md has its complete PLT
// stub resolution, including the finding that a controller which is NOT a
// QObject (ours, by design) is a supported, non-crashing input -- a failed
// internal dynamic_cast<QObject*> only skips one optional weak-pointer
// liveness feature, it does not reject the push.
extern void  *(*MainWindowController__sharedInstance)(void);
extern void   (*MainWindowController__push)(MainWindowController *_this, AbstractController *controller, bool animate);

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
