// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// IIgs MMU (FPI + Mega II). See IIgsMemory.h + DEV.md § Memory.
// Source of truth: MAME apple2gs.cpp (register semantics cited inline).
//
// M2 scope: enough of the banked space + $C0xx registers + language card to
// boot a real ROM (reset vector from ROM, native-mode execution, RAM). The
// //e main/aux redirection and full shadow write-through are staged in as the
// boot trace demands them.

#include "IIgsMemory.h"
#include <algorithm>

IIgsMemory::IIgsMemory() {
    slowRam_.assign(0x20000, 0);
    setFastRamKB(fastRamKB_);
}

void IIgsMemory::setFastRamKB(uint32_t kb) {
    fastRamKB_ = kb;
    fastRam_.assign(size_t(kb) * 1024, 0);
}

int IIgsMemory::vpos() const {
    return int((videoCycles_ / kLineCycles) % kLines);
}

void IIgsMemory::tick(int cpuCycles) {
    videoCycles_ += (cpuCycles > 0 ? cpuCycles : 1);
    const int vp = vpos();
    // Rising edge into VBL (line 192): latch the VBL interrupt flag.
    if (vp >= 192 && lastVpos_ < 192) intflag_ |= 0x08;   // INTFLAG_VBL
    lastVpos_ = vp;
}

void IIgsMemory::setTestMode(bool on) {
    testMode_ = on;
    if (on && flat_.empty()) flat_.assign(size_t(1) << 24, 0);   // 16 MB
}

bool IIgsMemory::loadRom(const std::vector<uint8_t>& rom) {
    if (rom.size() == 0x40000) { rom_ = rom; romBankBase_ = 0xFC; return true; }  // 256 KB → $FC-$FF
    if (rom.size() == 0x20000) { rom_ = rom; romBankBase_ = 0xFE; return true; }  // 128 KB → $FE-$FF
    return false;
}

void IIgsMemory::reset() {
    std::fill(fastRam_.begin(), fastRam_.end(), 0);
    std::fill(slowRam_.begin(), slowRam_.end(), 0);
    shadow_ = 0; speed_ = 0; state_ = 0; newvideo_ = 0;
    altzp_ = ramrd_ = ramwrt_ = page2_ = store80_ = hires_ = false;
    intcxrom_ = false; slotc3rom_ = false; eightyCol_ = false; altchar_ = false;
    lcRamRead_ = false; lcRamWrite_ = false; lcBank2_ = true; lcPreWrite_ = false;
    kbdLatch_ = 0;
    videoCycles_ = 0; lastVpos_ = 0; intflag_ = 0; inten_ = 0; vgcint_ = 0;
}

// Fast RAM cell, mirroring the top of the address space down if the machine
// has less RAM than the access bank (real FPI wraps / floats — good enough).
uint8_t& IIgsMemory::fastCell(uint32_t bank, uint16_t off) {
    size_t idx = (size_t(bank) << 16) | off;
    if (idx >= fastRam_.size()) idx %= fastRam_.size();
    return fastRam_[idx];
}

// ── Language card ($D000-$FFFF in banks $00/$01/$E0/$E1) ─────────────────
// !lcRamRead → the underlying ROM ($FF bank) shows through (this is why the
// reset/IRQ vectors read ROM at boot). Bank2 selects the alternate $D000-$DFFF
// 4 KB (stored in the unused $C000-$CFFF RAM window, the classic LC layout).
uint8_t IIgsMemory::lcRead(uint8_t bank, uint16_t off) {
    if (!lcRamRead_) {                              // ROM shows through
        uint32_t romTop = uint32_t(rom_.size()) - 0x10000;   // bank $FF
        return rom_.empty() ? 0 : rom_[romTop + off];
    }
    if (off < 0xE000 && !lcBank2_)                  // $D000-$DFFF bank 1
        return fastCell(bank, off);
    if (off < 0xE000 && lcBank2_)                   // $D000-$DFFF bank 2 (aliased to $Cxxx window)
        return fastCell(bank, uint16_t(off - 0x1000));
    return fastCell(bank, off);                     // $E000-$FFFF (single bank)
}

void IIgsMemory::lcWrite(uint8_t bank, uint16_t off, uint8_t v) {
    if (!lcRamWrite_) return;                       // writes ignored unless LC write enabled
    if (off < 0xE000 && lcBank2_) { fastCell(bank, uint16_t(off - 0x1000)) = v; return; }
    fastCell(bank, off) = v;
}

