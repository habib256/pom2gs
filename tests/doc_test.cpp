// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Ensoniq 5503 DOC gate (M6). Pins the MAME es5503.cpp semantics:
//   1. free-run tone through the Sound GLU (wavetable → audible oscillation)
//   2. native-rate pitch: docRate = 894886/(oscs+2); a known freq must land
//      in a known zero-crossing window at the host rate
//   3. swap mode: an ending oscillator halts and STARTS its partner (the
//      ping-pong GS/OS's Sound Tools + synthLAB use for streaming)
//   4. $E0 interrupt protocol: lowest pending osc in bits 5-1, read clears
//      that osc's pend, line stays up while others pend, bit7 = none left

#include "Es5503.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

static int fails = 0;
static void check(const char* what, bool ok) {
    std::printf("%-58s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

// GLU helpers.
static void ramWrite(Es5503& doc, uint16_t addr, const uint8_t* p, int n) {
    doc.gluWrite(0xC, 0x20 | 0x40 | 0x0F);           // RAM select + auto-inc + max volume
    doc.gluWrite(0xE, uint8_t(addr & 0xFF)); doc.gluWrite(0xF, uint8_t(addr >> 8));
    for (int i = 0; i < n; ++i) doc.gluWrite(0xD, p[i]);
}
static void wreg(Es5503& doc, uint8_t r, uint8_t v) {
    doc.gluWrite(0xC, 0x0F);                          // DOC-register select, max volume
    doc.gluWrite(0xE, r); doc.gluWrite(0xF, 0x00);
    doc.gluWrite(0xD, v);
}
static uint8_t rreg(Es5503& doc, uint8_t r) {
    doc.gluWrite(0xC, 0x0F);
    doc.gluWrite(0xE, r); doc.gluWrite(0xF, 0x00);
    doc.gluRead(0xD);                                 // dummy read (GLU latch pipeline)
    return doc.gluRead(0xD);
}

int main() {
    // ── 1. free-run tone ─────────────────────────────────────────────────
    Es5503 doc;
    uint8_t sine[256];
    for (int i = 0; i < 256; ++i) {
        int v = int(std::round(127.0 * std::sin(2.0 * M_PI * i / 256.0))) + 128;
        if (v < 1) v = 1; if (v > 255) v = 255;       // never 0 (end-of-wave marker)
        sine[i] = uint8_t(v);
    }
    ramWrite(doc, 0x0000, sine, 256);
    wreg(doc, 0xE1, 62);                              // 32 oscillators (GS/OS default)
    wreg(doc, 0x00, 0x00); wreg(doc, 0x20, 0x08);     // freq = $0800
    wreg(doc, 0x40, 0xFF);                            // volume max
    wreg(doc, 0x80, 0x00);                            // wavetable page $00
    wreg(doc, 0xC0, 0x00);                            // 256-byte table, res 0
    wreg(doc, 0xA0, 0x00);                            // free-run, start
    check("oscillator running after key-on", doc.anyActive());

    static int16_t buf[4000];
    doc.setOutputRate(44100);
    doc.render(buf, 4000);
    int pos = 0, neg = 0, xings = 0; long sumsq = 0; int prev = 0;
    for (int i = 0; i < 4000; ++i) {
        if (buf[i] > 100) ++pos; else if (buf[i] < -100) ++neg;
        sumsq += long(buf[i]) * buf[i];
        if ((buf[i] >= 0) != (prev >= 0)) ++xings;
        prev = buf[i];
    }
    double rms = std::sqrt(double(sumsq) / 4000.0);
    std::printf("  tone: rms=%.0f pos=%d neg=%d zero-crossings=%d\n", rms, pos, neg, xings);
    check("free-run tone audible (rms/pos/neg)", rms > 500 && pos > 200 && neg > 200);

    // ── 2. pitch at the native rate ──────────────────────────────────────
    // 32 oscs → docRate = 894886.25/34 = 26320 Hz. freq $0800 = 2048, res 0,
    // 256-byte table → resshift 9 → 4 table steps/sample → period 64 samples
    // → 411.3 Hz. In 4000 samples at 44.1 kHz (90.7 ms): 2×411.3×0.0907 ≈ 75
    // crossings. The old host-rate render gave ~125 (68% sharp).
    check("pitch: ~411 Hz at 32 oscs (65..85 crossings)", xings >= 65 && xings <= 85);

    // ── 3. swap-mode ping-pong ───────────────────────────────────────────
    // Osc 0 (swap, running) plays page $01; osc 1 (swap, halted) page $02.
    // When osc 0's table ends it must halt itself and START osc 1; when osc 1
    // ends it must re-start osc 0 — continuous ping-pong.
    Es5503 d2;
    uint8_t ramp[256]; for (int i = 0; i < 256; ++i) ramp[i] = uint8_t(1 + (i & 0x7F));
    ramWrite(d2, 0x0100, ramp, 256);
    ramWrite(d2, 0x0200, ramp, 256);
    wreg(d2, 0xE1, 2);                                // 2 oscillators
    wreg(d2, 0x00, 0x00); wreg(d2, 0x20, 0x20);       // osc0 freq $2000
    wreg(d2, 0x01, 0x00); wreg(d2, 0x21, 0x20);       // osc1 freq $2000
    wreg(d2, 0x40, 0xFF); wreg(d2, 0x41, 0xFF);
    wreg(d2, 0x80, 0x01); wreg(d2, 0x81, 0x02);       // pages $01 / $02
    wreg(d2, 0xC0, 0x00); wreg(d2, 0xC1, 0x00);
    wreg(d2, 0xA1, 0x07);                             // osc1: swap mode, HALTED
    wreg(d2, 0xA0, 0x06);                             // osc0: swap mode, START
    check("swap: osc0 runs, osc1 halted", !(rreg(d2, 0xA0) & 1) && (rreg(d2, 0xA1) & 1));
    d2.setOutputRate(44100);
    // Timing: 2 oscs → docRate = 894886/4 = 223.7 kHz → 5.07 DOC steps per host
    // sample. freq $2000 → 16 table entries/step → the 256-entry table ends after
    // 16 DOC steps. 5 host samples ≈ 25 steps: osc0 ended (16), osc1 mid-flight.
    d2.render(buf, 5);
    bool osc0Halted = (rreg(d2, 0xA0) & 1) != 0;
    bool osc1Started = (rreg(d2, 0xA1) & 1) == 0;
    check("swap: table end halts osc0 and starts osc1", osc0Halted && osc1Started);
    d2.render(buf, 3);                                // ≈40 steps: osc1 ended (32) → osc0 again
    check("swap: ping-pong returns to osc0", (rreg(d2, 0xA0) & 1) == 0 && (rreg(d2, 0xA1) & 1) != 0);

    // ── 4. $E0 interrupt protocol ────────────────────────────────────────
    Es5503 d3;
    ramWrite(d3, 0x0100, ramp, 256);
    wreg(d3, 0xE1, 2);                                // 2 oscillators
    wreg(d3, 0x00, 0x00); wreg(d3, 0x20, 0x20);
    wreg(d3, 0x01, 0x00); wreg(d3, 0x21, 0x20);
    wreg(d3, 0x40, 0xFF); wreg(d3, 0x41, 0xFF);
    wreg(d3, 0x80, 0x01); wreg(d3, 0x81, 0x01);
    wreg(d3, 0xC0, 0x00); wreg(d3, 0xC1, 0x00);
    wreg(d3, 0xA1, 0x0A);                             // osc1: one-shot + IRQ enable, start
    wreg(d3, 0xA0, 0x0A);                             // osc0: one-shot + IRQ enable, start
    d3.setOutputRate(44100);
    d3.render(buf, 80);                               // both tables end → both pend
    check("IRQ line up after one-shot completions", d3.irqPending());
    // The real ack loop: address $E0 (no auto-inc) and stream $C03D reads through
    // the GLU latch — the first is a discarded stale byte, then one ack per read
    // (each read latches the NEXT $E0 scan), ending on bit7 = no more.
    d3.gluWrite(0xC, 0x0F);
    d3.gluWrite(0xE, 0xE0); d3.gluWrite(0xF, 0x00);
    d3.gluRead(0xD);                                  // stale (primes the pipeline; acks osc0)
    check("IRQ line still up after 1st ack (osc1 pending)", d3.irqPending());
    uint8_t e0a = d3.gluRead(0xD);                    // osc0's value (acks osc1)
    check("$E0 stream #1 → osc 0 (bit7 clear)", (e0a & 0x80) == 0 && ((e0a >> 1) & 0x1F) == 0);
    check("IRQ line clear after both acked", !d3.irqPending());
    uint8_t e0b = d3.gluRead(0xD);                    // osc1's value
    check("$E0 stream #2 → osc 1", (e0b & 0x80) == 0 && ((e0b >> 1) & 0x1F) == 1);
    uint8_t e0c = d3.gluRead(0xD);                    // terminator
    check("$E0 stream #3 → no interrupt (bit7 set)", (e0c & 0x80) != 0);

    // ── 5. Sound GLU decode + latch (MAME apple2gs.cpp) ──────────────────
    // ctl bit6 = RAM select, bit5 = auto-increment. ctl=$20 must address DOC
    // REGISTERS with auto-inc (the bits were once swapped: register writes
    // landed in sound RAM and no oscillator ever keyed on — silent DOC).
    Es5503 d4;
    d4.gluWrite(0xC, 0x20 | 0x0F);                    // registers + auto-inc
    d4.gluWrite(0xE, 0x40); d4.gluWrite(0xF, 0x00);   // volume regs, osc 0…
    d4.gluWrite(0xD, 0x55); d4.gluWrite(0xD, 0x66);   // …auto-inc to osc 1
    check("GLU $20 = register mode + auto-inc (not RAM)",
          rreg(d4, 0x40) == 0x55 && rreg(d4, 0x41) == 0x66);
    // $C03D reads are latched one deep: the first read returns the previous
    // fetch (dummy-read protocol used by the $E0 ack ISRs).
    d4.gluWrite(0xC, 0x0F);
    d4.gluWrite(0xE, 0x40); d4.gluWrite(0xF, 0x00);
    uint8_t stale = d4.gluRead(0xD);                  // latch primed elsewhere
    uint8_t fresh = d4.gluRead(0xD);
    check("GLU $C03D read latch (dummy-read yields stale byte)",
          fresh == 0x55 && stale != 0x55);

    // ── 6. cycle-driven production path (tickMaster → drainResampled) ────
    // One native sample per 16×(oscs+2) master ticks; the MMU drives this from
    // the CPU loop so timer-oscillator IRQs land at their real time (tempo).
    Es5503 d5;
    ramWrite(d5, 0x0000, sine, 256);
    wreg(d5, 0xE1, 62);                               // 32 oscs → per = 16*34 = 544
    wreg(d5, 0x00, 0x00); wreg(d5, 0x20, 0x08);
    wreg(d5, 0x40, 0xFF);
    wreg(d5, 0x80, 0x00); wreg(d5, 0xC0, 0x00);
    wreg(d5, 0xA0, 0x00);                             // free-run, start
    d5.tickMaster(544 * 200);                         // exactly 200 native samples
    static int16_t o2[300];
    d5.drainResampled(o2, 300, 44100);                // consumes ~179 of them
    long ss = 0; for (int i = 0; i < 300; ++i) ss += long(o2[i]) * o2[i];
    check("tickMaster/drainResampled produce the tone", std::sqrt(double(ss) / 300.0) > 500);

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
