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
# teardown, the FIFO, the restore-on-any-exit net -- is copied from
# ../../koboy/scripts/koboy.sh, which already solved "how do you safely hand
# Nickel back its own process slot" for the same device. Copied, not
# reinvented: koboy.sh's own header says "Everything hard about this script
# is the way back," and that is equally true here. Read that file before
# changing this one.

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
#
# $LOG deliberately lives NEXT TO THIS SCRIPT, under /mnt/onboard, so it is
# readable over ssh after the fact -- but that is safe only because every
# write to it (via log(), below) opens, appends and closes within a single
# shell builtin, never held open. The two backgrounded processes THIS
# script launches (hindenburg/nickel, near the bottom) must NOT inherit an
# fd on this file for exactly that reason -- see the comment at the
# relaunch itself.
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
#
# The `env -u LD_LIBRARY_PATH dhcpcd` below only makes sense if
# LD_LIBRARY_PATH IS already present in this script's own environment by
# the time wifi_down runs -- unsetting a variable that was never set is a
# no-op, so this line is itself evidence for the reconciliation in the
# relaunch section's own comment, below: NickelMenu's `cmd_spawn` does not
# sanitise the environment it hands to a spawned command, so Nickel's own
# rcS-exported LD_LIBRARY_PATH=/usr/local/Kobo most likely IS already
# inherited this far, and this line strips it back off before running
# dhcpcd specifically (not independently confirmed here why dhcpcd would
# mind it, but koboy.sh's own copy does the same thing).
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

# ------------------------------------------------------------------- restore
#
# Idempotent (the `restored` guard), because it is invoked from the EXIT
# trap below NO MATTER HOW this script's own body ends -- normal
# completion, an early `exit`, or a signal -- and koboy.sh's own comment on
# why is copied verbatim because the reasoning is unchanged here: "the
# alternative is a user staring at a dead panel," and this script's own
# risk window is worse than a quiet script that merely forgot to relaunch
# something -- between "stop Nickel" (below) and this function running,
# BOTH nickel and hindenburg are dead, and koboy.sh's own measurement for
# that state left too long is a watchdog reboot ("PMU2: Watchdog timeout
# triggered"). So restore() is the ONLY place Nickel comes back, and it
# runs unconditionally on exit rather than being called once, hopefully,
# at the end of a script that might not reach its end.
restored=""
restore() {
    [ -n "$restored" ] && return 0
    restored=1

    # LD_LIBRARY_PATH is exported here as DEFENSE IN DEPTH, not because it
    # is surely absent: see wifi_down's own comment, above, for why this
    # script's environment most likely already carries it down from
    # Nickel's own rcS export. Exporting it again removes any doubt for
    # the one call that actually matters -- Nickel itself will not start
    # without it (rcS:325, libQtSolutions_IOCompressor-2.3.so.1). The two
    # GStreamer variables are koboy.sh's own audio-path exports, copied so
    # a Nickel restarted this way matches a freshly booted one for TTS and
    # Bluetooth output, not just for shared-library resolution.
    export LD_LIBRARY_PATH=/usr/local/Kobo
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK="alsasink"
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER="bluealsa:DEV=00:00:00:00:00:00"

    # Back to / before Nickel is started -- copied from koboy.sh's own
    # restore(), same measured reason: a stale working directory (or
    # OLDPWD) left on the user partition is what makes USB mass storage
    # misbehave later, because the partition cannot be unmounted while a
    # process holds a directory on it open. Do not rely on NickelMenu
    # happening to hand this script a safe cwd.
    cd / 2>/dev/null || true
    unset OLDPWD

    wifi_down

    # rcS creates this before Nickel starts and udev writes device events
    # into it; Nickel is the reader. It must exist again before Nickel
    # starts looking for it, and it is recreated fresh (not just left
    # alone) because a FIFO with a reader that went away can be left in a
    # state a new reader does not expect -- same reasoning, same
    # recreation, as koboy.sh's own restore() step 5.
    rm -f /tmp/nickel-hardware-status
    mkfifo /tmp/nickel-hardware-status 2>>"$LOG" || log "restore: WARNING mkfifo failed"

    sync

    # hindenburg first: koboy.sh's own comment records that leaving it dead
    # while Nickel is missing gets the device rebooted out from under you
    # by the watchdog. Then Nickel itself, with the exact flags rcS starts
    # it with. Then udevadm trigger, to replay the device events Nickel
    # missed while it was down (radio-down included).
    #
    # Both backgrounded processes redirect to /dev/null, NOT $LOG, and this
    # is load-bearing, not a style choice: $LOG lives under
    # /mnt/onboard/.adds/nfolders, and a background process holding a
    # write fd open on it for its ENTIRE LIFETIME -- these two run until
    # the next reboot -- is exactly the corruption risk CLAUDE.md names
    # ("never hold a file handle on /mnt/onboard for more than a few
    # hundred milliseconds -- a USB session while one is open risks
    # corruption"). Nickel is also the process that has to cleanly unmount
    # that partition to export it over USB, and it cannot do that with its
    # own stdout/stderr fd still pinned open on a file living on it. This
    # script's OWN log() calls are safe (open/append/close per line,
    # nothing held), which is why $LOG can live here at all and still be
    # readable over ssh afterward -- only these two long-lived children may
    # not inherit an fd on it. `udevadm trigger` is a one-shot command that
    # exits immediately, so a $LOG redirect for it does not hold anything
    # open past this function returning.
    log "restore: starting hindenburg and nickel"
    /usr/local/Kobo/hindenburg >/dev/null 2>&1 &
    LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad >/dev/null 2>&1 &
    udevadm trigger >>"$LOG" 2>&1 &

    log "restore: done"
}

