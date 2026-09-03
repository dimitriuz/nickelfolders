# NickelFolders v1 — design

Written 2026-09-03, after the spike answered all six of `NOTES.md`'s open
questions. Nothing in v1 re-derives anything `CLAUDE.md` lists as settled.

**What v1 is:** a folder browser on Nickel's own window stack. You reach it
from a menu item, walk one directory at a time, tap a book, and it opens in the
stock reader. One back press returns you to the folder you were in.

**What v1 is not:** covers, search, file operations, network. Those are wanted
and are addressed under *Keeping the future open*, not built.

## 1. Architecture: a native Nickel screen

An `AbstractController` subclass whose view is a plain Qt widget, pushed with
`MainWindowController::push`.

Measured off `libnickel.so.1.0.0` (4.38.23684), so the decision is priced
rather than assumed:

- `AbstractController`'s typeinfo at `0x163ff68` is a plain
  `__class_type_info` — **no base class. It is not a QObject.**
- Constructor `_ZN18AbstractControllerC1Ev`, both destructors, and the
  **vtable** `_ZTV18AbstractController` (`0x163ff70`) are all exported.
- The vtable holds **exactly one `__cxa_pure_virtual`**, at `+32`.
  `AbstractController::ensureViewLoaded` (`0xad1408`) calls precisely that slot
  (`ldr r3,[r3,#32]; blx r3`), so the one method a subclass must supply is
  "load your view". `size`, `viewWillAppear`, `viewWillDisappear`,
  `viewWillBeDestroyed`, `allowedOrientations` and `navSection` all have
  inheritable non-pure implementations.
- `AbstractController::viewLoaded() const` returns a **bool**, not the view
  pointer. **An earlier draft of this section said it "returns `this+8`" and
  that was wrong** — the instructions at `0xad13ac` are
  `ldr r3,[r0,#4]` … `ldr r3,[r3,#4]` … `ldr r3,[r0,#8]` … `adds r0,r3,#0` /
  `it ne` / `movne r0,#1`, i.e. a three-condition test normalised to 0/1. The
  error was read off a partial disassembly that stopped before the `movne`, and
  it propagated into the implementation plan and one task brief before a review
  caught it.

  What `+4`/`+8` actually are: a **`QWeakPointer<QWidget>`** pair — `d` at `+4`
  and `value` at `+8` — and every consumer in Nickel gates on
  `this[+4] && this[+4][+4]` *before* reading `this[+8]`. So the pure virtual at
  slot `+32` must leave a real weak pointer behind, not merely store a `QWidget*`
  at `+8`. Leaving only the raw pointer makes Nickel's view fetch return NULL and
  the screen never appears.

