// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Interrupt gate (P4). Exercises the Mega II / VGC / DOC interrupt sources
// through their real register protocol and checks the wire-OR CPU IRQ line
// (getIrqSourceMask) asserts only when the source is both flagged and enabled,
// and clears through the documented clear register.
//   ¼-second  : INTEN $C041 bit4 / INTFLAG $C046 bit4 / clear $C047
//   1-second  : VGCINT $C023 bit2 en / bit6 status / clear $C032
//   scan-line : VGCINT bit1 en / bit5 status (SHR + SCB bit6) / clear $C032
//   DOC osc   : IRQ-enabled oscillator completes → CPU IRQ, cleared by reading
//               the osc-interrupt register ($E0) via the Sound GLU.

#include "IIgsMemory.h"
#include "CPU65816.h"
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    IIgsMemory mem;
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);

    auto io  = [](uint16_t r) { return uint32_t(0xE0) << 16 | (0xC000 | r); };
    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };
    auto line = [&](int src) { return (cpu.getIrqSourceMask() & (1u << src)) != 0; };
    const int MEGA2 = CPU65816::IRQ_SRC_MEGA2_VBL;
    const int VGC   = CPU65816::IRQ_SRC_VGC_VBL;
    const int DOC   = CPU65816::IRQ_SRC_DOC;

    // ── ¼-second (Mega II) ───────────────────────────────────────────────
    mem.reset(); cpu.hardReset();
    mem.write8(io(0x41), 0x10);                       // INTEN: ¼-sec enable
    for (int i = 0; i < 15; ++i) mem.frameTick();     // 15 frames: not yet (fires on the 16th, MAME/KEGS)
    check("quarter not yet at 15", (mem.read8(io(0x46)) & 0x10) == 0);
    mem.frameTick();                                  // 16th frame → ¼-sec
    check("quarter INTFLAG bit4", (mem.read8(io(0x46)) & 0x10) != 0);
    check("quarter IRQ line",     line(MEGA2));
    mem.write8(io(0x47), 0);                          // CLRVBLINT
    check("quarter cleared",      !line(MEGA2));

    // masked: flag sets but line stays low without the enable
    mem.reset(); cpu.hardReset();
    for (int i = 0; i < 16; ++i) mem.frameTick();
    check("quarter masked flag",  (mem.read8(io(0x46)) & 0x10) != 0);
    check("quarter masked line",  !line(MEGA2));

    // ── 1-second (VGC) ───────────────────────────────────────────────────
    mem.reset(); cpu.hardReset();
    mem.write8(io(0x23), 0x04);                       // VGCINT: 1-sec enable
    for (int i = 0; i < 60; ++i) mem.frameTick();     // 60 frames → 1-sec
    check("1sec status bit6", (mem.read8(io(0x23)) & 0x40) != 0);
    check("1sec IRQ bit7",    (mem.read8(io(0x23)) & 0x80) != 0);
    check("1sec IRQ line",    line(VGC));
    mem.write8(io(0x32), 0);                          // VGCINTCLEAR
    check("1sec cleared",     !line(VGC));

    // ── scan-line (VGC, SHR + SCB bit6) ──────────────────────────────────
    mem.reset(); cpu.hardReset();
    mem.write8(io(0x29), 0x80);                       // NEWVIDEO: SHR on
    mem.write8((uint32_t(0xE1) << 16) | 0x9D05, 0x40);// SCB line 5 requests scan IRQ
    mem.write8(io(0x23), 0x02);                       // VGCINT: scan-line enable
    mem.frameTick();
    check("scanline status bit5", (mem.read8(io(0x23)) & 0x20) != 0);
    check("scanline IRQ line",    line(VGC));
    mem.write8(io(0x32), 0);
    check("scanline cleared",     !line(VGC));

    // ── DOC oscillator IRQ ───────────────────────────────────────────────
    mem.reset(); cpu.hardReset();
    // Program osc 0 control ($A0) = one-shot(mode1) + IRQ-enable, not halted.
    mem.write8(io(0x3C), 0x00);                       // GLU ctl: DOC-reg mode
    mem.write8(io(0x3E), 0xA0);                       // addr low  = $A0
    mem.write8(io(0x3F), 0x00);                       // addr high = $00
    mem.write8(io(0x3D), 0x0A);                       // reg[$A0] = one-shot | IRQ-en
    { int16_t buf[4]; mem.doc().render(buf, 1); }     // sndRam[0]=0 → osc halts + IRQ
    check("DOC irqPending", mem.doc().irqPending());
    mem.frameTick();                                  // MMU mirrors to CPU line
    check("DOC IRQ line", line(DOC));
    // Read the osc-interrupt register ($E0) → clears.
    mem.write8(io(0x3C), 0x00);
    mem.write8(io(0x3E), 0xE0);
    mem.write8(io(0x3F), 0x00);
    (void)mem.read8(io(0x3D));
    check("DOC IRQ cleared by $E0 read", !line(DOC));

    if (fails) { std::printf("irq_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: ¼-sec + 1-sec + scan-line + DOC interrupts (assert/mask/clear)\n");
    return 0;
}
