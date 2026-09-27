// PURE path-safety predicates -- see nfpath.h for what each one promises and
// why this layer is separated from the canonicalisation that feeds it.
#include "nfpath.h"

#include <QChar>
#include <QLatin1Char>
#include <QStringList>

// The four names nothing may touch. One table, iterated by every check, so
// adding a fifth is one line and cannot be added to one check and forgotten in
// another. QLatin1String rather than QStringLiteral: these are compared
// against, never returned, and a QLatin1String comparison allocates nothing
// at all -- which matters because nf_path_is_protected runs once per
// component per path per operation.
//
// NOT at file scope as QString objects: CLAUDE.md forbids a file-scope object
// with a dynamically initialised constructor, and a QString array is exactly
// that. `char const *const[]` is POD, lives in .rodata, and has no
// constructor to race NickelHook's nh_init.
static char const *const NF_PROTECTED_DIRS[] = {
    ".kobo",
    ".kobo-images",
    ".adds",
    "System Volume Information",
};
#define NF_PROTECTED_DIR_COUNT ((int)(sizeof NF_PROTECTED_DIRS / sizeof NF_PROTECTED_DIRS[0]))

char const *nf_path_verdict_text(nf_path_verdict v) {
    switch (v) {
        case NF_PATH_OK:                 return "allowed";
        case NF_PATH_UNCLEAN:            return "the path is not a clean absolute path";
        case NF_PATH_OUTSIDE_ROOT:       return "the path is outside " NF_PATH_ROOT;
        case NF_PATH_IS_ROOT:            return "that is " NF_PATH_ROOT " itself";
        case NF_PATH_PROTECTED:          return "that is a protected system folder";
        case NF_PATH_IS_CWD_OR_ANCESTOR: return "that is the folder you are in, or one containing it";
        case NF_PATH_DEST_INSIDE_SOURCE: return "the destination is inside the source";
    }
    // Unreachable while the switch above is exhaustive (-Wswitch -Werror makes
    // adding a verdict without a phrase a build failure), and answered anyway
    // rather than left to fall off the end: a refusal with no words is a
    // refusal the owner cannot act on.
    return "refused";
}

bool nf_path_is_clean(QString const& path) {
    if (path.isEmpty())
        return false;
    if (!path.startsWith(QLatin1Char('/')))
        return false;
    if (path == QLatin1String("/"))
        return true; // the one path allowed to "end in" a slash
    if (path.endsWith(QLatin1Char('/')))
        return false;
    if (path.contains(QLatin1String("//")))
        return false;
    if (path.contains(QChar(QLatin1Char('\0'))))
        return false;

    // Component by component, never by substring: ".." as a SUBSTRING is
    // perfectly ordinary in a book name ("Vol..2"), and a substring test would
    // refuse it while still missing nothing a component test catches. The
    // leading empty element that split() produces for a path starting with '/'
    // is skipped rather than special-cased away, because "//" is already
    // refused above -- so index 0 is the only empty one that can exist here.
    QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 1; i < parts.size(); i++) {
        QString const& p = parts.at(i);
        if (p.isEmpty())
            return false; // unreachable given the "//" check above; a floor, not a repair
        if (p == QLatin1String(".") || p == QLatin1String(".."))
            return false;
    }
    return true;
}

bool nf_path_is_inside(QString const& child, QString const& dir) {
    if (!nf_path_is_clean(child) || !nf_path_is_clean(dir))
        return false;
    if (child == dir)
        return false; // STRICTLY below -- nf_path_is_ancestor_or_self is the one that includes self

    // THE COMPONENT BOUNDARY, and this line is the whole function. A bare
    // child.startsWith(dir) answers YES for "/mnt/onboard/books2" against
    // "/mnt/onboard/books", because "books2" starts with "books" -- the
    // classic bug in this kind of check, and the one tests/test_nfpath.cc
    // names directly. Requiring the separator makes the comparison happen on
    // component boundaries, where paths actually nest.
    //
    // The bare root "/" is the one directory that already ends in its own
    // separator, so appending another would ask for "//..." and never match.
    QString prefix = (dir == QLatin1String("/")) ? dir : dir + QLatin1Char('/');
    return child.startsWith(prefix);
}

bool nf_path_is_ancestor_or_self(QString const& anc, QString const& path) {
    if (!nf_path_is_clean(anc) || !nf_path_is_clean(path))
        return false;
    if (anc == path)
        return true;
    return nf_path_is_inside(path, anc);
}

