#!/usr/bin/env bash
# POMIIGS — fetch Tom Harte SingleStepTests/65816 vectors for the CPU65816 gate.
# Each opcode has two files: <hex>.e.json (emulation) and <hex>.n.json (native),
# ~4-9 MB each, 10 000 vectors. The full 512-file corpus is ~2 GB, so by default
# we pull a curated coverage subset; pass "all" to pull everything.
#
#   tests/fetch_tomharte_65816.sh <out-dir> [all|<space-separated hex opcodes>]
#
# Source: https://github.com/SingleStepTests/65816  (branch main, dir v1/)
set -euo pipefail
OUT="${1:?usage: fetch_tomharte_65816.sh <out-dir> [all|<opcodes>]}"
BASE="https://raw.githubusercontent.com/SingleStepTests/65816/main/v1"
mkdir -p "$OUT"

# Curated subset: at least one opcode per addressing mode + arithmetic (incl.
# decimal), logic, compare, RMW, branch, stack, transfer, and mode-switch.
CURATED="a9 a5 b5 ad bd b9 a1 b1 b2 a7 b7 af bf a3 b3 \
85 8d 9d 99 81 91 92 87 8f 83 93 64 9c \
a2 a6 ae a0 a4 ac \
09 29 49 69 e9 c9 e0 c0 24 89 2c \
e6 c6 ee fe 06 0e 46 4e 26 2e 66 6e 04 0c \
aa a8 8a 98 9a ba 9b bb 5b 7b 1b 3b eb \
e8 c8 ca 88 1a 3a 0a 4a 2a 6a \
48 68 da fa 5a 7a 08 28 8b ab 0b 2b 4b f4 d4 62 \
d0 f0 90 b0 10 30 50 70 80 82 \
4c 6c 7c 5c dc 20 fc 22 60 6b 40 \
18 38 58 78 b8 d8 f8 c2 e2 fb ea 42 54 44 00 02"

if [[ "${2:-}" == "all" ]]; then
    OPS=$(printf '%02x ' $(seq 0 255))
elif [[ -n "${2:-}" ]]; then
    OPS="${*:2}"
else
    OPS="$CURATED"
fi

n=0
for op in $OPS; do
    for mode in e n; do
        f="$op.$mode.json"
        if [[ -s "$OUT/$f" ]] || curl -sf "$BASE/$f" -o "$OUT/$f"; then n=$((n+1)); fi
    done
done
echo "fetched/present: $n files in $OUT"
