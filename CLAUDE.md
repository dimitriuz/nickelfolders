# nickelfolders

A folder-navigating book browser for modern Kobo e-readers that runs **on top
of Nickel** — Kobo's own reading software stays running, and the chosen book
opens in the **stock reader**. Not a replacement reader; that is what KOReader
is for, and the whole point here is that Kobo's reader is the good part and its
library is the bad part.

It is a **Nickel mod**: a C++/Qt shared library injected into the Nickel process
as a Qt image-format plugin, built on
[NickelHook](https://github.com/pgaskin/NickelHook). Nickel is not modified on
disk; the mod is removable by deleting one file.

**STATUS: rungs 0–2 are device-verified and there is a working screen, but it
is a flat book list, not a folder tree.** Opening an arbitrary sideloaded book
by ContentID (rung 1) and pushing a screen of our own choosing onto Nickel's
window stack that lists books we chose, opens the tapped one in the stock
reader, and returns to our list on back (rung 2) are both proven on hardware.
**The folder tree itself does not exist yet** — rung 2's screen is built by
borrowing Nickel's own `ArticleListLibraryController`, which lists `Volume`s,
not filesystem entries, so it is a deliberately smaller deliverable than the
spec's folder browser, not a step toward one. `nfolders.cc`/`nfnickel.cc`/
`nfbrowser.cc` are the code; `NOTES.md` is the reverse-engineering record that
made it possible, including the rejected candidate that *would* have given
folders for free and why it was rejected anyway. Read both before proposing
anything.

## The one thing that was in doubt, and no longer is

Verified on hardware 2026-09-03 (Kobo Libra 2, firmware 4.38.23684):
`VolumeManager::getById` → `ReadBookActionProxy` → `onSelected()` opens an
arbitrary **sideloaded** book in the stock reader. `ndbCurrentView` went
`HomePageView` → `ReadingView`, a framebuffer grab showed the right book with
Nickel's own header and chapter footer, and Nickel's PID never changed. `dbName`
empty is correct.

The book also landed in Nickel's **Recents**, which is the evidence for using
`ReadBookActionProxy` rather than pushing a `ReadingController`: Nickel's own
bookkeeping ran. Do not "simplify" that call site.

**Back POPS to the view beneath**, measured from a non-Home baseline:
`DragonLibraryView` → `ReadingView` → back → `DragonLibraryView`. So the
browser can be a real screen on Nickel's window stack that you return to, which
is what the native-screen architecture needs to be worth building.

That measurement was botched once and the reason is worth carrying: the first
attempt set the baseline view to Home, which is also what the rival hypothesis
predicts, so it discriminated nothing — and it was written down as confirmed
anyway. **Never set a baseline to the value you expect to measure.** `NOTES.md`
has the full account under Results.

**Repeated opens are safe and readers do not stack.** 20 consecutive opens with
no back press in between: no crash, no slowdown, RSS byte-identical after the
first, the last-opened book is the one shown, and **one** back press returns to
where you started. So `browse → tap → read → back → same folder` needs no
reader lifetime management and no special handling, however long the session.

## Rung 2: a screen on Nickel's own window stack, device-verified

Verified on hardware 2026-09-03, same device and firmware: a screen of ours,
pushed with `MainWindowController::push`, listing books we chose, that opens
the tapped book in the stock reader and returns to our list on back —
achieved by **borrowing Nickel's own `ArticleListLibraryController`** rather
than building one, so with zero fabricated vtables, zero fabricated RTTI, zero
unnamed member writes and no tap hook. `ndbCurrentView` reads
`DragonLibraryView`, the same name Nickel's own library list uses; the screen
renders full-screen with back arrow, status bar, sort/filter chevron, covers,
titles, authors, format and size, all Nickel's own. `NOTES.md` ("Task 7, rung
2, device results") has the full chain and addresses.

**What it does NOT give, measured rather than feared**: this mod's own
label-shortening (spec 3.2) is unreachable — rows render from the `Volume`'s
own metadata in full — and these controllers list `Volume`s, not filesystem
entries, so **folders are still unsolved**. A candidate that *would* list
folders (`NotebookGridController`) was found and rejected: its `moveToPath`
mutates a process-wide singleton Nickel's own "My Notebooks" view reads from,
so using it would re-root the owner's own notebooks view. `NOTES.md` has the
full account, including a second rejected candidate
(`QuickAccessLibraryController`, kept behind a compile-time selector for
comparison) whose screen had no back chrome at all.

## Build

```sh
git submodule update --init      # NickelHook
./nickeltc make                  # -> libnfolders.so
./nickeltc make koboroot         # -> KoboRoot.tgz
./nickeltc make clean
```

`./nickeltc` runs the real toolchain inside Docker. Mods **must** be built with
[NickelTC](https://github.com/pgaskin/NickelTC) (`ghcr.io/pgaskin/nickeltc:1.0`)
— GCC 4.9.4 against Nickel's own Qt 5.2.1. This is **not** koboy's Linaro
toolchain and the two are not interchangeable: a mod that links fine with
anything else can still be subtly wrong in Nickel's process.

**`make test` exists (90 checks, two binaries) but does not reach the device.**
It builds `nffmt.cc`/`nflist.cc` — the *pure* display, ordering and listing
logic, deliberately free of libnickel, NickelHook and I/O (see the header
comments) — with the host's own compiler and Qt5, against `tests/test_nffmt.cc`
(63 checks) and `tests/test_nflist.cc` (27 checks). It is a genuinely separate
build path from `./nickeltc make`: host Qt is 5.15, the device's is 5.2.1, and
the two are deliberately kept skewed rather than pinned together, so that an
API that only exists in 5.15 breaks the NEXT `./nickeltc make` (loud, at build
time) rather than failing quietly on the panel. What it still cannot cover is
everything this file's verification culture exists for: every libnickel call
(`nfnickel.cc`, `nfbrowser.cc`) is untestable off-device by construction,
because there is no libnickel to link against on the host — that weakness is
real, not fixed by `make test`, and is why the device discipline below is
still not optional.

## Device workflow

```sh
python3 tools/kobo.py ssh 'pidof nickel'
python3 tools/kobo.py push libnfolders.so /usr/local/Kobo/imageformats/libnfolders.so
python3 tools/kobo.py reboot && python3 tools/kobo.py wait
python3 tools/kobo.py open 'file:///mnt/onboard/books/Some Book.epub' --stage 2
python3 tools/kobo.py shot screen.png
python3 tools/kobo.py pull /usr/local/Kobo/libnickel.so.1.0.0 libnickel.so.1.0.0
sh tools/nftest.sh               # push to the device and run there
```

Connection details are in `device.local`, which is **gitignored** —
see `DEVICE.local.md`.

- **A REBOOT IS THE UNIT OF ITERATION.** The mod loads when Nickel starts, so
  every code change costs a reboot, and a crash costs a reboot too. Budget
  accordingly and batch what you want to learn.
- Pushing the `.so` straight into `/usr/local/Kobo/imageformats/` and rebooting
  is the fast path. `KoboRoot.tgz` into `/mnt/onboard/.kobo/` is the route a
  *user* takes (Nickel extracts and deletes it on boot); it costs the same
  reboot, so prefer the direct push while developing.
- **`/tmp` is tmpfs and clears on reboot.** Anything staged there — including
  `tools/nftest.sh` — has to be re-pushed after every reboot. This has already
  wasted a cycle.
- `python3 tools/kobo.py wait` polls for **Nickel**, not for ssh. ssh answers
  well before Nickel has finished starting, and a mod checked too early looks
  absent.
- **`wait` requires low uptime, not just a `nickel` pid.** A pid alone cannot
  tell a dying old Nickel from a fresh one: caught on a real session, `wait`
  matched the OUTGOING Nickel while it was still shutting down for the very
  reboot it was waiting out, printed "nickel is up," and a `pidof nickel` a
  few seconds later came back empty with the newest log line still from the
  PREVIOUS boot — indistinguishable from a boot loop unless you know to
  expect it. `wait` now also reads `/proc/uptime` and requires it below a
  threshold before declaring victory.
- **`push`, `pull` and `reboot` now fail loudly on a failed ssh/scp**, instead
  of the old behavior (still exit 0, whatever `ssh`/`scp` printed to stdout
  notwithstanding). Two real sessions were burned by this before the fix: a
  `push` that hit `scp: .../libnfolders.so: Read-only file system` and still
  reported success, so the next test ran a stale `.so`; and a `reboot` whose
  ssh connection hung past its own `ConnectTimeout` ("Connection timed out")
  and still reported success, so `reboot && wait` chained straight past a
  reboot that never happened. Trust the exit code these three verbs now give
  you; do not re-add a piped `| tail -1` or similar around them — that is
  exactly what swallowed the error output that would have caught this sooner.

### Backing out

- Create `/mnt/onboard/nfolders_uninstall` and reboot — the mod deletes itself
  and the flag.
- Or delete `/usr/local/Kobo/imageformats/libnfolders.so` and reboot.

## Hard constraints — check these before proposing anything

- **Nickel's classes stay opaque.** `typedef void Volume;` and an explicitly
  written call signature, never a real C++ class or a redeclared method. A real
  class turns a libnickel layout change into a silent miscompile; the opaque
  form keeps every layout assumption in one place and written down.
- **No C++ standard library runtime.** Qt and libc only. The C++ *language* is
  fine (classes, `override`); anything needing stdlib runtime support or
  compiling templates into the library is not, because libstdc++ gives no ABI
  guarantee and Kobo can change it. NickelHook's README is explicit about this.
- **Over-allocate for every Nickel constructor**, and record the measured size
  in a comment. The constructor cannot be told how much room it has, so the
  headroom is what survives a firmware that grows the object.
- **Never hold a file handle on `/mnt/onboard`** for more than a few hundred
  milliseconds — a USB session while one is open risks corruption. The safe
  windows are the init function and Qt signal handlers. The spike's trigger
  file is on `/tmp` for exactly this reason.
- **Nickel's UI may only be touched from the GUI thread.** The trigger watch
  (`nfnickel.cc`'s `QSocketNotifier` on an inotify fd, replacing the spike's
  poll thread) already runs on the GUI thread by construction — its callback
  fires from the `activated()` signal inside Qt's own event loop — so there is
  no cross-thread hop left to get wrong. If a second thread is ever
  introduced, it must reach Nickel via `QCoreApplication::postEvent`
  (documented thread-safe), never by calling libnickel directly.
- **No file-scope object with a non-trivial (dynamically initialised)
  constructor** — no `QString`, `QByteArray`, `QObject`, `QVector`, etc. at
  file scope. NickelHook's `__attribute__((constructor)) nh_init` runs from
  this library's `.init_array` **before** this translation unit's own C++
  dynamic initialiser (`_GLOBAL__sub_I_<file>.cc`) does — confirmed with
  `readelf` — and `nh_init` calls straight through to `nf_init`. A file-scope
  `static QByteArray` here was still zeroed `.bss` (its constructor had not
  run yet) when `nf_init` read it, which is a load through a `d` pointer that
  was never constructed — a crash at plugin load, on every boot, boot-looping
  the mod into the shared failsafe below. POD types have no such race: `.bss`
  zero-initialises them with no constructor to be late. Check with
  `nm libnfolders.so | grep GLOBAL__sub_I`, which must print **nothing**.
- **The NickelHook failsafe is SHARED infrastructure.** A mod that fails `init`
  hard can cause the user's *other* NickelHook mods (NickelMenu, NickelDBus) to
  uninstall themselves. So treat anything non-essential as non-fatal and return
  0 — see `nf_init`.
- **GCC 4.9's C++ frontend rejects a designated initializer that SKIPS a
  field** ("non-trivial designated initializers not supported"). Spell out
  every field up to the last one set, in order — this is why `nh_info`'s
  `.uninstall_xflag = NULL` is there despite being unused.
- **Library names under 3 letters are reserved** for the upstream mod authors.
  `libnfolders.so`, never `libnf.so`.
- **`nh_log` truncates at 256 bytes**, silently. Book paths on this device run
  to 230+ characters and Cyrillic ones are 2 bytes per character, so a logged
  ContentID is routinely cut. Never build a measurement on a log line that is
  supposed to carry a whole path — it already produced 15 false results once
  (`NOTES.md`).
- The reference device already runs **NickelMenu, NickelDBus, kfmon, KOReader
  and koboy**. That is a feature (`qndb` is an oracle) and a risk (the shared
  failsafe above).

## What the hardware overruled — do not re-derive these

`NOTES.md` is the record with the disassembly; this is the short version.

- **`VolumeManager::getById` is a `static` member function AND returns by
  value.** The real signature is
  `getById(void *ret, QString const *id, QString const *dbName)` — `r0` is the
  hidden return buffer, `r1` is the id, `r2` is `dbName`, `r3` unused. The
  Itanium ABI mangles static and non-static members **identically**, so the
  symbol name cannot tell you there is no `this`. Reading `r1` as `this`
  compiles, links, resolves, and **crashed Nickel on the first device run**.
- **`dbName` is a `Repository` cache-partition key. Empty is correct for
  internal storage — and ONLY for internal storage.**
  `Device::calcDbName()` compares the device's path against the literal
  `/mnt/onboard/.kobo` and returns empty on a match, deriving a name via
  `QDir::cleanPath` otherwise. So **do not hardcode `""`**: call
  `Device::getCurrentDevice()` then `Device::getDbName()`, which returns a
  `QString const&` to an already-cached string (three instructions, nothing to
  free). A hardcoded empty string works on the reference device and silently
  finds nothing on a model with an SD card. `NOTES.md` has the derivation and
  the one part still unverified.
- **`ReadBookActionProxy` is the right entry point, not `ReadingController`.**
  It is the object behind the library's own Read button, so Nickel's
  bookkeeping — reading session, bookmark restore, analytics,
  download-if-needed — happens by itself instead of being reimplemented.
- `sizeof(Volume) == 8` (a vptr plus a refcounted pointer to a 408-byte shared
  block, so copies are cheap). `sizeof(ReadBookActionProxy) == 52`, read out of
  Nickel's own `operator new` call inside `ActionProxyMixin::readBookProxy`.
- **The proxy's `parent` argument is passed straight through to its QObject
  base**, so any `QObject` of ours works and Qt owns the proxy afterwards.
- **Neither NickelDBus nor NickelMenu can open a book.** NickelDBus exposes 64
  methods (`qndb -a`), none of them; NickelMenu's `nickel_open` takes views
  only. Both were checked — do not re-check them.
- **The folder tree is already in Nickel's database.** `content.ContentID` is
  `file:///mnt/onboard/<relative path>` verbatim; Nickel imports recursively and
  keeps the whole path, it just refuses to show it as a tree. So the browser
  needs **no import step and no filesystem walk** — one query gives the tree.
- **Import is effectively complete, but not universally.** 226 of the 227 book
  files on the reference device have a row `getById` finds; the one exception is
  a truncated 8 MiB copy. Every format present works — `.epub`, `.kepub.epub`,
  `.cbz`, `.cbr`, `.pdf`, `.txt` — including `.cbr`, which is not on Kobo's
  official list but imports and renders. Non-ASCII and 230-character paths are
  a non-issue. So **the browser must treat "file on disk with no row" as a
  normal case** and say so in the row, rather than asserting or hiding it.
- **Nickel already contains folder-browsing machinery**, used for notebooks and
  the Dropbox/Drive views: `RootFolder`, `BrowseableFolder`, `FolderItem`,
  `FolderItemManager`, `FolderItemDataSource`, `FolderItemListWidget`,
  `FolderItemMenuController`, and `folder://` among its URL schemes.
- `MainWindowController::sharedInstance()` / `::push(AbstractController*, bool)`
  and `LibraryNavMixin::pushShelf(QString const&)` are all exported.

## Method: adding a new libnickel call

The project's whole risk is here, so the procedure is fixed.

1. Find the symbol: `strings -n 8 libnickel.so.1.0.0 | grep '^_Z' | sort -u`.
2. Disassemble it (`arm-linux-gnueabihf-objdump -d --start-address=…`). koboy's
   Linaro toolchain supplies the ARM binutils; NickelTC's work too.
3. **Resolve EVERY unexplained PLT stub it calls, with `tools/plt.sh`.** This
   step is not optional and skipping it is what cost a crash: reading argument
   registers tells you a register holds a pointer, and only the relocation
   tells you *what kind*. `QVariant::QVariant(QString const&)` consuming `r1`
   is what proved `getById` takes no `this`.
   - **Neither the presence nor the absence of a `bl` in the disassembly is
     trustworthy on its own.** `tools/plt.sh` reports `<no PLT stub found>`
     for a real, un-PLT-routed direct `bl` to a local address **and** for a
     `bl`-shaped byte sequence that is not an instruction at all — it cannot
     tell the two apart, because it only matches against the relocation
     table, and a direct call generates no relocation. Five separate
     `bl`-shaped things in this firmware turned out to be **literal-pool
     bytes** parked at the end of a function body, not calls; each was
     caught by reading the *surrounding* words (do they decode as plausible
     code, or as `<UNDEFINED>`/GOT-offset garbage?) and confirming the
     function's real epilogue lands well before the address in question.
     Check what the bytes are before trusting either a hit or a miss.
4. Get object sizes from Nickel's **own** `operator new` call sites, never
   from a guess about layout. **`readelf -r | grep <full mangled name>`
   silently returns nothing even when a call site exists**, because
   `readelf -r` truncates the symbol column (e.g. `_ZN20InMemoryDataProv`,
   cut mid-name) — a full-name grep against a truncated column can never
   match. This produced a confident "no `operator new` call site exists
   anywhere in this firmware" conclusion after sweeping a 152,960-entry
   relocation table, which was false: `objdump -R` (untruncated) found it
   immediately. Prefer `objdump -R` for this kind of sweep, or verify a
   `readelf -r` search against a KNOWN-present symbol first — a sweep that
   cannot fail visibly is not a sweep.
5. Add a rung to `nf_open_book`'s `stage` and advance **one call at a time**.
6. Give it a **negative control** — an input that must fail — so a passing
   check is known not to be vacuous.

## Verification culture — this one is not optional

`make test` covers the pure logic; it cannot exercise a single libnickel call.
Every claim about what Nickel actually does rests on a device run, and device
runs are easy to misread.

- **`ndbCurrentView` (NickelDBus) is the oracle**, not the log. A log line only
  proves your call returned; the question is always whether Nickel *navigated*.
- **Detect a crash by watching Nickel's PID**, not by the absence of errors. A
  crash-and-restart leaves the view reading `HomePageView` and looks
  identical to a quiet success. `tools/nftest.sh` aborts on a PID change.
- **Stage everything.** Three unverified signatures behind one trigger meant
  the first crash accused all three at once. A crash at a known stage names one.
- **A negative control is what makes a check non-vacuous.** `isValid=false` for
  a ContentID no book has is the evidence that `isValid=true` means something.
- **A screenshot is the only way to know the RIGHT book opened.**
  `python3 tools/kobo.py shot` — `ReadingView` alone does not say which book.
  Note that sysfs `virtual_size` is padded (1280x1792) against a visible
  1264x1680 panel, so an uncropped grab has garbage at the edges.
- **Do not filter the crash log to lines mentioning your own mod.** The first
  run's `logread | grep nfolders` showed two frames and hid the rest of the
  hindenburg backtrace, which was then lost to the reboot. Grep
  `hindenburg|segfault|SIGSEGV` separately and unfiltered.

## Layout

```
nfolders.cc          NickelHook glue and the two trigger protocols
                     (/tmp/nfolders-open, /tmp/nfolders-show) that drive
                     nfnickel.cc/nfbrowser.cc from a shell over ssh, with no
                     UI of its own. `stage` (1-4) on the open trigger stops
                     after each libnickel call so a crash localises.
nfnickel.h/.cc        the mod's ENTIRE libnickel call surface, in one place
                     on purpose (see "Nickel's classes stay opaque"):
                     book-opening (rung 1) and the Volume -> data-source
                     chain rung 2's screen needs, plus the inotify watch
                     machinery that replaced the original poll thread.
nfbrowser.h/.cc       constructs the Volume data source and pushes
                     ArticleListLibraryController (rung 2's borrowed screen)
                     via MainWindowController::push. No fabricated vtable,
                     no fabricated RTTI, no tap hook -- see NOTES.md.
nffmt.h/.cc           PURE display/ordering logic (natural sort, book-
                     extension matching, common-prefix label stripping):
                     no libnickel, no NickelHook, no I/O, so it is the one
                     part of this project `make test` can actually run.
nflist.h/.cc          PURE listing pipeline (list -> hide junk -> fetch
                     metadata via an injected callback -> group -> order ->
                     label), also host-testable for the same reason.
tests/                the host test suite: test_nffmt.cc (63 checks),
                     test_nflist.cc (27 checks), nftest.h (tiny
                     dependency-free check macros -- a real framework would
                     tempt stdlib into the pure sources it is testing).
                     `make test` builds and runs both against the host's
                     own compiler and Qt5, never against libnickel.
NOTES.md             the reverse-engineering record: signatures, sizes, the
                     disassembly that establishes each one, the wrong turns
                     (crashed Nickel, a botched back-gesture baseline, two
                     disassembly-reading traps), and the measured Results
                     for both rung 1 (opening a book) and rung 2 (a screen
                     on the window stack).
DEVICE.local.md      where the device credentials live (not in git)
Makefile             NickelHook's cross build, plus a separate host test
                     build for tests/ -- see "Build" above for why the two
                     are deliberately Qt-version-skewed rather than pinned.
nickeltc             docker wrapper for the NickelTC toolchain
tools/kobo.py        ssh/push/pull/screenshot/trigger/reboot/wait. Drives ssh
                     through a pty because this host has no sshpass, no
                     expect and no paramiko. push/pull/reboot fail loudly
                     (non-zero exit) on a failed scp/ssh; wait requires low
                     uptime, not just a `nickel` pid -- see "Device workflow"
                     above for what each of those used to get away with.
tools/plt.sh         PLT stub -> symbol. See "Method" above.
tools/nftest.sh      the staged device driver, with PID-change abort
NickelHook/          submodule
libnickel.so.1.0.0   GITIGNORED, 24 MB of Kobo's proprietary binary, staged
                     locally so the archaeology needs no re-download.
                     `tools/kobo.py pull` fetches it. NEVER commit it.
```

## What comes next

The native-screen-vs-FBInk-overlay fork below is **resolved**: rung 2 proved
the native screen (touch, e-ink refresh, fonts and back all came free, exactly
as predicted) — but only as a **flat list of `Volume`s we chose by hand**, not
a folder tree. The fork's answer stands for the record; what is actually open
now is the folder half specifically.

- **A native Nickel screen**, pushed onto the window stack with
  `MainWindowController::push` — **done**, rung 2, `nfbrowser.cc`. Nickel's own
  `ArticleListLibraryController` renders every row; this mod supplies which
  `Volume`s go in and reacts to nothing else.
- **An FBInk overlay** drawn by a separate process, reusing koboy's existing
  folder-browsing list widget — not pursued, and rung 2's result makes it even
  less attractive than when this was written: the handoff problem it still
  has is now solved a cheaper way by the native route.

**The folder tree itself is the open problem**, not a fork to choose between
anymore. `NOTES.md` ("A rejected candidate worth recording") has one candidate
already ruled out (`NotebookGridController` — real folder navigation, but its
`moveToPath` mutates a singleton Nickel's own My Notebooks view reads from).
Two more are named but unmeasured: synthetic `folder://` `Volume`s, or a
compiler-generated shim now that rung 2 has measured the exact shape
(`InMemoryDataProvider`/`LinearLibraryDataSource`/controller) such a shim would
need to fit.

And the **zero-C++ fallback**, still not tried: write one Nickel collection
per folder into the `Shelf`/`ShelfContent` tables from the paths already in
`content`, then use the stock Collections view. Flat rather than a tree, and
it needs re-running when books are added, but it needs no injection and
survives firmware updates. Calibre creates collections from a *column*, not
from folders, so this is a script to write rather than an existing feature.

## Related: koboy

[`../koboy`](../koboy) is the same owner's other project on the same device — a
retro emulator front-end that takes the panel over from Nickel entirely. It is
a **different architecture** (a standalone C99/FBInk binary, no C++, no Qt, no
injection) and a **different toolchain** (Linaro 4.9-2014.09), so its code is
not a dependency here. What is worth reading across:

- `../koboy/src/romlist.c` and `../koboy/src/ui.c` — an existing
  folder-navigating list widget for this panel, if the FBInk fork is ever taken.
- `../koboy/CLAUDE.md` and `../koboy/TESTED.md` — the measured e-ink knowledge
  (waveforms, refresh cost, dirty rectangles, why four grey levels). Anything
  that draws on this panel will need it.
- `../koboy/docs/kobo-touch-protocols.md` — the four dialects a Kobo
  touchscreen speaks, if input is ever read directly rather than through Qt.
- Its ARM binutils, at
  `~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin`, are what the
  `objdump`/`nm` commands above were run with.

## Conventions

Comments record **why**, not what, and especially why a non-obvious choice is
not an oversight. Anything measured off the device carries the measurement.
Clamps, guards and deliberate leaks carry a note saying they are deliberate so
nobody deletes them as dead code. Match that voice.

Never commit `libnickel.so.1.0.0`, `device.local`, or a book.
