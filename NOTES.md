# Reverse-engineering notes

**Result: it works.** Measured on the device 2026-09-03 — see Results at the
bottom. The rest of this file is how the call sequence was derived, including
one wrong turn that is left in because the mistake is the instructive part.

Everything here was read off a real device's own `libnickel.so.1.0.0`, not
inferred from documentation. **Firmware 4.38.23684**, Kobo Libra 2 (Mark 9),
23,966,636 bytes, 70,755 exported symbols.

Addresses are for that exact build and are recorded only so the derivations can
be checked; the mod resolves everything by name at runtime and never uses an
address.

## What already exists, and does not do this

Checked first, so nobody re-treads it:

- **NickelDBus** (`libndb.so` + `/usr/bin/qndb`) exposes **64** methods.
  `qndb -a` lists them; the only book-adjacent ones are `pfmRescanBooks` and
  `pfmRescanBooksFull`. There is no method that opens a book. (The published
  docs at `shermp.github.io/NickelDBus` agree.)
- **NickelMenu**'s `nickel_open` action takes *views* only — `library:all`,
  `library:shelves`, `discover:storefront` and so on. Never a book.

So opening a specific book from outside Nickel's own UI needs an injected mod.
There is no script-level route.

## The call sequence

This is the path the library's own **Read** button takes, which is the reason to
prefer it over driving `ReadingController` directly: Nickel's bookkeeping
(reading session, bookmark restore, analytics, download-if-needed) happens by
itself instead of being reimplemented.

| Symbol | Meaning |
|---|---|
| `_ZN13VolumeManager7getByIdERK7QStringS2_` | `VolumeManager::getById(QString const&, QString const&)` — **static**, see below |
| `_ZNK6Volume7isValidEv` | `Volume::isValid() const` |
| `_ZN6VolumeD1Ev` | `Volume::~Volume()` |
| `_ZN19ReadBookActionProxyC1EP7QObjectRK6Volume` | `ReadBookActionProxy::ReadBookActionProxy(QObject*, Volume const&)` |
| `_ZN19ReadBookActionProxy10onSelectedEv` | `ReadBookActionProxy::onSelected()` |

`ActionProxyMixin::readBookProxy(QObject*, Volume const&)`
(`_ZN16ActionProxyMixin13readBookProxyEP7QObjectRK6Volume`) is Nickel's own
wrapper around the construction, and is where the sizes below came from. The mod
does not call it, because it also caches the proxy into the mixin at `+36`/`+40`
and a mixin is more state to fake than a proxy.

## getById has NEITHER implicit argument it appears to have

This is the trap, and it cost a crashed Nickel and a reboot before the second
half of it was found.

**It returns by value.** A `Volume` is not trivially returnable in registers,
so it comes back through a hidden buffer, and on the Itanium/ARM C++ ABI that
buffer is argument **zero** — the opposite of the Microsoft convention.

**And it is a `static` member function.** The Itanium mangling for a static
member is *identical* to a non-static one; `_ZN13VolumeManager7getByIdERK7QStringS2_`
cannot tell you which it is. There is no `this`.

So the real signature is:

```c
Volume *getById(void *ret, QString const *id, QString const *dbName);
//              ^^^ r0     ^^^ r1             ^^^ r2          r3 unused
```

### How the first half was established

From the prologue and epilogue at `0x00a6c4a0`:

```
a6c4ac: mov   r9, r0          ; r9 = incoming r0, saved untouched
...
a6c53c: str.w r3, [r9, #4]    ; writes through it ...
a6c542: str.w r2, [r9]        ; ... 8 bytes
a6c560: mov   r0, r9          ; and returns that same pointer
```

Writing the result through incoming `r0` and returning it is what a
memory-returning function does.

### How the second half was established, after getting it wrong

Reading `r1` as `this` compiles, links, resolves, and **crashes Nickel** the
first time it is called. What settles it is resolving the PLT stub called
fourteen bytes in:

```
a6c4b0: mov r0, r8            ; r0 = a local
a6c4b4: blx 675714            ; <- what is this?
```

