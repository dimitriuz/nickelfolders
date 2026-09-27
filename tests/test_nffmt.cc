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
        nf_row_suffix(rows[i], nf_view_flags_default(), &markup, &plain);
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
        nf_row_suffix(rows[i], nf_view_flags_default(), &markup, &plain);
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
    nf_row_suffix(nf_test_row(true, false, false, -1), nf_view_flags_default(), NULL, &plain);
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
    nf_row_suffix(nf_test_row(true, false, false, -1), nf_view_flags_default(), NULL, &plain);
    CHECK_EQ_STR(plain, "/");

    // isDir wins even over a percentage that should never be there for one.
    nf_row_suffix(nf_test_row(true, true, true, 40), nf_view_flags_default(), NULL, &plain);
    CHECK_EQ_STR(plain, "/");

    // A file with no library row says so, in the label TEXT.
    nf_row_suffix(nf_test_row(false, false, false, -1), nf_view_flags_default(), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[not in library]")));

    // ... and that reason outranks any progress the row happens to carry: a
    // row with no library row has no trustworthy progress to report.
    nf_row_suffix(nf_test_row(false, false, true, 40), nf_view_flags_default(), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[not in library]")));

    // "Finished" outranks a stale percentage from an abandoned re-read.
    nf_row_suffix(nf_test_row(false, true, true, 12), nf_view_flags_default(), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("[finished]")));

    nf_row_suffix(nf_test_row(false, true, false, 40), nf_view_flags_default(), NULL, &plain);
    CHECK(plain.endsWith(QStringLiteral("(40%)")));
}

// 0% and -1 both mean "nothing to show" (nffmt.cc has Nickel's own [1,99]
// clamp as the reason), and an unread book gets no suffix at all.
static void test_suffix_hides_zero_and_unknown_progress(void) {
    QString markup, plain;

    nf_row_suffix(nf_test_row(false, true, false, 0), nf_view_flags_default(), &markup, &plain);
    CHECK(plain.isEmpty());
    CHECK(markup.isEmpty());

    nf_row_suffix(nf_test_row(false, true, false, -1), nf_view_flags_default(), &markup, &plain);
    CHECK(plain.isEmpty());
    CHECK(markup.isEmpty());
}

