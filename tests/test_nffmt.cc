#include "nftest.h"
#include "nffmt.h"
#include <QStringList>

// The measured bug, from the reference card on 2026-09-03:
// books/Comics/English/Sandman/ holds DIRECTORIES named v1, v10, v2 ... v9.
// A plain case-insensitive sort puts v10 -- The Wake, the finale -- second.
static void test_unpadded_volume_dirs(void) {
    CHECK(nf_natural_compare("v1 - Preludes and Nocturnes",
                             "v2 - The Doll's House") < 0);
    CHECK(nf_natural_compare("v2 - The Doll's House",
                             "v9 - The Kindly Ones") < 0);
    CHECK(nf_natural_compare("v9 - The Kindly Ones",
                             "v10 - The Wake") < 0);
}

// Measured 2026-09-03. 27 volumes sharing a 20-character head and a
// 38-character tail; the year differs, so it survives and is informative.
static void test_strip_fullmetal(void) {
    QStringList n;
    for (int v = 1; v <= 3; v++) {
        n << QString("Fullmetal Alchemist v%1 (2005) (Digital) (LostNerevarine-Empire).cbz")
                 .arg(v, 2, 10, QLatin1Char('0'));
    }
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "v01");
    CHECK_EQ_STR(n.at(2), "v03");
}

// The case that broke the first implementation. These share
// "... Aventure (Part ", so cutting at the whitespace leaves rows starting
// "1) - Rouge ..." and loses the "(Part 1)" that distinguishes the sub-series.
static void test_strip_backs_off_past_open_bracket(void) {
    QStringList n;
    n << "Pokémon - La Grande Aventure (Part 1) - Rouge T01 (2014) [Manga FR].cbz"
      << "Pokémon - La Grande Aventure (Part 1) - Rouge T02 (2014) [Manga FR].cbz"
      << "Pokémon - La Grande Aventure (Part 4b) - Emeraude T03 (2017) [Manga FR].cbz";
    nf_strip_common(&n);
    CHECK(n.at(0).startsWith(QString::fromUtf8("(Part 1)")));
    CHECK(n.at(2).startsWith(QString::fromUtf8("(Part 4b)")));
    CHECK(n.at(0).contains(QString::fromUtf8("T01")));
}

// The extension is NOT stripped for free. ".cbz" has no whitespace inside it,
// so the token-boundary rule cannot cut there; it has to be split off first.
static void test_strip_handles_extension_explicitly(void) {
    QStringList n;
    n << "Batman v01.cbz" << "Batman v02.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "v01");
    CHECK_EQ_STR(n.at(1), "v02");
}

// A mixed-format listing keeps its extensions, because there the extension is
// the thing telling two rows apart.
static void test_strip_keeps_extension_when_mixed(void) {
    QStringList n;
    n << "Report.epub" << "Report.pdf";
    nf_strip_common(&n);
    CHECK(n.at(0).endsWith(".epub"));
    CHECK(n.at(1).endsWith(".pdf"));
}

// No common run at all: every row is left exactly as it was.
static void test_strip_no_common_run(void) {
    QStringList n;
    n << "v1 - Preludes and Nocturnes" << "v10 - The Wake" << "Sandman Mystery Theatre";
    QStringList before = n;
    nf_strip_common(&n);
    CHECK(n == before);
}

// One row has no "common" run worth the name -- it would strip to nothing.
static void test_strip_single_row_untouched(void) {
    QStringList n;
    n << "Solo Book.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "Solo Book.cbz");
}

// Refuse to strip rather than strip badly: a listing of full names is verbose,
// a listing of one-character rows is broken.
//
// Both this and test_strip_refuses_short_remainder_even_when_mixed are caught
// by the `keep` guard in normal execution -- it returns from inside the per-row
// loop, so the post-loop length check is never even reached. What separates
// them is MUTATION DETECTION, not which guard fires.
//
// Disable the `keep` guard and this case is still refused, because the post-loop
// check sees a trimmed "A" of one character. The mixed-extension case is NOT,
// because there the extension is appended first and "1.cbz" is five characters.
// So this test cannot detect a broken `keep` guard and the mixed one can, which
// is why both exist.
static void test_strip_refuses_when_remainder_too_short(void) {
    QStringList n;
    n << "Book A.cbz" << "Book B.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "Book A.cbz");
}

// The case that makes the keep-guard non-redundant, and it took a mutation
// coming back GREEN to find it. With a SAME-extension listing the post-loop
// length check backstops the keep guard, so mutating the keep guard changes
// nothing. With MIXED extensions the extension is appended AFTER the length
// check, so a one-character remainder becomes "1.cbz" -- five characters --
// and sails straight past the post-loop guard.
//
// Measured on the host: with `keep < 2` these stay whole; with `keep < 0` they
// become [1.cbz] and [2.pdf].
static void test_strip_refuses_short_remainder_even_when_mixed(void) {
    QStringList n;
    n << "Vol 1.cbz" << "Vol 2.pdf";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "Vol 1.cbz");
    CHECK_EQ_STR(n.at(1), "Vol 2.pdf");
}

// The device-found bug (NOTES.md, Task 10, measured 2026-09-04): the shared
// run at the card's root is the TITLE, not noise around it. nf_strip_common
// was working exactly as designed -- the run really is common to both
// names -- and stripped it anyway, leaving "1 - 2001" / "2 - 2021" with the
// title gone. Contrast test_strip_fullmetal/test_strip_backs_off_past_open_
// bracket below, the inverse case this guard must NOT fire for.
static void test_strip_refuses_when_remainder_has_no_letter(void) {
    QStringList n;
    n << "steven l. kent - the ultimate history of video games, volume 1 - 2001.kepub.epub"
      << "steven l. kent - the ultimate history of video games, volume 2 - 2021.kepub.epub";
    QStringList before = n;
    nf_strip_common(&n);
    CHECK(n == before);
}

// Review finding F1, reproduced: the letter guard used to run on the
// POST-extension label, which a MIXED-extension listing satisfies
// vacuously off the extension's own letters ("1 - 2001.kepub.epub" reads
// as "has a letter"). Kepubifying just ONE of the two Kent volumes -- a
// routine Kobo operation -- reaches exactly this case: same shared title,
// but now two different extensions. Must refuse exactly like the
// same-extension pair above.
static void test_strip_refuses_when_remainder_has_no_letter_even_mixed(void) {
    QStringList n;
    n << "steven l. kent - the ultimate history of video games, volume 1 - 2001.kepub.epub"
      << "steven l. kent - the ultimate history of video games, volume 2 - 2021.epub";
    QStringList before = n;
    nf_strip_common(&n);
    CHECK(n == before);
}

static nf_entry ent(char const *name, bool isDir, qint64 size = 0, qint64 mtime = 0) {
    nf_entry e;
    e.name  = QString::fromUtf8(name);
    e.isDir = isDir;
    e.size  = size;
    e.mtime = mtime;
    return e;
}

