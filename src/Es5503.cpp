// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Ensoniq 5503 DOC. See Es5503.h. Source of truth: MAME sound/es5503.cpp
// (register map, halt_osc mode semantics incl. the swap-partner start and the
// even-oscillator retrigger quirk "verified on IIgs hardware", the $E0
// interrupt-status protocol, and the wavesize/accmask/resshift tables).

#include "Es5503.h"
#include <istream>
#include <ostream>

// MAME es5503.cpp constant tables. `resshift = resshifts[res] - wavetblsize`
// scales the 24-bit phase accumulator to a table index; `accmasks` bounds the
// index; `wavemasks` aligns the wavetable pointer to the table size (the chip
// addresses 128 KB — the IIgs wires 64 KB, so RAM reads mask to $FFFF).
static constexpr uint16_t kWaveSizes[8] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
static constexpr uint32_t kWaveMasks[8] = { 0x1ff00, 0x1fe00, 0x1fc00, 0x1f800,
                                            0x1f000, 0x1e000, 0x1c000, 0x18000 };
static constexpr uint32_t kAccMasks[8]  = { 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff, 0x3fff, 0x7fff };
static constexpr int      kResShifts[8] = { 9, 10, 11, 12, 13, 14, 15, 16 };

void Es5503::reset() {
    sndRam_.fill(0);
    reg_.fill(0);
    acc_.fill(0);
    lastData_.fill(0x80);
    ctl_ = 0; addr_ = 0; readLatch_ = 0;
    irqPend_ = 0; rege0_ = 0xFF;
    frac_ = 0.0; cur_ = 0;
    ring_.fill(0); rHead_ = rTail_ = 0; masterAcc_ = 0; drainFrac_ = 0.0; drainHold_ = 0; drainPrev_ = 0;
    reg_[0xE1] = 0;                       // 1 oscillator enabled by default
    for (int o = 0; o < 32; ++o) reg_[0xA0 + o] = 0x01;   // all halted
}

// ── Sound GLU ────────────────────────────────────────────────────────────
// $C03C SOUNDCTL bits (MAME apple2gs.cpp $C03D handlers — these were WRONG here
// once: bit5/bit6 swapped, which routed every DOC *register* write into sound
// RAM, so no oscillator was ever keyed on and the DOC stayed silent):
//   bit7 = DOC busy (reads 0), bit6 ($40) = RAM(1)/registers(0),
//   bit5 ($20) = address auto-increment (reads AND writes), bits0-3 volume.
// $C03D reads go through a one-deep LATCH (MAME m_sndglu_dummy_read): the read
// returns the PREVIOUS fetch and then latches the currently-addressed byte —
// software does a dummy read first (the $E0 IRQ-ack ISRs depend on this).
uint8_t Es5503::gluRead(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0xC: return uint8_t(ctl_ & 0x7F);       // bit7 busy = 0
        case 0xD: {                                  // SOUNDDATA (latched)
            const uint8_t v = readLatch_;
            readLatch_ = (ctl_ & 0x40) ? sndRam_[addr_] : regRead(uint8_t(addr_ & 0xFF));
            if (ctl_ & 0x20) ++addr_;                // auto-increment
            return v;
        }
        case 0xE: return uint8_t(addr_ & 0xFF);
        case 0xF: return uint8_t(addr_ >> 8);
    }
    return 0;
}

void Es5503::gluWrite(uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
        case 0xC: ctl_ = uint8_t(v & 0x7F); break;
        case 0xD:                                    // SOUNDDATA
            if (ctl_ & 0x40) sndRam_[addr_] = v;
            else regWrite(uint8_t(addr_ & 0xFF), v);
            if (ctl_ & 0x20) ++addr_;
            break;
        case 0xE: addr_ = uint16_t((addr_ & 0xFF00) | v); break;
        case 0xF: addr_ = uint16_t((addr_ & 0x00FF) | (v << 8)); break;
    }
}

// ── DOC registers ────────────────────────────────────────────────────────
uint8_t Es5503::regRead(uint8_t r) {
    if (r == 0xE0) {
        // Interrupt status (MAME es5503.cpp case 0xe0): return the LOWEST pending
        // oscillator (number in bits 5-1, bit 7 clear = valid), clear its pend
        // flag; the line stays asserted while others are pending. With none
        // pending, the stale value (bit 7 set) reports "no interrupt".
        uint8_t v = rege0_;
        const int n = oscEnabled();
        for (int i = 0; i < n; ++i) {
            if (irqPend_ & (1u << i)) {
                v = uint8_t(i << 1);
                rege0_ = uint8_t(v | 0x80);
                irqPend_ &= ~(1u << i);
                break;
            }
        }
        return uint8_t(v | 0x41);
    }
    if (r >= 0x60 && r <= 0x7F) return lastData_[r - 0x60];   // data reg: last byte fetched
    return reg_[r];
}

