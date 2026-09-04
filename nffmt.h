// PURE display and ordering logic. Deliberately free of libnickel, NickelHook
// and I/O so that it builds and runs on the host, which is the only part of
// this project that can be tested off-device.
#ifndef NFFMT_H
#define NFFMT_H

#include <QString>
#include <QStringList>
#include <QVector>

// One raw directory entry. Shared by nffmt and nflist, which is why it lives
// in the pure header rather than in the browser.
//
// size/mtime come from QFileInfo (nfview.cc's nf_browser_scan_dir), which the
// browser already builds one of per entry to get `name`/`isDir` -- reading
// size()/lastModified() off the SAME QFileInfo costs nothing extra and, per
// spec section 6.2, has to happen for the WHOLE listing up front regardless,
// the same reason percentRead does. Every test fixture in this project's own
// tests leaves both at their default 0 -- harmless under NF_SORT_NAME (the
// default key), which never reads either field.
struct nf_entry {
    QString name;
    bool    isDir;
    qint64  size;   // bytes; 0 if unknown (never read unless key == NF_SORT_SIZE)
    qint64  mtime;  // QDateTime::toMSecsSinceEpoch(); 0 if unknown (ditto, NF_SORT_DATE)

    nf_entry() : isDir(false), size(0), mtime(0) {}
};

// Orders two names the way a reader expects when they contain numbers.
// Returns <0, 0 or >0. See nffmt.cc for why this is hand-written rather than
// QCollator.
int nf_natural_compare(QString const& a, QString const& b);

// The known book extension at the end of `name`, dot included, or an empty
// QString. Longest match first, because ".kepub.epub" also ends in ".epub".
QString nf_book_extension(QString const& name);

// Replaces every entry with the label the panel should show, by stripping the
// text common to all of them. Leaves ALL entries untouched if stripping any of
// them would be unsafe -- see nffmt.cc.
void nf_strip_common(QStringList *names);

// The three sort keys v1 offers, all read straight off QFileInfo -- no new
// libnickel call. NF_SORT_NAME is the only key v1 originally shipped with,
// and stays the default everywhere it matters (see nf_sort_entries' and
// nf_build_listing's own default arguments) so nothing that called either
// function before this feature existed needs to change to keep its old
// behaviour.
enum nf_sort_key {
    NF_SORT_NAME,
    NF_SORT_SIZE,
    NF_SORT_DATE,
};

// Sorts folders before files, then by `key` (nf_natural_compare for
// NF_SORT_NAME, numeric for the other two) within each kind, `descending` or
// not. Stable. Spec sections 3.3 and 6.2 -- 6.2 in particular: grouping
// (folders-before-files) and ordering (the key/direction) are separate
// concerns inside this one function, and `descending` reverses ONLY the
// latter. Folders sort before files in EITHER direction; seeing files above
// folders after tapping the direction toggle is exactly the bug 6.2 calls
// out "reverse the list" for producing.
void nf_sort_entries(QVector<nf_entry> *entries,
                     nf_sort_key key = NF_SORT_NAME, bool descending = false);

// The type filter, section 6.3. NF_FILTER_ALL is the default everywhere, for
// the same "old callers keep old behaviour" reason as nf_sort_entries' key
// default above. NF_FILTER_EPUB matches BOTH .epub and .kepub.epub -- the
// same "extension" nf_book_extension already treats as one unit, not two.
enum nf_filter_kind {
    NF_FILTER_ALL,
    NF_FILTER_CBZ,
    NF_FILTER_CBR,
    NF_FILTER_PDF,
    NF_FILTER_EPUB,
};

// True if FILE `name` matches `filter` (always true for NF_FILTER_ALL).
// Extension-based, like nf_book_extension/nf_is_book_name -- this is a
// SEPARATE layer from that allowlist, not a replacement for it: this
// function only narrows an already-admitted book down further ("which books
// do I want now"), it never has an opinion on whether `name` is a book at
// all ("is this a book"). Spec section 6.3. Meaningless for a directory --
// see nf_build_listing's own comment for why a folder is never even passed
// to this function, let alone filtered by its result.
bool nf_matches_filter(QString const& name, nf_filter_kind filter);

// True for a name v1 will show as a book. Extension allowlist only -- see
// NF_EXTS in nffmt.cc for why ".txt" is not on it.
bool nf_is_book_name(QString const& name);

// True for a directory v1 hides. An extension allowlist does not touch
// directories, so they need their own rule.
bool nf_is_hidden_dir(QString const& name);

#endif
