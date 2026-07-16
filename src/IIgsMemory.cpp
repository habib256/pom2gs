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
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <istream>
#include <ostream>
#include <string>

IIgsMemory::IIgsMemory() {
    slowRam_.assign(0x20000, 0);
    setFastRamKB(fastRamKB_);
}

void IIgsMemory::setFastRamKB(uint32_t kb) {
    fastRamKB_ = kb;
    fastRam_.assign(size_t(kb) * 1024, 0);
}

int IIgsMemory::vpos() const {
    return int((videoCycles_ / kMasterPerLine) % kLines);
}

void IIgsMemory::tick(int cpuCycles) {
    // ONE wall-clock timebase: master ticks (14.318 MHz) = CPU cycles × 5 fast /
    // 14 slow + the slow-side stall penalty (the beam keeps moving while the CPU
    // stalls). The video beam, the DOC, the ADB valves and the paddles all
    // advance on it. videoCycles_ used to count RAW CPU cycles: at 2.8 MHz the
    // beam scanned 2.8× too fast — VBL fired at ~164 Hz instead of 60, so every
    // VBL-clocked game engine ran ~2.8× fast (accelerated music, samples stopped
    // early — Captain Blood / Transylvania III). One video line = 65 slow cycles
    // × 14 = 910 master ticks; 262 lines = 238420 = exactly the frame target.
    // At reset the machine is SLOW (×14), so slow-side timing is unchanged.
    const uint32_t pen = uint32_t(slowPenMaster_ - slowPenSeen_);
    slowPenSeen_ = slowPenMaster_;
    const uint32_t mt = uint32_t(cpuCycles > 0 ? cpuCycles : 1) * (speedFast() ? 5u : 14u) + pen;
    videoCycles_ += mt;
    doc_.tickMaster(mt);
    mirrorDocIrq();
    const int vp = vpos();
    // Rising edge into VBL (line 192): latch the VBL flag and, if the VBL
    // interrupt is enabled ($C041 INTEN bit3), assert the CPU IRQ line.
    if (vp >= 192 && lastVpos_ < 192) {
        intflag_ |= 0x08;                                 // INTFLAG_VBL
        updateMega2Irq();
    }
    lastVpos_ = vp;

    // ADB mouse-interrupt safety valve: if a posted mouse sample isn't consumed
    // (ReadMouse via $C024) within a couple of frames — e.g. it arrived before
    // GS/OS installed the ADB mouse handler during early boot — drop it and
    // deassert IRQ_SRC_ADB so a never-serviced sample can't storm the ROM
    // interrupt manager. Normal servicing happens within microseconds.
    const uint64_t twoFrames = uint64_t(2) * kMasterPerLine * kLines;
    if (mouseDataFull_ && (videoCycles_ - mouseSetCycle_) > twoFrames) {
        mouseDataFull_ = false; mouseReadY_ = false; mouseDX_ = mouseDY_ = 0;
        updateAdbIrq();
    }
    if (kbdIntPending_ && (videoCycles_ - kbdSetCycle_) > twoFrames) {
        kbdIntPending_ = false; updateAdbIrq();
    }
}

// Recompute the two shared IRQ lines from the flag/enable registers. Both the
// VBL + ¼-second (Mega II) and the scan-line + 1-second (VGC) sources are
// wire-OR'd onto one CPU line each; assert only while an enabled source has a
// pending flag, so the level tracks the register state exactly.
void IIgsMemory::mirrorDocIrq() {
    const bool p = doc_.irqPending();
    if (p != docIrqLast_) { docIrqLast_ = p; if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_DOC, p); }
}

void IIgsMemory::updateMega2Irq() {
    const bool active = (intflag_ & inten_ & 0x18) != 0;      // VBL 0x08 | ¼-sec 0x10
    if (cpu_) cpu_->setIrqLine(CPU65816::IRQ_SRC_MEGA2_VBL, active);
}

// RTC seconds: the IIgs clock holds *local* time as seconds since 1 Jan 1904; the
// host counts UTC seconds since 1970, so add the 66-year epoch offset (2082844800)
// plus the local UTC offset (tm_gmtoff, e.g. +4 h) so the Control Panel shows wall
// time, not UTC. Byte 0 = LSB.
uint8_t IIgsMemory::rtcByte(int n) const {
    const std::time_t t = std::time(nullptr);
    long gmtoff = 0;
    if (const std::tm* lt = std::localtime(&t)) gmtoff = lt->tm_gmtoff;
    const uint32_t secs = uint32_t(int64_t(t) + gmtoff + 2082844800LL);
    return uint8_t(secs >> (8 * (n & 3)));
}

// Clock/BRAM serial transaction. One $C034 bit7 strobe = one byte clocked in or
// out of the chip; $C034 bit6 is that byte's direction (1 = read from chip). The
// command byte in $C033 (clocked in, bit6=0) splits into op = bits 6-4 and reg =
// bits 3-2 (decode per KEGS clock.c). op selects:
//   0        → the 4 RTC seconds registers (served from the host clock);
//   2        → battery RAM $10-$13   (single-byte address);
//   4-7      → battery RAM $00-$0F   (single-byte address);
//   3 & reg&2 → extended 256-byte BRAM (a second address byte follows);
//   3 & reg<2 → the internal test ($0) / write-protect ($1) registers.
// The command strobe only sets up the target; the firmware then clocks the data
// byte in/out on a SECOND strobe (read = bit6=1). Crucially the read value must be
// served on that data strobe, not the command strobe — the ROM clock driver and
// the toolbox ReadTimeHex both do command-then-read, so answering on the command
// strobe (and returning a dummy on the read) gave GS/OS 00s → a garbage date.
// BRAM/internal writes persist (ROM boot-time reinit + chip self-check stick).
void IIgsMemory::clockStrobe(uint8_t c034) {
    const bool rd = (c034 & 0x40) != 0;             // $C034 bit6: 1 = read byte from chip
    switch (clkState_) {
    case CLK_IDLE: {
        if (rd) break;                              // stray read strobe: hold last value
        const uint8_t cmd = clkData_;               // command byte (clocked in, bit6=0)
        clkRead_ = (cmd & 0x80) != 0;               // bit7: 1 = read, 0 = write
        const uint8_t reg = (cmd >> 2) & 0x03;      // bits 3-2: register within group
        const uint8_t op  = (cmd >> 4) & 0x07;      // bits 6-4: operation group
        if (op == 3 && (reg & 0x02)) {              // extended BRAM: a 2nd address byte
            clkSrc_ = SRC_BRAM;
            clkAddr_ = uint8_t((cmd & 0x03) << 6);  // address bits 7-6
            clkState_ = CLK_ADDR;
        } else {                                    // single-strobe address; data next strobe
            if (op == 0)        { clkSrc_ = SRC_SECONDS;  clkReg_ = reg; }
            else if (op == 3)   { clkSrc_ = SRC_INTERNAL; clkReg_ = reg; }   // test / WP
            else if (op == 2)   { clkSrc_ = SRC_BRAM; clkAddr_ = uint8_t(0x10 + reg); }
            else                { clkSrc_ = SRC_BRAM; clkAddr_ = uint8_t(((op & 3) << 2) | reg); }
            clkState_ = CLK_DATA;
        }
        break;
    }
    case CLK_ADDR:                                   // extended BRAM: 2nd byte = addr bits 5-0
        clkAddr_ = uint8_t(clkAddr_ | ((clkData_ >> 2) & 0x3F));
        clkState_ = CLK_DATA;
        break;
    case CLK_DATA:                                   // data phase — the firmware reads/writes
        // HERE, not on the command strobe: the IIgs clock driver clocks the command
        // in (bit6=0) then reads the byte out on a second strobe (bit6=1). Serve the
        // value now so ReadTimeHex/the Control Panel get the real seconds, not a dummy.
        if (rd) {
            clkData_ = (clkSrc_ == SRC_SECONDS)  ? rtcByte(clkReg_)
                     : (clkSrc_ == SRC_INTERNAL) ? clkInternal_[clkReg_ & 1]
                                                 : bram_[clkAddr_];
        } else if (clkSrc_ == SRC_BRAM) {
            bram_[clkAddr_] = clkData_;              // seconds/internal writes: host clock, ignore
        } else if (clkSrc_ == SRC_INTERNAL) {
            clkInternal_[clkReg_ & 1] = clkData_;
        }
        clkState_ = CLK_IDLE;
        break;
    }
}

