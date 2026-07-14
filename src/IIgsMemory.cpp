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
#include "CPU65816.h"
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
    // Rising edge into VBL (line 192): latch the VBL flag and, if the VBL
    // interrupt is enabled ($C041 INTEN bit3), assert the CPU IRQ line.
    if (vp >= 192 && lastVpos_ < 192) {
        intflag_ |= 0x08;                                 // INTFLAG_VBL
        updateMega2Irq();
    }
    lastVpos_ = vp;
}

// Recompute the two shared IRQ lines from the flag/enable registers. Both the
// VBL + ¼-second (Mega II) and the scan-line + 1-second (VGC) sources are
// wire-OR'd onto one CPU line each; assert only while an enabled source has a
// pending flag, so the level tracks the register state exactly.
void IIgsMemory::updateMega2Irq() {
    const bool active = (intflag_ & inten_ & 0x18) != 0;      // VBL 0x08 | ¼-sec 0x10
    if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_MEGA2_VBL, active);
}

void IIgsMemory::updateVgcIrq() {
    const bool oneSec = (vgcint_ & 0x04) && (vgcint_ & 0x40);  // enable 0x04 & status 0x40
    const bool scan   = (vgcint_ & 0x02) && (vgcint_ & 0x20);  // enable 0x02 & status 0x20
    const bool active = oneSec || scan;
    if (active) vgcint_ |= 0x80; else vgcint_ &= uint8_t(~0x80);
    if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_VGC_VBL, active);
}

// Once per host frame: the periodic Mega II / VGC timers, the (approximate)
// scan-line interrupt, and the DOC oscillator IRQ poll.
void IIgsMemory::frameTick() {
    ++frameCount_;
    // ¼-second (~0.25 s at 60 Hz) → Mega II INTFLAG bit4.
    if (frameCount_ % 15 == 0) { intflag_ |= 0x10; updateMega2Irq(); }
    // 1-second → VGC status bit6.
    if (frameCount_ % 60 == 0) { vgcint_ |= 0x40; updateVgcIrq(); }
    // Scan-line (SHR): if enabled ($C023 bit1) and any SCB line requests an
    // interrupt (SCB bit6), fire once. The renderer draws a whole frame in one
    // mode, so this is status/IRQ only — no mid-frame split yet.
    if (shrEnabled() && (vgcint_ & 0x02)) {
        const uint8_t* scb = slowRam_.data() + 0x10000 + 0x9D00;
        for (int l = 0; l < 200; ++l)
            if (scb[l] & 0x40) { vgcint_ |= 0x20; updateVgcIrq(); break; }
    }
    // DOC oscillator IRQ (sampled-sound / music): the chip flags a completed
    // IRQ-enabled oscillator during render(); mirror it onto the CPU line.
    if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_DOC, doc_.irqPending());
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
    shadow_ = 0; speed_ = 0; state_ = 0; newvideo_ = 0; txtColor_ = 0xF0;
    altzp_ = ramrd_ = ramwrt_ = page2_ = store80_ = hires_ = false;
    intcxrom_ = false; slotc3rom_ = false; eightyCol_ = false; altchar_ = false;
    textMode_ = true; mixed_ = false; dhgr_ = false;
    lcRamRead_ = false; lcRamWrite_ = false; lcBank2_ = true; lcPreWrite_ = false;
    kbdLatch_ = 0;
    videoCycles_ = 0; lastVpos_ = 0; intflag_ = 0; inten_ = 0; vgcint_ = 0; frameCount_ = 0;
    if (cpu_) {                          // drop any interrupt lines we own
        cpu_->setIrqLine(CPU65816::IRQ_SRC_MEGA2_VBL, false);
        cpu_->setIrqLine(CPU65816::IRQ_SRC_VGC_VBL, false);
        cpu_->setIrqLine(CPU65816::IRQ_SRC_DOC, false);
    }
    adbDataReady_ = false; adbResponse_ = 0; clkData_ = 0; clkCtl_ = 0;
    slowPenMaster_ = 0;
}

// Fast RAM cell, mirroring the top of the address space down if the machine
// has less RAM than the access bank (real FPI wraps / floats — good enough).
uint8_t& IIgsMemory::fastCell(uint32_t bank, uint16_t off) {
    size_t idx = (size_t(bank) << 16) | off;
    if (idx >= fastRam_.size()) idx %= fastRam_.size();
    return fastRam_[idx];
}

