// Host-only test macros. Deliberately tiny and dependency-free: the code under
// test must build against Nickel's Qt 5.2.1 with no C++ stdlib runtime, and a
// real test framework would tempt someone to let stdlib creep into nffmt.cc.
#ifndef NFTEST_H
#define NFTEST_H

#include <QString>
#include <stdio.h>

static int nf_checks = 0;
static int nf_fails  = 0;

#define CHECK(cond) do {                                                      \
    nf_checks++;                                                              \
    if (!(cond)) {                                                            \
        nf_fails++;                                                           \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                         \
} while (0)

#define CHECK_EQ_STR(got, want) do {                                          \
    nf_checks++;                                                              \
    QString g_ = (got);                                                       \
    QString w_ = QString::fromUtf8(want);                                     \
    if (g_ != w_) {                                                           \
        nf_fails++;                                                           \
        fprintf(stderr, "FAIL %s:%d  got \"%s\" want \"%s\"\n",               \
                __FILE__, __LINE__, g_.toUtf8().constData(),                  \
                w_.toUtf8().constData());                                     \
    }                                                                         \
} while (0)

#define NF_TEST_MAIN_END                                                      \
    fprintf(stderr, "%d checks, %d failures\n", nf_checks, nf_fails);         \
    return nf_fails ? 1 : 0;

#endif
