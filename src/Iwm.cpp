// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// IWM Disk II 5.25". See Iwm.h. Source of truth: MAME machine/iwm.cpp,
// Beneath Apple DOS (6-and-2 GCR), the WOZ reference.

#include "Iwm.h"

namespace {
// 6-and-2 write-translate table: 64 valid "disk bytes" (high bit set, no two
// consecutive zeros, etc.).
const uint8_t kGcr[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };
// DOS 3.3 physical→logical sector interleave (.dsk); ProDOS (.po) below.
const int kDos33[16] = {0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15};
const int kProDos[16] = {0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15};
}

Iwm::Iwm() { track_.resize(35); }

// Encode 256 data bytes into 342 6-and-2 nibbles + checksum, append to `out`.
static void encodeData(std::vector<uint8_t>& out, const uint8_t* data) {
    uint8_t nib[342] = {0};
    for (int i = 0; i < 256; ++i) nib[i + 86] = data[i] >> 2;      // top 6 bits
    for (int i = 0; i < 86; ++i) {
        uint8_t b = 0;
        b |= ((data[i] & 1) << 1) | ((data[i] & 2) >> 1);
        if (i + 86 < 256)  b |= (((data[i + 86] & 1) << 3) | ((data[i + 86] & 2) << 1));
        if (i + 172 < 256) b |= (((data[i + 172] & 1) << 5) | ((data[i + 172] & 2) << 3));
        nib[i] = b;
    }
    uint8_t last = 0;
    for (int i = 0; i < 342; ++i) { out.push_back(kGcr[(last ^ nib[i]) & 0x3F]); last = nib[i]; }
    out.push_back(kGcr[last & 0x3F]);   // checksum
}

static void push44(std::vector<uint8_t>& out, uint8_t v) {   // 4-and-4 encode
    out.push_back((v >> 1) | 0xAA);
    out.push_back(v | 0xAA);
}

void Iwm::nibblise(const std::vector<uint8_t>& img, bool prodosOrder) {
    const int* order = prodosOrder ? kProDos : kDos33;
    const uint8_t vol = 254;
    for (int t = 0; t < 35; ++t) {
        std::vector<uint8_t>& tk = track_[t];
        tk.clear();
        for (int i = 0; i < 48; ++i) tk.push_back(0xFF);           // track gap
        for (int ps = 0; ps < 16; ++ps) {
            int ls = order[ps];
            // Address field
            tk.push_back(0xD5); tk.push_back(0xAA); tk.push_back(0x96);
            push44(tk, vol); push44(tk, uint8_t(t)); push44(tk, uint8_t(ps));
            push44(tk, uint8_t(vol ^ t ^ ps));
            tk.push_back(0xDE); tk.push_back(0xAA); tk.push_back(0xEB);
            for (int i = 0; i < 6; ++i) tk.push_back(0xFF);        // gap 2
            // Data field
            tk.push_back(0xD5); tk.push_back(0xAA); tk.push_back(0xAD);
            encodeData(tk, &img[(t * 16 + ls) * 256]);
            tk.push_back(0xDE); tk.push_back(0xAA); tk.push_back(0xEB);
            for (int i = 0; i < 20; ++i) tk.push_back(0xFF);       // gap 3
        }
    }
    diskPresent_ = true;
}

bool Iwm::loadDisk525(const std::vector<uint8_t>& img, bool prodosOrder) {
    if (img.size() == 143360) { nibblise(img, prodosOrder); return true; }   // .dsk/.do/.po
    if (img.size() == 232960) {                                              // .nib
        for (int t = 0; t < 35; ++t)
            track_[t].assign(img.begin() + t * 6656, img.begin() + (t + 1) * 6656);
        diskPresent_ = true; return true;
    }
    return false;
}

void Iwm::stepPhase(int magnet, bool on) {
    phase_[magnet] = on ? 1 : 0;
    // Move the head toward an energised adjacent phase (quarter-track stepper).
    if (!on) return;
    int cur = halfTrack_ % 4;
    int delta = (magnet - cur + 4) % 4;
    if (delta == 1) halfTrack_++;
    else if (delta == 3) halfTrack_--;
    if (halfTrack_ < 0) halfTrack_ = 0;
    if (halfTrack_ > 68) halfTrack_ = 68;
}

void Iwm::advance(uint64_t cycle) {
    if (!motorOn_ || !diskPresent_) { lastCycle_ = cycle; return; }
    const auto& tk = track_[curTrack()];
    if (tk.empty()) { lastCycle_ = cycle; return; }
    uint64_t elapsed = cycle - lastCycle_;
    lastCycle_ = cycle;
    // ~4 CPU cycles per disk bit; a nibble is 8 bits → advance a nibble every
    // ~32 cycles. We step in whole nibbles for simplicity.
    size_t nibbles = size_t(elapsed / 32);
    if (nibbles) bitPos_ = (bitPos_ + nibbles) % tk.size();
}

uint8_t Iwm::access(uint8_t offset, bool isWrite, uint8_t writeVal, uint64_t cycle) {
    // $C0E0-$C0EF: even/odd pairs toggle a line low/high.
    switch (offset) {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x4: case 0x5: case 0x6: case 0x7:
            stepPhase(offset >> 1, offset & 1); break;
        case 0x8: motorOn_ = false; break;
        case 0x9: motorOn_ = true;  break;
        case 0xA: drive_ = 0; break;
        case 0xB: drive_ = 1; break;
        case 0xC: q6_ = false; break;
        case 0xD: q6_ = true;  break;
        case 0xE: q7_ = false; break;
        case 0xF: q7_ = true;  break;
    }
    advance(cycle);

    if (isWrite) {
        dataReg_ = writeVal;
        if (q7_ && q6_ && !motorOn_) mode_ = writeVal & 0x1F;   // write MODE register
        return 0;
    }

    // Read: the returned latch depends on Q7:Q6.
    if (!q7_ && !q6_) {                       // read data
        if (!motorOn_) return 0x00;
        if (!diskPresent_)                    // empty drive: spinning weak bits
            return uint8_t(0x80 | (cycle >> 5));
        const auto& tk = track_[curTrack()];
        if (tk.empty()) return 0x00;
        return tk[bitPos_ % tk.size()];
    }
    if (!q7_ && q6_) {                        // read status: low 5 = mode, hi = sense
        uint8_t st = mode_ & 0x1F;
        // bit7 = sense line (write-protect). An empty drive reads protected.
        if (writeProtect_ || !diskPresent_) st |= 0x80;
        if (motorOn_) st |= 0x20;             // bit5 = motor on
        return st;
    }
    if (q7_ && !q6_) return 0x80;             // read handshake (ready)
    return dataReg_;
}