// Measured: books/Comics/English/Sandman holds 11 subfolders AND 3 files, so a
// folder of both kinds is real on this card rather than hypothetical.
static void test_sort_folders_before_files(void) {
    QVector<nf_entry> e;
    e << ent("zeta.cbz", false) << ent("alpha", true) << ent("beta.cbz", false)
      << ent("omega", true);
    nf_sort_entries(&e);
    CHECK(e.at(0).isDir);
    CHECK(e.at(1).isDir);
    CHECK_EQ_STR(e.at(0).name, "alpha");
    CHECK_EQ_STR(e.at(1).name, "omega");
    CHECK_EQ_STR(e.at(2).name, "beta.cbz");
}

static void test_sort_uses_natural_order_within_kind(void) {
    QVector<nf_entry> e;
    e << ent("v10 - The Wake", true) << ent("v2 - The Doll's House", true)
      << ent("v1 - Preludes", true) << ent("v9 - The Kindly Ones", true);
    nf_sort_entries(&e);
    CHECK_EQ_STR(e.at(0).name, "v1 - Preludes");
    CHECK_EQ_STR(e.at(1).name, "v2 - The Doll's House");
    CHECK_EQ_STR(e.at(2).name, "v9 - The Kindly Ones");
    CHECK_EQ_STR(e.at(3).name, "v10 - The Wake");
}

// NF_SORT_NAME, descending -- the direction toggle's simplest case, no
// folders involved so there is nothing for it to float incorrectly.
static void test_sort_name_descending(void) {
    QVector<nf_entry> e;
    e << ent("alpha.cbz", false) << ent("beta.cbz", false) << ent("gamma.cbz", false);
    nf_sort_entries(&e, NF_SORT_NAME, /*descending=*/true);
    CHECK_EQ_STR(e.at(0).name, "gamma.cbz");
    CHECK_EQ_STR(e.at(1).name, "beta.cbz");
    CHECK_EQ_STR(e.at(2).name, "alpha.cbz");
}

static void test_sort_by_size_ascending(void) {
    QVector<nf_entry> e;
    e << ent("big.cbz", false, 3000) << ent("small.cbz", false, 100)
      << ent("medium.cbz", false, 1500);
    nf_sort_entries(&e, NF_SORT_SIZE, false);
    CHECK_EQ_STR(e.at(0).name, "small.cbz");
    CHECK_EQ_STR(e.at(1).name, "medium.cbz");
    CHECK_EQ_STR(e.at(2).name, "big.cbz");
}

static void test_sort_by_size_descending(void) {
    QVector<nf_entry> e;
    e << ent("big.cbz", false, 3000) << ent("small.cbz", false, 100)
      << ent("medium.cbz", false, 1500);
    nf_sort_entries(&e, NF_SORT_SIZE, true);
    CHECK_EQ_STR(e.at(0).name, "big.cbz");
    CHECK_EQ_STR(e.at(1).name, "medium.cbz");
    CHECK_EQ_STR(e.at(2).name, "small.cbz");
}

static void test_sort_by_date_ascending(void) {
    QVector<nf_entry> e;
    e << ent("newest.cbz", false, 0, 3000) << ent("oldest.cbz", false, 0, 100)
      << ent("middle.cbz", false, 0, 1500);
    nf_sort_entries(&e, NF_SORT_DATE, false);
    CHECK_EQ_STR(e.at(0).name, "oldest.cbz");
    CHECK_EQ_STR(e.at(1).name, "middle.cbz");
    CHECK_EQ_STR(e.at(2).name, "newest.cbz");
}

static void test_sort_by_date_descending(void) {
    QVector<nf_entry> e;
    e << ent("newest.cbz", false, 0, 3000) << ent("oldest.cbz", false, 0, 100)
      << ent("middle.cbz", false, 0, 1500);
    nf_sort_entries(&e, NF_SORT_DATE, true);
    CHECK_EQ_STR(e.at(0).name, "newest.cbz");
    CHECK_EQ_STR(e.at(1).name, "middle.cbz");
    CHECK_EQ_STR(e.at(2).name, "oldest.cbz");
}

// THE 6.2 TRAP. "Reverse the list" -- the obvious, wrong implementation of a
// descending toggle -- would put every file ahead of every folder here,
// because "zzz-folder" sorts alphabetically after every file below it and a
// whole-list reversal cannot tell kind from key. The right behaviour keeps
// BOTH folders ahead of BOTH files, in EITHER direction; only the order
// within each kind may flip.
static void test_sort_descending_does_not_float_files_above_folders(void) {
    QVector<nf_entry> e;
    e << ent("aaa-file.cbz", false) << ent("zzz-file.cbz", false)
      << ent("aaa-folder", true) << ent("zzz-folder", true);
    nf_sort_entries(&e, NF_SORT_NAME, /*descending=*/true);
    CHECK(e.at(0).isDir);
    CHECK(e.at(1).isDir);
    CHECK(!e.at(2).isDir);
    CHECK(!e.at(3).isDir);
    // and within each kind, genuinely reversed
    CHECK_EQ_STR(e.at(0).name, "zzz-folder");
    CHECK_EQ_STR(e.at(1).name, "aaa-folder");
    CHECK_EQ_STR(e.at(2).name, "zzz-file.cbz");
    CHECK_EQ_STR(e.at(3).name, "aaa-file.cbz");
}

// Same trap, this time with the size key, so a size-sort's descending toggle
// is checked independently of the name-sort case above.
static void test_sort_by_size_descending_does_not_float_files_above_folders(void) {
    QVector<nf_entry> e;
    // A folder is deliberately given a larger `size` than either file --
    // stat()'d folder sizes are not content sizes (nf_entry_before's own
    // comment), but this fixture does not need that to be realistic, only to
    // prove the folder stays first even when its own key value would put it
    // last under a naive whole-list sort.
    e << ent("small.cbz", false, 100) << ent("big.cbz", false, 3000)
      << ent("huge-folder", true, 999999);
    nf_sort_entries(&e, NF_SORT_SIZE, /*descending=*/true);
    CHECK(e.at(0).isDir);
    CHECK_EQ_STR(e.at(0).name, "huge-folder");
    CHECK_EQ_STR(e.at(1).name, "big.cbz");
    CHECK_EQ_STR(e.at(2).name, "small.cbz");
}

