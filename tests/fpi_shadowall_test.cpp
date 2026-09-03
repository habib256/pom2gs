// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// FPI/Mega II memory-management cases pinned from the MiSTer Apple IIgs
// custom `mmu_test` (hardware-verified expectations): CYAREG shadow-all-banks
// ($C036 b4), NEWVIDEO bank latch ($C029 b0), the floating bus, and the
// language card's physical layout under IOLC inhibit.

#include "IIgsMemory.h"

#include <cstdio>
#include <vector>

int main() {
    int fails = 0;
    auto expect = [&](const char* what, unsigned got, unsigned want) {
        if (got != want) { std::printf("FAIL %-52s $%02X != $%02X\n", what, got, want); ++fails; }
    };
    IIgsMemory m;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    m.loadRom(rom);
    m.reset();
    auto W = [&](uint32_t a, uint8_t v) { m.write8(a, v); m.takeBusPenalty(); };
    auto R = [&](uint32_t a) { uint8_t v = m.read8(a); m.takeBusPenalty(); return v; };
    W(0xE0C035, 0x08);                                     // SHR shadow inhibited, as under ProDOS/GS-OS text mode

    // mmu_test 03: shadow-all → an even bank's text page shadows to $E0
    W(0xE0C036, 0x94); W(0x020402, 0x56); W(0x020403, 0x78); W(0xE0C036, 0x84);
    expect("shadow-all: $02:0402 → $E0:0402", R(0xE00402), 0x56);
    expect("shadow-all: $02:0403 → $E0:0403", R(0xE00403), 0x78);
    // mmu_test 09: an odd bank's text page shadows to $E1
    W(0xE10400, 0); W(0xE0C036, 0x94); W(0x030400, 0x56); W(0xE0C036, 0x84);
    expect("shadow-all: $03:0400 → $E1:0400", R(0xE10400), 0x56);
    // mmu_test 07: RAMWRT redirects an even bank's write to the odd bank; $6000 is no video page
    W(0xE16000, 0); W(0x026000, 0); W(0x036000, 0);
    W(0xE0C036, 0x94); W(0xE0C005, 1); W(0x026000, 0x56); W(0xE0C004, 1); W(0xE0C036, 0x84);
    expect("shadow-all + RAMWRT: $02:6000 lands in $03", R(0x036000), 0x56);
    expect("shadow-all + RAMWRT: $02:6000 untouched", R(0x026000), 0x00);
    expect("shadow-all + RAMWRT: $E1:6000 not a video page", R(0xE16000), 0x00);
    // mmu_test 11/12: no IOLC in bank $02 normally, IOLC with shadow-all
    W(0x02C010, 0x00);
    W(0x02C010, 0x12);
    expect("no shadow-all: $02:C010 is plain RAM", R(0x02C010), 0x12);
    W(0x02C010, 0x00); W(0xE0C036, 0x94); W(0x02C010, 0x12); W(0xE0C036, 0x84);
    expect("shadow-all: $02:C010 is I/O, RAM untouched", R(0x02C010), 0x00);
    // mmu_test 13: bank latch off → $E1 RAM access lands in $E0
    W(0xE06000, 0); W(0xE16000, 0); W(0xE0C029, 0x00); W(0xE16000, 0x12); W(0xE0C029, 0x01);
    expect("bank latch off: $E1:6000 → $E0:6000", R(0xE06000), 0x12);
    expect("bank latch off: $E1:6000 untouched", R(0xE16000), 0x00);
    // mmu_test 25: floating bus above real RAM = last byte on the data bus
    W(0x000000, 0x81);                                     // the bus carried $81 last
    expect("floating bus keeps the last data byte", R(0x810000), 0x81);
    R(0x000001);                                           // reads $00 (cleared RAM)
    expect("floating bus follows the bus", R(0x820000), 0x00);
    // mmu_test 26: LC bank 2 is the primary $D000 block, bank 1 folds to $C000
    R(0xE0C083); R(0xE0C083); W(0x00D000, 0x5A);           // bank 2, write-enabled
    R(0xE0C08B); R(0xE0C08B); W(0x00D000, 0xA5);           // bank 1
    W(0xE0C035, 0x7F);                                     // IOLC inhibit → linear 64K
    expect("IOLC inhibit: linear $00:D000 = LC bank 2", R(0x00D000), 0x5A);
    expect("IOLC inhibit: linear $00:C000 = LC bank 1", R(0x00C000), 0xA5);
    W(0xE0C035, 0x08); R(0xE0C082);

    if (fails) { std::printf("fpi_shadowall_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: shadow-all banks, bank latch, floating bus, LC layout\n");
    return 0;
}
