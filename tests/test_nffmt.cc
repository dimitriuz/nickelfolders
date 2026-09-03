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
    NF_TEST_MAIN_END
}
