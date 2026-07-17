// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// 5.25" IWM gate (POM2 DiskImage bit-cell path). Four layers:
//  1. Read: mount a synthetic .dsk, hunt D5 AA 96 through the data latch
//     like RWTS (latch-paced polls), decode the 4-and-4 address field
//     (volume/track/sector/checksum) + epilogue on track 0.
//  2. Seek: step the phase magnets to track 2, re-read, verify the track
//     byte follows the head.
//  3. Write: hunt a target sector's address field, stream a freshly
//     encoded 6-and-2 data field (RWTS-style), close the session, flush —
//     the backing .dsk must hold the new 256 bytes and nothing else.
//  4. WOZ (soft): if a .woz exists in disks54/ or POM2's library, mount it
//     and verify address prologues stream out of the bit-cell path.
// Refs: POM2 DiskImage.h "LSS bit-cell stream", Beneath Apple DOS (GCR),
// MAME iwm.cpp; latch pacing discipline: DEV.md § Disk (3.5" write-up).

#include "IIgsMemory.h"
#include "CPU65816.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
const uint8_t kGcr[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };

// 6-and-2 encode of 256 bytes → 342 nibbles + checksum — the authentic
// RWTS layout as the boot PROM reconstructs it: low2 aux nibbles streamed
// in REVERSE order with bit-pair-swapped 2-bit groups, then high6, with a
// running XOR (POM2 DiskImage::writeDataField, validated against real DOS).
uint8_t rev2(uint8_t v) { return uint8_t(((v & 1) << 1) | ((v >> 1) & 1)); }
void encodeData(std::vector<uint8_t>& out, const uint8_t* src) {
    uint8_t high6[256], low2[86];
    for (int i = 0; i < 256; ++i) high6[i] = uint8_t(src[i] >> 2);
    for (int i = 0; i < 86; ++i) {
        uint8_t v = rev2(src[i] & 3);
        if (i + 86  < 256) v |= uint8_t(rev2(src[i + 86]  & 3) << 2);
        if (i + 172 < 256) v |= uint8_t(rev2(src[i + 172] & 3) << 4);
        low2[85 - i] = v;
    }
    uint8_t prev = 0;
    for (int j = 85; j >= 0; --j) { out.push_back(kGcr[(prev ^ low2[j]) & 0x3F]);  prev = low2[j]; }
    for (int i = 0; i < 256; ++i) { out.push_back(kGcr[(prev ^ high6[i]) & 0x3F]); prev = high6[i]; }
    out.push_back(kGcr[prev & 0x3F]);
}

uint8_t ioRead(IIgsMemory& mem, uint8_t off) {
    mem.tick(8);
    return mem.read8((uint32_t(0xE0) << 16) | (0xC0E0 + off));
}
void ioWrite(IIgsMemory& mem, uint8_t off, uint8_t v) {
    mem.tick(8);
    mem.write8((uint32_t(0xE0) << 16) | (0xC0E0 + off), v);
}
// Latch-paced data read: poll until MSB (one nibble per ~32 µs).
uint8_t readNib(IIgsMemory& mem) {
    ioWrite(mem, 0x0C, 0); ioWrite(mem, 0x0E, 0);   // Q6=0 Q7=0
    for (int i = 0; i < 64; ++i) {
        uint8_t v = ioRead(mem, 0x0C);
        if (v & 0x80) return v;
    }
    return 0;
}
int from44(uint8_t hi, uint8_t lo) { return ((hi << 1) | 1) & lo; }

// Step the head one half-track per adjacent phase pulse (RWTS SEEKABS).
void seekTrack(IIgsMemory& mem, int fromHalf, int toHalf) {
    int cur = fromHalf;
    while (cur != toHalf) {
        int next = cur + (toHalf > cur ? 1 : -1);
        int magnet = next & 3;
        ioWrite(mem, uint8_t(magnet * 2 + 1), 0);   // energise
        ioWrite(mem, uint8_t(magnet * 2), 0);       // release
        cur = next;
    }
}
}