// Determinism, which is what makes a listing reproducible across runs.
//
// This deliberately does NOT try to test stability directly: nf_entry carries
// no payload beyond name and isDir, so two tied entries are indistinguishable
// and any "stability" assertion over them can only restate the input. What IS
// observable is that the sort is idempotent and independent of input order,
// which is the property the listing actually depends on.
static void test_sort_is_deterministic(void) {
    QVector<nf_entry> sorted;
    sorted << ent("alpha", true) << ent("omega", true)
           << ent("v1.cbz", false) << ent("v2.cbz", false);

    QVector<nf_entry> again = sorted;
    nf_sort_entries(&again);          // sorting sorted input changes nothing
    for (int i = 0; i < sorted.size(); i++) {
        CHECK_EQ_STR(again.at(i).name, sorted.at(i).name.toUtf8().constData());
        CHECK(again.at(i).isDir == sorted.at(i).isDir);
    }

    QVector<nf_entry> reversed;
    for (int i = sorted.size() - 1; i >= 0; i--)
        reversed << sorted.at(i);
    nf_sort_entries(&reversed);       // and reversed input reaches the same order
    for (int i = 0; i < sorted.size(); i++) {
        CHECK_EQ_STR(reversed.at(i).name, sorted.at(i).name.toUtf8().constData());
        CHECK(reversed.at(i).isDir == sorted.at(i).isDir);
    }
}

// Every book format measured on the card, plus the .kepub.epub special case.
static void test_allowlist_admits_the_measured_formats(void) {
    CHECK(nf_is_book_name("Some Book.epub"));
    CHECK(nf_is_book_name("Some Book.kepub.epub"));
    CHECK(nf_is_book_name("Volume 1.cbz"));
    CHECK(nf_is_book_name("Sandman 50.cbr"));
    CHECK(nf_is_book_name("Booklet.pdf"));
    CHECK(nf_is_book_name("SHOUTING.CBZ"));
}

// Every non-book actually present on the card on 2026-09-03. If this list ever
// shrinks, something started leaking into the listing.
static void test_allowlist_rejects_the_measured_junk(void) {
    CHECK(!nf_is_book_name("metadata.calibre"));
    CHECK(!nf_is_book_name("driveinfo.calibre"));
    CHECK(!nf_is_book_name("KOBOY-INSTALL.md"));
    CHECK(!nf_is_book_name("sketch1.svg"));
    CHECK(!nf_is_book_name("metadata.pdf.lua"));
    CHECK(!nf_is_book_name("WPSettings.dat"));
    CHECK(!nf_is_book_name("IndexerVolumeGuid"));
}

// The card's ONLY .txt is koboy's probe file, which Nickel imported as a book.
// It HAS a database row, so no greyed-row logic would catch it -- the allowlist
// is the only thing that keeps it out. Spec section 3.5.
static void test_allowlist_excludes_txt_deliberately(void) {
    CHECK(!nf_is_book_name("koboy-probe-Io.txt"));
}

static void test_filter_all_admits_everything(void) {
    CHECK(nf_matches_filter("Volume 1.cbz", NF_FILTER_ALL));
    CHECK(nf_matches_filter("Volume 1.pdf", NF_FILTER_ALL));
    CHECK(nf_matches_filter("not a book at all", NF_FILTER_ALL));
}

static void test_filter_by_extension(void) {
    CHECK(nf_matches_filter("Volume 1.cbz", NF_FILTER_CBZ));
    CHECK(!nf_matches_filter("Volume 1.pdf", NF_FILTER_CBZ));

    CHECK(nf_matches_filter("Sandman 50.cbr", NF_FILTER_CBR));
    CHECK(!nf_matches_filter("Sandman 50.cbz", NF_FILTER_CBR));

    CHECK(nf_matches_filter("Booklet.pdf", NF_FILTER_PDF));
    CHECK(!nf_matches_filter("Booklet.epub", NF_FILTER_PDF));

    // NF_FILTER_EPUB matches BOTH plain and Kobo epub -- the same
    // one-format treatment nf_book_extension already gives them.
    CHECK(nf_matches_filter("Some Book.epub", NF_FILTER_EPUB));
    CHECK(nf_matches_filter("Some Book.kepub.epub", NF_FILTER_EPUB));
    CHECK(!nf_matches_filter("Some Book.pdf", NF_FILTER_EPUB));
}

static void test_hidden_dirs(void) {
    CHECK(nf_is_hidden_dir(".kobo"));
    CHECK(nf_is_hidden_dir(".adds"));
    // Measured: a KOReader sidecar sits in plain sight at the card root.
    CHECK(nf_is_hidden_dir("calibrewebdownload2055pdf2055.sdr"));
    CHECK(nf_is_hidden_dir("System Volume Information"));
    CHECK(!nf_is_hidden_dir("books"));
    CHECK(!nf_is_hidden_dir("Comics"));
    CHECK(!nf_is_hidden_dir("Sandman Mystery Theatre"));
}

// --- read state, and the three read-state filters -----------------------

// A row fixture. Every field nf_matches_read_filter can read is set here
// explicitly rather than left to nf_row's own constructor, because what these
// tests are ABOUT is which combination of them the predicate believes.
static nf_row rw(char const *name, bool isDir, bool hasRow, nf_read_state st) {
    nf_row r;
    r.name      = QString::fromUtf8(name);
    r.label     = r.name;
    r.isDir     = isDir;
    r.hasRow    = hasRow;
    r.readState = st;
    r.finished  = (st == NF_READ_FINISHED);
    return r;
}

// Kobo's own ReadingStatus values, which this project's enum deliberately is
// NOT numerically equal to -- nf_read_state_from_status is the one place
// those three numbers appear (nffmt.h), so this test is the one place they
// are pinned.
static void test_read_state_from_status(void) {
    CHECK(nf_read_state_from_status(0) == NF_READ_NOT_STARTED);
    CHECK(nf_read_state_from_status(1) == NF_READ_IN_PROGRESS);
    CHECK(nf_read_state_from_status(2) == NF_READ_FINISHED);
}

// THE GUARD, and the reason it is in the pure layer at all: an unexpected
// value must degrade to UNKNOWN, never to 0. 0 is a REAL bucket ("not
// started"), so a firmware that renumbered ReadingStatus, or an offset/symbol
// that came back wrong, would otherwise file every book under "not started"
// with nothing to tell it apart from a book that really is unread -- an
// invisible wrong answer. Same reasoning as the percentRead offset guard
// (nfnickel.cc), which treats an out-of-range percentage as unknown rather
// than clamping it into range.
static void test_read_state_out_of_range_degrades_to_unknown(void) {
    CHECK(nf_read_state_from_status(3) == NF_READ_UNKNOWN);
    CHECK(nf_read_state_from_status(-1) == NF_READ_UNKNOWN);
    CHECK(nf_read_state_from_status(1919) == NF_READ_UNKNOWN);
    // Negative control: a mapping that answered UNKNOWN for EVERY input would
    // pass all three checks above and be worthless, so one real bucket has to
    // be checked not to degrade.
    CHECK(nf_read_state_from_status(0) != NF_READ_UNKNOWN);
}

