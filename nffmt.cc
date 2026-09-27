#include "nffmt.h"

// --- the view flags -----------------------------------------------------
//
// See nffmt.h for what each flag means, why every default is `false`, and why
// `showHidden` is the one of the five that is allowed to change which rows
// exist.

nf_view_flags nf_view_flags_default(void) {
    // Spelled through the macro, so the defaults exist in exactly one place
    // and nfview.cc's file-scope copy (which MUST use the macro -- a function
    // call at file scope is a dynamic initialiser) cannot drift from what
    // every default argument in this project hands out.
    nf_view_flags v = NF_VIEW_FLAGS_DEFAULT;
    return v;
}

bool *nf_view_flag(nf_view_flags *view, nf_view_toggle toggle) {
    if (!view)
        return NULL;
    // No `default:`, deliberately: this switch names every nf_view_toggle, so
    // adding a toggle later fails the build here (-Wswitch, and both build
    // paths use -Werror) until somebody says which field it flips. A
    // `default:` would instead silently hand back NULL for the new toggle,
    // i.e. a menu row that renders and does nothing when tapped.
    switch (toggle) {
        case NF_VIEW_FILENAMES:  return &view->fullNames;
        case NF_VIEW_EXTENSIONS: return &view->hideExtensions;
        case NF_VIEW_COVERS:     return &view->hideCovers;
        case NF_VIEW_HIDDEN:     return &view->showHidden;
        case NF_VIEW_SIZE:       return &view->showSize;
    }
    return NULL;
}

// The const read, defined in terms of the ONE mapping above rather than as a
// second switch: two switches over the same enum are two things that can be
// edited apart, and a view row labelled off the wrong field is precisely the
// silent wrong answer this menu exists to prevent. The const_cast is safe --
// `view` is a real, non-const object at every call site, and the pointer is
// only ever read through here.
static bool nf_view_flag_value(nf_view_flags const& view, nf_view_toggle toggle) {
    bool *p = nf_view_flag(const_cast<nf_view_flags*>(&view), toggle);
    return p ? *p : false;
}

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
//
// The two DATE-METADATA keys are the one place that breaks down, and it
// breaks down harmlessly: there is no Volume for a folder, so nflist.cc's
// metadata stage skips directories and every folder's dateAdded/dateLastRead
// stays empty. All folders therefore tie under NF_SORT_ADDED/NF_SORT_READ and
// fall through to the name tie-break below, which is the only order a folder
// could honestly be given by a library date it does not have. Folders still
// group ahead of files in both directions, as ever.
//
// Takes an nf_row, not an nf_entry, and that direction is deliberate: the two
// date keys (NF_SORT_ADDED/NF_SORT_READ) are METADATA, so they exist only on
// a row -- an nf_entry has nowhere to put them. Before those keys existed
// this function took an nf_entry and nf_sort_rows adapted a row DOWN to one;
// adapting the other way keeps the ordering RULES in exactly one place, which
// is the property worth preserving (the alternative -- two comparators -- is
// two copies of 6.2's "reverse the list" trap and of the tie-break, one of
// which would eventually be edited alone). nf_sort_entries now adapts UP
// instead, and simply sees two empty date keys under either date-metadata
// key; see nf_sort_entries' own comment.
static bool nf_row_before(nf_row const& a, nf_row const& b,
                          nf_sort_key key, bool descending) {
    if (a.isDir != b.isDir)
        return a.isDir;

    int cmp;
    switch (key) {
        case NF_SORT_SIZE:  cmp = nf_compare_i64(a.size, b.size);   break;
        case NF_SORT_DATE:  cmp = nf_compare_i64(a.mtime, b.mtime); break;
        // Byte comparison, never a parse -- nf_date_compare (below) and
        // nf_row::dateAdded's own comment (nffmt.h) have why. A row whose key
        // is empty is not skipped or floated: nf_date_compare gives it
        // Nickel's own sentinel, so it orders as the oldest thing present.
        case NF_SORT_ADDED: cmp = nf_date_compare(a.dateAdded, b.dateAdded);       break;
        case NF_SORT_READ:  cmp = nf_date_compare(a.dateLastRead, b.dateLastRead); break;
        case NF_SORT_NAME:
        default:            cmp = nf_natural_compare(a.name, b.name); break;
    }
    if (cmp == 0)
        cmp = nf_natural_compare(a.name, b.name);

    return descending ? (cmp > 0) : (cmp < 0);
}

// The entry-level sort adapts UP to a row rather than restating any of the
// rules -- see nf_row_before's own comment for why that direction inverted.
// The copy is shallow: `name` is a refcounted QString and the two date keys
// are refcounted QByteArrays (empty, hence shared-null, on every entry that
// comes through here), so this is a handful of atomic increments per
// comparison against a 27-entry worst case measured on the reference card.
//
// NF_SORT_ADDED/NF_SORT_READ against bare ENTRIES therefore compare two empty
// keys for every pair and fall through to the name tie-break. That is correct
// rather than merely harmless: an nf_entry is a raw directory entry and has no
// library metadata to order by, so "order by a field nobody filled in" can
// only mean "leave the name order alone". The pipeline sorts rows
// (nf_sort_rows, via nflist.cc), which is where those two keys have values.
static nf_row nf_row_of(nf_entry const& e) {
    nf_row r;
    r.name  = e.name;   // the ON-DISK name, never a label: labels are derived last, after ordering (nflist.cc)
    r.isDir = e.isDir;
    r.size  = e.size;
    r.mtime = e.mtime;
    return r;
}

void nf_sort_entries(QVector<nf_entry> *entries, nf_sort_key key, bool descending) {
    for (int i = 1; i < entries->size(); i++) {
        nf_entry cur = entries->at(i);
        int j = i - 1;
        while (j >= 0 && nf_row_before(nf_row_of(cur), nf_row_of(entries->at(j)), key, descending)) {
            (*entries)[j + 1] = entries->at(j);
            j--;
        }
        (*entries)[j + 1] = cur;
    }
}

// The insertion-sort LOOP is what is duplicated here -- five self-evident
// lines -- and not the ordering RULES, which are the part with a measured trap
// in them (6.2's "reverse the list", and the tie-break) and stay in exactly
// one function.
void nf_sort_rows(QVector<nf_row> *rows, nf_sort_key key, bool descending) {
    for (int i = 1; i < rows->size(); i++) {
        nf_row cur = rows->at(i);
        int j = i - 1;
        while (j >= 0 && nf_row_before(cur, rows->at(j), key, descending)) {
            (*rows)[j + 1] = rows->at(j);
            j--;
        }
        (*rows)[j + 1] = cur;
    }
}

