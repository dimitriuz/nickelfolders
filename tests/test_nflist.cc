#include "nftest.h"
#include "nflist.h"
#include <QVector>

// A fake metadata lookup. Names beginning "missing" have no database row,
// which is how the greyed-row state is exercised without a device.
static void fake_meta(void *ctx, QString const& name, nf_row *row) {
    int *calls = static_cast<int *>(ctx);
    if (calls)
        (*calls)++;
    row->hasRow      = !name.startsWith(QStringLiteral("missing"));
    row->percentRead = row->hasRow ? 42 : -1;
    row->finished    = false;
}

static nf_entry ent(char const *name, bool isDir) {
    nf_entry e;
    e.name  = QString::fromUtf8(name);
    e.isDir = isDir;
    return e;
}

static void test_junk_is_dropped_before_anything_else(void) {
    QVector<nf_entry> e;
    e << ent("books", true) << ent(".kobo", true)
      << ent("calibrewebdownload2055pdf2055.sdr", true)
      << ent("Volume 1.cbz", false) << ent("metadata.calibre", false)
      << ent("koboy-probe-Io.txt", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 2);
    CHECK_EQ_STR(out.at(0).name, "books");
    CHECK_EQ_STR(out.at(1).name, "Volume 1.cbz");
}

// Metadata is fetched for EVERY row, not lazily for the visible ones. Spec
// section 6.2: a future sort by date added or percent read needs the field for
// every row before the sort runs, so a lazy fetch would have to be undone.
static void test_metadata_is_fetched_for_every_row(void) {
    QVector<nf_entry> e;
    for (int v = 1; v <= 27; v++)
        e << ent(QString("Volume %1.cbz").arg(v).toUtf8().constData(), false);
    QVector<nf_row> out;
    int calls = 0;
    nf_build_listing(e, fake_meta, &calls, &out);
    CHECK(out.size() == 27);
    CHECK(calls == 27);
}

// Directories get no metadata lookup: there is no Volume for a folder, and
// asking would cost 27 pointless libnickel calls on the reference card.
static void test_directories_are_not_looked_up(void) {
    QVector<nf_entry> e;
    e << ent("Comics", true) << ent("Sandman", true);
    QVector<nf_row> out;
    int calls = 0;
    nf_build_listing(e, fake_meta, &calls, &out);
    CHECK(out.size() == 2);
    CHECK(calls == 0);
    CHECK(out.at(0).hasRow == false);
    CHECK(out.at(0).percentRead == -1);
}

static void test_missing_row_is_kept_and_marked(void) {
    QVector<nf_entry> e;
    e << ent("Volume 25.cbz", false) << ent("missing Volume 26.cbz", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 2);
    bool found = false;
    for (int i = 0; i < out.size(); i++) {
        if (out.at(i).name.startsWith(QStringLiteral("missing"))) {
            found = true;
            CHECK(out.at(i).hasRow == false);
        }
    }
    CHECK(found);
}

// THE ORDER TEST. Labels are derived after filtering, so a common prefix that
// only existed among the junk cannot strip text off the survivors. Here the
// three "Foo - " names include two that the allowlist drops; if labelling ran
// before filtering it would see a common run and strip it.
static void test_labels_are_derived_after_filtering(void) {
    QVector<nf_entry> e;
    e << ent("Foo - Keep.cbz", false)
      << ent("Foo - Drop.calibre", false)
      << ent("Foo - Also drop.lua", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 1);
    // One surviving row: nf_strip_common refuses on a single row, so the label
    // is the full name. Had labelling run first, it would read "Keep.cbz".
    CHECK_EQ_STR(out.at(0).label, "Foo - Keep.cbz");
}

// Ordering happens before labelling, so labels line up with the rows they
// belong to rather than with the pre-sort positions.
static void test_labels_match_their_rows_after_sorting(void) {
    QVector<nf_entry> e;
    e << ent("Fullmetal Alchemist v27 (2011) (Digital).cbz", false)
      << ent("Fullmetal Alchemist v01 (2005) (Digital).cbz", false)
      << ent("Fullmetal Alchemist v09 (2006) (Digital).cbz", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 3);
    CHECK(out.at(0).name.contains(QStringLiteral("v01")));
    CHECK(out.at(0).label.contains(QStringLiteral("v01")));
    CHECK(out.at(2).name.contains(QStringLiteral("v27")));
    CHECK(out.at(2).label.contains(QStringLiteral("v27")));
}

