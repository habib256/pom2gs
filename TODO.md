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
| **M3** | Legacy video + VGC | `VGC` Super Hi-Res (320/640) + 40-col text from the authentic char ROM → GL display | 🟡 SHR renders (vgc_test green, PNG verified); text renders with authentic char ROM (344s0047); HGR + DHGR colour (NTSC + RGB, dhgr_test); scanline IRQ + 80col next |
| **M4** | ADB + BRAM/RTC | ADB GLU HLE + STATEREG-read fix + //e main/aux redirect | 🟡 ROM 03 boots through all self-tests to the **"Apple IIgs / ROM Version 3" banner**, then reaches disk-boot ($C0Ex, needs M5 IWM). Real kbd/mouse routing + BRAM persistence + ROM 01 banner = follow-ups |
| **M5** | Disk (IWM/SWIM) + **//e legacy** | Reuse POM2 `IWMDevice`+`DiskImage`; add `Swim` for ROM 03. **Plus full Apple //e compatibility**: main/aux memory redirection (RAMRD/RAMWRT/80STORE/PAGE2), LORES/HGR/DHGR video (reuse POM2 `Apple2Display`), so 8-bit //e software runs. | 🟡 IWM 5.25" read path + //e HGR/LORES video done; ROM boots to **"Check startup device!"** (no disk). Real disk boot + SWIM/3.5" + NTSC-colour + full //e mem = follow-ups |
| **M6** | Ensoniq 5503 DOC | `Es5503` — 32 osc, 64 KB sound RAM, Sound GLU ($C03C-$C03F) | 🟢 chip renders a tone (doc_test gate); DOC + 1-bit speaker ($C030) wired to miniaudio (`AudioOut`, cycle-exact speaker). DOC IRQ + native-rate pitch = follow-up |
| **M7** | Serial + slots | `Scc8530` serial | 🟡 SCC loopback (scc_test gate); slot bus / SmartPort / Mockingboard reuse from POM2 = follow-up |
| **M8** | Polish | WASM build (Emscripten) | 🟡 `./build_wasm.sh` → POMIIGS.html/.js/.wasm (emscripten main loop); snapshot/rewind, CLI, packaging = follow-ups |

## Parity dashboard (MAME `apple2gs.cpp` → POMIIGS)

| Hardware | MAME reference | POMIIGS | State |
|---|---|---|---|
| 65C816 CPU | `cpu/g65816/` | `CPU65816` | 🟢 384k Tom Harte vectors green (64 families ×2 modes; MVN/MVP excluded) |
| FPI speed/shadow regs ($C035-$C037) | `apple2gs.cpp` | `IIgsMemory` | 🟡 shadow/speed stored; write-through partial |
| Mega II slow-side + I/O shadow | `apple2gs.cpp` | `IIgsMemory` | 🟡 $E0/$E1 RAM + I/O; display shadow partial |
| STATEREG ($C068) | `apple2gs.cpp` | `IIgsMemory` | 🟢 compose/decompose |
| VGC Super Hi-Res + SCB/palette | `apple2gs.cpp` | `VGC` | 🟢 320/640 render (vgc_test) |
| VGC scanline / VBL interrupt | `apple2gs.cpp` | `IIgsMemory` | 🟡 VBL status + flag; scanline IRQ TODO |
| ES5503 DOC (32 osc) | `sound/es5503.cpp` | `Es5503` | 🟢 renders tone (doc_test); mixed to miniaudio |
| Sound GLU ($C03C-$C03F) | `apple2gs.cpp` | `Es5503` | 🟢 |
| 1-bit speaker ($C030) | `apple2gs.cpp` | `IIgsMemory`+`AudioOut` | 🟢 cycle-exact square wave → miniaudio |
| Audio host (miniaudio) | — | `AudioOut` | 🟢 mono f32 ring, speaker+DOC mix (native; WASM stub) |
| ADB GLU (keyboard/mouse) | `apple2gs.cpp` | `IIgsMemory` | 🟡 HLE handshake (boots); real kbd/mouse TODO |
| Battery RAM + RTC | `apple2gs.cpp` | `IIgsMemory` | 🟡 $C033/$C034 stub |
| IWM (5.25/3.5) | `machine/iwm.cpp` | `Iwm` | 🟢 5.25" read path (boots to Check startup device) |
| SWIM (ROM 03) | `machine/swim.cpp` | `Swim` | 🔴 |
| SCC 8530 serial | `machine/scc8530.cpp` | `Scc8530` | 🟢 loopback (scc_test) |
| Mega II interrupt regs ($C041-$C047) | `apple2gs.cpp` | `IIgsMemory` | 🟡 INTEN/INTFLAG/VBL |

## Reuse-from-POM2 checklist

Shared hardware where POM2's implementation ports/links directly. Verify each
still behaves under the IIgs bus (slow-side timing, 24-bit addresses) before
ticking:

- 🔴 `M6502` — legacy 6502/65C02 not needed (65C816 covers emulation mode) —
  **decision: drop; the 816 in E-mode is the fallback.**