void Es5503::regWrite(uint8_t r, uint8_t v) {
    if (r >= 0xA0 && r <= 0xBF) {
        // Control: a fresh key-on (halt 1 → 0) restarts the phase accumulator
        // (MAME case 0xa0). Everything else just stores.
        const int o = r - 0xA0;
        if ((reg_[r] & 0x01) && !(v & 0x01)) acc_[o] = 0;
        reg_[r] = v;
        return;
    }
    reg_[r] = v;   // $E1 (osc count) takes effect via docRate in render()
}

// ── Snapshot ─────────────────────────────────────────────────────────────
void Es5503::saveState(std::ostream& os) const {
    os.write((const char*)sndRam_.data(), sndRam_.size());
    os.write((const char*)reg_.data(), reg_.size());
    os.write((const char*)acc_.data(), sizeof(uint32_t) * acc_.size());
    os.write((const char*)lastData_.data(), lastData_.size());
    os.write((const char*)&ctl_, 1);
    os.write((const char*)&addr_, 2);
    os.write((const char*)&readLatch_, 1);
    os.write((const char*)&irqPend_, 4);
    os.write((const char*)&rege0_, 1);
}

void Es5503::loadState(std::istream& is) {
    is.read((char*)sndRam_.data(), sndRam_.size());
    is.read((char*)reg_.data(), reg_.size());
    is.read((char*)acc_.data(), sizeof(uint32_t) * acc_.size());
    is.read((char*)lastData_.data(), lastData_.size());
    is.read((char*)&ctl_, 1);
    is.read((char*)&addr_, 2);
    is.read((char*)&readLatch_, 1);
    is.read((char*)&irqPend_, 4);
    is.read((char*)&rege0_, 1);
    // Audio transport is host-side: restart it clean.
    rHead_ = rTail_ = 0; masterAcc_ = 0; frac_ = 0.0; cur_ = 0;
    drainFrac_ = 0.0; drainHold_ = 0; drainPrev_ = 0;
}

bool Es5503::anyActive() const {
    int n = oscEnabled();
    for (int o = 0; o < n && o < 32; ++o)
        if (!(ctrl(o) & 0x01)) return true;          // halt bit clear = running
    return false;
}

// ── Oscillator engine (MAME es5503.cpp halt_osc + sound_stream_update) ───
// Called when osc `o` hits the end of its wavetable (type 0) or reads a zero
// byte (type 1). Mode bits (control 2-1): 0 free-run, 1 one-shot, 2 sync/AM,
// 3 swap.
void Es5503::haltOsc(int o, int type, uint32_t& acc, int resshift) {
    uint8_t& c = reg_[0xA0 + o];
    const uint8_t partnerC = reg_[0xA0 + (o ^ 1)];
    int mode = (c >> 1) & 3;
    const int partnerMode = (partnerC >> 1) & 3;
    if (mode == 2) mode = 0;                         // sync/AM loops like free-run (MAME)

    if (mode != 0 || type != 0) {
        c |= 0x01;                                   // halt
    } else {
        // Free-run table wrap: loop, preserving the relative phase.
        const uint32_t wtsize = uint32_t(kWaveSizes[(wts(o) >> 3) & 7]) - 1;
        acc -= wtsize << resshift;
    }

    if (mode == 3) {                                 // swap: start the partner from the top
        reg_[0xA0 + (o ^ 1)] &= uint8_t(~0x01);
        acc_[o ^ 1] = 0;
    } else if (partnerMode == 3 && (o & 1) == 0) {
        // Even oscillator whose PARTNER is in swap (but we aren't): retrigger,
        // preserving phase — "verified on IIgs hardware" (MAME halt_osc).
        c &= uint8_t(~0x01);
        const uint32_t wtsize = uint32_t(kWaveSizes[(wts(o) >> 3) & 7]) - 1;
        acc -= wtsize << resshift;
    }

    if (c & 0x08) irqPend_ |= (1u << o);             // IRQ-enabled voice → pend
}

