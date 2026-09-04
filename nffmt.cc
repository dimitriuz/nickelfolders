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

void nf_row_suffix(nf_row const& row, QString *markup, QString *plain) {
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
    } else if (!row.hasRow) {
        // A file with NO library row gets its reason spelled out in the
        // label TEXT itself, not left to colour/style alone: this panel
        // gives four grey levels, and "slightly lighter" reads as "the
        // same", not "different". The reference card's own example is
        // exactly one row -- Fullmetal Alchemist v26, a truncated file
        // Nickel's own import rejected (NOTES.md) -- and it must render as
        // clearly wrong, not silently vanish and leave a reader wondering
        // where volume 26 went. Which is also why it must not be elided
        // away: it is paid for out of the row budget before the name is.
        p = nf_suffix_sep() + QStringLiteral("[not in library]");
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
        p = nf_suffix_sep() + QStringLiteral("[finished]");
    } else if (row.percentRead > 0) {
        // 0% and -1 (unknown, including a firmware that moved the +140
        // offset -- nf_volume_exists's own guard) both render as nothing,
        // deliberately: Nickel's own BookWidget::getPercentReadString
        // clamps display to [1,99] for the same reason an untouched book's
        // own stored percentage is 0, not a real progress value (NOTES.md).
        p = nf_suffix_sep() + QStringLiteral("(%1%)").arg(row.percentRead);
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
