# Compatibility triage

Headless boot triage over the game collections, produced by `tests/triage`
(one line per image, classifying how far it booted). Re-generate with:

```bash
# GS 3.5" (default HLE SmartPort path — the emulator's default)
for f in /path/to/apple2gs/*.2mg; do
  ./build/triage roms/iigs-rom03.rom "$f" --disk35 --frames 900; done > docs/compat_gs35.txt
# Apple II 5.25" WOZ (//e-compat mode)
for f in /path/to/woz/*.woz; do
  ./build/triage roms/iigs-rom03.rom "$f" --disk525 --frames 1500; done > docs/compat_woz525.txt
```

Status classes: `OK_GFX` (running in RAM, graphics up) · `TEXT` (running,
text mode) · `CRASH_BRK` (executed BRK in RAM) · `CRASH_ZP` (runaway in zero
page) · `CRASH_MON` (fell into the ROM monitor) · `HANG` (PC frozen) ·
`NOBOOT` (never left the boot ROM).

## GS 3.5" — 341 images (ROM 03, HLE SmartPort)

| Class | All 341 | After the slot-7 AppleTalk fix | After the SmartPort DIB/CONTROL fixes | After the hybrid Sony mount |
|---|---|---|---|---|
| OK_GFX | 114 | 144 (+30) | 149 (+5) | **151 (+2)** |
| CRASH_BRK | 34 | 3 (−31) | 3 | 3 |
| TEXT | 26 | 24 | 25 | 21 |
| CRASH_MON | 10 | 13 | 12 | 14 |
| CRASH_ZP | 7 | 7 | 2 (−5) | 2 |
| HANG | 150 | 150 | 150 | 150 |

**+30 real games** unblocked by one fix (see below): of the ~180 genuinely
bootable disks, **~80% now reach graphics**. Wins include Winter Games,
Questron II, King's Quest IV, Police Quest, Leisure Suit Larry, Tunnels of
Armageddon, Global Express Atlas, MathTalk, Roadwar 2000, Warlock, and the
long-standing Block Out / Beyond Zork / World Tour Golf failures. No working
title regressed.

### Root cause & fix (KEGS golden-trace diff)

The ~30 identically-crashing titles share a cracked-disk loader that scans
**slot-7 ROM at `$00:C7F9`** for the AppleTalk signature `"ATLK"` ($41 54 4C
4B). POMIIGS served the system ROM's internal `$FF:C7xx` image there — which
**is** the AppleTalk firmware — so the scan matched, the loader took its
"AppleTalk present" path, made a tool call that returned C=1 (error) and
`RTS`'d to `$0000` → zero page → `BRK`. Real hardware (and KEGS) only shows
the AppleTalk firmware when slot 7 is set to AppleTalk in the Control Panel;
by default `$C7F9` reads `$00`. Found by building KEGS headless, tracing the
same loader on World Tour Golf, and diffing instruction-by-instruction to the
first divergence: at `$00:3B40 LDA ($FE),Y` KEGS read `$00`, POMIIGS read
`$4B`. Fix: an empty slot 7 (no HDD card) reads `$00`, not the internal
AppleTalk firmware (`IIgsMemory::slotRomRead`).

**Reading it:** 148 of the 150 HANGs are **non-bootable images** — data disks
(Disk 2-9 of a set) or stripped images with no `PRODOS` system file in the
root directory. Their boot block correctly prints "UNABLE TO LOAD PRODOS" and
halts at `$00:0955`, exactly as real hardware would. These are **not**
emulator bugs. Of the 180 genuinely-bootable disks, **63% reach graphics**.

**The real bug bucket = the ~45 crashes on bootable disks.** They cluster
hard: Block Out, World Tour Golf, Beyond Zork, Dream Zone, Manhunter,
Cavern Cobra, Silpheed, Police Quest, Leisure Suit Larry, Questron II,
Warlock, King's Quest IV, Space Quest II, and ~20 more share an **identical
crash chain** — a common cracked-disk loader stub at `$00:3B81/$3C12` makes a
toolbox call (`JSL $E10068` → ROM dispatch `$FC:DC94`) that returns with the
stack 2 bytes off, so the loader's next `RTS` jumps to `$0000` and runs zero
page until it hits a `$00` (BRK). GS/OS itself makes thousands of toolbox
calls without issue, so the defect is a specific CPU-state / call edge case
this loader hits. **One root cause ≈ 30 titles.** (Landing PCs vary —
`$FE:00E4`, `$FF:CFxx`, `$FF:A5Fx` — because a BRK vectors into different ROM
handlers; the *source* chain is shared.)

## Apple II 5.25" WOZ — 726 images (ROM 03, //e-compat)

