// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ROM built-in self-diagnostic gate (cycle-accuracy catalog entry
// `rom01-rom03-selftest`). Holding ⌘ (open-apple) + Option through RESET makes
// the IIgs ROM run its self-test suite — ROM, RAM, soft switches, RAM address
// lines, speed, serial (SCC), clock, battery RAM, FPI/Mega II, interrupts,
// shadowing, sound (Ensoniq) — at native 2.8 MHz, and report "System Good" or
// "System Bad: xxxxxxxx" (one hex digit pair per test: number + sub-code) on
// the 40-column text page (Apple IIgs Technical Note #95, Apple IIgs
// Firmware Reference "Self-test"). The ROM samples the modifiers through the
// ADB GLU KEYMODREG ($C025), which is what setKeyModifiers() drives.
//
//   selftest_trace <rom> [--frames N] [--ram KB] [--mod HEX] [--dump]
//                        [--through N] [--test N | --entry BANK:ADDR]
//                        [--break BANK:ADDR] [--boot-frames N]
//
// `--through N` makes the full sequential run a gate for tests 1..N only: it
// passes on "System Good" or when the first failing test is above N. Test 09
// (ADB) checksums the ADB microcontroller's firmware through the "read µC
// memory" command; that 4 KB image (341-0632 for ROM 03, 341-0345 for ROM 01)
// is loaded from `<rom dir>/iigs-adb-uc-rom03.rom` / `iigs-adb-uc-rom01.rom`
// or $POMIIGS_ADB_UC_ROM when present, and `--test 9` reports SKIP (77)
// without it — never a false PASS.
//
// `--test N` runs one diagnostic directly, the way the MiSTer SELFTESTxx
// launchers do: a stub in bank $00 RAM sets 2.8 MHz, clears the status bytes
// $0315-$0319, JSLs the entry from the ROM's diagnostic pointer table (ROM 03:
// $FF:6403+, ROM 01: $FF:7143+) and stores the carry (1 = fail) at $0300 before
// STP. Tests past a failing one (09 needs the ADB µC firmware) stay gateable.
//
// Exit 0 = "System Good" / test passed; 1 = "System Bad", failed or no verdict;
// 2 = usage; 77 = ROM missing (CTest SKIP). The final text page and the raw
// status code are always printed so a golden can be archived.

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

std::string textRow(const IIgsMemory& mem, int row) {
    const uint8_t* e0 = mem.slowRam();
    const int rbase = 0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28;
    std::string line;
    for (int col = 0; col < 40; ++col) {
        uint8_t c = e0[rbase + col] & 0x7F;
        if (c < 0x20) c += 0x40;
        line += char(c);
    }
    return line;
}