- **Nickel's own controllers are QObjects, and the window stack requires it.**
  `MainWindowController::push` appends to a `QVector<QPointer<QObject>>` at
  `MainWindowController+60`, and `topController()` (`0xea8acc`) returns the last
  element. `PasswordController` and `HelpDialogController` place a `QObject` base
  at `+0` with the `AbstractController` subobject at `+8`. A controller that is
  not a QObject is *accepted* by `push` — the failed internal
  `dynamic_cast<QObject*>` only skips optional bookkeeping — but it enters the
  stack as a **null** `QPointer`, so `topController()` returns NULL and there is
  nothing for a back gesture to pop. The "no metaobject to fake" remark below is
  therefore only half true: no *moc* metaobject is needed (we declare no signals
  or slots of our own, exactly as `nfolders.cc`'s trigger object does), but a
  real `QObject` base is.

- The vtable's **destructor slots at `+0` and `+4` are zero in the file and have
  no relocations** (verified: `objdump -R` lists entries at `0x163ff74` and
  `0x163ff80`-`0x163ff98` and nothing between). So copying Nickel's live table
  yields NULL destructor slots, and any virtual destruction of our controller
  jumps to address 0. `_ZN18AbstractControllerD1Ev` (`0xad1358`) and
  `_ZN18AbstractControllerD0Ev` (`0xad1398`) are both exported and must be
  resolved by name and stored explicitly rather than copied.

So the native screen costs about what `getById` cost: one vtable to build, one
pure virtual to implement, one `push` to call. `sizeof(AbstractController)`
still has to come from a concrete controller's own `operator new` call site per
the fixed method in `CLAUDE.md` — it looks like 12–16 bytes, and that is a
guess until measured.

### Rejected: reusing `FolderItemListWidget`

The native screen's value is `MainWindowController::push` plus
`AbstractController` — touch, e-ink refresh, fonts and the back gesture. That
is four opaque symbols. Reusing `FolderItemListWidget` additionally means
faking `FolderItemDataSource` and `FolderItem`, which is subclassing more
Nickel classes whose vtables nobody has measured, for a cosmetic gain a plain
Qt widget already gets by inheriting Nickel's palette and fonts. It buys little
and spends the whole risk budget.

### Rejected: an FBInk overlay

Beyond being a second UI, it would draw into a framebuffer that a live Nickel
is actively repainting, with no arbitration between them. koboy works because
it takes the panel over entirely; this is the opposite situation.

### Rejected as a fallback: shelves-from-folders

Honestly priced, it is still about an hour of Python, and its real cost is not
flatness. Writing `Shelf`/`ShelfContent` behind a running Nickel races Nickel's
own `Repository` cache, and a 27-volume folder becomes one shelf whose name is a
path. It buys two-tap reach with no crash risk and firmware immunity. Worth
doing only as a throwaway measurement of how much a tree is actually worth,
never as v1's fallback.

## 2. Data: no SQL, and no database handle

The tree comes from the **filesystem**, one directory at a time. Per-row
metadata comes from the **already-proven `getById` call**.

This inverts the emphasis of the settled fact. "The tree is derivable from the
database alone" says the database *suffices*, not that the filesystem is
forbidden — and v1 has to read the current directory anyway, because a file
with no database row exists only on disk. Since that read is happening, the
filesystem becomes the tree source and the database is reached only through
Nickel's own lookup:

| Need | How |
|---|---|
| Folders and files in the current directory | `QDir::entryList`, non-recursive |
| Is this file openable? | `getById` then `Volume::isValid()` — proven, with a negative control |
| Title, reading progress | `Volume::getDbValues() const` (`0x00a63f64`) |
| Open it | the proven `ReadBookActionProxy` → `onSelected()` sequence |

`Volume::getDbValues()` is the reason this works without resolving a dozen
accessors: the typed getters (`title()`, percent-read) are **not exported** —
only `isValid`, `isFolder`, `entitlementId` and a store-related handful are —
because they are inline in Nickel's header. `getDbValues()` is the ORM's
serialisation half, paired with the exported
`Volume::fromAttributes(QHash<QString,QVariant> const&)`, so it hands back
every column keyed by name in one call.

**What this buys:** no `QSqlDatabase` connection, no handle on the 432 MB
`KoboReader.sqlite`, no race against Nickel's cache, and no archaeology to
discover Nickel's SQL connection name.

**Two things it owes, both recorded as rung-4 work:**

- `getDbValues()`'s calling convention. It returns a container by value, so it
  almost certainly uses a hidden return buffer as argument zero — the same trap
  that crashed Nickel once for `getById`. Every PLT stub it calls gets resolved
  before it is called, per the fixed method. No guessing.
- The cost of N `getById` calls per navigation. `NOTES.md` establishes that
  `getById` calls `Repository::refreshCache<Volume>(dbName)`, and nobody has
  timed that. A directory here holds 10–30 rows. If per-row lookup is too slow
  for e-ink, the fallback is one SQL query per folder — which reintroduces the
  database handle, so it is a fallback and not the plan.

`dbName` comes from `Device::getCurrentDevice()` then `Device::getDbName()`.
Never a hardcoded `""`.

## 3. Display rules — where the device measurements landed

Two of these came out of measuring the actual card on 2026-09-03 and neither
was in the original plan.

### 3.1 A numeric-aware comparator is a v1 requirement, and it applies to directory names

`/mnt/onboard/books/Comics/English/Sandman/` contains **directories** named
`v1`, `v10`, `v2`, `v3` … `v9` — **unpadded**. A plain case-insensitive sort
orders them `v1, v10, v2, …`, which puts *The Wake* — the finale — second in
the list.

This nearly went unnoticed, and the reason is worth carrying: the 27-volume
Fullmetal Alchemist folder's **files** are zero-padded (`v01` … `v27`), so
looking at files alone says a plain sort is fine. It is the *folder* names that
are unpadded. **Measure both kinds or the answer is wrong.**

`QCollator` has a numeric mode but needs ICU, which is not worth betting on in
Kobo's Qt 5.2.1. v1 ships a small numeric-aware compare of its own: split each
name into digit and non-digit runs, compare digit runs by value and the rest
case-insensitively. No stdlib, no ICU.

### 3.2 Strip the common prefix and suffix within a listing — do not use a middle ellipsis

The 13 files in
`books/Pokémon - La Grande Aventure (01-13+) …/` all begin with the same
**34 characters** and each carries roughly 60 more characters of release-group
tail. The token that identifies the volume (`T01`) sits in the **middle**:

```
Pokémon - La Grande Aventure (Part 1) - Rouge, Bleu et Jaune T01 (Kusaka-Mato) (2014) [Digital-1085] [Manga FR] (PapriKa+).cbz
```

koboy's `ui_fit_label` middle ellipsis would keep the useless head and the
useless tail and elide exactly the part that tells the volumes apart. That is
the same failure koboy recorded at 15 shared characters, made worse by the
identifier not being at the front.

So the rule is not an elision rule. **Within one listing, find the longest
common leading and trailing run across the rows and strip both for display**,
trimmed back to a token boundary so a row never begins mid-word. Applied here:

- Fullmetal Alchemist's 27 rows become `v01` … `v27`
- Pokémon's 13 become `(Part 1) - Rouge, Bleu et Jaune T01` and so on

Three properties make this safe rather than clever:

- It **cannot create duplicate labels**, because it removes text that is
  identical in every row.
- It **degrades correctly**: a folder of mixed formats has no common run, so
  nothing is stripped.
The extension, however, is **not** handled for free, and the original draft of
this section was wrong to say it was. Validated against the real names on
2026-09-03: `Fullmetal Alchemist …` strips its extension only because its
common suffix happens to contain whitespace, whereas a folder of
`Batman v01.cbz` / `Batman v02.cbz` has the common suffix `.cbz` with no
whitespace inside it, so the token-boundary rule cannot cut there and the
extension survives on every row.

So extensions are handled **explicitly, before the common run is computed**:
split a known extension off each name, compute the common prefix and suffix on
the stems, and re-append the extension only when the listing contains more than
one distinct extension — where it is the one thing distinguishing two rows and
must stay.

Two further rules came out of the same validation and are requirements, not
polish:

- **Backing the prefix off to a whitespace is not sufficient; it must also back
  off past an unmatched opening bracket.** The Pokémon folder's names share
  `… Aventure (Part `, and cutting at the whitespace leaves rows beginning
  `1) - Rouge …` — which loses the `(Part 1)` that actually distinguishes the
  sub-series. Backing off to before the `(` yields
  `(Part 1) - Rouge, Bleu et Jaune T01 …`, which is the wanted result.
- **Refuse to strip rather than strip badly.** If any row's remainder would come
  out under two characters, no row is stripped at all. A listing of full names is
  merely verbose; a listing of one-character rows is broken.

### 3.3 Ordering

Kind first — folders before files — then §3.1's comparator, and **the whole
listing is sorted before any cap is applied**. koboy's `romlist.h` calls
sort-before-cap "the original bug's fix" and says it must survive any change:
what survives a truncation must be alphabetical, never an artifact of readdir
order. Inherited verbatim.

### 3.4 v1: a collision guard, not folder-on-row

`FOLLOWUPS.md` #31 in koboy shows two same-named files, from different
folders, rendering as the same row once folder context is stripped from the
label. That is real, but it cannot happen **within a single directory**: a
directory's filenames are already unique, and `nf_strip_common` removes only
text that is identical across every row in the set, so unique names minus a
common run stay unique. v1 lists one directory at a time, so this failure mode
does not arise there.

What v1 still needs is a **guard**, not a fix for a case it cannot hit: the
per-row bracket truncation and `trimmed()` inside `nf_strip_common` are *not*
common to every row and could in principle make two labels collide by
accident. If any two labels **within one kind** collide, that kind's rows keep
their raw names. The check and the fallback are per kind, because §3.2's
stripping is: folders and files are labelled as separate sets, so a collision
among folder labels leaves the file labels stripped and vice versa. Degrading
rows that are perfectly distinguishable, because two others clashed, would be
a worse answer than the collision it was avoiding.

Showing the folder on the row **only for the rows that collide** is the right
fix for the case `FOLLOWUPS.md` #31 actually describes — a *pooled* result set
whose rows come from different folders — which is §6.5's flat search result
set, and belongs to v2.

### 3.5 What is hidden, and one accepted wart

Files are filtered by an **extension allowlist**: `.epub`, `.kepub.epub`,
`.cbz`, `.cbr`, `.pdf`. Measured against the whole card on 2026-09-03, that
allowlist admits 226 files and hides all nine that are not books — five
`.svg` sketches in `drawings/`, `KOBOY-INSTALL.md`, a KOReader `.lua` sidecar,
a Windows `.dat`, and one extensionless `IndexerVolumeGuid`.

**`.txt` is deliberately NOT on that list**, and the reason is a small
correction to the census. There is exactly one `.txt` file on the card and it is
`koboy-probe-Io.txt` — a probe file the sibling project left on `/mnt/onboard`,
which Nickel then imported. So the census's "1 `.txt`, with a row" was never a
book: the real count is **226 book files, 225 of them openable**, the one
exception still being the truncated Fullmetal Alchemist v26.

That single fact is what removes `.txt`. Because the probe file *has* a
database row, no greyed-row logic would catch it — it would render as an
ordinary, openable row called `koboy-probe-Io`, which is precisely the noise
this browser exists to remove. If a genuine `.txt` book is ever sideloaded,
putting the extension back is a one-line change; leaving it in today buys one
row of garbage and nothing else.

Directories need their own rule, because an extension allowlist does not touch
them: **hide dotdirs and `*.sdr`**. A `.sdr` sidecar sits in plain sight at the
card root (`calibrewebdownload2055pdf2055.sdr`), so this is not theoretical.

**Accepted wart:** `screensavers/` and `drawings/` contain no books, so they
show as folders that turn out to be empty once entered. Knowing that in advance
needs a recursive count, which is the walk this design avoids. The fix — a
cached per-folder book count — is deferred, not forgotten, and §3.6 at least
makes the empty folder say so.

### 3.6 "Empty" and "cannot read" stay distinct

A folder with no showable rows and a folder that cannot be listed are different
diagnoses to someone holding an e-reader with no terminal. koboy makes the same
distinction (`romlist` returning `-1` versus `0`) and arrived at it
independently, which is reason to trust it. Same rule as the greyed no-row
book: say what is wrong in the row, do not hide it.

### 3.7 Depth

The real card is five levels deep
(`books/Comics/English/Sandman/Sandman Mystery Theatre/Blackhawk`). Navigation
state is a path string with no depth assumption anywhere.

## 4. The six decisions

**Launch — a NickelMenu item in the library menu.** It is already installed,
and it keeps v1's one genuinely risky new thing — the controller subclass —
unaccompanied by a second archaeology project. Mechanism, which also pays debt
item 3: NickelMenu `cmd_spawn` touches a path on `/tmp`, and the mod watches it
with **inotify through a `QSocketNotifier`**. Event-driven, no poll thread, no
client binary to ship, and no handle on `/mnt/onboard`. Honest cost: v1 depends
on NickelMenu staying installed. Hooking our own menu item is the part of
NickelMenu that breaks across firmware, and a real library tab is the right end
state — both v2.

**Last folder — remembered. Scroll position — not.** With a 27-volume folder
you re-enter the same place constantly, and last-folder is one string written on
leaving. Per-folder scroll memory is a keyed map plus persistence, and it is not
needed: back-from-reader pops to our controller *still alive with its view
loaded*, so scroll survives within a session for free. That is the settled
replace-not-stack result paying out. The string is written in a Qt signal
handler, which `CLAUDE.md` names as a safe `/mnt/onboard` window.

**Sorting — §3.3.**

**Non-book files — hidden, per §3.5**, with the greyed no-row row kept. It costs
about fifteen lines given one-directory-at-a-time, and a silently missing
volume 26 is the exact failure `NOTES.md` warns about.

**Reading progress — shown, minimally.** `getDbValues()` already carries it, so
it is free: a percentage for in-progress books only, a marker for finished,
nothing for unread. For manga, "which volume am I on" is the most useful thing
on the screen.

**Collections — no interaction at all, deliberately.** The browser and
collections are two answers to the same question, and both writing is how the
library gets into a mess. The one interaction wanted is Recents, which
`ReadBookActionProxy` already gives for free. This is a non-goal, not an
omission.

## 5. Rungs

Each stops somewhere a device run can confirm, because a crash at a known rung
names one call and a crash after six names nothing.

1. **Debt only, no UI.** `Device::getDbName()`; free the proxy; inotify plus
   `QSocketNotifier` replaces the poll thread. Verify the existing open path
   still works and Nickel's PID is unchanged.
2. **An empty screen.** The `AbstractController` subclass, pushed on a
   NickelMenu tap, back pops out. Oracle: `ndbCurrentView` plus PID unchanged.
   Negative control: a push with a deliberately wrong vtable slot must fail
   visibly rather than silently.
3. **A list widget with hardcoded rows.** Confirmed by screenshot — a view
   name does not say what is on the panel.
4. **Real listings.** `QDir::entryList` plus `getById`/`getDbValues`, one
   directory at a time, sorted per §3.3, labelled per §3.2. This is the rung
   that owes the `getDbValues()` disassembly and the `getById` timing.
5. **Tap to open**, wiring in the proven sequence.
6. **Greyed rows, progress markers, last-folder memory.**

## 6. Keeping the future open

Wanted later: covers, search, sort options with a direction, filtering, group
operations, file operations, and possibly network. None are built. What follows
is what v1 does structurally so none of them needs a rewrite — and one of them
changes a v1 decision.

### 6.1 One pipeline, in this order

Every future feature above is an insertion into the same sequence, which is why
v1 builds it as a sequence rather than as one function that lists and sorts:

```
list the directory
  -> hide junk            (§3.5: extension allowlist, dotdirs, *.sdr)
  -> user filter          (FUTURE: by type, or anything else)
  -> fetch metadata       (getById + getDbValues, for EVERY row -- see §6.2)
  -> group by kind        (§3.3: folders before files)
  -> order within group   (§3.3's comparator; FUTURE: key + direction)
  -> cap                  (none in v1; §3.3 says this stays after ordering)
  -> derive labels        (§3.2: strip the common run, on the rows shown)
  -> disambiguate         (§3.4: v1 is a collision guard, raw-name fallback;
                                 folder-on-row is v2's flat search, §6.5)
```

The order of the last three is load-bearing rather than tidy. **Labels are
derived after filtering**, because filtering changes the row set and a common
prefix computed before it may no longer be common after it — strip it anyway and
rows lose text that was actually distinguishing. §3.2 is order-independent, so
re-sorting never needs the labels recomputed, but re-filtering always does.

### 6.2 Sort options change a v1 decision: metadata is fetched eagerly

A direction toggle alone is cheap: **grouping and ordering are separate stages
above, so descending reverses the order within each kind and never floats files
above folders.** Recording that now because "reverse the list" is the obvious
implementation and it is the wrong one.

Alternate sort *keys* are the part that reaches back into v1. Sorting by date
added, size or percent read needs that field for **every row before the sort
runs**, so metadata cannot be fetched lazily for the rows on screen. v1
therefore fetches it for the whole listing up front, even though v1's only sort
key is the name and would not need it.

**That raises §7's open timing question rather than answering it:** the cost is
`getById` on all 27 rows of the largest folder before first paint, not on the
handful that are visible. It is the same measurement, with a bigger N, and it is
still the number this data design rests on.

### 6.3 Filtering needs a third empty state

A user filter is a **separate layer from §3.5's allowlist**, not an extension of
it. The allowlist answers "is this a book"; a filter answers "which books do I
want to see right now". Merged, filtering to PDFs would start arguing with the
junk-hiding rule.

The new failure it introduces is worth building §3.6 to accommodate: a folder
where **everything was filtered out** must not look like an empty folder. Filter
to PDF, open the manga folder, see nothing, and the honest reading is "my books
are gone". So §3.6's two states become three — empty, unreadable, and filtered
to nothing — which is koboy's `-1`-versus-`0` lesson generalising exactly as it
did the first time.

Whether a filter also hides *folders* containing none of the wanted type needs
the recursive count already deferred in §3.5. Same fix, same deferral.

### 6.4 Group operations multiply an existing hazard

**File operations** carry a hazard worth recording now: `unlink`-ing a book
leaves an orphan `content` row *and* a stale `Repository` cache entry, so
`getById` would go on returning a valid `Volume` for a file that is gone. Any
future delete must go through Nickel's own removal path rather than the
filesystem. This is why v1 is strictly read-only.

**Group operations are that hazard with a blast radius of N**, and they add two
of their own:

- Tap means "open" in v1. Selection needs a mode, so tap becomes ambiguous and
  the mode has to be visible on the panel. Not foreclosed, but it is a real UI
  decision and not a checkbox.
- A group operation must act on the **selected rows**, never on "everything
  currently shown". With §6.1's filter and sort stages in between, those two
  sets diverge, and that divergence is the classic way a bulk delete takes the
  wrong files.

### 6.5 The cheap ones

- **Search** wants a flat result list, not a folder listing. So §3's display
  rules are written against *the set of rows being shown*, never against "the
  current folder". A search result set has no common run, so §3.2 does nothing
  to it and §3.4 does the work instead — correct behaviour with no special case.
- **Covers** need `ImageId`, which `getDbValues()` already returns. No new
  archaeology; the row model gains a field and the row gains a fixed height. The
  real cost is decode plus e-ink refresh time, which is why it is not v1.
- **Network** is unspecified, and nothing here blocks it.

## 7. Open, and owed

Device questions, batched — a reboot is the unit of iteration:

1. **Time N consecutive `getById` calls.** Drives §2's per-row lookup versus
   the SQL fallback, and it is the one number v1's data design rests on.

   **It cannot be measured with the installed spike, and believing otherwise
   would be this project's third instrument mistake.** Driving it through
   `--stage 2` measures the trigger path, not the lookup: the poll thread has a
   500 ms floor and syslog timestamps are one-second granular, so both swamp
   what is probably a sub-millisecond hash lookup. Worse, it would fail
   *patterned* — every sample landing near the poll interval — which is exactly
   the signature `CLAUDE.md` says to distrust.

   So it needs the loop and the timing inside the mod, and it lands in rung 4
   with a negative control: a deliberately slow path whose measured cost must
   differ, so a fast reading is known not to be the clock's resolution.

2. ~~Extension inventory and largest directory by entry count.~~ **Answered
   2026-09-03**, and the inventory corrected §3.5 — see there. Entry counts,
   measured: 27 (Fullmetal Alchemist), 18 (Sandman Mystery Theatre), 14
   (`Sandman/v9 - The Kindly Ones`), 14 (`Sandman`), 13 twice. Nothing is large
   enough for paging to be a v1 concern; a scrolling list covers all of it.

   `Sandman` is worth noting: 14 entries made of 11 subfolders and 3 files, so a
   folder holding both kinds is real here and §3.3's kind-first ordering is
   doing actual work rather than defending against a hypothetical.

Host-side, before rung 4:

3. `Volume::getDbValues()`'s return convention and every PLT stub it calls.
4. `sizeof(AbstractController)`, from a concrete controller's own `operator new`.
