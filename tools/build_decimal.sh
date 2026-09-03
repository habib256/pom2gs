#!/usr/bin/env bash
# POMIIGS — assemble Bruce Clark's decimal test (tests/cycle_accuracy/bruce_decimal.s)
# into a raw image for tests/klaus_test.cpp. Needs cc65 (ca65 + ld65).
#   tools/build_decimal.sh [out-dir]   → out-dir/bruce_decimal.bin + .lbl
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/tests/cycle_accuracy/cache/klaus}"
command -v ca65 >/dev/null && command -v ld65 >/dev/null || { echo "ca65/ld65 (cc65) not found" >&2; exit 77; }
mkdir -p "$OUT"
CFG="$OUT/bruce_decimal.cfg"
cat > "$CFG" <<'CFGEOF'
MEMORY {
    ZP:   start = $0000, size = $0100, type = rw;
    RAM:  start = $0200, size = $FE00, file = %O, fill = yes, fillval = $00;
}
SEGMENTS {
    ZEROPAGE: load = ZP,  type = zp;
    CODE:     load = RAM, type = ro;
}
CFGEOF
ca65 "$HERE/tests/cycle_accuracy/bruce_decimal.s" -o "$OUT/bruce_decimal.o"
ld65 -C "$CFG" -o "$OUT/bruce_decimal.img" -Ln "$OUT/bruce_decimal.lbl" "$OUT/bruce_decimal.o"
# The image starts at $0200: prepend the zero page/stack area so it loads at 0.
{ head -c 512 /dev/zero; cat "$OUT/bruce_decimal.img"; } > "$OUT/bruce_decimal.bin"
echo "built $OUT/bruce_decimal.bin ($(wc -c < "$OUT/bruce_decimal.bin") bytes)"