// Either output may be NULL -- the row loop asks for both, nf_icon_width_px
// asks for the plain form only, and nf_icon_markup for the markup only.
static void test_two_form_builders_accept_a_null_output(void) {
    nf_row_suffix(nf_test_row(false, false, false, -1), nf_view_flags_default(), NULL, NULL);
    nf_icon_badge(NF_ICON_PDF, NULL, NULL);

    QString one;
    nf_row_suffix(nf_test_row(false, true, false, 40), nf_view_flags_default(), &one, NULL);
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

// --- book covers --------------------------------------------------------
//
// The one real, device-measured cover path this project has. Everything in
// this block is pinned against it, because the whole feature is a filename
// prediction: get any character or either bucket component wrong and the
// prediction names a file that does not exist, the row falls back to its type
// icon, and the browser looks exactly as healthy as it does for a book Nickel
// has genuinely never rendered a cover for. There is no loud failure
// available on-device, so it has to be pinned here.
#define FMA_V25_CONTENTID \
    "file:///mnt/onboard/books/Comics/English/" \
    "Fullmetal Alchemist (v01-v27) (2005-2011) (Digital)/" \
    "Fullmetal Alchemist v25 (2011) (Digital) (LostNerevarine-Empire).cbz"
#define FMA_V25_IMAGEID \
    "file____mnt_onboard_books_Comics_English_" \
    "Fullmetal_Alchemist_(v01-v27)_(2005-2011)_(Digital)_" \
    "Fullmetal_Alchemist_v25_(2011)_(Digital)_(LostNerevarine-Empire)_cbz"

static void test_cover_mangling_matches_the_device_vector(void) {
    CHECK_EQ_STR(nf_clean_image_id(QString::fromUtf8(FMA_V25_CONTENTID)),
                 FMA_V25_IMAGEID);
}

// Image::cleanId replaces EXACTLY four characters. The survivors are the
// interesting half: a mangler that also took '(' , ')' , '-' or ',' -- all of
// which appear in this card's own filenames -- would produce a wrong name for
// most of the library while still looking like a plausible mangling.
static void test_cover_mangling_touches_only_four_characters(void) {
    CHECK_EQ_STR(nf_clean_image_id("a/b"), "a_b");
    CHECK_EQ_STR(nf_clean_image_id("a:b"), "a_b");
    CHECK_EQ_STR(nf_clean_image_id("a.b"), "a_b");
    CHECK_EQ_STR(nf_clean_image_id("a b"), "a_b");
    CHECK_EQ_STR(nf_clean_image_id("(a-b), c"), "(a-b),_c");
    CHECK_EQ_STR(nf_clean_image_id("a_b"), "a_b");
    CHECK_EQ_STR(nf_clean_image_id("a\tb"), "a\tb");
    CHECK_EQ_STR(nf_clean_image_id(""), "");
}

// What licenses nfview.cc trying the RAW ImageId first and this cleaned form
// only as a second attempt: on an id that is already mangled -- which is what
// the archaeology says the DB column holds -- cleaning it is a no-op, so the
// second attempt costs nothing and can never change a path that already
// resolved.
static void test_cover_mangling_is_idempotent(void) {
    QString once  = nf_clean_image_id(QString::fromUtf8(FMA_V25_CONTENTID));
    QString twice = nf_clean_image_id(once);
    CHECK(once == twice);
}

static void test_cover_bucket_matches_the_device_vector(void) {
    unsigned h = nf_bucket_hash(QString::fromUtf8(FMA_V25_IMAGEID));
    CHECK((h & 0xffu)        == 90u);
    CHECK(((h >> 8) & 0xffu) == 174u);
    // The mask the firmware's own `bic` applies: nothing above 28 bits ever
    // survives the loop, so a value that does means the masking is gone.
    CHECK(h <= 0x0fffffffu);
}

static void test_cover_path_matches_the_device_file(void) {
    CHECK_EQ_STR(nf_cover_path(QString::fromUtf8(FMA_V25_IMAGEID)),
                 "/mnt/onboard/.kobo-images/90/174/" FMA_V25_IMAGEID
                 " - N3_LIBRARY_GRID.parsed");
}

// WHY A CHECK ON THE FIRST BUCKET COMPONENT ALONE WOULD BE VACUOUS, which is
// the trap the archaeology found and the reason nfview.cc's device check has
// to compare a whole path (or stat it) rather than a directory number.
//
// Three near-miss manglings of the same ContentID, all measured:
//   nothing mangled at all          -> 26/129
//   only ':' and '/'                -> 186/43
//   ':' '/' and ' ' but NOT '.'     -> 90/190   <-- first component CORRECT
// The third is the instructive one: forgetting '.' still yields 90.
static void test_cover_bucket_rejects_the_near_miss_manglings(void) {
    QString cid = QString::fromUtf8(FMA_V25_CONTENTID);

    unsigned raw = nf_bucket_hash(cid);
    CHECK((raw & 0xffu)        == 26u);
    CHECK(((raw >> 8) & 0xffu) == 129u);

    QString slashColon = cid;
    slashColon.replace(QLatin1Char('/'), QLatin1Char('_'));
    slashColon.replace(QLatin1Char(':'), QLatin1Char('_'));
    unsigned h2 = nf_bucket_hash(slashColon);
    CHECK((h2 & 0xffu)        == 186u);
    CHECK(((h2 >> 8) & 0xffu) == 43u);

    QString noDot = slashColon;
    noDot.replace(QLatin1Char(' '), QLatin1Char('_'));
    unsigned h3 = nf_bucket_hash(noDot);
    CHECK((h3 & 0xffu)        == 90u);   // right, and the path is still wrong
    CHECK(((h3 >> 8) & 0xffu) == 190u);
}

// THE NEGATIVE CONTROL THIS FEATURE MOST NEEDS.
//
// The hash runs over UTF-16 code units; the ImageId arrives as UTF-8 bytes.
// For an ASCII-only name the two interpretations are BYTE-FOR-BYTE THE SAME
// SEQUENCE, so every other test in this block would pass unchanged against an
// implementation that hashed the raw bytes -- including the Fullmetal vector,
// which is the only device-measured path there is. A card with Cyrillic
// filenames (this one has them, under /mnt/onboard/books) would then get a
// wrong directory for every non-ASCII book and no error anywhere.
//
// So this vector is the only thing in the suite that can tell the correct
// implementation from that one. Both answers are spelled out: the code-unit
// hash gives 6/27, the byte hash gives 198/169.
static void test_cover_bucket_hashes_code_units_not_bytes(void) {
    QString id = nf_clean_image_id(QString::fromUtf8(
        "file:///mnt/onboard/books/\xd0\x9f\xd0\xb5\xd0\xbb\xd0\xb5\xd0\xb2"
        "\xd0\xb8\xd0\xbd - \xd0\xa7\xd0\xb0\xd0\xbf\xd0\xb0\xd0\xb5\xd0\xb2"
        " \xd0\xb8 \xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd0\xbe\xd1\x82\xd0\xb0.pdf"));
    CHECK_EQ_STR(id, "file____mnt_onboard_books_\xd0\x9f\xd0\xb5\xd0\xbb\xd0"
                     "\xb5\xd0\xb2\xd0\xb8\xd0\xbd_-_\xd0\xa7\xd0\xb0\xd0\xbf"
                     "\xd0\xb0\xd0\xb5\xd0\xb2_\xd0\xb8_\xd0\x9f\xd1\x83\xd1"
                     "\x81\xd1\x82\xd0\xbe\xd1\x82\xd0\xb0_pdf");

    unsigned h = nf_bucket_hash(id);
    CHECK((h & 0xffu)        == 6u);
    CHECK(((h >> 8) & 0xffu) == 27u);

    // The wrong answer, stated so this check cannot pass vacuously: if the
    // implementation is ever "simplified" to hash id.toUtf8() the numbers
    // below are what it will produce, and the two above are what it will not.
    CHECK((h & 0xffu)        != 198u);
    CHECK(((h >> 8) & 0xffu) != 169u);

    // And the byte hash, computed here rather than asserted from a table, so
    // the claim "these two interpretations really do differ for this string"
    // is checked by the test itself rather than trusted.
    QByteArray utf8 = id.toUtf8();
    unsigned b = 0;
    for (int i = 0; i < utf8.size(); i++) {
        b = (unsigned)(unsigned char)utf8.at(i) + (b << 4);
        b ^= (b & 0xf0000000u) >> 23;
        b &= 0x0fffffffu;
    }
    CHECK((b & 0xffu)        == 198u);
    CHECK(((b >> 8) & 0xffu) == 169u);
    CHECK(b != h);

    // The same construction over an ASCII id must AGREE, which is what makes
    // the disagreement above attributable to the non-ASCII characters and not
    // to the loop being written differently in the two places.
    QByteArray ascii = QByteArray(FMA_V25_IMAGEID);
    unsigned ab = 0;
    for (int i = 0; i < ascii.size(); i++) {
        ab = (unsigned)(unsigned char)ascii.at(i) + (ab << 4);
        ab ^= (ab & 0xf0000000u) >> 23;
        ab &= 0x0fffffffu;
    }
    CHECK(ab == nf_bucket_hash(QString::fromUtf8(FMA_V25_IMAGEID)));
}

// bucketById("") is 0/0 -- a real directory on this card -- so an empty
// ImageId must be refused rather than turned into a plausible-looking path.
// This is the only cover input that could name a file belonging to some other
// book instead of naming nothing.
static void test_cover_path_refuses_an_empty_image_id(void) {
    CHECK(nf_cover_path(QString()).isEmpty());
    CHECK(nf_cover_path(QString::fromUtf8("")).isEmpty());
    CHECK(nf_bucket_hash(QString()) == 0u);   // the firmware's own empty branch
    // A single space is NOT empty: it mangles to "_" upstream and is a
    // perfectly nameable id, so it must still produce a path.
    CHECK(!nf_cover_path(QString::fromUtf8(" ")).isEmpty());
}

// The ImageId goes into the filename VERBATIM (Image::fileNameForType memcpys
// it), so nf_cover_path must not clean, trim or case-fold what it is handed --
// that decision belongs to its caller.
static void test_cover_path_inserts_the_id_verbatim(void) {
    QString path = nf_cover_path(QString::fromUtf8("Raw Id: with/dots.and spaces"));
    CHECK(path.contains(QString::fromUtf8("Raw Id: with/dots.and spaces")));
    CHECK(path.endsWith(QString::fromUtf8(" - N3_LIBRARY_GRID.parsed")));
    CHECK(path.startsWith(QString::fromUtf8("/mnt/onboard/.kobo-images/")));
}

static void test_cover_width_keeps_the_native_aspect(void) {
    // The number the <img> actually gets, and the number the elision reserve
    // is charged: 149/223 of 70 px is 46.8, rounded.
    //
    // Pinned to a literal ON PURPOSE, even though it is derived: this check
    // is what makes changing NF_COVER_H_PX a deliberate act rather than a
    // silent one. It has already earned that once -- the height went 76 -> 70
    // when covers were given taller rows, and this line failed and forced the
    // arithmetic to be redone instead of the change sliding through green.
    // Recomputing the expectation from the constant would assert nothing.
    CHECK(nf_cover_width_px(NF_COVER_H_PX) == 47);
    CHECK(nf_cover_width_px(NF_COVER_NATIVE_H) == NF_COVER_NATIVE_W);
    CHECK(nf_cover_width_px(2 * NF_COVER_NATIVE_H) == 2 * NF_COVER_NATIVE_W);
    // Never zero-width, whatever it is handed -- a 0-width <img> draws as
    // nothing while the row still pays the height.
    CHECK(nf_cover_width_px(0)     >= 1);
    CHECK(nf_cover_width_px(-500)  >= 1);
    // A cover is TALLER than it is wide; a set of constants that ever made it
    // the other way round would be a swapped pair.
    CHECK(nf_cover_width_px(NF_COVER_H_PX) < NF_COVER_H_PX);
}

// --- the page bar's three labels ---------------------------------------

// The ordinary middle page: both ends live, and the counter is 1-based where
// the argument is 0-based.
static void test_page_bar_middle_page_has_both_ends(void) {
    QString prev, pageText, next;
    bool prevActive = false, nextActive = false;
    nf_page_bar_labels(1, 4, &prev, &prevActive, &pageText, &next, &nextActive);
    CHECK(prevActive);
    CHECK(nextActive);
    CHECK_EQ_STR(prev,     "< PREV");
    CHECK_EQ_STR(pageText, "page 2/4");
    CHECK_EQ_STR(next,     "NEXT >");
}

// AN UNAVAILABLE END IS NOT SHOWN AT ALL. It used to read "no prev"/"no
// next" -- words that existed only to keep the bar's layout from jumping
// between pages. The owner asked for them gone, and the property they were
// standing in for is held by the three equal-width SLOTS instead, which never
// depended on the text (nffmt.h, and nfview.cc's own page-bar comment for the
// setVisible(false) trap that would break it).
static void test_page_bar_inert_ends_render_as_nothing(void) {
    QString prev, pageText, next;
    bool prevActive = true, nextActive = true;
    nf_page_bar_labels(0, 1, &prev, &prevActive, &pageText, &next, &nextActive);
    CHECK(!prevActive);
    CHECK(!nextActive);
    CHECK(prev.isEmpty());
    CHECK(next.isEmpty());
    CHECK_EQ_STR(pageText, "page 1/1");
}

// The flags are now the ONLY way a caller can tell an unavailable end from an
// available one -- there is no label left to read it off -- so the active
// forms have to be non-empty and carry their arrow, or "empty means inert"
// would be vacuous.
static void test_page_bar_active_ends_are_the_only_ones_with_an_arrow(void) {
    QString prev, next;
    bool prevActive = true, nextActive = true;
    nf_page_bar_labels(0, 1, &prev, &prevActive, NULL, &next, &nextActive);
    CHECK(prev.isEmpty());
    CHECK(next.isEmpty());

    QString aPrev, aNext;
    bool aPrevActive = false, aNextActive = false;
    nf_page_bar_labels(1, 3, &aPrev, &aPrevActive, NULL, &aNext, &aNextActive);
    CHECK(aPrevActive);
    CHECK(aNextActive);
    CHECK(aPrev.contains(QLatin1Char('<')));
    CHECK(aNext.contains(QLatin1Char('>')));
    CHECK_EQ_STR(aPrev, "< PREV");
    CHECK_EQ_STR(aNext, "NEXT >");
}

// The two edges of a multi-page listing, which is where a fencepost error
// would live: page 0 of 4 has no PREV but does have NEXT, and page 3 of 4 is
// the mirror of that.
static void test_page_bar_first_and_last_page(void) {
    QString prev, pageText, next;
    bool prevActive = true, nextActive = false;
    nf_page_bar_labels(0, 4, &prev, &prevActive, &pageText, &next, &nextActive);
    CHECK(!prevActive);
    CHECK(nextActive);
    CHECK_EQ_STR(pageText, "page 1/4");

    prevActive = false;
    nextActive = true;
    nf_page_bar_labels(3, 4, &prev, &prevActive, &pageText, &next, &nextActive);
    CHECK(prevActive);
    CHECK(!nextActive);
    CHECK_EQ_STR(pageText, "page 4/4");
}

// The defensive clamp. nfview.cc clamps its own page before calling this --
// it has to, to slice the row vector -- so none of these inputs is reachable
// from today's caller; the check is that a stale or nonsense value degrades
// to a sensible label instead of producing "page 0/0" or "page -2/3".
static void test_page_bar_clamps_a_nonsense_page(void) {
    QString pageText;
    bool prevActive = true, nextActive = true;

    nf_page_bar_labels(-5, 3, NULL, &prevActive, &pageText, NULL, &nextActive);
    CHECK_EQ_STR(pageText, "page 1/3");
    CHECK(!prevActive);
    CHECK(nextActive);

    nf_page_bar_labels(99, 3, NULL, &prevActive, &pageText, NULL, &nextActive);
    CHECK_EQ_STR(pageText, "page 3/3");
    CHECK(prevActive);
    CHECK(!nextActive);

    // An EMPTY listing is showing its one and only page, not a zeroth of
    // none -- nfview.cc floors totalPages at 1 for the same reason, and this
    // pins the behaviour if that floor ever moves.
    nf_page_bar_labels(0, 0, NULL, &prevActive, &pageText, NULL, &nextActive);
    CHECK_EQ_STR(pageText, "page 1/1");
    CHECK(!prevActive);
    CHECK(!nextActive);
}

// --- the sort and filter submenus ---------------------------------------

// THE ROW ORDER IS THE OLD TAP-CYCLE ORDER. The owner learned
// `name -> size -> date -> added -> read` on hardware while the bar item
// cycled; the menu that replaced the cycle lists them in exactly that
// sequence, so the mental model survives. A reordering here would be silent
// on a screenshot and obvious only after a wrong tap.
static void test_sort_menu_rows_are_the_old_cycle_order(void) {
    nf_sort_key want[] = { NF_SORT_NAME, NF_SORT_SIZE, NF_SORT_DATE,
                           NF_SORT_ADDED, NF_SORT_READ };
    CHECK(nf_menu_row_count(NF_MENU_SORT) == 5);
    for (int i = 0; i < 5; i++) {
        nf_sort_key got = NF_SORT_READ; // NOT the first expected value, so a
                                        // function that wrote nothing at all
                                        // cannot pass row 0 by accident
        CHECK(nf_menu_sort_key_at(i, &got));
        CHECK(got == want[i]);
    }
}

static void test_filter_menu_rows_are_the_old_cycle_order(void) {
    nf_filter_kind want[] = { NF_FILTER_ALL, NF_FILTER_CBZ, NF_FILTER_CBR,
                              NF_FILTER_PDF, NF_FILTER_EPUB, NF_FILTER_FINISHED,
                              NF_FILTER_IN_PROGRESS, NF_FILTER_NOT_STARTED };
    CHECK(nf_menu_row_count(NF_MENU_FILTER) == 8);
    for (int i = 0; i < 8; i++) {
        nf_filter_kind got = NF_FILTER_NOT_STARTED; // ditto, not row 0's value
        CHECK(nf_menu_filter_at(i, &got));
        CHECK(got == want[i]);
    }
}

// An out-of-range index must REFUSE rather than repair: a caller that turned
// a nonsense index into a confident setting would change the reader's sort
// key on a tap that hit nothing. Same rule as nf_date_key_is_plausible's.
static void test_menu_index_out_of_range_refuses_and_writes_nothing(void) {
    nf_sort_key key = NF_SORT_DATE;
    CHECK(!nf_menu_sort_key_at(-1, &key));
    CHECK(!nf_menu_sort_key_at(5, &key));
    CHECK(key == NF_SORT_DATE); // untouched

    nf_filter_kind filter = NF_FILTER_PDF;
    CHECK(!nf_menu_filter_at(-1, &filter));
    CHECK(!nf_menu_filter_at(8, &filter));
    CHECK(filter == NF_FILTER_PDF); // untouched

    CHECK(nf_menu_row_count(NF_MENU_NONE) == 0);
    CHECK(nf_menu_row_label(NF_MENU_NONE, 0, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()).isEmpty());
    CHECK(nf_menu_row_label(NF_MENU_SORT, -1, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()).isEmpty());
    CHECK(nf_menu_row_label(NF_MENU_SORT, 5, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()).isEmpty());
    CHECK(nf_menu_row_label(NF_MENU_FILTER, 8, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()).isEmpty());
}

// EXACTLY ONE row is marked, in either menu, whatever the active value is.
// Two marks or none would both render as a menu that does not say what is in
// force -- which on this panel is unrecoverable, because the mark is text and
// there is no styling fallback behind it (nffmt.h).
static void test_menu_marks_exactly_one_active_row(void) {
    nf_sort_key keys[] = { NF_SORT_NAME, NF_SORT_SIZE, NF_SORT_DATE,
                           NF_SORT_ADDED, NF_SORT_READ };
    for (int k = 0; k < 5; k++) {
        int marked = 0;
        for (int i = 0; i < 5; i++)
            if (nf_menu_row_label(NF_MENU_SORT, i, keys[k], false, NF_FILTER_ALL, nf_view_flags_default())
                    .startsWith(QLatin1Char('*')))
                marked++;
        CHECK(marked == 1);
    }

    nf_filter_kind filters[] = { NF_FILTER_ALL, NF_FILTER_CBZ, NF_FILTER_CBR,
                                 NF_FILTER_PDF, NF_FILTER_EPUB, NF_FILTER_FINISHED,
                                 NF_FILTER_IN_PROGRESS, NF_FILTER_NOT_STARTED };
    for (int f = 0; f < 8; f++) {
        int marked = 0;
        for (int i = 0; i < 8; i++)
            if (nf_menu_row_label(NF_MENU_FILTER, i, NF_SORT_NAME, false, filters[f], nf_view_flags_default())
                    .startsWith(QLatin1Char('*')))
                marked++;
        CHECK(marked == 1);
    }
}

// An inactive row is the BARE word -- nothing else, so the mark on the active
// one is a difference a reader can see at a glance rather than a decoration
// every row carries.
static void test_inactive_menu_rows_are_the_bare_word(void) {
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_SORT, 1, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()), "size");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_SORT, 4, NF_SORT_NAME, true,  NF_FILTER_ALL, nf_view_flags_default()), "read");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_FILTER, 4, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()), "epub");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_FILTER, 7, NF_SORT_NAME, false, NF_FILTER_ALL, nf_view_flags_default()), "not started");
}

