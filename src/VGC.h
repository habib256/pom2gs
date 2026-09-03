// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── VGC (Video Graphics Controller) ──────────────────────────────────────
// Renders the IIgs video memory to an RGBA framebuffer:
//   * Super Hi-Res (320/640 × 200) from $E1:2000 + SCB ($9D00) + palettes
//     ($9E00) — the IIgs signature mode.
//   * Legacy 40/80-column text from $E0/$E1 $0400 (//e interleaved layout),
//     drawn with the AUTHENTIC Mega II character ROM the user supplies as
//     roms/iigs-char.rom (344s0047, 16 KB; a 4 KB or 2 KB //e generator also
//     loads). NO font is vendored — with no char ROM, text is skipped.
//   * Legacy LORES (40×48), HGR (280×192) and DHGR (140×192, 16 colour),
//     dispatched by the //e mode switches; NTSC composite artifact decode in
//     VGCNtsc.h (POM2 port), or clean RGB.
//
// Output is a fixed 640×400 buffer (SHR 200 lines doubled; text 24×16 rows).
// Source of truth: Apple IIgs Hardware Reference (VGC) + MAME apple2gs video.

#ifndef POMIIGS_VGC_H
#define POMIIGS_VGC_H

#include <cstdint>
#include <vector>

class IIgsMemory;

class VGC
{
public:
    static constexpr int kW = 640;
    static constexpr int kH = 400;

    // HGR colour rendering: composite NTSC artifact (fuzzy, OpenEmulator-style)
    // or clean RGB (sharp 6-colour, like the IIgs VGC's native RGB output).
    enum class HgrMode { CompositeNtsc, RgbClean };

    VGC() : fb_(size_t(kW) * kH, 0xFF000000u) {}

    void setHgrMode(HgrMode m) { hgrMode_ = m; }
    HgrMode hgrMode() const { return hgrMode_; }
    void toggleHgrMode() { hgrMode_ = (hgrMode_ == HgrMode::CompositeNtsc) ? HgrMode::RgbClean : HgrMode::CompositeNtsc; }

    // Load the authentic Apple IIgs character generator (Mega II ROM
    // 344s0047, 16 KB — user-provided as roms/iigs-char.rom, like the main
    // ROM). Text rendering is skipped until this is present. Returns false on
    // an unexpected size.
    bool setCharRom(const std::vector<uint8_t>& rom);
    bool hasCharRom() const { return !charRom_.empty(); }

    // Render the current frame from video memory. Returns the RGBA buffer
    // (0xAABBGGRR little-endian, kW*kH pixels).
    const uint32_t* render(const IIgsMemory& mem);

    int width()  const { return kW; }
    int height() const { return kH; }
    const uint32_t* framebuffer() const { return fb_.data(); }

    // The 16-colour lo-res / border / text palette entry (0xAABBGGRR). Shared
    // with the UI so it can draw the $C034 border in the authentic colour.
    static uint32_t loresColor(uint8_t idx);

private:
    std::vector<uint32_t> fb_;
    std::vector<uint8_t>  charRom_;   // Mega II 344s0047 (16 KB)
    HgrMode hgrMode_ = HgrMode::CompositeNtsc;
    // Text flash phase. The //e inverts flashing characters roughly every 16
    // fields (KEGS video.c:639-643 g_flash_count >= 16), i.e. ~1.9 Hz at 60 Hz.
    // Advanced once per render() — the renderer runs one frame per host frame.
    uint32_t frameCount_ = 0;
    bool flashOn() const { return ((frameCount_ / 16) & 1) != 0; }
    // Character code → char-ROM glyph, honouring ALTCHARSET and $C02B LANGSEL
    // as captured for the line being drawn.
    const uint8_t* glyph(uint8_t code, bool altChar, int langBank) const;
    // Per-scanline decoders, all drawing from IIgsMemory::ScanLine captures
    // (see IIgsMemory.h § Live scanout). `line` is the emulated scanline
    // (0-199); every capture line paints framebuffer rows 2*line and 2*line+1.
    void drawTextLine(const IIgsMemory& mem, int line);
    void drawLoresLine(const IIgsMemory& mem, int line);
    void drawHgrLine(const IIgsMemory& mem, int line);
    void drawDhgrLine(const IIgsMemory& mem, int line);
    void drawShrLine(const IIgsMemory& mem, int line);
    void fillLine(int line, uint32_t colour);
};

#endif // POMIIGS_VGC_H
