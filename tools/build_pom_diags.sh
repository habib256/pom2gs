#!/usr/bin/env bash
# POMIIGS — build the POMIIGS-authored guest diagnostics (tests/cycle_accuracy/diags/*.S,
# Merlin 8/16 syntax) into bootable 800K .2mg images with the rehosted
# Merlin32 + CiderPress2, on top of the MiSTer blank.2mg template.
#   tools/build_pom_diags.sh [out-dir]   (default: the MiSTer customtests dir)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/tests/cycle_accuracy/out/mister-custom-diagnostics/customtests}"
T="$HERE/tests/cycle_accuracy/cache/tools"
MERLIN_ROOT="${POMIIGS_MERLIN32_ROOT:-$T/Merlin32_v1.2_b2}"
MERLIN="${POMIIGS_MERLIN32:-$MERLIN_ROOT/MacOS/Merlin32}"
CP2="${POMIIGS_CP2:-$T/cp2_1.1.1_osx-x64_sc/cp2}"
[ -f "$OUT/blank.2mg" ] || { echo "no blank.2mg at $OUT (tools/cycle_suite.py prepare)" >&2; exit 77; }
[ -x "$MERLIN" ] && [ -x "$CP2" ] || { echo "Merlin32/cp2 not found ($MERLIN, $CP2)" >&2; exit 77; }
# Unformatted 800K .2mg for the FORMAT diagnostic (the runner copies it per run;
# the Sony LLE takes sector images, not WOZ bit streams).
[ -f "$OUT/blank35_unfmt.2mg" ] || ( cd "$OUT" && "$CP2" cdi blank35_unfmt.2mg 800k >/dev/null )
[ -f "$OUT/blank35_unfmt.woz" ] || ( cd "$OUT" && "$CP2" cdi blank35_unfmt.woz 800k >/dev/null )
[ -f "$OUT/prodos35.woz" ]      || ( cd "$OUT" && "$CP2" cdi prodos35.woz 800k prodos >/dev/null )
for src in "$HERE"/tests/cycle_accuracy/diags/*.S; do
    name="$(basename "$src" .S)"; sysname="$(grep -m1 '^ *DSK ' "$src" | awk '{print $2}')"
    cp "$src" "$OUT/$name.S"
    ( cd "$OUT" && "$MERLIN" -V "$MERLIN_ROOT/Library/" "$name.S" >/dev/null )
    disk="$(echo "$name" | tr 'A-Z' 'a-z').2mg"
    ( cd "$OUT" && cp blank.2mg "$disk" \
      && ( "$CP2" rm "$disk" BASIC.SYSTEM >/dev/null 2>&1 || true ) \
      && "$CP2" add "$disk" "$sysname" >/dev/null \
      && "$CP2" sa "$disk" type=SYS "$sysname" >/dev/null )
    echo "built $OUT/$disk"
done
