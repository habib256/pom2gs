// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Zilog 8530 SCC (Serial Communications Controller) ────────────────────
// The IIgs two-port serial chip (printer + modem), at $C038-$C03B:
//   $C038 = channel B command   $C039 = channel A command
//   $C03A = channel B data      $C03B = channel A data
// Each channel has 16 write (WR0-15) + read (RR0-15) registers reached through
// a register pointer: writing WR0's low bits sets the pointer for the next
// access, which then auto-resets to 0. Local-loopback (WR14 bit4) echoes TX to
// RX. M7 models the register protocol, TX/RX FIFOs, and RR0 status.
// Source of truth: MAME machine/z80scc.cpp / scc8530.cpp; Zilog 8530 manual.

#ifndef POMIIGS_SCC8530_H
#define POMIIGS_SCC8530_H

#include <cstdint>
#include <deque>

class Scc8530
{
public:
    Scc8530() { reset(); }
    void reset();

    // reg = low nibble of $C03x: 8=Bcmd, 9=Acmd, A=Bdata, B=Adata.
    uint8_t read(uint8_t reg);
    void    write(uint8_t reg, uint8_t v);

    // Test/host hooks: inject a received byte / drain a transmitted byte.
    void hostRx(int ch, uint8_t b) { chan_[ch & 1].rx.push_back(b); }
    bool hostTx(int ch, uint8_t& b);

private:
    struct Channel {
        uint8_t wr[16] = {0};
        uint8_t regPtr = 0;
        std::deque<uint8_t> rx;   // toward the CPU
        std::deque<uint8_t> tx;   // from the CPU toward the host
    };
    Channel chan_[2];             // [0]=B, [1]=A

    uint8_t rr0(const Channel& c) const;
    uint8_t cmdRead(int ch);
    void    cmdWrite(int ch, uint8_t v);
    uint8_t dataRead(int ch);
    void    dataWrite(int ch, uint8_t v);
};

#endif // POMIIGS_SCC8530_H
