// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See LICENSE.
//
// ── WDC 65C816 CPU core ──────────────────────────────────────────────────
// The 16-bit CPU of the Apple IIgs. Emulation mode (E=1) is a 65C02 superset
// and REPLACES POM2's separate M6502 (POMIIGS ships one CPU, not two).
//
// Interface deliberately mirrors POM2 `M6502` so `EmulationController` forks
// cleanly: constructed with a bus pointer, `run(maxCycles) -> actualCycles`,
// wire-OR IRQ source mask, soft/hard reset, snapshot register accessors.
//
// The opcode table returns *architectural* cycle counts (WDC datasheet). The
// 2.8 MHz-fast / 1.02 MHz-slow effective clock and the slow-side access
// penalty are applied by IIgsMemory, NOT here — same clock-agnostic split
// POM2 uses, so Tom Harte cycle-count vectors stay valid.
//
// Source of truth: MAME `cpu/g65816/` + wiring in `apple2gs.cpp`; WDC
// W65C816S datasheet. Gate: `tomharte_65816` (SingleStepTests/65816).

#ifndef POMIIGS_CPU65816_H
#define POMIIGS_CPU65816_H

#include <atomic>
#include <cstdint>

class IIgsMemory;   // 24-bit banked bus (fast FPI side + slow Mega II side)

class CPU65816
{
public:
    // Processor status bits. In native mode M/X select register widths; in
    // emulation mode (E=1) the byte at bit 4/5 aliases B/1 as on the 65C02.
    struct Status {
        static constexpr uint8_t N = 0x80;  // negative
        static constexpr uint8_t V = 0x40;  // overflow
        static constexpr uint8_t M = 0x20;  // accumulator/memory width (0=16-bit) — native only
        static constexpr uint8_t X = 0x10;  // index width (0=16-bit) — native only; B in E-mode
        static constexpr uint8_t D = 0x08;  // decimal
        static constexpr uint8_t I = 0x04;  // IRQ disable
        static constexpr uint8_t Z = 0x02;  // zero
        static constexpr uint8_t C = 0x01;  // carry
    };

    CPU65816();
    explicit CPU65816(IIgsMemory* mem);

    void start();
    void stop();
    void softReset();   // RAM survives; forces E=1, I set, PC=($00FFFC); MAME reset_w
    void hardReset();   // + zeros A/X/Y/D/DBR/PBR

    // ── Execution ────────────────────────────────────────────────────────
    void step();                 // one instruction
    int  run(int maxCycles);     // run >= maxCycles architectural cycles; returns actual
    bool isRunning() const { return running_; }

    // ── IRQ / NMI (wire-OR, identical policy to POM2 M6502) ──────────────
    enum IrqSource : int {
        IRQ_SRC_SLOT1 = 1, IRQ_SRC_SLOT2, IRQ_SRC_SLOT3, IRQ_SRC_SLOT4,
        IRQ_SRC_SLOT5, IRQ_SRC_SLOT6, IRQ_SRC_SLOT7,
        IRQ_SRC_VGC_SCANLINE = 8,   // VGC scanline interrupt
        IRQ_SRC_VGC_VBL      = 9,   // VGC vertical-blank
        IRQ_SRC_DOC          = 10,  // Ensoniq 5503 oscillator IRQ
        IRQ_SRC_ADB          = 11,  // ADB / keyboard
        IRQ_SRC_SCC          = 12,  // SCC 8530 serial
        IRQ_SRC_MEGA2_VBL    = 13,  // Mega II 1/4-sec + VBL ($C032/$C041)
        IRQ_SRC_LEGACY       = 31,
    };
    void setIrqLine(int sourceId, bool asserted);
    uint32_t getIrqSourceMask() const { return irqSourceMask_.load(std::memory_order_relaxed); }
    void setNMI();

    // ── Snapshot / debugger register file (16-bit + bank regs) ───────────
    // Wider than the 6502 file: A/X/Y are 16-bit, SP and D are 16-bit, and
    // there are program (PBR) and data (DBR) bank registers plus the E flag.
    uint16_t getA()  const { return a_; }
    uint16_t getX()  const { return x_; }
    uint16_t getY()  const { return y_; }
    uint16_t getSP() const { return sp_; }
    uint16_t getD()  const { return d_; }      // direct page register
    uint8_t  getDBR() const { return dbr_; }   // data bank
    uint8_t  getPBR() const { return pbr_; }   // program bank
    uint16_t getPC() const { return pc_; }
    uint8_t  getP()  const { return p_; }
    bool     getEmulationMode() const { return emulation_; }

    void setA(uint16_t v)  { a_ = v; }
    void setX(uint16_t v)  { x_ = v; }
    void setY(uint16_t v)  { y_ = v; }
    void setSP(uint16_t v) { sp_ = v; }
    void setD(uint16_t v)  { d_ = v; }
    void setDBR(uint8_t v) { dbr_ = v; }
    void setPBR(uint8_t v) { pbr_ = v; }
    void setP(uint8_t v)   { p_ = v; }
    void setPC(uint16_t v) { pc_ = v; }        // back-door for the Klaus/Harte harness
    void setEmulationMode(bool e) { emulation_ = e; }

    // Sub-instruction cycle accounting — same contract as POM2 M6502 so the
    // speaker/DOC/VIA timestamp $C0xx accesses to sub-opcode precision.
    int      getCurrentInstructionCycles() const { return cycles_; }
    uint64_t getCycleCountNow() const;

private:
    IIgsMemory* memory_ = nullptr;

    // 16-bit register file (8-bit halves used when M/X or E force narrow width)
    uint16_t a_ = 0, x_ = 0, y_ = 0, sp_ = 0x01FF, d_ = 0, pc_ = 0;
    uint8_t  p_ = Status::M | Status::X | Status::I;   // native widths default 8-bit-ish
    uint8_t  dbr_ = 0, pbr_ = 0;
    bool     emulation_ = true;    // reset enters emulation mode

    std::atomic<int>      IRQ_{0};
    std::atomic<uint32_t> irqSourceMask_{0};
    int  NMI_ = 0;

    int  cycles_ = 0;
    bool running_ = false;

    // Milestone 1 fills in: mode-aware fetch, the addressing-mode helpers
    // (adds long / [dp] / stack-relative / block-move over the M6502 set),
    // the opcode dispatch, and the E/M/X width plumbing. See DEV.md § CPU.
};

#endif // POMIIGS_CPU65816_H
