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
- [Serial — SCC 8530](#serial--scc-8530)
- [Clock & threading](#clock--threading)

---

## CPU — 65C816

*(Core complete. All 256 opcodes in `CPU65816.cpp`, gated by the Tom Harte
SingleStepTests/65816 corpus (registers + RAM + cycle count) — see the pinned
`tomharte_65816` test — minus the two deliberate exclusions below. The suite is
re-run as a differential oracle each bug-sweep pass; it caught real defects
static review missed: the WAI/STP cycle count (+3) and the (dp,X) emulation-mode
pointer wrap fixed below (validated 160000/160000 across all 8 (dp,X) opcodes,
both modes).)*

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

**(dp,X) emulation-mode pointer wrap.** When DL=0 (direct page page-aligned)
and the `dp+X` offset wraps to `$FF`, the pointer high byte is read at `ptr+1`
**without** a page-0 wrap (real silicon crosses into the next page), unlike the
DL≠0 branch. This is the opposite of the naive fix and was pinned by the
empirical oracle: 160 000/160 000 across all 8 `(dp,X)` opcodes
(`01/21/41/61/81/A1/C1/E1`, both modes).

**Implementation shape.** `step()` fetches one opcode from `PBR:PC` and
dispatches a switch; `this`-capturing lambdas provide width-aware (8/16-bit)
memory access and the full addressing-mode → 24-bit EA set. Hardware register
invariants are enforced at the top of every `step()`: in emulation mode `SPH`
is hardwired to `$01`, and while the index width is 8-bit the index high bytes
read as 0 (this is what the Tom Harte `*.e` vectors check even for
non-stack/non-index opcodes).

**Cycle model.** `rd`/`wr` each count one bus cycle; a static table adds the
*internal* (non-bus) cycles the datasheet spends — implied/accumulator ops +1,
pushes +1, pulls +2, XBA +2, REP/SEP/BRL/PER/PEI +1. Control-flow internal cycles
(JSR/RTS/RTL/JSL/RTI/BRK/COP) match the datasheet; WAI/STP consume 4
(3 internal + fetch), not 1.

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

*(Complete — `IIgsMemory` boots both **ROM 01** and **ROM 03** through the
self-diagnostic and into GS/OS from an HDD. Implements the 24-bit banked space:
ROM `$FC-$FF` (256 KB) / `$FE-$FF` (128 KB), fast RAM `$00-$7F`, Mega II slow
RAM `$E0/$E1` (128 KB), the `$C0xx` register file, language card, shadow
write-through, VBL/timer and Mega II IRQs. Traces: `boot_trace <rom>`,
`gsos_trace`, `hdd_trace`; gates `slowside_test`, `speed_test`, `irq_test`.)*

**IOLC shadow gates the boot.** `SHAD_IOLC` (`$C035` bit 6) = 0 at reset means
banks `$00/$01` `$C000-$FFFF` behave as the //e machine — `$C0xx` I/O, and the
language card at `$D000-$FFFF` with `!lcRamRead` showing the `$FF`-bank ROM
through. That is why the reset vector at `$00:FFFC` reads the ROM. Cited: MAME
apple2gs.cpp:556-558, :235-241 (shadow bits).

**//e main/aux redirection (`physBank01`).** Bank `$00` **and** bank `$E0`
accesses below `$D000` route through `physBank01(off, writing)`, which consults
ALTZP (ZP/stack), RAMRD/RAMWRT (`$0200-$BFFF`) and 80STORE/PAGE2 (display
pages) to pick main vs aux — the Mega II slow side is a full //e (`$E0` main,
`$E1` aux), and legacy 8-bit / ProDOS-8 / GS-OS code running in `$E0` under aux
switches must hit the right image (KEGS `moremem.c` fixups over `$00` **and**
`$E0`; MAME `auxbank_update`). The ROM runs its stack in aux under ALTZP.

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
- **`$C036` SPEED** — bit 7 (`SPEED_HIGH`) selects 2.8 MHz; bits 3-0 are
  per-slot Disk II motor-detect enables (`SL4/5/6/7`). `speedFast()` =
  `bit7 && !(SPEED_DISKIISL6 && iwm_.motorOn())`: a detect-enabled slot with its
  motor spinning **overrides** bit 7 back to 1 MHz (how the IIgs auto-slows for
  timing-sensitive 5.25" software), mirroring MAME `update_speed()`. Slot 6 =
  on-board IWM.
- **`$C068` STATEREG** — packs the classic MMU softswitches (ALTZP, PAGE2,
  RAMRD, RAMWRT, RDROM, LCBNK2, INTCXROM) into one byte for GS/OS.
- **`$C037` DMA/shadow-all** (ROM 03).
- **`$C031` DISKREG** — on the IIgs this is the disk register (`diskReg_` b7 =
  3.5" drive select, b6 = head select), **not** a speaker mirror. The classic
  Apple II's partial `$C030-$C03F` decode toggled the speaker on `$C031` too;
  on the IIgs only `$C030` toggles the speaker, and mirroring it onto `$C031`
  put a beep's toggles in the same cycle → the "random cracks" bug. Only
  `$C030` toggles; `$C031` is a readable/writable DISKREG.
- **Extended BRAM** (256-byte battery RAM) two-byte address decodes as
  `(cmd&7)<<5 | (data>>2)&0x1F` (3+5 bits), not 2+6 — corroborated by KEGS
  `clock.c`, GSSquared `RTC.hpp`, MAME `macrtc`.

POMIIGS folds POM2's `Memory` IIe-paging + language-card logic into the
slow-side of `IIgsMemory`; the fast side is a flat banked array with the
shadow write-through applied on store.

---

## Video — VGC + legacy

*(Complete — `VGC` renders `IIgsMemory`'s slow-side video RAM to a 640×400 RGBA
framebuffer (GL texture in the app). All modes dispatched in `render()`:
`renderSHR`, `renderText`/`renderText80`, `renderHGR`, `renderDHGR`,
`renderLores`, and `renderTextBand` for the mixed-mode text window. NTSC
composite decode in `VGCNtsc.h`. Gates: `vgc_test`, `shr_test`, `dhgr_test`,
`dhgr_page_test`, `text80_test`; `screenshot` tool for eyeballing.)*

**Super Hi-Res** (`renderSHR`). Reads `$E1:2000-9CFF` (200 × 160 bytes), the
per-line **SCB** at `$E1:9D00` (bit 7 = 640 mode, bits 0-3 = palette), and the
16 × 16 × 2-byte palettes at `$E1:9E00` (4-4-4 `$0RGB`). 320 mode = 2 × 4-bit
indices/byte; 640 mode = 4 × 2-bit with the column-offset palette groups
`{8,12,0,4}`. Lines doubled vertically to 400.

**Text.** 40-column from `$E0:0400` (//e interleaved) using the **authentic
Apple IIgs Mega II character ROM** (`roms/iigs-char.rom` = `344s0047.bin`,
16 KB — user-provided like the main ROM; **no public font is bundled**). Text
is skipped until the char ROM is present.

**Legacy //e graphics.** HGR (280×192), DHGR (140×192, 16 colour) and LORES
(40×48) all render, dispatched by the //e mode switches (`$C050-$C05F`); NTSC
composite artifact decode lives in `VGCNtsc.h`. **DHGR chroma phase gotcha:**
the composite decoder is shared with HGR and must be told which chroma phase to
use — HGR = bias 0, DHGR = bias 1 (80-column → `absX + 1`, MAME
`apple2video.cpp render_line_artifact_color`, POM2 `Apple2Display.cpp:2084`).
Getting it wrong rotates every DHGR hue 90° (blue↔orange, green↔magenta).

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

*(Complete — `src/Es5503` drives real GS/OS audio (validated against synthLAB
playback). Reset also resets the DOC + SCC so no stale oscillator or IRQ
survives a machine reset. Output is AC-coupled (one-pole DC blocker) and
saturating-clamped to [-1,1] before the float32 backend. Gate: `doc_test`.)*

The **Ensoniq 5503 DOC** is a 32-oscillator wavetable chip with its own
dedicated **64 KB sound RAM** (not CPU-mapped — reached through the **Sound
GLU** at `$C03C-$C03F`: control, data, and a 16-bit auto-inc address pointer).
Oscillators run in free-run / one-shot / sync / swap modes and generate IRQs.
Model it as a standalone chip (`Es5503`) that renders into POM2's `AudioDevice`
bus, cycle-stamped like every other POM2 audio source. **MAME reference**:
`sound/es5503.cpp`.

---

## ADB

*(Complete — register-level HLE. Keyboard, command-key menu shortcuts, and the
mouse all reach the GS/OS Finder; in-game µC-driven mice work (Battle Chess /
Captain Blood). Gate: `adb_test`.)*

The **Apple Desktop Bus** GLU (`$C024` mouse, `$C025` modifiers, `$C026`
command/data, `$C027` status) is modelled at the register level (HLE), not the
µC firmware — the KEGS approach. The ROM's ADB self-test writes command bytes
to `$C026`, waits for CMDFULL (`$C027` bit 0) to clear, then waits for
data-ready (`$C027` bit 5) and reads the response; with no ADB it times out and
raises **fatal error `$0911`** (`PEA $0911 / JSR $A6E4` at `$FF:81B6`). We
accept commands immediately (CMDFULL always clear) and queue a trivial response
(data-ready set), so the handshake completes and `$0911` clears.

**Real keyboard/mouse routing** (gate `adb_test`):
- **Keyboard** stays on the classic `$C000` latch / `$C010` strobe (the Mega II
  posts ADB keys there); **`$C025` KEYMODREG** now carries live host modifiers
  (b7 ⌘/command, b6 option, b2 caps, b1 control, b0 shift).
- **Mouse** via the GLU mouse register: **`$C024` MOUSEDATA** returns the X delta
  then the Y delta (toggled by **`$C027` bit 1**), each with the button in bit 7
  (0 = down) and a signed 7-bit delta; the Y read consumes the deltas and clears
  **`$C027` bit 7** (data-available). Host motion accumulates via `mouseMove`/
  `mouseButton` (wired from ImGui in `main.cpp`).
**Mouse interrupt** (what GS/OS actually uses — it reads `$C024` *zero* times
without it). The ROM interrupt manager, reached on every serviced IRQ, does at
`$FF:BE31`: `LDA $C027` → if b7 (mouse-data) **and** b6 (mouse-int) are set,
`JSL $E10034` → **ReadMouse (`$FE:B1E1`)**, which reads `$C024` (X, then Y if
`$C027` b1 set). So `mouseMove`/`mouseButton` set b7 (data) + b6 (int) and raise
`IRQ_SRC_ADB`; the `$C024` Y-read consumes the sample and drops the IRQ.

**Storm-safety.** A naive ADB IRQ wedges the boot: during early emulation-mode
boot the ADB mouse handler isn't installed, so an unclearable IRQ storms the ROM
Interrupt Manager. `updateAdbIrq` therefore only delivers when **native mode +
VBL interrupts enabled** (`$C041` b3) — a proxy for "the interrupt system is up"
— and `tick()` drops any sample unconsumed after ~2 frames. With this, continuous
mouse motion *during* boot still reaches the desktop, and post-boot the ROM
services the mouse (verified: `$C024` read at `$FE:B1EB`/`B1F8`). Gate:
`adb_test`.

**Keyboard interrupt** (typing reaches the GS/OS Finder — selects desktop icons).
GS/OS never polls `$C000` (5 reads in a whole boot, all early), so a key must
raise an interrupt. `keyEvent` latches the ASCII at `$C000` and raises
`IRQ_SRC_ADB`; the ROM interrupt manager reads a `$C026` *routing byte*
(`kbdIntStatus_ = $40`, b6) gated by `$C027` b5, and dispatches the keyboard
handler **`$FE:EC99`**, which reads the ASCII from `$C000` + modifiers from
`$C025`, clears the `$C010` strobe, and calls **EventMgr PostEvent (`$1406`)** —
the Finder's GetNextEvent then retrieves it. The `$C010` strobe-clear (read or
write) consumes the event and drops the IRQ; same native+VBL storm gate as the
mouse. Gate: `adb_test`.

**Command-key menu shortcuts fire** (⌘-A flashes the Edit menu). The keyboard
handler's modifier builder `$FE:EC46` maps each `$C025` KEYMODREG bit to a GS
event-modifier flag via the ROM table `$FEE267`: bit7→appleKey (`$0100`),
bit6→optionKey, bit1→controlKey, bit0→shiftKey, bit2→capsLock, bit4→keypad. So a
key posted with `$C025` bit7 set carries the command modifier and TaskMaster
routes it to MenuKey. The emulator side needed no change; the missing piece was
in `main.cpp` — Command/Option/Control suppress ImGui's `InputQueueCharacters`,
so the combo's letter is now delivered via `IsKeyPressed` while a shortcut
modifier is held (the `$C025` bits are set from LeftAlt=⌘/RightAlt=option each
frame). A hardware-accurate mouse-int *enable* (replacing the native+VBL proxy)
still needs the full ADB µC command model — the `Adb.cpp` subsystem.

**Two other fixes were load-bearing for the banner boot** (both surfaced by the
boot trace):
- **STATEREG (`$C068`) read must synthesize** from the live switches
  (ALTZP/PAGE2/RAMRD/RAMWRT/`!lcRamRead`/LCBNK2/INTCXROM), not return the last
  written byte — the ROM saves/restores the MMU state through it, so a stale
  read corrupts the language-card state on restore and the ROM jumps into empty
  LC RAM. Cited: MAME apple2gs.cpp:1926.
- **//e main/aux redirection** (`physBank01`): bank `$00` accesses redirect to
  the aux bank (`$01`) per ALTZP (ZP/stack), RAMRD/RAMWRT ($0200-$BFFF), and
  80STORE/PAGE2 (display pages) — the ROM runs its stack in aux under ALTZP.

**MAME reference**: ADB GLU + STATEREG in `apple2gs.cpp`.

---

## Clock / Battery RAM

*(Implemented — the clock/BRAM state machine handles `$C033` data / `$C034`
control and the 256-byte extended BRAM. POMIIGS passes the host clock through
for the RTC time-of-day (always shows correct real time); Control-Panel
time-set writes are dropped by design.)*

256 bytes of battery-backed **BRAM** (Control Panel settings), reached through
the clock/BRAM interface shared with the ADB GLU. **Extended-BRAM address
decode:** `(cmd&7)<<5 | (data>>2)&0x1F` (3+5 bits) — see the Memory section.
BRAM persists across all resets and to a host file. **MAME reference**:
clock/BRAM state machine in `apple2gs.cpp` / `macrtc`.

---

## Disk — IWM / SWIM

*(GS/OS boots from a hard disk. `src/Iwm` nibblises a 143 360-B .dsk/.do/.po
(6-and-2 GCR, DOS 3.3 / ProDOS interleave) into per-track streams timed by CPU
cycles at `$C0E0-$C0EF` (slot 6). The 3.5" / block path is a SmartPort HLE
(`src/ProDosHdd`, `smartportTrap`): the slot-5 ROM's `$Cn50` ProDOS-block and
`$Cn53` SmartPort dispatch entries are WDM traps handled in C++. Gates:
`disk35_test`, `smartport_test`, `hdd_test`; trace `hdd_trace`.)*

ROM 01 uses the **IWM** (reuse POM2 `IWMDevice` — same chip, already
cycle-faithful for 5.25"/3.5"). ROM 03 uses the **SWIM** (Super Woz Integrated
Machine): IWM-superset adding MFM; new `Swim` wraps the IWM path and adds the
SWIM mode register. **MAME reference**: `machine/iwm.cpp`, `machine/swim.cpp`.

**SmartPort dispatch gotcha.** The `$Cn53` entry serves both the classic
(`DFB cmd` / `DW paramList`, 2-byte bank-0 pointer) and the GS/OS **extended**
(`DFB cmd` / `DC I4'paramList'`, 4-byte bank-qualified 24-bit pointer) forms —
reading the extended pointer as a 2-byte bank-0 pointer fetches the wrong
param list. The extended STATUS for an **offline** block device returns `$80`
(is-block, bit 7) not `$00`. The trap PRESERVES the carry on return (GS/OS's
emulation-mode SmartPort trampolines stash it). Diagnosed with `hdd_trace`.

---

## Serial — SCC 8530

*(Milestone 7 — `src/Scc8530`, gate `scc_test`.)* The two-port Zilog 8530 at
`$C038-$C03B` (B/A command + data). The register-pointer protocol (write WR0
low nibble → next access hits that register, auto-resets to 0), TX/RX FIFOs,
RR0 status (Rx-available / Tx-empty), and WR14-bit4 local loopback are
modelled. Host hooks `hostRx`/`hostTx` bridge to a real port later. Slot
bus / SmartPort / Mockingboard reuse from POM2 is the remaining M7 work.
Source of truth: MAME `machine/z80scc.cpp`.

## Clock & threading

The emulation worker + ImGui UI live in `src/Ui.cpp` / `src/main.cpp` (no
separate `EmulationController` class was forked). Master clock
**14.31818 MHz**; fast CPU budget = 2.8 MHz. `IIgsMemory` converts
architectural cycles → master ticks using the *current* speed register
(fast = ×5, slow = ×14) and adds the slow-side penalty per access
(`chargeSlow`). `emuCycles` stamps every CPU→audio/UI event.