// //e main/aux redirection (Mega II) for a bank $00/$01 CPU access.
// Cited: Apple //e Technical Reference (MMU soft switches).
int IIgsMemory::physBank01(uint16_t off, bool writing) const {
    if (off < 0x0200) return altzp_ ? 1 : 0;                       // zero page + stack
    if (store80_ && off >= 0x0400 && off <= 0x07FF) return page2_ ? 1 : 0;            // text page 1
    if (store80_ && hires_ && off >= 0x2000 && off <= 0x3FFF) return page2_ ? 1 : 0;  // hi-res page 1
    if (off >= 0x0200 && off <= 0xBFFF) return (writing ? ramwrt_ : ramrd_) ? 1 : 0;  // main/aux RAM
    return 0;
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
    (void)bank;
    const int pb = altzp_ ? 1 : 0;                  // LC RAM main/aux per ALTZP
    if (off < 0xE000 && !lcBank2_)                  // $D000-$DFFF bank 1
        return fastCell(pb, off);
    if (off < 0xE000 && lcBank2_)                   // $D000-$DFFF bank 2 (aliased to $Cxxx window)
        return fastCell(pb, uint16_t(off - 0x1000));
    return fastCell(pb, off);                       // $E000-$FFFF (single bank)
}

void IIgsMemory::lcWrite(uint8_t bank, uint16_t off, uint8_t v) {
    (void)bank;
    if (!lcRamWrite_) return;                       // writes ignored unless LC write enabled
    const int pb = altzp_ ? 1 : 0;
    if (off < 0xE000 && lcBank2_) { fastCell(pb, uint16_t(off - 0x1000)) = v; return; }
    fastCell(pb, off) = v;
}

// Language-card bank switching ($C080-$C08F). Canonical //e decode:
//   read RAM  when bit0 == bit1   ($C080/$C083/$C088/$C08B)
//   read ROM  otherwise           ($C081/$C082/$C089/$C08A)
//   write RAM enabled by two consecutive ODD (bit0=1) accesses
//   bank 2 when bit3 == 0
void IIgsMemory::lcSwitch(uint16_t off, bool writing) {
    const uint8_t n = off & 0x0F;
    lcBank2_ = !(n & 0x08);
    lcRamRead_ = ((n & 0x01) == ((n >> 1) & 0x01));
    if (n & 0x01) {                                 // odd → arm/enable write
        if (!writing) { if (lcPreWrite_) lcRamWrite_ = true; lcPreWrite_ = true; }
    } else {                                        // even → disable write
        lcPreWrite_ = false; lcRamWrite_ = false;
    }
}

// Display soft switches $C050-$C05F (toggle on any access — read or write).
void IIgsMemory::applyDisplaySwitch(uint8_t r) {
    switch (r) {
        case 0x50: textMode_ = false; break;   case 0x51: textMode_ = true;  break;
        case 0x52: mixed_ = false;    break;   case 0x53: mixed_ = true;     break;
        case 0x54: page2_ = false;    break;   case 0x55: page2_ = true;     break;
        case 0x56: hires_ = false;    break;   case 0x57: hires_ = true;     break;
        case 0x5E: dhgr_ = true;      break;   case 0x5F: dhgr_ = false;     break;
        default: break;
    }
}