`0x675714` is a PLT stub; its GOT slot relocates to
**`_ZN8QVariantC1ERK7QString`** — `QVariant::QVariant(QString const&)`, called
with incoming `r1` as its argument. A `QString const&` cannot be a
`VolumeManager*`. Passing the singleton there made Nickel construct a QVariant
out of a pointer to itself.

Resolving three more stubs gives the whole function, and with it what `dbName`
is for:

```
getById(sret, QString const& id, QString const& dbName)
    QVariant(id)                              _ZN8QVariantC1ERK7QString
    Repository::cacheKey(dbName, ...)         _ZN10Repository8cacheKeyERK7QStringS2_
    Repository::refreshCache<Volume>(dbName)  _ZN10Repository12refreshCacheI6VolumeE...
    Volume::cacheKey(QVariant)                _ZN6Volume8cacheKeyERK8QVariant
    QHash<QByteArray,Volume>::findNode(key)   _ZNK5QHashI10QByteArray6VolumeE8findNodeE...
```

It is a static lookup in a `Repository` cache, and `dbName` selects the cache
partition. **Empty is correct** for local content — measured, see Results.

This also explains why `VolumeManager::sharedInstance()` exists separately and
is *not* wanted here: it is the signal hub (`volumeChanged`, `volumeRemoved`),
not the lookup path. The mod does not resolve it at all.

### Resolving a PLT stub

Worth writing down, because it is what turns a guess into a fact. An ARM PLT
stub is three instructions; add its two immediates and the `ldr` offset to
`pc+8`, then match the result against the relocation table:

```sh
arm-linux-gnueabihf-objdump -d --start-address=0x675714 --stop-address=0x675720 libnickel.so.1.0.0
# ip = (0x675714+8) + 0x1000000 + 0x36000 + 0xf44 = 0x16ac660
arm-linux-gnueabihf-objdump -R libnickel.so.1.0.0 | grep 16ac660
# 016ac660 R_ARM_JUMP_SLOT   _ZN8QVariantC1ERK7QString
```

`plt.sh` in the scratch notes automates it. **Do this for every unexplained
call in a function whose signature you are inferring** — it is the difference
between reading argument registers and knowing what they hold.

## Object sizes

Both are over-allocated in the mod. These are the measured values, and the
headroom is what absorbs a firmware that grows either object — neither
constructor can be told how much room it has.

**`sizeof(Volume) == 8`.** From `Volume::Volume()` at `0x00a63cd4`:

```
a63ce6: mov.w r0, #408        ; operator new(408) -- the shared block
a63cee: str   r3, [r5, #0]    ; vptr
a63cfa: str   r4, [r5, #4]    ; d-pointer to the 408 bytes
a63cfc: ldrex/strex loop      ; refcount++ on it
```

A vptr plus a refcounted pointer, which agrees with `getById` writing exactly 8
bytes. Copying a `Volume` is therefore cheap, and the proxy's own copy keeps the
shared block alive.

**`sizeof(ReadBookActionProxy) == 52`.** Straight out of Nickel's own
allocation for it, inside `readBookProxy` at `0x00c324b4`:

```
c324b4: movs r0, #52          ; 0x34
c324ba: blx  operator new
c324be: ldr  r2, [r7, #0]     ; Volume const&
c324c2: ldr  r1, [r7, #4]     ; QObject *parent
c324c4: blx  ReadBookActionProxy::ReadBookActionProxy
```

Confirming the constructor is `(this=r0, parent=r1, volume=r2)`, and that the
`parent` argument is passed straight through from the caller — so a `QObject`
of ours works as the parent and Qt owns the proxy afterwards.

`onSelected()` (`0x00c8bacc`) reads the proxy's own `Volume` at `+12` and
otherwise touches only globals; there is no visible dependence on a parent
widget. It allocates a 28-byte worker on one path, which is why the spike does
**not** free the proxy — see the comment in `nfolders.cc`.

## Re-deriving this on a new firmware

The mod resolves by name, so most firmware changes need nothing. If a symbol
stops resolving, NickelHook logs it and disarms rather than crashing. To redo
the measurements:

