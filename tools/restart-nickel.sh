#!/bin/sh
# DEVELOPMENT-ONLY: restart Nickel in place, without rebooting the device.
#
# The point of this script is entirely to remove the reboot from the
# iteration loop: `./nickeltc make` -> push the new libnfolders.so -> run
# this from NickelMenu -> the freshly-loaded .so is live, no power cycle.
# It is not something a shipped mod installs or a user ever runs -- see
# CLAUDE.md's device-workflow section for why it stays dev-only and what to
# check the first time it is used.
#
# Everything hard about this script -- the environment gate below, the wifi
# teardown, the FIFO -- is copied from ../../koboy/scripts/koboy.sh, which
# already solved "how do you safely hand Nickel back its own process slot"
# for the same device. Copied, not reinvented: koboy.sh's own header says
# "Everything hard about this script is the way back," and that is equally
# true here. Read that file before changing this one.

# Kobo's own rcS writes PATH with a trailing colon, which makes the empty
# last element mean "the current directory" -- not copied here, since this
# script runs as root with NickelMenu's own (or Nickel's) working directory,
# not necessarily one that is safe to implicitly search. Same fix as
# koboy.sh: spell PATH out explicitly.
PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib"

DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DIR=$(dirname "$0")
LOG="$DIR/restart-nickel.log"

# One rotation, so a script exercised many times over a long dev session
# does not fill the user partition with a log nobody is reading. Same
# threshold as koboy.sh, for no reason beyond "it already picked a
# reasonable number."
if [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 262144 ]; then
    mv -f "$LOG" "$LOG.1"
fi

log() { echo "$(date '+%F %T') $*" >>"$LOG"; }

log "=== restart-nickel.sh start (pid $$, dir $DIR)"

# ------------------------------------------------------------ environment gate
#
# THE most important check in this file, copied from koboy.sh's own gate
# essentially verbatim, because the failure mode it prevents is IDENTICAL
# here: this script's whole job is stopping and relaunching Nickel, and
# doing that from a process that did not inherit Nickel's own environment
# is what corrupted THIS device once already.
#
# rcS exports PLATFORM and PRODUCT (from /bin/kobo_config.sh) and NICKEL_HOME
# before Nickel is started -- which is exactly what a launch from
# NickelMenu gives us, because NickelMenu is itself spawned from inside
# Nickel, and inherits Nickel's own environment down to this script. A shell
# reached over ssh has NONE of that (measured on this project's own device:
# HOME, LOGNAME, PATH, PWD, SHELL, SHLVL, SSH_*, USER and nothing else).
#
# MEASURED on this exact device, the one time this went wrong (recorded in
# koboy.sh and repeated here because it is the reason this gate exists, not
# a hypothetical): a Nickel started WITHOUT this environment rewrote
# /mnt/onboard/.kobo/version --
#
#   before  N4XXXXXXXXXXX,4.1.15,4.38.23684,...,00000000-...-000000000388
#   after   11:22:33:44:55:66,4.1.15,4.38.23684,...,
#
# -- replacing the real serial with the placeholder "11:22:33:44:55:66" and
# leaving the trailing field EMPTY. That file is how FBInk identifies the
# device, so every FBInk-based tool afterward (koboy and KOReader both)
# reported deviceName='Unknown!' and lost its per-device quirks, and only a
# reboot repaired it. If /mnt/onboard/.kobo/version ever shows that exact
# signature after this script runs, that is what happened, and CLAUDE.md's
# device-workflow section says to check for it after this script's first
# use for exactly that reason.
#
# So: an ssh launch is refused HERE, before Nickel is touched at all, rather
# than being allowed to relaunch Nickel with a partial environment and risk
# doing that again.
#
# KNOWN LIMITATION, stated as plainly as koboy.sh states it: this is
# trivially spoofable (`export PLATFORM=... PRODUCT=... NICKEL_HOME=...`
# before running this script by hand gets straight past it), and no check
# in userspace can do better -- there is nothing a process can consult to
# prove who its parent really was. What this gate buys is that the ORDINARY
# ssh launch, which is how the corruption above actually happened, now
# refuses. It is not a defence against someone deliberately impersonating
# Nickel's environment.
missing=""
for v in PLATFORM PRODUCT NICKEL_HOME; do
    eval "val=\${$v}"
    [ -n "$val" ] || missing="$missing $v"
done

if [ -n "$missing" ]; then
    log "REFUSED: not launched from Nickel; missing env:$missing"
    log "         Nickel is untouched and still running. Run this from"
    log "         NickelMenu (the 'Restart Nickel (dev)' item this repo's"
    log "         tools/nickelmenu-nfolders.cfg adds), not from a shell."
    echo "restart-nickel.sh: refusing -- missing env:$missing" >&2
    echo "Launch this from NickelMenu, not a shell. Nickel was left running." >&2
    log "=== restart-nickel.sh refused, rc=1"
    exit 1
fi

log "env ok: PLATFORM=$PLATFORM PRODUCT=$PRODUCT NICKEL_HOME=$NICKEL_HOME"
log "        WIFI_MODULE=${WIFI_MODULE:-unset} INTERFACE=${INTERFACE:-unset}"

# ----------------------------------------------------------------- one at once
#
# Same reasoning as koboy.sh's own lock: NickelMenu spawns a second copy
# without complaint if the entry is tapped twice, and on e-ink a double tap
# is likely -- the menu closes silently and nothing visible happens for a
# second or two. Two copies of this script racing to kill and relaunch
# Nickel is worse than koboy's own case (two koboys fighting over the
# framebuffer): it risks starting Nickel twice. mkdir is the atomic claim;
# a plain -e test is not.
LOCK=/tmp/nfolders-restart.lock
if ! mkdir "$LOCK" 2>/dev/null; then
    other=$(cat "$LOCK/pid" 2>/dev/null)
    if [ -n "$other" ] && [ -d "/proc/$other" ]; then
        log "REFUSED: restart-nickel.sh is already running as pid $other"
        exit 1
    fi
    log "clearing a stale lock left by pid ${other:-unknown}"
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || { log "REFUSED: cannot take $LOCK"; exit 1; }
fi
echo $$ >"$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT INT TERM

