# TODO.md

Active backlog + **MAME↔POMIIGS parity dashboard** + milestone roadmap.
Legend: 🔴 not started · 🟡 in progress · 🟢 done + pinned test.

## Milestone roadmap

The build order is chosen so each layer is independently testable before the
next depends on it — the CPU against Tom Harte vectors, the MMU against a ROM
self-test, then the visible/audible subsystems.

| # | Milestone | Deliverable | Gate |
|---|---|---|---|
| **M0** | Foundation | Repo, CMake (native/WASM/headless), doc suite, subsystem map | 🟢 `cmake && make` builds an ImGui window |
| **M1** | 65C816 core | `CPU65816` — emulation + native mode, 24-bit, all 256 opcodes | 🟢 Tom Harte `SingleStepTests/65816` 100% on 64 opcode families × 2 modes (384k vectors, regs+RAM+**cycles**). MVN/MVP excluded (cycle-cap granularity, see DEV). Extending corpus to full 256 = ongoing |
| **M2** | MMU / FPI + Mega II | `IIgsMemory` — 16 MB banks, shadow, speed reg, slow/fast split | 🟡 ROM 01 **and** 03 boot from ROM vector → native → self-diagnostic → speed-calibration loop ($FF:FCDC). Needs VBL/timer to progress. Verify: `boot_trace` |
| **M3** | Legacy video + VGC | Reuse POM2 `Apple2Display`; add `VGC` Super Hi-Res + video IRQ | 🔴 SHR test pattern renders; scanline IRQ fires |
| **M4** | ADB + BRAM/RTC | `Adb`, `IIgsClock` — keyboard/mouse, Control Panel persistence | 🔴 boots to Finder desktop; CP settings survive |
| **M5** | Disk (IWM/SWIM) | Reuse POM2 `IWMDevice`+`DiskImage`; add `Swim` for ROM 03 | 🔴 boots a GS/OS 3.5" image |
| **M6** | Ensoniq 5503 DOC | `Es5503` — 32 osc, 64 KB sound RAM, Sound GLU → POM2 audio bus | 🔴 a `.stg`/System 6 boot chime plays |
| **M7** | Serial + slots | `Scc8530`; reuse POM2 slot bus / SmartPort / Mockingboard | 🔴 SmartPort HDD boots; SCC loopback |
| **M8** | Polish | Snapshot/rewind reuse, CLI, WASM, kiosk, packaging | 🔴 WASM build plays in browser |

## Parity dashboard (MAME `apple2gs.cpp` → POMIIGS)

| Hardware | MAME reference | POMIIGS | State |
|---|---|---|---|
| 65C816 CPU | `cpu/g65816/` | `CPU65816` | 🟢 384k Tom Harte vectors green (64 families ×2 modes; MVN/MVP excluded) |
| FPI speed/shadow regs ($C035-$C037) | `apple2gs.cpp` | `IIgsMemory` | 🟡 shadow/speed stored; write-through partial |
| Mega II slow-side + I/O shadow | `apple2gs.cpp` | `IIgsMemory` | 🟡 $E0/$E1 RAM + I/O; display shadow partial |
| STATEREG ($C068) | `apple2gs.cpp` | `IIgsMemory` | 🟢 compose/decompose |
| VGC Super Hi-Res + SCB/palette | `apple2gs.cpp` | `VGC` | 🔴 |
| VGC scanline / VBL interrupt | `apple2gs.cpp` | `VGC` | 🔴 |
| ES5503 DOC (32 osc) | `sound/es5503.cpp` | `Es5503` | 🔴 |
| Sound GLU ($C03C-$C03F) | `apple2gs.cpp` | `Es5503` | 🔴 |
| ADB GLU (keyboard/mouse) | `apple2gs.cpp` | `Adb` | 🔴 |
| Battery RAM + RTC | `apple2gs.cpp` | `IIgsClock` | 🔴 |
| IWM (5.25/3.5) | `machine/iwm.cpp` | reuse POM2 `IWMDevice` | 🔴 |
| SWIM (ROM 03) | `machine/swim.cpp` | `Swim` | 🔴 |
| SCC 8530 serial | `machine/scc8530.cpp` | `Scc8530` | 🔴 |
| Mega II interrupt regs ($C041-$C047) | `apple2gs.cpp` | `IIgsMemory` | 🔴 |

## Reuse-from-POM2 checklist

Shared hardware where POM2's implementation ports/links directly. Verify each
still behaves under the IIgs bus (slow-side timing, 24-bit addresses) before
ticking:

- 🔴 `M6502` — legacy 6502/65C02 not needed (65C816 covers emulation mode) —
  **decision: drop; the 816 in E-mode is the fallback.**
- 🔴 `Memory` IIe paging / language card → fold into `IIgsMemory` slow side
- 🔴 `Apple2Display` (text/LORES/HGR/DHGR) + `NtscPostProcessor` + `CrtEffectStack`
- 🔴 `AudioDevice` / `SpeakerDevice` / `Mockingboard` / `Ssi263`
- 🔴 `IWMDevice` / `DiskImage` / `Block512Backing` / WOZ / 2mg
- 🔴 `SmartPortCard` / `Sony35Drive` / `Disk35Image` / SmartPort hub
- 🔴 `SlotBus` / `SlotPeripheral` (wire-OR IRQ)
- 🔴 `MachineSnapshot` / `RewindBuffer`
- 🔴 `CliDispatcher` / `EmulationController` (fork, retune clock to 2.8/1.02)

## Open questions

- ROM 03 up to 8 MB RAM: model as flat fast-side array or paged expansion card?
- DOC oscillator IRQ cadence vs POM2's `emuCycles` audio bus — confirm the
  cycle-stamp path survives the DOC's own 894.886 kHz sample clock.
- Native-mode vector pull vs POM2 snapshot format — extend `SnapshotIO` for
  the 816's wider register file (16-bit A/X/Y, DBR, PBR, D, 16-bit SP).
