// PURE path-safety predicates for the file operations (nfops.h, nfview.cc).
// Deliberately free of libnickel, NickelHook and I/O -- same rule as nffmt.h
// and nflist.h, and for a sharper reason than either of those: this is the
// layer that decides whether a delete, a move or a copy is allowed to touch a
// path at all, on the owner's real book library, on a device they use daily.
// There is no undo anywhere in this design, so the guards are the whole
// safety story, and a guard that cannot be run off-device is a guard nobody
// can check.
//
// THE SPLIT IS THE POINT. Canonicalisation needs the filesystem (a symlink
// must actually be followed, a "." must actually be resolved), so it lives in
// nfops.cc. Everything this header declares operates on strings that are
// ALREADY canonical, which is exactly the part a host test can hammer --
// including the case that is the classic bug in this kind of check, and which
// has its own test: "/mnt/onboard/books2" is a string prefix of nothing and a
// path prefix of nothing, but a naive startsWith() against "/mnt/onboard/book"
// says otherwise.
#ifndef NFPATH_H
#define NFPATH_H

#include <QString>

// The one tree this mod is allowed to touch. Same literal as nfview.cc's
// NF_ROOT and NOTES.md's ContentID prefix; spelled again here rather than
// included from nfview.h because this file must stay pure (nfview.h drags in
// the whole libnickel side), and a second spelling of a constant that can
// never move is cheaper than the dependency. nf_path_root_is_consistent()
// below exists so a test can pin the two together anyway.
#define NF_PATH_ROOT "/mnt/onboard"

// Directories that must never be the target of an operation, nor contain one,
// even when `view: hidden files` makes them visible. The view toggle exists
// to let the owner SEE them, which is not the same as acting on them.
//
//   .kobo                      the library database AND the device identity
//                              file (/mnt/onboard/.kobo/version -- CLAUDE.md
//                              records what a corrupted one costs). Deleting
//                              this directory destroys the library.
//   .kobo-images               every rendered cover (nffmt.h's own
//                              NF_COVER_IMAGE_DIR).
//   .adds                      every other mod on this device -- NickelMenu,
//                              NickelDBus, kfmon, KOReader, koboy, and this
//                              mod's own restart-nickel.sh.
//   System Volume Information  Windows' own metadata on a VFAT card; a USB
//                              session will recreate it and chkdsk cares.
//
// MATCHED AT ANY DEPTH, not just directly under the root, and that is the
// conservative direction on purpose: a ".adds" nested three levels down is
// almost certainly a mod install someone unpacked in the wrong place, and
// refusing to delete a folder the owner really did name ".adds" themselves
// costs them one ssh command, where the other error costs them their mods.
// The negative controls in tests/test_nfpath.cc pin that this does NOT
// over-reach onto neighbours -- "adds", ".addsx", "kobo", ".kobold" and
// "System Volume Information Backup" are all ordinary names.
//
// CASE-INSENSITIVE, because /mnt/onboard is VFAT: the same directory can come
// back as "System Volume Information" or "SYSTEM~1"/"SYSTEM VOLUME
// INFORMATION" depending on what wrote it and how the card is mounted, and a
// case-sensitive check would be a guard that any Windows machine can switch
// off. (The 8.3 short name is a separate hazard this cannot see and does not
// claim to -- see the report's "out of scope".)

// Every way a path can be refused. NF_PATH_OK is the ZERO value on purpose,
// which is the OPPOSITE of the convention the rest of this project uses for
// its "unknown" enum zeros (NF_READ_UNKNOWN, NF_ICON_UNKNOWN, NF_MENU_NONE)
// -- and the inversion is deliberate and load-bearing. Those three are
// display state, where a zeroed value must read as "we do not know". This one
// gates a DESTRUCTIVE call, so a zeroed or uninitialised verdict must never
// read as "allowed": it is therefore checked at exactly one place per
// operation, immediately after the call that produces it, and never stored.
// Spelled out here so nobody "fixes" the inconsistency by adding an
// NF_PATH_UNKNOWN = 0 in front of it and silently turning every
// default-constructed verdict into a refusal that reads like an error.
enum nf_path_verdict {
    NF_PATH_OK,
    NF_PATH_UNCLEAN,            // empty, relative, "//", or a "." / ".." component survived canonicalisation
    NF_PATH_OUTSIDE_ROOT,       // not under /mnt/onboard at all
    NF_PATH_IS_ROOT,            // /mnt/onboard itself
    NF_PATH_PROTECTED,          // is, or is inside, one of the four directories above
    NF_PATH_IS_CWD_OR_ANCESTOR, // the folder being browsed, or one containing it
    NF_PATH_DEST_INSIDE_SOURCE, // a destination that is the source, or inside it
};

// One short phrase per verdict, for the log line and the refusal shown to the
// owner. Present tense, says what was refused and why, never "error".
char const *nf_path_verdict_text(nf_path_verdict v);

// LEXICALLY clean and absolute: begins with '/', no empty component ("//"),
// no "." or ".." component, and no trailing '/' (the bare root "/" is the one
// exception, and is still not NF_PATH_OK anywhere below -- it is simply not
// UNCLEAN).
//
// Checked even though every path reaching these functions has been through
// QFileInfo::canonicalFilePath (nfops.cc), which is supposed to produce
// exactly this shape. That is the reason to check it, not a reason to skip
// it: canonicalFilePath returns an EMPTY string for a path that does not
// exist, and an empty string that went unexamined would sail through a
// startsWith() test against the root as "not inside it" -- refused by luck
// rather than by rule. Here it is refused by name.
bool nf_path_is_clean(QString const& path);

