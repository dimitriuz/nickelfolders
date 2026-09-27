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

// --- the view flags -----------------------------------------------------
//
// What the browser SHOWS, as opposed to what it lists. Five independent
// toggles the reader flips in the `view:` submenu (nf_menu_kind's
// NF_MENU_VIEW, below), threaded through the pure layer so that every
// decision each one makes -- the label, the truncation, the extension, the
// size text, the page size -- is host-testable, and only the rendering is
// left on the untestable side.
//
// THEY ARE DISPLAY CONCERNS AND MUST NOT CHANGE WHICH ROWS EXIST, with ONE
// deliberate exception: `showHidden` is a genuine filter, because there is no
// way to "display" an entry the listing stage dropped. Everything else
// changes only how a surviving row is drawn or how many of them fit on a
// page. tests/test_nflist.cc pins that separation directly -- flipping the
// four display flags must leave the row SET identical, name for name.
//
// EVERY FLAG'S `false` IS TODAY'S BEHAVIOUR, and that polarity is
// load-bearing rather than cosmetic. This struct is POD with no constructor
// precisely so a copy of it can sit at file scope in nfview.cc without a
// dynamic initialiser -- the construct that once boot-looped this mod into
// NickelHook's SHARED failsafe (CLAUDE.md) -- which means a .bss-zeroed copy
// is a real possibility to design for rather than to rule out. All-false
// being the default makes a zeroed struct read as "the browser as it has
// always behaved", never as a mode nobody asked for. The same reasoning as
// NF_READ_UNKNOWN, NF_ICON_UNKNOWN and NF_MENU_NONE all being their enum's
// zero value; test_zeroed_view_flags_are_the_defaults pins it.
//
// Hence the names: `hideExtensions`/`hideCovers` rather than
// `showExtensions`/`showCovers`, which would have read better in isolation
// and would have made zero mean "no extensions, no covers".
struct nf_view_flags {
    bool fullNames;      // false: labels are truncated (nf_strip_common runs). true: the name as it is on disk
    bool hideExtensions; // false: a known book extension stays on the label
    bool hideCovers;     // false: a row shows the book's cover where Nickel has rendered one
    bool showHidden;     // false: nf_is_hidden_dir applies. THE ONE FLAG THAT FILTERS
    bool showSize;       // false: no size on a file row
};

// The defaults, as a CONSTANT INITIALISER. A macro rather than a function
// because nfview.cc's file-scope copy of this must be initialised without
// running any code at all -- a function call there would be a dynamic
// initialiser, i.e. exactly the `_GLOBAL__sub_I` entry CLAUDE.md forbids and
// `nm libnfolders.so | grep GLOBAL__sub_I` checks for. Every field is spelled
// out, in order, because GCC 4.9 rejects a designated initializer that SKIPS
// one (CLAUDE.md) and because a short aggregate would silently acquire a new
// field's value from nowhere if this struct ever grows.
#define NF_VIEW_FLAGS_DEFAULT { false, false, false, false, false }

// The same defaults as a VALUE, for default arguments and for callers that
// want to say what they mean. Defined in terms of the macro above, so there
// is one place the defaults are spelled.
nf_view_flags nf_view_flags_default(void);

// One toggle per row of the view submenu, in the order the menu lists them
// (NF_MENU_VIEW_ROWS, nffmt.cc). Unlike nf_sort_key/nf_filter_kind there is
// no "none" value and no zero-value trap to design around: a toggle is only
// ever produced by nf_menu_view_toggle_at, which refuses an out-of-range
// index rather than handing back a default.
enum nf_view_toggle {
    NF_VIEW_FILENAMES,   // truncated / full
    NF_VIEW_EXTENSIONS,  // shown / hidden
    NF_VIEW_COVERS,      // on / off
    NF_VIEW_HIDDEN,      // hidden / shown  -- the one that filters
    NF_VIEW_SIZE,        // hidden / shown
};

