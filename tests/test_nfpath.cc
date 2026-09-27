// Host tests for the path-safety guards (nfpath.h). This is the most
// important test file in the project: these predicates are the only thing
// standing between a mis-tap and the owner's book library, and there is no
// undo anywhere in the design they defend.
//
// EVERY RULE IS TESTED FROM BOTH SIDES. A guard that refuses everything
// passes a one-sided suite perfectly and ships as "file operations do not
// work"; a guard that allows everything passes the other half. So for each
// "this is refused" there is a "this legitimate neighbour is NOT refused",
// written next to it, and the negative control is named in the test's own
// name wherever it is the point of the test rather than a detail of it.
#include "nftest.h"
#include "nfpath.h"

static QString S(char const *s) { return QString::fromUtf8(s); }

// --- nf_path_is_clean ---------------------------------------------------

static void test_clean_accepts_ordinary_absolute_paths(void) {
    CHECK(nf_path_is_clean(S("/mnt/onboard")));
    CHECK(nf_path_is_clean(S("/mnt/onboard/books")));
    CHECK(nf_path_is_clean(S("/mnt/onboard/books/Some Book.epub")));
    CHECK(nf_path_is_clean(S("/")));
    // Names containing dots, and even "..", as SUBSTRINGS are ordinary --
    // "Vol..2" is a name a card really can hold, and a substring test for
    // ".." would refuse it while catching nothing a component test misses.
    CHECK(nf_path_is_clean(S("/mnt/onboard/books/Vol..2.epub")));
    CHECK(nf_path_is_clean(S("/mnt/onboard/books/...epub")));
    CHECK(nf_path_is_clean(S("/mnt/onboard/.kobo")));
}

static void test_clean_refuses_the_shapes_that_make_a_guard_meaningless(void) {
    CHECK(!nf_path_is_clean(QString()));            // empty
    CHECK(!nf_path_is_clean(S("")));                // empty, spelled the other way
    CHECK(!nf_path_is_clean(S("mnt/onboard")));     // relative
    CHECK(!nf_path_is_clean(S("books/x.epub")));    // relative
    CHECK(!nf_path_is_clean(S("/mnt/onboard/")));   // trailing slash
    CHECK(!nf_path_is_clean(S("/mnt//onboard")));   // empty component
    CHECK(!nf_path_is_clean(S("/mnt/onboard/..")));
    CHECK(!nf_path_is_clean(S("/mnt/onboard/../etc")));
    CHECK(!nf_path_is_clean(S("/mnt/onboard/./books")));
    CHECK(!nf_path_is_clean(S("/mnt/onboard/books/..")));
    CHECK(!nf_path_is_clean(S("/..")));
    CHECK(!nf_path_is_clean(S("/.")));
}

// --- nf_path_is_inside: THE classic bug ---------------------------------

static void test_a_string_prefix_is_not_a_path_prefix(void) {
    // The whole reason this function exists rather than a startsWith().
    // "/mnt/onboard/books2" begins with "/mnt/onboard/books" as a STRING and
    // is not inside it as a PATH -- and getting this wrong would let a
    // "refuse the current folder" rule refuse a sibling, or, far worse, let a
    // "stay inside /mnt/onboard" rule pass "/mnt/onboardX".
    CHECK(!nf_path_is_inside(S("/mnt/onboard/books2"), S("/mnt/onboard/books")));
    CHECK(!nf_path_is_inside(S("/mnt/onboard/books2/a.epub"), S("/mnt/onboard/books")));
    CHECK(!nf_path_is_inside(S("/mnt/onboardX"), S("/mnt/onboard")));
    CHECK(!nf_path_is_inside(S("/mnt/onboardX/books"), S("/mnt/onboard")));
    // ...and the negative control for the negative control: the real child
    // IS inside, so this is not a function that simply says no.
    CHECK(nf_path_is_inside(S("/mnt/onboard/books/a.epub"), S("/mnt/onboard/books")));
    CHECK(nf_path_is_inside(S("/mnt/onboard/books"), S("/mnt/onboard")));
    CHECK(nf_path_is_inside(S("/mnt/onboard/a/b/c"), S("/mnt/onboard")));
}

