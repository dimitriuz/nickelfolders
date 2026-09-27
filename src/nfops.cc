// The file operations themselves -- see nfops.h for the three deliberate
// limits, the never-overwrite rule and the temp-file/rename scheme.
//
// EVERY DESTRUCTIVE CALL IN THIS FILE IS PRECEDED BY THE SAME TWO STEPS, in
// the same order, and no path reaches a syscall without both:
//
//   1. CANONICALISE (QFileInfo::canonicalFilePath) -- resolve every symlink
//      and every "." / ".." for real, against the real filesystem. A string
//      check made before this step is a check on what the path CLAIMS to be.
//   2. JUDGE the canonical form with nfpath.h's pure predicates -- inside
//      /mnt/onboard, not the root itself, not a protected system directory,
//      not the folder being browsed nor an ancestor of it.
//
// Consequence worth naming rather than discovering: a symlink inside
// /mnt/onboard whose target is outside it canonicalises to the OUTSIDE path
// and is therefore refused -- so such a link cannot be deleted through this
// browser at all. That is the safe direction and it is deliberate. (The card
// is VFAT, which has no symlinks, so this is a guard against a future device
// rather than a case the reference card can produce.)
#include "nfops.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStringList>

#include <errno.h>
#include <stdio.h>   // ::rename
#include <string.h>  // strerror
#include <unistd.h>  // ::unlink, ::rmdir

#include <NickelHook.h>

char const *nf_op_result_text(nf_op_result r) {
    switch (r) {
        case NF_OP_OK:                   return "done";
        case NF_OP_REFUSED_PATH:         return "refused by a path rule";
        case NF_OP_REFUSED_DIR_NOT_EMPTY:return "refused: the folder is not empty";
        case NF_OP_REFUSED_IS_DIR:       return "refused: copying a folder is not supported";
        case NF_OP_REFUSED_DEST_EXISTS:  return "refused: a file of that name is already here";
        case NF_OP_REFUSED_MISSING:      return "refused: it is not there any more";
        case NF_OP_FAILED:               return "failed";
        case NF_OP_CANCELLED:            return "cancelled";
    }
    return "refused"; // unreachable under -Wswitch -Werror; a defined answer anyway
}

// --- logging ------------------------------------------------------------
//
// nh_log truncates at 256 bytes SILENTLY (CLAUDE.md), and the names on this
// card run to 230 characters with Cyrillic ones costing two bytes each -- so
// a log line built as "<verb> <path>" loses the verb's own outcome on any
// interesting row, which is precisely the line one would go looking for after
// something went wrong.
//
// So every line below puts THE LOAD-BEARING VALUE FIRST (the outcome, the
// errno, the byte count) and the name LAST, with its length beside it. A
// truncated line still says what happened and how long the name was; what is
// lost is only the tail of a name the reader is looking at on the panel
// anyway. Same rule nfnickel.cc already follows for ContentIDs.
//
// BASENAMES, never whole paths, for the same budget reason -- and because the
// directory is already named once per operation by the caller's own summary
// line rather than once per item.
static QString nf_op_base(QString const& path) {
    return QFileInfo(path).fileName();
}

// --- canonicalisation + the pure guards ---------------------------------