static void test_read_filter_matches_only_its_own_bucket(void) {
    nf_row fin  = rw("Done.epub", false, true, NF_READ_FINISHED);
    nf_row prog = rw("Halfway.epub", false, true, NF_READ_IN_PROGRESS);
    nf_row cold = rw("Untouched.epub", false, true, NF_READ_NOT_STARTED);

    CHECK(nf_matches_read_filter(fin, NF_FILTER_FINISHED));
    CHECK(!nf_matches_read_filter(fin, NF_FILTER_IN_PROGRESS));
    CHECK(!nf_matches_read_filter(fin, NF_FILTER_NOT_STARTED));

    CHECK(nf_matches_read_filter(prog, NF_FILTER_IN_PROGRESS));
    CHECK(!nf_matches_read_filter(prog, NF_FILTER_FINISHED));
    CHECK(!nf_matches_read_filter(prog, NF_FILTER_NOT_STARTED));

    CHECK(nf_matches_read_filter(cold, NF_FILTER_NOT_STARTED));
    CHECK(!nf_matches_read_filter(cold, NF_FILTER_FINISHED));
    CHECK(!nf_matches_read_filter(cold, NF_FILTER_IN_PROGRESS));
}

// The owner's real v26 case: a file on disk Nickel never imported. It has no
// MEASURABLE read state, which is not the same as being unread, so all three
// read-state filters hide it -- and every other filter still shows it.
static void test_read_filter_hides_unknown_state(void) {
    nf_row noRow = rw("missing Volume 26.cbz", false, false, NF_READ_UNKNOWN);
    CHECK(!nf_matches_read_filter(noRow, NF_FILTER_FINISHED));
    CHECK(!nf_matches_read_filter(noRow, NF_FILTER_IN_PROGRESS));
    CHECK(!nf_matches_read_filter(noRow, NF_FILTER_NOT_STARTED));
    CHECK(nf_matches_read_filter(noRow, NF_FILTER_ALL));

    // hasRow is checked as well as readState, not instead of it: a row with no
    // Volume must never land in a bucket however some future `meta` leaves
    // readState behind.
    nf_row lying = rw("Contradictory.epub", false, false, NF_READ_FINISHED);
    CHECK(!nf_matches_read_filter(lying, NF_FILTER_FINISHED));

    // And the reverse: a row that HAS a Volume whose state came back
    // unreadable is hidden too.
    nf_row unreadable = rw("Odd.epub", false, true, NF_READ_UNKNOWN);
    CHECK(!nf_matches_read_filter(unreadable, NF_FILTER_NOT_STARTED));
}

// Folders are never filtered out by anything -- nf_build_listing's type-filter
// comment has the reason (hiding a folder makes the files inside it
// unreachable, not merely invisible), and a folder has no Volume and so no
// read state to match against in the first place.
static void test_read_filter_never_hides_a_folder(void) {
    nf_row dir = rw("Comics", true, false, NF_READ_UNKNOWN);
    CHECK(nf_matches_read_filter(dir, NF_FILTER_FINISHED));
    CHECK(nf_matches_read_filter(dir, NF_FILTER_IN_PROGRESS));
    CHECK(nf_matches_read_filter(dir, NF_FILTER_NOT_STARTED));
    CHECK(nf_matches_read_filter(dir, NF_FILTER_ALL));
}

// The two axes ignore each other, in both directions: a read-state filter has
// no opinion about format and a type filter has none about read state. That is
// what keeps ONE enum and ONE chrome row honest.
static void test_the_two_filter_axes_ignore_each_other(void) {
    nf_row fin = rw("Done.epub", false, true, NF_READ_FINISHED);
    CHECK(nf_matches_read_filter(fin, NF_FILTER_PDF));
    CHECK(nf_matches_read_filter(fin, NF_FILTER_EPUB));
    CHECK(nf_matches_filter("Booklet.pdf", NF_FILTER_FINISHED));
    CHECK(nf_matches_filter("Some Book.epub", NF_FILTER_NOT_STARTED));
    // Negative control for the pair above: nf_matches_filter really does say
    // no to something, so "it says yes to a read-state filter" is not just
    // this function agreeing with everything.
    CHECK(!nf_matches_filter("Booklet.pdf", NF_FILTER_EPUB));
}

// nf_sort_rows applies the SAME rules as nf_sort_entries over the row type the
// pipeline actually holds by the time it orders (metadata now runs first --
// nflist.cc). Pinned here rather than only through nf_build_listing so a
// folders-before-files regression names the sort, not the pipeline.
static void test_sort_rows_matches_entry_sort_rules(void) {
    QVector<nf_row> r;
    nf_row a = rw("Volume 10.cbz", false, true, NF_READ_NOT_STARTED); a.size = 10;
    nf_row b = rw("Volume 2.cbz",  false, true, NF_READ_NOT_STARTED); b.size = 200;
    nf_row d = rw("Extras", true, false, NF_READ_UNKNOWN);            d.size = 1;
    r << a << b << d;

    nf_sort_rows(&r, NF_SORT_NAME, false);
    CHECK(r.at(0).isDir);                       // folders first, in either direction
    CHECK_EQ_STR(r.at(1).name, "Volume 2.cbz"); // natural order, not lexicographic
    CHECK_EQ_STR(r.at(2).name, "Volume 10.cbz");

    nf_sort_rows(&r, NF_SORT_SIZE, true);
    CHECK(r.at(0).isDir);                       // ... including descending
    CHECK_EQ_STR(r.at(1).name, "Volume 2.cbz"); // 200 bytes, the larger
    CHECK_EQ_STR(r.at(2).name, "Volume 10.cbz");
}

// --- the two library date sort keys ------------------------------------
//
// Everything below is pure by construction: nffmt.cc never sees a Volume, and
// these are the only checks in this project that can run against the date
// logic at all (every libnickel call that FETCHES the bytes is untestable
// off-device -- CLAUDE.md).
static nf_row dated(char const *name, char const *added, char const *lastRead) {
    nf_row r;
    r.name         = QString::fromUtf8(name);
    r.label        = r.name;
    r.isDir        = false;
    r.hasRow       = true;
    r.dateAdded    = QByteArray(added);
    r.dateLastRead = QByteArray(lastRead);
    return r;
}

// Byte order IS chronological order for fixed-field ISO-8601, which is the
// whole reason this mod compares bytes rather than parsing -- and the reason
// Nickel does too (qstrcmp in DateAddedKey's sorter, strcasecmp in
// RecentSorter's).
static void test_date_compare_orders_iso_strings_by_bytes(void) {
    CHECK(nf_date_compare(QByteArray("2024-01-02T03:04:05.000"),
                          QByteArray("2024-01-02T03:04:06.000")) < 0);
    CHECK(nf_date_compare(QByteArray("2024-12-31T23:59:59.999"),
                          QByteArray("2025-01-01T00:00:00.000")) < 0);
    CHECK(nf_date_compare(QByteArray("2025-01-01T00:00:00.000"),
                          QByteArray("2025-01-01T00:00:00.000")) == 0);
    CHECK(nf_date_compare(QByteArray("2025-06-01T00:00:00.000"),
                          QByteArray("2024-06-01T00:00:00.000")) > 0);
}