// THE ACTIVE SORT ROW SAYS WHAT TAPPING IT WILL DO. With cycling gone,
// tapping the already-active key is the ONLY way to reverse direction, and
// nothing else on screen says so -- so the row itself has to, and it has to
// name the direction a tap moves TO, not just the one in force.
//
// THE LITERALS WERE "* date ^ (tap for v)" AND "* date v (tap for ^)" and were
// changed deliberately, not loosened: the owner replaced the carets with
// spelled-out words because direction is the one thing on this screen that
// should not have to be decoded. Pinning the whole string is the point of this
// check -- a test that stopped pinning the literal (matching "date" and a
// bracket, say) would still pass against a row that said nothing useful --
// which is why this is an edit rather than a relaxation. There is precedent in
// this file: nf_cover_width_px(NF_COVER_H_PX) == 51 was updated to 47 the same
// way when the constant behind it changed.
//
// The inner word carries NO brackets ("tap for desc", not "tap for (desc)"):
// the brackets belong to the state marker, and a bracketed word nested inside
// another bracket reads as a typo.
static void test_active_sort_row_shows_the_direction_and_the_tap(void) {
    QString asc = nf_menu_row_label(NF_MENU_SORT, 2, NF_SORT_DATE, false, NF_FILTER_ALL, nf_view_flags_default());
    CHECK_EQ_STR(asc, "* date (asc) (tap for desc)");

    QString desc = nf_menu_row_label(NF_MENU_SORT, 2, NF_SORT_DATE, true, NF_FILTER_ALL, nf_view_flags_default());
    CHECK_EQ_STR(desc, "* date (desc) (tap for asc)");

    // THE NEGATIVE CONTROL for the change itself: neither form may still carry
    // a bare caret or a bare "v" as a direction. Without this, a half-applied
    // rename -- the bar updated and the menu row not, or the other way round --
    // would leave the two spellings live side by side and every assertion above
    // would still pass on its own line.
    CHECK(!asc.contains(QLatin1Char('^')));
    CHECK(!desc.contains(QLatin1Char('^')));
    CHECK(!asc.contains(QStringLiteral(" v ")));
    CHECK(!desc.contains(QStringLiteral(" v ")));

    // The two must differ in BOTH marks, or the row would be announcing a tap
    // that does not move anywhere.
    CHECK(asc != desc);

    // A filter has no direction, so its active row promises nothing about a
    // tap -- it states what the row is.
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_FILTER, 5, NF_SORT_NAME, false, NF_FILTER_FINISHED, nf_view_flags_default()),
                 "* finished (active)");
}

