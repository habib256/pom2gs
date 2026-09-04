// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Sony 3.5" 800K drive. See Sony35.h. GCR codec = exact port of KEGS iwm.c
// iwm_nibblize_track_35 (3125-3345) / iwm_denib_track35 (2409-2725); the
// /* 63xx / 62xx */ landmarks are the IIgs ROM nibblizer offsets KEGS kept.
// Register protocol: MAME floppy.cpp mac_floppy_device + KEGS iwm.c
// iwm_read_status35 / iwm_do_action35.

#include "Sony35.h"
#include "TwoImg.h"
#include <cstdio>
#include <fstream>

namespace {
// 6-and-2 write-translate table (same 64 disk bytes as the 5.25" GCR table;
// KEGS iwm.c to_disk_byte, iwm.c:58-70).
const uint8_t kGcr[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };

// Inverse: disk byte → 6-bit value, or 0x100+byte when invalid (KEGS
// g_from_disk_byte, iwm.c:180-188).
struct FromGcr {
    uint16_t t[256];
    FromGcr() {
        for (int i = 0; i < 256; ++i) t[i] = uint16_t(0x100 + i);
        for (int i = 0; i < 64; ++i) t[kGcr[i]] = uint16_t(i);
    }
};
const FromGcr kFromGcr;

// One GCR nibble ≈ 8 bits × 2 µs = 16 µs = ~229 master-clock (14.318 MHz)
// ticks (KEGS: fbit_mult=256 → 2 µs bit cells, iwm.c:29-33).
constexpr uint64_t kTicksPerNibble = 229;
}

// Image layout: tracks stored sequentially as cyl*2+side (side 0 then side 1
// per cylinder), each 512×sectorsOnTrack bytes — KEGS config.c:2046-2066.
size_t Sony35::imageOffset(int trkIdx) {
    size_t off = 0;
    for (int i = 0; i < trkIdx; ++i) off += size_t(sectorsOnTrack(i)) * 512;
    return off;
}

bool Sony35::loadImage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 2IMG envelope: offset/length/flags all come from the header, not from a
    // hard-coded 64 — see TwoImg.h and the matching note in ProDosHdd::loadImage
    // (a non-64 data offset loaded every sector shifted, and flush() then wrote
    // the 800K image back over the wrong part of the user's file).
    size_t hdr = 0;
    bool wp = false;
    // The Sony model is sector-backed (nibblised on load, de-nibblised on
    // flush): a WOZ bit stream is not decoded here. Refuse it explicitly —
    // treating the container as raw sectors produced garbage tracks AND
    // flush() then overwrote the user's .woz with raw sectors (found by the
    // sony_format gate, September 2026). Use .2mg/.po media on this path.
    if (img.size() >= 8 && img[0] == 'W' && img[1] == 'O' && img[2] == 'Z') {
        if (img[3] != '2') { std::fprintf(stderr, "Sony35: '%s': only WOZ2 3.5\" images are supported\n", path.c_str()); return false; }
        return loadWoz(img, path, wp);
    }
    wozBacked_ = false;
    const pom2::TwoImgPayload tw = pom2::twoImgProbe(img.data(), img.size());
    if (tw.is2img) {
        wp  = tw.writeProtected;
        hdr = tw.offset;
        img.erase(img.begin(), img.begin() + std::ptrdiff_t(tw.offset));
        img.resize(tw.length);
    }
    if (img.size() < kImageBytes) return false;      // 800K only (KEGS warns; we refuse)
    img.resize(kImageBytes);
    flush();                                          // don't lose a previous disk's writes
    image_ = std::move(img);
    path_ = path; headerBytes_ = hdr; writeProt_ = wp;
    for (int t = 0; t < kTracks; ++t) { nibbliseTrack(t); trkDirty_[t] = false; }
    present_ = true;
    switched_ = true;                                 // firmware re-detects media
    pos_ = 0; latchValid_ = false; nibbleClock_ = 0;
    return true;
}

