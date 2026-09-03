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

### Repeated and consecutive opens (#3)

Measured 2026-09-03: **20 consecutive opens** alternating a `.epub` and a
`.cbr`, with no back press anywhere in between, then one back press.

| | |
|---|---|
| Completed | 20 of 20, nickel PID unchanged, no `hindenburg`/segfault/OOM chatter |
| `onSelected()` duration | under a second every time, no degradation across the run |
| RSS | 126,288 kB before → 133,628 kB after the *first* open → then **byte-identical 133,628 kB for all nine remaining iterations** |
| Which book is on screen | the **last** one opened, confirmed by framebuffer grab (Sandman, not the Pratchett) — so a second open genuinely switches books |
| Stack depth | **1.** One back press after 20 opens landed on `HomePageView`, confirmed by the owner and by `ndbCurrentView`. Readers are **replaced, not stacked**. |
| RSS after that back | 123,324 kB — *below* the pre-test baseline, so nothing accumulated and the reader's memory is released. (Not reading much into the small decrease itself; ordinary reclaim would do that.) |

The flat RSS predicted the stack-depth answer before the back press did: had
each open pushed a controller, memory would have climbed.

**For the browser this is about as good as it gets.** Open a book, read, one
back press, you are where you were — every time, no matter how many books were
opened in the session, with no reader lifetime to manage and no accumulating
state. The `browse → tap → read → back → same folder` loop needs no special
handling.

It also prices the probe's deliberate leak. The measured *object* is 52 bytes
(see "Object sizes" above), but the probe's own `::operator new(512)` call —
the same deliberate over-allocation `getById`'s output buffer uses, so the
constructor is never told it has too little room — is what is actually charged
per open, so the real cost is 512 bytes × 20 opens, ~10 kB, and it still did
not register against RSS. Still worth fixing before this is a product — a mod
lives for weeks between reboots — but it is not a correctness problem at this
scale. See "The proxy leak, revisited" below for why it is still unpaid as of
Task 6.

### Open questions

**None** of #1–#6 remain open, as of 2026-09-03. Two of the three items below
were paid in Task 6 (2026-09-03, rung 1: `nfnickel.cc`); the proxy leak was
not, on purpose, and the paragraph after this list explains why.

Not device questions, but owed before this is a product:

- ~~The 500 ms poll thread is a probe mechanism~~ — replaced by an inotify
  watch through a `QSocketNotifier` in Task 6.
- ~~`dbName` must come from `Device::getDbName()` rather than being hardcoded
  empty~~ — done in Task 6; see "dbName, and SD cards (#6)" below, which this
  no longer contradicts.
- **The proxy leak is still unpaid.** Task 6 parents every
  `ReadBookActionProxy` to a `QObject` so Qt *could* own it, but that parent is
  itself never destroyed (a permanent, process-lifetime placeholder), so
  nothing is actually freed — the footprint is unchanged from the spike, at
  **512 bytes/open** (the allocation request above, not the 52-byte object).
  See "The proxy leak, revisited" below for what would need to be established
  before this can be paid.

### The proxy leak, revisited (#3, still open)