// Resolves `path` for real and judges it as an operation SOURCE. Fills
// `*canon` on success. `cwd` is judged too -- a guard that cannot be
// evaluated must refuse, never wave through (nfpath.h).
static nf_op_result nf_op_resolve_source(QString const& path, QString const& cwd,
                                         QString *canon, nf_path_verdict *why) {
    if (why)
        *why = NF_PATH_OK;

    QFileInfo fi(path);
    if (!fi.exists()) {
        // Distinguished from a path refusal on purpose: "it is not there any
        // more" is a race with a USB session or another mod, not a rule.
        nh_log("fileops: refused MISSING source -- '%s' (%d chars)",
               qPrintable(nf_op_base(path)), path.length());
        return NF_OP_REFUSED_MISSING;
    }

    QString c = fi.canonicalFilePath();
    if (c.isEmpty()) {
        // canonicalFilePath returns empty for a dangling link or a path that
        // vanished between exists() and here. Refused by name rather than
        // being allowed to sail through a startsWith() test as "not inside
        // the root" (nfpath.h's own note on why nf_path_is_clean exists).
        nh_log("fileops: refused UNRESOLVABLE source (canonicalFilePath empty) -- '%s' (%d chars)",
               qPrintable(nf_op_base(path)), path.length());
        if (why)
            *why = NF_PATH_UNCLEAN;
        return NF_OP_REFUSED_PATH;
    }

    // The browsing directory, canonicalised the same way, so the
    // cwd-or-ancestor rule compares like with like. An unresolvable cwd
    // refuses everything, which is the correct direction: it means this mod
    // does not know where it is.
    QString cwdCanon = QFileInfo(cwd).canonicalFilePath();
    if (cwdCanon.isEmpty())
        cwdCanon = cwd; // let nf_path_check_source judge it as UNCLEAN if it is

    nf_path_verdict v = nf_path_check_source(c, cwdCanon);
    if (v != NF_PATH_OK) {
        nh_log("fileops: refused source (%s) -- '%s' (%d chars)",
               nf_path_verdict_text(v), qPrintable(nf_op_base(c)), c.length());
        if (why)
            *why = v;
        return NF_OP_REFUSED_PATH;
    }

    // DEFENCE IN DEPTH, and it costs two string comparisons: the path as the
    // caller spelled it must ALSO be in-tree and unprotected, not only its
    // canonical form. The two can differ if something above this call built a
    // path out of a name it never checked, and this is the cheapest place to
    // notice.
    if (!nf_path_is_inside(path, QStringLiteral(NF_PATH_ROOT)) || nf_path_is_protected(path)) {
        nh_log("fileops: refused source -- the path AS GIVEN is outside %s or protected, though its canonical form was not: '%s' (%d chars)",
               NF_PATH_ROOT, qPrintable(nf_op_base(path)), path.length());
        if (why)
            *why = NF_PATH_OUTSIDE_ROOT;
        return NF_OP_REFUSED_PATH;
    }

    *canon = c;
    return NF_OP_OK;
}

// Resolves the destination DIRECTORY and builds the final path inside it.
// `destDir` must already exist (it is the folder the reader is looking at);
// the final path must not (that is checked by the caller, which is also what
// makes "never silently overwrite" one rule in one place).
static nf_op_result nf_op_resolve_dest(QString const& destDir, QString const& srcCanon,
                                       QString *destPath, QString *name, nf_path_verdict *why) {
    if (why)
        *why = NF_PATH_OK;

    QString n = nf_op_base(srcCanon);
    if (!nf_path_name_is_safe(n)) {
        // A name with a '/' in it is the one way a destination can escape
        // with no ".." anywhere -- destDir + "/" + name would land two levels
        // down. Refused before it is ever joined.
        nh_log("fileops: refused UNSAFE name for the destination -- (%d chars)", n.length());
        if (why)
            *why = NF_PATH_UNCLEAN;
        return NF_OP_REFUSED_PATH;
    }

    QString dirCanon = QFileInfo(destDir).canonicalFilePath();
    if (dirCanon.isEmpty()) {
        nh_log("fileops: refused UNRESOLVABLE destination folder (canonicalFilePath empty) -- '%s' (%d chars)",
               qPrintable(nf_op_base(destDir)), destDir.length());
        if (why)
            *why = NF_PATH_UNCLEAN;
        return NF_OP_REFUSED_PATH;
    }

    // The destination FOLDER's own rules -- nf_path_check_dest_dir, in the
    // pure layer, so this is a tested predicate rather than an ad-hoc chain of
    // clauses in the one file no host test can reach. It used to be exactly
    // that chain, re-deriving four of nf_path_check_source's rules by hand;
    // an inverted clause in it would have failed no test.
    nf_path_verdict dv = nf_path_check_dest_dir(dirCanon);
    if (dv != NF_PATH_OK) {
        nh_log("fileops: refused destination FOLDER (%s) -- '%s' (%d chars)",
               nf_path_verdict_text(dv), qPrintable(nf_op_base(dirCanon)), dirCanon.length());
        if (why)
            *why = dv;
        return NF_OP_REFUSED_PATH;
    }

    QString d = dirCanon + QLatin1Char('/') + n;
    nf_path_verdict v = nf_path_check_dest(d, srcCanon);
    if (v != NF_PATH_OK) {
        nh_log("fileops: refused destination (%s) -- '%s' (%d chars)",
               nf_path_verdict_text(v), qPrintable(n), n.length());
        if (why)
            *why = v;
        return NF_OP_REFUSED_PATH;
    }

    *destPath = d;
    *name     = n;
    return NF_OP_OK;
}