// Nickel uses strcasecmp, not strcmp, for the recent key (measured -- the
// date-getter archaeology, section 6), and it matters at the 'T' separator.
// NEGATIVE CONTROL: a case-SENSITIVE byte compare would answer non-zero for
// this pair ('T' is 0x54, 't' is 0x74), so a check that only ever fed one
// spelling of the separator would pass against the wrong implementation.
static void test_date_compare_is_case_insensitive_like_nickel(void) {
    CHECK(nf_date_compare(QByteArray("2024-01-02T03:04:05.000"),
                          QByteArray("2024-01-02t03:04:05.000")) == 0);
    // and the case difference must not swamp a real ordering difference
    CHECK(nf_date_compare(QByteArray("2024-01-02t03:04:05.000"),
                          QByteArray("2024-01-03T00:00:00.000")) < 0);
}

// THE EMPTY-DATE RULE. Empty is substituted with Nickel's own
// ZERO_DB_DATE_ARRAY sentinel, so a dateless row is the OLDEST thing in the
// listing -- never the newest, which is the invisible wrong answer.
//
// NEGATIVE CONTROL: an implementation that put empty LAST (or that compared
// "" against a real date with plain strcmp, where "" is also less-than, but
// for the wrong reason) is distinguished by the second pair below -- empty
// must compare EQUAL to the sentinel spelled out in full, not merely less
// than everything.
static void test_date_compare_treats_empty_as_nickels_zero_sentinel(void) {
    CHECK(nf_date_compare(QByteArray(), QByteArray("2024-01-02T03:04:05.000")) < 0);
    CHECK(nf_date_compare(QByteArray("2024-01-02T03:04:05.000"), QByteArray()) > 0);
    // empty IS the sentinel, not merely smaller than everything
    CHECK(nf_date_compare(QByteArray(), QByteArray(NF_ZERO_DB_DATE)) == 0);
    CHECK(nf_date_compare(QByteArray(NF_ZERO_DB_DATE),
                          QByteArray("0001-01-01T00:00:00.000")) < 0);
    // two unknowns tie, so the caller's name tie-break decides -- not the
    // arbitrary order two dateless rows happened to arrive in
    CHECK(nf_date_compare(QByteArray(), QByteArray()) == 0);
}

// The shape check that stands in for a hardcoded offset's missing dlsym
// safety net. Accept empty (the honest unknown) and real ISO-8601 prefixes.
static void test_date_validator_accepts_what_the_db_holds(void) {
    CHECK(nf_date_key_is_plausible(QByteArray()));                              // no row: normal, not an error
    CHECK(nf_date_key_is_plausible(QByteArray("2024-01-02T03:04:05.000")));
    CHECK(nf_date_key_is_plausible(QByteArray("2024-01-02t03:04:05")));         // lowercase separator
    CHECK(nf_date_key_is_plausible(QByteArray("2024-01-02 03:04:05")));         // SQLite's space separator
    CHECK(nf_date_key_is_plausible(QByteArray("2024-01-02T03:04:05.000Z")));    // zone suffix
    // Nickel's OWN sentinel must pass -- it is a real value the DB path can
    // hand back, and rejecting it would log a layout failure for a book that
    // simply has no date.
    CHECK(nf_date_key_is_plausible(QByteArray(NF_ZERO_DB_DATE)));
}

// THE NEGATIVE CONTROL FOR THE VALIDATOR. Every input below is what a MOVED
// STRUCT OFFSET actually lands on -- a title, an image id, a raw integer's
// bytes -- and a naive "is it non-empty" or "does it contain a dash" check
// would accept all of them and hand the sort a garbage key with nothing
// logged. That is the failure this predicate exists to make loud.
static void test_date_validator_rejects_what_a_moved_offset_lands_on(void) {
    CHECK(!nf_date_key_is_plausible(QByteArray("Fullmetal Alchemist v01")));    // a title
    CHECK(!nf_date_key_is_plausible(QByteArray("2024")));                       // too short to be a date
    CHECK(!nf_date_key_is_plausible(QByteArray("\x2a\x00\x00\x00", 4)));        // a raw int's four bytes, NULs included
    CHECK(!nf_date_key_is_plausible(QByteArray("file:///mnt/onboard/x.epub"))); // a ContentID
    // shaped ALMOST right, which is the case a laxer check waves through
    CHECK(!nf_date_key_is_plausible(QByteArray("2024/01/02T03:04:05")));        // wrong separators
    CHECK(!nf_date_key_is_plausible(QByteArray("202-01-02T03:04:05")));         // three-digit year
    CHECK(!nf_date_key_is_plausible(QByteArray("2024-01-02X03:04:05")));        // wrong date/time separator
    CHECK(!nf_date_key_is_plausible(QByteArray("abcd-ef-ghT00:00:00")));        // right punctuation, no digits
}

// The sort itself, and the control the ___SyncTime collapse forces: the new
// order must DIFFER from the name order. Names here are deliberately
// alphabetical while the dates are reversed, so an implementation that
// ignored the key (or read an always-empty field) would silently return
// a/b/c and pass a weaker check.
static void test_sort_by_added_is_not_the_name_order(void) {
    QVector<nf_row> r;
    r << dated("a.epub", "2024-01-01T00:00:00.000", "")
      << dated("b.epub", "2023-01-01T00:00:00.000", "")
      << dated("c.epub", "2022-01-01T00:00:00.000", "");

    nf_sort_rows(&r, NF_SORT_ADDED, false);
    CHECK_EQ_STR(r.at(0).name, "c.epub");   // oldest added first, ascending
    CHECK_EQ_STR(r.at(1).name, "b.epub");
    CHECK_EQ_STR(r.at(2).name, "a.epub");
    // ... which is NOT the name order, the whole point of the control
    CHECK(r.at(0).name != QString("a.epub"));

    nf_sort_rows(&r, NF_SORT_ADDED, true);
    CHECK_EQ_STR(r.at(0).name, "a.epub");   // and descending reverses it
    CHECK_EQ_STR(r.at(2).name, "c.epub");
}

