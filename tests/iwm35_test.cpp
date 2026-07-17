// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Real IWM 3.5" Sony drive gate (LLE path, `iwm35`). Four layers:
//  1. GCR codec self-check: nibblise 160 zoned tracks → de-nibblise →
//     byte-identical image (KEGS g_check_nibblization; pins the exact
//     iwm_nibblize_track_35 / iwm_denib_track35 port incl. checksums).
//  2. Sony register protocol through $C031 + $C0E0-$C0EF like the ROM
//     firmware: 35SEL routing, sense bits (CSTIN/WRTPRT/MOTORON/TK0/
//     SWITCHED/SIDES/DRVIN/READY), commands (motor, step, direction,
//     clear-switched, eject).
//  3. Address field through the IWM data latch: seek cyl 3 side 1, hunt
//     D5 AA 96, check track/side/format/XOR-checksum/epilogue.
//  4. Firmware-style sector WRITE: read an address field on cyl 0 side 0,
//     switch the latches to write, emit a freshly encoded data field (the
//     ROM nibbliser algorithm), flush, and verify the backing file holds the
//     new sector — and only that sector changed.
// Refs: KEGS iwm.c:912-1091 (status/commands) & 3125-3345 (encode); MAME
// floppy.cpp mac_floppy_device; MAME apple2gs.cpp:268-269/1995-2006 ($C031).

#include "IIgsMemory.h"
#include "CPU65816.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
// Each I/O access advances EMULATED time via mem.tick() — the IWM stamps
// accesses with videoCycles_, so the latch pacing needs real ticks.

const uint8_t kGcr[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };
int fromGcr(uint8_t nib) {
    for (int i = 0; i < 64; ++i) if (kGcr[i] == nib) return i;
    return -1;
}

uint8_t ioRead(IIgsMemory& mem, uint8_t off) {
    mem.tick(8);
    return mem.read8((uint32_t(0xE0) << 16) | (0xC0E0 + off));
}
void ioWrite(IIgsMemory& mem, uint8_t off, uint8_t v) {
    mem.tick(8);
    mem.write8((uint32_t(0xE0) << 16) | (0xC0E0 + off), v);
}
void diskReg(IIgsMemory& mem, uint8_t v) {
    mem.write8((uint32_t(0xE0) << 16) | 0xC031, v);
}

// Sony sense read: CA0-2 = phases, SEL = $C031 b7, Q6=1 Q7=0, status bit 7.
// idx = {CA2,CA1,CA0,SEL}.
int sense(IIgsMemory& mem, int idx, bool sel35on = true) {
    diskReg(mem, uint8_t((sel35on ? 0x40 : 0x00) | ((idx & 1) ? 0x80 : 0x00)));
    ioWrite(mem, uint8_t(((idx >> 1) & 1) ? 0x01 : 0x00), 0);   // CA0
    ioWrite(mem, uint8_t(((idx >> 2) & 1) ? 0x03 : 0x02), 0);   // CA1
    ioWrite(mem, uint8_t(((idx >> 3) & 1) ? 0x05 : 0x04), 0);   // CA2
    ioWrite(mem, 0x0E, 0);                                       // Q7 off
    ioWrite(mem, 0x0D, 0);                                       // Q6 on
    uint8_t st = ioRead(mem, 0x0D);
    ioWrite(mem, 0x0C, 0);                                       // Q6 off
    return (st >> 7) & 1;
}

// Sony command: address as above, pulse LSTRB (phase 3) with ENABLE on.
void command(IIgsMemory& mem, int idx) {
    diskReg(mem, uint8_t(0x40 | ((idx & 1) ? 0x80 : 0x00)));
    ioWrite(mem, uint8_t(((idx >> 1) & 1) ? 0x01 : 0x00), 0);
    ioWrite(mem, uint8_t(((idx >> 2) & 1) ? 0x03 : 0x02), 0);
    ioWrite(mem, uint8_t(((idx >> 3) & 1) ? 0x05 : 0x04), 0);
    ioWrite(mem, 0x07, 0);                                       // LSTRB rising edge
    ioWrite(mem, 0x06, 0);
}