bool Sony35::checkNibblization() {
    if (!present_) return false;
    std::vector<uint8_t> orig = image_;
    std::fill(image_.begin(), image_.end(), 0);
    bool ok = true;
    for (int t = 0; t < kTracks; ++t) ok = denibbliseTrack(t) && ok;
    ok = ok && image_ == orig;
    image_ = std::move(orig);
    return ok;
}

void Sony35::eject() {
    flush();
    present_ = false;
    switched_ = true;                                 // KEGS iwm_eject_disk sets just_ejected
    path_.clear(); image_.clear();
    for (int t = 0; t < kTracks; ++t) { trk_[t].clear(); trkDirty_[t] = false; }
}

// De-nibblise dirty tracks back to sectors and patch them into the backing
// file in place (past any .2mg header) — ProDosHdd::flushBlock pattern.
void Sony35::flush() {
    if (!present_) return;
    endWrite();
    bool any = false;
    for (int t = 0; t < kTracks; ++t) {
        if (!trkDirty_[t]) continue;
        if (denibbliseTrack(t)) any = true;
        else std::fprintf(stderr, "Sony35: bad nibble data on track %d.%d — sector(s) lost\n", t >> 1, t & 1);
        trkDirty_[t] = false;
    }
    if (!any || path_.empty() || writeProt_ || !writeBack_) return;
    if (wozBacked_) { if (!saveWoz()) std::fprintf(stderr, "Sony35: cannot write back WOZ %s\n", path_.c_str()); return; }
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) { std::fprintf(stderr, "Sony35: cannot write back to %s\n", path_.c_str()); return; }
    f.seekp(std::streamoff(headerBytes_), std::ios::beg);
    f.write(reinterpret_cast<const char*>(image_.data()), std::streamsize(image_.size()));
}

// ── WOZ2 3.5" images ─────────────────────────────────────────────────────
// Applesauce WOZ 2.x (https://applesaucefdc.com/woz/reference2/): INFO (disk
// type 2 = 3.5"), TMAP (160 entries, cyl*2+side → TRKS index), TRKS (8-byte
// entries: starting 512-byte block, block count, bit count; data at block*512).
// Reading: the IWM assembles a nibble by shifting bits in until bit 7 is set,
// so the two trailing zero bits of a 10-bit self-sync $FF are simply absorbed
// — exactly what bitsToNibbles does, which lets the KEGS-ported denibbliser
// decode the track. Untouched tracks keep their original bits (weak bits,
// timing quirks and all); a track the guest wrote is re-encoded from its
// nibbles with $FF as a 10-bit sync (GSSquared Floppy35_woz, Applesauce).
namespace {
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
inline void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
}

void Sony35::bitsToNibbles(const uint8_t* bits, uint32_t bitCount, std::vector<uint8_t>& nibbles) {
    nibbles.clear();
    nibbles.reserve(bitCount / 8);
    uint32_t v = 0;
    for (uint32_t i = 0; i < bitCount; ++i) {
        const uint32_t bit = (bits[i >> 3] >> (7 - (i & 7))) & 1;
        v = ((v << 1) | bit) & 0xFF;
        if (v & 0x80) { nibbles.push_back(uint8_t(v)); v = 0; }
    }
}

void Sony35::nibblesToBits(const std::vector<uint8_t>& nibbles, std::vector<uint8_t>& bits, uint32_t& bitCount) {
    bits.clear(); bitCount = 0;
    auto put = [&](uint32_t bit) { if ((bitCount & 7) == 0) bits.push_back(0); if (bit) bits[bitCount >> 3] |= uint8_t(0x80 >> (bitCount & 7)); ++bitCount; };
    for (uint8_t n : nibbles) {
        for (int b = 7; b >= 0; --b) put((n >> b) & 1);
        if (n == 0xFF) { put(0); put(0); }             // self-sync: 10-bit $FF
    }
}

