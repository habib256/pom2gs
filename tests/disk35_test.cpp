// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Slot-5 3.5" SmartPort/block device gate. Mounts a synthetic ProDOS block
// image on slot 5 and checks (1) the slot-5 ROM ($C5xx) ProDOS signature and
// (2) block streaming through the slot-5 device-select window $C0D0-$C0DF
// (base $C0(8+n)0 = $C0D0 for slot 5) — the same block protocol as the slot-7
// HDD, on the authentic IIgs 3.5" slot.

#include "IIgsMemory.h"
#include "CPU65816.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

int main() {
    // Synthetic 16-block image; block 0 carries a recognisable pattern.
    const int kBlocks = 16;
    std::vector<uint8_t> img(size_t(kBlocks) * 512, 0);
    for (int i = 0; i < 512; ++i) img[i] = uint8_t(i * 7 + 3);
    const char* path = "/tmp/pomiigs_disk35_test.po";
    { std::ofstream o(path, std::ios::binary); o.write((const char*)img.data(), img.size()); }

    IIgsMemory mem;
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };

    check("loadDisk35", mem.loadDisk35(path));

    // $C5xx slot ROM ProDOS signature (via the $E0 slot-ROM window).
    auto rom5 = [&](uint16_t off) { return mem.read8((uint32_t(0xE0) << 16) | off); };
    check("sig $C501=$20", rom5(0xC501) == 0x20);
    check("sig $C503=$00", rom5(0xC503) == 0x00);
    check("sig $C505=$03", rom5(0xC505) == 0x03);
    check("sig $C507=$00", rom5(0xC507) == 0x00);   // SmartPort device

    // Device-select $C0D0-$C0DF (slot 5). Select block 0, stream 512 bytes.
    auto io = [&](uint16_t r) { return uint32_t(0xE0) << 16 | (0xC000 | r); };
    mem.write8(io(0x0D0), 0x00);                     // block# low  (resets stream)
    mem.write8(io(0x0D1), 0x00);                     // block# high
    int mism = 0;
    for (int i = 0; i < 512; ++i)
        if (mem.read8(io(0x0D2)) != img[i]) ++mism;
    check("block 0 stream matches", mism == 0);

    // Status ($C0D3): image present (bit7=0), not write-protected (bit6=0).
    check("status ok", mem.read8(io(0x0D3)) == 0x00);
    // Block count ($C0D4/5).
    unsigned n = mem.read8(io(0x0D4)) | (mem.read8(io(0x0D5)) << 8);
    check("block count = 16", n == 16);

    // Eject clears it.
    mem.ejectDisk35();
    check("ejected → status no-media", (mem.read8(io(0x0D3)) & 0x80) != 0);

    std::remove(path);
    if (fails) { std::printf("disk35_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: slot-5 3.5-inch block device (ROM sig + block stream + status)\n");
    return 0;
}