// Folders and files are labelled as SEPARATE sets. A folder name and a book
// name share nothing useful, and pooling them would find a common run of ""
// and strip nothing -- which silently disables the feature in any folder
// holding both kinds. Sandman holds 11 folders and 3 files, so this is the
// common case on the reference card, not an edge one.
static void test_folders_and_files_are_labelled_separately(void) {
    QVector<nf_entry> e;
    // Zero-padded on purpose. With "Volume 1" / "Volume 2" the remainder is a
    // SINGLE character, and Task 2's refuse-under-two-characters guard then
    // correctly declines to strip -- so the unpadded form tests that guard, not
    // the separation. Traced by hand before this plan was written.
    //
    // Trailing "a"/"b" on purpose too, added for the label-guard test below:
    // a bare "01"/"02" remainder has no letter, so nf_strip_common's newer
    // "refuse if a label has no letter" rule (added for the Steven L. Kent
    // root-of-card case, test_nffmt.cc) would now decline to strip THIS
    // fixture as well -- which is correct behaviour for that guard, but
    // would leave this test unable to demonstrate labelling actually
    // happening separately per kind. "01a"/"02b" keeps a letter in the
    // remainder so this test still exercises separation, not the guard.
    e << ent("Series - Volume 01a.cbz", false)
      << ent("Series - Volume 02b.cbz", false)
      << ent("Extras", true);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 3);
    CHECK(out.at(0).isDir);
    CHECK_EQ_STR(out.at(0).label, "Extras");
    CHECK_EQ_STR(out.at(1).label, "01a");
    CHECK_EQ_STR(out.at(2).label, "02b");
}

// The collision guard, with a fixture that genuinely reaches it. Stripping
// these leaves "zz" for BOTH rows: with no space before the bracket the prefix
// backs off to the earlier whitespace, so the remainder keeps "zz(a"/"zz(b",
// and the per-row truncation at the unclosed "(" collapses both to "zz" -- two
// characters, so the post-loop length guard does not catch it either.
//
// Measured on the host against nf_strip_common directly:
//     ["Vol zz(a", "Vol zz(b"] -> [zz] [zz]
//
// Unique filenames must stay distinguishable, so the listing keeps raw names.
static void test_colliding_labels_fall_back_to_raw_names(void) {
    QVector<nf_entry> e;
    e << ent("Vol zz(a.cbz", false) << ent("Vol zz(b.cbz", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 2);
    CHECK_EQ_STR(out.at(0).label, "Vol zz(a.cbz");
    CHECK_EQ_STR(out.at(1).label, "Vol zz(b.cbz");
}

// The filter stage, section 6.1/6.3. Folders survive regardless of `filter`
// -- see nf_build_listing's own comment (nflist.cc) for why -- while files
// that do not match are removed same as junk, just one stage later.
static void test_filter_removes_non_matching_files_but_keeps_folders(void) {
    QVector<nf_entry> e;
    e << ent("Comics", true)
      << ent("Volume 1.cbz", false)
      << ent("Report.pdf", false)
      << ent("Notes.epub", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out, NF_FILTER_PDF);
    CHECK(out.size() == 2); // the folder, and Report.pdf
    bool sawFolder = false, sawPdf = false;
    for (int i = 0; i < out.size(); i++) {
        if (out.at(i).isDir) {
            sawFolder = true;
            CHECK_EQ_STR(out.at(i).name, "Comics");
        } else {
            sawPdf = true;
            CHECK_EQ_STR(out.at(i).name, "Report.pdf");
        }
    }
    CHECK(sawFolder);
    CHECK(sawPdf);
}

// Filtering to NF_FILTER_ALL is a no-op -- same row set as no filter at all.
static void test_filter_all_is_a_no_op(void) {
    QVector<nf_entry> e;
    e << ent("Volume 1.cbz", false) << ent("Report.pdf", false);
    QVector<nf_row> unfiltered, allFiltered;
    nf_build_listing(e, fake_meta, NULL, &unfiltered);
    nf_build_listing(e, fake_meta, NULL, &allFiltered, NF_FILTER_ALL);
    CHECK(unfiltered.size() == allFiltered.size());
    CHECK(unfiltered.size() == 2);
}

// Labels are recomputed after the FILTER stage, same load-bearing ordering
// test_labels_are_derived_after_filtering (above) pins for the junk-hiding
// stage, and the task brief calls out as the reason a filter needs its own
// version of that test: a common run computed on the PRE-filter set of three
// names is not the same common run as on the two PDFs that actually survive.
static void test_labels_recomputed_after_filter(void) {
    QVector<nf_entry> e;
    // All three share "Book - " as a prefix, but ONLY the two PDFs also share
    // the trailing " (PDF).pdf" -- Notes.epub does not, so if labels were
    // derived from the PRE-filter set of three, the common SUFFIX would be
    // empty (Notes.epub does not end in " (PDF).pdf") and only the prefix
    // "Book - " would strip. Derived AFTER filtering the epub out, the
    // remaining two PDFs share both the prefix AND the suffix, so the
    // stripped label is shorter -- a directly observable difference between
    // the two orderings, not just "does it happen to match either way".
    e << ent("Book - Alpha (PDF).pdf", false)
      << ent("Book - Beta (PDF).pdf", false)
      << ent("Book - Gamma.epub", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out, NF_FILTER_PDF);
    CHECK(out.size() == 2);
    for (int i = 0; i < out.size(); i++) {
        // Had labelling run on the pre-filter set of three, the shared
        // suffix would be "" (Gamma.epub breaks it) and "(PDF)" would
        // survive in the label. Derived after filtering, the suffix
        // " (PDF).pdf" is common to the two PDF survivors and is stripped.
        CHECK(!out.at(i).label.contains(QStringLiteral("(PDF)")));
    }
    CHECK(out.at(0).label.contains(QStringLiteral("Alpha")));
    CHECK(out.at(1).label.contains(QStringLiteral("Beta")));
}

