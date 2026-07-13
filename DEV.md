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

*(Milestone 1 — core complete. All 256 opcodes implemented in `CPU65816.cpp`.
Validated against Tom Harte SingleStepTests/65816 at **100% (registers + RAM +
cycle count)** across 64 opcode families spanning every addressing mode, in
both emulation and native mode — 384 000 vectors. The remaining M1 work is
extending the downloaded corpus to the full 256 opcodes and the ROM
self-diagnostic once the MMU (M2) exists.)*

**Known gate exclusion — MVN/MVP (`$54`/`$44`).** Tom Harte caps block-move
vectors at 100 cycles, i.e. it captures a *partial* execution (14 iterations)
of what is a multi-iteration instruction. POMIIGS is instruction-stepped —
`step()` moves one byte and re-points PC at the opcode until the count wraps,
which is functionally correct when driven by the CPU loop but does not match
Tom Harte's cycle-capped snapshot. The two are excluded from the gate
(`--skip 44,54`); their per-byte semantics are otherwise correct.

**Emulation-mode stack quirk.** The *new* 65816 push/pull instructions
(PEA/PEI/PER/PHD/PLD/JSL/RTL) use a full 16-bit stack pointer even in emulation
mode — SP may leave page 1 mid-instruction — with SPH reset to `$01` at the end
of `step()`. The *old* 6502 stack ops (PH*/PL*/JSR/RTS/RTI/BRK) keep the
page-1 wrap. PEI additionally reads its DP pointer as full 16-bit (no page-0
wrap). All three were surfaced by the vectors.

**Implementation shape.** `step()` fetches one opcode from `PBR:PC` and
dispatches a switch; `this`-capturing lambdas provide width-aware (8/16-bit)
memory access and the full addressing-mode → 24-bit EA set. Hardware register
invariants are enforced at the top of every `step()`: in emulation mode `SPH`
is hardwired to `$01`, and while the index width is 8-bit the index high bytes
read as 0 (this is what the Tom Harte `*.e` vectors check even for
non-stack/non-index opcodes).

**Cycle model.** `rd`/`wr` each count one bus cycle; a static table adds the
*internal* (non-bus) cycles the datasheet spends — implied/accumulator ops +1,
pushes +1, pulls +2, XBA +2, REP/SEP/BRL/PER/PEI +1. Control-flow internal
cycles (JSR/RTS/RTL/JSL/RTI/BRK/COP) are **not yet exact** — tracked WIP.

**Test.** `tomharte_65816 <dir>` (harness in `tests/`, fetch via
`tests/fetch_tomharte_65816.sh`). Each vector's `e` field selects the mode; P
is compared with the phantom bits (`0x30`) masked only in emulation mode
(native M/X are real flags). `--no-cycles` isolates state from timing;
`--only hh` / `--max N` scope a run.

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

*(Milestone 2 — MMU boots a real ROM. `IIgsMemory` implements the 24-bit banked
space: ROM at `$FC-$FF` (256 KB) / `$FE-$FF` (128 KB), fast RAM `$00-$7F`,
Mega II slow RAM `$E0/$E1` (128 KB), the `$C0xx` register file, the language
card, and shadow write-through. Both a real **ROM 01** and **ROM 03** reset
from the ROM vector, switch to native mode, and execute ~340-420 distinct ROM
addresses of self-diagnostic before parking in the CPU speed-calibration loop
at `$FF:FCDC` (`SBC #1 / BNE`, writing `$C036`) — which needs a VBL/timer
reference to converge, i.e. M3+ hardware. Verify with `boot_trace <rom>`.)*

**IOLC shadow gates the boot.** `SHAD_IOLC` (`$C035` bit 6) = 0 at reset means
banks `$00/$01` `$C000-$FFFF` behave as the //e machine — `$C0xx` I/O, and the
language card at `$D000-$FFFF` with `!lcRamRead` showing the `$FF`-bank ROM
through. That is why the reset vector at `$00:FFFC` reads the ROM. Cited: MAME
apple2gs.cpp:556-558, :235-241 (shadow bits).

**Still staged in:** //e main/aux (bank `$00`↔`$01`) redirection under
RAMRD/RAMWRT/80STORE/PAGE2; the second LC `$D000` bank uses the `$Cxxx` RAM
window (approximate); `$C100-$CFFF` slot/internal ROM; VBL/timer status. These
land as the boot trace demands them (next: VBL for the speed loop).

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

*(Milestone 3 — Super Hi-Res renders; 40-col text renders from the authentic
char ROM. `VGC` renders `IIgsMemory`'s slow-side video RAM to a 640×400 RGBA
framebuffer, displayed in the app via a GL texture. Verified by `vgc_test`
(the M3 gate: a 320/640 SHR colour-bar pattern) and the `screenshot` tool.)*

**Super Hi-Res** (`renderSHR`). Reads `$E1:2000-9CFF` (200 × 160 bytes), the
per-line **SCB** at `$E1:9D00` (bit 7 = 640 mode, bits 0-3 = palette), and the
16 × 16 × 2-byte palettes at `$E1:9E00` (4-4-4 `$0RGB`). 320 mode = 2 × 4-bit
indices/byte; 640 mode = 4 × 2-bit with the column-offset palette groups
`{8,12,0,4}`. Lines doubled vertically to 400.

**Text.** 40-column from `$E0:0400` (//e interleaved) using the **authentic
Apple IIgs Mega II character ROM** (`roms/iigs-char.rom` = `344s0047.bin`,
16 KB — user-provided like the main ROM; **no public font is bundled**). Text
is skipped until the char ROM is present. 80-col, LORES/HGR/DHGR, and the NTSC
composite / CRT stack (reuse POM2 `Apple2Display`, `NtscPostProcessor`,
`CrtEffectStack`) are staged in next.

*Original plan:* legacy text/LORES/HGR/DHGR + the NTSC composite and CRT effect
stack are **reused verbatim from POM2** (`Apple2Display`, `NtscPostProcessor`,
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
