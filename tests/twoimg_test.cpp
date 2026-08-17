// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// 2IMG envelope gate for the BLOCK loaders (ProDosHdd slot-7 HDD / slot-5
// SmartPort 3.5", Sony35 800K). Both used to assume "2IMG magic ⇒ payload at
// byte 64, running to EOF" and tested only flags bit 31 for the lock. The
// header says otherwise (spec: DiskImage_2MG_Info.txt):
//   * bytes 24-27 dataOffset — need NOT be 64. Skipping a fixed 64 loads every
//     block shifted, and the write-back path (flushBlock / flush) then patches
//     the backing FILE at the same wrong offset, corrupting the user's image.
//   * bytes 28-31 dataLength — bounds the payload. A trailing comment/creator
//     chunk otherwise reads back as extra phantom blocks.
//   * bytes 16-19 flags — the lock rule lives in TwoImg.h (bit 31, plus the
//     lenient bit-0 signal when no volume number is declared). DiskImage has
//     always used it; these two hand-rolled a bit-31-only test, so the same
//     .2mg was write-protected as a 5.25" image and writable as a HDD/3.5".
//
// Every case here is built so the LEGACY behaviour (fixed 64 / EOF / bit 31)
// gives a visibly wrong answer.

#include "ProDosHdd.h"
#include "Sony35.h"
#include "TwoImg.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& v, size_t o, uint16_t x) {
    v[o] = uint8_t(x); v[o + 1] = uint8_t(x >> 8);
}
void put32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[o + size_t(i)] = uint8_t(x >> (8 * i));
}

// Build a 2IMG file: `hdrLen`-byte header, `padBeforeData` filler bytes, the
// payload, then `trailer` filler bytes. dataOffset/dataLength describe reality.
std::vector<uint8_t> make2mg(const std::vector<uint8_t>& payload, uint16_t hdrLen,
                             size_t padBeforeData, size_t trailer, uint32_t flags) {
    std::vector<uint8_t> f(hdrLen, 0);
    std::memcpy(f.data(), "2IMGPOM2", 8);
    put16(f, 8, hdrLen);
    put16(f, 10, 1);                                     // version
    put32(f, 12, 1);                                     // ProDOS order
    put32(f, 16, flags);
    put32(f, 20, uint32_t(payload.size() / 512));        // block count
    put32(f, 24, uint32_t(hdrLen + padBeforeData));      // dataOffset
    put32(f, 28, uint32_t(payload.size()));              // dataLength
    f.insert(f.end(), padBeforeData, 0xEE);              // comment chunk BEFORE the data
    f.insert(f.end(), payload.begin(), payload.end());
    f.insert(f.end(), trailer, 0xCC);                    // comment chunk AFTER the data
    return f;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    return bool(o);
}

// Block b is tagged {0x01 if b==0 else 0xAA, b&0xFF, b>>8, ...}.
std::vector<uint8_t> makePayload(size_t blocks) {
    std::vector<uint8_t> p(blocks * 512, 0);
    for (size_t b = 0; b < blocks; ++b) {
        p[b * 512 + 0] = uint8_t(b == 0 ? 0x01 : 0xAA);
        p[b * 512 + 1] = uint8_t(b);
        p[b * 512 + 2] = uint8_t(b >> 8);
    }
    return p;
}

int fails = 0;
void check(const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } }

}  // namespace

