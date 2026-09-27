// PURE display and ordering logic. Deliberately free of libnickel, NickelHook
// and I/O so that it builds and runs on the host, which is the only part of
// this project that can be tested off-device.
#ifndef NFFMT_H
#define NFFMT_H

#include <QByteArray>
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
    // Nickel's OWN two date sort keys, as RAW ISO-8601 BYTES, exactly as they
    // sit in the library row -- never parsed into a QDateTime here or
    // anywhere else in this project. That is not laziness: Nickel itself
    // never converts them either. Its DateAddedKey<Volume>::key tail-calls
    // Volume::getDateAddedSortKey and compares the results with qstrcmp, and
    // its RecentKey<Volume>::key returns max(___DateLastRead, ___SyncTime)
    // and is compared with strcasecmp (both measured -- the date-getter
    // archaeology, sections 5 and 6). The stored format is fixed-field
    // ISO-8601, so byte order IS chronological order, and Nickel's own
    // sorters rely on exactly that in two independent places. Comparing
    // bytes therefore reproduces Nickel's ordering with no parse, no
    // QDateTime, and -- decisively for this project -- no hidden-return-
    // buffer (sret) libnickel call of the shape that crashed Nickel once
    // already (CLAUDE.md, VolumeManager::getById).
    //
    // EMPTY means "no date known": no library row at all (the
    // `[not in library]` case), an unresolved symbol, or raw bytes that
    // failed nf_date_key_is_plausible below. Empty is NOT a separate
    // ordering bucket -- nf_date_compare substitutes Nickel's own
    // ZERO_DB_DATE_ARRAY sentinel for it, so it sorts exactly where Nickel
    // puts a dateless row. See nf_date_compare for the full reasoning.
    //
    // Filled in by an nf_meta_fn (nfview.cc -> nf_volume_exists), the same
    // way percentRead/readState are, and for the same reason: they are
    // metadata, so the listing pipeline must fetch them BEFORE it sorts
    // (nflist.cc, stage 3 ahead of stage 5).
    QByteArray    dateAdded;    // Volume::getDateAddedSortKey() -- ___SyncTime for sideloaded content
    QByteArray    dateLastRead; // ___DateLastRead, Volume::d() + 40
    // ATTRIBUTE_IMAGE_ID, exactly as it sits in the library row: the name
    // Nickel builds its cover filenames out of (nf_cover_path, above).
    // RAW BYTES, and UTF-8 ones -- Content::getImageId() converts this same
    // field with QString::fromUtf8, so it must be DECODED before it is
    // hashed, never hashed as bytes (nf_bucket_hash's own comment has what
    // that mistake costs).
    //
    // EMPTY means "no cover can be named": no library row at all, an
    // unresolved Content::getImageIdRaw, or a row whose ImageId column is
    // genuinely blank. Every one of those degrades to the row's type icon,
    // silently -- a missing cover is the COMMON case (Nickel renders one
    // only once a book has been seen in its own library views), so it must
    // never read as an error.
    //
    // Filled in by an nf_meta_fn (nfview.cc -> nf_volume_exists), the same
    // way the two date keys above are, off the same Volume, inside the same
    // isValid() branch, and COPIED before ~Volume() runs.
    QByteArray    imageId;

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

// The sort keys. The first three are read straight off QFileInfo -- no
// libnickel call at all. NF_SORT_NAME is the only key v1 originally shipped
// with, and stays the default everywhere it matters (see nf_sort_entries' and
// nf_build_listing's own default arguments) so nothing that called either
// function before this feature existed needs to change to keep its old
// behaviour.
//
// NF_SORT_ADDED/NF_SORT_READ are METADATA keys: they read nf_row::dateAdded
// and ::dateLastRead, which only an nf_meta_fn can fill in, so unlike the
// first three they are meaningless on a bare nf_entry (nf_sort_entries with
// either of them compares two empty keys for every entry and falls through to
// the name tie-break -- documented, not a trap, because the pipeline sorts
// ROWS). Appended AFTER the existing three so the sort row's tap cycle
// (nfview.cc) keeps the order a reader has already learned.
//
// THE TRAP THAT MAKES A DEVICE CHECK VACUOUS, recorded here because this enum
// is what a reader lands on first: BOTH of Nickel's date sorts fall back to
// ___SyncTime for SIDELOADED content, and everything this browser lists is
// sideloaded. Volume::getDateAddedSortKey returns ___SyncTime outright for a
// sideloaded volume, and RecentKey<Volume>::key returns
// max(___DateLastRead, ___SyncTime) -- so for a never-opened sideloaded book
// THE TWO KEYS COINCIDE EXACTLY. A test folder of never-opened books shows
// NF_SORT_ADDED and NF_SORT_READ in identical order and proves nothing at
// all. The control that implies: a meaningful device check needs a folder
// where AT LEAST ONE BOOK HAS ACTUALLY BEEN OPENED, and the new sort must
// produce an order DIFFERENT from NF_SORT_NAME's and must REVERSE under
// `descending`. Output identical to name order is a FAILURE, not a
// coincidence -- an empty or constant key hands every row the same value and,
// under this file's stable insertion sort, silently reproduces the name order
// with no error anywhere.
enum nf_sort_key {
    NF_SORT_NAME,
    NF_SORT_SIZE,
    NF_SORT_DATE,
    NF_SORT_ADDED,
    NF_SORT_READ,
};

