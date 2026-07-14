// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Apple II NTSC composite artifact-decode primitives for colour HGR/DHGR.
// Ported from POM2's Apple2VideoDecode.h (which itself ports MAME
// apple2video.cpp, PR #10773 by benrg — the OpenEmulator-lineage composite
// model). The IIgs' composite output shows these artifact colours; its RGB
// output would instead do a clean 6-colour decode.
//
// Convention: bit 0 of an HGR byte = leftmost pixel, bit 6 = rightmost,
// bit 7 = per-byte half-dot delay flag.

#ifndef POMIIGS_VGCNTSC_H
#define POMIIGS_VGCNTSC_H

#include <array>
#include <cstdint>

namespace pomiigs::ntsc {

inline constexpr int kStreamLen = 560;        // 280 colour clocks × 2 sub-pixels

// Bit doubler: kBitDoubler[i] doubles each of the 7 low bits of i (b,b).
inline constexpr std::array<uint16_t, 128> makeBitDoubler() {
    std::array<uint16_t, 128> t{};
    for (unsigned i = 1; i < 128; ++i)
        t[i] = uint16_t(t[i >> 1] * 4 + (i & 1) * 3);
    return t;
}
inline constexpr std::array<uint16_t, 128> kBitDoubler = makeBitDoubler();

// MAME apple2video.cpp artifact_color_lut[0] (canonical composite/NTSC).
// Each byte packs four 4-bit lo-res palette indices, one per NTSC phase.
inline constexpr uint8_t kArtifactColorLut[128] = {
    0x00,0x00,0x00,0x00,0x88,0x00,0x00,0x00,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x22,0x22,0x66,0x66,0xaa,0xaa,0xee,0xee,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x55,0x55,0x55,0x55,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0x77,0x77,0x77,0x77,0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00,0x88,0x88,0x88,0x88,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xaa,0xaa,0xaa,0xaa,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x11,0x11,0x55,0x55,0x99,0x99,0xdd,0xdd,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0xff,0xff,0xff,0x77,0xff,0xff,0xff,0xff,
};

// 16-colour NTSC lo-res / artifact palette (0xAABBGGRR little-endian).
inline constexpr uint32_t kPalette[16] = {
    0xFF000000, 0xFF400BA7, 0xFFF71C40, 0xFFFF28E6, 0xFF407400, 0xFF808080, 0xFFFF9019, 0xFFFF9CBF,
    0xFF006340, 0xFF006FE6, 0xFF808080, 0xFFBF8BFF, 0xFF00D719, 0xFF08E3BF, 0xFFBFF458, 0xFFFFFFFF,
};

// rotl4b(n, count): the 4-bit nibble of n at logical phase `count` (mod 4).
inline constexpr unsigned rotl4b(unsigned n, unsigned count) {
    return (n >> ((-int(count)) & 3)) & 0x0Fu;
}

// Decode 40 HGR bytes → 40 × 14-bit doubled words (half-dot delay on MSB).
inline void buildHgrWordRow(const uint8_t* row, uint16_t (&words)[40]) {
    unsigned lastBit = 0;
    for (int col = 0; col < 40; ++col) {
        const uint8_t b = row[col];
        uint16_t word = kBitDoubler[b & 0x7F];
        if (b & 0x80) word = uint16_t(((word << 1) | lastBit) & 0x3FFF);
        words[col] = word;
        lastBit = (word >> 13) & 1u;
    }
}

// DHGR word row: 80-column mode lays aux memory (leftmost 7 dots) then main
// memory (next 7 dots) per column — 40 columns × 14 dots = 560. No bit-
// doubling and no half-dot delay (DHGR ignores bit 7). This yields the same
// 14-bit-per-column `words[]` shape the artifact decoder consumes, so the
// composite decode below is shared with HGR. Cited: MAME apple2video.cpp
// (dhgr builds the 560-dot stream from interleaved aux/main 7-bit groups).
inline void buildDhgrWordRow(const uint8_t* aux, const uint8_t* main,
                             uint16_t (&words)[40]) {
    for (int col = 0; col < 40; ++col)
        words[col] = uint16_t((aux[col] & 0x7F) | ((main[col] & 0x7F) << 7));
}

// Shared windowed artifact decode: 40 × 14-bit words → 280 RGBA pixels via the
// 560-sub-pixel LUT + pair average. Used by both HGR and DHGR composite paths.
inline void decodeWordRow(const uint16_t (&words)[40], uint32_t* out /*[280]*/) {
    constexpr int kCtx = 3;
    uint32_t sub[kStreamLen];
    uint32_t w = uint32_t(words[0]) << kCtx;
    for (int col = 0; col < 40; ++col) {
        if (col + 1 < 40) w |= uint32_t(words[col + 1]) << (14 + kCtx);
        for (int b = 0; b < 14; ++b) {
            const int absX = col * 14 + b;
            unsigned idx = rotl4b(kArtifactColorLut[w & 0x7F], unsigned(absX));
            sub[absX] = kPalette[idx];
            w >>= 1;
        }
    }
    for (int x = 0; x < 280; ++x) {                 // downsample 560 → 280 (pair average)
        uint32_t a = sub[2 * x], b = sub[2 * x + 1];
        uint32_t r = ((a & 0xFF) + (b & 0xFF)) >> 1;
        uint32_t g = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) >> 1;
        uint32_t bl = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) >> 1;
        out[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
    }
}

// Full colour decode of one HGR scanline (40 bytes at `row`) into 280 RGBA
// pixels (`out`), via the 560-sub-pixel windowed artifact LUT + pair average.
inline void decodeHgrLine(const uint8_t* row, uint32_t* out /*[280]*/) {
    uint16_t words[40];
    buildHgrWordRow(row, words);
    decodeWordRow(words, out);
}

// Composite (NTSC) DHGR: 40 aux + 40 main bytes → 280 RGBA pixels.
inline void decodeDhgrLine(const uint8_t* aux, const uint8_t* main, uint32_t* out /*[280]*/) {
    uint16_t words[40];
    buildDhgrWordRow(aux, main, words);
    decodeWordRow(words, out);
}

// Clean RGB DHGR: the IIgs RGB monitor shows DHGR as 140 fat pixels, each a
// 4-dot colour clock indexing the 16-colour lo-res palette (no NTSC fringing).
// Aligned to 4-dot boundaries from column 0, so the dot phase is constant and
// the nibble maps directly (dot 0 = LSB). Output is 280 (each fat pixel ×2).
inline void decodeDhgrRgbLine(const uint8_t* aux, const uint8_t* main, uint32_t* out /*[280]*/) {
    uint8_t bits[kStreamLen];
    for (int col = 0; col < 40; ++col) {
        const uint8_t a = aux[col] & 0x7F, m = main[col] & 0x7F;
        for (int i = 0; i < 7; ++i) {
            bits[col * 14 + i]     = (a >> i) & 1;
            bits[col * 14 + 7 + i] = (m >> i) & 1;
        }
    }
    for (int p = 0; p < 140; ++p) {                 // 140 fat pixels, 4 dots each
        unsigned v = bits[p * 4] | (bits[p * 4 + 1] << 1) |
                     (bits[p * 4 + 2] << 2) | (bits[p * 4 + 3] << 3);
        uint32_t c = kPalette[v];
        out[p * 2] = c; out[p * 2 + 1] = c;
    }
}

} // namespace pomiigs::ntsc

#endif // POMIIGS_VGCNTSC_H
