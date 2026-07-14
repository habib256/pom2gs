// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Ensoniq 5503 DOC. See Es5503.h. Source of truth: MAME sound/es5503.cpp.

#include "Es5503.h"

void Es5503::reset() {
    sndRam_.fill(0);
    reg_.fill(0);
    acc_.fill(0);
    ctl_ = 0; addr_ = 0;
    reg_[0xE1] = 0;                       // 1 oscillator enabled by default
    for (int o = 0; o < 32; ++o) reg_[0xA0 + o] = 0x01;   // all halted
}

uint8_t Es5503::gluRead(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0xC: return ctl_;
        case 0xD: {                                  // SOUNDDATA
            uint8_t v = (ctl_ & 0x20) ? sndRam_[addr_] : reg_[addr_ & 0xFF];
            if (ctl_ & 0x40) ++addr_;                // auto-increment
            return v;
        }
        case 0xE: return uint8_t(addr_ & 0xFF);
        case 0xF: return uint8_t(addr_ >> 8);
    }
    return 0;
}

void Es5503::gluWrite(uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
        case 0xC: ctl_ = v; break;
        case 0xD:                                    // SOUNDDATA
            if (ctl_ & 0x20) sndRam_[addr_] = v;
            else {
                reg_[addr_ & 0xFF] = v;
                // Writing a control register with halt cleared restarts the osc.
                if ((addr_ & 0xFF) >= 0xA0 && (addr_ & 0xFF) <= 0xBF && !(v & 0x01))
                    acc_[(addr_ & 0xFF) - 0xA0] = 0;
            }
            if (ctl_ & 0x40) ++addr_;
            break;
        case 0xE: addr_ = uint16_t((addr_ & 0xFF00) | v); break;
        case 0xF: addr_ = uint16_t((addr_ & 0x00FF) | (v << 8)); break;
    }
}

bool Es5503::anyActive() const {
    int n = oscEnabled();
    for (int o = 0; o < n && o < 32; ++o)
        if (!(ctrl(o) & 0x01)) return true;          // halt bit clear = running
    return false;
}

void Es5503::render(int16_t* out, int n) {
    const int nosc = oscEnabled();
    for (int s = 0; s < n; ++s) {
        int32_t mix = 0;
        for (int o = 0; o < nosc && o < 32; ++o) {
            const uint8_t c = ctrl(o);
            if (c & 0x01) continue;                  // halted
            // Wavetable size: bits 3-5 of $C0+o select 256..32768-byte tables.
            const uint8_t sizeSel = (wts(o) >> 3) & 0x07;
            const uint8_t resSel  = wts(o) & 0x07;
            const uint32_t tableBits = 8 + sizeSel;  // 256 << sizeSel entries
            const uint32_t accShift = 9 - resSel;    // fractional resolution
            acc_[o] += freq(o);
            uint32_t idx = acc_[o] >> accShift;
            const uint32_t tableMask = (1u << tableBits) - 1;
            if (idx > tableMask) {                    // reached end of wave
                const uint8_t mode = (c >> 1) & 0x03;
                if (mode == 0) { acc_[o] &= (tableMask << accShift) | ((1u << accShift) - 1); idx &= tableMask; } // free-run: loop
                else { reg_[0xA0 + o] |= 0x01; continue; }   // one-shot/sync/swap: halt (approx)
            }
            uint32_t base = uint32_t(wtp(o)) << 8;
            uint8_t sample = sndRam_[(base + (idx & tableMask)) & 0xFFFF];
            if (sample == 0) { reg_[0xA0 + o] |= 0x01; continue; }  // zero = end-of-wave marker
            mix += int32_t(int(sample) - 0x80) * vol(o);
        }
        // Scale: each osc contributes (±128 * 255); clamp the mix.
        int32_t v = mix >> 4;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[s] = int16_t(v);
    }
}
