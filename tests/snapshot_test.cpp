// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Snapshot gate: boot GS/OS from the HDD for a while, SAVE, run further (the
// machine diverges), LOAD, and verify the restore is exact — registers equal,
// and the next 200k instructions retrace the same PC path as a control run
// from the save point. Requires roms/ + hdv/GSOS.hdv (skips cleanly if absent,
// POM2 pinned-test convention).

#include "CPU65816.h"
#include "IIgsMemory.h"
#include "Snapshot.h"

#include <cstdio>
#include <fstream>
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok) {
    std::printf("%-58s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static void run(CPU65816& cpu, IIgsMemory& mem, long n) {
    for (long i = 0; i < n; ++i) { int c = cpu.run(1); mem.tick(c); }
}

int main() {
    std::ifstream in("roms/iigs-rom03.rom", std::ios::binary);
    if (!in) { std::printf("SKIP: no ROM\n"); return 0; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    { std::ifstream h("hdv/GSOS.hdv"); if (!h) { std::printf("SKIP: no hdv/GSOS.hdv\n"); return 0; } }

    IIgsMemory mem; CPU65816 cpu(&mem);
    mem.loadRom(rom); mem.setCpu(&cpu); mem.reset();
    mem.loadHdd("hdv/GSOS.hdv");
    cpu.hardReset();
    run(cpu, mem, 3000000);                            // into the GS/OS boot

    const char* path = "/tmp/pomiigs_snapshot_test.pgss";
    check("saveSnapshot", saveSnapshot(path, cpu, mem));

    // Control: record the next 200k PCs from the save point.
    std::vector<uint32_t> control; control.reserve(200000);
    for (int i = 0; i < 200000; ++i) {
        control.push_back((uint32_t(cpu.getPBR()) << 16) | cpu.getPC());
        int c = cpu.run(1); mem.tick(c);
    }
    const uint16_t aAfter = cpu.getA();

    run(cpu, mem, 1000000);                            // diverge further

    check("loadSnapshot", loadSnapshot(path, cpu, mem));
    // Replay: the same 200k PCs must retrace exactly.
    bool same = true; uint32_t firstDiff = 0; int diffAt = -1;
    for (int i = 0; i < 200000; ++i) {
        const uint32_t pc = (uint32_t(cpu.getPBR()) << 16) | cpu.getPC();
        if (pc != control[size_t(i)]) { same = false; firstDiff = pc; diffAt = i; break; }
        int c = cpu.run(1); mem.tick(c);
    }
    if (!same) std::printf("  diverged at instr %d: got %06X want %06X\n",
                           diffAt, firstDiff, control[size_t(diffAt)]);
    check("restored run retraces 200k instructions exactly", same);
    check("registers coherent after replay", cpu.getA() == aAfter);

    std::remove(path);
    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