- 🔴 `Memory` IIe paging / language card → fold into `IIgsMemory` slow side
- 🔴 `Apple2Display` (text/LORES/HGR/DHGR) + `NtscPostProcessor` + `CrtEffectStack`
- 🟡 `AudioDevice` / `SpeakerDevice` — folded into `AudioOut` (miniaudio host +
  cycle-exact speaker + DOC mix). `Mockingboard` / `Ssi263` = follow-up.
- 🔴 `IWMDevice` / `DiskImage` / `Block512Backing` / WOZ / 2mg
- 🔴 `SmartPortCard` / `Sony35Drive` / `Disk35Image` / SmartPort hub
- 🔴 `SlotBus` / `SlotPeripheral` (wire-OR IRQ)
- 🔴 `MachineSnapshot` / `RewindBuffer`
- 🔴 `CliDispatcher` / `EmulationController` (fork, retune clock to 2.8/1.02)

## Post-M8 backlog — specialist gap analysis (what makes it a *real* IIgs)

POMIIGS runs the 8-bit / Apple II world well (ROM, ProDOS, Total Replay, colour
HGR, keyboard/joystick). The 16-bit IIgs experience (GS/OS, the desktop, sound)
needs the following, in rough priority order.

**P1 — Sound (the machine's identity)**
- 🟢 Wire the Ensoniq 5503 DOC to real audio output (miniaudio) — `AudioOut`
  mixes `Es5503::render` into the output ring each frame.
- 🟢 1-bit **speaker** ($C030 toggle) → audio — cycle-exact square-wave
  reconstruction from the MMU toggle stamps (Total Replay games audible).
- 🔴 DOC **oscillator IRQ** (sampled-sound / music players).
- 🔴 DOC accuracy: swap-interrupt pops, "never plays the same twice" timing,
  native-rate pitch (currently rendered at the output rate → pitch approximate).
- 🔴 WASM audio: Web Audio backend needs the Emscripten audio-worklet link
  flags; the WASM build ships a silent `AudioOut` stub for now.

**P2 — GS/OS boot (3.5" + desktop)**
- 🔴 **3.5" Sony disk** (IWM 3.5" mode) + **SmartPort** (slot 5) — boot 800K/GS-OS.
- 🔴 **SWIM** (ROM 03 disk chip, MFM superset).
- 🔴 **ADB mouse** (the GS Finder is mouse-driven; only $C000 keyboard exists).

**P3 — VGC completeness (games / demos / apps)**
- 🔴 **Scanline interrupts** (SCB bit6) — split modes (640 menu + 320 gfx).
- 🔴 SHR **colour-fill** mode (SCB bit5); **border colour** ($C034).
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
  frame in **master-clock ticks** (238420 = one Mega II frame): each CPU step
  costs 5 master (fast) or 14 (slow) by the *live* $C036 bit7, so **mid-frame
  speed switches** are honoured. //e slow-mode runs at 1.022 MHz (measured in
  Total Replay). Per-access **slow-side penalty**: Mega II accesses (banks
  $E0/$E1, $Cxxx I/O + LC, shadowed writes) add +9 master (5→14) in fast mode.
  Gates: `speed_test`, `slowside_test`. Minor remaining nuance: Mega II
  fast/slow *phase-sync* sub-cycle alignment (we model the 1 MHz cost only).
- 🟡 IRQ set: **VBL** (tick edge), **¼-second + 1-second** timers (frame-driven,
  60 Hz), **scan-line** (VGCINT enable + SCB bit6), and **DOC** oscillator IRQ
  (IRQ-enabled osc completes → CPU line, cleared by the $E0 osc-int reg) are
  wired through their real registers ($C023/$C032/$C041/$C046/$C047) onto the
  wire-OR CPU lines. Gate: `irq_test`. Remaining: **ADB**, **SCC**, **Mega II
  mouse** IRQs have no active source yet (keyboard is the $C000 latch, SCC is
  loopback, no mouse — see P2); scan-line is IRQ-only (the renderer doesn't
  split a frame mid-screen yet — see P3).

**P5 — Peripherals / infra**
- 🔴 **Battery RAM + RTC** (Control Panel persistence; currently $C033/$C034 stub).
- 🔴 Slot **internal firmware** $C100-$CFFF (returns 0 today).
- 🔴 Real **SCC** serial (modem/printer/AppleTalk) — loopback only.
- 🔴 **Mockingboard**, save-states/debugger, configurable RAM/slots/Control Panel.

## Open questions

- ROM 03 up to 8 MB RAM: model as flat fast-side array or paged expansion card?
- DOC oscillator IRQ cadence vs POM2's `emuCycles` audio bus — confirm the
  cycle-stamp path survives the DOC's own 894.886 kHz sample clock.
- Native-mode vector pull vs POM2 snapshot format — extend `SnapshotIO` for
  the 816's wider register file (16-bit A/X/Y, DBR, PBR, D, 16-bit SP).