// ADB mouse: accumulate host motion/button. A movement or button change makes
// data available ($C027 bit7) so the firmware/toolbox will read $C024 (X then Y)
// on its next poll — GS/OS's Event Manager reads the mouse off the already-firing
// VBL/heartbeat, so this is poll-based (no dedicated ADB IRQ, which the HLE'd ADB
// couldn't yet clear cleanly). MAME apple2gs keyglu mouse; IIgs Hardware Ref.
void IIgsMemory::mouseMove(int dx, int dy) {
    if (!dx && !dy) return;
    mouseDX_ += dx; mouseDY_ += dy;
    if (!mouseDataFull_) mouseSetCycle_ = videoCycles_;
    mouseDataFull_ = true; updateAdbIrq();
}
void IIgsMemory::mouseButton(bool down) {
    if (down != mouseBtn_) {
        mouseBtn_ = down;
        if (!mouseDataFull_) mouseSetCycle_ = videoCycles_;
        mouseDataFull_ = true; updateAdbIrq();
    }
}
// Host key → latch the ASCII at $C000 and post an ADB keyboard interrupt.
void IIgsMemory::keyEvent(uint8_t latch) {
    kbdLatch_ = latch;
    if (!kbdIntPending_) kbdSetCycle_ = videoCycles_;
    kbdIntPending_ = true;
    updateAdbIrq();
}

void IIgsMemory::updateAdbIrq() {
    // Hardware rule first: a source whose $C027 interrupt-ENABLE bit is set
    // (software-programmed — games do this) always delivers. The legacy
    // "system up" gate (native mode + VBL int enabled) is kept for the ROM/
    // GS/OS path, which never programs the enables and relies on the VBL-driven
    // poll — but still needs the line for the keyboard handler; asserting it
    // during early emulation-mode boot wedged the ROM (handlers not installed).
    if (!cpu_) return;
    const bool sysUp = !cpu_->getEmulationMode() && (inten_ & 0x08);   // native + VBL int on
    const bool deliver =
        (mouseDataFull_  && ((adbIntEn_ & 0x40) || sysUp)) ||         // mouse int en (b6)
        (kbdIntPending_  && ((adbIntEn_ & 0x04) || sysUp)) ||         // keyboard int en (b2)
        (adbDataReady_   &&  (adbIntEn_ & 0x10));                     // DATAREG int en (b4)
    cpu_->setIrqLine(CPU65816::IRQ_SRC_ADB, deliver);
}

// ── ADB µC command machine ($C026 writes — KEGS adb.c command set) ────────
// A write is a parameter if a command is pending, else a new command byte.
// Commands consume a fixed parameter count and queue fixed responses; unknown
// commands are swallowed parameterless (real µC ignores garbage).
void IIgsMemory::adbCommand(uint8_t v) {
    if (adbParamsLeft_ > 0) {                          // parameter byte
        if (adbParamCount_ < 8) adbParams_[adbParamCount_++] = v;
        if (--adbParamsLeft_ == 0) {
            switch (adbCmd_) {
                case 0x04: adbMode_ |= adbParams_[0];              break;   // set modes
                case 0x05: adbMode_ &= uint8_t(~adbParams_[0]);    break;   // clear modes
                case 0x09: adbQueue(0x00);                         break;   // read RAM → dummy
                default: break;                                    // config/sync/write: absorbed
            }
        }
        updateAdbIrq();
        return;
    }
    adbCmd_ = v; adbParamCount_ = 0;
    switch (v) {
        case 0x00: case 0x01: case 0x02: case 0x03: break;         // abort / reset kbd / flush
        case 0x04: case 0x05: case 0x11: adbParamsLeft_ = 1; break;// set/clear modes, send code
        case 0x06: adbParamsLeft_ = 3; break;                      // set config
        case 0x07: adbParamsLeft_ = (romBankBase_ == 0xFC) ? 8 : 4; break; // sync (ROM03: 8)
        case 0x08: case 0x09: adbParamsLeft_ = 2; break;           // write/read RAM
        case 0x0A: adbQueue(adbMode_); break;                      // read modes
        case 0x0B: adbQueue(0); adbQueue(0); adbQueue(0); adbQueue(0); break; // read config
        case 0x0D: adbQueue(0x06); break;                          // version byte (KEGS)
        case 0x0E: adbQueue(0x01); adbQueue(0x00); break;          // read charsets
        case 0x0F: adbQueue(0x01); adbQueue(0x00); break;          // read layouts
        case 0x10: break;                                          // reset system (ignored)
        default:   break;                                          // unknown: no params
    }
    updateAdbIrq();
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
    // DOC oscillator IRQ: safety re-mirror (the per-tick path is the fast one).
    mirrorDocIrq();
}

