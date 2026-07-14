// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Ensoniq 5503 DOC (Digital Oscillator Chip) ───────────────────────────
// The IIgs sound chip: 32 wavetable oscillators reading from a dedicated
// 64 KB sound RAM (not CPU-mapped — reached through the Sound GLU at
// $C03C-$C03F). Oscillators run free-run / one-shot / sync / swap and
// generate IRQs. A sample byte of 0 is the end-of-wave marker (halts the
// oscillator). Output = (sample - 0x80) * volume, mixed across active
// oscillators. Enabled-oscillator count = (reg $E1 >> 1) + 1.
//
// Source of truth: MAME sound/es5503.cpp; Ensoniq 5503 datasheet.

#ifndef POMIIGS_ES5503_H
#define POMIIGS_ES5503_H

#include <cstdint>
#include <array>

class Es5503
{
public:
    Es5503() { reset(); }
    void reset();

    // ── Sound GLU ($C03C-$C03F) ──────────────────────────────────────────
    // $C03C SOUNDCTL: bit6 auto-inc addr, bit5 RAM(1)/DOC-reg(0) select, bits0-3 vol.
    // $C03D SOUNDDATA: read/write the addressed DOC register or sound RAM byte.
    // $C03E/$C03F: 16-bit address pointer (auto-increments if enabled).
    uint8_t gluRead(uint8_t reg);            // reg = 0xC..0xF (low nibble of $C03x)
    void    gluWrite(uint8_t reg, uint8_t v);

    // Render `n` mono samples (signed 16-bit) of the current oscillator state.
    void render(int16_t* out, int n);

    // True while any enabled oscillator is running (for the IRQ line / tests).
    bool anyActive() const;

    // DOC oscillator IRQ: set when an IRQ-enabled oscillator (control bit3)
    // completes during render(); read of the osc-interrupt register ($E0) via
    // the Sound GLU clears it. The MMU mirrors this onto the CPU IRQ line.
    bool irqPending() const { return irqLine_; }

private:
    std::array<uint8_t, 0x10000> sndRam_{};    // 64 KB sound RAM
    std::array<uint8_t, 0x100>   reg_{};       // 256 DOC registers
    std::array<uint32_t, 32>     acc_{};       // per-osc phase accumulators

    uint8_t  ctl_ = 0;                         // $C03C
    uint16_t addr_ = 0;                        // $C03E/F pointer
    bool     irqLine_ = false;                 // an IRQ-enabled osc has completed
    uint8_t  irqOsc_ = 0;                      // last oscillator that raised the IRQ

    int oscEnabled() const { return ((reg_[0xE1] >> 1) & 0x1F) + 1; }   // reg $E1 bits 1-5
    // Per-oscillator register accessors (base + osc index).
    uint32_t freq(int o) const { return reg_[0x00 + o] | (reg_[0x20 + o] << 8); }
    uint8_t  vol(int o)  const { return reg_[0x40 + o]; }
    uint8_t  wtp(int o)  const { return reg_[0x80 + o]; }
    uint8_t  ctrl(int o) const { return reg_[0xA0 + o]; }
    uint8_t  wts(int o)  const { return reg_[0xC0 + o]; }
};

#endif // POMIIGS_ES5503_H
