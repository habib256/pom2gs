// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// gilyon 65C816 functional tests (snes-tests/cputest, MIT) on the flat test
// bus — catalog entry cpu-functional-crosschecks / gilyon_native. The LoROM
// image built by tools/build_gilyon.sh is mapped the SNES way (32 KB chunk n
// → bank n and bank $80+n at $8000-$FFFF), the CPU starts at `main` and the
// driver STPs with a status byte: 1 = every test passed, 2 = a test failed
// (its number is in test_num), 3 = a test ran out of order.
//
//   gilyon_test <cputest_pom.sfc> <cputest_pom.lbl> [--max-steps N]
//
// Exit 0 = success; 1 = failure; 2 = usage; 77 = image missing (CTest SKIP).

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <cputest_pom.sfc> <cputest_pom.lbl> [--max-steps N]\n", argv[0]); return 2; }
    long maxSteps = 400000000;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max-steps") && i + 1 < argc) maxSteps = std::strtol(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown arg '%s'\n", argv[i]); return 2; }
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::ifstream lbl(argv[2]);
    if (!in || !lbl) { std::fprintf(stderr, "[gilyon] image or label file missing — build with tools/build_gilyon.sh — SKIP\n"); return 77; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::map<std::string, uint32_t> labels;                  // VICE format: "al 00XXXX .name"
    for (std::string line; std::getline(lbl, line);) {
        std::istringstream ss(line); std::string al, addr, name;
        if (ss >> al >> addr >> name && al == "al" && !name.empty() && name[0] == '.')
            labels[name.substr(1)] = uint32_t(std::strtoul(addr.c_str(), nullptr, 16));
    }
    if (!labels.count("main") || !labels.count("status") || !labels.count("test_num")) {
        std::fprintf(stderr, "[gilyon] labels main/status/test_num not found in %s\n", argv[2]); return 2;
    }

    IIgsMemory mem;
    mem.setTestMode(true);                                   // flat 16 MB, no I/O
    CPU65816 cpu(&mem);
    for (size_t off = 0; off < rom.size(); ++off) {          // LoROM: chunk n → $n:8000 and $(80+n):8000
        const uint32_t bank = uint32_t(off / 0x8000), a = 0x8000 + uint32_t(off % 0x8000);
        mem.write8((bank << 16) | a, rom[off]);
        mem.write8(((0x80 + bank) << 16) | a, rom[off]);
    }
    const uint32_t entry = labels["main"], statusA = labels["status"], testA = labels["test_num"];
    cpu.setEmulationMode(true); cpu.setPBR(uint8_t(entry >> 16)); cpu.setPC(uint16_t(entry));
    cpu.setDBR(0); cpu.setD(0); cpu.setSP(0x01FF); cpu.setP(0x34);
    long steps = 0;
    for (; steps < maxSteps; ++steps) {
        cpu.run(1);
        if (!cpu.isRunning()) break;                         // driver STP
    }
    const uint8_t status = mem.read8(statusA);
    const unsigned testNum = mem.read8(testA) | (mem.read8(testA + 1) << 8);
    std::printf("[gilyon] status=%u test_num=$%04X steps=%ld pc=$%02X:%04X A=%04X X=%04X Y=%04X P=%02X %s\n",
                status, testNum, steps, cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(), cpu.getP(),
                cpu.getEmulationMode() ? "e" : "n");
    if (status == 1) { std::printf("[gilyon] SUCCESS: %u tests passed\n", testNum + 1); return 0; }
    if (status == 2) std::printf("[gilyon] FAIL at test $%04X (see tests-full.inc: test%04x) — results A=%04X X=%04X Y=%04X P=%04X S=%04X D=%04X DBR=%02X\n",
                                 testNum, testNum, mem.read8(testA + 2) | (mem.read8(testA + 3) << 8), mem.read8(testA + 4) | (mem.read8(testA + 5) << 8),
                                 mem.read8(testA + 6) | (mem.read8(testA + 7) << 8), mem.read8(testA + 8) | (mem.read8(testA + 9) << 8),
                                 mem.read8(testA + 10) | (mem.read8(testA + 11) << 8), mem.read8(testA + 12) | (mem.read8(testA + 13) << 8), mem.read8(testA + 14));
    else if (status == 3) std::printf("[gilyon] FAIL: invalid test order at $%04X\n", testNum);
    else std::printf("[gilyon] FAIL: %s\n", cpu.isRunning() ? "step budget exhausted" : "stopped without a verdict");
    return 1;
}
