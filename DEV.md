# DEV.md

Implementation deep-dives. Each section: **what the hardware does → how
POMIIGS models it → the MAME `apple2gs.cpp` (or chip driver) citation → the
pinned test**. This file grows one section per subsystem as milestones land
(see `TODO.md`). Sections marked *(planned)* are design notes, not yet code.

## Table of contents

- [CPU — 65C816](#cpu--65c816)
- [Memory — FPI + Mega II](#memory--fpi--mega-ii)
- [Video — VGC + legacy](#video--vgc--legacy)
- [Sound — Ensoniq 5503 DOC](#sound--ensoniq-5503-doc)
- [ADB](#adb)
- [Clock / Battery RAM](#clock--battery-ram)
- [Disk — IWM / SWIM](#disk--iwm--swim)
- [Clock & threading](#clock--threading)

---

## CPU — 65C816

*(planned — Milestone 1)*

The **WDC 65C816** is a 16-bit superset of the 65C02. Design notes for
`CPU65816.h/.cpp`:

- **Two operating modes** gated by the hidden **E (emulation) flag**:
  - `E=1` **emulation mode** — behaves like a 65C02 with an 8-bit stack fixed
    in page 1, 8-bit registers. Reset always enters E=1. This is the fallback
    that replaces POM2's `M6502` (so POMIIGS ships **one** CPU, not two).
  - `E=0` **native mode** — 16-bit A/X/Y selectable per-register via the **M**
    (accumulator/memory width) and **X** (index width) status bits; relocatable
    16-bit **direct page** (D) and 16-bit **stack**; 24-bit program counter via
    the **PBR** (program bank) and data accesses via the **DBR** (data bank).
- **24-bit addressing**: 16 MB as 256 × 64 KB banks. Addressing modes add
  long (24-bit) absolute, `[dp]`/`[dp],Y` (24-bit indirect), stack-relative,
  and block-move (`MVN`/`MVP`). Bank-wrap vs address-wrap semantics differ per
  mode and are a classic bug source — pin them against Tom Harte.
- **Interface** (mirror POM2 `M6502`): constructed with a `Memory*`
  (here `IIgsMemory*`), `run(maxCycles) → actualCycles`, wire-OR IRQ source
  mask, `setNMI()`, `softReset()/hardReset()`. Add `getEmulationMode()` and the
  wide register file to the snapshot.
- **Timing**: base cycle counts follow the WDC datasheet; the *effective* clock
  (2.8 vs 1.02 MHz) and the extra cycle for crossing into slow-side memory are
  applied by `IIgsMemory`, not baked into the opcode table — keep the opcode
  table clock-agnostic (returns architectural cycles), exactly like POM2.

**Gate**: `tomharte_65816` — Tom Harte
[SingleStepTests/65816](https://github.com/SingleStepTests/65816) (both `v1`
E-mode and native vectors), reusing POM2's hand-rolled JSON scanner harness
(`tomharte_cpu_test`). Also Klaus Dormann in E-mode as a fast smoke gate.

**MAME reference**: `cpu/g65816/` (the 816 core) + the wiring in
`apple2gs.cpp`.

---

## Memory — FPI + Mega II

*(planned — Milestone 2)*

The IIgs is two machines bolted together by two custom chips:

- **FPI (Fast Processor Interface)** — the 2.8 MHz "fast side": the 65C816 plus
  fast RAM (banks `$00`–`$7F`) and ROM (`$FC`–`$FF`). It arbitrates every
  access, decides fast vs slow, and applies the **shadow** and **speed**
  registers.
- **Mega II** — an entire Apple //e on a chip: the 1.02 MHz "slow side" (banks
  `$E0`/`$E1`), the classic I/O space, and legacy video generation.

Key registers (all in `$C0xx`, cited to `apple2gs.cpp` when implemented):

- **`$C035` SHADOW** — per-region enables that mirror writes to bank `$00`/`$01`
  down into the slow-side `$E0`/`$E1` (text page 1/2, HGR page 1/2, SHR, aux-HGR,
  I/O+LC, and ROM 03's shadow-all). This is *the* mechanism that keeps the fast
  CPU's writes visible to the slow-side video generator.
- **`$C036` SPEED** — bit 7 selects 2.8 MHz; low nibble = disk-II motor-on
  detect (forces 1 MHz during 5.25" I/O).
- **`$C068` STATEREG** — packs the classic MMU softswitches (ALTZP, PAGE2,
  RAMRD, RAMWRT, RDROM, LCBNK2, INTCXROM) into one byte for GS/OS.
- **`$C037` DMA/shadow-all** (ROM 03).

POMIIGS folds POM2's `Memory` IIe-paging + language-card logic into the
slow-side of `IIgsMemory`; the fast side is a flat banked array with the
shadow write-through applied on store.

---

## Video — VGC + legacy

*(planned — Milestone 3)*

Legacy text/LORES/HGR/DHGR + the NTSC composite and CRT effect stack are
**reused verbatim from POM2** (`Apple2Display`, `NtscPostProcessor`,
`CrtEffectStack`), driven from the slow-side bank `$E0`/`$E1` image.

New: the **VGC (Video Graphics Controller)** Super Hi-Res —
`$E1/2000-9FFF` (32 KB): 200 lines × 160 bytes of pixel data + 256 bytes of
**Scan Control Bytes** (`$9D00`) + 256 bytes of palette RAM (`$9E00`, 16
palettes × 16 colours × 12-bit RGB). Per line the SCB picks 320-mode
(16 colours/line) or 640-mode (4 colours + dither), a palette, and fill/IRQ
bits — up to 256 on-screen colours. VGC also raises the **scanline interrupt**
and **VBL**, load-bearing for beam-raced demos. Reuse POM2's beam-racing
softswitch event log so mid-frame SCB/palette writes land on the right line.

---

## Sound — Ensoniq 5503 DOC

*(planned — Milestone 6)*

The **Ensoniq 5503 DOC** is a 32-oscillator wavetable chip with its own
dedicated **64 KB sound RAM** (not CPU-mapped — reached through the **Sound
GLU** at `$C03C-$C03F`: control, data, and a 16-bit auto-inc address pointer).
Oscillators run in free-run / one-shot / sync / swap modes and generate IRQs.
Model it as a standalone chip (`Es5503`) that renders into POM2's `AudioDevice`
bus, cycle-stamped like every other POM2 audio source. **MAME reference**:
`sound/es5503.cpp`.

---

## ADB

*(planned — Milestone 4)*

The **Apple Desktop Bus** microcontroller manages keyboard + mouse over a
low-speed serial bus. The CPU talks to it via a GLU command/data register
interface (`$C024` mouse data, `$C025` modifiers, `$C026` command/data,
`$C027` status). Model the command protocol + register semantics (HLE), not
the µC firmware — the KEGS/MAME approach. **MAME reference**: ADB GLU in
`apple2gs.cpp`.

---

## Clock / Battery RAM

*(planned — Milestone 4)*

256 bytes of battery-backed **BRAM** (Control Panel settings) + a hardware
**RTC**, reached through the clock/BRAM interface shared with the ADB GLU
(`$C033` data, `$C034` control). BRAM/RTC persist across all resets and to a
host file. **MAME reference**: clock/BRAM state machine in `apple2gs.cpp`.

---

## Disk — IWM / SWIM

*(planned — Milestone 5)*

ROM 01 uses the **IWM** (reuse POM2 `IWMDevice` — same chip, already
cycle-faithful for 5.25"/3.5"). ROM 03 uses the **SWIM** (Super Woz Integrated
Machine): IWM-superset adding MFM; new `Swim` wraps the IWM path and adds the
SWIM mode register. DiskImage/WOZ/2mg/SmartPort all reuse POM2. **MAME
reference**: `machine/iwm.cpp`, `machine/swim.cpp`.

---

## Clock & threading

*(planned — fork POM2 `EmulationController`)*

Master clock **14.31818 MHz**. Fast CPU budget = 2.8 MHz; the worker converts
architectural cycles → wall-clock using the *current* speed register, and adds
the slow-side penalty per access. Single `stateMutex` guards CPU + Memory, as
in POM2. `emuCycles` stamps every CPU→audio/UI event.
