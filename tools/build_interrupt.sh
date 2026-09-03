#!/usr/bin/env bash
# POMIIGS — translate and assemble Klaus Dormann's 6502 interrupt test for
# tests/klaus_test.cpp (needs cc65). Feedback register at $BFFC: bit 0 = IRQ
# level, bit 1 = NMI (rising edge), open collector without DDR, D cleared on
# interrupt (65C816). Output: <out>/interrupt_test.bin + .lbl
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/tests/cycle_accuracy/cache/klaus}"
SRC="$OUT/6502_interrupt_test.a65"
command -v ca65 >/dev/null && command -v ld65 >/dev/null || { echo "ca65/ld65 (cc65) not found" >&2; exit 77; }
[ -f "$SRC" ] || { echo "no source at $SRC (tools/cycle_suite.py fetch)" >&2; exit 77; }
# Configuration for the 65C816: D is cleared on interrupts.
sed -e 's/^D_clear     = 0 /D_clear     = 1 /' "$SRC" > "$OUT/interrupt_test.cfg.a65"
python3 "$HERE/tools/as65_to_ca65.py" "$OUT/interrupt_test.cfg.a65" "$OUT/interrupt_test.s" "$OUT/interrupt_test.ld.cfg"
ca65 "$OUT/interrupt_test.s" -o "$OUT/interrupt_test.o"
ld65 -C "$OUT/interrupt_test.ld.cfg" -o "$OUT/interrupt_test.bin" -Ln "$OUT/interrupt_test.lbl" "$OUT/interrupt_test.o"
echo "built $OUT/interrupt_test.bin ($(wc -c < "$OUT/interrupt_test.bin") bytes)"