// THE ONE PLACE a toggle maps onto a field of nf_view_flags, handed back as a
// pointer so that flipping one is `*flag = !*flag` at the call site rather
// than a second switch over the same enum living in nfview.cc. Returns NULL
// for a toggle this build does not know, so a nonsense value can never flip
// the WRONG flag -- the same refuse-rather-than-guess rule as
// nf_menu_sort_key_at's.
bool *nf_view_flag(nf_view_flags *view, nf_view_toggle toggle);

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
//
// This function is unchanged by the `hidden files` view toggle and knows
// nothing about it: the toggle decides whether nf_build_listing CONSULTS this
// rule at all (nflist.cc, stage 1), not what the rule says. One rule, one
// place, and "shown" is then exactly "the rule was not applied" rather than a
// second, looser copy of it that could drift.
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
// COUPLED TO nf_items_per_page (below) -- which now COMPUTES the page size
// from this constant rather than restating it, so changing this one number
// carries the page size with it. The two used to live in different files
// (this, and an NF_ITEMS_PER_PAGE macro in nfview.cc) with each comment
// pointing at the other and the arithmetic written out twice; the `covers`
// toggle needed a per-mode page size anyway, and moving the quotient onto
// this side of the boundary made it host-testable at the same time. What is
// still NOT automatic is the ARITHMETIC IN THE COMMENTS: redo it there.
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

// --- the page size, per MODE --------------------------------------------
//
// Every term below is DEVICE-MEASURED on this panel (Kobo Libra 2, firmware
// 4.38.23684) and none of it is estimated, because every layout number this
// project ever guessed turned out wrong -- NF_COVER_H_PX was 76 by eyeball
// and clipped its own rows, and the page size was 14 and then 12 against a
// row count read off a screenshot's margin rather than counted.
#define NF_CONTENT_AREA_PX 1330  // between the first row and the bottom margin
                                 // (1680 visible panel px less Nickel's own
                                 // chrome -- NOTES.md)
#define NF_FONT_ASCENT_PX  46    // the row font's ascent, measured 2026-09-27
#define NF_FONT_DESCENT_PX 29    // ...and its descent
#define NF_TEXT_ROW_PX     75    // a text-or-icon row. Measured, and equal to
                                 // ascent + descent -- a test pins that
                                 // identity, so a re-measurement that broke it
                                 // would be visible rather than absorbed
#define NF_CHROME_BAR_PX   75    // one horizontal chrome bar, same height as a
                                 // text row
#define NF_CHROME_BARS     2     // the ORDINARY number of chrome bar ROWS: a
                                 // one-row command bar on top and the page bar
                                 // pinned to the bottom. Both UNCONDITIONAL --
                                 // see the page bar's own comment (nfview.cc)
                                 // for why its ends stay present-but-inert
                                 // rather than disappearing.
                                 //
                                 // NOT a ceiling any more, which is why
                                 // nf_items_per_page takes the count as an
                                 // ARGUMENT rather than reading this: the
                                 // command bar wraps to a SECOND row when its
                                 // items do not fit side by side
                                 // (nf_bar_plan_layout, below), and that page
                                 // costs one item row. This is the value the
                                 // unwrapped case passes.

