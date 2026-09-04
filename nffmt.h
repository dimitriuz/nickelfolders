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

// A file's reading state, as far as this mod can measure it. Read state is
// metadata, not a filename property, so unlike the format filters it can only
// be known once a library row has been looked up -- which is why the listing
// pipeline fetches metadata BEFORE it filters or orders (nflist.cc).
//
// Deliberately NOT numerically equal to Kobo's own ReadingStatus (0 = not
// started, 1 = in progress, 2 = finished, measured -- NOTES.md and
// nfnickel.cc). nf_read_state_from_status, below, is the ONE place those three
// numbers appear, so a firmware that renumbers them is a one-function change
// and there is no second, implicit copy of the mapping hiding inside an int
// cast somewhere. NF_READ_UNKNOWN is the zero value on purpose: a zeroed or
// calloc()ed nf_read_state then reads as "we do not know", never as a real
// bucket -- the same reasoning as nf_row::percentRead's -1.
enum nf_read_state {
    NF_READ_UNKNOWN,      // no library row, an unresolved symbol, or a value outside 0..2
    NF_READ_NOT_STARTED,
    NF_READ_IN_PROGRESS,
    NF_READ_FINISHED,
};

// One row the panel will show: an nf_entry that has been through the listing
// pipeline (nflist.h). Lives HERE, next to nf_entry, rather than in nflist.h
// where it started -- nf_matches_read_filter and nf_sort_rows (both below)
// have to see a whole row, and nflist.h includes THIS header, not the other
// way round, so the struct has to be on this side of that edge. Same reason
// nf_entry is here rather than in the browser.
struct nf_row {
    QString       name;         // the on-disk name, never modified
    QString       label;        // what the panel shows
    bool          isDir;
    bool          hasRow;       // a Volume exists for it; always false for a directory
    int           percentRead;  // -1 when unknown, not applicable, or no row
    nf_read_state readState;    // NF_READ_UNKNOWN unless a library row said otherwise
    // DERIVED from readState by nf_build_listing (nflist.cc), never filled in
    // by an nf_meta_fn: two independently-written sources for the same fact
    // are two things that can disagree, and the row renderer (nfview.cc) reads
    // this one. Kept as a field rather than becoming an accessor only because
    // every existing reader spells it `r.finished`.
    bool          finished;
    // Carried forward from the nf_entry this row came from, because ordering
    // now happens AFTER metadata (nflist.cc) and therefore over rows -- an
    // nf_row that lost these two would silently order by nothing at all under
    // NF_SORT_SIZE/NF_SORT_DATE while every name-sorted test kept passing.
    qint64        size;
    qint64        mtime;

