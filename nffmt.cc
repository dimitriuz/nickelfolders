#include "nffmt.h"

// Hand-written rather than QCollator::setNumericMode, because QCollator needs
// ICU and betting on ICU inside Kobo's Qt 5.2.1 is not a bet worth making.
//
// Digit runs are compared WITHOUT being parsed to an integer: by significant
// length first, then lexically. That is not a micro-optimisation -- parsing
// would overflow on a pathological name, and an overflow here silently
// reorders the list rather than failing.
int nf_natural_compare(QString const& a, QString const& b) {
    int i = 0, j = 0;
    while (i < a.length() && j < b.length()) {
        QChar ca = a.at(i), cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            int si = i, sj = j;
            while (i < a.length() && a.at(i).isDigit()) i++;
            while (j < b.length() && b.at(j).isDigit()) j++;
            int zi = si; while (zi < i - 1 && a.at(zi) == QLatin1Char('0')) zi++;
            int zj = sj; while (zj < j - 1 && b.at(zj) == QLatin1Char('0')) zj++;
            int li = i - zi, lj = j - zj;
            if (li != lj)
                return li < lj ? -1 : 1;
            for (int k = 0; k < li; k++) {
                QChar da = a.at(zi + k), db = b.at(zj + k);
                if (da != db)
                    return da < db ? -1 : 1;
            }
            // Equal in value. Fall back to the raw runs so the order is
            // deterministic -- "v01" and "v1" must not compare equal, or a
            // sort can reorder them between runs.
            int ri = i - si, rj = j - sj;
            if (ri != rj)
                return ri < rj ? -1 : 1;
            continue;
        }
        QChar fa = ca.toCaseFolded(), fb = cb.toCaseFolded();
        if (fa != fb)
            return fa < fb ? -1 : 1;
        i++; j++;
    }
    int ra = a.length() - i, rb = b.length() - j;
    if (ra != rb)
        return ra < rb ? -1 : 1;
    return QString::compare(a, b, Qt::CaseSensitive);
}

// v1's formats, measured by censusing the whole reference card on 2026-09-03:
// 122 .cbz, 94 .cbr, 6 .pdf, 4 .epub (2 of them .kepub.epub). ".txt" is
// deliberately absent -- the card's only .txt is a probe file the sibling
// koboy project left on /mnt/onboard, which Nickel imported as a book, so
// admitting .txt buys exactly one row of garbage. Spec section 3.5.
//
// LONGEST FIRST is load-bearing: ".kepub.epub" also ends in ".epub", and a
// shortest-match scan would report the wrong extension for a Kobo epub.
static char const *const NF_EXTS[] = {
    ".kepub.epub", ".epub", ".cbz", ".cbr", ".pdf", NULL,
};

QString nf_book_extension(QString const& name) {
    for (int i = 0; NF_EXTS[i]; i++) {
        QString ext = QString::fromLatin1(NF_EXTS[i]);
        if (name.endsWith(ext, Qt::CaseInsensitive))
            return name.right(ext.length());
    }
    return QString();
}

static bool nf_is_open(QChar c)  { return c == QLatin1Char('(') || c == QLatin1Char('['); }
static bool nf_is_close(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']'); }

// Index of the first opener in [0,end) never closed within it, or -1.
static int nf_first_unclosed(QString const& s, int end) {
    int depth = 0, first = -1;
    for (int i = 0; i < end; i++) {
        if (nf_is_open(s.at(i))) {
            if (depth == 0)
                first = i;
            depth++;
        } else if (nf_is_close(s.at(i))) {
            if (depth > 0)
                depth--;
        }
    }
    return depth > 0 ? first : -1;
}