// How many item rows fit on one page, for the mode `covers` selects and for a
// chrome that occupies `chromeBarRows` bar rows (NF_CHROME_BARS ordinarily;
// three when the command bar has wrapped -- nf_bar_plan_layout below).
//
// THE PAGE SIZE DEPENDS ON THE MODE, NOT ON THE PAGE. That distinction is the
// whole design and it is easy to collapse by accident later, so: a page of
// cover rows is taller than a page of icon rows, so the two MODES get
// different budgets -- but within one mode every page gets the same number,
// even a page that happens to hold no covers at all. A page size that varied
// with how many covers HAPPENED to land on a given page was considered and
// deliberately rejected: the row count would jump around as a reader pages
// through a single folder, which is worse than the white space an icon-heavy
// page leaves under the covers-on budget.
//
// THE ARITHMETIC, both modes, from the constants above:
//
//   covers ON.  An inline <img> sits on the TEXT BASELINE, so a row carrying
//   one is max(ascent, coverHeight) + descent tall: max(46, 70) + 29 = 99.
//   The worst case is a page of nothing but covers, because that is the
//   tallest a page of N items can be.
//
//       N * 99 + 2 * 75 <= 1330  ->  N <= (1330 - 150) / 99 = 11.92  ->  11
//       11 * 99 + 150 = 1239, 91 px to spare (less than one row of either
//       height, so 11 is the real ceiling and not a conservative pick).
//       12 would need 1338 and overflow by 8.
//
//   covers OFF. Every row is a 75 px text-or-icon row, so the worst case and
//   the ordinary case are the same page.
//
//       N * 75 + 2 * 75 <= 1330  ->  N <= (1330 - 150) / 75 = 15.73  ->  15
//       15 * 75 + 150 = 1275, 55 px to spare.
//       16 would need 1350 and overflow by 20.
//
// So turning covers off buys FOUR more items per page, not the one or two a
// glance at the numbers suggests -- which is most of the reason the toggle is
// worth having at all.
//
// AND THE SAME TWO, FOR A WRAPPED COMMAND BAR (chromeBarRows = 3, i.e. 225 px
// of chrome instead of 150). This is the arithmetic the second bar row costs,
// and the asymmetry in it is the interesting part -- it is NOT one item off
// each mode:
//
//   covers ON.   N * 99 + 3 * 75 <= 1330  ->  N <= (1330 - 225) / 99 = 11.16
//                -> 11. UNCHANGED, because the covers-on page already had
//                91 px to spare and a 75 px bar row eats only that slack:
//                11 * 99 + 225 = 1314, 16 px left. 12 would need 1413.
//   covers OFF.  N * 75 + 3 * 75 <= 1330  ->  N <= (1330 - 225) / 75 = 14.73
//                -> 14, one item row lost. 14 * 75 + 225 = 1275, 55 px left
//                (the same 55 the unwrapped case had, since a bar row and a
//                text row are the same 75 px -- the row was swapped, not
//                squeezed). 15 would need 1350.
//
// Floored at 1: a firmware whose rows were taller than the whole content area
// must still show one item rather than an empty listing with working page
// arrows. COUPLED TO NF_COVER_H_PX above -- change it and this follows
// automatically, which is the point of computing rather than hardcoding; what
// must be REDONE by hand is the arithmetic in this comment.
//
// A first device screenshot must COUNT the item rows on a full page, in BOTH
// modes: if either shows fewer than this returns, the 1330/75/99 terms are
// what to re-measure, not this quotient.
//
// `chromeBarRows` is clamped at 0 from below, so a caller that somehow asks
// for a negative number of bars gets the whole content area rather than a
// page size inflated by nonsense.
int nf_items_per_page(bool covers, int chromeBarRows);

// --- the command bar's layout -------------------------------------------
//
// THE BAR IS LAID OUT AT NATURAL WIDTHS, and that is a device-measured
// correction rather than a preference. It used to hand every item an equal
// share of the width (one QHBoxLayout slot of stretch 1 each) and elide
// anything that did not fit its share, which on 2026-09-27 produced:
//
//     browser: bar item 'sort' is 357 px against a 190 px slot (6 items) -- eliding it
//     browser: bar item 'BACK' is 178 px against a 163 px slot (7 items) -- eliding it
//
// i.e. `< BACK  sort: n...  filter: all  view  select  rescan` on the panel,
// with the sort state -- the one thing on that bar the owner asked to have
// SPELLED OUT rather than carried by a caret -- elided into invisibility. The
// items' natural widths add up to well under the bar; it was the equal slot
// that broke them. An elided command label is a control whose function cannot
// be read, which is strictly worse than one more page turn, so:
//
//   1. if the items fit side by side at their natural widths, they are laid
//      out at those widths and the leftover becomes SPACING BETWEEN them;
//   2. if they do not, the bar WRAPS to a second row (and the page loses one
//      item row -- nf_items_per_page above has that arithmetic);
//   3. only if a single row of that wrapped bar still overflows does anything
//      elide, and then proportionally to what each item asked for rather than
//      into equal shares.
//
// This function is the whole of decisions 1-3 and it is PURE -- widths in,
// row assignments and per-item budgets out -- so the rule is host-testable
// even though the widths themselves come from QFontMetrics on the device.

// The most items either bar can hold. SEVEN is the worst case the command bar
// reaches today (BACK, sort, filter, view, select, rescan and -- only while
// something is on the clipboard -- paste); eight leaves one spare so the next
// control added is not also an edit to this line.
//
// It lives HERE rather than in nfview.cc (where it used to) because
// nf_bar_plan below is sized by it and the plan is what the browser reads its
// layout out of -- two spellings of the same bound is one of them being wrong
// later.
#define NF_BAR_MAX_ITEMS 8

