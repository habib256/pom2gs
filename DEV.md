# DEV.md

Implementation deep-dives. Each section: **what the hardware does → how
POMIIGS models it → the MAME `apple2gs.cpp` (or chip driver) citation → the
pinned test**. This file grows one section per subsystem as milestones land
(see `TODO.md`). Each section opens with a parenthesised status line saying how
far that subsystem actually is and which gate pins it.

## Table of contents

- [CPU — 65C816](#cpu--65c816)
- [Memory — FPI + Mega II](#memory--fpi--mega-ii)
- [Video — VGC + legacy](#video--vgc--legacy)
- [Sound — Ensoniq 5503 DOC](#sound--ensoniq-5503-doc)
- [ADB](#adb)
- [Clock / Battery RAM](#clock--battery-ram)
- [Disk — IWM](#disk--iwm)
- [Serial — SCC 8530](#serial--scc-8530)
- [Clock & threading](#clock--threading)
- [Dev environment variables](#dev-environment-variables)

---

## CPU — 65C816

*(Core complete. All 256 opcodes in `CPU65816.cpp`, gated by the Tom Harte
SingleStepTests/65816 corpus (registers + RAM + cycle count, with **every**
bus cycle — active transactions and internal cycles alike — checked in order,
pin-by-pin) — see the pinned `tomharte_65816` test — minus the two deliberate
exclusions below. The suite is
re-run as a differential oracle each bug-sweep pass; it caught real defects
static review missed: the WAI/STP cycle count (+3) and the (dp,X) emulation-mode
pointer wrap fixed below (validated 160000/160000 across all 8 (dp,X) opcodes,
both modes).)*

**MVN/MVP (`$54`/`$44`) under the gate.** Tom Harte caps block-move vectors
at 100 cycles: 14 complete 7-cycle iterations plus the opcode and
destination-bank fetches of the 15th, leaving PC advanced by two and the
registers untouched. POMIIGS is instruction-stepped — `step()` moves one byte
and re-points PC at the opcode until the count wraps — so the harness runs
whole iterations up to the cap and compares against the vector with those two
trailing fetch cycles removed (`runVector`, `blockMove`). No opcode is
excluded any more: 512 files, 5.12 M vectors, every cycle compared.

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

**Cycle model.** Every cycle of an instruction goes through exactly one
helper: `rd`/`wr`/`fetch`/`rdVector` for the active bus transactions and
`io`/`ioAt`/`RMW` for the *internal* (VDA=VPA=VPB low) cycles, each placed at
its position in the WDC Table 5-7 microsequence rather than summed from a
table. The opcode-adjacent ones come from a small `leadIo[]` table (implied/
accumulator ops, XCE, NOP, pushes = 1; pulls, XBA, RTS/RTL/RTI = 2; WAI/STP =
3, address bus = PC+1); the rest sit inside the addressing-mode helpers or the
opcode body — the DL≠0 and index-add cycles of direct page, the indexed
page-cross cycle (address bus = the *uncarried* sum, "AAH,AAL+XL"), the
stack-relative cycles, the read-modify-write modify cycle (in emulation mode
the 6502-style dummy write of the unmodified byte, VDA low; MLB asserted from
first read to last write, write-back **high byte first**), and the control-flow
cycles of JSR/JSL (PBR is pushed between AAH and AAB)/JSR (abs,X) (the return
address is pushed between AAL and AAH)/JMP(abs,X)/RTS/REP/SEP/BRL/PER/MVN/MVP.
WDM's signature byte is an internal cycle (peeked for the SmartPort trap via
`IIgsMemory::peek8`); RTI applies the pulled P only after its last pull, so the
PC/PBR pulls still show the old M/X on the pins; WAI/STP end in a park cycle
with the address bus, RWB and E/M/X released (`BUS_HALT`). Each
`io` also calls `IIgsMemory::internalCycles()`, which is what lets the master-
clock scheduler phase the *following* transaction correctly (see § Timing).
Direct-page and stack-relative 16-bit data wrap inside bank 0 (`$00FFFF+1 =
$000000`); absolute/long/indexed data spill into the next bank.

**Hardware over corpus — two rulings (September 2026).** `(dp,X)` in emulation
mode reads the pointer's HIGH byte within the page of the low byte's address:
with DL=0 both bytes stay in the direct page (Bruce Clark, *Investigating the
65C816's Operation* §5.11), with DL≠0 the low byte is at the full 16-bit
D+LL+X and only the +1 wraps — a silicon quirk verified by gilyon's hardware
test 0027 and modelled by bsnes (`readDirectX`). `JSR (abs,X)` is a new 65816
instruction and pushes through the raw 16-bit stack in emulation mode (no
page-1 wrap — 6502.org opcodes appendix, bsnes, gilyon 0277). The
SingleStepTests corpus disagrees on both (issues #3 and #6 upstream); the
harness carries exactly those vectors — `(dp,X)` with the pointer at `$xxFF`,
`$FC` with S=`$xx00` — as issue-linked xfails (`knownCorpusError`, 308 vectors)
and fails on anything else.

**Functional cross-checks** (catalog `cpu-functional-crosschecks`):
`klaus_test` runs Klaus Dormann's assembled 6502 functional test on the flat
bus in emulation mode until it parks in a `jmp *` trap (success = `$3469` in
the pinned listing; 30.6 M instructions); `gilyon_test` runs the gilyon
`snes-tests/cputest` generator — 1610 native/emulation tests of every opcode
and addressing mode with wrapping edge cases — rehosted on the flat bus by
`tests/cycle_accuracy/gilyon_flatbus.asm` (the SNES PPU driver replaced by a
status byte + `STP`; `tools/build_gilyon.sh` assembles it with cc65 against
the pinned generator output, LoROM layout kept); `bruce_decimal` runs Bruce
Clark's decimal-mode test — 2 × 256 × 256 ADC/SBC pairs predicted with binary
arithmetic — ported from as65 to ca65 (`tests/cycle_accuracy/bruce_decimal.s`,
cputype 65C816, A and N/V/Z/C all checked, `tools/build_decimal.sh`) until
its `STP` with `ERROR = 0`; `klaus_interrupt` runs Klaus's IRQ/NMI/BRK test
— the as65 source translated to ca65 by `tools/as65_to_ca65.py`
(`tools/build_interrupt.sh`, D cleared on interrupt for the 65C816) with the
harness driving its `$BFFC` feedback register after every instruction (bit 0
= IRQ level, bit 1 = NMI on the rising edge) until the success trap; its
WAI/STP sections are manual single-step tests and stay out of scope. All four
SKIP when their inputs are not fetched/built.

**Test.** `tomharte_65816 <dir>` (harness in `tests/`, fetch via
`tests/fetch_tomharte_65816.sh`). Each vector's `e` field selects the mode; P
is compared with the phantom bits (`0x30`) masked only in emulation mode
(native M/X are real flags). `--no-cycles` isolates state from timing;
`--no-bus` isolates state/timing from bus tracing, `--active-only` drops the
internal cycles from the comparison (triage: is it a transaction or a
placement defect?); `--only hh` / `--max N` scope a run.

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
  (2.8 vs 1.02 MHz), the PH0 side-sync wait and the DRAM-refresh stall are
  applied by `IIgsMemory` per bus cycle as the CPU emits them, not baked into
  the opcode bodies — keep the CPU clock-agnostic (it reports architectural
  cycles and their order), exactly like POM2.

**Gate**: `tomharte_65816` — Tom Harte
[SingleStepTests/65816](https://github.com/SingleStepTests/65816) (both `v1`
E-mode and native vectors), reusing POM2's hand-rolled JSON scanner harness
(`tomharte_cpu_test`). `--examples N` / `--verbose` dump failing vectors.
(POM2 also runs Klaus Dormann's 6502 suite as a fast smoke gate; POMIIGS has
no such harness — the Tom Harte corpus is the only CPU gate here.)

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
  I/O+LC). This is *the* mechanism that keeps the fast CPU's writes visible to
  the slow-side video generator. Text page 2 shadowing exists on ROM 03 only.
- **`$C036` bit 4 — shadow all banks** (CYAREG): every fast bank behaves like
  bank `$00` (even) / `$01` (odd) — //e main/aux redirect (RAMWRT sends a bank
  `$02` write to `$03`), I/O + language card in `$C000-$FFFF`, and video
  shadowing to `$E0`/`$E1` (`bank01Like()`; MiSTer `mmu_test` 03/07/09/0C,
  pinned by `fpi_shadowall_test`).
- **`$C029` bit 0 — bank latch**: while clear, bank `$E1` RAM accesses land in
  `$E0` (`mmu_test` 13). Apple says the bit must always be set; the reset
  state is 1 (MAME) and software must write `$81`, not `$80`, to turn SHR on.
- **Floating bus**: unmapped (`$80-$DF`, `$E2-$FB`) and unpopulated fast-RAM
  reads return the last byte that crossed the data bus (`busLast_`), so
  `LDA >$810000` reads `$81` — the operand's bank byte (`mmu_test` 25).
- **Language card physical layout** (Hardware Reference, Sather; gssquared,
  Clemens; `mmu_test` 26): bank 2 is the primary block at physical `$D000`,
  bank 1 folds into the `$C000-$CFFF` window — the layout that shows through as
  linear RAM when the IOLC shadow is inhibited (`$C035` bit 6).
- **`$C036` SPEED** — bit 7 (`SPEED_HIGH`) selects 2.8 MHz; bits 3-0 are
  per-slot Disk II motor-detect enables (`SL4/5/6/7`). `speedFast()` =
  `bit7 && !(SPEED_DISKIISL6 && iwm_.motorOn())`: a detect-enabled slot with its
  motor spinning **overrides** bit 7 back to 1 MHz (how the IIgs auto-slows for
  timing-sensitive 5.25" software), mirroring MAME `update_speed()`. Slot 6 =
  on-board IWM.
- **`$C068` STATEREG** — packs the classic MMU softswitches (ALTZP, PAGE2,
  RAMRD, RAMWRT, RDROM, LCBNK2, INTCXROM) into one byte for GS/OS.
- **`$C037` DMAREG** (DMA bank) — latched and read back; DMA is not modelled.
- **`$C031` DISKREG** — on the IIgs this is the disk register (`diskReg_` b6 =
  35SEL, the 3.5" drive select; b7 = HDSEL, the Sony SEL / head select), **not**
  a speaker mirror. The classic Apple II's partial `$C030-$C03F` decode toggled
  the speaker on `$C031` too;
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

*(Complete — `VGC` renders to a 640×400 RGBA framebuffer (GL texture in the
app) **from a live scanout capture**, not from final memory: `IIgsMemory::tick()`
runs the video scanner cycle by cycle, and `VGC::render()` draws each of the
200 captured scanlines with `drawTextLine` (40/80 col), `drawLoresLine`,
`drawHgrLine`, `drawDhgrLine` or `drawShrLine`. NTSC composite decode in
`VGCNtsc.h`. Gates: `scanout_test`, `vgc_test`, `shr_test`, `dhgr_test`,
`dhgr_page_test`, `text80_test`; `screenshot` tool for eyeballing.)*

**Live scanout** (`IIgsMemory::ScanLine`, `scanCycle`/`scanAdvance`). For
every Mega II cycle the beam crosses (65 per 912-tick line, 262 lines), the
scanner does what the VGC does at that cycle: cycle 0 (start of HBL) reads
the line's SCB; cycle 24 (end of HBL) latches the mode switches, `$C022`
colours, ALTCHAR/LANGSEL and the 32-byte palette the SCB selects; cycles 25-64
(HORIZCNT `$58-$7F`) fetch display slot n = h-25 — main + aux byte of the
text/lo-res or hi-res row in the legacy modes, four SHR bytes per cycle. A
write that lands after the beam passed shows next frame; a palette or mode
change during HBL takes effect on that line (per-line splits, 3200-colour
pictures); a CPU rewriting a text row while it is scanned crosses the scanner
byte by byte (TextFunk). `videoBusByte()` is the last byte the scanner fetched
— the Mega II floating bus — and `ioRead` serves it for display soft-switch
reads (`$C050-$C05F`), the speaker toggle and every unassigned `$C0xx` read
(Sather 5-40; the vapor-lock beam sync reads `$C050`/`$C051` for it). Slot
`$C0n0` I/O that a card model decodes (IWM, SmartPort) keeps its own value.
Callers that render without running the clock (unit tests, tools) get a
full-frame `scanSnapshot()`; the render loop reads the capture on the UI
thread without locking, the same benign race the direct-memory renderer had.
Cost: ~25 s of emulated time per host second on Apple Silicon.

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
and **VBL**, load-bearing for beam-raced demos.

**Scanline interrupt (done, `irq_test`).** The tick() beam walk fires for
every display line the beam enters: an SHR SCB with bit 6 latches $C023 bit 5
at that line (status even when disabled — MAME apple2gs.cpp apple2_vgc
~1090-1125) and asserts the VGC IRQ when $C023 bit 1 is set; $C032 writes and
**$C02E/$C02F reads** acknowledge it (clear_vgcint(~SCANLINE), MAME
:1674-1683). One IRQ per flagged line per frame. The live scanout latches
each line's palette at the end of its HBL, so a handler that rewrites the
palette from the scanline IRQ (3200-colour pictures, 640/320 menu splits)
shows correctly.

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
(data-ready set), so the handshake completes and `$0911` clears. The "read µC
memory" command (`$09`, address lo/hi) answers from the ADB microcontroller's
firmware when one is loaded (`IIgsMemory::loadAdbMicroRom`, 4 KB mapped at µC
`$1000-$1FFF`; 341-0632 for ROM 03, 341-0345 for ROM 01) and `$00` otherwise —
the ROM self-test 09 checksums that range, see § ROM self-test.

**Response timing and bus commands** (September 2026, from the MiSTer
`customtests` + `rtl/adb.v` and KEGS `adb.c`): a µC response is posted
`kAdbResponseTicks` (≈200 µs) after the command; only then do DATAREG-full
(`$C027` b5) and its interrupt assert. An instantaneous response let the ROM's
interrupt manager (`$FF:BE34`/`BE6A`) steal the byte in the `PLP…SEI` gap
between the ROM's send (`$FF:6F9A`) and receive (`$FF:6FBF`) helpers whenever
ProDOS 8 had enabled the DATAREG interrupt — ROM test 0A's version read timed
out under the ProDOS launchers while passing under `selftest_trace`. ADB-bus
commands (`$40-$FF`, high nibble = operation, low nibble = device; `$7x`
disable SRQ, `$8x-$Bx` Listen, `$Cx-$Fx` Talk) complete with a status byte
(bit 7 set, bits 2-0 = data count-1) readable at `$C026`; a data-less
completion (flush, reset, absent device, Listen) does **not** raise b5 or an
interrupt — the ROM's boot-time `$73`/`$B3` never read a reply, and a queued
`$80` there wedged the IRQ manager. Talk reg 3 of the keyboard (2) or mouse
(3) answers `$81` + `{address, handler $06}` like KEGS/Clemens.

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
still needs the full ADB µC command model (today the GLU lives in
`IIgsMemory`; splitting it into its own `Adb` subsystem is the planned
follow-up).

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
control and the 256-byte extended BRAM. The RTC seconds counter is the host
clock plus a guest-settable offset: Control-Panel time-set writes and the ROM
self-test's clock test (07, a walking-bit write through `$E1/0088` read back
via `$E1/008C`) both persist, while time keeps advancing.)*

**Host persistence** (`loadBram`/`saveBram`, gate `bram_test`): the 256 BRAM
bytes and the clock offset live in `states/bram.bin` ("PGBR", version 1, 256
bytes, int64 offset). `main.cpp` loads it before the first boot and saves it on
exit and about five seconds after any change (`bramDirty()`), so Control
Panel settings — startup slot, display, keyboard, clock — survive a restart.
No file, or a malformed one, behaves like a dead battery: the ROM's own BRAM
validation reinitialises the defaults.

256 bytes of battery-backed **BRAM** (Control Panel settings), reached through
the clock/BRAM interface shared with the ADB GLU. **Extended-BRAM address
decode:** `(cmd&7)<<5 | (data>>2)&0x1F` (3+5 bits) — see the Memory section.
BRAM persists across all resets (`reset()` leaves `bram_` alone) and across
runs through `states/bram.bin`; the ROM seeds the Control Panel defaults when
the checksum is invalid (first run). **MAME reference**:
clock/BRAM state machine in `apple2gs.cpp` / `macrtc`.

---

## Disk — IWM

*(GS/OS boots from a hard disk. `src/Iwm` drives the `$C0E0-$C0EF` port (slot 6)
against POM2's ported `src/DiskImage` bit-cell model — .dsk/.do/.po/.nib/.d13/
.2mg and WOZ 1/2+FLUX, read **and** write. The 3.5" / block path has two
implementations: the default SmartPort HLE (`src/ProDosHdd`, `smartportTrap` —
the slot-5 ROM's `$Cn50` ProDOS-block and `$Cn53` SmartPort dispatch entries are
WDM traps handled in C++), and the low-level Sony drive (`src/Sony35`,
`iwm35 = 1`). Gates: `disk35_test`, `smartport_test`, `hdd_test`, `iwm525_test`,
`iwm35_test`, `twoimg_test`; trace `hdd_trace`.)*

**Every production IIgs — ROM 01 and ROM 03 — uses the IWM.** The SWIM
(Super Woz Integrated Machine, IWM-superset adding MFM/1.44M) only ever
appeared on the unreleased 1991 **"Mark Twain" prototype** (SWIM1 344S0061,
15.6672 MHz — MAME `apple2gs.cpp:15, 3891-3896`, machine `apple2gsmt`,
`MACHINE_NOT_WORKING`, its own 512K ROM). Earlier docs here claimed "ROM 03
uses the SWIM" — a common misconception, now corrected; SWIM is out of scope.
**MAME reference**: `machine/iwm.cpp` (+ `swim1.cpp` for the prototype only).

### Real IWM 3.5" — Sony drive LLE (`src/Sony35`, `iwm35 = 1`)

The SmartPort HLE above stays the default, but `iwm35 = 1` in `pomiigs.cfg`
(or `--iwm35`) mounts 800K media on a **low-level Sony 3.5" drive model**
instead: slot 5 then serves the **genuine internal ROM firmware** at `$C500`
(no WDM traps), which drives the IWM at `$C0E0-$C0EF` + `$C031` DISKREG
nibble-by-nibble exactly like hardware. Gate: `iwm35_test` (codec round-trip
on all 160 tracks, Sony status/command protocol, address-field decode through
the data latch, firmware-style sector write). Validated end-to-end: **GS/OS
6.0.1 boots from the 3.5" System Disk to the full Finder desktop** via this
path (`screenshot --iwm35`, ~6000 frames — reads pace at realistic speed).

Model (all cited in the sources):

- **$C031 DISKREG** — bit 6 (35SEL) reroutes the IWM phase/enable lines from
  the 5.25" stepper to the Sony register protocol; bit 7 (HDSEL) is the Sony
  SEL line (register-address bit + head/side select). MAME
  `apple2gs.cpp:268-269, 1995-2006, 3684-3721`. NB: `Iwm::motorOn()` (the
  $C036 disk-motor-detect speed coupling) reports the 5.25" motor only —
  with 35SEL set the ENABLE line belongs to the 3.5" drive.
- **Sony register protocol** — 4-bit index `{CA2,CA1,CA0,SEL}` (CA0-2 = IWM
  phases 0-2, LSTRB = phase 3). Status via the SENSE line (IWM status bit 7),
  commands on the LSTRB rising edge. Tables normalized from three sources
  that each pack the index differently: KEGS `iwm.c:912-1091`
  ({PH1,PH0,SEL,PH2}), MAME `floppy.cpp` mac_floppy ({SEL,CA2,CA1,CA0}),
  Neil Parker's note (= GSSquared) — all agree after normalization. One KEGS
  divergence kept: index `0b1010` returns 1 (a ROM 03 probe; MAME's
  non-SuperDrive returns 0) — KEGS is the IIgs-validated behaviour.
- **800K GCR codec** — 160 tracks (80 cyl × 2 sides), 5 speed zones of 16
  cylinders with 12/11/10/9/8 × 512-B sectors, interleave 2. The 6-and-2
  encode with the 3-byte rolling carry checksum is an **exact port of KEGS
  `iwm_nibblize_track_35` / `iwm_denib_track35`** (iwm.c:3125-3345 /
  2409-2725) — itself a disassembly of the IIgs ROM's own nibblizer (the
  `/* 63xx */` landmarks). `Sony35::checkNibblization()` = KEGS's
  `g_check_nibblization` self-check, pinned in `iwm35_test`.
- **Write path** — nibble-level: data writes land on the track stream right
  after the address field the firmware just read; dirty tracks are
  de-nibblised back to sectors on motor-off / step / eject / exit and patched
  into the backing file (.po in place, .2mg at the header's own **dataOffset** —
  *not* a hard-coded 64; see `TwoImg.h` / `twoimg_test`, which also owns the
  lock-flag rule the three loaders once each got wrong on their own).

**Three boot-blockers, all timing semantics (root-caused with an IWM access
trace against the ROM's own read routines):**

1. **Latch pacing** — the ROM's address-field hunt budgets *poll iterations*,
   expecting most `LDA $C0EC / BPL` polls to see bit 7 clear (a byte
   assembles only every ~16 µs). Delivering a fresh nibble on every read (our
   5.25" policy) exhausted the budget in 64 reads. Now a nibble is valid at
   most once per 16 µs (229 master ticks) and polls in between read $00 —
   with **elastic delivery** (KEGS `g_fast_disk_emul` discipline): the stream
   advances exactly one nibble per delivery, so a slow poller loses nothing.
   (A strict rotational model dropped nibbles whenever an iteration exceeded
   one nibble time and field decodes never lined up.)
2. **RDDATA sense toggling** — status `0b1000/0b1001` (instantaneous head
   data) must *toggle* while the platter spins; the ROM polls it for flux
   activity between fields and hangs on a constant line. Derived from time
   (`cycle>>4` ≈ 1.1 µs), not position.
3. **Handshake run-dry** — after its last data write the ROM waits at
   `$FF:57B7` (`LDA $C0EC / AND #$40 / BNE`) for handshake bit 6 to **drop**
   ("shifter empty"). KEGS parity (iwm.c:1147-1162): bit 6 is set only within
   ~8 bit-times of the last write — not MAME's constant `0xC0`.

Also deliberate: the sector-0 sync leader is 100 × $FF, not KEGS's
ROM-format 400 — with elastic delivery a hunt can start at the leader head
and the ROM's hunt budget (~420 nibble reads, measured) expires inside a
400-FF run. De-nibblisation does not care about leader length.

Also modelled: **write sessions** — data writes collect in a session buffer
closed by Q7/ENABLE falling, a data read, a step, motor-off or eject; a
session covering ≥ half the track **replaces it wholesale** (the ROM's FORMAT
writes whole tracks whose nibble count differs from ours), shorter sessions
splice in place (normal sector write). **Tach** pulses at the zone's real
rate (120 inversions/rev at 394/429/472/525/590 rpm — MAME
floppy.cpp:3374-3389), which the ROM's FORMAT counts to verify drive speed.
**Both internal drives** exist ($C0EA/$C0EB select, DRVIN = installed for
each); drive 2 mounts via `disk35b =` in `pomiigs.cfg` (or `--disk35b` on the
screenshot harness). All pinned in `iwm35_test`.

Open: end-to-end Finder "Initialize" pass (mechanics pinned by the full-track
rewrite test). (SWIM: out of scope — Mark Twain prototype only.)

### 5.25" bit-cell path — POM2 DiskImage port (read + write + WOZ)

The minimal nibble-stream 5.25" reader was replaced by **POM2's `DiskImage`
ported verbatim** (`src/DiskImage.{h,cpp}` + `TwoImg.h`/`Logger.h` — content
detector for .dsk/.do/.po/.nib/.d13/.2mg and **WOZ 1/2 incl. FLUX**, 160
quarter-track bit-cell streams, sync-aware expansion, flux splice, sector
de-nibblise + save-back). `Iwm` reads it bit-by-bit: the latch assembles one
nibble at a time from `bitAt()`, skipping leading zero cells while the MSB is
clear — 10-cell sync $FFs slip the byte boundary exactly like the real LSS —
paced at 8×~57 master ticks (32 µs) with elastic delivery (the 3.5"
discipline). Writes collect in a session (closed by Q7/ENABLE fall, read,
step, eject) and splice back as flux with the `computeCellWidths` sync rule
applied on emission (a $FF run ≥ 5 gets 10 cells — without this every written
gap $FF drifted the splice 2 cells). Validated end-to-end: **Choplifter
(.dsk) boots to gameplay and A.E. — a protected WOZ original — boots to its
title screen** via the internal $C600 PROM; RWTS-style sector write + file
write-back pinned by `iwm525_test` (gate) — mount via `disk525 =` in
`pomiigs.cfg` or File ▸ Load 5.25" Disk… (the headless `screenshot` / `triage`
harnesses take `--disk525 <image>`).

**Two hardware behaviours the boot ROM depends on (root-caused by tracing
the $C600 PROM instruction flow):**

1. **PH1 forces the sense line high** — the 5.25" status bit 7 is
   write-protect OR phase-1-energised (MAME floppy.cpp:799-805): the
   internal driver's drive-detect probe at `$FF:581C` polls status with PH1
   on and hangs unless the line answers.
2. **ENABLE2 (PH1+PH3 energised) addresses the external SmartPort chain**,
   not the dumb drive (KEGS iwm.c:494-505). The ROM's disk-port probe WRITES
   UniDisk command packets in this state; without the gate those bytes
   landed on the disk surface at the head position — wiping sector 0's
   address field on every boot (sectors 1-15 kept decoding, so the file
   itself survived via saveDirty's pre-fill). ENABLE2 now swallows writes,
   reads $FF, senses 1 (no chain device).

Limitations (documented, not blocking): quarter-track stepping is half-track
granular (qt = ht×2; adjacent-phase-pair positions = follow-up), partial-nibble
latch reads return $00 rather than the shifting register (bit-banging
protections like Spiradisc may object), write pacing is layout-inferred rather
than cycle-true.

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

---

## Clock & threading

The emulation worker + ImGui UI live in `src/Ui.cpp` / `src/main.cpp` (no
separate `EmulationController` class was forked). Master clock
**14.31818 MHz**; fast CPU budget = 2.8 MHz. `IIgsMemory` converts
architectural cycles → master ticks using the speed captured at the first bus
access of each instruction. Fast cycles cost 5 ticks; slow cycles follow the
Mega II grid (64×14 + one 16-tick cycle = 912 ticks/line). Fast-to-slow accesses
wait for the next PH0 boundary, while fast DRAM alone is stalled by the
5-tick-in-50 refresh window; ROM and fast FPI registers hide refresh. The
running total lives in `IIgsMemory::videoCycles_` and is
**the** timebase: the beam walk, the DOC (`tickMaster`), the ADB valves, the
paddle RC timers and the speaker's `$C030` toggle stamps all read it, the last
via `audioCycles()`. (POM2 calls its equivalent `emuCycles` and counts CPU
cycles; POMIIGS cannot — see `CLAUDE.md § Conventions`.)

`megaii_timing_test` pins the line/frame totals, the 16-tick long slot, the
25-line FPI/PH0 realignment, phase-dependent side-sync and refresh
classification, and — at instruction level — the placement of internal CPU
cycles: the CPU feeds each one to `IIgsMemory::internalCycles()` at its
microsequence position (five ticks, never stalled: with VDA low the FPI runs no
DRAM cycle), so `INC abs`'s modify cycle pushes its write-back into the refresh
window while `STA abs,X`'s index cycle lifts its write past it. The reset phase
of the refresh/PH0 grids still awaits calibration against a real-IIgs trace
before a whole-machine cycle-accuracy claim.

---

## ROM self-test gate

`selftest_trace` (`tests/selftest_trace.cpp`, CTest `rom0[13]_selftest_*`)
runs the ROMs' built-in diagnostics — ROM, RAM, soft switches, RAM address
lines, speed, serial, clock, battery RAM, ADB, FPI/Mega II, interrupts,
shadowing, sound — at native 2.8 MHz. The ROM samples ⌘+Option through the
classic push buttons `$C061/$C062` (ROM 03: `$FF:7F92`) as well as `$C025`, so
the tool holds all of them through `hardReset()`. Two modes:

- **Sequential** (`--through N`): the real self-test; verdict = "System Good"
  or "System Bad: TTSSSSSS" on the text page. Tests 02/04 (RAM) only work here.
- **Single** (`--test N` / `--entry BANK:ADDR`): boots the ROM for a few
  frames (bank `$E1` vectors, toolbox dispatch), then runs a stub at `$00:2000`
  that sets `$C036=$80`, clears `$0315-$0319`, JSLs the entry from the pointer
  table (ROM 03 `$FF:6403+`, count at `$FF:6402`; ROM 01 `$FF:7143+`, count×2
  at `$FF:7142`), forces 8-bit M/X back and takes the carry. ROM 01 tests may
  `RTS` instead of `RTL`, returning to `$FF:2018`; both landings are accepted.
  `--break BANK:ADDR` dumps registers/zero page at a PC, `SELFTEST_RING=1`
  prints the last 256 instructions on a missing verdict, `SELFTEST_JSL=1` logs
  caller→target pairs (how the ROM 01 table was found).

Three hardware facts fell out of it: the clock chip's seconds are writable
(§ Clock), unpopulated fast RAM reads as open bus instead of mirroring the
populated banks (the address-line test 04 loops forever on a mirror), and a
ROM 01 machine never shadows text page 2 (test 04 writes `$E1` first and the
shadowed bank `$01` write clobbered it). Test 09 needs the ADB µC firmware
(§ ADB) and SKIPs without it. `tools/dis65816.py <rom> BANK:ADDR [count]
[--m16] [--x16]` is the disassembler used for this archaeology.

---

## MiSTer custom diagnostics gate

The 21 open diagnostics of the MiSTer Apple IIgs core (`customtests/`, pinned
tarball in the catalog) are the broadest independent MMU/SCC/ADB/IWM/RTC
cross-check we have. Toolchain, rehosted under `tests/cycle_accuracy/cache/tools/`
(ignored): **Merlin32 v1.2** (Brutal Deluxe; universal macOS binary, the
sources are Merlin 8/16 syntax) and **CiderPress2 1.1.1** (`cp2`, x64
self-contained build, runs under Rosetta on Apple Silicon). Both archives are
SHA-256-pinned in `catalog.json § sources`.

```bash
T=$PWD/tests/cycle_accuracy/cache/tools
python3 tools/cycle_suite.py build --merlin-root $T/Merlin32_v1.2_b2 \
                                   --cp2 $T/cp2_1.1.1_osx-x64_sc/cp2   # 21 .2mg + WOZ media
python3 tools/mister_diags.py                                          # CTest: mister_diagnostics
```

`diag_trace` boots a disk (`--disk35/--disk35b/--disk525/--hdd`, `--iwm35`,
`--writeback`) for N frames with the periodic interrupts and dumps the text
page; `tools/mister_diags.py` matches each verdict against
`tests/cycle_accuracy/mister_goldens.json` (`expect: pass | xfail`, the
xfails name the gap; `mmu_test` is compared sub-test by sub-test). The floppy
test boots from the HDD slot with fresh copies of the cp2-generated WOZ media
in slots 5/6. Findings so far: ROM test 0B (interrupts) only passes with the
periodic Mega II/VGC interrupts running; ROM test 0A's failure under the
ProDOS launcher is the ADB µC version read (`$FF:6FD8`) timing out, the same
handshake `adb_device_enum` fails — the native `selftest_trace` launcher passes
both.

---

## Dev environment variables

Diagnostic taps compiled into the normal build — all opt-in, all off unless the
variable is set. Kept because each one has root-caused real bugs; documented
here so they don't get rediscovered by grepping.

| Variable | Where | What it does |
|---|---|---|
| `POMDBG=1` | `main.cpp` | Once-per-second health line on stderr: wall FPS (60 = keeping up), slot-5 device calls/s (the GS/OS removable poll's liveness — ~1-4/s at an idle Finder, 0 = the poll is dead and disk swaps won't mount), DOC samples/s (~26320 with 32 oscillators), audio underruns/drops (each one is an audible crackle) and ring fill. |
| `POMWAV=<path>` | `Audio.cpp` | Record the mixed output to a WAV for offline crackle analysis (header patched in the destructor, so exit cleanly). |
| `POMSP_LOG=1` | `IIgsMemory.cpp` | One line per SmartPort `$Cn53` dispatch (command, param list, block, buffer, error). This is the trace that root-caused the DIB / CONTROL / identity-byte game-loader bugs in `docs/COMPAT.md`. |
| `ADBDBG=1` | `IIgsMemory.cpp` | Log every `$C024-$C027` ADB GLU read/write with the issuing `PBR:PC` (first 120, then every 1000th). |

Headless harnesses under `tests/` add their own flags:

- `screenshot <rom> <out.png>` — `--frames N`, `--char <file>`, `--hdd <img>`,
  `--disk35 <img>`, `--disk35b <img>`, `--disk525 <img>`, `--iwm35`,
  `--writeback`. Env: `MOUSE_JIGGLE`, `MOUSE_DIR`, `KEY_INJECT`, `KEY_MOD`,
  `FRAMETICK`.
- `triage <rom> <disk>` — `--frames N`, `--disk35` / `--disk525` (which drive
  the single positional disk goes in; default 3.5"), `--iwm35`, `--char <file>`,
  `--png <out>`. Env: `TRIAGE_BRK`.
- `gsos_trace` — env `GSOS_WINDOW`, `KEY_INJECT`, `KEY_MOD`, `KEY_OA`,
  `MOUSE_JIGGLE`; `hdd_trace` — env `WATCH43`.

`screenshot` and `triage` mount every image **read-only** by default
(`screenshot` opts in with `--writeback`, `triage` never writes), so a triage
sweep can never edit the user's disk collection.
