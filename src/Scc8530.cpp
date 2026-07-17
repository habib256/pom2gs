// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Zilog 8530 SCC. See Scc8530.h. Source of truth: MAME z80scc / scc8530.

#include "Scc8530.h"

#include <istream>
#include <ostream>

void Scc8530::reset() {
    for (auto& c : chan_) { for (auto& r : c.wr) r = 0; c.regPtr = 0; c.rx.clear(); c.tx.clear(); }
}

// RR0 status: bit0 = Rx char available, bit2 = Tx buffer empty (always, since
// we transmit instantly), bit5 = CTS, bit3 = DCD. Minimal set.
uint8_t Scc8530::rr0(const Channel& c) const {
    uint8_t s = 0x04;                      // Tx buffer empty
    if (!c.rx.empty()) s |= 0x01;          // Rx char available
    return s;
}

uint8_t Scc8530::cmdRead(int ch) {
    Channel& c = chan_[ch];
    uint8_t p = c.regPtr;
    c.regPtr = 0;
    if (p == 0) return rr0(c);
    if (p == 1) return 0x01;               // RR1: all sent
    // Other RRn mostly mirror WRn defaults for our purposes.
    return c.wr[p & 0x0F];
}

void Scc8530::cmdWrite(int ch, uint8_t v) {
    Channel& c = chan_[ch];
    if (c.regPtr == 0) {
        // Low 3 bits = register 0-7; the "point high" command (bits 3-5 = 001)
        // adds 8, so v & 0x0F resolves both to the 0-15 register number.
        c.regPtr = v & 0x0F;
        return;
    }
    c.wr[c.regPtr] = v;
    c.regPtr = 0;
}

uint8_t Scc8530::dataRead(int ch) {
    Channel& c = chan_[ch];
    if (c.rx.empty()) return 0;
    uint8_t b = c.rx.front(); c.rx.pop_front();
    return b;
}

void Scc8530::dataWrite(int ch, uint8_t v) {
    Channel& c = chan_[ch];
    c.tx.push_back(v);
    if (c.wr[14] & 0x10) c.rx.push_back(v);   // WR14 bit4 = local loopback → echo to Rx
}

bool Scc8530::hostTx(int ch, uint8_t& b) {
    Channel& c = chan_[ch & 1];
    if (c.tx.empty()) return false;
    b = c.tx.front(); c.tx.pop_front();
    return true;
}

uint8_t Scc8530::read(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0x8: return cmdRead(0);   // B command
        case 0x9: return cmdRead(1);   // A command
        case 0xA: return dataRead(0);  // B data
        case 0xB: return dataRead(1);  // A data
    }
    return 0;
}

void Scc8530::write(uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
        case 0x8: cmdWrite(0, v); break;
        case 0x9: cmdWrite(1, v); break;
        case 0xA: dataWrite(0, v); break;
        case 0xB: dataWrite(1, v); break;
    }
}

// ── Snapshot ─────────────────────────────────────────────────────────────
// Both channels' WR file (WR0-15) + register pointer + rx/tx FIFOs, each FIFO
// length-prefixed. Mirrors every other C0xx device so a restore round-trips
// mid-transfer serial state instead of retaining the live object's contents.
namespace {
void putQueue(std::ostream& os, const std::deque<uint8_t>& q) {
    uint32_t n = uint32_t(q.size()); os.write((const char*)&n, sizeof n);
    for (uint8_t b : q) os.write((const char*)&b, 1);
}
void getQueue(std::istream& is, std::deque<uint8_t>& q) {
    q.clear(); uint32_t n = 0; is.read((char*)&n, sizeof n);
    if (n > 4096) n = 0;                       // guard against corrupt streams
    for (uint32_t i = 0; i < n; ++i) { uint8_t b = 0; is.read((char*)&b, 1); q.push_back(b); }
}
}

void Scc8530::saveState(std::ostream& os) const {
    for (const Channel& c : chan_) {
        os.write((const char*)c.wr, sizeof c.wr);
        os.write((const char*)&c.regPtr, sizeof c.regPtr);
        putQueue(os, c.rx);
        putQueue(os, c.tx);
    }
}

void Scc8530::loadState(std::istream& is) {
    for (Channel& c : chan_) {
        is.read((char*)c.wr, sizeof c.wr);
        is.read((char*)&c.regPtr, sizeof c.regPtr);
        getQueue(is, c.rx);
        getQueue(is, c.tx);
    }
}