// --- the two date keys, as bytes ----------------------------------------
//
// Nickel's own no-date sentinel, ZERO_DB_DATE_ARRAY -- measured, not
// invented: RecentKey<Volume>::key returns constData() of a static
// QByteArray whose GOT relocation resolves to that name, and whose string
// sits in .rodata at 0x136b994 (the date-getter archaeology, A3). It is
// never NULL and never empty, and it sorts before every real date
// lexicographically, i.e. Nickel's "unknown" bucket lands FIRST ascending.
#define NF_ZERO_DB_DATE "0000-00-00T00:00:00.000"

// Orders two raw date keys the way Nickel orders them: CASE-INSENSITIVELY,
// byte-wise, with no parse. Returns <0, 0 or >0.
//
// Case-insensitive because that is what Nickel does -- ReverseSorter<Volume,
// RecentSorter<Volume> > compares two RecentKey results with strcasecmp, not
// strcmp (measured, archaeology section 6). It matters for the 'T'/'t' date/
// time separator and a trailing 'Z', and Nickel reaching for the
// case-insensitive form at all is itself evidence the column is not perfectly
// uniform.
//
// An EMPTY key is substituted with NF_ZERO_DB_DATE rather than being given a
// bucket of its own. This is the whole empty-date rule, in one place:
//
//   * Nickel's own no-date path never yields an empty string -- it yields
//     that sentinel -- so "empty" is a state only THIS mod can be in (a file
//     with no library row at all, an unresolved symbol, or bytes that failed
//     nf_date_key_is_plausible). Mapping it onto the sentinel makes our
//     dateless rows sort exactly where Nickel's dateless rows sort, which is
//     the only definition of "consistent" available here.
//   * It therefore sorts BEFORE every real date ascending, and last
//     descending. A row with no date is the OLDEST thing in the listing, not
//     the newest -- claiming a file with no library row was just added would
//     be an invisible wrong answer, which is what this project's percentRead
//     and read-state guards both already refuse to produce.
//   * A row with no library row must never vanish or crash on this path: it
//     gets a comparable key like every other row, and its `[not in library]`
//     suffix (nf_row_suffix) is what actually tells the reader why.
int nf_date_compare(QByteArray const& a, QByteArray const& b);

// True if `raw` is something this mod is willing to treat as a date key.
// Accepts EMPTY (that is the honest "unknown", see nf_date_compare) or
// anything shaped like ^\d{4}-\d\d-\d\d[Tt ] -- four digits, '-', two
// digits, '-', two digits, then 'T', 't' or a space.
//
// This is the safety net a hardcoded struct offset does not otherwise have.
// ___DateLastRead is read as a QByteArray at Volume::d() + 40 (nfnickel.cc),
// and a renamed SYMBOL fails loudly through dlsym while a MOVED OFFSET does
// not -- it just hands back whatever is sitting at +40 on the new layout. A
// shifted offset lands on a title, an image id or a raw integer, none of
// which passes this test. Same role, and the same refusal to REPAIR a failing
// value, as the 0..100 range check on ___PercentRead (nfnickel.cc has the
// full reasoning): a repaired value hides the layout change the check exists
// to catch behind a plausible-looking answer, so the caller logs once and
// treats the key as unknown instead.
bool nf_date_key_is_plausible(QByteArray const& raw);

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

// --- book covers --------------------------------------------------------
//
// Where Nickel keeps a book's already-rendered cover JPEG, and how to name
// it. This is the whole cover feature except the one libnickel call that
// hands over the ImageId (Content::getImageIdRaw, nfnickel.h) and the
// QFile::exists that decides whether the file is really there (nfview.cc) --
// which is deliberate: everything below is integer arithmetic and string
// building, so it is the part `make test` can actually run, and it is also
// the part where a silent wrong answer is most likely (see nf_bucket_hash's
// own comment for the one that would compile, run and be wrong on every
// row).
//
// Full derivation: .superpowers/sdd/v2-features/cover-path-archaeology.md.
// The measured layout, read out of Image::fileNameForType,
// IOUtil::getImageDataDir and IOUtil::bucketById:
//
//   <dir>/<h & 0xff>/<(h >> 8) & 0xff>/<ImageId> - <TYPE>.parsed
//
// with the two path components in plain decimal and the ImageId inserted
// VERBATIM (Nickel memcpys it, it does not reformat it).

