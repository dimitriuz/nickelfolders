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

**STATUS: the spike is answered and the browser does not exist yet.** What
lives here is a probe (`nfolders.cc`) plus the reverse-engineering record that
made it possible (`NOTES.md`). Read both before proposing anything.

## The one thing that was in doubt, and no longer is

Verified on hardware 2026-09-03 (Kobo Libra 2, firmware 4.38.23684):
`VolumeManager::getById` → `ReadBookActionProxy` → `onSelected()` opens an
arbitrary **sideloaded** book in the stock reader. `ndbCurrentView` went
`HomePageView` → `ReadingView`, a framebuffer grab showed the right book with
Nickel's own header and chapter footer, and Nickel's PID never changed. `dbName`
empty is correct.

Still unconfirmed: the **back gesture**. The reader is pushed *on top* of the
previous view rather than replacing the stack, so a browser view of ours should
be returned to — but nothing in this repo can press back, so it needs a finger.
That row decides whether the browser is a screen you return to or a one-shot
launcher.

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

**There is no host test suite and no `make test`.** Nothing here is meaningfully
testable off-device: every interesting call is a libnickel call. That is a real
weakness of this project compared to koboy, not an oversight, and it is why the
verification discipline below is not optional.

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
- **Nickel's UI may only be touched from the GUI thread.** The poller thread
  reaches it via `QCoreApplication::postEvent`, which is documented
  thread-safe; it must not call libnickel directly.
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
- **`dbName` is a `Repository` cache-partition key, and empty is correct** for
  local content. Measured, not assumed.
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
4. Get object sizes from Nickel's **own** `operator new` call sites, never from
   a guess about layout.
5. Add a rung to `nf_open_book`'s `stage` and advance **one call at a time**.
6. Give it a **negative control** — an input that must fail — so a passing
   check is known not to be vacuous.

## Verification culture — this one is not optional

Because there is no host test suite, every claim here rests on a device run,
and device runs are easy to misread.

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
nfolders.cc          the spike: watches /tmp/nfolders-open and runs the
                     library-tap sequence on the ContentID it finds. Draws
                     nothing, adds no menu item. `stage` (1-4) stops after
                     each libnickel call so a crash localises.
NOTES.md             the reverse-engineering record: signatures, sizes, the
                     disassembly that establishes each one, the wrong turn
                     that crashed Nickel, and the measured Results.
DEVICE.local.md      where the device credentials live (not in git)
Makefile             NickelHook's build; LIBRARY/SOURCES and little else
nickeltc             docker wrapper for the NickelTC toolchain
tools/kobo.py        ssh/push/pull/screenshot/trigger/reboot/wait. Drives ssh
                     through a pty because this host has no sshpass, no
                     expect and no paramiko.
tools/plt.sh         PLT stub -> symbol. See "Method" above.
tools/nftest.sh      the staged device driver, with PID-change abort
NickelHook/          submodule
libnickel.so.1.0.0   GITIGNORED, 24 MB of Kobo's proprietary binary, staged
                     locally so the archaeology needs no re-download.
                     `tools/kobo.py pull` fetches it. NEVER commit it.
```

## What comes next

The browser, and there is a real fork in it:

- **A native Nickel screen**, pushed onto the window stack with
  `MainWindowController::push`. Touch, e-ink refresh, fonts and the back
  gesture all come free, and Nickel's own `FolderItemListWidget` may be
  reusable. More archaeology, much the better result.
- **An FBInk overlay** drawn by a separate process. Reuses koboy's existing
  folder-browsing list widget, but the handoff *still* needs this mod — so it
  is the hard part plus a whole second UI. Probably not worth it.

And a **zero-C++ fallback worth trying first**, because it might end the
problem in an hour: write one Nickel collection per folder into the
`Shelf`/`ShelfContent` tables from the paths already in `content`, then use the
stock Collections view. Flat rather than a tree, and it needs re-running when
books are added, but it needs no injection and survives firmware updates.
Calibre creates collections from a *column*, not from folders, so this is a
script to write rather than an existing feature.

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
