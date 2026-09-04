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

int main(void) {
    test_junk_is_dropped_before_anything_else();
    test_metadata_is_fetched_for_every_row();
    test_directories_are_not_looked_up();
    test_missing_row_is_kept_and_marked();
    test_labels_are_derived_after_filtering();
    test_labels_match_their_rows_after_sorting();
    test_folders_and_files_are_labelled_separately();
    test_colliding_labels_fall_back_to_raw_names();
    NF_TEST_MAIN_END
}