// At most TWO bar rows. Not an arbitrary cap: a third row would cost a second
// item row off the page (NF_CHROME_BAR_PX is the same 75 px as a text row),
// and a command bar occupying a fifth of the panel to show controls nobody
// asked to see is a worse answer than eliding the one label that overflows --
// which is exactly what rule 3 above then does.
#define NF_BAR_MAX_ROWS 2

// The least horizontal space allowed between two adjacent bar labels, i.e.
// the width at which two controls stop reading as two.
//
// EIGHT, which is about one space at this row font (measured ascent 46 px, so
// a pixel size near 34 and a space near a quarter of that). It is deliberately
// the MINIMUM and not the gap that actually gets drawn: the leftover width is
// spread evenly between the items, so the drawn gap is (avail - sum)/(n-1) and
// is far larger whenever there is slack. This number only binds at the wrap
// boundary -- and at that boundary a tight row beats a second bar row that
// costs an item off every page, which is why it is small.
//
// If a device run's bar-geometry line reports a WRAP whose natural total is
// within a few tens of px of the width, THIS is the number to revisit; the log
// line prints the total and the gap budget separately so that judgement is
// made on the measurement rather than on a screenshot.
#define NF_BAR_MIN_GAP_PX 8

// What nf_bar_plan_layout decides. POD with no constructor, so a caller can
// leave it a plain local (nfview.cc has no file-scope objects with dynamic
// initialisers -- CLAUDE.md) and the function fills every field.
struct nf_bar_plan {
    int  rows;                        // 1 or NF_BAR_MAX_ROWS
    int  rowOf[NF_BAR_MAX_ITEMS];     // which bar row item i belongs on, 0-based
    int  budgetPx[NF_BAR_MAX_ITEMS];  // the px item i's label may occupy: its
                                      // natural width unless it had to shrink
    bool elided[NF_BAR_MAX_ITEMS];    // true exactly where budgetPx < natural
    int  naturalTotalPx;              // every label plus (n-1) minimum gaps, as
                                      // ONE row -- the number the log prints
                                      // and the number the fit test is made on
    bool anyElided;                   // true iff any elided[] is
};

// Lays `n` items of the given natural label widths into at most
// NF_BAR_MAX_ROWS rows of `availPx`, per rules 1-3 above.
//
// Rows are filled GREEDILY, in order, which is what keeps item 0 -- always
// this screen's EXIT (nf_bar_commands, nfview.cc) -- on the first row in every
// case, and keeps the bar's reading order the order the caller asked for. A
// balanced split was considered and rejected for exactly that: it can push the
// exit onto the second row for no gain a reader would notice.
//
// `n` above NF_BAR_MAX_ITEMS is clamped rather than refused, `naturalPx` may
// hold zeros, and a non-positive `availPx` (a bar measured before layout) is
// treated as "everything fits" -- eliding against a width that is not a
// measurement would hide labels for a reason that is not real.
void nf_bar_plan_layout(int const *naturalPx, int n, int availPx, nf_bar_plan *out);

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

// A file size a reader can read: "512 B", "1.5 KB", "11.8 MB", "1.2 GB".
// EMPTY for a negative size, which is a refusal rather than a repair -- the
// same rule as nf_date_key_is_plausible's: a negative size is not a size, and
// rendering it as "0 B" would hide whatever produced it behind a
// plausible-looking answer. A size of 0 IS rendered ("0 B"): a zero-byte file
// is a real thing to find on a card, and this browser's whole job is to say
// what is actually there.
//
// INTEGER ARITHMETIC ONLY, no floating point and no maths runtime -- same
// reasoning as nf_cover_width_px's rounding. One decimal place for KB and
// above, none for bytes, so a column of sizes reads consistently.
//
// THE SPACE BEFORE THE UNIT IS A U+00A0, like every other separator this file
// builds (nf_nbsp, above), for two reasons that are the same reason: an
// ordinary space is a wrap opportunity, so "11.8 MB" could break across the
// row edge, and rich text collapses runs of ordinary whitespace, so the width
// this string MEASURES at could stop being the width it RENDERS at. That
// mismatch, in exactly these suffixes, is what clipped every row on
// 2026-09-04.
QString nf_format_size(qint64 bytes);