// ONE VOCABULARY. The word in the menu row and the word in the command bar
// have to be the same one, or nothing tells a reader that tapping `added` is
// what makes the bar read `sort: added (asc)`.
static void test_the_bar_and_the_menu_share_one_vocabulary(void) {
    // The literals moved from "sort: name ^"/"sort: read v" with the same
    // deliberate edit as the menu row's own, above -- see there.
    CHECK_EQ_STR(nf_sort_bar_label(NF_SORT_NAME, false),  "sort: name (asc)");
    CHECK_EQ_STR(nf_sort_bar_label(NF_SORT_READ, true),   "sort: read (desc)");
    CHECK_EQ_STR(nf_filter_bar_label(NF_FILTER_ALL),      "filter: all");
    CHECK_EQ_STR(nf_filter_bar_label(NF_FILTER_NOT_STARTED), "filter: not started");

    for (int i = 0; i < 5; i++) {
        nf_sort_key key = NF_SORT_NAME;
        CHECK(nf_menu_sort_key_at(i, &key));
        QString word = nf_sort_key_name(key);
        // the row that selects this key spells the word, and so does the bar
        // label that selecting it produces
        CHECK(nf_menu_row_label(NF_MENU_SORT, i, key, false, NF_FILTER_ALL, nf_view_flags_default()).contains(word));
        CHECK(nf_sort_bar_label(key, false).contains(word));
    }
    for (int i = 0; i < 8; i++) {
        nf_filter_kind filter = NF_FILTER_ALL;
        CHECK(nf_menu_filter_at(i, &filter));
        CHECK(nf_filter_bar_label(filter).contains(nf_filter_name(filter)));
    }
}

