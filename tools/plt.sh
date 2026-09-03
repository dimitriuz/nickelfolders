#!/bin/sh
# Resolve an ARM PLT stub address in libnickel to the symbol it jumps to.
#
# THIS IS THE MOST IMPORTANT TOOL IN THE REPO. Reading argument registers off a
# disassembly tells you a register holds a pointer; only the relocation tells
# you WHAT KIND of pointer. Assuming instead of resolving is what crashed
# Nickel once already -- see NOTES.md.
#
#   tools/plt.sh libnickel.so.1.0.0 0x675714 0x66e350
#   0x675714  -> 0x16ac660  _ZN8QVariantC1ERK7QString
#   0x66e350  -> 0x16aa138  _ZN6Device23getCurrentDeviceMutableEv
#
# A stub is three instructions:
#     add ip, pc, #<a>      ; pc here is that instruction's address + 8
#     add ip, ip, #<b>
#     ldr pc, [ip, #<c>]!
# so the GOT slot is (first add's address + 8) + a + b + c.
#
# The triple is NOT always at the address you were given: a call from Thumb
# code can land on a `bx pc; nop` veneer first, putting the real stub 4 bytes
# later. So this FINDS the triple in a window instead of assuming where it
# starts -- getting that wrong silently computes a slot that resolves to the
# wrong symbol, or to none.

set -e
LIB="$1"; shift
: "${OBJDUMP:=arm-linux-gnueabihf-objdump}"

for A in "$@"; do
    ADDR=$(printf '%d' "$A")
    SLOT=$("$OBJDUMP" -d --start-address="$ADDR" --stop-address="$((ADDR + 32))" "$LIB" 2>/dev/null \
        | python3 -c '
import re, sys

rows = []
for line in sys.stdin:
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 3:
        continue
    m = re.match(r"\s*([0-9a-f]+):$", parts[0])
    if not m:
        continue
    rows.append((int(m.group(1), 16), "\t".join(parts[2:]).strip()))

def imm(text):
    # Prefer the "; 0x..." comment objdump prints for rotated immediates,
    # because the raw "#16, 12" form is a value/rotation pair, not a number.
    c = text.split(";")
    if len(c) > 1:
        try:
            return int(c[1].strip().split()[0], 16)
        except ValueError:
            pass
    m = re.search(r"#(\d+)", text)
    return int(m.group(1)) if m else None

for i in range(len(rows) - 2):
    (aa, ai), (ba, bi), (ca, ci) = rows[i], rows[i + 1], rows[i + 2]
    if not (ba == aa + 4 and ca == aa + 8):
        continue
    if not (ai.startswith("add") and "pc" in ai and bi.startswith("add")
            and ci.startswith("ldr") and "pc" in ci):
        continue
    v = [imm(ai), imm(bi), imm(ci)]
    if None in v:
        continue
    print("%x" % (aa + 8 + v[0] + v[1] + v[2]))
    break
')
    if [ -z "$SLOT" ]; then
        printf '%-11s -> %s\n' "$A" "<no PLT stub found within 32 bytes>"
        continue
    fi
    NAME=$("$OBJDUMP" -R "$LIB" 2>/dev/null | grep -i "^0*$SLOT " | awk '{print $3}')
    printf '%-11s -> 0x%-10s %s\n' "$A" "$SLOT" "${NAME:-<unresolved>}"
done
