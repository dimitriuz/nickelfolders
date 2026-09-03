# NickelFolders v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A folder browser on Nickel's own window stack that walks one directory at a time and opens the tapped book in Kobo's stock reader.

**Architecture:** An `AbstractController` subclass whose view is a plain Qt widget, pushed with `MainWindowController::push`. The folder tree comes from the filesystem one directory at a time; per-row metadata comes from the already-proven `VolumeManager::getById` plus `Volume::getDbValues()`. There is no SQL and no database handle. All display and ordering logic is pure string code in its own file with a host test suite; everything that touches libnickel is isolated in one other file.

**Tech Stack:** C++ (GCC 4.9.4, `-std=gnu++11`) against Nickel's Qt 5.2.1, built with NickelTC in Docker via `./nickeltc`. NickelHook for injection. Host tests build natively against Qt 5.15 with the system g++.

**Spec:** `docs/superpowers/specs/2026-09-03-nickelfolders-v1-design.md` — the plan argues from the spec; read both.

## Global Constraints

Copied from `CLAUDE.md`. Every task's requirements implicitly include these.

- **Nickel's classes stay opaque.** `typedef void Volume;` and an explicitly written call signature. Never a real C++ class, never a redeclared method.
- **No C++ standard library runtime.** Qt and libc only. The C++ *language* is fine. Qt containers are fine. Nothing needing libstdc++ runtime support.
- **Over-allocate for every Nickel constructor**, and record the measured size in a comment.
- **Never hold a file handle on `/mnt/onboard`** for more than a few hundred milliseconds. Safe windows: the init function and Qt signal handlers.
- **Nickel's UI may only be touched from the GUI thread.** Cross-thread work goes through `QCoreApplication::postEvent`.
- **The NickelHook failsafe is SHARED infrastructure.** Anything non-essential must be non-fatal and `nf_init` must return 0, or the user's NickelMenu and NickelDBus can uninstall themselves.
- **GCC 4.9 rejects a designated initializer that SKIPS a field.** Spell out every field up to the last one set, in order.
- **`nh_log` truncates at 256 bytes, silently.** Never build a measurement on a log line meant to carry a whole path.
- **Library name stays `libnfolders.so`.** Names under 3 letters are reserved for upstream authors.
- **Never commit** `libnickel.so.1.0.0`, `device.local`, or a book.
- **Comments record why, not what.** Anything measured off the device carries the measurement. Clamps, guards and deliberate leaks carry a note saying they are deliberate.

### Verification constraints

There is no way to unit-test a libnickel call. For device rungs the spec's verification culture replaces the test cycle and is not optional:

- **`ndbCurrentView` (NickelDBus) is the oracle**, never the log. A log line proves a call returned; the question is whether Nickel navigated.
- **Detect a crash by watching Nickel's PID.** A crash-and-restart reads as `HomePageView` and looks identical to quiet success.
- **A screenshot is the only way to know the right thing is on the panel.** `python3 tools/kobo.py shot`. Crop to 1264x1680; sysfs `virtual_size` is padded to 1280x1792.
- **Every device rung needs a negative control** — an input that must fail — or a passing check is not known to be non-vacuous.
- **Do not filter the crash log to your own mod.** Grep `hindenburg|segfault|SIGSEGV` separately and unfiltered.
- **A reboot is the unit of iteration.** Batch device questions.

### Two instrument failures this project has already had

Both announced themselves by failing in *patterns* rather than randomly. If results look patterned, suspect the instrument first.

- A baseline set to the value the test expected to measure.
- An association check built on `nh_log` lines that truncate at 256 bytes.

---

## File Structure

The split exists so that the logic that can be tested is separated from the logic that cannot, and so that every libnickel layout assumption lives in one file.

| File | Responsibility | Testable on host? |
|---|---|---|
| `nffmt.h` / `nffmt.cc` | **Pure** display and ordering logic: numeric-aware compare, common-run stripping, kind-first ordering, duplicate disambiguation, extension allowlist. QString only. No libnickel, no NickelHook, no I/O. | **Yes** |
| `nflist.h` / `nflist.cc` | **Pure** listing pipeline (spec §6.1) over injected inputs: takes raw entries plus a metadata lookup callback, returns finished rows. No QDir call of its own. | **Yes** |
| `nfnickel.h` / `nfnickel.cc` | Every libnickel symbol, opaque typedef, object size and calling convention. The one place a layout assumption may live. | No |
| `nfbrowser.h` / `nfbrowser.cc` | The `AbstractController` subclass, its vtable, the Qt list view, navigation state, the `QDir` read. | No |
| `nfolders.cc` | `nf_init`, the inotify/`QSocketNotifier` trigger, wiring. Stays the entry point. | No |
| `tests/nftest.h` | Test macros. Host only. | — |
| `tests/test_nffmt.cc` | Tests for `nffmt`. | **Yes** |
| `tests/test_nflist.cc` | Tests for `nflist`, including pipeline order. | **Yes** |
| `Makefile` | Adds a `test` target beside NickelHook's build. | — |

`nffmt` and `nflist` must not `#include <NickelHook.h>` or anything from `nfnickel.h`. That restriction is what keeps them host-buildable, and the host build is what enforces it.

---

## Rung 0: a host test suite and the pure logic

**This rung is not in the spec's §5 and is prepended deliberately.** The spec's rungs 1-6 all cost a reboot each. Rung 0 costs none, needs no device, and covers the part of v1 most likely to be *wrong* rather than *broken* — ordering and labelling, where the two device measurements on 2026-09-03 already overturned two assumptions. `CLAUDE.md` calls the missing host suite "a real weakness of this project"; this rung is where that gets fixed.

Host Qt is 5.15.19, device Qt is 5.2.1. Every API used below exists in both. If someone reaches for a 5.15-only call, `./nickeltc make` fails to compile on the next device build — loud and immediate, which is why the version skew is acceptable.

### Task 1: The host test harness

**Files:**
- Create: `tests/nftest.h`
- Create: `tests/test_nffmt.cc`
- Create: `nffmt.h`
- Create: `nffmt.cc`
- Modify: `Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: `make test` builds and runs every `tests/test_*.cc`. `CHECK(cond)` and `CHECK_EQ_STR(QString, const char*)` macros. `nf_natural_compare(QString const&, QString const&) -> int`, declared but returning 0.

- [ ] **Step 1: Write the failing test**

`tests/nftest.h`:

```cpp
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
```

`nffmt.h`:

```cpp
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
```

`nffmt.cc`:

```cpp
#include "nffmt.h"

int nf_natural_compare(QString const&, QString const&) {
    return 0;
}
```

`tests/test_nffmt.cc`:

```cpp
#include "nftest.h"
#include "nffmt.h"

// The measured bug, from the reference card on 2026-09-03:
// books/Comics/English/Sandman/ holds DIRECTORIES named v1, v10, v2 ... v9.
// A plain case-insensitive sort puts v10 -- The Wake, the finale -- second.
static void test_unpadded_volume_dirs(void) {
    CHECK(nf_natural_compare("v1 - Preludes and Nocturnes",
                             "v2 - The Doll's House") < 0);
    CHECK(nf_natural_compare("v2 - The Doll's House",
                             "v9 - The Kindly Ones") < 0);
    CHECK(nf_natural_compare("v9 - The Kindly Ones",
                             "v10 - The Wake") < 0);
}

