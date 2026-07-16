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
// The chip's native sample rate depends on that count:
//   docRate = clock/8 / (oscs+2)   (clock = 7.15909 MHz → 894886/(N+2) Hz)
// render() steps the oscillators at the native rate and zero-order-hold
// resamples to the host rate (setOutputRate) so pitch and tempo are correct
// for any oscillator count (GS/OS enables all 32 → 26.3 kHz).
//
// Source of truth: MAME sound/es5503.cpp; Ensoniq 5503 datasheet.

#ifndef POMIIGS_ES5503_H
#define POMIIGS_ES5503_H

#include <cstdint>
#include <array>
#include <iosfwd>

class Es5503
{
public:
    Es5503() { reset(); }
    void reset();

    // ── Sound GLU ($C03C-$C03F) ──────────────────────────────────────────
    // $C03C SOUNDCTL: bit6 RAM(1)/DOC-reg(0) select, bit5 auto-inc addr,
    // bits0-3 volume (MAME apple2gs.cpp). $C03D SOUNDDATA: read/write the
    // addressed DOC register or sound RAM byte — reads are LATCHED one deep
    // (dummy-read protocol). $C03E/$C03F: 16-bit address pointer.
    uint8_t gluRead(uint8_t reg);            // reg = 0xC..0xF (low nibble of $C03x)
    void    gluWrite(uint8_t reg, uint8_t v);

    // Render `n` mono samples (signed 16-bit) at the host rate. Test/offline
    // path — the production path is tickMaster() + drainResampled().
    void render(int16_t* out, int n);
    void setOutputRate(uint32_t hz) { if (hz) hostRate_ = hz; }

    // ── Master-clock-driven production (sample-accurate IRQs) ────────────
    // The MMU calls tickMaster() as the CPU executes; one native sample is
    // produced every 16×(oscs+2) master ticks (14.31818 MHz master → 7.16 MHz
    // chip clock, 8 clocks per oscillator slot). Music engines clock their
    // TEMPO from a timer oscillator's IRQs — per-frame batch rendering merged
    // those IRQs (3-4 → 1 at 60 Hz) and played everything ~3× too slow.
    void tickMaster(uint32_t masterTicks);
    // Drain the produced samples, resampled (ZOH) to hostRate, into out[n];
    // holds the last level if the ring runs dry (frame jitter).
    void drainResampled(int16_t* out, int n, uint32_t hostRate);

    // True while any enabled oscillator is running (for tests).
    bool anyActive() const;
    uint64_t producedTotal() const { return producedTotal_; }   // native samples (diagnostics)

    // Snapshot: chip state (sound RAM, registers, accumulators, GLU). The
    // audio-transport transients (ring, resampler phase) reset on load.
    void saveState(std::ostream& os) const;
    void loadState(std::istream& is);

    // DOC oscillator IRQ line: an IRQ-enabled oscillator (control bit3) has
    // completed and its pend flag hasn't been consumed by an $E0 read yet.
    // Masked to the ENABLED oscillator count: the $E0 ack scan only covers
    // enabled oscillators, so a stale pend above the count (after software
    // shrinks $E1) would hold the line high forever — an IRQ storm.
    bool irqPending() const {
        const int n = oscEnabled();
        const uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1);
        return (irqPend_ & mask) != 0;
    }

private:
    std::array<uint8_t, 0x10000> sndRam_{};    // 64 KB sound RAM
    std::array<uint8_t, 0x100>   reg_{};       // 256 DOC registers
    std::array<uint32_t, 32>     acc_{};       // per-osc phase accumulators
    std::array<uint8_t, 32>      lastData_{};  // per-osc last wavetable byte ($60+o reads)

    uint8_t  ctl_ = 0;                         // $C03C (bit0-3 = master volume)
    uint16_t addr_ = 0;                        // $C03E/F pointer
    uint8_t  readLatch_ = 0;                   // $C03D one-deep read latch (GLU pipeline)
    uint32_t irqPend_ = 0;                     // bit per oscillator (MAME irqpend)
    uint8_t  rege0_ = 0xFF;                    // last $E0 value (MAME m_rege0)
    uint32_t hostRate_ = 44100;                // host output rate (render target)
    double   frac_ = 0.0;                      // native→host resampler phase
    int32_t  cur_ = 0;                         // current native sample (ZOH hold)

    uint8_t regRead(uint8_t r);                // DOC register read ($E0 IRQ scan)
    void    regWrite(uint8_t r, uint8_t v);
    int32_t step1();                           // one native-rate sample (all oscs)
    int16_t scaleOut(int32_t mix) const;       // osc mix → 16-bit out (GLU volume)
    void    haltOsc(int o, int type, uint32_t& acc, int resshift);   // MAME halt_osc

    // Native-sample ring (producer: tickMaster; consumer: drainResampled).
    static constexpr uint32_t kRing = 16384;   // ~0.6 s at 26 kHz
    std::array<int16_t, kRing> ring_{};
    uint32_t rHead_ = 0, rTail_ = 0;
    uint32_t masterAcc_ = 0;                   // master ticks toward the next sample
    uint64_t producedTotal_ = 0;               // lifetime native-sample count (diagnostics)
    double   drainFrac_ = 0.0;                 // resampler phase
    int16_t  drainHold_ = 0;                   // newest native sample (hold on dry ring)
    int32_t  drainPrev_ = 0;                   // previous native sample (linear interp)

    int oscEnabled() const { return ((reg_[0xE1] >> 1) & 0x1F) + 1; }   // reg $E1 bits 1-5
    // Per-oscillator register accessors (base + osc index).
    uint32_t freq(int o) const { return reg_[0x00 + o] | (reg_[0x20 + o] << 8); }
    uint8_t  vol(int o)  const { return reg_[0x40 + o]; }
    uint8_t  wtp(int o)  const { return reg_[0x80 + o]; }
    uint8_t  ctrl(int o) const { return reg_[0xA0 + o]; }
    uint8_t  wts(int o)  const { return reg_[0xC0 + o]; }
};

#endif // POMIIGS_ES5503_H