Task 6 (2026-09-03) tried to settle whether `onSelected()` frees or
self-deletes the proxy on some path, so that the leak could be closed rather
than merely deferred to an immortal parent. It could not: `tools/plt.sh`
could not resolve `onSelected`'s call targets in the time available, so
whether the object handed to the 28-byte worker mentioned above (in "Object
sizes") is `this` — which would mean the worker, not our caller, ends up
responsible for the proxy's lifetime — is still unknown.

**What would settle it**, the next time this is picked up:

1. Disassemble `ReadBookActionProxy::onSelected()` at `0x00c8bacc` in full —
   the earlier pass only established that it "reads the proxy's own `Volume`
   at `+12` and otherwise touches only globals" and allocates a 28-byte
   worker on one path; it did not follow what happens to `this` afterward.
2. Resolve **every** PLT stub it calls, per the standing method (`tools/plt.sh`,
   `CLAUDE.md` "Method: adding a new libnickel call") — this is exactly the
   step that caught `getById`'s missing `this` the first time, and skipping it
   here would be the same mistake in a new place.
3. Specifically determine whether the 28-byte worker is handed `this` (the
   proxy) and takes ownership of it (e.g. deletes it when the worker itself
   finishes), or whether the proxy is expected to outlive `onSelected()`
   entirely and be freed by its caller. Nickel's own call site
   (`ActionProxyMixin::readBookProxy`, see "Object sizes") is worth
   re-examining for what it does with the proxy pointer *after* calling
   `onSelected()` on it, if anything — that is the real caller this mod is
   standing in for.

Do not ship a `deleteLater()` (or a direct `delete`) on the proxy based on a
guess about the answer. `VolumeManager::getById`'s missing `this` is the
standing example of what a wrong guess about a Nickel object's ownership
costs here: it crashed Nickel on the first device run, from code that also
compiled, linked and resolved cleanly. 512 bytes/open (~300 kB/month at twenty
opens a day) is a real but small cost; a wrong guess about who owns a live
`QObject` inside Nickel's own UI is not a comparable risk to take to close it
before the archaeology above is done.

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

### The measured value contradicts the derivation above, and this is left in on purpose

Measured on hardware 2026-09-03 (Task 6, rung 1, firmware 4.38.23684):
`Device::getDbName()` did **not** return the empty string on internal
storage. It returned **`/mnt/onboard/.kobo/KoboReader.sqlite`**:

```
open: stage=2 contentId='file:///mnt/onboard/books/Pratchett_ Terry - The Color of Magic_ A Discworld Novel.epub' dbName='/mnt/onboard/.kobo/KoboReader.sqlite'
open: isValid=true
```

This **contradicts** the `calcDbName()` reading two sections above: that
reading says the literal-string comparison against `/mnt/onboard/.kobo`
should return *empty* for internal storage, and the spike's own hardcoded
`""` was justified on exactly that basis (and worked, on the same device).
The reading is not being quietly softened into agreement here — it was wrong
about what the accessor returns, and this project's notes keep wrong turns
on purpose (see `getById`'s missing `this`, above) rather than editing them
into looking right in hindsight.

**What is now open, that was not before**: `""` and
`/mnt/onboard/.kobo/KoboReader.sqlite` are different strings, yet `getById`
found the book (`isValid=true`) with the runtime-measured
`/mnt/onboard/.kobo/KoboReader.sqlite` value, and separately, historically,
with a hardcoded `""` in the spike. Both work; why is unexplained. Three
candidate explanations, not yet distinguished:

1. `Repository::cacheKey(dbName, ...)` (called from inside `getById`, per
   "The call sequence" above) normalises both strings to the same cache
   partition — e.g. by treating `/mnt/onboard/.kobo/<anything>` as
   equivalent to empty for the internal-storage case, or by deriving the
   partition key from something other than a literal string match.
2. The lookup has a fallback path that finds the book regardless of which
   partition is queried first (a miss in one cache falling through to
   another).
3. `Device::getDbName()` is not actually returning `calcDbName()`'s result
   verbatim — the field it reads (`this->d`, `+48`, per the disassembly
   above) may be a *database file path* the Device caches for a different
   purpose than the `dbName` cache-partition key `calcDbName` computes, and
   the two only happen to coincide in the cases measured so far.

**Archaeology that would settle it**, not yet done: re-read
`Device::calcDbName()` at `0x009686c0` and `Device::getDbName()` at
`0x009688f4` against the *current* firmware build (the addresses above are
from the same 4.38.23684 image the rung-1 device run used, so a re-read is a
sanity check on the reading, not a re-target), and resolve
`Repository::cacheKey`'s PLT stubs with `tools/plt.sh` to see what it
actually does with the two different strings measured. **This does not block
v1**: `getDbName()`'s value works, is what Nickel's own 117 call sites
compute rather than a constant, and is strictly better than a hardcoded `""`
regardless of which explanation above turns out to be right.

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

## Rung 1 device results (Task 6, 2026-09-03)

Verified on hardware, firmware 4.38.23684, Nickel PID 222 **unchanged
throughout the whole run** — no crash, no restart, at any point below.

- **No boot loop.** The `nf_watch_name` fix (a file-scope `QByteArray`'s
  dynamic initialiser racing `nh_init` — see `nfnickel.cc`'s comment on the
  declaration for the mechanism) is confirmed on hardware, not just by the
  `readelf` check against `.init_array` that predicted it.
- **All seven `NFNickelDlsym` symbols resolved**, each logged `(optional)`.
  The failsafe armed and was destroyed after the configured 3s delay, and
  the `.so` was renamed back to its installed location — the owner's other
  NickelHook mods (NickelMenu, NickelDBus, kfmon) were never at risk on this
  run, which is the outcome marking every entry `.optional = true` exists
  for.
- **The inotify trigger fires.** `nf_watch_init`'s `QSocketNotifier` path
  works end to end: writing the trigger file drove `nf_on_trigger` and
  `nf_open_book_staged` on this firmware.
- **Negative control passed**: a ContentID no book has gave `isValid=false`
  and "no book in the library has that ContentID" logged, with no
  navigation — the evidence that `isValid=true` (below) means something
  rather than being vacuously true.
- **Full open (stage 4) reached `ndbCurrentView = ReadingView`**, and a
  framebuffer grab showed the right book with Nickel's own header and
  chapter footer — the stock reader, not a bare render. `onSelected()`
  returned in ~4s.
- **Zero crash lines**: unfiltered `logread | grep -icE
  "hindenburg|segfault|SIGSEGV"` = 0 across the run.
- **The `QCoreApplication`/dispatcher init-ordering question (Task 6's own
  concern 1) is answered**: `nf_watch_init`'s two instrumentation lines
  (`nfnickel.cc`) log only on failure, and neither appeared; `init: ready`
  itself requires `dispatcherLive == true` to be reached at all (see
  `nf_watch_init`'s return value). So on this firmware, `nf_init` runs
  *after* `QCoreApplication` exists and the thread's event dispatcher is
  already live — genuinely unestablished before this instrumentation, and
  now settled for 4.38.23684 specifically (not necessarily every firmware).
- **The poll thread's removal is a source fact, not a device measurement.**
  `nfnickel.cc`/`nfolders.cc` contain no `pthread_create` and no
  `nanosleep` calls at all — grep confirms it — and the trigger firing
  above demonstrates the inotify replacement works. A `/proc/<pid>/task`
  thread count was taken before and after this run (12 threads on a Nickel
  that had been up four hours with browser use as the baseline, 9 threads
  post-reboot) but that is **not** a controlled comparison across a reboot
  — Nickel's own thread count varies with what it has done since boot, for
  reasons unrelated to this mod — and is not cited here as evidence for
  anything.

**The dbName measurement from this same run contradicts a written derivation
elsewhere in this file** — see "The measured value contradicts the
derivation above, and this is left in on purpose", under "dbName, and SD
cards (#6)".

## Task 7, rung 2: a screen on the window stack

All addresses below are firmware 4.38.23684, the same image every other
measurement in this file was taken against. Read `libnickel.so.1.0.0` with

```sh
NM=~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin/arm-linux-gnueabihf-nm
OD=~/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin/arm-linux-gnueabihf-objdump
```

and `tools/plt.sh` (`OBJDUMP=$OD sh tools/plt.sh libnickel.so.1.0.0 0x<stub> ...`)
for every PLT resolution quoted here.

### `sizeof(AbstractController)` == 12 bytes — three independent readings

`AbstractController::AbstractController()` (`_ZN18AbstractControllerC1Ev` and
`C2Ev` alias to the same address, `0xad1334` — confirmed with
`nm -D --defined-only`, both destructors alias the same way at `0xad1358`)
is three instructions of substance:

```
ad1340: ldr r3, [r3, r1]   ; r3 = resolved address of _ZTV18AbstractController (via GOT)
ad1342: str r4, [r0, #4]   ; this[+4] = 0
ad1344: str r4, [r0, #8]   ; this[+8] = 0
ad1346: adds r3, #8        ; r3 = vtable_addr + 8 -- the Itanium "address point",
                            ;      skipping the offset-to-top and RTTI header words
ad1348: str r3, [r0, #0]   ; this[+0] = r3  (the vptr)
```

**No `bl`/`blx` at all** — the constructor calls nothing. That is the entire
answer to "resolve every PLT stub in `AbstractController::AbstractController`":
there are none to resolve. This is also why it is safe to call through a
resolved pointer to build our own object with it — its only effect is these
three word-writes.

So the constructor's own writes put a **lower bound** of 12 bytes (offsets 0,
4, 8, each a word) — "at least 12 bytes" per the task brief, not yet a size.
The upper bound came from two independent derived classes, each of which
places its `AbstractController` base subobject at **+8** (right after an
8-byte `QObject` base — confirmed by the first call in each constructor being
`QObject::QObject(QObject*)`, per `tools/plt.sh` on the stub each calls) and
then begins writing **its own** next field at **+20**:

```
# PasswordController::PasswordController(), 0x1056348
105635c: blx 6a3708        ; AbstractController::ctor(this = r4+8)   -- 6a3708 -> _ZN18AbstractControllerC2Ev
1056372: str r0, [r4, #8]  ; overwrite the AbstractController-subobject vptr with
1056374: str r6, [r4, #0]  ;   PasswordController's own combined vtable (standard:
                            ;   base ctor sets the base vtable, derived ctor
                            ;   immediately re-sets it — see "Two vtable writes" below)
1056378: str r3, [r4, #20] ; PasswordController's OWN first field, at +20

# HelpDialogController::HelpDialogController(), 0xfe1d94
fe1dac: blx 6a3708         ; AbstractController::ctor(this = r8 = r4+8)
fe1dc6: blx 67ec30         ; DefaultDialogController::ctor(this = r4+20, AbstractController* = r4+8)
                            ;   -- 67ec30 -> _ZN23DefaultDialogControllerC2EP18AbstractController
```

`20 - 8 == 12` in both, independently, matching the constructor's own three
writes exactly, with no gap. **`sizeof(AbstractController) == 12`.** The mod
over-allocates to 256 bytes for it — more than 20x — per CLAUDE.md's
"over-allocate for every Nickel constructor," on the same reasoning as
`volbuf`/the `ReadBookActionProxy` buffer in `nfnickel.cc`: the constructor
cannot be told how much room it has.

### The two vtable writes, and what it means for us

Every derived controller checked (`PasswordController`, `HelpDialogController`,
`AwardsController`, `N3BrowserController`, `N3HotspotController`,
`N3ToasterController`, `BaseImeController`) follows the same shape: call the
base's own constructor (which sets the base's OWN vptr), then immediately
overwrite that same vptr slot with the derived class's own combined vtable
pointer. This is standard Itanium ABI construction order, and it is exactly
what the mod's own `nf_browser_show` does: call the real, resolved
`AbstractController::AbstractController` to zero +4/+8 and set a
(momentarily correct, soon overwritten) vptr, then immediately overwrite
`this[+0]` with our own vtable — the pattern is not invented for this mod, it
is what every one of Nickel's own controllers already does to itself.

### `MainWindowController::push` gracefully handles a controller that is NOT a `QObject`

This is the load-bearing finding of the whole rung, because our controller —
by design (`AbstractController` is not a `QObject`, so there is no metaobject
to fake) — is exactly this case, and getting it wrong would mean `push`
either silently drops the controller or crashes on it.

`push(AbstractController *controller, bool animate)`, `0xeaacac`:

```
eaacbe: cmp r1, #0
eaacc0: beq.w eaafb6          ; controller == NULL -> jump into the shared tail below
eaacd6: blx 6a7fe4             ; __dynamic_cast(controller, srcType=AbstractController, dstType=QObject, -2)
eaacda: mov r5, r0
eaacdc: cmp r0, #0
eaacde: beq.w eaafba           ; dynamic_cast<QObject*>(controller) FAILED -> ALSO the shared tail
eaace2: blx 673564             ; (only on SUCCESS) QtSharedPointer::ExternalRefCountData::getAndRef(QObject const*)
...                             ;   -- sets up a QWeakPointer<QObject> used later to notice if the
                                ;      controller gets destroyed out from under the stack
eaafb6: add.w r9, r0, #60      ; the NULL-controller landing pad
eaafba: ldr.w r2, [r8, #60]    ; the failed-cast landing pad -- SAME shared code follows both
eaafbe: movs r5, #0            ;   -- r5 = "not already on the stack", skipping the weak-ref setup
```

Both the `controller == NULL` guard and the "controller is not a `QObject`"
outcome fall into the **same shared tail** — the QVector-based
already-on-the-stack scan, then the actual push machinery
(`prepareView`/`pushView` below). A failed `dynamic_cast<QObject*>` does not
reject the push; it only skips one optional feature (a `QWeakPointer<QObject>`
Nickel uses to notice if the controller is deleted out from under it — call
site `QObject::connectImpl`, `0x690760`, further down, wires a signal off that
weak pointer). Our mod fully owns the controller's lifetime and does not
delete it while pushed, so this gap costs nothing. **A bare
`AbstractController` with no `QObject` base is a supported, non-crashing
input to `push` — not a guess, read directly off this exact branch.**

`push` then calls, as ordinary (PLT-routed — see below) intra-library calls,
not virtual dispatch: `MainWindowController::unprepareView(topController(),
animate)` on whatever was on top before, then
`MainWindowController::prepareView(controller)` and
`MainWindowController::pushView(QWidget*)`. `prepareView`/`pushView`
themselves were **not** further disassembled — resolving their names via
`plt.sh` satisfies "every unexplained PLT stub in `push`," and tracing their
own bodies is a different function's archaeology, out of this rung's stated
scope. What matters is established without it: `push` calls something that
ultimately needs a `QWidget*`, and `AbstractController::ensureViewLoaded`
(next section) is the only place that widget can come from.

### Every PLT stub in `MainWindowController::push` (27, all resolved)

```
663de8  _ZN12QWeakPointerI7QObjectED1Ev                    QWeakPointer<QObject>::~QWeakPointer
665190  __cxa_begin_catch
66840c  _ZN11QTextStreamlsERK7QString                      QTextStream::operator<<(QString const&)
66d088  _ZN11QTextStreamlsEc                                QTextStream::operator<<(char)
670dc8  _ZN7QVectorI8QPointerI7QObjectEE6appendERKS2_        QVector<QPointer<QObject>>::append
671358  _ZN20MainWindowController13unprepareViewEP18AbstractControllerb
672404  _ZdlPv                                              operator delete
6732f0  _Znwj                                               operator new
673564  _ZN15QtSharedPointer20ExternalRefCountData9getAndRefEPK7QObject
678264  _ZN7QStringD1Ev                                     QString::~QString
67ab9c  _ZN10QArrayData10deallocateEPS_jj
681628  __cxa_end_cleanup
681f4c  _ZN20MainWindowController8pushViewEP7QWidget
68e6c8  _ZN11QMetaObject10ConnectionD1Ev
68f7bc  _ZN7QString15fromUtf8_helperEPKci
690760  _ZN7QObject11connectImplEPKS_PPvS1_S3_PN9QtPrivate15QSlotObjectBaseEN2Qt14ConnectionTypeEPKiPK11QMetaObject
6982bc  _ZNK14QMessageLogger7warningEv
698800  _ZN11QTextStreamlsEPKv                              QTextStream::operator<<(void const*)
69a304  _Z2uiv                                              (an accessor, not further traced -- see below)
69a7c8  _ZN20MainWindowController13topControllerEv
69c900  _ZN11QTextStreamD1Ev
6a0ba0  __cxa_end_catch
6a5274  _Z17qt_message_output9QtMsgTypeRK18QMessageLogContextRK7QString
6a7fe4  __dynamic_cast
6aaa88  _ZN6QDebugD1Ev
6ab7e8  _ZN20MainWindowController11prepareViewEP18AbstractController
6adb28  _ZSt9terminatev
```

`_Z2uiv` (`ui()`, no args) is called once, inside the QVector already-on-stack
scan; its result is not chained into anything this mod's own call path
depends on, and it was not traced further — flagged here rather than silently
left out, per "resolve EVERY unexplained PLT stub," but its role does not
change anything this rung relies on.

The rest of `push` (`QMessageLogger::warning` + `QTextStream` +
`QString::fromUtf8_helper` + `qt_message_output`, repeated several times) is
Nickel's own `qWarning() << ...` diagnostic logging around edge cases (a
duplicate push, an already-present controller) — noise for archaeology
purposes, not calls this mod's own flow needs to understand further.

### `AbstractController::ensureViewLoaded` calls slot 8, and what happens next is non-fatal either way

`0xad1408`, not one of the two functions this rung's stub-resolution
requirement names, but the caller of slot 8 (per the given measured facts),
so its immediate aftermath matters for knowing what slot 8's contract is:

```
ad142a: ldr r3, [r4, #0]   ; r3 = vptr
ad142e: ldr r3, [r3, #32]  ; r3 = vtable[8]           -- confirms the +32 offset independently
ad1430: blx r3             ; call it, this = r4, return value DISCARDED (r0 is
                             ;   overwritten by `mov r0, r4` two instructions later
                             ;   and never read before that)
ad1440: blx 6a7fe4          ; __dynamic_cast(this, AbstractController, <some other type>, -2)
ad1444: cbz r0, ad1456      ; cast failed -> qWarning() (QMessageLogger::warning,
                             ;   QString::fromUtf8_helper, QTextStream<< -- same
                             ;   logging shape as push's) and CONTINUE, does not abort
```

So slot 8's contract, read off its only caller: **it is called with `this` in
r0, its return value is never used, and it is expected to leave the view
pointer at `this+8`.** `AbstractController::viewLoaded()` (`0xad13ac`) is a
BOOL-returning function, not a pointer-returning one, and its first read is
`this+4` (the "d" pointer AbstractController's own constructor zeroes), not
`this+8`:

```
ad13ac: ldr r3, [r0, #4]   ; r3 = this->d
ad13b2: cbz r3, ad13c8      ; d == 0 -> return false
ad13b4: ldr r3, [r3, #4]    ; r3 = d[+4]
ad13b6: cbz r3, ad13c8      ; d[+4] == 0 -> return false
ad13b8: ldr r3, [r0, #8]    ; r3 = this->view -- ONLY reached once d and d[+4] are both non-null
ad13be: movne r0, #1        ; return bool(r3 != 0)
```

So `this+8` is where the view pointer lives — confirmed independently by
this being the only other offset the function reads at all, and by nothing
in the vtable slots this mod copies writing anywhere else — but
`viewLoaded()` answering `true` also depends on a "d" pointer at `this+4`
that AbstractController's own constructor leaves zero and that this rung
never sets up (not established whose job that is; see "What this rung does
NOT establish", below). The `dynamic_cast` after slot 8 returns targets a
type our copied, plain-`AbstractController` RTTI (see next section) cannot
satisfy, and failing it is already a handled, logged, non-fatal outcome in
Nickel's own code — not a path this mod needed to avoid, just one it will
exercise once, harmlessly, the first time the screen loads.

### The vtable header matters, not just the 9 slots

`_ZTV18AbstractController` (`0x163ff70`) is preceded by its own `_ZTI` object
(`0x163ff68`, the `__class_type_info` RTTI struct: vtable-for-type_info +
name pointer, matches "a plain `__class_type_info`, no base class" from the
given measured facts) and begins with two Itanium ABI header words the
constructor's own `adds r3, #8` (above) confirms are there:

```
0x163ff70  offset-to-top       (0)
0x163ff74  RTTI pointer        -> _ZTI18AbstractController        [only ABS32 reloc in this range]
0x163ff78  vtable[0] = D1      \
0x163ff7c  vtable[1] = D0       |  the address point (vptr value) is 0x163ff78 --
0x163ff80  vtable[2] = size     |  table_base + 8, exactly matching the ctor
   ...                          |
0x163ff98  vtable[8] = __cxa_pure_virtual   -- vptr+32, matching ensureViewLoaded's own `#32` above
```

Slots 2 through 8 (`size`, `viewWillAppear`, `viewWillDisappear`,
`viewWillBeDestroyed`, `allowedOrientations`, `navSection`,
`__cxa_pure_virtual`) each carry an `R_ARM_ABS32` relocation naming the
symbol — `readelf -r` confirms all seven. **Slots 0 and 1 (the two
destructors) carry no relocation at all that this build's `objdump`/`readelf`
can show** — not `R_ARM_ABS32`, not `R_ARM_RELATIVE` — despite other, nearby
vtables (`QObject`'s) showing plain `R_ARM_RELATIVE` entries with the target
address baked directly into the file for their own destructor-adjacent slots.
This was checked, not shrugged off: `readelf -r | grep 163ff7` finds only the
one RTTI-pointer relocation; a wider byte-for-byte dump of `.data.rel.ro`
across that whole range reads as zero. Unresolved, and left unresolved on
purpose rather than guessed at, because it does not matter to correctness
here: **the mod never hand-constructs these two words.** It resolves
`_ZTV18AbstractController` by name and copies all 11 words (both header words
plus all 9 slots) out of the LIVE, already-relocated table at runtime, after
Nickel's own dynamic linker has resolved every slot by whatever mechanism it
uses — the mod does not need to know which mechanism that is, only that by
the time `nf_init` runs, the table in memory is correct, which is the same
trust every other resolved-by-name symbol in this project already rests on.

### The paragraph above was wrong, and the mistake is left in on purpose

Code review (not a device run this time — a second pair of eyes on the
archaeology itself) caught it before it shipped: "it does not matter to
correctness here" and "the mod does not need to know which mechanism [fills
these in]" both assume there **is** a mechanism — that the two words read
zero in the static file only because this project's tools can't decode
whatever populates them at load time. That assumption was never checked
against the alternative: that they are zero in the file because they are
**meant to stay zero**, forever, at both compile time and runtime, because
nothing in Nickel's own compiled code ever dispatches through
`_ZTV18AbstractController`'s own D1/D0 slots specifically. Every real
Nickel controller multiply-inherits `QObject` (see "The two vtable writes"
below) and its own constructor immediately overwrites the vptr
`AbstractController::AbstractController` set with the DERIVED class's own
combined vtable — so nothing in normal operation ever destroys an object
through the base class's isolated table. The linker plausibly never needed
to populate those two slots because no reachable code path calls through
them, which is a completely different, and much worse for this mod, than
"populated by a mechanism these tools can't see."

**The consequence: reading `_ZTV18AbstractController` live at runtime gives
NULL in both destructor slots, not Nickel's real destructors.** A copy of
this table into a controller of ours, followed by any virtual destruction of
that controller (a `D0`/`D1` call through the vtable — which our own
controller's construction pattern makes newly reachable, unlike every real
Nickel controller, precisely because we deliberately keep pointing at a copy
of the BASE table rather than building a full derived one), jumps to address
0 inside Nickel. This is the exact "ungated call through a resolved pointer"
class of bug `CLAUDE.md` exists to prevent, self-inflicted by trusting a
live memory read instead of resolving the two symbols that answer it
directly.

**The fix**: `_ZN18AbstractControllerD1Ev` (`0xad1358`) and
`_ZN18AbstractControllerD0Ev` (`0xad1398`) are BOTH separately exported —
confirmed with `nm -D --defined-only`, same as every other symbol this mod
resolves — so there was never a reason to lean on the live-copy assumption
for these two specifically. They are now resolved by name
(`AbstractController__dtor1`/`__dtor0`, `nfnickel.h`/`nfnickel.cc`) and
stored into `NFControllerVTable`'s slots 0/1 explicitly
(`nf_browser_vtable_ready`, `nfbrowser.cc`), NULL-gated the same as every
other resolved pointer in this project. The live copy is still correct, and
still used, for the RTTI pointer and slots 2–8 — every one of those DOES
carry a normal relocation naming its symbol, confirmed by `readelf -r`, so
reading them live is this project's ordinary "resolve by name, don't
hardcode" discipline, not a second guess in the same place.

**The generalisable lesson, which is now the same shape twice in this
project** (the other being the back-gesture baseline in "The back-gesture
measurement was wrong once, and the reason generalises", above): an absence
in the data (a missing relocation, a baseline that happens to match one
hypothesis) was explained by reaching for "the tooling must not be showing
me something" rather than treating the absence itself as the fact and asking
what it would mean if that fact were simply true. It compiled, linked, and
would have passed every check in this rung's own build — the same way
`getById`'s missing `this` did. It did not reach a device.

### How `ndbCurrentView` almost certainly identifies a view: `MainWindowController::currentViewName()`

Not one of the two required functions either, but the whole point of this
rung is a screen that "reports itself to the oracle," and the task asked
specifically not to guess at an `objectName` if the real mechanism could be
found instead. It could:

`_ZNK20MainWindowController15currentViewNameEv`, `0xea8010`:

```
ea8018: ldr r0, [r1, #68]      ; this->[+68] -- a QStackedWidget* (confirmed: the same
                                 ;   field backs currentView(), 0xea8000, which tail-calls
                                 ;   QStackedWidget::currentWidget() on it directly)
ea801c: blx 682cd8              ; QStackedWidget::currentWidget()             -> r5
ea802a: blx 66d000              ; QMetaObject::cast(QObject*) against some fixed
                                 ;   QMetaObject (an N3Dialog check) -- if the current
                                 ;   widget IS an N3Dialog, r5 is replaced with...
ea8030: blx 66c850              ; N3Dialog::content()                        -> r5
ea804c: blx 69b2ec              ; QObject::property(char const*) on r5, with the
                                 ;   literal string "mainNavView" (read directly out of
                                 ;   the binary at the address the call's own PC-relative
                                 ;   literal resolves to -- confirmed by dumping those raw
                                 ;   bytes: `mainNavView\0`)
ea8054: blx 66bd64              ; QVariant::toString() on that property's value
ea8088: cbnz r6, ea80c4          ; non-empty -> RETURN IT, done
ea809c: blx 6aaff4               ; (only if empty) QMetaObject::className() -- the fallback
```

So `currentViewName()` reads `widget->property("mainNavView").toString()`,
and only falls back to `widget->metaObject()->className()` if that property
is unset or empty. This was not traced into NickelDBus's own binary (out of
scope, and NickelDBus's source is not staged here) — so "almost certainly"
is as far as this goes: the name match to `ndbCurrentView` and the fact that
the strings NickelDBus is documented to report (`HomePageView`, `ReadingView`,
...) are exactly what `className()` on Nickel's own view classes would
produce, is what makes the fallback plausible as what real views normally
hit (most of Nickel's own views presumably never set `mainNavView` and fall
through to their own class name).

Either mechanism identifies our screen without guessing: `nf_browser_load_view`
sets `mainNavView` explicitly (`"NFBrowserView"`, taking the primary path if
this reading is right), and even if the property read is not what
`ndbCurrentView` actually calls, a bare `QWidget`'s `className()` is
`"QWidget"` — distinct from every real Nickel view, still enough to prove a
non-baseline screen appeared. Both paths were kept rather than one guessed
at.

### What this rung does NOT establish

- Whether `dynamic_cast<QObject*>(controller)` failing costs anything
  **beyond** skipping the weak-pointer bookkeeping described above — e.g.
  whether some other, not-yet-found call site also branches on it. Only the
  one call site inside `push` itself was checked, per the brief's scope.
- `prepareView`/`pushView`'s own bodies — not traced, per the brief's stated
  scope (only `AbstractController::AbstractController` and
  `MainWindowController::push` needed every stub resolved).
- Whether `ndbCurrentView` genuinely calls `currentViewName()` — plausible,
  not confirmed, see above.
- Whose job it is to set up the "d" pointer at `this+4` that
  `AbstractController::viewLoaded()` also requires before it will answer
  `true` (see "`AbstractController::ensureViewLoaded` calls slot 8", above).
  AbstractController's own constructor leaves it zero and nothing this rung
  adds sets it, so `viewLoaded()` answering `false` even after slot 8 has
  run a valid `QWidget*` into `this+8` is a real, unverified possibility —
  not established to matter to anything this rung's own flow depends on
  (nothing here calls `viewLoaded()`), but worth knowing before anything
  later in this project does depend on it.
- **Back.** NOTES.md's rung-1 back-gesture result (`ReadingView` pops to the
  view beneath) was measured for a *reader* pushed via
  `ReadBookActionProxy::onSelected()`, a completely different code path from
  `MainWindowController::push`. Nothing here establishes that back pops a
  controller pushed this way, or what happens when the controller's `D0`
  (deleting destructor, resolved by name — `AbstractController__dtor0`, see
  "The paragraph above was wrong", above — and stored explicitly, NOT
  copied from the live vtable) runs on an object this mod allocated with its
  own `::operator new` rather than Nickel's. This is exactly what Step 6
  (device-only, not this rung's job) exists to check.
