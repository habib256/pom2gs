# TODO.md

Active backlog + **MAME↔POMIIGS parity dashboard** + milestone roadmap.
Legend: 🔴 not started · 🟡 in progress · 🟢 done + pinned test.

## 🟡 PRIORITY — GS/OS + hard disk (install to HDD)

**Goal:** boot the System 6 install disk (3.5", slot 5) with a hard disk present,
format it, and install GS/OS onto it — then boot GS/OS from the HDD.

**✅ FULL INSTALL WORKS (July 2026):** the System 6 Installer walks all 7 disks and
installs GS/OS to a blank HDD **in full**. Last blocker was **3.5" disk-swap detection
with a writable HDD mounted** — GS/OS answered the Installer from its cached VCR and
never re-read the drive after a menu swap+OK. Fix: **3.5" SmartPort DIB subtype `$80`**
(`smartportStatus`) — bit7 = disk-switched-capable ("poll me") WITHOUT bit6 (the
Apple-3.5 driver bit that `$C0` set → "AppleDisk3.5 requires a driver" crash under our
HLE). `$00` UniDisk lacked bit7 so GS/OS never polled. Now GS/OS polls STATUS, sees
bit0, hits `$2E`, re-mounts. Pinned by `smartport_test` (subtype `$80`); root-caused
with a temporary `SP35LOG` SmartPort trace (since removed). See CHANGELOG.

**✅ Unblocked (July 2026):** GS/OS now boots **all the way to the Finder desktop
with a hard disk mounted** on slot 7 (blank or formatted) — the first time GS/OS +
a hard disk has worked in POMIIGS. Screenshot-parity with the no-HDD boot (SHR menu
bar + desktop drawn, ~150k–258k toolbox dispatches, no derail). Two bugs fixed (see
CHANGELOG "GS/OS + hard disk" for the full why), both cited from KEGS / MAME /
GSSquared `pdblock3`:
1. **Boot-device stuck on slot 7** (`START.GS.OS  Error=$0046`) — the ROM scan
   commits slot 7 (first ProDOS signature found); our slot-7 chain now re-points
   the boot-slot globals (`$00/$01`, MSLOT `$07F8`) to `$C5` and re-issues the ROM's
   `JMP ($0000)` so slot 5 boots. (`ProDosHdd`.)
2. **Emulation-mode IRQ vector from LC RAM → `$00:0000`** — GS/OS calls the ProDOS-8
   block driver in emulation mode without masking IRQs; the emul vector `$00/FFFE`
   must read ROM (VP line), not uninitialised LC RAM. (`CPU65816::serviceInt`.)
   HDD writes now persist to the image (`ProDosHdd::flushBlock`), so format/install
   sticks. Pinned by `tests/hdd_test`; diagnosed with the new `tests/hdd_trace`.

**✅ Installer runs (July 2026):** "Disk 1 of 7 Install" now boots to the **Apple IIGS
Installer — Easy Update** window (screenshot-verified) with the blank GSOS HDD mounted.
Fixes (cross-checked vs KEGS/MAME/GSSquared — see CHANGELOG): SmartPort DIB **device
type $01** (3.5", not $02); STATUS **errors ($28) for unit > 1** so the Installer's
device scan terminates; extended-STATUS **4-byte block count**; extended SmartPort
**4-byte inline param pointer**; `spReturn` **preserves the B accumulator**.

**Round-trip install → boot-from-HDD: DONE.**
1. ✅ **Drive the install in the UI** — Easy Update installs the System Folder to
   `hdv/GSOS.hdv` (writes persist) across all 7 disks, hot-swapped via the "3.5\" Drive"
   menu. The `$80` subtype fix (above) made the disk-swap prompts advance.
2. ✅ **Boot GS/OS *from* the HDD** — `boot = hdd` starts GS/OS to the Finder straight
   from `hdv/GSOS.hdv` (SHR on at ~750k steps, ~141k toolbox dispatches, desktop drawn,
   no derail — headless `hdd_trace none hdv/GSOS.hdv`). **Gotcha found & fixed:** Easy
   Update copies the System Folder + `PRODOS` loader but does **NOT** rewrite the ProDOS
   **boot block** (block 0) on an already-formatted volume, so the installed disk stayed
   non-bootable (byte 0 = `$00`, slot-7 card chained to slot 5). Fix: `make_prodos_hdd.py`
   grew a `--boot-from <prodos-disk>` option that copies a standard ProDOS boot block; the
   shipped `hdv/GSOS.hdv` now carries one (byte 0 = `$01`).

**Remaining (nice-to-have):**
3. **SmartPort FORMAT (`$03`)** for initialising a raw HDD from the Installer /
   Advanced Disk Utility (currently the target must be a pre-formatted ProDOS volume via
   `tools/make_prodos_hdd.py`).
4. **In-emulator "make bootable"** — model whatever GS/OS call would write the boot block
   during a real install, so `--boot-from` isn't needed. BRAM startup-slot persistence to
   a file is a related follow-up.

## 🟢 RESOLVED — GS/OS runtime bugs (July 2026, see CHANGELOG "BASIC.System")

Both "known bugs" traced to the same root class — **silent BRK-to-monitor from two MMU
gaps** — and fixed (MAME `apple2gs.cpp` + KEGS `moremem.c` cited):
1. **BASIC.System / P8 launch crash** → (a) the **Mega II language card** (`$E0/$E1
   $D000-$FFFF`) was flat RAM: no bank-2 `$D000` window, no ROM read-through, no
   write-enable, no ALTZP — GS/OS's P8 launch glue in the `$E0` LC was aliased/corrupt →
   `slowLcRead/slowLcWrite` per MAME `lc_r/lc_w`; (b) the **internal `$C100-$CFFF`
   firmware** was unmapped (0 for non-card slots) — BASIC's `JSR $C300` (80-col firmware)
   executed `$00`s → `slotRomRead` serves the bank-`$FF` ROM image for unclaimed slots +
   `$C800` window. **Verified headless**: Finder → keyboard-driven launch → `]` prompt →
   `PRINT 2+2` → `4`. **User-confirmed interactively** ("Accès BASIC réglé"); the two
   APPLEDISK5.25 desktop icons are the authentic slot-6 internal firmware now being
   visible (expected, matches a real IIgs with the disk port enabled).
2. **Intermittent boot hang + audio crackle** — re-diagnosed: the "EMStatus spin at
   `$FF:CF94`" is the ROM's **KEYIN key-wait** (the `JSR $CF94` caller at `$CEFC`
   increments the `$4E/$4F` entropy seed = classic key wait). The machine had silently
   BRK'd to the monitor (`*` prompt, hidden under the SHR boot screen) and sat waiting
   for a key — same silent-crash class as bug 1, most plausibly the same MMU gaps hit on
   a timing-dependent boot path. **Post-fix user report: crashed once on the very first
   boot, then OK on every subsequent boot** — the one-time crash is plausibly stale
   pre-fix BRAM/desktop state being reinitialised; keep watching. If it recurs, capture
   where the BRK comes from (re-add a BRK trace to `CPU65816.cpp`).

## 🔴 OPEN — GS/OS↔ProDOS-8 round-trip edge case (July 2026)

- **Launching the bare `ProDOS` kernel file from the Finder** (GSOS window → ProDOS icon
  → Open) ends in **"Unable to load START.GS.OS file. Error=$0028"** (no device
  connected). The normal paths are FINE: BASIC.System launches (verified interactively +
  headless), and `BYE` from BASIC reloads GS/OS to the Finder ("One moment please…" →
  desktop, verified headless). Facts from the headless repro (`basic_launch … p` in the
  session scratchpad — boots the HDD, keyboard-drives the Finder): at the error,
  **MSLOT(`$07F8`)=$C5 and DEVNUM(`$43`)=$50** — the GS/OS reload bootstrap is pointed at
  **slot 5 (the EMPTY 3.5")** instead of slot 7 (the boot HDD); and an error-log tap on
  every slot-5 HLE error path stayed **silent** — no device call ever failed, so GS/OS
  synthesizes the `$28` from its own device bookkeeping. Suspect: the P8 kernel COLD-BOOT
  path (running the kernel as an app) re-derives its boot device from MSLOT/zp residue
  (last slot-firmware call = a slot-5 SmartPort poll) rather than the launch device, then
  the GS/OS quit-return inherits it. Low priority — launching the raw kernel is not a
  normal user action and every normal launch/quit path works; **workaround: launch
  BASIC.System (or any .SYSTEM app), not the kernel file.**

## 🟡 GAMES COMPATIBILITY PASS (July 2026, first-ever — collection at
## /media/gistarcade/SHARE/roms/apple2gs/, 361 images, 1.6 GB)

First 8-game headless triage (`screenshot --disk35 <game> --frames 700`):

| Game | Verdict |
|---|---|
| Arkanoid | ✅ SHR intro (Taito "Fun Time Arcade") pixel-perfect |
| Arkanoid II | ✅ SHR crack-intro perfect |
| Battle Chess | ✅ board + attract mode (froze at the board until the $C02F HORIZCNT fix — the game raster-syncs on the horizontal beam counter) |
| Blackjack Academy | ✅ SHR title perfect |
| Airball | ✅ boots to its bundled desktop (game = double-click) |
| Aaargh! | ⚠️ black SHR at 700 frames, code running in bank $01 — maybe still loading; retest longer |
| Block Out | ❌ "PRODOS 16 v1.3 / Loader v1.3" splash → **BRK @ 00/0003** |
| Beyond Zork | ❌ "ROM Version 3" splash → **BRK @ 00/0006** |

**5/8 clean on the first ever run.** The two crashes share a signature: an OLD
boot path (ProDOS 16 v1.3 / a ProDOS-8 Infocom loader) BRKs into the monitor at
zero page right after its splash — same silent-BRK family as the earlier LC/slot
firmware gaps. Diagnose with a P8LOG-style BRK ring trace on Block Out first.
Interactive play test (sound + mouse): File → "Load 3.5\" Disk..." boots the
game disk directly (ejects the HDD + cold reset).

**Launch-mode rule (user-verified):** a self-booting crack (Arkanoid — full
attract mode incl. gameplay screen verified headless) must be BOOTED via
"Load 3.5\" Disk..."; launching it from the GS/OS Finder gives its loader's
"Unable to load tools! 0000". A "needs ToolXXX" crack (Arkanoid II → Tool025 =
Note Sequencer) is the opposite: launch it FROM the Finder so the HDD's full
System provides the tools.

**Play-tested (July 2026, post-ADB-v1):** Battle Chess **playable** ✅; Black
Cauldron "nickel" ✅; Bubble Ghost runs ✅; Captain Blood: intro + digitized
samples play (**some cut slightly early**) then the planet screen stops
responding — probe shows the CPU alive in an unrolled SHR blit loop
(`LDA $17xx,X / STA $E120xx,X` at $01:BEFE), i.e. the game animates but its
logic stalls, most plausibly waiting for a **sample-completion event** that our
DOC ends too early (same root as the truncated samples). 🔴 NEXT DOC LEAD:
one-shot / zero-byte end-of-sample semantics vs MAME (do we halt one sample
early? does the completion IRQ fire at the right time?) — repro: Captain Blood.

**🟢 FIXED (most likely) — "samples cut early / accelerated game music"
(Captain Blood, Transylvania III): the wall-clock timebase counted CPU cycles**
→ at 2.8 MHz the VBL fired at ~164 Hz and every VBL-clocked game engine ran
2.8× fast. Fixed with the single master-tick timebase (see CHANGELOG). Captain
Blood now completes its intro to "PRESS FIRE TO BEGIN" (was stalled at the
planet screen). Transylvania III to be re-play-tested by the user.

**🟢 FIXED — in-game mice (ADB GLU v1, July 2026).** Games program the GLU
directly: a $C026 µC command stream at init + **$C027 interrupt-enable writes**
($30 = data int, Arkanoid) and then take ADB IRQs. Implemented (KEGS adb.c):
$C027 enables latched + reported, updateAdbIrq follows the hardware enables
(legacy sysUp gate kept for the ROM path), real $C026 command machine with
parameter counts + response queue. Headless proof: Battle Chess cursor moves
with injected motion (676 px vs 0 control). Follow-ups if a game still misses:
mouse-via-SRQ DATAREG packets, µC RAM/keycode commands (0x08/0x11 absorbed).

## Milestone roadmap

The build order is chosen so each layer is independently testable before the
next depends on it — the CPU against Tom Harte vectors, the MMU against a ROM
self-test, then the visible/audible subsystems.

| # | Milestone | Deliverable | Gate |
|---|---|---|---|
| **M0** | Foundation | Repo, CMake (native/WASM/headless), doc suite, subsystem map | 🟢 `cmake && make` builds an ImGui window |
| **M1** | 65C816 core | `CPU65816` — emulation + native mode, 24-bit, all 256 opcodes | 🟢 Tom Harte `SingleStepTests/65816` 100% on 64 opcode families × 2 modes (384k vectors, regs+RAM+**cycles**). MVN/MVP excluded (cycle-cap granularity, see DEV). Extending corpus to full 256 = ongoing |
| **M2** | MMU / FPI + Mega II | `IIgsMemory` — 16 MB banks, shadow, speed reg, slow/fast split | 🟡 ROM 01 **and** 03 boot from ROM vector → native → self-diagnostic → speed-calibration loop ($FF:FCDC). Needs VBL/timer to progress. Verify: `boot_trace` |
| **M3** | Legacy video + VGC | `VGC` Super Hi-Res (320/640) + 40-col text from the authentic char ROM → GL display | 🟡 SHR renders (vgc_test green, PNG verified); text renders with authentic char ROM (344s0047); HGR + DHGR colour (NTSC + RGB, dhgr_test); scanline IRQ + 80col next |
| **M4** | ADB + BRAM/RTC | ADB GLU HLE + STATEREG-read fix + //e main/aux redirect | 🟡 ROM 03 boots through all self-tests to the **"Apple IIgs / ROM Version 3" banner**, then reaches disk-boot ($C0Ex, needs M5 IWM). Real kbd/mouse routing + BRAM persistence + ROM 01 banner = follow-ups |
| **M5** | Disk (IWM/SWIM) + **//e legacy** | Reuse POM2 `IWMDevice`+`DiskImage`; add `Swim` for ROM 03. **Plus full Apple //e compatibility**: main/aux memory redirection (RAMRD/RAMWRT/80STORE/PAGE2), LORES/HGR/DHGR video (reuse POM2 `Apple2Display`), so 8-bit //e software runs. | 🟡 IWM 5.25" read path + //e HGR/LORES video done; ROM boots to **"Check startup device!"** (no disk). Real disk boot + SWIM/3.5" + NTSC-colour + full //e mem = follow-ups |
| **M6** | Ensoniq 5503 DOC | `Es5503` — 32 osc, 64 KB sound RAM, Sound GLU ($C03C-$C03F) | 🟢 MAME es5503 parity (July 2026): swap mode + partner start + retrigger quirk, $E0 IRQ protocol, native-rate pitch (894886/(N+2) Hz→host), GLU bit6/bit5 decode + $C03D read latch FIXED (were swapped/missing → DOC was silent). Gate: doc_test (16 checks). Interactive gate = synthLAB music |
| **M7** | Serial + slots | `Scc8530` serial | 🟡 SCC loopback (scc_test gate); slot bus / SmartPort / Mockingboard reuse from POM2 = follow-up |
| **M8** | Polish | WASM build (Emscripten) | 🟡 `./build_wasm.sh` → POMIIGS.html/.js/.wasm (emscripten main loop); snapshot/rewind, CLI, packaging = follow-ups |

## Parity dashboard (MAME `apple2gs.cpp` → POMIIGS)

| Hardware | MAME reference | POMIIGS | State |
|---|---|---|---|
| 65C816 CPU | `cpu/g65816/` | `CPU65816` | 🟢 384k Tom Harte vectors green (64 families ×2 modes; MVN/MVP excluded) |
| FPI speed/shadow regs ($C035-$C037) | `apple2gs.cpp` | `IIgsMemory` | 🟡 shadow/speed stored; write-through partial |
| Mega II slow-side + I/O shadow | `apple2gs.cpp` | `IIgsMemory` | 🟡 $E0/$E1 RAM + I/O; text/HGR/**SHR** shadow ($01→$E1, SuperHiRes over Hi-Res ranges); aux-HGR bit4 = follow-up |
| STATEREG ($C068) | `apple2gs.cpp` | `IIgsMemory` | 🟢 compose/decompose |
| VGC Super Hi-Res + SCB/palette | `apple2gs.cpp` | `VGC` | 🟢 320/640 render (vgc_test) |
| VGC scanline / VBL interrupt | `apple2gs.cpp` | `IIgsMemory` | 🟡 VBL status + flag; scanline IRQ TODO |
| ES5503 DOC (32 osc) | `sound/es5503.cpp` | `Es5503` | 🟢 renders tone (doc_test); mixed to miniaudio |
| Sound GLU ($C03C-$C03F) | `apple2gs.cpp` | `Es5503` | 🟢 |
| 1-bit speaker ($C030) | `apple2gs.cpp` | `IIgsMemory`+`AudioOut` | 🟢 cycle-exact square wave → miniaudio |
| Audio host (miniaudio) | — | `AudioOut` | 🟢 mono f32 ring, speaker+DOC mix (native; WASM stub) |
| ADB GLU (keyboard/mouse) | `apple2gs.cpp` | `IIgsMemory` | 🟡 mouse **interrupt** ($C024) + **keyboard interrupt** (key→IRQ_SRC_ADB→ROM `$FE:EC99` reads $C000→PostEvent; typing selects Finder icons) + modifiers ($C025→event via `$FEE267` table, **⌘-key menu shortcuts fire**); native+VBL gated, storm-safe; `adb_test`. Full ADB µC command model = follow-up |
| Battery RAM + RTC | `apple2gs.cpp` | `IIgsMemory` | 🟢 $C033/$C034 full serial protocol (KEGS decode): RTC seconds = host **local** time, BRAM read/write + internal regs, 2-strobe read timing; Control Panel shows correct date/time. File persistence = follow-up |
| IWM (5.25/3.5) | `machine/iwm.cpp` | `Iwm` | 🟢 5.25" read path (boots to Check startup device) |
| SWIM (ROM 03) | `machine/swim.cpp` | `Swim` | 🔴 |
| SCC 8530 serial | `machine/scc8530.cpp` | `Scc8530` | 🟢 loopback (scc_test) |
| Mega II interrupt regs ($C041-$C047) | `apple2gs.cpp` | `IIgsMemory` | 🟡 INTEN/INTFLAG/VBL |

## Reuse-from-POM2 checklist

Shared hardware where POM2's implementation ports/links directly. Verify each
still behaves under the IIgs bus (slow-side timing, 24-bit addresses) before
ticking:

- 🔴 `M6502` — legacy 6502/65C02 not needed (65C816 covers emulation mode) —
  **decision: drop; the 816 in E-mode is the fallback.**
- 🔴 `Memory` IIe paging / language card → fold into `IIgsMemory` slow side
- 🔴 `Apple2Display` (text/LORES/HGR/DHGR) + `NtscPostProcessor` + `CrtEffectStack`
- 🟡 `AudioDevice` / `SpeakerDevice` — folded into `AudioOut` (miniaudio host +
  cycle-exact speaker + DOC mix). `Mockingboard` / `Ssi263` = follow-up.
- 🔴 `IWMDevice` / `DiskImage` / `Block512Backing` / WOZ / 2mg
- 🔴 `SmartPortCard` / `Sony35Drive` / `Disk35Image` / SmartPort hub
- 🔴 `SlotBus` / `SlotPeripheral` (wire-OR IRQ)
- 🔴 `MachineSnapshot` / `RewindBuffer`
- 🔴 `CliDispatcher` / `EmulationController` (fork, retune clock to 2.8/1.02)

## Post-M8 backlog — specialist gap analysis (what makes it a *real* IIgs)

POMIIGS runs the 8-bit / Apple II world well (ROM, ProDOS, Total Replay, colour
HGR, keyboard/joystick). The 16-bit IIgs experience (GS/OS, the desktop, sound)
needs the following, in rough priority order.

**P1 — Sound (the machine's identity)**
- 🟢 Wire the Ensoniq 5503 DOC to real audio output (miniaudio) — `AudioOut`
  mixes `Es5503::render` into the output ring each frame.
- 🟢 1-bit **speaker** ($C030 toggle) → audio — cycle-exact square-wave
  reconstruction from the MMU toggle stamps (Total Replay games audible).
- 🟢 DOC **oscillator IRQ** — per-osc pend flags + the $E0 stream protocol
  (lowest pending, read-acks one, bit7 = none), mirrored to the CPU IRQ line.
- 🟢 DOC accuracy (July 2026, MAME es5503 parity): swap mode + partner start +
  even-osc retrigger quirk, phase-preserving free-run wrap, native-rate pitch
  (894886/(oscs+2) Hz), cycle-driven production (tickMaster → ring →
  self-balancing drain + linear interp), Sound GLU bit6/bit5 decode + $C03D
  read latch. Gate: doc_test (16 checks).
- 🟢 **synthLAB VALIDATED interactively** (July 2026): mount, tempo, level,
  clean playback. The historical "random cracks" were **$C031 toggling the
  speaker** (it's DISKREG on the IIgs — every 16-bit `LDA $C030` beep collapsed
  to clicks) + the speaker's **DC offset amplifying device-level gaps** (fixed
  with an AC-coupling DC blocker, fc≈20 Hz) + audio-clock drift (fixed with
  dynamic rate control ±0.65%). User verdict: "Il n'y a plus aucun gros cracks".
- 🔴 **Very light residual cracks, rare, during synthLAB playback** (user:
  "exceptionnellement de très légers cracks"). Next probe when picked up:
  POMDBG during playback — if `underruns`/`drops` tick at crack moments →
  scheduling (move mixing to the audio callback thread); if zero with a clean
  POMWAV → host audio stack (test `aplay` of the capture). Low priority.
- 🔴 DOC follow-ups: stereo/channel routing (ctrl bits 4-7), sync/AM modulation.
- 🔴 WASM audio: Web Audio backend needs the Emscripten audio-worklet link
  flags; the WASM build ships a silent `AudioOut` stub for now.

**P2 — GS/OS boot (3.5" + desktop)**
- 🟢 **MILESTONE: GS/OS 6.0.1 boots to the complete Finder desktop.** Full menu
  bar (🍎 File/Edit/Windows/View/Disk/Special/Colors/Extras), mouse cursor,
  **System.Disk** volume icon, Trash; the "Welcome" window is dismissed.
  Screenshot-verified, all 13 gates green. Got here via four fixes: interrupt
  vectors (`$C071-$C07F` ROM + native vector pull); full fast-RAM backing (8 MB —
  was a 1 MB `fastCell` bank wrap corrupting the zero page); bank-1 language-card
  RAM (`lcRead`/`lcWrite` use aux for bank `$01`); and the **SHR fast-side
  shadow** — `maybeShadow` must check the SuperHiRes gate in the `$2000-5FFF`
  ranges it shares with Hi-Res (GS/OS inhibits Hi-Res shadowing but enables SHR),
  so the menu bar / disk icon drawn to `$01:2xxx` finally reach `$E1`. The desktop
  was never input-gated — the register-level ADB mouse + the mouse interrupt were
  a parallel effort, not the blocker.
- 🟡 **(historical) GS/OS boots to the "Welcome to the IIgs" splash** (icon +
  QuickDraw text + progress bar).
  Diagnosis (July 2026): 800K 3.5" `.po`/`.2mg` images are standard ProDOS
  volumes; the slot-7 ProDOS block device loads + runs their boot code; our SHR
  path renders their splash faithfully. GS/OS then clears $C029 (SHR off) and
  stalls — it needs the rest of the boot chain to continue. Arkanoid II behaves
  the same (SHR dialog → stall; reacts to keys). Test disks in `disks35/` and
  `docs/System 6.0.1/`.
- 🟢 **Slot-5 SmartPort 3.5" drive** (`disk35_`, ROM $C500 = SmartPort,
  $Cn07=$00 + extended bit). The dispatch entries are **WDM traps** handled in
  C++: `$Cn50` = ProDOS block ($42-$47), `$Cn53` = SmartPort (`JSR` + inline
  cmd/param-list). Implements **STATUS** (+ DIB, code 3), **READBLOCK**,
  **WRITEBLOCK**, both standard and the **GS/OS extended** long-address form
  (4-byte buffer + block, any bank). UI: File ▸ Load 3.5" Disk. Gates:
  `disk35_test`, `smartport_test`. **GS/OS 6.0.1 boots to its Welcome screen
  through the real SmartPort path.** Full GS/OS still needs the toolbox (below);
  the low-level IWM-3.5"/Sony path (POM2 `Sony35Drive`) remains an alternative
  for games that bit-bang the drive.
- 🟡 **GS toolbox** — runs from ROM (LLE), not reimplemented. GS/OS starts the
  whole toolbox correctly (Tool Locator, Memory Mgr, QuickDraw II, Event/Window/
  Menu/Control Mgrs, the Loader — every `_xxStartUp` dispatches from ROM; watch
  with `gsos_trace`). **The post-welcome crash is fixed**: the IIgs interrupt
  path was incomplete — `$C071-$C07F` now returns internal ROM (the native
  vector stubs live there: IRQ `$FFEE`=`$C074`=`CLV/JML $E10010`), and native
  vector pulls read ROM even under LC RAM (`IIgsMemory::vectorPull`). GS/OS now
  runs stably past the welcome screen. Remaining to reach the Finder desktop:
  ADB **mouse**, and whatever the VBL-timed boot loop (`$FF/A5Ex`) is waiting on
  (next boot-file load / desktop bring-up).
- 🔴 **SWIM** (ROM 03 disk chip, MFM superset).
- 🟡 **ADB mouse interrupt + keyboard modifiers** — mouse motion/button raise
  `IRQ_SRC_ADB`; the ROM interrupt manager services it via ReadMouse
  (`$FE:B1E1` → `$C024`). Storm-safe (gated on native + VBL-int-enabled, +2-frame
  drop). KEYMODREG ($C025) + `$C024`/`$C027` register interface + host wiring;
  `adb_test`. **Edge case:** mouse motion *during* the disk-load boot phase can
  disrupt the desktop draw — the native+VBL gate is too loose there; a proper fix
  needs the ADB µC enable state (below). Post-desktop mouse motion is safe.
  **Command-key menu shortcuts fire** (⌘-A flashes the Edit menu): the ROM maps
  `$C025` → GS event modifiers via the `$FEE267` table (bit7→appleKey), and
  `main.cpp` delivers the modifier-combo key via `IsKeyPressed` (ImGui suppresses
  `InputQueueCharacters` for it). Follow-ups: (a) full **ADB µC command model**
  (TALK/LISTEN/SRQ) for a hardware-accurate interrupt-enable in place of the
  native+VBL proxy; (b) verify mouse *cursor movement* end-to-end (needs
  directional host motion).

**P3 — VGC completeness (games / demos / apps)**
- 🔴 **Scanline interrupts** (SCB bit6) — split modes (640 menu + 320 gfx).
- 🟢 SHR **colour-fill** mode (SCB bit5 — index-0 repeats the previous pixel)
  and **border colour** ($C034, drawn as an authentic frame around the display).
  Gate: `shr_test`.
- 🟢 **Double Hi-Res** (DHGR 140×192, 16 colour) — aux/main interleave,
  Composite NTSC + Clean RGB (same toggle as HGR). Gate: `dhgr_test`.
  80STORE display-page quirk honoured (PAGE2 = aux-bank select, not page
  flip, when 80STORE+HIRES) → Total Replay DHGR title fades correct. Gate:
  `dhgr_page_test`.
- 🟢 **80-column text** (aux/main interleaved 80×24). Gate: `text80_test`.
- 🟢 **Text colour** from $C022 (fg/bg via the 16-colour lo-res palette;
  applies to 40- and 80-col). Interlaced/VOC mode still TODO.

**P4 — Timing + interrupts**
- 🟢 Real **2.8 / 1.02 MHz** fast/slow clock. The host loop now accounts a
  frame in **master-clock ticks** (238420 = one Mega II frame): each CPU step
  costs 5 master (fast) or 14 (slow) by the *live* $C036 bit7, so **mid-frame
  speed switches** are honoured. //e slow-mode runs at 1.022 MHz (measured in
  Total Replay). Per-access **slow-side penalty**: Mega II accesses (banks
  $E0/$E1, $Cxxx I/O + LC, shadowed writes) add +9 master (5→14) in fast mode.
  Gates: `speed_test`, `slowside_test`. Minor remaining nuance: Mega II
  fast/slow *phase-sync* sub-cycle alignment (we model the 1 MHz cost only).
- 🟡 IRQ set: **VBL** (tick edge), **¼-second + 1-second** timers (frame-driven,
  60 Hz), **scan-line** (VGCINT enable + SCB bit6), and **DOC** oscillator IRQ
  (IRQ-enabled osc completes → CPU line, cleared by the $E0 osc-int reg) are
  wired through their real registers ($C023/$C032/$C041/$C046/$C047) onto the
  wire-OR CPU lines. Gate: `irq_test`. **IRQ dispatch now vectors correctly**:
  native/emulation vector table lives in the `$C071-$C07F` reserved-reads-ROM
  region ($FFEE=$C074=`CLV/JML $E10010`), and native vector pulls read ROM under
  LC RAM — so GS/OS's VBL IRQ reaches the ROM Interrupt Mgr instead of crashing.
  Remaining: **ADB**, **SCC**, **Mega II mouse** IRQs have no active source yet
  (keyboard is the $C000 latch, SCC is loopback, no mouse — see P2); scan-line is
  IRQ-only (the renderer doesn't split a frame mid-screen yet — see P3).

**P5 — Peripherals / infra**
- 🔴 **Battery RAM + RTC** (Control Panel persistence; currently $C033/$C034 stub).
- 🔴 Slot **internal firmware** $C100-$CFFF (returns 0 today).
- 🔴 Real **SCC** serial (modem/printer/AppleTalk) — loopback only.
- 🔴 **Mockingboard**, save-states/debugger, configurable RAM/slots/Control Panel.

## Open questions

- ROM 03 up to 8 MB RAM: model as flat fast-side array or paged expansion card?
- DOC oscillator IRQ cadence vs POM2's `emuCycles` audio bus — confirm the
  cycle-stamp path survives the DOC's own 894.886 kHz sample clock.
- Native-mode vector pull vs POM2 snapshot format — extend `SnapshotIO` for
  the 816's wider register file (16-bit A/X/Y, DBR, PBR, D, 16-bit SP).
