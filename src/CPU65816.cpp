// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// WDC 65C816 core. See CPU65816.h + DEV.md § CPU. Gate: tomharte_65816.
//
// Design: step() fetches one opcode from PBR:PC and dispatches a big switch.
// `this`-capturing lambdas provide width-aware (8/16-bit) memory access and
// the full addressing-mode → 24-bit effective-address set, so the header
// stays the public interface only (POM2 keeps its op helpers in the header;
// we localise them for the wider 816 mode matrix instead).
//
// Cycle counts follow the WDC W65C816S datasheet: base counts + the mode-
// dependent add-cycles (m=0 / x=0 width, DL!=0 direct page, page cross,
// branch taken). Applied here as architectural cycles; the 2.8/1.02 MHz
// effective clock is IIgsMemory's job (M2), not this table's.

#include "CPU65816.h"
#include "IIgsMemory.h"

CPU65816::CPU65816() = default;
CPU65816::CPU65816(IIgsMemory* mem) : memory_(mem) {}

void CPU65816::start() { running_ = true; }
void CPU65816::stop()  { running_ = false; }

void CPU65816::setNMI() { NMI_ = 1; }

void CPU65816::setIrqLine(int sourceId, bool asserted) {
    uint32_t mask = irqSourceMask_.load(std::memory_order_relaxed);
    const uint32_t bit = (sourceId >= 0 && sourceId < 32) ? (1u << sourceId) : 0;
    if (asserted) mask |= bit; else mask &= ~bit;
    irqSourceMask_.store(mask, std::memory_order_relaxed);
    IRQ_.store(mask != 0 ? 1 : 0, std::memory_order_relaxed);
}

void CPU65816::recordBusCycle(uint32_t address, uint8_t value, bool write, uint8_t roles) {
    if (!busTraceEnabled_) return;
    BusCycle cycle;
    cycle.address = address & IIgsMemory::kAddrMask;
    cycle.value = value;
    cycle.write = write;
    cycle.vda = (roles & BUS_DATA) != 0;
    cycle.vpa = (roles & BUS_PROGRAM) != 0;
    cycle.vpb = (roles & BUS_VECTOR) != 0;
    const bool halt = (roles & BUS_HALT) != 0;
    cycle.halt = halt;
    cycle.e = !halt && emulation_;
    cycle.m = !halt && (emulation_ || (p_ & Status::M) != 0);
    cycle.x = !halt && (emulation_ || (p_ & Status::X) != 0);
    cycle.mlb = (roles & BUS_LOCK) != 0;
    busTrace_.push_back(cycle);
}

void CPU65816::softReset() {
    // Reset always enters emulation mode with an 8-bit page-1 stack.
    emulation_ = true;
    p_ |= (Status::M | Status::X | Status::I);
    p_ &= ~Status::D;
    dbr_ = 0; pbr_ = 0;
    d_ = 0;
    sp_ = 0x0100 | (sp_ & 0xFF);
    x_ &= 0xFF; y_ &= 0xFF;
    // RESET is a vector pull like IRQ/NMI/ABORT: the FPI forces $00FFFC/FFFD to
    // ROM regardless of the language card (see IIgsMemory::vectorPull and the
    // note in serviceInt). Reading it through the normal bus only happened to
    // work because every current caller resets the MMU first (which parks the LC
    // in read-ROM mode); a bare softReset() with LC RAM banked in — what the
    // "RAM survives" contract invites — pulled an uninitialised RAM vector.
    if (memory_) pc_ = uint16_t(memory_->vectorPull(0xFFFC) | (memory_->vectorPull(0xFFFD) << 8));
    IRQ_.store(0, std::memory_order_relaxed);
    irqSourceMask_.store(0, std::memory_order_relaxed);
    NMI_ = 0;
    waiting_ = false;          // RESET breaks a WAI stall
}

void CPU65816::hardReset() {
    a_ = x_ = y_ = 0;
    d_ = 0; dbr_ = pbr_ = 0;
    sp_ = 0x01FF;
    p_ = Status::M | Status::X | Status::I;
    softReset();
}

// ── width predicates ────────────────────────────────────────────────────
namespace {
inline bool bit(uint8_t p, uint8_t b) { return (p & b) != 0; }
}

int CPU65816::run(int maxCycles) {
    running_ = true;
    int total = 0;
    do {
        step();               // step() resets cycles_ to 0 then counts this opcode
        total += cycles_;
    } while (running_ && total < maxCycles);
    return total;
}

