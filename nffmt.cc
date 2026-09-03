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