// NO COLLAPSIBLE SPACE RUNS, in any label, in any state. These are set as
// PLAIN text today -- but a run of two spaces silently becomes one the moment
// anything renders them as rich text, and the measured width would then stop
// matching the drawn width. That exact mismatch (U+0020 measured, `&nbsp;`
// rendered) is what clipped every row on 2026-09-04, so the shape is barred
// here rather than being relied on not to happen.
static void test_menu_labels_have_no_collapsible_space_runs(void) {
    nf_sort_key keys[] = { NF_SORT_NAME, NF_SORT_SIZE, NF_SORT_DATE,
                           NF_SORT_ADDED, NF_SORT_READ };
    for (int k = 0; k < 5; k++) {
        for (int d = 0; d < 2; d++) {
            for (int i = 0; i < 5; i++) {
                QString s = nf_menu_row_label(NF_MENU_SORT, i, keys[k], d != 0, NF_FILTER_ALL, nf_view_flags_default());
                CHECK(!s.contains(QStringLiteral("  ")));
                CHECK(!s.isEmpty());
            }
        }
    }
    nf_filter_kind filters[] = { NF_FILTER_ALL, NF_FILTER_CBZ, NF_FILTER_CBR,
                                 NF_FILTER_PDF, NF_FILTER_EPUB, NF_FILTER_FINISHED,
                                 NF_FILTER_IN_PROGRESS, NF_FILTER_NOT_STARTED };
    for (int f = 0; f < 8; f++) {
        for (int i = 0; i < 8; i++) {
            QString s = nf_menu_row_label(NF_MENU_FILTER, i, NF_SORT_NAME, false, filters[f], nf_view_flags_default());
            CHECK(!s.contains(QStringLiteral("  ")));
            CHECK(!s.isEmpty());
        }
    }
    // ...and the bar labels, which are set the same way.
    CHECK(!nf_sort_bar_label(NF_SORT_ADDED, true).contains(QStringLiteral("  ")));
    CHECK(!nf_filter_bar_label(NF_FILTER_IN_PROGRESS).contains(QStringLiteral("  ")));
}

// Selecting a DIFFERENT key keeps the direction, which is what makes the
// menu's own active-row text true after the selection: the newly active row
// must carry the same arrow the old one did. Pure restatement of the rule the
// browser implements, pinned here because the browser's half is untestable.
static void test_selecting_a_different_key_keeps_the_direction(void) {
    // descending, active key = size; select `added` (row 3)
    nf_sort_key picked = NF_SORT_NAME;
    CHECK(nf_menu_sort_key_at(3, &picked));
    CHECK(picked == NF_SORT_ADDED);
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_SORT, 3, picked, true, NF_FILTER_ALL, nf_view_flags_default()),
                 "* added (desc) (tap for asc)");
    CHECK_EQ_STR(nf_sort_bar_label(picked, true), "sort: added (desc)");
}

// Same contract as nf_row_suffix/nf_icon_badge: a caller that wants only one
// of the outputs must not have to supply the rest.
static void test_menu_lookups_accept_a_null_output(void) {
    CHECK(nf_menu_sort_key_at(0, NULL));
    CHECK(nf_menu_filter_at(0, NULL));
    CHECK(!nf_menu_sort_key_at(99, NULL));
    CHECK(!nf_menu_filter_at(99, NULL));
}

// Same contract as nf_row_suffix/nf_icon_badge: a caller that wants only one
// of the outputs must not have to supply the rest.
static void test_page_bar_accepts_null_outputs(void) {
    nf_page_bar_labels(1, 4, NULL, NULL, NULL, NULL, NULL);
    QString only;
    nf_page_bar_labels(1, 4, NULL, NULL, &only, NULL, NULL);
    CHECK_EQ_STR(only, "page 2/4");
}


// --- the view flags -----------------------------------------------------

// Field-by-field, because a memcmp over a struct of bools can read padding
// the standard does not require to be zero.
static bool nf_test_view_eq(nf_view_flags a, nf_view_flags b) {
    return a.fullNames      == b.fullNames
        && a.hideExtensions == b.hideExtensions
        && a.hideCovers     == b.hideCovers
        && a.showHidden     == b.showHidden
        && a.showSize       == b.showSize;
}

// THE .bss PROPERTY, made checkable. nfview.cc keeps a copy of this struct at
// FILE SCOPE, which CLAUDE.md allows only for POD with a constant initialiser
// -- and the reason that rule exists (NickelHook's nh_init runs before this
// translation unit's dynamic initialisers, and a file-scope non-POD read there
// is a load through an unconstructed pointer) means a zeroed copy is a state
// to design for, not to rule out. Every default being `false` is what makes a
// zeroed copy read as "the browser as it has always behaved".
//
// The negative control is the second half: a struct with a flag SET must not
// compare equal to the defaults, or the comparison above would be vacuous and
// would pass just as happily against a function that ignored its argument.
static void test_zeroed_view_flags_are_the_defaults(void) {
    nf_view_flags zeroed;
    // Written field by field rather than memset, for the same padding reason
    // as nf_test_view_eq -- and because this is what a .bss copy looks like.
    zeroed.fullNames = zeroed.hideExtensions = zeroed.hideCovers
        = zeroed.showHidden = zeroed.showSize = false;
    CHECK(nf_test_view_eq(zeroed, nf_view_flags_default()));

    // ...and the macro nfview.cc's file-scope copy actually uses agrees with
    // the function every default argument in this project hands out.
    nf_view_flags fromMacro = NF_VIEW_FLAGS_DEFAULT;
    CHECK(nf_test_view_eq(fromMacro, nf_view_flags_default()));

    nf_view_flags changed = nf_view_flags_default();
    changed.hideCovers = true;
    CHECK(!nf_test_view_eq(changed, nf_view_flags_default()));
}