void CPU65816::step() {
    IIgsMemory& m = *memory_;
    cycles_ = 0;

    const bool eM = emulation_ || bit(p_, Status::M);   // 8-bit accumulator/memory
    const bool eX = emulation_ || bit(p_, Status::X);   // 8-bit index

    // Hardware register invariants, enforced continuously (the CPU can never
    // observe them violated): in emulation mode SPH is hardwired to $01, and
    // whenever the index width is 8-bit the index high bytes read as 0.
    if (emulation_) sp_ = uint16_t(0x0100 | (sp_ & 0xFF));
    if (eX) { x_ &= 0xFF; y_ &= 0xFF; }

    // ── raw bus helpers (24-bit) ─────────────────────────────────────────
    // Every cycle of the instruction goes through exactly one of these: rd/wr/
    // fetch/rdVector for the active bus transactions and io/ioAt/RMW for the
    // inactive (VDA=VPA=VPB low) cycles. cycles_ therefore equals the number
    // of traced cycles, and the MMU sees every cycle in order, which is what
    // lets it phase each transaction against the Mega II grid and the DRAM
    // refresh window (IIgsMemory::internalCycles).
    uint32_t lastAddr = (uint32_t(pbr_) << 16) | pc_;   // last value on the address bus
    bool lock = false;                                   // MLB: inside a read-modify-write
    auto rd = [&](uint32_t a) -> uint8_t {
        ++cycles_;
        lastAddr = a;
        const uint8_t v = m.read8(a);
        recordBusCycle(a, v, false, uint8_t(BUS_DATA | (lock ? BUS_LOCK : 0)));
        return v;
    };
    auto wr = [&](uint32_t a, uint8_t v) {
        ++cycles_;
        lastAddr = a;
        m.write8(a, v);
        recordBusCycle(a, v, true, uint8_t(BUS_DATA | (lock ? BUS_LOCK : 0)));
    };
    // Vector pull: VPB is orthogonal to VDA.
    auto rdVector = [&](uint16_t a, bool forceRom) -> uint8_t {
        ++cycles_;
        lastAddr = a;
        const uint8_t v = forceRom ? m.vectorPull(a) : m.read8(a);
        recordBusCycle(a, v, false, BUS_DATA | BUS_VECTOR);
        return v;
    };
    // Inactive cycle(s). The address bus keeps the last address for the IO
    // cycles that follow an operand or data access (WDC W65C816S datasheet
    // Table 5-7, confirmed by the SingleStepTests corpus); ioAt() serves the
    // cases that show something else (PC+1 on implied opcodes, the effective
    // address on an indexed page-cross).
    auto ioAt = [&](uint32_t a, int n = 1) {
        cycles_ += n;
        m.internalCycles(uint32_t(n));
        lastAddr = a;
        for (int i = 0; i < n; ++i) recordBusCycle(a, 0, false, lock ? BUS_LOCK : 0);
    };
    auto io = [&](int n = 1) { ioAt(lastAddr, n); };
    // WAI/STP third internal cycle: the chip parks with the address bus and
    // the E/M/X outputs released (SingleStepTests corpus: address null, all
    // pins low). Timed like any internal cycle.
    auto haltCycle = [&]() {
        ++cycles_;
        m.internalCycles(1);
        recordBusCycle(0, 0, false, BUS_HALT);
    };

    // Program-counter fetch in the program bank (PC wraps within the bank).
    bool opcodeFetch = true;
    auto fetch = [&]() -> uint8_t {
        const uint32_t a = (uint32_t(pbr_) << 16) | pc_;
        ++cycles_;
        lastAddr = a;
        const uint8_t v = m.read8(a);
        recordBusCycle(a, v, false, uint8_t(BUS_PROGRAM | (opcodeFetch ? BUS_DATA : 0)));
        opcodeFetch = false;
        pc_ = uint16_t(pc_ + 1);
        return v;
    };
    auto fetch16 = [&]() -> uint16_t { uint16_t lo = fetch(); return uint16_t(lo | (fetch() << 8)); };

    // 16-bit data read/write. Absolute/long/indexed operands use the "bank
    // increment" wrap (the high byte can spill into the next bank); direct
    // page and stack-relative data (`wrap0`) always stay inside bank 0, so
    // $00FFFF+1 is $000000 there.
    auto nextA = [&](uint32_t a, bool wrap0) -> uint32_t {
        return wrap0 ? uint32_t(uint16_t(a + 1)) : ((a + 1) & IIgsMemory::kAddrMask);
    };
    auto rd16 = [&](uint32_t a, bool wrap0 = false) -> uint16_t {
        uint16_t lo = rd(a);
        return uint16_t(lo | (rd(nextA(a, wrap0)) << 8));
    };
    auto wr16 = [&](uint32_t a, uint16_t v, bool wrap0 = false) {
        wr(a, uint8_t(v));
        wr(nextA(a, wrap0), uint8_t(v >> 8));
    };

    // Direct-page 16-bit access wraps within bank 0; in emulation mode with
    // DL==0 it further wraps within the page (the classic 6502 quirk).
    auto rdDP16 = [&](uint16_t off) -> uint16_t {
        uint16_t a0 = uint16_t(d_ + off);
        uint8_t lo = rd(a0);
        uint16_t a1 = (emulation_ && (d_ & 0xFF) == 0)
                        ? uint16_t((a0 & 0xFF00) | ((a0 + 1) & 0xFF))
                        : uint16_t(a0 + 1);
        return uint16_t(lo | (rd(a1) << 8));
    };

    auto pushB = [&](uint8_t v) {
        wr(sp_, v);
        if (emulation_) sp_ = uint16_t(0x0100 | ((sp_ - 1) & 0xFF));
        else            sp_ = uint16_t(sp_ - 1);
    };
    auto pullB = [&]() -> uint8_t {
        if (emulation_) sp_ = uint16_t(0x0100 | ((sp_ + 1) & 0xFF));
        else            sp_ = uint16_t(sp_ + 1);
        return rd(sp_);
    };
    auto pushW = [&](uint16_t v) { pushB(uint8_t(v >> 8)); pushB(uint8_t(v)); };
    auto pullW = [&]() -> uint16_t { uint8_t lo = pullB(); return uint16_t(lo | (pullB() << 8)); };
    // "Raw" 16-bit stack ops used by the NEW 65816 instructions (PEA/PEI/PER/
    // PHD/PLD/JSL/RTL): even in emulation mode the SP is a full 16-bit counter
    // that may leave page 1 mid-op; SPH is reset to $01 at end of step. The old
    // 6502 stack ops keep using pushB/pullB (page-1 wrap).
    auto pushBraw = [&](uint8_t v) { wr(sp_, v); sp_ = uint16_t(sp_ - 1); };
    auto pullBraw = [&]() -> uint8_t { sp_ = uint16_t(sp_ + 1); return rd(sp_); };
    auto pushWraw = [&](uint16_t v) { pushBraw(uint8_t(v >> 8)); pushBraw(uint8_t(v)); };
    auto pullWraw = [&]() -> uint16_t { uint8_t lo = pullBraw(); return uint16_t(lo | (pullBraw() << 8)); };

    // ── flag helpers ─────────────────────────────────────────────────────
    auto setZN8  = [&](uint8_t v)  { p_ = (p_ & ~(Status::Z | Status::N)) | (v == 0 ? Status::Z : 0) | (v & 0x80); };
    auto setZN16 = [&](uint16_t v) { p_ = (p_ & ~(Status::Z | Status::N)) | (v == 0 ? Status::Z : 0) | ((v & 0x8000) ? Status::N : 0); };
    auto setZNa  = [&](uint16_t v) { if (eM) setZN8(uint8_t(v)); else setZN16(v); };

    // ── addressing modes → 24-bit effective address ─────────────────────
    // (DBR-relative for absolute data; bank-0 for direct page.)
    auto ea_dp    = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); return uint16_t(d_ + o); };
    // Direct-page indexed: in emulation mode with DL=0, the index add wraps
    // within page 0 (the 6502 quirk), same as ea_indx below.
    auto ea_dpx   = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); io(); uint16_t xx = eX ? (x_ & 0xFF) : x_;
        return (emulation_ && (d_ & 0xFF) == 0) ? uint16_t((d_ & 0xFF00) | uint8_t(o + xx)) : uint16_t(d_ + o + xx); };
    auto ea_dpy   = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); io(); uint16_t yy = eX ? (y_ & 0xFF) : y_;
        return (emulation_ && (d_ & 0xFF) == 0) ? uint16_t((d_ & 0xFF00) | uint8_t(o + yy)) : uint16_t(d_ + o + yy); };
    // Indexed page-cross penalty: the real chip spends one IO cycle when the
    // index is 16-bit (x=0) OR the low-byte add carries into a new page; a
    // write or read-modify-write always pays it (`always`). It sits between
    // the operand fetch and the data access, and the address bus shows the
    // partial sum — the high byte before the carry (datasheet "AAH,AAL+XL").
    auto idxIo = [&](uint32_t base, uint32_t e, bool always) {
        if (always || !eX || (((base ^ e) & 0xFF00) != 0)) ioAt((base & 0xFFFF00u) | (e & 0xFF));
    };
    auto ea_abs   = [&]() -> uint32_t { uint16_t a = fetch16(); return (uint32_t(dbr_) << 16) | a; };
    auto ea_absx  = [&](bool w = false) -> uint32_t { uint16_t a = fetch16(); uint32_t base = (uint32_t(dbr_) << 16) | a; uint32_t e = (base + (eX ? (x_ & 0xFF) : x_)) & IIgsMemory::kAddrMask; idxIo(base, e, w); return e; };
    auto ea_absy  = [&](bool w = false) -> uint32_t { uint16_t a = fetch16(); uint32_t base = (uint32_t(dbr_) << 16) | a; uint32_t e = (base + (eX ? (y_ & 0xFF) : y_)) & IIgsMemory::kAddrMask; idxIo(base, e, w); return e; };
    auto ea_absl  = [&]() -> uint32_t { uint16_t a = fetch16(); uint8_t b = fetch(); return (uint32_t(b) << 16) | a; };
    auto ea_abslx = [&]() -> uint32_t { uint16_t a = fetch16(); uint8_t b = fetch(); uint32_t base = (uint32_t(b) << 16) | a; return (base + (eX ? (x_ & 0xFF) : x_)) & IIgsMemory::kAddrMask; };
    auto ea_indx  = [&]() -> uint32_t {
        uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io();
        io();                                               // index add
        uint16_t xx = eX ? (x_ & 0xFF) : x_;
        uint8_t lo, hi;
        // Emulation mode: the pointer's HIGH byte is read within the page of
        // the low byte's address. With DL=0 both bytes stay in the direct page
        // (0:DH:(LL+X)&FF / 0:DH:(LL+X+1)&FF — Bruce Clark, "Investigating the
        // 65C816's Operation" §5.11); with DL!=0 the low byte is at the full
        // 16-bit D+LL+X while the +1 for the high byte wraps within that page
        // — a silicon quirk verified on hardware (gilyon cputest 0027, README
        // "undocumented behavior") and modelled by bsnes (wdc65816 readDirectX).
        // The SingleStepTests corpus reads the high byte at lo+1 in both cases;
        // the harness carries those page-crossing vectors as issue-linked
        // xfails (upstream issue #3 patched a single SBC vector).
        if (emulation_) {
            const uint16_t ploc = ((d_ & 0xFF) == 0) ? uint16_t((d_ & 0xFF00) | uint8_t(o + xx))
                                                     : uint16_t(d_ + o + xx);
            lo = rd(ploc);
            hi = rd(uint16_t((ploc & 0xFF00) | uint8_t(ploc + 1)));
        } else {
            uint16_t ptr = uint16_t(d_ + o + xx);
            lo = rd(ptr); hi = rd(uint16_t(ptr + 1));
        }
        return (uint32_t(dbr_) << 16) | uint16_t(lo | (hi << 8));
    };
    auto ea_indy  = [&](bool w = false) -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); uint16_t a = rdDP16(o); uint32_t base = ((uint32_t(dbr_) << 16) | a); uint32_t e = (base + (eX ? (y_ & 0xFF) : y_)) & IIgsMemory::kAddrMask; idxIo(base, e, w); return e; };
    auto ea_ind   = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); uint16_t a = rdDP16(o); return (uint32_t(dbr_) << 16) | a; };
    auto ea_indl  = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); uint16_t p0 = uint16_t(d_ + o); uint8_t lo = rd(p0); uint8_t mid = rd(uint16_t(p0 + 1)); uint8_t hi = rd(uint16_t(p0 + 2)); return (uint32_t(hi) << 16) | (mid << 8) | lo; };
    auto ea_indly = [&]() -> uint32_t { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io(); uint16_t p0 = uint16_t(d_ + o); uint8_t lo = rd(p0); uint8_t mid = rd(uint16_t(p0 + 1)); uint8_t hi = rd(uint16_t(p0 + 2)); uint32_t base = (uint32_t(hi) << 16) | (mid << 8) | lo; return (base + (eX ? (y_ & 0xFF) : y_)) & IIgsMemory::kAddrMask; };
    auto ea_sr    = [&]() -> uint32_t { uint8_t o = fetch(); io(); return uint16_t(sp_ + o); };
    auto ea_sriy  = [&]() -> uint32_t { uint8_t o = fetch(); io(); uint16_t p0 = uint16_t(sp_ + o); uint16_t a = uint16_t(rd(p0) | (rd(uint16_t(p0 + 1)) << 8)); io(); uint32_t base = (uint32_t(dbr_) << 16) | a; return (base + (eX ? (y_ & 0xFF) : y_)) & IIgsMemory::kAddrMask; };

    // Immediate operand (width-dependent). Returns value + advances PC.
    auto immA = [&]() -> uint16_t { if (eM) return fetch(); return fetch16(); };
    auto immX = [&]() -> uint16_t { if (eX) return fetch(); return fetch16(); };

    // Generic ALU on accumulator with a memory/immediate value at A-width.
    // `wrap0` = bank-0 wrap for direct-page / stack-relative operands.
    auto rdA = [&](uint32_t a, bool wrap0 = false) -> uint16_t { return eM ? rd(a) : rd16(a, wrap0); };
    auto wrA = [&](uint32_t a, uint16_t v, bool wrap0 = false) { if (eM) wr(a, uint8_t(v)); else wr16(a, v, wrap0); };
    auto rdX = [&](uint32_t a, bool wrap0 = false) -> uint16_t { return eX ? rd(a) : rd16(a, wrap0); };
    auto wrX = [&](uint32_t a, uint16_t v, bool wrap0 = false) { if (eX) wr(a, uint8_t(v)); else wr16(a, v, wrap0); };
    // Read-modify-write: read lo (hi), one internal "modify" cycle, write
    // hi then lo — the write-back order is reversed relative to a plain
    // 16-bit store. MLB is asserted from the first read to the last write. In
    // emulation mode the modify cycle is the 6502-style dummy write of the
    // unmodified byte (VDA low, so no memory is touched), in native mode a
    // plain internal cycle (SingleStepTests 65816 corpus; W65C816S Table 5-7).
    auto RMW = [&](uint32_t a, auto modify, bool wrap0 = false) -> uint16_t {
        lock = true;
        uint16_t v = rdA(a, wrap0);
        ++cycles_;
        m.internalCycles(1);
        if (emulation_) recordBusCycle(a, uint8_t(v), true, BUS_LOCK);
        else            recordBusCycle(lastAddr, 0, false, BUS_LOCK);
        v = modify(v);
        if (eM) wr(a, uint8_t(v));
        else { wr(nextA(a, wrap0), uint8_t(v >> 8)); wr(a, uint8_t(v)); }
        lock = false;
        return v;
    };

    auto ORA = [&](uint16_t v) { if (eM) { uint8_t r = uint8_t(a_) | uint8_t(v); a_ = (a_ & 0xFF00) | r; setZN8(r);} else { a_ |= v; setZN16(a_);} };
    auto AND = [&](uint16_t v) { if (eM) { uint8_t r = uint8_t(a_) & uint8_t(v); a_ = (a_ & 0xFF00) | r; setZN8(r);} else { a_ &= v; setZN16(a_);} };
    auto EOR = [&](uint16_t v) { if (eM) { uint8_t r = uint8_t(a_) ^ uint8_t(v); a_ = (a_ & 0xFF00) | r; setZN8(r);} else { a_ ^= v; setZN16(a_);} };

    auto ADC = [&](uint16_t v) {
        const bool dec = bit(p_, Status::D);
        const uint32_t c = bit(p_, Status::C) ? 1 : 0;
        if (eM) {
            uint8_t av = uint8_t(a_), bv = uint8_t(v); uint32_t r;
            if (!dec) { r = av + bv + c; }
            else {                                  // running-accumulator BCD (mirrors the passing 16-bit path)
                uint32_t s = (av & 0x0F) + (bv & 0x0F) + c;   if (s > 0x09) s += 0x06;
                s = (av & 0xF0) + (bv & 0xF0) + (s > 0x0F ? 0x10 : 0) + (s & 0x0F);
                bool v6 = (~(av ^ bv) & (av ^ (s & 0xFF)) & 0x80);
                p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0);
                if (s > 0x9F) s += 0x60;
                r = s;
            }
            if (!dec) { bool v6 = (~(av ^ bv) & (av ^ (r & 0xFF)) & 0x80); p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0); }
            p_ = (p_ & ~Status::C) | ((r > 0xFF) ? Status::C : 0);
            a_ = (a_ & 0xFF00) | uint8_t(r); setZN8(uint8_t(r));
        } else {
            uint16_t bv = v; uint32_t r;
            if (!dec) { r = a_ + bv + c; bool v6 = (~(a_ ^ bv) & (a_ ^ (r & 0xFFFF)) & 0x8000); p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0);}
            else {
                // 16-bit BCD, nibble by nibble (carry rippling up each digit).
                uint32_t s = (a_ & 0x000F) + (bv & 0x000F) + c;              if (s > 0x0009) s += 0x0006;
                s = (a_ & 0x00F0) + (bv & 0x00F0) + (s > 0x000F ? 0x10 : 0) + (s & 0x000F);  if (s > 0x009F) s += 0x0060;
                s = (a_ & 0x0F00) + (bv & 0x0F00) + (s > 0x00FF ? 0x100 : 0) + (s & 0x00FF); if (s > 0x09FF) s += 0x0600;
                s = (a_ & 0xF000) + (bv & 0xF000) + (s > 0x0FFF ? 0x1000 : 0) + (s & 0x0FFF);
                bool v6 = (~(a_ ^ bv) & (a_ ^ (s & 0xFFFF)) & 0x8000); p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0);
                if (s > 0x9FFF) s += 0x6000;
                r = s;
            }
            p_ = (p_ & ~Status::C) | ((r > 0xFFFF) ? Status::C : 0);
            a_ = uint16_t(r); setZN16(a_);
        }
    };
    auto SBC = [&](uint16_t v) {
        const bool dec = bit(p_, Status::D);
        const uint32_t c = bit(p_, Status::C) ? 1 : 0;
        if (eM) {
            uint8_t av = uint8_t(a_), bv = uint8_t(v);
            int32_t r = av + (bv ^ 0xFF) + c;
            bool v6 = ((av ^ bv) & (av ^ (r & 0xFF)) & 0x80);
            if (dec) {                              // bit-accurate BCD borrow (see 16-bit path)
                int32_t lo = (av & 0x0F) - (bv & 0x0F) - (1 - int32_t(c));
                int32_t hi = (av >> 4) - (bv >> 4);
                if (lo & 0x10) { lo -= 6; hi -= 1; }
                if (hi & 0x10) { hi -= 6; }
                r = (r > 0xFF ? 0x100 : 0) | ((hi & 0x0F) << 4) | (lo & 0x0F);
            }
            p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0);
            p_ = (p_ & ~Status::C) | ((r > 0xFF) ? Status::C : 0);
            a_ = (a_ & 0xFF00) | uint8_t(r); setZN8(uint8_t(r));
        } else {
            // V and C come from the *binary* difference in both modes.
            int32_t r = a_ + (v ^ 0xFFFF) + int32_t(c);
            bool v6 = ((a_ ^ v) & (a_ ^ (r & 0xFFFF)) & 0x8000);
            p_ = (p_ & ~Status::V) | (v6 ? Status::V : 0);
            p_ = (p_ & ~Status::C) | ((r > 0xFFFF) ? Status::C : 0);
            uint16_t res;
            if (!dec) res = uint16_t(r);
            else {   // bit-accurate BCD borrow: bit-4 test, -6 correction, mask
                int32_t n0 = int32_t(a_ & 0xF)         - int32_t(v & 0xF)         - int32_t(1 - c);
                int32_t n1 = int32_t((a_ >> 4)  & 0xF) - int32_t((v >> 4)  & 0xF);
                int32_t n2 = int32_t((a_ >> 8)  & 0xF) - int32_t((v >> 8)  & 0xF);
                int32_t n3 = int32_t((a_ >> 12) & 0xF) - int32_t((v >> 12) & 0xF);
                if (n0 & 0x10) { n0 -= 6; n1 -= 1; }
                if (n1 & 0x10) { n1 -= 6; n2 -= 1; }
                if (n2 & 0x10) { n2 -= 6; n3 -= 1; }
                if (n3 & 0x10) { n3 -= 6; }
                res = uint16_t(((n3 & 0xF) << 12) | ((n2 & 0xF) << 8) | ((n1 & 0xF) << 4) | (n0 & 0xF));
            }
            a_ = res; setZN16(a_);
        }
    };
    auto CMPr = [&](uint16_t reg, uint16_t v, bool w8) {
        if (w8) { uint8_t r = uint8_t(reg) - uint8_t(v); p_ = (p_ & ~Status::C) | (uint8_t(reg) >= uint8_t(v) ? Status::C : 0); setZN8(r);}
        else    { uint16_t r = uint16_t(reg - v); p_ = (p_ & ~Status::C) | (reg >= v ? Status::C : 0); setZN16(r);}
    };

    auto branch = [&](bool take) {
        int8_t off = int8_t(fetch());
        if (take) {
            uint16_t old = pc_;
            pc_ = uint16_t(pc_ + off);
            io();                                       // taken
            if (emulation_ && ((old ^ pc_) & 0xFF00)) io();  // page cross (e-mode only)
        }
    };

    // ── Hardware interrupts (sampled before the opcode fetch) ────────────
    // NMI is edge-triggered; IRQ is level, gated by the I flag. Both push
    // PBR(native)/PC/P (with the B flag CLEAR — distinguishing them from BRK)
    // and vector through the appropriate bank-0 vector. Nothing asserts these
    // lines yet (the MMU doesn't wire VBL/DOC IRQ to the CPU), so this is inert
    // until wired — but the mechanism must be correct.
    auto serviceInt = [&](uint16_t vecNative, uint16_t vecEmu) {
        // Two internal cycles first (the aborted opcode fetch and its
        // successor), then the pushes and the vector pull: 7 cycles in
        // emulation mode, 8 in native (WDC W65C816S datasheet; MAME
        // g65816cm.h g65816i_interrupt_hardware charges CLK(7)/CLK(8)).
        ioAt(lastAddr, 2);
        if (!emulation_) pushB(pbr_);
        pushW(pc_);
        pushB(uint8_t(emulation_ ? ((p_ & ~0x10) | 0x20) : p_));        // bit4(B)=0 → hardware int
        p_ |= Status::I; p_ &= ~Status::D;
        pbr_ = 0;
        uint16_t vec = emulation_ ? vecEmu : vecNative;
        // Hardware-vector pulls read ROM even with LC RAM banked in, in BOTH
        // native and emulation mode: the IIgs asserts the 65816 VP (vector-pull)
        // line, and the FPI forces the vector fetch to ROM regardless of the
        // language-card state (see IIgsMemory::vectorPull; flat-bus CPU tests
        // fall back to RAM). GS/OS never installs RAM interrupt vectors — its
        // handlers are reached through the fixed ROM stubs at $C071-$C07F
        // ($00FFFE emul IRQ = $00FFEE native = $C074 = the GS Interrupt Mgr).
        // This matters when GS/OS briefly drops to emulation mode to call a
        // ProDOS-8 block driver (e.g. enumerating a slot-7 hard disk) WITHOUT
        // masking IRQs: a VBL IRQ during that window pulled $00FFFE from LC RAM
        // ($0000) and derailed to $00:0000. (Diagnosed with tests/hdd_trace.)
        pc_ = uint16_t(rdVector(vec, true) |
                       (rdVector(uint16_t(vec + 1), true) << 8));
        // (This used to add a flat 7 on top of the pushes, billing every
        // IRQ/NMI 10/11 cycles instead of 7/8 — a ~40 % surcharge on every
        // interrupt, which stole time from scanline/DOC-IRQ-driven code.
        // bug-hunt finding, August 2026.)
    };
    // WAI ($CB) parks the CPU until an interrupt is *asserted*. The wake is
    // independent of the I flag: with I clear the interrupt is taken (returning
    // to the instruction after WAI), with I set the CPU simply resumes there —
    // the classic "wait for the beam / for the DOC" idiom. While parked the chip
    // still burns bus cycles, so bill one per poll and let the caller's tick()
    // advance the video/DOC clocks that will eventually raise the line.
    if (waiting_) {
        if (!NMI_ && !IRQ_.load(std::memory_order_relaxed)) { io(); return; }
        waiting_ = false;
    }
    if (NMI_) { NMI_ = 0; serviceInt(0xFFEA, 0xFFFA); return; }
    if (IRQ_.load(std::memory_order_relaxed) && !bit(p_, Status::I)) {
        serviceInt(0xFFEE, 0xFFFE); return;
    }

    const uint8_t opc = fetch();

    // Internal cycles that immediately follow the opcode fetch (W65C816S
    // datasheet Table 5-7): implied/accumulator register ops, XCE and NOP = 1;
    // pushes = 1; pulls = 2; XBA = 2; RTS/RTL/RTI = 2 (RTS spends a third one
    // after the pull, see case 0x60); WAI/STP = 2 plus the halt cycle in their
    // opcode body. The address bus shows PC+1.
    // Every other internal cycle is positioned inside the addressing-mode
    // helpers or the opcode body: the DL!=0 and index-add cycles of direct
    // page, the indexed page-cross cycle, the read-modify-write modify cycle,
    // the stack-relative and control-flow (JSR/JSL/JMP(abs,X)/RTS/REP/SEP/
    // BRL/PER/MVN/MVP) cycles. BRK/COP and the hardware interrupt have no
    // opcode-adjacent internal cycle besides the two the interrupt itself
    // spends before its pushes.
    static uint8_t leadIo[256] = {0};
    static bool leadInit = false;
    if (!leadInit) {
        leadInit = true;
        auto S = [&](std::initializer_list<int> ops, int n) { for (int o : ops) leadIo[o] = uint8_t(n); };
        S({0xAA,0xA8,0x8A,0x98,0x9A,0xBA,0x9B,0xBB,0x5B,0x7B,0x1B,0x3B,   // transfers
           0xE8,0xC8,0xCA,0x88, 0x1A,0x3A, 0x0A,0x4A,0x2A,0x6A,           // inc/dec/shift-A
           0x18,0x38,0x58,0x78,0xB8,0xD8,0xF8, 0xEA, 0xFB,                // flags/NOP/XCE
           0x48,0xDA,0x5A,0x08,0x8B,0x0B,0x4B}, 1);                       // pushes
        S({0x68,0xFA,0x7A,0x28,0xAB,0x2B, 0xEB, 0x60,0x6B,0x40, 0xCB,0xDB}, 2); // pulls, XBA, RTS/RTL/RTI, WAI/STP
    }
    if (leadIo[opc]) ioAt((uint32_t(pbr_) << 16) | pc_, leadIo[opc]);

    switch (opc) {
        // ── ORA ──
        case 0x09: ORA(immA()); break;
        case 0x05: ORA(rdA(ea_dp(), true)); break;
        case 0x15: ORA(rdA(ea_dpx(), true)); break;
        case 0x0D: ORA(rdA(ea_abs())); break;
        case 0x1D: ORA(rdA(ea_absx())); break;
        case 0x19: ORA(rdA(ea_absy())); break;
        case 0x01: ORA(rdA(ea_indx())); break;
        case 0x11: ORA(rdA(ea_indy())); break;
        case 0x12: ORA(rdA(ea_ind())); break;
        case 0x07: ORA(rdA(ea_indl())); break;
        case 0x17: ORA(rdA(ea_indly())); break;
        case 0x0F: ORA(rdA(ea_absl())); break;
        case 0x1F: ORA(rdA(ea_abslx())); break;
        case 0x03: ORA(rdA(ea_sr(), true)); break;
        case 0x13: ORA(rdA(ea_sriy())); break;
        // ── AND ──
        case 0x29: AND(immA()); break;
        case 0x25: AND(rdA(ea_dp(), true)); break;
        case 0x35: AND(rdA(ea_dpx(), true)); break;
        case 0x2D: AND(rdA(ea_abs())); break;
        case 0x3D: AND(rdA(ea_absx())); break;
        case 0x39: AND(rdA(ea_absy())); break;
        case 0x21: AND(rdA(ea_indx())); break;
        case 0x31: AND(rdA(ea_indy())); break;
        case 0x32: AND(rdA(ea_ind())); break;
        case 0x27: AND(rdA(ea_indl())); break;
        case 0x37: AND(rdA(ea_indly())); break;
        case 0x2F: AND(rdA(ea_absl())); break;
        case 0x3F: AND(rdA(ea_abslx())); break;
        case 0x23: AND(rdA(ea_sr(), true)); break;
        case 0x33: AND(rdA(ea_sriy())); break;
        // ── EOR ──
        case 0x49: EOR(immA()); break;
        case 0x45: EOR(rdA(ea_dp(), true)); break;
        case 0x55: EOR(rdA(ea_dpx(), true)); break;
        case 0x4D: EOR(rdA(ea_abs())); break;
        case 0x5D: EOR(rdA(ea_absx())); break;
        case 0x59: EOR(rdA(ea_absy())); break;
        case 0x41: EOR(rdA(ea_indx())); break;
        case 0x51: EOR(rdA(ea_indy())); break;
        case 0x52: EOR(rdA(ea_ind())); break;
        case 0x47: EOR(rdA(ea_indl())); break;
        case 0x57: EOR(rdA(ea_indly())); break;
        case 0x4F: EOR(rdA(ea_absl())); break;
        case 0x5F: EOR(rdA(ea_abslx())); break;
        case 0x43: EOR(rdA(ea_sr(), true)); break;
        case 0x53: EOR(rdA(ea_sriy())); break;
        // ── ADC ──
        case 0x69: ADC(immA()); break;
        case 0x65: ADC(rdA(ea_dp(), true)); break;
        case 0x75: ADC(rdA(ea_dpx(), true)); break;
        case 0x6D: ADC(rdA(ea_abs())); break;
        case 0x7D: ADC(rdA(ea_absx())); break;
        case 0x79: ADC(rdA(ea_absy())); break;
        case 0x61: ADC(rdA(ea_indx())); break;
        case 0x71: ADC(rdA(ea_indy())); break;
        case 0x72: ADC(rdA(ea_ind())); break;
        case 0x67: ADC(rdA(ea_indl())); break;
        case 0x77: ADC(rdA(ea_indly())); break;
        case 0x6F: ADC(rdA(ea_absl())); break;
        case 0x7F: ADC(rdA(ea_abslx())); break;
        case 0x63: ADC(rdA(ea_sr(), true)); break;
        case 0x73: ADC(rdA(ea_sriy())); break;
        // ── SBC ──
        case 0xE9: SBC(immA()); break;
        case 0xE5: SBC(rdA(ea_dp(), true)); break;
        case 0xF5: SBC(rdA(ea_dpx(), true)); break;
        case 0xED: SBC(rdA(ea_abs())); break;
        case 0xFD: SBC(rdA(ea_absx())); break;
        case 0xF9: SBC(rdA(ea_absy())); break;
        case 0xE1: SBC(rdA(ea_indx())); break;
        case 0xF1: SBC(rdA(ea_indy())); break;
        case 0xF2: SBC(rdA(ea_ind())); break;
        case 0xE7: SBC(rdA(ea_indl())); break;
        case 0xF7: SBC(rdA(ea_indly())); break;
        case 0xEF: SBC(rdA(ea_absl())); break;
        case 0xFF: SBC(rdA(ea_abslx())); break;
        case 0xE3: SBC(rdA(ea_sr(), true)); break;
        case 0xF3: SBC(rdA(ea_sriy())); break;
        // ── CMP ──
        case 0xC9: CMPr(a_, immA(), eM); break;
        case 0xC5: CMPr(a_, rdA(ea_dp(), true), eM); break;
        case 0xD5: CMPr(a_, rdA(ea_dpx(), true), eM); break;
        case 0xCD: CMPr(a_, rdA(ea_abs()), eM); break;
        case 0xDD: CMPr(a_, rdA(ea_absx()), eM); break;
        case 0xD9: CMPr(a_, rdA(ea_absy()), eM); break;
        case 0xC1: CMPr(a_, rdA(ea_indx()), eM); break;
        case 0xD1: CMPr(a_, rdA(ea_indy()), eM); break;
        case 0xD2: CMPr(a_, rdA(ea_ind()), eM); break;
        case 0xC7: CMPr(a_, rdA(ea_indl()), eM); break;
        case 0xD7: CMPr(a_, rdA(ea_indly()), eM); break;
        case 0xCF: CMPr(a_, rdA(ea_absl()), eM); break;
        case 0xDF: CMPr(a_, rdA(ea_abslx()), eM); break;
        case 0xC3: CMPr(a_, rdA(ea_sr(), true), eM); break;
        case 0xD3: CMPr(a_, rdA(ea_sriy()), eM); break;
        // ── CPX / CPY ──
        case 0xE0: CMPr(x_, immX(), eX); break;
        case 0xE4: CMPr(x_, rdX(ea_dp(), true), eX); break;
        case 0xEC: CMPr(x_, rdX(ea_abs()), eX); break;
        case 0xC0: CMPr(y_, immX(), eX); break;
        case 0xC4: CMPr(y_, rdX(ea_dp(), true), eX); break;
        case 0xCC: CMPr(y_, rdX(ea_abs()), eX); break;
        // ── LDA ──
        case 0xA9: { uint16_t v = immA(); if (eM) { a_ = (a_ & 0xFF00) | uint8_t(v); setZN8(uint8_t(v)); } else { a_ = v; setZN16(v);} } break;
        case 0xA5: { uint16_t v = rdA(ea_dp(), true);  if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB5: { uint16_t v = rdA(ea_dpx(), true); if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xAD: { uint16_t v = rdA(ea_abs()); if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xBD: { uint16_t v = rdA(ea_absx());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB9: { uint16_t v = rdA(ea_absy());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xA1: { uint16_t v = rdA(ea_indx());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB1: { uint16_t v = rdA(ea_indy());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB2: { uint16_t v = rdA(ea_ind()); if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xA7: { uint16_t v = rdA(ea_indl());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB7: { uint16_t v = rdA(ea_indly());if(eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xAF: { uint16_t v = rdA(ea_absl());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xBF: { uint16_t v = rdA(ea_abslx());if(eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xA3: { uint16_t v = rdA(ea_sr(), true);  if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        case 0xB3: { uint16_t v = rdA(ea_sriy());if (eM){a_=(a_&0xFF00)|uint8_t(v);setZN8(uint8_t(v));} else {a_=v;setZN16(v);} } break;
        // ── LDX ──
        case 0xA2: { uint16_t v = immX(); if (eX){x_=uint8_t(v);setZN8(uint8_t(v));} else {x_=v;setZN16(v);} } break;
        case 0xA6: { uint16_t v = rdX(ea_dp(), true);  if (eX){x_=uint8_t(v);setZN8(uint8_t(v));} else {x_=v;setZN16(v);} } break;
        case 0xB6: { uint16_t v = rdX(ea_dpy(), true); if (eX){x_=uint8_t(v);setZN8(uint8_t(v));} else {x_=v;setZN16(v);} } break;
        case 0xAE: { uint16_t v = rdX(ea_abs()); if (eX){x_=uint8_t(v);setZN8(uint8_t(v));} else {x_=v;setZN16(v);} } break;
        case 0xBE: { uint16_t v = rdX(ea_absy());if (eX){x_=uint8_t(v);setZN8(uint8_t(v));} else {x_=v;setZN16(v);} } break;
        // ── LDY ──
        case 0xA0: { uint16_t v = immX(); if (eX){y_=uint8_t(v);setZN8(uint8_t(v));} else {y_=v;setZN16(v);} } break;
        case 0xA4: { uint16_t v = rdX(ea_dp(), true);  if (eX){y_=uint8_t(v);setZN8(uint8_t(v));} else {y_=v;setZN16(v);} } break;
        case 0xB4: { uint16_t v = rdX(ea_dpx(), true); if (eX){y_=uint8_t(v);setZN8(uint8_t(v));} else {y_=v;setZN16(v);} } break;
        case 0xAC: { uint16_t v = rdX(ea_abs()); if (eX){y_=uint8_t(v);setZN8(uint8_t(v));} else {y_=v;setZN16(v);} } break;
        case 0xBC: { uint16_t v = rdX(ea_absx());if (eX){y_=uint8_t(v);setZN8(uint8_t(v));} else {y_=v;setZN16(v);} } break;
        // ── STA ──
        case 0x85: wrA(ea_dp(), a_, true); break;
        case 0x95: wrA(ea_dpx(), a_, true); break;
        case 0x8D: wrA(ea_abs(),  a_); break;
        case 0x9D: wrA(ea_absx(true), a_); break;   // write always pays the index cycle
        case 0x99: wrA(ea_absy(true), a_); break;
        case 0x81: wrA(ea_indx(), a_); break;
        case 0x91: wrA(ea_indy(true), a_); break;
        case 0x92: wrA(ea_ind(),  a_); break;
        case 0x87: wrA(ea_indl(), a_); break;
        case 0x97: wrA(ea_indly(),a_); break;
        case 0x8F: wrA(ea_absl(), a_); break;
        case 0x9F: wrA(ea_abslx(),a_); break;
        case 0x83: wrA(ea_sr(), a_, true); break;
        case 0x93: wrA(ea_sriy(), a_); break;
        // ── STX / STY / STZ ──
        case 0x86: wrX(ea_dp(), x_, true); break;
        case 0x96: wrX(ea_dpy(), x_, true); break;
        case 0x8E: wrX(ea_abs(), x_); break;
        case 0x84: wrX(ea_dp(), y_, true); break;
        case 0x94: wrX(ea_dpx(), y_, true); break;
        case 0x8C: wrX(ea_abs(), y_); break;
        case 0x64: wrA(ea_dp(), 0, true); break;
        case 0x74: wrA(ea_dpx(), 0, true); break;
        case 0x9C: wrA(ea_abs(),  0); break;
        case 0x9E: wrA(ea_absx(true), 0); break;
        // ── INC / DEC (memory + A) ──
        case 0x1A: if (eM){uint8_t r=uint8_t(a_)+1;a_=(a_&0xFF00)|r;setZN8(r);}else{a_=uint16_t(a_+1);setZN16(a_);} break; // INC A
        case 0x3A: if (eM){uint8_t r=uint8_t(a_)-1;a_=(a_&0xFF00)|r;setZN8(r);}else{a_=uint16_t(a_-1);setZN16(a_);} break; // DEC A
        case 0xE6: { uint16_t v = RMW(ea_dp(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x+1)) : uint16_t(x+1); }, true); setZNa(v); } break;
        case 0xF6: { uint16_t v = RMW(ea_dpx(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x+1)) : uint16_t(x+1); }, true); setZNa(v); } break;
        case 0xEE: { uint16_t v = RMW(ea_abs(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x+1)) : uint16_t(x+1); }); setZNa(v); } break;
        case 0xFE: { uint16_t v = RMW(ea_absx(true), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x+1)) : uint16_t(x+1); }); setZNa(v); } break;
        case 0xC6: { uint16_t v = RMW(ea_dp(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x-1)) : uint16_t(x-1); }, true); setZNa(v); } break;
        case 0xD6: { uint16_t v = RMW(ea_dpx(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x-1)) : uint16_t(x-1); }, true); setZNa(v); } break;
        case 0xCE: { uint16_t v = RMW(ea_abs(), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x-1)) : uint16_t(x-1); }); setZNa(v); } break;
        case 0xDE: { uint16_t v = RMW(ea_absx(true), [&](uint16_t x){ return eM ? uint16_t(uint8_t(x-1)) : uint16_t(x-1); }); setZNa(v); } break;
        // ── INX/INY/DEX/DEY ──
        case 0xE8: if (eX){x_=uint8_t(x_+1);setZN8(uint8_t(x_));}else{x_=uint16_t(x_+1);setZN16(x_);} break;
        case 0xC8: if (eX){y_=uint8_t(y_+1);setZN8(uint8_t(y_));}else{y_=uint16_t(y_+1);setZN16(y_);} break;
        case 0xCA: if (eX){x_=uint8_t(x_-1);setZN8(uint8_t(x_));}else{x_=uint16_t(x_-1);setZN16(x_);} break;
        case 0x88: if (eX){y_=uint8_t(y_-1);setZN8(uint8_t(y_));}else{y_=uint16_t(y_-1);setZN16(y_);} break;
        // ── ASL/LSR/ROL/ROR (A + memory) ──
        case 0x0A: if(eM){uint8_t v=uint8_t(a_);p_=(p_&~Status::C)|((v&0x80)?Status::C:0);v<<=1;a_=(a_&0xFF00)|v;setZN8(v);}else{p_=(p_&~Status::C)|((a_&0x8000)?Status::C:0);a_=uint16_t(a_<<1);setZN16(a_);} break;
        case 0x4A: if(eM){uint8_t v=uint8_t(a_);p_=(p_&~Status::C)|((v&1)?Status::C:0);v>>=1;a_=(a_&0xFF00)|v;setZN8(v);}else{p_=(p_&~Status::C)|((a_&1)?Status::C:0);a_=uint16_t(a_>>1);setZN16(a_);} break;
        case 0x2A: if(eM){uint8_t v=uint8_t(a_);uint8_t c=bit(p_,Status::C)?1:0;p_=(p_&~Status::C)|((v&0x80)?Status::C:0);v=uint8_t((v<<1)|c);a_=(a_&0xFF00)|v;setZN8(v);}else{uint16_t c=bit(p_,Status::C)?1:0;p_=(p_&~Status::C)|((a_&0x8000)?Status::C:0);a_=uint16_t((a_<<1)|c);setZN16(a_);} break;
        case 0x6A: if(eM){uint8_t v=uint8_t(a_);uint8_t c=bit(p_,Status::C)?0x80:0;p_=(p_&~Status::C)|((v&1)?Status::C:0);v=uint8_t((v>>1)|c);a_=(a_&0xFF00)|v;setZN8(v);}else{uint16_t c=bit(p_,Status::C)?0x8000:0;p_=(p_&~Status::C)|((a_&1)?Status::C:0);a_=uint16_t((a_>>1)|c);setZN16(a_);} break;
        // memory shift/rotate helper via macro-like lambdas
        case 0x06: case 0x16: case 0x0E: case 0x1E:  // ASL mem
        case 0x46: case 0x56: case 0x4E: case 0x5E:  // LSR mem
        case 0x26: case 0x36: case 0x2E: case 0x3E:  // ROL mem
        case 0x66: case 0x76: case 0x6E: case 0x7E: { // ROR mem
            uint32_t a; bool dp = false; switch (opc & 0x1F) {
                case 0x06: case 0x16: a = (opc & 0x10) ? ea_dpx() : ea_dp(); dp = true; break; // 0x_6 dp / 0x_6|10 dpx
                default:    a = (opc & 0x10) ? ea_absx(true) : ea_abs(); break;   // RMW abs,X always pays the index cycle
            }
            const bool rot = (opc & 0x20);
            const bool right = (opc & 0x40);
            uint16_t cIn = bit(p_, Status::C) ? 1 : 0;
            uint16_t msb = eM ? 0x80 : 0x8000;
            uint16_t v = RMW(a, [&](uint16_t x) {
                if (!right) { p_ = (p_ & ~Status::C) | ((x & msb) ? Status::C : 0); x = uint16_t(x << 1) | (rot ? cIn : 0); }
                else        { p_ = (p_ & ~Status::C) | ((x & 1) ? Status::C : 0);  x = uint16_t(x >> 1) | (rot ? (cIn ? msb : 0) : 0); }
                return eM ? uint16_t(uint8_t(x)) : x;
            }, dp);
            setZNa(v);
        } break;
        // ── BIT ──
        case 0x89: { uint16_t v = immA(); uint16_t r = (eM?uint8_t(a_):a_) & v; p_ = (p_ & ~Status::Z) | (( (eM?uint8_t(r):r)==0)?Status::Z:0); } break; // BIT #imm: only Z
        case 0x24: case 0x34: case 0x2C: case 0x3C: {
            uint32_t a; switch (opc) { case 0x24:a=ea_dp();break; case 0x34:a=ea_dpx();break; case 0x2C:a=ea_abs();break; default:a=ea_absx();break; }
            uint16_t v = rdA(a); uint16_t msb = eM?0x80:0x8000; uint16_t nb2 = eM?0x40:0x4000;
            uint16_t r = (eM?uint8_t(a_):a_) & v;
            p_ = (p_ & ~(Status::Z|Status::N|Status::V));
            if ((eM?uint8_t(r):r) == 0) p_ |= Status::Z;
            if (v & msb) p_ |= Status::N;
            if (v & nb2) p_ |= Status::V;
        } break;
        // ── TSB / TRB ──
        case 0x04: case 0x0C: case 0x14: case 0x1C: {
            const bool dp = (opc & 0x08) == 0;                       // $04/$14 dp, $0C/$1C abs
            uint32_t a = dp ? ea_dp() : ea_abs();
            uint16_t av = eM?uint8_t(a_):a_;
            RMW(a, [&](uint16_t v) {
                p_ = (p_ & ~Status::Z) | (((v & av)== 0)?Status::Z:0);
                return (opc & 0x10) ? uint16_t(v & ~av) : uint16_t(v | av);
            }, dp);
        } break;
        // ── transfers ──
        case 0xAA: if(eX){x_=uint8_t(a_);setZN8(uint8_t(x_));}else{x_=a_;setZN16(x_);} break; // TAX
        case 0xA8: if(eX){y_=uint8_t(a_);setZN8(uint8_t(y_));}else{y_=a_;setZN16(y_);} break; // TAY
        case 0x8A: if(eM){a_=(a_&0xFF00)|uint8_t(x_);setZN8(uint8_t(a_));}else{a_=x_;setZN16(a_);} break; // TXA
        case 0x98: if(eM){a_=(a_&0xFF00)|uint8_t(y_);setZN8(uint8_t(a_));}else{a_=y_;setZN16(a_);} break; // TYA
        case 0x9A: sp_ = emulation_ ? uint16_t(0x0100 | (x_ & 0xFF)) : x_; break; // TXS (no flags)
        case 0xBA: if(eX){x_=uint8_t(sp_);setZN8(uint8_t(x_));}else{x_=sp_;setZN16(x_);} break; // TSX
        case 0x9B: if(eX){y_=uint8_t(x_);setZN8(uint8_t(y_));}else{y_=x_;setZN16(y_);} break; // TXY (X->Y)
        case 0xBB: if(eX){x_=uint8_t(y_);setZN8(uint8_t(x_));}else{x_=y_;setZN16(x_);} break; // TYX (Y->X)
        case 0x5B: d_ = a_; setZN16(d_); break;                 // TCD
        case 0x7B: a_ = d_; setZN16(a_); break;                 // TDC
        case 0x1B: sp_ = emulation_ ? uint16_t(0x0100 | (a_ & 0xFF)) : a_; break; // TCS (no flags)
        case 0x3B: a_ = sp_; setZN16(a_); break;                // TSC
        // ── stack ──
        case 0x48: if(eM) pushB(uint8_t(a_)); else pushW(a_); break; // PHA
        case 0x68: if(eM){uint8_t v=pullB();a_=(a_&0xFF00)|v;setZN8(v);}else{a_=pullW();setZN16(a_);} break; // PLA
        case 0xDA: if(eX) pushB(uint8_t(x_)); else pushW(x_); break; // PHX
        case 0xFA: if(eX){uint8_t v=pullB();x_=v;setZN8(v);}else{x_=pullW();setZN16(x_);} break; // PLX
        case 0x5A: if(eX) pushB(uint8_t(y_)); else pushW(y_); break; // PHY
        case 0x7A: if(eX){uint8_t v=pullB();y_=v;setZN8(v);}else{y_=pullW();setZN16(y_);} break; // PLY
        case 0x08: pushB(uint8_t(p_ | (emulation_ ? 0x30 : 0))); break; // PHP
        case 0x28: { uint8_t v = pullB(); if (emulation_) p_ = v | Status::M | Status::X; else { p_ = v; if (bit(p_,Status::X)) { x_ &= 0xFF; y_ &= 0xFF; } } } break; // PLP
        // PHB / PLB / PHK are 65816-only stack ops, so they use the RAW 16-bit
        // stack like PEA/PEI/PER/PHD/PLD/JSL/RTL — not the 6502 page-1 wrap.
        // Only the PULL shows it: from S=$01FF a wrapping pull reads $0100, a raw
        // one reads $0200 (Tom Harte ab.e vectors 75/421/707/… — 36/10000, i.e.
        // exactly the 1-in-256 vectors that land on the page boundary; final S is
        // $0100 either way because SPH is forced back at end of instruction).
        // The single-byte pushes are indistinguishable between the two models,
        // and are written raw only so the three stay consistent.
        // (bug-hunt finding, August 2026 — pre-existing, found while
        // re-validating the CPU after the IRQ/WAI fixes.)
        case 0x8B: pushBraw(dbr_); break;                          // PHB
        case 0xAB: { uint8_t v = pullBraw(); dbr_ = v; setZN8(v);} break; // PLB
        case 0x0B: pushWraw(d_); break;                         // PHD
        case 0x2B: { d_ = pullWraw(); setZN16(d_);} break;      // PLD
        case 0x4B: pushBraw(pbr_); break;                       // PHK (see PHB/PLB above)
        case 0xF4: { uint16_t v = fetch16(); pushWraw(v);} break;  // PEA
        case 0xD4: { uint8_t o = fetch(); if ((d_ & 0xFF) != 0) io();   // PEI: DP pointer read is full 16-bit (no page-0 wrap)
            uint16_t a0 = uint16_t(d_ + o); pushWraw(uint16_t(rd(a0) | (rd(uint16_t(a0 + 1)) << 8))); } break;
        case 0x62: { int16_t off = int16_t(fetch16()); io(); pushWraw(uint16_t(pc_ + off)); } break; // PER
        // ── branches ──
        case 0xD0: branch(!bit(p_,Status::Z)); break; // BNE
        case 0xF0: branch(bit(p_,Status::Z));  break; // BEQ
        case 0x90: branch(!bit(p_,Status::C)); break; // BCC
        case 0xB0: branch(bit(p_,Status::C));  break; // BCS
        case 0x10: branch(!bit(p_,Status::N)); break; // BPL
        case 0x30: branch(bit(p_,Status::N));  break; // BMI
        case 0x50: branch(!bit(p_,Status::V)); break; // BVC
        case 0x70: branch(bit(p_,Status::V));  break; // BVS
        case 0x80: branch(true); break;               // BRA
        case 0x82: { int16_t off = int16_t(fetch16()); io(); pc_ = uint16_t(pc_ + off);} break; // BRL
        // ── jumps ──
        case 0x4C: pc_ = fetch16(); break;                                   // JMP abs
        case 0x6C: { uint16_t p0 = fetch16(); pc_ = uint16_t(rd(p0) | (rd(uint16_t(p0+1))<<8)); } break; // JMP (abs)
        case 0x7C: { uint16_t p0 = fetch16(); io(); uint16_t ptr = uint16_t(p0 + (eX?(x_&0xFF):x_)); pc_ = uint16_t(rd((uint32_t(pbr_)<<16)|ptr) | (rd((uint32_t(pbr_)<<16)|uint16_t(ptr+1))<<8)); } break; // JMP (abs,X)
        case 0x5C: { uint16_t a = fetch16(); uint8_t b = fetch(); pc_ = a; pbr_ = b; } break; // JML long
        case 0xDC: { uint16_t p0 = fetch16(); uint8_t lo=rd(p0),mid=rd(uint16_t(p0+1)),hi=rd(uint16_t(p0+2)); pc_=uint16_t(lo|(mid<<8)); pbr_=hi; } break; // JML [abs]
        case 0x20: { uint16_t a = fetch16(); io(); pushW(uint16_t(pc_ - 1)); pc_ = a; } break; // JSR abs
        // JSR (abs,X): the return address (= the AAH byte's address) is pushed
        // between the two operand fetches — opcode, AAL, PCH, PCL, AAH, IO,
        // new PCL, new PCH (W65C816S Table 5-7; SingleStepTests corpus). It is
        // one of the NEW 65816 instructions, so the push uses the raw 16-bit
        // stack even in emulation mode (no page-1 wrap: 6502.org 65C816 opcodes
        // appendix, bsnes, gilyon cputest 0277; SingleStepTests issue #6 — the
        // corpus wraps, carried as an xfail by the harness).
        case 0xFC: { uint8_t lo = fetch(); pushWraw(pc_); uint8_t hi = fetch(); io();
            uint16_t ptr=uint16_t(uint16_t(lo | (hi << 8))+(eX?(x_&0xFF):x_));
            pc_=uint16_t(rd((uint32_t(pbr_)<<16)|ptr)|(rd((uint32_t(pbr_)<<16)|uint16_t(ptr+1))<<8)); } break; // JSR (abs,X)
        case 0x22: { uint16_t a = fetch16(); pushBraw(pbr_); io(); uint8_t b = fetch(); pushWraw(uint16_t(pc_ - 1)); pc_ = a; pbr_ = b; } break; // JSL long: PBR is pushed between AAH and AAB
        case 0x60: pc_ = uint16_t(pullW() + 1); io(); break;                 // RTS: third internal cycle after the pull
        case 0x6B: { uint16_t a = pullWraw(); uint8_t b = pullBraw(); pc_ = uint16_t(a + 1); pbr_ = b; } break; // RTL
        // RTI: the pulled P takes effect after the last pull — the PC/PBR pull
        // cycles still show the old M/X on the pins (SingleStepTests corpus).
        case 0x40: { uint8_t np = pullB(); pc_ = pullW(); if (!emulation_) pbr_ = pullB();
            if (emulation_) p_ = np | Status::M | Status::X; else { p_ = np; if (bit(p_,Status::X)){x_&=0xFF;y_&=0xFF;} } } break; // RTI
        // ── flag ops ──
        case 0x18: p_ &= ~Status::C; break; // CLC
        case 0x38: p_ |=  Status::C; break; // SEC
        case 0x58: p_ &= ~Status::I; break; // CLI
        case 0x78: p_ |=  Status::I; break; // SEI
        case 0xB8: p_ &= ~Status::V; break; // CLV
        case 0xD8: p_ &= ~Status::D; break; // CLD
        case 0xF8: p_ |=  Status::D; break; // SED
        case 0xC2: { uint8_t mimm = fetch(); io(); p_ &= ~mimm; if (emulation_) p_ |= Status::M | Status::X; } break; // REP
        case 0xE2: { uint8_t mimm = fetch(); io(); p_ |= mimm; if (bit(p_,Status::X)){x_&=0xFF;y_&=0xFF;} } break;     // SEP
        case 0xFB: { bool c = bit(p_,Status::C); bool e = emulation_; p_ = (p_ & ~Status::C) | (e?Status::C:0); emulation_ = c; if (emulation_){ p_ |= Status::M|Status::X; x_&=0xFF; y_&=0xFF; sp_=uint16_t(0x0100|(sp_&0xFF)); } } break; // XCE
        // ── misc ──
        case 0xEB: { uint8_t lo = uint8_t(a_), hi = uint8_t(a_ >> 8); a_ = uint16_t((lo << 8) | hi); setZN8(uint8_t(a_)); } break; // XBA
        case 0xEA: break;                                        // NOP
        // WDM: the signature byte is consumed by an internal cycle (VPA low —
        // the chip does not treat it as an operand fetch), so it is peeked for
        // the SmartPort HLE trap ($C5/$C6) without a bus transaction.
        case 0x42: { const uint32_t a = (uint32_t(pbr_) << 16) | pc_; ioAt(a); pc_ = uint16_t(pc_ + 1);
            if (memory_) memory_->smartportTrap(memory_->peek8(a)); } break;
        // Block moves. Object code is `54/44 destbank srcbank`. The counter is
        // the FULL 16-bit accumulator regardless of the M flag; X/Y are used at
        // the index width. One byte per execution; PC re-points at the opcode
        // until the counter wraps past $0000 (i.e. becomes $FFFF). 7 cycles.
        case 0x54: { uint8_t db = fetch(); uint8_t sb = fetch(); // MVN (X/Y ++)
            uint8_t v = rd((uint32_t(sb) << 16) | (eX ? (x_ & 0xFF) : x_));
            wr((uint32_t(db) << 16) | (eX ? (y_ & 0xFF) : y_), v);
            dbr_ = db;
            x_ = eX ? uint8_t(x_ + 1) : uint16_t(x_ + 1);
            y_ = eX ? uint8_t(y_ + 1) : uint16_t(y_ + 1);
            a_ = uint16_t(a_ - 1);
            if (a_ != 0xFFFF) pc_ = uint16_t(pc_ - 3);
            io(2); } break;
        case 0x44: { uint8_t db = fetch(); uint8_t sb = fetch(); // MVP (X/Y --)
            uint8_t v = rd((uint32_t(sb) << 16) | (eX ? (x_ & 0xFF) : x_));
            wr((uint32_t(db) << 16) | (eX ? (y_ & 0xFF) : y_), v);
            dbr_ = db;
            x_ = eX ? uint8_t(x_ - 1) : uint16_t(x_ - 1);
            y_ = eX ? uint8_t(y_ - 1) : uint16_t(y_ - 1);
            a_ = uint16_t(a_ - 1);
            if (a_ != 0xFFFF) pc_ = uint16_t(pc_ - 3);
            io(2); } break;
        // ── interrupts ──
        case 0x00: { fetch(); /* signature byte */
            if (!emulation_) pushB(pbr_);
            pushW(pc_);
            pushB(uint8_t(p_ | (emulation_ ? 0x30 : 0x00)));   // native: push P as-is (no B)
            p_ |= Status::I; p_ &= ~Status::D;
            uint16_t vec = emulation_ ? 0xFFFE : 0xFFE6; pbr_ = 0;
            pc_ = uint16_t(rdVector(vec, !emulation_) |
                           (rdVector(uint16_t(vec+1), !emulation_)<<8)); } break; // BRK
        case 0x02: { fetch();
            if (!emulation_) pushB(pbr_);
            pushW(pc_);
            pushB(uint8_t(p_ | (emulation_ ? 0x30 : 0)));
            p_ |= Status::I; p_ &= ~Status::D;
            uint16_t vec = emulation_ ? 0xFFF4 : 0xFFE4; pbr_ = 0;
            // COP keeps //e language-card semantics in emulation mode: GS/OS
            // *does* install a RAM COP handler at $00/FFF4 (→ $FC85) and dispatches
            // its emulation-mode COP calls through it, unlike the IRQ/BRK vector
            // ($00/FFFE) which it leaves null and expects to reach ROM. Reading
            // ROM here would send GS/OS's COP calls to the ROM monitor and hang.
            pc_ = uint16_t(rdVector(vec, !emulation_) |
                           (rdVector(uint16_t(vec+1), !emulation_)<<8)); } break; // COP
        // WAI: 3 cycles (4 incl. the fetch — Tom Harte) and then the chip stalls
        // until an interrupt line goes active; the wait itself is served at the
        // top of step(). It used to be a plain NOP, so a `WAI`-based beam/DOC
        // sync spun through its whole loop at full speed instead of parking.
        case 0xCB: /* WAI */ waiting_ = true; haltCycle(); break;        // 2 internal cycles via leadIo + halt
        case 0xDB: /* STP */ running_ = false; haltCycle(); break;       // 2 internal cycles via leadIo + halt, then stop

        default: break;   // remaining opcodes land here until implemented
    }

    // The raw 16-bit stack ops may have left SP out of page 1 in emulation
    // mode; the hardware resets SPH to $01 at the end of the instruction.
    if (emulation_) sp_ = uint16_t(0x0100 | (sp_ & 0xFF));
}