bool Sony35::loadWoz(const std::vector<uint8_t>& file, const std::string& path, bool& wp) {
    if (file.size() < 12 + 8) return false;
    const uint8_t* info = nullptr; const uint8_t* tmap = nullptr; const uint8_t* trks = nullptr;
    size_t trksLen = 0; std::vector<uint8_t> meta;
    size_t p = 12;
    while (p + 8 <= file.size()) {
        const uint32_t id = rd32(&file[p]), len = rd32(&file[p + 4]);
        const uint8_t* body = &file[p + 8];
        if (size_t(len) > file.size() - (p + 8)) return false;
        if      (id == 0x4F464E49) info = body;                       // "INFO"
        else if (id == 0x50414D54) tmap = body;                       // "TMAP"
        else if (id == 0x534B5254) { trks = body; trksLen = len; }    // "TRKS"
        else if (id == 0x4154454D) meta.assign(body, body + len);     // "META"
        p += 8 + len;
    }
    if (!info || !tmap || !trks) return false;
    if (info[1] != 2) { std::fprintf(stderr, "Sony35: '%s' is not a 3.5\" WOZ (disk type %u)\n", path.c_str(), info[1]); return false; }
    flush();                                          // don't lose a previous disk's writes
    image_.assign(kImageBytes, 0);
    wozInfo_.assign(info, info + 60);
    wozMeta_ = std::move(meta);
    for (int t = 0; t < kTracks; ++t) {
        wozBits_[t].clear(); wozBitCount_[t] = 0; trk_[t].clear(); trkDirty_[t] = false;
        const uint8_t slot = tmap[t];
        if (slot == 0xFF || size_t(slot) * 8 + 8 > trksLen) continue;
        const uint8_t* e = trks + size_t(slot) * 8;
        const uint32_t startBlock = rd16(e), blocks = rd16(e + 2), bitCount = rd32(e + 4);
        const size_t off = size_t(startBlock) * 512;
        if (bitCount == 0 || off + size_t(blocks) * 512 > file.size() || (bitCount + 7) / 8 > size_t(blocks) * 512) continue;
        wozBits_[t].assign(file.begin() + std::ptrdiff_t(off), file.begin() + std::ptrdiff_t(off + (bitCount + 7) / 8));
        wozBitCount_[t] = bitCount;
        bitsToNibbles(wozBits_[t].data(), bitCount, trk_[t]);
        if (!trk_[t].empty()) denibbliseTrack(t);     // unformatted / protected tracks keep their raw nibbles
    }
    path_ = path; headerBytes_ = 0; writeProt_ = wp = (info[2] != 0);
    wozBacked_ = true;
    present_ = true;
    switched_ = true;
    pos_ = 0; latchValid_ = false; nibbleClock_ = 0;
    return true;
}