// ONE toggle, ONE field. nfview.cc flips a flag through this pointer and has
// no switch of its own, so a toggle wired to the wrong field here would
// silently make one menu row change another row's setting.
static void test_view_flag_maps_each_toggle_to_its_own_field(void) {
    nf_view_toggle const all[] = { NF_VIEW_FILENAMES, NF_VIEW_EXTENSIONS,
                                   NF_VIEW_COVERS, NF_VIEW_HIDDEN, NF_VIEW_SIZE };
    for (int i = 0; i < 5; i++) {
        nf_view_flags v = nf_view_flags_default();
        bool *flag = nf_view_flag(&v, all[i]);
        CHECK(flag != NULL);
        if (!flag)
            continue;
        *flag = true;
        // Exactly one field moved, and it is the one the OTHER four toggles
        // do not point at.
        int moved = 0;
        for (int k = 0; k < 5; k++) {
            bool *other = nf_view_flag(&v, all[k]);
            if (other && *other)
                moved++;
        }
        CHECK(moved == 1);
    }

    // The refusals. A NULL struct, and a value outside the enum, must both
    // hand back NULL rather than a pointer into something -- the same
    // refuse-rather-than-guess rule as nf_menu_sort_key_at's, and the negative
    // control that makes the five successes above mean something.
    CHECK(nf_view_flag(NULL, NF_VIEW_COVERS) == NULL);
    nf_view_flags v = nf_view_flags_default();
    CHECK(nf_view_flag(&v, (nf_view_toggle)99) == NULL);
}

// The row order this menu documents, pinned the same way the sort and filter
// menus' orders are. Unlike theirs it preserves no cycle a reader has learned
// on hardware -- this menu has no predecessor -- so what it pins is simply
// that the order does not drift between builds under a reader's fingers.
static void test_view_menu_rows_are_the_documented_order(void) {
    CHECK(nf_menu_row_count(NF_MENU_VIEW) == 5);

    nf_view_toggle const want[] = { NF_VIEW_FILENAMES, NF_VIEW_EXTENSIONS,
                                    NF_VIEW_COVERS, NF_VIEW_HIDDEN, NF_VIEW_SIZE };
    for (int i = 0; i < 5; i++) {
        nf_view_toggle got = NF_VIEW_SIZE;
        CHECK(nf_menu_view_toggle_at(i, &got));
        CHECK(got == want[i]);
    }
}

static void test_view_menu_index_out_of_range_refuses_and_writes_nothing(void) {
    nf_view_toggle t = NF_VIEW_COVERS;
    CHECK(!nf_menu_view_toggle_at(-1, &t));
    CHECK(!nf_menu_view_toggle_at(5, &t));
    CHECK(t == NF_VIEW_COVERS); // untouched
    CHECK(nf_menu_view_toggle_at(0, NULL));
    CHECK(!nf_menu_view_toggle_at(99, NULL));
    CHECK(nf_menu_row_label(NF_MENU_VIEW, -1, NF_SORT_NAME, false, NF_FILTER_ALL,
                            nf_view_flags_default()).isEmpty());
    CHECK(nf_menu_row_label(NF_MENU_VIEW, 5, NF_SORT_NAME, false, NF_FILTER_ALL,
                            nf_view_flags_default()).isEmpty());
}

// EVERY VIEW ROW STATES ITS OWN STATE, IN ITS OWN TEXT, in both states. This
// panel has four grey levels and "slightly lighter" reads as "the same", which
// is the same finding that puts "[not in library]" into a row's words -- so a
// toggle whose state lived in styling would be a toggle a reader could not
// read at all.
static void test_view_rows_show_their_state_in_the_text(void) {
    nf_view_flags def = nf_view_flags_default();
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 0, NF_SORT_NAME, false, NF_FILTER_ALL, def),
                 "filenames: truncated");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 1, NF_SORT_NAME, false, NF_FILTER_ALL, def),
                 "extensions: shown");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 2, NF_SORT_NAME, false, NF_FILTER_ALL, def),
                 "covers: on");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 3, NF_SORT_NAME, false, NF_FILTER_ALL, def),
                 "hidden files: hidden");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 4, NF_SORT_NAME, false, NF_FILTER_ALL, def),
                 "size: hidden");

    nf_view_flags on;
    on.fullNames = on.hideExtensions = on.hideCovers = on.showHidden = on.showSize = true;
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 0, NF_SORT_NAME, false, NF_FILTER_ALL, on),
                 "filenames: full");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 1, NF_SORT_NAME, false, NF_FILTER_ALL, on),
                 "extensions: hidden");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 2, NF_SORT_NAME, false, NF_FILTER_ALL, on),
                 "covers: off");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 3, NF_SORT_NAME, false, NF_FILTER_ALL, on),
                 "hidden files: shown");
    CHECK_EQ_STR(nf_menu_row_label(NF_MENU_VIEW, 4, NF_SORT_NAME, false, NF_FILTER_ALL, on),
                 "size: shown");

    // THE NEGATIVE CONTROL: every row's text must actually MOVE when its flag
    // moves. Without this, a label built off the wrong field -- or off no
    // field at all -- would satisfy every equality above for one of the two
    // states and be wrong for the other, and a reader would be told "covers:
    // on" on a screen with no covers on it.
    for (int i = 0; i < 5; i++) {
        QString a = nf_menu_row_label(NF_MENU_VIEW, i, NF_SORT_NAME, false, NF_FILTER_ALL, def);
        QString b = nf_menu_row_label(NF_MENU_VIEW, i, NF_SORT_NAME, false, NF_FILTER_ALL, on);
        CHECK(a != b);
        // ...and the row is still recognisably about the same setting, so the
        // difference is the STATE and not the whole row.
        CHECK(a.section(QLatin1Char(':'), 0, 0) == b.section(QLatin1Char(':'), 0, 0));
    }
}

// NO "* " MARK ON ANY VIEW ROW. The other two menus use it for "this is the
// one in force", which is a question a set of independent toggles does not
// ask -- and one glyph meaning two things on adjacent screens is how a reader
// learns the wrong one. Also: no collapsible space runs, same rule and same
// reason as the sort/filter rows'.
static void test_view_rows_carry_no_active_mark_and_no_space_runs(void) {
    for (int bits = 0; bits < 32; bits++) {
        nf_view_flags v;
        v.fullNames      = (bits & 1)  != 0;
        v.hideExtensions = (bits & 2)  != 0;
        v.hideCovers     = (bits & 4)  != 0;
        v.showHidden     = (bits & 8)  != 0;
        v.showSize       = (bits & 16) != 0;
        for (int i = 0; i < 5; i++) {
            QString row = nf_menu_row_label(NF_MENU_VIEW, i, NF_SORT_NAME, false,
                                            NF_FILTER_ALL, v);
            CHECK(!row.isEmpty());
            CHECK(!row.startsWith(QLatin1Char('*')));
            CHECK(!row.contains(QStringLiteral("  ")));
            CHECK(row.contains(QStringLiteral(": ")));
        }
    }
}