static void test_inside_is_strict_and_refuses_unclean_arguments(void) {
    CHECK(!nf_path_is_inside(S("/mnt/onboard"), S("/mnt/onboard"))); // strictly below
    CHECK(!nf_path_is_inside(S("/mnt/onboard/books/"), S("/mnt/onboard"))); // trailing slash
    CHECK(!nf_path_is_inside(S("/mnt/onboard/../etc"), S("/mnt/onboard")));
    CHECK(!nf_path_is_inside(S("books"), S("/mnt/onboard")));
    CHECK(!nf_path_is_inside(S("/mnt/onboard/books"), S("onboard")));
    CHECK(!nf_path_is_inside(QString(), S("/mnt/onboard")));
    CHECK(!nf_path_is_inside(S("/mnt/onboard/books"), QString()));
    // The bare root is a directory like any other, and its own separator must
    // not be doubled.
    CHECK(nf_path_is_inside(S("/mnt"), S("/")));
}

static void test_ancestor_or_self_includes_self_and_nothing_sideways(void) {
    CHECK(nf_path_is_ancestor_or_self(S("/mnt/onboard"), S("/mnt/onboard")));
    CHECK(nf_path_is_ancestor_or_self(S("/mnt/onboard"), S("/mnt/onboard/books/a.epub")));
    CHECK(nf_path_is_ancestor_or_self(S("/mnt/onboard/books"), S("/mnt/onboard/books/a.epub")));
    // Not an ancestor: a child, a sibling, and the string-prefix trap again.
    CHECK(!nf_path_is_ancestor_or_self(S("/mnt/onboard/books/a.epub"), S("/mnt/onboard/books")));
    CHECK(!nf_path_is_ancestor_or_self(S("/mnt/onboard/comics"), S("/mnt/onboard/books")));
    CHECK(!nf_path_is_ancestor_or_self(S("/mnt/onboard/books"), S("/mnt/onboard/books2")));
    CHECK(!nf_path_is_ancestor_or_self(S("/mnt/onboard/books2"), S("/mnt/onboard/books")));
}

// --- nf_path_is_protected -----------------------------------------------

