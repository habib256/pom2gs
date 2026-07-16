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

### What it boots — `pomiigs.cfg`

Startup reads **`pomiigs.cfg`** (repo root). The shipped default boots **GS/OS to
the Finder desktop** from a slot-5 3.5" disk:

```ini
boot   = gsos                                       # gsos|finder → Finder; hdd → slot-7 HDD
disk35 = disks35/System 6.0.1/Disk 2 of 7 System Disk.2mg
hdd    = hdv/Total Replay v6.0.hdv
```

Edit it, or override per-run on the CLI (CLI > config > built-in):

```bash
./run_emulator.sh              # uses pomiigs.cfg → Finder
./run_emulator.sh --hdd        # force the slot-7 ProDOS HDD (e.g. Total Replay)
./run_gsos.sh ["disk.2mg"]     # force the Finder (alias: --finder [disk])
```

Disk images are copyrighted and not bundled — place your own GS/OS system disk
under `disks35/` and hard disks under `hdv/`.

### ROMs (user-provided — none bundled)

The IIgs ROM is copyrighted; drop your own dump in `roms/`:

- `roms/iigs-rom03.rom` — 256 KB (Apple IIgs ROM 03, 1989) — default
- `roms/iigs-rom01.rom` — 128 KB (Apple IIgs ROM 01, 1986) — best compatibility
- `roms/iigs-char.rom` — 16 KB (Mega II character generator, MAME `344s0047.bin`,
  SHA1 `5a5a77c8ec45632aea1a57cd9c9257f7f6e44668`) — required for text rendering.
  Super Hi-Res needs no font.

## Controls

A Dear ImGui **menu bar** drives the machine: **File** (Load ROM… / Load Hard
Disk… / Quit), **Machine** (Run·Pause / Reset), **Video** (HGR·DHGR colour:
Composite NTSC ↔ Clean RGB, and window scale), **Audio** (mute + volume), and
**Help** (About). A bottom **status bar** shows run state, the CPU PC/mode,
the shadow/speed registers, audio level, and the loaded ROM. Shortcuts (chosen
to avoid the Apple II keyboard):

| Key | Action |
|-----|--------|
| `Del` | **Capture / release the mouse** (drives the GS/OS cursor) |
| `F6` | Run / Pause |
| `F5` | Reset |
| `F2` | Toggle HGR/DHGR colour mode (Composite NTSC ↔ Clean RGB) |
| `Ctrl+Q` | Quit |

**Mouse:** press `Del` to capture — the OS pointer is hidden/locked and its
relative motion + left button drive the GS/OS mouse (needed since GS/OS draws its
own cursor). `Del` again releases it back to the ImGui menu bar. The host
keyboard feeds the ADB keyboard (typing, arrows, Return/Esc, and ⌘/option menu
shortcuts via Left/Right-Alt); a game controller drives the paddles + buttons.

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