void nf_strip_common(QStringList *names) {
    if (names->size() < 2)
        return;

    // Extensions come off FIRST rather than falling out as a common suffix.
    // Measured: ".cbz" contains no whitespace, so the token-boundary rule below
    // cannot cut there. Fullmetal Alchemist appeared to work only because its
    // common suffix happens to contain spaces. Spec section 3.2.
    QStringList stems;
    QStringList exts;
    bool mixed = false;
    for (int i = 0; i < names->size(); i++) {
        QString ext = nf_book_extension(names->at(i));
        exts  << ext;
        stems << names->at(i).left(names->at(i).length() - ext.length());
        if (i > 0 && ext.compare(exts.at(0), Qt::CaseInsensitive) != 0)
            mixed = true;
    }

    int minLen = stems.at(0).length();
    for (int i = 1; i < stems.size(); i++)
        minLen = qMin(minLen, stems.at(i).length());

    int p = 0;
    while (p < minLen) {
        QChar c = stems.at(0).at(p);
        bool same = true;
        for (int i = 1; i < stems.size(); i++) {
            if (stems.at(i).at(p) != c) {
                same = false;
                break;
            }
        }
        if (!same)
            break;
        p++;
    }

    int s = 0;
    while (s < minLen - p) {
        QChar c = stems.at(0).at(stems.at(0).length() - 1 - s);
        bool same = true;
        for (int i = 1; i < stems.size(); i++) {
            if (stems.at(i).at(stems.at(i).length() - 1 - s) != c) {
                same = false;
                break;
            }
        }
        if (!same)
            break;
        s++;
    }

    // A row must not begin mid-word.
    int pw = p;
    while (pw > 0 && !stems.at(0).at(pw - 1).isSpace())
        pw--;
    // Nor inside a bracket. Measured case: 13 names sharing "... (Part ",
    // where the whitespace cut alone leaves rows reading "1) - Rouge ..." and
    // throws away the "(Part 1)" that distinguishes the sub-series.
    int unclosed = nf_first_unclosed(stems.at(0), pw);
    if (unclosed >= 0)
        pw = unclosed;

    int sw = s;
    {
        QString const& n = stems.at(0);
        while (sw > 0 && !n.at(n.length() - sw).isSpace())
            sw--;
    }

    QStringList out;
    for (int i = 0; i < stems.size(); i++) {
        QString const& n = stems.at(i);
        int keep = n.length() - pw - sw;
        if (keep < 2)
            return;                 // deliberate: leave EVERY row alone, not just this one
        QString r = n.mid(pw, keep);
        int u = nf_first_unclosed(r, r.length());
        if (u >= 0)
            r = r.left(u);          // never show a dangling half-bracket
        r = r.trimmed();

        // Refuse to strip if the REMAINDER -- before any extension is
        // appended below -- carries no letter at all. Measured on the
        // reference card's own root: the two Steven L. Kent "Ultimate
        // History of Video Games" volumes share "steven l. kent - the
        // ultimate history of video games, volume " -- the run common to
        // BOTH, which strips to "1 - 2001" / "2 - 2021". Both remainders
        // are well past the two-character floor above, so that guard does
        // not fire; the title -- the one thing a reader needs -- is
        // exactly what got stripped, because here the shared run IS the
        // title rather than noise around it (contrast Fullmetal
        // Alchemist/Pokémon, where the shared run really is noise and this
        // guard must NOT fire for them). "Contains a letter" is a low bar
        // on purpose: "v01 (2005)" keeps its "v", "(Part 1) - Rouge, Bleu
        // et Jaune T01" keeps plenty, and only a remainder of nothing but
        // digits/punctuation/whitespace fails it.
        //
        // MUST run on `r` here, BEFORE `if (mixed) r += exts.at(i)` below
        // -- a review finding (F1), reproduced on the host: checking the
        // POST-extension `out` instead let the extension's own letters
        // satisfy the test vacuously, so kepubifying just ONE of the two
        // Kent volumes (a routine Kobo operation, and .kepub.epub is
        // already on this card) made the listing MIXED, and
        // "1 - 2001.kepub.epub" / "2 - 2021.epub" both contain a letter --
        // silently restoring the exact bug this guard exists to catch.
        // Checking the bare remainder is immune to that: the extension is
        // not part of what is being judged "a title or not" here.
        //
        // QChar::isLetter() reads Qt's own bundled Unicode character-
        // property table, and that table is measurably older on the
        // device's Qt 5.2.1 than on the host's Qt 5.15 (NOTES.md, Task 9's
        // natural-sort finding). The check below is deliberately the
        // WEAKEST possible use of that table -- "does this string contain
        // any letter of any script at all", true for every alphabet either
        // Unicode revision has ever known about -- rather than anything
        // alphabet- or script-specific, so a table skew could only matter
        // for a codepoint added as "a letter" between those two revisions,
        // and even then the failure mode is the safe direction (falling
        // through to full, untouched names), never the unsafe one
        // (stripping a title away).
        bool hasLetter = false;
        for (int k = 0; k < r.length(); k++) {
            if (r.at(k).isLetter()) {
                hasLetter = true;
                break;
            }
        }
        if (!hasLetter)
            return;                 // deliberate: leave EVERY row alone, not just this one

        if (mixed)
            r += exts.at(i);        // in a mixed listing the extension is the distinguisher
        out << r;
    }
    for (int i = 0; i < out.size(); i++) {
        if (out.at(i).length() < 2)
            return;
    }

    *names = out;
}

