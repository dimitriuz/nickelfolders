#include "nflist.h"

void nf_build_listing(QVector<nf_entry> const& entries,
                      nf_meta_fn meta, void *ctx,
                      QVector<nf_row> *out) {
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

    // 2. Order: folders before files, natural within each kind.
    nf_sort_entries(&kept);

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
}
