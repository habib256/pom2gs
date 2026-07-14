// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Regression gate for the 80STORE display-page quirk (Sather IIe §5-25 table
// 5.10). Total Replay's DHGR title fades set 80STORE + HIRES and toggle PAGE2
// to steer aux/main writes — the video scanner must keep displaying page 1,
// not jump to page 2. Before the fix, renderDHGR read the page purely from
// PAGE2 and showed page-2 garbage during the fade.
//
// Setup: put the MMU in DHGR (graphics + HIRES + DHIRES + 80COL + 80STORE),
// fill hi-res page 1 (main $E0:2000 + aux $E1:2000) with $7F (→ white DHGR)
// and leave page 2 ($4000) black, then assert the rendered frame is bright
// with PAGE2=1. A raw-PAGE2 renderer would show black.

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

    // Live slow-side I/O is always reachable at $E0:C0xx regardless of shadow.
    auto io = [&](uint16_t reg) { return uint32_t(0xE0) << 16 | (0xC000 | reg); };
    mem.write8(io(0x050), 0);   // TXTCLR  → graphics
    mem.write8(io(0x057), 0);   // HIRES on
    mem.write8(io(0x05E), 0);   // DHIRES on
    mem.write8(io(0x00D), 0);   // 80COL on
    mem.write8(io(0x001), 0);   // 80STORE on

    // Fill hi-res page 1 across both banks: PAGE2 selects aux vs main under
    // 80STORE, so write once with each to cover aux ($E1) and main ($E0).
    for (int pass = 0; pass < 2; ++pass) {
        mem.write8(io(pass ? 0x055 : 0x054), 0);           // PAGE2 on / off
        for (uint16_t off = 0x2000; off < 0x4000; ++off)
            mem.write8((uint32_t(0x00) << 16) | off, 0x7F);// $7F → solid white DHGR
    }
    // Page 2 ($4000-$5FFF) stays 0 (black) in both banks.

    // Leave PAGE2 = 1 (the fade state that used to break): scanner must still
    // show page 1 (white), not page 2 (black).
    mem.write8(io(0x055), 0);

    VGC vgc;
    vgc.setHgrMode(VGC::HgrMode::RgbClean);
    const uint32_t* fb = vgc.render(mem);

    // Sample a mid-screen row; count bright pixels.
    int bright = 0, total = 0;
    const int y = 100 * vgc.width();
    for (int x = vgc.width() / 4; x < vgc.width() * 3 / 4; ++x) {
        uint32_t p = fb[y + x];
        int lum = (p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF);
        if (lum > 400) ++bright;
        ++total;
    }
    std::printf("PAGE2=1 under 80STORE: bright=%d/%d\n", bright, total);
    if (bright < total * 3 / 4) {
        std::printf("FAIL: display followed PAGE2 to page 2 (black) — 80STORE quirk broken\n");
        return 1;
    }
    std::printf("OK: 80STORE keeps DHGR display on page 1 (fade fix)\n");
    return 0;
}
