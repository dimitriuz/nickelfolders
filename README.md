# NickelFolders

A folder-navigating book browser for Kobo e-readers that runs **on top of
Nickel** — Kobo's own software keeps running, and the book you pick opens in
the **stock reader**.

Kobo's library has no folder tree. Your books are in folders on the card,
Nickel imports them recursively and keeps the whole path, and then shows you a
flat list grouped by author or series. KOReader solves that by replacing the
reader, which is the wrong trade if you *like* Kobo's reader. NickelFolders
adds the folder tree and leaves the reader alone.

It is a [NickelHook](https://github.com/pgaskin/NickelHook) mod: a C++/Qt
shared library injected into the Nickel process as a Qt image-format plugin.
Nickel is not modified on disk, and the mod is removable by deleting one file.

> **Status:** works, and is used daily on the developer's own device. Verified
> on a **Kobo Libra 2, firmware 4.38.23684**. Other models and firmwares are
> untested — see [Compatibility](#compatibility) before installing.

## What it does

- **Browse the folder tree** on the card, one directory at a time, and open any
  book in the stock reader. Back returns you to the same folder.
- **Book covers** on the rows, taken from the ones Nickel has already rendered.
  Books Nickel has not drawn a cover for yet fall back to a type icon.
- **Sort** by name, size, date, recently added or recently read, ascending or
  descending. Names sort naturally, so `v2` comes before `v10`.
- **Filter** by file type (`.epub`, `.kepub.epub`, `.cbz`, `.cbr`, `.pdf`) or by
  reading state (finished, in progress, not started).
- **Shorter labels.** In a folder of
  `Series Name v01 (2005) (Digital) (Group).cbz`, the part every row shares is
  stripped, leaving `v01 (2005)`. Long names elide in the *middle*, so the end
  — where the volume number usually lives — survives.
- **Reading progress** on each row, and a plain `[not in library]` marker for
  files on the card that Nickel has no row for.
- **File operations**: select several rows, then delete, copy, cut and paste.
- **A view menu** for filename truncation, extensions, covers, hidden files and
  file sizes.

## Install

Download `KoboRoot.tgz` from the
[releases](https://github.com/dimitriuz/nickelfolders/releases), then:

1. Connect the Kobo over USB.
2. Copy `KoboRoot.tgz` into the `.kobo` folder on the device.
3. Eject. The Kobo reboots and installs it — this is how every Nickel mod is
   installed, and the file deletes itself in the process.

NickelFolders has no menu entry of its own. It is opened from
[NickelMenu](https://github.com/pgaskin/NickelMenu), which most people running
Kobo mods already have. Add this to NickelMenu's config:

```
menu_item :main :NickelFolders :cmd_spawn :quiet:/bin/touch /tmp/nfolders-native
```

## Uninstall

Either:

- create a file called `nfolders_uninstall` in the root of the card and reboot —
  the mod removes itself and the flag; or
- delete `/usr/local/Kobo/imageformats/libnfolders.so` and reboot.

Nothing else on the device is touched, and nothing is written to your library
database.

## Compatibility

**Verified on a Kobo Libra 2, firmware 4.38.23684.** Nothing else has been
tested.

This mod calls into Nickel's own code by symbol name, which means a firmware
that renames or reshapes those functions can break it. Every such call is
optional and NULL-checked, so a missing one degrades a feature rather than
failing to load — a firmware change is far more likely to cost you covers or
reading progress than to break the browser. `NOTES.md` records the measured
signature behind each one.

Two things to know before installing:

- **Deleting a book leaves Nickel's own library row behind** until Nickel
  rescans. The `rescan` button asks Nickel to do that, and warns you first,
  because Nickel's rescan also turns the Wi-Fi on.
- **Moving a book detaches its reading progress.** Nickel keys progress,
  bookmarks and collections to the file's path, so a moved book arrives at its
  destination looking unread. That is how Nickel stores it, not a choice this
  mod makes.

## Build

Mods **must** be built with
[NickelTC](https://github.com/pgaskin/NickelTC) — GCC 4.9.4 against Nickel's
own Qt 5.2.1. `./nickeltc` runs it in Docker.

```sh
git submodule update --init      # NickelHook
./nickeltc make                  # -> libnfolders.so
./nickeltc make koboroot         # -> KoboRoot.tgz
```

The display, ordering, listing and path-safety logic is deliberately free of
libnickel and Qt-GUI dependencies, so it can be tested on the host:

```sh
make test                        # ~2300 checks, host compiler and Qt5
```

Host Qt is 5.15 and the device's is 5.2.1, and the two are kept deliberately
skewed rather than pinned: an API that only exists in 5.15 then breaks the next
cross build loudly, at build time, instead of failing quietly on the panel.
That has already caught a real bug.

## How it works, briefly

The browser is one dialog built from Nickel's own chrome and its own tappable
row widget, pushed onto Nickel's window stack. `QDir` lists one directory at a
time — the folder tree needs no import step and no database query, because the
filesystem already is the tree. Per-file metadata (reading progress, covers,
dates) comes from Nickel through a small, deliberately centralised set of
calls, all in `nfnickel.cc`.

`NOTES.md` is the reverse-engineering record: the signature of every call, the
disassembly that establishes it, the candidates that were rejected and why, and
the wrong turns — including the ones that crashed the device. It is the most
useful thing here for anyone else writing a Nickel mod.

## Credits

- **[NickelHook](https://github.com/pgaskin/NickelHook)** and
  **[NickelTC](https://github.com/pgaskin/NickelTC)** by
  [pgaskin](https://github.com/pgaskin) — the framework and the toolchain this
  is built on.
- **[NickelMenu](https://github.com/pgaskin/NickelMenu)** by pgaskin (MIT) —
  where the gesture wiring and the signal-adaptor trick are explained by the
  author, which saved re-deriving both from a disassembler.
- **[NickelHardcover](https://codeberg.org/StrayRose/NickelHardcover)** by
  RedHatter/StrayRose (MIT) — the custom-screen pattern this browser's UI is
  built on.

## Licence

[MIT](LICENSE).

Not affiliated with, endorsed by, or supported by Rakuten Kobo. Installing mods
on your e-reader is at your own risk.
