# Apple IIgs cycle-accuracy qualification

This document defines the evidence required before POMIIGS may claim cycle-level
accuracy. It accompanies the machine-readable catalog in
`tests/cycle_accuracy/catalog.json` and the runner in `tools/cycle_suite.py`.

## What “cycle accurate” means here

A successful boot, a correct-looking final framebuffer, or agreement with one
other emulator is not sufficient. A cycle-level gate must bind an input to at
least one observable quantity at a specific emulated time:

- final CPU registers, touched memory and instruction-cycle count;
- a timestamped bus/register trace in 14.31818 MHz master ticks;
- an interrupt edge at a specified beam position;
- a VGC fetch or floating-bus byte at a specified horizontal/vertical count;
- an audio/DOC state transition at a specified master tick;
- a frame captured at a specified frame and master-tick phase.

The Apple IIgs splits fast and slow domains. MAME documents 5 master ticks for
a 2.8 MHz CPU cycle, 64 slow cycles of 14 ticks plus one stretched 16-tick
cycle per line (912 ticks), fast/slow re-alignment every 25 lines, and a
5-tick DRAM-refresh window every 50 master ticks.[1] These are testable
contracts, not merely implementation notes.

## Current POMIIGS baseline and known blockers

The existing CTest suite already provides useful deterministic gates for the
CPU, MMU, video primitives, interrupts, ADB, SCC, DOC, IWM/Sony and SmartPort.
The new catalog groups those tests without overstating them as whole-machine
cycle proof.

The first timing-scheduler tranche now uses 912-tick lines, puts the 16-tick
long cycle in slot 65, phase-aligns fast-to-Mega-II accesses, and applies the
5-in-50-tick refresh stall only to fast DRAM. `megaii_timing_test` pins those
contracts, including ROM/FPI/slow-side refresh hiding and 25-line realignment.

The CPU now emits every cycle in microsequence order — active transactions
and internal cycles alike — and the scheduler places each one, so an
instruction's bus accesses land on the correct PH0 slot and refresh phase
(`megaii_timing_test` pins `INC abs` / `STA abs,X` shapes at instruction
level). Two gaps still prevent a valid TextFunk/FloatBus pass today:

1. The reset phase of the PH0 and refresh grids is assumed, not measured; it
   awaits calibration against a real-IIgs trace.
2. `VGC::render` reconstructs a frame from final memory. It does not consume
   memory in raster order while the CPU is writing, and unmapped/floating-bus
   reads are still approximate.

For that reason, TextFunk and FloatBus are catalogued as explicit blockers,
not green tests. This prevents “software booted” from being reported as a
false cycle-accuracy pass.

## Test inventory

### 1. 65C816 instruction vectors — automated subset now

Tom Harte's `SingleStepTests/65816` corpus contains 512 JSON files — every
opcode in emulation and native mode — with 10,000 cases each, for up to 5.12
million cases. Each case describes initial/final CPU state, relevant RAM and an
ordered bus-cycle sequence.[2] POMIIGS already has `tomharte_65816`; its fetcher
is pinned to revision `db6b10401729d5f20f2181dde5d3d7b037093a4a`.

The repository has no explicit licence. The corpus is therefore never vendored
and is marked `reference-only`; local operators may explicitly fetch selected
files for internal validation, but redistribution is not asserted.

The harness returns CTest skip code 77 when vectors are absent. CTest and the
catalog runner both preserve that as **SKIP**, never a false PASS. It checks
final registers, touched memory and total cycle count, and now parses the full
cycle array to compare every transaction on which VDA, VPA or VPB is asserted:
address, data, VDA/VPA/VPB, RWB, E, M, X and MLB are compared in order. Program
fetches and vector pulls therefore cannot masquerade as generic RAM reads.

Cycles with VDA=VPA=VPB=0 are compared too: their address-bus value and
RWB/E/M/X/MLB state, in sequence with the active transactions, so a misplaced
internal cycle fails even when the total count is right (the harness self-test
proves that). `--no-bus` separates a state/timing defect from a trace defect
during triage, and `--active-only` separates a transaction defect from a
placement defect. Turning corpus disputes into exact issue-linked `xfail`
entries remains open.

