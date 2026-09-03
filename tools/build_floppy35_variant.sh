#!/usr/bin/env bash
# POMIIGS — derive a slot-5 (3.5" Sony) variant of the MiSTer FLOPPY_RW_TEST
# diagnostic. The upstream test probes unit $60 whenever slot 6 holds a Disk II
# ROM and falls back to $50 only without one; the IIgs always has the $C600
# PROM, so its 3.5" path never ran. The variant jumps straight to the slot-5
# path (one instruction), so the ProDOS block driver exercises the genuine
# slot-5 ROM firmware over the Sony LLE — read, write, verify, patterns.
#   tools/build_floppy35_variant.sh [customtests-dir] (needs POMIIGS_MERLIN32_ROOT + POMIIGS_CP2 or the tools cache)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HERE/tests/cycle_accuracy/out/mister-custom-diagnostics/customtests}"
T="$HERE/tests/cycle_accuracy/cache/tools"
MERLIN_ROOT="${POMIIGS_MERLIN32_ROOT:-$T/Merlin32_v1.2_b2}"
MERLIN="${POMIIGS_MERLIN32:-$MERLIN_ROOT/MacOS/Merlin32}"
CP2="${POMIIGS_CP2:-$T/cp2_1.1.1_osx-x64_sc/cp2}"
[ -f "$DIR/FLOPPY_RW_TEST.S" ] && [ -f "$DIR/blank.2mg" ] || { echo "no MiSTer customtests at $DIR (tools/cycle_suite.py prepare)" >&2; exit 77; }
[ -x "$MERLIN" ] && [ -x "$CP2" ] || { echo "Merlin32/cp2 not found ($MERLIN, $CP2)" >&2; exit 77; }
cd "$DIR"
python3 - <<'PY'
s = open('FLOPPY_RW_TEST.S').read()
old = "        LDA   $C6FF\n        BNE   :TRY50           ; No Disk II at slot 6, try slot 5"
new = "        JMP   :TRY50           ; POMIIGS variant: always test the slot-5 3.5\" drive"
assert s.count(old) == 1, "upstream source changed — re-check the patch"
open('FLOPPY_RW35_TEST.S', 'w').write(s.replace(old, new).replace("DSK   FLPTEST.SYSTEM", "DSK   FLP35.SYSTEM"))
PY
"$MERLIN" -V "$MERLIN_ROOT/Library/" FLOPPY_RW35_TEST.S >/dev/null
cp blank.2mg floppy_rw35_test.2mg
"$CP2" rm floppy_rw35_test.2mg BASIC.SYSTEM >/dev/null 2>&1 || true
"$CP2" add floppy_rw35_test.2mg FLP35.SYSTEM >/dev/null
"$CP2" sa floppy_rw35_test.2mg type=SYS FLP35.SYSTEM >/dev/null
echo "built $DIR/floppy_rw35_test.2mg"