// Insertion sort, and the choice is deliberate rather than lazy. std::sort is
// a libstdc++ template and CLAUDE.md forbids compiling stdlib templates into
// the library; Qt 5.2's qSort is deprecated in the host Qt 5.15 the tests build
// against, so -Werror rejects it. The largest listing measured on the reference
// card is 27 entries, where an insertion sort is not worth optimising -- and it
// is STABLE, which is what keeps the order reproducible when two names tie.
//
// Plain subtraction on two qint64s can overflow (a size or an epoch-
// millisecond difference both fit in 63 bits individually, but their
// difference is not guaranteed to), so this compares rather than subtracts.
static int nf_compare_i64(qint64 a, qint64 b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Spec section 6.1 keeps grouping and ordering separate stages so that a future
// descending toggle reverses WITHIN each kind and never floats files above
// folders. Do not collapse the isDir test into the comparator, and do not let
// `descending` see it either -- folders sort before files in BOTH directions;
// only the order WITHIN a kind flips. This is the exact trap 6.2 names
// "reverse the list" for: reversing the WHOLE sorted vector would put every
// file ahead of every folder whenever `descending` is set.
//
// A tie on `key` (two files sharing a size, or a modification second -- both
// measurably possible on this card, not just theoretical) falls back to name
// rather than being left to insertion-sort stability alone, so the order a
// size/date sort produces is as reproducible as the name sort's always was.
//
// Folders get the SAME key applied as files, including size/mtime -- not
// specially exempted or forced to a name-only order. A folder's mtime bumps
// whenever anything inside it changes and its stat()'d size is a filesystem
// block size, not a sum of its contents, so neither is a particularly
// meaningful number for a folder on its own terms -- but nf_sort_entries
// stays the ONE place ordering happens (this file's own header comment, and
// the task this added it for), which means no second, folder-only code path
// to keep in sync with this one, and "recently touched" is still a
// defensible reading of a folder's own mtime for a reader who tapped
// "sort: date".
static bool nf_entry_before(nf_entry const& a, nf_entry const& b,
                             nf_sort_key key, bool descending) {
    if (a.isDir != b.isDir)
        return a.isDir;

    int cmp;
    switch (key) {
        case NF_SORT_SIZE: cmp = nf_compare_i64(a.size, b.size);   break;
        case NF_SORT_DATE: cmp = nf_compare_i64(a.mtime, b.mtime); break;
        case NF_SORT_NAME:
        default:            cmp = nf_natural_compare(a.name, b.name); break;
    }
    if (cmp == 0)
        cmp = nf_natural_compare(a.name, b.name);

    return descending ? (cmp > 0) : (cmp < 0);
}

void nf_sort_entries(QVector<nf_entry> *entries, nf_sort_key key, bool descending) {
    for (int i = 1; i < entries->size(); i++) {
        nf_entry cur = entries->at(i);
        int j = i - 1;
        while (j >= 0 && nf_entry_before(cur, entries->at(j), key, descending)) {
            (*entries)[j + 1] = entries->at(j);
            j--;
        }
        (*entries)[j + 1] = cur;
    }
}

bool nf_is_book_name(QString const& name) {
    return !nf_book_extension(name).isEmpty();
}

// Reuses nf_book_extension rather than re-deriving anything from `name`
// itself -- the LONGEST-FIRST match that already tells ".kepub.epub" apart
// from a plain ".epub" (NF_EXTS' own comment) is exactly what NF_FILTER_EPUB
// needs to treat both as one format rather than two.
bool nf_matches_filter(QString const& name, nf_filter_kind filter) {
    if (filter == NF_FILTER_ALL)
        return true;
    QString ext = nf_book_extension(name);
    switch (filter) {
        case NF_FILTER_CBZ:  return ext.compare(QStringLiteral(".cbz"), Qt::CaseInsensitive) == 0;
        case NF_FILTER_CBR:  return ext.compare(QStringLiteral(".cbr"), Qt::CaseInsensitive) == 0;
        case NF_FILTER_PDF:  return ext.compare(QStringLiteral(".pdf"), Qt::CaseInsensitive) == 0;
        case NF_FILTER_EPUB: return ext.compare(QStringLiteral(".epub"), Qt::CaseInsensitive) == 0
                                  || ext.compare(QStringLiteral(".kepub.epub"), Qt::CaseInsensitive) == 0;
        case NF_FILTER_ALL:
        default:              return true;
    }
}

bool nf_is_hidden_dir(QString const& name) {
    if (name.startsWith(QLatin1Char('.')))
        return true;
    // A KOReader sidecar directory, and one sits at the reference card's root
    // in plain sight rather than hidden.
    if (name.endsWith(QStringLiteral(".sdr"), Qt::CaseInsensitive))
        return true;
    // Windows leaves this on any FAT volume it has touched. Not ours to show.
    if (name.compare(QStringLiteral("System Volume Information"), Qt::CaseInsensitive) == 0)
        return true;
    return false;
}