// True iff `child` is STRICTLY below `dir`, comparing whole path components.
// This is the function the classic bug lives in: "/mnt/onboard/books2" is not
// inside "/mnt/onboard/books", and a bare startsWith() says it is. Both
// arguments must be clean; a non-clean one is never "inside" anything.
bool nf_path_is_inside(QString const& child, QString const& dir);

// True iff `anc` IS `path` or contains it. The self case is included
// deliberately: every caller that asks this is asking "would operating on
// `anc` pull `path` out from under someone", and `anc == path` is the most
// direct way for that to be true.
bool nf_path_is_ancestor_or_self(QString const& anc, QString const& path);

// True iff any component of `path` is one of the four protected directory
// names, compared case-insensitively. See the block comment above for the
// list and for why any depth and why case-insensitive.
bool nf_path_is_protected(QString const& path);

// True for a single path COMPONENT that is safe to append to a directory:
// non-empty, not "." or "..", and containing no '/' and no NUL. Used on a
// name that came off the card (a directory entry) or out of the clipboard
// before it is joined to a destination directory -- a name containing a '/'
// would silently make `destDir + "/" + name` a path two levels down, which is
// the one way a destination can escape without any ".." being involved.
bool nf_path_name_is_safe(QString const& name);

// THE WHOLE VERDICT for a path a destructive operation is about to READ FROM
// or DESTROY -- the source of a delete, a cut or a copy. `cwd` is the folder
// the browser is currently showing, and must itself be clean (if it is not,
// this returns NF_PATH_UNCLEAN rather than skipping the ancestor rule: a
// guard that cannot be evaluated must refuse, never wave through).
//
// The order the rules are applied in is the order they are listed in
// nf_path_verdict, and it is chosen so the FIRST answer is the most specific
// one a reader can act on -- "that is the folder you are in" beats "that is
// inside /mnt/onboard", which is true of everything.
nf_path_verdict nf_path_check_source(QString const& path, QString const& cwd);

// THE WHOLE VERDICT for a path an operation is about to CREATE -- the
// destination of a paste. Takes the source as well, because the one rule a
// destination has that a source does not is that it may not be the source
// itself, nor anywhere inside it: `rename("/a/b", "/a/b/c")` is how a
// directory move eats itself, and `rename(x, x)` succeeds silently while
// having done nothing, which would then have the caller report a move that
// never happened.
//
// It does NOT check whether the destination already exists -- that is a
// filesystem question and belongs in nfops.cc, which refuses it separately
// (paste must never silently overwrite).
nf_path_verdict nf_path_check_dest(QString const& dest, QString const& src);

// THE VERDICT for the destination FOLDER a paste is landing in -- the folder
// the reader is currently looking at. The source rules minus the cwd test,
// because this path IS the cwd and passing it as its own `cwd` would refuse
// every paste.
//
// It lives here, rather than as a chain of `if` clauses in nfops.cc where it
// started, for the reason this whole header exists: it is the only guard that
// was written on the untestable side, and an inverted clause in it would have
// failed no test. `nf_path_check_dest` independently re-covers everything it
// says except the root case -- so the exposure was never data loss, only a
// rule nothing pinned.
//
// /mnt/onboard ITSELF is refused, and that one IS a judgement rather than a
// safety rule: the root is where Nickel's own .kobo/.adds live, it is the
// directory a USB session mounts, and a book dropped straight into it is the
// mess this browser exists to help the owner out of. Pasting into any real
// folder under it works, which is what the tests pin on the other side.
nf_path_verdict nf_path_check_dest_dir(QString const& dir);

// The temporary name a chunked copy writes to inside the DESTINATION
// directory, before renaming it into place on success. Returns an empty
// QString for a name nf_path_name_is_safe rejects, so a caller that forgets
// to check gets a refusal rather than a path.
//
// THE SHAPE IS "." + name + ".nfolders-part", and every part of it is load-
// bearing:
//
//   - the leading "." makes it a dotfile, so nothing on the card treats it as
//     content while it is incomplete;
//   - the TRAILING marker, not a leading one, is what keeps it out of this
//     mod's own listing: files are admitted by an extension ALLOWLIST
//     (nf_is_book_name, nffmt.h), so ".Foo.epub.nfolders-part" does not end
//     in a book extension and is never listed, where a leading-marker form
//     (".nfolders-part-Foo.epub") still ends in ".epub" and WOULD show up as
//     a row -- as a partial file, mid-copy, tappable.
//   - bounded to NF_TEMP_NAME_MAX QChars, because VFAT's long-name limit is
//     255 UTF-16 code units and the reference card already holds 230-
//     character names. THE TAIL OF THE BASE NAME IS WHAT GETS DROPPED, not
//     its middle: the tail is where the extension is, and dropping it is what
//     keeps a long name's temp out of the listing's extension allowlist for
//     the same reason the trailing marker does. (This comment said "middle"
//     and the code has always cut the tail -- a stated measurement the code
//     contradicted, in the one file whose whole argument is that its rules are
//     written down and pinned. The code is what was meant; the words were
//     wrong.) A trailing surrogate is never split.
//
// It is NOT unique per call, and does not need to be: pastes run one item at
// a time on the GUI thread (nfview.cc's nf_op_busy guard), and each item's
// temp is renamed into place or unlinked before the next one starts. What it
// must survive is a LEFTOVER from a crashed run, which nfops.cc removes by
// name before it starts writing.
#define NF_TEMP_NAME_MAX 200
QString nf_temp_name(QString const& finalName);

// True iff NF_PATH_ROOT is still the literal this file's rules are written
// against. Exists so a host test can pin the constant that every predicate
// here is implicitly parameterised by -- there is no other way for a test to
// notice that the root moved and took the meaning of every check with it.
bool nf_path_root_is_consistent(void);

#endif
