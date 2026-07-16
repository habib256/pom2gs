// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// SmartPort extended-call gate. Drives the slot-5 SmartPort dispatch the way
// GS/OS and games do — `JSR $C553` (the SmartPort entry = ProDOS entry + 3)
// followed by an inline `cmd, paramListPtr` — and checks the WDM-trap handler
// executes READBLOCK / STATUS against the mounted 3.5" image.

#include "IIgsMemory.h"
#include "CPU65816.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

int main() {
    const int kBlocks = 16;
    std::vector<uint8_t> img(size_t(kBlocks) * 512, 0);
    for (int i = 0; i < 512; ++i) img[3 * 512 + i] = uint8_t(i ^ 0x5A);   // block 3 pattern
    const char* path = "/tmp/pomiigs_smartport_test.po";
    { std::ofstream o(path, std::ios::binary); o.write((const char*)img.data(), img.size()); }

    IIgsMemory mem;
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();
    cpu.hardReset();
    if (!mem.loadDisk35(path)) { std::printf("FAIL loadDisk35\n"); return 1; }

    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };
    auto wr = [&](uint16_t a, uint8_t v) { mem.write8(a, v); };

    // SmartPort ROM now advertises SmartPort ($Cn07=$00) with the extended bit.
    auto rom5 = [&](uint16_t off) { return mem.read8((uint32_t(0xE0) << 16) | off); };
    check("sig $C507=$00 (SmartPort)", rom5(0xC507) == 0x00);
    check("$C5FB extended bit", (rom5(0xC5FB) & 0x02) != 0);
    check("$C553 = WDM $C5", rom5(0xC553) == 0x42 && rom5(0xC554) == 0xC5);

    // Program at $0300: JSR $C553 / DFB READBLOCK / DW paramlist / STP-ish loop.
    wr(0x0300, 0x20); wr(0x0301, 0x53); wr(0x0302, 0xC5);   // JSR $C553
    wr(0x0303, 0x01);                                       // cmd = READBLOCK (standard)
    wr(0x0304, 0x10); wr(0x0305, 0x03);                     // param list @ $0310
    wr(0x0306, 0x80); wr(0x0307, 0xFE);                     // BRA * (park here)
    // Param list: count(3), unit(1), buffer(2)=$0400, block(3)=3.
    wr(0x0310, 0x03); wr(0x0311, 0x01);
    wr(0x0312, 0x00); wr(0x0313, 0x04);                     // buffer $0400
    wr(0x0314, 0x03); wr(0x0315, 0x00); wr(0x0316, 0x00);   // block 3

    cpu.setPBR(0); cpu.setDBR(0); cpu.setPC(0x0300);
    for (int i = 0; i < 6; ++i) cpu.step();                 // JSR, WDM(trap), RTS, BRA…

    check("carry clear (success)", (cpu.getP() & 0x01) == 0);
    int mism = 0;
    for (int i = 0; i < 512; ++i) if (mem.read8(uint16_t(0x0400 + i)) != img[3 * 512 + i]) ++mism;
    check("READBLOCK filled buffer with block 3", mism == 0);

    // STATUS (code 3, DIB) into $0500 for unit 1.
    wr(0x0300, 0x20); wr(0x0301, 0x53); wr(0x0302, 0xC5);   // JSR $C553
    wr(0x0303, 0x00);                                       // cmd = STATUS
    wr(0x0304, 0x20); wr(0x0305, 0x03);                     // param list @ $0320
    wr(0x0306, 0x80); wr(0x0307, 0xFE);
    wr(0x0320, 0x03); wr(0x0321, 0x01);                     // count, unit 1
    wr(0x0322, 0x00); wr(0x0323, 0x05);                     // status list $0500
    wr(0x0324, 0x03);                                       // status code 3 (DIB)
    cpu.setPC(0x0300);
    for (int i = 0; i < 6; ++i) cpu.step();
    // DIB: status byte bit7 (block device) + 3-byte block count = 16.
    check("DIB status = block device", (mem.read8(0x0500) & 0x80) != 0);
    unsigned n = mem.read8(0x0501) | (mem.read8(0x0502) << 8) | (mem.read8(0x0503) << 16);
    check("DIB block count = 16", n == 16);
    check("DIB device type = 3.5\"", mem.read8(0x0515) == 0x01);   // offset 21: $01 = 3.5" (SmartPort TN #4)
    check("DIB subtype = removable/disk-switched, no driver ($80)", mem.read8(0x0516) == 0x80);  // offset 22

    // STATUS to a non-existent unit (2) must FAIL — only unit 1 exists. The System 6
    // Installer scans units 1,2,3,… and only stops on a no-device error; answering
    // every unit with a valid DIB loops it forever. Expect carry set + A = $28.
    wr(0x0321, 0x02);                                      // unit 2 (does not exist)
    cpu.setPC(0x0300);
    for (int i = 0; i < 6; ++i) cpu.step();
    check("STATUS unit 2 → error (carry set)", (cpu.getP() & 0x01) != 0);
    check("STATUS unit 2 → A = $28 (no device)", (cpu.getA() & 0xFF) == 0x28);

    // Disk swap: the first block READ (not STATUS) after a swap returns $2E (disk
    // switched, KEGS just_ejected) so GS/OS drops the cached volume and re-reads it.
    { std::vector<uint8_t> img2(size_t(kBlocks) * 512, 0xBB);
      const char* path2 = "/tmp/pomiigs_smartport_swap.po";
      { std::ofstream o(path2, std::ios::binary); o.write((const char*)img2.data(), img2.size()); }
      mem.swapDisk35(path2);
      // A STATUS in between must NOT consume the $2E one-shot (GS/OS polls STATUS).
      wr(0x0303, 0x00); wr(0x0304, 0x20); wr(0x0305, 0x03);   // cmd STATUS, param list @ $0320
      wr(0x0321, 0x01); wr(0x0324, 0x00);                     // unit 1, status code 0
      cpu.setPC(0x0300); for (int i = 0; i < 6; ++i) cpu.step();
      check("STATUS after swap succeeds (bit0 switched), no $2E",
            (cpu.getP() & 0x01) == 0 && (mem.read8(0x0500) & 0x01) != 0);
      // First READBLOCK after the swap → $2E (STATUS didn't eat it).
      wr(0x0303, 0x01); wr(0x0304, 0x10); wr(0x0305, 0x03);   // cmd READ, param list @ $0310
      wr(0x0311, 0x01);
      cpu.setPC(0x0300); for (int i = 0; i < 6; ++i) cpu.step();
      check("READBLOCK after swap → $2E (disk switched)",
            (cpu.getP() & 0x01) && (cpu.getA() & 0xFF) == 0x2E);
      // Latch cleared: retry returns the NEW disk's data.
      cpu.setPC(0x0300); for (int i = 0; i < 6; ++i) cpu.step();
      check("READBLOCK retry → new disk data", (cpu.getP() & 0x01) == 0 && mem.read8(0x0400) == 0xBB);
      std::remove(path2);
    }

    std::remove(path);
    if (fails) { std::printf("smartport_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: SmartPort dispatch (READBLOCK + STATUS/DIB) on slot 5\n");
    return 0;
}