# --------------------------------------------------------------- stop Nickel
#
# By name, via killall, and deliberately not `pkill -f /usr/local/Kobo/
# nickel`: a pattern passed to pkill -f matches the command line of the
# shell running IT too, so that form would kill this script's own launcher
# before it kills Nickel. Same name list as koboy.sh's own "stopping
# Nickel" step -- copied rather than trimmed to just "nickel", because
# koboy.sh already worked out which of Kobo's own alternate reader/DRM
# helper binaries can be running alongside it on some firmware/device
# combinations, and a stale one of those left running is exactly the kind
# of thing that makes a "restart" not actually be one.
log "stopping Nickel"
killall -q -TERM nickel hindenburg sickel fickel strickel fontickel \
                 adobehost foxitpdf iink
i=0
while pkill -0 nickel 2>/dev/null; do
    [ "$i" -ge 40 ] && { log "WARNING nickel still alive after 10s, continuing anyway"; break; }
    usleep 250000 2>/dev/null || sleep 1
    i=$((i + 1))
done
log "Nickel stopped after $((i * 250))ms"

# --------------------------------------------------------------------- wifi
#
# Copied from koboy.sh's own wifi_down, unchanged in shape: Nickel's own
# WiFi state machine expects to bring the radio up itself as it starts, and
# koboy.sh's own comment records what happens if it is left up instead (the
# restarted process fails to insert the module with "File exists" and the
# device reboots itself a few minutes later). NFOLDERS_KEEP_WIFI is this
# script's own escape hatch for exactly the same development case koboy.sh's
# KOBOY_KEEP_WIFI exists for -- testing over an ssh session that is itself
# running over the radio this would otherwise take down -- and is
# deliberately a DIFFERENT variable name: it has nothing to do with koboy's
# own runs and must not be confused for a setting that affects them.
wifi_down() {
    if [ -z "$WIFI_MODULE" ]; then
        log "restart: WARNING WIFI_MODULE is not set, so the WiFi teardown was"
        log "         SKIPPED. If the radio is up, the restarted Nickel may try"
        log "         to load its own driver, fail with 'File exists', and the"
        log "         device may reboot itself a few minutes later. Set"
        log "         WIFI_MODULE in the environment (see /proc/modules) if"
        log "         that happens."
        return 0
    fi
    if ! grep -q "^$WIFI_MODULE" /proc/modules 2>/dev/null; then
        log "restart: WiFi module $WIFI_MODULE is not loaded, nothing to tear down"
        return 0
    fi
    if [ "$NFOLDERS_KEEP_WIFI" = "1" ]; then
        log "restart: NFOLDERS_KEEP_WIFI=1, leaving the radio up (development only)"
        return 0
    fi
    log "restart: taking WiFi down ($WIFI_MODULE on ${INTERFACE:-wlan0})"
    if [ -x /sbin/dhcpcd ]; then
        env -u LD_LIBRARY_PATH dhcpcd -d -k "${INTERFACE:-wlan0}" >/dev/null 2>&1
    fi
    killall -q -TERM udhcpc default.script dhcpcd dhcpcd-dbus
    wpa_cli terminate >/dev/null 2>&1
    ifconfig "${INTERFACE:-wlan0}" down 2>/dev/null
    usleep 250000 2>/dev/null || sleep 1
    rmmod "$WIFI_MODULE" 2>/dev/null
    if grep -q '^sdio_wifi_pwr' /proc/modules 2>/dev/null; then
        usleep 250000 2>/dev/null || sleep 1
        rmmod sdio_wifi_pwr 2>/dev/null
    fi
}
wifi_down

# ------------------------------------------------------- the hardware-status FIFO
#
# rcS creates this before Nickel starts and udev writes device events into
# it; Nickel is the reader. It must exist again before Nickel starts
# looking for it, and it is recreated fresh (not just left alone) because a
# FIFO with a reader that went away can be left in a state a new reader does
# not expect -- same reasoning, same recreation, as koboy.sh's own restore()
# step 5.
rm -f /tmp/nickel-hardware-status
mkfifo /tmp/nickel-hardware-status 2>>"$LOG" || log "restart: WARNING mkfifo failed"

sync

# ------------------------------------------------------------------- relaunch
#
# LD_LIBRARY_PATH is the one late rcS export (rcS:325) a launch from inside
# Nickel does not already carry down to a spawned child the way PLATFORM/
# PRODUCT/NICKEL_HOME do: Nickel's own Qt libraries (including
# libQtSolutions_IOCompressor-2.3.so.1) live in /usr/local/Kobo, and without
# this Nickel dies immediately on that missing library.
#
# hindenburg first: koboy.sh's own comment records that leaving it dead
# while Nickel is missing gets the device rebooted out from under you by
# the watchdog ("PMU2: Watchdog timeout triggered"). Then Nickel itself,
# with the exact flags rcS starts it with. Then udevadm trigger, to replay
# the device events Nickel missed while it was down (radio-down included).
export LD_LIBRARY_PATH=/usr/local/Kobo

log "restart: starting hindenburg and nickel"
/usr/local/Kobo/hindenburg >>"$LOG" 2>&1 &
LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"$LOG" 2>&1 &
udevadm trigger >>"$LOG" 2>&1 &

log "=== restart-nickel.sh done, rc=0"