```sh
# on the device
strings -n 8 /usr/local/Kobo/libnickel.so.1.0.0 | grep '^_Z' | sort -u > /tmp/syms.txt
grep -E '19ReadBookActionProxy|13VolumeManager7getById|6VolumeC1|6Volume7isValid' /tmp/syms.txt

# on the host, against a copy of libnickel
arm-linux-gnueabihf-nm -D --defined-only libnickel.so.1.0.0 | grep <symbol>
arm-linux-gnueabihf-objdump -d --start-address=0x<addr> --stop-address=0x<addr+0x60> libnickel.so.1.0.0
```

The two things worth re-checking on a firmware bump are the **argument order of
`getById`** (does it still write through incoming `r0` and return it?) and the
**`movs r0, #52`** in `readBookProxy`.

## Other findings, for the browser that comes next

- **The folder structure is already in the database.** `content.ContentID` is
  `file:///mnt/onboard/<relative path>` verbatim — Nickel imports recursively
  and keeps the whole path, it just will not show it as a tree. So a browser
  needs no import step and no filesystem walk; the tree is derivable from one
  query. Confirmed against real rows on the device, e.g.
  `file:///mnt/onboard/audiobooks/Portuguese Foundation/Booklet_Portuguese_foundation.pdf`.
- **Nickel already contains a folder browser**, used for notebooks and the
  Dropbox/Drive views: `RootFolder`, `BrowseableFolder`, `FolderItem`,
  `FolderItemManager`, `FolderItemDataSource`, `FolderItemDataProvider`,
  `FolderItemListWidget`, `FolderItemMenuController`,
  `NotebookGridView::folderItemTapped(QString const&, QString const&)`, and
  `folder://` among its URL schemes (with `epub://`, `kobo://`,
  `audiobook://`). Reusing it would look perfectly native at the cost of much
  deeper archaeology than the open call needed.
- **`LibraryNavMixin::pushShelf(QString const&)`** exists
  (`_ZN15LibraryNavMixin9pushShelfERK7QString`), so a mod can jump straight
  into a named collection. That is the hook a shelves-per-folder approach would
  want.
- `MainWindowController::sharedInstance()` / `::push(AbstractController*, bool)`
  are exported, which is what would let a browser be a real Nickel screen on
  the window stack rather than an FBInk overlay.

## Results

Measured on the reference device 2026-09-03, firmware 4.38.23684, with Nickel
up and driven over ssh. The probe advances one libnickel call at a time
(`stage` in the trigger file) and aborts if nickel's PID changes; it did not
change at any point, i.e. **Nickel survived every stage**.

| Question | Answer |
|---|---|
| Does `getById` find a **sideloaded** book by ContentID? | **Yes**, with `dbName` empty. `isValid=true` for a real book, `false` for a ContentID no book has — so it discriminates, and the negative control proves the check is not vacuous. |
| Does the proxy constructor accept an arbitrary `QObject` parent? | **Yes.** Constructed against the mod's own trigger object. |
| Does `onSelected()` actually navigate? | **Yes.** `ndbCurrentView` went `HomePageView` → `ReadingView`, and a framebuffer grab shows the right book open in the stock reader with Nickel's own header and chapter footer. Took ~4 s. |
| Does Nickel's own bookkeeping run? | **Yes**, and this is the payoff for choosing `ReadBookActionProxy` over `ReadingController`: the book appeared in Nickel's **Recents** afterwards. A direct `ReadingController` push would have rendered the book with no reading session and no Recents entry. |
| Where does **back** go? | **It POPS to the view beneath.** Measured 2026-09-03 with a deliberately non-Home baseline: `DragonLibraryView` → (open) → `ReadingView` → (owner presses back) → `DragonLibraryView`, nickel PID unchanged throughout. Both the owner's report and `ndbCurrentView` agree. So a browser view of ours on the window stack **will** be returned to — the browser can be a screen, not a one-shot launcher. |

The test used `file:///mnt/onboard/books/Pratchett_ Terry - The Color of Magic_ A Discworld Novel.epub`
— a plain sideloaded `.epub` one directory down, chosen because it is exactly
the case the stock library will not let you navigate to.

`ndbCurrentView` (from the already-installed NickelDBus) is worth knowing about
as an oracle: a log line only proves a call returned, and the question here was
always whether Nickel *navigated*.