// Measured: the ONLY reference to "../.kobo-images" in the whole firmware is
// IOUtil::getImageDataDir, and it resolves against the device root, so on
// this Libra 2 the directory is this literal. Hardcoded rather than obtained
// by calling getImageDataDir, which costs an sret plus a QDir::cd on
// /mnt/onboard PER ROW and, on the branch where the directory is missing,
// a ScopedFSWrite + QDir::mkpath -- i.e. a WRITE to the owner's card from a
// render loop, which CLAUDE.md's file-handle rule exists to prevent.
// A hardcoded constant fails SAFE: a wrong directory does not exist, so the
// row falls back to its type icon exactly as it does for a book Nickel has
// never rendered a cover for. Wrong on any device where
// Device::supportsHashBucketImages() is false (an older Kobo, which uses an
// unbucketed layout) -- same failure mode, no covers rather than wrong ones.
#define NF_COVER_IMAGE_DIR "/mnt/onboard/.kobo-images"

// N3_LIBRARY_GRID, measured 149x223 by Image::sizeForType with NO device
// predicate at all -- the reason it is the variant used here. N3_LIBRARY_FULL
// is model-dependent (up to 1404x1872, a 2.6 MP JPEG per row) and
// N3_LIBRARY_LIST, though it has a size in the binary, generates ZERO files
// on this firmware.
#define NF_COVER_TYPE "N3_LIBRARY_GRID"
#define NF_COVER_NATIVE_W 149
#define NF_COVER_NATIVE_H 223

// The height the <img> is forced to, and therefore the row's own height
// budget. THIS IS THE OWNER'S NUMBER, not a measurement this project made:
// the brief states the current row height is ~76 px and chose to keep it
// rather than grow rows, because NF_ITEMS_PER_PAGE (nfview.cc) is keyed
// to a measured 17-row panel and every px of row height risks an item off
// the page. The only row measurement on record here is weaker than that --
// 17 rows fitting the 1680 px visible panel with margin reading as room for
// roughly 20 (NOTES.md, Task 10) -- so a first device screenshot must COUNT
// the item rows on a full page. If it shows fewer than NF_ITEMS_PER_PAGE,
// lower this one constant; nothing else needs to change, because the width
// follows from it
// (nf_cover_width_px) and the elision reserve is charged from the same
// number that is emitted (nfview.cc).
// COUPLED TO NF_ITEMS_PER_PAGE (nfview.cc) -- change one and you must
// recompute the other. Both are keyed to the same measured panel geometry:
// 1330 px of content area between the first row and the bottom margin, and
// ~75 px per text row (17 rows total, NOTES.md).
//
// An inline <img> sits on the TEXT BASELINE, so a row holding one is
// `max(ascent, coverHeight) + descent` tall. Measured on this device
// 2026-09-27: ascent 46, descent ~29, so a 70 px cover makes a ~99 px row
// against ~75 px for a text-or-icon row.
//
// Worst case is a page of nothing but covers, against the TWO horizontal
// chrome bars the browser now has (a command bar on top, a page bar pinned to
// the bottom -- it was five stacked full-width rows when this comment was
// first written): 11 * 99 + 2 * 75 = 1239, inside the 1330 available with 91
// px to spare. A page of icon rows is shorter and simply leaves white space,
// which is the deliberate trade -- a page size that varied with how many
// covers happened to be on it would make the row count jump around as you
// page through one folder. NF_ITEMS_PER_PAGE's own comment (nfview.cc) has
// the same arithmetic written from the page-size side.
//
// 70 rather than "as large as fits": at 70 the cover is ~47x70 and legible
// enough to pick a volume by its art, which is the whole point of the
// feature; larger buys little and costs another item off the page.
//
// This number was 76 once, and 76 was NOT a measurement -- it came from
// eyeballing row spacing in a screenshot. It produced rows whose text was
// clipped by the row below. Anything put here needs the arithmetic above
// redone, not an estimate.
#define NF_COVER_H_PX 70

