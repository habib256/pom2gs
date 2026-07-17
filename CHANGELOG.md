# CHANGELOG

Resolved items + the **why** behind non-obvious decisions.

## [Unreleased] — Milestone 0: foundation

### Fixed — pass 9: differential audit round 3 (DOC IRQ, SCC, ADB, RTC, speed, CPU int)
Third differential pass on fresh routines; 4 confirmed 3/3–2/3, 3 applied (5 weaker findings correctly
rejected by the 3-judge panel):
- **$C036 SPEED: disk-motor-detect nibble never forced 1 MHz** (`IIgsMemory.h`, `Iwm.h`) — MEDIUM. Bits
  3-0 are per-slot Disk II motor-detect enables; on real hardware bit7 (2.8 MHz) is overridden back to
  1 MHz while a detect-enabled slot's drive motor spins — how the IIgs auto-slows for timing-sensitive /
  copy-protected 5.25" software. `speedFast()` looked only at bit7. Now `fast = bit7 && !(SL6-bit &&
  iwm_.motorOn())`, mirroring MAME `update_speed()`. Zero effect on normal software (bits 3-0 = 0, and
  GS/OS boots via SmartPort not the 5.25" IWM); only 5.25" titles that set the detect bit slow correctly.
- **SCC RR1 returned 0x01 instead of 0x07** (`Scc8530.cpp`) — the two residue-code bits (0x06) read set
  at reset per KEGS/MAME; drivers that check them now see the right idle value.
- **SCC RR3 read back WR3 garbage** (`Scc8530.cpp`) — RR3 is the interrupt-pending register (0 in our
  minimal, no-IP model; ch B always 0), not the WR3 contents. Now returns 0.

Deferred: RTC seconds-register writes are dropped (Control-Panel time-set doesn't stick) — 2/3, and
POMIIGS intentionally passes through the host clock (always shows correct real time); making it settable
(base-time offset) is a feature, not a correctness fix. Panel-rejected (weak/wrong): RR2 vector mod, WR2
shared-vs-per-channel, KMSTATUS keyboard-data bit position, $C025 no-key-down bit, extended-BRAM 2nd-byte guard.

### Fixed — pass 8: differential audit round 2 (fresh routines) + empirical CPU fix
Second differential pass on routines pass 7 didn't touch; 6 confirmed 3/3, 4 applied:
- **Bank $E0/$E1 accesses ignored //e aux redirection** (`IIgsMemory.cpp` read8/write8) — HIGH. The
  Mega II slow side is a full //e (bank $E0 = main, $E1 = aux), but $E0/$E1 accesses below $D000 used a
  flat `slowRam_[(bank-0xE0)*0x10000+off]` that never consulted ALTZP/RAMRD/RAMWRT/80STORE/PAGE2. Now
  bank $E0 routes through `physBank01()` exactly like bank $00 (KEGS moremem.c fixup_* loop over $00 AND
  $E0; MAME auxbank_update m_e0_*bank). Legacy 8-bit / ProDOS-8 / GS-OS code running in $E0 with aux
  switches active now hits the right image.
- **(dp,X) emulation-mode pointer high-byte page-wrap was wrong** (`CPU65816.cpp`) — the empirical Tom
  Harte oracle flagged $E1 SBC (dp,X) failing 1/10 000. Traced the exact vector (D=$F400, DL=0, offset
  wraps to $FF): the DL=0 branch page-wrapped the pointer hi byte (→ $F400) but real silicon reads it at
  ptr+1 (→ $F500, no wrap). NB: the diff agent's suggested fix (add wrap to the DL≠0 branch) was the
  OPPOSITE and would have regressed — caught by empirical trace. Fixed the DL=0 branch; **validated
  160000/160000 across all 8 (dp,X) opcodes** (01/21/41/61/81/A1/C1/E1, both modes).
- **Quarter-second Mega II interrupt fired every 15 VBLs, not 16** (`IIgsMemory.cpp`) — ~6.7% too fast.
  MAME uses `frame & 0xf`, KEGS `>= 16`. Fixed to 16; irq_test updated to assert the exact boundary
  (nothing at 15, fires at 16).
- **SmartPort STATUS for an offline drive returned $00 instead of $80** (`IIgsMemory.cpp`) — an empty
  3.5" drive is still a block device (bit7 set) though not online (bit4 clear), per KEGS.

Deferred: DOC control-register-write halt_osc swap/sync retrigger (touches the carefully-tuned oscillator
engine, narrow edge, was rejected in pass 2, can't validate audio headless); $C046 AN3 status bit (low
value, needs new AN3 annunciator state).

### Fixed — pass 7: differential audit vs the actual KEGS / MAME / GSSquared source
Agents fetched the real reference source and diffed it line-by-line against POMIIGS (verified by a
3-judge panel). 14 confirmed, 0 rejected — comparing against real reference code is far higher-signal
than LLM speculation. 8 applied, 1 reverted (broke a regression test), the rest deferred:
- **Extended-BRAM address decoded into the wrong bit-fields** (`IIgsMemory.cpp`) — HIGH. The 256-byte
  battery-RAM two-byte address used `(cmd&3)<<6` + `(data>>2)&0x3F` (2+6 bits) where KEGS clock.c,
  GSSquared RTC.hpp and MAME macrtc all use `(cmd&7)<<5` + `(data>>2)&0x1F` (3+5 bits) — dropping
  command bit 2 and shifting the low field. Every Control-Panel / GS-OS extended-BRAM byte (incl. the
  checksum bytes) landed on the wrong offset. Fixed to the 3-reference-corroborated decode.
- **VGC scanline STATUS bit only set when the enable bit was set** (`IIgsMemory.cpp`) — MAME sets the
  $C023 bit5 status unconditionally and gates only the IRQ on the enable; now matches (status visible
  to pollers, `updateVgcIrq()` still gates the interrupt).
- **SCC WR9 reset command was stored but never executed** (`Scc8530.cpp`) — WR9 bits 7-6 (0x40 ch-B,
  0x80 ch-A, 0xC0 hardware) now perform the reset (clear WR file + regPtr + FIFOs), per KEGS/MAME z80scc.
- **$C023 VGCINT write mask wrong** (`IIgsMemory.cpp`) — was `(&0xE0)|(v&0x06)`, MAME is `(&0xF0)|(v&0x07)`
  (preserve full status nibble, store all 3 enable bits incl. external-IRQ enable bit0).
- **SCC RR0 omitted CTS/DCD** (`Scc8530.cpp`) — seeded 0x2C (Tx-empty + CTS + DCD asserted) per KEGS,
  so drivers that gate transmit on CTS don't stall.
- **IWM handshake read returned 0x80 not 0xC0** (`Iwm.cpp`) — MAME `m_whd` = 0xC0 (ready + no underrun).
- **IWM motor-off data read returned 0x00 not 0xFF** (`Iwm.cpp`) — floating-high bus per MAME/KEGS.
- **Ensoniq $E1 osc-enable readback returned the raw byte** (`Es5503.cpp`) — MAME returns `(oscsenabled-1)<<1`.

Reverted after testing: the $C024 mouse-X-button differential (KEGS forces bit7 high on the X read)
broke `adb_test`, which asserts button-down → b7=0 on BOTH reads — the model that makes real games'
mouse work (Battle Chess / Captain Blood). The regression test is authoritative for observed behaviour.

Deferred (real per the diff, but risk-vs-reward unfavourable): last-oscillator 3× mix glitch (interacts
with the deliberately-tuned `>>7` output scale — would need audio re-tuning), ADB $10 µC-reset (2/3, would
perturb the tuned ADB stream), $C02D SLOTROMSEL (needs slot-ROM-dispatch wiring; a read-back-only half-fix
is misleading), IWM ~1 s motor spin-down timer (behavioural timer change to a working disk path).

### Fixed — pass 6: empirical CPU oracle (full Tom Harte 65816, all 256 opcodes)
Approach change: ran the FULL Tom Harte SingleStepTests/65816 corpus (256 opcodes × emul+native,
10 000 vectors each) as a differential oracle instead of more LLM review. This is empirical
ground truth and caught real CPU defects five static-review passes missed:
- **WAI ($CB) / STP ($DB) cycle count** (`CPU65816.cpp`). Both were bare NOPs consuming 1 cycle;
  Tom Harte requires 4 (3 internal + fetch). Added `cycles_ += 3`. Now 10000/10000 both modes.
  (The pass-1 "WAI/STP are NOPs" refutation was right about *behaviour* but missed the *cycles*.)

Investigated, NOT changed (with reasons):
- **MVN ($54) / MVP ($44)** fail the Tom Harte vectors, but the vectors are cycle-capped
  *mid-block-move* (e.g. 14 bytes / 100 cycles, PC = opcode+2). POMIIGS steps one byte per
  instruction and rewinds PC to the opcode — which is the documented *interruptible* MVN
  behaviour (an IRQ mid-move resumes correctly), and the full move copies correctly via run().
  Matching Tom Harte's mid-move micro-snapshot needs cycle-accurate micro-stepping — a large
  rewrite that would *break* interruptibility for no real-software gain. Left as-is by design.
- **SBC ($E1) (dp,X)** fails 1/10 000 — a rare emulation-mode direct-page-indirect pointer edge
  (not decimal: D=0 in the failing vector). Real impact is negligible (needs a specific DL/wrap
  combination emulation-mode software almost never hits); flagged for a future focused fix rather
  than a rushed change to the shared `ea_indx` path.

### Fixed — adversarial bug sweep, pass 5 (under-audited subsystems, 3-judge verify)
Fresh subsystems (NTSC decode, audio pipeline, IWM mechanics, DOC arithmetic, reset/init,
memory safety) verified by a NEUTRAL 3-judge panel (majority vote) instead of a single skeptic.
3 confirmed 3/3, 1 rejected:
- **DHGR composite artifact colours were phase-rotated 90°** (`VGCNtsc.h`). `decodeWordRow()`
  was shared verbatim by the HGR and DHGR composite paths and always used the HGR chroma phase
  (`rotl4b(lut, absX)`). DHGR is 80-column, so it needs `is_80_column = 1` → `absX + 1` (MAME
  `apple2video.cpp` render_line_artifact_color; POM2 `Apple2Display.cpp:2084`). Every DHGR
  composite hue was one phase off (blue↔orange, green↔magenta swapped). Parameterized the
  decoder with a phase bias (0 HGR, 1 DHGR).
- **Machine reset never reset the Ensoniq DOC (or SCC)** (`IIgsMemory.cpp` reset). Oscillators,
  sound RAM and a pending DOC IRQ persisted across reset, so sound kept playing and a stale DOC
  IRQ could be re-asserted into the booting ROM. Added `doc_.reset(); scc_.reset();` (MAME resets
  the ES5503 subdevice on machine reset).
- **Audio: no final clamp before the float32 backend** (`Audio.cpp`). The one-pole DC blocker
  (and volume) can transiently push a sample to ~1.44×; the miniaudio f32 device hard-clips and
  the POMWAV int16 cast wraps — both audible clicks on large transitions. Added a saturating
  clamp to [-1,1] after volume, before push()/WAV (also resolves the rejected POMWAV-overflow
  finding, same root cause).

### Fixed — adversarial bug sweep, pass 4 (neutral re-judging recovers false negatives)
Approach change after diminishing returns (5→5→2): instead of more static review, a
NEUTRAL 3-judge panel re-tried 10 findings the earlier skeptics had *refuted* (those
skeptics were biased toward "not a bug"). 9/10 flipped to REAL by majority — the earlier
refutations had a high false-negative rate. Each was then re-verified by hand against the
code + MAME before acting; **3 applied, the rest deferred as risky-or-wrong** (see below):
- **SmartPort/ProDOS: empty 3.5" drive returned $27 (I/O error) instead of $2F (offline)**
  (`IIgsMemory.cpp` smartportCall + prodosBlockCall). `readBlock`/`writeBlock` fail both when
  empty and on a bad block; now distinguishes `!disk35_.loaded()` → $2F (no disk) from a
  loaded-but-out-of-range block → $27, per the ProDOS/SmartPort error convention. Volume-scan
  logic that treats $2F as "skip, no media" now behaves correctly.
- **VGC: $C032 VGCINTCLEAR cleared BOTH status bits unconditionally** (`IIgsMemory.cpp`).
  Acking one VGC interrupt (scanline) while writing bit6=1 to *preserve* a pending one-second
  interrupt silently dropped the latter. Now write-0-to-clear per bit, matching MAME
  `clear_vgcint()`. (Also independently surfaced by the IRQ-consistency finder.)
- **Sound GLU: $C03C read returned volume bits verbatim** (`Es5503.cpp` gluRead). The write-only
  volume bits 0-4 read back as 1 on hardware; now `(ctl_ & 0x7F) | 0x1F` per MAME
  `m_sndglu_ctrl | 0x1f`.

Two more of the recovered findings were then applied (real, safely fixable, verified):
- **KBDSTRB $C010 bit7 always read 0** (`IIgsMemory.cpp`, `IIgsMemory.h`, `main.cpp`). The read
  masked bit7, so software could never see any-key-down (auto-repeat / "hold a key" logic).
  Added a live `anyKeyDown_` state driven each frame from the physical key state (char events
  are edge-only) and OR'd it into $C010 bit7, per MAME `GLU_ANY_KEY_DOWN`.
- **ADB $Bn / $11 consumed 0 parameter bytes** (`IIgsMemory.cpp`). Listen-dev-reg-3 ($B0-$BF, 2
  data bytes) and Send-ADB-keycodes ($11, 1 byte) fell to the default and left the following
  data bytes to be mis-parsed as commands, desyncing the µC stream. Added both per KEGS `adb.c`.

Still DEFERRED with concrete reasons (holding beats regressing tuned/working code): IWM data-latch
force-advance — the deliver-a-nibble-per-read hack currently boots 5.25" reads; the accurate-timing
rework is unvalidatable without a WOZ round-trip test and isn't the GS/OS boot path (SmartPort HLE).
CLI/SEI/PLP IRQ delay-slot — MAME's own g65816 (our source of truth) does NOT model it, so adding it
would diverge from the reference and Tom Harte can't validate it. $C02D SLTROMSEL read-back (needs
slot-dispatch wiring) and $C068 bit1 (uncertain ROMBANK-vs-RAMDIS semantics) left as low-value.
SPEED reset default = slow is INTENTIONAL, pinned by speed_test (firmware raises it to fast on boot).
mouse ±64 clamp is CORRECT (7-bit two's-complement is -64..+63, not ±63) — a re-judge false positive
caught by hand-verification.

### Fixed — adversarial bug sweep, pass 3 (2 confirmed on fresh subsystems)
Third find-then-refute audit (CPU control-flow, language card, IWM/Sony, Sound GLU,
video IRQ, main loop — all 10 prior fixes excluded). 10 candidates, 8 refuted, 2 fixed
(diminishing returns: 5 → 5 → 2 across the three sweeps):
- **Input: host keyboard forced every letter to UPPERCASE** (`main.cpp`). The typed-char
  path did `if (a>='a'&&a<='z') a -= 0x20` before latching at $C000, so lowercase input
  was impossible in *all* IIgs software (filenames, text fields, source). ImGui's
  `InputQueueCharacters` already delivers OS-cased ASCII and the $C000 latch returns the
  low 7 bits verbatim — the IIgs ADB keyboard is not the uppercase-only //e. Dropped the
  fold; the separate Cmd/Ctrl shortcut path (A-Z for GS/OS menu keys) is untouched.
- **Language card: pre-write flip-flop not cleared on an odd $C08x WRITE** (`IIgsMemory.cpp
  lcSwitch`). Write-enable requires two consecutive odd READS; any write access must reset
  the pre-write latch and can never arm write-enable. The odd branch only handled reads, so
  `LDA $C089 ; STA $C089 ; LDA $C089` spuriously write-enabled the LC where hardware leaves
  it protected. Now clears `lcPreWrite_` on an odd write (KEGS `g_c08x_prewrite = !write`,
  Sather *Understanding the Apple IIe*).

Refuted in pass 3 (non-bugs): CLI/SEI/PLP/RTI IRQ delay-slot, SmartPort 3.5" write-allowed
bit / $27-vs-$2F empty-drive code / data-latch force-advance, $C03C SOUNDCTL pointer-clear
& read bits, $C032 VGCINTCLEAR write-zero semantics, SPEED register slow-vs-fast power-on
default — each read against the code and found harmless or already handled.

### Fixed — adversarial bug sweep, pass 2 (5 more confirmed, complementary lenses)
A second find-then-refute audit (6 reviewers on CPU-values, ADB, RTC/clock, video,
audio/DOC, disk-write — each told the 5 pass-1 fixes to skip) surfaced 13 candidates;
8 refuted, 5 confirmed and fixed:
- **VGC: MIXED mode never honoured** (`VGC.cpp`). `render()` drew every legacy graphics
  mode full-screen and never read `mem.mixed()`, so the bottom 4-row text window of
  HGR/DHGR/LORES games ($C053) rendered as graphics garbage. Added `renderTextBand()`
  (draws text rows 20-23 over the graphics, 40- or 80-col per softswitch), matching
  POM2 `Apple2Display` (graphics 0-159, text band 160-191).
- **DOC: SYNC/AM (mode 2) mix-path missing** (`Es5503.cpp step1`). Pass 1 fixed the
  hard-sync accumulator reset in `haltOsc`; the *mix-side* AM is separate and was still
  absent — every osc summed unconditionally. Per MAME `sound_stream_update` MODE_SYNCAM,
  an ODD mode-2 osc must not sound and instead writes its sample byte into osc+1's
  volume register (AM of the carrier); EVEN mixes normally. Verified via live MAME that
  `partner.vol = data ^ 0x80` with `data = byte ^ 0x80` = the raw byte. Added the branch
  (guarded `o<31` + partner-running).
- **VGC: VERTCNT ($C02E) missing the vertical-counter bias** (`IIgsMemory.cpp`). Returned
  raw `vpos()>>1`, so the first visible line read 0x00 instead of 0x80 and VERTCNT bit7
  was inverted across the whole screen. Ported MAME `get_vpos()`: since POMIIGS `vpos()`
  is already the visible-line index, the `256 - BORDER_TOP` bias reduces to `256 + vpos()`
  wrapping past 511 by a frame height. Line 0 → 0x80, line 191 → 0xDF (raster-split titles).
- **Disk: 2mg 'locked' flag ignored** (`ProDosHdd.cpp`). `loadImage()` stripped the 64-byte
  2IMG header but never read the flags dword (offset 16, bit31 = write-protected), and
  `writeProtect_` was never assigned — so a locked .2mg was silently writable and
  `flushBlock()` corrupted the host file. Now parses the flag (and resets it per load);
  the existing write-protect machinery (`writeBlock`/STATUS DIB) does the rest.
- **ADB: commands $0E/$0F returned no bytes** (`IIgsMemory.cpp`). Read-available-char-sets
  ($0E) and read-keyboard-layouts ($0F) had no case, fell through, and left the DATAREG
  FIFO empty → firmware/Control Panel read 0x00,0x00. Added both (count + current id),
  per KEGS `adb.c`.

Refuted in pass 2 (non-bugs): emulation-mode BRK vector via `read8` (again — intentional,
matches the code's documented //e-compat choice), DP/stack 16-bit operand bank-1 crossing,
ADB $Bn/$11 param counts, KBDSTRB any-key-down clear, mouse ±64 vs ±63 clamp, DOC newCtrl
IRQ-suppression guard, DOC last-osc triple-weight / ÷8 (POMIIGS uses a different >>7 scale),
2mg variable header length.

### Fixed — adversarial bug sweep (5 confirmed defects, cited to MAME/datasheet)
An adversarial find-then-refute agent audit (6 subsystem reviewers → per-finding
skeptics) surfaced 14 candidates; 9 were refuted as non-bugs, 5 confirmed and fixed:
- **CPU: RMW abs,X under-counted one cycle** (`CPU65816.cpp`). ASL/LSR/ROL/ROR/INC/DEC
  absolute,X form their EA via `ea_absx()`, whose `idxCross` only charges the index
  cycle on a 16-bit index OR a page cross. But an indexed **write-back** always pays
  that cycle (the write is unconditional) — exactly why the STA/STZ abs,X sites add
  `cycles_ += !idxPen`. The RMW sites omitted it, yielding 6 cycles instead of 7 when
  `eX` and no page cross. Added the same compensation to 0xFE/0xDE and the shift/rotate
  block. **Validated cycle-exact: Tom Harte 65816 1e/3e/5e/7e/de/fe = 120000/120000 OK.**
- **MMU: text/lores PAGE 2 ($0800-$0BFF) never shadowed** (`IIgsMemory.cpp maybeShadow`).
  Only page 1 was gated; the `SHAD_TXTPG2` (0x20) bit was dead code and the VGC scans
  page 2 from the slow side (`VGC.cpp` via `textPage2()`), so fast-side page-2 writes
  in a PAGE2-flip (`page2_ && !store80_`) config showed a stale image. Added the
  `0x0800-0x0BFF` branch gated by `SHAD_TXTPG2`, mirroring page 1. (MAME `shadow_w`.)
- **DOC: SYNCAM (mode 2) hard-sync not implemented** (`Es5503.cpp haltOsc`). The code
  collapsed sync/AM to plain free-run; MAME `halt_osc` also resets the odd partner
  oscillator's accumulator (onum-1, verified against live MAME source) when an even
  oscillator wraps and the partner is still running, locking the pair to one period.
  Added the partner-accumulator reset with an `o>=1` guard (avoids the negative-index
  underflow MAME leaves latent at oscillator 0).
- **VGC: lo-res/text/border colour 4 (dark green) lost its blue** (`VGC.cpp`). Entry 4
  was `0xFF007700` = $070; the canonical IIgs value is $072 → `0xFF227700` (blue nibble
  0x2). Isolated transcription slip; all 15 other entries matched. Fixed the one entry.
- **Snapshot: SCC 8530 state was omitted** (`Scc8530.*`, `IIgsMemory.cpp`, `Snapshot.cpp`).
  Every other C0xx device round-trips but the SCC had no serializer, so a load kept the
  live object's WR file / regPtr / rx-tx FIFOs. Added `Scc8530::saveState/loadState`
  (per-channel WR[16] + regPtr + length-prefixed FIFOs), wired alongside `doc_`, bumped
  snapshot `kVersion` 2→3.

Refuted (documented as non-bugs, kept for the record): emulation-mode BRK vector via
`read8`, WAI/STP as NOPs, slot-ROM INTCXROM gating, IWM stream ~14× (feeds `videoCycles_`,
not raw CPU cycles), ProDOS-8 buffer bank-0 truncation, SCC WR0 command decode / RR2-3 —
each checked against the code and found harmless or already compensated.

### Fixed — ADB µC command param counts (Captain Blood's mouse was frozen)
- **Captain Blood's arm-cursor was stuck top-left** (mouse deltas never reached it).
  Unlike Battle Chess (native ADB-IRQ path) it does its ADB setup through the µC
  **command stream on $C026** then reads the mouse via the ROM's `ReadMouse`
  ($C024). Trace: my µC command machine had **wrong parameter counts and invented
  commands** ($0E/$0F/$10/$11), so the setup stream desynced, `do_adb_cmd` never
  completed, and the game never reached the $C024 autopoll read. Rewrote the table
  to KEGS `adb.c` exactly (abort/flush 0, set/clear-modes 1, set-config 3, sync 8/4
  by ROM, write/read-mem 2, read-modes/config/version responses; ADB-bus $2n/$Bn
  bytes absorbed). Now the ADB init completes and `ReadMouse` (FE:B1EB) reads the
  deltas correctly. Battle Chess / Arkanoid / the Finder mouse unaffected (Battle
  Chess still 676 px cursor motion). Open: Captain Blood's "planet colours look
  buggy" — NOT reproduced headless (title + planet-nav SHR render correctly, 2
  per-scanline palettes applied); needs a user screenshot of the exact frame.

### Fixed — the video/wall-clock timebase counted CPU cycles (VBL at 164 Hz!)
- **Game music ran ~2.8× fast and digitized samples stopped ~2.8× early** (Captain
  Blood, Transylvania III), while synthLAB's tempo was correct. Root cause: the
  machine's wall-clock timebase (`videoCycles_`) counted **raw CPU cycles**, so at
  2.8 MHz the emulated video beam scanned 2.8× too fast — **VBL fired at ~164 Hz
  instead of 60** — and every VBL-clocked engine (game music tick, sample-duration
  timers) ran accelerated. synthLAB was immune because it clocks from the DOC.
  A chip-level audit had already cleared the DOC (a one-shot sample test measures
  EXACTLY the hardware duration at 1/5/32 oscillators — pinned in scratchpad).
- **Fix: one master-clock timebase** (14.318 MHz) for everything: `tick()` computes
  the instruction's master ticks once (CPU cycles × 5 fast / 14 slow + the
  slow-side stall penalty — the beam keeps moving while the CPU stalls) and
  advances the beam AND the DOC with it. One line = 65 slow cycles × 14 = 910
  ticks; 262 lines = 238420 = exactly the frame target. Converted consumers:
  `vpos()`, HORIZCNT (`/14`), the ADB 2-frame valves, the paddle RC constant
  (11 slow cycles × 14 per count). At reset the machine is slow (×14), so all
  slow-side timing — and every existing test — is bit-identical.
- Verified: **Captain Blood now completes its intro to "PRESS FIRE TO BEGIN"**
  (previously stalled at the planet screen waiting on its sample/VBL state
  machine); GS/OS boots clean; Battle Chess raster sync intact; 15/15 gates.

### Fixed — in-game mice: ADB GLU interrupt-enables + µC command machine
- **Games' mice were dead** (Battle Chess's hand cursor frozen; Arkanoid's paddle
  barely responding) while the Finder mouse worked. Probes (ADBDBG read/write traces
  + injected motion): games program the ADB GLU directly — Arkanoid writes a µC
  command stream to `$C026` at init (`$04/$05` set/clear modes, `$06` set config…)
  then writes **`$C027 = $30`** to enable the DATA interrupt, and waits for ADB
  **IRQs** instead of polling; the old stub ignored `$C027` writes entirely and
  answered every `$C026` command with a canned byte, and the ADB IRQ was gated on a
  "native mode + VBL int enabled" heuristic games never satisfy. Three fixes
  (KEGS `adb.c` as the HLE reference):
  1. **`$C027` writes latch the interrupt-enable bits** (b6 mouse / b4 data / b2
     keyboard) and reads report them (b6 still also raised with b7 for the ROM's
     poll path, which never programs enables).
  2. **`updateAdbIrq` follows the hardware rule**: a source delivers when ITS
     enable bit is set (the legacy sysUp gate is kept for the ROM/GS-OS path).
  3. **A real `$C026` µC command machine** (`adbCommand`): commands consume their
     parameter counts and queue real responses (modes byte read-back, version,
     charsets/layouts, config), drained by `$C026` reads.
  Verified headless: Battle Chess attract, 120 frames of injected motion → 676
  pixels changed (cursor moves) vs 0 in the no-motion control. Snapshot format
  bumped to v2 (+ the ADB enables/modes).

### Fixed — HORIZCNT ($C02F) horizontal beam counter (Battle Chess froze at the board)
- **Battle Chess drew its chessboard then froze**: the game raster-syncs by polling
  `$C02F` in a tight loop at `$00:AB26` (`LDA $C02F / AND #$1F / BEQ`), waiting for
  the beam to enter a horizontal window. POMIIGS returned only the vertical parity
  bit — **the horizontal counter bits 6-0 were stuck at 0**, so the loop spun on a
  frozen beam forever. Fix (IIgs HW Ref): `$C02F` = vertical count bit0 in bit7 +
  the horizontal counter in bits 6-0 (one `$00` state then `$40-$7F` across the
  65-cycle scan line, derived from `videoCycles_ % 65`). Battle Chess now runs into
  its attract mode (replaying Saemisch–Nimzovich, Copenhagen 1923). First fix from
  the games-compat pass — any raster-synced title benefits.