// Slot / internal ROM $C100-$CFFF. Per-slot ROM at $Cn00-$CnFF (slots 1-7);
// $C800-$CFFF is the shared expansion window. The slot-7 HDD card exposes its
// firmware here; the IIgs internal slot firmware is not yet mapped (returns 0).
uint8_t IIgsMemory::slotRomRead(uint16_t off) {
    int slot = (off >> 8) & 0x0F;                 // $C7xx → 7
    if (slot == hdd_.slot() && hdd_.loaded())
        return hdd_.romRead(uint8_t(off & 0xFF));
    return 0;
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
        // ── ADB GLU ──
        case 0x24: return 0x00;                             // MOUSEDATA (no mouse yet)
        case 0x25: return 0x00;                             // KEYMODREG (no modifiers)
        case 0x26: { uint8_t v = adbResponse_; adbDataReady_ = false; return v; } // DATAREG
        case 0x27: return adbDataReady_ ? 0x20 : 0x00;      // KMSTATUS: bit5 data-ready, bit0 CMDFULL=0
        case 0x33: return clkData_;                          // CLOCKDATA
        case 0x34: return clkCtl_;                           // CLOCKCTL
        case 0x19: return inVbl() ? 0x80 : 0x00;            // RDVBL (MAME 1425)
        case 0x1A: return textMode_ ? 0x80 : 0x00;         // RDTEXT
        case 0x1B: return mixed_ ? 0x80 : 0x00;             // RDMIXED
        case 0x1C: return page2_ ? 0x80 : 0x00;
        case 0x1D: return hires_ ? 0x80 : 0x00;
        case 0x1E: return altchar_ ? 0x80 : 0x00;
        case 0x1F: return eightyCol_ ? 0x80 : 0x00;
        case 0x30: case 0x31:                               // SPKR: toggle 1-bit speaker
            if (spkEvents_.size() < 65536) spkEvents_.push_back(videoCycles_);
            return 0;
        case 0x38: case 0x39: case 0x3A: case 0x3B: return scc_.read(r);    // SCC serial
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: {                       // Sound GLU
            uint8_t v = doc_.gluRead(r);
            if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_DOC, doc_.irqPending());
            return v;
        }
        case 0x22: return txtColor_;                        // SCREENCOLOR (text fg/bg)
        case 0x23: return vgcint_;                          // VGCINT
        case 0x29: return newvideo_;
        case 0x2E: return uint8_t(vpos() >> 1);             // VERTCNT (MAME 1467)
        case 0x2F: return uint8_t((vpos() & 1) ? 0x80 : 0x00); // HORIZCNT (vpos parity bit)
        case 0x35: return shadow_;
        case 0x36: return speed_;
        // ── Joystick: buttons + paddles ──
        case 0x61: return button_[0] ? 0x80 : 0x00;         // PB0 / open-apple
        case 0x62: return button_[1] ? 0x80 : 0x00;         // PB1 / solid-apple
        case 0x63: return button_[2] ? 0x80 : 0x00;         // PB2
        case 0x64: case 0x65: case 0x66: case 0x67: {       // PADDL0-3: RC timing
            uint64_t elapsed = videoCycles_ - paddleReset_;
            return (elapsed < uint64_t(paddle_[r - 0x64]) * 11) ? 0x80 : 0x00;
        }
        case 0x70: paddleReset_ = videoCycles_; return 0;   // PTRIG: start the RC timers
        case 0x41: return inten_;                           // INTEN
        case 0x46: return intflag_;                         // INTFLAG (VBL bit etc.)
        case 0x47: intflag_ &= uint8_t(~0x18); updateMega2Irq(); return 0; // CLRVBLINT (VBL + ¼-sec)
        case 0x68:                                          // STATEREG — synthesize (MAME 1926)
            return uint8_t((altzp_ ? 0x80 : 0) | (page2_ ? 0x40 : 0) |
                           (ramrd_ ? 0x20 : 0) | (ramwrt_ ? 0x10 : 0) |
                           (lcRamRead_ ? 0x00 : 0x08) | (lcBank2_ ? 0x04 : 0) |
                           (intcxrom_ ? 0x01 : 0));
        default: break;
    }
    // display / paging soft switches with read side-effects
    if (r >= 0x80 && r <= 0x8F) { lcSwitch(r, false); return 0; }
    if (r >= 0x50 && r <= 0x5F) { applyDisplaySwitch(r); return 0; }
    if (r >= 0xE0 && r <= 0xEF) return iwm_.access(r - 0xE0, false, 0, videoCycles_); // slot 6 IWM
    if (r >= 0xF0 && r <= 0xFF) return hdd_.deviceRead(r & 0x0F);                     // slot 7 HDD device-select
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
        case 0x26: adbResponse_ = 0x00; adbDataReady_ = true; return; // DATAREG: accept cmd, queue response
        case 0x27: return;                                  // KMSTATUS write (ignored)
        case 0x33: clkData_ = v; return;                    // CLOCKDATA
        case 0x34: clkCtl_ = v & 0x6F; return;              // CLOCKCTL
        case 0x30: case 0x31:                               // SPKR: toggle 1-bit speaker
            if (spkEvents_.size() < 65536) spkEvents_.push_back(videoCycles_);
            return;
        case 0x38: case 0x39: case 0x3A: case 0x3B: scc_.write(r, v); return;    // SCC serial
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: doc_.gluWrite(r, v); return; // Sound GLU
        case 0x22: txtColor_ = v; return;                   // SCREENCOLOR (text fg/bg)
        case 0x23: vgcint_ = uint8_t((vgcint_ & 0xE0) | (v & 0x06)); updateVgcIrq(); return; // VGCINT: enables (bits1-2)
        case 0x29: newvideo_ = v & 0xE1; return;            // NEWVIDEO (MAME 1707)
        case 0x32: vgcint_ &= uint8_t(~0x60); updateVgcIrq(); return;   // VGCINTCLEAR (scan + 1-sec status)
        case 0x70: paddleReset_ = videoCycles_; return;     // PTRIG (write also strobes)
        case 0x41: inten_ = v & 0x1F; updateMega2Irq(); return;   // INTEN (MAME 1811)
        case 0x47: intflag_ &= uint8_t(~0x18); updateMega2Irq(); return; // CLRVBLINT (VBL + ¼-sec)
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
    if (r >= 0x50 && r <= 0x5F) { applyDisplaySwitch(r); return; }
    if (r >= 0x80 && r <= 0x8F) { lcSwitch(r, true); return; }
    if (r >= 0xE0 && r <= 0xEF) { iwm_.access(r - 0xE0, true, v, videoCycles_); return; } // slot 6 IWM
    if (r >= 0xF0 && r <= 0xFF) { hdd_.deviceWrite(r & 0x0F, v); return; }                // slot 7 HDD device-select
}