int main() {
    int fails = 0;
    auto check = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };

    // Synthetic 143 360-byte .dsk, every sector tagged.
    std::vector<uint8_t> img(143360);
    for (size_t s = 0; s < img.size() / 256; ++s)
        for (int i = 0; i < 256; ++i)
            img[s * 256 + size_t(i)] = uint8_t(s * 17 + size_t(i) * 3);
    const char* path = "/tmp/pomiigs_iwm525_test.dsk";
    { std::ofstream o(path, std::ios::binary); o.write((const char*)img.data(), std::streamsize(img.size())); }

    IIgsMemory mem;
    CPU65816 cpu(&mem);
    mem.setCpu(&cpu);
    std::vector<uint8_t> rom(256 * 1024, 0xEA);
    mem.loadRom(rom);
    mem.reset();

    check("loadDisk525", mem.loadDisk525(path));

    ioWrite(mem, 0x09, 0);                          // ENABLE (motor) on
    ioWrite(mem, 0x0A, 0);                          // drive 0

    // ── 1. Address field on track 0 ──────────────────────────────────────
    auto huntAddr = [&](int wantTrack, int wantSector /* -1 = any */) -> int {
        for (int guard = 300000; guard-- > 0;) {
            if (readNib(mem) != 0xD5) continue;
            if (readNib(mem) != 0xAA) continue;
            if (readNib(mem) != 0x96) continue;
            uint8_t v0 = readNib(mem), v1 = readNib(mem);   // volume 4&4
            uint8_t t0 = readNib(mem), t1 = readNib(mem);   // track 4&4
            uint8_t s0 = readNib(mem), s1 = readNib(mem);   // sector 4&4
            uint8_t c0 = readNib(mem), c1 = readNib(mem);   // checksum 4&4
            const int vol = from44(v0, v1), trk = from44(t0, t1);
            const int sec = from44(s0, s1), ck = from44(c0, c1);
            if (ck != (vol ^ trk ^ sec)) continue;
            if (readNib(mem) != 0xDE) continue;
            if (readNib(mem) != 0xAA) continue;
            if (trk != wantTrack) continue;
            if (wantSector >= 0 && sec != wantSector) continue;
            return sec;
        }
        return -1;
    };
    check("track 0: valid address field", huntAddr(0, -1) >= 0);

    // ── 2. Seek to track 2 ───────────────────────────────────────────────
    seekTrack(mem, 0, 4);                            // 4 half-tracks = track 2
    check("track 2: address field follows the head", huntAddr(2, -1) >= 0);
    seekTrack(mem, 4, 0);                            // back to track 0

    // ── 3. RWTS-style sector write on track 0 ────────────────────────────
    {
        const int wrSec = huntAddr(0, -1);
        check("write: found target address field", wrSec >= 0);
        readNib(mem);   // consume the EB — the 5.25" address epilogue is DE AA EB;
                        // RWTS lets it pass before arming the write current.
        uint8_t newData[256];
        for (int i = 0; i < 256; ++i) newData[i] = uint8_t(0xC3 ^ (i * 29));

        std::vector<uint8_t> field;
        for (int i = 0; i < 5; ++i) field.push_back(0xFF);   // write sync
        field.push_back(0xD5); field.push_back(0xAA); field.push_back(0xAD);
        encodeData(field, newData);
        field.push_back(0xDE); field.push_back(0xAA); field.push_back(0xEB);

        ioRead(mem, 0x0D); ioRead(mem, 0x0F);               // Q6 on, Q7 on
        for (uint8_t nib : field) ioWrite(mem, 0x0D, nib);
        ioRead(mem, 0x0E); ioRead(mem, 0x0C);               // session closes on Q7 fall

        // Read the freshly written field back through the head.
        {
            int sec2 = huntAddr(0, wrSec);
            bool proOk = false;
            if (sec2 == wrSec) {
                uint8_t a = 0, b = 0, c = 0;
                for (int i = 0; i < 40; ++i) {          // skip the gap
                    a = b; b = c; c = readNib(mem);
                    if (a == 0xD5 && b == 0xAA && c == 0xAD) { proOk = true; break; }
                }
            }
            check("write: readback finds data prologue", proOk);
        }

        mem.iwm().flushDisk525();
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check("write: file size intact", back.size() == img.size());
        if (wrSec >= 0 && back.size() == img.size()) {
            // .dsk file offset for (track 0, PHYSICAL sector wrSec): DOS 3.3
            // physical→logical skew (Beneath Apple DOS).
            static const int kDos33[16] = {0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15};
            const size_t off = size_t(kDos33[wrSec]) * 256;
            check("write: sector holds new data", std::memcmp(back.data() + off, newData, 256) == 0);
            std::vector<uint8_t> expect = img;
            std::memcpy(expect.data() + off, newData, 256);
            check("write: rest of image untouched", back == expect);
        }
    }

    // ── 4. WOZ read path (soft — needs a real .woz nearby) ───────────────
    {
        std::string woz;
        for (const char* dir : {"disks54", "/home/gistarcade/src/POM2/disks_5.4/woz"}) {
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".woz") { woz = e.path().string(); break; }
            }
            if (!woz.empty()) break;
        }
        if (woz.empty()) {
            std::printf("iwm525_test: no .woz found — WOZ layer skipped\n");
        } else {
            check("woz: load", mem.loadDisk525(woz));
            seekTrack(mem, 0, 0);
            bool found = false;
            for (int guard = 400000; guard-- > 0;) {
                if (readNib(mem) != 0xD5) continue;
                uint8_t b = readNib(mem);
                if (b != 0xAA && b != 0x9D) continue;        // 96/AD std, 9D = 13-sector
                found = true; break;
            }
            check("woz: prologue bytes stream from bit cells", found);
        }
    }

    std::remove(path);
    if (fails) { std::printf("iwm525_test: %d FAILURES\n", fails); return 1; }
    std::printf("iwm525_test: all checks passed\n");
    return 0;
}