// Write a complete WOZ2 image: INFO (largest-track field refreshed), an
// identity TMAP over the tracks that exist, TRKS entries + block-aligned bit
// streams, META copied through. CRC32 left at 0 (allowed by the spec).
bool Sony35::saveAsWoz(const std::string& outPath) const {
    if (!present_) return false;
    std::vector<uint8_t> bits[kTracks]; uint32_t counts[kTracks];
    uint16_t largest = 0; std::vector<uint8_t> tmap(160, 0xFF);
    uint32_t next = 3;                                  // first data block after the 1536-byte header area
    std::vector<uint8_t> trks;
    for (int t = 0; t < kTracks; ++t) {
        if (trkDirty_[t] || (wozBitCount_[t] == 0 && !trk_[t].empty())) nibblesToBits(trk_[t], bits[t], counts[t]);
        else { bits[t] = wozBits_[t]; counts[t] = wozBitCount_[t]; }
        if (counts[t] == 0) { wr16(trks, 0); wr16(trks, 0); wr32(trks, 0); continue; }
        const uint16_t blocks = uint16_t((bits[t].size() + 511) / 512);
        tmap[size_t(t)] = uint8_t(t);
        wr16(trks, uint16_t(next)); wr16(trks, blocks); wr32(trks, counts[t]);
        if (blocks > largest) largest = blocks;
        next += blocks;
    }
    std::vector<uint8_t> out;
    const char magic[12] = {'W', 'O', 'Z', '2', char(0xFF), 0x0A, 0x0D, 0x0A, 0, 0, 0, 0};
    out.insert(out.end(), magic, magic + 12);
    std::vector<uint8_t> info = wozInfo_;
    if (info.empty()) { info.assign(60, 0); info[0] = 2; info[1] = 2; info[3] = 1; info[37] = 2; info[39] = 16; }   // v2, 3.5", synchronised, 2 sides, 2 µs bit cells
    info.resize(60, 0);
    info[44] = uint8_t(largest); info[45] = uint8_t(largest >> 8);
    info[2] = writeProt_ ? 1 : 0;
    out.push_back('I'); out.push_back('N'); out.push_back('F'); out.push_back('O'); wr32(out, 60); out.insert(out.end(), info.begin(), info.end());
    out.push_back('T'); out.push_back('M'); out.push_back('A'); out.push_back('P'); wr32(out, 160); out.insert(out.end(), tmap.begin(), tmap.end());
    out.push_back('T'); out.push_back('R'); out.push_back('K'); out.push_back('S');
    const uint32_t trksLen = 1280 + (next - 3) * 512;
    wr32(out, trksLen);
    out.insert(out.end(), trks.begin(), trks.end());
    out.resize(1536, 0);                                 // header + INFO + TMAP + 160 TRKS entries end at block 3
    for (int t = 0; t < kTracks; ++t) {
        if (counts[t] == 0) continue;
        const size_t blocks = (bits[t].size() + 511) / 512;
        out.insert(out.end(), bits[t].begin(), bits[t].end());
        out.resize(out.size() + (blocks * 512 - bits[t].size()), 0);
    }
    if (!wozMeta_.empty()) { out.push_back('M'); out.push_back('E'); out.push_back('T'); out.push_back('A'); wr32(out, uint32_t(wozMeta_.size())); out.insert(out.end(), wozMeta_.begin(), wozMeta_.end()); }
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

// ── Sony register protocol ────────────────────────────────────────────────
// Status table, idx = {CA2,CA1,CA0,SEL}. Sense polarity is mostly inverted
// (0 = true). MAME floppy.cpp:3217-3290 (wpt_r) ∕ KEGS iwm.c:912-1017,
// re-indexed; KEGS-only entries kept where MAME diverges (0xA: KEGS returns
// 1 for a ROM 03 probe — iwm.c:961-964 — where MAME's non-SuperDrive
// returns 0; KEGS is the IIgs-validated behaviour).
int Sony35::sense(int idx, uint64_t cycle) const {
    switch (idx & 0x0F) {
        case 0x0: return stepDir_ ? 1 : 0;            // DIRTN: 1 = outward (toward cyl 0)
        case 0x1: return present_ ? 0 : 1;            // CSTIN: 0 = disk in place
        case 0x2: return 1;                           // STEP: 1 = step complete
        case 0x3: return writeProt_ ? 0 : 1;          // WRTPRT: 0 = protected
        case 0x4: return motorOn_ ? 0 : 1;            // MOTORON: 0 = spinning
        case 0x5: return cyl_ != 0 ? 1 : 0;           // TK0: 0 = at cylinder 0
        case 0x6: return switched_ ? 1 : 0;           // SWITCHED: 1 = media changed
        case 0x7: {                                   // TACH: 120 inversions/rev at the
            // zone's rpm (MAME floppy.cpp:3374-3389). Inversions/s = 2·rpm →
            // toggle every 14 318 180 / (2·rpm) master ticks. The ROM's
            // FORMAT counts these pulses to verify per-zone drive speed.
            if (!present_ || !motorOn_) return 0;
            static const uint32_t kRpm[5] = {394, 429, 472, 525, 590};
            const uint64_t half = 7159090ull / kRpm[(cyl_ >> 4) & 7];
            return int((cycle / half) & 1);
        }
        case 0x8:                                     // RDDATA0 (instantaneous head data;
        case 0x9:                                     // RDDATA1). Must TOGGLE while the
            // platter spins — the ROM polls it for flux activity between
            // field reads and hangs on a constant line. GCR flux ≈ 1 µs
            // scale → (cycle>>4) ≈ 1.1 µs at 14.318 MHz.
            return (present_ && motorOn_) ? int((cycle >> 4) & 1) : 0;
        case 0xA: return 1;                           // ROM 03 probe (KEGS iwm.c:961-964)
        case 0xB: return 0;                           // MFM mode: 0 = GCR
        case 0xC: return 1;                           // SIDES: 1 = double-sided
        case 0xD: return motorOn_ ? 0 : 1;            // READY: 0 = ready (KEGS: = motor on)
        case 0xE: return 1;                           // eject-in-progress false read (KEGS)
        case 0xF: return 0;                           // DRVIN: 0 = drive installed
    }
    return 1;
}

// Command table, idx = {CA2,CA1,CA0,SEL}, fired on LSTRB rising edge.
// MAME floppy.cpp:3292-3368 (seek_phase_w) ∕ KEGS iwm.c:1019-1091.
void Sony35::command(int idx) {
    switch (idx & 0x0F) {
        case 0x0: stepDir_ = false; break;            // direction inward (+1 cyl)
        case 0x8: stepDir_ = true;  break;            // direction outward (−1 cyl)
        case 0x2: {                                   // STEP one cylinder
            endWrite();                               // close the session; dirty tracks
                                                      // stay in memory until motor-off
            cyl_ += stepDir_ ? -1 : 1;
            if (cyl_ < 0) cyl_ = 0;
            if (cyl_ > kCyls - 1) cyl_ = kCyls - 1;
            break;
        }
        case 0x4: motorOn_ = true;  break;            // spindle on
        case 0xC: motorOn_ = false; flush(); break;   // spindle off → commit writes
        case 0xE:                                     // eject
            eject();
            break;
        case 0x9: switched_ = false; break;           // clear disk-switched flag
        default: break;
    }
}

uint8_t Sony35::readNibble(int side, uint64_t cycle) {
    endWrite();                           // a data read closes any write session
    if (!present_) return 0x00;           // empty drive: latch never assembles
    const auto& tk = trk_[curTrk(side)];
    if (!motorOn_ || tk.empty()) return 0xFF;   // no rotation → floating bus
    // Latch pacing, elastic delivery (KEGS g_fast_disk_emul discipline): a
    // new nibble is available at most once per ~16 µs — polls in between
    // read $00 so the ROM's iteration-budgeted hunt loops behave — but the
    // stream advances exactly ONE nibble per delivery, so a poller slower
    // than the real rate loses nothing (a strict rotational model dropped
    // nibbles whenever an iteration exceeded one nibble time and the ROM's
    // field decodes never lined up).
    if (cycle - nibbleClock_ >= kTicksPerNibble) {
        pos_ = (pos_ + 1) % tk.size();
        latch_ = tk[pos_];
        latchValid_ = true;
        nibbleClock_ = cycle;
    }
    if (latchValid_) { latchValid_ = false; return latch_; }
    return 0x00;                          // byte not assembled yet (MSB clear)
}

void Sony35::writeNibble(int side, uint8_t v) {
    if (!present_ || writeProt_ || !motorOn_) return;
    auto& tk = trk_[curTrk(side)];
    if (tk.empty()) return;
    // One nibble per data write (the async-handshake path always reports
    // ready, so the ROM streams bytes back-to-back and they land
    // sequentially right after the address field it just read). The bytes
    // collect in a session buffer applied by endWrite().
    if (!wrActive_) {
        wrActive_ = true;
        wrTrk_ = curTrk(side);
        wrStart_ = (pos_ + 1) % tk.size();
        wrBuf_.clear();
    }
    wrBuf_.push_back(v);
    pos_ = (pos_ + 1) % tk.size();
    latchValid_ = false;                  // reading resumes from live rotation
}

void Sony35::endWrite() {
    if (!wrActive_) return;
    wrActive_ = false;
    auto& tk = trk_[wrTrk_];
    if (tk.empty() || wrBuf_.empty()) { wrBuf_.clear(); return; }
    if (wrBuf_.size() * 2 >= tk.size()) {
        // FORMAT-class session: the written stream IS the new track (its
        // nibble count is the ROM's own layout, not ours).
        tk = wrBuf_;
        pos_ %= tk.size();
    } else {
        for (size_t i = 0; i < wrBuf_.size(); ++i)
            tk[(wrStart_ + i) % tk.size()] = wrBuf_[i];
    }
    trkDirty_[wrTrk_] = true;
    wrBuf_.clear();
}

// ── GCR codec ─────────────────────────────────────────────────────────────
// Exact port of KEGS iwm_nibblize_track_35 (iwm.c:3125-3345). 10-bit sync
// $FF nibbles are stored as plain bytes.
void Sony35::nibbliseTrack(int trkIdx) {
    const int numSectors = sectorsOnTrack(trkIdx);
    const int track = trkIdx >> 1, side = trkIdx & 1;
    auto& tk = trk_[trkIdx];
    tk.clear();
    tk.reserve(size_t(numSectors) * 850 + 400);
    const uint8_t* trackBuf = image_.data() + imageOffset(trkIdx);

    // Physical→logical interleave 2 (KEGS iwm.c:3148-3162).
    int physToLog[16];
    for (int i = 0; i < numSectors; ++i) physToLog[i] = -1;
    int physSec = 0;
    for (int logSec = 0; logSec < numSectors; ++logSec) {
        while (physToLog[physSec] >= 0) { if (++physSec >= numSectors) physSec = 0; }
        physToLog[physSec] = logSec;
        physSec += 2;
        if (physSec >= numSectors) physSec -= numSectors;
    }

    uint32_t bufC[0x100], bufD[0x100], bufE[0x100];
    for (physSec = 0; physSec < numSectors; ++physSec) {
        const int logSec = physToLog[physSec];

        // Sync leader. KEGS formats 400 nibbles before sector 0 (the ROM's
        // own track layout), 54 between sectors. With our elastic delivery a
        // hunt can start AT the leader head (worst case: the whole run), and
        // the ROM's address-hunt budget (~420 nibble reads, measured) expires
        // inside a 400-FF run — so keep the lead-in generous but shorter.
        for (int i = 0, n = physSec == 0 ? 100 : 54; i < n; ++i) tk.push_back(0xFF);

        // Address field: D5 AA 96, track[5:0], sector, side<<5|track>>6,
        // format $22 (800K double-sided), XOR checksum, DE AA.
        const uint32_t physTrack = uint32_t(track & 0x3F);
        const uint32_t physSide  = uint32_t((side << 5) + (track >> 6));
        const uint32_t capacity  = 0x22;
        tk.push_back(0xD5); tk.push_back(0xAA); tk.push_back(0x96);
        tk.push_back(kGcr[physTrack]);
        tk.push_back(kGcr[logSec]);
        tk.push_back(kGcr[physSide]);
        tk.push_back(kGcr[capacity]);
        tk.push_back(kGcr[(physTrack ^ uint32_t(logSec) ^ physSide ^ capacity) & 0x3F]);
        tk.push_back(0xDE); tk.push_back(0xAA);

        // Inter-field sync + data prologue.
        for (int i = 0; i < 5; ++i) tk.push_back(0xFF);
        tk.push_back(0xD5); tk.push_back(0xAA); tk.push_back(0xAD);
        tk.push_back(kGcr[logSec]);

        // 6-and-2 encode with the 3-byte rolling carry checksum — ROM
        // nibblizer via KEGS (/* 6320..6440 */). The 12 tag bytes are the
        // zeroed top 4 entries of each buffer.
        const uint8_t* buf = trackBuf + (size_t(logSec) << 9);
        uint32_t t5c = 0, t5d = 0, t5e = 0, val;
        int carry, x = 0xAF, y;
        bufC[0] = bufD[0] = bufE[0] = bufE[1] = 0;
        for (y = 4; y > 0; --y) { bufC[x] = bufD[x] = bufE[x] = 0; --x; }
        y = 0;
        while (x >= 0) {
            /* 6338 */
            t5c <<= 1; carry = int(t5c >> 8); t5c = (t5c + uint32_t(carry)) & 0xFF;
            val = buf[y]; t5e = val + t5e + uint32_t(carry); carry = int(t5e >> 8); t5e &= 0xFF;
            bufC[x] = val ^ t5c; ++y;
            /* 634c */
            val = buf[y]; t5d = t5d + val + uint32_t(carry); carry = int(t5d >> 8); t5d &= 0xFF;
            bufD[x] = val ^ t5e; ++y;
            --x;
            if (x <= 0) break;
            /* 632a */
            val = buf[y]; t5c = t5c + val + uint32_t(carry); carry = int(t5c >> 8); t5c &= 0xFF;
            bufE[x + 1] = val ^ t5d; ++y;
        }
        /* 635f-6373: pack the checksums' top 2-bit groups */
        val = ((t5c >> 2) ^ t5d) & 0x3F;
        val = (val ^ t5d) >> 2;
        val = (val ^ t5e) & 0x3F;
        val = (val ^ t5e) >> 2;
        const uint32_t t5f = val;
        /* 6375-6429: pipelined emit — 2-bit-group nibble + 3 data nibbles */
        uint32_t t63 = 0, t64 = 0, t65 = 0, accHi = 0;
        y = 0xAE;
        while (y >= 0) {
            tk.push_back(kGcr[accHi & 0x3F]);
            val = t63 & 0x3F; t63 = bufC[y]; accHi = t63 >> 6;                tk.push_back(kGcr[val]);
            val = t64 & 0x3F; t64 = bufD[y]; accHi = (accHi << 2) + (t64 >> 6); tk.push_back(kGcr[val]);
            --y;
            if (y < 0) break;
            val = t65 & 0x3F; t65 = bufE[y + 1]; accHi = (accHi << 2) + (t65 >> 6); tk.push_back(kGcr[val]);
        }
        tk.push_back(kGcr[t5f & 0x3F]);
        tk.push_back(kGcr[t5e & 0x3F]);
        tk.push_back(kGcr[t5d & 0x3F]);
        tk.push_back(kGcr[t5c & 0x3F]);
        /* 6440: data epilogue */
        tk.push_back(0xDE); tk.push_back(0xAA); tk.push_back(0xFF);
    }
}

// Exact port of KEGS iwm_denib_track35 (iwm.c:2409-2725): scan the nibble
// stream for address fields, decode each data field, verify all checksums,
// and place sectors back into image_. Returns false unless every sector on
// the track decoded cleanly.
bool Sony35::denibbliseTrack(int trkIdx) {
    const auto& tk = trk_[trkIdx];
    if (tk.empty()) return false;
    const int numSectors = sectorsOnTrack(trkIdx);
    const int track = trkIdx >> 1, side = trkIdx & 1;
    uint8_t* outbuf = image_.data() + imageOffset(trkIdx);

    bool sectorDone[16] = {false};
    int sectorsRemaining = numSectors;
    uint32_t bufC[0x100], bufD[0x100], bufE[0x100];

    size_t p = 0;
    auto rd = [&]() -> uint32_t { uint32_t v = tk[p % tk.size()]; ++p; return v; };

    uint32_t val = 0;
    size_t limit = 2 * tk.size();
    while (limit-- > 0 && sectorsRemaining > 0) {
        // Address prologue.
        if (val != 0xD5) { val = rd(); continue; }
        val = rd(); if (val != 0xAA) continue;
        val = rd(); if (val != 0x96) continue;

        const uint32_t physTrack = kFromGcr.t[rd()];
        if (physTrack != uint32_t(track & 0x3F)) return false;
        const uint32_t sec = kFromGcr.t[rd()];
        if (sec >= uint32_t(numSectors)) return false;
        const uint32_t physSide = kFromGcr.t[rd()];
        if (physSide != uint32_t((side << 5) + (track >> 6))) return false;
        const uint32_t capacity = kFromGcr.t[rd()];   // $22/$24 both seen in the wild
        if (capacity != 0x22 && capacity != 0x24) return false;
        const uint32_t cksum = kFromGcr.t[rd()];
        if (cksum != ((physTrack ^ sec ^ physSide ^ capacity) & 0x3F)) return false;
        if (sectorDone[sec]) { val = 0; continue; }   // second lap over a done sector

        // Data prologue within the gap (KEGS allows 38 nibbles).
        val = 0;
        for (int i = 0; i < 38 && val != 0xD5; ++i) val = rd();
        if (val != 0xD5) return false;
        if (rd() != 0xAA) return false;
        if (rd() != 0xAD) return false;
        if (kFromGcr.t[rd()] != sec) return false;

        // 6-and-2 decode with rolling checksums (/* 626f..62d0 */).
        uint32_t t5c = 0, t5d = 0, t5e = 0, t66, v2;
        int carry = 0, y = 0xAF;
        bool bad = false;
        while (y > 0) {
            t66 = kFromGcr.t[rd()];
            if (t66 >= 0x100) { bad = true; break; }
            t5c <<= 1; carry = int(t5c >> 8); t5c = (t5c + uint32_t(carry)) & 0xFF;

            v2 = kFromGcr.t[rd()];
            if (v2 >= 0x100) { bad = true; break; }
            v2 = (v2 + ((t66 << 2) & 0xC0)) ^ t5c;
            bufC[y] = v2;
            t5e = v2 + t5e + uint32_t(carry); carry = int(t5e >> 8); t5e &= 0xFF;
            /* 62b8 */
            v2 = kFromGcr.t[rd()];
            if (v2 >= 0x100) { bad = true; break; }
            v2 = (v2 + ((t66 << 4) & 0xC0)) ^ t5e;
            bufD[y] = v2;
            t5d = v2 + t5d + uint32_t(carry); carry = int(t5d >> 8); t5d &= 0xFF;
            --y;
            if (y <= 0) break;
            /* 6274 */
            v2 = kFromGcr.t[rd()];
            if (v2 >= 0x100) { bad = true; break; }
            v2 = (v2 + ((t66 << 6) & 0xC0)) ^ t5d;
            bufE[y + 1] = v2;
            t5c = v2 + t5c + uint32_t(carry); carry = int(t5c >> 8); t5c &= 0xFF;
        }
        if (bad) return false;

        /* 62d0: checksum nibbles (2-bit-group prefix then 5e, 5d, 5c) */
        v2 = kFromGcr.t[rd()];
        t66 = (v2 << 6) & 0xC0;
        const uint32_t t67 = (v2 << 4) & 0xC0;
        v2 = (v2 << 2) & 0xC0;
        if (t5e != kFromGcr.t[rd()] + v2) return false;
        if (t5d != kFromGcr.t[rd()] + t67) return false;
        if (t5c != kFromGcr.t[rd()] + t66) return false;
        if (rd() != 0xDE) return false;
        if (rd() != 0xAA) return false;

        /* 6459: interleaved buffers → 512 output bytes */
        uint8_t* out = outbuf + (size_t(sec) << 9);
        int n = 0;
        for (int x = 0xAB; x >= 0 && n < 0x200; --x) {
            *out++ = uint8_t(bufC[x]); if (++n >= 0x200) break;
            *out++ = uint8_t(bufD[x]); if (++n >= 0x200) break;
            *out++ = uint8_t(bufE[x]); ++n;
        }
        sectorDone[sec] = true;
        --sectorsRemaining;
        val = 0;
    }
    return sectorsRemaining == 0;
}
