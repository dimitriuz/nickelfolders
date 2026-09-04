// The listing pipeline of spec section 6.1. PURE: the one part that needs
// libnickel is injected as a callback, which is what keeps this file
// host-testable.
#ifndef NFLIST_H
#define NFLIST_H

#include "nffmt.h"

struct nf_row {
    QString name;         // the on-disk name, never modified
    QString label;        // what the panel shows
    bool    isDir;
    bool    hasRow;       // a Volume exists for it; always false for a directory
    int     percentRead;  // -1 when unknown, not applicable, or no row
    bool    finished;
};

// Fills hasRow, percentRead and finished for one FILE name. Never called for a
// directory.
typedef void (*nf_meta_fn)(void *ctx, QString const& name, nf_row *row);

// list -> hide junk -> user filter -> fetch metadata -> group by kind ->
// order -> label -> disambiguate. Spec section 6.1; `filter`/`key`/
// `descending` are that spec's "FUTURE" insertions, now built.
//
// The four new parameters are all DEFAULTED to v1's original, pre-filter
// behaviour (NF_FILTER_ALL, NF_SORT_NAME, ascending, no `filteredToNothing`)
// rather than being required at every call site -- every existing test in
// this project, and every existing caller, keeps compiling and keeps its old
// behaviour unchanged.
//
// `filteredToNothing`, if non-NULL, distinguishes spec section 6.3/3.6's
// third empty state: set true only when the directory held at least one
// entry that survived hide-junk (checked BEFORE the user filter runs) and
// the user filter then removed every last one of them. A directory that was
// already empty after hide-junk (nothing to filter in the first place) can
// never set this -- see nflist.cc for why that is the right line to draw.
void nf_build_listing(QVector<nf_entry> const& entries,
                      nf_meta_fn meta, void *ctx,
                      QVector<nf_row> *out,
                      nf_filter_kind filter = NF_FILTER_ALL,
                      nf_sort_key key = NF_SORT_NAME,
                      bool descending = false,
                      bool *filteredToNothing = 0);

#endif