### Added — universal media browser (the Load dialog grew directory navigation)
- Every "Load…"/"Insert…" dialog is now a real file browser: **Places** bar
  (disks35 / hdv / roms / the games share / Home — shown only if they exist),
  **Up**, directory entries (`[+]`) with double-click navigation, files filtered
  by media kind (.rom/.bin, .hdv/.po/.2mg, .2mg/.po/.dsk), double-click-to-load,
  a sticky last-directory per kind, and the manual path field kept for exotic
  locations (typing a folder navigates into it). Unblocks browsing the 361-image
  games collection from the UI.

### Added — Save/Load state (F7/F8, `Snapshot.h/.cpp`)
- Machine snapshots to `states/quick.pgss` (Machine menu or F7 save / F8 load):
  a `PGSS` v1 header + 65C816 registers + `IIgsMemory::saveState` (fast+slow RAM,
  every MMU/LC/video/ADB/clock softswitch, BRAM, the full DOC via
  `Es5503::saveState`, and the mounted media paths — remounted on load if they
  differ). POM2's MachineSnapshot split: the wrapper owns the file format, each
  subsystem owns its field roster. Host transients (speaker stamps, audio rings,
  penalty accumulators) reset on load and the IRQ lines are re-derived from the
  restored registers. **Gate `snapshot_test`: boots GS/OS from the HDD, saves,
  diverges 1M instructions, loads — the next 200 000 instructions retrace the
  identical PC path.** The RewindBuffer ring (POM2 port) comes later — the IIgs
  fast side is up to 8 MB per snapshot, so the ring needs delta compression.

