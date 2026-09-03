# TODO.md

Active backlog + **MAME↔POMIIGS parity dashboard** + milestone roadmap.
Legend: 🔴 not started · 🟡 in progress · 🟢 done + pinned test · ⚪ out of scope.

## 🟡 IN PROGRESS — Cycle-accuracy qualification suite (September 2026)

**Goal:** turn the strongest publicly documented Apple IIgs diagnostics, timing
probes and demos into reproducible POMIIGS tests, with provenance, licensing,
fixed inputs, machine-readable verdicts and hardware-derived oracles. A title
merely booting or producing a plausible final framebuffer is not a cycle-level
pass.

- 🟢 Build a versioned test catalog covering CPU, FPI/CYA, Mega II, VGC,
  interrupts, shadowing, ADB, SCC, DOC, IWM/Sony, SmartPort and RTC.
- 🟡 Extend 65816 qualification: 🟢 the harness compares **every** bus cycle
  in order — address/data plus VDA/VPA/VPB/RWB/E/M/X/MLB for active
  transactions, address plus pins for internal cycles — while retaining the
  independent total-cycle check; 🟢 the core is microsequenced (internal
  cycles positioned per WDC Table 5-7, RMW dummy write/MLB/high-first write-
  back, uncarried index address, JSL/WDM/RTI ordering); 🟢 MVN/MVP covered
  (the harness runs whole iterations to the corpus's 100-cycle cap) — 5.12 M
  vectors, no exclusion, 308 issue-linked xfails where hardware contradicts
  the corpus ((dp,X) high-byte page wrap, JSR (abs,X) raw stack); 🟢 gilyon
  generator rehosted (1610/1610) and Klaus 6502 functional test green;
  🟢 Bruce Clark decimal test ported to ca65 (all flags checked, green);
  🟢 Klaus interrupt test (IRQ/NMI/BRK via the $BFFC feedback register) green.
- 🟡 Turn Crazy Cycles, Fancy Lores and a legally usable 3200-colour fixture
  into deterministic raster checkpoints; keep FTA/Ninjaforce media
  user-supplied until redistribution terms are explicit.
- 🟡 Integrate the MiSTer Apple-IIgs custom diagnostics (`MMU_TEST`, `GSQMMU`,
  ROM selftest slices, SCC, ADB, floppy and RTC): 🟢 pinned fetch, safe
  subtree-only extraction and an explicit Merlin32/CiderPress2 build adapter
  now produce a tool/output-hashed manifest under the ignored output tree;
  🟢 Merlin32 v1.2 + CiderPress2 1.1.1 rehosted (archives SHA-256-pinned in the
  catalog, unpacked under `tests/cycle_accuracy/cache/tools/`), all 21 disks
  build and boot; `tools/mister_diags.py` + `mister_goldens.json` gate them
  (CTest `mister_diagnostics`): 15 pass, 6 xfail. 🟢 `mmu_test` 26/26 and
  `gsqmmu_test` (shadow-all banks, bank latch, floating bus, LC layout);
  🟢 ROM 0A under ProDOS (ADB response latency). The 6 xfails are all
  documented non-emulator causes: three corpus disputes (`adb_device_enum`
  polls `$C027` bit 7; `scc_selftest` assumes the 8530 pointer survives a
  read; `scc_crosschan` needs the MiSTer core's A↔B wiring), the ADB µC
  firmware (`adb_rom_checksum`), and the two RAM tests that overwrite a RAM
  launcher (`selftest02/04`, covered by the sequential `selftest_trace`).
- 🟡 Make **TextFunk Viewer** a first-class beam-race gate: deterministic boot,
  scripted screen selection, consecutive-frame captures and comparison against
  a real-IIgs golden. 🟢 The live scanout model exists (`scanout_test`: the
  scanner crosses a text row being rewritten byte by byte); 🔴 the media and a
  hardware golden are still user-supplied (`tests/cycle_accuracy/disks/`).
- 🟡 Catalog the hardware-verified VidModes and FloatBus archives with exact
  provenance, sizes and SHA-256. Their sources have no explicit licence, so the
  harness records them as `reference-only` and never auto-downloads them.
  Implement execution after permission is established, then verify CPU-visible video fetch bytes,
  `$C02E/$C02F`, HBL/VBL position and scanner phase against a hardware trace.
- 🟡 Replace the approximate timing model: 🟢 the master-clock scheduler now
  uses 64×14 + one stretched 16-tick cycle = **912 ticks/line**, phase-aligns
  fast accesses to the Mega II PH0 grid (which realigns every 25 lines), and
  conditionally stalls fast DRAM for the 5-tick refresh window every 50 ticks
  while ROM/FPI accesses hide it (`megaii_timing_test`). 🟢 Inactive CPU
  cycles are fed to that scheduler at their microsequence position
  (`IIgsMemory::internalCycles`, pinned at instruction level by
  `megaii_timing_test`; GS/OS HDD boot unchanged under `hdd_trace`). 🔴
  Calibrate reset phase against a hardware trace and add Total Replay and
  disk-timing regression gates.
- 🟡 Automate ROM 01/03 built-in selftests at native 2.8 MHz: 🟢 `selftest_trace`
  gates the sequential run (01-08) and every other diagnostic individually on
  both ROMs (ROM 03: 0A-0C, ROM 01: 0A-0B all PASS; the ROM 01 text-page-2 rule
  is modelled); 🔴 test 09 (ADB) needs the user-supplied ADB µC firmware
  (`roms/iigs-adb-uc-rom0[13].rom`) — SKIP until provided.
- 🔴 Add **TrueGS** and Apple IIgs Diagnostics as user-supplied acceptance
  workloads; record results without redistributing proprietary disk images.
- 🔴 Add frame/trace differential runs against MAME, KEGS/GSplus and real IIgs
  captures. Goldens must record machine profile, ROM revision, input schedule,
  frame/master-tick number and SHA-256.
- 🟢 Separate gates into `unit`, `cycle`, `external-open`, and
  `external-user-supplied`; missing external assets must report SKIP, never a
  false PASS.

## 🟢 RESOLVED — GS/OS + hard disk (install to HDD)

**Goal (met):** boot the System 6 install disk (3.5", slot 5) with a hard disk
present, format it, and install GS/OS onto it — then boot GS/OS from the HDD.
The full round-trip works; the two 🔴 items under "Remaining (nice-to-have)"
below are conveniences, not blockers.

**✅ FULL INSTALL WORKS (July 2026):** the System 6 Installer walks all 7 disks and
installs GS/OS to a blank HDD **in full**. Last blocker was **3.5" disk-swap detection
with a writable HDD mounted** — GS/OS answered the Installer from its cached VCR and
never re-read the drive after a menu swap+OK. Fix: a **3.5" SmartPort DIB subtype with
bit7 set** (`smartportStatus`) — bit7 = disk-switched-capable ("poll me"); the `$00`
UniDisk value lacked it so GS/OS never polled. Now GS/OS polls STATUS, sees bit0, hits
`$2E`, re-mounts. The subtype first shipped as `$80`, then moved to **`$C0`** (extended
+ disk-switched, what KEGS and a real Apple 3.5 report) when Silent Service's loader
turned out to whitelist exactly `$00`/`$C0` — see `docs/COMPAT.md § SmartPort
DIB/CONTROL fixes`. `$C0` is what the code and `smartport_test` pin today. Root-caused
with the SmartPort dispatch trace, still available as `POMSP_LOG=1` (see
`DEV.md § Dev environment variables`). See CHANGELOG.

**✅ Unblocked (July 2026):** GS/OS now boots **all the way to the Finder desktop
with a hard disk mounted** on slot 7 (blank or formatted) — the first time GS/OS +
a hard disk has worked in POMIIGS. Screenshot-parity with the no-HDD boot (SHR menu
bar + desktop drawn, ~150k–258k toolbox dispatches, no derail). Two bugs fixed (see
CHANGELOG "GS/OS + hard disk" for the full why), both cited from KEGS / MAME /
GSSquared `pdblock3`:
1. **Boot-device stuck on slot 7** (`START.GS.OS  Error=$0046`) — the ROM scan
   commits slot 7 (first ProDOS signature found); our slot-7 chain now re-points
   the boot-slot globals (`$00/$01`, MSLOT `$07F8`) to `$C5` and re-issues the ROM's
   `JMP ($0000)` so slot 5 boots. (`ProDosHdd`.)
2. **Emulation-mode IRQ vector from LC RAM → `$00:0000`** — GS/OS calls the ProDOS-8
   block driver in emulation mode without masking IRQs; the emul vector `$00/FFFE`
   must read ROM (VP line), not uninitialised LC RAM. (`CPU65816::serviceInt`.)
   HDD writes now persist to the image (`ProDosHdd::flushBlock`), so format/install
   sticks. Pinned by `tests/hdd_test`; diagnosed with the new `tests/hdd_trace`.

**✅ Installer runs (July 2026):** "Disk 1 of 7 Install" now boots to the **Apple IIGS
Installer — Easy Update** window (screenshot-verified) with the blank GSOS HDD mounted.
Fixes (cross-checked vs KEGS/MAME/GSSquared — see CHANGELOG): SmartPort DIB **device
type $01** (3.5", not $02); STATUS **errors ($28) for unit > 1** so the Installer's
device scan terminates; extended-STATUS **4-byte block count**; extended SmartPort
**4-byte inline param pointer**; `spReturn` **preserves the B accumulator**.

**Round-trip install → boot-from-HDD: DONE.**
1. ✅ **Drive the install in the UI** — Easy Update installs the System Folder to
   `hdv/GSOS.hdv` (writes persist) across all 7 disks, hot-swapped via the "3.5\" Drive"
   menu. The disk-switched subtype fix (above) made the disk-swap prompts advance.
2. ✅ **Boot GS/OS *from* the HDD** — `boot = hdd` starts GS/OS to the Finder straight
   from `hdv/GSOS.hdv` (SHR on at ~750k steps, ~141k toolbox dispatches, desktop drawn,
   no derail — headless `hdd_trace none hdv/GSOS.hdv`). **Gotcha found & fixed:** Easy
   Update copies the System Folder + `PRODOS` loader but does **NOT** rewrite the ProDOS
   **boot block** (block 0) on an already-formatted volume, so the installed disk stayed
   non-bootable (byte 0 = `$00`, slot-7 card chained to slot 5). Fix: `make_prodos_hdd.py`
   grew a `--boot-from <prodos-disk>` option that copies a standard ProDOS boot block; the
   shipped `hdv/GSOS.hdv` now carries one (byte 0 = `$01`).

**Remaining (nice-to-have):**
- 🔴 **SmartPort FORMAT (`$03`)** for initialising a raw HDD from the Installer /
  Advanced Disk Utility (currently the target must be a pre-formatted ProDOS volume via
  `tools/make_prodos_hdd.py`).
- 🔴 **3.5" WOZ media on the Sony LLE** — `Sony35` is sector-backed and now refuses
  `.woz` (it used to nibblise the container as sectors and overwrite the file on
  flush). Decode the WOZ 3.5" GCR bit stream into the IWM nibble stream (GSSquared
  `Floppy35_woz`, MAME `floppy.cpp` mac_floppy) so protected/flux 800K originals run.
- 🔴 **In-emulator "make bootable"** — model whatever GS/OS call would write the boot block
  during a real install, so `--boot-from` isn't needed. (BRAM startup-slot
  persistence: done — `states/bram.bin`.)

## 🟢 RESOLVED — GS/OS runtime bugs (July 2026, see CHANGELOG "BASIC.System")

Both "known bugs" traced to the same root class — **silent BRK-to-monitor from two MMU
gaps** — and fixed (MAME `apple2gs.cpp` + KEGS `moremem.c` cited):
1. **BASIC.System / P8 launch crash** → (a) the **Mega II language card** (`$E0/$E1
   $D000-$FFFF`) was flat RAM: no bank-2 `$D000` window, no ROM read-through, no
   write-enable, no ALTZP — GS/OS's P8 launch glue in the `$E0` LC was aliased/corrupt →
   `slowLcRead/slowLcWrite` per MAME `lc_r/lc_w`; (b) the **internal `$C100-$CFFF`
   firmware** was unmapped (0 for non-card slots) — BASIC's `JSR $C300` (80-col firmware)
   executed `$00`s → `slotRomRead` serves the bank-`$FF` ROM image for unclaimed slots +
   `$C800` window. **Verified headless**: Finder → keyboard-driven launch → `]` prompt →
   `PRINT 2+2` → `4`. **User-confirmed interactively** ("Accès BASIC réglé"); the two
   APPLEDISK5.25 desktop icons are the authentic slot-6 internal firmware now being
   visible (expected, matches a real IIgs with the disk port enabled).
2. **Intermittent boot hang + audio crackle** — re-diagnosed: the "EMStatus spin at
   `$FF:CF94`" is the ROM's **KEYIN key-wait** (the `JSR $CF94` caller at `$CEFC`
   increments the `$4E/$4F` entropy seed = classic key wait). The machine had silently
   BRK'd to the monitor (`*` prompt, hidden under the SHR boot screen) and sat waiting
   for a key — same silent-crash class as bug 1, most plausibly the same MMU gaps hit on
   a timing-dependent boot path. **Post-fix user report: crashed once on the very first
   boot, then OK on every subsequent boot** — the one-time crash is plausibly stale
   pre-fix BRAM/desktop state being reinitialised; keep watching. If it recurs, capture
   where the BRK comes from (re-add a BRK trace to `CPU65816.cpp`).

## 🔴 OPEN — GS/OS↔ProDOS-8 round-trip edge case (July 2026)

- **Launching the bare `ProDOS` kernel file from the Finder** (GSOS window → ProDOS icon
  → Open) ends in **"Unable to load START.GS.OS file. Error=$0028"** (no device
  connected). The normal paths are FINE: BASIC.System launches (verified interactively +
  headless), and `BYE` from BASIC reloads GS/OS to the Finder ("One moment please…" →
  desktop, verified headless). Facts from the headless repro (`basic_launch … p` in the
  session scratchpad — boots the HDD, keyboard-drives the Finder): at the error,
  **MSLOT(`$07F8`)=$C5 and DEVNUM(`$43`)=$50** — the GS/OS reload bootstrap is pointed at
  **slot 5 (the EMPTY 3.5")** instead of slot 7 (the boot HDD); and an error-log tap on
  every slot-5 HLE error path stayed **silent** — no device call ever failed, so GS/OS
  synthesizes the `$28` from its own device bookkeeping. Suspect: the P8 kernel COLD-BOOT
  path (running the kernel as an app) re-derives its boot device from MSLOT/zp residue
  (last slot-firmware call = a slot-5 SmartPort poll) rather than the launch device, then
  the GS/OS quit-return inherits it. Low priority — launching the raw kernel is not a
  normal user action and every normal launch/quit path works; **workaround: launch
  BASIC.System (or any .SYSTEM app), not the kernel file.**

## 🟡 GAMES COMPATIBILITY PASS (July 2026, first-ever)

Collection at `/media/gistarcade/SHARE/roms/apple2gs/` — 361 images, 1.6 GB, of
which 341 are GS 3.5" disks (the set `tests/triage` sweeps, see `docs/COMPAT.md`).

First 8-game headless triage (`screenshot --disk35 <game> --frames 700`):

| Game | Verdict |
|---|---|
| Arkanoid | ✅ SHR intro (Taito "Fun Time Arcade") pixel-perfect |
| Arkanoid II | ✅ SHR crack-intro perfect |
| Battle Chess | ✅ board + attract mode (froze at the board until the $C02F HORIZCNT fix — the game raster-syncs on the horizontal beam counter) |
| Blackjack Academy | ✅ SHR title perfect |
| Airball | ✅ boots to its bundled desktop (game = double-click) |
| Aaargh! | ⚠️ black SHR at 700 frames, code running in bank $01 — maybe still loading; retest longer |
| Block Out | ✅ SHR title (was ❌ BRK @ 00/0003 — **fixed**, see below) |
| Beyond Zork | ✅ boots (was ❌ BRK @ 00/0006 — **fixed**) |

**Full 341-image triage now runs** via `tests/triage` (see `docs/COMPAT.md`):
of ~180 genuinely-bootable GS disks, **151 (~84%) reach graphics**. The Block Out /
Beyond Zork BRK — and ~28 more titles with the identical crash — were a
**slot-7 AppleTalk false-positive**: a shared cracked loader scans `$00:C7F9`
for "ATLK"; POMIIGS served the internal AppleTalk firmware there where an
empty slot 7 should read `$00`. One fix (`slotRomRead`) → OK_GFX 114→144.
Root-caused with a KEGS golden-trace diff (method in the `toolbox-loader-crash`
memory note). The 150 HANGs are non-bootable images (no PRODOS file — correct).
Follow-up sweep on the remaining clusters (July 2026): **OK_GFX 144→149** —
SmartPort HLE identity bytes fixed ($C5FB extended bit → Dungeon Master; DIB
subtype $C0 → Silent Service; ctl $06 → $21 → Marble Madness's protection
fallback) + triage's CRASH_ZP heuristic gated on !gfx (Pirates!/Silent
Service run legit code from zero page), then **149→151** with the hybrid Sony
mount (an HLE-mounted 3.5" image is also inserted read-only into the real
IWM/Sony drive, so direct-IWM loaders — Sensei, Space Cluster — see the same
medium). See `docs/COMPAT.md` + CHANGELOG.
Interactive play test (sound + mouse): File → "Load 3.5\" Disk..." boots the
game disk directly (ejects the HDD + cold reset).

**Launch-mode rule (user-verified):** a self-booting crack (Arkanoid — full
attract mode incl. gameplay screen verified headless) must be BOOTED via
"Load 3.5\" Disk..."; launching it from the GS/OS Finder gives its loader's
"Unable to load tools! 0000". A "needs ToolXXX" crack (Arkanoid II → Tool025 =
Note Sequencer) is the opposite: launch it FROM the Finder so the HDD's full
System provides the tools.

**Play-tested (July 2026, post-ADB-v1):** Battle Chess **playable** ✅; Black
Cauldron "nickel" ✅; Bubble Ghost runs ✅; Captain Blood: intro + digitized
samples play (**some cut slightly early**) then the planet screen stops
responding — probe shows the CPU alive in an unrolled SHR blit loop
(`LDA $17xx,X / STA $E120xx,X` at $01:BEFE), i.e. the game animates but its
logic stalls, most plausibly waiting for a **sample-completion event** that our
DOC ends too early (same root as the truncated samples). 🔴 NEXT DOC LEAD:
one-shot / zero-byte end-of-sample semantics vs MAME (do we halt one sample
early? does the completion IRQ fire at the right time?) — repro: Captain Blood.

**🟢 FIXED (most likely) — "samples cut early / accelerated game music"
(Captain Blood, Transylvania III): the wall-clock timebase counted CPU cycles**
→ at 2.8 MHz the VBL fired at ~164 Hz and every VBL-clocked game engine ran
2.8× fast. Fixed with the single master-tick timebase (see CHANGELOG). Captain
Blood now completes its intro to "PRESS FIRE TO BEGIN" (was stalled at the
planet screen). Transylvania III to be re-play-tested by the user.

**🟢 FIXED — in-game mice (ADB GLU v1, July 2026).** Games program the GLU
directly: a $C026 µC command stream at init + **$C027 interrupt-enable writes**
($30 = data int, Arkanoid) and then take ADB IRQs. Implemented (KEGS adb.c):
$C027 enables latched + reported, updateAdbIrq follows the hardware enables
(legacy sysUp gate kept for the ROM path), real $C026 command machine with
parameter counts + response queue. Headless proof: Battle Chess cursor moves
with injected motion (676 px vs 0 control). Follow-ups if a game still misses:
mouse-via-SRQ DATAREG packets, µC RAM/keycode commands (0x08/0x11 absorbed).

## Milestone roadmap

The build order is chosen so each layer is independently testable before the
next depends on it — the CPU against Tom Harte vectors, the MMU against a ROM
self-test, then the visible/audible subsystems.

| # | Milestone | Deliverable | Gate |
|---|---|---|---|
| **M0** | Foundation | Repo, CMake (native + WASM), doc suite, subsystem map | 🟢 `cmake && make` builds an ImGui window. No separate headless target: the `tests/` harnesses (screenshot, triage, *_trace) link the emulator without ImGui/GL |
| **M1** | 65C816 core | `CPU65816` — emulation + native mode, 24-bit, all 256 opcodes | 🟢 Tom Harte `SingleStepTests/65816` 100% on all 256 opcodes × 2 modes (5.12M vectors, regs+RAM+every bus cycle incl. internal ones; MVN/MVP via the capped-iteration rule, see DEV) |
| **M2** | MMU / FPI + Mega II | `IIgsMemory` — 16 MB banks, shadow, speed reg, slow/fast split | 🟢 ROM 01 **and** 03 boot from ROM vector → native → self-diagnostic → banner → disk boot; shadow write-through, 912-tick lines, phase-aware slow-side/refresh stalls, STATEREG, VBL + Mega II ¼/1-second timers all in. Gates: `megaii_timing_test`, `slowside_test`, `speed_test`, `irq_test`; verify with `boot_trace`. Remaining nuance: feed inactive CPU cycles into the bus scheduler and calibrate phase from hardware traces |
| **M3** | Legacy video + VGC | `VGC` Super Hi-Res (320/640) + 40-col text from the authentic char ROM → GL display | 🟡 SHR renders (vgc_test green, PNG verified); text renders with authentic char ROM (344s0047); HGR + DHGR colour (NTSC + RGB, dhgr_test); 80-column text (text80_test); per-line scanline IRQ done (irq_test). Mid-frame render splits (3200-colour) = follow-up |
| **M4** | ADB + BRAM/RTC | ADB GLU HLE + STATEREG-read fix + //e main/aux redirect | 🟡 ROM 03 boots through all self-tests to the **"Apple IIgs / ROM Version 3" banner**, then reaches disk-boot ($C0Ex, needs M5 IWM). Real kbd/mouse routing done (`adb_test`: IRQ keyboard + mouse, ⌘-menu shortcuts). BRAM host-file persistence + ROM 01 banner = follow-ups |
| **M5** | Disk (IWM) + **//e legacy** | POM2-lineage IWM + native `Sony35` 3.5" LLE. **Plus full Apple //e compatibility**: main/aux memory redirection (RAMRD/RAMWRT/80STORE/PAGE2), LORES/HGR/DHGR video (reuse POM2 `Apple2Display`), so 8-bit //e software runs. (SWIM: out of scope — Mark Twain prototype only, never shipped; ROM 01 **and** 03 use the IWM, MAME apple2gs.cpp:15/3891.) | 🟡 IWM 5.25" read path + //e HGR/LORES video done; **real IWM 3.5" Sony LLE done** (`Sony35`, `iwm35 = 1` — GS/OS boots to the Finder via the genuine slot-5 ROM firmware). 5.25" write + WOZ done (`iwm525_test`), NTSC/RGB colour done (`dhgr_test`); 3.5" read/write/verify (`floppy_rw35_test`) and SmartPort FORMAT of an unformatted WOZ (`sony_format`) through the genuine slot-5 firmware gated |
| **M6** | Ensoniq 5503 DOC | `Es5503` — 32 osc, 64 KB sound RAM, Sound GLU ($C03C-$C03F) | 🟢 MAME es5503 parity (July 2026): swap mode + partner start + retrigger quirk, $E0 IRQ protocol, native-rate pitch (894886/(N+2) Hz→host), GLU bit6/bit5 decode + $C03D read latch FIXED (were swapped/missing → DOC was silent). Gate: doc_test (16 checks). Interactive gate = synthLAB music |
| **M7** | Serial + slots | `Scc8530` serial | 🟡 SCC loopback (scc_test gate); slot bus / SmartPort / Mockingboard reuse from POM2 = follow-up |
| **M8** | Polish | WASM build (Emscripten) | 🟡 `./build_wasm.sh` → POMIIGS.html/.js/.wasm (emscripten main loop; audio = silent stub). Snapshot done (F7/F8, `snapshot_test`) and the CLI/config are in; rewind ring + packaging = follow-ups |

## Parity dashboard (MAME `apple2gs.cpp` → POMIIGS)

| Hardware | MAME reference | POMIIGS | State |
|---|---|---|---|
| 65C816 CPU | `cpu/g65816/` | `CPU65816` | 🟢 5.12M Tom Harte vectors green (all 256 opcodes ×2 modes, every cycle) |
| FPI speed/shadow regs ($C035-$C037) | `apple2gs.cpp` | `IIgsMemory` | 🟢 SHADOW write-through for every region + SPEED motor-detect coupling (`speed_test`, `slowside_test`). $C037 DMAREG is latched only — DMA / ROM 03 shadow-all not modelled |
| Mega II slow-side + I/O shadow | `apple2gs.cpp` | `IIgsMemory` | 🟢 $E0/$E1 RAM + I/O; text/HGR/**SHR** shadow ($01→$E1, SuperHiRes over Hi-Res ranges) and the aux-HGR inhibit ($C035 bit4, MAME `b1ram2000_w`) |
| STATEREG ($C068) | `apple2gs.cpp` | `IIgsMemory` | 🟢 compose/decompose |
| VGC Super Hi-Res + SCB/palette | `apple2gs.cpp` | `VGC` | 🟢 320/640 render (vgc_test) |
| VGC scanline / VBL interrupt | `apple2gs.cpp` | `IIgsMemory` | 🟢 VBL status + flag; **per-line SCB bit-6 scanline IRQ** fired from the tick() beam walk, $C02E/$C02F reads acknowledge (MAME :1674-1683), pinned in `irq_test`. Mid-frame render splits (3200-colour) = follow-up |
| ES5503 DOC (32 osc) | `sound/es5503.cpp` | `Es5503` | 🟢 renders tone (doc_test); mixed to miniaudio |
| Sound GLU ($C03C-$C03F) | `apple2gs.cpp` | `Es5503` | 🟢 |
| 1-bit speaker ($C030) | `apple2gs.cpp` | `IIgsMemory`+`AudioOut` | 🟢 cycle-exact square wave → miniaudio |
| Audio host (miniaudio) | — | `AudioOut` | 🟢 mono f32 ring, speaker+DOC mix (native; WASM stub) |
| ADB GLU (keyboard/mouse) | `apple2gs.cpp` | `IIgsMemory` | 🟡 mouse **interrupt** ($C024) + **keyboard interrupt** (key→IRQ_SRC_ADB→ROM `$FE:EC99` reads $C000→PostEvent; typing selects Finder icons) + modifiers ($C025→event via `$FEE267` table, **⌘-key menu shortcuts fire**); native+VBL gated, storm-safe; `adb_test`. Full ADB µC command model = follow-up |
| Battery RAM + RTC | `apple2gs.cpp` | `IIgsMemory` | 🟢 $C033/$C034 full serial protocol (KEGS decode): RTC seconds = host **local** time + guest-set offset, BRAM read/write + internal regs, 2-strobe read timing; Control Panel shows correct date/time; both persist in `states/bram.bin` (`bram_test`) |
| IWM (5.25/3.5) | `machine/iwm.cpp` | `Iwm` | 🟢 5.25" bit-cell **read+write+WOZ** (POM2 `DiskImage` port — Choplifter boots to gameplay, protected WOZ originals boot via the $C600 PROM, writes persist; gate `iwm525_test`; ENABLE2 + PH1-sense modelled); **3.5" Sony LLE 🟢** — `iwm35 = 1` routes 800K media to the real IWM/Sony drive, the genuine slot-5 ROM firmware drives it, **GS/OS 6.0.1 boots to the Finder** (gate: `iwm35_test`) |
| Sony 3.5" drive + 800K GCR codec | `floppy.cpp` mac_floppy + KEGS `iwm.c` | `Sony35` | 🟢 status/command tables, read+write, codec = exact KEGS/ROM nibblizer port; block write/read/verify (`floppy_rw35_test`) and FORMAT + tach check (`sony_format`) through the ROM firmware gated |
| SWIM1 (MFM/1.44M) | `machine/swim1.cpp` | — | ⚪ out of scope: SWIM never shipped on a production IIgs (only the unreleased 1991 "Mark Twain" prototype, MAME apple2gs.cpp:15/3891-3896); ROM 01 **and** ROM 03 drive the IWM |
| SCC 8530 serial | `machine/scc8530.cpp` | `Scc8530` | 🟢 loopback (scc_test) |
| Mega II interrupt regs ($C041-$C047) | `apple2gs.cpp` | `IIgsMemory` | 🟡 INTEN/INTFLAG/VBL |

## Reuse-from-POM2 checklist

Shared hardware where POM2's implementation ports/links directly. Verify each
still behaves under the IIgs bus (slow-side timing, 24-bit addresses) before
ticking:

- ⚪ `M6502` — legacy 6502/65C02 not needed (65C816 covers emulation mode) —
  **decision: drop; the 816 in E-mode is the fallback.**
- 🟢 `Memory` IIe paging / language card — folded into the `IIgsMemory` slow side
  (`physBank01` main/aux redirect, `lcSwitch`, `slowLcRead`/`slowLcWrite`).
  Gates: `slowside_test`, `speed_test`.
- 🟡 `Apple2Display` (text/LORES/HGR/DHGR) + `NtscPostProcessor` — the modes and
  the composite decoder are ported into `VGC` / `VGCNtsc.h` (gates `vgc_test`,
  `dhgr_test`, `dhgr_page_test`, `text80_test`). `CrtEffectStack` (scanlines /
  bloom / curvature) is **not** ported.
- 🟡 `AudioDevice` / `SpeakerDevice` — folded into `AudioOut` (miniaudio host +
  cycle-exact speaker + DOC mix). `Mockingboard` / `Ssi263` = follow-up.
- 🟢 `IWMDevice` / `DiskImage` / WOZ / 2mg — `DiskImage.{h,cpp}` + `TwoImg.h`
  ported verbatim and driven by `Iwm` bit-by-bit (gates `iwm525_test`,
  `twoimg_test`). `Block512Backing` was not needed: `ProDosHdd` backs blocks
  natively.
- 🟢 `SmartPortCard` / `Sony35Drive` / `Disk35Image` — covered natively: slot-5
  SmartPort HLE (`ProDosHdd`) + real IWM/Sony 3.5" LLE (`Sony35`, `iwm35 = 1`)
- 🔴 `SlotBus` / `SlotPeripheral` (wire-OR IRQ)
- 🟡 `MachineSnapshot` / `RewindBuffer` — `Snapshot.{h,cpp}` saves/loads the whole
  machine (F7/F8, `snapshot_test`); the rewind ring is not written.
- ⚪ `CliDispatcher` / `EmulationController` — **decision: not forked.** Flag +
  `pomiigs.cfg` parsing lives in `main.cpp`, and the frame loop (master-tick
  budget, 2.8/1.02 MHz) in `main.cpp` + `Ui`.

## Post-M8 backlog — specialist gap analysis (what makes it a *real* IIgs)

POMIIGS runs the 8-bit / Apple II world well (ROM, ProDOS, Total Replay, colour
HGR, keyboard/joystick). The 16-bit IIgs experience (GS/OS, the desktop, sound)
needs the following, in rough priority order.

**P1 — Sound (the machine's identity)**
- 🟢 Wire the Ensoniq 5503 DOC to real audio output (miniaudio) — `AudioOut`
  mixes `Es5503::render` into the output ring each frame.
- 🟢 1-bit **speaker** ($C030 toggle) → audio — cycle-exact square-wave
  reconstruction from the MMU toggle stamps (Total Replay games audible).
- 🟢 DOC **oscillator IRQ** — per-osc pend flags + the $E0 stream protocol
  (lowest pending, read-acks one, bit7 = none), mirrored to the CPU IRQ line.
- 🟢 DOC accuracy (July 2026, MAME es5503 parity): swap mode + partner start +
  even-osc retrigger quirk, phase-preserving free-run wrap, native-rate pitch
  (894886/(oscs+2) Hz), cycle-driven production (tickMaster → ring →
  self-balancing drain + linear interp), Sound GLU bit6/bit5 decode + $C03D
  read latch. Gate: doc_test (16 checks).
- 🟢 **synthLAB VALIDATED interactively** (July 2026): mount, tempo, level,
  clean playback. The historical "random cracks" were **$C031 toggling the
  speaker** (it's DISKREG on the IIgs — every 16-bit `LDA $C030` beep collapsed
  to clicks) + the speaker's **DC offset amplifying device-level gaps** (fixed
  with an AC-coupling DC blocker, fc≈20 Hz) + audio-clock drift (fixed with
  dynamic rate control ±0.65%). User verdict: "Il n'y a plus aucun gros cracks".
- 🔴 **Very light residual cracks, rare, during synthLAB playback** (user:
  "exceptionnellement de très légers cracks"). Next probe when picked up:
  POMDBG during playback — if `underruns`/`drops` tick at crack moments →
  scheduling (move mixing to the audio callback thread); if zero with a clean
  POMWAV → host audio stack (test `aplay` of the capture). Low priority.
- 🔴 DOC follow-ups: stereo/channel routing (ctrl bits 4-7), sync/AM modulation.
- 🔴 WASM audio: Web Audio backend needs the Emscripten audio-worklet link
  flags; the WASM build ships a silent `AudioOut` stub for now.

**P2 — GS/OS boot (3.5" + desktop)**
- 🟢 **MILESTONE: GS/OS 6.0.1 boots to the complete Finder desktop.** Full menu
  bar (🍎 File/Edit/Windows/View/Disk/Special/Colors/Extras), mouse cursor,
  **System.Disk** volume icon, Trash; the "Welcome" window is dismissed.
  Screenshot-verified, all 13 gates green. Got here via four fixes: interrupt
  vectors (`$C071-$C07F` ROM + native vector pull); full fast-RAM backing (8 MB —
  was a 1 MB `fastCell` bank wrap corrupting the zero page); bank-1 language-card
  RAM (`lcRead`/`lcWrite` use aux for bank `$01`); and the **SHR fast-side
  shadow** — `maybeShadow` must check the SuperHiRes gate in the `$2000-5FFF`
  ranges it shares with Hi-Res (GS/OS inhibits Hi-Res shadowing but enables SHR),
  so the menu bar / disk icon drawn to `$01:2xxx` finally reach `$E1`. The desktop
  was never input-gated — the register-level ADB mouse + the mouse interrupt were
  a parallel effort, not the blocker.
- 🟡 **(historical) GS/OS boots to the "Welcome to the IIgs" splash** (icon +
  QuickDraw text + progress bar).
  Diagnosis (July 2026): 800K 3.5" `.po`/`.2mg` images are standard ProDOS
  volumes; the slot-7 ProDOS block device loads + runs their boot code; our SHR
  path renders their splash faithfully. GS/OS then clears $C029 (SHR off) and
  stalls — it needs the rest of the boot chain to continue. Arkanoid II behaves
  the same (SHR dialog → stall; reacts to keys). Test disks in `disks35/` and
  `docs/System 6.0.1/`.
- 🟢 **Slot-5 SmartPort 3.5" drive** (`disk35_`, ROM $C500 = SmartPort,
  $Cn07=$00 + extended bit). The dispatch entries are **WDM traps** handled in
  C++: `$Cn50` = ProDOS block ($42-$47), `$Cn53` = SmartPort (`JSR` + inline
  cmd/param-list). Implements **STATUS** (+ DIB, code 3), **READBLOCK**,
  **WRITEBLOCK**, both standard and the **GS/OS extended** long-address form
  (4-byte buffer + block, any bank). UI: File ▸ Load 3.5" Disk. Gates:
  `disk35_test`, `smartport_test`. **GS/OS 6.0.1 boots to its Welcome screen
  through the real SmartPort path.** Full GS/OS still needs the toolbox (below);
  the low-level IWM-3.5"/Sony path (POM2 `Sony35Drive`) remains an alternative
  for games that bit-bang the drive.
- 🟡 **GS toolbox** — runs from ROM (LLE), not reimplemented. GS/OS starts the
  whole toolbox correctly (Tool Locator, Memory Mgr, QuickDraw II, Event/Window/
  Menu/Control Mgrs, the Loader — every `_xxStartUp` dispatches from ROM; watch
  with `gsos_trace`). **The post-welcome crash is fixed**: the IIgs interrupt
  path was incomplete — `$C071-$C07F` now returns internal ROM (the native
  vector stubs live there: IRQ `$FFEE`=`$C074`=`CLV/JML $E10010`), and native
  vector pulls read ROM even under LC RAM (`IIgsMemory::vectorPull`). GS/OS now
  runs stably past the welcome screen, and (per the milestone above) all the way
  to the Finder desktop. Remaining: the toolbox itself is never reimplemented —
  this entry stays 🟡 only because ROM-LLE coverage is verified by observation
  (`gsos_trace`), not by a gate.
- ⚪ **SWIM** — dropped: never shipped on a production IIgs (Mark Twain
  prototype only); ROM 01 and 03 both drive the IWM (MAME apple2gs.cpp:15).
- 🟡 **ADB mouse interrupt + keyboard modifiers** — mouse motion/button raise
  `IRQ_SRC_ADB`; the ROM interrupt manager services it via ReadMouse
  (`$FE:B1E1` → `$C024`). Storm-safe (gated on native + VBL-int-enabled, +2-frame
  drop). KEYMODREG ($C025) + `$C024`/`$C027` register interface + host wiring;
  `adb_test`. **Edge case:** mouse motion *during* the disk-load boot phase can
  disrupt the desktop draw — the native+VBL gate is too loose there; a proper fix
  needs the ADB µC enable state (below). Post-desktop mouse motion is safe.
  **Command-key menu shortcuts fire** (⌘-A flashes the Edit menu): the ROM maps
  `$C025` → GS event modifiers via the `$FEE267` table (bit7→appleKey), and
  `main.cpp` delivers the modifier-combo key via `IsKeyPressed` (ImGui suppresses
  `InputQueueCharacters` for it). Follow-ups: (a) full **ADB µC command model**
  (TALK/LISTEN/SRQ) for a hardware-accurate interrupt-enable in place of the
  native+VBL proxy; (b) verify mouse *cursor movement* end-to-end (needs
  directional host motion).

**P3 — VGC completeness (games / demos / apps)**
- 🟢 **Scanline interrupts** (SCB bit6) — per-line beam walk latches `$C023` bit5
  and raises the VGC IRQ; `$C032` writes + `$C02E`/`$C02F` reads acknowledge.
  Gate: `irq_test`. 🟢 The renderer draws from the live scanout (per-line
  SCB/palette/mode latch, per-cycle fetches — `scanout_test`), so mid-frame
  split modes (640 menu + 320 gfx, 3200-colour pictures) show.
- 🟢 SHR **colour-fill** mode (SCB bit5 — index-0 repeats the previous pixel)
  and **border colour** ($C034, drawn as an authentic frame around the display).
  Gate: `shr_test`.
- 🟢 **Double Hi-Res** (DHGR 140×192, 16 colour) — aux/main interleave,
  Composite NTSC + Clean RGB (same toggle as HGR). Gate: `dhgr_test`.
  80STORE display-page quirk honoured (PAGE2 = aux-bank select, not page
  flip, when 80STORE+HIRES) → Total Replay DHGR title fades correct. Gate:
  `dhgr_page_test`.
- 🟢 **80-column text** (aux/main interleaved 80×24). Gate: `text80_test`.
- 🟢 **Text colour** from $C022 (fg/bg via the 16-colour lo-res palette;
  applies to 40- and 80-col). Interlaced/VOC mode still TODO.

**P4 — Timing + interrupts**
- 🟢 Real **2.8 / 1.02 MHz** fast/slow clock. The host loop now accounts a
  frame in **master-clock ticks** (238944 = 912×262): slow mode consumes 64
  14-tick cycles plus one 16-tick cycle per line. Fast Mega II accesses wait
  for the next PH0 boundary and occupy one complete slow cycle; fast DRAM
  accesses stretch only when they intersect the 5-in-50-tick refresh window,
  while ROM and the fast FPI registers hide refresh. A motor-detect slot's
  spinning Disk II still holds the whole machine at 1.02 MHz without a second
  differential penalty (MAME `update_speed`). Gates: `megaii_timing_test`,
  `speed_test`, `slowside_test`. Remaining nuance: inactive CPU cycles are
  count-correct but not yet positioned in the bus scheduler, and reset phase
  still needs a real-IIgs trace oracle.
- 🟡 IRQ set: **VBL** (tick edge), **¼-second + 1-second** timers (frame-driven,
  60 Hz), **scan-line** (VGCINT enable + SCB bit6), and **DOC** oscillator IRQ
  (IRQ-enabled osc completes → CPU line, cleared by the $E0 osc-int reg) are
  wired through their real registers ($C023/$C032/$C041/$C046/$C047) onto the
  wire-OR CPU lines. Gate: `irq_test`. **IRQ dispatch now vectors correctly**:
  native/emulation vector table lives in the `$C071-$C07F` reserved-reads-ROM
  region ($FFEE=$C074=`CLV/JML $E10010`), and native vector pulls read ROM under
  LC RAM — so GS/OS's VBL IRQ reaches the ROM Interrupt Mgr instead of crashing.
  **ADB** now has a real source too: keyboard and mouse events raise
  `IRQ_SRC_ADB` (gate `adb_test`, see P2). Remaining: **SCC** has no active
  source (loopback only), the hardware-accurate ADB interrupt *enable* still uses
  the native+VBL proxy instead of the µC model, and scan-line is IRQ-only (the
  renderer doesn't split a frame mid-screen yet — see P3).

**P5 — Peripherals / infra**
- 🟢 **Battery RAM + RTC** — full `$C033`/`$C034` serial protocol, host local time,
  256-byte BRAM read/write. 🟢 BRAM + clock offset persist in `states/bram.bin`
  (`bram_test`); the ROM seeds the Control Panel defaults on a missing file.
- 🟢 Slot **internal firmware** `$C100-$CFFF` — `slotRomRead` serves the bank-`$FF`
  ROM image for unclaimed slots plus the `$C800` window (an *empty* slot 7 reads
  `$00`, see the AppleTalk false-positive in `docs/COMPAT.md`).
- 🔴 Real **SCC** serial (modem/printer/AppleTalk) — loopback only.
- 🟢 **Save states** — F7/F8 → `states/quick.pgss` (`Snapshot.h/.cpp`,
  `snapshot_test`).
- 🔴 **Mockingboard**, rewind ring, debugger, configurable RAM/slots/Control Panel.

## Answered questions

The three questions this section opened with have all been settled by shipped
code — kept here because the *reasoning* is load-bearing:

- **ROM 03's 8 MB: flat array or paged expansion card?** → **Flat.**
  `fastRam_` is one `std::vector` covering banks `$00-$7F`, sized by
  `setFastRamKB` (default 8192 KB, clamped to [256 KB, 8 MB]). A 1 MB backing
  wrapped bank indices and corrupted the zero page — that was the last blocker
  before the Finder desktop drew.
- **DOC IRQ cadence vs a cycle-stamped audio bus.** → POM2's CPU-cycle
  `emuCycles` stamp does not survive the IIgs's per-access clock switching, so
  POMIIGS stamps in **master ticks** instead and the DOC produces one native
  sample every `16×(oscs+2)` of them (`tickMaster`). Per-frame batch rendering
  merged 3-4 timer IRQs into 1 at 60 Hz and played music ~3× too slow; the
  master-tick production path fixed it. See `CLAUDE.md § Conventions`.
- **Snapshot format for the 816's wider register file.** → `Snapshot.{h,cpp}`
  ("PGSS" + version, currently v4) writes 16-bit A/X/Y/SP/D/PC plus DBR, PBR, P
  and the E flag, then delegates to `IIgsMemory::saveState`. A version mismatch
  is refused outright rather than half-loaded. Gate: `snapshot_test`.

Still genuinely open: see the 🔴 items in the backlog above.