// ── Snapshot ─────────────────────────────────────────────────────────────
// Field-by-field binary blob, versioned by the Snapshot wrapper (Snapshot.cpp).
// Covers everything a resumed machine needs; deliberately skips host-side
// transients (speaker stamps, slow-penalty accumulators, diagnostics counters)
// which reset cleanly. Media are stored as paths and remounted on load.
namespace {
template <typename T> void put(std::ostream& os, const T& v) { os.write((const char*)&v, sizeof v); }
template <typename T> void get(std::istream& is, T& v)       { is.read((char*)&v, sizeof v); }
void putStr(std::ostream& os, const std::string& s) {
    uint32_t n = uint32_t(s.size()); put(os, n); os.write(s.data(), n);
}
std::string getStr(std::istream& is) {
    uint32_t n = 0; get(is, n); if (n > 4096) return {};
    std::string s(n, '\0'); is.read(&s[0], n); return s;
}
}

void IIgsMemory::saveState(std::ostream& os) const {
    put(os, fastRamKB_);
    os.write((const char*)fastRam_.data(), fastRam_.size());
    os.write((const char*)slowRam_.data(), slowRam_.size());
    // MMU / soft switches.
    put(os, shadow_); put(os, speed_); put(os, state_); put(os, newvideo_); put(os, txtColor_);
    put(os, altzp_); put(os, ramrd_); put(os, ramwrt_); put(os, page2_);
    put(os, store80_); put(os, hires_); put(os, intcxrom_); put(os, slotc3rom_);
    put(os, eightyCol_); put(os, altchar_); put(os, textMode_); put(os, mixed_); put(os, dhgr_);
    put(os, lcRamRead_); put(os, lcRamWrite_); put(os, lcBank2_); put(os, lcPreWrite_);
    put(os, kbdLatch_); put(os, diskReg_);
    os.write((const char*)paddle_, sizeof paddle_);
    os.write((const char*)button_, sizeof button_);
    // ADB / mouse / keyboard.
    put(os, adbDataReady_); put(os, adbResponse_);
    put(os, adbIntEn_); put(os, adbMode_);
    put(os, mouseDX_); put(os, mouseDY_); put(os, mouseBtn_); put(os, mouseDataFull_);
    put(os, mouseReadY_); put(os, keyMod_); put(os, kbdIntPending_); put(os, kbdIntStatus_);
    // Clock / BRAM.
    put(os, clkState_); put(os, clkSrc_); put(os, clkData_); put(os, clkCtl_);
    put(os, clkAddr_); put(os, clkReg_); os.write((const char*)clkInternal_, 2);
    put(os, clkRead_); os.write((const char*)bram_, sizeof bram_);
    // Video / interrupt timing.
    put(os, videoCycles_); put(os, lastVpos_); put(os, intflag_); put(os, inten_);
    put(os, vgcint_); put(os, frameCount_);
    // Subsystems + media.
    doc_.saveState(os);
    putStr(os, hdd_.path());
    putStr(os, disk35_.path());
}

bool IIgsMemory::loadState(std::istream& is) {
    uint32_t kb = 0; get(is, kb);
    if (kb != fastRamKB_) setFastRamKB(kb);
    is.read((char*)fastRam_.data(), fastRam_.size());
    is.read((char*)slowRam_.data(), slowRam_.size());
    get(is, shadow_); get(is, speed_); get(is, state_); get(is, newvideo_); get(is, txtColor_);
    get(is, altzp_); get(is, ramrd_); get(is, ramwrt_); get(is, page2_);
    get(is, store80_); get(is, hires_); get(is, intcxrom_); get(is, slotc3rom_);
    get(is, eightyCol_); get(is, altchar_); get(is, textMode_); get(is, mixed_); get(is, dhgr_);
    get(is, lcRamRead_); get(is, lcRamWrite_); get(is, lcBank2_); get(is, lcPreWrite_);
    get(is, kbdLatch_); get(is, diskReg_);
    is.read((char*)paddle_, sizeof paddle_);
    is.read((char*)button_, sizeof button_);
    get(is, adbDataReady_); get(is, adbResponse_);
    get(is, adbIntEn_); get(is, adbMode_);
    get(is, mouseDX_); get(is, mouseDY_); get(is, mouseBtn_); get(is, mouseDataFull_);
    get(is, mouseReadY_); get(is, keyMod_); get(is, kbdIntPending_); get(is, kbdIntStatus_);
    adbCmd_ = 0; adbParamsLeft_ = 0; adbParamCount_ = 0; adbRespN_ = adbRespI_ = 0;   // µC transients
    get(is, clkState_); get(is, clkSrc_); get(is, clkData_); get(is, clkCtl_);
    get(is, clkAddr_); get(is, clkReg_); is.read((char*)clkInternal_, 2);
    get(is, clkRead_); is.read((char*)bram_, sizeof bram_);
    get(is, videoCycles_); get(is, lastVpos_); get(is, intflag_); get(is, inten_);
    get(is, vgcint_); get(is, frameCount_);
    doc_.loadState(is);
    const std::string hddPath = getStr(is);
    const std::string d35Path = getStr(is);
    if (!is.good()) return false;
    // Remount media if it differs from what's in the drives right now.
    if (hddPath != hdd_.path())     { if (hddPath.empty()) hdd_.eject();     else hdd_.loadImage(hddPath); }
    if (d35Path != disk35_.path())  { if (d35Path.empty()) disk35_.eject();  else disk35_.loadImage(d35Path); }
    // Host-side transients reset; re-derive the shared IRQ lines from the
    // restored register state.
    spkEvents_.clear(); slowPenMaster_ = 0; slowPenSeen_ = 0;
    mouseSetCycle_ = kbdSetCycle_ = videoCycles_;
    disk35Changed_ = false; disk35SwitchIo_ = false;
    docIrqLast_ = false;
    updateMega2Irq(); updateVgcIrq(); updateAdbIrq(); mirrorDocIrq();
    return true;
}

// ── SmartPort HLE (slot-5 3.5" drive) ────────────────────────────────────
// The slot-5 ROM's dispatch entries are WDM traps; the CPU calls this on WDM.
void IIgsMemory::smartportTrap(uint8_t sig) {
    if (!cpu_) return;
    if (sig == 0xC6) prodosBlockCall();       // $Cn50 ProDOS block ($42-$47)
    else if (sig == 0xC5) smartportCall();    // $Cn53 SmartPort dispatch
    // any other WDM signature: NOP
}

