# NickelFolders

A folder-navigating book browser for Kobo e-readers that runs **alongside**
Nickel and opens the chosen book in the **stock reader** — no Nickel shutdown,
no replacement reading app.

**Status: the spike is answered — yes, this works.** Verified on a Kobo Libra 2
(firmware 4.38.23684) on 2026-09-03: an injected mod handed a sideloaded book's
ContentID to Nickel and the stock reader opened it, with Nickel staying up
throughout. `NOTES.md` has the measurements and the one row a finger still has
to confirm.

Nothing here browses anything yet. What exists is the probe.

## The question

Kobo's library has no folder tree. The books are in folders on the card, Nickel
imports them recursively and keeps the full path, and then shows a flat list
grouped by author or series. KOReader solves this by replacing the reader, which
is the wrong trade if you like Kobo's reader.

So: can something outside Nickel's own UI hand it a book and have it open
normally?

Neither of the two established control surfaces can. **NickelDBus** exposes 64
methods, none of which open a book. **NickelMenu**'s `nickel_open` takes views
only, never a book. That leaves an injected mod — and Nickel does export the
pieces, including the exact object behind the library's own **Read** button.
`NOTES.md` has the derivation.

## What the spike does

`nfolders.cc` is a [NickelHook](https://github.com/pgaskin/NickelHook) mod that
watches `/tmp/nfolders-open` and, when a ContentID appears there, runs the
library-tap sequence on it: `VolumeManager::getById` → `ReadBookActionProxy` →
`onSelected()`. It draws nothing and adds no menu item.

It settled four things that only a device could answer — all four now measured,
see `NOTES.md`:

1. Does `getById` return a valid `Volume` for a **sideloaded** ContentID, and
   what does its second argument (`dbName`) want? The spike takes that as a
   runtime parameter so it can be tried without a rebuild.
2. Does the proxy constructor accept an arbitrary `QObject` as parent?
3. Does `onSelected()` actually navigate to the reader?
4. Where does **back** go afterwards — to a caller's view, or to Nickel's home?

Question 4 is the one that decides the shape of the real thing: whether the
browser can be a screen you return to, or whether it is a one-shot launcher.

## Building

Needs Docker (for [NickelTC](https://github.com/pgaskin/NickelTC), the only
toolchain NickelHook mods should be built with — GCC 4.9 against Nickel's own
Qt 5.2.1) and the `NickelHook` submodule.

```sh
git submodule update --init
./nickeltc make            # -> libnfolders.so
./nickeltc make koboroot   # -> KoboRoot.tgz
```

## Running it

Installing means copying `KoboRoot.tgz` to `.kobo/` on the device and letting
it reboot, which is how every Nickel mod is installed.

```sh
# with the device mounted over USB, or over ssh to /mnt/onboard/.kobo/
cp KoboRoot.tgz /mnt/onboard/.kobo/
# eject; the device reboots and extracts it

# then, over ssh, with Nickel up:
echo 'file:///mnt/onboard/books/it/Some Book.epub' > /tmp/nfolders-open
logread | grep -i nickelfolders
```

A bare path works too — the scheme is added if it is missing. The trigger file
takes up to three lines: the ContentID, then `getById`'s `dbName` (blank is
correct for local content), then how far to go — `1` `getById`, `2` `+ isValid`,
`3` `+ construct the proxy`, `4` `+ onSelected` (the default).

Those stages are not decoration. A wrong guess about any of these signatures
takes Nickel down, which on this device costs a reboot, and the first run
proved it: with all three calls behind one trigger, the crash pointed at all
three at once. Advance one rung at a time when a signature is unverified.

### Backing out

Two independent routes, which is the reason to be willing to try this at all:

- **Uninstall flag.** Create `/mnt/onboard/nfolders_uninstall` (an empty file is
  enough) and reboot. The mod deletes itself and the flag.
- **Delete the library.** Remove
  `/usr/local/Kobo/imageformats/libnfolders.so` and reboot.

NickelHook also carries a failsafe: if the mod fails to initialise it disarms
itself and dumps the log to `/mnt/onboard/.kobo/`, rather than taking Nickel
down. It is worth knowing that the failsafe is shared infrastructure — a mod
that fails init hard can cause *other* NickelHook mods to remove themselves
too, which is why this one treats a failed thread start as non-fatal.

## What comes next

The browser is the next project, and there is a real fork in it:

- **A native Nickel screen**, pushed onto the window stack via
  `MainWindowController::push`. Touch, e-ink refresh, fonts and the back gesture
  all come free, and it can reuse Nickel's own folder widgets (`NOTES.md`).
  More archaeology, best result.
- **An FBInk overlay** drawn by a separate process. Reuses a lot of
  [koboy](../koboy)'s existing folder-browsing list widget, but the handoff
  still needs this mod, so it is the hard part *plus* a second UI.

There is also a **zero-C++ fallback** worth trying before either: generate one
Nickel collection per folder by writing the `Shelf`/`ShelfContent` tables from
the paths already in `content`, and use the stock Collections view. Flat rather
than a tree, and it needs re-running when books are added, but it needs no
injection at all and survives firmware updates.

## Licence

GPL-3, matching NickelHook's ecosystem. Nickel itself is not modified on disk;
the mod is loaded as a Qt image-format plugin, which is the standard mechanism
for this and is what makes it removable by deleting one file.
