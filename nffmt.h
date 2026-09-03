// PURE display and ordering logic. Deliberately free of libnickel, NickelHook
// and I/O so that it builds and runs on the host, which is the only part of
// this project that can be tested off-device.
#ifndef NFFMT_H
#define NFFMT_H

#include <QString>

// Orders two names the way a reader expects when they contain numbers.
// Returns <0, 0 or >0. See nffmt.cc for why this is hand-written rather than
// QCollator.
int nf_natural_compare(QString const& a, QString const& b);

#endif
