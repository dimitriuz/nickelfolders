#!/bin/sh
# Resolve an ARM PLT stub address in libnickel to the symbol it jumps to.
#
# THIS IS THE MOST IMPORTANT TOOL IN THE REPO. Reading argument registers off a
# disassembly tells you a register holds a pointer; only the relocation tells
# you WHAT KIND of pointer. Assuming instead of resolving is what crashed
# Nickel once already -- see NOTES.md.
#
#   tools/plt.sh libnickel.so.1.0.0 0x675714
#   -> 0x675714 -> 0x16ac660 _ZN8QVariantC1ERK7QString
#
# An ARM PLT stub is three instructions:
#     add ip, pc, #<a>      ; pc here is stub+8
#     add ip, ip, #<b>
#     ldr pc, [ip, #<c>]!
# so the GOT slot is (stub + 8) + a + b + c.

set -e
LIB="$1"; shift
: "${OBJDUMP:=arm-linux-gnueabihf-objdump}"

for A in "$@"; do
    ADDR=$(printf '%d' "$A")
    D=$("$OBJDUMP" -d --start-address="$ADDR" --stop-address="$((ADDR + 12))" "$LIB" 2>/dev/null \
        | grep -E '\badd\b|\bldr\b')
    A1=$(echo "$D" | sed -n '1p' | sed 's/.*; //; s/[^0-9a-fx].*//')
    A2=$(echo "$D" | sed -n '2p' | sed 's/.*; //; s/[^0-9a-fx].*//')
    A3=$(echo "$D" | sed -n '3p' | sed 's/.*!.*;[[:space:]]*//; s/[^0-9a-fx].*//')
    SLOT=$(python3 -c "print('%x' % ($ADDR + 8 + $A1 + $A2 + $A3))")
    NAME=$("$OBJDUMP" -R "$LIB" 2>/dev/null | grep -i "^0*$SLOT " | awk '{print $3}')
    printf '%-12s -> 0x%-10s %s\n' "$A" "$SLOT" "${NAME:-<unresolved>}"
done
