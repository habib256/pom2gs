<div align="center">

# 🍏 POMIIGS — Apple IIgs Emulator

### *The 16-bit sibling of [POM2](https://github.com/habib256/POM2). A cycle-faithful Apple IIgs, MAME-true, in C++.*

Built with Dear ImGui & OpenGL — native (Linux · macOS · Windows) and WebAssembly.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-orange.svg)](#)
[![Status](https://img.shields.io/badge/status-boots%20GS%2FOS%20%26%20games-brightgreen.svg)](TODO.md)

</div>

---

## What this is

POMIIGS emulates the **Apple IIgs** (ROM 01 / ROM 03): the WDC **65C816** CPU,
the **Mega II** and **FPI** custom chips, the **VGC** Super Hi-Res video, the
**Ensoniq 5503 DOC** 32-oscillator sound chip, **ADB** keyboard/mouse, the
battery-backed clock, **IWM** 5.25" disk, and a **SmartPort** hard-disk stack —
with POM2's Apple II legacy video modes and NTSC/RGB rendering ported in.

It follows POM2's conventions (see `CLAUDE.md`): C++17, Dear ImGui + OpenGL,
cycle-stamped events, one-subsystem-per-file, and **MAME as the source of truth**
— every hardware port cites `apple2gs.cpp` / KEGS / GSSquared and is pinned by a test.

> **Status:** broad KEGS/MAME/GSSquared parity. Working 65C816 (Tom Harte-
> validated), FPI/Mega II MMU, VGC Super Hi-Res + legacy //e video, Ensoniq DOC
> audio, ADB keyboard/mouse, IWM + SmartPort disk, SCC. **GS/OS 6.0.1 installs
> and boots from a ProDOS hard disk**; games run (Total Replay, Arkanoid, Battle
> Chess); save states work (F7/F8). See [`TODO.md`](TODO.md) for the roadmap and
> the MAME↔POMIIGS parity dashboard.

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

Startup reads **`pomiigs.cfg`** (repo root). The shipped default (`boot = hdd`)
boots **GS/OS 6.0.1 straight from the slot-7 ProDOS hard disk** (a full install
in `hdv/GSOS.hdv`), with the configured 3.5" disk mounted alongside:

```ini
boot   = hdd                                        # hdd → slot-7 HDD; gsos|finder → Finder from 3.5"
disk35 = disks35/System 6.0.1/Disk 6 of 7 synthLAB.2mg
hdd    = hdv/GSOS.hdv
#iwm35 = 1     # 3.5" on the REAL IWM/Sony drive (genuine slot-5 ROM firmware,
               # realistic seek/read speed) instead of the fast SmartPort HLE
#disk35b = …   # second internal 3.5" drive (needs iwm35 = 1)
```

Edit it, or override per-run on the CLI (CLI > config > built-in):

```bash
./run_emulator.sh              # uses pomiigs.cfg (default: GS/OS from the HDD)
./run_emulator.sh --hdd        # force the slot-7 ProDOS HDD (e.g. Total Replay)
./run_gsos.sh ["disk.2mg"]     # force the Finder from a 3.5" disk (--gsos / --finder [disk])
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
Disk… / Load 3.5" Disk… / Quit), a **3.5" Drive** quick-swap menu (insert/eject
disks mid-run without a reset, so the Installer can prompt for the next disk),
**Machine** (Run·Pause / Reset / Save State / Load State), **Video** (HGR·DHGR
colour: Composite NTSC ↔ Clean RGB, and window scale), **Audio** (mute + volume),
and **Help** (About). A bottom **status bar** shows run state, the CPU PBR:PC +
mode, the shadow/speed registers, audio level, and the loaded ROM. Shortcuts
(chosen to avoid the Apple II keyboard):

| Key | Action |
|-----|--------|
| `Del` | **Capture / release the mouse** (drives the GS/OS cursor) |
| `F6` | Run / Pause |
| `F5` | Reset |
| `F2` | Toggle HGR/DHGR colour mode (Composite NTSC ↔ Clean RGB) |
| `F7` | Save state → `states/quick.pgss` |
| `F8` | Load state ← `states/quick.pgss` |
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