// --- delete -------------------------------------------------------------

nf_op_result nf_op_delete(QString const& path, QString const& cwd, nf_path_verdict *why) {
    QString canon;
    nf_op_result r = nf_op_resolve_source(path, cwd, &canon, why);
    if (r != NF_OP_OK)
        return r;

    QByteArray raw = QFile::encodeName(canon);
    QFileInfo fi(canon);

    if (fi.isDir()) {
        // EMPTY DIRECTORIES ONLY. This is nfops.h's limit 1, and the refusal
        // is the feature: a recursive delete of a folder of books is
        // unrecoverable on this device and there is no undo anywhere in this
        // design. Do not "complete" this later without deciding, on purpose
        // and in writing, that an unrecoverable recursive delete is something
        // the owner wants one tap away on a panel they operate with a thumb.
        //
        // Hidden entries are counted too (QDir::Hidden): an .sdr sidecar or a
        // dotfile makes a directory non-empty even though this browser's own
        // listing hides it, and rmdir(2) would fail on it anyway -- refusing
        // here means the reason is a sentence rather than an errno.
        QDir d(canon);
        QStringList left = d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                       QDir::NoSort);
        if (!left.isEmpty()) {
            nh_log("fileops: refused DELETE, folder holds %d entr(y/ies) -- '%s' (%d chars)",
                   left.size(), qPrintable(nf_op_base(canon)), canon.length());
            return NF_OP_REFUSED_DIR_NOT_EMPTY;
        }
        if (::rmdir(raw.constData()) != 0) {
            nh_log("fileops: DELETE folder FAILED errno=%d (%s) -- '%s' (%d chars)",
                   errno, strerror(errno), qPrintable(nf_op_base(canon)), canon.length());
            return NF_OP_FAILED;
        }
        nh_log("fileops: DELETE folder OK -- '%s' (%d chars)",
               qPrintable(nf_op_base(canon)), canon.length());
        return NF_OP_OK;
    }

    qint64 size = fi.size();
    if (::unlink(raw.constData()) != 0) {
        nh_log("fileops: DELETE file FAILED errno=%d (%s) -- '%s' (%d chars)",
               errno, strerror(errno), qPrintable(nf_op_base(canon)), canon.length());
        return NF_OP_FAILED;
    }
    nh_log("fileops: DELETE file OK, %lld bytes -- '%s' (%d chars)",
           (long long)size, qPrintable(nf_op_base(canon)), canon.length());
    return NF_OP_OK;
}

// --- the chunked copy ---------------------------------------------------