// Set the CPU carry flag + A = error, the SmartPort/ProDOS return convention.
// The error is a *byte* in the low half of A; when the accumulator is 8-bit
// (emulation mode or M=1) the high half (the B "hidden" accumulator) must be
// PRESERVED, not cleared — GS/OS's emulation-mode SmartPort trampolines stash
// the caller's stack pointer high byte in B across the call (TSC → 8-bit stores)
// and reconstruct a 16-bit value from it afterward. Clearing B derailed the
// System 6 Installer: an 8-bit `LDA sp_lo` recombined with a zeroed B gave a
// page-0 stack, and the routine's closing RTL returned to $0000. (Diagnosed
// with tests/hdd_trace.)
static void spReturn(CPU65816* cpu, uint8_t err) {
    const bool a8 = cpu->getEmulationMode() || (cpu->getP() & 0x20);   // 8-bit accumulator
    cpu->setA(a8 ? uint16_t((cpu->getA() & 0xFF00) | err) : err);
    uint8_t p = cpu->getP();
    if (err) p |= 0x01; else p &= uint8_t(~0x01);   // C = error
    cpu->setP(p);
}

// ProDOS block call: cmd=$42, unit=$43, buffer=$44/$45 (bank 0), block=$46/$47.
void IIgsMemory::prodosBlockCall() {
    ++sp5Calls_;
    const uint8_t cmd  = read8(0x42);
    const uint16_t buf = uint16_t(read8(0x44) | (read8(0x45) << 8));
    const uint16_t blk = uint16_t(read8(0x46) | (read8(0x47) << 8));
    uint8_t err = 0;
    if (cmd != 0 && disk35SwitchIo_) {                // disk switched → $2E on first block I/O
        disk35SwitchIo_ = false; err = 0x2E;          // (ProDOS-8 path; see smartportCall)
    } else if (cmd == 0) {                            // STATUS → block count in X/Y
        size_t n = disk35_.blockCount(); if (n > 0xFFFF) n = 0xFFFF;
        cpu_->setX(uint16_t(n & 0xFF)); cpu_->setY(uint16_t((n >> 8) & 0xFF));
    } else if (cmd == 1) {                            // READ
        uint8_t b[512];
        if (disk35_.readBlock(blk, b)) for (int i = 0; i < 512; ++i) write8(uint16_t(buf + i), b[i]);
        else err = 0x27;                              // I/O error
    } else if (cmd == 2) {                            // WRITE
        uint8_t b[512]; for (int i = 0; i < 512; ++i) b[i] = read8(uint16_t(buf + i));
        if (!disk35_.writeBlock(blk, b)) err = disk35_.writeProtected() ? 0x2B : 0x27;
    } else err = 0x01;                                // bad command
    spReturn(cpu_, err);
}

// SmartPort dispatch: JSR here, then inline `cmd(1), paramListPtr`. `cmd & $40`
// selects the GS/OS **extended** form — and the two forms differ in the inline
// layout, not just the param-list contents:
//   * standard : `DFB cmd`  `DW  paramList`      → 2-byte (bank-0) pointer, 3
//                inline bytes, return skips 3.
//   * extended : `DFB cmd`  `DC I4'paramList'`   → 4-byte (bank-qualified, 24-bit)
//                pointer, 5 inline bytes, return skips 5; param-list fields
//                (status list / buffer / block) are also 4-byte.
// Reading the extended pointer as a 2-byte bank-0 pointer (the old code) fetched
// a zeroed param list from the wrong bank and left 2 inline bytes unconsumed —
// the caller then executed the pointer's high bytes as opcodes (a stray COP) and
// derailed. This surfaced on the System 6 Installer, which drives the 3.5" drive
// through extended SmartPort STATUS calls. (Diagnosed with tests/hdd_trace.)
void IIgsMemory::smartportCall() {
    ++sp5Calls_;
    const uint8_t  pbr = cpu_->getPBR();
    const uint16_t sp  = cpu_->getSP();
    // JSR pushed (return-1); the inline bytes follow that+1, in the caller PBR.
    uint16_t ret = uint16_t(read8(uint16_t(sp + 1)) | (read8(uint16_t(sp + 2)) << 8));
    auto codeRd = [&](uint16_t o) { return read8((uint32_t(pbr) << 16) | uint16_t(ret + o)); };
    const uint8_t cmd = codeRd(1);
    const bool ext = (cmd & 0x40) != 0;
    const uint8_t base = cmd & 0x3F;
    // Param-list base: 24-bit (4 inline bytes) for extended, bank-0 16-bit for
    // standard. `inlineLen` is how many bytes to skip on return (cmd + pointer).
    const uint32_t plp = ext
        ? (uint32_t(codeRd(2)) | (codeRd(3) << 8) | (codeRd(4) << 16) | (uint32_t(codeRd(5)) << 24)) & kAddrMask
        : uint32_t(codeRd(2) | (codeRd(3) << 8));
    const uint16_t inlineLen = ext ? 5 : 3;
    auto pRd = [&](uint32_t o) { return read8((plp + o) & kAddrMask); };

    const uint8_t unit = pRd(1);
    uint8_t err = 0;
    if (base == 0x00) {                               // STATUS
        uint32_t list; uint8_t code;
        if (ext) { list = uint32_t(pRd(2)) | (pRd(3) << 8) | (pRd(4) << 16) | (uint32_t(pRd(5)) << 24); code = pRd(6); }
        else     { list = uint32_t(pRd(2)) | (pRd(3) << 8);                                           code = pRd(4); }
        err = smartportStatus(unit, code, list, ext);
    } else if (base == 0x01 || base == 0x02) {        // READ / WRITE BLOCK
        uint32_t buf, blk;
        if (ext) { buf = uint32_t(pRd(2)) | (pRd(3) << 8) | (pRd(4) << 16) | (uint32_t(pRd(5)) << 24);
                   blk = uint32_t(pRd(6)) | (pRd(7) << 8) | (pRd(8) << 16) | (uint32_t(pRd(9)) << 24); }
        else     { buf = uint32_t(pRd(2)) | (pRd(3) << 8);
                   blk = uint32_t(pRd(4)) | (pRd(5) << 8) | (pRd(6) << 16); }
        // Disk-switched one-shot (KEGS just_ejected → $2E DISK SWITCHED). Returned on
        // the first block I/O after a swap/eject so GS/OS drops the cached volume and
        // re-reads block 2 to re-identify it by name. Deliberately NOT on STATUS — the
        // ~1 Hz STATUS poll would consume the one-shot before the file access.
        if (unit != 1) err = 0x28;                    // only unit 1 exists (no device)
        else if (disk35SwitchIo_) { err = 0x2E; disk35SwitchIo_ = false; }
        else if (base == 0x01) { uint8_t b[512]; if (disk35_.readBlock(blk, b)) for (int i = 0; i < 512; ++i) write8(buf + i, b[i]); else err = 0x27; }
        else                   { uint8_t b[512]; for (int i = 0; i < 512; ++i) b[i] = read8(buf + i); if (!disk35_.writeBlock(blk, b)) err = disk35_.writeProtected() ? 0x2B : 0x27; }
        if (!err) { cpu_->setX(0x00); cpu_->setY(0x02); }   // 512 bytes transferred
    } else if (base == 0x03) {                        // FORMAT — nothing to do (the image is
        if (unit != 1) err = 0x28;                    // already a fixed-size volume); succeed so
    } else if (base == 0x04) {                        // the Installer / Disk Utility can proceed.
        // CONTROL. The control code follows the control-list pointer (pRd(4) std /
        // pRd(6) ext). Code $04 = EJECT — actually eject the 3.5" (STATUS then reports
        // bit4=0, no disk) and arm the disk-switched latch, so the Installer's "insert
        // the next disk" prompt advances and the menu re-insert is picked up as $2E.
        // Returning the old "bad command" ($01) wedged that prompt.
        const uint8_t ctlCode = ext ? pRd(6) : pRd(4);
        if (unit != 1) err = 0x28;
        else if (ctlCode == 0x04) { disk35_.eject(); disk35Changed_ = true; disk35SwitchIo_ = true; }
    } else {
        err = 0x01;                                   // unsupported SmartPort command
    }
    spReturn(cpu_, err);
    // Skip the inline bytes: RTS returns to (stacked+1); we want cmdAddr+inlineLen.
    uint16_t nret = uint16_t(ret + inlineLen);
    write8(uint16_t(sp + 1), uint8_t(nret & 0xFF));
    write8(uint16_t(sp + 2), uint8_t(nret >> 8));
}

