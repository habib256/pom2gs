// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Slow-side access penalty gate. In FAST mode ($C036 bit7 = 1) any access to
// the Mega II slow side (banks $E0/$E1, most $Cxxx I/O + language card of banks
// $00/$01, shadowed video writes) waits for PH0 and contains one full 14-tick
// cycle (16 on each line's last slot). At aligned phase zero, the first 64
// accesses cost +9 over a fast cycle and the 65th costs +11.

#include "IIgsMemory.h"
#include <cstdio>
#include <vector>

int main() {
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    const uint32_t slowRam = (uint32_t(0xE0) << 16) | 0x0000;   // bank $E0 RAM
    const uint32_t fastRam = (uint32_t(0x02) << 16) | 0x0000;   // bank $02 RAM
    const uint32_t io000   = (uint32_t(0x00) << 16) | 0xC000;   // $00:C000 (I/O, shadowed)
    const uint32_t txt1     = (uint32_t(0x00) << 16) | 0x0400;  // $00:0400 (shadowed text)
    auto io036 = [] { return uint32_t(0xE0) << 16 | 0xC036; };
    auto io035 = [] { return uint32_t(0xE0) << 16 | 0xC035; };

    int fails = 0;
    auto expect = [&](const char* what, int got, int want) {
        if (got != want) { std::printf("FAIL %s: %d != %d\n", what, got, want); ++fails; }
    };

    // ── FAST mode ────────────────────────────────────────────────────────
    mem.write8(io036(), 0x80);
    mem.takeSlowPenalty();                                   // drain the $C036 write itself

    for (int i = 0; i < 5; ++i) (void)mem.read8(slowRam);   // 5 slow reads = 45 master
    expect("5 slow RAM reads", mem.takeSlowPenalty(), 45);

    for (int i = 0; i < 5; ++i) (void)mem.read8(fastRam);   // fast side → no penalty
    expect("5 fast RAM reads", mem.takeSlowPenalty(), 0);

    for (int i = 0; i < 5; ++i) (void)mem.read8(io000);     // 5 I/O reads = slow
    expect("5 I/O reads", mem.takeSlowPenalty(), 45);

    mem.write8(io035(), 0x00); mem.takeSlowPenalty();       // shadow all ON, drain
    for (int i = 0; i < 5; ++i) mem.write8(txt1, 0x20);     // shadowed writes → slow
    expect("5 shadowed writes", mem.takeSlowPenalty(), 45);

    mem.write8(io035(), IIgsMemory::SHAD_TXTPG1); mem.takeSlowPenalty();  // inhibit text-pg1 shadow
    for (int i = 0; i < 5; ++i) mem.write8(txt1, 0x20);     // no longer shadowed → fast
    expect("5 unshadowed writes", mem.takeSlowPenalty(), 0);

    // 100 aligned slow reads cross one 16-tick line slot: 99*9 + 11 = 902.
    mem.write8(io035(), 0x00); mem.takeSlowPenalty();
    for (int i = 0; i < 100; ++i) (void)mem.read8(slowRam);
    expect("100 slow reads incl long slot", mem.takeSlowPenalty(), 902);

    // ── SLOW mode: no differential penalty ───────────────────────────────
    mem.write8(io036(), 0x00);
    mem.takeSlowPenalty();
    for (int i = 0; i < 100; ++i) (void)mem.read8(slowRam);
    expect("slow-mode slow reads", mem.takeSlowPenalty(), 0);

    // ── Disk II motor-detect drop: bit7 set but the FPI is at 1 MHz ──────
    // $C036 bit7 (fast) + bit2 (slot-6 motor detect). While the 5.25" motor
    // spins, speedFast() is false — the host loop bills every CPU cycle 14
    // master ticks — so there is NO fast/slow differential left to charge.
    // Gating chargeSlow() on the raw bit7 instead billed 14+9 = 23 master per
    // slow access and starved the RWTS read loop.
    mem.write8(io036(), 0x80 | 0x04);
    mem.takeSlowPenalty();
    if (!mem.speedFast()) { std::printf("FAIL motor-off should be fast\n"); ++fails; }
    for (int i = 0; i < 5; ++i) (void)mem.read8(slowRam);
    expect("motor off, fast", mem.takeSlowPenalty(), 45);

    (void)mem.read8(uint32_t(0xE0) << 16 | 0xC0E9);      // $C0E9: 5.25" motor ON
    mem.takeSlowPenalty();
    if (mem.speedFast()) { std::printf("FAIL motor-on should force 1 MHz\n"); ++fails; }
    for (int i = 0; i < 5; ++i) (void)mem.read8(slowRam);
    expect("motor on, forced 1 MHz", mem.takeSlowPenalty(), 0);

    (void)mem.read8(uint32_t(0xE0) << 16 | 0xC0E8);      // $C0E8: motor OFF again
    mem.takeSlowPenalty();
    for (int i = 0; i < 5; ++i) (void)mem.read8(slowRam);
    expect("motor off again, fast", mem.takeSlowPenalty(), 45);

    if (fails) { std::printf("slowside_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: phase-aware slow-side penalty (including 16-tick line slot)\n");
    return 0;
}
