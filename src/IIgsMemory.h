// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── IIgs memory bus: FPI (fast side) + Mega II (slow side) ────────────────
// M2: the 24-bit banked address space of the Apple IIgs.
//
//   $00-$7F   Fast RAM (FPI, 2.8 MHz). Banks $00/$01 are the "//e machine":
//             $C000-$CFFF I/O, $D000-$FFFF language card — when IOLC shadow
//             is on (SHAD_IOLC=0). Otherwise plain fast RAM.
//   $E0/$E1   Mega II slow RAM (1 MHz), 128 KB. Always carries live I/O +
//             LC + the legacy //e video image + Super Hi-Res buffer.
//   $FC-$FF   ROM (256 KB ROM 03) or $FE-$FF (128 KB ROM 01).
//
// Writes to the shadowed display regions of banks $00/$01 mirror into
// $E0/$E1 so the slow-side video generator sees them (SHADOW reg $C035).
// Source of truth: MAME apple2gs.cpp (shadow/speed/state semantics cited).
//
// The CPU's only bus hooks remain read8/write8(addr24) — stable since M1.

#ifndef POMIIGS_IIGSMEMORY_H
#define POMIIGS_IIGSMEMORY_H

#include "Iwm.h"
#include "Es5503.h"
#include "Scc8530.h"

#include <cstdint>
#include <vector>

class CPU65816;   // for VBL/IRQ assertion (non-owning back-pointer)

class IIgsMemory
{
public:
    static constexpr uint32_t kAddrMask = 0x00FFFFFF;   // 24-bit / 16 MB

    // Shadow register ($C035) bits — 1 = inhibit that region's shadowing.
    // Cited: MAME apple2gs.cpp:235-241.
    enum Shadow : uint8_t {
        SHAD_TXTPG1     = 0x01, SHAD_HIRESPG1 = 0x02, SHAD_HIRESPG2 = 0x04,
        SHAD_SUPERHIRES = 0x08, SHAD_AUXHIRES = 0x10, SHAD_TXTPG2   = 0x20,
        SHAD_IOLC       = 0x40,   // inhibit I/O + language card in banks $00/$01
    };
    // Speed register ($C036) bits — MAME apple2gs.cpp:242-248.
    enum Speed : uint8_t { SPEED_HIGH = 0x80, SPEED_POWERON = 0x40, SPEED_ALLBANKS = 0x10 };

    IIgsMemory();

    // ── configuration / boot ─────────────────────────────────────────────
    // Load a IIgs ROM (128 KB → banks $FE-$FF; 256 KB → $FC-$FF). Returns
    // false if the size is not a supported ROM image.
    bool loadRom(const std::vector<uint8_t>& rom);
    void setFastRamKB(uint32_t kb);   // total FPI RAM (banks $00+). Default 1 MB.
    void reset();                     // power-on-ish: clears RAM, resets MMU state

    // Advance the video/timing clock by `cpuCycles` (call after each CPU step).
    // Drives VBL / VERTCNT and the VBL interrupt flag. Approximate: 65 cycles
    // per scanline, 262 lines/frame (NTSC), VBL over lines 192-261.
    void tick(int cpuCycles);
    int  vpos() const;                // current scanline 0..261
    bool inVbl() const { return vpos() >= 192; }
    // Wire the CPU so the MMU can raise the VBL (and later DOC/scanline) IRQ.
    void setCpu(CPU65816* c) { cpu_ = c; }

    // Disk: mount a 5.25" image into the on-board IWM (slot 6).
    bool loadDisk525(const std::vector<uint8_t>& img, bool prodosOrder) {
        return iwm_.loadDisk525(img, prodosOrder);
    }
    Iwm& iwm() { return iwm_; }
    Es5503& doc() { return doc_; }
    Scc8530& scc() { return scc_; }

    // Flat 16 MB RAM mode: bypasses all banking/I/O so the CPU can be tested
    // in isolation against Tom Harte (which models a flat bus). POM2 pattern.
    void setTestMode(bool on);

    // ── the CPU's bus hooks (non-virtual, stable since M1) ───────────────
    uint8_t read8(uint32_t addr24);
    void    write8(uint32_t addr24, uint8_t v);

