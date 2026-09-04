#include "nflist.h"

void nf_build_listing(QVector<nf_entry> const& entries,
                      nf_meta_fn meta, void *ctx,
                      QVector<nf_row> *out,
                      nf_filter_kind filter, nf_sort_key key, bool descending,
                      bool *filteredToNothing) {
    out->clear();

    // 1. Hide junk. FIRST, because everything downstream is computed over the
    //    set of rows that survive -- see the label stage.
    QVector<nf_entry> kept;
    for (int i = 0; i < entries.size(); i++) {
        nf_entry const& e = entries.at(i);
        if (e.isDir) {
            if (!nf_is_hidden_dir(e.name))
                kept << e;
        } else if (nf_is_book_name(e.name)) {
            kept << e;
        }
    }

    // 1.5. User filter. Spec section 6.1's own stage, right after hide-junk
    //    and before everything else -- in particular before labelling
    //    (section 4, below), which is what test_labels_are_derived_after_
    //    filtering (test_nflist.cc) pins for the ORIGINAL junk-hiding stage
    //    and now pins for this one too.
    //
    //    A SEPARATE layer from the allowlist above (section 6.3): that
    //    answers "is this a book", this answers "which books do I want to
    //    see right now". Merging them would make filtering to PDF start
    //    arguing with the junk-hiding rule -- e.g. a filter that only
    //    understood "keep .pdf" would have to independently reject
    //    "metadata.pdf.lua" the allowlist already rejects for an unrelated
    //    reason, the same logic duplicated in two places.
    //
    //    A folder is NEVER removed here, regardless of `filter` -- a folder
    //    may contain matching files one level down, and this function has no
    //    way to know that without the recursive walk section 3.5/6.3
    //    explicitly defers. Hiding the folder would make those files
    //    unreachable, not merely invisible, which is a strictly worse
    //    failure than showing a folder that turns out to hold none of the
    //    wanted type once entered.
    //
    //    `hadAnythingBeforeFilter` is read BEFORE the filter runs, which is
    //    what makes it able to tell "genuinely empty" apart from "everything
    //    here was filtered out" below -- see filteredToNothing's own
    //    derivation at the bottom of this function.
    bool hadAnythingBeforeFilter = !kept.isEmpty();
    QVector<nf_entry> filteredKept;
    for (int i = 0; i < kept.size(); i++) {
        nf_entry const& e = kept.at(i);
        if (e.isDir || nf_matches_filter(e.name, filter))
            filteredKept << e;
    }
    kept = filteredKept;

    // 2. Order: folders before files, then `key`/`descending` within each
    //    kind -- see nf_sort_entries' own comment (nffmt.cc) for why
    //    grouping and ordering stay two separate concerns inside it. This
    //    runs BEFORE metadata (stage 3, below) rather than after it, which
    //    is safe today ONLY because every current key (name, size, date)
    //    reads a field nf_entry already carries off QFileInfo, never
    //    anything `meta` fills in -- see stage 3's own comment for what
    //    WOULD force these two stages to swap.
    nf_sort_entries(&kept, key, descending);

    // 3. Metadata, for EVERY file row rather than the visible ones. Spec
    //    section 6.2: a future sort by a metadata field needs it before the
    //    sort runs, and building it lazily now would have to be undone then.
    //    Directories are skipped -- there is no Volume for a folder.
    for (int i = 0; i < kept.size(); i++) {
        nf_row r;
        r.name        = kept.at(i).name;
        r.label       = kept.at(i).name;
        r.isDir       = kept.at(i).isDir;
        r.hasRow      = false;
        r.percentRead = -1;
        r.finished    = false;
        if (!r.isDir && meta)
            meta(ctx, r.name, &r);
        *out << r;
    }

    // 4. Labels, LAST and over the surviving rows only. Folders and files are
    //    labelled as separate sets: a folder name and a book name share nothing
    //    useful, so pooling them finds a common run of "" and silently disables
    //    stripping in any folder holding both kinds -- which on this card is the
    //    common case, Sandman having 11 folders and 3 files.
    for (int pass = 0; pass < 2; pass++) {
        bool wantDir = (pass == 0);
        QStringList names;
        QVector<int> idx;
        for (int i = 0; i < out->size(); i++) {
            if (out->at(i).isDir == wantDir) {
                names << out->at(i).name;
                idx   << i;
            }
        }
        if (names.size() < 2)
            continue;
        nf_strip_common(&names);
        // Collision guard. nf_strip_common's per-row bracket truncation and
        // trimming are NOT common to every row, so they could in principle
        // make two labels equal. Unique names must stay distinguishable, so on
        // any collision this set keeps its raw names. Spec section 3.4 -- the
        // folder-on-row form of that rule belongs to v2's flat search results,
        // where rows really can come from different folders.
        bool collided = false;
        for (int a = 0; a < names.size() && !collided; a++) {
            for (int b = a + 1; b < names.size(); b++) {
                if (names.at(a) == names.at(b)) {
                    collided = true;
                    break;
                }
            }
        }
        if (collided)
            continue;
        for (int k = 0; k < idx.size(); k++)
            (*out)[idx.at(k)].label = names.at(k);
    }

    // Section 6.3/3.6's third empty state. `out` is empty here in exactly two
    // situations this function can tell apart: nothing survived hide-junk at
    // all (a genuinely empty, or all-junk, directory -- hadAnythingBeforeFilter
    // is false), or something DID survive hide-junk and the user filter then
    // removed every one of it (hadAnythingBeforeFilter is true). Only the
    // second is "everything here was filtered out" -- a directory that was
    // never going to show anything regardless of `filter` is just empty, full
    // stop, and must not be blamed on a filter the user may not even have
    // touched.
    //
    // A folder-only directory can never trip this: folders always survive the
    // filter stage above (nf_matches_filter is never even consulted for one),
    // so `out` is non-empty whenever hide-junk left any folder behind, no
    // matter what `filter` removed from alongside it. That is deliberate --
    // there is still somewhere to navigate, which is a different situation
    // from a folder of files that all got filtered out.
    if (filteredToNothing)
        *filteredToNothing = hadAnythingBeforeFilter && out->isEmpty();
}
