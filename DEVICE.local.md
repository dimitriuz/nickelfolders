# The test device

Connection details are NOT in git. They live in `device.local` in this
directory, which `.gitignore` excludes and `tools/kobo.py` reads:

```
host=<lan ip>
pass=<root password>
panel=<visible WxH>
```

`KOBO_HOST`, `KOBO_PASS` and `KOBO_PANEL` override it.

If `device.local` is missing, the reference device is a **Kobo Libra 2**
(Mark 9, firmware 4.38.23684) on the LAN over dropbear as `root`, with the
stock developer password. Its IP is DHCP and **has changed before** -- ping it
before assuming. Ask the owner rather than guessing.

The device already has NickelMenu, NickelDBus, kfmon, KOReader and koboy
installed. That matters twice over: `qndb` is available as an oracle, and a
NickelHook failsafe trip can make those other mods uninstall themselves too.
