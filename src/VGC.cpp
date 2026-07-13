// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// VGC renderer. See VGC.h + DEV.md § Video.
// Source of truth: Apple IIgs Hardware Reference (VGC / Super Hi-Res) + MAME
// apple2gs video. Text uses the authentic Mega II character ROM (344s0047).

#include "VGC.h"
#include "IIgsMemory.h"

namespace {
// IIgs palette entry: 2 bytes = $0RGB (4-4-4). low = $GB, high = $0R.
inline uint32_t rgb12(uint8_t lo, uint8_t hi) {
    uint8_t r = (hi & 0x0F) * 17;
    uint8_t g = ((lo >> 4) & 0x0F) * 17;
    uint8_t b = (lo & 0x0F) * 17;
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
}

bool VGC::setCharRom(const std::vector<uint8_t>& rom) {
    if (rom.size() == 0x4000 || rom.size() == 0x1000 || rom.size() == 0x800) { charRom_ = rom; return true; }
    return false;
}

const uint32_t* VGC::render(const IIgsMemory& mem) {
    if (mem.shrEnabled())      renderSHR(mem);
    else if (mem.textMode())   renderText(mem);
    else if (mem.hires())      renderHGR(mem);
    else                       renderLores(mem);
    return fb_.data();
}

// //e address of text/lo-res row (interleaved) and hi-res row.
static inline int textRowBase(int row, int page2base) {
    return page2base + (row % 8) * 0x80 + (row / 8) * 0x28;
}
static inline int hgrRowBase(int y, int base) {
    return base + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

// ── Legacy hi-res 280×192 (monochrome green; NTSC colour = later) ────────
void VGC::renderHGR(const IIgsMemory& mem) {
    const uint32_t fg = 0xFF33FF33u, bg = 0xFF000000u;
    for (auto& px : fb_) px = bg;
    const uint8_t* e0 = mem.slowRam();
    const int base = mem.page2() ? 0x4000 : 0x2000;
    const int ox = (kW - 560) / 2;                 // centre 560-wide (280×2)
    for (int y = 0; y < 192; ++y) {
        const uint8_t* row = e0 + hgrRowBase(y, base);
        for (int b = 0; b < 40; ++b) {
            uint8_t v = row[b];
            for (int bit = 0; bit < 7; ++bit) {
                uint32_t c = (v & (1 << bit)) ? fg : bg;
                int x = (b * 7 + bit) * 2 + ox, py = y * 2;
                fb_[size_t(py) * kW + x] = c;      fb_[size_t(py) * kW + x + 1] = c;
                fb_[size_t(py + 1) * kW + x] = c;  fb_[size_t(py + 1) * kW + x + 1] = c;
            }
        }
    }
}

// ── Legacy lo-res 40×48 (16 colours) ─────────────────────────────────────
void VGC::renderLores(const IIgsMemory& mem) {
    // Apple II 16-colour lo-res palette (approx NTSC), 4-4-4 → 8-8-8.
    static const uint32_t lut[16] = {
        0xFF000000,0xFF601490,0xFF7F1040,0xFFE0207F,0xFF006040,0xFF808080,0xFF1090F0,0xFFC0A0F0,
        0xFF405010,0xFFF06000,0xFF808080,0xFFF090A0,0xFF10D000,0xFFF0F050,0xFF50F090,0xFFFFFFFF };
    for (auto& px : fb_) px = 0xFF000000u;
    const uint8_t* e0 = mem.slowRam();
    const int page = mem.page2() ? 0x0800 : 0x0400;
    const int cellW = kW / 40, cellH = kH / 48;
    for (int row = 0; row < 24; ++row) {
        int rb = textRowBase(row, page);
        for (int col = 0; col < 40; ++col) {
            uint8_t v = e0[rb + col];
            uint32_t top = lut[v & 0x0F], bot = lut[v >> 4];
            for (int half = 0; half < 2; ++half) {
                uint32_t c = half ? bot : top;
                int y0 = (row * 2 + half) * cellH;
                for (int dy = 0; dy < cellH; ++dy)
                    for (int dx = 0; dx < cellW; ++dx)
                        fb_[size_t(y0 + dy) * kW + col * cellW + dx] = c;
            }
        }
    }
}

// ── Super Hi-Res (320/640 × 200) ─────────────────────────────────────────
void VGC::renderSHR(const IIgsMemory& mem) {
    const uint8_t* e1 = mem.slowRam() + 0x10000;   // bank $E1
    const uint8_t* scb = e1 + 0x9D00;              // scan-control bytes
    const uint8_t* pal = e1 + 0x9E00;              // 16 palettes × 16 colours × 2

    for (int line = 0; line < 200; ++line) {
        const uint8_t s = scb[line];
        const bool mode640 = (s & 0x80) != 0;      // SCB bit 7 = 640 mode
        const int palNum = s & 0x0F;
        const uint8_t* p = pal + palNum * 32;
        auto color = [&](int idx) { return rgb12(p[idx * 2], p[idx * 2 + 1]); };
        const uint8_t* src = e1 + 0x2000 + line * 160;

        uint32_t row[640];
        if (!mode640) {                            // 320: byte = 2 × 4-bit index, each dot doubled to 640
            for (int b = 0; b < 160; ++b) {
                uint8_t v = src[b];
                uint32_t cl = color(v >> 4), cr = color(v & 0x0F);
                row[b * 4 + 0] = cl; row[b * 4 + 1] = cl;
                row[b * 4 + 2] = cr; row[b * 4 + 3] = cr;
            }
        } else {                                   // 640: byte = 4 × 2-bit, column-offset palette
            static const int off[4] = { 8, 12, 0, 4 };
            for (int b = 0; b < 160; ++b) {
                uint8_t v = src[b];
                for (int d = 0; d < 4; ++d) {
                    int two = (v >> ((3 - d) * 2)) & 0x03;
                    row[b * 4 + d] = color(off[d] + two);
                }
            }
        }
        // Blit line, doubled vertically (200 → 400).
        for (int yy = 0; yy < 2; ++yy) {
            uint32_t* dst = &fb_[size_t(line * 2 + yy) * kW];
            for (int x = 0; x < 640; ++x) dst[x] = row[x];
        }
    }
}

// ── 40-column text ($E0:0400, //e interleaved) via the IIgs char ROM ─────
void VGC::renderText(const IIgsMemory& mem) {
    const uint32_t fg = 0xFF33FF33u;   // green phosphor
    const uint32_t bg = 0xFF000000u;
    for (auto& px : fb_) px = bg;

    if (charRom_.empty()) return;      // authentic font required (roms/iigs-char.rom)

    const uint8_t* e0 = mem.slowRam();
    const int page = mem.page2() ? 0x0800 : 0x0400;
    for (int rowc = 0; rowc < 24; ++rowc) {
        int rbase = page + (rowc % 8) * 0x80 + (rowc / 8) * 0x28;
        for (int colc = 0; colc < 40; ++colc) {
            uint8_t b = e0[rbase + colc];
            // Char-ROM glyph: primary set, 8 bytes/char, 7 pixels (bit 0 = left).
            const uint8_t* glyph = &charRom_[(b * 8) & (charRom_.size() - 1)];
            for (int gy = 0; gy < 8; ++gy) {
                uint8_t bits = glyph[gy] & 0x7F;
                for (int gx = 0; gx < 7; ++gx) {
                    uint32_t c = (bits & (1 << gx)) ? fg : bg;
                    int px = colc * 16 + gx * 2;
                    int py = rowc * 16 + gy * 2;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            fb_[size_t((py + dy)) * kW + (px + dx)] = c;
                }
            }
        }
    }
}
