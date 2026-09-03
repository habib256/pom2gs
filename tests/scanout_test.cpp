// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Live scanout gate: the VGC fetches memory as the beam crosses it, so a
// change made after the beam has passed a line shows on the NEXT frame, a
// palette rewritten during a line's HBL applies to that line, and a text row
// rewritten while it is scanned crosses the scanner byte by byte. Also pins
// the snapshot fallback used by callers that never run the clock.

#include "IIgsMemory.h"
#include "VGC.h"

#include <cstdio>
#include <vector>

int main() {
    int fails = 0;
    auto expect = [&](const char* what, unsigned got, unsigned want) {
        if (got != want) { std::printf("FAIL %-56s $%02X != $%02X\n", what, got, want); ++fails; }
    };
    using SL = IIgsMemory::ScanLine;
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();                                        // slow mode: one Mega II cycle per CPU cycle
    auto W = [&](uint32_t a, uint8_t v) { mem.write8(a, v); mem.takeBusPenalty(); };
    // Advance the beam to (line, cycle) from the current position.
    auto beamTo = [&](int line, int cycle) {
        const long target = long(line) * 65 + cycle;
        const long now = long(mem.masterTicks() / 912) * 65 + long(mem.masterTicks() % 912) / 14;
        if (target > now) mem.tick(int(target - now));
    };

    // Text page 1: 'A' on row 0 and row 20 before the frame starts.
    W(0xE0C051, 0);                                     // TEXT
    W(0xE00400, 'A' | 0x80);
    W(0xE00400 + 0x0250, 'A' | 0x80);                   // row 20 base = $0400 + (20%8)*$80 + (20/8)*$28 = $0650
    beamTo(100, 0);                                     // beam past rows 0..12
    W(0xE00400, 'B' | 0x80);                            // too late for this frame's row 0
    W(0xE00650, 'B' | 0x80);                            // row 20 not scanned yet
    beamTo(191, 64);                                    // end of the visible field
    expect("row 0 col 0 was fetched before the rewrite", mem.scanLine(0).main[0], 'A' | 0x80);
    expect("row 20 col 0 fetched after the rewrite", mem.scanLine(160).main[0], 'B' | 0x80);
    expect("line flagged TEXT", mem.scanLine(0).flags & SL::TEXT, SL::TEXT);

    // Beam race inside one line: rewrite the row while its scan is under way.
    beamTo(262 + 8 * 5, 25 + 20);                       // next frame, row 5, scanner at column 20
    for (int c = 0; c < 40; ++c) W(0xE00400 + 5 * 0x80 + c, 'C' | 0x80);   // row 5 base = $0680
    beamTo(262 + 8 * 5, 64);
    expect("column 10 was fetched before the rewrite", mem.scanLine(40).main[10], 0x00);
    expect("column 30 was fetched after the rewrite", mem.scanLine(40).main[30], 'C' | 0x80);

    // SHR: a palette written during line 50's HBL applies to line 50, not 49.
    W(0xE0C029, 0x81);                                  // SHR on, bank latch kept
    for (int l = 0; l < 200; ++l) W(0xE19D00 + l, 0x00);      // SCB: palette 0, 320 mode
    W(0xE19E02, 0x0F); W(0xE19E03, 0x00);               // palette 0 entry 1 = blue... ($00F)
    beamTo(262 * 2, 0);                                 // frame 3
    beamTo(262 * 2 + 50, 3);                            // in line 50's HBL
    W(0xE19E02, 0xF0); W(0xE19E03, 0x00);               // entry 1 → green ($0F0)
    beamTo(262 * 2 + 51, 0);
    expect("line 49 keeps the old palette entry", mem.scanLine(49).pal[2], 0x0F);
    expect("line 50 latched the HBL palette write", mem.scanLine(50).pal[2], 0xF0);
    expect("line flagged SHR", mem.scanLine(50).flags & SL::SHR, SL::SHR);

    // Snapshot fallback: a fresh machine rendered without any clock shows memory.
    IIgsMemory m2; m2.loadRom(rom); m2.reset();
    m2.write8(0xE00400, 'Z' | 0x80); m2.takeBusPenalty();
    VGC vgc;
    vgc.render(m2);
    expect("snapshot fallback captured the text byte", m2.scanLine(0).main[0], 'Z' | 0x80);
    expect("snapshot consumed the live flag", m2.scanLive() ? 1 : 0, 0);

    if (fails) { std::printf("scanout_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: live scanout — frame ordering, in-line beam race, HBL palette latch, snapshot fallback\n");
    return 0;
}