// The trailing suffix for one row: the size (if `view` asks for it) followed
// by the folder marker, the "not in library" reason, or the reading-progress
// marker. Exactly one of those three, in that priority order, or nothing --
// see nffmt.cc for what each one means and why the order is what it is.
// Either output pointer may be NULL.
//
// `view` is taken whole rather than as a bare `bool showSize` so that a
// future suffix driven by another toggle has somewhere to read it from
// without changing every call site again -- and because nfview.cc holds
// exactly this struct, so nothing at the call site has to be unpacked.
//
// ONE FUNCTION DECIDES BOTH THE MARKUP AND THE WIDTH, and the size suffix is
// inside it for that reason alone: the row's elision reserve is measured off
// the PLAIN twin this function returns (nfview.cc), so a size appended
// anywhere else would be drawn without being paid for and would push the same
// one-or-two characters off the right edge that NOTES.md Task 13 records at
// length.
void nf_row_suffix(nf_row const& row, nf_view_flags view,
                   QString *markup, QString *plain);

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
// AN UNAVAILABLE END IS NOT SHOWN AT ALL: this returns an EMPTY label and
// `false` for that end's active flag. It used to return the words "no
// prev"/"no next", which existed for one reason only -- to stop the bar's
// layout jumping as a reader pages through a folder, leaving the page counter
// sliding around under their thumb. The owner asked for the words gone.
//
// THE PROPERTY THEY EXISTED FOR IS KEPT, and it was never the text that was
// holding it up: the bar is three slots of EQUAL STRETCH (nfview.cc's
// nf_bar_add, addWidget(w, 1)), so the counter's slot is the middle third of
// the bar whatever the other two contain. An empty end therefore moves
// nothing. What nfview.cc must not do is REMOVE the end from the layout --
// a hidden QWidget reports isEmpty() to its layout, and what a box layout
// then does with the slot is a Qt internal no layout guarantee should rest
// on. Its own comment has that trap, and the geometry that rules out the
// symmetric-stretch alternative, written out in full.
//
// The empty label is also why the two flags still exist and are still
// separate from it: "there is no previous page" is a fact a caller has to act
// on (do not allocate a TouchLabel, do not wire a tap), and it can no longer
// be read back off a label that says so in words.
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

// --- the sort and filter submenus ---------------------------------------
//
// Tapping `sort:` or `filter:` in the command bar used to CYCLE to the next
// value. With five sort keys times two directions and eight filter values,
// reaching a specific one took up to eight taps. Both now open a SUBMENU
// instead: the ITEM LIST is replaced, in place, by one row per option, using
// the same TouchLabel rows the listing uses.
//
// What those rows SAY is here rather than in nfview.cc, for the reason the
// rest of this header exists: it is a pure function of the menu, the row
// index and the currently active setting, so it is the one part of the
// submenus a host test can run. What stays in nfview.cc is the part that
// cannot be tested off-device at all -- allocating Nickel's own TouchLabel
// per row, wiring its tap, and swapping the content widget.
//
// THE ROW ORDER IS THE OLD CYCLE ORDER, deliberately: the owner has learned
// `name -> size -> date -> added -> read` and `all -> cbz -> cbr -> pdf ->
// epub -> finished -> in progress -> not started` on hardware, so the menus
// list them in exactly that sequence and that mental model survives. A menu
// that reordered them (alphabetically, say) would be a second vocabulary for
// the same set.
enum nf_menu_kind {
    // The item listing is showing. The ZERO value on purpose, the same
    // reasoning as NF_READ_UNKNOWN and NF_ICON_UNKNOWN: a zeroed or .bss
    // browser mode then reads as "browsing", never as a menu nobody opened.
    NF_MENU_NONE,
    NF_MENU_SORT,
    NF_MENU_FILTER,
    // The five view toggles (nf_view_flags, above). A SIBLING of the two
    // above rather than a new mechanism: same TouchLabel rows, same command
    // bar over it, same BACK-closes-it routing, same return-to-the-page-you
    // -were-on. What differs is only that each row is a TOGGLE rather than a
    // selection -- so there is no single "active" row to mark, and each row
    // states its own state instead.
    NF_MENU_VIEW,