// Writes `srcCanon` to `tempPath` in NF_COPY_CHUNK_BYTES pieces, closing and
// reopening BOTH files between pieces and calling `tick` after each one. See
// nfops.h for the chunk size's own derivation and for why the handles are
// reopened rather than held.
//
// THE LOOP'S ADVANCEMENT IS THE THING TO READ. `done` moves forward by
// exactly the number of bytes written, every iteration, and every path that
// cannot write bytes BREAKS. There is no `continue` in it at all: a `continue`
// in a loop whose increment is the last body statement hung Nickel's GUI
// thread once in this project -- no crash, PID unchanged, the device needed a
// power cycle -- and a copy loop is exactly where that would happen again. A
// read that returns zero bytes while `done < total` is therefore a FAILURE,
// not a retry: it is the one shape that would otherwise spin forever.
static nf_op_result nf_op_copy_chunks(QString const& srcCanon, QString const& tempPath,
                                      qint64 total, nf_op_tick_fn tick, void *ctx) {
    // Create (or truncate) the temp file once, up front, so that a zero-byte
    // source -- a real thing to find on a card, and one the reference device
    // already has (the truncated 8 MiB copy NOTES.md records is the near
    // miss) -- still produces a file to rename into place rather than an
    // ENOENT at the end of a copy that "succeeded".
    {
        QFile out(tempPath);
        if (!out.open(QIODevice::WriteOnly)) {
            nh_log("fileops: COPY could not create the temp file, QFile error=%d -- '%s' (%d chars)",
                   (int)out.error(), qPrintable(nf_op_base(tempPath)), tempPath.length());
            return NF_OP_FAILED;
        }
        out.close();
    }

    qint64 done = 0;
    while (done < total) {
        QByteArray buf;
        {
            QFile in(srcCanon);
            if (!in.open(QIODevice::ReadOnly)) {
                nh_log("fileops: COPY read-open FAILED at %lld/%lld, QFile error=%d -- '%s' (%d chars)",
                       (long long)done, (long long)total, (int)in.error(),
                       qPrintable(nf_op_base(srcCanon)), srcCanon.length());
                return NF_OP_FAILED;
            }
            if (!in.seek(done)) {
                nh_log("fileops: COPY seek to %lld FAILED, QFile error=%d -- '%s' (%d chars)",
                       (long long)done, (int)in.error(),
                       qPrintable(nf_op_base(srcCanon)), srcCanon.length());
                in.close();
                return NF_OP_FAILED;
            }
            buf = in.read(NF_COPY_CHUNK_BYTES);
            in.close(); // handle time is one chunk, never the whole file
        }
        if (buf.isEmpty()) {
            // Short of `total` with nothing left to read: the file shrank
            // under us, or the read failed. Either way `done` cannot advance,
            // so this MUST return rather than loop -- see the comment above.
            nh_log("fileops: COPY read returned 0 bytes at %lld/%lld -- the source shrank or the read failed; aborting -- '%s' (%d chars)",
                   (long long)done, (long long)total,
                   qPrintable(nf_op_base(srcCanon)), srcCanon.length());
            return NF_OP_FAILED;
        }

        {
            // Append, not WriteOnly alone: QFile's WriteOnly implies Truncate,
            // which would throw away every chunk before this one.
            QFile out(tempPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Append)) {
                nh_log("fileops: COPY write-open FAILED at %lld/%lld, QFile error=%d -- '%s' (%d chars)",
                       (long long)done, (long long)total, (int)out.error(),
                       qPrintable(nf_op_base(tempPath)), tempPath.length());
                return NF_OP_FAILED;
            }
            // Cheap consistency check, and the reason it is worth the syscall:
            // `done` is this function's idea of how much is already on disk,
            // and the temp file is the only place that can disagree. If
            // anything else has touched it the copy would silently interleave
            // into a corrupt file that then gets renamed over a real name.
            if (out.size() != done) {
                nh_log("fileops: COPY ABORTED -- the temp file is %lld bytes where %lld were written; something else is writing it -- '%s' (%d chars)",
                       (long long)out.size(), (long long)done,
                       qPrintable(nf_op_base(tempPath)), tempPath.length());
                out.close();
                return NF_OP_FAILED;
            }
            qint64 wrote = out.write(buf);
            bool flushed = out.flush();
            out.close();
            if (wrote != (qint64)buf.size() || !flushed) {
                nh_log("fileops: COPY write FAILED at %lld/%lld (wrote %lld of %d, flush %s) -- '%s' (%d chars)",
                       (long long)done, (long long)total, (long long)wrote, buf.size(),
                       flushed ? "ok" : "failed",
                       qPrintable(nf_op_base(tempPath)), tempPath.length());
                return NF_OP_FAILED;
            }
            done += wrote; // ALWAYS advances: wrote == buf.size() and buf is non-empty
        }

        // THE YIELD, between chunks and never during one. Both handles are
        // closed by the time this runs, which is the whole point of reopening
        // them per chunk: whatever the event loop does in here -- including a
        // USB session being plugged in -- this mod is holding nothing open on
        // /mnt/onboard while it does it.
        if (tick && !tick(ctx, done, total)) {
            nh_log("fileops: COPY CANCELLED by the reader at %lld/%lld bytes -- '%s' (%d chars)",
                   (long long)done, (long long)total,
                   qPrintable(nf_op_base(srcCanon)), srcCanon.length());
            return NF_OP_CANCELLED;
        }
    }
    return NF_OP_OK;
}

