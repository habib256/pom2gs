// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// SPEED register ($C036 bit7) gate. The per-frame CPU cycle budget must follow
// the fast/slow bit so legacy //e software that selects slow mode runs at
// 1.02 MHz instead of the old fixed 2.8 MHz budget (which was ~2.7× too fast).
//
//   fast (bit7=1) → ROM-only ceiling 47789 cyc/frame
//   slow (bit7=0) → 1.02 MHz → 17030 cyc/frame   (= 262 lines × 65 cycles)
// The fast number uses the exact 912×262 master-tick frame. DRAM refresh and
// side-sync stalls reduce the number actually executed by the host loop.

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
    if (fast != 47789) { std::printf("FAIL fast budget %d != 47789\n", fast); ++fails; }

    mem.write8(io036(), 0x00);                 // back to slow
    const int slow = mem.frameCycleBudget();
    if (slow != 17030) { std::printf("FAIL slow budget %d != 17030\n", slow); ++fails; }

    if (mem.masterPerFrame() != 238944) { std::printf("FAIL frame ticks %ld != 238944\n", mem.masterPerFrame()); ++fails; }

    // A low slot-motor bit (bits 0-3) must not flip the speed selection.
    mem.write8(io036(), 0x0F);
    if (mem.frameCycleBudget() != 17030) { std::printf("FAIL low nibble affected speed\n"); ++fails; }
    mem.write8(io036(), 0x8F);
    if (mem.frameCycleBudget() != 47789) { std::printf("FAIL fast+lownibble\n"); ++fails; }

    if (fails) { std::printf("speed_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: frame budget follows $C036 (fast ceiling 47789 / slow 17030; 238944 master ticks)\n");
    return 0;
}