// --- the two date keys, as bytes ----------------------------------------
//
// nffmt.h carries the full derivation for both of these, including the
// empty-date rule and why a failing value is never repaired. Kept together
// here because they are one decision split across two functions: what a key
// is allowed to contain, and how two of them order.
int nf_date_compare(QByteArray const& a, QByteArray const& b) {
    // NOT a file-scope QByteArray: a plain char array with a string-literal
    // initialiser is POD, so it lives in .rodata with no dynamic initialiser
    // to run -- which is what keeps `nm libnfolders.so | grep GLOBAL__sub_I`
    // empty (CLAUDE.md). A `static QByteArray` here would be exactly the
    // construct that boot-looped this mod once already.
    static char const zero[] = NF_ZERO_DB_DATE;

    // constData() on an empty (or null) QByteArray is a valid pointer to a
    // '\0' in Qt's shared-null, never NULL, so the substitution below is the
    // only reason to special-case empty -- not pointer safety.
    char const *pa = a.isEmpty() ? zero : a.constData();
    char const *pb = b.isEmpty() ? zero : b.constData();

    // qstricmp, not strcasecmp: identical ASCII case folding, but it comes
    // from QtCore, which this project links against on both the host and the
    // device, and it is locale-independent by construction. Nickel's own
    // RecentSorter uses strcasecmp (archaeology section 6) and these date
    // strings only ever differ in case at the 'T' separator or a trailing
    // 'Z', where ASCII folding is the whole of the question.
    return qstricmp(pa, pb);
}

bool nf_date_key_is_plausible(QByteArray const& raw) {
    // Empty is the honest "unknown" and must be accepted, or a file with no
    // library row at all -- a NORMAL case on this card, not an error
    // (CLAUDE.md: 226 of 227 files import, and the browser must say so in the
    // row rather than hide it) -- would be logged as a layout failure on every
    // single listing.
    if (raw.isEmpty())
        return true;

    // ^\d{4}-\d\d-\d\d[Tt ] -- eleven characters is the shortest prefix
    // that pins the shape; anything shorter cannot be a date and anything
    // after the separator is not this check's business (the milliseconds and
    // any zone suffix vary, per the archaeology's own open question 8.2, and
    // guessing a width here would reject valid rows).
    static int const kPrefix = 11;
    if (raw.size() < kPrefix)
        return false;

    char const *p = raw.constData();
    for (int i = 0; i < 4; i++)
        if (p[i] < '0' || p[i] > '9') return false;
    if (p[4] != '-') return false;
    if (p[5] < '0' || p[5] > '9') return false;
    if (p[6] < '0' || p[6] > '9') return false;
    if (p[7] != '-') return false;
    if (p[8] < '0' || p[8] > '9') return false;
    if (p[9] < '0' || p[9] > '9') return false;
    // 'T' is what QDateTime::fromString(..., Qt::ISODate) parses and what
    // Nickel's own ZERO_DB_DATE_ARRAY sentinel spells; 't' and ' ' are the
    // other two spellings ISO-8601 and SQLite respectively allow, accepted
    // because rejecting a genuinely valid row would look exactly like a moved
    // offset and send the next reader after the wrong cause.
    if (p[10] != 'T' && p[10] != 't' && p[10] != ' ') return false;
    return true;
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
        // Every read-state value lands here and admits the file, which is
        // exactly right: a read-state filter has no opinion about FORMAT.
        // The mirror image is nf_matches_read_filter returning true for every
        // format filter -- one enum, two axes, and each function answers only
        // its own.
        case NF_FILTER_ALL:
        default:              return true;
    }
}

// --- the read-state axis ------------------------------------------------
//
// Kobo's own ReadingStatus values decoded into this file's enum. THE ONLY
// place 0/1/2 appear in this project -- see nffmt.h for why the two
// numberings are deliberately kept apart.
nf_read_state nf_read_state_from_status(int status) {
    switch (status) {
        case 0:  return NF_READ_NOT_STARTED;
        case 1:  return NF_READ_IN_PROGRESS;
        case 2:  return NF_READ_FINISHED;
        // Anything else degrades to UNKNOWN rather than to a bucket. 0 is a
        // REAL bucket here, so treating an unexpected value as 0 would file
        // every book under "not started" with nothing to distinguish it from
        // a book that genuinely is unread -- an invisible wrong answer, where
        // UNKNOWN is a visible one (the row simply appears under no
        // read-state filter at all). Same reasoning, and the same refusal to
        // clamp into range, as the percentRead offset guard in nfnickel.cc.
        default: return NF_READ_UNKNOWN;
    }
}

// Deliberately no `default:`: this switch names every nf_filter_kind, so
// adding a filter value later fails the build here (-Wswitch, and the Makefile
// builds with -Werror) until somebody says which of the two axes it belongs
// to. A `default:` would instead silently classify it as a format filter.
static bool nf_filter_is_read_state(nf_filter_kind filter) {
    switch (filter) {
        case NF_FILTER_FINISHED:
        case NF_FILTER_IN_PROGRESS:
        case NF_FILTER_NOT_STARTED:
            return true;
        case NF_FILTER_ALL:
        case NF_FILTER_CBZ:
        case NF_FILTER_CBR:
        case NF_FILTER_PDF:
        case NF_FILTER_EPUB:
            break;
    }
    return false;
}