// Language-card bank switching ($C080-$C08F). Standard //e decode.
void IIgsMemory::lcSwitch(uint16_t off, bool writing) {
    const uint8_t n = off & 0x0F;
    lcBank2_ = !(n & 0x08);                         // A3=0 → bank2, A3=1 → bank1
    const bool ramInSel = (n & 0x03) == 0x03;       // 11 → read RAM
    const bool romInSel = (n & 0x03) == 0x00;       // 00 → read ROM (write RAM)
    lcRamRead_  = ramInSel;
    if (romInSel) lcRamRead_ = false;
    // Write-enable requires two consecutive odd reads (pre-write latch).
    if (n & 0x01) {                                 // odd access
        if (!writing) { if (lcPreWrite_) lcRamWrite_ = true; lcPreWrite_ = true; }
    } else {
        lcPreWrite_ = false; lcRamWrite_ = false;
    }
    if ((n & 0x03) == 0x01 || (n & 0x03) == 0x02) {
        // 01/10 combos: RAM read off, but write handling per parity above
        lcRamRead_ = (n & 0x03) == 0x03;
    }
}

// ── $C000-$C0FF I/O ──────────────────────────────────────────────────────
uint8_t IIgsMemory::ioRead(uint8_t bank, uint16_t off) {
    (void)bank;
    const uint8_t r = off & 0xFF;
    if (r <= 0x0F) return kbdLatch_;                        // keyboard latch
    switch (r) {
        case 0x10: { uint8_t v = kbdLatch_ & 0x7F; kbdLatch_ &= 0x7F; return v; } // clear strobe
        case 0x11: return lcBank2_ ? 0x80 : 0x00;           // RDLCBNK2
        case 0x12: return lcRamRead_ ? 0x80 : 0x00;         // RDLCRAM
        case 0x13: return ramrd_ ? 0x80 : 0x00;
        case 0x14: return ramwrt_ ? 0x80 : 0x00;
        case 0x15: return intcxrom_ ? 0x80 : 0x00;
        case 0x16: return altzp_ ? 0x80 : 0x00;
        case 0x17: return slotc3rom_ ? 0x80 : 0x00;
        case 0x18: return store80_ ? 0x80 : 0x00;
        case 0x19: return inVbl() ? 0x80 : 0x00;            // RDVBL (MAME 1425)
        case 0x1A: return 0x00;                             // TEXT
        case 0x1B: return 0x00;                             // MIXED
        case 0x1C: return page2_ ? 0x80 : 0x00;
        case 0x1D: return hires_ ? 0x80 : 0x00;
        case 0x1E: return altchar_ ? 0x80 : 0x00;
        case 0x1F: return eightyCol_ ? 0x80 : 0x00;
        case 0x23: return vgcint_;                          // VGCINT
        case 0x29: return newvideo_;
        case 0x2E: return uint8_t(vpos() >> 1);             // VERTCNT (MAME 1467)
        case 0x2F: return uint8_t((vpos() & 1) ? 0x80 : 0x00); // HORIZCNT (vpos parity bit)
        case 0x35: return shadow_;
        case 0x36: return speed_;
        case 0x41: return inten_;                           // INTEN
        case 0x46: return intflag_;                         // INTFLAG (VBL bit etc.)
        case 0x47: intflag_ &= ~0x08; return 0;             // CLRVBLINT
        case 0x68: return state_;                           // STATEREG
        default: break;
    }
    // display / paging soft switches with read side-effects
    if (r >= 0x80 && r <= 0x8F) { lcSwitch(r, false); return 0; }
    if (r >= 0x50 && r <= 0x5F) { /* display toggles */ }
    return 0;   // floating bus (approx)
}