    // ── introspection (for the boot-trace harness / video / debugger) ────
    uint8_t shadowReg() const { return shadow_; }
    uint8_t speedReg()  const { return speed_; }
    bool    romLoaded() const { return !rom_.empty(); }
    uint32_t romBankBase() const { return romBankBase_; }
    const uint8_t* slowRam() const { return slowRam_.data(); }   // $E0/$E1 (video)
    uint8_t newVideo()  const { return newvideo_; }              // $C029
    bool    shrEnabled() const { return (newvideo_ & 0x80) != 0; }
    bool    text80()    const { return eightyCol_; }
    bool    page2()     const { return page2_; }
    // Legacy //e display mode (for the VGC).
    bool    textMode()  const { return textMode_; }
    bool    mixed()     const { return mixed_; }
    bool    hires()     const { return hires_; }
    bool    dhires()    const { return dhgr_ && eightyCol_; }

private:
    // Backing stores.
    bool testMode_ = false;           // flat-16MB bypass (CPU unit tests)
    std::vector<uint8_t> flat_;       // 16 MB, only allocated in test mode
    std::vector<uint8_t> fastRam_;    // banks $00.. (size = fastRamKB_)
    std::vector<uint8_t> slowRam_;    // 128 KB, banks $E0/$E1
    std::vector<uint8_t> rom_;        // 128 or 256 KB
    uint32_t fastRamKB_ = 1024;
    uint32_t romBankBase_ = 0xFC;     // $FC (256 KB) or $FE (128 KB)

    // MMU / soft-switch state.
    uint8_t  shadow_ = 0;             // $C035 (0 = all shadowing enabled)
    uint8_t  speed_  = 0;             // $C036
    uint8_t  state_  = 0;             // $C068 STATEREG composite
    uint8_t  newvideo_ = 0;           // $C029
    // //e paging soft switches (Mega II).
    bool altzp_ = false, ramrd_ = false, ramwrt_ = false, page2_ = false;
    bool store80_ = false, hires_ = false, intcxrom_ = false, slotc3rom_ = false;
    bool eightyCol_ = false, altchar_ = false;
    bool textMode_ = true, mixed_ = false, dhgr_ = false;   // $C050-$C053, $C05E/F
    // Language card.
    bool lcRamRead_ = false, lcRamWrite_ = false, lcBank2_ = true, lcPreWrite_ = false;
    // Keyboard latch.
    uint8_t kbdLatch_ = 0;
    // ADB GLU (HLE — $C024-$C027). The ROM's ADB self-test sends command bytes
    // to DATAREG ($C026), waits for CMDFULL ($C027 bit0) to clear, then waits
    // for data-ready ($C027 bit5) and reads the response. We accept commands
    // immediately and queue a trivial response so the handshake completes
    // (real keyboard/mouse routing lands later). See DEV § ADB.
    bool    adbDataReady_ = false;
    uint8_t adbResponse_ = 0;
    // Battery RAM ($C033/$C034 serial clock/BRAM interface).
    uint8_t clkData_ = 0, clkCtl_ = 0;
    uint8_t bram_[256] = {0};
    // Video / interrupt timing.
    static constexpr int kLineCycles = 65;
    static constexpr int kLines      = 262;
    uint64_t videoCycles_ = 0;
    int      lastVpos_ = 0;
    uint8_t  intflag_ = 0;            // $C046 INTFLAG (VBL=0x08, QUARTER=0x10)
    uint8_t  inten_ = 0;              // $C041 INTEN
    uint8_t  vgcint_ = 0;             // $C023 VGCINT
    Iwm      iwm_;                    // on-board 5.25" IWM ($C0E0-$C0EF)
    Es5503   doc_;                    // Ensoniq 5503 DOC (Sound GLU $C03C-$C03F)
    Scc8530  scc_;                    // SCC 8530 serial ($C038-$C03B)
    CPU65816* cpu_ = nullptr;         // non-owning; for IRQ assertion

    // helpers
    bool   iolcShadow() const { return !(shadow_ & SHAD_IOLC); }
    uint8_t ioRead(uint8_t bank, uint16_t off);
    void    ioWrite(uint8_t bank, uint16_t off, uint8_t v);
    void    applyDisplaySwitch(uint8_t r);   // $C050-$C05F toggles
    uint8_t& fastCell(uint32_t bank, uint16_t off);   // fast RAM cell (with mirroring for out-of-range)
    // //e main/aux redirection for a bank $00/$01 access: returns physical
    // bank 0 (main) or 1 (aux) per ALTZP / RAMRD / RAMWRT / 80STORE / PAGE2.
    int physBank01(uint16_t off, bool writing) const;
    uint8_t  lcRead(uint8_t bank, uint16_t off);
    void     lcWrite(uint8_t bank, uint16_t off, uint8_t v);
    void     lcSwitch(uint16_t off, bool writing);
    void     maybeShadow(uint8_t bank, uint16_t off, uint8_t v);
};

#endif // POMIIGS_IIGSMEMORY_H
