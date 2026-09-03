// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Battery-backed state gate: BRAM bytes and the guest-set clock written
// through the $C033/$C034 serial protocol persist to the host file and come
// back in a fresh machine; a missing file is a dead battery (defaults).

#include "IIgsMemory.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "bram_test.bin";
    int fails = 0;
    auto expect = [&](const char* what, long got, long want) {
        if (got != want) { std::printf("FAIL %-50s %ld != %ld\n", what, got, want); ++fails; }
    };
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    // One clock-chip transaction: command byte, strobe, then the data byte
    // (write: clock it in; read: clock it out) on a second strobe.
    auto strobe = [](IIgsMemory& m, uint8_t c034) { m.write8(0xE0C034, uint8_t(0x80 | c034)); m.takeBusPenalty(); };
    auto data   = [](IIgsMemory& m, uint8_t v)    { m.write8(0xE0C033, v); m.takeBusPenalty(); };
    auto readData = [](IIgsMemory& m) { uint8_t v = m.read8(0xE0C033); m.takeBusPenalty(); return v; };
    auto bramWrite = [&](IIgsMemory& m, uint8_t addr, uint8_t v) {   // extended BRAM: op 3, 2 address bytes
        data(m, uint8_t(0x38 | (addr >> 5))); strobe(m, 0x00);          // op=3, bit 3 = extended, bits 2-0 = addr 7-5
        data(m, uint8_t((addr & 0x1F) << 2));         strobe(m, 0x00);   // addr bits 4-0
        data(m, v);                                    strobe(m, 0x00);
    };
    auto bramRead = [&](IIgsMemory& m, uint8_t addr) {
        data(m, uint8_t(0x80 | 0x38 | (addr >> 5))); strobe(m, 0x00);
        data(m, uint8_t((addr & 0x1F) << 2));               strobe(m, 0x00);
        strobe(m, 0x40);                                    // read strobe: byte clocked out
        return readData(m);
    };

    IIgsMemory a; a.loadRom(rom); a.reset();
    expect("fresh machine is not dirty", a.bramDirty(), 0);
    bramWrite(a, 0x20, 0xA5);
    bramWrite(a, 0xFF, 0x5A);
    expect("BRAM write lands ($20)", a.bram()[0x20], 0xA5);
    expect("BRAM write lands ($FF)", a.bram()[0xFF], 0x5A);
    expect("BRAM read-back through the chip", bramRead(a, 0x20), 0xA5);
    expect("write marks the state dirty", a.bramDirty(), 1);
    // Set the clock 1000 s ahead through the seconds register (op 0), byte 1 (+256 s × k).
    const uint32_t before = a.rtcSeconds();
    data(a, uint8_t(0x00 | (1 << 2))); strobe(a, 0x00);            // op 0, reg 1 = seconds byte 1
    data(a, uint8_t(((before >> 8) & 0xFF) + 4)); strobe(a, 0x00); // +4 × 256 s
    const long delta = long(a.rtcSeconds()) - long(before);
    expect("clock write moves the seconds counter (+1024 ±1)", delta >= 1023 && delta <= 1025, 1);
    expect("save succeeds", a.saveBram(path), 1);
    expect("save clears dirty", a.bramDirty(), 0);

    IIgsMemory b; b.loadRom(rom); b.reset();
    expect("load succeeds", b.loadBram(path), 1);
    expect("BRAM $20 restored", b.bram()[0x20], 0xA5);
    expect("BRAM $FF restored", b.bram()[0xFF], 0x5A);
    const long d2 = long(b.rtcSeconds()) - long(before);
    expect("clock offset restored", d2 >= 1023 && d2 <= 1026, 1);
    expect("restored state is clean", b.bramDirty(), 0);
    expect("missing file is a dead battery", b.loadBram(path + ".missing"), 0);

    std::remove(path.c_str());
    if (fails) { std::printf("bram_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: BRAM + clock offset persist through states/bram.bin\n");
    return 0;
}