    // --- THE CONFIRMATION SCREENS ---------------------------------------
    //
    // Three more modes of the SAME screen, not a new mechanism and not a
    // second mode variable: same TouchLabel rows, same command bar over them,
    // same single BACK routing function (nfview.cc's nf_browser_back), same
    // return-to-the-page-you-were-on. They are here, in nf_menu_kind, for
    // exactly that reason -- two mode variables are two things that can
    // disagree about where the reader is, and the whole argument for one BACK
    // function is that there is then one place to read "where am I" from.
    //
    // What differs from the three above is only WHO BUILDS THE ROWS. A sort
    // or view row is a pure function of the menu and an index, so
    // nf_menu_row_label builds it. A confirmation row has to name a COUNT
    // that only the browser knows, so it comes from nf_confirm_row_label
    // below instead -- which is still pure, still host-tested, and still the
    // one place those words are spelled. nf_menu_row_count therefore answers
    // 0 for all three: they have no rows OF THAT KIND, and saying so is
    // honest rather than a gap.
    //
    // EVERY ONE OF THESE IS ENTERED FROM A COMMAND-BAR TAP AND LEFT BY
    // `cancel`, BACK, or the action row itself. None of them performs
    // anything on the way in.
    NF_MENU_CONFIRM_DELETE,
    NF_MENU_CONFIRM_PASTE,
    NF_MENU_CONFIRM_RESCAN,
};

// How many rows `menu` has. 0 for NF_MENU_NONE and for all three confirmation
// screens -- see nf_confirm_row_count for those.
int nf_menu_row_count(nf_menu_kind menu);

// One word per sort key and per filter value, and the ONE place either
// vocabulary is spelled. The command bar's labels (below) and the menu rows
// both read from these, so "the word in the bar" and "the word in the menu"
// cannot drift apart -- which is the only thing that tells a reader that
// tapping `date` in the menu is what makes the bar read `sort: date ^`.
QString nf_sort_key_name(nf_sort_key key);
QString nf_filter_name(nf_filter_kind filter);

// The command bar's own three labels: "sort: name (asc)", "filter: all" and
// "view: default".
//
// THE DIRECTION IS SPELLED OUT -- "(asc)" and "(desc)", not the "^" and "v"
// this used to carry. The owner's reason: the carets are terse, and the
// direction is the one thing on this bar that should not have to be decoded.
// Still plain ASCII, so still e-ink-safe with no glyph this panel's font may
// not carry, and still the same convention as "< BACK"/"< PREV". The
// cycle and selection semantics are untouched by that change; only the words
// moved, and tests pin the literals on both sides (the bar label and the
// menu's own active row) so the two cannot drift apart again.
//
// "date" is the FILE's own mtime and "added"/"read" are the LIBRARY's two
// dates; three one-word names for three genuinely different questions, all
// short enough not to eat the bar slot's width budget the way "date added"
// and "date last read" would. "read" is the reading DATE, not the read STATE
// -- read state lives on the filter side ("filter: finished"), and the two
// never both show a word from the other's vocabulary.
QString nf_sort_bar_label(nf_sort_key key, bool descending);
QString nf_filter_bar_label(nf_filter_kind filter);

// The view bar item, which is STATIC TEXT -- the bare word "view", always,
// with no state suffix at all.
//
// It used to read "view: default" or "view: custom". That suffix existed
// because five independent toggles cannot fit one ~316 px bar slot, so it was
// a compressed summary of whether anything had been changed. The owner has
// now seen it on the device and would rather have no summary than a vague
// one: "custom" says something is different without saying what, which is one
// tap short of the answer either way, and the view submenu already states
// every toggle's real state on its own row -- which is where that information
// belongs.
//
// STILL A FUNCTION, and still taking `view`, rather than being folded into a
// string literal at the call site: the bar item, the submenu rows and the
// per-build log line all read their words from this file, and collapsing this
// one into nfview.cc would be the first crack in that. The argument is
// deliberately ignored; a test pins the literal on both sides.
QString nf_view_bar_label(nf_view_flags view);