// One sample at the chip's native rate: step every running oscillator once.
int32_t Es5503::step1() {
    const int nosc = oscEnabled();
    int32_t mix = 0;
    for (int o = 0; o < nosc; ++o) {
        const uint8_t c = ctrl(o);
        if (c & 0x01) continue;                      // halted
        const uint8_t szSel = (wts(o) >> 3) & 7;
        const uint8_t resSel = wts(o) & 7;
        const int resshift = kResShifts[resSel] - szSel;
        const uint32_t wtptr = (uint32_t(wtp(o)) << 8) & kWaveMasks[szSel];
        uint32_t acc = acc_[o];
        const uint32_t altram = acc >> resshift;
        const uint32_t ramptr = altram & kAccMasks[szSel];
        acc += freq(o);
        const uint8_t raw = sndRam_[(ramptr + wtptr) & 0xFFFF];   // IIgs: 64 KB
        lastData_[o] = raw;
        if (raw == 0x00) {
            haltOsc(o, 1, acc, resshift);            // zero byte = end-of-wave marker
        } else {
            mix += (int32_t(raw) - 0x80) * vol(o);
            if (altram >= uint32_t(kWaveSizes[szSel]) - 1)
                haltOsc(o, 0, acc, resshift);        // end of table
        }
        acc_[o] = acc;
    }
    return mix;
}

// Oscillator mix → 16-bit output. The Sound GLU master volume (ctl bits 0-3)
// scales the analog output; >>7 keeps a typical multi-voice mix near full scale
// at max volume (a lone max-volume oscillator is ±32k>>7×16 = ±4k, musical mixes
// of 4-8 voices land in the ±10-30k range).
int16_t Es5503::scaleOut(int32_t mix) const {
    int32_t v = (mix * ((ctl_ & 0x0F) + 1)) >> 7;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    return int16_t(v);
}

// Host-rate render: run the oscillators at docRate = clock/8/(oscs+2) (MAME
// output_rate; clock = 7.15909 MHz) and zero-order-hold up/down to hostRate_.
// Test/offline path — production goes through tickMaster()/drainResampled().
void Es5503::render(int16_t* out, int n) {
    const double docRate = (7159090.0 / 8.0) / double(oscEnabled() + 2);
    const double step = docRate / double(hostRate_);
    for (int s = 0; s < n; ++s) {
        frac_ += step;
        while (frac_ >= 1.0) { cur_ = step1(); frac_ -= 1.0; }
        out[s] = scaleOut(cur_);
    }
}

// ── Master-clock-driven production ───────────────────────────────────────
// One native sample per 16×(oscs+2) master ticks: master 14.31818 MHz → chip
// clock /2 = 7.159 MHz, /8 per oscillator slot, /(oscs+2) slots (MAME
// output_rate). Producing as the CPU runs makes oscillator IRQs land at their
// real time — the tempo timers music engines rely on. Ring overflow (audio
// stalled) drops the oldest samples.
void Es5503::tickMaster(uint32_t masterTicks) {
    masterAcc_ += masterTicks;
    for (;;) {
        const uint32_t per = 16u * uint32_t(oscEnabled() + 2);
        if (masterAcc_ < per) break;
        masterAcc_ -= per;
        ++producedTotal_;
        const int16_t s = scaleOut(step1());
        const uint32_t next = (rHead_ + 1) & (kRing - 1);
        if (next == rTail_) rTail_ = (rTail_ + 1) & (kRing - 1);   // full → drop oldest
        ring_[rHead_] = s; rHead_ = next;
    }
}

void Es5503::drainResampled(int16_t* out, int n, uint32_t hostRate) {
    (void)hostRate;
    // Self-balancing consumption: spread exactly what was produced since the
    // last drain across this frame's n output samples. A fixed nominal ratio
    // drifts against the emulated production rate (slow-penalty variance,
    // frame jitter) — the ring either backs up (latency) or runs dry (gaps =
    // crackle). Occupancy-driven consumption is gapless by construction; the
    // pitch equals the emulated rate. Empty ring → hold the last level.
    const uint32_t avail = (rHead_ + kRing - rTail_) & (kRing - 1);
    const double step = double(avail) / double(n > 0 ? n : 1);
    for (int s = 0; s < n; ++s) {
        drainFrac_ += step;
        while (drainFrac_ >= 1.0) {
            drainFrac_ -= 1.0;
            drainPrev_ = drainHold_;
            if (rTail_ != rHead_) { drainHold_ = ring_[rTail_]; rTail_ = (rTail_ + 1) & (kRing - 1); }
        }
        // Linear interpolation between the last two native samples (plain ZOH
        // upsampling 26 kHz → 44.1 kHz stair-steps audibly).
        out[s] = int16_t(drainPrev_ + int32_t((drainHold_ - drainPrev_) * drainFrac_));
    }
}
