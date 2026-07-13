// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// VGC render smoke test: paint a Super Hi-Res test pattern into $E1 video RAM
// and verify the VGC renders it. Prints a downsampled ASCII view + pixel
// checks (no image libs needed). M3 gate.

#include "VGC.h"
#include "IIgsMemory.h"
#include <cstdint>
#include <cstdio>

int main() {
    IIgsMemory mem;
    // 16-colour rainbow palette 0 ($E1:9E00), 4-4-4 entries.
    auto wr = [&](uint32_t a, uint8_t v) { mem.write8(a, v); };
    const uint32_t E1 = 0xE10000;
    for (int i = 0; i < 16; ++i) {
        uint8_t r = (i & 8) ? 0xF : (i & 1 ? i : 0);
        uint8_t g = (i * 17) & 0x0F;
        uint8_t b = 15 - i;
        wr(E1 + 0x9E00 + i * 2 + 0, uint8_t((g << 4) | b));   // $GB
        wr(E1 + 0x9E00 + i * 2 + 1, r);                       // $0R
    }
    // SCB: all 200 lines → 320 mode, palette 0.
    for (int l = 0; l < 200; ++l) wr(E1 + 0x9D00 + l, 0x00);
    // Pixel data: 16 vertical colour bars (each bar = colour index 0..15).
    for (int l = 0; l < 200; ++l)
        for (int byte = 0; byte < 160; ++byte) {
            int bar = (byte * 2) / (320 / 16);        // which of 16 bars
            uint8_t idx = uint8_t(bar & 0x0F);
            wr(E1 + 0x2000 + l * 160 + byte, uint8_t((idx << 4) | idx));
        }
    // Enable Super Hi-Res ($C029 bit 7).
    wr(0xE0C029, 0x80);

    VGC vgc;
    if (!mem.shrEnabled()) { std::printf("FAIL: SHR not enabled\n"); return 1; }
    const uint32_t* fb = vgc.render(mem);

    // Downsampled ASCII view (each cell = brightness of a region).
    const char* ramp = " .:-=+*#%@";
    std::printf("VGC %dx%d — SHR 320 test pattern (16 colour bars):\n", vgc.width(), vgc.height());
    for (int ry = 0; ry < 16; ++ry) {
        for (int rx = 0; rx < 64; ++rx) {
            int x = rx * vgc.width() / 64, y = ry * vgc.height() / 16;
            uint32_t p = fb[y * vgc.width() + x];
            int lum = ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3;
            std::putchar(ramp[lum * 9 / 255]);
        }
        std::putchar('\n');
    }
    // Checks: 16 distinct bar colours across the top row.
    int distinct = 0; uint32_t prev = 0xDEADBEEF;
    for (int bar = 0; bar < 16; ++bar) {
        int x = bar * 640 / 16 + 8;
        uint32_t p = fb[100 * 640 + x];
        if (p != prev) { ++distinct; prev = p; }
    }
    std::printf("distinct bar colours sampled: %d/16\n", distinct);
    std::printf("%s\n", distinct >= 12 ? "OK: SHR renders" : "FAIL: bars not distinct");
    return distinct >= 12 ? 0 : 1;
}
