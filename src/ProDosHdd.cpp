// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ProDOS hard-disk card. See ProDosHdd.h. Firmware ported from POM2's
// ProDOSHardDiskCard (AppleWin lineage). Source: Apple ProDOS 8 Tech Ref.

#include "ProDosHdd.h"
#include <algorithm>
#include <fstream>

bool ProDosHdd::loadImage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    img_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // .2mg images carry a 64-byte header; a raw .hdv/.po is a multiple of 512.
    if (img_.size() >= 64 && img_[0] == '2' && img_[1] == 'I' && img_[2] == 'M' && img_[3] == 'G')
        img_.erase(img_.begin(), img_.begin() + 64);
    // Round down to a whole number of blocks.
    img_.resize((img_.size() / kBlockBytes) * kBlockBytes);
    selectedBlock_ = 0; streamOffset_ = 0;
    return !img_.empty();
}

bool ProDosHdd::readBlock(uint32_t blk, uint8_t* out) const {
    size_t off = size_t(blk) * kBlockBytes;
    if (img_.empty() || off + kBlockBytes > img_.size()) return false;
    std::copy(img_.begin() + off, img_.begin() + off + kBlockBytes, out);
    return true;
}

bool ProDosHdd::writeBlock(uint32_t blk, const uint8_t* in) {
    if (writeProtect_) return false;
    size_t off = size_t(blk) * kBlockBytes;
    if (img_.empty() || off + kBlockBytes > img_.size()) return false;
    std::copy(in, in + kBlockBytes, img_.begin() + off);
    return true;
}

uint8_t ProDosHdd::deviceRead(uint8_t low4) {
    switch (low4) {
        case 0x2: {                                 // next data byte of selected block
            if (img_.empty()) return 0xFF;
            size_t a = size_t(selectedBlock_) * kBlockBytes + streamOffset_;
            uint8_t v = (a < img_.size()) ? img_[a] : 0xFF;
            streamOffset_ = (streamOffset_ + 1) % kBlockBytes;
            return v;
        }
        case 0x3: {                                 // status
            uint8_t s = img_.empty() ? 0x80 : 0x00; // bit7 = no image
            if (writeProtect_) s |= 0x40;           // bit6 = write-protected
            return s;
        }
        case 0x4: case 0x5: {                        // block count low/high (clamped)
            size_t n = img_.empty() ? 0 : blockCount();
            if (n > 0xFFFF) n = 0xFFFF;
            return uint8_t((n >> (low4 == 0x5 ? 8 : 0)) & 0xFF);
        }
    }
    return 0xFF;
}

void ProDosHdd::deviceWrite(uint8_t low4, uint8_t v) {
    if (low4 == 0x0)      { selectedBlock_ = uint16_t((selectedBlock_ & 0xFF00) | v);          streamOffset_ = 0; }
    else if (low4 == 0x1) { selectedBlock_ = uint16_t((selectedBlock_ & 0x00FF) | (v << 8));   streamOffset_ = 0; }
    else if (low4 == 0x2) {                          // write byte into selected block
        if (img_.empty() || writeProtect_) return;
        size_t a = size_t(selectedBlock_) * kBlockBytes + streamOffset_;
        if (a < img_.size()) img_[a] = v;
        streamOffset_ = (streamOffset_ + 1) % kBlockBytes;
    }
}

// Emit a byte sequence at `pc`, advancing it.
static void emit(std::array<uint8_t, 256>& rom, uint8_t& pc, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) rom[pc++] = b;
}

