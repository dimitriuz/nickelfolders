// PURE display and ordering logic. Deliberately free of libnickel, NickelHook
// and I/O so that it builds and runs on the host, which is the only part of
// this project that can be tested off-device.
#ifndef NFFMT_H
#define NFFMT_H

#include <QString>
#include <QStringList>

// Orders two names the way a reader expects when they contain numbers.
// Returns <0, 0 or >0. See nffmt.cc for why this is hand-written rather than
// QCollator.
int nf_natural_compare(QString const& a, QString const& b);

// The known book extension at the end of `name`, dot included, or an empty
// QString. Longest match first, because ".kepub.epub" also ends in ".epub".
QString nf_book_extension(QString const& name);

// Replaces every entry with the label the panel should show, by stripping the
// text common to all of them. Leaves ALL entries untouched if stripping any of
// them would be unsafe -- see nffmt.cc.
void nf_strip_common(QStringList *names);

#endif
