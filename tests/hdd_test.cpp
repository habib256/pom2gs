// POMIIGS — hard-disk boot + persistence gate.
//
// Pins the two fixes that let GS/OS boot to the Finder with a hard disk mounted
// (see CHANGELOG "GS/OS + hard disk"). These are fast unit checks; the full
// boot-to-Finder is exercised by the tests/hdd_trace dev tool.
//
//   1. Emulation-mode hardware-vector pulls read ROM (the IIgs VP line forces
//      the fetch to ROM even under language-card RAM), like native mode. GS/OS
//      drops to emulation mode to call the ProDOS-8 block driver while
//      enumerating a slot-7 hard disk WITHOUT masking IRQs; a VBL IRQ in that
//      window must vector through ROM ($00FFFE = $C074), not uninitialised LC
//      RAM ($0000). Without this the boot derails to $00:0000.
//   2. The slot-7 boot firmware, on a non-bootable (blank/install-target) disk,
//      re-points the boot-slot globals to slot 5 and re-issues the ROM's own
//      JMP ($0000), so GS/OS loads START.GS.OS from the 3.5" — not the HDD.
//   3. Block writes persist to the backing file so a format / install sticks.

#include "CPU65816.h"
#include "IIgsMemory.h"
#include "ProDosHdd.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } }

int main() {
    // ── 1. Emulation-mode IRQ vector reads ROM, not LC RAM ──────────────────
    {
        std::vector<uint8_t> rom(0x40000, 0x00);
        // ROM occupies banks $FC-$FF; bank $FF offset 0x30000. Put a recognisable
        // emulation IRQ vector at $FF/FFFE-FFFF = $ABCD.
        rom[0x30000 + 0xFFFE] = 0xCD;
        rom[0x30000 + 0xFFFF] = 0xAB;
        IIgsMemory mem;
        mem.loadRom(rom);
        mem.reset();
        CPU65816 cpu(&mem);
        mem.setCpu(&cpu);
        cpu.hardReset();                     // emulation mode, I set

        // Poison the language-card RAM IRQ vector so a wrong (RAM) read is caught:
        // enable LC RAM read+write ($C083 twice) then write $0000 to $FFFE/$FFFF.
        mem.read8(0xC083); mem.read8(0xC083);
        mem.write8(0xFFFE, 0x00); mem.write8(0xFFFF, 0x00);

        // Clear I so the IRQ is taken, assert the line, single-step once.
        uint8_t p = cpu.getP(); p &= uint8_t(~0x04); cpu.setP(p);
        cpu.setIrqLine(CPU65816::IRQ_SRC_MEGA2_VBL, true);
        check("emul mode starts E=1", cpu.getEmulationMode());
        cpu.run(1);                          // services the pending IRQ
        check("emul IRQ vectored through ROM ($ABCD), not LC RAM ($0000)",
              cpu.getPC() == 0xABCD);
    }

    // ── 2. Slot-7 boot firmware re-points the boot slot to $C5 on chain ─────
    {
        ProDosHdd hdd(7, false);
        // The chain stub lives in the $Cn08 scratch area: LDA #$C5 / STA $01 /
        // STZ $00 / LDA #$C5 / STA $07F8 / JMP ($0000).
        check("chain sets $01=$C5 (A9 C5 85 01)",
              hdd.romRead(0x08) == 0xA9 && hdd.romRead(0x09) == 0xC5 &&
              hdd.romRead(0x0A) == 0x85 && hdd.romRead(0x0B) == 0x01);
        check("chain zeroes $00 then sets MSLOT $07F8=$C5 (64 00 / A9 C5 / 8D F8 07)",
              hdd.romRead(0x0C) == 0x64 && hdd.romRead(0x0D) == 0x00 &&
              hdd.romRead(0x0E) == 0xA9 && hdd.romRead(0x0F) == 0xC5 &&
              hdd.romRead(0x10) == 0x8D && hdd.romRead(0x11) == 0xF8 && hdd.romRead(0x12) == 0x07);
        check("chain re-issues JMP ($0000) (6C 00 00)",
              hdd.romRead(0x13) == 0x6C && hdd.romRead(0x14) == 0x00 && hdd.romRead(0x15) == 0x00);
    }

    // ── 3. Block writes persist to the backing file ─────────────────────────
    {
        const char* path = "/tmp/pomiigs_hdd_test.hdv";
        { std::vector<uint8_t> blank(64 * 512, 0); std::ofstream o(path, std::ios::binary);
          o.write(reinterpret_cast<const char*>(blank.data()), std::streamsize(blank.size())); }
        { ProDosHdd hdd(7, false); hdd.loadImage(path);
          uint8_t buf[512]; std::memset(buf, 0x5A, sizeof(buf));
          check("writeBlock ok", hdd.writeBlock(10, buf)); }
        // Reload from disk: the write must have hit the file.
        ProDosHdd re(7, false); re.loadImage(path);
        uint8_t rb[512]; re.readBlock(10, rb);
        check("block persisted across reload", rb[0] == 0x5A && rb[511] == 0x5A);
    }

    if (fails == 0) std::printf("hdd_test: all checks passed\n");
    return fails ? 1 : 0;
}