// The bar cannot show five toggles in one slot, so it shows whether ANY of
// them has been changed. The negative control is the whole test: flipping each
// flag ON ITS OWN must reach "custom", or the bar would quietly keep saying
// "default" for a mode that is not the default -- exactly the misattribution
// this feature's logging exists to prevent, one level up.
static void test_view_bar_label_says_custom_for_every_single_flip(void) {
    CHECK_EQ_STR(nf_view_bar_label(nf_view_flags_default()), "view: default");

    nf_view_toggle const all[] = { NF_VIEW_FILENAMES, NF_VIEW_EXTENSIONS,
                                   NF_VIEW_COVERS, NF_VIEW_HIDDEN, NF_VIEW_SIZE };
    for (int i = 0; i < 5; i++) {
        nf_view_flags v = nf_view_flags_default();
        bool *flag = nf_view_flag(&v, all[i]);
        CHECK(flag != NULL);
        if (!flag)
            continue;
        *flag = true;
        CHECK_EQ_STR(nf_view_bar_label(v), "view: custom");
        // ...and flipping it back reaches "default" again, so this is reading
        // the flag rather than latching on the first change it ever saw.
        *flag = false;
        CHECK_EQ_STR(nf_view_bar_label(v), "view: default");
    }
}

// The per-build log line is built out of the MENU'S OWN ROWS, so a screenshot
// and the log can never disagree about what mode the browser was in. Checked
// by containment rather than by restating the words, which would be the second
// spelling this property exists to rule out.
static void test_view_flags_summary_is_the_menu_rows(void) {
    nf_view_flags v = nf_view_flags_default();
    v.hideCovers = true;
    v.showSize   = true;

    QString summary = nf_view_flags_summary(v);
    for (int i = 0; i < 5; i++) {
        CHECK(summary.contains(nf_menu_row_label(NF_MENU_VIEW, i, NF_SORT_NAME, false,
                                                 NF_FILTER_ALL, v)));
    }
    // All five, separated -- not one row and a truncation.
    CHECK(summary.count(QStringLiteral(" | ")) == 4);
    // The negative control: the summary must describe THIS mode, not the
    // default one. "covers: on" is the default row and must be absent.
    CHECK(summary.contains(QStringLiteral("covers: off")));
    CHECK(!summary.contains(QStringLiteral("covers: on")));
    // nh_log truncates at 256 bytes SILENTLY (CLAUDE.md), and nfview.cc adds a
    // prefix and an items/page tail to this, so a summary anywhere near that
    // limit would lose the last rows without saying so.
    CHECK(summary.length() < 160);
}

// --- the page size, per mode --------------------------------------------

// BOTH MODES' ARITHMETIC, restated as bounds rather than as the two answers
// alone: the answers on their own would pass against a function that returned
// two hardcoded numbers unrelated to the geometry, which is precisely the
// thing this project has been burned by (a page size of 14, then 12, read off
// a screenshot's margin rather than counted).
static void test_items_per_page_for_both_modes(void) {
    int const chrome = NF_CHROME_BARS * NF_CHROME_BAR_PX;   // 150
    int const avail  = NF_CONTENT_AREA_PX - chrome;         // 1180
    int const coverRow = NF_COVER_H_PX + NF_FONT_DESCENT_PX; // 99, since the cover is taller than the ascent

    // The measured identity the text-row constant rests on: a text row is the
    // font's ascent plus its descent, which is also why a cover no taller than
    // the ascent would cost nothing. A re-measurement that broke this would
    // otherwise be absorbed silently.
    CHECK(NF_FONT_ASCENT_PX + NF_FONT_DESCENT_PX == NF_TEXT_ROW_PX);
    CHECK(NF_COVER_H_PX > NF_FONT_ASCENT_PX);

    int withCovers = nf_items_per_page(true);
    CHECK(withCovers == 11);
    CHECK(withCovers * coverRow + chrome <= NF_CONTENT_AREA_PX);        // 1239 <= 1330
    CHECK((withCovers + 1) * coverRow + chrome > NF_CONTENT_AREA_PX);   // 1338 >  1330

    int noCovers = nf_items_per_page(false);
    CHECK(noCovers == 15);
    CHECK(noCovers * NF_TEXT_ROW_PX + chrome <= NF_CONTENT_AREA_PX);      // 1275 <= 1330
    CHECK((noCovers + 1) * NF_TEXT_ROW_PX + chrome > NF_CONTENT_AREA_PX); // 1350 >  1330

    // THE NEGATIVE CONTROL: the two modes must differ. A function that ignored
    // its argument would satisfy one of the two exact answers above and, if
    // both happened to be written to the same number, every bound as well.
    CHECK(withCovers != noCovers);
    CHECK(noCovers > withCovers);
    // Taller rows can never fit MORE of themselves.
    CHECK(avail / coverRow <= avail / NF_TEXT_ROW_PX);
}

// --- human-readable sizes -----------------------------------------------

