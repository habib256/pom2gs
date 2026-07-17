// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ProDOS hard-disk card. See ProDosHdd.h. Firmware ported from POM2's
// ProDOSHardDiskCard (AppleWin lineage). Source: Apple ProDOS 8 Tech Ref.

#include "ProDosHdd.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

bool ProDosHdd::loadImage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    img_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // .2mg images carry a 64-byte header; a raw .hdv/.po is a multiple of 512.
    headerBytes_ = 0;
    writeProtect_ = false;
    if (img_.size() >= 64 && img_[0] == '2' && img_[1] == 'I' && img_[2] == 'M' && img_[3] == 'G') {
        // 2IMG flags dword at offset 16 (LE); bit 31 = volume locked / write-protected.
        const uint32_t flags = uint32_t(img_[16]) | (uint32_t(img_[17]) << 8)
                             | (uint32_t(img_[18]) << 16) | (uint32_t(img_[19]) << 24);
        writeProtect_ = (flags & 0x80000000u) != 0;
        img_.erase(img_.begin(), img_.begin() + 64);
        headerBytes_ = 64;
    }
    // Round down to a whole number of blocks.
    img_.resize((img_.size() / kBlockBytes) * kBlockBytes);
    selectedBlock_ = 0; streamOffset_ = 0;
    path_ = path;
    return !img_.empty();
}

// Write one 512-byte block back to the backing file (in place, past any .2mg
// header) so a format / GS/OS install persists. Silently skipped if not
// file-backed, write-protected, or out of range.
void ProDosHdd::flushBlock(uint32_t blk) {
    if (path_.empty() || writeProtect_) return;
    const size_t off = size_t(blk) * kBlockBytes;
    if (off + kBlockBytes > img_.size()) return;
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return;
    f.seekp(std::streamoff(headerBytes_ + off), std::ios::beg);
    f.write(reinterpret_cast<const char*>(img_.data() + off), kBlockBytes);
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
    flushBlock(blk);
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
        if (streamOffset_ == 0) flushBlock(selectedBlock_);   // block complete → persist
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
        // SmartPort ID type byte (Firmware Ref fig 7-4): bit7 = Extended
        // SmartPort. The old $02 actually meant "SCSI" (bit1) and left bit7
        // clear, so probes for extended support failed — Dungeon Master
        // aborts with "SmartPort firmware not detected in slot 5!". The real
        // ROM 03 slot-5 firmware has $C0, but mirroring bit6 too makes Silent
        // Service treat us as the *internal* Apple 3.5 controller and call
        // past the public dispatch into firmware internals our stub doesn't
        // have (BRK). $80 = extended-capable third-party card — what the HLE
        // actually is (matches POM2's SmartPortCard).
        rom_[0xFB] = 0x80;
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
    // $CnFE = ProDOS device characteristics (ProDOS 8 TRM Table 6-1): bit0 status,
    // bit1 read, bit2 WRITE, bit3 format, bits5-4 #volumes, bit6 interrupt, bit7
    // removable. Was $03 = read+status only → GS/OS/the Installer treated the hard
    // disk as **locked** ("Writing to this disk is not allowed"). $07 = read + write
    // + status makes it a writable install target. (No format bit: the streaming
    // driver has no FORMAT command and the image is a fixed pre-formatted volume.)
    rom_[0xFE] = 0x07; rom_[0xFF] = kDrv;

    // Boot ($Cn20): read block 0 to $0800. If it's a valid ProDOS boot block
    // (byte 0 = $01) JMP $0801; otherwise (blank / non-bootable disk, or read
    // error) chain to slot 5 so the ROM boots the 3.5" install disk there. This
    // lets a blank hard disk sit on slot 7 as an install *target* without
    // hijacking the boot — GS/OS installs a real boot block later.
    //
    // The chain must re-point the boot device to slot 5, not just JMP $C500:
    // the IIgs ROM commits the startup slot in zero-page $00/$01 and boots it
    // via `JMP ($0000)` (ROM $FF/FAD4). Scanning $C8→ it finds slot 7 first (our
    // ProDOS signature), sets $01=$C7, and boots us. A bare `JMP $C500` leaves
    // $01=$C7, so the 3.5"-loaded GS/OS bootstrap derives its boot device from
    // $01 = slot 7 and looks for `*/SYSTEM/START.GS.OS` on the (blank) HDD →
    // "Unable to load START.GS.OS  Error=$0046" and a hung boot. Instead we set
    // $01=$C5 and re-issue the ROM's own `JMP ($0000)`, so slot 5 boots exactly
    // as if the scan had selected it and every downstream boot-slot global reads
    // $C5. (Diagnosed with tests/hdd_trace: $43 DEVNUM went $70→$50→$70, the last
    // set by the RAM bootstrap at $00:21C8 from a slot-7 boot global.)
    uint8_t pc = kBoot;
    emit(rom_, pc, {
        0xA9,0x01, 0x85,0x42,               // LDA #1 / STA $42 (read)
        0xA9,unit, 0x85,0x43,               // unit
        0xA9,0x00, 0x85,0x44,               // buffer lo = 0
        0xA9,0x08, 0x85,0x45,               // buffer hi = $08
        0xA9,0x00, 0x85,0x46, 0x85,0x47,    // block = 0
        0x20,kDrv,hi,                       // JSR $Cn50 (read block 0)
        0xB0,0x0E,                          // BCS chain (read error → slot 5)
        0xAD,0x00,0x08,                     // LDA $0800 (block-0 byte 0)
        0xC9,0x01,                          // CMP #$01 (ProDOS boot block?)
        0xD0,0x07,                          // BNE chain (not bootable → slot 5)
        0xA2,unit, 0xA9,0x00, 0x4C,0x01,0x08, // LDX unit / LDA #0 / JMP $0801
        0x4C,0x08,hi                          // chain: JMP $Cn08 (re-point + boot slot 5)
    });
    // Chain-to-slot-5 stub in the free $Cn08-$Cn1F scratch area. Re-point every
    // boot-slot global the ROM/GS bootstrap consults from slot 7 ($C7) to slot 5
    // ($C5): zero-page $00/$01 (the ROM's `JMP ($0000)` boot vector) and MSLOT
    // $07F8 (which the 3.5"/GS boot block reads to derive its slot). Then re-issue
    // the ROM's own boot mechanism, `JMP ($0000)`, so slot 5 boots exactly as if
    // the startup scan had selected it. Without fixing MSLOT the RAM bootstrap
    // still derives slot 7 → START.GS.OS not found on the blank HDD ($46).
    { uint8_t sp = 0x08; emit(rom_, sp, {
        0xA9,0xC5, 0x85,0x01, 0x64,0x00,     // LDA #$C5 / STA $01 / STZ $00
        0xA9,0xC5, 0x8D,0xF8,0x07,           // LDA #$C5 / STA $07F8 (MSLOT)
        0x6C,0x00,0x00 }); }                 // JMP ($0000) → $C500

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