finish() {
    restore
    rm -rf "$LOCK"
}

# Unconditional, same as koboy.sh's own `trap 'finish' EXIT`: a normal
# finish, a crash, a kill -TERM -- Nickel comes back either way. INT/TERM
# get their OWN traps, each ending in an explicit `exit`, because a POSIX
# signal handler that does not exit returns control to whatever the script
# was doing and CONTINUES running -- with the lock already removed by
# `finish` above, which is precisely the double-start the lock exists to
# prevent (a second NickelMenu tap could then take the now-free lock while
# this instance is still limping toward its own relaunch). The exit codes
# (130 = 128+SIGINT, 143 = 128+SIGTERM) match koboy.sh's own convention.
trap 'finish' EXIT
trap 'log "signal INT";  finish; exit 130' INT
trap 'log "signal TERM"; finish; exit 143' TERM

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
#
# koboy.sh stops here: it warns and "continues anyway" if Nickel outlives
# the wait. Measured on THIS device, twice, that is not actually safe --
# Nickel catches SIGTERM (it is a Qt app with its own shutdown path:
# session save, bookmark flush, DB close) and can still be mid-shutdown
# when koboy.sh's identical wait gives up. Both runs here logged exactly
# that ("still alive after 10s, continuing anyway") and it happened to
# resolve to one Nickel both times -- but "happened to" is not a safety
# property, and starting a second nickel on top of a first that is still
# holding the framebuffer, the input devices and a write handle on
# /mnt/onboard is a strictly worse state than this script refusing to
# proceed. So: escalate instead of hoping, in two more steps below.
log "stopping Nickel"
NICKEL_KILLNAMES="nickel hindenburg sickel fickel strickel fontickel adobehost foxitpdf iink"
killall -q -TERM $NICKEL_KILLNAMES