// SmartPort STATUS: unit 0 = controller (device count); unit 1 = the drive.
// code 0 = device status (status byte + 3-byte block count); code 3 = DIB
// (adds the 16-char name + type/subtype) — GS/OS reads this to identify it.
uint8_t IIgsMemory::smartportStatus(uint8_t unit, uint8_t code, uint32_t list, bool ext) {
    auto w = [&](uint32_t o, uint8_t v) { write8(list + o, v); };
    // The **extended** STATUS call ($40) uses a 4-byte block count; the standard
    // call uses 3. KEGS `smartport.c` handles this by writing the 4th byte then
    // shifting every following DIB field +1 (`status_ptr++`). Skipping that shift
    // put the device-type/subtype in the wrong slots and the System 6 Installer —
    // which enumerates drives with extended STATUS and reads the type/subtype
    // WORD, requiring $0001 for a 3.5" — looped forever. `bc` is the count width.
    const uint32_t bc = ext ? 4u : 3u;
    if (unit == 0) {                                  // controller status
        w(0, 1);                                      // one device attached
        for (uint32_t i = 1; i <= bc; ++i) w(i, 0);
        return 0;
    }
    // Only unit 1 exists (the single 3.5" drive). A STATUS to any higher unit must
    // fail — the System 6 Installer *scans* SmartPort units (1, 2, 3, …) and only
    // stops when it hits a non-existent one; answering every unit with a valid DIB
    // made the scan run forever. $28 = Bus/Device error (no device connected).
    if (unit > 1) return 0x28;
    const size_t n = disk35_.loaded() ? disk35_.blockCount() : 0;
    // Status byte: bit7 block / bit6 write / bit5 read / bit4 online (disk in drive) /
    // bit3 format / bit0 disk-switched. bit0 is the passive "switched since last STATUS"
    // hint; the media-change error GS/OS acts on is the $2E returned by the next block
    // READ (see smartportCall).
    uint8_t st = disk35_.loaded()
        ? uint8_t(0xF8 | (disk35_.writeProtected() ? 0x04 : 0x00))   // block/write/read/online/format
        : 0x00;
    if (disk35Changed_) { st |= 0x01; disk35Changed_ = false; }      // bit0 = disk switched
    w(0, st);
    for (uint32_t i = 0; i < bc; ++i) w(1 + i, uint8_t((n >> (8 * i)) & 0xFF));  // block count
    if (code == 3) {                                  // Device Information Block
        static const char name[17] = "POMIIGS 3.5     ";       // 16 chars
        const uint32_t o = 1 + bc;                    // ID-string-length offset (5 std / 6 ext)
        w(o, 16);                                     // ID string length
        for (int i = 0; i < 16; ++i) w(o + 1 + i, uint8_t(name[i]));
        // Device type/subtype. The SmartPort subtype byte's high bits (Firmware Ref
        // fig 7-7; A2 Tech Note "UniDisk 3.5 #2"): bit7 = supports disk-switched errors
        // ("removable, poll me"), bit6 = it's the unintelligent **Apple 3.5 Drive** that
        // needs the host `AppleDisk3.5` low-level driver.
        //   $00 UniDisk 3.5  : intelligent, but NOT disk-switched-capable → GS/OS treats
        //                      it as fixed and never polls it → an Installer disk swap is
        //                      never noticed (it answers the app from its cached VCR).
        //   $C0 Apple 3.5 Dr : pollable, but demands the AppleDisk3.5 driver — POMIIGS is
        //                      HLE (WDM-trap SmartPort, no real IWM) so that dialog crashes.
        //   $80              : bit7 only = disk-switched-capable (GS/OS polls it) WITHOUT
        //                      the Apple-3.5 driver bit. Intelligent + removable — what our
        //                      HLE drive actually is; POM2's SmartPortCard uses exactly this.
        // Pairs with the general-status bit0 (disk switched) + the $2E block-READ one-shot,
        // which GS/OS's now-armed poll consumes to invalidate the VCR and re-mount.
        // type at o+17, subtype o+18.
        w(o + 17, 0x01);                              // device type $01 = 3.5" disk
        w(o + 18, 0x80);                              // subtype $80 = removable/disk-switched, no driver
        w(o + 19, 0x01); w(o + 20, 0x00);             // version
    }
    return 0;
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
    // MAME apple2gs.cpp: "LC default state: read ROM, write enabled, Dxxx bank 2".
    lcRamRead_ = false; lcRamWrite_ = true; lcBank2_ = true; lcPreWrite_ = false;
    kbdLatch_ = 0;
    videoCycles_ = 0; lastVpos_ = 0; intflag_ = 0; inten_ = 0; vgcint_ = 0; frameCount_ = 0;
    if (cpu_) {                          // drop any interrupt lines we own
        cpu_->setIrqLine(CPU65816::IRQ_SRC_MEGA2_VBL, false);
        cpu_->setIrqLine(CPU65816::IRQ_SRC_VGC_VBL, false);
        cpu_->setIrqLine(CPU65816::IRQ_SRC_DOC, false);
        docIrqLast_ = false;              // keep the edge-tracked mirror in sync
    }
    adbDataReady_ = false; adbResponse_ = 0;
    adbIntEn_ = 0; adbMode_ = 0; adbCmd_ = 0; adbParamsLeft_ = 0; adbParamCount_ = 0;
    adbRespN_ = adbRespI_ = 0;
    clkData_ = 0; clkCtl_ = 0;
    clkState_ = CLK_IDLE;   // BRAM contents survive reset (battery-backed)
    mouseDX_ = mouseDY_ = 0; mouseBtn_ = false; mouseDataFull_ = false;
    mouseReadY_ = false; keyMod_ = 0; kbdIntPending_ = false;
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
    // The language-card RAM follows the physical bank: a bank-$01 access is the
    // aux 64K (pb=1) regardless of ALTZP; a bank-$00 access picks main/aux by
    // ALTZP (which aliases bank-0 ZP/stack/LC to aux). Ignoring the bank made
    // $01:Dxxx read main LC when ALTZP=0, so GS/OS's bank-1 LC code (JSR into
    // relocated routines) landed in the wrong bank and derailed.
    const int pb = (bank == 1 || altzp_) ? 1 : 0;
    if (off < 0xE000 && !lcBank2_)                  // $D000-$DFFF bank 1
        return fastCell(pb, off);
    if (off < 0xE000 && lcBank2_)                   // $D000-$DFFF bank 2 (aliased to $Cxxx window)
        return fastCell(pb, uint16_t(off - 0x1000));
    return fastCell(pb, off);                       // $E000-$FFFF (single bank)
}