### The back-gesture measurement was wrong once, and the reason generalises

It was first "answered" by a run that did `qndb -m mwcHome` before triggering,
on the reasoning that back needed a *known* destination. That made Home the
view beneath the reader — and "pops to the view beneath" and "always goes Home"
then predict the same observation, so landing on Home discriminated nothing. It
was recorded as confirmed anyway, and the owner's own doubt is what caught it.

**Setting a baseline to the value you expect to measure destroys the
measurement.** Re-run from `DragonLibraryView` it took one attempt.

Note also that NickelDBus cannot navigate to the library on its own — its only
view-changing methods are `mwcHome` and `bwmOpenBrowser` — so a non-Home
baseline needs either a tap from the owner or the web browser view standing in.

### Formats, and whether the library is fully imported (#2, #5, #4)

Measured 2026-09-03 by censusing **every one of the 227 book files** on the card
through the mod's own `getById` + `isValid` at stage 2 — no navigation, and
authoritative because it is Nickel's own lookup rather than a guess about the
schema.

| | |
|---|---|
| Files with a row `getById` finds | **226 of 227** |
| The one exception | `Fullmetal Alchemist v26 …cbz` at **exactly 8,388,608 bytes** (8 MiB) against 99 MB and 119 MB for v25 and v27 — a **truncated copy**. It still has the `PK\003\004` zip header, so it looks like a file, and Nickel's import rejected it. Not a Nickel limitation. |
| Formats present | 122 `.cbz`, 94 `.cbr`, 6 `.pdf`, 4 `.epub` (2 of them `.kepub.epub`), 1 `.txt` — **all with rows** |
| Formats that open | `.kepub.epub`, `.epub`, `.cbz`, `.cbr`, `.pdf`, `.txt` all navigate `HomePageView` → `ReadingView`, nickel PID unchanged |
| Rendering | Confirmed by framebuffer grab for `.epub`, `.cbr` and `.pdf`. **`.cbr` works** — Sandman #50 rendered, rotated to landscape by Nickel's comic reader — worth knowing because CBR is not on Kobo's official supported-format list. The `.pdf` rendered Cyrillic body text cleanly. |
| Reading position | The PDF opened at *Об авторах*, near the END of the book rather than page 1 — the bookmark was restored. Third independent sign that `ReadBookActionProxy` runs Nickel's real bookkeeping (the others being the Recents entry and the reading session). |
| Non-ASCII and long paths | **A non-issue.** Accented (`Pokémon …`), Cyrillic, parenthesised and 230-character paths all resolve. This is most of #4. |
| KEPUB ContentID shape | The plain volume path is what `getById` wants. The worry that a KEPUB needs its chapter-suffixed ContentID was unfounded. |

**What this means for the browser:** the folder tree really is derivable from
the database alone, and the assumption that a listed file can be opened holds
for 226 of 227. But it does not hold universally, and the exception is the
mundane kind — an interrupted copy — so **the browser must handle "file on disk
with no row" as a normal case**, not an assertion. Greying the row out with the
reason is better than hiding it, since a silently missing book is how you spend
an evening wondering where volume 26 went.

### A second instrument mistake, and the constraint behind it

The census first reported **15 files as `STALE`** — no result associable with
the trigger. The pattern gave it away: the 13 longest paths and the 2 Cyrillic
ones. `nh_log` **truncates at 256 bytes** (NickelHook.h says so plainly), so
the `contentId` log line was cut, and the census proved association by matching
the file's *full* path against the log. Re-run matching a 60-byte **prefix**
instead, all 15 came back `VALID`.

So: **do not build a measurement on log lines carrying untruncated data.** That
is now the second time in this project an instrument produced a confident wrong
answer — after the back-gesture baseline — and both times the tell was that the
failures were *patterned* rather than random. A patterned failure is almost
always the instrument.

### Open questions — what a device run still has to answer

These are cheap now: the mod is installed and `tools/kobo.py open` drives it
without a rebuild. Numbered so they can be referred to; #1 was answered on
2026-09-03 and is in Results.