// The four characters Image::cleanId replaces with '_', and ONLY those four:
// '/', ':', '.' and space. Parentheses, hyphens and commas survive, measured
// character for character off cleanId's own four QString::replace calls.
//
// Nickel does NOT run this over the ImageId on the way to a filename --
// cleanId is import-time only (its three call sites are all parsers), and
// fileNameForType inserts the ImageId verbatim -- which is the evidence that
// the ImageId column ALREADY holds the mangled form. So this is not on the
// primary path: nfview.cc builds the path from the raw ImageId first, and
// only consults this if that file does not exist (see there). It is here,
// tested, because that inference is the single assumption the whole feature
// rests on, and because the mangling is what the test vector pins.
QString nf_clean_image_id(QString const& id);

// IOUtil::bucketById's hash, reimplemented. Returns the raw 28-bit value;
// the two path components are its low byte and its second byte.
//
// THIS IS THE QT 4 ELF HASH, NOT Qt 5's qHash. Kobo froze the old algorithm
// in IOUtil so the on-disk bucket layout survives a Qt upgrade, and Qt 5
// replaced qHash(QString) with a seeded Murmur-derived function -- so
// calling qHash here would compile, link, run, and compute a different
// number for every book, giving a directory that never exists. There would
// be no error anywhere: every row would simply fall back to its type icon,
// which is also what a perfectly healthy device does for a book with no
// rendered cover.
//
// It runs over UTF-16 CODE UNITS (the disassembly loads with ldrh and
// QChar::unicode() is the same quantity), which is why this takes a QString
// and not the QByteArray the ImageId arrives as. Hashing the raw UTF-8 BYTES
// instead gives the IDENTICAL answer for every ASCII-only name and a
// different one for anything above U+007F -- so an ASCII-only test suite
// cannot tell the two apart. tests/test_nffmt.cc carries a Cyrillic vector
// for exactly that reason.
unsigned nf_bucket_hash(QString const& imageId);

// The absolute path to the N3_LIBRARY_GRID cover for `imageId`, or an EMPTY
// QString if `imageId` is empty.
//
// The empty case is a refusal, not an oversight: bucketById("") returns
// "0/0" (measured -- its own empty-string branch skips the loop with h = 0),
// so an empty ImageId would otherwise produce a plausible-looking path under
// a real directory, which is the one input that could silently name the
// WRONG file rather than a missing one.
QString nf_cover_path(QString const& imageId);

// The <img> width that keeps a cover at its native 149x223 aspect when
// forced to `heightPx`. Rounded, and floored at 1 so a nonsense height can
// never produce a zero-width box (Qt draws a 0-width <img> as nothing, which
// would read as "no cover" while the row still paid for the height).
int nf_cover_width_px(int heightPx);

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

// --- the page bar's three labels ----------------------------------------
//
// The bottom bar reads "< PREV        page 2/4        NEXT >", and the only
// real DECISION in it is what the two ends say when there is no such page.
// That decision is pure, so it lives here where a host test can run it --
// the bar's LAYOUT is not host-testable at all (it needs Nickel's own
// TouchLabel and a panel to lay out on), which is exactly why the part that
// can be tested is separated from the part that cannot.
//
// BOTH ENDS ARE ALWAYS PRESENT, never omitted: a control that disappears on
// the first and last page makes the bar's own layout jump as a reader pages
// through a folder, and the three slots are fixed-width precisely so it does
// not. So an unavailable end returns a label AND `false` for its active
// flag; nfview.cc renders the inactive form as a plain QLabel rather than a
// TouchLabel, so it is not merely un-wired but not a tap target at all.
//
// HOW "INERT" IS CONVEYED, and why it is not styling: this panel has four
// grey levels and "slightly lighter" does not read as "different" on it --
// the same finding that puts "[not in library]" in a row's TEXT rather than
// leaving it to colour (nf_row_suffix, above). So the inactive form differs
// in the CHARACTERS: the arrow -- the whole affordance -- is gone, the case
// drops to lowercase, and the word "no" says which direction is unavailable.
// Three independent differences, none of which depends on a grey level.
//
// "no prev"/"no next" rather than "first page"/"last page", deliberately:
// a label reading "first page" beside a page counter is exactly what a
// jump-to-the-start control would say, so it would invite the tap it is
// there to refuse. "no prev" cannot be read as a control at all.
//
// `page` is 0-based (nf_browser_page's own convention) and `pageText` is
// 1-based ("page 1/4" for page == 0), because a counter a reader sees is
// 1-based everywhere else in the world. Both arguments are clamped here as
// well as by the caller: nfview.cc must clamp anyway to slice the row
// vector, and a second clamp costs two comparisons and means this function
// has no input that produces a nonsense label. Any output pointer may be
// NULL.
void nf_page_bar_labels(int page, int totalPages,
                        QString *prev, bool *prevActive,
                        QString *pageText,
                        QString *next, bool *nextActive);

#endif
