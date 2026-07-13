// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// M2 boot-trace: load a real IIgs ROM, reset, and run the 65C816 against the
// IIgsMemory MMU, tracing PC so we can see how far the ROM's boot / self-
// diagnostic gets. Not a pass/fail gate — a development microscope.
//
//   boot_trace <rom-file> [--steps N] [--trace K] [--from PBRPC]

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <rom> [--steps N] [--trace K]\n", argv[0]); return 2; }
    const char* romPath = argv[1];
    long steps = 200000; int traceN = 40;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--steps") && i + 1 < argc) steps = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) traceN = int(std::strtol(argv[++i], nullptr, 10));
    }

    std::ifstream in(romPath, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", romPath); return 2; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    IIgsMemory mem;
    if (!mem.loadRom(rom)) { std::fprintf(stderr, "unsupported ROM size %zu (want 128K/256K)\n", rom.size()); return 2; }
    mem.reset();
    CPU65816 cpu(&mem);
    cpu.hardReset();

    std::printf("ROM %s: %zu KB, banks $%02X-$FF; reset PBR:PC = $%02X:%04X (E=%d)\n",
                romPath, rom.size() / 1024, mem.romBankBase(), cpu.getPBR(), cpu.getPC(), cpu.getEmulationMode());

    // Loop / hang detection: track how often each (PBR:PC) recurs.
    std::map<uint32_t, long> seen;
    uint32_t lastAddr = 0xFFFFFFFF; long sameRun = 0; long hangAt = -1;
    long i = 0;
    for (; i < steps; ++i) {
        const uint32_t addr = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();
        if (i < traceN)
            std::printf("  %6ld  $%02X:%04X  A=%04X X=%04X Y=%04X SP=%04X P=%02X %s\n",
                        i, cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(),
                        cpu.getSP(), cpu.getP(), cpu.getEmulationMode() ? "e" : "n");
        if (addr == lastAddr) { if (++sameRun > 1000 && hangAt < 0) { hangAt = i; } }
        else { sameRun = 0; lastAddr = addr; }
        ++seen[addr];
        cpu.run(1);
    }

    std::printf("\nran %ld instructions.\n", i);
    std::printf("final $%02X:%04X  A=%04X X=%04X Y=%04X SP=%04X P=%02X %s\n",
                cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(),
                cpu.getSP(), cpu.getP(), cpu.getEmulationMode() ? "e" : "n");
    std::printf("MMU: shadow=$%02X speed=$%02X\n", mem.shadowReg(), mem.speedReg());
    if (hangAt >= 0)
        std::printf("HANG: PC stuck at $%08X after ~%ld instructions\n", lastAddr, hangAt);

    // Hottest addresses — where the ROM spends its time (loops / self-test).
    std::vector<std::pair<uint32_t, long>> hot(seen.begin(), seen.end());
    std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::printf("hottest PCs:\n");
    for (int k = 0; k < 8 && k < int(hot.size()); ++k)
        std::printf("   $%06X : %ld\n", hot[k].first, hot[k].second);
    std::printf("distinct PCs visited: %zu\n", seen.size());
    return 0;
}
