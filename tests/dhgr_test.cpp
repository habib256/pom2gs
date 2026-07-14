// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Double Hi-Res decode gate. A DHGR line whose 560-dot stream repeats a 4-bit
// nibble `v` must render as the solid lo-res palette colour kPalette[v] in the
// clean-RGB path; the composite path must render an all-ones line bright.
// Header-only (VGCNtsc.h) — no emulator link needed.

#include "VGCNtsc.h"
#include <cstdint>
#include <cstdio>

using namespace pomiigs::ntsc;

int main() {
    int fails = 0;

    for (unsigned v = 0; v < 16; ++v) {
        // Pack a stream that repeats nibble v (dot i = bit (i%4)) into the
        // aux(7)+main(7) column layout the renderer reads.
        uint8_t aux[40], main[40];
        for (int col = 0; col < 40; ++col) {
            uint8_t a = 0, m = 0;
            for (int i = 0; i < 7; ++i) {
                if ((v >> ((col * 14 + i) % 4)) & 1)     a |= uint8_t(1 << i);
                if ((v >> ((col * 14 + 7 + i) % 4)) & 1) m |= uint8_t(1 << i);
            }
            aux[col] = a; main[col] = m;
        }
        uint32_t line[280];
        decodeDhgrRgbLine(aux, main, line);
        const uint32_t want = kPalette[v];
        int bad = 0;
        for (int p = 2; p < 138; ++p) if (line[p * 2] != want) ++bad;   // skip edges
        if (bad) { std::printf("FAIL v=%2u: %d px != %08X\n", v, bad, want); ++fails; }
    }

    // Composite: an all-ones line should be bright (white artifact).
    uint8_t aux[40], main[40];
    for (int c = 0; c < 40; ++c) { aux[c] = 0x7F; main[c] = 0x7F; }
    uint32_t line[280];
    decodeDhgrLine(aux, main, line);
    int bright = 0;
    for (int x = 20; x < 260; ++x) if ((line[x] & 0xFF) > 180) ++bright;
    if (bright < 150) { std::printf("FAIL composite: only %d bright px\n", bright); ++fails; }

    if (fails) { std::printf("dhgr_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: DHGR clean RGB (16 colours) + composite decode\n");
    return 0;
}