3. **Repeated and consecutive opens.** Opening a second book without an
   intervening back, and opening the same book twice, are both untested. The
   proxy is currently leaked per open, so this is also where that shows up.

Answered 2026-09-03: **#1** (back pops, in Results), **#2** and **#5** (below),
**#4** as a side effect of #5's census, and **#6** by static analysis (below) —
the reference device has no SD slot, so that one can never be closed on
hardware here.

Not device questions, but owed before this is a product:

- The 52-byte-per-open proxy leak is a deliberate probe shortcut.
- The 500 ms poll thread is a probe mechanism; a real mod is driven by a menu
  item or a view, not by a file in `/tmp`.

## dbName, and SD cards (#6)

The reference Libra 2 **has no microSD slot**, so this cannot be tested here at
all. But it matters for the models that do — the Kobo Aura H2O in koboy's
device matrix is one — so it was answered by reading libnickel instead.

**`dbName` is not always empty, and a browser that hardcodes `""` will silently
fail to find SD content.**

`Device::calcDbName() const` (`_ZNK6Device10calcDbNameEv`, `0x009686c0`) builds
the literal string **`/mnt/onboard/.kobo`** with `QString::fromUtf8_helper`,
compares it against the device's own storage path with
`operator==(QString const&, QString const&)`, and:

- **equal** — internal storage — returns the empty QString. This is why `""`
  measured correct on this device, and it is correct there for a *reason*, not
  by luck.
- **not equal** — external SD — derives a name through `QDir::cleanPath`.

The cached accessor is `Device::getDbName() const`
(`_ZNK6Device9getDbNameEv`, `0x009688f4`), and it is trivial — three
instructions:

```
ldr r0, [r0, #4]    ; r0 = this->d
adds r0, #48        ; r0 = &d->dbName
bx lr
```

so it returns a `QString const&` — a pointer to the already-cached string.
Nothing to allocate, nothing to destroy. The Device comes from
`Device::getCurrentDevice()` (`_ZN6Device16getCurrentDeviceEv`,
`0x0096ac74`), which is a tail-call to
`Device::getCurrentDeviceMutable()` and returns a `Device*` in `r0`.

### So the correct call, on every model

```c
static void          *(*Device__getCurrentDevice)(void);
static QString const *(*Device__getDbName)(void const *_this);

void          *dev = Device__getCurrentDevice();
QString const *db  = Device__getDbName(dev);   // "" internal, non-empty on SD
VolumeManager__getById(volbuf, &contentId, db);
```

This is also what Nickel itself does: all **117** call sites of
`VolumeManager::getById` pass `dbName` in a register from an earlier
computation — never a constant — and the one whose signature names its source
is `VolumeManager::getUserGuide(Device const&)`.

### What is still not established

Whether `getCurrentDevice()` returns the right Device *for a book that lives on
the SD card*. `calcDbName` comparing a per-Device path against
`/mnt/onboard/.kobo` implies a Device can represent either store, and there are
`Device::isSDCardInserted()`, `Device::externalSDMountPoint()` and
`Device::sdSerial()` alongside it — but nothing here can test which instance a
lookup needs. Using `getDbName()` is strictly better than hardcoding `""` and
is what Nickel does; the SD path itself needs a device with a slot.

### How the call sites were found

Worth recording, because it is the reverse of `tools/plt.sh` and it took a
detour. Calls to a global function inside libnickel go **through the PLT**, so
grepping the disassembly for the function's own address finds nothing — which
is why an early attempt at this reported zero callers and was wrong.

```sh
# 1. the function's GOT slot
arm-linux-gnueabihf-objdump -R libnickel.so.1.0.0 | grep 7getByIdERK7QStringS2_
#    016b999c R_ARM_JUMP_SLOT _ZN13VolumeManager7getByIdERK7QStringS2_
# 2. scan .plt for the stub whose (addr+8+a+b+c) equals that slot  -> 0x69f7f4
# 3. grep the full disassembly for calls to the stub
arm-linux-gnueabihf-objdump -d libnickel.so.1.0.0 | grep -E 'blx?\s+69f7f4'
```

Step 3 produces a ~500 MB text file from this 24 MB binary. Delete it after.