// Latch-paced data read: poll until the MSB flags a valid nibble (one every
// ~16 µs of platter time; each I/O access advances the fake clock 40 ticks).
uint8_t readNib(IIgsMemory& mem) {
    ioWrite(mem, 0x0C, 0); ioWrite(mem, 0x0E, 0);                // Q6=0 Q7=0
    for (int i = 0; i < 64; ++i) {
        uint8_t v = ioRead(mem, 0x0C);
        if (v & 0x80) return v;
    }
    return 0;                                                    // stalled latch
}

// Encode 512 data bytes as a 3.5" GCR data field payload (699 nibbles + 4
// checksum nibbles) — the ROM nibbliser algorithm (KEGS iwm.c:3216-3327),
// duplicated here as "the firmware" writing a sector.
void encodeField(const uint8_t* buf, std::vector<uint8_t>& out) {
    uint32_t bufC[0x100], bufD[0x100], bufE[0x100];
    uint32_t t5c = 0, t5d = 0, t5e = 0, val;
    int carry, x = 0xAF, y;
    bufC[0] = bufD[0] = bufE[0] = bufE[1] = 0;
    for (y = 4; y > 0; --y) { bufC[x] = bufD[x] = bufE[x] = 0; --x; }
    y = 0;
    while (x >= 0) {
        t5c <<= 1; carry = int(t5c >> 8); t5c = (t5c + uint32_t(carry)) & 0xFF;
        val = buf[y]; t5e = val + t5e + uint32_t(carry); carry = int(t5e >> 8); t5e &= 0xFF;
        bufC[x] = val ^ t5c; ++y;
        val = buf[y]; t5d = t5d + val + uint32_t(carry); carry = int(t5d >> 8); t5d &= 0xFF;
        bufD[x] = val ^ t5e; ++y;
        --x;
        if (x <= 0) break;
        val = buf[y]; t5c = t5c + val + uint32_t(carry); carry = int(t5c >> 8); t5c &= 0xFF;
        bufE[x + 1] = val ^ t5d; ++y;
    }
    val = ((t5c >> 2) ^ t5d) & 0x3F;
    val = (val ^ t5d) >> 2;
    val = (val ^ t5e) & 0x3F;
    val = (val ^ t5e) >> 2;
    const uint32_t t5f = val;
    uint32_t t63 = 0, t64 = 0, t65 = 0, accHi = 0;
    y = 0xAE;
    while (y >= 0) {
        out.push_back(kGcr[accHi & 0x3F]);
        val = t63 & 0x3F; t63 = bufC[y]; accHi = t63 >> 6;                 out.push_back(kGcr[val]);
        val = t64 & 0x3F; t64 = bufD[y]; accHi = (accHi << 2) + (t64 >> 6); out.push_back(kGcr[val]);
        --y;
        if (y < 0) break;
        val = t65 & 0x3F; t65 = bufE[y + 1]; accHi = (accHi << 2) + (t65 >> 6); out.push_back(kGcr[val]);
    }
    out.push_back(kGcr[t5f & 0x3F]);
    out.push_back(kGcr[t5e & 0x3F]);
    out.push_back(kGcr[t5d & 0x3F]);
    out.push_back(kGcr[t5c & 0x3F]);
}
}

