# CLAUDE.md

Orientation **always-loaded index** — keep terse, defer detail to other docs.

POMIIGS is an **Apple IIgs** emulator. It is the 16-bit sibling of
[POM2](../POM2/) (the Apple II-family emulator) and deliberately reuses POM2's
architecture, conventions, and — where the hardware is shared — its actual
code: `DiskImage`/`TwoImg`/`Logger` (5.25" bit cells, WOZ, 2IMG) are ported
verbatim, and the legacy //e video + NTSC composite decode came across into
`VGC`/`VGCNtsc.h`. The slot bus, Mockingboard and POM2's CRT effect stack are
**not** ported (see `TODO.md § Reuse-from-POM2 checklist`).

- `README.md` — user walkthrough (build, ROM/disk placement, keys, CLI).
- `DEV.md` — implementation deep-dives (MAME-parity ports, internals, gotchas, pinned tests).
- `TODO.md` — active backlog + **MAME↔POMIIGS parity dashboard** + milestone roadmap.
- `CHANGELOG.md` — resolved items + the **why** behind non-obvious fixes.

## Source of truth

The IIgs is documented by several emulators; POMIIGS pins its behaviour to a
ranked reference set (recorded here so every port cites the same source):

1. **MAME `apple2gs.cpp`** (R. Belmont) — *primary*. C++, cycle-accurate,
   models the 14.31818 MHz master clock and Mega II fast/slow-side sync,
   cites the Apple II Documentation Project. When porting hardware, **cite
   the MAME file + line range in a comment** and pin with a smoke test under
   `tests/` — identical discipline to POM2.
   <https://github.com/mamedev/mame/blob/master/src/mame/apple/apple2gs.cpp>
2. **GSSquared** (Jawaid Bazyar, GPLv3) — modern C++/SDL3 from-scratch peer;
   mirror its subsystem decomposition (`cpus/`, `mmus/`, `devices/es5503`,
   `devices/adb`, `devices/iwm`, `devices/rtc`, …).
   <https://github.com/jawaidbazyar2/gssquared>
3. **KEGS** (Kent Dickey) — readable canonical C implementation; good for
   overall architecture + IWM/disk logic. <https://github.com/a2kegs/kegs>
4. **Clemens IIGS** (MIT) — permissively-licensed structural reference +
   self-diagnostic parity target. <https://github.com/samkusin/clemens_iigs>
5. **Crossrunner** (closed source) — behavioural *oracle* only: compare
   observable behaviour, never code. <https://www.crossrunner.gs/>

Primary hardware docs to cite directly: *Apple IIgs Hardware Reference* &
*Firmware Reference* (Addison-Wesley), *Apple IIgs Technical Notes*, the WDC
**W65C816S** datasheet, and the **Ensoniq 5503 DOC** datasheet.

## Conventions (inherited from POM2)

- **One concern per file** — each `.cpp/.h` pair owns one subsystem.
- **MAME = source of truth** — cite `apple2gs.cpp` file + line range in a
  comment; pin with a smoke test under `tests/`.
- **One timebase: master ticks** — every CPU → audio/UI/video event is stamped
  with `IIgsMemory::videoCycles_` (exposed as `audioCycles()`), a running count
  of **14.31818 MHz master-clock ticks**. POM2 stamps in CPU cycles; the IIgs
  cannot, because the CPU switches between **2.8 MHz fast and 1.02 MHz slow**
  per access (see Memory) — a CPU-cycle stamp is not a duration. Counting raw
  CPU cycles is precisely the bug that fired VBL at ~164 Hz and ran every
  VBL-clocked game 2.8× too fast (see `CHANGELOG.md`).
- **Docs in English** — reference language for all Markdown.
- **License**: GPLv3 (POM2 is GPL; MAME/GSSquared/KEGS are GPL). ROMs are
  **user-provided** and never committed.

## Build & run

System packages first: a C++17 compiler, CMake ≥ 3.16, and the **GLFW 3.3+ and
OpenGL** dev packages (CMakeLists finds them via `find_package`/pkg-config).
`setup_imgui.sh` installs none of that — it only clones Dear ImGui and makes
`build/`.

```bash
./setup_imgui.sh             # one-time: clones imgui/ + creates build/
cd build && cmake .. && make # → build/POMIIGS
./run_emulator.sh            # cwd = repo root so roms/ probes resolve
```

Master clock **14.31818 MHz**. Fast CPU **2.8 MHz** (÷5); slow-side **1.02 MHz**
uses 64 cycles at ÷14 plus one ÷16 cycle per 912-tick scanline. ROMs are
user-provided: **ROM 03** (256 KB,
→ banks `$FC`-`$FF`) and **ROM 01** (128 KB, → `$FE`-`$FF`); probe order (rom03
first) in [System profiles](#system-profiles). Char ROM 344s0047 (16 KB; a 4 KB
or 2 KB //e char ROM also loads) → `roms/iigs-char.rom` for text.

## Subsystem map

Detail lives in `DEV.md`. POMIIGS is a compact self-contained codebase (14
subsystems), **not** a POM2 link-fork — it reuses POM2's *conventions* and ports
its hardware logic into these files. 🟢 = working + pinned test.

| Subsystem | Files | Status | Source |
|---|---|---|---|
| **65C816 CPU** (emul + native, 24-bit, all opcodes, microsequenced) | `CPU65816.h/.cpp` | 🟢 5.12M Tom Harte vectors (all 256 opcodes ×2 modes, every bus cycle incl. internal ones) | MAME `g65816/`, WDC datasheet |
| **MMU** — FPI + Mega II (16 MB banks, shadow, speed, //e main/aux redirect on `$00`+`$E0`, STATEREG, VBL/Mega II IRQ timing) | `IIgsMemory.h/.cpp` | 🟢 | MAME `apple2gs.cpp`, KEGS |
| **ADB GLU** (keyboard/mouse/modifiers, HLE) — in the MMU file | `IIgsMemory.h/.cpp` | 🟢 IRQ kbd/mouse, ⌘-menu shortcuts (`adb_test`) | MAME `apple2gs.cpp` ADB GLU |
| **Battery RAM + RTC** ($C033/$C034 serial) — in the MMU file | `IIgsMemory.h/.cpp` | 🟢 host local time + guest-settable offset; BRAM r/w (ROM self-test 07/08); both persist in `states/bram.bin` (`bram_test`) | KEGS clock.c, MAME |
| **SmartPort / ProDOS HDD** (slot-7 block device; slot-5 3.5" HLE via the `WDM $C5`/`$C6` traps) | `IIgsMemory.h/.cpp` + `ProDosHdd.h/.cpp` | 🟢 GS/OS installs+boots from HDD | KEGS, Apple SmartPort firmware |
| **VGC** — Super Hi-Res 320/640 + SCB/palettes, **and** legacy 40/80-col text (char ROM 344s0047) + HGR/DHGR (NTSC-composite / RGB-clean) → 640×400 GL, drawn from a **live per-cycle scanout** | `VGC.h/.cpp`, `VGCNtsc.h` | 🟢 SHR/text/HGR/DHGR render per scanline from the beam capture (`scanout_test`) + per-line SCB scanline IRQ ($C023/$C032, $C02E/2F ack — `irq_test`) | MAME `apple2gs.cpp` VGC |
| **Ensoniq 5503 DOC** — 32 osc, 64 KB sound RAM, Sound GLU ($C03C-$F) | `Es5503.h/.cpp` | 🟢 MAME es5503 parity (`doc_test`) | MAME `es5503.cpp`, Ensoniq datasheet |
| **Audio host** — miniaudio mono-f32 ring; speaker ($C030) + DOC mix | `Audio.h/.cpp` | 🟢 (native; WASM stub) | POM2 AudioDevice pattern |
| **IWM** — 5.25" bit-cell read/**write** + **WOZ** (POM2 `DiskImage` port) + **3.5" Sony LLE** | `Iwm.h/.cpp`, `DiskImage.h/.cpp`, `Sony35.h/.cpp`, `TwoImg.h`, `Logger.h` | 🟢 5.25": .dsk/.po/.nib/.d13/.2mg/.woz via the $C600 PROM — **Choplifter boots to gameplay, protected WOZ originals (A.E.) boot**; writes persist (`iwm525_test`). 3.5": `iwm35 = 1` → real Sony drive, **GS/OS boots to the Finder via the genuine slot-5 ROM firmware** (`iwm35_test`); `.2mg`/`.po`/**WOZ2** media, FORMAT + write-back cross-checked with CiderPress2 (`sony_woz_test`, `sony_format_woz`) | POM2 `DiskImage`, MAME `iwm.cpp`+`floppy.cpp`, KEGS `iwm.c` |
| **SCC 8530 serial** | `Scc8530.h/.cpp` | 🟢 loopback (`scc_test`) | MAME `scc8530.cpp` |
| **Snapshot** (save/load state, F7/F8 → `states/quick.pgss`) | `Snapshot.h/.cpp` | 🟢 (`snapshot_test`) | POM2 pattern |
| **UI** (ImGui desktop chrome, menus, file picker) | `Ui.h/.cpp` | 🟢 | — |
| **Main loop / config / CLI** (GLFW+GL) | `main.cpp` | 🟢 `pomiigs.cfg` + flags | — |
| **WebAssembly build** | `build_wasm.sh` | 🟡 builds; audio = stub | — |

## Memory map (24-bit, banked)

The 65C816 addresses **16 MB** as 256 × 64 KB banks (`$00`–`$FF`). The IIgs
splits into a **fast side** (FPI, 2.8 MHz: banks `$00`–`$7F` RAM, `$FC`–`$FF`
ROM) and a **slow side** (Mega II, 1.02 MHz: banks `$E0`/`$E1`, the classic
Apple II I/O + video). See `DEV.md § Memory` for the full model.

```
Bank $00        Classic Apple II RAM image (zero page, stack, text, HGR).
                Fast-side RAM, but I/O ($00/C000-CFFF) is SHADOWed to $E0.
Bank $01        Fast-side RAM; shadowed to $E1 when SHR/text/hires shadow on.
Bank $02-$7F    Fast-side expansion RAM (up to 8 MB on ROM 03; 1 MB stock).
                POMIIGS backs the full 8 MB by default (setFastRamKB).
Bank $E0        Mega II slow RAM (aux/main //e image) + LIVE I/O space:
  $E0/C000-CFFF   IIgs I/O — softswitches, GLU registers (below).
Bank $E1        Mega II slow RAM; $E1/2000-9FFF holds the Super Hi-Res buffer.
Bank $FC-$FF    ROM (Applesoft/Monitor at $FF; toolbox + firmware below).

Slow-side I/O ($E0/E1 $Cnnn), also visible at $00/$01 $Cnnn via shadow:
  $C000-$C08F  Classic //e softswitches + IIgs new-register block
  $C000/$C010  Keyboard latch / any-key-down + strobe clear
  $C019        VBL status                 $C01A-$C01F  //e mode readbacks
  $C022        SCREENCOLOR (text fg/bg)   $C023  VGC interrupt enable
  $C024-$C027  ADB GLU: MOUSEDATA / KEYMODREG / DATAREG (µC) / KMSTATUS
  $C029        NEWVIDEO (SHR enable, linearize)
  $C02B        LANGSEL (char-ROM language b7-5, PAL b4)
  $C02D        SLOTROMSEL (per-slot internal/card)
  $C02E/$C02F  VERTCNT / HORIZCNT beam counters (also ack the scanline IRQ)
  $C030        SPEAKER toggle             $C031  DISKREG (b6 35SEL, b7 HDSEL)
  $C032        VGCINTCLEAR (write-0-to-clear per bit)
  $C033/$C034  CLOCKDATA / CLOCKCTL — RTC + BRAM serial; $C034 b0-3 = border colour
  $C035        SHADOW register (bank $00/$01 shadow inhibit per region)
  $C036        SPEED/CYAREG (bit 7 = 2.8 MHz, bit 4 = shadow ALL banks, bit 0-3 slot motor detect)
  $C037        DMAREG — latched only (DMA not modelled)
  $C038-$C03B  SCC 8530 serial (command/data, ports A/B)
  $C03C-$C03F  Sound GLU: $C03C ctrl, $C03D data (auto-inc), $C03E/F addr (→ DOC)
  $C041/$C046/$C047  Mega II INTEN / INTFLAG / CLRVBLINT
  $C050-$C05F  //e video softswitches + (IIgs) DHIRES ($C05E/$C05F)
  $C061-$C067  Buttons (open/solid-apple) + paddles PADDL0-3
  $C068        STATEREG — composite MMU state (ALTZP/PAGE2/RAMRD/… in one byte)
  $C070        PTRIG — paddle-timer strobe
  $C071-$C07F  Reserved reads — always internal ROM (the native vector stubs)
  $C080-$C08F  Language Card bank switching (slow-side D000-FFFF)
  $C0E0-$C0EF  IWM (slot 6 5.25"; slot 5 3.5" under $C031 35SEL)
  $Cn00-$CnFF  Slot n firmware (INTCXROM/SLOTROMSEL gated, IIgs $C02D)
```

## System profiles

| Profile | ROM | CPU boot mode | Notes |
|---|---|---|---|
| Apple IIgs ROM 01 (1986) | `iigs-rom01.rom` (128 KB) | 65C816 emul → native | DOC, VGC, ADB, IWM. 256 KB–1 MB RAM. Best compatibility. |
| Apple IIgs ROM 03 (1989) | `iigs-rom03.rom` (256 KB) | 65C816 emul → native | IWM (like ROM 01 — SWIM only ever shipped on the unreleased 1991 "Mark Twain" prototype), shadow-all banks (CYAREG $C036 bit 4), up to 8 MB RAM. |

The ROM probe accepts a 128 KB or 256 KB image and reports the size it loaded;
anything else is rejected with a console warning (no checksum is verified).
Probe order: `roms/iigs-rom03.rom`, then `roms/iigs-rom01.rom`. Default = ROM 03.

## Reset architecture

Two paths, as on the machine:

- **/RESET** — `Ui::doReset()` (F5 / Machine ▸ Reset) = `IIgsMemory::softReset()`
  + `CPU65816::softReset()`. RAM and the master clock survive; every soft
  switch re-parks (LC = read ROM / write enable / bank 2, `$C031` DISKREG
  cleared, shadow/speed/NEWVIDEO reset), the DOC, SCC, IWM latches and ADB GLU
  transients reset. The CPU re-enters **emulation mode** (E=1) and pulls the
  reset vector from `$00/FFFC` through `vectorPull` (ROM regardless of the
  language card). The ROM then reads ⌘/Option (`$C061/$C062`, `$C025`) to
  choose a warm start, a cold boot (⌘ = Left Alt held) or the self-test
  (⌘+Option = both Alts) — the same decision it makes on hardware.
- **Power cycle** — `Ui::doPowerCycle()` (Shift+F5 / Machine ▸ Power Cycle) =
  `IIgsMemory::reset()` + `CPU65816::hardReset()`: fast and slow RAM cleared to
  `$00`, then the /RESET path.

BRAM/RTC survive both (battery-backed — `bram_` is untouched) and persist
across runs in `states/bram.bin` (loaded before the first boot, saved on change
and on exit; the ROM seeds the Control Panel defaults when the file is absent).
Gate: `reset_test`.

## Status

**Broadly working — GS/OS boots.** Twelve differential bug-sweep passes brought
POMIIGS to broad KEGS/MAME/GSSquared parity:

- 65C816 🟢 (5.12M Tom Harte vectors, all 256 opcodes, every bus cycle; gilyon
  1610/1610 and Klaus functional cross-checks), FPI/Mega II MMU 🟢 (shadow, speed,
  //e main/aux redirect, STATEREG, VBL + Mega II quarter-second IRQ).
- VGC 🟢 Super Hi-Res + SCB/palettes, legacy text (authentic char ROM),
  HGR/DHGR (NTSC + RGB). Ensoniq DOC 🟢 (synthLAB music validated).
- ADB 🟢 (IRQ kbd/mouse, ⌘-menu shortcuts), BRAM/RTC 🟢, SCC 🟢.
- **MiSTer custom diagnostics 🟡** (`tools/mister_diags.py`, 21 disks built
  with rehosted Merlin32/CiderPress2, plus a derived slot-5 floppy variant
  proving 3.5" write/read/verify on the Sony LLE, and a POMIIGS-authored
  SmartPort FORMAT diagnostic): 17 pass (MMU 26/26, GSQMMU), 6
  xfails all with non-emulator causes (three corpus disputes, the ADB µC
  firmware, the two RAM tests that overwrite a RAM launcher).
- **ROM 01/03 built-in self-test 🟢** (`selftest_trace`): every diagnostic
  passes on both ROMs except 09 (ADB), which needs the user-supplied ADB µC
  firmware (`roms/iigs-adb-uc-rom0[13].rom`) and SKIPs without it.
- IWM 5.25" **read+write, WOZ 1/2+FLUX** 🟢 (POM2 `DiskImage` port):
  **Choplifter boots to gameplay, protected WOZ originals (A.E.) boot** via
  the genuine $C600 PROM; writes persist. **SmartPort HLE 🟢 — GS/OS 6.0.1
  installs and boots from HDD to the full Finder desktop**; games run;
  save/load state (F7/F8) 🟢.
- **Real IWM 3.5" Sony LLE 🟢** (`Sony35`, `iwm35 = 1`): the genuine slot-5 ROM
  firmware drives the drive nibble-by-nibble — **GS/OS boots to the Finder**.

Open: reset-phase calibration of the PH0/
refresh grids against a hardware trace, rewind ring, WASM audio, full ADB µC
command model. (SWIM is out of scope: it only existed on the
unreleased "Mark Twain" prototype — every production IIgs uses the IWM.)
See `TODO.md` for the parity dashboard + backlog.