void ProDosHdd::buildRom() {
    rom_.fill(0xEA);
    const uint8_t hi   = uint8_t(0xC0 + slot_);          // $Cn.. high byte
    const uint8_t dev  = uint8_t(0x80 + slot_ * 16);     // device base $C0(8+n)0 low byte
    const uint8_t unit = uint8_t(slot_ << 4);            // ProDOS unit (slot n, drive 1)
    const uint8_t kBoot = 0x20, kDrv = 0x50;

    // ── SmartPort variant: boot + ProDOS-block + SmartPort dispatch entries
    // are WDM traps ($C6 = ProDOS block, $C5 = SmartPort) handled in C++.
    if (smartport_) {
        rom_[0x00] = 0x4C; rom_[0x01] = kBoot; rom_[0x02] = hi;    // JMP $Cn20 (boot)
        rom_[0x03] = 0x00; rom_[0x05] = 0x03; rom_[0x07] = 0x00;   // SmartPort ($Cn07=$00)
        rom_[0xFB] = 0x02;                                         // extended SmartPort supported
        rom_[0xFE] = 0xBF;                                         // status: read/write/status/format
        rom_[0xFF] = kDrv;                                         // ProDOS entry $Cn50
        // Boot: read block 0 to $0800 via the ProDOS-block trap, then JMP $0801.
        uint8_t pc = kBoot;
        emit(rom_, pc, {
            0xA9,0x01, 0x85,0x42, 0xA9,unit, 0x85,0x43,        // cmd=read, unit
            0xA9,0x00, 0x85,0x44, 0xA9,0x08, 0x85,0x45,        // buffer = $0800
            0xA9,0x00, 0x85,0x46, 0x85,0x47,                   // block = 0
            0x20,kDrv,hi, 0xB0,0x07,                           // JSR $Cn50 / BCS err
            0xA2,unit, 0xA9,0x00, 0x4C,0x01,0x08,              // LDX unit/LDA #0/JMP $0801
            0x4C,0xE0,hi });                                   // err: JMP $CnE0
        rom_[0xE0] = 0x4C; rom_[0xE1] = 0xE0; rom_[0xE2] = hi; // $CnE0 halt loop
        rom_[0x50] = 0x42; rom_[0x51] = 0xC6; rom_[0x52] = 0x60;  // $Cn50 ProDOS block: WDM $C6 / RTS
        rom_[0x53] = 0x42; rom_[0x54] = 0xC5; rom_[0x55] = 0x60;  // $Cn53 SmartPort:    WDM $C5 / RTS
        return;
    }

    // ── ProDOS block device (slot-7 HDD): streaming firmware, no traps ──────
    // ProDOS signature + entry offsets.
    rom_[0x00] = 0x4C; rom_[0x01] = kBoot; rom_[0x02] = hi;   // JMP $Cn20
    rom_[0x03] = 0x00; rom_[0x05] = 0x03; rom_[0x07] = 0x01;
    rom_[0xFE] = 0x03; rom_[0xFF] = kDrv;

    // Boot ($Cn20): read block 0 to $0800, JMP $0801 with X=unit.
    uint8_t pc = kBoot;
    emit(rom_, pc, {
        0xA9,0x01, 0x85,0x42,               // LDA #1 / STA $42 (read)
        0xA9,unit, 0x85,0x43,               // unit
        0xA9,0x00, 0x85,0x44,               // buffer lo = 0
        0xA9,0x08, 0x85,0x45,               // buffer hi = $08
        0xA9,0x00, 0x85,0x46, 0x85,0x47,    // block = 0
        0x20,kDrv,hi,                       // JSR $Cn50
        0xB0,0x07,                          // BCS error
        0xA2,unit, 0xA9,0x00, 0x4C,0x01,0x08, // LDX unit / LDA #0 / JMP $0801
        0x4C,0xE0,hi                        // error: JMP $CnE0
    });
    rom_[0xE0] = 0x4C; rom_[0xE1] = 0xE0; rom_[0xE2] = hi;    // $CnE0: halt loop

    // Driver dispatch ($Cn50).
    pc = kDrv;
    emit(rom_, pc, {
        0xA5,0x42, 0xC9,0x01, 0xF0,0x10,    // cmd==1 → read (+16)
        0xC9,0x02, 0xF0,0x37,               // cmd==2 → write (+55)
        0xC9,0x00, 0xF0,0x04,               // cmd==0 → status (+4)
        0xA9,0x01, 0x38, 0x60,              // bad command
        0x4C,0xC0,hi, 0xEA                  // status: JMP $CnC0
    });
    // STATUS ($CnC0): block count → X/Y.
    { uint8_t sp = 0xC0; emit(rom_, sp, {
        0xAE, uint8_t(dev+0x04), 0xC0,       // LDX $C0(8+n)4
        0xAC, uint8_t(dev+0x05), 0xC0,       // LDY $C0(8+n)5
        0xA9,0x00, 0x18, 0x60 }); }          // LDA #0 / CLC / RTS

    const uint8_t data = uint8_t(dev + 0x02), stat = uint8_t(dev + 0x03);
    // read block (from $Cn66):
    emit(rom_, pc, {
        0xAD,stat,0xC0, 0x10,0x04,           // LDA status / BPL ok
        0xA9,0x28, 0x38, 0x60,               // NO DEVICE
        0xA5,0x46, 0x8D,uint8_t(dev+0),0xC0, // block lo → $C0(8+n)0
        0xA5,0x47, 0x8D,uint8_t(dev+1),0xC0, // block hi → $C0(8+n)1
        0xA0,0x00,
        0xAD,data,0xC0, 0x91,0x44, 0xC8, 0xD0,0xF8,  // page 1
        0xE6,0x45,
        0xAD,data,0xC0, 0x91,0x44, 0xC8, 0xD0,0xF8,  // page 2
        0xC6,0x45, 0x18, 0x60                // CLC / RTS
    });
    // write block (from $Cn91):
    emit(rom_, pc, {
        0xAD,stat,0xC0, 0x29,0x40, 0xF0,0x04,
        0xA9,0x2B, 0x38, 0x60,               // write-protected
        0xA5,0x46, 0x8D,uint8_t(dev+0),0xC0,
        0xA5,0x47, 0x8D,uint8_t(dev+1),0xC0,
        0xA0,0x00,
        0xB1,0x44, 0x8D,data,0xC0, 0xC8, 0xD0,0xF8,  // page 1
        0xE6,0x45,
        0xB1,0x44, 0x8D,data,0xC0, 0xC8, 0xD0,0xF8,  // page 2
        0xC6,0x45, 0xA9,0x00, 0x18, 0x60
    });
}