// All five view rows joined with " | ", for the one log line every content
// build carries (nfview.cc). Built out of nf_menu_row_label itself rather
// than out of a second set of words, so the log line and the menu can never
// disagree about what mode the browser is in -- which is the entire point of
// logging it: a screenshot taken under a changed flag is otherwise
// indistinguishable from a rendering bug, and this project has already
// discarded a working fix once because a stale screenshot was read as "it
// does not work".
QString nf_view_flags_summary(nf_view_flags view);

// What row `index` of the sort / filter / view menu selects. False, with no
// write, for an index outside 0..count-1 -- so a caller can never turn a
// nonsense index into a confident wrong setting, the same
// refusal-rather-than-repair rule as nf_date_key_is_plausible's.
bool nf_menu_sort_key_at(int index, nf_sort_key *key);
bool nf_menu_filter_at(int index, nf_filter_kind *filter);
bool nf_menu_view_toggle_at(int index, nf_view_toggle *toggle);

// The text row `index` of `menu` shows, given the currently active settings.
// EMPTY for an out-of-range index or for NF_MENU_NONE.
//
// AN INACTIVE ROW IS THE BARE WORD -- "size", "epub" -- and nothing else.
//
// THE ACTIVE ROW IS MARKED IN THE TEXT, never by styling: this panel has four
// grey levels and "slightly lighter" does not read as "different" on it,
// which is the same finding that puts "[not in library]" in a row's words
// rather than leaving it to colour (nf_row_suffix, above). So the active row
// carries a leading "* " that no other row has.
//
// THE ACTIVE SORT ROW ALSO SAYS WHAT TAPPING IT WILL DO, because that is the
// only way direction is reachable now that tapping `sort:` opens a menu
// instead of cycling:
//
//     name                            an inactive key -- tapping it selects it
//     * date (asc) (tap for desc)     the active key, ascending
//     * date (desc) (tap for asc)     the active key, descending
//
// The direction is spelled out rather than carried by "^"/"v" -- see
// nf_sort_bar_label above for the owner's reason. The word a tap moves TO is
// written WITHOUT its brackets ("tap for desc", not "tap for (desc)"): the
// brackets belong to the state marker, and nesting one bracketed word inside
// another reads as a typo rather than as emphasis.
//
// Tapping a DIFFERENT key selects it and KEEPS the current direction, so the
// mark on the newly active row is the same word the old one carried. The
// active FILTER row reads "* epub (active)" instead: a filter has no
// direction, so tapping it again simply closes the menu, and the parenthesis
// says what the row is rather than what a tap does.
//
// A VIEW ROW IS A TOGGLE, so it has no active/inactive distinction at all and
// carries no "* " mark: every row states its own state in its own TEXT --
// "covers: on", "size: hidden" -- because that is the only channel this panel
// reliably has. Styling is not an option: four grey levels, on which
// "slightly lighter" reads as "the same", which is the same finding that puts
// "[not in library]" into a row's words (nf_row_suffix, above).
//
// SINGLE SPACES ONLY, everywhere, and that is a rendering constraint rather
// than a style: these labels are set as PLAIN text (nfview.cc), but a run of
// two spaces would collapse to one the moment anything set them as rich text
// -- the same measure-versus-render mismatch that clipped every row on
// 2026-09-04 (nf_nbsp above). A test pins it so the two can never diverge
// silently.
//
// `view` is REQUIRED rather than defaulted, unlike nf_build_listing's own
// late-added arguments: those default to v1's behaviour, which is a
// defensible no-op, whereas a view row built against defaulted flags would
// state a mode the browser is not in -- a confident wrong answer in the one
// place a reader looks to find out what mode they are in.
QString nf_menu_row_label(nf_menu_kind menu, int index,
                          nf_sort_key activeKey, bool activeDesc,
                          nf_filter_kind activeFilter,
                          nf_view_flags view);

// --- select mode and the file operations --------------------------------
//
// The WORDS the file-operation UI uses, and nothing else: no path is touched
// here, no filesystem is consulted, and nothing below can delete anything.
// The guards live in nfpath.h (also pure, also host-tested) and the calls in
// nfops.h. This block exists because a confirmation screen that does not say
// plainly what it is about to do is worse than no confirmation at all, and
// "says plainly" is a property a host test can pin literally.

