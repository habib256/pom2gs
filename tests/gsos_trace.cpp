// POMIIGS — GS/OS + toolbox boot microscope (dev tool, not a gate).
//
// Boots an 800K GS/OS system disk from the slot-5 3.5" SmartPort drive and
// traces how far the ROM toolbox / GS/OS Loader gets. Reports:
//   * every Tool Locator dispatch (JSL $E1/0000) with the tool call number in X,
//   * the SHR-enable ($C029 bit7) timeline,
//   * hang detection + hottest PCs.
//
//   gsos_trace <rom> <disk.2mg> [--steps N] [--tools K]

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

// IIgs tool-set numbers (low byte of X in a _ToolCall). Enough to label traces.
static const char* toolsetName(uint8_t ts) {
    switch (ts) {
        case 0x01: return "ToolLocator";
        case 0x02: return "MemoryMgr";
        case 0x03: return "MiscTools";
        case 0x04: return "QuickDrawII";
        case 0x05: return "DeskMgr";
        case 0x06: return "EventMgr";
        case 0x07: return "SchedulerMgr";
        case 0x08: return "SoundMgr";
        case 0x09: return "ADBTool";
        case 0x0A: return "SANE";
        case 0x0B: return "IntegerMath";
        case 0x0C: return "TextTool";
        case 0x0E: return "WindowMgr";
        case 0x0F: return "MenuMgr";
        case 0x10: return "ControlMgr";
        case 0x11: return "SystemLoader";
        case 0x12: return "QuickDrawAux";
        case 0x13: return "PrintMgr";
        case 0x14: return "LineEditTool";
        case 0x15: return "DialogMgr";
        case 0x16: return "ScrapMgr";
        case 0x17: return "StdFileTool";
        case 0x18: return "DiskUtil";
        case 0x19: return "NoteSynth";
        case 0x1A: return "NoteSeq";
        case 0x1B: return "FontMgr";
        case 0x1C: return "ListMgr";
        case 0x1E: return " resourceMgr";
        default:   return "?";
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <rom> <disk.2mg> [--steps N] [--tools K]\n", argv[0]); return 2; }
    const char* romPath = argv[1];
    const char* diskPath = argv[2];
    long steps = 40000000; int toolsN = 400;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--steps") && i + 1 < argc) steps = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--tools") && i + 1 < argc) toolsN = int(std::strtol(argv[++i], nullptr, 10));
    }

    std::ifstream in(romPath, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", romPath); return 2; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    IIgsMemory mem;
    if (!mem.loadRom(rom)) { std::fprintf(stderr, "unsupported ROM size %zu\n", rom.size()); return 2; }
    mem.reset();
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::printf("disk35 (slot5): %s -> %s\n", diskPath, mem.loadDisk35(diskPath) ? "mounted" : "FAILED");
    mem.ejectHdd();  // clear slot 7 so slot 5 boots
    cpu.hardReset();

    std::printf("ROM %s: %zuKB reset=$%02X:%04X E=%d\n",
                romPath, rom.size()/1024, cpu.getPBR(), cpu.getPC(), cpu.getEmulationMode());

    std::map<uint32_t, long> seen;
    std::map<uint16_t, long> toolCalls;   // X value -> count
    uint32_t lastAddr = 0xFFFFFFFF; long sameRun = 0; long hangAt = -1;
    int toolsShown = 0; int lastShr = -1; long lastShrStep = 0;
    long i = 0;
    for (; i < steps; ++i) {
        const uint32_t addr = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();

        // Tool Locator dispatch entry.
        if (addr == 0xE10000) {
            uint16_t x = cpu.getX();
            ++toolCalls[x];
            if (toolsShown < toolsN) {
                std::printf("  [%9ld] TOOL $%04X  set=$%02X(%s) func=$%02X\n",
                            i, x, x & 0xFF, toolsetName(x & 0xFF), (x >> 8) & 0xFF);
                ++toolsShown;
            }
        }

        int shr = mem.shrEnabled() ? 1 : 0;
        if (shr != lastShr) {
            std::printf("  [%9ld] SHR %s ($C029=$%02X)\n", i, shr ? "ON" : "off", mem.newVideo());
            lastShr = shr; lastShrStep = i;
        }

        // Steady-state window: dump PC + opcode bytes so the wait loop can be
        // decoded by hand. Enable with GSOS_WINDOW=<step>.
        static long winAt = [](){ const char* e = getenv("GSOS_WINDOW"); return e ? atol(e) : -1; }();
        if (winAt >= 0 && i >= winAt && i < winAt + 400) {
            uint8_t b0 = mem.read8(addr), b1 = mem.read8(addr + 1), b2 = mem.read8(addr + 2), b3 = mem.read8(addr + 3);
            std::printf("  <%ld> %02X:%04X  %02X %02X %02X %02X  A=%04X X=%04X Y=%04X S=%04X P=%02X%s\n",
                        i, cpu.getPBR(), cpu.getPC(), b0, b1, b2, b3,
                        cpu.getA(), cpu.getX(), cpu.getY(), cpu.getSP(), cpu.getP(), cpu.getEmulationMode()?" e":"");
        }

        // Keyboard-poster ($FE:EC99) + PostEvent ($1406) tracing.
        if (addr == 0xFEEC99) std::printf("  [%9ld] KBD poster $FEEC99 runs (A=%04X)\n", i, cpu.getA());
        if (addr == 0xE10000 && cpu.getX() == 0x1406) std::printf("  [%9ld] EventMgr PostEvent ($1406)\n", i);
        // Dump all tool calls in a short window right after injection.
        if (getenv("KEY_INJECT") && addr == 0xE10000 && i >= 35000000 && i < 35050000)
            std::printf("  [%9ld] TOOL $%04X set=$%02X func=$%02X\n", i, cpu.getX(), cpu.getX()&0xFF, (cpu.getX()>>8)&0xFF);

        if (addr == lastAddr) { if (++sameRun > 2000000 && hangAt < 0) { hangAt = i; break; } }
        else { sameRun = 0; lastAddr = addr; }
        ++seen[addr];
        const int cyc = cpu.run(1);
        mem.tick(cyc);
        static bool jig = getenv("MOUSE_JIGGLE"); static long jc = 0;
        if (jig && i > 30000000 && (++jc % 40000) == 0) mem.mouseMove((jc & 0x40000) ? 4 : -4, 2);
        // Inject one keypress after the desktop is up; KEY_STATUS tunes the
        // $C026 keycode byte to find the value that routes to the kbd handler.
        static const char* ki = getenv("KEY_INJECT");
        if (ki) { static bool done=false;
            if (!done && i == 35000000) { const char* km=getenv("KEY_MOD"); if(km) mem.setKeyModifiers(uint8_t(strtol(km,0,16)));
                if (getenv("KEY_OA")) mem.setButton(0, true);   // open-apple (command)
                mem.keyDown(uint8_t(ki[0])); done=true; } }
    }

    std::printf("\nran %ld instructions.\n", i);
    std::printf("final $%02X:%04X A=%04X X=%04X Y=%04X SP=%04X P=%02X %s\n",
                cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(),
                cpu.getSP(), cpu.getP(), cpu.getEmulationMode()?"e":"n");
    if (hangAt >= 0) std::printf("HANG at $%06X after %ld instrs\n", lastAddr, hangAt);
    (void)lastShrStep;

    // 40-column text page ($E0:0400, //e interleaved) — GS/OS prints boot status
    // / error text here even while SHR shows the welcome splash.
    std::printf("CSWL vec $0036/37 = $%02X%02X\n", mem.read8(0x37), mem.read8(0x36));
    std::printf("--- text page 1 ($E0:0400) ---\n");
    { const uint8_t* e0 = mem.slowRam();
      for (int row = 0; row < 24; ++row) {
          int rbase = 0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28;
          std::string line;
          for (int col = 0; col < 40; ++col) { uint8_t c = e0[rbase + col] & 0x7F; if (c < 0x20) c += 0x40; line += char(c); }
          std::printf("|%s|\n", line.c_str());
      } }

    // SHR menu-bar check: dump scanline 0/8 pixels ($E1:2000) + their SCBs.
    // A drawn menu bar = mostly-white top lines with black title text; the
    // desktop pattern = a uniform 2-colour dither.
    { const uint8_t* e1 = mem.slowRam() + 0x10000;   // bank $E1
      auto line = [&](int y){ std::printf("SHR L%-3d ($E1:%04X):", y, 0x2000 + y*160);
          for (int x = 0; x < 24; ++x) std::printf(" %02X", e1[0x2000 + y*160 + x]); std::printf("\n"); };
      line(0); line(4); line(8); line(12); line(100);
      std::printf("SCB L0=%02X L8=%02X L100=%02X ; shrOn=%d shadow=%02X\n",
                  e1[0x9D00], e1[0x9D08], e1[0x9D64], mem.shrEnabled(), mem.shadowReg());
      // Compare fast-side SHR (bank $01) — if the menu bar is there but not in
      // $E1, the SHR shadow is dropping it.
      auto b01 = [&](int y){ std::printf("b01 L%-3d ($01:%04X):", y, 0x2000 + y*160);
          for (int x = 0; x < 24; ++x) std::printf(" %02X", mem.read8(0x010000 + 0x2000 + y*160 + x)); std::printf("\n"); };
      b01(0); b01(8);
    }

    std::printf("distinct tool calls: %zu\n", toolCalls.size());
    std::vector<std::pair<uint16_t,long>> tc(toolCalls.begin(), toolCalls.end());
    std::sort(tc.begin(), tc.end(), [](auto&a,auto&b){return a.second>b.second;});
    std::printf("tool calls by frequency (X=funcfunc,set):\n");
    for (auto& p : tc)
        std::printf("   $%04X set=%s func=$%02X : %ld\n", p.first, toolsetName(p.first&0xFF), (p.first>>8)&0xFF, p.second);

    // Dump RAM around the hottest bank-0 (RAM) PC so we can decode the loader
    // wait loop that GS/OS spins in after the welcome screen.
    {
        uint32_t hotBank0 = 0; long best = 0;
        for (auto& p : seen) if ((p.first >> 16) == 0 && (p.first & 0xFFFF) >= 0xC000 && p.second > best) { best = p.second; hotBank0 = p.first; }
        if (hotBank0) {
            uint32_t base = (hotBank0 & 0xFFFF) & ~0xFu;
            std::printf("hottest bank-0 RAM PC $%06X (%ld hits); RAM $%04X..$%04X:\n", hotBank0, best, base - 0x10, base + 0x30);
            for (uint32_t a = base - 0x10; a <= base + 0x30; a += 16) {
                std::printf("  %04X:", a);
                for (int b = 0; b < 16; ++b) std::printf(" %02X", mem.read8(a + b));
                std::printf("\n");
            }
        }
    }

    std::vector<std::pair<uint32_t,long>> hot(seen.begin(), seen.end());
    std::sort(hot.begin(), hot.end(), [](auto&a,auto&b){return a.second>b.second;});
    std::printf("hottest PCs:\n");
    for (int k=0;k<12 && k<int(hot.size());++k) std::printf("   $%06X : %ld\n", hot[k].first, hot[k].second);
    std::printf("distinct PCs: %zu\n", seen.size());
    return 0;
}
