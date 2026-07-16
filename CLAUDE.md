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
(÷14, the classic Apple II clock). ROMs are user-provided: **ROM 01** (256 KB)
and **ROM 03** (128 KB) — probe order in [System profiles](#system-profiles).

## Subsystem map

Detail lives in `DEV.md`. `[reuse: POM2]` = shared hardware, port/link POM2's
file; `[new]` = IIgs-specific, built here.

| Subsystem | Files | Status | Source |
|---|---|---|---|
| **65C816 CPU** (emul + native mode, 24-bit) | `CPU65816.h/.cpp` | new | MAME `g65816/`, WDC datasheet, Tom Harte 65816 |
| 65C02 / 6502 (legacy-mode fallback) | `[reuse: POM2 M6502]` | reuse | — |
| **MMU / FPI + Mega II** (24-bit space, shadow, speed) | `IIgsMemory.h/.cpp` | new | MAME `apple2gs.cpp` |
| IIe paging / language card (slow-side) | `[reuse: POM2 Memory]` | reuse | — |
| **VGC** — Super Hi-Res 320/640 + SCB/palettes + video IRQ | `VGC.h/.cpp` | new | MAME `apple2gs.cpp` VGC |
| Legacy //e video (text/LORES/HGR/DHGR) + NTSC/CRT | `[reuse: POM2 Apple2Display, NtscPostProcessor, CrtEffectStack]` | reuse | — |
| **Ensoniq 5503 DOC** — 32 osc, 64 KB sound RAM, Sound GLU | `Es5503.h/.cpp` | new | MAME `es5503.cpp`, Ensoniq datasheet |
| Audio bus / speaker / Mockingboard / SSI263 | `[reuse: POM2 AudioDevice, SpeakerDevice, Mockingboard, Ssi263]` | reuse | — |
| **ADB** — Apple Desktop Bus GLU (keyboard/mouse) | `Adb.h/.cpp` | new | MAME `apple2gs.cpp` ADB GLU |
| **Battery RAM + RTC** (persisted Control Panel) | `IIgsClock.h/.cpp` | new | MAME `apple2gs.cpp` clock |
| **IWM / SWIM** (5.25" + 3.5", ROM 01 IWM / ROM 03 SWIM) | `[reuse: POM2 IWMDevice]` + `Swim.h/.cpp` | reuse+new | MAME `iwm.cpp`, `swim.cpp` |
| DiskImage / WOZ / 2mg / ProDOS blocks | `[reuse: POM2 DiskImage, Block512Backing]` | reuse | — |
| SmartPort / 3.5" Sony stack | `[reuse: POM2 SmartPort*, Sony35Drive]` | reuse | — |
| SCC 8530 serial (2 ports) | `Scc8530.h/.cpp` | new | MAME `scc8530.cpp` |
| Slot bus + wire-OR IRQ | `[reuse: POM2 SlotBus, SlotPeripheral]` | reuse | — |
| Snapshot (save/load state, F7/F8) | `Snapshot.h/.cpp` + per-subsystem `saveState` | new (POM2 pattern) | gate: snapshot_test |
| Rewind ring | `[reuse: POM2 RewindBuffer]` + delta compression (8 MB fast side) | todo | — |
| UI (ImGui) | `MainWindow.*`, `*_ImGui.*` | new (fork POM2) | — |
| Clock & threading | `EmulationController.h/.cpp` | new (fork POM2) | — |
| System profiles (ROM 01 / ROM 03) | `SystemProfile.h/.cpp` | new (fork POM2) | — |
| CLI | `[reuse: POM2 CliDispatcher]` | reuse | — |
| WebAssembly build | `build_wasm.sh` | reuse pattern | — |

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
  $C03C-$C03F  Sound GLU: $C03C ctrl, $C03D data, $C03E/F addr ptr (→ DOC)
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
| Apple IIgs ROM 03 (1989) | `iigs-rom03.rom` (256 KB) | 65C816 emul → native | SWIM, shadow-all, up to 8 MB RAM. |

ROM probe warns on size/checksum mismatch (POM2 pattern). Default = ROM 03.

## Reset architecture

Mirrors POM2's three-class split (soft / hard / cold) — see POM2 `CLAUDE.md
§ Reset`. IIgs additions: reset re-enters **65C816 emulation mode** (E=1) with
the reset vector at `$00/FFFC`; BRAM/RTC survive all resets (battery-backed);
cold boot clears fast RAM with the `00 FF` pattern and re-seeds BRAM defaults.

## Status

**Milestone 0 — foundation (in progress).** Build system, docs, and subsystem
map established. Next: 65C816 core gated by Tom Harte `SingleStepTests/65816`,
then the FPI/Mega II MMU. See `TODO.md` for the milestone roadmap.
