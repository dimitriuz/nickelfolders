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

// list -> hide junk -> fetch metadata -> group by kind -> order -> label.
void nf_build_listing(QVector<nf_entry> const& entries,
                      nf_meta_fn meta, void *ctx,
                      QVector<nf_row> *out);

#endif
