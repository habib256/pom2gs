# CHANGELOG

Resolved items + the **why** behind non-obvious decisions.

## [Unreleased] — Milestone 0: foundation

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