// THE CONTROL THAT CATCHES THE ONE MISTAKE THE ___SyncTime COLLAPSE HIDES.
// For a never-opened sideloaded book both of Nickel's date keys collapse onto
// ___SyncTime and are therefore IDENTICAL, so an implementation that wired
// NF_SORT_READ to ::dateAdded by mistake would pass every "recently added"
// check above and every device run over never-opened books. The rows here
// give the two fields DELIBERATELY OPPOSITE orders, which is the only way to
// tell the two keys apart at all.
static void test_the_two_date_keys_read_different_fields(void) {
    QVector<nf_row> r;
    r << dated("a.epub", "2022-01-01T00:00:00.000", "2024-01-01T00:00:00.000")
      << dated("b.epub", "2023-01-01T00:00:00.000", "2023-06-01T00:00:00.000")
      << dated("c.epub", "2024-01-01T00:00:00.000", "2022-01-01T00:00:00.000");

    nf_sort_rows(&r, NF_SORT_ADDED, false);
    CHECK_EQ_STR(r.at(0).name, "a.epub");
    CHECK_EQ_STR(r.at(2).name, "c.epub");

    nf_sort_rows(&r, NF_SORT_READ, false);
    CHECK_EQ_STR(r.at(0).name, "c.epub");   // exactly the opposite order
    CHECK_EQ_STR(r.at(2).name, "a.epub");
}

// A file with no library row at all must not vanish, must not crash, and must
// land where Nickel puts a dateless row: first ascending, last descending.
static void test_sort_by_date_key_keeps_a_row_with_no_library_row(void) {
    QVector<nf_row> r;
    r << dated("has-a-row.epub", "2024-01-01T00:00:00.000", "2024-01-01T00:00:00.000");
    nf_row orphan;                       // hasRow false, both keys empty -- the [not in library] case
    orphan.name  = QString("orphan.epub");
    orphan.label = orphan.name;
    r << orphan;

    nf_sort_rows(&r, NF_SORT_READ, false);
    CHECK(r.size() == 2);                              // nothing dropped
    CHECK_EQ_STR(r.at(0).name, "orphan.epub");         // no date == oldest
    nf_sort_rows(&r, NF_SORT_READ, true);
    CHECK(r.size() == 2);
    CHECK_EQ_STR(r.at(1).name, "orphan.epub");         // ... and last descending
}

// Folders have no Volume, so nflist.cc never fills their date keys: every
// folder ties under either date key and falls through to the name tie-break.
// The 6.2 trap still applies -- folders group ahead of files in BOTH
// directions, and a whole-list reversal would break exactly that.
static void test_date_keys_group_folders_first_in_both_directions(void) {
    QVector<nf_row> r;
    r << dated("zzz-file.epub", "2024-01-01T00:00:00.000", "2024-01-01T00:00:00.000")
      << dated("aaa-file.epub", "2022-01-01T00:00:00.000", "2022-01-01T00:00:00.000");
    nf_row d1; d1.name = QString("aaa-folder"); d1.label = d1.name; d1.isDir = true;
    nf_row d2; d2.name = QString("zzz-folder"); d2.label = d2.name; d2.isDir = true;
    r << d1 << d2;

    nf_sort_rows(&r, NF_SORT_ADDED, false);
    CHECK(r.at(0).isDir);
    CHECK(r.at(1).isDir);
    CHECK(!r.at(2).isDir);
    CHECK_EQ_STR(r.at(0).name, "aaa-folder");   // dateless tie -> name tie-break
    CHECK_EQ_STR(r.at(1).name, "zzz-folder");

    nf_sort_rows(&r, NF_SORT_ADDED, true);
    CHECK(r.at(0).isDir);                       // still folders first, descending
    CHECK(r.at(1).isDir);
    CHECK(!r.at(2).isDir);
    CHECK_EQ_STR(r.at(0).name, "zzz-folder");   // tie-break reverses within the kind
}

// Two rows sharing a date key -- the ___SyncTime collapse makes this the
// COMMON case on this card, not a corner one -- must fall back to the name
// order rather than to whatever order they arrived in.
static void test_date_key_ties_fall_back_to_the_name_order(void) {
    QVector<nf_row> r;
    r << dated("Volume 10.cbz", "2024-01-01T00:00:00.000", "")
      << dated("Volume 2.cbz",  "2024-01-01T00:00:00.000", "");
    nf_sort_rows(&r, NF_SORT_ADDED, false);
    CHECK_EQ_STR(r.at(0).name, "Volume 2.cbz");   // natural order, not lexicographic
    CHECK_EQ_STR(r.at(1).name, "Volume 10.cbz");
}

// nf_sort_entries with a date-METADATA key: an nf_entry has nowhere to hold
// one, so both keys are empty for every entry and the name tie-break decides.
// Documented behaviour, pinned so it cannot drift into something surprising.
static void test_entry_sort_under_a_metadata_key_falls_back_to_name(void) {
    QVector<nf_entry> e;
    e << ent("c.cbz", false) << ent("a.cbz", false) << ent("b.cbz", false);
    nf_sort_entries(&e, NF_SORT_ADDED, false);
    CHECK_EQ_STR(e.at(0).name, "a.cbz");
    CHECK_EQ_STR(e.at(1).name, "b.cbz");
    CHECK_EQ_STR(e.at(2).name, "c.cbz");
}

// --- row icons ----------------------------------------------------------
//
// The reported defect this mapping answers, from the reference card:
// "The Road - A Graphic Novel Adaptation (2024) (Digital) (phillywilly-Empire).cbr"
// is a FILE and read as a folder, because the only folder/file marker was a
// trailing "/" on a long, elided label. So the one thing that must never be
// wrong is which side of that line a name lands on.
static void test_icon_kind_folder_wins_over_the_extension(void) {
    // A directory that happens to be NAMED like a comic is still a directory.
    CHECK(nf_icon_kind_for("Comics.cbz", true) == NF_ICON_FOLDER);
    CHECK(nf_icon_kind_for("English", true)    == NF_ICON_FOLDER);
    // ... and the measured file that started this is not a folder.
    CHECK(nf_icon_kind_for("The Road - A Graphic Novel Adaptation (2024) (Digital) (phillywilly-Empire).cbr", false)
              == NF_ICON_COMIC);
}

// The ordering trap NF_EXTS exists for: ".kepub.epub" also ends in ".epub",
// and both are one format to a reader. Pinned here as well as through
// nf_matches_filter so a regression names the icon map, not the filter.
static void test_icon_kind_treats_both_epub_spellings_as_one(void) {
    CHECK(nf_icon_kind_for("Dune.epub", false)       == NF_ICON_BOOK);
    CHECK(nf_icon_kind_for("Dune.kepub.epub", false) == NF_ICON_BOOK);
}

static void test_icon_kind_treats_both_comic_archives_as_one(void) {
    CHECK(nf_icon_kind_for("Sandman v01.cbz", false) == NF_ICON_COMIC);
    CHECK(nf_icon_kind_for("Sandman v01.cbr", false) == NF_ICON_COMIC);
}

static void test_icon_kind_pdf_is_its_own_kind(void) {
    // Nickel ships no PDF and no generic-document icon (task brief), so this
    // kind is the one that renders as a text badge rather than an image. That
    // is a rendering decision (nfview.cc); the KIND still has to be distinct,
    // or a PDF would be badged as something it is not.
    CHECK(nf_icon_kind_for("manual.pdf", false) == NF_ICON_PDF);
}