// "1 item" / "3 items" / "0 items". Proper pluralisation rather than
// "item(s)": this string ends up inside the one row a reader taps to destroy
// something, and a row that reads "delete 1 item(s)" invites being skimmed
// as boilerplate. Negative counts are clamped to 0 -- a refusal to render
// nonsense, the same rule as nf_format_size's on a negative size, except
// that a count has a sensible floor where a size does not.
QString nf_item_count_text(int count);

// True for the three NF_MENU_CONFIRM_* modes. THE ONE PLACE that set is
// spelled, so nfview.cc's BACK routing, its title switch and its content
// dispatch all agree about which modes are confirmations -- three
// independently written `menu == A || menu == B || menu == C` tests are three
// chances to forget the third one, and the one that gets forgotten is
// whichever was added last.
bool nf_menu_is_confirm(nf_menu_kind menu);

// The sentence at the top of a confirmation screen. `count` is how many items
// the action would touch; `cut` distinguishes a pending MOVE from a pending
// COPY and is ignored by the other two kinds.
//
// THE RESCAN HEADER NAMES THE WI-FI, in plain words, and that is a
// requirement rather than a nicety: PlugWorkflowManager::sync() is not a bare
// rescan -- it is the front of Nickel's whole post-USB workflow, and on
// completion it calls WirelessWorkflowManager::connectWirelessSilently(),
// i.e. it TURNS THE WI-FI ON (.superpowers/sdd/v2-features/
// rescan-archaeology.md, section 7.2). The owner asked for a manual button
// specifically BECAUSE of that, so the consequence has to be on the screen
// they tap, not only in a comment. A test pins the word "Wi-Fi" in this
// string so it cannot be edited out by someone tidying the wording.
//
// THE DELETE HEADER SAYS IT CANNOT BE UNDONE, for the same reason: there is
// no undo anywhere in this design and no recycle bin on this device.
QString nf_confirm_header(nf_menu_kind menu, int count, bool cut);

// How many ACTION rows a confirmation screen has (0 for anything that is not
// one), and what each says. `cancel` is ALWAYS index 0, i.e. always the top
// row, and that ordering is the safety decision in this function: a reader
// who taps before reading hits the harmless row, and the destructive one is
// never where a mis-tap lands by default.
//
// THE CONFIRMING ROW SAYS WHAT IT WILL DO -- "delete 3 items", "move 2 items
// here", "rescan now (turns Wi-Fi on)" -- never "OK". A row labelled "OK"
// carries none of the count, none of the verb and none of the consequence, so
// it can only be read by remembering the header; these can be read on their
// own.
int nf_confirm_row_count(nf_menu_kind menu);
QString nf_confirm_row_label(nf_menu_kind menu, int index, int count, bool cut);

// The command-bar item that leaves select mode, carrying the count: "done (3)".
// The count is in the bar because it is the one number a reader needs before
// tapping `delete`, and the rows that carry the ticks may be on another page.
QString nf_select_bar_label(int selected);

// The command-bar item that pastes, carrying the number of items waiting:
// "paste (2)". Shown only when that number is non-zero (nfview.cc) -- a
// `paste` control with an empty clipboard would be a tap target that does
// nothing, which this project does not build.
QString nf_paste_bar_label(int pending);

// The tick a row carries in SELECT MODE, in the same TWO FORMS every other
// row fragment comes in (nf_icon_badge, nf_row_suffix): the rich-text markup
// that gets rendered, and the plain twin that gets MEASURED so the marker is
// paid for out of the row's width budget before the name is elided into what
// is left. Substituting `&nbsp;` in the markup form must yield the plain form
// exactly -- the invariant a test pins, and the one whose violation clipped
// every row on 2026-09-04 (nf_nbsp, above).
//
// IN THE TEXT, NOT IN A STYLE. This panel has four grey levels and "slightly
// lighter" reads as "the same", which is the same finding that puts
// "[not in library]" into a row's words. "[x]" and "[ ]" are both three
// characters wide, so the names after them line up whether a row is ticked or
// not -- and an UNTICKED row carries "[ ]" rather than nothing, so the tick
// column exists before anything is in it and a reader can see that select
// mode is on from any row, not only from the bar.
void nf_select_marker(bool selected, QString *markup, QString *plain);

#endif