    nf_row() : isDir(false), hasRow(false), percentRead(-1),
               readState(NF_READ_UNKNOWN), finished(false), size(0), mtime(0) {}
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

// The SAME grouping and ordering rules, over the nf_row the listing pipeline
// is actually holding by the time it orders: metadata now runs first
// (nflist.cc), so ordering can no longer be done on nf_entry. Shares the one
// comparison function with nf_sort_entries rather than restating the rules --
// see nffmt.cc, where the folders-before-files trap 6.2 names lives in exactly
// one place. nf_sort_entries stays because it is the entry-level primitive
// this one is defined in terms of, and because ten tests pin the ordering
// rules through it; neither is dead code.
void nf_sort_rows(QVector<nf_row> *rows,
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
    // Read state, section 6.3's second axis, in ONE enum and ONE chrome row
    // with the formats above rather than a second filter axis of their own:
    // the panel's measured row budget is 17 rows total and every chrome row
    // costs an item row, so "combine two axes" would buy a combination
    // ("unread epubs") nobody asked for at the price of a row everybody pays
    // for. Appended AFTER the formats so the cycle order a reader has already
    // learned (all -> cbz -> cbr -> pdf -> epub) does not shift under them.
    //
    // These are answered by nf_matches_read_filter, NOT nf_matches_filter: a
    // filename cannot tell you whether a book has been read.
    NF_FILTER_FINISHED,
    NF_FILTER_IN_PROGRESS,
    NF_FILTER_NOT_STARTED,
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

// Kobo's ReadingStatus (measured: 0 = not started, 1 = in progress, 2 =
// finished) mapped onto this file's own enum, with anything else degrading to
// NF_READ_UNKNOWN. Pure, and deliberately NOT inlined into nfnickel.cc next to
// the Content::getReadStatus() call it decodes: every libnickel call site is
// untestable off-device by construction, and this range check is exactly the
// kind of guard that has to be tested -- see nffmt.cc for why an unexpected
// value must not become 0.
nf_read_state nf_read_state_from_status(int status);

// True if `row` matches `filter`'s READ-STATE question (always true for
// NF_FILTER_ALL and for every format filter, which have no opinion about read
// state -- the mirror image of nf_matches_filter admitting every file under a
// read-state filter). Takes the whole ROW, not a name, because none of the
// three questions can be answered from a filename; that also lets this
// function answer for a FOLDER itself instead of leaving that to its caller
// the way the name-only nf_matches_filter has to. Spec section 6.3. See
// nffmt.cc for the two rulings baked in here: a folder is never filtered out,
// and an unknown read state is not "unread".
bool nf_matches_read_filter(nf_row const& row, nf_filter_kind filter);

// True for a name v1 will show as a book. Extension allowlist only -- see
// NF_EXTS in nffmt.cc for why ".txt" is not on it.
bool nf_is_book_name(QString const& name);

// True for a directory v1 hides. An extension allowlist does not touch
// directories, so they need their own rule.
bool nf_is_hidden_dir(QString const& name);

// --- row icons ----------------------------------------------------------
//
// Which leading icon a row gets. PURE, and here rather than in nfview.cc for
// the usual reason: the mapping is a decision about a NAME, so it is the one
// part of the icon work a host test can actually run, and it is the part most
// likely to be got wrong (see NF_EXTS' longest-match-first comment in
// nffmt.cc -- ".kepub.epub" also ends in ".epub"). What stays in nfview.cc is
// only the markup that turns a kind into a Qt resource path, which is
// libnickel/Qt-rendering territory and untestable off-device by construction.
//
// NF_ICON_UNKNOWN is the zero value on purpose, the same reasoning as
// nf_read_state's NF_READ_UNKNOWN and nf_row::percentRead's -1: a zeroed or
// calloc()ed kind then reads as "we do not know", never as a confident
// NF_ICON_FOLDER, which is the one wrong answer that would actively mislead
// (the whole reason this feature exists is a FILE that read as a folder --
// see the task brief).
//
// One kind for .epub and .kepub.epub, and one for .cbz and .cbr, because a
// reader does not distinguish either pair: the first two are the same book
// format with and without Kobo's own preprocessing, and the second two are
// the same comic archive with a different compressor inside.
enum nf_icon_kind {
    NF_ICON_UNKNOWN,  // no known book extension -- unreachable while the
                      // hide-junk allowlist (nf_is_book_name) gates every
                      // file row, and kept anyway: the allowlist and this
                      // map are separate layers, so a future ".txt" on
                      // NF_EXTS must get a badge rather than silently no icon
    NF_ICON_FOLDER,
    NF_ICON_BOOK,     // .epub, .kepub.epub
    NF_ICON_COMIC,    // .cbz, .cbr
    NF_ICON_PDF,
};

// The icon kind for one row. `isDir` WINS over anything the name says: a
// directory called "Comics.cbz" is still a directory, and mistaking one for
// a file is precisely the defect this mapping exists to make impossible to
// miss. Reuses nf_book_extension rather than re-deriving extension parsing,
// same as nf_matches_filter does.
nf_icon_kind nf_icon_kind_for(QString const& name, bool isDir);

// --- the two-form label pieces ------------------------------------------
//
// Every fragment nfview.cc appends to a row label exists in TWO forms: the
// RICH-TEXT markup that actually gets rendered, and a plain-text twin whose
// only job is to be measured (QFontMetrics measures plain text, and rich
// text collapses runs of ordinary whitespace, so neither form can do the
// other's job). The twin is what pays for the fragment out of the row's
// width budget BEFORE the name is elided into what is left -- the suffixes
// are what a row MEANS, so they must never be the part that falls off the
// right edge.
//
// Both forms are built HERE, in one function per fragment, for the reason
// this project keeps repeating: a pair written out twice is a pair that
// gets edited on one side. Being pure, they are also the one part of the
// elision work a host test can run, and the invariant the tests actually
// pin is the one that matters to the measurement -- substituting `&nbsp;`
// in the markup form must yield the plain form EXACTLY.
//
// That invariant is not hypothetical. Device-measured 2026-09-04: rows
// clipped at the right edge (".pd" for ".pdf", "(40%" for "(40%)") with the
// suffix already paid for, and the twins were spelling their separators
// with ASCII spaces (U+0020) while the markup spelled the same separators
// `&nbsp;` (U+00A0) -- so every measurement was short by the difference
// between those two characters, per separator, and nothing could see it
// while both forms were built inline in the row loop.
inline QChar nf_nbsp(void) { return QChar(0x00a0); }

// The DEGRADED icon fallback: what a row shows when its PNG could not be
// written or would not load back (nfview.cc draws and verifies those). All
// five badges are five characters wide, so the labels after them line up.
// The markup form is DERIVED from the plain one by substitution rather than
// written out a second time.
void nf_icon_badge(nf_icon_kind kind, QString *markup, QString *plain);

// The trailing suffix for one row: the folder marker, the "not in library"
// reason, or the reading-progress marker. Exactly one of them, in that
// priority order, or nothing -- see nffmt.cc for what each one means and
// why the order is what it is. Either output pointer may be NULL.
void nf_row_suffix(nf_row const& row, QString *markup, QString *plain);

// What is left of a row's width for the NAME, once the leading icon and the
// trailing suffix have both been paid for. Floored (never below
// NF_NAME_MIN_PX) because QFontMetrics::elidedText at or below the
// ellipsis' own width returns the ellipsis alone: a row degrades to "a stub
// plus its suffix", never to "no name at all".
#define NF_NAME_MIN_PX 60
int nf_name_budget_px(int rowWidth, int iconWidth, int suffixWidth);

#endif
