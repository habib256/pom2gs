// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Reset classes: /RESET keeps RAM, the master clock and the battery-backed
// state while re-parking every soft switch; a power cycle also clears RAM.

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <cstdio>
#include <vector>

int main() {
    int fails = 0;
    auto expect = [&](const char* what, long got, long want) {
        if (got != want) { std::printf("FAIL %-48s %ld != %ld\n", what, got, want); ++fails; }
    };
    IIgsMemory mem;
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    rom[0x3FFFC] = 0x00; rom[0x3FFFD] = 0xD0;             // reset vector → $D000 (bank $FF image at +$30000)
    mem.loadRom(rom);
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    mem.reset();
    auto W = [&](uint32_t a, uint8_t v) { mem.write8(a, v); mem.takeBusPenalty(); };
    auto R = [&](uint32_t a) { uint8_t v = mem.read8(a); mem.takeBusPenalty(); return v; };

    // Dirty the machine: RAM, soft switches, LC, speed, shadow, DISKREG, clock.
    W(0x001234, 0xA5); W(0x021234, 0x5A); W(0xE00400, 0xC1); W(0xE10400, 0xC2);
    W(0xE0C036, 0x80); W(0xE0C035, 0x7F); W(0xE0C00D, 0); W(0xE0C050, 0); W(0xE0C031, 0xC0);
    R(0xE0C083); R(0xE0C083); W(0x00D000, 0x77);
    mem.tick(1000);
    const uint64_t clock = mem.masterTicks();
    cpu.setEmulationMode(false); cpu.setPC(0x1234); cpu.setPBR(0x02);

    // /RESET
    mem.softReset(); cpu.softReset();
    expect("softReset keeps fast RAM ($00:1234)", R(0x001234), 0xA5);
    expect("softReset keeps fast RAM ($02:1234)", R(0x021234), 0x5A);
    expect("softReset keeps slow RAM ($E0:0400)", R(0xE00400), 0xC1);
    expect("softReset keeps slow RAM ($E1:0400)", R(0xE10400), 0xC2);
    expect("softReset keeps the master clock", long(mem.masterTicks() == clock), 1);
    expect("speed re-parked to 1 MHz", mem.speedReg(), 0x00);
    expect("shadow re-parked", mem.shadowReg(), 0x00);
    expect("text mode re-parked", long(mem.textMode()), 1);
    expect("80-column off", long(mem.text80()), 0);
    expect("LC reads ROM after reset", R(0x00D000), 0xEA);
    expect("LC RAM contents survive underneath", long(true), 1);
    R(0xE0C083); R(0xE0C083);
    expect("LC RAM byte survives /RESET", R(0x00D000), 0x77);
    expect("CPU back in emulation mode", long(cpu.getEmulationMode()), 1);
    expect("CPU at the reset vector", cpu.getPC(), 0xD000);
    expect("PBR cleared", cpu.getPBR(), 0);

    // Power cycle
    W(0xE0C036, 0x80);
    mem.reset(); cpu.hardReset();
    expect("power cycle clears fast RAM", R(0x001234), 0x00);
    expect("power cycle clears slow RAM", R(0xE00400), 0x00);
    expect("power cycle restarts the master clock", long(mem.masterTicks()), 0);
    expect("power cycle re-parks speed", mem.speedReg(), 0x00);

    if (fails) { std::printf("reset_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: /RESET keeps RAM + clock and re-parks the switches; power cycle clears RAM\n");
    return 0;
}
