// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Pure 14.318 MHz timing helpers shared by the FPI/Mega II bus model and its
// deterministic tests.  Keeping the arithmetic here makes the phase contract
// explicit instead of spreading another set of 65*14 approximations around
// the MMU. Source contract: MAME 0.287 apple2gs.cpp:19-29.

#ifndef POMIIGS_MEGA2TIMING_H
#define POMIIGS_MEGA2TIMING_H

#include <cstdint>

namespace Mega2Timing {

static constexpr uint32_t kNormalSlowTicks = 14;
static constexpr uint32_t kLongSlowTicks   = 16;
static constexpr uint32_t kSlowCyclesPerLine = 65;
static constexpr uint32_t kLinesPerFrame   = 262;
static constexpr uint32_t kMasterPerLine =
    64 * kNormalSlowTicks + kLongSlowTicks;                 // 912
static constexpr uint32_t kMasterPerFrame =
    kMasterPerLine * kLinesPerFrame;                        // 238944

static constexpr uint32_t kFastCycleTicks = 5;
static constexpr uint32_t kRefreshPeriodTicks = 50;
static constexpr uint32_t kRefreshTicks = 5;

// Index of the active Mega II cycle (0..64).  The final cycle occupies master
// phases 896..911; a plain phase/14 would incorrectly invent cycle 65 for its
// last two stretched ticks.
inline uint32_t horizontalCycle(uint64_t masterTick) {
    const uint32_t phase = uint32_t(masterTick % kMasterPerLine);
    const uint32_t cycle = phase / kNormalSlowTicks;
    return cycle < kSlowCyclesPerLine ? cycle : kSlowCyclesPerLine - 1;
}

// First Mega II PH0 boundary at or after masterTick.  There are 64 regular
// 14-tick intervals followed by one 16-tick interval per scanline.
inline uint64_t nextSlowCycleStart(uint64_t masterTick) {
    const uint64_t line = masterTick / kMasterPerLine;
    const uint32_t phase = uint32_t(masterTick % kMasterPerLine);
    if (phase <= 64 * kNormalSlowTicks) {
        const uint32_t slot = (phase + kNormalSlowTicks - 1) / kNormalSlowTicks;
        if (slot <= 64) return line * kMasterPerLine + slot * kNormalSlowTicks;
    }
    return (line + 1) * kMasterPerLine;
}

inline uint32_t slowCycleWidth(uint64_t cycleStart) {
    return (cycleStart % kMasterPerLine) == 64 * kNormalSlowTicks
             ? kLongSlowTicks : kNormalSlowTicks;
}

// Wall time needed for a CPU/bus cycle which must contain one complete Mega II
// cycle.  An already aligned request takes 14 ticks (16 on the long slot); an
// asynchronous fast-side request waits for the next PH0 boundary first.
inline uint32_t slowCycleTicks(uint64_t masterTick) {
    const uint64_t start = nextSlowCycleStart(masterTick);
    return uint32_t(start - masterTick) + slowCycleWidth(start);
}

inline uint64_t slowCyclesTicks(uint64_t masterTick, uint32_t cycles) {
    const uint64_t begin = masterTick;
    while (cycles-- != 0) masterTick += slowCycleTicks(masterTick);
    return masterTick - begin;
}

// Reset anchors refresh so nine native 5-tick cycles complete before the
// first five-tick refresh window (phases 45..49 of each 50-tick period).  A
// DRAM transaction needs five usable ticks; if the periodic refresh intersects
// it, the unavailable portion stretches that transaction.  ROM, FPI registers
// and Mega II-side cycles call a different path and therefore hide refresh.
inline uint32_t fastDramCycleTicks(uint64_t masterTick) {
    const uint32_t phase = uint32_t(masterTick % kRefreshPeriodTicks);
    const uint32_t refreshStart = kRefreshPeriodTicks - kRefreshTicks; // 45
    if (phase >= refreshStart)
        return (kRefreshPeriodTicks - phase) + kFastCycleTicks;
    if (phase + kFastCycleTicks > refreshStart)
        return kFastCycleTicks + kRefreshTicks;
    return kFastCycleTicks;
}

static_assert(kMasterPerLine == 912, "Mega II scanline cadence changed");
static_assert(kMasterPerFrame == 238944, "Mega II frame cadence changed");

} // namespace Mega2Timing

#endif // POMIIGS_MEGA2TIMING_H
