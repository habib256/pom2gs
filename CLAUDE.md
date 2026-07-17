# CLAUDE.md

Orientation **always-loaded index** — keep terse, defer detail to other docs.

POMIIGS is an **Apple IIgs** emulator. It is the 16-bit sibling of
[POM2](../POM2/) (the Apple II-family emulator) and deliberately reuses POM2's
architecture, conventions, and — where the hardware is shared (IWM, disk
formats, legacy //e video, slot bus, Mockingboard, CRT/NTSC stack) — its
actual code.

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
- **`emuCycles` everywhere** — CPU → audio/UI events carry a CPU-cycle stamp,
  not wall-clock. The IIgs makes this doubly important: the CPU runs at
  **2.8 MHz fast / 1.0 MHz slow** and switches per-access (see Memory), so a
  wall-clock stamp is meaningless.
- **Docs in English** — reference language for all Markdown.
- **License**: GPLv3 (POM2 is GPL; MAME/GSSquared/KEGS are GPL). ROMs are
  **user-provided** and never committed.

## Build & run

```bash
./setup_imgui.sh             # one-time deps + clones imgui/
cd build && cmake .. && make # → build/POMIIGS
./run_emulator.sh            # cwd = repo root so roms/ probes resolve
```

Master clock **14.31818 MHz**. Fast CPU **2.8 MHz** (÷5), slow-side **1.02 MHz**
(÷14, the classic Apple II clock). ROMs are user-provided: **ROM 03** (256 KB,
→ banks `$FC`-`$FF`) and **ROM 01** (128 KB, → `$FE`-`$FF`); probe order (rom03
first) in [System profiles](#system-profiles). Char ROM 344s0047 (16 KB) →
`roms/iigs-char.rom` for text.

## Subsystem map

Detail lives in `DEV.md`. POMIIGS is a compact self-contained codebase (11
subsystems), **not** a POM2 link-fork — it reuses POM2's *conventions* and ports
its hardware logic into these files. 🟢 = working + pinned test.

| Subsystem | Files | Status | Source |
|---|---|---|---|
| **65C816 CPU** (emul + native, 24-bit, all opcodes) | `CPU65816.h/.cpp` | 🟢 384k Tom Harte vectors (64 families ×2 modes; MVN/MVP excluded) | MAME `g65816/`, WDC datasheet |
| **MMU** — FPI + Mega II (16 MB banks, shadow, speed, //e main/aux redirect on `$00`+`$E0`, STATEREG, VBL/Mega II IRQ timing) | `IIgsMemory.h/.cpp` | 🟢 | MAME `apple2gs.cpp`, KEGS |
| **ADB GLU** (keyboard/mouse/modifiers, HLE) — in the MMU file | `IIgsMemory.h/.cpp` | 🟢 IRQ kbd/mouse, ⌘-menu shortcuts (`adb_test`) | MAME `apple2gs.cpp` ADB GLU |
| **Battery RAM + RTC** ($C033/$C034 serial) — in the MMU file | `IIgsMemory.h/.cpp` | 🟢 Control Panel shows host local time; BRAM r/w | KEGS clock.c, MAME |
| **SmartPort / ProDOS HDD** (HLE via `WDM $42` trap; slot-7 block device) | `IIgsMemory.h/.cpp` + `ProDosHdd.h/.cpp` | 🟢 GS/OS installs+boots from HDD | KEGS, Apple SmartPort firmware |
| **VGC** — Super Hi-Res 320/640 + SCB/palettes, **and** legacy 40/80-col text (char ROM 344s0047) + HGR/DHGR (NTSC-composite / RGB-clean) → 640×400 GL | `VGC.h/.cpp`, `VGCNtsc.h` | 🟢 SHR/text/HGR/DHGR render + per-line SCB scanline IRQ ($C023/$C032, $C02E/2F ack — `irq_test`) | MAME `apple2gs.cpp` VGC |
| **Ensoniq 5503 DOC** — 32 osc, 64 KB sound RAM, Sound GLU ($C03C-$F) | `Es5503.h/.cpp` | 🟢 MAME es5503 parity (`doc_test`) | MAME `es5503.cpp`, Ensoniq datasheet |
| **Audio host** — miniaudio mono-f32 ring; speaker ($C030) + DOC mix | `Audio.h/.cpp` | 🟢 (native; WASM stub) | POM2 AudioDevice pattern |
| **IWM** — 5.25" bit-cell read/**write** + **WOZ** (POM2 `DiskImage` port) + **3.5" Sony LLE** | `Iwm.h/.cpp`, `DiskImage.h/.cpp`, `Sony35.h/.cpp` | 🟢 5.25": .dsk/.po/.nib/.d13/.2mg/.woz via the $C600 PROM — **Choplifter boots to gameplay, protected WOZ originals (A.E.) boot**; writes persist (`iwm525_test`). 3.5": `iwm35 = 1` → real Sony drive, **GS/OS boots to the Finder via the genuine slot-5 ROM firmware** (`iwm35_test`) | POM2 `DiskImage`, MAME `iwm.cpp`+`floppy.cpp`, KEGS `iwm.c` |
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
Bank $E0        Mega II slow RAM (aux/main //e image) + LIVE I/O space:
  $E0/C000-CFFF   IIgs I/O — softswitches, GLU registers (below).
Bank $E1        Mega II slow RAM; $E1/2000-9FFF holds the Super Hi-Res buffer.
Bank $FC-$FF    ROM (Applesoft/Monitor at $FF; toolbox + firmware below).

Slow-side I/O ($E0/E1 $Cnnn), also visible at $00/$01 $Cnnn via shadow:
  $C000-$C08F  Classic //e softswitches + IIgs new-register block
  $C019        VBL status                 $C02x  Monochrome / VGC control
  $C022        SCREEN COLOR (text fg/bg)  $C023  VGC interrupt enable
  $C029        NEWVIDEO (SHR enable, linearize)
  $C02D        SLOTROM select (per-slot internal/card)
  $C034        Border color + BRAM/clock control (bit 5-7)
  $C035        SHADOW register (bank $00/$01 shadow enable per region)
  $C036        SPEED register (bit 7 = 2.8 MHz, bit 0-3 slot motor detect)
  $C037        DMA / (ROM 03) shadow-all
  $C038-$C03B  SCC 8530 serial (command/data, ports A/B)
  $C03C-$C03F  Sound GLU: $C03C ctrl, $C03D data (auto-inc), $C03E/F addr (→ DOC)
  $C041-$C047  Mega II interrupt (IRQ) registers + mouse
  $C044-$C047  (mouse delta on some maps)
  $C058-$C05F  Annunciators / (IIgs) DHIRES + softswitch mirrors
  $C068        STATEREG — composite MMU state (ALTZP/PAGE2/RAMRD/… in one byte)
  $C070        Paddle strobe / (IIgs) reads AN inputs
  $C080-$C08F  Language Card bank switching (slow-side D000-FFFF)
  $Cn00-$CnFF  Slot n firmware (INTCXROM/SLOTROM gated, IIgs $C02D)
```

## System profiles

| Profile | ROM | CPU boot mode | Notes |
|---|---|---|---|
| Apple IIgs ROM 01 (1986) | `iigs-rom01.rom` (128 KB) | 65C816 emul → native | DOC, VGC, ADB, IWM. 256 KB–1 MB RAM. Best compatibility. |
| Apple IIgs ROM 03 (1989) | `iigs-rom03.rom` (256 KB) | 65C816 emul → native | IWM (like ROM 01 — SWIM only ever shipped on the unreleased 1991 "Mark Twain" prototype), shadow-all, up to 8 MB RAM. |

ROM probe warns on size/checksum mismatch (POM2 pattern). Default = ROM 03.

## Reset architecture

Mirrors POM2's three-class split (soft / hard / cold) — see POM2 `CLAUDE.md
§ Reset`. IIgs additions: reset re-enters **65C816 emulation mode** (E=1) with
the reset vector at `$00/FFFC`; BRAM/RTC survive all resets (battery-backed);
cold boot clears fast RAM with the `00 FF` pattern and re-seeds BRAM defaults.

## Status

**Broadly working — GS/OS boots.** Nine differential bug-sweep passes brought
POMIIGS to broad KEGS/MAME/GSSquared parity:

- 65C816 🟢 (384k Tom Harte vectors), FPI/Mega II MMU 🟢 (shadow, speed,
  //e main/aux redirect, STATEREG, VBL + Mega II quarter-second IRQ).
- VGC 🟢 Super Hi-Res + SCB/palettes, legacy text (authentic char ROM),
  HGR/DHGR (NTSC + RGB). Ensoniq DOC 🟢 (synthLAB music validated).
- ADB 🟢 (IRQ kbd/mouse, ⌘-menu shortcuts), BRAM/RTC 🟢, SCC 🟢.
- IWM 5.25" **read+write, WOZ 1/2+FLUX** 🟢 (POM2 `DiskImage` port):
  **Choplifter boots to gameplay, protected WOZ originals (A.E.) boot** via
  the genuine $C600 PROM; writes persist. **SmartPort HLE 🟢 — GS/OS 6.0.1
  installs and boots from HDD to the full Finder desktop**; games run;
  save/load state (F7/F8) 🟢.
- **Real IWM 3.5" Sony LLE 🟢** (`Sony35`, `iwm35 = 1`): the genuine slot-5 ROM
  firmware drives the drive nibble-by-nibble — **GS/OS boots to the Finder**.

Open: 3.5" FORMAT/tach calibration, mid-frame SHR palette splits (scanline IRQ
timing is real; the renderer still draws whole frames), rewind ring, WASM audio,
full ADB µC command model. (SWIM is out of scope: it only existed on the
unreleased "Mark Twain" prototype — every production IIgs uses the IWM.)
See `TODO.md` for the parity dashboard + backlog.