// The unit boundaries, both sides of each, plus the rounding. Written as
// literals rather than as expressions over the same constants the function
// uses, so a changed divisor fails here instead of following along.
static void test_format_size_units(void) {
    CHECK_EQ_STR(nf_format_size(0),    QString(QStringLiteral("0%1B")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(512),  QString(QStringLiteral("512%1B")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(1023), QString(QStringLiteral("1023%1B")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(1024), QString(QStringLiteral("1.0%1KB")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(1536), QString(QStringLiteral("1.5%1KB")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(1024LL * 1024),      QString(QStringLiteral("1.0%1MB")).arg(nf_nbsp()).toUtf8().constData());
    CHECK_EQ_STR(nf_format_size(1024LL * 1024 * 1024), QString(QStringLiteral("1.0%1GB")).arg(nf_nbsp()).toUtf8().constData());

    // The reference card's own shape: a ~12 MB comic archive.
    CHECK_EQ_STR(nf_format_size(12345678), QString(QStringLiteral("11.8%1MB")).arg(nf_nbsp()).toUtf8().constData());

    // The rounding CARRY. 1024*1024 - 1 rounds to 1024.0 KB at one decimal
    // place, which must read as "1.0 MB" and never as "1024.0 KB": an integer
    // rounding that carried out of the fraction and was not renormalised is
    // exactly the kind of arithmetic slip that looks right on every other
    // input.
    CHECK_EQ_STR(nf_format_size(1024LL * 1024 - 1), QString(QStringLiteral("1.0%1MB")).arg(nf_nbsp()).toUtf8().constData());

    // A size far past anything on this card, to show nothing overflows on the
    // way: ~8 EB is near qint64's ceiling.
    CHECK(!nf_format_size(8000000000000000000LL).isEmpty());
    CHECK(nf_format_size(8000000000000000000LL).endsWith(QStringLiteral("GB")));
}

// The space before the unit is a NON-BREAKING one, for the same reason every
// other separator in nffmt.cc is: an ordinary space is a wrap opportunity and
// a collapsible run, so the string would render at a width the plain twin did
// not measure -- which is what clipped every row on 2026-09-04. The negative
// control is the second line: an ASCII space must not appear at all.
static void test_format_size_uses_a_non_breaking_space(void) {
    QString s = nf_format_size(12345678);
    CHECK(s.contains(nf_nbsp()));
    CHECK(!s.contains(QLatin1Char(' ')));
}

// A negative size is refused, not clamped -- the same refuse-rather-than-
// repair rule as nf_date_key_is_plausible's. Rendering it as "0 B" would hide
// whatever produced it behind a plausible-looking answer, and the caller
// (nf_row_suffix) simply omits an empty size.
static void test_format_size_refuses_a_negative(void) {
    CHECK(nf_format_size(-1).isEmpty());
    CHECK(nf_format_size(-1024).isEmpty());
    // ...and 0 is NOT refused: a zero-byte file is a real thing to find on a
    // card, and this browser's job is to say what is there.
    CHECK(!nf_format_size(0).isEmpty());
}

static nf_row nf_test_sized_row(bool isDir, bool hasRow, bool finished,
                                int percentRead, qint64 size) {
    nf_row r = nf_test_row(isDir, hasRow, finished, percentRead);
    r.size = size;
    return r;
}

// The size is an ADDITION to the state marker, never a replacement for it: a
// file can be both 11.8 MB and [not in library], and the state marker is what
// the row MEANS. It also has to be in the PLAIN twin, because that twin is
// what the row's elision reserve is measured off (nfview.cc) -- a size drawn
// without being paid for is the one-or-two characters of clipping NOTES.md
// Task 13 records.
static void test_row_suffix_shows_the_size_only_when_asked(void) {
    nf_view_flags off = nf_view_flags_default();
    nf_view_flags on  = nf_view_flags_default();
    on.showSize = true;

    nf_row const rows[] = {
        nf_test_sized_row(false, false, false, -1, 12345678), // no library row
        nf_test_sized_row(false, true,  true,   0, 12345678), // finished
        nf_test_sized_row(false, true,  false, 40, 12345678), // (40%)
        nf_test_sized_row(false, true,  false,  0, 12345678), // unread: no state marker
    };
    for (int i = 0; i < 4; i++) {
        QString offMarkup, offPlain, onMarkup, onPlain;
        nf_row_suffix(rows[i], off, &offMarkup, &offPlain);
        nf_row_suffix(rows[i], on,  &onMarkup,  &onPlain);

        // Hidden by default -- the default must be today's behaviour.
        CHECK(!offPlain.contains(QStringLiteral("MB")));
        // Shown when asked, and the STATE MARKER SURVIVES: the on form is the
        // off form with the size inserted, never the size instead of it.
        CHECK(onPlain.contains(QStringLiteral("11.8")));
        CHECK(onPlain.endsWith(offPlain));
        // Paid for: the reserve is measured off the plain twin, and it has to
        // have grown by the size or the row would draw text it did not budget.
        CHECK(onPlain.length() > offPlain.length());
        // The twin invariant still holds with the size in it.
        CHECK(nf_test_rendered(onMarkup) == onPlain);
        CHECK(!onPlain.contains(QLatin1Char('&')));
    }
}

// A FOLDER NEVER CARRIES A SIZE, even with the toggle on: a directory's
// stat()'d size is a filesystem block size and not the sum of what is inside
// it, so any number here would be a confident wrong answer -- the one kind
// this project refuses to render.
static void test_folder_rows_never_carry_a_size(void) {
    nf_view_flags on = nf_view_flags_default();
    on.showSize = true;

    QString plain;
    nf_row_suffix(nf_test_sized_row(true, false, false, -1, 4096), on, NULL, &plain);
    CHECK_EQ_STR(plain, "/");

    // The negative control: the same flags on a FILE of the same size do
    // produce a size, so the silence above is the folder rule and not the
    // toggle failing to arrive.
    QString filePlain;
    nf_row_suffix(nf_test_sized_row(false, true, false, 0, 4096), on, NULL, &filePlain);
    CHECK(filePlain.contains(QStringLiteral("4.0")));
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
    test_cover_mangling_matches_the_device_vector();
    test_cover_mangling_touches_only_four_characters();
    test_cover_mangling_is_idempotent();
    test_cover_bucket_matches_the_device_vector();
    test_cover_path_matches_the_device_file();
    test_cover_bucket_rejects_the_near_miss_manglings();
    test_cover_bucket_hashes_code_units_not_bytes();
    test_cover_path_refuses_an_empty_image_id();
    test_cover_path_inserts_the_id_verbatim();
    test_cover_width_keeps_the_native_aspect();
    test_page_bar_middle_page_has_both_ends();
    test_page_bar_inert_ends_render_as_nothing();
    test_page_bar_active_ends_are_the_only_ones_with_an_arrow();
    test_page_bar_first_and_last_page();
    test_page_bar_clamps_a_nonsense_page();
    test_page_bar_accepts_null_outputs();
    test_sort_menu_rows_are_the_old_cycle_order();
    test_filter_menu_rows_are_the_old_cycle_order();
    test_menu_index_out_of_range_refuses_and_writes_nothing();
    test_menu_marks_exactly_one_active_row();
    test_inactive_menu_rows_are_the_bare_word();
    test_active_sort_row_shows_the_direction_and_the_tap();
    test_the_bar_and_the_menu_share_one_vocabulary();
    test_menu_labels_have_no_collapsible_space_runs();
    test_selecting_a_different_key_keeps_the_direction();
    test_menu_lookups_accept_a_null_output();
    test_zeroed_view_flags_are_the_defaults();
    test_view_flag_maps_each_toggle_to_its_own_field();
    test_view_menu_rows_are_the_documented_order();
    test_view_menu_index_out_of_range_refuses_and_writes_nothing();
    test_view_rows_show_their_state_in_the_text();
    test_view_rows_carry_no_active_mark_and_no_space_runs();
    test_view_bar_label_says_custom_for_every_single_flip();
    test_view_flags_summary_is_the_menu_rows();
    test_items_per_page_for_both_modes();
    test_format_size_units();
    test_format_size_uses_a_non_breaking_space();
    test_format_size_refuses_a_negative();
    test_row_suffix_shows_the_size_only_when_asked();
    test_folder_rows_never_carry_a_size();
    NF_TEST_MAIN_END
}
