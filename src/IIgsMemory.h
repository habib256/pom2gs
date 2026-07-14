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
#include "ProDosHdd.h"

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

    // CPU cycles to execute per 60 Hz video frame, selected by the SPEED
    // register ($C036 bit7). The slow side (Mega II, 1.02 MHz) runs exactly one
    // video frame per host frame = kLines × kLineCycles = 17030 cycles; the
    // fast side (FPI, 2.8 MHz) is 14/5× faster → 47684. Legacy //e software
    // that selects slow mode therefore runs at the correct 1 MHz, instead of
    // the fixed 2.8 MHz budget that made it ~2.7× too fast.
    // Cited: MAME apple2gs.cpp SPEED ($C036 bit7 = 2.8 MHz).
    int frameCycleBudget() const {
        const int slow = kLineCycles * kLines;                   // 17030 @ 1.02 MHz
        return (speed_ & SPEED_HIGH) ? (slow * 14 / 5) : slow;   // 47684 : 17030
    }
    // Wire the CPU so the MMU can raise the VBL (and later DOC/scanline) IRQ.
    void setCpu(CPU65816* c) { cpu_ = c; }

    // Host keyboard → the classic $C000 latch (bit7 = strobe). The Mega II
    // presents ADB keys here for 8-bit software; $C010 clears the strobe.
    void keyDown(uint8_t ascii) { kbdLatch_ = uint8_t(ascii | 0x80); }

    // Host joystick → paddles ($C064-$C067, RC-timed via $C070) + push buttons
    // ($C061-$C063, bit7 = pressed). Axes are 0..255 (128 = centre).
    void setPaddle(int n, uint8_t v) { if (n >= 0 && n < 4) paddle_[n] = v; }
    void setButton(int n, bool down) { if (n >= 0 && n < 3) button_[n] = down; }

    // Speaker ($C030): the audio mixer drains the toggle cycle-stamps each
    // frame to reconstruct the 1-bit square wave. `cur` is the current level.
    void takeSpeakerEvents(std::vector<uint64_t>& out) { out.swap(spkEvents_); spkEvents_.clear(); }
    uint64_t audioCycles() const { return videoCycles_; }
    Es5503& docChip() { return doc_; }

    // Disk: mount a 5.25" image into the on-board IWM (slot 6).
    bool loadDisk525(const std::vector<uint8_t>& img, bool prodosOrder) {
        return iwm_.loadDisk525(img, prodosOrder);
    }
    // Mount a ProDOS hard-disk image (.hdv/.po/.2mg) on the slot-7 HDD card.
    bool loadHdd(const std::string& path) { return hdd_.loadImage(path); }
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
    uint8_t textColor() const { return txtColor_; }              // $C022: fg = hi nibble, bg = lo
    bool    page2()     const { return page2_; }
    // Legacy //e display mode (for the VGC).
    bool    textMode()  const { return textMode_; }
    bool    mixed()     const { return mixed_; }
    bool    hires()     const { return hires_; }
    bool    dhires()    const { return dhgr_ && eightyCol_; }
    // Display page for the video scanner, honouring the 80STORE quirk (Sather
    // "Understanding the Apple IIe" §5-25 table 5.10 / MAME use_page_2): PAGE2
    // only steers the scanner to page 2 when it is NOT repurposed as the
    // aux-bank select. With 80STORE on, PAGE2 routes writes to main/aux and the
    // display is forced to page 1 — this is how DHGR (and 80STORE HGR page-flip
    // fades, e.g. Total Replay) address aux memory without moving the display.
    bool    textPage2() const { return page2_ && !store80_; }
    bool    hgrPage2()  const { return page2_ && !(store80_ && hires_); }

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
    uint8_t  txtColor_ = 0xF0;        // $C022 SCREENCOLOR — white fg / black bg at boot
    // //e paging soft switches (Mega II).
    bool altzp_ = false, ramrd_ = false, ramwrt_ = false, page2_ = false;
    bool store80_ = false, hires_ = false, intcxrom_ = false, slotc3rom_ = false;
    bool eightyCol_ = false, altchar_ = false;
    bool textMode_ = true, mixed_ = false, dhgr_ = false;   // $C050-$C053, $C05E/F
    // Language card.
    bool lcRamRead_ = false, lcRamWrite_ = false, lcBank2_ = true, lcPreWrite_ = false;
    // Keyboard latch.
    uint8_t kbdLatch_ = 0;
    // Joystick / paddles.
    uint8_t  paddle_[4] = {128, 128, 128, 128};
    bool     button_[3] = {false, false, false};
    uint64_t paddleReset_ = 0;      // videoCycles_ at the last $C070 strobe
    // Speaker ($C030): absolute cycle-stamps of level toggles this frame.
    std::vector<uint64_t> spkEvents_;
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
    ProDosHdd hdd_{7};                // ProDOS hard-disk card in slot 7
    CPU65816* cpu_ = nullptr;         // non-owning; for IRQ assertion

    // helpers
    bool   iolcShadow() const { return !(shadow_ & SHAD_IOLC); }
    uint8_t ioRead(uint8_t bank, uint16_t off);
    void    ioWrite(uint8_t bank, uint16_t off, uint8_t v);
    void    applyDisplaySwitch(uint8_t r);   // $C050-$C05F toggles
    uint8_t slotRomRead(uint16_t off);       // $C100-$CFFF slot/internal ROM
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
