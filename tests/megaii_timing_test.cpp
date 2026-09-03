// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Master-clock timing gate: exact NTSC line/frame cadence, phase-aware PH0
// side sync, and conditional DRAM refresh.  These checks deliberately use raw
// bus accesses too, so failures identify the MMU timing classifier separately
// from the instruction-level CPU core.

#include "IIgsMemory.h"
#include "Mega2Timing.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    int fails = 0;
    auto expect = [&](const char* what, uint64_t got, uint64_t want) {
        if (got != want) {
            std::printf("FAIL %-42s %llu != %llu\n", what,
                        (unsigned long long)got, (unsigned long long)want);
            ++fails;
        }
    };

    expect("master ticks per line", Mega2Timing::kMasterPerLine, 912);
    expect("master ticks per frame", Mega2Timing::kMasterPerFrame, 238944);
    expect("65 aligned slow cycles", Mega2Timing::slowCyclesTicks(0, 65), 912);
    expect("long horizontal slot at phase 910", Mega2Timing::horizontalCycle(910), 64);
    expect("fast->slow sync from phase 5", Mega2Timing::slowCycleTicks(5), 23);
    expect("long slow cycle at phase 896", Mega2Timing::slowCycleTicks(896), 16);
    expect("25-line PH0/FPI realignment", Mega2Timing::slowCycleTicks(22800), 14);

    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    const uint32_t speed = 0xE0C036;
    const uint32_t slowRam = 0xE00000;
    const uint32_t fastRam = 0x020000;
    const uint32_t fpiShadow = 0x00C035;
    const uint32_t romByte = 0xFC0000;

    mem.reset();
    expect("tick(64) before long slot", mem.tick(64), 896);
    expect("beam remains on line zero", mem.vpos(), 0);
    expect("65th slow cycle is stretched", mem.tick(1), 16);
    expect("beam enters line one", mem.vpos(), 1);
    expect("master clock after one line", mem.masterTicks(), 912);

    // A fast request five ticks into the line waits to PH0=14, then occupies
    // the complete 14-tick Mega II cycle: 23 total, hence +18 over fast.
    mem.reset();
    mem.write8(speed, 0x80); mem.takeBusPenalty();
    expect("one free-running fast cycle", mem.tick(1), 5);
    (void)mem.read8(slowRam);
    expect("side-sync is included by tick", mem.tick(1), 23);
    expect("tick consumes the bus penalty", mem.takeBusPenalty(), 0);

    // The 65th slow slot itself is 16 ticks, so an aligned fast-side request
    // is charged +11 rather than the ordinary +9.
    mem.reset();
    mem.tick(64);                            // phase 896
    mem.write8(speed, 0x80); mem.takeBusPenalty();
    (void)mem.read8(slowRam);
    expect("side-sync penalty on long slot", mem.takeBusPenalty(), 11);

    // Nine fast DRAM cycles fit before refresh; the tenth overlaps the
    // five-tick window and is stretched from 5 to 10 ticks.
    mem.reset();
    mem.write8(speed, 0x80); mem.takeBusPenalty();
    for (int i = 0; i < 10; ++i) (void)mem.read8(fastRam);
    expect("ten fast DRAM cycles include refresh", mem.tick(10), 55);
    expect("tick consumes refresh penalty", mem.takeBusPenalty(), 0);

    // ROM and the selected FPI registers hide that same refresh window.
    for (int i = 0; i < 10; ++i) (void)mem.read8(romByte);
    expect("ROM hides refresh", mem.takeBusPenalty(), 0);
    for (int i = 0; i < 10; ++i) (void)mem.read8(fpiShadow);
    expect("FPI register hides refresh", mem.takeBusPenalty(), 0);

    // A slow-side access which lands on refresh pays only PH0 synchronization;
    // refresh is hidden by the longer Mega II transaction.
    mem.reset();
    mem.write8(speed, 0x80); mem.takeBusPenalty();
    for (int i = 0; i < 9; ++i) (void)mem.read8(fastRam); // cursor = phase 45
    (void)mem.read8(slowRam);                            // 45 -> PH0 56 -> 70
    expect("slow side hides refresh", mem.takeBusPenalty(), 20);

    // A speed-register write is timed at the speed in force when the bus cycle
    // began; only the following cycle observes the new mode.
    mem.reset();
    mem.write8(speed, 0x80);
    expect("$C036 write completes at old speed", mem.tick(1), 14);
    expect("next cycle observes fast speed", mem.tick(1), 5);

    if (fails) { std::printf("megaii_timing_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: 912-tick scanline + PH0 side-sync + conditional DRAM refresh\n");
    return 0;
}