167 OK_GFX · 191 TEXT (many //e titles boot to Applesoft/an input prompt) ·
132 HANG (84 at `$00:C5E0` = non-bootable/second-side disks) · 232 crashes.
The 5.25" //e surface (bank switching, softswitch timing, cassette/lores) is
a separate large effort from the GS-native path above.

### SmartPort DIB/CONTROL fixes (July 2026)

Chasing the `$FF:A5Fx` / CRASH_ZP clusters surfaced three distinct issues in
the slot-5 **HLE SmartPort** identity bytes, all fixed with 0 regressions:

1. **`$C5FB` ID type byte** was `$02` ("SCSI") instead of bit7 = Extended
   SmartPort. Dungeon Master probes it and aborted with *"SmartPort firmware
   not detected in slot 5!"* (masked as OK_GFX at 900 frames — it crashed
   right after the splash). Now `$80`+dispatch (real ROM 03 has `$C0`).
2. **DIB subtype** was `$80` — a value no real device reports. Silent
   Service's loader whitelists `$00` (UniDisk 3.5) / `$C0` (Apple 3.5) and
   BRK'd on anything else. Now `$C0` (= extended + disk-switched, same as
   KEGS); the GS/OS Installer swap flow still works (Finder boots pinned).
3. **CONTROL codes**: everything unknown succeeds as a no-op (a shared
   loader family — Qix, Life & Death, Jack Nicklaus, 2088 — sends `$0A` and
   dies on any error) **except `$06`**, which returns `$21` BADCTL: Marble
   Madness's protection installs an I/O hook (ctl `$05`) and uses `$06` to
   have it patch a vector; on a silent no-op it jumps through the
   never-initialized vector (BRK), while `$21` sends it down its clean
   no-protection fallback. Protected originals that *verify* the real drive
   protocol still need the LLE path (`iwm35 = 1`).

Also fixed the triage heuristic itself: `CRASH_ZP` is now gated on `!gfx` —
Pirates!, Silent Service, Reader Rabbit, Wheel of Fortune, Ancient Legends
legitimately run code from zero page with a live SHR screen (a real runaway
lands on `$00` = BRK and is already caught by CRASH_BRK).

### Hybrid Sony mount + I/O-hook protocol findings (July 2026)

Splitting the residual crashes by **HLE vs LLE** (`--iwm35`) exposed two
loader families that bypass the SmartPort:

- **Direct-IWM loaders** (Sensei, Space Cluster — same boot block, extended
  SmartPort reads from `$00:0954`, then raw `$C0Ex` access): they boot via
  the HLE but then poll the 3.5" drive **through the IWM**, and died with
  "Fatal Disk Error : DEAD" (their own `SysFailMgr $DEAD` timeout exit).
  **Fix: hybrid mount** — an HLE-mounted 3.5" image is also inserted
  READ-ONLY into the real Sony/IWM drive, so both paths reach the same
  medium exactly like real hardware. Both titles now boot; GS/OS boots
  (HDD + 3.5" HLE + 3.5" LLE) are unaffected.
- **I/O-hook protections** (Marble Madness; the Cinemaware loader family —
  Defender of the Crown, Mean 18, King of Chicago, Aesop's Fables,
  Impossible Mission II, Mini Putt, JSR site `$00:8213`): ctl `$05`
  installs a RAM routine into the firmware's E1-RAM vector table
  (`$E1:0F77`), called during in-window READs with the drive spinning
  (traced under LLE: A=1, X=Y=0, P=$B4 native, DBR=$E1); ctl `$06`
  restores the vector. Re-creating that call in the HLE (per-READ or at
  `$06`, hybrid drive armed on the read track) satisfied only Marble
  Madness and crashed the Cinemaware family — their hooks need the
  firmware's exact in-read stack context. Kept behavior: `$05` = silent
  success, `$06` = `$21` BADCTL → Marble Madness and the whole Cinemaware
  family boot through their no-protection fallbacks. Only **Defender of
  the Crown** insists ("Put Master Disk…" prompt) — it boots under
  `iwm35 = 1`, where the genuine firmware runs the real protocol.

## Next fix (highest ROI)

The remaining crashes are few and heterogeneous (3 CRASH_BRK, 14 CRASH_MON,
2 CRASH_ZP) — no single dominant signature. Both-paths crashers worth a
look: Wolfenstein 3D, Alien Mind, Skate or Die, Ancient Land of Ys,
Solitaire & Cribbage, Tomahawk. Known non-bugs: Rastan boots to a Computist
crack prompt waiting for keyboard input (TEXT = correct); Star Saga One is
a working text-mode RPG (TEXT = correct); "Dungeon Master v2.0 Original"
fails its own protection even under the LLE real drive ("game disk
damaged" — the flat .2mg lost the protection data); Silpheed correctly
waits for its Disk 2; Defender of the Crown needs `iwm35 = 1` (above). The
150 HANGs remain non-bootable images (no PRODOS system file — correct
behavior, not bugs).
