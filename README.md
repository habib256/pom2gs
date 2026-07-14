<div align="center">

# 🍏 POMIIGS — Apple IIgs Emulator

### *The 16-bit sibling of [POM2](https://github.com/habib256/POM2). A cycle-faithful Apple IIgs, MAME-true, in C++.*

Built with Dear ImGui & OpenGL — native (Linux · macOS · Windows) and WebAssembly.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-orange.svg)](#)
[![Status](https://img.shields.io/badge/status-foundation%20(M0)-yellow.svg)](TODO.md)

</div>

---

## What this is

POMIIGS emulates the **Apple IIgs** (ROM 01 / ROM 03): the WDC **65C816** CPU,
the **Mega II** and **FPI** custom chips, the **VGC** Super Hi-Res video, the
**Ensoniq 5503 DOC** 32-oscillator sound chip, **ADB** keyboard/mouse, the
battery-backed clock, and **IWM/SWIM** disk — while inheriting POM2's Apple II
legacy modes, disk stack, CRT/NTSC rendering, snapshot/rewind, and slot bus.

It reuses POM2's architecture wholesale (see `CLAUDE.md`): C++17, Dear ImGui +
OpenGL, cycle-stamped events, one-subsystem-per-file, and **MAME as the source
of truth** — every hardware port cites `apple2gs.cpp` and is pinned by a test.

> **Status: Milestone 0 (foundation).** The project scaffold, build system, and
> subsystem plan are in place. The 65C816 core is next. See [`TODO.md`](TODO.md)
> for the roadmap and the MAME↔POMIIGS parity dashboard.

## Reference set (source of truth)

| Reference | Role |
|---|---|
| [MAME `apple2gs.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/apple/apple2gs.cpp) | **Primary** — cycle-accurate, cited in every port |
| [GSSquared](https://github.com/jawaidbazyar2/gssquared) | Modern C++/SDL3 peer — subsystem decomposition |
| [KEGS](https://github.com/a2kegs/kegs) | Readable canonical C implementation |
| [Clemens IIGS](https://github.com/samkusin/clemens_iigs) | MIT-licensed structural reference |
| [Crossrunner](https://www.crossrunner.gs/) | Closed-source behavioural oracle |

## Quick Start

```bash
git clone <this repo> && cd POMIIGS
./setup_imgui.sh                    # fetch Dear ImGui + deps (one-time)
cd build && cmake .. && make -j
cd .. && ./run_emulator.sh          # cwd = repo root so roms/ probes resolve
```

### ROMs (user-provided — none bundled)

The IIgs ROM is copyrighted; drop your own dump in `roms/`:

- `roms/iigs-rom03.rom` — 256 KB (Apple IIgs ROM 03, 1989) — default
- `roms/iigs-rom01.rom` — 128 KB (Apple IIgs ROM 01, 1986) — best compatibility
- `roms/iigs-char.rom` — 16 KB (Mega II character generator, MAME `344s0047.bin`,
  SHA1 `5a5a77c8ec45632aea1a57cd9c9257f7f6e44668`) — required for text rendering.
  Super Hi-Res needs no font.

## WebAssembly

```bash
source /path/to/emsdk/emsdk_env.sh   # activate Emscripten
./build_wasm.sh                     # → build_wasm/POMIIGS.html
cd build_wasm && python3 -m http.server 8000   # open /POMIIGS.html
```

Single-threaded; serves on any static host. ROMs are uploaded at runtime
(not bundled — copyright).

## License

GPLv3. See [LICENSE](LICENSE). POMIIGS shares POM2's lineage and reuses code
from POM2 (GPL); its hardware behaviour is pinned to MAME (GPL).