Known corpus disputes must not be normalized into silent exceptions: MVN/MVP
can exceed the generator's 100-cycle capture and are currently excluded,[16]
while SBC direct-X page wrapping and JSR absolute-X stack wrapping have tracked
corrections/disputes.[17][18] Any `xfail` must name the upstream issue and the
exact case; an unexplained opcode-wide skip is not acceptable.

Independent functional cross-checks are also catalogued. The MIT `gilyon`
65C816 generator covers native opcodes but must be rehosted from SNES mapping
and explicitly does not test cycle timing.[19] Klaus Dormann's GPL suite and
Bruce Clark's public-domain decimal program strengthen 6502/65C02 emulation
mode and 8-bit decimal coverage.[20] The WDC datasheet remains the official
cycle-count and bus-signal specification, but no public machine-readable WDC
test suite was located.[21]

### 2. Local subsystem gates — automated now

These are fast regression gates and remain mandatory:

- `megaii_timing_test`, `speed_test`, `slowside_test`, `irq_test`, `hdd_test`:
  current FPI/Mega II timing, shadow and interrupt behaviour;
- `vgc_test`, `shr_test`, `dhgr_test`, `dhgr_page_test`, `text80_test`: render
  primitives and video soft switches;
- `adb_test`, `scc_test`, `doc_test`: register protocols and audio state;
- `iwm35_test`, `iwm525_test`, `disk35_test`, `smartport_test`, `twoimg_test`:
  disk protocols, codecs and persistence.

Clemens provides an independent MIT-licensed comparison set for CPU ADC,
minimal emulation, game port, video MMIO, SCC and disk containers.[6]
Differential tests should compare observable behaviour, not copy internal
implementation assumptions.

### 3. MiSTer Apple IIgs custom diagnostics — staged open-source tests

The MiSTer core publishes 21 bootable 65816 diagnostics under GPL-3.0. They
produce an on-screen PASS/FAIL verdict and are built with Merlin32 and
CiderPress2.[3] The set includes:

- `mmu_test` and `gsqmmu_test`: MMU, language card, RAMWRT, aux-bank and
  shadowing cases;