void IIgsMemory::ioWrite(uint8_t bank, uint16_t off, uint8_t v) {
    (void)bank;
    const uint8_t r = off & 0xFF;
    switch (r) {
        // //e paging soft switches ($C000-$C00F write)
        case 0x00: store80_ = false; return;
        case 0x01: store80_ = true;  return;
        case 0x02: ramrd_ = false; return;
        case 0x03: ramrd_ = true;  return;
        case 0x04: ramwrt_ = false; return;
        case 0x05: ramwrt_ = true;  return;
        case 0x06: intcxrom_ = false; return;
        case 0x07: intcxrom_ = true;  return;
        case 0x08: altzp_ = false; return;
        case 0x09: altzp_ = true;  return;
        case 0x0A: slotc3rom_ = false; return;
        case 0x0B: slotc3rom_ = true;  return;
        case 0x0C: eightyCol_ = false; return;
        case 0x0D: eightyCol_ = true;  return;
        case 0x0E: altchar_ = false; return;
        case 0x0F: altchar_ = true;  return;
        case 0x23: vgcint_ = v; return;                     // VGCINT enable
        case 0x29: newvideo_ = v & 0xE1; return;            // NEWVIDEO (MAME 1707)
        case 0x32: vgcint_ &= v; return;                    // VGCINTCLEAR
        case 0x41: inten_ = v & 0x1F; return;               // INTEN (MAME 1811)
        case 0x47: intflag_ &= ~0x08; return;               // CLRVBLINT
        case 0x35: shadow_ = v; return;                     // SHADOW (MAME 1751)
        case 0x36: speed_ = v; return;                      // SPEED (MAME 1766)
        case 0x68:                                          // STATEREG (composite)
            state_ = v;
            altzp_    = v & 0x80; page2_    = v & 0x40;
            ramrd_    = v & 0x20; ramwrt_   = v & 0x10;
            lcRamRead_= !(v & 0x08); lcBank2_ = v & 0x04;
            intcxrom_ = v & 0x01;
            return;
        default: break;
    }
    if (r >= 0x50 && r <= 0x5F) {                            // display soft switches
        switch (r) {
            case 0x54: page2_ = false; return;
            case 0x55: page2_ = true;  return;
            case 0x56: hires_ = false; return;
            case 0x57: hires_ = true;  return;
            default: return;
        }
    }
    if (r >= 0x80 && r <= 0x8F) { lcSwitch(r, true); return; }
}

// Writes to shadowed display regions of banks $00/$01 mirror into $E0/$E1 so
// the slow-side video generator sees them (SHADOW reg gates each region).
void IIgsMemory::maybeShadow(uint8_t bank, uint16_t off, uint8_t v) {
    if (bank > 1) return;
    bool doit = false;
    if (off >= 0x0400 && off <= 0x07FF) doit = !(shadow_ & SHAD_TXTPG1);       // text/lores page 1
    else if (off >= 0x2000 && off <= 0x3FFF) doit = !(shadow_ & SHAD_HIRESPG1);// hires page 1
    else if (off >= 0x4000 && off <= 0x5FFF) doit = !(shadow_ & SHAD_HIRESPG2);// hires page 2
    else if (bank == 1 && off >= 0x2000 && off <= 0x9FFF) doit = !(shadow_ & SHAD_SUPERHIRES); // SHR
    if (doit) slowRam_[(size_t(bank) * 0x10000) + off] = v;
}

// ── the CPU's bus hooks ──────────────────────────────────────────────────
uint8_t IIgsMemory::read8(uint32_t a) {
    a &= kAddrMask;
    if (testMode_) return flat_[a];
    const uint8_t bank = a >> 16;
    const uint16_t off = a & 0xFFFF;

    if (bank >= romBankBase_ && !rom_.empty())
        return rom_[a - (uint32_t(romBankBase_) << 16)];

    if (bank == 0xE0 || bank == 0xE1) {
        if (off >= 0xC000 && off <= 0xCFFF) return ioRead(bank, off);
        return slowRam_[(size_t(bank - 0xE0) * 0x10000) + off];
    }

    if (bank <= 0x01) {
        if (iolcShadow()) {
            if (off >= 0xC000 && off <= 0xCFFF) return ioRead(bank, off);
            if (off >= 0xD000)                  return lcRead(bank, off);
        }
        return fastCell(bank, off);
    }

    if (bank <= 0x7F) return fastCell(bank, off);
    return 0;   // $E2-$FB unmapped → floating bus (approx)
}

void IIgsMemory::write8(uint32_t a, uint8_t v) {
    a &= kAddrMask;
    if (testMode_) { flat_[a] = v; return; }
    const uint8_t bank = a >> 16;
    const uint16_t off = a & 0xFFFF;

    if (bank >= romBankBase_ && !rom_.empty()) return;   // ROM write ignored

    if (bank == 0xE0 || bank == 0xE1) {
        if (off >= 0xC000 && off <= 0xCFFF) { ioWrite(bank, off, v); return; }
        slowRam_[(size_t(bank - 0xE0) * 0x10000) + off] = v;
        return;
    }

    if (bank <= 0x01) {
        if (iolcShadow()) {
            if (off >= 0xC000 && off <= 0xCFFF) { ioWrite(bank, off, v); return; }
            if (off >= 0xD000)                  { lcWrite(bank, off, v); return; }
        }
        fastCell(bank, off) = v;
        maybeShadow(bank, off, v);
        return;
    }

    if (bank <= 0x7F) { fastCell(bank, off) = v; return; }
    // $E2-$FB unmapped → drop
}