int main(void) {
    test_unpadded_volume_dirs();
    NF_TEST_MAIN_END
}
```

`Makefile` — append below the existing includes:

```make
# Host test build. Separate from NickelHook's cross build on purpose: these
# compile the PURE sources (nffmt, nflist) with the system compiler and host
# Qt5, which is the only way anything here runs off-device.
#
# The host Qt is 5.15 and the device's is 5.2.1. Every API the pure sources use
# exists in both, and reaching for a 5.15-only call breaks `./nickeltc make`
# on the next device build rather than failing quietly on the panel -- which is
# why the skew is acceptable rather than merely tolerated.
HOST_CXX      ?= g++
HOST_PURE     := nffmt.cc
HOST_TESTSRC  := $(wildcard tests/test_*.cc)
HOST_TESTBIN  := $(patsubst tests/%.cc,build/%,$(HOST_TESTSRC))
# Header prerequisites are load-bearing, not tidiness: without them a test
# binary is not relinked when only a header changed, so a deliberately broken
# guard in a header runs against a stale binary and appears not to fail --
# which would silently invalidate the one discipline this rung leans on.
HOST_HDR      := $(wildcard *.h) $(wildcard tests/*.h)
HOST_CXXFLAGS := -std=gnu++11 -O1 -g -Wall -Wextra -Werror -I. -Itests \
                 $(shell pkg-config --cflags Qt5Core)
HOST_LDLIBS   := $(shell pkg-config --libs Qt5Core)

build:
	@mkdir -p build

build/test_%: tests/test_%.cc $(HOST_PURE) $(HOST_HDR) | build
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_PURE) $(HOST_LDLIBS)

.PHONY: test
test: $(HOST_TESTBIN)
	@rc=0; for t in $(HOST_TESTBIN); do echo "== $$t"; ./$$t || rc=1; done; exit $$rc

override SKIPCONFIGURE += test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: builds, then `FAIL tests/test_nffmt.cc:...  nf_natural_compare(...) < 0` three times, `3 checks, 3 failures`, exit 1.

If it fails to *build* instead, that is a different failure — fix the build before continuing, because a plan step that cannot distinguish "test failed" from "test did not run" is the instrument problem this project has had twice.

- [ ] **Step 3: Write minimal implementation**

`nffmt.cc`:

```cpp
#include "nffmt.h"

// Hand-written rather than QCollator::setNumericMode, because QCollator needs
// ICU and betting on ICU inside Kobo's Qt 5.2.1 is not a bet worth making.
//
// Digit runs are compared WITHOUT being parsed to an integer: by significant
// length first, then lexically. That is not a micro-optimisation -- parsing
// would overflow on a pathological name, and an overflow here silently
// reorders the list rather than failing.
int nf_natural_compare(QString const& a, QString const& b) {
    int i = 0, j = 0;
    while (i < a.length() && j < b.length()) {
        QChar ca = a.at(i), cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            int si = i, sj = j;
            while (i < a.length() && a.at(i).isDigit()) i++;
            while (j < b.length() && b.at(j).isDigit()) j++;
            int zi = si; while (zi < i - 1 && a.at(zi) == QLatin1Char('0')) zi++;
            int zj = sj; while (zj < j - 1 && b.at(zj) == QLatin1Char('0')) zj++;
            int li = i - zi, lj = j - zj;
            if (li != lj)
                return li < lj ? -1 : 1;
            for (int k = 0; k < li; k++) {
                QChar da = a.at(zi + k), db = b.at(zj + k);
                if (da != db)
                    return da < db ? -1 : 1;
            }
            // Equal in value. Fall back to the raw runs so the order is
            // deterministic -- "v01" and "v1" must not compare equal, or a
            // sort can reorder them between runs.
            int ri = i - si, rj = j - sj;
            if (ri != rj)
                return ri < rj ? -1 : 1;
            continue;
        }
        QChar fa = ca.toCaseFolded(), fb = cb.toCaseFolded();
        if (fa != fb)
            return fa < fb ? -1 : 1;
        i++; j++;
    }
    int ra = a.length() - i, rb = b.length() - j;
    if (ra != rb)
        return ra < rb ? -1 : 1;
    return QString::compare(a, b, Qt::CaseSensitive);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: `3 checks, 0 failures`, exit 0.

- [ ] **Step 5: Prove the test is not vacuous**

Temporarily weaken the digit branch's `if (li != lj) return li < lj ? -1 : 1;`
to `if (false && li != lj) return li < lj ? -1 : 1;` — that is the mutation that
turns the comparator back into a plain lexical sort.

Use that form and **not** a bare `if (false) {}`: deleting the comparison
leaves `lj` unused, and `-Werror` then fails the *build*. A build failure is not
a test failure, and a mutation step that cannot distinguish the two proves
nothing. (Found by running it.)

Run: `make test`
Expected: FAIL on the `v9` vs `v10` check specifically.

Then revert the mutation and confirm `make test` passes again. This step is the whole reason rung 0 exists; do not skip it because the test already passed.

- [ ] **Step 6: Commit**

```bash
git add Makefile nffmt.h nffmt.cc tests/nftest.h tests/test_nffmt.cc
git commit -m "test: a host suite, and a numeric compare for unpadded volumes

The reference card's Sandman folder holds DIRECTORIES named v1, v10, v2 ... v9,
so a plain sort puts the finale second. Fullmetal Alchemist's FILES are
zero-padded, which is why looking at files alone said a plain sort was fine.

Digit runs are compared by significant length then lexically rather than parsed
to an integer, because an overflow would silently reorder rather than fail.

CLAUDE.md calls the missing host suite a real weakness; this is the start of
fixing it. The pure sources stay free of libnickel so they keep building here."
```

### Task 2: Common-run stripping

**Files:**
- Modify: `nffmt.h`, `nffmt.cc`
- Modify: `tests/test_nffmt.cc`

**Interfaces:**
- Consumes: `nf_natural_compare` (not used here, same file).
- Produces: `QString nf_book_extension(QString const& name)` — the known book extension including the dot, longest match first, or an empty `QString`. `void nf_strip_common(QStringList *names)` — replaces each entry with its display label, or leaves all of them untouched.

**The code below was validated against the reference card's real filenames on 2026-09-03 before this plan was written.** Do not simplify the bracket rule or the refuse-to-strip guard; both exist because a simpler version produced wrong labels on real data. See spec §3.2.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_nffmt.cc` (and add `#include <QStringList>` at the top):

```cpp
// Measured 2026-09-03. 27 volumes sharing a 20-character head and a
// 38-character tail; the year differs, so it survives and is informative.
static void test_strip_fullmetal(void) {
    QStringList n;
    for (int v = 1; v <= 3; v++) {
        n << QString("Fullmetal Alchemist v%1 (2005) (Digital) (LostNerevarine-Empire).cbz")
                 .arg(v, 2, 10, QLatin1Char('0'));
    }
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "v01");
    CHECK_EQ_STR(n.at(2), "v03");
}

// The case that broke the first implementation. These share
// "... Aventure (Part ", so cutting at the whitespace leaves rows starting
// "1) - Rouge ..." and loses the "(Part 1)" that distinguishes the sub-series.
static void test_strip_backs_off_past_open_bracket(void) {
    QStringList n;
    n << "Pokémon - La Grande Aventure (Part 1) - Rouge T01 (2014) [Manga FR].cbz"
      << "Pokémon - La Grande Aventure (Part 1) - Rouge T02 (2014) [Manga FR].cbz"
      << "Pokémon - La Grande Aventure (Part 4b) - Emeraude T03 (2017) [Manga FR].cbz";
    nf_strip_common(&n);
    CHECK(n.at(0).startsWith(QString::fromUtf8("(Part 1)")));
    CHECK(n.at(2).startsWith(QString::fromUtf8("(Part 4b)")));
    CHECK(n.at(0).contains(QString::fromUtf8("T01")));
}

// The extension is NOT stripped for free. ".cbz" has no whitespace inside it,
// so the token-boundary rule cannot cut there; it has to be split off first.
static void test_strip_handles_extension_explicitly(void) {
    QStringList n;
    n << "Batman v01.cbz" << "Batman v02.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "v01");
    CHECK_EQ_STR(n.at(1), "v02");
}

// A mixed-format listing keeps its extensions, because there the extension is
// the thing telling two rows apart.
static void test_strip_keeps_extension_when_mixed(void) {
    QStringList n;
    n << "Report.epub" << "Report.pdf";
    nf_strip_common(&n);
    CHECK(n.at(0).endsWith(".epub"));
    CHECK(n.at(1).endsWith(".pdf"));
}

// No common run at all: every row is left exactly as it was.
static void test_strip_no_common_run(void) {
    QStringList n;
    n << "v1 - Preludes and Nocturnes" << "v10 - The Wake" << "Sandman Mystery Theatre";
    QStringList before = n;
    nf_strip_common(&n);
    CHECK(n == before);
}

// One row has no "common" run worth the name -- it would strip to nothing.
static void test_strip_single_row_untouched(void) {
    QStringList n;
    n << "Solo Book.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "Solo Book.cbz");
}

// Refuse to strip rather than strip badly: a listing of full names is verbose,
// a listing of one-character rows is broken.
static void test_strip_refuses_when_remainder_too_short(void) {
    QStringList n;
    n << "Book A.cbz" << "Book B.cbz";
    nf_strip_common(&n);
    CHECK_EQ_STR(n.at(0), "Book A.cbz");
}
```

Add each to `main()` beside `test_unpadded_volume_dirs()`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test`
Expected: build failure — `nf_strip_common` and `nf_book_extension` are not declared yet. Add the declarations to `nffmt.h` with empty bodies in `nffmt.cc`, re-run, and expect the seven new checks to fail on values rather than on linkage. A test that fails to *compile* has not run, which is not the same evidence.

- [ ] **Step 3: Write the implementation**

Add to `nffmt.h`:

```cpp
#include <QStringList>

// The known book extension at the end of `name`, dot included, or an empty
// QString. Longest match first, because ".kepub.epub" also ends in ".epub".
QString nf_book_extension(QString const& name);

// Replaces every entry with the label the panel should show, by stripping the
// text common to all of them. Leaves ALL entries untouched if stripping any of
// them would be unsafe -- see nffmt.cc.
void nf_strip_common(QStringList *names);
```

Add to `nffmt.cc`:

```cpp
// v1's formats, measured by censusing the whole reference card on 2026-09-03:
// 122 .cbz, 94 .cbr, 6 .pdf, 4 .epub (2 of them .kepub.epub). ".txt" is
// deliberately absent -- the card's only .txt is a probe file the sibling
// koboy project left on /mnt/onboard, which Nickel imported as a book, so
// admitting .txt buys exactly one row of garbage. Spec section 3.5.
//
// LONGEST FIRST is load-bearing: ".kepub.epub" also ends in ".epub", and a
// shortest-match scan would report the wrong extension for a Kobo epub.
static char const *const NF_EXTS[] = {
    ".kepub.epub", ".epub", ".cbz", ".cbr", ".pdf", NULL,
};

QString nf_book_extension(QString const& name) {
    for (int i = 0; NF_EXTS[i]; i++) {
        QString ext = QString::fromLatin1(NF_EXTS[i]);
        if (name.endsWith(ext, Qt::CaseInsensitive))
            return name.right(ext.length());
    }
    return QString();
}

static bool nf_is_open(QChar c)  { return c == QLatin1Char('(') || c == QLatin1Char('['); }
static bool nf_is_close(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']'); }

// Index of the first opener in [0,end) never closed within it, or -1.
static int nf_first_unclosed(QString const& s, int end) {
    int depth = 0, first = -1;
    for (int i = 0; i < end; i++) {
        if (nf_is_open(s.at(i))) {
            if (depth == 0)
                first = i;
            depth++;
        } else if (nf_is_close(s.at(i))) {
            if (depth > 0)
                depth--;
        }
    }
    return depth > 0 ? first : -1;
}

void nf_strip_common(QStringList *names) {
    if (names->size() < 2)
        return;

    // Extensions come off FIRST rather than falling out as a common suffix.
    // Measured: ".cbz" contains no whitespace, so the token-boundary rule below
    // cannot cut there. Fullmetal Alchemist appeared to work only because its
    // common suffix happens to contain spaces. Spec section 3.2.
    QStringList stems;
    QStringList exts;
    bool mixed = false;
    for (int i = 0; i < names->size(); i++) {
        QString ext = nf_book_extension(names->at(i));
        exts  << ext;
        stems << names->at(i).left(names->at(i).length() - ext.length());
        if (i > 0 && ext.compare(exts.at(0), Qt::CaseInsensitive) != 0)
            mixed = true;
    }

    int minLen = stems.at(0).length();
    for (int i = 1; i < stems.size(); i++)
        minLen = qMin(minLen, stems.at(i).length());

    int p = 0;
    while (p < minLen) {
        QChar c = stems.at(0).at(p);
        bool same = true;
        for (int i = 1; i < stems.size(); i++) {
            if (stems.at(i).at(p) != c) {
                same = false;
                break;
            }
        }
        if (!same)
            break;
        p++;
    }

    int s = 0;
    while (s < minLen - p) {
        QChar c = stems.at(0).at(stems.at(0).length() - 1 - s);
        bool same = true;
        for (int i = 1; i < stems.size(); i++) {
            if (stems.at(i).at(stems.at(i).length() - 1 - s) != c) {
                same = false;
                break;
            }
        }
        if (!same)
            break;
        s++;
    }

    // A row must not begin mid-word.
    int pw = p;
    while (pw > 0 && !stems.at(0).at(pw - 1).isSpace())
        pw--;
    // Nor inside a bracket. Measured case: 13 names sharing "... (Part ",
    // where the whitespace cut alone leaves rows reading "1) - Rouge ..." and
    // throws away the "(Part 1)" that distinguishes the sub-series.
    int unclosed = nf_first_unclosed(stems.at(0), pw);
    if (unclosed >= 0)
        pw = unclosed;

    int sw = s;
    {
        QString const& n = stems.at(0);
        while (sw > 0 && !n.at(n.length() - sw).isSpace())
            sw--;
    }

    QStringList out;
    for (int i = 0; i < stems.size(); i++) {
        QString const& n = stems.at(i);
        int keep = n.length() - pw - sw;
        if (keep < 2)
            return;                 // deliberate: leave EVERY row alone, not just this one
        QString r = n.mid(pw, keep);
        int u = nf_first_unclosed(r, r.length());
        if (u >= 0)
            r = r.left(u);          // never show a dangling half-bracket
        r = r.trimmed();
        if (mixed)
            r += exts.at(i);        // in a mixed listing the extension is the distinguisher
        out << r;
    }
    for (int i = 0; i < out.size(); i++) {
        if (out.at(i).length() < 2)
            return;
    }
    *names = out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test`
Expected: all checks pass, `0 failures`.

- [ ] **Step 5: Prove the two hard guards are not vacuous**

Two mutations, applied and reverted one at a time:

1. Delete the `if (unclosed >= 0) pw = unclosed;` lines.
   Run `make test`. Expected: `test_strip_backs_off_past_open_bracket` fails.
2. Change `if (keep < 2) return;` to `if (keep < 0) return;`.
   Run `make test`. Expected: `test_strip_refuses_when_remainder_too_short` fails.

Revert both and confirm `make test` passes. If either mutation leaves the suite green, the test is wrong, not the mutation.

- [ ] **Step 6: Commit**

```bash
git add nffmt.h nffmt.cc tests/test_nffmt.cc
git commit -m "feat: strip the run common to a listing, instead of eliding it

The Pokemon folder's 13 names share a 34-character head and each carry ~60
characters of release-group tail, with the volume token in the MIDDLE. A middle
ellipsis keeps the useless head and tail and eats the identifier.

Two rules came from running this against the real names rather than reasoning
about them: the prefix must back off past an unmatched opening bracket or rows
read '1) - Rouge ...' and lose their '(Part 1)', and the extension must be
split off explicitly because '.cbz' has no whitespace for the boundary rule to
cut at."
```

### Task 3: Kind-first ordering, and a sort that is ours

**Files:**
- Modify: `nffmt.h`, `nffmt.cc`
- Modify: `tests/test_nffmt.cc`

**Interfaces:**
- Consumes: `nf_natural_compare`.
- Produces: `void nf_sort_entries(QVector<nf_entry> *entries)` — stable, folders before files, `nf_natural_compare` within each kind. `nf_entry` is declared in `nffmt.h` so both pure files share it.

**Why the sort is hand-written.** `std::sort` is a libstdc++ template and `CLAUDE.md` forbids compiling stdlib templates into the library. Qt 5.2's `qSort` exists but is deprecated in the host's Qt 5.15, so `-Werror` rejects it on the test build. The largest listing on the reference card is 27 entries, so an insertion sort is both sufficient and stable — and stability is what makes the order reproducible when two names compare equal.

- [ ] **Step 1: Write the failing tests**

Add to `nffmt.h`, above the function declarations:

```cpp
#include <QVector>

// One raw directory entry. Shared by nffmt and nflist, which is why it lives
// in the pure header rather than in the browser.
struct nf_entry {
    QString name;
    bool    isDir;
};
```

Add to `tests/test_nffmt.cc`:

```cpp
static nf_entry ent(char const *name, bool isDir) {
    nf_entry e;
    e.name  = QString::fromUtf8(name);
    e.isDir = isDir;
    return e;
}

// Measured: books/Comics/English/Sandman holds 11 subfolders AND 3 files, so a
// folder of both kinds is real on this card rather than hypothetical.
static void test_sort_folders_before_files(void) {
    QVector<nf_entry> e;
    e << ent("zeta.cbz", false) << ent("alpha", true) << ent("beta.cbz", false)
      << ent("omega", true);
    nf_sort_entries(&e);
    CHECK(e.at(0).isDir);
    CHECK(e.at(1).isDir);
    CHECK_EQ_STR(e.at(0).name, "alpha");
    CHECK_EQ_STR(e.at(1).name, "omega");
    CHECK_EQ_STR(e.at(2).name, "beta.cbz");
}

static void test_sort_uses_natural_order_within_kind(void) {
    QVector<nf_entry> e;
    e << ent("v10 - The Wake", true) << ent("v2 - The Doll's House", true)
      << ent("v1 - Preludes", true) << ent("v9 - The Kindly Ones", true);
    nf_sort_entries(&e);
    CHECK_EQ_STR(e.at(0).name, "v1 - Preludes");
    CHECK_EQ_STR(e.at(1).name, "v2 - The Doll's House");
    CHECK_EQ_STR(e.at(2).name, "v9 - The Kindly Ones");
    CHECK_EQ_STR(e.at(3).name, "v10 - The Wake");
}

// Determinism, which is what makes a listing reproducible across runs.
//
// This deliberately does NOT try to test stability directly: nf_entry carries
// no payload beyond name and isDir, so two tied entries are indistinguishable
// and any "stability" assertion over them can only restate the input. What IS
// observable is that the sort is idempotent and independent of input order,
// which is the property the listing actually depends on.
static void test_sort_is_deterministic(void) {
    QVector<nf_entry> sorted;
    sorted << ent("alpha", true) << ent("omega", true)
           << ent("v1.cbz", false) << ent("v2.cbz", false);

    QVector<nf_entry> again = sorted;
    nf_sort_entries(&again);          // sorting sorted input changes nothing
    for (int i = 0; i < sorted.size(); i++) {
        CHECK_EQ_STR(again.at(i).name, sorted.at(i).name.toUtf8().constData());
        CHECK(again.at(i).isDir == sorted.at(i).isDir);
    }

    QVector<nf_entry> reversed;
    for (int i = sorted.size() - 1; i >= 0; i--)
        reversed << sorted.at(i);
    nf_sort_entries(&reversed);       // and reversed input reaches the same order
    for (int i = 0; i < sorted.size(); i++) {
        CHECK_EQ_STR(reversed.at(i).name, sorted.at(i).name.toUtf8().constData());
        CHECK(reversed.at(i).isDir == sorted.at(i).isDir);
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test`
Expected: build fails on undeclared `nf_sort_entries`. Add the declaration with an empty body, re-run, expect the ordering checks to fail on values.

- [ ] **Step 3: Write the implementation**

Add to `nffmt.h`:

```cpp
// Sorts folders before files, then by nf_natural_compare within each kind.
// Stable. Spec section 3.3.
void nf_sort_entries(QVector<nf_entry> *entries);
```

Add to `nffmt.cc`:

```cpp
// Insertion sort, and the choice is deliberate rather than lazy. std::sort is
// a libstdc++ template and CLAUDE.md forbids compiling stdlib templates into
// the library; Qt 5.2's qSort is deprecated in the host Qt 5.15 the tests build
// against, so -Werror rejects it. The largest listing measured on the reference
// card is 27 entries, where an insertion sort is not worth optimising -- and it
// is STABLE, which is what keeps the order reproducible when two names tie.
//
// Spec section 6.1 keeps grouping and ordering separate stages so that a future
// descending toggle reverses WITHIN each kind and never floats files above
// folders. Do not collapse the isDir test into the comparator.
static bool nf_entry_before(nf_entry const& a, nf_entry const& b) {
    if (a.isDir != b.isDir)
        return a.isDir;
    return nf_natural_compare(a.name, b.name) < 0;
}

void nf_sort_entries(QVector<nf_entry> *entries) {
    for (int i = 1; i < entries->size(); i++) {
        nf_entry key = entries->at(i);
        int j = i - 1;
        while (j >= 0 && nf_entry_before(key, entries->at(j))) {
            (*entries)[j + 1] = entries->at(j);
            j--;
        }
        (*entries)[j + 1] = key;
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test`
Expected: `0 failures`.

- [ ] **Step 5: Prove the kind split is not vacuous**

Mutate `nf_entry_before` to drop the `isDir` branch entirely (`return nf_natural_compare(...) < 0;`).
Run: `make test`
Expected: `test_sort_folders_before_files` fails.
Revert and confirm green.

- [ ] **Step 6: Commit**

```bash
git add nffmt.h nffmt.cc tests/test_nffmt.cc
git commit -m "feat: kind-first ordering over a sort that is ours

Folders before files, natural order within each kind, stable. Hand-written
because std::sort is a libstdc++ template CLAUDE.md forbids in the library and
qSort is deprecated in the Qt the host tests build against; 27 entries is the
largest listing measured, so insertion sort is ample.

Grouping and ordering stay separate stages so a future descending toggle
reverses within each kind rather than floating files above folders."
```

### Task 4: The allowlist, and what a directory hides

**Files:**
- Modify: `nffmt.h`, `nffmt.cc`
- Modify: `tests/test_nffmt.cc`

**Interfaces:**
- Consumes: `nf_book_extension`.
- Produces: `bool nf_is_book_name(QString const&)`, `bool nf_is_hidden_dir(QString const&)`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_nffmt.cc`:

```cpp
// Every book format measured on the card, plus the .kepub.epub special case.
static void test_allowlist_admits_the_measured_formats(void) {
    CHECK(nf_is_book_name("Some Book.epub"));
    CHECK(nf_is_book_name("Some Book.kepub.epub"));
    CHECK(nf_is_book_name("Volume 1.cbz"));
    CHECK(nf_is_book_name("Sandman 50.cbr"));
    CHECK(nf_is_book_name("Booklet.pdf"));
    CHECK(nf_is_book_name("SHOUTING.CBZ"));
}

// Every non-book actually present on the card on 2026-09-03. If this list ever
// shrinks, something started leaking into the listing.
static void test_allowlist_rejects_the_measured_junk(void) {
    CHECK(!nf_is_book_name("metadata.calibre"));
    CHECK(!nf_is_book_name("driveinfo.calibre"));
    CHECK(!nf_is_book_name("KOBOY-INSTALL.md"));
    CHECK(!nf_is_book_name("sketch1.svg"));
    CHECK(!nf_is_book_name("metadata.pdf.lua"));
    CHECK(!nf_is_book_name("WPSettings.dat"));
    CHECK(!nf_is_book_name("IndexerVolumeGuid"));
}

// The card's ONLY .txt is koboy's probe file, which Nickel imported as a book.
// It HAS a database row, so no greyed-row logic would catch it -- the allowlist
// is the only thing that keeps it out. Spec section 3.5.
static void test_allowlist_excludes_txt_deliberately(void) {
    CHECK(!nf_is_book_name("koboy-probe-Io.txt"));
}

static void test_hidden_dirs(void) {
    CHECK(nf_is_hidden_dir(".kobo"));
    CHECK(nf_is_hidden_dir(".adds"));
    // Measured: a KOReader sidecar sits in plain sight at the card root.
    CHECK(nf_is_hidden_dir("calibrewebdownload2055pdf2055.sdr"));
    CHECK(nf_is_hidden_dir("System Volume Information"));
    CHECK(!nf_is_hidden_dir("books"));
    CHECK(!nf_is_hidden_dir("Comics"));
    CHECK(!nf_is_hidden_dir("Sandman Mystery Theatre"));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test`
Expected: build fails on the two undeclared functions; declare them returning `false`, re-run, and expect the admit and hide checks to fail on values.

- [ ] **Step 3: Write the implementation**

Add to `nffmt.h`:

```cpp
// True for a name v1 will show as a book. Extension allowlist only -- see
// NF_EXTS in nffmt.cc for why ".txt" is not on it.
bool nf_is_book_name(QString const& name);

// True for a directory v1 hides. An extension allowlist does not touch
// directories, so they need their own rule.
bool nf_is_hidden_dir(QString const& name);
```

Add to `nffmt.cc`:

```cpp
bool nf_is_book_name(QString const& name) {
    return !nf_book_extension(name).isEmpty();
}

bool nf_is_hidden_dir(QString const& name) {
    if (name.startsWith(QLatin1Char('.')))
        return true;
    // A KOReader sidecar directory, and one sits at the reference card's root
    // in plain sight rather than hidden.
    if (name.endsWith(QStringLiteral(".sdr"), Qt::CaseInsensitive))
        return true;
    // Windows leaves this on any FAT volume it has touched. Not ours to show.
    if (name.compare(QStringLiteral("System Volume Information"), Qt::CaseInsensitive) == 0)
        return true;
    return false;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test`
Expected: `0 failures`.

- [ ] **Step 5: Prove the allowlist is an allowlist**

Add `".txt"` to `NF_EXTS`.
Run: `make test`
Expected: `test_allowlist_excludes_txt_deliberately` fails.
Remove it again and confirm green. That mutation is the guard against someone
"fixing" the missing `.txt` without reading why it is missing.

- [ ] **Step 6: Commit**

```bash
git add nffmt.h nffmt.cc tests/test_nffmt.cc
git commit -m "feat: an extension allowlist, and a hide rule for directories

Tested against every non-book actually on the reference card: .calibre
sidecars, an .md, a KOReader .lua, a Windows .dat and one extensionless file.

.txt is excluded on purpose. The card's only .txt is koboy's own probe file,
which Nickel imported as a book -- it HAS a row, so no greyed-row logic would
catch it and the allowlist is the only thing keeping it out. A mutation test
guards that so nobody puts it back without reading why."
```

### Task 5: The listing pipeline

**Files:**
- Create: `nflist.h`, `nflist.cc`
- Create: `tests/test_nflist.cc`

**Interfaces:**
- Consumes: `nf_entry`, `nf_sort_entries`, `nf_strip_common`, `nf_is_book_name`, `nf_is_hidden_dir`.
- Produces: `struct nf_row`, `typedef void (*nf_meta_fn)(void *ctx, QString const& name, nf_row *row)`, and `void nf_build_listing(QVector<nf_entry> const& entries, nf_meta_fn meta, void *ctx, QVector<nf_row> *out)`.

The metadata lookup is injected as a function pointer for one reason: it is the
only part of the pipeline that touches libnickel, and injecting it is what keeps
the pipeline itself host-testable. `nfbrowser.cc` will pass a callback that
calls `getById`; the tests pass a fake.

**The order of the stages is load-bearing, not tidiness** (spec §6.1). Two of
these tests exist only to pin that order down.

#### A correction to spec §3.4 that this task settles

Spec §3.4 says duplicate labels show their folder. **Within one directory that
cannot happen**: filenames are unique, and `nf_strip_common` removes text that
is identical in every row, so unique names stay unique. The folder-on-row rule
belongs to the flat search result set of §6.5, which is v2.

What v1 needs instead is a **collision guard**, because the per-row bracket
truncation and `trimmed()` inside `nf_strip_common` are *not* common to all rows
and could in principle collide. If labels collide, fall back to the raw names
for the whole listing. Amend §3.4 to say so as part of this task's commit.

- [ ] **Step 1: Write the failing tests**

`tests/test_nflist.cc`:

```cpp
#include "nftest.h"
#include "nflist.h"
#include <QVector>

// A fake metadata lookup. Names beginning "missing" have no database row,
// which is how the greyed-row state is exercised without a device.
static void fake_meta(void *ctx, QString const& name, nf_row *row) {
    int *calls = static_cast<int *>(ctx);
    if (calls)
        (*calls)++;
    row->hasRow      = !name.startsWith(QStringLiteral("missing"));
    row->percentRead = row->hasRow ? 42 : -1;
    row->finished    = false;
}

static nf_entry ent(char const *name, bool isDir) {
    nf_entry e;
    e.name  = QString::fromUtf8(name);
    e.isDir = isDir;
    return e;
}

static void test_junk_is_dropped_before_anything_else(void) {
    QVector<nf_entry> e;
    e << ent("books", true) << ent(".kobo", true)
      << ent("calibrewebdownload2055pdf2055.sdr", true)
      << ent("Volume 1.cbz", false) << ent("metadata.calibre", false)
      << ent("koboy-probe-Io.txt", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 2);
    CHECK_EQ_STR(out.at(0).name, "books");
    CHECK_EQ_STR(out.at(1).name, "Volume 1.cbz");
}

// Metadata is fetched for EVERY row, not lazily for the visible ones. Spec
// section 6.2: a future sort by date added or percent read needs the field for
// every row before the sort runs, so a lazy fetch would have to be undone.
static void test_metadata_is_fetched_for_every_row(void) {
    QVector<nf_entry> e;
    for (int v = 1; v <= 27; v++)
        e << ent(QString("Volume %1.cbz").arg(v).toUtf8().constData(), false);
    QVector<nf_row> out;
    int calls = 0;
    nf_build_listing(e, fake_meta, &calls, &out);
    CHECK(out.size() == 27);
    CHECK(calls == 27);
}

// Directories get no metadata lookup: there is no Volume for a folder, and
// asking would cost 27 pointless libnickel calls on the reference card.
static void test_directories_are_not_looked_up(void) {
    QVector<nf_entry> e;
    e << ent("Comics", true) << ent("Sandman", true);
    QVector<nf_row> out;
    int calls = 0;
    nf_build_listing(e, fake_meta, &calls, &out);
    CHECK(out.size() == 2);
    CHECK(calls == 0);
    CHECK(out.at(0).hasRow == false);
    CHECK(out.at(0).percentRead == -1);
}

static void test_missing_row_is_kept_and_marked(void) {
    QVector<nf_entry> e;
    e << ent("Volume 25.cbz", false) << ent("missing Volume 26.cbz", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 2);
    bool found = false;
    for (int i = 0; i < out.size(); i++) {
        if (out.at(i).name.startsWith(QStringLiteral("missing"))) {
            found = true;
            CHECK(out.at(i).hasRow == false);
        }
    }
    CHECK(found);
}

// THE ORDER TEST. Labels are derived after filtering, so a common prefix that
// only existed among the junk cannot strip text off the survivors. Here the
// three "Foo - " names include two that the allowlist drops; if labelling ran
// before filtering it would see a common run and strip it.
static void test_labels_are_derived_after_filtering(void) {
    QVector<nf_entry> e;
    e << ent("Foo - Keep.cbz", false)
      << ent("Foo - Drop.calibre", false)
      << ent("Foo - Also drop.lua", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 1);
    // One surviving row: nf_strip_common refuses on a single row, so the label
    // is the full name. Had labelling run first, it would read "Keep.cbz".
    CHECK_EQ_STR(out.at(0).label, "Foo - Keep.cbz");
}

// Ordering happens before labelling, so labels line up with the rows they
// belong to rather than with the pre-sort positions.
static void test_labels_match_their_rows_after_sorting(void) {
    QVector<nf_entry> e;
    e << ent("Fullmetal Alchemist v27 (2011) (Digital).cbz", false)
      << ent("Fullmetal Alchemist v01 (2005) (Digital).cbz", false)
      << ent("Fullmetal Alchemist v09 (2006) (Digital).cbz", false);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 3);
    CHECK(out.at(0).name.contains(QStringLiteral("v01")));
    CHECK(out.at(0).label.contains(QStringLiteral("v01")));
    CHECK(out.at(2).name.contains(QStringLiteral("v27")));
    CHECK(out.at(2).label.contains(QStringLiteral("v27")));
}

// Folders and files are labelled as SEPARATE sets. A folder name and a book
// name share nothing useful, and pooling them would find a common run of ""
// and strip nothing -- which silently disables the feature in any folder
// holding both kinds. Sandman holds 11 folders and 3 files, so this is the
// common case on the reference card, not an edge one.
static void test_folders_and_files_are_labelled_separately(void) {
    QVector<nf_entry> e;
    // Zero-padded on purpose. With "Volume 1" / "Volume 2" the remainder is a
    // SINGLE character, and Task 2's refuse-under-two-characters guard then
    // correctly declines to strip -- so the unpadded form tests the guard, not
    // the separation. Traced by hand before this plan was written.
    e << ent("Series - Volume 01.cbz", false)
      << ent("Series - Volume 02.cbz", false)
      << ent("Extras", true);
    QVector<nf_row> out;
    nf_build_listing(e, fake_meta, NULL, &out);
    CHECK(out.size() == 3);
    CHECK(out.at(0).isDir);
    CHECK_EQ_STR(out.at(0).label, "Extras");
    CHECK_EQ_STR(out.at(1).label, "01");
    CHECK_EQ_STR(out.at(2).label, "02");
}

int main(void) {
    test_junk_is_dropped_before_anything_else();
    test_metadata_is_fetched_for_every_row();
    test_directories_are_not_looked_up();
    test_missing_row_is_kept_and_marked();
    test_labels_are_derived_after_filtering();
    test_labels_match_their_rows_after_sorting();
    test_folders_and_files_are_labelled_separately();
    NF_TEST_MAIN_END
}
```

- [ ] **Step 2: Add `nflist.cc` to the host test build and run to verify failure**

Modify `Makefile`: `HOST_PURE := nffmt.cc nflist.cc`

Run: `make test`
Expected: build fails, `nflist.h` does not exist. Create the header and an empty `nf_build_listing`, re-run, and expect the `nflist` checks to fail on values while `test_nffmt` stays green.

- [ ] **Step 3: Write the implementation**

`nflist.h`:

```cpp
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
```

`nflist.cc`:

```cpp
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test`
Expected: both binaries green, `0 failures` each.

- [ ] **Step 5: Prove the order tests are not vacuous**

Two mutations:

1. Move the label stage (4) above the junk filter (1) — compute `names` from
   `entries` instead of `out`.
   Run `make test`. Expected: `test_labels_are_derived_after_filtering` fails.
2. Replace the two-pass loop with a single pass over all rows regardless of
   `isDir`.
   Run `make test`. Expected: `test_folders_and_files_are_labelled_separately`
   fails.

Revert both, confirm green. If either mutation stays green the test is not
pinning the order down and needs fixing before moving on.

- [ ] **Step 6: Amend spec §3.4 and commit**

Change §3.4 to record that within a single directory duplicate labels cannot
arise, that v1 therefore implements a collision guard which falls back to raw
names, and that the folder-on-row rule belongs to v2's search results.

```bash
git add nflist.h nflist.cc tests/test_nflist.cc Makefile \
        docs/superpowers/specs/2026-09-03-nickelfolders-v1-design.md
git commit -m "feat: the listing pipeline, with its stage order pinned by tests

Filter, order, fetch metadata, label -- in that order, and two tests exist only
to keep it that way. Labels are computed over the SURVIVING rows, so a common
prefix that only existed among junk cannot strip text off what is shown.

Folders and files are labelled as separate sets. Pooling them finds a common
run of "" and silently disables stripping in any folder holding both kinds,
which on this card is the common case rather than an edge one.

Metadata is fetched for every row rather than lazily for the visible ones,
because a future sort by a metadata field needs it before the sort runs.

Also corrects spec 3.4: within one directory, unique filenames minus a run
common to all of them stay unique, so duplicate labels cannot arise. v1 needs
a collision guard, not folder-on-row; that rule belongs to v2's flat search."
```

---

## Rungs 1-6: the device

**Everything from here needs hardware, and none of it can be unit-tested.**
The verification constraints in Global Constraints replace the test cycle. Every
rung follows the same protocol, and it is not optional:

```sh
python3 tools/kobo.py ssh 'pidof nickel'                 # record the PID FIRST
./nickeltc make
python3 tools/kobo.py push libnfolders.so /usr/local/Kobo/imageformats/libnfolders.so
python3 tools/kobo.py reboot && python3 tools/kobo.py wait
python3 tools/kobo.py ssh 'pidof nickel'                 # note it changed: that is the reboot
# ... exercise the rung ...
python3 tools/kobo.py ssh 'qndb -m ndbCurrentView'       # the ORACLE
python3 tools/kobo.py ssh 'pidof nickel'                 # unchanged = no crash
python3 tools/kobo.py ssh 'logread | grep -iE "hindenburg|segfault|SIGSEGV"'   # UNFILTERED
python3 tools/kobo.py shot screen.png                    # what is actually on the panel
```

`tools/kobo.py wait` polls for **Nickel**, not ssh — ssh answers well before
Nickel has started and a mod checked too early looks absent. `/tmp` is tmpfs and
clears on reboot, so anything staged there is re-pushed after every reboot.

### Task 6 (Rung 1): Pay the three debts, with no UI

**Files:**
- Create: `nfnickel.h`, `nfnickel.cc`
- Modify: `nfolders.cc`

**Interfaces:**
- Consumes: nothing from rung 0.
- Produces: `bool nf_nickel_resolve(void)`, `QString const *nf_db_name(void)`, `bool nf_open_book(QString const& contentId)`, and `int nf_watch_init(char const *path, void (*cb)(void))`.

Nothing here changes what the mod *does*; it changes what it is built on. Doing
it first means every later rung inherits a correct `dbName`, no leak and no poll
thread, instead of carrying three known-wrong things through five rungs.

- [ ] **Step 1: Move the libnickel symbols into `nfnickel.cc`**

Move the five `nh_dlsym` entries and the five function pointers out of
`nfolders.cc` verbatim, and add the two the spec requires:

```cpp
// Device::getDbName returns a QString const& -- a pointer to an ALREADY CACHED
// string. Three instructions (ldr/adds/bx), nothing allocated, nothing to free.
// NOTES.md has the disassembly.
static void          *(*Device__getCurrentDevice)(void);
static QString const *(*Device__getDbName)(void const *_this);
```

```cpp
{.name = "_ZN6Device16getCurrentDeviceEv", .out = nh_symoutptr(Device__getCurrentDevice), .desc = "Device::getCurrentDevice"},
{.name = "_ZNK6Device9getDbNameEv",        .out = nh_symoutptr(Device__getDbName),        .desc = "Device::getDbName"},
```

- [ ] **Step 2: Replace the hardcoded dbName**

```cpp
// dbName is a Repository cache-partition key. Device::calcDbName compares the
// device's storage path against the literal /mnt/onboard/.kobo and returns
// empty ONLY on a match, deriving a name via QDir::cleanPath otherwise. So a
// hardcoded "" is correct on this Libra 2 for a REASON and silently finds
// nothing on a model with an SD card. Nickel itself never passes a constant:
// all 117 call sites pass it in a register. NOTES.md has the derivation.
QString const *nf_db_name(void) {
    void *dev = Device__getCurrentDevice();
    if (!dev)
        return NULL;
    return Device__getDbName(dev);
}
```

- [ ] **Step 3: Free the proxy**

The spike leaks 52 bytes per open on purpose. `onSelected()` allocates a
28-byte worker on one path, which is why the spike did not free it. The proxy's
`parent` argument is passed straight through to its `QObject` base, so **give it
a parent and let Qt own it** rather than deleting it by hand:

```cpp
// Qt owns the proxy through the parent we pass, so there is nothing to delete
// here and nothing leaks -- the parent's destruction takes it. This replaces
// the spike's deliberate 52-byte-per-open leak (~1 kB per 20 opens, measured;
// immaterial in a probe, wrong in a mod that runs for weeks).
```

Then verify the claim rather than asserting it — step 6 measures RSS.

- [ ] **Step 4: Replace the poll thread with inotify**

```cpp
// The 500 ms poll thread was a probe mechanism. inotify through a
// QSocketNotifier is event-driven, runs on the GUI thread (so it is one of the
// safe windows for touching Nickel), and needs no second thread at all -- which
// also removes the postEvent hop the poller needed.
//
// The watch is on a /tmp path deliberately: /tmp is tmpfs, so this never holds
// a handle on /mnt/onboard, where one open during a USB session risks
// corruption. It also means the watch directory is recreated after every
// reboot.
```

Watch the *directory* `/tmp` for `IN_CLOSE_WRITE|IN_MOVED_TO` on the trigger
name, not the file — a watch on a file that does not exist yet cannot be
established, and NickelMenu's `touch` creates it fresh each time.

- [ ] **Step 5: Build and push**

Run: `./nickeltc make`
Expected: clean, no warnings (the Makefile has `-Werror`).

Then the push/reboot/wait sequence from the protocol above.

- [ ] **Step 6: Verify on the device**

| Check | Command | Expected |
|---|---|---|
| Mod loaded | `logread \| grep -i nickelfolders` | the init line |
| dbName is right | trigger a known-good ContentID | opens, as before |
| **Negative control** | trigger a ContentID no book has | `isValid=false`, no navigation, PID unchanged |
| Navigated | `qndb -m ndbCurrentView` | `ReadingView` |
| Right book | `python3 tools/kobo.py shot` | the book asked for |
| No crash | `pidof nickel`, then unfiltered `logread \| grep -iE 'hindenburg\|segfault\|SIGSEGV'` | PID unchanged, nothing |
| **Leak is gone** | `grep VmRSS /proc/$(pidof nickel)/status` before, after 1 open, after 20 | flat after the first, as it was before — but now with no 52-byte creep |
| Poller is gone | `ls /proc/$(pidof nickel)/task \| wc -l` | one fewer thread than before |

The RSS check is the one that matters, and note what it can and cannot show:
52 bytes × 20 is ~1 kB, which is **below** what `VmRSS` resolves. So a flat RSS
does **not** prove the leak is fixed — it only fails to contradict it. The proof
is structural: Qt owns the proxy through its parent. Record that reasoning rather
than claiming the measurement proved something it cannot.

- [ ] **Step 7: Commit**

```bash
git add nfnickel.h nfnickel.cc nfolders.cc
git commit -m "refactor: pay the three recorded debts before building on them

- dbName comes from Device::getCurrentDevice + getDbName. A hardcoded \"\" is
  correct on this Libra 2 for a reason -- calcDbName compares against the
  literal /mnt/onboard/.kobo -- and silently finds nothing on a model with an
  SD card.
- The proxy gets a parent, so Qt owns it and the spike's deliberate
  52-byte-per-open leak is gone. Note that VmRSS cannot resolve 1 kB, so the
  device check fails to contradict the fix rather than proving it; the proof is
  that the parent owns the object.
- inotify through a QSocketNotifier replaces the 500 ms poll thread, which also
  removes the cross-thread postEvent hop. The watch is on a /tmp DIRECTORY,
  because a watch cannot be established on a file that does not exist yet.

All libnickel symbols now live in nfnickel.cc -- one place for every layout
assumption, which is what CLAUDE.md asks for."
```

### Task 7 (Rung 2): An empty screen on Nickel's window stack

**Files:**
- Create: `nfbrowser.h`, `nfbrowser.cc`
- Modify: `nfnickel.h`, `nfnickel.cc`, `nfolders.cc`

**Interfaces:**
- Consumes: `nf_nickel_resolve`, `nf_watch_init`.
- Produces: `void nf_browser_show(void)` — constructs the controller and pushes it.

This is the rung the whole architecture rests on, so it deliberately draws
nothing. A screen that appears, reports itself to the oracle, and pops on back
is the entire deliverable.

- [ ] **Step 1: Measure `sizeof(AbstractController)` — do not guess it**

Read it out of Nickel's **own** `operator new` for a concrete controller, per
`CLAUDE.md`'s fixed method. `AbstractController::viewLoaded()` returns
`this+8`, so the object is at least 12 bytes, but "at least" is not a size.

```sh
NM=~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin/arm-linux-gnueabihf-nm
OD=~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin/arm-linux-gnueabihf-objdump
# Find a small concrete controller and its constructor
$NM -D --defined-only libnickel.so.1.0.0 | grep -E 'ControllerC1E' | head -40
# Disassemble the caller that news it, and read the movs/mov.w immediate
$OD -d --start-address=0x<addr> --stop-address=0x<addr+0x80> libnickel.so.1.0.0
```

Record the number and the address it came from in a comment. Over-allocate on
top of it, because the constructor cannot be told how much room it has.

- [ ] **Step 2: Resolve every PLT stub in `AbstractController::AbstractController` and `MainWindowController::push`**

Not optional. `tools/plt.sh` does the arithmetic. Skipping this step is what
cost the crash that `NOTES.md` documents: reading argument registers tells you
a register holds a pointer, and only the relocation tells you what kind.

```sh
sh tools/plt.sh 0x<stub-address>
```

- [ ] **Step 3: Build our vtable**

The measured layout, from `_ZTV18AbstractController` at `0x163ff70` with its
relocations (see the spec §1):

| Slot | Offset from vptr | Nickel's entry |
|---|---|---|
| 0 | +0 | `~AbstractController` (D1) |
| 1 | +4 | `~AbstractController` (D0) |
| 2 | +8 | `size()` |
| 3 | +12 | `viewWillAppear()` |
| 4 | +16 | `viewWillDisappear()` |
| 5 | +20 | `viewWillBeDestroyed()` |
| 6 | +24 | `allowedOrientations() const` |
| 7 | +28 | `navSection() const` |
| 8 | **+32** | `__cxa_pure_virtual` — **the one we must supply** |

`ensureViewLoaded` (`0xad1408`) calls slot 8 (`ldr r3,[r3,#32]; blx r3`), which
is what identifies it as the load-the-view method.

```cpp
// Our vtable is Nickel's with slot 8 replaced. Copying the rest rather than
// reimplementing it is the point: size, viewWillAppear, viewWillDisappear,
// viewWillBeDestroyed, allowedOrientations and navSection all have real
// implementations we want, and AbstractController is NOT a QObject so there is
// no metaobject to fake.
//
// Slot 8 is __cxa_pure_virtual in Nickel's own vtable, so leaving it unset is
// not an option -- it is the only member with no default.
```

Resolve `_ZTV18AbstractController` by name with `nh_dlsym` like everything else;
never use the address.

- [ ] **Step 4: Wire the NickelMenu entry**

Add to the device's NickelMenu config (`/mnt/onboard/.adds/nm/`):

```
menu_item :library :Folders :cmd_spawn :quiet:/bin/touch /tmp/nfolders-show
```

This is device configuration, not code, and it is why rung 2 needs no new
trigger mechanism — rung 1 already built the watch.

- [ ] **Step 5: Build, push, reboot**

Per the protocol at the head of this section.

- [ ] **Step 6: Verify — and this is where the negative control matters most**

| Check | Command | Expected |
|---|---|---|
| Baseline view | tap into the stock library first | `qndb -m ndbCurrentView` reports a library view, **not** Home |
| Screen appeared | tap the NickelMenu item, then the oracle | a view that is **not** the baseline |
| No crash | `pidof nickel` unchanged; unfiltered crash grep | PID same, nothing logged |
| Back pops | press back, then the oracle | **the baseline view**, not Home |
| Panel agrees | `python3 tools/kobo.py shot` | an empty screen where the library was |

**Set the baseline to a library view, never to Home.** This is the exact
mistake `NOTES.md` records: Home is also what the rival hypothesis predicts, so
a measurement taken from Home discriminates nothing — and it was written down as
confirmed anyway. If the baseline is Home, the run proves nothing regardless of
its result.

**Negative control:** temporarily point slot 8 at a null pointer, push, and
trigger. Nickel must crash or refuse *visibly*. If a broken slot 8 produces the
same observation as a working one, the check is vacuous and the rung is not
verified. Restore, rebuild, re-verify.

- [ ] **Step 7: Commit**

```bash
git add nfbrowser.h nfbrowser.cc nfnickel.h nfnickel.cc nfolders.cc
git commit -m "feat: a real screen on Nickel's window stack

An AbstractController subclass over Nickel's own vtable with slot 8 replaced --
the single __cxa_pure_virtual, at +32, which ensureViewLoaded calls. Every other
slot keeps Nickel's implementation, and AbstractController is not a QObject so
there is no metaobject to fake.

It draws nothing on purpose. Appearing, reporting to ndbCurrentView, and popping
on back is the whole deliverable, measured from a LIBRARY baseline rather than
Home -- Home is what the rival hypothesis predicts too, and NOTES.md records
what happened the last time a baseline was set to the expected value."
```

### Task 8 (Rung 3): A list widget with hardcoded rows

**Files:**
- Modify: `nfbrowser.h`, `nfbrowser.cc`

**Interfaces:**
- Consumes: `nf_row` from `nflist.h`.
- Produces: `void nf_browser_set_rows(QVector<nf_row> const&)`.

Hardcoded rows on purpose: this rung answers "does our widget draw, scroll and
take a tap on this panel" without any question about where the data came from.
Use the **real measured names** as the hardcoded set — the 27 Fullmetal
Alchemist labels and the 4 Pokémon ones — because a row of `"Test 1"` cannot
show a label-width problem and those two folders are the ones that have one.

- [ ] **Step 1: Draw the list**

A plain `QListView` over a small `QAbstractListModel`, or a `QWidget` with a
hand-painted list. Prefer the `QListView`: it gets kinetic scrolling and the
platform's touch handling for free, which is most of why the native screen was
chosen.

Inherit Nickel's palette and font rather than setting either — that is the other
half of the reason.

- [ ] **Step 2: Build, push, reboot, screenshot**

- [ ] **Step 3: Verify**

| Check | Expected |
|---|---|
| `python3 tools/kobo.py shot` | 27 readable rows, cropped to 1264x1680 |
| Label width | the Fullmetal labels are not clipped, and the Pokémon ones show `(Part …)` |
| Scroll | a drag moves the list and the e-ink refresh is not visibly torn |
| Tap | tapping a row logs the row index — no navigation yet |
| PID | unchanged |

**A screenshot is the only evidence here.** `ndbCurrentView` says our controller
is current; it says nothing about whether the panel shows a list or a blank
rectangle.

- [ ] **Step 4: Commit**

```bash
git add nfbrowser.h nfbrowser.cc
git commit -m "feat: draw the list, with the measured names as the fixture

Hardcoded rows on purpose, so this rung answers only whether the widget draws,
scrolls and takes a tap. The fixture is the real 27 Fullmetal Alchemist labels
and the 4 Pokemon ones rather than 'Test 1' -- those two folders are the ones
with a label-width problem, so a synthetic fixture would hide the thing worth
looking at."
```

### Task 9 (Rung 4): Real listings

**Files:**
- Modify: `nfnickel.h`, `nfnickel.cc`, `nfbrowser.h`, `nfbrowser.cc`

**Interfaces:**
- Consumes: `nf_build_listing`, `nf_meta_fn`, `nf_db_name`.
- Produces: `nf_nickel_meta(void *ctx, QString const& name, nf_row *row)` matching `nf_meta_fn`, and navigation state in `nfbrowser`.

This rung wires rung 0's tested pipeline to rung 1's libnickel wrappers. The
pipeline itself needs no new tests — it has them — so the work here is the
metadata callback and the `QDir` read.

- [ ] **Step 1: Establish `Volume::getDbValues()`'s calling convention**

`_ZNK6Volume11getDbValuesEv` at `0x00a63f64` — **11**, not 12; the length
prefix in an Itanium mangling counts the identifier's characters and
`getDbValues` has eleven. Verified against the symbol table rather than
hand-mangled, because the plan's first draft got this wrong and a wrong symbol
name resolves to nothing at load time while NickelHook logs it and disarms.
It returns a container by value,
so expect a hidden return buffer as argument zero — the same shape that crashed
Nickel once for `getById`.

```sh
OD=~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin/arm-linux-gnueabihf-objdump
$OD -d --start-address=0x00a63f64 --stop-address=0x00a64010 libnickel.so.1.0.0
```

Then **resolve every PLT stub it calls** with `tools/plt.sh`. The relocations
are what identify the container type — `QHash<QString,QVariant>` versus
`QVariantMap` — and reading argument registers alone cannot. Record the
derivation in `NOTES.md`, not just the conclusion.

Cross-check against the exported `Volume::fromAttributes(QHash<QString,QVariant> const&)`, which is the same ORM's other half and names its own type.

- [ ] **Step 2: Write the metadata callback**

```cpp
// Runs inside the browser's own Qt signal handling, which CLAUDE.md names as a
// safe window for /mnt/onboard -- but this function touches no files at all: it
// asks Nickel's in-memory Repository cache, which is what getById is.
//
// ContentID is file:///mnt/onboard/<relative path> VERBATIM. Measured: accented,
// Cyrillic, parenthesised and 230-character paths all resolve, so no escaping
// or normalisation belongs here. Adding any would be the bug.
```

Over-allocate the `Volume` buffer as the spike does, with the measured size in
a comment. Destroy every `Volume` — one per row, 27 on the largest folder.

- [ ] **Step 3: Read the directory**

```cpp
// QDir::entryList, NON-RECURSIVE, one directory per navigation. The tree is
// derivable from the database alone, but a file with no database row exists
// ONLY on disk, and that is the row this browser must not hide. Since the read
// is happening anyway, the filesystem is the tree source.
//
// This opens and closes within one call, so it never holds a handle on
// /mnt/onboard across a USB session. Do not cache a QDir.
```

- [ ] **Step 4: Time the metadata pass — the measurement this design rests on**

`getById` calls `Repository::refreshCache<Volume>(dbName)` and nobody has timed
it. Spec §6.2 raised the count to every row in the listing, so the number that
matters is **27 lookups before first paint**, not the handful visible.

Add a temporary `nh_log` of elapsed microseconds around the whole metadata pass,
using `clock_gettime(CLOCK_MONOTONIC)`.

**Do not measure this through the trigger path.** The trigger has an inotify
latency and syslog timestamps are one-second granular, so both swamp a
sub-millisecond lookup — and would fail *patterned*, every sample landing near
the trigger interval, which is this project's known instrument signature. Time
the loop from inside the mod and log the delta.

**Negative control for the clock:** log the elapsed time of a deliberately slow
path too — 27 lookups plus a `nanosleep(10ms)` — and confirm the two readings
differ by about the sleep. If they read the same, the clock or the logging is
the thing being measured, not the lookups.

If 27 lookups cost more than roughly 200 ms, stop and reconsider: the fallback
is one SQL query per folder, which reintroduces a database handle and is
therefore a fallback rather than the plan.

- [ ] **Step 5: Verify**

| Check | Expected |
|---|---|
| Root listing | `screensavers`, `books`, `audiobooks`, `drawings`, `Digital Editions`, and the 2 root `.kepub.epub` files. **No** `metadata.calibre`, `KOBOY-INSTALL.md`, `koboy-probe-Io.txt`, `System Volume Information` or `*.sdr` |
| Navigate in | `books/Comics/English/Sandman` shows 11 folders then 3 files, folders first |
| **Ordering** | `v1 … v9, v10` — **not** `v1, v10, v2` |
| **Labels** | the Fullmetal folder reads `v01 (2005)` … `v27 (2011)` |
| Depth | `Sandman/Sandman Mystery Theatre/Blackhawk` reachable, 5 levels down |
| Timing | logged, with the negative control beside it |
| PID | unchanged throughout |

The ordering and label checks are the payoff for rung 0: both were already
proven on the host, so a failure here is a wiring fault rather than a logic
fault, which is a much smaller search.

- [ ] **Step 6: Remove the timing instrumentation, record the number in `NOTES.md`, commit**

```bash
git add nfnickel.h nfnickel.cc nfbrowser.h nfbrowser.cc NOTES.md
git commit -m "feat: real listings, from the filesystem plus Nickel's own lookup

QDir::entryList one directory at a time for the tree, getById and getDbValues
for each row's metadata. No SQL, no handle on the 432 MB database, no race with
Nickel's cache.

getDbValues' calling convention and every PLT stub it calls are derived in
NOTES.md rather than assumed -- it returns a container by value, the same shape
that crashed Nickel once for getById.

Also records the measured cost of 27 lookups before first paint, timed from
inside the mod with a negative control on the clock. Timing it through the
trigger path would have measured inotify latency and syslog granularity, and
would have failed patterned."
```

### Task 10 (Rung 5): Tap to open

**Files:**
- Modify: `nfbrowser.cc`

**Interfaces:**
- Consumes: `nf_open_book` from rung 1, `nf_row`.
- Produces: nothing new.

- [ ] **Step 1: Wire the tap**

A tap on a folder row navigates into it. A tap on a book row calls
`nf_open_book` with the row's ContentID. A tap on a row with `hasRow == false`
does nothing yet — rung 6 gives it its reason.

```cpp
// The proven sequence, unchanged: getById -> ReadBookActionProxy -> onSelected.
// Do NOT "simplify" this into a ReadingController push. The proxy is the object
// behind the library's own Read button, which is why Nickel's own bookkeeping
// runs -- the book lands in Recents, the reading session starts, and the
// bookmark is restored. All three were measured; a direct push gets none.
```

- [ ] **Step 2: Verify the loop that is the whole point**

| Step | Check |
|---|---|
| Navigate to the Fullmetal folder | 27 rows, labels correct |
| Tap `v03` | `qndb -m ndbCurrentView` reports `ReadingView` |
| Screenshot | volume **3**, not another one |
| Press back | the oracle reports our browser, **in the Fullmetal folder** |
| Tap `v04`, back, `v05`, back — ten times | still the Fullmetal folder every time |
| PID | unchanged throughout |
| `VmRSS` | flat after the first open |

Then the long form: **20 consecutive opens with no back press**, then one back.
`NOTES.md` measured exactly this for the spike — readers are replaced, not
stacked, and one back returns you to where you started. Re-measuring it here
confirms that a *pushed controller of ours* underneath behaves the same as a
library view did.

**Negative control:** tap a row for a ContentID no book has (rung 6's greyed
state, reachable now by pointing a row at a bogus path). It must not navigate,
and the PID must not change.

- [ ] **Step 3: Commit**

```bash
git add nfbrowser.cc
git commit -m "feat: tap a book and it opens in the stock reader

browse -> tap -> read -> back -> the same folder, confirmed ten times over and
then across 20 consecutive opens with a single back press. NOTES.md measured
readers being replaced rather than stacked with a library view underneath; this
confirms a pushed controller of ours behaves the same, which is what makes the
loop need no reader lifetime management."
```

### Task 11 (Rung 6): Greyed rows, progress, and the folder you left

**Files:**
- Modify: `nfbrowser.cc`, `nfbrowser.h`
- Modify: `nflist.cc` only if a state is missing

**Interfaces:**
- Consumes: `nf_row.hasRow`, `nf_row.percentRead`, `nf_row.finished`.
- Produces: nothing new.

- [ ] **Step 1: Render the three row states**

- `hasRow == false`: greyed, and **say why on the row** — "not in library". A
  silently missing book is how you spend an evening wondering where volume 26
  went. The reference card has exactly one: the truncated 8 MiB
  `Fullmetal Alchemist v26`, against 99 MB and 119 MB for v25 and v27.
- `percentRead` in 1..99: show the percentage.
- `finished`: show a finished marker. Nothing at all for unread.

- [ ] **Step 2: Distinguish empty from unreadable**

```cpp
// "This folder has nothing to show" and "this folder could not be read" are
// different diagnoses to someone holding an e-reader with no terminal, and
// koboy arrived at the same distinction independently (romlist returning -1
// versus 0). Spec section 3.6.
//
// A third state arrives with filtering in v2 -- "everything here was filtered
// out" -- which must not read as an empty folder either. The message is chosen
// per state rather than shared, so adding the third one is an addition and not
// a rewrite.
```

Note that `screensavers/` and `drawings/` are reachable and contain no books, so
the empty message is on screen the first time anyone opens either — this state
is not hypothetical on the reference card.

- [ ] **Step 3: Remember the folder, not the scroll**

```cpp
// Written on leaving, in a Qt signal handler -- one of the two windows
// CLAUDE.md names as safe for /mnt/onboard, and the write is one short string.
//
// Scroll position is deliberately NOT persisted. Back from the reader pops to
// this controller still alive with its view loaded, so scroll survives within a
// session for free -- that is the measured replace-not-stack result paying out.
// Persisting it would be a keyed map for something already free.
```

- [ ] **Step 4: Verify**

| Check | Expected |
|---|---|
| Fullmetal folder | 26 normal rows and **v26 greyed, with its reason visible** |
| Progress | a book part-read shows a percentage; an unread one shows nothing |
| Screenshot | the greyed row is legible as greyed on e-ink, not merely lighter |
| `drawings/` | the empty message, not a blank list |
| Last folder | leave the browser from the Sandman folder, exit, re-enter — lands in Sandman |
| Across a reboot | re-enter after a reboot — still Sandman |
| PID | unchanged |

The greyed-row screenshot matters more than it looks: four grey levels is what
this panel gives, and "greyed out" that reads as "slightly different" is not a
usable signal. `../koboy/CLAUDE.md` and `TESTED.md` carry the measured e-ink
knowledge if it needs tuning.

- [ ] **Step 5: Update `CLAUDE.md` and commit**

`CLAUDE.md`'s STATUS says the browser does not exist. It does now. Update the
status, the Layout table for the new files, and add `make test` to the Build
section — the claim "there is no host test suite and no `make test`" is no
longer true, and leaving it there would send the next reader looking for a
weakness that has been fixed.

```bash
git add nfbrowser.cc nfbrowser.h CLAUDE.md
git commit -m "feat: greyed rows, reading progress, and the folder you left

The greyed row carries its reason, because a silently missing book is how you
spend an evening wondering where volume 26 went -- and v26 is exactly the case,
a truncated 8 MiB copy against 99 and 119 MB for its neighbours.

Empty and unreadable stay separate diagnoses, chosen per state so that v2's
'everything was filtered out' is an addition rather than a rewrite. Both are
reachable on this card: screensavers/ and drawings/ hold no books.

Last folder is remembered; scroll position deliberately is not, because back
from the reader pops to a controller still alive with its view loaded.

CLAUDE.md no longer claims there is no host test suite."
```

---

## Self-review

Run against the spec after the plan was written.

**Spec coverage.** §1 architecture → Task 7. §2 data, no SQL → Tasks 6, 9. §3.1
comparator → Task 1. §3.2 stripping → Task 2. §3.3 ordering → Task 3. §3.4
duplicates → Task 5, which corrects the spec. §3.5 allowlist → Task 4. §3.6
empty versus unreadable → Task 11. §3.7 depth → Task 9. §4 launch → Task 7 step
4; last folder → Task 11; sorting → Task 3; non-book files → Task 4; progress →
Task 11; collections non-goal → nothing to build, correctly. §5 rungs → Tasks
6-11. §6.1 pipeline order → Task 5, with two tests pinning it. §6.2 eager
metadata → Task 5, measured in Task 9. §6.3, §6.4, §6.5 → v2, deliberately no
tasks.

**One gap found and closed:** §6.2's claim that eager metadata "raises §7's
timing question" had no task actually measuring it. Task 9 step 4 now does,
with a negative control on the clock.

**Type consistency.** `nf_entry` is declared once in `nffmt.h` and used by
`nflist.h`, `nflist.cc` and both test files. `nf_row` is declared once in
`nflist.h`. `nf_meta_fn`'s signature matches `fake_meta` in the tests and
`nf_nickel_meta` in Task 9. `nf_book_extension` is used by both
`nf_is_book_name` and `nf_strip_common`.

**Known deviations from the plan template, and why.** The skill's TDD cycle
applies to Tasks 1-5 and cannot apply to Tasks 6-11: every interesting call
there is a libnickel call, which `CLAUDE.md` names as a real weakness of this
project rather than an oversight. Those tasks carry the spec's verification
protocol instead — oracle, PID watch, screenshot, negative control — and each
one names its negative control explicitly, because a device check without one is
not known to be non-vacuous.
