// The listing pipeline of spec section 6.1. PURE: the one part that needs
// libnickel is injected as a callback, which is what keeps this file
// host-testable.
#ifndef NFLIST_H
#define NFLIST_H

#include "nffmt.h"

// nf_row itself lives in nffmt.h -- see its own comment there for why (the
// pure predicate and the pure row sort both need to see a whole row, and this
// header includes that one, not the other way round).

// Fills hasRow, percentRead and readState for one FILE name. Never called for
// a directory.
//
// Must NOT set nf_row::finished: nf_build_listing derives that from readState
// once `meta` has returned, so that the two can never disagree. Setting it
// here would be a second source of truth for the same fact.
typedef void (*nf_meta_fn)(void *ctx, QString const& name, nf_row *row);

// list -> hide junk -> type filter -> fetch metadata -> read-state filter ->
// group by kind -> order within group -> label -> disambiguate. Spec section
// 6.1; `filter`/`key`/`descending` are that spec's "FUTURE" insertions, now
// built.
//
// NOTE metadata now runs BEFORE ordering, which is spec section 6.1's own
// prose order and NOT what v1 originally built: v1 ordered first, because the
// only keys it had (name, size, date) all read nf_entry fields
// nf_browser_scan_dir fills off QFileInfo, none of them anything `meta`
// supplies. The read-state filters are what forced the swap the old stage-2
// comment predicted would force it -- read state is metadata, so it cannot be
// known before `meta` runs, and the brief this was built from requires it
// filtered and ordered afterwards. The consequence is that ordering now
// happens over nf_row (nf_sort_rows), not nf_entry, which is why nf_row
// carries size/mtime forward.
//
// The four new parameters are all DEFAULTED to v1's original, pre-filter
// behaviour (NF_FILTER_ALL, NF_SORT_NAME, ascending, no `filteredToNothing`)
// rather than being required at every call site -- every existing test in
// this project, and every existing caller, keeps compiling and keeps its old
// behaviour unchanged.
//
// `filteredToNothing`, if non-NULL, distinguishes spec section 6.3/3.6's
// third empty state: set true only when the directory held at least one
// entry that survived hide-junk (checked BEFORE either filter stage runs) and
// the filters then removed every last one of them. A directory that was
// already empty after hide-junk (nothing to filter in the first place) can
// never set this -- see nflist.cc for why that is the right line to draw. It
// does not distinguish WHICH filter emptied the listing, and does not need to:
// there is one filter row and one `filter` argument, so whichever value is
// active is the one to name in the message (nfview.cc).
void nf_build_listing(QVector<nf_entry> const& entries,
                      nf_meta_fn meta, void *ctx,
                      QVector<nf_row> *out,
                      nf_filter_kind filter = NF_FILTER_ALL,
                      nf_sort_key key = NF_SORT_NAME,
                      bool descending = false,
                      bool *filteredToNothing = 0);

#endif
