// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── VGC (Video Graphics Controller) ──────────────────────────────────────
// Renders the IIgs video memory to an RGBA framebuffer:
//   * Super Hi-Res (320/640 × 200) from $E1:2000 + SCB ($9D00) + palettes
//     ($9E00) — the IIgs signature mode.
//   * Legacy 40/80-column text from $E0/$E1 $0400 (//e interleaved layout),
//     drawn with the vendored 8x8 font.
// Legacy LORES/HGR/DHGR are staged in next (reuse POM2's decode).
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
    void renderSHR(const IIgsMemory& mem);
    void renderText(const IIgsMemory& mem);    // 40-column text
    void renderText80(const IIgsMemory& mem);  // 80-column text (aux/main interleaved)
    void renderHGR(const IIgsMemory& mem);    // legacy 280×192 hi-res
    void renderDHGR(const IIgsMemory& mem);   // double hi-res 140×192 (16 colour)
    void renderLores(const IIgsMemory& mem);  // legacy 40×48 lo-res
};

#endif // POMIIGS_VGC_H