static void test_icon_kind_is_case_insensitive(void) {
    // The card holds mixed-case extensions; nf_book_extension already matches
    // case-insensitively, and this must not quietly compare case-sensitively
    // on top of it.
    CHECK(nf_icon_kind_for("DUNE.EPUB", false)       == NF_ICON_BOOK);
    CHECK(nf_icon_kind_for("Sandman.CBZ", false)     == NF_ICON_COMIC);
    CHECK(nf_icon_kind_for("Manual.PDF", false)      == NF_ICON_PDF);
    CHECK(nf_icon_kind_for("Dune.KEpub.ePub", false) == NF_ICON_BOOK);
}

// The negative control that makes "not unknown" mean something: a name the
// allowlist rejects must come out UNKNOWN, and every name it admits must not.
// Without the first half, an implementation that returned NF_ICON_BOOK for
// everything would pass every check above.
static void test_icon_kind_agrees_with_the_allowlist(void) {
    char const *const admitted[] = {
        "a.epub", "a.kepub.epub", "a.cbz", "a.cbr", "a.pdf", NULL,
    };
    for (int i = 0; admitted[i]; i++) {
        CHECK(nf_is_book_name(admitted[i]));
        CHECK(nf_icon_kind_for(admitted[i], false) != NF_ICON_UNKNOWN);
    }
    char const *const rejected[] = {
        "notes.txt", "cover.jpg", "README", "metadata.opf", "", NULL,
    };
    for (int i = 0; rejected[i]; i++) {
        CHECK(!nf_is_book_name(rejected[i]));
        CHECK(nf_icon_kind_for(rejected[i], false) == NF_ICON_UNKNOWN);
    }
}

// NF_ICON_UNKNOWN is the zero value on purpose (nffmt.h): a zeroed kind must
// read as "we do not know", never as a confident NF_ICON_FOLDER -- calling a
// file a folder is the exact wrong answer this whole feature exists to stop.
static void test_icon_kind_zero_value_is_unknown_not_folder(void) {
    CHECK(NF_ICON_UNKNOWN == 0);
    CHECK(NF_ICON_FOLDER != 0);
}

// --- the two-form label pieces ------------------------------------------
//
// THE INVARIANT these tests exist for, and the measurement behind it: on
// 2026-09-04 the device clipped its rows at the right edge (".pd" for ".pdf",
// "(40%" for "(40%)") even though the suffix was already being measured and
// paid for before the name was elided. The twins were spelling their
// separators with ASCII spaces while the rendered markup spelled the same
// separators `&nbsp;` -- two different characters, two different advances --
// so every measurement was short by the difference, per separator. Words
// matching is not enough: the measured form has to be, character for
// character, what the markup RENDERS as.
//
// The device's own QFontMetrics is not reachable from here, so what is
// testable is exactly that equality, which is the part that was wrong.

// `&nbsp;` is the only entity these fragments carry (nffmt.cc says why no
// other one can appear), so substituting it is a complete de-markup here.
static QString nf_test_rendered(QString const& markup) {
    return QString(markup).replace(QStringLiteral("&nbsp;"), QString(nf_nbsp()));
}

static nf_row nf_test_row(bool isDir, bool hasRow, bool finished, int percentRead) {
    nf_row r;
    r.isDir       = isDir;
    r.hasRow      = hasRow;
    r.finished    = finished;
    r.percentRead = percentRead;
    return r;
}

static void test_suffix_twin_is_what_the_markup_renders_as(void) {
    nf_row const rows[] = {
        nf_test_row(true,  false, false, -1),  // a folder
        nf_test_row(false, false, false, -1),  // no library row
        nf_test_row(false, true,  true,   0),  // finished
        nf_test_row(false, true,  false, 40),  // the measured "(40%)" row
        nf_test_row(false, true,  false,  0),  // unread: no suffix at all
    };
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++) {
        QString markup, plain;
        nf_row_suffix(rows[i], &markup, &plain);
        CHECK(nf_test_rendered(markup) == plain);
        // The twin must not still be carrying entity SOURCE text either --
        // that would measure "&nbsp;" as six characters instead of one.
        CHECK(!plain.contains(QLatin1Char('&')));
    }
}

// The equality above is satisfied BY CONSTRUCTION (nffmt.cc derives the
// markup from the plain form by substitution), so on its own it cannot see
// the bug that started this: it passes just as happily when BOTH forms spell
// the separator with ASCII spaces. Reintroducing that is what this test
// catches -- the separator has to be the non-breaking character on both
// sides, because an ordinary space is a collapsible run and a wrap
// opportunity in rich text, i.e. rendered at a width the plain twin did not
// measure. (Checked as a negative control: with the separator reverted to
// ASCII spaces, this test fails and the one above does not.)
static void test_suffix_separator_is_non_breaking(void) {
    QString const sepMarkup = QStringLiteral("&nbsp;&nbsp;");
    QString const sepPlain(2, nf_nbsp());

    nf_row const rows[] = {
        nf_test_row(false, false, false, -1),  // no library row
        nf_test_row(false, true,  true,   0),  // finished
        nf_test_row(false, true,  false, 40),  // the measured "(40%)" row
    };
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++) {
        QString markup, plain;
        nf_row_suffix(rows[i], &markup, &plain);
        CHECK(markup.startsWith(sepMarkup));
        CHECK(plain.startsWith(sepPlain));
        // Nothing but the separator run may be non-breaking: the spaces
        // INSIDE "[not in library]" are ordinary words, and a single space
        // between words is not a collapsible run.
        CHECK(!plain.mid(sepPlain.length()).contains(nf_nbsp()));
    }

    // A folder's "/" has no separator at all -- it terminates the name it
    // belongs to rather than standing apart from it.
    QString plain;
    nf_row_suffix(nf_test_row(true, false, false, -1), NULL, &plain);
    CHECK(!plain.startsWith(sepPlain));
}

static void test_icon_badge_twin_is_what_the_markup_renders_as(void) {
    nf_icon_kind const kinds[] = {
        NF_ICON_UNKNOWN, NF_ICON_FOLDER, NF_ICON_BOOK, NF_ICON_COMIC, NF_ICON_PDF,
    };
    for (int i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
        QString markup, plain;
        nf_icon_badge(kinds[i], &markup, &plain);
        CHECK(nf_test_rendered(markup) == plain);
        CHECK(!plain.contains(QLatin1Char('&')));
        // Five characters of badge plus one separator, all five kinds, so the
        // labels after them line up -- "[ ? ]" included, whose inner spaces
        // are non-breaking for exactly that reason.
        CHECK(plain.length() == 6);
        CHECK(!plain.contains(QLatin1Char(' ')));
    }
}

