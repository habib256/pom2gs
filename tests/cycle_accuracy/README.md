# Cycle-accuracy test assets

`catalog.json` is the source of truth for POMIIGS cycle qualification. It
separates tests that execute today from staged tests and architectural blockers.

## Readiness labels

- `automated`: executable now through `tools/cycle_suite.py run`.
- `staged`: source/workload identified, but an adapter or golden is still needed.
- `blocked-live-scanout`: cannot be a valid gate until VGC reads memory during
  raster scanout.
- `blocked-floating-bus`: cannot be a valid gate until the CPU sees the live
  Mega II/VGC bus value.

## Asset policies

- `none`: no external asset.
- `fetchable`: URL and SHA-256 are pinned; the runner may download it.
- `reference-only`: a public URL and checksum are recorded, but redistribution
  terms are missing; the runner deliberately does not download it.
- `user-supplied`: copyrighted or redistribution status is unclear. The catalog
  may name an environment variable but is forbidden from containing a download
  URL.
- `bundled`: only for assets whose compatible redistribution terms are recorded.

Caches and generated output are ignored by Git:

- `tests/cycle_accuracy/cache/`
- `tests/cycle_accuracy/out/`

## Usage

```bash
python3 tools/cycle_suite.py validate
python3 tools/cycle_suite.py list
python3 tools/cycle_suite.py doctor
python3 tools/cycle_suite.py fetch --id mister-custom-diagnostics
python3 tools/cycle_suite.py prepare --id mister-custom-diagnostics
POMIIGS_MERLIN32_ROOT=/path/to/Merlin32_v1.2_b2 \
POMIIGS_CP2=/path/to/cp2 \
  python3 tools/cycle_suite.py build --id mister-custom-diagnostics
python3 tools/cycle_suite.py run --tier unit
python3 tools/cycle_suite.py run --tier cycle
```

The Tom Harte adapter enables active-bus comparison by default. Its
`--no-bus` switch is a triage aid, not a qualification mode: it leaves only
register/RAM/cycle-count checks enabled. Exit code 77 from any automated
external adapter is preserved as SKIP by both CTest and `cycle_suite.py`.

`prepare` re-verifies the pinned archive and safely extracts only the declared
subtree. `build` never downloads a tool implicitly: it requires explicit local
Merlin32/CiderPress2 paths and records their SHA-256 values alongside hashes of
all generated disks in the ignored output tree.

The detailed rationale, sources, oracles and implementation order are in
[`docs/CYCLE_ACCURACY_TESTS.md`](../../docs/CYCLE_ACCURACY_TESTS.md).

## Non-negotiable rule

A missing ROM, disk, vector corpus, trace or golden is a **SKIP or blocker**, not
a PASS. POMIIGS must not claim cycle accuracy from a successful boot or a frame
reconstructed only from final memory.
