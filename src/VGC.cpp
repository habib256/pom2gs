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

// ── Character-generator lookup ───────────────────────────────────────────
// The 16 KB IIgs char ROM (344s0047) is eight 2 KB banks — one localisation
// each, selected by $C02B LANGSEL bits 7-5 — and every bank holds the 256
// glyphs of the //e ALTERNATE character set: $00-$3F inverse, $40-$5F
// MouseText, $60-$7F inverse lowercase, $80-$FF normal.
//
// With ALTCHARSET OFF ($C00E — the //e PRIMARY set, and the power-on state)
// codes $40-$7F are not MouseText at all: they are the FLASHING range, drawn
// as the inverse glyph ($00-$3F) and the normal glyph ($80-$BF) on alternate
// flash phases. KEGS video.c:1086-1094 does exactly this remap (`val += ±0x40`).
// POMIIGS used to index the ROM with the raw code, so every program that
// printed flashing text — Applesoft FLASH, the DOS/ProDOS prompts, countless
// game menus — drew MouseText glyphs instead of letters.
// (bug-hunt finding, August 2026.)
const uint8_t* VGC::glyph(uint8_t code, bool altChar, int langBank) const {
    if (!altChar && code >= 0x40 && code < 0x80)
        code = uint8_t(code + (flashOn() ? 0x40 : 0xC0));   // +$40 normal / −$40 inverse
    // Language bank (16 KB ROM only; the 4 KB/2 KB //e-style ROMs hold one set).
    const size_t bank = (charRom_.size() >= 0x4000) ? size_t(langBank) * 0x800 : 0;
    return &charRom_[(bank + size_t(code) * 8) % charRom_.size()];
}

// ── Frame assembly from the live scanout ─────────────────────────────────
// Every emulated scanline was captured by IIgsMemory as the beam crossed it
// (mode switches, colours, SCB/palette, and the bytes the VGC fetched at each
// display cycle). Drawing line by line from that capture is what makes
// mid-frame palette splits, per-line mode switches and beam-race text
// rewrites come out the way the CRT shows them. A caller that never ran the
// clock (unit tests, tools) gets a full-frame snapshot of current memory.
const uint32_t* VGC::render(const IIgsMemory& mem) {
    ++frameCount_;                     // drives the text flash phase
    if (!mem.scanLive()) mem.scanSnapshot();
    mem.scanConsume();
    using SL = IIgsMemory::ScanLine;
    for (int line = 0; line < IIgsMemory::kScanLines; ++line) {
        const SL& sl = mem.scanLine(line);
        if (sl.flags & SL::SHR)        { drawShrLine(mem, line); continue; }
        if (line >= 192) {             // legacy modes: 192 lines, the rest is border/blank
            const uint32_t bg = (sl.flags & SL::TEXT) ? kLoresPalette[sl.textColor & 0x0F] : 0xFF000000u;
            fillLine(line, bg); continue;
        }
        if (sl.flags & SL::TEXT)       drawTextLine(mem, line);
        else if (sl.flags & SL::DHGR)  drawDhgrLine(mem, line);
        else if (sl.flags & SL::HIRES) drawHgrLine(mem, line);
        else                           drawLoresLine(mem, line);
    }
    return fb_.data();
}