### Fixed — residual cracks: speaker DC offset amplified device-level gaps
- After the `$C031` fix, a 100 s `POMWAV` capture of a full synthLAB session showed the
  MIXED signal essentially clean (2 sub-1100 jumps in 100 s, both plausibly note
  attacks) — so the remaining audible cracks happen **after the tap**, at the device
  layer (an underrun's zero-padding or a push drop). The amplifier: the 1-bit speaker
  level parked the output at a **±0.15 DC offset**, so any zero-padded gap was a
  full-amplitude step = a loud click. Fix: **AC-couple the mix** (one-pole DC blocker,
  fc ≈ 20 Hz) — a real speaker can't hold DC; gaps now land on a ~0-centred signal
  (nearly inaudible) and the square wave gets the real cone's exponential sag. Also
  added a push-side `drops` counter to the `POMDBG` line (the other loss the WAV tap
  can't see).

### Fixed — THE "random cracks" root cause: $C031 is DISKREG, not a speaker mirror
- **Every system beep rendered as a sharp CLICK instead of a tone — the "cracks" heard
  since the first boot.** Diagnosed from a `POMWAV` capture (beeps appeared as 3-4
  isolated edges with the speaker level then STUCK for 18 s) + a headless probe that
  compared raw `$C030` toggle stamps against the rendered wave: the boot beep's **196
  toggles rendered as 4 edges**, and the stamps arrived in **same-cycle pairs**
  (`0, 1792, 0, 1792 …`) that cancelled inside one output sample. The pair-maker: the
  IIgs ROM's beep loop is a **16-bit `LDA $C030`** (`FF:9AE7`, m=0) whose second bus
  access hits **$C031** — and POMIIGS toggled the speaker on `$C031` too, keeping the
  classic Apple II's partial `$C030-$C03F` decode. **On the IIgs, `$C031` is DISKREG**
  (3.5" drive/head select — MAME `apple2gs.cpp`), NOT a speaker mirror. Fix: only
  `$C030` toggles; `$C031` is now a readable/writable DISKREG stub. After the fix the
  probe shows **93 toggles → 93 edges**, a clean ~285 Hz square — the classic IIgs
  boot "bong", audible for the first time. Every GS/OS beep (alerts, synthLAB events)
  was a crack before this.

### Fixed — random crackles during music (clock drift) + DOC output level
- **Random cracks that ruined long music playback**: the mixer pushes vsync-paced
  (~60 Hz × 735 samples) while the audio device pulls at its own crystal's 44100 Hz —
  the two clocks drift by fractions of a percent, so the ring slowly drains (underrun
  crackle) or fills (burst drop) every few minutes. Fix: **dynamic rate control**
  (KEGS `sound.c` approach) — each frame nudges the sample count ±8 (±0.65 %,
  inaudible) to keep the ring centred in a [1100..1900] band; the startup pre-fill sits
  at the band centre (1536 ≈ 35 ms). Underruns and drops become structurally
  impossible while the emulator keeps ~60 fps.
- **DOC still too quiet**: mix gain `kDocGain` 0.55 → 0.95 (music near full scale; the
  final float mix clamps). NB the GS Control Panel volume slider genuinely attenuates
  now (Sound GLU bits 0-3) — half volume = half amplitude.
- `pomiigs.cfg` default `disk35 =` is now the synthLAB disk (mounted online at boot in
  `boot = hdd`, see below).

### Fixed — 3.5" menu inserts invisible when booting from the HDD (empty drive)
- **Inserting a 3.5" disk from the menu did nothing in `boot = hdd`** (synthLAB never
  mounted). Root-caused with the new `POMDBG=1` per-second health line (wall FPS,
  slot-5 device calls, DOC samples/s, audio underruns): the user's trace showed
  **`sp5=0/s` forever** — GS/OS never enumerated the drive. Two layers:
- **(1) The 3.5" card's firmware vanished without a disk** (`slotRomRead` gated the
  slot-5 card ROM on `loaded()`): an empty-at-boot drive served the INTERNAL SmartPort
  firmware instead, GS/OS bound the real-IWM Sony driver to a drive we don't model at
  that level, and our HLE was invisible. The card ROM is now always present (a real
  drive's firmware doesn't vanish with the media); STATUS reports "no disk" until an
  image is inserted. After the fix the boot enumerates the drive (`sp5-boot=4`).
- **(2) GS/OS never re-polls a SmartPort drive that was OFFLINE at boot** — its media
  detection rides the `$2E` disk-switched error on the next access of an *online*
  volume, and an empty drive gets no accesses. That's why every mount that ever worked
  had a disk in the drive at boot. All references (KEGS/MAME) sidestep this by
  modelling the real IWM/Sony port whose driver polls disk-in-place — our "low-level
  IWM 3.5" TODO. Until then, **`boot = hdd` now also mounts the configured
  `disk35 =`** so the drive is online from boot — and hot-swapping then works:
  headless-verified end-to-end for the first time (boot HDD + Install online, menu-swap
  to synthLAB → 35 slot-5 calls and the desktop icon within **1 s**).
- Also: the audio device now pre-fills ~90 ms of silence — the empty-ring gap between
  device start and the first mixed frame was ~55 underruns (an audibly crackly boot
  beep; steady-state underruns measured 0/s for 100 s).

### Fixed — DOC ring under-production (crackles) after the cycle-driven change
- The first cycle-driven build **crackled much more**: the tick→master conversion
  omitted the **slow-side stall penalty**, so the DOC ring under-produced every frame
  (production = 238420−P ticks vs the frame loop's 238420) and the drain — at a fixed
  nominal ratio — ran the ring dry, emitting hold-gaps. Two fixes: (1) `tick()` now
  adds the per-instruction penalty delta (`slowPenMaster_` high-water peek — the same
  accounting the frame loop uses); (2) `drainResampled` is **self-balancing**: it
  spreads exactly what was produced since the last drain across the frame's output
  samples (occupancy-driven, gapless by construction; pitch = the emulated rate; an
  empty ring holds the last level). The CPU IRQ mirror is also edge-only now (was a
  per-instruction atomic store). Pinned by doc_test 16.

### Fixed — DOC follow-ups from the first synthLAB run (tempo, IRQ storm, volume)
- **synthLAB played music (first DOC audio ever) but ~3× too slow, too quiet, and
  eventually crashed to a BRK cascade in unmapped banks (`$4C`, `$B1`).** Three fixes:
- **(1) Cycle-driven DOC (tempo).** The chip was only advanced once per video frame
  (inside the audio mixer), so the timer-oscillator IRQs music engines derive their
  TEMPO from were batched at 60 Hz — 3-4 IRQs merged into one → ~3× slow playback.
  Now `IIgsMemory::tick` drives `Es5503::tickMaster` with the master-clock ticks each
  instruction consumed (one native sample per 16×(oscs+2) ticks — MAME output_rate
  restated in master ticks), into a ring that `Audio::mixFrame` drains resampled
  (`drainResampled`). IRQs land at their real time; the CPU IRQ line mirrors per tick.
  Boot perf unchanged (1500 frames ≈ 1.2 s headless). The project's own `emuCycles`
  convention, finally applied to the DOC.
- **(2) IRQ-line mask (the crash).** `irqPending()` tested ALL 32 pend bits but the
  `$E0` ack scan only covers the ENABLED oscillator count — a stale pend above a
  shrunken `$E1` count could hold the CPU IRQ line high forever: an interrupt storm
  that chews the stack and derails into unmapped banks (the observed BRK cascade,
  `K=$4C`/`$B1`). The line is now masked to the enabled count, and the per-tick
  delivery (1) removes the batched multi-IRQ bursts that stressed the handler.
- **(3) Output scaling (volume).** `(mix × gluVol) >> 7` (was >>8): a 4-8-voice
  musical mix at max GLU volume now lands near full scale (doc_test tone rms 1436 →
  2871). Pinned by `doc_test` check 16 (tickMaster/drainResampled tone).

### Fixed — Ensoniq 5503 DOC: MAME-parity engine + two Sound GLU bugs (silent DOC)
- **The Sound GLU control-register bits were SWAPPED** (`Es5503::gluRead/gluWrite`):
  hardware (MAME `apple2gs.cpp` `$C03D` handlers) is **bit6 = RAM/registers select,
  bit5 = address auto-increment** — POMIIGS had them inverted, so every DOC *register*
  write done the normal way (`ctl=$2x`, registers + auto-inc) landed in **sound RAM**
  instead: no oscillator was ever keyed on and the DOC stayed silent forever. (RAM
  loads used `ctl=$6x` — both bits set — and worked by luck, masking the bug.)
  Found by tracing a full GS/OS boot + BASIC run: 160k+ GLU writes, **zero** register
  writes. Also added the **`$C03D` one-deep read latch** (MAME `m_sndglu_dummy_read`):
  reads return the *previous* fetch — the dummy-read/stream protocol the `$E0` IRQ-ack
  ISRs rely on.
- **The oscillator engine was rewritten to MAME `es5503.cpp` parity** (was a partial
  stub): **swap mode** with partner start (`pPartner->control &= ~1`) — the ping-pong
  GS/OS's Sound Tools and synthLAB stream audio with; the **even-oscillator retrigger
  quirk** ("verified on IIgs hardware", MAME `halt_osc`); free-run **phase-preserving
  wrap** (`acc -= wtsize << resshift`); sync/AM halting like free-run; per-oscillator
  **IRQ pend flags + the `$E0` protocol** (lowest pending osc in bits 5-1, read clears
  one pend, line holds while others pend, bit7 = none); the exact
  wavesizes/wavemasks/accmasks/resshifts tables; and **native-rate rendering** —
  `docRate = clock/8/(oscs+2)` (894886/(N+2) Hz; 26.3 kHz at GS/OS's 32 oscillators)
  resampled to the host rate, fixing the former "rendered at 44.1 kHz" ~68 %-sharp
  pitch. GLU master volume (ctl bits 0-3) now scales the output. Pinned by the
  rewritten `doc_test` (15 checks: tone, absolute pitch, swap ping-pong, `$E0` stream,
  GLU decode + latch).
- Verified along the way: the IIgs **boot beep and the text BELL are `$C030` speaker
  tones, not DOC** (identical ~190-toggle signatures at boot and after a BASIC
  `PRINT CHR$(7)`) — so a silent DOC during boot/Finder/BASIC is correct; the DOC's
  first real customers are synthLAB / games / the Media Control sound patches.

### Fixed — BASIC.System / ProDOS 8 launches from the GS/OS Finder (two MMU gaps)
- **Launching BASIC.System (or any ProDOS-8 app) from the Finder crashed to the //e
  monitor** (register dump on the text screen). Diagnosed with a temporary CPU trace
  (`P8LOG`, since removed) and fixed as **two missing MMU pieces**, both cited from
  MAME `apple2gs.cpp` + KEGS `moremem.c`:
- **(1) The Mega II language card was missing** (`IIgsMemory::slowLcRead/slowLcWrite`).
  Banks `$E0/$E1 $D000-$FFFF` are a full //e language card over slow RAM (MAME `lc_r`/
  `lc_w` + the `m_lcbank` views): read-ROM mode shows the bank-`$FF` ROM; RAM mode banks
  `$D000` (bank 2 lives in the unused `$C000-$CFFF` window, `m_megaii_ram[off+0xc000]`);
  ALTZP swaps `$E0` onto the aux side; writes gated by the LC write-enable. POMIIGS
  served **flat slow RAM** — LC bank 1/2 aliased into one region, so GS/OS's ProDOS-8
  launch glue in the `$E0` LC (P8 lives in LC bank 2) was corrupted, and the launch
  stub's RTL to `$E0:D3FA` executed garbage → BRK at `$E0:D406`. Also reset now matches
  MAME's LC default: *read ROM, write **enabled**, bank 2* (write was disabled).
- **(2) The internal `$C100-$CFFF` firmware was unmapped** (`IIgsMemory::slotRomRead`).
  Slots other than our cards (5 = 3.5", 7 = HDD) returned **0** — so BASIC.System's
  PR#3-style `JSR $C300` (80-column firmware) executed zeros (`$00` = BRK) → monitor.
  Now every unclaimed slot page + the `$C800-$CFFF` expansion window serves the IIgs
  internal firmware image from the bank-`$FF` ROM (`FF:C100-CFFF`), same as KEGS
  (`g_rom_fc_ff`) and MAME (inh views).
- **Verified end-to-end headless** (`scratchpad/basic_launch` harness: boots the HDD to
  the Finder, drives it by keyboard — type-select `G`, ⌘-O, `B`, ⌘-O): ProDOS 8 V2.0.3
  splash → `PRODOS BASIC 1.5` → the `]` prompt → typed `PRINT 2+2` → **`4`**. The
  earlier "80-column display bug" report was this crash (the monitor's register dump
  happens to render on the 80-col text screen); the 80-col renderer itself was fine.
- Likely also fixes the **intermittent boot hang with audio crackle**: its trace
  signature (spin at `FF:CF94` "polling EMStatus") turned out to be the ROM's **KEYIN
  key-wait** — i.e. the machine had silently BRK'd to the monitor (`*` prompt, invisible
  under the SHR boot screen) and was waiting for a keypress; same class of silent Cxxx/LC
  crash as above. Needs interactive confirmation.

### Added — GS/OS boots from the hard disk (`boot = hdd`)
- With a full System 6.0.1 install on `hdv/GSOS.hdv`, **`boot = hdd` now starts GS/OS to
  the Finder straight from the hard disk** (SHR on ~750k steps, ~141k toolbox dispatches,
  desktop drawn, no derail; headless `hdd_trace none hdv/GSOS.hdv`). **Gotcha:** the
  Installer's Easy Update copies the System Folder + the `PRODOS` loader but does **not**
  rewrite the ProDOS **boot block** (block 0) on an already-formatted volume, so the
  freshly-installed disk stayed non-bootable (byte 0 = `$00` → the slot-7 card chained to
  the slot-5 install disk). The ProDOS boot block is a standard, device-agnostic block-0
  loader (finds `PRODOS` in the volume directory and runs it). Fix: `make_prodos_hdd.py`
  gained `--boot-from <prodos-disk>` to copy a real boot block from any bootable ProDOS/
  GS-OS image (e.g. a System 6 install disk); `hdv/GSOS.hdv` now carries one. `pomiigs.cfg`
  defaults to `boot = hdd`.

### Fixed — full GS/OS 6.0.1 install to a hard disk (3.5" disk-swap detection)
- **The System 6 Installer's disk-swap prompt ("insert SystemTools1") was never detected
  once a *writable* HDD was mounted**, so a from-scratch install to the HDD couldn't
  complete. Root-caused with a temporary SmartPort/ProDOS-block/HDD trace (`SP35LOG`)
  fed by three interactive Installer runs: after a menu hot-swap **and** the dialog's OK
  click, GS/OS made **zero** further slot-5 accesses — it answered the Installer from its
  cached Volume Control Record and re-showed the prompt without ever re-reading the drive.
  GS/OS runs **no periodic disk-insertion poll** on a drive it doesn't consider
  disk-switched-capable, so neither the `$2E` block-READ one-shot nor STATUS bit0 could
  fire (nothing polled/read the drive). **Fix: advertise the 3.5" SmartPort subtype
  `$80`** (`IIgsMemory::smartportStatus`, std + ext DIB). The subtype byte's high bits
  (Firmware Ref fig 7-7; A2 Tech Note "UniDisk 3.5 #2"): **bit7 = supports disk-switched
  errors** ("removable, poll me"), **bit6 = the unintelligent Apple 3.5 Drive that needs
  the host `AppleDisk3.5` driver**. Prior `$00` (UniDisk) lacked bit7 → GS/OS treated the
  drive as fixed and never polled it. `$C0` (Apple 3.5) set bit7 **and** bit6 → GS/OS
  demanded the AppleDisk3.5 low-level driver and crashed (POMIIGS is HLE — WDM-trap
  SmartPort, no real IWM). **`$80` = bit7 only**: disk-switched-capable so GS/OS polls it,
  without the driver-requiring Apple-3.5 bit — intelligent + removable, exactly our HLE
  drive (and the value POM2's `SmartPortCard` uses for its 3.5"). With the drive now
  polled, STATUS bit0 + the `$2E` one-shot invalidate the VCR and re-mount, so the
  Installer walks all 7 disks and **installs GS/OS to the HDD in full**. Pinned by
  `smartport_test` (DIB subtype = `$80`). *Sources: A2 Tech Note UniDisk 3.5 #2; Apple
  IIgs Firmware Reference ch.7 fig 7-7; POM2 `SmartPortCard`.*

### Fixed — GS/OS boots to the Finder with a hard disk mounted (install unblocked)
- **GS/OS + a hard disk had never worked** — mounting any emulated HDD during boot
  crashed (`TODO.md` PRIORITY: derails to `$A500`/`$4200`, or "Unable to load
  START.GS.OS  Error=$0046"). GS/OS now boots all the way to the **Finder desktop**
  with a blank or formatted HDD on slot 7 (screenshot-parity: the SHR menu bar +
  desktop render identically to the no-HDD boot; ~150k–258k toolbox dispatches, no
  derail). Diagnosed end-to-end with the new `tests/hdd_trace` dev tool; pinned by
  `tests/hdd_test`. **Two independent bugs, both cited from the ranked references
  (KEGS `iwm.c`/`clock.c`, MAME `apple2gs.cpp`, GSSquared `pdblock3`):**
- **(1) Boot device stuck on slot 7 → `$0046`.** The ROM boot scan (`$FF/FAB6`,
  `JMP ($0000)` at `$FF/FAD4`) walks slots `$C8`→ and finds the slot-7 HDD first
  (our ProDOS signature `$Cn01/03/05` matches), committing it as the startup device
  ($00/$01 = `$C7`, MSLOT `$07F8` = `$C7`). Our slot-7 firmware correctly chains to
  slot 5 for a non-GS-bootable (blank/install-target) volume, but a bare `JMP $C500`
  left those boot-slot globals reading `$C7`, so the 3.5"-loaded GS/OS bootstrap
  (`$00:21C8`) derived its boot device as slot 7 and looked for `*/SYSTEM/START.GS.OS`
  on the empty HDD → not found. **Fix (`ProDosHdd`):** the chain stub now sets
  `$01`=`$C5` + MSLOT `$07F8`=`$C5` and re-issues the ROM's own `JMP ($0000)`, so
  slot 5 boots exactly as if the scan had picked it. Traced with `hdd_trace`
  (`$43` DEVNUM `$70`→`$50`→`$70`, the last write from the RAM bootstrap).
- **(2) Emulation-mode IRQ vector pulled from LC RAM → `$00:0000`.** With (1) fixed,
  boot reached SHR then derailed to `$0000`. GS/OS drops to **emulation mode** to
  call the ProDOS-8 block driver while enumerating the HDD (`$22D0 XCE` / `JSR
  $C550` = `WDM $C6`) **without masking IRQs**; a VBL IRQ in that window pulled the
  emulation IRQ vector `$00/FFFE` from language-card RAM (`$0000`, never set by
  GS/OS) instead of ROM. **Fix (`CPU65816::serviceInt`):** emulation-mode hardware
  vector pulls now read ROM too (the IIgs VP line forces the fetch to ROM under LC
  RAM, same as the native-mode fix), so `$00/FFFE` → `$C074` → the GS Interrupt Mgr.
  The `vectorPull` test-mode fallback keeps the flat-bus Tom Harte CPU tests on RAM.
- **HDD writes now persist to the backing file** (`ProDosHdd::flushBlock`, on both
  the SmartPort/direct `writeBlock` and the streaming `$C0(8+n)2` byte path), so a
  ProDOS **format / GS/OS install** survives across sessions. `main.cpp` GS/OS mode
  now mounts the configured `hdd =` alongside the installer 3.5" instead of ejecting
  it (a *bootable* HDD is skipped so it can't hijack the GS/OS boot). All 14 gates
  green + new `hdd_test`.

### Fixed — System 6 Installer no longer crashes (extended SmartPort + B accumulator)
- Booting the **Installer disk** (System 6.0.1 "Disk 1 of 7 Install") crashed to the
  ROM monitor (`BRK` at `$00:0001`). Two more HLE bugs, both in the slot-5 SmartPort
  path the Installer drives (diagnosed with `tests/hdd_trace`; independent of the
  HDD — the crash reproduced with no hard disk mounted):
  - **Extended SmartPort inline pointer** (`IIgsMemory::smartportCall`). A GS/OS
    *extended* call (`cmd & $40`) passes its parameter list as a **4-byte,
    bank-qualified** inline pointer (`DFB cmd; DC I4'plist'`), not the 2-byte bank-0
    pointer of a standard call — and the caller resumes **5** bytes after the JSR,
    not 3. The old code read a 2-byte bank-0 pointer (→ a zeroed param list in the
    wrong bank) and skipped only 3 bytes, so the caller ran the pointer's high bytes
    as a stray `COP`. Now the extended form reads the 24-bit pointer, addresses the
    param list in its real bank, and skips 5.
  - **`spReturn` clobbered the B "hidden" accumulator.** The SmartPort/ProDOS error
    is a *byte* in the low half of A; in 8-bit mode the high half (B) must be
    preserved. GS/OS's emulation-mode SmartPort trampoline stashes the caller's
    stack-pointer high byte in B across the call (`TSC` then 8-bit stores) and
    rebuilds a 16-bit SP from it afterward — clearing B gave a page-0 stack and the
    trampoline's closing `RTL` returned to `$0000`. `spReturn` now writes only A's
    low byte when the accumulator is 8-bit.
### Fixed — System 6 Installer runs to its window (SmartPort DIB + unit scan)
- After the crash fix the Installer reached the "Welcome to the IIgs" splash and then
  **stalled** — it enumerates SmartPort devices and never finished. Two more SmartPort
  STATUS bugs (diagnosed with `tests/hdd_trace`; the fix advice was cross-checked
  against **KEGS `smartport.c`**, **MAME / Apple IIgs Firmware Reference ch.7**, and
  **GSSquared `pdblock3`**, which agree on the DIB layout):
  - **DIB device type was $02 (a ProFile hard disk) instead of $01 (3.5" disk).**
    SmartPort TN #4. The Installer keys on this field; a 3.5" reports type **$01**.
  - **STATUS answered every unit number with a valid DIB.** The Installer *scans*
    SmartPort units (1, 2, 3, …) and only stops when one returns an error — since only
    unit 1 exists, a STATUS to unit ≥ 2 must fail (`$28`, no device). Answering all of
    them made the scan loop forever. Also implemented the extended-STATUS **4-byte
    block count** (vs 3-byte standard) so the extended DIB's fields line up (KEGS
    `status_ptr++`); the 3.5" reports type $01 / subtype $A0 (removable, disk-switch).
- **Result:** booting "Disk 1 of 7 Install" now brings up the **"Apple IIGS Installer —
  Easy Update"** window (screenshot-verified) — Easy Update / Change Disk / Customize /
  Quit, with the blank **GSOS** hard disk mounted as an install target.

### Fixed — hard disk was mounted read-only ("Writing to this disk is not allowed")
- The Installer reached its target-select but reported **"Installations … are not
  possible. Writing to this disk is not allowed (it may be locked)."** for the GSOS
  hard disk, and any write raised GS/OS **$4E "Access not allowed"**. Cause: the
  slot-7 ProDOS-block firmware's device-characteristics byte **`$CnFE` was `$03`**
  (ProDOS 8 TRM Table 6-1: bit1 read + bit0 status only) — **no write bit (bit 2)**,
  so GS/OS mounted the volume read-only. Set `$CnFE = $07` (read + write + status).
  The HDD is now a writable install target; block writes persist (`ProDosHdd::flushBlock`).

### Fixed — Installer disk-swap (SmartPort disk-switched $2E + EJECT / CONTROL / FORMAT)
- When the Installer asks for the next disk ("Please insert SystemTools1") and you
  swap via the "3.5\" Drive" menu + OK, GS/OS didn't notice the new disk (it kept the
  old volume) and the prompt wedged. Fixed per **KEGS `smartport.c` `do_c70d`** and
  **MAME / Apple IIgs Firmware Reference** (both consulted, and both agree):
    Two media-change signals are needed (KEGS + MAME), and a menu swap must present
    BOTH:
  - **bit-4 presence pulse (MAME).** GS/OS polls the removable drive's STATUS ~1 Hz and
    auto-mounts on a bit-4 (disk-in-drive) **0→1 transition**. A menu swap is instant —
    the disk is never "absent" — so GS/OS's poll saw no change and never re-mounted. Now
    the **first STATUS after a swap reports bit4=0 (no disk), the next bit4=1**, so the
    poll sees the insert and re-reads the volume. *Verified end-to-end*: after swapping
    the 3.5" to "SystemTools1", GS/OS mounts the SYSTEMTOOLS1 volume within ~1 s.
  - **$2E on the first block READ (KEGS `just_ejected`).** The first block READ/WRITE
    after a swap returns `$2E` (disk switched) and does no I/O, then the latch clears;
    GS/OS drops the cached Volume Control Record and re-reads block 2. **Not on STATUS**
    — the ~1 Hz STATUS poll would consume the one-shot before the actual file access
    (this regressed a first attempt). STATUS carries only the passive bit0 hint.
    Verified swap→STATUS(bit4 pulse)→READ=$2E→retry=new-data (`smartport_test`).
  - **CONTROL / EJECT (`$04`)** ejects the 3.5" (STATUS then reports bit4=0, no disk)
    and arms the latch; other control codes and **FORMAT (`$03`)** succeed. (All were
    the "bad command" `$01` before, which wedged the prompt.)
  - **Unit validation**: any SmartPort call to a unit other than 1 returns `$28`
    (no device) instead of aliasing to the one drive.

### Added — `3.5" Drive` menu: hot-swap install disks without a reset
- New menu bar entry lists every `.2mg`/`.po` image in the boot disk's folder (so all
  seven System 6.0.1 disks are one click away) and swaps the slot-5 3.5" **live** —
  `swapDisk35()` changes the image without a cold reset, and the next SmartPort STATUS
  reports bit 0 = *disk switched* so GS/OS re-reads the new volume. Plus "Insert
  other 3.5" disk…" (hot, no reset) and "Eject". This is the disk-swap the Installer
  needs when it prompts "insert disk X" mid-install. `Ui.*`, `main.cpp`, `IIgsMemory`.
- Note: **installing to a hard disk still doesn't work** — GS/OS crashes when it
  mounts any emulated HDD during boot (see `TODO.md` PRIORITY). This menu is the
  groundwork (disk swapping) for when that blocker is fixed.

### Fixed — real-time clock (RTC/BRAM) now returns host time without wedging boot
- Replaced the clock/BRAM stub (which echoed writes → garbage date) with the real
  Mega II serial protocol at `$C033`/`$C034`. The command byte selects **battery
  RAM** (`(cmd & 0x78) == 0x38` → 1-byte address then data) or the **RTC seconds**
  (`$81/$85/$89/$8D` → registers 0-3); command bit 7 is the read/write direction.
  `rtcByte()` returns live host time in the IIgs epoch (seconds since 1 Jan 1904 =
  host Unix time + 2 082 844 800). Verified against `date`: reads of registers 1-3
  return the exact high bytes of the host clock; the Finder/Control Panel now show
  the correct date. `IIgsMemory.cpp`.
- **Why it kept wedging the boot:** the first attempt matched BRAM with
  `(cmd & 0x7C) == 0x38`, which includes bit 2 — that catches the read command
  `$B8` but *not* the write command `$3F` (both are really `x011 1xxx`, bits 6-3 =
  `0111`). GS/OS reinitialises an invalid (all-zero) battery RAM by *writing* it
  back via `$3F`; the too-strict mask sent those writes down the RTC path, where a
  data byte like `$FF` (bit 7 set) was misread as an RTC *read* and answered with a
  time byte — desyncing the transaction and hanging boot at the "Welcome" splash.
  Widening the mask to `0x78` plus **persisting** BRAM writes lets the reinit stick
  (valid checksum → boot proceeds to the Finder). All 13 gates green.
- **Full command decode + 2-strobe read timing (KEGS `clock.c`).** Two fixes for
  the Control Panel showing `1900`/garbage even though the RTC seconds were correct.
  (1) Decode `op = (cmd>>4)&7`, `reg = (cmd>>2)&3`: `op 0` = seconds, `op 2` = BRAM
  `$10-$13`, `op 4-7` = BRAM `$00-$0F`, `op 3 & reg&2` = extended BRAM (2-byte
  address), `op 3 & reg<2` = internal test/write-protect registers — the `0x78`
  mask had misrouted the single-byte and internal commands to the seconds path.
  (2) **The real bug:** a clock read is *two* strobes — the firmware clocks the
  command byte in ($C034 bit6=0), then clocks the data byte **out on a second
  strobe** (bit6=1). We were answering on the *command* strobe and returning the
  firmware's dummy (`00`) on the read strobe, so GS/OS's `ReadTimeHex` assembled
  `00`s → a bogus date, independent of the real time. Captured the live Control
  Panel read (a `CLKLOG` file trace) to see it, then reworked the state machine to
  set up the target on the command strobe and serve the value (seconds/BRAM/
  internal) on the data strobe, with direction taken from `$C034` bit 6. Boot to
  the Finder intact; all gates green.

### Added — mouse capture (`Del`) — usable GS/OS mouse
- **`Del` captures / releases the mouse.** GS/OS draws its own cursor, so a usable
  pointer needs *relative* capture: on `Del`, GLFW locks + hides the OS cursor
  (`GLFW_CURSOR_DISABLED`) and each frame's raw cursor delta + left button feed
  the ADB mouse (`mouseMove`/`mouseButton`); `ImGuiConfigFlags_NoMouse` keeps
  clicks/hover out of the menu bar while captured. `Del` again releases it back to
  ImGui. Verified the GS cursor tracks host motion (it moves across the Finder
  desktop). A startup hint + a toggle status message point it out. `main.cpp`.

### Fixed — lo-res / text palette (IIgs boot banner was orange, now blue)
- The shared 16-colour lo-res / `$C022` text palette (`kLoresPalette`) was
  scrambled — e.g. colour 6 (the IIgs boot-banner background) rendered **orange**
  instead of **medium blue**, plus wrong greens/yellows/browns. Replaced with the
  canonical IIgs colours (MAME `apple2gs.cpp`). The "Apple IIgs / ROM Version 3"
  boot banner is now white-on-blue as on real hardware; lo-res graphics get
  correct colours too. HGR/DHGR (separate NTSC palette) and SHR unaffected.
  `text80_test`'s palette mirror updated to match; all 13 gates green.

### Added — boot config file (`pomiigs.cfg`) + `--gsos`
- New **`pomiigs.cfg`** (repo root, `key = value`) selects what boots:
  `boot = gsos|finder` → GS/OS to the Finder from a slot-5 3.5" disk (`disk35 =`),
  `boot = hdd` → the slot-7 ProDOS HDD (`hdd =`); also `rom =`. **Shipped default
  is `boot = gsos`**, so a plain launch now lands on the Finder desktop.
- Precedence **CLI > config > built-in**: `--gsos [disk]` (alias `--finder`) and
  `run_gsos.sh` force the Finder; `--hdd` forces the HDD; positional args still
  give ROM then HDD. `main.cpp` only.

### Added — command-key menu shortcuts (⌘-A etc.)
- **GS/OS Finder menu shortcuts now fire** — pressing ⌘-A flashes the Edit menu
  title (the standard menu-shortcut feedback), screenshot-verified across frames.
- The ROM side was already correct: the keyboard handler (`$FE:EC46`) maps
  `$C025` KEYMODREG bits to GS event modifiers through the `$FEE267` table
  (bit7→appleKey `$0100`, bit1→controlKey, bit0→shiftKey, …), so a key posted
  with `$C025` bit7 set carries the command modifier and TaskMaster routes it to
  MenuKey. (The prior "doesn't work" note was a false negative — wrong MenuKey
  tool number + a too-late screenshot that missed the transient flash.)
- The real gap was in the app: **Command/Option/Control suppress ImGui's
  `InputQueueCharacters`**, so the letter of a modifier-combo never reached
  `keyDown`. `main.cpp` now delivers A-Z / 0-9 via `IsKeyPressed` whenever a
  shortcut modifier is held, alongside the `$C025` modifier bits it already sets.

### Added — GS keyboard (interact with the Finder)
- **Typing now reaches the GS/OS Finder** — a keypress selects a desktop icon
  (screenshot-verified: typing highlights the System.Disk icon). GS/OS drives the
  keyboard entirely by interrupt (it never polls `$C000` — 5 reads in 200M
  instrs, all at boot), so a key must both latch the ASCII *and* raise the ADB
  IRQ. Reverse-engineered path: `keyDown` → `IRQ_SRC_ADB` → the ROM interrupt
  manager reads a `$C026` routing byte (b6 = `$40`) + `$C027` b5, dispatches the
  keyboard handler `$FE:EC99`, which reads the ASCII from `$C000` + modifiers
  from `$C025`, clears the `$C010` strobe, and calls **EventMgr PostEvent
  (`$1406`)**. The Finder's GetNextEvent then retrieves it.
- Storm-safe (same native + VBL-int-enabled gate + 2-frame drop as the mouse);
  the `$C010` strobe-clear consumes the event and drops the IRQ. Gate: `adb_test`
  (extended — latch, `$C026`/`$C027` status, IRQ raise/clear via `$C010`).
- **Follow-up:** command-key menu shortcuts. Plain keys work; the command
  (open-apple) *modifier* flows through the ADB µC's GS/OS-installed translation
  table (`$E100E7`), not `$C025`/`$C061` directly, so Command-key combos don't
  trigger MenuKey yet — part of the full ADB µC command model.

### Fixed — the full GS/OS Finder desktop renders (SHR fast-side shadow)
- **GS/OS 6.0.1 now boots all the way to a complete Finder desktop** — the menu
  bar (🍎 File Edit Windows View Disk Special Colors Extras), the mouse cursor,
  the **System.Disk** volume icon, and the Trash all render, and the boot
  "Welcome" window is dismissed.
- Root cause was **not** input (the earlier suspicion): the Finder drew the menu
  bar / disk icon to the **fast-side SHR (bank `$01`, `$2xxx`)** all along, but
  `maybeShadow` never mirrored those writes to `$E1` (what the VGC scans), so the
  display showed a stale image. The SHR region (`$01:2000-9FFF`, gated by
  `SHAD_SUPERHIRES` bit 3) **overlaps** the Hi-Res ranges (`$2000-5FFF`), and the
  Hi-Res `else if` branches matched first — using the Hi-Res shadow bits, which
  GS/OS *sets* (inhibits), while it *clears* the SuperHiRes bit (shadow on). So
  fast-side SHR writes fell through unshadowed. Fixed by OR-ing the SHR gate into
  the overlapping ranges (`!(HIRESPG1) || shr`) and covering `$6000-9FFF`. MAME
  `apple2gs.cpp` shadow_w. All 13 gates green (DHGR/HGR/text shadowing unchanged —
  the SHR term only activates when SuperHiRes shadowing is enabled).

### Added — ADB mouse interrupt (GS/OS Event Manager delivery)
- Modeled the ADB microcontroller **mouse interrupt** so mouse motion reaches
  GS/OS. Reverse-engineered the ROM path: the interrupt manager (`$FF:BE31`,
  reached on every serviced IRQ) reads `$C027` and, when b7 (mouse-data) + b6
  (mouse-int) are set, dispatches the mouse handler (`$E10034`) which calls
  **ReadMouse (`$FE:B1E1`)** — that reads `$C024` (X then Y). Host motion/button
  now raises `IRQ_SRC_ADB`; the ROM services it and reads `$C024` (verified:
  post-boot mouse motion produces `$C024` reads at `$FE:B1EB`/`$FE:B1F8`, where
  before it read `$C024` zero times).
- **Storm-safe.** A naive ADB IRQ wedges the boot — during early emulation-mode
  boot the ADB mouse handler isn't installed, so an unclearable IRQ storms the
  ROM interrupt manager. Delivery is therefore gated on **native mode + VBL
  interrupts enabled** (`$C041` b3 — a proxy for "GS/OS's interrupt system is
  up"), plus a **2-frame safety valve** that drops an unconsumed sample. Result:
  continuous mouse motion *during* boot still reaches the desktop unharmed, and
  post-boot the ROM services the mouse. Gate: `adb_test` (extended — b6/b7
  toggle, IRQ raised only when native+VBL-on, cleared on the `$C024` read).
- **Keyboard is a follow-up.** GS keyboard events flow through the ADB µC's
  `$C026` DATAREG command/response protocol (the interrupt manager reads
  `$C025`/`$C026`, *not* `$C000`), which needs the full ADB µC command model; the
  classic `$C000` latch (8-bit path) is unchanged. Modeling the µC's TALK/LISTEN
  + SRQ + register-0 protocol would also give a hardware-accurate interrupt
  enable (replacing the native+VBL proxy above).
- Note: this does **not** yet complete the Finder desktop — the Finder reaches
  its event loop but the menu bar / disk icon / welcome-close is a separate,
  non-mouse-gated blocker.

### Fixed — GS/OS boots to the Finder desktop (fast-RAM banking)
Two memory-banking bugs kept GS/OS 6.0.1 hung in its loader just after the
welcome screen. Fixing them takes GS/OS from "loads 60 blocks then hangs" all
the way to **the Finder loading the full system (1140+ blocks over the real
SmartPort driver) and rendering the desktop** (background pattern + Trash icon).
- **Full fast side is now backed RAM.** `fastCell` *wrapped* out-of-range banks
  (`idx %= fastRam_.size()`), so with only 1 MB of RAM bank `$10` aliased onto
  bank `$00`. GS/OS's `MVN $10,$00` relocation then copied bank 0 onto itself,
  scrambling the zero page (incl. the COUT vector `$0036`) — and the wrap made
  the RAM probe detect phantom RAM. Fixed by backing the whole fast side
  (`$00-$7F` = 8 MB, the ROM 03 maximum), so every fast bank is real and the
  probe reports the true size.
- **Language-card RAM follows the physical bank.** `lcRead`/`lcWrite` ignored
  the bank and always picked main/aux by ALTZP, so a bank-`$01` LC access
  (`$01:Dxxx`) read *main* LC when ALTZP=0. GS/OS relocates code into bank-1
  (aux) LC and `JSR`s into it; reading the wrong bank landed those calls in a
  data table and derailed to a `BRK` in slot-ROM space (`$01:CFFA`). Now a
  bank-`$01` LC access is always aux (`pb = bank==1 || altzp`), matching the
  rest of the //e aux-memory model.
- Result: GS/OS boots through the loader, switches the SmartPort driver in,
  loads the Finder, and the Finder draws the GS/OS desktop. All 13 gates green;
  Tom Harte unaffected (flat-bus test mode bypasses this banking). Remaining: the
  Finder reaches its main event loop (TaskMaster/GetNextEvent, ~32k idle
  iterations) but the desktop stays incomplete (no menu bar / disk icon, welcome
  window not closed). Diagnosis: GS/OS **never reads the mouse register `$C024`**
  (0 reads in 200M instructions) — it drives the mouse through **interrupt-driven
  ADB** (the µC autopolls and interrupts), which the register-level ADB HLE
  doesn't provide, so the event queue stays empty. Interrupt-driven ADB event
  delivery is the next milestone.

### Added — ADB GLU mouse + keyboard modifiers (real host input)
- **Mouse** now reaches the GS toolbox through the ADB GLU registers. `$C024`
  MOUSEDATA returns the X delta then the Y delta (the `$C027` bit1 X/Y toggle),
  each carrying the button in bit7 (0 = down) and a signed 7-bit delta; the Y
  read consumes the deltas and clears the mouse-data-available bit (`$C027`
  bit7). Host motion/button accumulate via `IIgsMemory::mouseMove/mouseButton`,
  wired from ImGui in `main.cpp` (suppressed while ImGui owns the mouse).
- **Keyboard modifiers**: `$C025` KEYMODREG now reports real shift/control/
  caps/option(⌘)/command state (`setKeyModifiers`, fed from the host each
  frame) instead of always 0.
- **Poll-based, not interrupt-driven** (deliberately): GS/OS's Event Manager
  reads the mouse off the already-firing VBL/heartbeat, so no dedicated ADB IRQ
  is asserted yet — the HLE'd ADB couldn't cleanly clear one, and a spurious
  ADB IRQ would storm the ROM Interrupt Manager and break the boot. The
  interrupt-driven ADB/mouse path is a follow-up (needs a reachable GS UI to
  validate against MAME).
- Gate: **`adb_test`** — the `$C024`/`$C025`/`$C027` register protocol
  (X/Y toggle, button bit, 7-bit clamp, data-available flag, modifier
  pass-through). Source: Apple IIgs Hardware Reference (ADB GLU), MAME
  `apple2gs.cpp` keyglu. GS/OS still boots past the welcome screen unchanged.

### Fixed — GS toolbox runs: IIgs interrupt vectors (`$C071-$C07F` ROM + native vector pull)
- **Root cause of the post-"Welcome" crash.** GS/OS 6.0.1 loads and starts the
  entire ROM toolbox correctly (LLE: the Tool Locator, Memory Mgr, QuickDraw II,
  Event/Window/Menu/Control Mgrs, the Loader — every `_xxStartUp` dispatches from
  ROM). It then enables the **VBL interrupt** (`$C041` bit3, ROM routine
  `$FE/B80F`) and, on the very next hardware IRQ, the machine derailed into a RAM
  data table and sledded to a crash. Two missing pieces of the IIgs interrupt
  path caused it:
  1. **`$C071-$C07F` reads must return internal ROM**, not floating-bus I/O
     (Apple IIgs Hardware Reference: this softswitch sub-range is *reserved,
     reads return ROM*). The 65C816 native/emulation **vector table**
     (`$FFE4-$FFEF` / `$FFF4-$FFFF`) points here — e.g. native IRQ `$FFEE` =
     `$C074`, which in ROM is `CLV : JML $E10010` (the GS Interrupt Manager
     entry). Reading garbage there sent every IRQ into nonsense. Fixed in
     `IIgsMemory::ioRead`.
  2. **Native-mode vector pulls read ROM even with LC RAM banked in.** GS/OS runs
     with the language card in RAM-read mode but never installs RAM interrupt
     vectors (it reaches its handlers through the fixed ROM stubs), so a native
     vector pull of `$00FFEE` must hit ROM, not the uninitialised LC RAM.
     `IIgsMemory::vectorPull()` serves the top-bank ROM byte; `CPU65816`
     interrupt/`BRK`/`COP` dispatch uses it in native mode only — **emulation
     mode keeps //e language-card vector semantics** so 8-bit software (Total
     Replay) can still install RAM IRQ vectors. Flat-bus CPU unit tests fall
     back to the normal bus.
- **Result:** interrupts now dispatch correctly (`$00FFEE → $C074 → $E10010`);
  GS/OS runs stably past the welcome screen into its VBL-timed boot loop instead
  of crashing. Tom Harte 65816 (384k vectors) still green; all 12 subsystem
  gates green. Dev tool `gsos_trace` added (boot an 800K GS/OS disk from slot 5,
  trace toolbox dispatch + SHR timeline + hang/sled detection); `screenshot`
  gained `--disk35`.

### Added — SmartPort extended dispatch (GS/OS disk protocol)
- The slot-5 3.5" drive is now a real **SmartPort** device ($C500 ROM,
  $Cn07=$00 + the extended bit $CnFB). Its dispatch entries are **WDM traps**
  ($Cn50 = ProDOS block via $42-$47, $Cn53 = SmartPort via `JSR` + inline
  cmd/param-list), handled in C++ by `IIgsMemory::smartportTrap()` — the trap
  reads the JSR return address, executes the call, writes results into the
  caller's buffer (any bank), sets A/carry and skips the inline bytes.
- Commands: **STATUS** (+ DIB / status code 3 with name + 3.5" type),
  **READBLOCK**, **WRITEBLOCK**, in both the standard form and the **GS/OS
  extended** long-address form (4-byte buffer pointer + 4-byte block number, so
  GS/OS buffers anywhere in the 16 MB space work). `ProDosHdd` gained
  `readBlock`/`writeBlock` and a `smartport` ROM variant; the CPU's `WDM` ($42)
  opcode now calls the trap.
- **GS/OS System 6.0.1 boots to its "Welcome to the IIgs" screen through the
  real SmartPort path** (faster than the block-device boot). Gates:
  `smartport_test` (JSR-driven READBLOCK + STATUS/DIB), `disk35_test`. The
  slot-7 HDD (Total Replay) keeps the plain block-device ROM — unaffected.
  Full GS/OS boot past the welcome/loading screen still needs the GS toolbox.

### Added — slot-5 3.5" drive (boot 800K disks from the authentic slot)
- **`disk35_`** — a second `ProDosHdd` on **slot 5** (device-select $C0D0-$C0DF,
  slot ROM $C500), the IIgs 3.5" convention. 800K `.po`/`.2mg` images boot from
  slot 5 via the same block-level HLE POM2's SmartPortCard uses. `ProDosHdd`
  gained `eject()`; `IIgsMemory` gained `loadDisk35()`/`ejectHdd()`.
- **UI**: File ▸ Load 3.5" Disk (loads slot 5 and ejects the slot-7 HDD so the
  ROM boots the 3.5"). The load dialog now handles ROM / hard disk / 3.5".
- Gate: `disk35_test` (slot-5 ROM signature + block streaming + status/eject).
- Findings: `Arkanoid II.2mg` runs from slot 5 (SHR toggles, active game loop);
  the `LoGo crack` and GS/OS 6.0.1 stall after their first SHR frame — they need
  the SmartPort *extended* call dispatch (GS/OS's SmartPort driver) and/or
  low-level IWM 3.5". Recorded in TODO P2.

### Added — Super Hi-Res completeness: color-fill + border colour
- **SHR color-fill** (SCB bit5, 320 mode): a pixel index of 0 now repeats the
  previous pixel's colour instead of palette[0] (the hardware "fill" trick for
  cheap horizontal runs), seeded from palette[0] at each line start.
- **Border colour** ($C034 bits 0-3): `IIgsMemory::borderColor()` +
  `VGC::loresColor()` (the 16-colour palette hoisted to a shared static); the UI
  draws the active display inside an authentic border frame in that colour
  (both the palette entry and ImU32 are 0xAABBGGRR, so no conversion).
- Gate: `shr_test` (fill run repeats red vs palette[0] blue; border index/colour).

### Added — P4: master-clock timing + interrupt set
- **Mid-frame speed changes.** The host loop now accounts each frame in
  master-clock ticks (`masterPerFrame()` = 238420 = one Mega II frame); each CPU
  step costs 5 master (fast) or 14 (slow) read from the *live* $C036 bit7, plus
  the slow-side penalty (now returned in master ticks). Speed switches within a
  frame are honoured, not just at frame start.
- **Interrupt set** wired onto the wire-OR CPU lines through the real registers:
  - **¼-second** (INTEN $C041 bit4 / INTFLAG $C046 bit4 / clear $C047) and
    **1-second** (VGCINT $C023 bit2 en / bit6 status / clear $C032) timers,
    driven at 60 Hz by a new `frameTick()` (once per host frame).
  - **Scan-line** (VGCINT bit1 en / bit5 status, fired when SHR is on and an SCB
    line has bit6) — IRQ/status only; the renderer doesn't split a frame yet.
  - **DOC oscillator IRQ**: `Es5503` flags an IRQ-enabled oscillator that
    completes during render(); reading the osc-interrupt register ($E0) via the
    Sound GLU clears it; the MMU mirrors it onto the CPU line.
  - VBL now routes through the shared `updateMega2Irq()` (level tracks
    flag&enable), and $C047 clears both VBL and ¼-second.
  Gate: `irq_test` (assert / mask / clear for all four sources). Total Replay
  still boots to its menu; no spurious-IRQ storm. Remaining (P4 notes): ADB /
  SCC / Mega II mouse IRQs await their devices.

### Added — per-access slow-side timing penalty
- In fast mode (2.8 MHz) every access that lands on the Mega II slow side —
  banks $E0/$E1, the $Cxxx I/O + slot ROM + language card of banks $00/$01, and
  shadowed video writes — is now stretched to the 1.02 MHz clock: 14 master
  ticks instead of the fast side's 5. `read8`/`write8` accrue the +9 master
  extra per slow access (`chargeSlow`), and the main loop drains it via
  `takeSlowPenalty()` (fast-cycle units) into the frame budget, so video/DOC/
  speaker-heavy code correctly throttles toward 1 MHz even in fast mode. In slow
  mode the whole CPU is already 1 MHz, so nothing is charged. `maybeShadow` now
  returns whether it shadowed (a shadowed write hits the slow side). Gate:
  `slowside_test` (exact +9-master accounting; zero in slow mode). Total Replay
  still boots; ~2.6% of the fast-mode boot/menu budget is slow-side stretch.

### Fixed — //e speed: honour the SPEED register ($C036)
- Legacy //e software ran ~2.7× too fast because the main loop burned a fixed
  46000 CPU cycles/frame (≈2.8 MHz) regardless of the SPEED register, which was
  stored but never consulted. `IIgsMemory::frameCycleBudget()` now derives the
  per-frame budget from $C036 bit7: **47684 cyc/frame fast (2.8 MHz)** vs
  **17030 slow (1.02 MHz = 262 lines × 65 cycles)**, an exact 14/5 ratio. main's
  loop uses it. Measured on Total Replay: the register is actively toggled
  (fast for the GS menu, slow for //e games); steady-state gameplay now runs at
  **1.022 MHz** instead of 2.76 MHz. Gate: `speed_test`. (Mid-frame speed
  changes + the per-access slow-side penalty remain TODO — see P4.)

### Added — 80-column text + $C022 text colour
- **80-column text** (`VGC::renderText80`) — each 40-byte row is split across
  the banks: aux ($E1) byte at offset k is the even screen column 2k, main
  ($E0) is the odd column 2k+1 (the //e interleave, reusing the aux/main model
  from DHGR). 8-px cells (7-px glyph + gap) → 80×8 = 640. Dispatched when
  `text80()` (80COL soft switch) is set.
- **$C022 SCREENCOLOR** — text foreground = high nibble, background = low
  nibble, both indexing the 16-colour lo-res palette (hoisted to a shared
  `kLoresPalette`). Applies to 40- and 80-col; default $F0 = white on black.
  Replaces the previously hard-coded white/black.
- Gate: `text80_test` (synthetic font, verifies aux=even / main=odd interleave
  and fg/bg colour); visually confirmed with the real 344s0047 font.

### Added — quality menu UI (inspired by POM1)
- **`src/Ui.{h,cpp}`** — the desktop chrome extracted from `main.cpp` into one
  module (mirrors POM1's `MainWindow_Menu` separation): a top main-menu bar
  (**File** Load ROM·Disk / Quit · **Machine** Run·Pause / Reset · **Video**
  HGR·DHGR colour + scale · **Audio** mute + volume · **Help** About), the
  screen window, a bottom **status bar** (run state, CPU PC/mode, shadow/speed
  regs, audio level, ROM name), and modal Load/About dialogs. Shortcuts F6
  (run/pause), F5 (reset), F2 (colour), Ctrl+Q — F-keys chosen so they never
  collide with the Apple II `$C000` keyboard. Runtime ROM/disk loading via
  injected callbacks (file I/O stays in `main`). Apple II key input is gated on
  `io.WantTextInput`/`WantCaptureKeyboard` so UI typing never leaks to `$C000`.

### Added — sound + Double Hi-Res (audio out, DHGR titles)
- **Audio → miniaudio** (`src/Audio.{h,cpp}`, `AudioOut`): mono float32
  playback device fed by a lock-free SPSC ring. Once per emulated frame
  `mixFrame()` reconstructs the **1-bit speaker** ($C030) square wave from the
  MMU's toggle cycle-stamps (cycle-exact — 8-bit games play at correct pitch)
  and mixes the **Ensoniq 5503 DOC**. WASM ships a silent stub (Web Audio
  backend needs audio-worklet link flags). Gate: speaker reconstruction
  verified headlessly (20 toggles → 19 zero-crossings).
- **Double Hi-Res** (DHGR, 140×192, 16 colour) — reuses the HGR artifact
  decoder: 80-column mode lays aux (leftmost 7 dots) then main (next 7) per
  column → the same 14-bit-word windowed LUT. Both HgrMode paths: Composite
  NTSC (`decodeDhgrLine`) and Clean RGB (`decodeDhgrRgbLine`, 140 fat pixels →
  lo-res palette). Gate: `dhgr_test`.
- **Fix — Total Replay DHGR title fades.** The video scanner now honours the
  **80STORE quirk** (Sather *Understanding the Apple IIe* §5-25 table 5.10 /
  POM2 `videoHgrPage2`): when 80STORE+HIRES are on, PAGE2 is the aux-bank
  select, **not** a display page flip — the scanner stays on page 1. Total
  Replay's DHGR fades toggle PAGE2 to interleave aux/main writes; treating that
  as a page-2 flip showed page-2 garbage mid-fade. `IIgsMemory::hgrPage2()` /
  `textPage2()` encapsulate the rule; all four VGC renderers use them. Gate:
  `dhgr_page_test`.

### Added — input + colour HGR (navigate Total Replay)
- **Host keyboard → $C000** (Mega II latch, bit7 strobe; $C010 clears it) and
  **joystick → paddles $C064-$C067 (RC-timed via $C070) + buttons $C061-$C063**.
  The app (main.cpp) feeds ImGui typed chars + special keys (arrows/Return/Esc,
  Apple II codes) and GLFW joystick axes/buttons each frame, before the frame's
  emulation runs. **Total Replay is now navigable** — typing "K" jumps to
  Kaboom!, etc.
- **Colour HGR** with two selectable modes (radio buttons + F2 toggle):
  * **Composite NTSC** — artifact-colour decode ported from POM2's
    Apple2VideoDecode.h (MAME apple2video.cpp / OpenEmulator lineage):
    bit-doubled 14-bit words, a 7-bit sliding window into the artifact LUT with
    4-phase rotation, 16-colour palette, 560→280 pair-average downsample.
  * **Clean RGB** — sharp 6-colour pair decode (the IIgs VGC's native RGB
    output; Le Chat Mauve did the same on //c/e, which lacked built-in RGB).
  `src/VGCNtsc.h` holds the NTSC primitives.
- **40-column text is now white** (colour/RGB-monitor default), not green
  phosphor.

### Added — ProDOS hard disk: boots Total Replay
- `src/ProDosHdd.{h,cpp}` — a synthetic ProDOS block device (slot 7) for
  .hdv/.po/.2mg images. Slot ROM at $C700 advertises the ProDOS signature
  ($Cn01=$20/$Cn03=$00/$Cn05=$03) with a block driver at $Cn50; block data
  streams through the slot device-select window $C0F0-$C0FF (block# lo/hi,
  data byte, status, block count). Firmware ported from POM2's
  ProDOSHardDiskCard (AppleWin lineage).
- MMU: split $C000-$C0FF (registers) from $C100-$CFFF (slot ROM); route the
  slot-7 ROM + device-select to the card. `IIgsMemory::loadHdd()` mounts an
  image; the app auto-loads `hdv/*.hdv` (or argv[2]) onto slot 7.
- **The IIgs now finds the slot-7 device, boots block 0 -> ProDOS ->
  LAUNCHER.SYSTEM, and Total Replay v6.0 renders its hi-res title screen**
  ("TOTAL REPLAY", 518 games, "Type to search") -- waiting for keyboard input.
  Booting without a disk still reaches "Check startup device". Disk images are
  git-ignored (bundled games are copyrighted).

### Fixed — audit / bug-fix pass
- **CPU: emulation-mode direct-page indexed wrap.** `ea_dpx`/`ea_dpy` now wrap
  within page 0 when E=1 and DL=0 (the 6502 quirk), matching `ea_indx`. Was a
  latent state bug on untested `dp,X`/`dp,Y` opcodes.
- **CPU: missing internal index cycle on `dp,X`/`dp,Y` (and RMW `dp,X`).** These
  opcodes were a cycle short (never covered by the original Harte set). Added
  the index-add cycle; the cycle table is now additive so RMW `dp,X` correctly
  gets modify+index. Verified 116800/116800 across 146 opcode files (73×2).
- **CPU: no interrupt servicing.** Added IRQ/NMI polling + vectoring (native
  $FFEE/$FFEA, emulation $FFFE/$FFFA; B clear to distinguish from BRK), and
  wired the **VBL interrupt** ($C041 INTEN bit3 → $C046/$C047) from the MMU to
  the CPU IRQ line. Boot to "Check startup device" is unaffected.
- **DOC: oscillator-count mask.** `oscEnabled()` now masks `$E1` bits 1-5
  (`& 0x1F`); previously a large $E1 over-counted (loop-guarded, no OOB).
- **IWM: data-latch read now advances the nibble cursor.** A tight CPU read
  loop (<32 cycles/read) was returning the same nibble forever, which would
  hang a real disk read. One nibble per data read now guarantees progress.
- **App: Emscripten object lifetime.** `mem`/`cpu`/`vgc` are `static` so the
  `Ctx` references stay valid under `emscripten_set_main_loop_arg` (avoids a
  potential use-after-return on the web build).
- Verified NOT bugs (audit false positives, left unchanged): the 640-mode
  palette groups `{8,12,0,4}` (the documented Apple mapping), the IWM half-track
  stepper, `maybeShadow` using the physical bank, and the DOC end-of-wave test.

### Added — Milestone 8 (WebAssembly build)
- The whole emulator builds to WebAssembly via Emscripten: `./build_wasm.sh`
  produces `build_wasm/POMIIGS.{html,js,wasm}` (~510 KB wasm). `main.cpp`'s
  render loop is refactored into a `frame()` callback driven by
  `emscripten_set_main_loop_arg` in the browser (blocking `while` on native).
  Single-threaded, static-host-friendly (POM2 model). CMake emits the HTML
  shell under Emscripten. Snapshot/rewind, CLI, and desktop packaging remain.

### Added — Milestone 7 (SCC 8530 serial)
- `src/Scc8530.{h,cpp}` — the two-port Zilog 8530 SCC at $C038-$C03B: the
  register-pointer protocol (WR0 low nibble selects the next register, then
  auto-resets), TX/RX FIFOs, RR0 status (Rx-available / Tx-empty), and WR14
  local loopback. Wired into the MMU; host hooks for a real port bridge later.
  Gate `scc_test`: enable loopback, transmit "IIgs", receive it back + host Tx
  drains 4 bytes. Slot bus / SmartPort / Mockingboard (POM2 reuse) = follow-up.

### Added — Milestone 6 (Ensoniq 5503 DOC sound)
- `src/Es5503.{h,cpp}` — the 32-oscillator DOC: 64 KB sound RAM, the full
  oscillator register set (freq/vol/wavetable-pointer/control/size), the Sound
  GLU at $C03C-$C03F (ctl/data/addr with auto-increment, RAM vs DOC-register
  select), free-run wavetable playback with the zero-byte end-of-wave marker,
  and a mixing render(). Wired into the MMU. Gate `doc_test`: a sine wavetable
  on one oscillator produces a clean tone (rms 1429, 31 zero-crossings).
  miniaudio output + DOC oscillator IRQ = follow-up.

### Added — Milestone 5 (IWM disk + //e video — ROM boots to "Check startup device")
- **The ROM 03 now completes the full boot sequence to the authentic "Check
  startup device!" screen** (banner -> self-tests -> disk search -> no-disk
  prompt), exactly what a real IIgs shows with no disk. 4190 distinct PCs.
- `src/Iwm.{h,cpp}` -- IWM Disk II 5.25" controller at $C0E0-$C0EF (slot 6):
  phase stepper, motor, drive select, Q6/Q7 latches, the MODE register (the
  write/verify at $FF:4724 that had blocked boot), status + write-protect
  sense. A 143360-B .dsk/.do/.po is nibblised (6-and-2 GCR, DOS 3.3/ProDOS
  interleave) into per-track streams; .nib passes through. Nibbles advance with
  CPU cycles. loadDisk525() mounts an image.
- //e legacy video (M5 pt.1): HGR 280x192 + LORES 40x48 in the VGC, display
  soft switches $C050-$C05F in the MMU.
- Diagnosed each blocker via boot_trace + disassembly (IWM mode register,
  write-protect sense loop at $FF:5829).

### Added — Milestone 4 (ADB + MMU fixes — ROM boots to the banner)
- **ROM 03 now boots through every self-test to the authentic "Apple IIgs /
  Copyright Apple Computer, Inc. 1977-1989 / ROM Version 3" banner**, then
  proceeds to disk-boot ($C0Ex IWM reads — the M5 boundary). Distinct PCs
  executed jumped 832 → 3451.
- **ADB GLU HLE** ($C024-$C027 in IIgsMemory): accept commands immediately,
  queue a trivial data-ready response — clears the ROM's ADB self-test **fatal
  error $0911** ($FF:81B6). Diagnosed by disassembling the ADB poll loop.
- **STATEREG ($C068) read now synthesizes** from the live switches instead of
  returning the last-written byte (MAME apple2gs.cpp:1926). This was the key
  bug: the ROM saves/restores the MMU state via STATEREG, and a stale read
  corrupted the language-card state, sending it into empty LC RAM ($00:F8B0).
- **//e main/aux redirection** (`physBank01`): bank $00 accesses route to aux
  ($01) under ALTZP (ZP/stack), RAMRD/RAMWRT, and 80STORE/PAGE2 — the ROM runs
  its stack in aux. Language card selects main/aux by ALTZP.
- Corrected the language-card $C08x decode (read RAM when bit0==bit1).
- Battery-RAM/clock ($C033/$C034) stubbed (full serial protocol = follow-up).

### Added — Milestone 3 (VGC video — Super Hi-Res renders)
- `src/VGC.{h,cpp}` — the Video Graphics Controller: renders `IIgsMemory`'s
  slow-side video RAM to a 640×400 RGBA framebuffer. **Super Hi-Res** 320 and
  640 modes (SCB + 4-4-4 palettes from $E1:9D00/$9E00), verified by `vgc_test`
  (M3 gate: 16-colour bars, both modes) and a rendered PNG. **40-column text**
  from the authentic **Apple IIgs Mega II character ROM** (`roms/iigs-char.rom`
  = MAME `344s0047.bin`, 16 KB, SHA1 5a5a77c8…) — user-provided like the main
  ROM; **no public font is bundled** (a placeholder font8x8 was removed).
- `src/main.cpp` now runs the emulator (ROM + MMU + CPU) and displays the VGC
  framebuffer live via a GL texture, with a status/Run/Reset panel.
- `tests/screenshot.cpp` — headless PNG of a booting ROM (dev tool).
- The authentic char ROM (`344s0047.bin`, SHA1 5a5a77c8…) drops into
  `roms/iigs-char.rom` — the ROM's **"Check startup device!" text renders
  crisply in the genuine IIgs font** (verified). Sourced from a MAME apple2gs
  BIOS set (`bios_apple2gs/`, git-ignored).

### Added — Milestone 2 (FPI + Mega II MMU, boots a real ROM)
- `src/IIgsMemory.{h,cpp}` grown from the flat M1 stub into the real MMU: ROM
  mapping ($FC-$FF / $FE-$FF), fast RAM $00-$7F, Mega II slow RAM $E0/$E1, the
  $C0xx register file (keyboard, //e paging switches, NEWVIDEO $C029, SHADOW
  $C035, SPEED $C036, STATEREG $C068, language card $C08x), the language card
  ($D000-$FFFF with ROM show-through when !lcRamRead — so reset vectors read
  ROM), and shadow write-through of the display regions. Register semantics
  cited to MAME apple2gs.cpp.
- `tests/boot_trace.cpp` — dev tool that loads a real ROM, resets, and traces
  CPU execution / loop-detection.
- **A real ROM 01 and ROM 03 both boot**: reset from the ROM vector at
  $00:FA62, `REP #$30` into native mode, run 340-420 distinct ROM addresses of
  self-diagnostic, then reach the CPU speed-calibration loop at $FF:FCDC (needs
  a VBL/timer reference — M3+ — to converge). Fetched via scrapling from the
  Apple II Documentation Project / asimov mirrors; git-ignored (Apple
  copyright). Doc note: ROM 01 = 128 KB, ROM 03 = 256 KB (earlier docs had the
  sizes swapped).

### Added — Milestone 1 (65C816 core, in progress)
- `src/CPU65816.cpp` — the WDC 65C816 core: emulation + native mode, 8/16-bit
  register widths (M/X flags), 24-bit banked addressing, the full addressing-
  mode set (dp/abs/long/indexed/indirect/stack-relative/block-move), mode
  switches (XCE/REP/SEP), interrupts (BRK/COP/RTI), and a cycle model
  (bus + internal cycles).
- `src/IIgsMemory.h` — flat 16 MB bus (M1 stub; M2 adds FPI/Mega II). Stable
  `read8/write8(addr24)` interface.
- `tests/tomharte_65816.cpp` + `tests/fetch_tomharte_65816.sh` — the CPU gate
  (SingleStepTests/65816), wired into CTest.
- **Validated 100% (regs + RAM + cycle count) across 64 opcode families in
  both emulation and native mode — 384 000 vectors.** Fixes surfaced by the
  vectors: emulation-mode SPH=$01 / 8-bit-index high-byte invariants; RMW,
  (dp,X)/(dp),Y indexed, stack-relative, and control-flow internal + page-cross
  cycles; bit-accurate BCD (−6 / bit-4 borrow) for 8/16-bit ADC & SBC decimal;
  emulation-mode (dp,X) pointer page-0 wrap; native BRK pushes P without B;
  the new-instruction 16-bit emulation stack (PEA/PEI/PER/PHD/PLD/JSL/RTL leave
  page 1, SPH reset at end of step); PEI full-16-bit DP-pointer read.
- **MVN/MVP** kept per-byte (correct for the emulator loop) but **excluded from
  the Tom Harte gate** — the vectors cap block moves at 100 cycles (partial
  execution), incompatible with an instruction-stepped core. See DEV.md § CPU.

### Added
- Project scaffold: git repo, directory layout (`src/`, `tests/`, `docs/`,
  `roms/`, `wasm/`), GPLv3 `LICENSE`, `.gitignore`.
- Build system `CMakeLists.txt` mirroring POM2 — native (GLFW+OpenGL+ImGui),
  Emscripten/WASM (single-threaded), and headless targets; `Version.h.in`
  single-source-of-truth version string.
- Doc suite following POM2's discipline: `CLAUDE.md` (always-loaded index +
  source-of-truth ranking + IIgs subsystem map + 24-bit memory map),
  `DEV.md` (per-subsystem deep-dive skeleton), `TODO.md` (milestone roadmap +
  MAME↔POMIIGS parity dashboard), this file.
- `src/CPU65816.h` — 65C816 core interface (emulation + native mode), designed
  to mirror POM2's `M6502` API so `EmulationController` forks cleanly.

### Decisions (the why)
- **Source of truth = MAME `apple2gs.cpp`**, ranked above GSSquared / KEGS /
  Clemens / Crossrunner. Rationale: it is C++, cycle-accurate, continuously
  maintained, cites the Apple II Documentation Project, and — critically —
  keeps POM2's existing "MAME = truth, cite file+line, pin a test" convention
  intact, so the two codebases share one verification culture.
- **One CPU, not two.** The 65C816 in emulation mode (E=1) *is* a 65C02
  superset, so POMIIGS drops POM2's separate `M6502` rather than running two
  cores. Simpler bus, one snapshot format, one test gate.
- **Reuse over rewrite.** Shared IIgs/Apple-II hardware (IWM, disk formats,
  legacy //e video, NTSC/CRT stack, audio bus, slot bus, snapshot/rewind) is
  ported/linked from POM2 rather than reimplemented; only the genuinely
  IIgs-specific chips (65C816, FPI/Mega II MMU, VGC, ES5503, ADB, RTC, SWIM,
  SCC) are new. Recorded per-subsystem in `CLAUDE.md`'s map (`[reuse: POM2]`).
- **Clock-agnostic opcode table.** The CPU returns *architectural* cycle counts;
  the 2.8/1.02 MHz effective clock and slow-side access penalty are applied by
  `IIgsMemory`, not the opcode table — same separation POM2 uses, so Tom Harte
  cycle-count vectors stay valid.
- ROMs are **user-provided** and git-ignored (IIgs ROM is copyrighted).