// Removes a temp file, best-effort, and says so. Never fatal: the copy has
// already succeeded or failed by the time this runs, and a temp that could
// not be removed is a hidden, unlistable file the owner can delete over ssh,
// not a reason to report a different outcome.
static void nf_op_drop_temp(QString const& tempPath, char const *when) {
    if (tempPath.isEmpty() || !QFile::exists(tempPath))
        return;
    QByteArray raw = QFile::encodeName(tempPath);
    if (::unlink(raw.constData()) != 0)
        nh_log("fileops: could not remove the temp file %s, errno=%d (%s) -- '%s' (%d chars)",
               when, errno, strerror(errno), qPrintable(nf_op_base(tempPath)), tempPath.length());
    else
        nh_log("fileops: temp file removed %s -- '%s' (%d chars)",
               when, qPrintable(nf_op_base(tempPath)), tempPath.length());
}

nf_op_result nf_op_copy(QString const& src, QString const& destDir, QString const& cwd,
                        nf_op_tick_fn tick, void *ctx, nf_path_verdict *why) {
    QString srcCanon;
    nf_op_result r = nf_op_resolve_source(src, cwd, &srcCanon, why);
    if (r != NF_OP_OK)
        return r;

    if (QFileInfo(srcCanon).isDir()) {
        // nfops.h's limit 2. A recursive copy needs its own progress model,
        // its own partial-state cleanup and its own answer to a failure
        // half-way through a tree; none of those is built, so this refuses
        // rather than doing something that looks like a copy.
        nh_log("fileops: refused COPY of a FOLDER (recursive copy is out of scope) -- '%s' (%d chars)",
               qPrintable(nf_op_base(srcCanon)), srcCanon.length());
        return NF_OP_REFUSED_IS_DIR;
    }

    QString destPath, name;
    r = nf_op_resolve_dest(destDir, srcCanon, &destPath, &name, why);
    if (r != NF_OP_OK)
        return r;

    // NEVER SILENTLY OVERWRITE -- nfops.h's rule that outranks the three
    // limits. Checked before any byte is written, and again immediately
    // before the final rename, because rename(2) itself would replace the
    // destination without a word.
    if (QFile::exists(destPath)) {
        nh_log("fileops: refused COPY, the name is already here -- '%s' (%d chars)",
               qPrintable(name), name.length());
        return NF_OP_REFUSED_DEST_EXISTS;
    }

    QString tempName = nf_temp_name(name);
    if (tempName.isEmpty()) {
        nh_log("fileops: refused COPY, could not build a temp name -- '%s' (%d chars)",
               qPrintable(name), name.length());
        return NF_OP_FAILED;
    }
    QString tempPath = QFileInfo(destPath).path() + QLatin1Char('/') + tempName;

    // A leftover from a run that was killed mid-copy is OURS BY NAME, so it
    // is removed rather than appended to. Logged, because its existence says
    // a previous copy did not finish and that is worth knowing.
    if (QFile::exists(tempPath))
        nf_op_drop_temp(tempPath, "left over from an earlier run");

    qint64 total = QFileInfo(srcCanon).size();
    nh_log("fileops: COPY start, %lld bytes in %d-byte chunks -- '%s' (%d chars)",
           (long long)total, NF_COPY_CHUNK_BYTES, qPrintable(name), name.length());

    r = nf_op_copy_chunks(srcCanon, tempPath, total, tick, ctx);
    if (r != NF_OP_OK) {
        nf_op_drop_temp(tempPath, r == NF_OP_CANCELLED ? "after a cancel" : "after a failure");
        return r;
    }

    // THE SOURCE MUST STILL BE THE SIZE WE COPIED. `total` was sampled once,
    // before a copy that for the 820 MB .cbr runs for a minute and a half
    // with a yield to the event loop in every chunk -- so the source really
    // can change underneath it (a USB session, another mod, a download
    // finishing).
    //
    // The SHRINK case is already caught inside the loop: a read that returns
    // zero bytes while `done < total` aborts, which is also what stops the
    // loop spinning. The GROW case has no such symptom -- the loop simply
    // stops at the stale `total`, every chunk succeeded, and without this
    // check a TRUNCATED file would be renamed onto the final name and logged
    // as "COPY OK". That is precisely what the temp-and-rename scheme exists
    // to prevent, so it is checked here rather than trusted.
    //
    // Refused rather than repaired: copying the extra tail would race the
    // same writer again, and a file still being written is not a file to
    // duplicate. The temp is dropped and the source is untouched.
    qint64 nowSize = QFileInfo(srcCanon).size();
    if (nowSize != total) {
        nh_log("fileops: COPY ABORTED at the last step -- the source is now %lld bytes where %lld were copied; it changed while the copy ran, so the partial copy is discarded -- '%s' (%d chars)",
               (long long)nowSize, (long long)total, qPrintable(name), name.length());
        nf_op_drop_temp(tempPath, "after the source changed size");
        return NF_OP_FAILED;
    }

    // The second existence check. The window between the first one and here
    // is as long as the copy took, which for the 820 MB .cbr is a minute and
    // a half of event loop -- plenty of time for a USB session to have landed
    // a file of the same name. rename(2) would overwrite it silently.
    if (QFile::exists(destPath)) {
        nh_log("fileops: COPY ABORTED at the last step -- '%s' appeared while the copy ran; the copy is discarded rather than overwriting it (%d chars)",
               qPrintable(name), name.length());
        nf_op_drop_temp(tempPath, "after the destination appeared");
        return NF_OP_REFUSED_DEST_EXISTS;
    }

    QByteArray rawTemp = QFile::encodeName(tempPath);
    QByteArray rawDest = QFile::encodeName(destPath);
    if (::rename(rawTemp.constData(), rawDest.constData()) != 0) {
        nh_log("fileops: COPY rename-into-place FAILED errno=%d (%s) -- '%s' (%d chars)",
               errno, strerror(errno), qPrintable(name), name.length());
        nf_op_drop_temp(tempPath, "after a failed rename");
        return NF_OP_FAILED;
    }

    nh_log("fileops: COPY OK, %lld bytes -- '%s' (%d chars)",
           (long long)total, qPrintable(name), name.length());
    return NF_OP_OK;
}