int main() {
    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };

    // Synthetic 800K image: every 512-byte sector carries a distinct pattern.
    std::vector<uint8_t> img(819200);
    for (size_t s = 0; s < img.size() / 512; ++s)
        for (int i = 0; i < 512; ++i)
            img[s * 512 + size_t(i)] = uint8_t((s * 31 + size_t(i) * 7 + (s >> 8)) & 0xFF);
    const char* path = "/tmp/pomiigs_iwm35_test.po";
    { std::ofstream o(path, std::ios::binary); o.write((const char*)img.data(), std::streamsize(img.size())); }

    IIgsMemory mem;
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();
    mem.setIwm35(true);

    check("loadDisk35 (IWM)", mem.loadDisk35(path));

    // ── 1. GCR codec self-check (KEGS g_check_nibblization) ──────────────
    check("codec round-trip 160 tracks", mem.iwm().sony35().checkNibblization());

    // ── 2. Sony register protocol ────────────────────────────────────────
    diskReg(mem, 0x40);                             // 35SEL
    ioWrite(mem, 0x09, 0);                          // ENABLE on
    ioWrite(mem, 0x0A, 0);                          // drive 0
    command(mem, 0x4);                              // motor on
    command(mem, 0x8);                              // direction outward
    for (int i = 0; i < 82; ++i) command(mem, 0x2); // step to cylinder 0 (clamped)
    check("TK0 at cyl 0 (0)", sense(mem, 0x5) == 0);
    command(mem, 0x0);                              // direction inward
    command(mem, 0x2);                              // step to cyl 1
    check("TK0 off cyl 0 (1)", sense(mem, 0x5) == 1);
    check("DIRTN inward (0)", sense(mem, 0x0) == 0);
    check("CSTIN disk present (0)", sense(mem, 0x1) == 0);
    check("STEP complete (1)", sense(mem, 0x2) == 1);
    check("WRTPRT writable (1)", sense(mem, 0x3) == 1);
    check("MOTORON spinning (0)", sense(mem, 0x4) == 0);
    check("SIDES double-sided (1)", sense(mem, 0xC) == 1);
    check("DRVIN installed (0)", sense(mem, 0xF) == 0);
    check("READY (0)", sense(mem, 0xD) == 0);
    check("SWITCHED set after insert (1)", sense(mem, 0x6) == 1);
    command(mem, 0x9);                              // clear disk-switched
    check("SWITCHED cleared (0)", sense(mem, 0x6) == 0);
    command(mem, 0xC);                              // motor off
    check("MOTORON off (1)", sense(mem, 0x4) == 1);
    command(mem, 0x4);                              // motor back on
    // With 35SEL clear the same status read is the 5.25" write-protect
    // sense — an empty 5.25" drive reads protected (bit7 = 1).
    check("35SEL=0 routes to 5.25\"", sense(mem, 0x3, false) == 1);

    // ── 3. Address field through the data latch ──────────────────────────
    command(mem, 0x8);
    for (int i = 0; i < 82; ++i) command(mem, 0x2);   // back to cyl 0
    command(mem, 0x0);
    for (int i = 0; i < 3; ++i) command(mem, 0x2);    // cyl 3
    diskReg(mem, 0x40 | 0x80);                        // HDSEL = side 1
    {
        bool found = false;
        int guard = 40000;
        while (guard-- > 0) {
            if (readNib(mem) != 0xD5) continue;
            if (readNib(mem) != 0xAA) continue;
            if (readNib(mem) != 0x96) continue;
            const int trkV  = fromGcr(readNib(mem));
            const int secV  = fromGcr(readNib(mem));
            const int sideV = fromGcr(readNib(mem));
            const int fmtV  = fromGcr(readNib(mem));
            const int ckV   = fromGcr(readNib(mem));
            check("addr: track 3", trkV == 3);
            check("addr: side 1", sideV == (1 << 5));
            check("addr: format $22", fmtV == 0x22);
            check("addr: sector in range", secV >= 0 && secV < 12);
            check("addr: checksum", ckV == ((trkV ^ secV ^ sideV ^ fmtV) & 0x3F));
            check("addr: epilogue DE", readNib(mem) == 0xDE);
            check("addr: epilogue AA", readNib(mem) == 0xAA);
            found = true;
            break;
        }
        check("found D5 AA 96 on trk 3.1", found);
    }

    // ── 4. Firmware-style sector write ───────────────────────────────────
    // Cyl 0 side 0: find an address field, note its sector, then write a
    // fresh data field over the old one exactly like the ROM does.
    command(mem, 0x8);
    for (int i = 0; i < 82; ++i) command(mem, 0x2);   // cyl 0
    diskReg(mem, 0x40);                               // side 0
    {
        int wrSec = -1;
        int guard = 40000;
        while (guard-- > 0) {
            if (readNib(mem) != 0xD5) continue;
            if (readNib(mem) != 0xAA) continue;
            if (readNib(mem) != 0x96) continue;
            const int trkV = fromGcr(readNib(mem));
            const int secV = fromGcr(readNib(mem));
            readNib(mem); readNib(mem); readNib(mem);  // side, fmt, cksum
            if (readNib(mem) != 0xDE) continue;
            if (readNib(mem) != 0xAA) continue;
            if (trkV != 0) continue;
            wrSec = secV;
            break;
        }
        check("write: found target address field", wrSec >= 0);
        if (wrSec < 0) wrSec = 0;   // keep going so later checks report, not crash

        // New sector contents.
        uint8_t newData[512];
        for (int i = 0; i < 512; ++i) newData[i] = uint8_t(0xA5 ^ (i * 13));

        // The full field the firmware emits after the address epilogue.
        std::vector<uint8_t> field;
        for (int i = 0; i < 5; ++i) field.push_back(0xFF);       // sync
        field.push_back(0xD5); field.push_back(0xAA); field.push_back(0xAD);
        field.push_back(kGcr[wrSec]);
        encodeField(newData, field);
        field.push_back(0xDE); field.push_back(0xAA); field.push_back(0xFF);

        // Switch the latches to write mode via READS (a write to $C0EF with
        // Q6+ENABLE already on would latch a spurious nibble), then stream
        // the nibbles with data writes; the handshake ($C0EE, Q7=1 Q6=0)
        // always reads ready in our model.
        ioRead(mem, 0x0D); ioRead(mem, 0x0F);                    // Q6 on, Q7 on
        for (uint8_t nib : field) ioWrite(mem, 0x0D, nib);
        ioRead(mem, 0x0E); ioRead(mem, 0x0C);                    // back to read mode

        mem.iwm().flushDisk35();
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check("write: file size intact", back.size() == img.size());
        bool secOk = back.size() == img.size() &&
                     std::memcmp(back.data() + size_t(wrSec) * 512, newData, 512) == 0;
        check("write: sector holds new data", secOk);
        // Every other byte untouched.
        bool restOk = back.size() == img.size();
        if (restOk) {
            std::vector<uint8_t> expect = img;
            std::memcpy(expect.data() + size_t(wrSec) * 512, newData, 512);
            restOk = back == expect;
        }
        check("write: rest of image untouched", restOk);
    }

    // ── 5. FORMAT-style full-track rewrite ───────────────────────────────
    // Write an ENTIRE track 0.0 in one session (like the ROM's FORMAT): the
    // session buffer replaces the track wholesale, and the flush de-nibblises
    // the ROM-layout stream back into the image.
    std::vector<uint8_t> fmtData(12 * 512);
    for (size_t i = 0; i < fmtData.size(); ++i) fmtData[i] = uint8_t(0x3C ^ (i * 11) ^ (i >> 7));
    {
        std::vector<uint8_t> trackStream;
        for (int sec = 0; sec < 12; ++sec) {
            for (int i = 0; i < 54; ++i) trackStream.push_back(0xFF);
            trackStream.push_back(0xD5); trackStream.push_back(0xAA); trackStream.push_back(0x96);
            trackStream.push_back(kGcr[0]);            // track 0
            trackStream.push_back(kGcr[sec]);
            trackStream.push_back(kGcr[0]);            // side 0
            trackStream.push_back(kGcr[0x22]);
            trackStream.push_back(kGcr[(0 ^ sec ^ 0 ^ 0x22) & 0x3F]);
            trackStream.push_back(0xDE); trackStream.push_back(0xAA);
            for (int i = 0; i < 5; ++i) trackStream.push_back(0xFF);
            trackStream.push_back(0xD5); trackStream.push_back(0xAA); trackStream.push_back(0xAD);
            trackStream.push_back(kGcr[sec]);
            encodeField(fmtData.data() + size_t(sec) * 512, trackStream);
            trackStream.push_back(0xDE); trackStream.push_back(0xAA); trackStream.push_back(0xFF);
        }
        // Stream it in one write session (Q6+Q7 on, ENABLE on, motor on).
        ioRead(mem, 0x0D); ioRead(mem, 0x0F);
        for (uint8_t nib : trackStream) ioWrite(mem, 0x0D, nib);
        ioRead(mem, 0x0E); ioRead(mem, 0x0C);          // Q7 fall ends the session

        mem.iwm().flushDisk35();
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bool fmtOk = back.size() == img.size() &&
                     std::memcmp(back.data(), fmtData.data(), fmtData.size()) == 0;
        check("format: whole track 0.0 re-imaged", fmtOk);
        // And it must still be readable through the head.
        bool refound = false;
        for (int guard = 200000; guard-- > 0;) {
            if (readNib(mem) != 0xD5) continue;
            if (readNib(mem) != 0xAA) continue;
            if (readNib(mem) == 0x96) { refound = true; break; }
        }
        check("format: track readable after rewrite", refound);
    }

    // ── 6. Tach: zone-dependent pulse rate (394 vs 590 rpm) ──────────────
    {
        auto countTachTransitions = [&](int calls) {
            int last = sense(mem, 0x7), n = 0;
            for (int i = 0; i < calls; ++i) {
                int v = sense(mem, 0x7);
                if (v != last) { ++n; last = v; }
            }
            return n;
        };
        command(mem, 0x8);                             // outward
        for (int i = 0; i < 82; ++i) command(mem, 0x2);   // cyl 0 (zone 0: 394 rpm)
        int slowZone = countTachTransitions(4000);
        command(mem, 0x0);                             // inward
        for (int i = 0; i < 79; ++i) command(mem, 0x2);   // cyl 79 (zone 4: 590 rpm)
        int fastZone = countTachTransitions(4000);
        check("tach: pulses present in zone 0", slowZone > 0);
        check("tach: zone 4 spins faster than zone 0", fastZone > slowZone);
    }

    // ── 7. Second internal drive ($C0EB select) ──────────────────────────
    {
        ioWrite(mem, 0x0B, 0);                         // select drive 1
        check("drive 2: CSTIN empty (1)", sense(mem, 0x1) == 1);
        check("drive 2: DRVIN installed (0)", sense(mem, 0xF) == 0);

        // Mount a second image with its own pattern and read it.
        std::vector<uint8_t> img2(819200, 0x5A);
        const char* path2 = "/tmp/pomiigs_iwm35_test_b.po";
        { std::ofstream o(path2, std::ios::binary); o.write((const char*)img2.data(), std::streamsize(img2.size())); }
        check("drive 2: loadDisk35(drive 1)", mem.loadDisk35(path2, 1));
        check("drive 2: CSTIN present (0)", sense(mem, 0x1) == 0);
        command(mem, 0x4);                             // motor on (drive 1)
        command(mem, 0x8);
        for (int i = 0; i < 82; ++i) command(mem, 0x2);   // its own cyl 0
        check("drive 2: TK0 (0)", sense(mem, 0x5) == 0);
        bool found2 = false;
        for (int guard = 200000; guard-- > 0;) {
            if (readNib(mem) != 0xD5) continue;
            if (readNib(mem) != 0xAA) continue;
            if (readNib(mem) == 0x96) { found2 = true; break; }
        }
        check("drive 2: address field readable", found2);
        command(mem, 0xC);                             // motor off
        ioWrite(mem, 0x0A, 0);                         // back to drive 0
        check("drive 1 unaffected: CSTIN (0)", sense(mem, 0x1) == 0);
        std::remove(path2);
    }

    // ── Eject via command $E: media out, switched set ────────────────────
    command(mem, 0xC);                                // motor off first
    command(mem, 0xE);                                // eject
    check("CSTIN empty after eject (1)", sense(mem, 0x1) == 1);
    check("SWITCHED after eject (1)", sense(mem, 0x6) == 1);

    std::remove(path);
    if (fails) { std::printf("iwm35_test: %d FAILURES\n", fails); return 1; }
    std::printf("iwm35_test: all checks passed\n");
    return 0;
}