bool nf_matches_read_filter(nf_row const& row, nf_filter_kind filter) {
    if (!nf_filter_is_read_state(filter))
        return true;

    // A folder is NEVER filtered out, by any filter -- nf_build_listing's own
    // type-filter comment (nflist.cc) has the full reason: a folder may hold
    // matching files one level down, and hiding it makes them unreachable
    // rather than merely invisible. On this axis there is a second reason as
    // well: a folder has no Volume, so there is no read state to match against
    // even in principle. Answered HERE rather than short-circuited by the
    // caller the way the type filter's folder rule has to be -- a name-only
    // predicate cannot see isDir, this one can, so the rule lives in one place.
    if (row.isDir)
        return true;

    // No library row means no MEASURABLE read state, which is not the same
    // thing as "not started". The reference card carries a truncated 8 MiB
    // file Nickel failed to import (CLAUDE.md) -- calling that book unread
    // would be a guess dressed up as a fact, and it would show up under
    // "not started" alongside books that really are. So an unknown state is
    // hidden by all three read-state filters, and shown by every other filter.
    //
    // hasRow is checked as WELL as readState, not instead of it: hasRow is the
    // fact an nf_meta_fn establishes first, and a row with no Volume must never
    // land in a bucket however some future `meta` happens to leave readState.
    if (!row.hasRow || row.readState == NF_READ_UNKNOWN)
        return false;

    switch (filter) {
        case NF_FILTER_FINISHED:    return row.readState == NF_READ_FINISHED;
        case NF_FILTER_IN_PROGRESS: return row.readState == NF_READ_IN_PROGRESS;
        case NF_FILTER_NOT_STARTED: return row.readState == NF_READ_NOT_STARTED;
        // Unreachable: nf_filter_is_read_state above already returned for
        // every other value. Present because a switch over an enum still needs
        // a return on every path, and answering "yes" is the safe direction --
        // a filter this function does not understand must not make rows vanish.
        default:                     return true;
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

// --- row icons ----------------------------------------------------------
//
// The mapping only, not the markup: what a kind LOOKS like is Qt-resource and
// rendering territory and lives in nfview.cc, which nothing off-device can
// run. This half is a decision about a name, so it is testable, and it is the
// half with the trap in it -- ".kepub.epub" also ends in ".epub", which is
// exactly why nf_book_extension is reused here rather than re-derived (see
// NF_EXTS' longest-match-first comment above, and nf_matches_filter, which
// leans on the same property for NF_FILTER_EPUB).
nf_icon_kind nf_icon_kind_for(QString const& name, bool isDir) {
    // FIRST, before the name is even looked at. A directory named
    // "Comics.cbz" is a directory; the reported defect this feature answers
    // was a .cbr FILE reading as a folder, so letting an extension outvote
    // isDir here would reintroduce the same confusion with the icon that is
    // supposed to end it.
    if (isDir)
        return NF_ICON_FOLDER;

    QString ext = nf_book_extension(name);
    if (ext.compare(QStringLiteral(".epub"), Qt::CaseInsensitive) == 0
     || ext.compare(QStringLiteral(".kepub.epub"), Qt::CaseInsensitive) == 0)
        return NF_ICON_BOOK;
    if (ext.compare(QStringLiteral(".cbz"), Qt::CaseInsensitive) == 0
     || ext.compare(QStringLiteral(".cbr"), Qt::CaseInsensitive) == 0)
        return NF_ICON_COMIC;
    if (ext.compare(QStringLiteral(".pdf"), Qt::CaseInsensitive) == 0)
        return NF_ICON_PDF;
    // Everything else, including an empty extension. Unreachable for a row
    // the current hide-junk stage produced (nf_is_book_name gates every file
    // row on the same NF_EXTS allowlist), which is why it is worth stating
    // that this is a real answer and not a fall-through: the allowlist and
    // this map are separate layers, and a ".txt" added to NF_EXTS later must
    // come out of here with a badge rather than with no icon at all.
    return NF_ICON_UNKNOWN;
}

// --- the two-form label pieces ------------------------------------------
//
// See nffmt.h for why both forms are built together and what the tests pin.
// The shape of every function here is the same and is the whole point: ONE
// authored string, in the plain form, with nf_nbsp() wherever the rendered
// markup will carry a `&nbsp;` -- then the markup is DERIVED from it by
// substitution. There is no second place to spell a separator, so the two
// forms cannot drift and the measurement cannot be short by one.
//
// Substituting only `&nbsp;` is safe for these fragments specifically
// because every other character in them is HTML-inert -- "/", "[", "]",
// "(", ")", "%", letters and digits. Nothing here is card data (a filename
// is escaped separately, by the caller, BEFORE our markup is appended --
// nfview.cc's own ordering comment), so no fragment here can ever contain a
// "&" or a "<" that would need escaping first.

// The two-space separator, as the two real U+00A0 characters. Both suffix
// separators are non-breaking on purpose: an ordinary space is a wrap
// opportunity and a collapsible run, and a suffix that wrapped or collapsed
// would be measured as one width and rendered at another.
static QString nf_suffix_sep(void) {
    return QString(2, nf_nbsp());
}

static QString nf_to_markup(QString const& plain) {
    return QString(plain).replace(nf_nbsp(), QStringLiteral("&nbsp;"));
}

void nf_icon_badge(nf_icon_kind kind, QString *markup, QString *plain) {
    char const *text = NULL;
    switch (kind) {
        case NF_ICON_FOLDER:  text = "[DIR]"; break;
        case NF_ICON_BOOK:    text = "[EPB]"; break;
        case NF_ICON_COMIC:   text = "[CMC]"; break;
        case NF_ICON_PDF:     text = "[PDF]"; break;
        case NF_ICON_UNKNOWN:
        default:              text = "[ ? ]"; break;
    }
    // Every space becomes non-breaking, the inner ones in "[ ? ]" included:
    // that badge is a five-character box and a collapsed run would make it
    // three, so the labels after it would no longer line up with the other
    // four badges'.
    QString p = QString::fromLatin1(text).replace(QLatin1Char(' '), nf_nbsp())
              + nf_nbsp(); // the separator between the badge and the name
    if (plain)
        *plain = p;
    if (markup)
        *markup = nf_to_markup(p);
}

QString nf_format_size(qint64 bytes) {
    // The refusal, not a clamp -- see nffmt.h. A negative size is not a small
    // size, and "0 B" would hide whatever produced it.
    if (bytes < 0)
        return QString();

    // 1024, not 1000, and "KB" rather than "KiB": binary multiples with the
    // short names, which is what Kobo's own library rows show and therefore
    // what a reader of this device already reads sizes in. Being consistent
    // with the surrounding software matters more here than being correct
    // about a unit suffix nobody on this panel is checking.
    static qint64 const kUnit[] = { 1024LL * 1024 * 1024, 1024LL * 1024, 1024LL };
    static char const *const kName[] = { "GB", "MB", "KB" };

    for (int i = 0; i < 3; i++) {
        if (bytes < kUnit[i])
            continue;
        qint64 unit  = kUnit[i];
        qint64 whole = bytes / unit;
        qint64 rem   = bytes % unit;
        // Rounded to a tenth, in INTEGERS. `rem` is strictly below `unit`,
        // i.e. below 2^30, so rem * 10 cannot come near overflowing a qint64
        // -- which the obvious `bytes * 10 / unit` could, on a size this
        // browser has no business rejecting just because it is large.
        int tenth = (int)((rem * 10 + unit / 2) / unit);
        if (tenth == 10) { // the rounding carried out of the fraction
            whole++;
            tenth = 0;
        }
        // ...and the carry can push `whole` to exactly 1024, i.e. one whole
        // unit of the NEXT size up: 1 MiB - 1 byte lands in the KB branch and
        // rounds to "1024.0 KB", which is arithmetically right and reads as a
        // mistake. `whole` cannot exceed 1024 here -- the loop only reached
        // this unit because `bytes` was below the previous one -- so the
        // promotion is always to exactly 1.0 of it. No promotion exists above
        // GB, where a large `whole` is simply the honest answer.
        if (whole >= 1024 && i > 0)
            return QStringLiteral("1.0%1%2").arg(nf_nbsp())
                       .arg(QString::fromLatin1(kName[i - 1]));
        return QStringLiteral("%1.%2%3%4").arg(whole).arg(tenth)
                   .arg(nf_nbsp()).arg(QString::fromLatin1(kName[i]));
    }

    // Under 1 KB, including 0: whole bytes, no decimal. A "0.0 KB" row would
    // be less informative than "512 B" and no shorter.
    return QStringLiteral("%1%2B").arg(bytes).arg(nf_nbsp());
}

// The one state marker a FILE row carries, or nothing. Split out of
// nf_row_suffix when the size suffix landed, so that "exactly one of these
// three, in this priority order" stays a single `else if` chain with one
// return path rather than becoming a chain nested inside another branch --
// the priority IS the rule here, and it is easier to check when nothing else
// shares the function.
static QString nf_row_state_suffix(nf_row const& row) {
    if (!row.hasRow) {
        // A file with NO library row gets its reason spelled out in the
        // label TEXT itself, not left to colour/style alone: this panel
        // gives four grey levels, and "slightly lighter" reads as "the
        // same", not "different". The reference card's own example is
        // exactly one row -- Fullmetal Alchemist v26, a truncated file
        // Nickel's own import rejected (NOTES.md) -- and it must render as
        // clearly wrong, not silently vanish and leave a reader wondering
        // where volume 26 went. Which is also why it must not be elided
        // away: it is paid for out of the row budget before the name is.
        return nf_suffix_sep() + QStringLiteral("[not in library]");
    } else if (row.finished) {
        // A file WITH a library row gets a progress marker: a percentage
        // for in-progress books, a word for finished, and NOTHING for
        // unread. "Finished" takes priority over any number sitting in
        // percentRead -- a re-read that stopped partway through leaves a
        // lower value there, and the word is the more informative answer
        // regardless of what that number is.
        //
        // row.finished itself is DERIVED (nf_build_listing, nflist.cc) from
        // row.readState, whose primary source is Content::getReadStatus()
        // -- measured 0 = not started, 1 = in progress, 2 = finished
        // (NOTES.md).
        return nf_suffix_sep() + QStringLiteral("[finished]");
    } else if (row.percentRead > 0) {
        // 0% and -1 (unknown, including a firmware that moved the +140
        // offset -- nf_volume_exists's own guard) both render as nothing,
        // deliberately: Nickel's own BookWidget::getPercentReadString
        // clamps display to [1,99] for the same reason an untouched book's
        // own stored percentage is 0, not a real progress value (NOTES.md).
        return nf_suffix_sep() + QStringLiteral("(%1%)").arg(row.percentRead);
    }
    // Nothing at all: an unread book with a library row. Deliberately not a
    // word -- "unread" on the majority of rows would be noise, and its absence
    // already means exactly that.
    return QString();
}

void nf_row_suffix(nf_row const& row, nf_view_flags view,
                   QString *markup, QString *plain) {
    QString p;
    if (row.isDir) {
        // A folder gets a trailing "/" as well as the folder icon. KEPT
        // rather than replaced by the icon, even though the two now say the
        // same thing: the icon depends on a PNG nfview.cc drew, wrote to
        // /tmp and loaded back -- that is the whole reason it verifies
        // rather than assumes -- and if any step of that failed, this one
        // plain ASCII character is the only folder/file marker left that
        // does not depend on it. Same reasoning as the [not in library]
        // text below: the text carries the meaning, the picture is the
        // addition, never the other way round.
        //
        // No separator: the "/" belongs to the name it terminates, and a
        // gap in front of it would read as a row whose name ends in a
        // slash-shaped decoration rather than as a directory.
        //
        // A folder never carries a progress marker either -- there is no
        // Volume for one, so percentRead/finished are always -1/false for
        // it (nf_build_listing, nflist.cc: metadata is never fetched for a
        // directory row).
        p = QStringLiteral("/");
        // ...and NO SIZE, even when the reader asked for sizes. A directory's
        // stat()'d size is a filesystem block size, not the sum of what is
        // inside it -- nf_row_before says the same thing on its own side about
        // sorting folders by size -- so "4.0 KB" on a folder holding 3 GB of
        // comics would be a confident wrong answer, which is the one kind this
        // project refuses to render (the same rule as nf_date_key_is_plausible
        // and the percentRead range guard). Computing a real one would mean
        // walking the tree, which the browser deliberately never does.
    } else {
        // THE SIZE COMES FIRST, before whichever state marker follows, and it
        // is an ADDITION to that marker rather than one more branch of the
        // same either/or: a file can perfectly well be both 11.8 MB and
        // [not in library], and hiding one behind the other would make the
        // toggle look broken on exactly the rows it is most useful on.
        //
        // First because the state marker is the louder, rarer signal and reads
        // best at the end of the row, while a size is a routine attribute that
        // belongs next to the name -- the same order a file manager's columns
        // put them in. Both are paid for out of the elision reserve before the
        // name is (nfview.cc), so neither can be the part that falls off the
        // right edge; the order is about reading, not survival.
        if (view.showSize) {
            QString size = nf_format_size(row.size);
            if (!size.isEmpty()) // empty only for a negative size -- see nf_format_size
                p += nf_suffix_sep() + size;
        }
        p += nf_row_state_suffix(row);
    }
    if (plain)
        *plain = p;
    if (markup)
        *markup = nf_to_markup(p);
}

int nf_name_budget_px(int rowWidth, int iconWidth, int suffixWidth) {
    int w = rowWidth - iconWidth - suffixWidth;
    // The floor is what makes a pathological row degrade instead of
    // disappear (nffmt.h). It is deliberately applied to the RESULT rather
    // than to the inputs: a nonsense row width or a suffix wider than the
    // whole row are exactly the cases that must not come out negative, and
    // clamping the terms one by one would leave their sum unguarded anyway.
    if (w < NF_NAME_MIN_PX)
        w = NF_NAME_MIN_PX;
    return w;
}

// --- book covers --------------------------------------------------------
//
// nffmt.h has the measured path layout and the reason each of these three
// lives on the pure, host-tested side of the boundary rather than in
// nfview.cc next to the QFile::exists that consumes them.

QString nf_clean_image_id(QString const& id) {
    // Image::cleanId's own four QString::replace(QChar, QChar) calls, in its
    // own order (which cannot matter -- '_' is not one of the four searched
    // characters, so no replacement can create work for a later one; stated
    // because "these are order-independent" is exactly the kind of thing
    // that is obvious until '_' joins the set).
    QString out = id;
    out.replace(QLatin1Char('/'), QLatin1Char('_'));
    out.replace(QLatin1Char(':'), QLatin1Char('_'));
    out.replace(QLatin1Char('.'), QLatin1Char('_'));
    out.replace(QLatin1Char(' '), QLatin1Char('_'));
    return out;
}

unsigned nf_bucket_hash(QString const& imageId) {
    // IOUtil::bucketById's tail loop, instruction for instruction:
    //   ldrh   -> one UTF-16 code unit
    //   add    -> h = c + (h << 4)
    //   and/eor-> h ^= (h & 0xf0000000) >> 23
    //   bic    -> h &= 0x0fffffff
    // The 16x-unrolled body above it in the firmware computes the same
    // thing; there is no separate algorithm for long strings.
    //
    // `unsigned` rather than a signed int on purpose: the masks are defined
    // on an unsigned value, and >> on a negative signed int is
    // implementation-defined. The final mask keeps it inside 28 bits anyway,
    // so no shift here can reach the sign bit -- but relying on that would
    // be relying on the arithmetic to stay exactly as it is.
    unsigned h = 0;
    for (int i = 0; i < imageId.size(); i++) {
        h = (unsigned)imageId.at(i).unicode() + (h << 4);
        h ^= (h & 0xf0000000u) >> 23;
        h &= 0x0fffffffu;
    }
    return h;
}

QString nf_cover_path(QString const& imageId) {
    // The refusal, not a guard against a crash -- see nffmt.h: an empty id
    // hashes to bucket 0/0, which is a REAL directory on this card, so the
    // path would look entirely plausible and name a file that has nothing to
    // do with any book.
    if (imageId.isEmpty())
        return QString();

    unsigned h = nf_bucket_hash(imageId);
    // Plain decimal, both components, and the ImageId inserted verbatim --
    // Image::fileNameForType memcpys its argument in with no transform of
    // any kind, so anything this function does to `imageId` beyond
    // concatenation would be a divergence from Nickel's own naming.
    return QStringLiteral(NF_COVER_IMAGE_DIR "/%1/%2/%3 - " NF_COVER_TYPE ".parsed")
               .arg(h & 0xffu)
               .arg((h >> 8) & 0xffu)
               .arg(imageId);
}

int nf_items_per_page(bool covers, int chromeBarRows) {
    // An inline <img> sits on the TEXT BASELINE, so a row carrying one is
    // max(ascent, imageHeight) + descent tall -- which is why this is a
    // qMax and not a plain addition. A cover SHORTER than the ascent costs
    // nothing at all in row height (the text already reserves that much), and
    // then the two modes' page sizes coincide; today's 70 px cover is taller
    // than the 46 px ascent, so they do not. Written this way so that lowering
    // NF_COVER_H_PX below the ascent produces the right answer rather than a
    // quietly pessimistic one.
    int rowPx = covers ? (qMax(NF_FONT_ASCENT_PX, NF_COVER_H_PX) + NF_FONT_DESCENT_PX)
                       : NF_TEXT_ROW_PX;
    // The bar rows are the CALLER'S count now, not NF_CHROME_BARS, because the
    // command bar wraps to a second row when its items will not fit side by
    // side (nf_bar_plan_layout) and that row costs an item off every page. The
    // clamp is for a nonsense argument only: a negative count would otherwise
    // ADD area that does not exist and inflate the page size.
    if (chromeBarRows < 0)
        chromeBarRows = 0;
    int avail = NF_CONTENT_AREA_PX - chromeBarRows * NF_CHROME_BAR_PX;
    int n = avail / rowPx;
    // The floor is what makes a pathological geometry degrade instead of
    // disappear -- the same shape as nf_name_budget_px's. A page of zero items
    // would render as an empty folder with working page arrows, which reads as
    // "the card is broken" rather than as "the numbers above are wrong".
    if (n < 1)
        n = 1;
    return n;
}

int nf_cover_width_px(int heightPx) {
    // Rounded rather than truncated: at NF_COVER_H_PX (76) the exact width
    // is 50.8, and truncating would squeeze every cover by most of a pixel
    // in one direction only. The +/- half is done in integers to keep this
    // file free of anything that would pull in a maths runtime.
    int w = (heightPx * NF_COVER_NATIVE_W + NF_COVER_NATIVE_H / 2) / NF_COVER_NATIVE_H;
    // A zero-width <img> renders as nothing at all while the row still pays
    // for the height, which reads as "the cover is missing" when in fact the
    // height was nonsense. Refuse to produce that shape.
    if (w < 1)
        w = 1;
    return w;
}

void nf_bar_plan_layout(int const *naturalPx, int n, int availPx, nf_bar_plan *out) {
    if (!out)
        return;

    // EVERY FIELD IS WRITTEN before any early return below. The caller's
    // `nf_bar_plan` is an uninitialised local (it has to be -- nfview.cc may
    // not have file-scope objects with dynamic initialisers), so a field this
    // function left alone would be read as stack garbage, and `rowOf` garbage
    // in particular would index a bar row that does not exist.
    out->rows           = 1;
    out->naturalTotalPx = 0;
    out->anyElided      = false;
    for (int i = 0; i < NF_BAR_MAX_ITEMS; i++) {
        out->rowOf[i]    = 0;
        out->budgetPx[i] = 0;
        out->elided[i]   = false;
    }

    if (!naturalPx || n <= 0)
        return; // an empty bar is one empty row, not zero rows
    if (n > NF_BAR_MAX_ITEMS)
        n = NF_BAR_MAX_ITEMS;

    int sum = 0;
    for (int i = 0; i < n; i++) {
        // A negative width is nonsense from QFontMetrics and is taken as zero
        // rather than subtracted from the total, where it would buy room the
        // bar does not have.
        int w = naturalPx[i] > 0 ? naturalPx[i] : 0;
        out->budgetPx[i] = w; // provisional: what this item asked for
        sum += w;
    }
    out->naturalTotalPx = sum + (n - 1) * NF_BAR_MIN_GAP_PX;

    // RULE 1, and the non-positive-width case with it. A bar measured before
    // its dialog has been laid out has no honest width to elide against (the
    // 600-versus-1264 trap, CLAUDE.md), and hiding a label for a width that is
    // not a measurement is the failure this whole function exists to stop.
    if (availPx <= 0 || out->naturalTotalPx <= availPx)
        return;

    // RULE 2: fill row 0 until the next item would overflow, then wrap. The
    // gap is charged per JOIN, not per item, so a row of k items pays
    // (k-1) gaps -- the same accounting naturalTotalPx above uses.
    int row = 0, used = 0, count = 0;
    for (int i = 0; i < n; i++) {
        int need = (count > 0 ? NF_BAR_MIN_GAP_PX : 0) + out->budgetPx[i];
        if (count > 0 && used + need > availPx && row < NF_BAR_MAX_ROWS - 1) {
            row++;
            used  = 0;
            count = 0;
            need  = out->budgetPx[i]; // first on its row: no join to pay for
        }
        out->rowOf[i] = row;
        used  += need;
        count++;
    }
    out->rows = row + 1;

    // RULE 3: a row that STILL overflows shrinks proportionally. Only two
    // rows can reach here -- the last one (greedy has nowhere left to wrap
    // to) and, in the pathological case, a row holding one item wider than
    // the whole bar.
    //
    // PROPORTIONALLY, not into equal shares, and that is the whole point of
    // this task: equal shares are what elided a 357 px sort label against a
    // 190 px slot while 'view' sat in an identical slot it needed a third of.
    for (int r = 0; r < out->rows; r++) {
        int k = 0, rsum = 0;
        for (int i = 0; i < n; i++) {
            if (out->rowOf[i] != r)
                continue;
            k++;
            rsum += out->budgetPx[i];
        }
        if (k == 0)
            continue;
        int forItems = availPx - (k - 1) * NF_BAR_MIN_GAP_PX;
        // A floor of one px per item, so a pathological width degrades to
        // unreadable-but-present rather than to a row of nothing at all --
        // the same shape as nf_items_per_page's own floor.
        if (forItems < k)
            forItems = k;
        if (rsum <= forItems)
            continue; // this row fits at its natural widths
        for (int i = 0; i < n; i++) {
            if (out->rowOf[i] != r)
                continue;
            // rsum is summed BEFORE this loop writes anything, so every
            // share is computed against the row's natural total and not
            // against a total that shrank underneath it.
            int b = (int)(((qint64)out->budgetPx[i] * forItems) / rsum);
            if (b < 1)
                b = 1;
            if (b < out->budgetPx[i]) {
                out->budgetPx[i] = b;
                out->elided[i]   = true;
                out->anyElided   = true;
            }
        }
    }
}

void nf_page_bar_labels(int page, int totalPages,
                        QString *prev, bool *prevActive,
                        QString *pageText,
                        QString *next, bool *nextActive) {
    // The defensive clamp described in nffmt.h. totalPages floors at 1 so an
    // EMPTY listing still reads "page 1/1" rather than "page 1/0" -- an empty
    // folder is showing its one and only page, not a zeroth of none.
    if (totalPages < 1)
        totalPages = 1;
    if (page < 0)
        page = 0;
    if (page > totalPages - 1)
        page = totalPages - 1;

    bool hasPrev = page > 0;
    bool hasNext = page < totalPages - 1;

    // ASCII only, same as every other piece of chrome this mod draws
    // ("< BACK", "sort: name ^"): the arrow is "<" and ">", not a glyph this
    // panel's font may not carry.
    //
    // An unavailable end is an EMPTY string, not "no prev"/"no next" -- see
    // nffmt.h for why the words went and what keeps the counter from sliding
    // now that they are gone (the three equal-width slots, which never
    // depended on the text in the first place).
    if (prev)
        *prev = hasPrev ? QStringLiteral("< PREV") : QString();
    if (prevActive)
        *prevActive = hasPrev;
    if (pageText)
        *pageText = QStringLiteral("page %1/%2").arg(page + 1).arg(totalPages);
    if (next)
        *next = hasNext ? QStringLiteral("NEXT >") : QString();
    if (nextActive)
        *nextActive = hasNext;
}

// --- the sort, filter and view submenus ---------------------------------
//
// See nffmt.h for what the rows say, why the active one is marked in TEXT,
// and why the sort/filter row order is the cycle order those two menus
// replaced.

// THE ROW ORDER, once, as data rather than as a switch repeated per function:
// the count, the value at an index and the label at an index all read the
// same array, so the three cannot disagree about what row 3 is. Plain arrays
// of an enum type with constant initialisers -- no constructor to run, so
// they cost nothing at load and `nm libnfolders.so | grep GLOBAL__sub_I`
// stays empty, which is why there is no QString table here (CLAUDE.md's
// file-scope rule, and the crash that produced it).
static nf_sort_key const NF_MENU_SORT_ROWS[] = {
    NF_SORT_NAME, NF_SORT_SIZE, NF_SORT_DATE, NF_SORT_ADDED, NF_SORT_READ,
};

static nf_filter_kind const NF_MENU_FILTER_ROWS[] = {
    NF_FILTER_ALL, NF_FILTER_CBZ, NF_FILTER_CBR, NF_FILTER_PDF, NF_FILTER_EPUB,
    NF_FILTER_FINISHED, NF_FILTER_IN_PROGRESS, NF_FILTER_NOT_STARTED,
};

// THE VIEW MENU'S ROW ORDER, same shape and same reasoning as the two above.
// No cycle to preserve here -- this menu has no predecessor -- so the order is
// chosen: the two that change what a LABEL says first (they interact with each
// other, so they read best adjacent), then the one that changes the PAGE SIZE,
// then the one that changes which rows EXIST, then the one that adds a
// suffix. The one flag that is a genuine filter rather than a display choice
// (`hidden files`) is deliberately not first, where a reader scanning the menu
// would meet it before anything that explains the difference.
static nf_view_toggle const NF_MENU_VIEW_ROWS[] = {
    NF_VIEW_FILENAMES, NF_VIEW_EXTENSIONS, NF_VIEW_COVERS, NF_VIEW_HIDDEN,
    NF_VIEW_SIZE,
};

#define NF_MENU_SORT_ROW_COUNT   ((int)(sizeof NF_MENU_SORT_ROWS / sizeof NF_MENU_SORT_ROWS[0]))
#define NF_MENU_FILTER_ROW_COUNT ((int)(sizeof NF_MENU_FILTER_ROWS / sizeof NF_MENU_FILTER_ROWS[0]))
#define NF_MENU_VIEW_ROW_COUNT   ((int)(sizeof NF_MENU_VIEW_ROWS / sizeof NF_MENU_VIEW_ROWS[0]))

int nf_menu_row_count(nf_menu_kind menu) {
    switch (menu) {
        case NF_MENU_SORT:   return NF_MENU_SORT_ROW_COUNT;
        case NF_MENU_FILTER: return NF_MENU_FILTER_ROW_COUNT;
        case NF_MENU_VIEW:   return NF_MENU_VIEW_ROW_COUNT;
        case NF_MENU_NONE:
        default:             return 0;
    }
}

bool nf_menu_sort_key_at(int index, nf_sort_key *key) {
    if (index < 0 || index >= NF_MENU_SORT_ROW_COUNT)
        return false;
    if (key)
        *key = NF_MENU_SORT_ROWS[index];
    return true;
}

bool nf_menu_filter_at(int index, nf_filter_kind *filter) {
    if (index < 0 || index >= NF_MENU_FILTER_ROW_COUNT)
        return false;
    if (filter)
        *filter = NF_MENU_FILTER_ROWS[index];
    return true;
}

bool nf_menu_view_toggle_at(int index, nf_view_toggle *toggle) {
    if (index < 0 || index >= NF_MENU_VIEW_ROW_COUNT)
        return false;
    if (toggle)
        *toggle = NF_MENU_VIEW_ROWS[index];
    return true;
}

QString nf_sort_key_name(nf_sort_key key) {
    switch (key) {
        case NF_SORT_SIZE:  return QStringLiteral("size");
        case NF_SORT_DATE:  return QStringLiteral("date");
        case NF_SORT_ADDED: return QStringLiteral("added");
        case NF_SORT_READ:  return QStringLiteral("read");
        case NF_SORT_NAME:
        default:            return QStringLiteral("name");
    }
}

// The read-state names are spelled out in words ("finished", "in progress",
// "not started") rather than shortened to match the four lowercase format
// abbreviations above them: those abbreviations are the formats' own file
// extensions, which a reader already knows, whereas an abbreviated read state
// would be this mod inventing a vocabulary. This word is also what the
// "everything here was filtered out" message quotes back (nfview.cc), so it
// has to read as a sentence fragment, not as a code.
QString nf_filter_name(nf_filter_kind filter) {
    switch (filter) {
        case NF_FILTER_CBZ:         return QStringLiteral("cbz");
        case NF_FILTER_CBR:         return QStringLiteral("cbr");
        case NF_FILTER_PDF:         return QStringLiteral("pdf");
        case NF_FILTER_EPUB:        return QStringLiteral("epub");
        case NF_FILTER_FINISHED:    return QStringLiteral("finished");
        case NF_FILTER_IN_PROGRESS: return QStringLiteral("in progress");
        case NF_FILTER_NOT_STARTED: return QStringLiteral("not started");
        case NF_FILTER_ALL:
        default:                    return QStringLiteral("all");
    }
}

// THE DIRECTION, SPELLED OUT. This was "^" and "v" -- terse, and the owner's
// objection is that direction is the one thing on this screen a reader should
// not have to decode: a caret pointing at the top of the list reads as
// "ascending" only once you have been told that it does, and a "v" next to a
// word reads as a letter at least as readily as an arrow.
//
// Two forms, because the direction appears in two grammatical positions:
//   nf_sort_dir_mark  "(asc)" / "(desc)"  -- a STATE marker, bracketed like
//                                            the filter menu's "(active)"
//   nf_sort_dir_word  "asc"   / "desc"    -- inside another bracket, in the
//                                            active sort row's "(tap for
//                                            desc)". "(tap for (desc))" reads
//                                            as a typo, so the inner one
//                                            drops its brackets.
// Still plain ASCII: no glyph this panel's font may not carry, same as
// "< BACK"/"< PREV". Still single spaces only, so the no-collapsible-run rule
// (nffmt.h) holds.
static QString nf_sort_dir_word(bool descending) {
    return descending ? QStringLiteral("desc") : QStringLiteral("asc");
}

static QString nf_sort_dir_mark(bool descending) {
    return QStringLiteral("(%1)").arg(nf_sort_dir_word(descending));
}

QString nf_sort_bar_label(nf_sort_key key, bool descending) {
    return QStringLiteral("sort: %1 %2").arg(nf_sort_key_name(key),
                                             nf_sort_dir_mark(descending));
}

QString nf_filter_bar_label(nf_filter_kind filter) {
    return QStringLiteral("filter: %1").arg(nf_filter_name(filter));
}

// --- the view toggles' vocabulary ---------------------------------------
//
// Each toggle owns TWO words -- what it is called, and what each of its two
// states is called -- and both live here, in one switch each, for the same
// reason nf_sort_key_name/nf_filter_name do: the menu row, the bar item and
// the per-build log line all read them, so there is no second spelling to
// drift. Neither switch has a `default:`, so adding a toggle fails the build
// (-Wswitch, -Werror) until both words exist for it.
//
// THE STATE WORDS ARE THE TOGGLE'S OWN, not a shared on/off pair: "covers: on"
// and "hidden files: shown" both mean "the non-default one", but "covers:
// shown" and "hidden files: on" would each read as slightly wrong English,
// and a menu a reader has to translate is a menu they will misread once.
static QString nf_view_toggle_name(nf_view_toggle toggle) {
    switch (toggle) {
        case NF_VIEW_FILENAMES:  return QStringLiteral("filenames");
        case NF_VIEW_EXTENSIONS: return QStringLiteral("extensions");
        case NF_VIEW_COVERS:     return QStringLiteral("covers");
        case NF_VIEW_HIDDEN:     return QStringLiteral("hidden files");
        case NF_VIEW_SIZE:       return QStringLiteral("size");
    }
    return QString();
}

// `on` is the FLAG's value, i.e. always the non-default state -- see
// nf_view_flags (nffmt.h) for why every flag's `false` is today's behaviour
// and why the field names are spelled `hideExtensions`/`hideCovers` to keep
// it that way.
static QString nf_view_state_word(nf_view_toggle toggle, bool on) {
    switch (toggle) {
        case NF_VIEW_FILENAMES:  return on ? QStringLiteral("full")  : QStringLiteral("truncated");
        case NF_VIEW_EXTENSIONS: return on ? QStringLiteral("hidden"): QStringLiteral("shown");
        case NF_VIEW_COVERS:     return on ? QStringLiteral("off")   : QStringLiteral("on");
        case NF_VIEW_HIDDEN:     return on ? QStringLiteral("shown") : QStringLiteral("hidden");
        case NF_VIEW_SIZE:       return on ? QStringLiteral("shown") : QStringLiteral("hidden");
    }
    return QString();
}

QString nf_view_bar_label(nf_view_flags view) {
    // STATIC TEXT, deliberately -- see nffmt.h for the owner's reason. The
    // field-by-field comparison against the defaults that used to live here,
    // and produced "view: default"/"view: custom", is gone with the suffix it
    // fed; the submenu's own rows are where a toggle's state is stated, and
    // nf_view_flags_summary is where the whole set is logged.
    //
    // `view` is accepted and ignored. Keeping the parameter is not
    // absent-mindedness: this function is the bar's half of the one-place-per-
    // word rule that nf_sort_bar_label and nf_filter_bar_label also serve, and
    // a signature change would be the moment a call site starts spelling its
    // own label instead. (void) to keep -Wunused-parameter quiet under
    // -Werror without turning the parameter into a comment.
    (void)view;
    return QStringLiteral("view");
}

QString nf_view_flags_summary(nf_view_flags view) {
    QString out;
    for (int i = 0; i < NF_MENU_VIEW_ROW_COUNT; i++) {
        if (i)
            out += QStringLiteral(" | ");
        // THE MENU'S OWN ROW TEXT, not a second rendering of the same facts.
        // The whole purpose of this line is to make a screenshot
        // attributable to a mode (nffmt.h), and a log line that could say
        // something the menu does not would defeat exactly that.
        out += nf_menu_row_label(NF_MENU_VIEW, i, NF_SORT_NAME, false,
                                 NF_FILTER_ALL, view);
    }
    return out;
}

QString nf_menu_row_label(nf_menu_kind menu, int index,
                          nf_sort_key activeKey, bool activeDesc,
                          nf_filter_kind activeFilter,
                          nf_view_flags view) {
    if (menu == NF_MENU_SORT) {
        nf_sort_key key = NF_SORT_NAME;
        if (!nf_menu_sort_key_at(index, &key))
            return QString();
        QString name = nf_sort_key_name(key);
        if (key != activeKey)
            return name;
        // The active row carries three things a tap needs to be predictable:
        // the "* " mark (this is the one in force), the CURRENT direction, and
        // the direction a tap will move it to. The last of those is the whole
        // discoverability argument -- with cycling gone, tapping the already-
        // active key is the only way to reverse, and nothing else on screen
        // would say so.
        return QStringLiteral("* %1 %2 (tap for %3)")
                   .arg(name, nf_sort_dir_mark(activeDesc),
                        nf_sort_dir_word(!activeDesc));
    }

    if (menu == NF_MENU_FILTER) {
        nf_filter_kind filter = NF_FILTER_ALL;
        if (!nf_menu_filter_at(index, &filter))
            return QString();
        QString name = nf_filter_name(filter);
        if (filter != activeFilter)
            return name;
        // No "(tap for ...)" twin here, deliberately: a filter has no second
        // axis, so tapping the active row re-selects the same value and
        // closes the menu. Saying "(active)" states what the row IS, which is
        // the only honest thing to promise -- a hint about what a tap does
        // would have to describe doing nothing.
        return QStringLiteral("* %1 (active)").arg(name);
    }

    if (menu == NF_MENU_VIEW) {
        nf_view_toggle toggle = NF_VIEW_FILENAMES;
        if (!nf_menu_view_toggle_at(index, &toggle))
            return QString();
        // "name: state", and NO "* " mark on any row: there is no active row
        // to mark, because every row is a toggle carrying its own state
        // rather than one of a set of which exactly one is in force. A mark
        // here would have to mean "not the default", which is a different
        // question from the one the other two menus' marks answer, and two
        // meanings for the same glyph on adjacent screens is how a reader
        // learns the wrong one.
        //
        // The STATE IS IN THE TEXT, which is the whole rule for this panel:
        // four grey levels, on which "slightly lighter" reads as "the same"
        // -- the same finding that puts "[not in library]" into a row's words
        // rather than leaving it to colour (nf_row_suffix).
        return QStringLiteral("%1: %2").arg(nf_view_toggle_name(toggle),
                                            nf_view_state_word(toggle, nf_view_flag_value(view, toggle)));
    }

    return QString();
}

// --- select mode and the file operations --------------------------------
//
// Words only -- see nffmt.h. Nothing below can touch a path; the guards are
// nfpath.h's and the calls are nfops.h's.

QString nf_item_count_text(int count) {
    if (count < 0)
        count = 0; // a count has a floor where a size does not -- nffmt.h
    return (count == 1)
        ? QStringLiteral("1 item")
        : QStringLiteral("%1 items").arg(count);
}

bool nf_menu_is_confirm(nf_menu_kind menu) {
    return menu == NF_MENU_CONFIRM_DELETE ||
           menu == NF_MENU_CONFIRM_PASTE  ||
           menu == NF_MENU_CONFIRM_RESCAN;
}

QString nf_confirm_header(nf_menu_kind menu, int count, bool cut) {
    switch (menu) {
        case NF_MENU_CONFIRM_DELETE:
            // "cannot be undone" is not decoration: this device has no
            // recycle bin, this mod has no undo, and a book deleted here is
            // gone from the owner's own library.
            return QStringLiteral("Delete %1 from this folder? This cannot be undone.")
                       .arg(nf_item_count_text(count));
        case NF_MENU_CONFIRM_PASTE:
            // The VERB distinguishes the two clipboards, because the
            // consequence differs: a move leaves nothing behind where a copy
            // duplicates. The clipboard's own source folder is named on the
            // screen itself (nfview.cc), not here -- a path does not belong
            // in a sentence that has to survive nh_log's truncation as well
            // as fit a panel.
            return cut
                ? QStringLiteral("Move %1 into this folder?").arg(nf_item_count_text(count))
                : QStringLiteral("Copy %1 into this folder?").arg(nf_item_count_text(count));
        case NF_MENU_CONFIRM_RESCAN:
            // WI-FI IS NAMED, and a test pins the word. PlugWorkflowManager::
            // sync() runs Nickel's whole post-USB workflow on completion,
            // which calls connectWirelessSilently() -- the owner chose a
            // manual button because of exactly that, so the screen they tap
            // has to say it (rescan-archaeology.md, section 7.2).
            //
            // "may show its own dialogs" covers the up-to-three modal dialogs
            // and the processing screen that workflow can also put up, which
            // is the other thing a reader would otherwise read as this mod
            // having gone wrong.
            return QStringLiteral("Rescan the library now? When it finishes, Nickel turns Wi-Fi ON "
                                  "and may show its own dialogs. It does not block -- the scan runs "
                                  "in the background.");
        case NF_MENU_NONE:
        case NF_MENU_SORT:
        case NF_MENU_FILTER:
        case NF_MENU_VIEW:
        default:
            return QString();
    }
}

int nf_confirm_row_count(nf_menu_kind menu) {
    switch (menu) {
        case NF_MENU_CONFIRM_DELETE: return 2; // cancel, delete
        case NF_MENU_CONFIRM_PASTE:  return 3; // cancel, paste, clear the clipboard
        case NF_MENU_CONFIRM_RESCAN: return 2; // cancel, rescan
        case NF_MENU_NONE:
        case NF_MENU_SORT:
        case NF_MENU_FILTER:
        case NF_MENU_VIEW:
        default:                     return 0;
    }
}

QString nf_confirm_row_label(nf_menu_kind menu, int index, int count, bool cut) {
    if (index < 0 || index >= nf_confirm_row_count(menu))
        return QString(); // refuse an out-of-range index rather than guess one
                          // -- the same rule as nf_menu_sort_key_at's

    // INDEX 0 IS ALWAYS CANCEL, on every confirmation screen, so the top row
    // is harmless on all three and a reader who has learned "the first row
    // backs out" is never wrong. See nffmt.h.
    if (index == 0)
        return QStringLiteral("cancel");

    switch (menu) {
        case NF_MENU_CONFIRM_DELETE:
            return QStringLiteral("delete %1").arg(nf_item_count_text(count));
        case NF_MENU_CONFIRM_PASTE:
            if (index == 1)
                return cut
                    ? QStringLiteral("move %1 here").arg(nf_item_count_text(count))
                    : QStringLiteral("copy %1 here").arg(nf_item_count_text(count));
            // The only way to put a pending clipboard DOWN. Without it a cut
            // that the reader changed their mind about would follow them from
            // folder to folder for the rest of the session, with the bar
            // permanently one item wider and `paste` permanently offering to
            // move files they had stopped thinking about.
            return QStringLiteral("clear the clipboard");
        case NF_MENU_CONFIRM_RESCAN:
            // The consequence is repeated on the row itself, not only in the
            // header: the row is what gets tapped, and a row that reads
            // "rescan now" alone could be tapped by someone who skimmed past
            // the sentence above it.
            return QStringLiteral("rescan now (turns Wi-Fi on)");
        case NF_MENU_NONE:
        case NF_MENU_SORT:
        case NF_MENU_FILTER:
        case NF_MENU_VIEW:
        default:
            return QString(); // unreachable -- nf_confirm_row_count is 0 for these
    }
}

QString nf_select_bar_label(int selected) {
    if (selected < 0)
        selected = 0;
    return QStringLiteral("done (%1)").arg(selected);
}

QString nf_paste_bar_label(int pending) {
    if (pending < 0)
        pending = 0;
    return QStringLiteral("paste (%1)").arg(pending);
}

void nf_select_marker(bool selected, QString *markup, QString *plain) {
    // Both forms three characters plus one separator wide, ticked or not, so
    // the names after them line up down the page -- the same reason
    // nf_icon_badge's five badges are all five characters.
    //
    // The inner space of "[ ]" is non-breaking like every other space this
    // file builds: rich text collapses a run of ordinary whitespace, so an
    // ASCII space here would render "[]" while MEASURING as "[ ]", which is
    // precisely the measure-versus-render mismatch that clipped every row on
    // 2026-09-04 (nf_nbsp).
    QString p = selected
        ? QStringLiteral("[x]")
        : (QStringLiteral("[") + nf_nbsp() + QStringLiteral("]"));
    p += nf_nbsp(); // the separator between the marker and the name
    if (plain)
        *plain = p;
    if (markup)
        *markup = nf_to_markup(p);
}
