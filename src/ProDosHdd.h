// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── ProDOS hard-disk card (synthetic block device) ───────────────────────
// A minimal ProDOS block device for raw .hdv/.po images, so the IIgs can boot
// a hard-disk volume (e.g. Total Replay). Self-contained "synthetic block"
// model (AppleWin HardDisk.cpp / POM2 ProDOSHardDiskCard lineage).
//
// The slot ROM ($Cn00) advertises the ProDOS signature ($Cn01=$20, $Cn03=$00,
// $Cn05=$03) and a block driver at $Cn50; block data streams through the
// slot's device-select window $C0(8+n)0..F:
//   $C0(8+n)0 write = block# low     $C0(8+n)1 write = block# high
//   $C0(8+n)2 read/write = next byte of the selected 512-byte block
//   $C0(8+n)3 read = status (bit7=no image, bit6=write-protected)
//   $C0(8+n)4/5 read = total block count (low/high) for STATUS
//
// Source of truth: Apple ProDOS 8 Technical Reference (block device protocol).

#ifndef POMIIGS_PRODOSHDD_H
#define POMIIGS_PRODOSHDD_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class ProDosHdd
{
public:
    static constexpr size_t kBlockBytes = 512;

    explicit ProDosHdd(int slot = 7) : slot_(slot) { buildRom(); }

    bool loadImage(const std::string& path);
    bool loaded() const { return !img_.empty(); }
    int  slot() const { return slot_; }
    size_t blockCount() const { return img_.size() / kBlockBytes; }

    // $Cn00-$CnFF slot ROM.
    uint8_t romRead(uint8_t off) const { return rom_[off]; }
    // $C0(8+n)0..F device-select window.
    uint8_t deviceRead(uint8_t low4);
    void    deviceWrite(uint8_t low4, uint8_t v);

private:
    int slot_;
    std::vector<uint8_t> img_;            // raw block image
    std::array<uint8_t, 256> rom_{};
    uint16_t selectedBlock_ = 0;
    uint32_t streamOffset_ = 0;
    bool writeProtect_ = false;

    void buildRom();
};

#endif // POMIIGS_PRODOSHDD_H
