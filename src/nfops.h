// THE FILE OPERATIONS: delete, move and copy, on the owner's real book
// library, on a device they use daily. The most dangerous code in this
// project, and the only code in it that destroys anything.
//
// WHAT IS AND IS NOT IN HERE. This file holds the canonicalisation, the
// syscalls and the errno reporting -- the part that needs a filesystem and
// therefore cannot be host-tested. Every RULE about whether a path may be
// touched at all lives in nfpath.h, which is pure and is tested to death,
// and every WORD the reader sees lives in nffmt.h, which is the same. There
// is no libnickel here at all: nothing below calls into Nickel, nothing below
// touches a widget, and the one Nickel-shaped concern (the GUI thread must
// keep breathing) is handled by a callback the caller supplies.
//
// THE THREE LIMITS, stated here because they are deliberate and a later pass
// must not quietly "complete" them:
//
//   1. DELETE TAKES FILES AND EMPTY DIRECTORIES ONLY. A recursive delete of a
//      folder of books is catastrophic and unrecoverable on this device --
//      there is no recycle bin, no undo anywhere in this design, and the
//      reference card holds 227 books the owner sideloaded one at a time.
//      A non-empty directory is REFUSED, with a message saying so, and that
//      refusal is the feature.
//   2. COPY TAKES FILES ONLY. A recursive copy is out of scope for this pass:
//      it needs its own progress model (a total that is not known until the
//      tree has been walked), its own partial-state cleanup (a half-copied
//      tree is not one temp file to unlink) and its own answer to what
//      happens when one file inside it fails. A directory is REFUSED.
//   3. MOVE IS rename(2) AND NOTHING ELSE, except on EXDEV. /mnt/onboard is a
//      single mount on this device, so a move is instant and atomic even for
//      the 820 MB .cbr on the reference card -- no bytes move at all. The
//      EXDEV fallback (copy, then delete) exists for a device where it is
//      not, and its delete half runs ONLY after the copy has fully succeeded.
//
// AND THE RULE THAT OUTRANKS ALL THREE: a paste NEVER silently overwrites. If
// the destination name exists, the operation is refused. Overwriting a book
// the owner still has open, or replacing the wrong file because two folders
// happened to hold the same name, is not recoverable either.
#ifndef NFOPS_H
#define NFOPS_H

#include <QString>
#include <QtGlobal> // qint64

// The pure guard layer every function below runs its paths through before it
// touches anything, and the source of nf_path_verdict, which those functions
// hand back so a caller can say WHICH rule refused rather than only that one
// did.
#include "nfpath.h"

// Every way an operation can end. NF_OP_OK is the zero value, and that is
// safe here in a way it would not be for a path verdict (see nf_path_verdict's
// own comment, nfpath.h): nothing branches on a DEFAULT-constructed
// nf_op_result -- every one is the immediate return value of a call, checked
// at the call site, never stored and never zero-initialised.
enum nf_op_result {
    NF_OP_OK,
    NF_OP_REFUSED_PATH,          // a path-safety verdict said no; the verdict is in *why
    NF_OP_REFUSED_DIR_NOT_EMPTY, // delete, limit 1 above
    NF_OP_REFUSED_IS_DIR,        // copy, limit 2 above
    NF_OP_REFUSED_DEST_EXISTS,   // paste would overwrite
    NF_OP_REFUSED_MISSING,       // the source is not there any more
    NF_OP_FAILED,                // the syscall itself failed; errno is in the log
    NF_OP_CANCELLED,             // the reader cancelled a copy in progress
};

// One short phrase per result, for the log line and for the message shown to
// the owner. As with nf_path_verdict_text, present tense and specific: a
// refusal the reader cannot act on is barely better than a silent no-op.
char const *nf_op_result_text(nf_op_result r);

// THE YIELD. Called by nf_op_copy between chunks, never during one, and this
// is the ONLY thing standing between an 820 MB copy and a frozen panel: every
// one of these operations runs on Nickel's GUI thread (CLAUDE.md -- Nickel's
// UI may only be touched from it, and this code is reached from a Qt signal
// handler), so nothing here may run for more than a moment without handing
// the event loop back.
//
// `done`/`total` are bytes, for the progress the caller draws. RETURN FALSE
// TO CANCEL: the copy then stops at a chunk boundary, removes its temp file
// and returns NF_OP_CANCELLED, having left the destination directory exactly
// as it found it.
//
// The callback is what pumps the event loop, so it is also where re-entrancy
// would come from -- a tap delivered inside processEvents() reaching another
// handler while this copy is half done. nfview.cc guards that with a busy
// flag; this file makes no assumption about it beyond calling the callback
// between chunks and honouring its answer.
typedef bool (*nf_op_tick_fn)(void *ctx, qint64 done, qint64 total);

