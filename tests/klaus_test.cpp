// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Klaus Dormann's 6502/65C02 functional tests as emulation-mode cross-checks
// (catalog entry cpu-functional-crosschecks). The 64 KB binary is loaded on
// the flat test bus at $000000, the CPU starts in emulation mode at $0400 and
// runs until it parks in a `jmp *` trap: the success trap address (from the
// listing) passes, any other trap is the failing test's error trap.
//
//   klaus_test <binary> <success-addr-hex> [--start HEX] [--max-steps N]
//              [--labels FILE --success NAME --error NAME]
//
// With --labels (VICE "al ADDR .name" file from ld65 -Ln) the success and
// error labels replace the hex address: the program must park at `success`
// AND the byte at `error` must read 0 — Bruce Clark's decimal test ends in
// STP at DONE with ERROR = 0 on a pass. Images shorter than 64 KB load at 0.
//
// Exit 0 = success trap; 1 = other trap or step budget exhausted; 2 = usage;
// 77 = binary missing (CTest SKIP).

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
    if (argc < 3) { std::fprintf(stderr, "usage: %s <binary> <success-addr-hex> [--start HEX] [--max-steps N]\n", argv[0]); return 2; }
    const char* path = argv[1];
    uint32_t success = uint32_t(std::strtoul(argv[2], nullptr, 16));
    uint32_t start = 0x0400; long maxSteps = 200000000;
    const char* labelsPath = nullptr; std::string successLabel, errorLabel;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--start") && i + 1 < argc) start = uint32_t(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--max-steps") && i + 1 < argc) maxSteps = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--labels") && i + 1 < argc) labelsPath = argv[++i];
        else if (!std::strcmp(argv[i], "--success") && i + 1 < argc) successLabel = argv[++i];
        else if (!std::strcmp(argv[i], "--error") && i + 1 < argc) errorLabel = argv[++i];
        else { std::fprintf(stderr, "unknown arg '%s'\n", argv[i]); return 2; }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "[klaus] no binary at '%s' — SKIP\n", path); return 77; }
    std::vector<uint8_t> bin((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bin.empty() || bin.size() > 65536) { std::fprintf(stderr, "[klaus] unexpected size %zu (want <= 65536)\n", bin.size()); return 2; }
    long errorAddr = -1;
    if (labelsPath) {
        std::ifstream lbl(labelsPath);
        if (!lbl) { std::fprintf(stderr, "[klaus] no label file at '%s' — SKIP\n", labelsPath); return 77; }
        std::map<std::string, uint32_t> labels;
        for (std::string line; std::getline(lbl, line);) {
            std::istringstream ss(line); std::string al, addr, name;
            if (ss >> al >> addr >> name && al == "al" && !name.empty() && name[0] == '.')
                labels[name.substr(1)] = uint32_t(std::strtoul(addr.c_str(), nullptr, 16));
        }
        if (!successLabel.empty()) { if (!labels.count(successLabel)) { std::fprintf(stderr, "[klaus] label %s missing\n", successLabel.c_str()); return 2; } success = labels[successLabel]; }
        if (!errorLabel.empty())   { if (!labels.count(errorLabel))   { std::fprintf(stderr, "[klaus] label %s missing\n", errorLabel.c_str());   return 2; } errorAddr = labels[errorLabel]; }
    }

    IIgsMemory mem;
    mem.setTestMode(true);                                  // flat 16 MB, no I/O
    CPU65816 cpu(&mem);
    for (uint32_t a = 0; a < bin.size(); ++a) mem.write8(a, bin[a]);
    cpu.setEmulationMode(true); cpu.setPC(uint16_t(start)); cpu.setPBR(0); cpu.setDBR(0);
    cpu.setSP(0x01FF); cpu.setP(0x34); cpu.setD(0);

    uint32_t lastPc = 0xFFFFFFFF; int same = 0; long steps = 0;
    for (; steps < maxSteps; ++steps) {
        const uint32_t pc = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();
        if (pc == lastPc) { if (++same >= 3) break; } else { same = 0; lastPc = pc; }
        cpu.run(1);
        if (!cpu.isRunning()) { lastPc = ((uint32_t(cpu.getPBR()) << 16) | cpu.getPC()) - 1; break; }   // STP: park at its opcode
    }
    const uint32_t pc = cpu.isRunning() ? ((uint32_t(cpu.getPBR()) << 16) | cpu.getPC()) : lastPc;
    std::printf("[klaus] %s: stopped at $%06X after %ld steps (A=%04X X=%04X Y=%04X SP=%04X P=%02X)\n",
                path, pc, steps, cpu.getA(), cpu.getX(), cpu.getY(), cpu.getSP(), cpu.getP());
    const bool parked = same >= 3 || !cpu.isRunning();          // jmp * loop, or STP
    if (pc == success && parked) {
        if (errorAddr >= 0 && mem.read8(uint32_t(errorAddr)) != 0) {
            std::printf("[klaus] FAIL: parked at the end but %s = %u\n", errorLabel.c_str(), mem.read8(uint32_t(errorAddr)));
            return 1;
        }
        std::printf("[klaus] SUCCESS trap reached%s\n", errorAddr >= 0 ? " with ERROR = 0" : ""); return 0;
    }
    std::printf("[klaus] FAIL: %s\n", parked ? "parked in an error trap" : "step budget exhausted");
    return 1;
}
