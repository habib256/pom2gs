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

| Class | All 341 | Bootable only (180, have a PRODOS system file) |
|---|---|---|
| OK_GFX | 114 | **114 (63%)** |
| CRASH_BRK | 34 | 32 (17%) |
| TEXT | 26 | 19 (10%) |
| CRASH_MON | 10 | 7 (3%) |
| CRASH_ZP | 7 | 6 (3%) |
| HANG | 150 | 2 (1%) |

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

## Next fix (highest ROI)

Root-cause the shared toolbox-call stack imbalance (`$00:3B81` loader →
`$FC:DC94` dispatch). Fixing it unblocks ~30 GS titles at once. Reproduce
with any of: Block Out, World Tour Golf, Beyond Zork (all crash identically).
