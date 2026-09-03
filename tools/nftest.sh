#!/bin/sh
# Device-side spike driver, staged.
#
# The first run crashed Nickel, which on this device costs a reboot, so this
# version advances ONE libnickel call at a time and aborts the moment nickel's
# PID changes. A crash then names the call that caused it instead of leaving
# three suspects.
#
# ndbCurrentView (NickelDBus) is the oracle for "did the reader actually open":
# a log line only proves the call returned, and the question is whether Nickel
# NAVIGATED. Every qndb call is wrapped in a timeout because qndb blocks
# forever on dbus if nickel is gone.

BOOK='file:///mnt/onboard/books/Pratchett_ Terry - The Color of Magic_ A Discworld Novel.epub'
BAD='file:///mnt/onboard/books/DefinitelyNotABookThatExists.epub'

PID0=$(pidof nickel 2>/dev/null | head -1)

view()  { timeout 15 qndb -m ndbCurrentView 2>&1 | tail -1; }
modlog() { logread | grep -i 'NickelFolders)' | grep -v NickelHook | tail -"$1"; }
crashlog() { logread | grep -iE 'hindenburg|segfault|SIGSEGV' | tail -"$1"; }

alive() {
    P=$(pidof nickel 2>/dev/null | head -1)
    if [ "$P" != "$PID0" ]; then
        echo "!!!! nickel PID changed ($PID0 -> ${P:-gone}): IT CRASHED"
        echo "---- crash log:"
        crashlog 25
        return 1
    fi
    return 0
}

# fire <stage> <contentid> <label>
fire() {
    echo
    echo "###### stage $1: $3"
    printf '%s\n%s\n%s\n' "$2" "" "$1" > /tmp/nfolders-open
    sleep 5
    modlog 8
    if ! alive; then exit 1; fi
    echo "-- nickel alive (pid $PID0), view: $(view)"
}

echo "###### 0. install + init"
ls -la /usr/local/Kobo/imageformats/libnfolders.so 2>&1
echo "-- init log:"
logread | grep -i 'NickelFolders' | grep -iE 'resolving|dlsym|init:|failsafe: info: restoring' | tail -12
echo "-- nickel pid: $PID0"
echo "-- view: $(view)"

fire 1 "$BOOK" "getById only, real book"
fire 2 "$BAD"  "getById + isValid, UNKNOWN book (expect isValid=false)"
fire 2 "$BOOK" "getById + isValid, real book (expect isValid=true)"
fire 3 "$BOOK" "+ construct ReadBookActionProxy"

echo
echo "###### Q4 setup: go home so the back-destination is known"
timeout 15 qndb -m mwcHome 2>&1 | tail -1
sleep 3
echo "-- view before: $(view)"

fire 4 "$BOOK" "+ onSelected() -- this is the whole question"

echo
echo "###### done, nickel survived everything"
