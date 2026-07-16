// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ADB GLU mouse + keyboard-modifier gate. Exercises the register protocol the
// IIgs firmware/toolbox uses to read the mouse and modifier keys:
//   $C024 MOUSEDATA — X then Y, b7 = button (0 = down), b6-0 = signed 7-bit delta
//   $C025 KEYMODREG — host modifier bits
//   $C027 KMSTATUS  — b7 mouse-data-available, b1 mouse X/Y select
// Source: Apple IIgs Hardware Reference (ADB GLU); MAME apple2gs keyglu.

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
    mem.reset();

    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };
    auto rd = [&](uint16_t off) { return mem.read8((uint32_t(0x00) << 16) | off); };  // bank-0 I/O

    // 1) At reset: no mouse data pending, button up (b7 = 1), X/Y select = X.
    check("reset: KMSTATUS b7 clear", (rd(0xC027) & 0x80) == 0);
    check("reset: KMSTATUS b1 clear", (rd(0xC027) & 0x02) == 0);

    // 2) Movement makes data available and reads back X then Y with the toggle.
    mem.mouseMove(10, -5);
    check("move: data available (b7)", (rd(0xC027) & 0x80) != 0);
    uint8_t x = rd(0xC024);
    check("X delta = 10", (x & 0x7F) == 10);
    check("X button up (b7=1)", (x & 0x80) != 0);
    check("after X read: X/Y select = Y (b1)", (rd(0xC027) & 0x02) != 0);
    uint8_t y = rd(0xC024);
    check("Y delta = -5 (7-bit)", (y & 0x7F) == uint8_t(-5 & 0x7F));
    check("after Y read: data cleared (b7=0)", (rd(0xC027) & 0x80) == 0);
    check("after Y read: X/Y back to X (b1=0)", (rd(0xC027) & 0x02) == 0);

    // 3) Button down → b7 = 0 in both axis reads.
    mem.mouseButton(true);
    check("button down: data available", (rd(0xC027) & 0x80) != 0);
    check("button down: X read b7=0", (rd(0xC024) & 0x80) == 0);
    check("button down: Y read b7=0", (rd(0xC024) & 0x80) == 0);

    // 4) Deltas accumulate between reads and clamp to signed 7-bit.
    mem.mouseMove(100, 0);
    mem.mouseMove(100, 0);                 // 200 → clamps to +63
    check("clamp +: X = 63", (rd(0xC024) & 0x7F) == 63);
    rd(0xC024);                            // consume Y
    mem.mouseMove(-100, 0);
    mem.mouseMove(-100, 0);                // -200 → clamps to -64
    check("clamp -: X = -64", (rd(0xC024) & 0x7F) == uint8_t(-64 & 0x7F));
    rd(0xC024);

    // 5) Keyboard modifiers pass straight through to KEYMODREG.
    mem.setKeyModifiers(0x83);             // command + control + shift
    check("KEYMODREG = 0x83", rd(0xC025) == 0x83);

    // 6) KMSTATUS b6 (mouse interrupt) mirrors b7 so the ROM interrupt manager
    //    ($FF:BE31) dispatches ReadMouse.
    mem.mouseMove(1, 1);
    check("KMSTATUS b6+b7 set on move", (rd(0xC027) & 0xC0) == 0xC0);
    rd(0xC024); rd(0xC024);                // consume X+Y
    check("KMSTATUS b6+b7 clear after read", (rd(0xC027) & 0xC0) == 0x00);

    // 7) Mouse IRQ is storm-safe: raised only in native mode with VBL interrupts
    //    enabled (proxy for "GS/OS interrupt system is up"), and cleared when the
    //    $C024 pair is read.
    auto adbIrq = [&] { return (cpu.getIrqSourceMask() & (1u << CPU65816::IRQ_SRC_ADB)) != 0; };
    cpu.setEmulationMode(true);            // emulation (early boot): no ADB IRQ
    mem.write8(0x00C041, 0x08);            // enable VBL int
    mem.mouseMove(2, 2);
    check("no ADB IRQ in emulation mode", !adbIrq());
    rd(0xC024); rd(0xC024);
    cpu.setEmulationMode(false);           // native, VBL enabled
    mem.write8(0x00C041, 0x08);
    mem.mouseMove(3, 3);
    check("ADB IRQ raised (native + VBL on)", adbIrq());
    rd(0xC024); rd(0xC024);                // ReadMouse consumes the sample
    check("ADB IRQ cleared after $C024 read", !adbIrq());

    // 8) Keyboard interrupt: a key latches the ASCII at $C000, posts the $C026
    //    routing byte + $C027 b5, and raises the ADB IRQ (native+VBL); the ROM
    //    reads $C000 then clears the strobe via $C010, which drops the IRQ.
    check("no key pending initially", (rd(0xC027) & 0x20) == 0);
    mem.keyDown('S');                      // 'S' → $C000 = $D3
    check("$C000 latch = 'S'|strobe", rd(0xC000) == 0xD3);
    check("KMSTATUS b5 (data) set", (rd(0xC027) & 0x20) != 0);
    check("$C026 routes to kbd handler ($40)", rd(0xC026) == 0x40);
    check("keyboard raises ADB IRQ", adbIrq());
    rd(0xC010);                            // strobe clear consumes the key event
    check("ADB IRQ cleared after $C010", !adbIrq());
    check("KMSTATUS b5 clear after $C010", (rd(0xC027) & 0x20) == 0);

    if (fails == 0) std::printf("adb_test OK\n");
    return fails ? 1 : 0;
}
