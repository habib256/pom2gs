#!/usr/bin/env bash
# POMIIGS — build the gilyon 65C816 tests for the flat-bus harness.
# Needs the pinned cputest tree (tools/cycle_suite.py prepare / fetch) and
# cc65 (ca65 + ld65) on PATH. Output goes next to the generated tests.
#   tools/build_gilyon.sh [cputest-dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HERE/tests/cycle_accuracy/cache/gilyon/cputest}"
command -v ca65 >/dev/null && command -v ld65 >/dev/null || { echo "ca65/ld65 (cc65) not found" >&2; exit 77; }
[ -f "$DIR/make_cpu_tests.py" ] || { echo "no cputest tree at $DIR" >&2; exit 77; }
cd "$DIR"
[ -f tests-full.inc ] || python3 make_cpu_tests.py
ca65 "$HERE/tests/cycle_accuracy/gilyon_flatbus.asm" -I . -o cputest_pom.o
ld65 -C lorom.cfg -o cputest_pom.sfc -Ln cputest_pom.lbl -m cputest_pom.map cputest_pom.o
echo "built $DIR/cputest_pom.sfc ($(wc -c < cputest_pom.sfc) bytes)"