std::string screenText(const IIgsMemory& mem) {
    std::string all;
    for (int row = 0; row < 24; ++row) all += textRow(mem, row) + "\n";
    return all;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom> [--frames N] [--ram KB] [--mod HEX] [--dump]\n", argv[0]);
        return 2;
    }
    const char* romPath = argv[1];
    long frames = 12000;         // 200 emulated seconds: ROM 03 + 1 MB needs ~155 s for the sequence
    long ramKB = 0;              // default: stock for the ROM (256 KB ROM 01, 1 MB ROM 03)
    unsigned mod = 0xC0;         // ⌘ + Option
    bool dump = false;
    long breakAt = -1;           // --break BANK:ADDR → stop and dump state when PC reaches it
    int testN = 0;               // --test N → run one diagnostic via a RAM launcher stub
    long bootFrames = 120;       // --boot-frames N: let the ROM initialise (bank $E1 vectors…) first
    int through = 0;             // --through N: sequential gate for tests 1..N
    long entry = -1;             // --entry BANK:ADDR → explicit diagnostic entry point
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) ramKB = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--mod") && i + 1 < argc) mod = unsigned(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--dump")) dump = true;
        else if (!std::strcmp(argv[i], "--test") && i + 1 < argc) testN = int(std::strtol(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--boot-frames") && i + 1 < argc) bootFrames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--through") && i + 1 < argc) through = int(std::strtol(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--entry") && i + 1 < argc) {
            const char* a = argv[++i]; const char* colon = std::strchr(a, ':');
            entry = colon ? long((std::strtoul(a, nullptr, 16) << 16) | std::strtoul(colon + 1, nullptr, 16))
                          : long(std::strtoul(a, nullptr, 16));
        }
        else if (!std::strcmp(argv[i], "--break") && i + 1 < argc) {
            const char* a = argv[++i]; const char* colon = std::strchr(a, ':');
            breakAt = colon ? long((std::strtoul(a, nullptr, 16) << 16) | std::strtoul(colon + 1, nullptr, 16))
                            : long(std::strtoul(a, nullptr, 16));
        }
        else { std::fprintf(stderr, "unknown arg '%s'\n", argv[i]); return 2; }
    }

    std::ifstream in(romPath, std::ios::binary);
    if (!in) { std::fprintf(stderr, "[selftest] no ROM at '%s' — SKIP\n", romPath); return 77; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    IIgsMemory mem;
    if (!mem.loadRom(rom)) { std::fprintf(stderr, "unsupported ROM size %zu\n", rom.size()); return 2; }
    if (ramKB <= 0) ramKB = rom.size() > 128 * 1024 ? 1024 : 256;
    mem.setFastRamKB(uint32_t(ramKB));
    {   // optional ADB microcontroller firmware (see header)
        std::string ucPath;
        if (const char* env = std::getenv("POMIIGS_ADB_UC_ROM")) ucPath = env;
        else {
            std::string dir(romPath);
            const size_t slash = dir.find_last_of('/');
            dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
            ucPath = dir + (rom.size() > 128 * 1024 ? "iigs-adb-uc-rom03.rom" : "iigs-adb-uc-rom01.rom");
        }
        std::ifstream uc(ucPath, std::ios::binary);
        std::vector<uint8_t> ucRom((std::istreambuf_iterator<char>(uc)), std::istreambuf_iterator<char>());
        if (!ucRom.empty()) std::printf("[selftest] ADB µC firmware %s: %s\n", ucPath.c_str(),
                                        mem.loadAdbMicroRom(ucRom) ? "loaded" : "wrong size (want 4096)");
    }
    mem.reset();
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    // ⌘ and Option are also the classic //e push buttons: the ROM's reset code
    // ($FF:7F92 on ROM 03) samples $C061/$C062 for the self-test request, and
    // KEYMODREG for the ADB path — hold all of them through RESET.
    const bool single = testN > 0 || entry >= 0;
    if (!single) {
        mem.setKeyModifiers(uint8_t(mod));
        mem.setButton(0, (mod & 0x80) != 0);  // open-apple / ⌘
        mem.setButton(1, (mod & 0x40) != 0);  // option / solid-apple
    }
    cpu.hardReset();
    if (single) {
        // Normal boot first: the ROM fills the bank $E1 vector page, the
        // toolbox dispatch vectors and the zero page the diagnostics rely on
        // (the MiSTer launchers run from a booted ProDOS for the same reason).
        for (long b = 0; b < bootFrames; ++b) {
            uint64_t spent = 0;
            while (spent < uint64_t(mem.masterPerFrame())) spent += mem.tick(cpu.run(1));
            mem.frameTick();
        }
        if (entry < 0) {
            const bool rom3 = rom.size() > 128 * 1024;
            const uint32_t table = rom3 ? 0xFF6403 : 0xFF7143;             // 2-byte entries, test 1 first
            const int count = rom3 ? mem.peek8(0xFF6402) : mem.peek8(0xFF7142) / 2;
            if (testN > count) { std::fprintf(stderr, "[selftest] test %d > %d in table\n", testN, count); return 2; }
            const uint32_t p = table + uint32_t(testN - 1) * 2;
            entry = long(0xFF0000u | mem.peek8(p) | (mem.peek8(p + 1) << 8));
        }
        // Launcher stub at $00:2000 (native, m=1 x=1, DBR=0), see header.
        const uint8_t stub[] = {
            0xA9, 0x80, 0x8D, 0x36, 0xC0,                   // LDA #$80 : STA $C036   (2.8 MHz)
            0x9C, 0x15, 0x03, 0x9C, 0x16, 0x03, 0x9C, 0x17, 0x03, 0x9C, 0x18, 0x03, 0x9C, 0x19, 0x03,
            0x22, uint8_t(entry), uint8_t(entry >> 8), uint8_t(entry >> 16),   // JSL entry
            0xE2, 0x30,                                     // SEP #$30 (tests return with any M/X width)
            0xB0, 0x04,                                     // BCS fail
            0xA9, 0x00, 0x80, 0x02,                         // LDA #0 : BRA store
            0xA9, 0x01,                                     // fail: LDA #1
            0x4B, 0xAB,                                     // store: PHK : PLB (tests leave DBR anywhere)
            0x8D, 0x00, 0x03,                               // STA $0300
            0xDB };                                         // STP
        for (size_t i = 0; i < sizeof stub; ++i) mem.write8(0x2000 + uint32_t(i), stub[i]);
        mem.write8(0x0300, 0xFF); mem.takeBusPenalty();
        cpu.setEmulationMode(false); cpu.setP(0x34); cpu.setSP(0x01FF); cpu.softReset();
        cpu.setEmulationMode(false); cpu.setP(0x34); cpu.setSP(0x01FF);
        cpu.setDBR(0); cpu.setPBR(0); cpu.setPC(0x2000); cpu.setD(0);
        std::printf("[selftest] single test %d entry $%06lX via launcher stub\n", testN, entry);
        if (testN == 9 && !mem.hasAdbMicroRom()) {
            std::printf("[selftest] test 9 (ADB) needs the ADB microcontroller firmware — SKIP\n");
            return 77;
        }
    }
    std::printf("[selftest] ROM %s (%zu KB, banks $%02X-$FF) RAM %ld KB modifiers $%02X frames<=%ld\n",
                romPath, rom.size() / 1024, mem.romBankBase(), ramKB, mod, frames);

    std::string verdict;
    std::vector<uint32_t> hot(1u << 24, 0);   // instruction count per PBR:PC
    const bool logJsl = std::getenv("SELFTEST_JSL") != nullptr;   // discover the test dispatcher's targets
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> jsl;
    struct Last { uint32_t pc; uint8_t op; uint16_t a, x, y, sp; uint8_t p; };
    std::vector<Last> ring(256); size_t ringI = 0;                 // last instructions, for derail triage
    long f = 0;
    for (; f < frames; ++f) {
        uint64_t spent = 0;
        const uint64_t perFrame = uint64_t(mem.masterPerFrame());
        while (spent < perFrame) {
            const uint32_t pc = ((uint32_t(cpu.getPBR()) << 16) | cpu.getPC()) & 0xFFFFFF;
            ++hot[pc];
            ring[ringI++ & 255] = {pc, mem.peek8(pc), cpu.getA(), cpu.getX(), cpu.getY(), cpu.getSP(), cpu.getP()};
            if (long(pc) == breakAt) {
                std::printf("[selftest] BREAK at $%06X frame %ld: A=%04X X=%04X Y=%04X SP=%04X P=%02X %s D=%04X DBR=%02X\n",
                            pc, f, cpu.getA(), cpu.getX(), cpu.getY(), cpu.getSP(), cpu.getP(),
                            cpu.getEmulationMode() ? "e" : "n", cpu.getD(), cpu.getDBR());
                std::printf("[selftest] zp $00-$0F:");
                for (uint32_t a = 0; a < 16; ++a) std::printf(" %02X", mem.peek8(a));
                std::printf("  $F0-$FF:");
                for (uint32_t a = 0xF0; a < 0x100; ++a) std::printf(" %02X", mem.peek8(a));
                std::printf("\n[selftest] shadow=$%02X speed=$%02X\n", mem.shadowReg(), mem.speedReg());
                frames = 0; break;
            }
            if (logJsl && mem.peek8(pc) == 0x22) {                       // JSL long: record caller → target
                const uint32_t target = mem.peek8(pc + 1) | (mem.peek8(pc + 2) << 8) | (mem.peek8(pc + 3) << 16);
                ++jsl[std::make_pair(pc, target)];
            }
            spent += mem.tick(cpu.run(1));
            if (single && !cpu.isRunning()) { frames = 0; break; }         // launcher hit STP
            // The diagnostic returned to the stub: RTL lands at $00:2018, but
            // the ROM 01 tests that end with RTS land at $FF:2018 (PBR stays
            // in the ROM bank — the ROM's own dispatcher lives there). Take the
            // verdict straight from the carry either way.
            if (single && cpu.getPC() == 0x2018 && (cpu.getPBR() == 0x00 || cpu.getPBR() == 0xFF)) {
                mem.write8(0x0300, uint8_t(cpu.getP() & 0x01)); mem.takeBusPenalty();
                frames = 0; break;
            }
        }
        mem.frameTick();
        if ((f % 30) == 29) {                                   // poll twice per second
            const std::string text = screenText(mem);
            const size_t good = text.find("System Good"), bad = text.find("System Bad");
            if (good != std::string::npos) { verdict = "System Good"; break; }
            if (bad != std::string::npos) { verdict = text.substr(bad, text.find('\n', bad) - bad); break; }
        }
    }
    std::printf("[selftest] stopped after %ld frames (%.1f s emulated) at $%02X:%04X, speed=$%02X shadow=$%02X\n",
                f, f / 60.0, cpu.getPBR(), cpu.getPC(), mem.speedReg(), mem.shadowReg());
    if (dump || verdict.empty()) {
        std::printf("[selftest] A=%04X X=%04X Y=%04X SP=%04X P=%02X %s D=%04X DBR=%02X\n", cpu.getA(), cpu.getX(),
                    cpu.getY(), cpu.getSP(), cpu.getP(), cpu.getEmulationMode() ? "e" : "n", cpu.getD(), cpu.getDBR());
        std::vector<uint32_t> idx;
        for (uint32_t a = 0; a < hot.size(); ++a) if (hot[a]) idx.push_back(a);
        std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(12, idx.size()), idx.end(),
                          [&](uint32_t a, uint32_t b) { return hot[a] > hot[b]; });
        std::printf("[selftest] hottest PCs:");
        for (size_t k = 0; k < idx.size() && k < 12; ++k) std::printf(" $%06X:%u", idx[k], hot[idx[k]]);
        // Self-test scratch/status bytes (the MiSTer launchers clear $0315-$0319
        // before calling a test and read them back as the status code).
        std::printf("\n[selftest] $00:0300-031F:");
        for (uint32_t a = 0x300; a < 0x320; ++a) std::printf("%s%02X", (a & 7) ? " " : "  ", mem.peek8(a));
        std::printf("\n--- text page 1 ($E0:0400) ---\n");
        for (int row = 0; row < 24; ++row) std::printf("|%s|\n", textRow(mem, row).c_str());
    }
    if (logJsl) {
        std::printf("[selftest] JSL caller→target (count):\n");
        for (const auto& kv : jsl) std::printf("   $%06X → $%06X  x%u\n", kv.first.first, kv.first.second, kv.second);
    }
    if (single) {
        const uint8_t r = mem.peek8(0x0300);
        if (r > 1 && std::getenv("SELFTEST_RING")) {
            std::printf("[selftest] last instructions:\n");
            for (size_t k = 0; k < 256; ++k) {
                const Last& l = ring[(ringI + k) & 255];
                if (l.pc || l.op) std::printf("   $%06X op=%02X A=%04X X=%04X Y=%04X SP=%04X P=%02X\n", l.pc, l.op, l.a, l.x, l.y, l.sp, l.p);
            }
        }
        std::printf("[selftest] test %d: %s (status $0315-$0319 = %02X %02X %02X %02X %02X)\n", testN,
                    r == 0 ? "PASS" : r == 1 ? "FAIL" : "NO VERDICT (STP not reached)",
                    mem.peek8(0x315), mem.peek8(0x316), mem.peek8(0x317), mem.peek8(0x318), mem.peek8(0x319));
        return r == 0 ? 0 : 1;
    }
    if (verdict.empty()) { std::printf("[selftest] NO VERDICT\n"); return 1; }
    // Trim trailing spaces of the status line for the archive.
    while (!verdict.empty() && verdict.back() == ' ') verdict.pop_back();
    std::printf("[selftest] %s\n", verdict.c_str());
    if (verdict == "System Good") return 0;
    if (through > 0) {
        // "System Bad:  TTSSSSSS" — TT = first failing test (hex), SSSSSS = sub-code.
        const size_t digits = verdict.find_first_of("0123456789ABCDEF", 10);
        const int failing = digits == std::string::npos ? 0 : int(std::strtol(verdict.substr(digits, 2).c_str(), nullptr, 16));
        if (failing > through) {
            std::printf("[selftest] tests 01-%02X passed sequentially; first failure %02X is beyond the gate\n", through, failing);
            return 0;
        }
    }
    return 1;
}
