// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── IIgs memory bus ──────────────────────────────────────────────────────
// M1 stub: a flat 16 MB address space so the 65C816 core is testable in
// isolation against Tom Harte SingleStepTests/65816 (which model a flat bus).
//
// M2 replaces the internals with the real FPI (2.8 MHz fast side) + Mega II
// (1.02 MHz slow side) model — bank shadowing, speed/state registers, the
// $C0xx I/O + GLU space, language card, and the slow-side access penalty.
// The public read8/write8(addr24) interface stays stable across that change,
// exactly like POM2's Memory::memRead/memWrite are the CPU's only bus hook.
//
// Source of truth (from M2): MAME `apple2gs.cpp`.

#ifndef POMIIGS_IIGSMEMORY_H
#define POMIIGS_IIGSMEMORY_H

#include <algorithm>
#include <cstdint>
#include <vector>

class IIgsMemory
{
public:
    static constexpr uint32_t kAddrMask = 0x00FFFFFF;   // 24-bit / 16 MB
    static constexpr uint32_t kSize     = 0x01000000;

    IIgsMemory() : ram_(kSize, 0) {}

    // The 65C816's only bus hooks. Non-virtual (POM2 convention): the CPU
    // holds a concrete IIgsMemory* so the hot path inlines and Tom Harte's
    // final-RAM check reads the same array the opcode wrote.
    inline uint8_t read8(uint32_t addr24) const { return ram_[addr24 & kAddrMask]; }
    inline void    write8(uint32_t addr24, uint8_t v) { ram_[addr24 & kAddrMask] = v; }

    // Test/reset helpers.
    void clear() { std::fill(ram_.begin(), ram_.end(), uint8_t(0)); }

private:
    std::vector<uint8_t> ram_;
};

#endif // POMIIGS_IIGSMEMORY_H
