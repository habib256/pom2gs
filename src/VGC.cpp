// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// VGC renderer. See VGC.h + DEV.md § Video.
// Source of truth: Apple IIgs Hardware Reference (VGC / Super Hi-Res) + MAME
// apple2gs video. Text uses the authentic Mega II character ROM (344s0047).

#include "VGC.h"
#include "IIgsMemory.h"
#include "VGCNtsc.h"

namespace {
// IIgs palette entry: 2 bytes = $0RGB (4-4-4). low = $GB, high = $0R.
inline uint32_t rgb12(uint8_t lo, uint8_t hi) {
    uint8_t r = (hi & 0x0F) * 17;
    uint8_t g = ((lo >> 4) & 0x0F) * 17;
    uint8_t b = (lo & 0x0F) * 17;
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

// Apple IIgs 16-colour lo-res / text palette, 0xAABBGGRR (little-endian RGBA).
// The canonical IIgs colours (4-bit-per-channel nibbles replicated, e.g. $D→$DD)
// — matches MAME apple2gs.cpp. Shared by lo-res and the $C022 text fg/bg. The
// previous table was scrambled (e.g. colour 6, the IIgs boot-banner background,
// rendered orange instead of medium blue).
const uint32_t kLoresPalette[16] = {
    0xFF000000, 0xFF3300DD, 0xFF990000, 0xFFDD22DD,  //  0 black   1 deep red  2 dk blue  3 purple
    0xFF227700, 0xFF555555, 0xFFFF2222, 0xFFFFAA66,  //  4 dk green 5 dk grey   6 med blue 7 lt blue
    0xFF005588, 0xFF0066FF, 0xFFAAAAAA, 0xFF8899FF,  //  8 brown    9 orange   10 lt grey 11 pink
    0xFF00DD11, 0xFF00FFFF, 0xFF99FF44, 0xFFFFFFFF };//  12 lt green 13 yellow 14 aqua    15 white
}

uint32_t VGC::loresColor(uint8_t idx) { return kLoresPalette[idx & 0x0F]; }

bool VGC::setCharRom(const std::vector<uint8_t>& rom) {
    if (rom.size() == 0x4000 || rom.size() == 0x1000 || rom.size() == 0x800) { charRom_ = rom; return true; }
    return false;
}

const uint32_t* VGC::render(const IIgsMemory& mem) {
    if (mem.shrEnabled())              { renderSHR(mem); return fb_.data(); }
    if (mem.textMode())                { if (mem.text80()) renderText80(mem); else renderText(mem); return fb_.data(); }
    // Legacy graphics. In MIXED mode ($C053) the bottom 4 character rows show
    // text drawn from the text page while the top 20 rows stay graphics — the
    // status/score window of countless HGR/LORES games. (POM2 Apple2Display.)
    if (mem.hires() && mem.dhires())   renderDHGR(mem);
    else if (mem.hires())              renderHGR(mem);
    else                               renderLores(mem);
    if (mem.mixed())                   renderTextBand(mem, 20, 24);
    return fb_.data();
}

// Draw text rows [rowStart,rowEnd) OVER the existing framebuffer (no full-screen
// clear) for the mixed-mode text window. Picks 40- or 80-column to match the
// current softswitch, mirroring renderText / renderText80.
void VGC::renderTextBand(const IIgsMemory& mem, int rowStart, int rowEnd) {
    if (charRom_.empty()) return;
    const uint8_t sc = mem.textColor();
    const uint32_t fg = kLoresPalette[sc >> 4];
    const uint32_t bg = kLoresPalette[sc & 0x0F];
    // Paint the band background across the full width first (glyph gaps → bg).
    for (int py = rowStart * 16; py < rowEnd * 16 && py < kH; ++py)
        for (int x = 0; x < kW; ++x) fb_[size_t(py) * kW + x] = bg;

    const bool col80 = mem.text80();
    const uint8_t* main = mem.slowRam();               // bank $E0
    const uint8_t* aux  = mem.slowRam() + 0x10000;     // bank $E1 (80-col even cols)
    const int page = mem.textPage2() ? 0x0800 : 0x0400;
    const int ncol = col80 ? 80 : 40;
    for (int rowc = rowStart; rowc < rowEnd; ++rowc) {
        int rbase = page + (rowc % 8) * 0x80 + (rowc / 8) * 0x28;
        for (int colc = 0; colc < ncol; ++colc) {
            uint8_t b = col80 ? ((colc & 1) ? main : aux)[rbase + colc / 2]
                              : main[rbase + colc];
            const uint8_t* glyph = &charRom_[(b * 8) & (charRom_.size() - 1)];
            for (int gy = 0; gy < 8; ++gy) {
                uint8_t bits = glyph[gy] & 0x7F;
                for (int gx = 0; gx < 7; ++gx) {
                    uint32_t c = (bits & (1 << gx)) ? fg : bg;
                    int py = rowc * 16 + gy * 2;
                    if (col80) {                        // 8-px cell, 1× glyph
                        int px = colc * 8 + gx;
                        fb_[size_t(py) * kW + px]       = c;
                        fb_[size_t(py + 1) * kW + px]   = c;
                    } else {                            // 16-px cell, 2× glyph
                        int px = colc * 16 + gx * 2;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                fb_[size_t(py + dy) * kW + (px + dx)] = c;
                    }
                }
            }
        }
    }
}

// //e address of text/lo-res row (interleaved) and hi-res row.
static inline int textRowBase(int row, int page2base) {
    return page2base + (row % 8) * 0x80 + (row / 8) * 0x28;
}
static inline int hgrRowBase(int y, int base) {
    return base + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

// Clean RGB HGR decode (6 colours, sharp) — pairs of consecutive bits, the
// byte's MSB selecting the palette bank. This is what the IIgs VGC's native
// RGB output (and Le Chat Mauve on //c/e) produces: no NTSC fringing.
// Palette: POM2 kChatMauveHGR (AppleWin "Feline" capture), 0xAABBGGRR.
static void decodeHgrRgbLine(const uint8_t* row, uint32_t* out /*[280]*/) {
    static const uint32_t pal[2][4] = {
        { 0xFF000000, 0xFFD11AAA, 0xFF2CE66F, 0xFFFFFFFF },   // MSB=0: black/magenta/green/white
        { 0xFF000000, 0xFFB58A00, 0xFF4772FF, 0xFFFFFFFF },   // MSB=1: black/blue/orange/white
    };
    uint8_t px[280]; uint8_t msb[40];
    for (int col = 0; col < 40; ++col) {
        uint8_t b = row[col]; msb[col] = (b >> 7) & 1;
        for (int bit = 0; bit < 7; ++bit) px[col * 7 + bit] = (b >> bit) & 1;
    }
    for (int p = 0; p < 280; p += 2) {
        unsigned code = px[p] | (px[p + 1] << 1);
        uint32_t c = pal[msb[p / 7]][code];
        out[p] = c; out[p + 1] = c;
    }
}

// ── Legacy hi-res 280×192 — composite NTSC or clean RGB (selectable) ────────
void VGC::renderHGR(const IIgsMemory& mem) {
    for (auto& px : fb_) px = 0xFF000000u;
    const uint8_t* e0 = mem.slowRam();
    const int base = mem.hgrPage2() ? 0x4000 : 0x2000;
    const int ox = (kW - 560) / 2;                 // centre 560-wide (280×2)
    for (int y = 0; y < 192; ++y) {
        uint32_t line[280];
        if (hgrMode_ == HgrMode::RgbClean) decodeHgrRgbLine(e0 + hgrRowBase(y, base), line);
        else                               pomiigs::ntsc::decodeHgrLine(e0 + hgrRowBase(y, base), line);
        for (int x = 0; x < 280; ++x) {            // ×2 horizontally, ×2 vertically
            uint32_t c = line[x];
            int px = x * 2 + ox, py = y * 2;
            fb_[size_t(py) * kW + px] = c;      fb_[size_t(py) * kW + px + 1] = c;
            fb_[size_t(py + 1) * kW + px] = c;  fb_[size_t(py + 1) * kW + px + 1] = c;
        }
    }
}

// ── Double Hi-Res 140×192 (16 colours) — composite NTSC or clean RGB ────────
// 80-column interleave: the leftmost 7 dots of each column come from aux
// memory (bank $E1), the next 7 from main (bank $E0). Same HgrMode toggle as
// HGR: fuzzy composite artifacts vs sharp 16-colour RGB.
void VGC::renderDHGR(const IIgsMemory& mem) {
    for (auto& px : fb_) px = 0xFF000000u;
    const uint8_t* main = mem.slowRam();               // bank $E0
    const uint8_t* aux  = mem.slowRam() + 0x10000;     // bank $E1
    const int base = mem.hgrPage2() ? 0x4000 : 0x2000;
    const int ox = (kW - 560) / 2;                     // centre 560-wide (280×2)
    for (int y = 0; y < 192; ++y) {
        const int rb = hgrRowBase(y, base);
        uint32_t line[280];
        if (hgrMode_ == HgrMode::RgbClean)
            pomiigs::ntsc::decodeDhgrRgbLine(aux + rb, main + rb, line);
        else
            pomiigs::ntsc::decodeDhgrLine(aux + rb, main + rb, line);
        for (int x = 0; x < 280; ++x) {                // ×2 horizontally, ×2 vertically
            uint32_t c = line[x];
            int px = x * 2 + ox, py = y * 2;
            fb_[size_t(py) * kW + px] = c;      fb_[size_t(py) * kW + px + 1] = c;
            fb_[size_t(py + 1) * kW + px] = c;  fb_[size_t(py + 1) * kW + px + 1] = c;
        }
    }
}

// ── Legacy lo-res 40×48 (16 colours) ─────────────────────────────────────
void VGC::renderLores(const IIgsMemory& mem) {
    const uint32_t* lut = kLoresPalette;   // 16-colour lo-res palette (shared)
    for (auto& px : fb_) px = 0xFF000000u;
    const uint8_t* e0 = mem.slowRam();
    const int page = mem.textPage2() ? 0x0800 : 0x0400;
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
        const bool fill    = (s & 0x20) != 0;      // SCB bit 5 = color-fill (320 only)
        const int palNum = s & 0x0F;
        const uint8_t* p = pal + palNum * 32;
        auto color = [&](int idx) { return rgb12(p[idx * 2], p[idx * 2 + 1]); };
        const uint8_t* src = e1 + 0x2000 + line * 160;

        uint32_t row[640];
        if (!mode640) {                            // 320: byte = 2 × 4-bit index, each dot doubled to 640
            // Color-fill: a pixel index of 0 repeats the previous pixel's colour
            // instead of palette[0] (fast horizontal runs). Seeds from palette[0].
            uint32_t last = color(0);
            for (int b = 0; b < 160; ++b) {
                uint8_t v = src[b];
                for (int half = 0; half < 2; ++half) {
                    int idx = half ? (v & 0x0F) : (v >> 4);
                    uint32_t c = (fill && idx == 0) ? last : color(idx);
                    if (!(fill && idx == 0)) last = c;
                    row[b * 4 + half * 2 + 0] = c;
                    row[b * 4 + half * 2 + 1] = c;
                }
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
    // $C022 SCREENCOLOR: fg = high nibble, bg = low nibble (16-colour palette).
    const uint8_t sc = mem.textColor();
    const uint32_t fg = kLoresPalette[sc >> 4];
    const uint32_t bg = kLoresPalette[sc & 0x0F];
    for (auto& px : fb_) px = bg;

    if (charRom_.empty()) return;      // authentic font required (roms/iigs-char.rom)

    const uint8_t* e0 = mem.slowRam();
    const int page = mem.textPage2() ? 0x0800 : 0x0400;
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

// ── 80-column text (aux/main interleaved) via the IIgs char ROM ──────────
// Each row's 40-byte line is split across the banks: the aux byte at offset k
// is column 2k (even, leftmost of the pair), the main byte at k is column
// 2k+1. Cells are 8 px wide (7-px glyph + 1-px gap) → 80×8 = 640.
void VGC::renderText80(const IIgsMemory& mem) {
    const uint8_t sc = mem.textColor();
    const uint32_t fg = kLoresPalette[sc >> 4];
    const uint32_t bg = kLoresPalette[sc & 0x0F];
    for (auto& px : fb_) px = bg;

    if (charRom_.empty()) return;

    const uint8_t* main = mem.slowRam();               // bank $E0 → odd columns
    const uint8_t* aux  = mem.slowRam() + 0x10000;     // bank $E1 → even columns
    const int page = mem.textPage2() ? 0x0800 : 0x0400;
    for (int rowc = 0; rowc < 24; ++rowc) {
        int rbase = page + (rowc % 8) * 0x80 + (rowc / 8) * 0x28;
        for (int colc = 0; colc < 80; ++colc) {
            const uint8_t* bank = (colc & 1) ? main : aux;   // even = aux, odd = main
            uint8_t b = bank[rbase + colc / 2];
            const uint8_t* glyph = &charRom_[(b * 8) & (charRom_.size() - 1)];
            for (int gy = 0; gy < 8; ++gy) {
                uint8_t bits = glyph[gy] & 0x7F;
                for (int gx = 0; gx < 7; ++gx) {
                    uint32_t c = (bits & (1 << gx)) ? fg : bg;
                    int px = colc * 8 + gx;              // 8-px cell, 7-px glyph
                    int py = rowc * 16 + gy * 2;
                    fb_[size_t(py) * kW + px] = c;
                    fb_[size_t(py + 1) * kW + px] = c;
                }
            }
        }
    }
}