# 10s / 40 * 250ms, UNCHANGED from koboy.sh's own budget: that figure is
# sized for a real, catchable-signal shutdown to run to completion (the Qt
# event loop processing the signal, flushing state, tearing down its own
# windows), and koboy.sh's own measurements are the only precedent this
# project has for how long that legitimately takes. There is no basis here
# for shortening it -- doing so would just turn "graceful shutdown that is
# slightly slow today" into "escalates to SIGKILL more often for no gain."
i=0
while pkill -0 nickel 2>/dev/null; do
    [ "$i" -ge 40 ] && break
    usleep 250000 2>/dev/null || sleep 1
    i=$((i + 1))
done

if ! pkill -0 nickel 2>/dev/null; then
    log "Nickel stopped after SIGTERM, $((i * 250))ms"
else
    # SIGTERM did not finish the job inside its budget. SIGKILL cannot be
    # caught, blocked or ignored -- the kernel delivers it unconditionally
    # -- so unlike the wait above, this second wait is not accommodating a
    # shutdown ROUTINE, only the kernel's own teardown of the process (and,
    # rarely, a process stuck in an uninterruptible D-state on flash I/O
    # that even SIGKILL cannot interrupt until the I/O completes). 8 * 250ms
    # = 2s is generous for that and far short of the 10s above on purpose:
    # if Nickel is still there after being SIGKILLed, waiting longer buys
    # nothing, because a process that ignores an uncatchable signal past a
    # couple of seconds is not going to disappear on its own.
    log "WARNING nickel still alive after SIGTERM+${i}*250ms, escalating to SIGKILL"
    killall -q -KILL $NICKEL_KILLNAMES
    j=0
    while pkill -0 nickel 2>/dev/null; do
        [ "$j" -ge 8 ] && break
        usleep 250000 2>/dev/null || sleep 1
        j=$((j + 1))
    done

    if ! pkill -0 nickel 2>/dev/null; then
        log "Nickel stopped after SIGKILL, $((j * 250))ms"
    else
        # Still here. This is the one outcome this script refuses to paper
        # over: Nickel is ALIVE (not stopped, not "probably fine") and this
        # script has no third escalation to offer. Starting a new
        # hindenburg/nickel now would be the exact two-Nickels-at-once state
        # the whole escalation above exists to avoid, so this path does NOT
        # fall through to the trap's normal relaunch.
        #
        # That is deliberately safe rather than deliberately unhelpful: the
        # dangerous outcome of a dev script is a dead panel with nothing
        # backing it, and this is not that -- Nickel is still running, so
        # there is still a UI, just not a freshly-loaded one. Exiting here
        # leaves the device in the same usable state it was in before this
        # script was ever run.
        #
        # `restored=1` before exiting is what keeps the EXIT trap (below)
        # from undoing that safety: `finish` calls `restore`, and `restore`
        # is guarded to run at most once (see its own idempotency comment,
        # above) -- setting the guard here makes that call a deliberate
        # no-op instead of touching it. The trap's OTHER job, `rm -rf
        # "$LOCK"`, is untouched and still runs, so a later, cleanly-run
        # attempt is not left blocked by this one. Every path that DID
        # successfully kill Nickel (both branches above) leaves `restored`
        # unset, so for those the trap's relaunch behaves exactly as it did
        # before this change.
        log "FATAL nickel survived SIGTERM and SIGKILL after $((i * 250))ms + $((j * 250))ms."
        log "      Nickel is still running, so the panel still has a UI. Refusing"
        log "      to start a second one on top of it. NOT relaunching."
        restored=1
        echo "restart-nickel.sh: nickel survived SIGKILL -- refusing to start a second one. See $LOG." >&2
        log "=== restart-nickel.sh aborting, rc=1 (old nickel left running, untouched)"
        exit 1
    fi
fi

# Nothing else to do in the main body: restore() (above) is what brings
# Nickel back, and it runs from the EXIT trap when this script reaches its
# own end, exactly like koboy.sh's own `exit "$rc"` fires its `finish` trap.
log "=== restart-nickel.sh reaching its own end, handing off to the EXIT trap"