- `selftest01` through `selftest0c`: direct entry into each ROM diagnostic;
  (POMIIGS status, September 2026 — see `tests/cycle_accuracy/mister_goldens.json`:
  the 21 disks are built with the rehosted Merlin32 v1.2 + CiderPress2 1.1.1
  (`tools/cycle_suite.py build`, archives pinned by SHA-256 in the catalog
  sources) and booted by `tools/mister_diags.py`; 12 pass, 9 are xfails that
  each name the emulator gap: `$C037` shadow-all/bank-latch MMU cases, SCC
  serializer cross-channel and register selftest, ADB bus-command handshake,
  ADB µC firmware checksum, the two RAM tests that overwrite a RAM launcher,
  and ROM test 0A's ADB version read under ProDOS.)
- `scc_selftest`, `scc_crosschan`, `scc_both`: SCC register, serializer,
  cross-channel and FIFO behaviour;
- `adb_device_enum`, `adb_rom_checksum`: ADB GLU protocol and ROM-3-specific
  controller checksum;
- `floppy_rw_test`: write/read/verify over generated 5.25-inch and 3.5-inch WOZ
  media;
- `time`: ProDOS `GET_TIME` through the RTC driver.

The catalog pins source revision
`9adbba8622f378253765b7d438b6cbcc4d03fc57` and a verified archive SHA-256.
The archive may be fetched into the ignored cache with:

```bash
python3 tools/cycle_suite.py fetch --id mister-custom-diagnostics
python3 tools/cycle_suite.py prepare --id mister-custom-diagnostics
```

Preparation verifies the archive SHA-256 again and extracts only the pinned
`customtests/` subtree. Absolute paths, parent traversal, links, special files
and unexpectedly large payloads are rejected; an existing directory without a
matching provenance marker is never overwritten.

The upstream build uses Merlin32 v1.2 beta 2 and CiderPress2 v1.1.1.[26][27] The runner
does not let the upstream Makefile download tools implicitly. Supply reviewed
local copies explicitly, then build all 21 disks and two writable floppy
fixtures:

```bash
export POMIIGS_MERLIN32_ROOT=/path/to/Merlin32_v1.2_b2
export POMIIGS_CP2=/path/to/cp2_1.1.1/cp2
python3 tools/cycle_suite.py build --id mister-custom-diagnostics
```

`POMIIGS_MERLIN32` may override the platform-default executable below the
Merlin root. A successful build writes `pomiigs-build-manifest.json` containing
the source-archive digest, both actual tool-binary digests and every generated
disk digest. Sources and outputs remain in `tests/cycle_accuracy/out/`, never in
Git. Missing tools report SKIP rather than triggering a network download.

Before these become gates, their disk builder must be adapted to POMIIGS and
each final PASS screen must receive a frame-pinned golden. The upstream
regression script demonstrates the correct discipline: fixed frame numbers,
scripted inputs, fixed RTC where needed, exact image comparison, and hard
failure when an expected image differs.[4]

### 4. ROM 01 and ROM 03 built-in diagnostics — automated, user ROM required

The built-in self-test is entered with Command-Option-Control-Reset. The Apple
diagnostic note documents the numbered sequence and six-digit failure status.[10]
KEGS reports that the test must run at native 2.8 MHz and that ROM 01 needs Text
Page 2 shadow disabled.[7] (The latter is a ROM 01 *hardware* property — its
Mega II never shadows `$0800-$0BFF`; POMIIGS now models that, and the RAM
address-line test 04 depends on it.)

`selftest_trace` (CTest `rom0[13]_selftest_*`) holds ⌘+Option through RESET
(`$C061/$C062` push buttons and `$C025` modifiers) and reads the verdict from
the 40-column text page. The sequential run gates tests 01-08 (`--through 8`);
the RAM tests 02 and 04 can only run in sequence because they overwrite any
RAM launcher. Every other diagnostic is launched individually (`--test N`) the
way the MiSTer `SELFTESTxx` launchers do: the ROM boots normally first (the
bank `$E1` vector page must exist), then a bank `$00` stub sets 2.8 MHz, clears
`$0315-$0319`, JSLs the entry from the ROM's pointer table (ROM 03 `$FF:6403`,
ROM 01 `$FF:7143`) and takes the carry as the verdict. ROM 01 diagnostics may
return with `RTS` and land in bank `$FF`; the launcher accepts the return in
either bank.

Status (September 2026): ROM 03 passes 01-08 and 0A-0C, ROM 01 passes 01-08,
0A and 0B. Test 09 (ADB) checksums the ADB microcontroller's 4 KB firmware
through the "read µC memory" command (`$09`); that image (341-0632 / 341-0345,
part of the MAME `apple2gs` BIOS set) is user-supplied via
`roms/iigs-adb-uc-rom0[13].rom` or `$POMIIGS_ADB_UC_ROM`, and the gate reports
SKIP without it. Getting here required a guest-writable RTC seconds counter
(test 07 writes a walking bit through `$E1/0088` and reads it back), open-bus
reads for unpopulated fast RAM (mirroring made the address-line test loop), and
the ROM 01 text-page-2 rule above. ROM bytes are copyrighted and remain
user-supplied.

### 5. TextFunk Viewer — highest-value beam-race blocker

TextFunk maps the 65816 stack into text page 1 and rewrites it once per
scanline using unrolled pushes. Writes travel right-to-left while the video
scanner reads left-to-right, so the two cross within each line.[5]

A valid test must:

1. boot a legally obtained `textfunk.po` at stock speed;
2. use a deterministic frame/input schedule;
3. capture the tunnel and the grid reached by repeated Space presses;
4. compare several consecutive frames with real-IIgs goldens;
5. record CPU writes to `$0400-$07ff`, VGC fetches, `$C02E/$C02F`, PH0 phase,
   slow-side sync and refresh stalls in the same master-tick timeline.

The acceptance image must have no central seam, streak or frame-to-frame
jitter. A clean image generated from final memory is explicitly invalid: the
beam race itself must be reproduced.[5]

### 6. VidModes and FloatBus — hardware-verified probes, reference only

The author-published `VidModes_260421.zip` contains a ProDOS disk plus sources
for `VIDSYNC`, `BEAMPOS`, `IRQTEST`, video modes and soft-switch checks. Its
readme says the expected behaviour was measured on Apple IIgs ROM 3 with a
ZipGSX, as well as IIe, IIc PAL and II+ hardware.[12] The catalog records the
109,497-byte archive with SHA-256
`0d7d57de18350b5e6e503797c8c83e34a03f5d097fe0868e5b4e5ee82362467a`.

`FloatBus_260630.zip` similarly contains a 140K ProDOS image and source probes
for floating-bus ambiguities. The catalog records its 77,941 bytes and SHA-256
`3862b5e7c01107c82475fdf54783d28b781f301016699ad2a692582438eb144c`.[12]

No explicit licence was found inside either author attachment. Both are marked
`reference-only`: their URLs and checksums are preserved, but the fetch command
deliberately refuses to download or vendor them. They can become CI fixtures
only after permission or an explicit licence is obtained.

The FloatBus golden must assert the byte visible to the CPU during video fetch,
not only H/V counters. It must include HBL/VBL boundaries, fetch address/byte,
`$C02E/$C02F`, and the corresponding 14 MHz tick. POMIIGS cannot pass this
until floating-bus reads return the live VGC/Mega II fetch value and the
scanline/PH0 model is phase-correct.

### 7. Open memory audits — staged

`a2audit` is MIT-licensed and supplies disks/sources for main/aux memory,
language-card and soft-switch behaviour. It targets Apple II/II+/IIe/IIc, so
only the Mega II-compatible subset is valid on IIgs; its floating-bus area is
not a finished IIgs oracle.[13]

`MiniMemoryTester` is MIT-licensed IIgs software for destructive expansion-RAM
patterns.[14] It is useful for bank reachability and corruption, but it does
not measure wait states, refresh phase or MMU decode timing. Pair it with the
MiSTer `MMU_TEST`/`GSQMMU` cases rather than treating a clean RAM pass as cycle
proof.

### 8. DOC, ADB, SCC, IWM and SmartPort gaps

The MiSTer custom suite gives usable coarse gates for ADB device absence and
ROM checksum, SCC register/FIFO/serializer paths, ProDOS-level floppy
read/write, RTC, and ROM selftest 0C for DOC. These tests do not cover every
cycle-sensitive path.

Additional open microtests are still required:

- **DOC/ES5503:** timestamp oscillator state, one-shot/free/swap/sync-AM modes,
  IRQ assertion/reassertion and deterministic PCM. MAME's BSD-3 implementation
  is a differential model, not hardware truth.[15]
- **ADB:** scripted key down/up, modifiers, mouse deltas, absent devices,
  collisions and autopoll; compare timestamped `$C026/$C027` accesses.
- **SCC:** bit timing, IRQ/FIFO edges, parity/framing/overrun and external clock
  cases, not only loopback success.
- **IWM/Sony:** log `$C0E0-$C0EF`, phase lines, `RDDATA/WRDATA`, HDSEL and zoned
  3.5-inch timing; a successful ProDOS block round trip is insufficient.
- **SmartPort:** no complete open IIgs cycle corpus was found. A new guest test
  must cover classic and extended STATUS/READ/WRITE/FORMAT/CONTROL, absent and
  write-protected media, media changes and REQ/BSY timing.

### 9. TrueGS — staged guest acceptance test

