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
int nf_watch_init(char const *path, void (*cb)(void));

#endif
