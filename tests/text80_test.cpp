// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// 80-column text gate. Verifies: (1) the 80-col dispatch, (2) the aux/main
// column interleave (even columns from aux $E1, odd from main $E0), and
// (3) $C022 SCREENCOLOR (text fg = high nibble, bg = low nibble).
//
// Uses a synthetic 2 KB char ROM: glyph $01 = solid block (all 7 dots each
// row), glyph $00 = blank. Aux is filled with $01, main with $00, so even
// screen columns must be lit (fg) and odd columns dark (bg).

#include "IIgsMemory.h"
#include "VGC.h"
#include <cstdint>
#include <cstdio>
#include <vector>

// The shared lo-res / text palette (mirror of VGC.cpp kLoresPalette).
static const uint32_t kPal[16] = {
    0xFF000000, 0xFF3300DD, 0xFF990000, 0xFFDD22DD, 0xFF007700, 0xFF555555, 0xFFFF2222, 0xFFFFAA66,
    0xFF005588, 0xFF0066FF, 0xFFAAAAAA, 0xFF8899FF, 0xFF00DD11, 0xFF00FFFF, 0xFF99FF44, 0xFFFFFFFF };

int main() {
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    auto io = [&](uint16_t reg) { return uint32_t(0xE0) << 16 | (0xC000 | reg); };
    mem.write8(io(0x00D), 0);            // 80COL on  → text80()
    mem.write8(io(0x022), 0xF2);         // SCREENCOLOR: fg = white(15), bg = $7F1040(2)

    // Fill text page 1 row 0 directly in the banks the scanner reads:
    // aux ($E1) = solid glyph, main ($E0) = blank.
    for (int k = 0; k < 40; ++k) {
        mem.write8((uint32_t(0xE1) << 16) | (0x0400 + k), 0x01);   // aux  → even cols
        mem.write8((uint32_t(0xE0) << 16) | (0x0400 + k), 0x00);   // main → odd cols
    }

    VGC vgc;
    std::vector<uint8_t> chr(0x800, 0);
    for (int i = 0; i < 8; ++i) chr[0x08 + i] = 0x7F;    // glyph $01 = solid
    vgc.setCharRom(chr);

    const uint32_t* fb = vgc.render(mem);
    const int W = vgc.width();
    const uint32_t fg = kPal[0xF], bg = kPal[0x2];

    // Row 0 is framebuffer rows 0..15; sample the glyph mid-row (y = 4).
    const int y = 4 * W;
    int fails = 0, evenLit = 0, oddDark = 0;
    for (int col = 0; col < 80; ++col) {
        // Cell is 8 px wide, glyph occupies px 0..6; sample px 3 (a set dot in
        // a solid glyph, blank otherwise).
        uint32_t p = fb[y + col * 8 + 3];
        if (col & 1) {                       // odd column ← main (blank) → bg
            if (p == bg) ++oddDark; else ++fails;
        } else {                             // even column ← aux (solid) → fg
            if (p == fg) ++evenLit; else ++fails;
        }
    }
    std::printf("80-col: evenLit=%d/40 oddDark=%d/40 fails=%d\n", evenLit, oddDark, fails);
    if (fails || evenLit != 40 || oddDark != 40) {
        std::printf("FAIL: 80-col interleave/colour wrong\n");
        return 1;
    }
    std::printf("OK: 80-column text (aux/main interleave + $C022 colour)\n");
    return 0;
}