int main() {
    const std::string dir = "/tmp/";

    // ── 1. Pure header parse (TwoImg.h) ──────────────────────────────────
    {
        const auto payload = makePayload(4);
        const auto file    = make2mg(payload, 64, 64, 300, 0);
        const auto tw = pom2::twoImgProbe(file.data(), file.size());
        check("probe: recognised", tw.is2img);
        check("probe: honours dataOffset", tw.offset == 128);
        check("probe: honours dataLength", tw.length == payload.size());
        check("probe: not locked", !tw.writeProtected);
    }
    {   // A header whose offset/length fields are unusable must fall back to
        // the legacy fixed-64 / run-to-EOF behaviour, never refuse the image.
        auto file = make2mg(makePayload(4), 64, 0, 0, 0);
        put32(file, 24, 0);            // dataOffset = 0  (nonsense)
        put32(file, 28, 0);            // dataLength = 0  (some writers leave it)
        const auto tw = pom2::twoImgProbe(file.data(), file.size());
        check("probe: bogus fields fall back to 64/EOF",
              tw.is2img && tw.offset == 64 && tw.length == file.size() - 64);
    }
    {   // Non-2IMG bytes are passed through untouched.
        std::vector<uint8_t> raw(1024, 0x5A);
        const auto tw = pom2::twoImgProbe(raw.data(), raw.size());
        check("probe: raw image untouched", !tw.is2img && tw.offset == 0);
    }

    // ── 2. ProDosHdd — offset, length and the shared lock rule ───────────
    {
        const auto payload = makePayload(8);
        // dataOffset = 128 (a 64-byte comment chunk sits before the data) and a
        // 1 KB trailing chunk. Legacy: block 0 reads the $EE filler and the
        // trailer shows up as 2 extra blocks.
        const std::string p = dir + "pomiigs_2mg_offset.2mg";
        check("hdd: write fixture", writeFile(p, make2mg(payload, 64, 64, 1024, 0)));

        ProDosHdd hdd(7, false);
        check("hdd: loads", hdd.loadImage(p));
        check("hdd: block count excludes the trailing chunk", hdd.blockCount() == 8);
        uint8_t blk[512] = {0};
        check("hdd: block 0 readable", hdd.readBlock(0, blk));
        check("hdd: block 0 is the payload, not the pre-data chunk",
              blk[0] == 0x01 && blk[1] == 0x00);
        check("hdd: bootable() sees the ProDOS boot block", hdd.bootable());
        check("hdd: block 7 lands at the right offset",
              hdd.readBlock(7, blk) && blk[0] == 0xAA && blk[1] == 7);

        // Write-back must patch the FILE at dataOffset + block*512, not 64 + …
        uint8_t out[512];
        std::memset(out, 0x77, sizeof out);
        check("hdd: writeBlock", hdd.writeBlock(3, out));
        std::ifstream in(p, std::ios::binary);
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        check("hdd: write-back landed at dataOffset + 3*512",
              back.size() > 128 + 4 * 512 && back[128 + 3 * 512] == 0x77 &&
              back[128 + 3 * 512 + 511] == 0x77);
        check("hdd: write-back did not touch the pre-data chunk", back[64] == 0xEE);
        std::remove(p.c_str());
    }
    {
        // flags bit 0 set with bit 8 clear = locked, per TwoImg.h's lenient rule
        // (DiskImage has always read it this way).
        const std::string p = dir + "pomiigs_2mg_locked.2mg";
        check("hdd-wp: write fixture", writeFile(p, make2mg(makePayload(4), 64, 0, 0, 1)));
        ProDosHdd hdd(7, false);
        check("hdd-wp: loads", hdd.loadImage(p));
        check("hdd-wp: locked flag honoured (matches DiskImage)", hdd.writeProtected());
        uint8_t out[512] = {0};
        check("hdd-wp: writeBlock refused", !hdd.writeBlock(1, out));
        std::remove(p.c_str());
    }

    // ── 3. Sony35 — same envelope rules on the 800K path ─────────────────
    // 52 is the shortest header the spec defines, and DiskImage accepts it. A
    // 52-byte-header .2mg holding a full 800K payload has only 819188 bytes past
    // a hard-coded byte 64, so the legacy loader declared it "not 800K" and
    // REFUSED to mount an image its 5.25" sibling loads happily.
    {
        const auto payload = makePayload(Sony35::kImageBytes / 512);
        const std::string p = dir + "pomiigs_2mg_sony.2mg";
        check("sony: write fixture", writeFile(p, make2mg(payload, 52, 0, 0, 0)));

        Sony35 drv;
        check("sony: mounts a 52-byte-header 2IMG", drv.loadImage(p));
        check("sony: GCR round-trip over the loaded image", drv.checkNibblization());

        // Dirty a track without changing its content: park inside a sync gap
        // (a run of $FF) and rewrite $FF over $FF, so the track still decodes.
        // flush() then patches the file at dataOffset — the header must survive.
        drv.command(0x4);                       // spindle on
        uint64_t cyc = 0;
        int ffRun = 0;
        for (int i = 0; i < 40000 && ffRun < 8; ++i) {
            cyc += 256;                         // > one nibble time (~229 master)
            ffRun = (drv.readNibble(0, cyc) == 0xFF) ? ffRun + 1 : 0;
        }
        check("sony: found a sync gap to write into", ffRun >= 8);
        for (int i = 0; i < 4; ++i) drv.writeNibble(0, 0xFF);
        drv.endWrite();
        drv.flush();

        std::ifstream in(p, std::ios::binary);
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        check("sony: flush kept the 2IMG header intact", back.size() >= 52 &&
              back[0] == '2' && back[1] == 'I' && back[2] == 'M' && back[3] == 'G');
        check("sony: flush wrote the payload back at dataOffset",
              back.size() >= 52 + Sony35::kImageBytes &&
              back[52] == 0x01 && back[52 + 512] == 0xAA && back[52 + 513] == 1);
        std::remove(p.c_str());
    }
    {
        const std::string p = dir + "pomiigs_2mg_sony_wp.2mg";
        check("sony-wp: write fixture",
              writeFile(p, make2mg(makePayload(Sony35::kImageBytes / 512), 64, 0, 0, 1)));
        Sony35 drv;
        check("sony-wp: loads", drv.loadImage(p));
        check("sony-wp: locked flag honoured (matches DiskImage)", drv.writeProtected());
        std::remove(p.c_str());
    }

    if (fails) { std::printf("twoimg_test: %d FAILURES\n", fails); return 1; }
    std::printf("OK: 2IMG dataOffset/dataLength/lock honoured by the block loaders\n");
    return 0;
}