void IIgsMemory::lcWrite(uint8_t bank, uint16_t off, uint8_t v) {
    if (!lcRamWrite_) return;                       // writes ignored unless LC write enabled
    const int pb = (bank == 1 || altzp_) ? 1 : 0;   // bank-1 LC = aux (see lcRead)
    if (off < 0xE000 && lcBank2_) { fastCell(pb, uint16_t(off - 0x1000)) = v; return; }
    fastCell(pb, off) = v;
}

// ── Mega II language card (banks $E0/$E1 $D000-$FFFF) ────────────────────
// MAME apple2gs.cpp lc_r/lc_w + the m_lcbank address-map views: the slow banks
// carry a FULL //e language card over slowRam_, sharing the $C08x state with the
// fast side:
//   * read-ROM mode → the $FF ROM image shows through (m_lcbank[0] = .rom()).
//   * RAM mode      → bank-2 $D000 lives in the otherwise-unused $C000-$CFFF
//     window (lc_r: m_megaii_ram[off + 0xc000]); $E000-$FFFF is single-banked.
//   * ALTZP swaps the $E0 handler onto the aux (E1) side (the Mega II is a real
//     //e: its LC honours ALTZP), and bank $E1 is always the aux side.
//   * writes are gated by the LC write-enable (lc_w: if (!m_lcwriteenable) return).
// The old flat model (plain slowRam_ reads/writes, no banking) merged LC bank 1
// and bank 2 into one region — GS/OS's ProDOS-8 launch glue in the $E0 LC (P8
// lives in LC bank 2) was aliased/corrupted, and the launch stub's RTL to
// $E0:D3FA executed garbage → BRK into the monitor (BASIC.System crash).
uint8_t IIgsMemory::slowLcRead(uint8_t bank, uint16_t off) {
    if (!lcRamRead_) {                              // ROM shows through (bank $FF image)
        uint32_t romTop = uint32_t(rom_.size()) - 0x10000;
        return rom_.empty() ? 0 : rom_[romTop + off];
    }
    const size_t base = (bank == 0xE1 || altzp_) ? 0x10000 : 0;
    if (off < 0xE000 && lcBank2_) return slowRam_[base + uint16_t(off - 0x1000)];
    return slowRam_[base + off];
}

void IIgsMemory::slowLcWrite(uint8_t bank, uint16_t off, uint8_t v) {
    if (!lcRamWrite_) return;                       // write-protected
    const size_t base = (bank == 0xE1 || altzp_) ? 0x10000 : 0;
    if (off < 0xE000 && lcBank2_) { slowRam_[base + uint16_t(off - 0x1000)] = v; return; }
    slowRam_[base + off] = v;
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
    // $Cn00-$CnFF per-slot windows: our emulated cards claim slots 5 (3.5") and
    // 7 (HDD); everything else serves the IIgs INTERNAL firmware image, which
    // lives at the same offsets in the bank-$FF ROM (FF:C100-CFFF — 80-column
    // firmware at $C300, real SmartPort at $C500, the $C800-$CFFF expansion
    // window, …). Returning 0 for unclaimed slots made BASIC.System's PR#3-style
    // JSR $C300 execute zeros ($00 = BRK) and drop into the monitor — the
    // GS/OS→ProDOS-8 launch crash. (KEGS moremem.c maps the same region from
    // g_rom_fc_ff; MAME apple2gs.cpp inh views ditto.)
    if (off < 0xC800) {                           // per-slot page $Cn00-$CnFF
        int slot = (off >> 8) & 0x0F;             // $C7xx → 7
        if (slot == hdd_.slot() && hdd_.loaded())
            return hdd_.romRead(uint8_t(off & 0xFF));
        // The 3.5" card's firmware is present even with NO disk in the drive — a
        // real drive's ROM doesn't vanish with the media. Gating it on loaded()
        // meant a `boot = hdd` start (empty 3.5") served the INTERNAL SmartPort
        // firmware instead, GS/OS enumerated a dead device (real-IWM driver, no
        // IWM 3.5" model) and never polled our HLE drive — menu inserts were
        // invisible (sp5=0/s in the POMDBG trace). STATUS reports "no disk"
        // (status byte 0, bit4 clear) until an image is inserted.
        if (slot == disk35_.slot())                         // $C5xx → 3.5" SmartPort
            return disk35_.romRead(uint8_t(off & 0xFF));
    }
    uint32_t romTop = uint32_t(rom_.size()) - 0x10000;      // bank $FF image
    return rom_.empty() ? 0 : rom_[romTop + off];
}

