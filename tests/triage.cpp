// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Headless game-compatibility triage. Boots one disk image for N frames and
// prints ONE machine-readable line classifying how far it got, so a batch
// script can build a compatibility map over the whole collection and rank
// failures by signature. Dev tool (not a CTest gate).
//
//   triage <rom> <disk> [--frames N] [--disk525|--disk35] [--char file]
//
// Output (stdout, one line):
//   STATUS=<class> mode=<m> pc=<bb:pppp> hang=<0|1> zp=<n> rom=<0|1> gfx=<0|1> text=<0|1> | <disk>
//
// Classes: OK_GFX (running in RAM, graphics up) · TEXT (running, text mode) ·
//   CRASH_ZP (executing in zero page / stack — runaway) · HANG (PC frozen) ·
//   NOBOOT (still in the boot ROM — no/failed media) · UNKNOWN.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include "CPU65816.h"
#include "IIgsMemory.h"
#include "VGC.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char* p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <rom> <disk> [--frames N] [--disk525|--disk35] [--char f] [--png out]\n", argv[0]); return 2; }
    const char* romPath = argv[1];
    const char* disk    = argv[2];
    long frames = 900; const char* charPath = "roms/iigs-char.rom";
    bool use525 = false; bool iwm35 = false; const char* pngOut = nullptr;
    for (int i = 3; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--disk525")) use525 = true;
        else if (!std::strcmp(argv[i], "--disk35"))  use525 = false;
        else if (!std::strcmp(argv[i], "--iwm35"))   iwm35 = true;   // 3.5" via the real IWM/Sony LLE (default = HLE SmartPort)
        else if (!std::strcmp(argv[i], "--char") && i + 1 < argc) charPath = argv[++i];
        else if (!std::strcmp(argv[i], "--png")  && i + 1 < argc) pngOut = argv[++i];
    }

    std::vector<uint8_t> rom = readFile(romPath);
    IIgsMemory mem; CPU65816 cpu(&mem); VGC vgc;
    if (rom.empty() || !mem.loadRom(rom)) { std::printf("STATUS=NOROM | %s\n", disk); return 2; }
    mem.setCpu(&cpu); mem.reset();
    if (!use525) mem.setIwm35(iwm35);   // default 3.5" = HLE SmartPort (the emulator's default)
    bool mounted = use525 ? mem.loadDisk525(disk) : mem.loadDisk35(disk);
    if (!mounted) { std::printf("STATUS=BADIMG | %s\n", disk); return 0; }
    mem.ejectHdd();
    cpu.hardReset();
    std::vector<uint8_t> chr = readFile(charPath);
    vgc.setCharRom(chr);

    // Per-instruction metrics.
    uint32_t lastAddr = 0xFFFFFFFF; long sameRun = 0; bool hang = false; uint32_t hangAddr = 0;
    long zpExec = 0;                 // instructions run in bank 0, PC < $0200 (runaway)
    long brkExec = 0;                // BRK ($00) opcodes executed in RAM (crash signal)
    bool leftRom = false;            // ever executed outside the boot ROM banks

    for (long f = 0; f < frames; ++f) {
        long spent = 0;
        while (spent < 46000) {
            const uint8_t pb = cpu.getPBR();
            const uint16_t pc = cpu.getPC();
            const uint32_t addr = (uint32_t(pb) << 16) | pc;
            // Hang: same PC for a long run (tight infinite loop / dead spin).
            if (addr == lastAddr) { if (++sameRun > 200000 && !hang) { hang = true; hangAddr = addr; } }
            else { sameRun = 0; lastAddr = addr; }
            // Runaway: executing in zero page / stack page (bank 0, < $0200).
            if (pb == 0x00 && pc < 0x0200) ++zpExec;
            // Left the boot ROM (banks $FC-$FF) into RAM or the slow side.
            if (pb < 0xFC) leftRom = true;
            // BRK detector: peek the opcode about to run (guarded against the
            // $Cnxx I/O page, where a read would fire a soft switch). A BRK
            // executed in game/loader RAM is a hard crash — ROM firmware never
            // runs one, so only count it below the ROM banks.
            if (pb < 0xFC && (pc & 0xFF00) != 0xC000 && mem.read8(addr) == 0x00) {
                if (brkExec == 0 && getenv("TRIAGE_BRK"))   // dev: report the first BRK site
                    std::fprintf(stderr, "first BRK at %02X:%04X frame=%ld\n", pb, pc, f);
                ++brkExec;
            }
            int c = cpu.run(1); mem.tick(c); spent += (c > 0 ? c : 1);
        }
    }

    const uint32_t finalAddr = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();
    const bool gfx  = mem.shrEnabled() || (!mem.textMode() && mem.hires());
    const bool text = mem.textMode() && !mem.shrEnabled();
    const char* mode = mem.shrEnabled() ? "SHR"
                     : (mem.textMode() ? "text" : (mem.dhires() ? "DHGR" : (mem.hires() ? "HGR" : "LORES")));

    // A BRK vectors into the ROM monitor/firmware; landing in ROM text mode
    // after having run the game is the "clean crash" tell even when no BRK
    // opcode was sampled (the crash BRK ran between samples).
    const bool inRomMon = cpu.getPBR() >= 0xFC && text && leftRom;

    // Classify (order = priority).
    const char* status;
    // CRASH_ZP is gated on !gfx: some games legitimately run code from zero
    // page / the stack page (Pirates!, Silent Service — millions of ZP
    // instructions with a live SHR menu). A *runaway* into zero page lands on
    // $00 bytes = BRK and is caught by CRASH_BRK above, so sustained ZP
    // execution with graphics up and no BRK is a working title, not a crash.
    if (brkExec > 0)                            status = "CRASH_BRK";  // executed BRK in RAM
    else if (zpExec > 2000 && !gfx)             status = "CRASH_ZP";   // sustained runaway in ZP
    else if (inRomMon)                          status = "CRASH_MON";  // fell into the ROM monitor
    else if (hang && !leftRom)                  status = "NOBOOT";     // frozen inside the boot ROM
    else if (hang)                              status = "HANG";       // frozen in game/loader code
    else if (!leftRom)                          status = "NOBOOT";     // never escaped the ROM
    else if (gfx)                               status = "OK_GFX";     // running, graphics up
    else if (text)                              status = "TEXT";       // running, text mode (ambiguous)
    else                                        status = "UNKNOWN";

    std::printf("STATUS=%s mode=%s pc=%02X:%04X hang=%d zp=%ld brk=%ld rom=%d gfx=%d text=%d | %s\n",
                status, mode, cpu.getPBR(), cpu.getPC(), hang ? 1 : 0, zpExec, brkExec,
                leftRom ? 0 : 1, gfx ? 1 : 0, text ? 1 : 0, disk);
    (void)finalAddr; (void)hangAddr;

    if (pngOut) {
        const uint32_t* fb = vgc.render(mem);
        stbi_write_png(pngOut, vgc.width(), vgc.height(), 4, fb, vgc.width() * 4);
    }
    return 0;
}
