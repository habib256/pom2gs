# CHANGELOG

Resolved items + the **why** behind non-obvious decisions.

## [Unreleased] — Milestone 0: foundation

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
