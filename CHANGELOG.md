# CHANGELOG

Resolved items + the **why** behind non-obvious decisions.

## [Unreleased] — Milestone 0: foundation

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

## [Unreleased] — Milestone 0: foundation

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

## [Unreleased] — Milestone 0: foundation

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

## [Unreleased] — Milestone 0: foundation

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