TrueGS is useful for broad MMU and SHR-linearisation coverage in Quick and
Comprehensive modes.[9] It is not a substitute for a bus trace: capture every
result screen and map each failure to the lower-level cycle gate that explains
it. Until redistribution terms are established, the image is user-supplied.

### 10. Apple service diagnostics — staged, user-supplied

Apple's dealer/service diagnostic disk exercises RAM, ROM and machine devices
end to end.[11] GSSquared's diagnostic log is valuable because it records
specific ADB/keyboard behaviours exposed by that software, including `$C010`
any-key-down and modifier/mouse semantics.[8]

Treat the disk as proprietary. The runner records results but does not download
or bundle it.

### 11. Raster demos and historical software acceptance

Three inspectable raster workloads are now explicit staged entries:

- **Crazy Cycles / CC65C02** by GROUiK is published with GPLv3 source and
  exercises Mega II 1 MHz timing plus HGR/TEXT changes within a scanline.[22]
- **Fancy Lores Rasterbars** by Vince Weaver uses vaporlock, 65-cycle Apple II
  lines and PAGE1/PAGE2 switching; the author's code is GPL-2.0, while any
  third-party assets still require separate review.[23]
- **PicViewer** supplies an inspectable IIgs route to SHR and 3200-colour
  palette-per-scanline validation, but its repository has no declared licence,
  so no code or disk is vendored.[24]

FTA Nucleus is retained as an integrated SHR/DOC smoke test. FTA XMAS is more
discriminating for `$C019`, VBL and `$C02F`; Ninjaforce Megademo exercises
long timing-sensitive sequences and bit-16 shadowing.[7][25] Their public
hosting is not an explicit redistribution licence, so the catalog requires
operator-supplied media.

Pitch Dark, Total Replay, Battle Chess, GS/OS and protected WOZ titles remain
secondary compatibility workloads. None is a cycle oracle by itself. A title
becomes a gate only with a fixed machine profile, deterministic input schedule,
hard timeout and a real-hardware or instrumented reference trace.

## Golden and trace format

Every external golden should be accompanied by a JSON metadata file containing:

```json
{
  "test_id": "textfunk-beamrace",
  "machine": "Apple IIgs ROM 03, stock 2.8 MHz",
  "rom_sha256": "user-recorded, never the ROM bytes",
  "media_sha256": "user-recorded",
  "master_clock_hz": 14318181.8,
  "rtc_epoch_utc": "1986-09-15T12:00:00Z",
  "timezone": "UTC",
  "initial_nvram_sha256": "...",
  "frame": 970,
  "master_tick": 0,
  "inputs": [{"frame": 460, "key": "SPACE"}],
  "oracle_origin": "real-hardware",
  "artifact_sha256": "..."
}
```

Goldens must never be silently regenerated after a failure. A baseline change
requires a reviewed explanation and retention of the previous artifact. Every
run starts from a fresh writable copy of media/NVRAM, fixes RTC/timezone,
removes previous outputs, enforces an emulated-time timeout and verifies that
each capture is newly produced. Image comparison decodes PNG pixels to a
canonical buffer; it does not compare encoder bytes or host-scaled screenshots.

## Commands

Validate and inspect the catalog:

```bash
python3 tools/cycle_suite.py validate
python3 tools/cycle_suite.py list
python3 tools/cycle_suite.py doctor
```

Run only existing automated gates:

```bash
python3 tools/cycle_suite.py run --tier unit
python3 tools/cycle_suite.py run --tier cycle
```

Fetch the pinned open-source diagnostic archive:

```bash
python3 tools/cycle_suite.py fetch --id mister-custom-diagnostics
python3 tools/cycle_suite.py prepare --id mister-custom-diagnostics
POMIIGS_MERLIN32_ROOT=/path/to/Merlin32_v1.2_b2 \
POMIIGS_CP2=/path/to/cp2 \
  python3 tools/cycle_suite.py build --id mister-custom-diagnostics
```

User assets are supplied only through documented environment variables. The
runner refuses to attach download URLs to any `user-supplied` catalog entry.