// --- move ---------------------------------------------------------------

nf_op_result nf_op_move(QString const& src, QString const& destDir, QString const& cwd,
                        nf_op_tick_fn tick, void *ctx, nf_path_verdict *why) {
    QString srcCanon;
    nf_op_result r = nf_op_resolve_source(src, cwd, &srcCanon, why);
    if (r != NF_OP_OK)
        return r;

    QString destPath, name;
    r = nf_op_resolve_dest(destDir, srcCanon, &destPath, &name, why);
    if (r != NF_OP_OK)
        return r;

    // NEVER SILENTLY OVERWRITE. This check is load-bearing in a way it is not
    // for the copy: rename(2) REPLACES an existing destination file without
    // an error, so without this a paste over a same-named book would look
    // like a clean success and the old book would be gone.
    //
    // There is a window between this check and the rename. It cannot be
    // closed on this device: renameat2(RENAME_NOREPLACE) needs a kernel this
    // firmware does not have, and link(2)+unlink(2) -- which WOULD be atomic,
    // failing with EEXIST -- does not work on VFAT, which is what
    // /mnt/onboard is. The window is microseconds on a single-threaded GUI
    // with no other writer; it is named here so the next person does not have
    // to rediscover why it is not closed.
    if (QFile::exists(destPath)) {
        nh_log("fileops: refused MOVE, the name is already here -- '%s' (%d chars)",
               qPrintable(name), name.length());
        return NF_OP_REFUSED_DEST_EXISTS;
    }

    QByteArray rawSrc  = QFile::encodeName(srcCanon);
    QByteArray rawDest = QFile::encodeName(destPath);
    bool isDir = QFileInfo(srcCanon).isDir();

    if (::rename(rawSrc.constData(), rawDest.constData()) == 0) {
        // The whole reason cut+paste is rename(2): /mnt/onboard is ONE mount
        // on this device, so this moved the 820 MB .cbr without copying a
        // byte, atomically, in the time one directory entry takes to rewrite.
        nh_log("fileops: MOVE OK (rename, no bytes copied)%s -- '%s' (%d chars)",
               isDir ? " [folder]" : "", qPrintable(name), name.length());
        return NF_OP_OK;
    }

    int err = errno;
    if (err != EXDEV) {
        nh_log("fileops: MOVE FAILED errno=%d (%s)%s -- '%s' (%d chars)",
               err, strerror(err), isDir ? " [folder]" : "", qPrintable(name), name.length());
        return NF_OP_FAILED;
    }

    // EXDEV ONLY: the two paths are on different filesystems, so rename(2)
    // cannot do it. Cannot happen on this device (single mount) and is here
    // for a model with an SD card -- see CLAUDE.md's own note that the empty
    // dbName which is correct for internal storage is NOT correct there.
    if (isDir) {
        // A cross-filesystem directory move is a recursive copy, which is out
        // of scope (nfops.h, limit 2). Refused rather than half-done.
        nh_log("fileops: MOVE of a FOLDER across filesystems (EXDEV) refused -- recursive copy is out of scope -- '%s' (%d chars)",
               qPrintable(name), name.length());
        return NF_OP_REFUSED_IS_DIR;
    }

    nh_log("fileops: MOVE hit EXDEV -- falling back to copy-then-delete for '%s' (%d chars)",
           qPrintable(name), name.length());
    nf_op_result c = nf_op_copy(src, destDir, cwd, tick, ctx, why);
    if (c != NF_OP_OK) {
        // THE DELETE HALF DOES NOT RUN. This is the only ordering that
        // matters in the fallback: a copy that was cancelled, refused or
        // failed leaves the source exactly where it was, and the reader has
        // lost nothing.
        nh_log("fileops: MOVE fallback stopped at the copy (%s) -- the source is untouched -- '%s' (%d chars)",
               nf_op_result_text(c), qPrintable(name), name.length());
        return c;
    }

    nf_op_result d = nf_op_delete(src, cwd, why);
    if (d != NF_OP_OK) {
        // The copy succeeded and the source could not be removed, so the file
        // now exists twice. Reported as a failure and said plainly, because
        // the alternative -- reporting OK -- would have the reader believe a
        // move happened when what happened was a duplicate.
        nh_log("fileops: MOVE fallback copied but could NOT delete the source (%s) -- the file now exists in BOTH folders -- '%s' (%d chars)",
               nf_op_result_text(d), qPrintable(name), name.length());
        return NF_OP_FAILED;
    }

    nh_log("fileops: MOVE OK via copy-then-delete (EXDEV) -- '%s' (%d chars)",
           qPrintable(name), name.length());
    return NF_OP_OK;
}
