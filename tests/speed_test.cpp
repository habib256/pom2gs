// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// SPEED register ($C036 bit7) gate. The per-frame CPU cycle budget must follow
// the fast/slow bit so legacy //e software that selects slow mode runs at
// 1.02 MHz instead of the old fixed 2.8 MHz budget (which was ~2.7× too fast).
//
//   fast (bit7=1) → 2.8 MHz → 47684 cyc/frame
//   slow (bit7=0) → 1.02 MHz → 17030 cyc/frame   (= 262 lines × 65 cycles)
//   ratio must be exactly 14/5 (the FPI : Mega II clock divisor ratio).

#include "IIgsMemory.h"
#include <cstdio>
#include <vector>

int main() {
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    auto io036 = [] { return uint32_t(0xE0) << 16 | 0xC036; };
    int fails = 0;

    // Reset default is slow (speed_ = 0) — the ROM raises it to fast on boot.
    const int slow0 = mem.frameCycleBudget();
    if (slow0 != 17030) { std::printf("FAIL reset budget %d != 17030\n", slow0); ++fails; }

    mem.write8(io036(), 0x80);                 // SPEED_HIGH → fast
    const int fast = mem.frameCycleBudget();
    if (fast != 47684) { std::printf("FAIL fast budget %d != 47684\n", fast); ++fails; }

    mem.write8(io036(), 0x00);                 // back to slow
    const int slow = mem.frameCycleBudget();
    if (slow != 17030) { std::printf("FAIL slow budget %d != 17030\n", slow); ++fails; }

    // Exact 2.8× (14/5) ratio: fast·5 == slow·14.
    if (fast * 5 != slow * 14) { std::printf("FAIL ratio: %d vs %d\n", fast * 5, slow * 14); ++fails; }

    // A low slot-motor bit (bits 0-3) must not flip the speed selection.
    mem.write8(io036(), 0x0F);
    if (mem.frameCycleBudget() != 17030) { std::printf("FAIL low nibble affected speed\n"); ++fails; }
    mem.write8(io036(), 0x8F);
    if (mem.frameCycleBudget() != 47684) { std::printf("FAIL fast+lownibble\n"); ++fails; }

    if (fails) { std::printf("speed_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: frame budget follows $C036 (fast 47684 / slow 17030, 2.8x)\n");
    return 0;
}