// THE CHUNK SIZE, and the two numbers it is a compromise between:
//
//   HANDLE-OPEN TIME. CLAUDE.md: never hold a file handle on /mnt/onboard for
//   more than a few hundred milliseconds, because a USB session opened while
//   one is open risks corrupting the card. This copy therefore opens, reads
//   or writes ONE chunk, and closes -- every iteration, both files -- so the
//   longest a handle is ever open is the time to move one chunk. At a
//   conservatively slow 8 MB/s for this eMMC, 1 MiB is ~125 ms, inside that
//   budget with room for the filesystem to be having a bad day.
//
//   SYSCALL OVERHEAD. The 820 MB .cbr on the reference card is 820 chunks at
//   this size, i.e. 820 open/seek/read/close plus 820 open/write/close pairs.
//   At even a millisecond each that is under two seconds against the ~100
//   seconds the data itself takes -- noise. Ten times smaller would still be
//   noise; a hundred times smaller (16 KiB, 52,000 chunks) would not, and
//   would also put 52,000 processEvents() calls and 52,000 e-ink-capable
//   repaint opportunities into one copy.
//
// 1 MiB is also a whole number of FAT32 clusters at this card's 32 KiB
// cluster size, so no chunk boundary falls inside a cluster.
#define NF_COPY_CHUNK_BYTES (1024 * 1024)

// Delete ONE path: a file, or an EMPTY directory. `cwd` is the folder the
// browser is showing, and is part of the guard (nfpath.h: a path that is the
// current directory or an ancestor of it is refused -- deleting the ground
// you are standing on).
//
// `why`, if non-NULL, receives the path verdict when the result is
// NF_OP_REFUSED_PATH, so the caller can say WHICH rule refused rather than
// only that one did.
nf_op_result nf_op_delete(QString const& path, QString const& cwd, nf_path_verdict *why);

// Move ONE path into `destDir`, keeping its name. rename(2), which on a
// single mount is atomic and instant whatever the file's size.
//
// Directories ARE allowed here, unlike copy: rename(2) moves a directory
// whole, with no recursion, no progress and no partial state to clean up --
// the three things that put a recursive COPY out of scope do not apply.
//
// On EXDEV, falls back to a chunked copy followed by a delete of the source,
// and the delete runs ONLY if the copy returned NF_OP_OK. `tick` is used for
// that fallback exactly as nf_op_copy uses it, and may be NULL (in which case
// an EXDEV move cannot be cancelled and cannot report progress, which is
// acceptable only because EXDEV cannot happen on this device's single mount).
nf_op_result nf_op_move(QString const& src, QString const& destDir, QString const& cwd,
                        nf_op_tick_fn tick, void *ctx, nf_path_verdict *why);

// Copy ONE FILE into `destDir`, keeping its name. Chunked, yielding between
// chunks, cancellable, and -- the part that matters most -- never leaving a
// partial copy under the final name:
//
//   THE TEMP-FILE AND RENAME SCHEME. Bytes go to nf_temp_name()'s hidden,
//   non-listable name (nfpath.h) IN THE DESTINATION DIRECTORY, and that file
//   is rename(2)d onto the final name only after the last chunk has been
//   written and flushed. The rename is atomic, and it is the same reasoning
//   tools/kobo.py's own push uses for landing a new libnfolders.so: a reader
//   (or Nickel's own importer) sees either no file or the whole file, never a
//   truncated one that looks complete. The temp is IN THE DESTINATION
//   DIRECTORY and not in /tmp precisely so that rename stays a rename rather
//   than becoming a second copy across a mount boundary.
//
//   CLEANUP. The temp is removed on cancel, on any failure, and before the
//   copy starts (a leftover from a run that was killed mid-copy is ours by
//   name, and is logged when it is found).
//
// A directory is REFUSED with NF_OP_REFUSED_IS_DIR -- see limit 2 above.
nf_op_result nf_op_copy(QString const& src, QString const& destDir, QString const& cwd,
                        nf_op_tick_fn tick, void *ctx, nf_path_verdict *why);

#endif