static void test_the_four_protected_directories_are_protected(void) {
    CHECK(nf_path_is_protected(S("/mnt/onboard/.kobo")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.kobo/KoboReader.sqlite")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.kobo/version")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.kobo-images")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.kobo-images/12/34/x - N3_LIBRARY_GRID.parsed")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.adds")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.adds/nm/config")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/System Volume Information")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/System Volume Information/IndexerVolumeGuid")));
    // At ANY depth -- the conservative direction, and stated as such in
    // nfpath.h. A ".adds" three levels down is someone's mod unpacked in the
    // wrong place, not a book folder.
    CHECK(nf_path_is_protected(S("/mnt/onboard/books/.adds")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/a/b/.kobo/c")));
}

static void test_protected_is_case_insensitive_because_the_card_is_vfat(void) {
    CHECK(nf_path_is_protected(S("/mnt/onboard/.KOBO")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.Kobo-Images")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/.ADDS")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/SYSTEM VOLUME INFORMATION")));
    CHECK(nf_path_is_protected(S("/mnt/onboard/system volume information/x")));
}

static void test_protected_does_not_over_reach_onto_ordinary_names(void) {
    // THE NEGATIVE CONTROL, and it is the half that decides whether this is a
    // guard or a wall. Each of these SHARES CHARACTERS with a protected name
    // and is an ordinary folder a reader may genuinely want to delete.
    CHECK(!nf_path_is_protected(S("/mnt/onboard/books")));
    CHECK(!nf_path_is_protected(S("/mnt/onboard/adds")));          // no leading dot
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.addsx")));        // longer
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.add")));          // shorter
    CHECK(!nf_path_is_protected(S("/mnt/onboard/kobo")));          // no leading dot
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.kobold")));       // shares a prefix
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.kobo2")));
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.kobo-image")));   // singular
    CHECK(!nf_path_is_protected(S("/mnt/onboard/.kobo-imagesx")));
    CHECK(!nf_path_is_protected(S("/mnt/onboard/System Volume Information Backup")));
    CHECK(!nf_path_is_protected(S("/mnt/onboard/System Volume")));
    // A FILE whose name merely contains one of them is not protected either.
    CHECK(!nf_path_is_protected(S("/mnt/onboard/books/about .kobo internals.epub")));
}

// --- nf_path_name_is_safe -----------------------------------------------

static void test_a_component_with_a_slash_or_dots_is_not_safe(void) {
    CHECK(nf_path_name_is_safe(S("Some Book.epub")));
    CHECK(nf_path_name_is_safe(S(".hidden")));
    CHECK(nf_path_name_is_safe(S("...")));       // three dots is an ordinary name
    CHECK(nf_path_name_is_safe(S("a b  c.cbz")));
    CHECK(!nf_path_name_is_safe(QString()));
    CHECK(!nf_path_name_is_safe(S("")));
    CHECK(!nf_path_name_is_safe(S(".")));
    CHECK(!nf_path_name_is_safe(S("..")));
    CHECK(!nf_path_name_is_safe(S("a/b")));      // the escape with no ".." in it
    CHECK(!nf_path_name_is_safe(S("/abs")));
    CHECK(!nf_path_name_is_safe(S("../up")));
}

// --- nf_path_check_source ------------------------------------------------

static void test_source_check_allows_an_ordinary_book_in_an_ordinary_folder(void) {
    // THE NEGATIVE CONTROL FOR THE WHOLE GUARD. If this ever starts failing,
    // file operations are simply broken, and every "it refuses X" test below
    // would still pass.
    CHECK(nf_path_check_source(S("/mnt/onboard/books/Some Book.epub"),
                               S("/mnt/onboard/books")) == NF_PATH_OK);
    CHECK(nf_path_check_source(S("/mnt/onboard/books/sub"),
                               S("/mnt/onboard/books")) == NF_PATH_OK);
    CHECK(nf_path_check_source(S("/mnt/onboard/a/b/c/x.pdf"),
                               S("/mnt/onboard/a/b/c")) == NF_PATH_OK);
    // A sibling of the current folder is fine -- only the folder itself and
    // its ancestors are off limits.
    CHECK(nf_path_check_source(S("/mnt/onboard/comics"),
                               S("/mnt/onboard/books")) == NF_PATH_OK);
    // ...including the sibling whose name is a string prefix of the cwd's.
    CHECK(nf_path_check_source(S("/mnt/onboard/books2"),
                               S("/mnt/onboard/books")) == NF_PATH_OK);
}

static void test_source_check_refuses_everything_outside_the_root(void) {
    CHECK(nf_path_check_source(S("/etc/passwd"), S("/mnt/onboard")) == NF_PATH_OUTSIDE_ROOT);
    CHECK(nf_path_check_source(S("/mnt/sd/books/x.epub"), S("/mnt/onboard")) == NF_PATH_OUTSIDE_ROOT);
    CHECK(nf_path_check_source(S("/mnt/onboardX/x.epub"), S("/mnt/onboard")) == NF_PATH_OUTSIDE_ROOT);
    CHECK(nf_path_check_source(S("/usr/local/Kobo/libnickel.so.1.0.0"), S("/mnt/onboard")) == NF_PATH_OUTSIDE_ROOT);
}

static void test_source_check_refuses_the_root_itself(void) {
    CHECK(nf_path_check_source(S("/mnt/onboard"), S("/mnt/onboard")) == NF_PATH_IS_ROOT);
    // ...and it is refused as the root, not as an ancestor, even when the
    // reader is several folders down -- the more specific answer wins.
    CHECK(nf_path_check_source(S("/mnt/onboard"), S("/mnt/onboard/a/b")) == NF_PATH_IS_ROOT);
}

static void test_source_check_refuses_the_protected_directories(void) {
    CHECK(nf_path_check_source(S("/mnt/onboard/.kobo"), S("/mnt/onboard")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_source(S("/mnt/onboard/.kobo/KoboReader.sqlite"), S("/mnt/onboard/.kobo")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_source(S("/mnt/onboard/.adds/koreader"), S("/mnt/onboard/.adds")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_source(S("/mnt/onboard/.kobo-images/1/2/x.parsed"), S("/mnt/onboard")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_source(S("/mnt/onboard/System Volume Information"), S("/mnt/onboard")) == NF_PATH_PROTECTED);
    // NEGATIVE CONTROL: the ordinary neighbours are not.
    CHECK(nf_path_check_source(S("/mnt/onboard/adds"), S("/mnt/onboard")) == NF_PATH_OK);
    CHECK(nf_path_check_source(S("/mnt/onboard/.kobold"), S("/mnt/onboard")) == NF_PATH_OK);
}

static void test_source_check_refuses_the_current_folder_and_its_ancestors(void) {
    CHECK(nf_path_check_source(S("/mnt/onboard/books"), S("/mnt/onboard/books")) == NF_PATH_IS_CWD_OR_ANCESTOR);
    CHECK(nf_path_check_source(S("/mnt/onboard/books"), S("/mnt/onboard/books/sub")) == NF_PATH_IS_CWD_OR_ANCESTOR);
    CHECK(nf_path_check_source(S("/mnt/onboard/a"), S("/mnt/onboard/a/b/c")) == NF_PATH_IS_CWD_OR_ANCESTOR);
    // NEGATIVE CONTROL, and it is the string-prefix trap wearing this rule's
    // clothes: "/mnt/onboard/books2" is not an ancestor of "/mnt/onboard/books",
    // and refusing it would make a whole sibling folder undeletable for no
    // reason a reader could ever work out.
    CHECK(nf_path_check_source(S("/mnt/onboard/books2"), S("/mnt/onboard/books")) == NF_PATH_OK);
    CHECK(nf_path_check_source(S("/mnt/onboard/books"), S("/mnt/onboard/books2")) == NF_PATH_OK);
    // A DESCENDANT of the current folder is fine -- that is every row on the
    // screen.
    CHECK(nf_path_check_source(S("/mnt/onboard/books/sub"), S("/mnt/onboard/books")) == NF_PATH_OK);
}

static void test_source_check_refuses_when_the_cwd_cannot_be_judged(void) {
    // A guard that cannot be evaluated must refuse, never wave through.
    CHECK(nf_path_check_source(S("/mnt/onboard/books/x.epub"), QString()) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_source(S("/mnt/onboard/books/x.epub"), S("books")) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_source(S("/mnt/onboard/books/x.epub"), S("/mnt/onboard/books/")) == NF_PATH_UNCLEAN);
    // ...and an unclean SOURCE is refused the same way, including the one
    // that matters: a ".." that somehow survived canonicalisation.
    CHECK(nf_path_check_source(QString(), S("/mnt/onboard")) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_source(S("/mnt/onboard/books/../../etc/passwd"), S("/mnt/onboard/books")) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_source(S("/mnt/onboard/books/"), S("/mnt/onboard/books")) == NF_PATH_UNCLEAN);
}

// --- nf_path_check_dest --------------------------------------------------

static void test_dest_check_allows_an_ordinary_paste(void) {
    CHECK(nf_path_check_dest(S("/mnt/onboard/comics/Some Book.epub"),
                             S("/mnt/onboard/books/Some Book.epub")) == NF_PATH_OK);
    CHECK(nf_path_check_dest(S("/mnt/onboard/a/b/x.cbz"),
                             S("/mnt/onboard/c/x.cbz")) == NF_PATH_OK);
    // Pasting a folder into a SIBLING of itself is fine.
    CHECK(nf_path_check_dest(S("/mnt/onboard/comics/sub"),
                             S("/mnt/onboard/books/sub")) == NF_PATH_OK);
}

static void test_dest_check_refuses_the_destination_being_the_source(void) {
    // rename(x, x) SUCCEEDS and does nothing, so allowing it would report a
    // move that never happened. A cut and a paste in the same folder is
    // exactly how a reader produces this.
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/x.epub"),
                             S("/mnt/onboard/books/x.epub")) == NF_PATH_DEST_INSIDE_SOURCE);
}

static void test_dest_check_refuses_moving_a_folder_into_itself(void) {
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/sub/books"),
                             S("/mnt/onboard/books")) == NF_PATH_DEST_INSIDE_SOURCE);
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/a/b/c"),
                             S("/mnt/onboard/books")) == NF_PATH_DEST_INSIDE_SOURCE);
    // NEGATIVE CONTROL, the string-prefix trap once more: "/mnt/onboard/books2"
    // is NOT inside "/mnt/onboard/books", so moving into it is legitimate.
    CHECK(nf_path_check_dest(S("/mnt/onboard/books2/books"),
                             S("/mnt/onboard/books")) == NF_PATH_OK);
}

