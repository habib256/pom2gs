// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Super Hi-Res completeness gate: SCB color-fill (bit5) and $C034 border colour.
//
// Color-fill (320 mode): a pixel index of 0 repeats the previous pixel's colour
// instead of palette[0]. We paint one red pixel then a run of index-0 pixels
// with fill ON — the run must be red (filled), not blue (palette[0]).

#include "IIgsMemory.h"
#include "VGC.h"
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    auto io  = [](uint16_t r) { return uint32_t(0xE0) << 16 | (0xC000 | r); };
    auto e1  = [](uint32_t off) { return (uint32_t(0xE1) << 16) | off; };
    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };

    mem.write8(io(0x29), 0x80);                       // NEWVIDEO: SHR on
    // Palette 0: entry 0 = blue, entry 5 = red (both opaque).
    mem.write8(e1(0x9E00 + 0 * 2), 0x0F);            // entry0 lo → blue (b=15)
    mem.write8(e1(0x9E00 + 0 * 2 + 1), 0x00);
    mem.write8(e1(0x9E00 + 5 * 2), 0x00);            // entry5 hi → red (r=15)
    mem.write8(e1(0x9E00 + 5 * 2 + 1), 0x0F);
    const uint32_t blue = 0xFFFF0000, red = 0xFF0000FF;
    check("palette blue", VGC::loresColor(0) != blue);   // sanity: palette != lo-res table
    // Line 0 data: one red pixel (hi nibble 5) then a run of index-0 pixels.
    mem.write8(e1(0x2000 + 0), 0x50);                // px0 = red, px1 = idx0
    for (int b = 1; b < 160; ++b) mem.write8(e1(0x2000 + b), 0x00);

    VGC vgc;

    // Fill OFF (SCB = palette 0, no bits): the index-0 run shows palette[0] = blue.
    mem.write8(e1(0x9D00), 0x00);
    const uint32_t* fb = vgc.render(mem);
    check("no-fill: index-0 run = palette[0] (blue)", fb[400] == blue);
    check("no-fill: first pixel red", fb[0] == red);

    // Fill ON (SCB bit5): the index-0 run repeats the last colour = red.
    mem.write8(e1(0x9D00), 0x20);
    fb = vgc.render(mem);
    check("fill: index-0 run = last colour (red)", fb[400] == red);

    // Border colour ($C034 bits 0-3).
    mem.write8(io(0x34), 0x06);
    check("border nibble", mem.borderColor() == 6);
    check("border colour", VGC::loresColor(6) == VGC::loresColor(mem.borderColor()));

    if (fails) { std::printf("shr_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: SHR color-fill (SCB bit5) + $C034 border colour\n");
    return 0;
}
