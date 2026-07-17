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

| Class | All 341 | After the slot-7 AppleTalk fix |
|---|---|---|
| OK_GFX | 114 | **144 (+30)** |
| CRASH_BRK | 34 | **3 (−31)** |
| TEXT | 26 | 24 |
| CRASH_MON | 10 | 13 |
| CRASH_ZP | 7 | 7 |
| HANG | 150 | 150 |

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

## Next fix (highest ROI)

The remaining crashes are now few and heterogeneous (3 CRASH_BRK, 13
CRASH_MON, 7 CRASH_ZP) — no single dominant signature. The `$FF:A5Fx`
cluster (Rastan, Dungeon Master, Tower of Myraglen) and the CRASH_ZP titles
(Pirates!, Silent Service, Wheel of Fortune) are the next candidates, each
likely a distinct root cause. The 150 HANGs remain non-bootable images (no
PRODOS system file — correct behavior, not bugs).