bool nf_path_is_protected(QString const& path) {
    if (!nf_path_is_clean(path))
        return false; // an unclean path is refused by nf_path_is_clean's own verdict, not by this one

    QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 1; i < parts.size(); i++) {
        for (int k = 0; k < NF_PROTECTED_DIR_COUNT; k++) {
            // WHOLE COMPONENT, case-insensitively -- see nfpath.h for why both
            // halves of that are deliberate. compare() rather than toLower()
            // on either side: toLower() on a Turkish locale maps 'I' to a
            // dotless 'ı', which would quietly stop ".KOBO" matching ".kobo".
            if (parts.at(i).compare(QLatin1String(NF_PROTECTED_DIRS[k]), Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

bool nf_path_name_is_safe(QString const& name) {
    if (name.isEmpty())
        return false;
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    if (name.contains(QLatin1Char('/')))
        return false;
    if (name.contains(QChar(QLatin1Char('\0'))))
        return false;
    return true;
}

nf_path_verdict nf_path_check_source(QString const& path, QString const& cwd) {
    if (!nf_path_is_clean(path))
        return NF_PATH_UNCLEAN;
    // The browsing directory is part of this rule, so a `cwd` that cannot be
    // read is a rule that cannot be applied -- refuse rather than skip it.
    // Reported as UNCLEAN because that is literally what is wrong, and the
    // log line names which of the two paths it was.
    if (!nf_path_is_clean(cwd))
        return NF_PATH_UNCLEAN;

    if (path == QLatin1String(NF_PATH_ROOT))
        return NF_PATH_IS_ROOT;
    if (!nf_path_is_inside(path, QLatin1String(NF_PATH_ROOT)))
        return NF_PATH_OUTSIDE_ROOT;
    if (nf_path_is_protected(path))
        return NF_PATH_PROTECTED;
    // Checked LAST of the four because it is the narrowest: a path can only be
    // the cwd or an ancestor of it once it is already known to be a real,
    // in-tree, unprotected path, and reporting the narrow reason first is what
    // makes the refusal act-on-able ("that is the folder you are in" rather
    // than "that is inside /mnt/onboard", which is true of everything).
    if (nf_path_is_ancestor_or_self(path, cwd))
        return NF_PATH_IS_CWD_OR_ANCESTOR;
    return NF_PATH_OK;
}

nf_path_verdict nf_path_check_dest(QString const& dest, QString const& src) {
    if (!nf_path_is_clean(dest))
        return NF_PATH_UNCLEAN;
    if (!nf_path_is_clean(src))
        return NF_PATH_UNCLEAN;

    if (dest == QLatin1String(NF_PATH_ROOT))
        return NF_PATH_IS_ROOT;
    if (!nf_path_is_inside(dest, QLatin1String(NF_PATH_ROOT)))
        return NF_PATH_OUTSIDE_ROOT;
    if (nf_path_is_protected(dest))
        return NF_PATH_PROTECTED;
    // dest == src is included here, via nf_path_is_ancestor_or_self's own self
    // case: rename(x, x) SUCCEEDS and does nothing, so a caller that allowed
    // it would report a move that never happened. A cut-and-paste into the
    // same folder is exactly how a reader would produce it.
    if (nf_path_is_ancestor_or_self(src, dest))
        return NF_PATH_DEST_INSIDE_SOURCE;
    return NF_PATH_OK;
}

nf_path_verdict nf_path_check_dest_dir(QString const& dir) {
    if (!nf_path_is_clean(dir))
        return NF_PATH_UNCLEAN;
    // The root is refused BEFORE the inside-the-root test, so the answer names
    // the specific thing rather than the true-of-everything one -- the same
    // ordering nf_path_check_source uses and for the same reason.
    if (dir == QLatin1String(NF_PATH_ROOT))
        return NF_PATH_IS_ROOT;
    if (!nf_path_is_inside(dir, QLatin1String(NF_PATH_ROOT)))
        return NF_PATH_OUTSIDE_ROOT;
    if (nf_path_is_protected(dir))
        return NF_PATH_PROTECTED;
    // Deliberately NO cwd-or-ancestor test: this path IS the folder being
    // browsed, so that rule would refuse every paste there is.
    return NF_PATH_OK;
}

QString nf_temp_name(QString const& finalName) {
    if (!nf_path_name_is_safe(finalName))
        return QString();

    QString const prefix = QStringLiteral(".");
    QString const suffix = QStringLiteral(".nfolders-part");
    int const fixed = prefix.length() + suffix.length();

    QString base = finalName;
    if (fixed + base.length() > NF_TEMP_NAME_MAX) {
        int room = NF_TEMP_NAME_MAX - fixed;
        if (room < 1)
            return QString(); // unreachable while NF_TEMP_NAME_MAX > 15; a floor, not a repair
        // THE TAIL IS WHAT GOES, not the middle -- see nfpath.h. Keeping the
        // head is what takes the extension off a long name, which is the same
        // property the trailing ".nfolders-part" marker gives a short one.
        //
        // A TRAILING surrogate must not be split: QString counts UTF-16 code
        // units, so cutting between a high and a low surrogate leaves half a
        // character, which is a filename no filesystem should be asked to
        // store. Dropping one more unit is always safe -- the pair is two.
        if (room > 0 && base.at(room - 1).isHighSurrogate())
            room--;
        base = base.left(room);
        if (base.isEmpty())
            return QString();
    }
    return prefix + base + suffix;
}

bool nf_path_root_is_consistent(void) {
    // Spelled as a comparison against the literal rather than `return true`,
    // so the check is against the STRING and not against this function having
    // been compiled at all.
    return QLatin1String(NF_PATH_ROOT) == QStringLiteral("/mnt/onboard");
}
