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

    // 2. Type filter. Spec section 6.1's own stage, right after hide-junk and
    //    before everything else -- in particular before labelling (stage 6,
    //    below), which is what test_labels_are_derived_after_filtering
    //    (test_nflist.cc) pins for the ORIGINAL junk-hiding stage and now
    //    pins for this one too.
    //
    //    Deliberately still FIRST of the two filter stages, ahead of metadata,
    //    even though its sibling (the read-state filter, stage 4) cannot be:
    //    an extension is knowable from the name alone, and every file this
    //    stage drops is a libnickel round trip stage 3 does not have to make.
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
    //    `hadAnythingBeforeFilter` is read BEFORE either filter runs, which is
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

    // 3. Metadata, for EVERY file row rather than the visible ones. Spec
    //    section 6.2: a sort or a filter that reads a metadata field needs it
    //    before either runs, and building it lazily would have to be undone
    //    then. That "would" is now a "did": the read-state filters (stage 4)
    //    are the metadata-derived stage the OLD stage-2 comment predicted
    //    would force ordering and metadata to swap places, and this is that
    //    swap. Directories are skipped -- there is no Volume for a folder.
    //
    //    hasRow/percentRead/readState are left at nf_row's own constructor
    //    defaults (nffmt.h) rather than restated here: "nothing is known yet"
    //    belongs in one place, and `meta` overwrites what it can establish.
    for (int i = 0; i < kept.size(); i++) {
        nf_row r;
        r.name  = kept.at(i).name;
        r.label = kept.at(i).name;
        r.isDir = kept.at(i).isDir;
        // Carried forward so stage 5 can still order by size/date now that it
        // orders rows rather than entries -- nf_row's own comment (nffmt.h)
        // has why losing these would be a silent regression.
        r.size  = kept.at(i).size;
        r.mtime = kept.at(i).mtime;
        if (!r.isDir && meta)
            meta(ctx, r.name, &r);
        // DERIVED, never supplied by `meta` (nflist.h says so at nf_meta_fn):
        // readState is the single source of truth for read state and this is a
        // convenience the row renderer (nfview.cc) reads. A folder keeps
        // NF_READ_UNKNOWN and so lands here as false, which is what it was
        // before this field existed.
        r.finished = (r.readState == NF_READ_FINISHED);
        *out << r;
    }

    // 4. Read-state filter, section 6.3's second axis. It has to be HERE and
    //    not up alongside the type filter: read state is metadata, so nothing
    //    before stage 3 knows it. Everything the stage-2 comment says about
    //    the type filter being a separate layer from the junk allowlist
    //    applies to this one too.
    //
    //    A folder is never removed -- but unlike stage 2, this loop does not
    //    have to say so, because nf_matches_read_filter takes the whole row
    //    and owns that rule itself (nffmt.cc). One rule, one place; a
    //    name-only predicate could not have done the same.
    //
    //    Runs unconditionally rather than behind an "is this a read-state
    //    filter" test: nf_matches_read_filter already answers true for every
    //    format filter and for NF_FILTER_ALL, so this stage is a no-op copy of
    //    at most 27 rows (the largest listing measured on the reference card)
    //    in those cases -- not worth a second classifier function in the
    //    header, and one less place that has to be updated when a filter value
    //    is added.
    {
        QVector<nf_row> surviving;
        for (int i = 0; i < out->size(); i++) {
            if (nf_matches_read_filter(out->at(i), filter))
                surviving << out->at(i);
        }
        *out = surviving;
    }

    // 5. Order: folders before files, then `key`/`descending` within each
    //    kind -- see nf_sort_entries' own comment (nffmt.cc) for why grouping
    //    and ordering stay two separate concerns inside it. Over ROWS
    //    (nf_sort_rows), because stages 3 and 4 have already turned the
    //    entries into rows; the ordering rules themselves are the same single
    //    function either way.
    nf_sort_rows(out, key, descending);

    // 6. Labels, LAST and over the surviving rows only. Folders and files are
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
    // is false), or something DID survive hide-junk and one of the two filter
    // stages then removed every one of it (hadAnythingBeforeFilter is true).
    // Only the second is "everything here was filtered out" -- a directory
    // that was never going to show anything regardless of `filter` is just
    // empty, full stop, and must not be blamed on a filter the user may not
    // even have touched.
    //
    // Unchanged by the read-state filters, and deliberately not extended for
    // them: this expression asks "did hide-junk leave anything, and is `out`
    // empty now", which is true of whichever filter stage did the emptying.
    // There is one filter row and one `filter` value, so a second flag saying
    // WHICH stage emptied the listing would have nothing to tell the message
    // that `filter` does not already say.
    //
    // A folder-only directory can never trip this: folders always survive both
    // filter stages above (nf_matches_filter is never even consulted for one,
    // and nf_matches_read_filter always answers true for one), so `out` is
    // non-empty whenever hide-junk left any folder behind, no matter what
    // `filter` removed from alongside it. That is deliberate -- there is still
    // somewhere to navigate, which is a different situation from a folder of
    // files that all got filtered out.
    if (filteredToNothing)
        *filteredToNothing = hadAnythingBeforeFilter && out->isEmpty();
}