// The suffixes are what a row MEANS, so which one a row gets is worth pinning
// separately from how it is spelled -- this is the same priority order the row
// loop used to carry inline (nfview.cc), moved, not changed.
static void test_suffix_priority(void) {
    QString plain;

    // A folder: the trailing "/", and NO separator in front of it.
    nf_row_suffix(nf_test_row(true, false, false, -1), NULL, &plain);
    CHECK_EQ_STR(plain, "/");

    // isDir wins even over a percentage that should never be there for one.
    nf_row_suffix(nf_test_row(true, true, true, 40), NULL, &plain);
    CHECK_EQ_STR(plain, "/");

    // A file with no library row says so, in the label TEXT.
    nf_row_suffix(nf_test_row(false, false, false, -1), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[not in library]")));

    // ... and that reason outranks any progress the row happens to carry: a
    // row with no library row has no trustworthy progress to report.
    nf_row_suffix(nf_test_row(false, false, true, 40), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[not in library]")));

    // "Finished" outranks a stale percentage from an abandoned re-read.
    nf_row_suffix(nf_test_row(false, true, true, 12), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[finished]")));

    nf_row_suffix(nf_test_row(false, true, false, 40), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("(40%)")));
}

// 0% and -1 both mean "nothing to show" (nffmt.cc has Nickel's own [1,99]
// clamp as the reason), and an unread book gets no suffix at all.
static void test_suffix_hides_zero_and_unknown_progress(void) {
    QString markup, plain;

    nf_row_suffix(nf_test_row(false, true, false, 0), &markup, &plain);
    CHECK(plain.isEmpty());
    CHECK(markup.isEmpty());

    nf_row_suffix(nf_test_row(false, true, false, -1), &markup, &plain);
    CHECK(plain.isEmpty());
    CHECK(markup.isEmpty());
}

// Either output may be NULL -- the row loop asks for both, nf_icon_width_px
// asks for the plain form only, and nf_icon_markup for the markup only.
static void test_two_form_builders_accept_a_null_output(void) {
    nf_row_suffix(nf_test_row(false, false, false, -1), NULL, NULL);
    nf_icon_badge(NF_ICON_PDF, NULL, NULL);

    QString one;
    nf_row_suffix(nf_test_row(false, true, false, 40), &one, NULL);
    CHECK(one.endsWith(QStringLiteral("(40%)")));
    nf_icon_badge(NF_ICON_PDF, NULL, &one);
    CHECK(one.startsWith(QStringLiteral("[PDF]")));
}

// The suffix is subtracted BEFORE the name is elided -- that is the whole
// point of the budget -- so a longer suffix must leave a shorter name, never
// push itself off the edge.
static void test_name_budget_pays_for_the_icon_and_the_suffix_first(void) {
    CHECK(nf_name_budget_px(1000, 0, 0)   == 1000);
    CHECK(nf_name_budget_px(1000, 45, 0)  == 955);
    CHECK(nf_name_budget_px(1000, 45, 120) == 835);
    // Strictly decreasing in the suffix width, which is the property the
    // clipped "(40%" row needed and did not get.
    CHECK(nf_name_budget_px(1000, 45, 120) < nf_name_budget_px(1000, 45, 60));
}

// A pathological row degrades to "a stub plus its suffix", never to "no name
// at all" (an elide width at or below the ellipsis' own returns the ellipsis
// alone) and never to a negative width.
static void test_name_budget_floors_instead_of_going_negative(void) {
    CHECK(nf_name_budget_px(200, 300, 300) == NF_NAME_MIN_PX);
    CHECK(nf_name_budget_px(0, 0, 0)       == NF_NAME_MIN_PX);
    CHECK(nf_name_budget_px(-1000, 0, 0)   == NF_NAME_MIN_PX);
    CHECK(NF_NAME_MIN_PX > 0);
}

int main(void) {
    test_unpadded_volume_dirs();
    test_strip_fullmetal();
    test_strip_backs_off_past_open_bracket();
    test_strip_handles_extension_explicitly();
    test_strip_keeps_extension_when_mixed();
    test_strip_no_common_run();
    test_strip_single_row_untouched();
    test_strip_refuses_when_remainder_too_short();
    test_strip_refuses_short_remainder_even_when_mixed();
    test_strip_refuses_when_remainder_has_no_letter();
    test_strip_refuses_when_remainder_has_no_letter_even_mixed();
    test_sort_folders_before_files();
    test_sort_uses_natural_order_within_kind();
    test_sort_is_deterministic();
    test_sort_name_descending();
    test_sort_by_size_ascending();
    test_sort_by_size_descending();
    test_sort_by_date_ascending();
    test_sort_by_date_descending();
    test_sort_descending_does_not_float_files_above_folders();
    test_sort_by_size_descending_does_not_float_files_above_folders();
    test_allowlist_admits_the_measured_formats();
    test_allowlist_rejects_the_measured_junk();
    test_allowlist_excludes_txt_deliberately();
    test_filter_all_admits_everything();
    test_filter_by_extension();
    test_read_state_from_status();
    test_read_state_out_of_range_degrades_to_unknown();
    test_read_filter_matches_only_its_own_bucket();
    test_read_filter_hides_unknown_state();
    test_read_filter_never_hides_a_folder();
    test_the_two_filter_axes_ignore_each_other();
    test_sort_rows_matches_entry_sort_rules();
    test_date_compare_orders_iso_strings_by_bytes();
    test_date_compare_is_case_insensitive_like_nickel();
    test_date_compare_treats_empty_as_nickels_zero_sentinel();
    test_date_validator_accepts_what_the_db_holds();
    test_date_validator_rejects_what_a_moved_offset_lands_on();
    test_sort_by_added_is_not_the_name_order();
    test_the_two_date_keys_read_different_fields();
    test_sort_by_date_key_keeps_a_row_with_no_library_row();
    test_date_keys_group_folders_first_in_both_directions();
    test_date_key_ties_fall_back_to_the_name_order();
    test_entry_sort_under_a_metadata_key_falls_back_to_name();
    test_hidden_dirs();
    test_icon_kind_folder_wins_over_the_extension();
    test_icon_kind_treats_both_epub_spellings_as_one();
    test_icon_kind_treats_both_comic_archives_as_one();
    test_icon_kind_pdf_is_its_own_kind();
    test_icon_kind_is_case_insensitive();
    test_icon_kind_agrees_with_the_allowlist();
    test_icon_kind_zero_value_is_unknown_not_folder();
    test_suffix_twin_is_what_the_markup_renders_as();
    test_suffix_separator_is_non_breaking();
    test_icon_badge_twin_is_what_the_markup_renders_as();
    test_suffix_priority();
    test_suffix_hides_zero_and_unknown_progress();
    test_two_form_builders_accept_a_null_output();
    test_name_budget_pays_for_the_icon_and_the_suffix_first();
    test_name_budget_floors_instead_of_going_negative();
    NF_TEST_MAIN_END
}
