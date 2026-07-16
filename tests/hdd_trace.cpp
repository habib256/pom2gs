// POMIIGS — GS/OS + hard-disk derail microscope (dev tool, not a gate).
//
// Boots GS/OS from the slot-5 3.5" SmartPort drive WITH a hard disk mounted,
// and traces where GS/OS derails when it initialises/mounts the HDD. Unlike
// gsos_trace (which ejects the HDD), this keeps it so we can catch the
// $A500/$4200-class runaway. A PC ring buffer prints the trail into the
// derail, plus every SmartPort/ProDOS-block trap and slot-7 firmware touch.
//
//   hdd_trace <rom> <disk35.2mg> <hdd.hdv> [--steps N] [--sp]
//
// --sp : mount the HDD as a slot-5 SmartPort unit-2 instead of the slot-7
//        plain block device (experimental topology).

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <rom> <disk35> <hdd> [--steps N]\n", argv[0]); return 2; }
    const char* romPath = argv[1];
    const char* d35Path = argv[2];
    const char* hddPath = argv[3];
    long steps = 60000000;
    for (int i = 4; i < argc; ++i)
        if (!std::strcmp(argv[i], "--steps") && i + 1 < argc) steps = std::strtol(argv[++i], nullptr, 10);

    std::ifstream in(romPath, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", romPath); return 2; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    IIgsMemory mem;
    if (!mem.loadRom(rom)) { std::fprintf(stderr, "unsupported ROM size %zu\n", rom.size()); return 2; }
    mem.reset();
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    if (std::strcmp(d35Path, "none") == 0) { mem.ejectDisk35(); std::printf("disk35 (slot5): (none)\n"); }
    else std::printf("disk35 (slot5): %s -> %s\n", d35Path, mem.loadDisk35(d35Path) ? "mounted" : "FAILED");
    if (std::strcmp(hddPath, "none") == 0) { mem.ejectHdd(); std::printf("hdd    (slot7): (ejected)\n"); }
    else std::printf("hdd    (slot7): %s -> %s\n", hddPath, mem.loadHdd(hddPath) ? "mounted" : "FAILED");
    cpu.hardReset();

    std::printf("ROM %s: %zuKB reset=$%02X:%04X E=%d\n",
                romPath, rom.size()/1024, cpu.getPBR(), cpu.getPC(), cpu.getEmulationMode());

    // PC ring buffer for post-mortem trail.
    constexpr int RING = 64;
    uint32_t ring[RING] = {0}; int rp = 0;

    uint32_t lastAddr = 0xFFFFFFFF; long sameRun = 0; long hangAt = -1;
    // "Sled" catch: a long monotone PC+=1/2/3 walk through RAM = a runaway.
    uint32_t sledPrev = 0; long sledRun = 0; long sledAt = -1; uint32_t sledStart = 0;
    // Ping-pong hang: two addresses alternating for a long time (a wait loop).
    uint32_t ppA = 0, ppB = 0; long ppRun = 0; long ppAt = -1;
    int lastShr = -1; long tools = 0; long firstShrStep = -1;
    long slot7fw = 0, slot7dev = 0;   // slot-7 firmware / device-window touches
    long i = 0;
    for (; i < steps; ++i) {
        const uint32_t addr = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();
        ring[rp] = addr; rp = (rp + 1) % RING;

        if (addr == 0xE10000) ++tools;
        // Watch DEVNUM ($43) transition to $70 (slot 7) — who sets the boot dev?
        if (getenv("WATCH43")) { static uint8_t prev43 = 0;
            uint8_t v = mem.read8(0x43);
            if (v != prev43) { std::printf("  [%9ld] $43: %02X->%02X at PC=%06X\n", i, prev43, v, addr); prev43 = v; } }
        int shr = mem.shrEnabled() ? 1 : 0;
        if (shr != lastShr) { std::printf("  [%9ld] SHR %s\n", i, shr?"ON":"off"); lastShr = shr; if (shr && firstShrStep<0) firstShrStep=i; }

        // Runaway sled: consecutive tiny forward steps in RAM (not ROM $Fx/$E1 IO).
        uint32_t bank = addr >> 16;
        if ((addr > sledPrev) && (addr - sledPrev) <= 3 && bank < 0xE0) {
            if (sledRun == 0) sledStart = sledPrev;
            if (++sledRun > 300 && sledAt < 0) { sledAt = i; break; }
        } else sledRun = 0;
        sledPrev = addr;

        // Derail: PC stuck executing zero-page garbage ($0000-$000F) many steps.
        { static long zpRun = 0;
          if (addr < 0x000010) { if (++zpRun > 1 && sledAt < 0) { sledAt = i; sledStart = addr; break; } }
          else zpRun = 0; }

        // Ping-pong detector: addr alternates A,B,A,B...
        if (addr == ppA || addr == ppB) {
            if (++ppRun > 4000000 && ppAt < 0) { ppAt = i; break; }
        } else { ppB = ppA; ppA = addr; ppRun = 0; }

        if (addr == lastAddr) { if (++sameRun > 3000000 && hangAt < 0) { hangAt = i; break; } }
        else { sameRun = 0; lastAddr = addr; }

        const int cyc = cpu.run(1);
        mem.tick(cyc);
    }
    std::printf("SHR first-on step=%ld ; tool dispatches=%ld\n", firstShrStep, tools);
    (void)slot7fw; (void)slot7dev;

    std::printf("\nran %ld instructions.\n", i);
    std::printf("final $%02X:%04X A=%04X X=%04X Y=%04X SP=%04X P=%02X %s\n",
                cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(),
                cpu.getSP(), cpu.getP(), cpu.getEmulationMode()?"e":"n");
    if (sledAt >= 0) std::printf("SLED/derail near step %ld: started ~$%06X, now $%06X\n", sledAt, sledStart, lastAddr);
    if (hangAt >= 0) std::printf("HANG at $%06X after %ld instrs\n", lastAddr, hangAt);

    // Zero page + key GS/OS boot globals — hunt the boot-device byte ($70 slot7
    // vs $50 slot5). ProDOS DEVNUM is at $E100xx / zero page during boot.
    std::printf("--- zero page $00:00-$FF ---\n");
    for (int r = 0; r < 16; ++r) {
        std::printf("%02X:", r*16);
        for (int c = 0; c < 16; ++c) std::printf(" %02X", mem.read8(uint16_t(r*16+c)));
        std::printf("\n");
    }
    // GS/OS/ProDOS DEVNUM globals in bank $E1 (Misc + boot work area).
    std::printf("--- $E1:00xx boot globals ---\n");
    for (int r = 0; r < 4; ++r) {
        std::printf("E1/00%02X:", r*16);
        for (int c = 0; c < 16; ++c) std::printf(" %02X", mem.read8(0xE10000 + r*16 + c));
        std::printf("\n");
    }

    // SHR menu-bar check ($E1:2000). A drawn Finder = a mostly-white top line
    // (menu bar) over the desktop dither; all-zero = nothing drawn.
    { const uint8_t* e1 = mem.slowRam() + 0x10000;
      auto rowNonZero = [&](int y){ int n=0; for (int x=0;x<160;x++) if (e1[0x2000+y*160+x]) ++n; return n; };
      std::printf("SHR row0 non-zero bytes=%d/160  row8=%d  row100=%d  shrOn=%d\n",
                  rowNonZero(0), rowNonZero(8), rowNonZero(100), mem.shrEnabled());
      std::printf("SHR L0 :"); for (int x=0;x<24;x++) std::printf(" %02X", e1[0x2000+x]); std::printf("\n"); }

    // Dump the BRAM image GS/OS reinitialised (valid checksum for this ROM).
    std::printf("--- BRAM (256 bytes, hex) ---\n");
    { const uint8_t* b = mem.bram();
      for (int r = 0; r < 16; ++r) {
          std::printf("%02X:", r*16);
          for (int c = 0; c < 16; ++c) std::printf(" %02X", b[r*16+c]);
          std::printf("\n");
      } }

    // 40-column text page ($E0:0400) — the ROM prints boot/error text here.
    std::printf("--- text page 1 ($E0:0400) ---\n");
    { const uint8_t* e0 = mem.slowRam();
      for (int row = 0; row < 24; ++row) {
          int rbase = 0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28;
          std::string line;
          for (int col = 0; col < 40; ++col) { uint8_t c = e0[rbase + col] & 0x7F; if (c < 0x20) c += 0x40; line += char(c); }
          std::printf("|%s|\n", line.c_str());
      } }

    // Dump the stack around SP so we can read the JSR return-address chain of
    // whoever is spinning in this wait loop.
    std::printf("--- stack ($00:%04X..) ---\n", cpu.getSP());
    for (int k = 1; k <= 32; ++k) {
        uint16_t a = uint16_t(cpu.getSP() + k);
        std::printf(" %02X", mem.read8(a));
        if (k % 16 == 0) std::printf("\n");
    }

    std::printf("--- PC trail (last %d) ---\n", RING);
    for (int k = 0; k < RING; ++k) {
        uint32_t a = ring[(rp + k) % RING];
        uint8_t b0 = mem.read8(a), b1 = mem.read8(a+1), b2 = mem.read8(a+2);
        std::printf("  %06X : %02X %02X %02X\n", a, b0, b1, b2);
    }
    return 0;
}