static void test_dest_check_refuses_outside_root_the_protected_dirs_and_the_root(void) {
    CHECK(nf_path_check_dest(S("/tmp/x.epub"), S("/mnt/onboard/books/x.epub")) == NF_PATH_OUTSIDE_ROOT);
    CHECK(nf_path_check_dest(S("/mnt/onboardX/x.epub"), S("/mnt/onboard/books/x.epub")) == NF_PATH_OUTSIDE_ROOT);
    CHECK(nf_path_check_dest(S("/mnt/onboard"), S("/mnt/onboard/books/x.epub")) == NF_PATH_IS_ROOT);
    CHECK(nf_path_check_dest(S("/mnt/onboard/.kobo/x.epub"), S("/mnt/onboard/books/x.epub")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_dest(S("/mnt/onboard/.adds/x.epub"), S("/mnt/onboard/books/x.epub")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_dest(S("/mnt/onboard/System Volume Information/x"), S("/mnt/onboard/books/x")) == NF_PATH_PROTECTED);
    CHECK(nf_path_check_dest(QString(), S("/mnt/onboard/books/x.epub")) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/../x"), S("/mnt/onboard/books/x.epub")) == NF_PATH_UNCLEAN);
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/x.epub"), QString()) == NF_PATH_UNCLEAN);
    // NEGATIVE CONTROL for the whole block.
    CHECK(nf_path_check_dest(S("/mnt/onboard/books/x.epub"),
                             S("/mnt/onboard/comics/x.epub")) == NF_PATH_OK);
}

// --- verdict wording -----------------------------------------------------

static void test_every_verdict_has_words(void) {
    nf_path_verdict const all[] = {
        NF_PATH_OK, NF_PATH_UNCLEAN, NF_PATH_OUTSIDE_ROOT, NF_PATH_IS_ROOT,
        NF_PATH_PROTECTED, NF_PATH_IS_CWD_OR_ANCESTOR, NF_PATH_DEST_INSIDE_SOURCE,
    };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); i++) {
        char const *t = nf_path_verdict_text(all[i]);
        CHECK(t != NULL);
        CHECK(t[0] != '\0');
    }
    // The two a reader is most likely to hit say the specific thing rather
    // than "error". Pinned literally, because "refused" would pass a
    // non-empty check and tell the owner nothing.
    CHECK(QString::fromUtf8(nf_path_verdict_text(NF_PATH_PROTECTED)).contains(QStringLiteral("protected")));
    CHECK(QString::fromUtf8(nf_path_verdict_text(NF_PATH_IS_CWD_OR_ANCESTOR)).contains(QStringLiteral("folder you are in")));
    CHECK(QString::fromUtf8(nf_path_verdict_text(NF_PATH_OUTSIDE_ROOT)).contains(QStringLiteral("/mnt/onboard")));
}

// --- nf_temp_name --------------------------------------------------------

static void test_temp_name_is_hidden_and_not_a_book(void) {
    QString t = nf_temp_name(S("Some Book.epub"));
    CHECK_EQ_STR(t, ".Some Book.epub.nfolders-part");
    // The two properties the shape exists for, checked as properties and not
    // only as a literal: a dotfile, and NOT ending in a book extension (the
    // listing admits files by an extension allowlist, so a partial file whose
    // name still ended in ".epub" would be listed and tappable mid-copy).
    CHECK(t.startsWith(QLatin1Char('.')));
    CHECK(!t.endsWith(QStringLiteral(".epub")));
    CHECK(!t.endsWith(QStringLiteral(".kepub.epub")));
    CHECK(!t.endsWith(QStringLiteral(".cbz")));
    CHECK(!t.endsWith(QStringLiteral(".pdf")));
    CHECK(nf_temp_name(S("x.kepub.epub")).endsWith(QStringLiteral(".nfolders-part")));
    CHECK(nf_temp_name(S("x.cbr")).endsWith(QStringLiteral(".nfolders-part")));
}

static void test_temp_name_refuses_an_unsafe_name(void) {
    CHECK(nf_temp_name(QString()).isEmpty());
    CHECK(nf_temp_name(S(".")).isEmpty());
    CHECK(nf_temp_name(S("..")).isEmpty());
    CHECK(nf_temp_name(S("a/b")).isEmpty());
    // NEGATIVE CONTROL: an ordinary name still produces one.
    CHECK(!nf_temp_name(S("a.epub")).isEmpty());
}

static void test_temp_name_is_bounded_for_the_long_names_this_card_holds(void) {
    // The reference card holds 230-character names, and VFAT's long-name
    // limit is 255 UTF-16 units -- so an unbounded ".<name>.nfolders-part"
    // really can be unstorable.
    QString longName = QString(240, QLatin1Char('x')) + QStringLiteral(".epub");
    QString t = nf_temp_name(longName);
    CHECK(!t.isEmpty());
    CHECK(t.length() <= NF_TEMP_NAME_MAX);
    CHECK(t.startsWith(QLatin1Char('.')));
    CHECK(t.endsWith(QStringLiteral(".nfolders-part")));
    // A name that fits is NOT truncated -- the bound must not be a haircut
    // applied to everything.
    CHECK_EQ_STR(nf_temp_name(S("short.epub")), ".short.epub.nfolders-part");
    // Exactly at the boundary, from both sides.
    int fixed = 1 + 14; // "." + ".nfolders-part"
    QString justFits(NF_TEMP_NAME_MAX - fixed, QLatin1Char('y'));
    CHECK(nf_temp_name(justFits).length() == NF_TEMP_NAME_MAX);
    QString oneOver(NF_TEMP_NAME_MAX - fixed + 1, QLatin1Char('y'));
    CHECK(nf_temp_name(oneOver).length() == NF_TEMP_NAME_MAX);
}

static void test_temp_name_never_splits_a_surrogate_pair(void) {
    // A non-BMP character is two QChars, and cutting between them would make
    // half a character -- a filename no filesystem should be asked to store.
    // U+1F4D6 OPEN BOOK, repeated, is exactly that shape.
    QString emoji;
    for (int i = 0; i < 200; i++)
        emoji += QString::fromUtf8("\xf0\x9f\x93\x96"); // one surrogate PAIR each
    QString t = nf_temp_name(emoji);
    CHECK(!t.isEmpty());
    CHECK(t.length() <= NF_TEMP_NAME_MAX);
    // The last character of the truncated BASE must not be an unpaired high
    // surrogate. The base sits between the leading "." and the suffix.
    QString suffix = QStringLiteral(".nfolders-part");
    CHECK(t.endsWith(suffix));
    QString base = t.mid(1, t.length() - 1 - suffix.length());
    CHECK(!base.isEmpty());
    CHECK(!base.at(base.length() - 1).isHighSurrogate());
}

// --- the constant everything here is parameterised by --------------------

static void test_the_root_is_still_the_root(void) {
    CHECK(nf_path_root_is_consistent());
    // And the guards really are written against it: the classic escape is
    // refused and the ordinary case is not, both spelled with the literal.
    CHECK(nf_path_is_inside(S("/mnt/onboard/books"), S(NF_PATH_ROOT)));
    CHECK(!nf_path_is_inside(S("/mnt/onboardX/books"), S(NF_PATH_ROOT)));
}

int main(void) {
    test_clean_accepts_ordinary_absolute_paths();
    test_clean_refuses_the_shapes_that_make_a_guard_meaningless();
    test_a_string_prefix_is_not_a_path_prefix();
    test_inside_is_strict_and_refuses_unclean_arguments();
    test_ancestor_or_self_includes_self_and_nothing_sideways();
    test_the_four_protected_directories_are_protected();
    test_protected_is_case_insensitive_because_the_card_is_vfat();
    test_protected_does_not_over_reach_onto_ordinary_names();
    test_a_component_with_a_slash_or_dots_is_not_safe();
    test_source_check_allows_an_ordinary_book_in_an_ordinary_folder();
    test_source_check_refuses_everything_outside_the_root();
    test_source_check_refuses_the_root_itself();
    test_source_check_refuses_the_protected_directories();
    test_source_check_refuses_the_current_folder_and_its_ancestors();
    test_source_check_refuses_when_the_cwd_cannot_be_judged();
    test_dest_check_allows_an_ordinary_paste();
    test_dest_check_refuses_the_destination_being_the_source();
    test_dest_check_refuses_moving_a_folder_into_itself();
    test_dest_check_refuses_outside_root_the_protected_dirs_and_the_root();
    test_every_verdict_has_words();
    test_temp_name_is_hidden_and_not_a_book();
    test_temp_name_refuses_an_unsafe_name();
    test_temp_name_is_bounded_for_the_long_names_this_card_holds();
    test_temp_name_never_splits_a_surrogate_pair();
    test_the_root_is_still_the_root();
    NF_TEST_MAIN_END
}
