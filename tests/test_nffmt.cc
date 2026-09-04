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
    test_hidden_dirs();
    NF_TEST_MAIN_END
}