// The third empty state, section 6.3/3.6. A folder that HAD books before the
// filter ran, all of which the filter then removed, must be flagged distinct
// from a folder that never had anything -- see nf_build_listing's own
// derivation of `filteredToNothing` (nflist.cc) for why this is checked
// before the filter runs rather than after.
static void test_filtered_to_nothing_is_distinguishable_from_empty(void) {
    // Case 1: a manga folder, all .cbz, filtered to PDF -- everything here
    // WAS a book, and the filter is why none of it shows.
    QVector<nf_entry> manga;
    manga << ent("Volume 1.cbz", false) << ent("Volume 2.cbz", false);
    QVector<nf_row> out1;
    bool filteredToNothing1 = false;
    nf_build_listing(manga, fake_meta, NULL, &out1,
                     NF_FILTER_PDF, NF_SORT_NAME, false, &filteredToNothing1);
    CHECK(out1.isEmpty());
    CHECK(filteredToNothing1);

    // Case 2: a genuinely empty directory (nothing survives hide-junk) --
    // the SAME filter must not claim credit for an emptiness it had nothing
    // to do with.
    QVector<nf_entry> empty;
    QVector<nf_row> out2;
    bool filteredToNothing2 = true; // deliberately pre-set to the WRONG value
    nf_build_listing(empty, fake_meta, NULL, &out2,
                     NF_FILTER_PDF, NF_SORT_NAME, false, &filteredToNothing2);
    CHECK(out2.isEmpty());
    CHECK(!filteredToNothing2);

    // Case 3: an all-junk directory (hide-junk removes everything, same as
    // case 2 from the filter's point of view) -- also not "filtered to
    // nothing", for the same reason as case 2.
    QVector<nf_entry> allJunk;
    allJunk << ent("metadata.calibre", false) << ent(".kobo", true);
    QVector<nf_row> out3;
    bool filteredToNothing3 = true;
    nf_build_listing(allJunk, fake_meta, NULL, &out3,
                     NF_FILTER_PDF, NF_SORT_NAME, false, &filteredToNothing3);
    CHECK(out3.isEmpty());
    CHECK(!filteredToNothing3);

    // Case 4: a folder holding both a folder and files, filtered so only the
    // files drop out -- there is still something to show (the folder), so
    // this must NOT read as "filtered to nothing" even though every FILE
    // was removed.
    QVector<nf_entry> mixed;
    mixed << ent("Extras", true) << ent("Volume 1.cbz", false);
    QVector<nf_row> out4;
    bool filteredToNothing4 = true;
    nf_build_listing(mixed, fake_meta, NULL, &out4,
                     NF_FILTER_PDF, NF_SORT_NAME, false, &filteredToNothing4);
    CHECK(out4.size() == 1);
    CHECK(!filteredToNothing4);

    // Case 5 (negative control): NF_FILTER_ALL never removes a match, so
    // even a folder full of real books must never report filteredToNothing
    // -- a passing case-1 check is only meaningful because this one does
    // NOT also come back true.
    QVector<nf_row> out5;
    bool filteredToNothing5 = true;
    nf_build_listing(manga, fake_meta, NULL, &out5,
                     NF_FILTER_ALL, NF_SORT_NAME, false, &filteredToNothing5);
    CHECK(out5.size() == 2);
    CHECK(!filteredToNothing5);
}

int main(void) {
    test_junk_is_dropped_before_anything_else();
    test_metadata_is_fetched_for_every_row();
    test_directories_are_not_looked_up();
    test_missing_row_is_kept_and_marked();
    test_labels_are_derived_after_filtering();
    test_labels_match_their_rows_after_sorting();
    test_folders_and_files_are_labelled_separately();
    test_colliding_labels_fall_back_to_raw_names();
    test_filter_removes_non_matching_files_but_keeps_folders();
    test_filter_all_is_a_no_op();
    test_labels_recomputed_after_filter();
    test_filtered_to_nothing_is_distinguishable_from_empty();
    NF_TEST_MAIN_END
}