void VGC::fillLine(int line, uint32_t colour) {
    for (int yy = 0; yy < 2; ++yy) {
        uint32_t* dst = &fb_[size_t(line * 2 + yy) * kW];
        for (int x = 0; x < kW; ++x) dst[x] = colour;
    }
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

// Blit a 280-dot legacy graphics line, doubled to 560 and centred, ×2 vertically.
static void blit280(std::vector<uint32_t>& fb, int line, const uint32_t* row280) {
    const int ox = (VGC::kW - 560) / 2;
    for (int yy = 0; yy < 2; ++yy) {
        uint32_t* dst = &fb[size_t(line * 2 + yy) * VGC::kW];
        for (int x = 0; x < ox; ++x) dst[x] = 0xFF000000u;
        for (int x = 0; x < 280; ++x) { dst[ox + x * 2] = row280[x]; dst[ox + x * 2 + 1] = row280[x]; }
        for (int x = ox + 560; x < VGC::kW; ++x) dst[x] = 0xFF000000u;
    }
}

// ── Legacy hi-res 280-dot line — composite NTSC or clean RGB (selectable) ──
void VGC::drawHgrLine(const IIgsMemory& mem, int line) {
    const IIgsMemory::ScanLine& sl = mem.scanLine(line);
    uint32_t row[280];
    if (hgrMode_ == HgrMode::RgbClean) decodeHgrRgbLine(sl.main, row);
    else                               pomiigs::ntsc::decodeHgrLine(sl.main, row);
    blit280(fb_, line, row);
}

// ── Double Hi-Res line (16 colours) — aux bytes are the left 7 dots of each
// column, main the right 7. Same HgrMode toggle as HGR.
void VGC::drawDhgrLine(const IIgsMemory& mem, int line) {
    const IIgsMemory::ScanLine& sl = mem.scanLine(line);
    uint32_t row[280];
    if (hgrMode_ == HgrMode::RgbClean) pomiigs::ntsc::decodeDhgrRgbLine(sl.aux, sl.main, row);
    else                               pomiigs::ntsc::decodeDhgrLine(sl.aux, sl.main, row);
    blit280(fb_, line, row);
}

// ── Legacy lo-res line (16 colours): each byte is a 2-block column, the low
// nibble for the top 4 scanlines of a text row, the high one for the bottom 4.
void VGC::drawLoresLine(const IIgsMemory& mem, int line) {
    const IIgsMemory::ScanLine& sl = mem.scanLine(line);
    const bool bottom = (line & 7) >= 4;
    for (int yy = 0; yy < 2; ++yy) {
        uint32_t* dst = &fb_[size_t(line * 2 + yy) * kW];
        for (int col = 0; col < 40; ++col) {
            const uint8_t v = sl.main[col];
            const uint32_t c = kLoresPalette[bottom ? (v >> 4) : (v & 0x0F)];
            for (int dx = 0; dx < 16; ++dx) dst[col * 16 + dx] = c;
        }
    }
}

// ── Super Hi-Res line (320/640) from the captured SCB, palette and 160 bytes ─
void VGC::drawShrLine(const IIgsMemory& mem, int line) {
    const IIgsMemory::ScanLine& sl = mem.scanLine(line);
    const bool mode640 = (sl.scb & 0x80) != 0;     // SCB bit 7 = 640 mode
    const bool fill    = (sl.scb & 0x20) != 0;     // SCB bit 5 = colour-fill (320 only)
    auto color = [&](int idx) { return rgb12(sl.pal[idx * 2], sl.pal[idx * 2 + 1]); };
    uint32_t row[640];
    if (!mode640) {                                // 320: byte = 2 × 4-bit index, each dot doubled
        // Colour-fill: index 0 repeats the previous pixel's colour instead of
        // palette[0] (fast horizontal runs). Seeds from palette[0].
        uint32_t last = color(0);
        for (int b = 0; b < 160; ++b) {
            const uint8_t v = sl.shr[b];
            for (int half = 0; half < 2; ++half) {
                const int idx = half ? (v & 0x0F) : (v >> 4);
                const uint32_t c = (fill && idx == 0) ? last : color(idx);
                if (!(fill && idx == 0)) last = c;
                row[b * 4 + half * 2] = c; row[b * 4 + half * 2 + 1] = c;
            }
        }
    } else {                                       // 640: byte = 4 × 2-bit, column-offset palette
        static const int off[4] = { 8, 12, 0, 4 };
        for (int b = 0; b < 160; ++b) {
            const uint8_t v = sl.shr[b];
            for (int d = 0; d < 4; ++d) row[b * 4 + d] = color(off[d] + ((v >> ((3 - d) * 2)) & 0x03));
        }
    }
    for (int yy = 0; yy < 2; ++yy) {
        uint32_t* dst = &fb_[size_t(line * 2 + yy) * kW];
        for (int x = 0; x < 640; ++x) dst[x] = row[x];
    }
}

// ── Text line, 40 or 80 columns, via the IIgs char ROM ───────────────────
// 40 columns: 16-px cells, glyph dots doubled. 80 columns: 8-px cells (7-px
// glyph + 1-px gap), the aux byte of a pair is the even (left) column, the
// main byte the odd one. $C022 SCREENCOLOR: fg = high nibble, bg = low.
void VGC::drawTextLine(const IIgsMemory& mem, int line) {
    const IIgsMemory::ScanLine& sl = mem.scanLine(line);
    const uint32_t fg = kLoresPalette[sl.textColor >> 4];
    const uint32_t bg = kLoresPalette[sl.textColor & 0x0F];
    if (charRom_.empty()) { fillLine(line, bg); return; }   // authentic font required (roms/iigs-char.rom)
    const bool col80 = (sl.flags & IIgsMemory::ScanLine::COL80) != 0;
    const bool altChar = (sl.flags & IIgsMemory::ScanLine::ALTCHAR) != 0;
    const int gy = line & 7;
    uint32_t row[640];
    const int ncol = col80 ? 80 : 40;
    for (int colc = 0; colc < ncol; ++colc) {
        const uint8_t code = col80 ? ((colc & 1) ? sl.main : sl.aux)[colc / 2] : sl.main[colc];
        const uint8_t bits = glyph(code, altChar, sl.langBank)[gy] & 0x7F;
        if (col80) {
            for (int gx = 0; gx < 7; ++gx) row[colc * 8 + gx] = (bits & (1 << gx)) ? fg : bg;
            row[colc * 8 + 7] = bg;
        } else {
            for (int gx = 0; gx < 7; ++gx) { const uint32_t c = (bits & (1 << gx)) ? fg : bg; row[colc * 16 + gx * 2] = c; row[colc * 16 + gx * 2 + 1] = c; }
            row[colc * 16 + 14] = bg; row[colc * 16 + 15] = bg;
        }
    }
    for (int yy = 0; yy < 2; ++yy) {
        uint32_t* dst = &fb_[size_t(line * 2 + yy) * kW];
        for (int x = 0; x < 640; ++x) dst[x] = row[x];
    }
}
