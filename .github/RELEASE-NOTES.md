<!--
REWRITE THE TOP SECTION BEFORE YOU TAG. Everything below "## Install" is
stable boilerplate and can stay as it is.

This comment is stripped by the release workflow before publishing. That
strip is not cosmetic: GitHub HIDES html comments when it renders markdown in
a browser, but the API, the RSS feed and the release notification email all
carry the RAW body -- so a maintainer note like this one reads fine on the
release page and still goes out to everyone watching the repo. koboy shipped
exactly that in its v0.5.4.

The workflow also refuses to publish if this file does not mention the tag
version, so stale notes fail the build rather than shipping quietly.
-->

## NickelFolders VERSION

What changed in this release.

## Install

1. Connect the Kobo over USB.
2. Copy `KoboRoot.tgz` into the `.kobo` folder on the device.
3. Eject. The Kobo reboots and installs it; the file deletes itself.

NickelFolders has no menu entry of its own — open it from
[NickelMenu](https://github.com/pgaskin/NickelMenu):

```
menu_item :main :NickelFolders :cmd_spawn :quiet:/bin/touch /tmp/nfolders-native
```

To uninstall, create a file called `nfolders_uninstall` in the root of the card
and reboot, or delete `/usr/local/Kobo/imageformats/libnfolders.so`.

## Before you install

Built and tested against a **Kobo Libra 2 on firmware 4.38.23684**. Other
models and firmwares are untested.

This mod calls into Nickel by symbol name. Every such call is optional and
NULL-checked, so a firmware that renames one costs a feature rather than
stopping the browser from loading — but it is why the tested firmware is
stated rather than implied.

Two behaviours worth knowing before you use the file operations:

- **Deleting a book leaves Nickel's library row behind** until Nickel rescans.
  The `rescan` button asks it to, and warns first, because Nickel's rescan also
  turns the Wi-Fi on.
- **Moving a book detaches its reading progress.** Nickel keys progress,
  bookmarks and collections to the file's path.

Verify downloads with `SHA256SUMS`.