// Writes to shadowed display regions of banks $00/$01 mirror into $E0/$E1 so
// the slow-side video generator sees them (SHADOW reg gates each region).
bool IIgsMemory::maybeShadow(uint8_t bank, uint16_t off, uint8_t v) {
    if (bank > 1) return false;
    bool doit = false;
    if (off >= 0x0400 && off <= 0x07FF) doit = !(shadow_ & SHAD_TXTPG1);       // text/lores page 1
    else if (off >= 0x2000 && off <= 0x3FFF) doit = !(shadow_ & SHAD_HIRESPG1);// hires page 1
    else if (off >= 0x4000 && off <= 0x5FFF) doit = !(shadow_ & SHAD_HIRESPG2);// hires page 2
    else if (bank == 1 && off >= 0x2000 && off <= 0x9FFF) doit = !(shadow_ & SHAD_SUPERHIRES); // SHR
    if (doit) slowRam_[(size_t(bank) * 0x10000) + off] = v;
    return doit;
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
        chargeSlow();                                 // slow side (Mega II)
        if (off >= 0xC000 && off <= 0xC0FF) return ioRead(bank, off);
        if (off >= 0xC100 && off <= 0xCFFF) return slotRomRead(off);
        return slowRam_[(size_t(bank - 0xE0) * 0x10000) + off];
    }

    if (bank <= 0x01) {
        if (iolcShadow()) {
            if (off >= 0xC000 && off <= 0xC0FF) { chargeSlow(); return ioRead(bank, off); }
            if (off >= 0xC100 && off <= 0xCFFF) { chargeSlow(); return slotRomRead(off); }
            if (off >= 0xD000)                  { chargeSlow(); return lcRead(bank, off); }
        }
        int pb = (bank == 0) ? physBank01(off, false) : 1;
        return fastCell(pb, off);
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
        chargeSlow();                                 // slow side (Mega II)
        if (off >= 0xC000 && off <= 0xC0FF) { ioWrite(bank, off, v); return; }
        if (off >= 0xC100 && off <= 0xCFFF) return;   // slot ROM: read-only
        slowRam_[(size_t(bank - 0xE0) * 0x10000) + off] = v;
        return;
    }

    if (bank <= 0x01) {
        if (iolcShadow()) {
            if (off >= 0xC000 && off <= 0xC0FF) { chargeSlow(); ioWrite(bank, off, v); return; }
            if (off >= 0xC100 && off <= 0xCFFF) return;   // slot ROM: read-only
            if (off >= 0xD000)                  { chargeSlow(); lcWrite(bank, off, v); return; }
        }
        int pb = (bank == 0) ? physBank01(off, true) : 1;
        fastCell(pb, off) = v;
        if (maybeShadow(uint8_t(pb), off, v)) chargeSlow();   // shadowed write hits slow side too
        return;
    }

    if (bank <= 0x7F) { fastCell(bank, off) = v; return; }
    // $E2-$FB unmapped → drop
}