// ── $C000-$C0FF I/O ──────────────────────────────────────────────────────
uint8_t IIgsMemory::ioRead(uint8_t bank, uint16_t off) {
    (void)bank;
    const uint8_t r = off & 0xFF;
    if (std::getenv("ADBDBG") && cpu_ && r >= 0x24 && r <= 0x27) {   // TEMP diag
        static long k = 0;
        if (++k <= 120 || k % 1000 == 0)
            std::fprintf(stderr, "[ADB] rd $C0%02X @ %02X:%04X (#%ld)\n", r, cpu_->getPBR(), cpu_->getPC(), k);
    }
    if (r <= 0x0F) return kbdLatch_;                        // keyboard latch
    switch (r) {
        case 0x10: { uint8_t v = kbdLatch_ & 0x7F; kbdLatch_ &= 0x7F;   // clear strobe
                     if (kbdIntPending_) { kbdIntPending_ = false; updateAdbIrq(); } return v; }
        case 0x11: return lcBank2_ ? 0x80 : 0x00;           // RDLCBNK2
        case 0x12: return lcRamRead_ ? 0x80 : 0x00;         // RDLCRAM
        case 0x13: return ramrd_ ? 0x80 : 0x00;
        case 0x14: return ramwrt_ ? 0x80 : 0x00;
        case 0x15: return intcxrom_ ? 0x80 : 0x00;
        case 0x16: return altzp_ ? 0x80 : 0x00;
        case 0x17: return slotc3rom_ ? 0x80 : 0x00;
        case 0x18: return store80_ ? 0x80 : 0x00;
        // ── ADB GLU ──
        case 0x24: {                                        // MOUSEDATA: X then Y
            // b7 = button (0 = down), b6-0 = signed 7-bit delta for this axis.
            // First read returns X, second Y ($C027 bit1 toggle); the Y read
            // consumes the deltas and clears data-available ($C027 bit7).
            auto clamp7 = [](int v) { return v < -64 ? -64 : (v > 63 ? 63 : v); };
            int d = mouseReadY_ ? clamp7(mouseDY_) : clamp7(mouseDX_);
            uint8_t v = uint8_t(uint8_t(d) & 0x7F) | (mouseBtn_ ? 0x00 : 0x80);
            if (!mouseReadY_) mouseReadY_ = true;           // next read = Y
            else { mouseReadY_ = false; mouseDX_ = mouseDY_ = 0; mouseDataFull_ = false; updateAdbIrq(); }
            return v;
        }
        case 0x25: return keyMod_;                          // KEYMODREG (host modifiers)
        case 0x26:                                          // DATAREG
            // A pending key posts its $C026 status byte (routes the interrupt
            // manager to the keyboard handler, which reads the ASCII from $C000);
            // it stays pending until the $C010 strobe-clear. Otherwise drain the
            // µC command-response queue (adbCommand).
            if (kbdIntPending_) return kbdIntStatus_;
            {
                uint8_t v = 0;
                if (adbRespI_ < adbRespN_) v = adbResp_[adbRespI_++];
                if (adbRespI_ >= adbRespN_) { adbRespI_ = adbRespN_ = 0; adbDataReady_ = false; }
                updateAdbIrq();
                return v;
            }
        case 0x27:                                          // KMSTATUS
            // b7 mouse-data-available + b6 mouse-interrupt: the ROM interrupt
            // manager ($FF:BE31, entered every VBL) reads $C027 and, when both
            // are set, dispatches the mouse handler ($E10034 → ReadMouse) which
            // reads $C024 and posts a mouse event. b6 is a status flag (the ROM
            // never writes it), so we raise it with b7 on mouse activity; the
            // $C024 Y read clears both. No dedicated ADB IRQ is needed — the
            // ever-present VBL interrupt already drives the manager each frame.
            // Status bits + the latched software enables. b6 is ALSO raised with
            // b7 for the ROM path (it never programs the enables and its VBL
            // dispatch requires b7&b6 together).
            return uint8_t((mouseDataFull_ ? 0xC0 : 0) |    // b7 data avail (+b6 for the ROM)
                           (adbIntEn_ & 0x54) |             // latched int enables (b6/b4/b2)
                           ((adbDataReady_ || kbdIntPending_) ? 0x20 : 0) | // b5 DATAREG full
                           (mouseReadY_    ? 0x02 : 0));    // b1 mouse X/Y select
        case 0x33: return clkData_;                          // CLOCKDATA (RTC/BRAM serial)
        case 0x34: return clkCtl_;                           // CLOCKCTL (bit7=busy, always 0 = done)
        case 0x19: return inVbl() ? 0x80 : 0x00;            // RDVBL (MAME 1425)
        case 0x1A: return textMode_ ? 0x80 : 0x00;         // RDTEXT
        case 0x1B: return mixed_ ? 0x80 : 0x00;             // RDMIXED
        case 0x1C: return page2_ ? 0x80 : 0x00;
        case 0x1D: return hires_ ? 0x80 : 0x00;
        case 0x1E: return altchar_ ? 0x80 : 0x00;
        case 0x1F: return eightyCol_ ? 0x80 : 0x00;
        case 0x30:                                          // SPKR: toggle 1-bit speaker
            // ONLY $C030 — on the IIgs $C031 is DISKREG (MAME apple2gs.cpp), NOT a
            // partially-decoded speaker mirror like the classic Apple II. The ROM's
            // beep loop is a 16-bit `LDA $C030` (m=0), whose second bus access hits
            // $C031: toggling on both made every beep's toggles arrive in same-cycle
            // pairs that cancelled in the mixer — beeps rendered as single CLICKS
            // (the long-standing "random cracks").
            if (spkEvents_.size() < 65536) spkEvents_.push_back(videoCycles_);
            return 0;
        case 0x31: return diskReg_;                         // DISKREG (b7 = 3.5" sel, b6 = head)
        case 0x38: case 0x39: case 0x3A: case 0x3B: return scc_.read(r);    // SCC serial
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: {                       // Sound GLU
            uint8_t v = doc_.gluRead(r);
            mirrorDocIrq();                            // $E0 ack may have dropped the line
            return v;
        }
        case 0x22: return txtColor_;                        // SCREENCOLOR (text fg/bg)
        case 0x23: return vgcint_;                          // VGCINT
        case 0x29: return newvideo_;
        case 0x2E: return uint8_t(vpos() >> 1);             // VERTCNT (MAME 1467)
        case 0x2F: {                                        // HORIZCNT (HW Ref): bit7 = vertical
            // count bit0, bits 6-0 = the horizontal counter — one $00 state then
            // $40-$7F across the 65-cycle scan line. It was stuck at 0, so
            // raster-synced code (Battle Chess waits for `$C02F & $1F` to enter a
            // beam window at $00:AB26) spun forever on a frozen beam.
            const uint32_t h = uint32_t((videoCycles_ % uint64_t(kMasterPerLine)) / 14);   // 0..64
            return uint8_t(uint32_t((vpos() & 1) << 7) | (h == 0 ? 0u : 0x3Fu + h));
        }
        case 0x35: return shadow_;
        case 0x36: return speed_;
        // ── Joystick: buttons + paddles ──
        case 0x61: return button_[0] ? 0x80 : 0x00;         // PB0 / open-apple
        case 0x62: return button_[1] ? 0x80 : 0x00;         // PB1 / solid-apple
        case 0x63: return button_[2] ? 0x80 : 0x00;         // PB2
        case 0x64: case 0x65: case 0x66: case 0x67: {       // PADDL0-3: RC timing
            uint64_t elapsed = videoCycles_ - paddleReset_;                    // master ticks
            return (elapsed < uint64_t(paddle_[r - 0x64]) * 11 * 14) ? 0x80 : 0x00;   // 11 slow cyc/count
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
    // $C071-$C07F are RESERVED soft switches whose *reads return internal ROM*
    // (Apple IIgs Hardware Reference, memory map). The ROM's native-mode
    // interrupt vectors ($FFE4-$FFEF) point here: e.g. IRQ=$C074 is `CLV / JML
    // $E10010`. Without this, every hardware interrupt vectors into floating-bus
    // garbage and GS/OS crashes just after it enables the VBL IRQ ($C041 bit3).
    // The internal $C0xx ROM image is the top ROM bank ($FF); map bank-0 offset
    // straight into it. MAME apple2gs.cpp c000_r reserved-range handling.
    if (r >= 0x71 && r <= 0x7F && !rom_.empty())
        return rom_[(uint32_t(0xFF - romBankBase_) << 16) + off];
    // display / paging soft switches with read side-effects
    if (r >= 0x80 && r <= 0x8F) { lcSwitch(r, false); return 0; }
    if (r >= 0x50 && r <= 0x5F) { applyDisplaySwitch(r); return 0; }
    if (r >= 0xD0 && r <= 0xDF) return disk35_.deviceRead(r & 0x0F);                  // slot 5 3.5" device-select
    if (r >= 0xE0 && r <= 0xEF) return iwm_.access(r - 0xE0, false, 0, videoCycles_); // slot 6 IWM
    if (r >= 0xF0 && r <= 0xFF) return hdd_.deviceRead(r & 0x0F);                     // slot 7 HDD device-select
    return 0;   // floating bus (approx)
}

void IIgsMemory::ioWrite(uint8_t bank, uint16_t off, uint8_t v) {
    if (std::getenv("ADBDBG") && cpu_) {                        // TEMP diag
        const uint8_t rr = off & 0xFF;
        if (rr >= 0x24 && rr <= 0x27) { static long k = 0;
            if (++k <= 120 || k % 1000 == 0)
                std::fprintf(stderr, "[ADB] wr $C0%02X = %02X @ %02X:%04X (#%ld)\n",
                             rr, v, cpu_->getPBR(), cpu_->getPC(), k); }
    }
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
        case 0x10: kbdLatch_ &= 0x7F;                        // KBDSTRB write also clears strobe
                   if (kbdIntPending_) { kbdIntPending_ = false; updateAdbIrq(); } return;
        case 0x26: adbCommand(v); return;                   // DATAREG: µC command/parameter byte
        case 0x27:                                          // KMSTATUS: latch the int-enable bits
            adbIntEn_ = uint8_t(v & 0x54);                  // b6 mouse / b4 data / b2 keyboard
            updateAdbIrq();
            return;
        case 0x33: clkData_ = v; return;                    // CLOCKDATA (command/address/data)
        case 0x34: clkCtl_ = v & 0x6F;                      // border colour (b0-3) + clock ctl
                   if (v & 0x80) clockStrobe(v);            // b7 = start/continue a transaction
                   return;
        case 0x30:                                          // SPKR: toggle 1-bit speaker
            if (spkEvents_.size() < 65536) spkEvents_.push_back(videoCycles_);
            return;
        case 0x31: diskReg_ = v; return;                    // DISKREG (see ioRead; no IWM 3.5" yet)
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
    if (r >= 0xD0 && r <= 0xDF) { disk35_.deviceWrite(r & 0x0F, v); return; }             // slot 5 3.5" device-select
    if (r >= 0xE0 && r <= 0xEF) { iwm_.access(r - 0xE0, true, v, videoCycles_); return; } // slot 6 IWM
    if (r >= 0xF0 && r <= 0xFF) { hdd_.deviceWrite(r & 0x0F, v); return; }                // slot 7 HDD device-select
}

// Writes to shadowed display regions of banks $00/$01 mirror into $E0/$E1 so
// the slow-side video generator sees them (SHADOW reg gates each region).
bool IIgsMemory::maybeShadow(uint8_t bank, uint16_t off, uint8_t v) {
    if (bank > 1) return false;
    // Bank $01 $2000-$9FFF is the Super Hi-Res buffer; its shadowing is gated by
    // SHAD_SUPERHIRES (bit3), *independent* of the Hi-Res page bits — and the SHR
    // range overlaps the Hi-Res ranges ($2000-$5FFF). GS/OS runs SHR with the
    // Hi-Res bits SET (inhibited) but SUPERHIRES CLEAR (shadow on), so the SHR
    // gate must be OR'd into the overlapping ranges; otherwise fast-side SHR
    // writes (e.g. the Finder menu bar drawn to $01:2xxx) never reach $E1 and the
    // display shows a stale image. MAME apple2gs.cpp shadow_w.
    const bool shr = (bank == 1) && !(shadow_ & SHAD_SUPERHIRES);
    bool doit = false;
    if (off >= 0x0400 && off <= 0x07FF) doit = !(shadow_ & SHAD_TXTPG1);              // text/lores page 1
    else if (off >= 0x2000 && off <= 0x3FFF) doit = !(shadow_ & SHAD_HIRESPG1) || shr;// hires page 1 / SHR
    else if (off >= 0x4000 && off <= 0x5FFF) doit = !(shadow_ & SHAD_HIRESPG2) || shr;// hires page 2 / SHR
    else if (off >= 0x6000 && off <= 0x9FFF) doit = shr;                              // SHR upper half
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
        if (off >= 0xD000)                  return slowLcRead(bank, off);   // Mega II language card
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
        if (off >= 0xD000)                  { slowLcWrite(bank, off, v); return; }   // Mega II language card
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
