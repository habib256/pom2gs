// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Ensoniq 5503 DOC smoke test: load a sine wavetable into sound RAM via the
// Sound GLU, start one oscillator, render, and verify audible oscillation.
// M6 gate.

#include "Es5503.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

int main() {
    Es5503 doc;

    // Write a 256-byte sine wavetable into sound RAM at $0000 (values 1..255,
    // never 0 so the end-of-wave marker doesn't fire mid-table).
    doc.gluWrite(0xC, 0x20 | 0x40);          // ctl: RAM select + auto-increment
    doc.gluWrite(0xE, 0x00); doc.gluWrite(0xF, 0x00);  // addr = $0000
    for (int i = 0; i < 256; ++i) {
        int v = int(std::round(127.0 * std::sin(2.0 * M_PI * i / 256.0))) + 128;
        if (v < 1) v = 1; if (v > 255) v = 255;
        doc.gluWrite(0xD, uint8_t(v));       // auto-increments addr
    }

    // Program oscillator 0 (DOC-register writes: ctl bit5=0).
    auto wreg = [&](uint8_t r, uint8_t v) {
        doc.gluWrite(0xC, 0x00);             // DOC-register select, no auto-inc
        doc.gluWrite(0xE, r); doc.gluWrite(0xF, 0x00);
        doc.gluWrite(0xD, v);
    };
    wreg(0xE1, 0x00);                        // 1 oscillator enabled
    wreg(0x00, 0x00); wreg(0x20, 0x04);      // frequency (high byte 4)
    wreg(0x40, 0xFF);                        // volume max
    wreg(0x80, 0x00);                        // wavetable pointer = page $00
    wreg(0xC0, 0x00);                        // 256-byte table, full resolution
    wreg(0xA0, 0x00);                        // control: free-run, halt cleared → start

    if (!doc.anyActive()) { std::printf("FAIL: oscillator not running\n"); return 1; }

    int16_t buf[2000];
    doc.render(buf, 2000);

    int pos = 0, neg = 0; long sumsq = 0; int zeroXings = 0; int prev = 0;
    for (int i = 0; i < 2000; ++i) {
        if (buf[i] > 100) ++pos; else if (buf[i] < -100) ++neg;
        sumsq += long(buf[i]) * buf[i];
        if ((buf[i] >= 0) != (prev >= 0)) ++zeroXings;
        prev = buf[i];
    }
    double rms = std::sqrt(double(sumsq) / 2000.0);
    std::printf("DOC render: rms=%.0f  pos=%d neg=%d  zero-crossings=%d\n", rms, pos, neg, zeroXings);
    bool ok = rms > 1000 && pos > 100 && neg > 100 && zeroXings > 4;
    std::printf("%s\n", ok ? "OK: DOC oscillator produces a tone" : "FAIL: no audible oscillation");
    return ok ? 0 : 1;
}
