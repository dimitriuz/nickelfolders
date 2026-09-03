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