## Delivery sequence

1. Preserve all current local CTests as the fast gate.
2. Build and boot the open MiSTer diagnostics; establish exact PASS goldens.
3. ~~Feed inactive CPU cycles into the landed 912-tick/PH0/refresh
   scheduler~~ (done) and calibrate its reset phase from a real-IIgs trace.
4. Implement raster-time VGC fetches and live floating-bus values.
5. Capture real-IIgs TextFunk and FloatBus traces; make them hard gates.
6. ~~Automate ROM selftests~~ (done; test 09 awaits the ADB µC firmware) and
   user-supplied TrueGS/service diagnostics.
7. Add MAME/KEGS differential reporting, while retaining hardware as the final
   authority when emulators disagree.

## Sources

[1] https://github.com/mamedev/mame/blob/mame0287/src/mame/apple/apple2gs.cpp — MAME 0.287 Apple IIgs driver
[2] https://github.com/SingleStepTests/65816/tree/db6b10401729d5f20f2181dde5d3d7b037093a4a/v1 — SingleStepTests 65816 vectors
[3] https://github.com/MiSTer-devel/Apple-IIgs_MiSTer/blob/9adbba8622f378253765b7d438b6cbcc4d03fc57/customtests/README.md — MiSTer Apple IIgs custom diagnostics
[4] https://github.com/MiSTer-devel/Apple-IIgs_MiSTer/blob/9adbba8622f378253765b7d438b6cbcc4d03fc57/vsim/regression.sh — MiSTer Apple IIgs deterministic regression
[5] https://github.com/pcornier/iigs_simulation/blob/61568fcb2f505301ffdd6807f6d5c829ec25ea4f/doc/textfunk-beamrace-handoff.md — TextFunk beam-race analysis
[6] https://github.com/samkusin/clemens_iigs/tree/8208d629b1de0e523213e4d7251c3005b4faa9a2/tests — Clemens IIgs test suite
[7] https://kegs.sourceforge.net/README.kegs.txt — KEGS 1.34 documentation
[8] https://jawaidbazyar2.github.io/gssquared/AppleIIgsDiagnostics — GSSquared Apple IIgs Diagnostics notes
[9] http://krue.net/truegs — TrueGS
[10] https://archive.org/details/IIgs_2523095_Diagnostics — Apple IIgs Technical Note 95 — Diagnostics
[11] https://archive.org/details/apple-service-apple-iie-iic-iic-plus-iigs-diagnostic-version-4.1-1991 — Apple Service IIgs Diagnostic 4.1
[12] https://github.com/mamedev/mame/pull/15247
[13] https://github.com/zellyn/a2audit/tree/6fdcedfd909b0d3a41dc7b6511d0932c952ecca0
[14] https://github.com/digarok/MiniMemoryTester/tree/c497b6bc5c9449b33d95b6236efc04ba6376dc95
[15] https://github.com/mamedev/mame/blob/mame0287/src/devices/sound/es5503.cpp
[16] https://github.com/SingleStepTests/65816/issues/8
[17] https://github.com/SingleStepTests/65816/pull/5
[18] https://github.com/SingleStepTests/65816/issues/6
[19] https://github.com/gilyon/snes-tests/tree/5ecdf555da920f0bd7b157542141965a8120186d/cputest
[20] https://github.com/Klaus2m5/6502_65C02_functional_tests/tree/7954e2dbb49c469ea286070bf46cdd71aeb29e4b
[21] https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf
[22] http://fr3nch.t0uch.free.fr/CC/CC.html
[23] http://www.deater.net/weave/vmwprod/rasterbars
[24] https://github.com/antoinevignau/source/tree/7244479d9222f9956b44a0cf1cd7a7769a3d2c76/picviewer
[25] https://www.ninjaforce.com/html/products.html
[26] https://brutaldeluxe.fr/products/crossdevtools/merlin/
[27] https://github.com/fadden/CiderPress2/releases/tag/v1.1.1
