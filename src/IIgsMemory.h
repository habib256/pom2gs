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
#include <iosfwd>
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
    enum Speed : uint8_t { SPEED_HIGH = 0x80, SPEED_POWERON = 0x40, SPEED_ALLBANKS = 0x10,
                           SPEED_DISKIISL4 = 0x01, SPEED_DISKIISL5 = 0x02, SPEED_DISKIISL6 = 0x04, SPEED_DISKIISL7 = 0x08 };

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
        return speedFast() ? (slow * 14 / 5) : slow;             // 47684 : 17030
    }
    // Fast only if $C036 bit7 is set AND no motor-detect slot with its bit set has a
    // spinning drive: the FPI drops to 1 MHz during 5.25" Disk II access so copy-
    // protected/timing-sensitive titles bit-cell correctly. MAME apple2gs update_speed()
    // `(m_speed & SPEED_HIGH) && !(m_speed & m_motors_active)`. Slot 6 = on-board IWM.
    bool speedFast() const {
        if (!(speed_ & SPEED_HIGH)) return false;
        if ((speed_ & SPEED_DISKIISL6) && iwm_.motorOn()) return false;
        return true;
    }
    // Master-clock (14.318 MHz) ticks per 60 Hz video frame = one Mega II frame
    // (kLines × kLineCycles slow cycles × 14 master). The host loop runs CPU
    // steps — each costing 5 master (fast) or 14 (slow), plus the slow-side
    // penalty — until this target, so mid-frame speed switches are honoured.
    long masterPerFrame() const { return long(kLineCycles) * kLines * 14; }   // 238420

    // Called once per host frame: drives the periodic Mega II / VGC interrupts
    // (VBL edge stays in tick(); this adds ¼-second, 1-second, scan-line).
    void frameTick();

    // WDM ($42) trap from the CPU: the slot-5 SmartPort ROM's dispatch entries
    // are `WDM $C5` (SmartPort call) and `WDM $C6` (ProDOS block call). Executes
    // the disk operation on the slot-5 3.5" drive in C++ (STATUS / READBLOCK /
    // WRITEBLOCK, standard + GS/OS extended long-address form) and returns via
    // the CPU registers. Other WDM signatures are ignored (NOP).
    void smartportTrap(uint8_t sig);

    // Per-access slow-side penalty. In FAST mode (2.8 MHz) any access that
    // lands on the Mega II slow side — banks $E0/$E1, the $Cxxx I/O + slot ROM
    // + language card of banks $00/$01, and shadowed video writes — is stretched
    // to the 1.02 MHz clock: 14 master ticks instead of the fast side's 5, i.e.
    // 9 extra master ticks per slow access. The CPU already charged the access
    // as 1 fast cycle (5 master); read8/write8 accrue the +9 here. The main loop
    // drains this each step (in fast-cycle units, 5 master each) and adds it to
    // the frame budget, so slow-side-heavy code correctly throttles toward
    // 1 MHz even in fast mode. In slow mode the whole CPU is already 1 MHz, so
    // nothing is charged. Cited: MAME apple2gs.cpp (fast/slow clock sync).
    // Drain the accrued slow-side penalty, in master-clock ticks (the host loop
    // accounts the whole frame in master ticks).
    int takeSlowPenalty() { int m = int(slowPenMaster_); slowPenMaster_ = 0; slowPenSeen_ = 0; return m; }
    // Wire the CPU so the MMU can raise the VBL (and later DOC/scanline) IRQ.
    void setCpu(CPU65816* c) { cpu_ = c; }

    // Host keyboard → the classic $C000 latch (bit7 = strobe) + an ADB keyboard
    // interrupt. GS/OS reads the ASCII from $C000 in an interrupt handler (it
    // never polls $C000), so a key must both latch the ASCII and raise the ADB
    // IRQ; the $C010 strobe-clear consumes the event. See keyEvent()/DEV § ADB.
    void keyDown(uint8_t ascii) { keyEvent(uint8_t(ascii | 0x80)); }
    void keyEvent(uint8_t latch);
    // Live any-key-down state → $C010 (KBDSTRB) bit7. The host loop sets it each
    // frame from the physical key state (edge-based char events can't model a held
    // key). Matches MAME apple2gs.cpp $C010 = GLU_ANY_KEY_DOWN bit7.
    void setAnyKeyDown(bool d) { anyKeyDown_ = d; }
    void setKbdIntStatus(uint8_t s) { kbdIntStatus_ = s; }   // dev: tune the $C026 keycode byte

    // Host keyboard modifiers → ADB KEYMODREG ($C025). Bit layout (Apple IIgs
    // Hardware Reference, ADB GLU): b7 command(⌘/open-apple), b6 option(solid-
    // apple), b4 keypad, b3 repeat, b2 caps-lock, b1 control, b0 shift.
    void setKeyModifiers(uint8_t mod) { keyMod_ = mod; }

    // Host mouse → ADB GLU mouse register ($C024 MOUSEDATA, status $C027). The
    // firmware/toolbox reads X then Y (7-bit signed delta + button in bit7),
    // gated by the mouse-data-available bit. Deltas accumulate between reads.
    void mouseMove(int dx, int dy);
    void mouseButton(bool down);

    // Host joystick → paddles ($C064-$C067, RC-timed via $C070) + push buttons
    // ($C061-$C063, bit7 = pressed). Axes are 0..255 (128 = centre).
    void setPaddle(int n, uint8_t v) { if (n >= 0 && n < 4) paddle_[n] = v; }
    void setButton(int n, bool down) { if (n >= 0 && n < 3) button_[n] = down; }

    // Speaker ($C030): the audio mixer drains the toggle cycle-stamps each
    // frame to reconstruct the 1-bit square wave. `cur` is the current level.
    void takeSpeakerEvents(std::vector<uint64_t>& out) { out.swap(spkEvents_); spkEvents_.clear(); }
    uint64_t audioCycles() const { return videoCycles_; }
    Es5503& docChip() { return doc_; }

    // Disk: mount a 5.25" image into the on-board IWM (slot 6). Path-based
    // (POM2 DiskImage: .dsk/.do/.po/.nib/.d13/.2mg/.woz, write-back).
    bool loadDisk525(const std::string& path) { return iwm_.loadDisk525(path); }
    void ejectDisk525() { iwm_.eject(); }
    // Mount a ProDOS hard-disk image (.hdv/.po/.2mg) on the slot-7 HDD card.
    bool loadHdd(const std::string& path) { return hdd_.loadImage(path); }
    // Mount an 800K 3.5" image (.po/.2mg). Default = slot-5 SmartPort HLE
    // (block-level, ProDosHdd WDM-trap ROM). With setIwm35(true) the image
    // goes to the REAL IWM Sony 3.5" drive instead and the genuine internal
    // ROM firmware at $C500 drives it (KEGS/MAME LLE path).
    // `drive` 0/1 = the two internal Sony drives (drive 1 needs iwm35_ —
    // the SmartPort HLE models a single unit).
    bool loadDisk35(const std::string& path, int drive = 0) {
        if (iwm35_) return iwm_.loadDisk35(path, drive);
        return drive == 0 && disk35_.loadImage(path);
    }
    void ejectHdd() { hdd_.eject(); }        // clear slot 7 so a slot-5 3.5" disk boots
    void ejectDisk35(int drive = 0) {
        if (iwm35_) { iwm_.ejectDisk35(drive); return; }
        if (drive != 0) return;
        disk35_.eject(); disk35Changed_ = true; disk35SwitchIo_ = true;
    }
    // Route 3.5" media to the real IWM Sony drive (vs SmartPort HLE).
    void setIwm35(bool on) { iwm35_ = on; }
    bool iwm35() const { return iwm35_; }
    bool hddLoaded() const { return hdd_.loaded(); }
    bool hddBootable() const { return hdd_.bootable(); }   // block-0 byte 0 == $01
    // Hot-swap the 3.5" without a reset (Installer disk swaps): change the image and
    // arm the disk-switched signal. The media-change signal is a one-shot SmartPort
    // **$2E** returned on the next block READ/WRITE (KEGS' just_ejected), which makes
    // GS/OS drop the cached volume and re-read block 2 to re-identify it by name (the
    // Installer's "insert SystemTools1" prompt). NB: only on READ/WRITE, *not* STATUS
    // — GS/OS STATUS-polls the drive continuously, and firing $2E on STATUS lets that
    // poll consume the one-shot before the actual file access, so the swap goes
    // unnoticed. STATUS reports it as bit0 of the status byte instead. See smartportCall.
    bool swapDisk35(const std::string& path, int drive = 0) {
        if (iwm35_) return iwm_.loadDisk35(path, drive);   // Sony35 arms its own switched flag
        if (drive != 0) return false;                      // HLE models a single unit
        bool ok = disk35_.loadImage(path);
        disk35Changed_ = true; disk35SwitchIo_ = true;
        return ok;
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
    uint32_t sp5Calls() const { return sp5Calls_; }   // slot-5 SmartPort/ProDOS calls (poll liveness)

    // Snapshot: RAM + MMU/softswitch/LC/video-timing/ADB/clock/BRAM + DOC +
    // the mounted media paths (remounted on load if they differ). Transients
    // (speaker stamps, slow-penalty accumulators, diagnostics) reset on load.
    void saveState(std::ostream& os) const;
    bool loadState(std::istream& is);
    uint8_t speedReg()  const { return speed_; }
    bool    romLoaded() const { return !rom_.empty(); }
    uint32_t romBankBase() const { return romBankBase_; }
    const uint8_t* slowRam() const { return slowRam_.data(); }   // $E0/$E1 (video)
    const uint8_t* bram() const { return bram_; }                // 256-byte battery RAM
    uint8_t newVideo()  const { return newvideo_; }              // $C029
    bool    shrEnabled() const { return (newvideo_ & 0x80) != 0; }
    bool    text80()    const { return eightyCol_; }
    uint8_t textColor() const { return txtColor_; }              // $C022: fg = hi nibble, bg = lo
    uint8_t borderColor() const { return clkCtl_ & 0x0F; }       // $C034 bits0-3 (16-colour index)
    bool    page2()     const { return page2_; }
    // CPU hardware-vector pull ($00FFE4-$00FFFF). On the IIgs these always read
    // ROM even when the language card has RAM banked in at $E000-$FFFF: GS/OS
    // runs with LC RAM read-enabled but never installs RAM vectors — it reaches
    // its interrupt handlers through the fixed ROM stubs at $C071-$C07F. Without
    // this, every IRQ under GS/OS pulls an uninitialised-RAM vector and crashes.
    uint8_t vectorPull(uint16_t off) {
        // Flat-bus CPU unit tests (Tom Harte) have no ROM: fall back to the
        // normal bus so BRK/COP vector reads still hit the test's RAM image.
        if (testMode_ || rom_.empty()) return read8(off);
        return rom_[(uint32_t(0xFF - romBankBase_) << 16) + off];
    }
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
    uint32_t fastRamKB_ = 8192;   // full fast side $00-$7F (ROM 03 max 8 MB)
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
    bool lcRamRead_ = false, lcRamWrite_ = true, lcBank2_ = true, lcPreWrite_ = false;
    // Keyboard latch.
    uint8_t kbdLatch_ = 0;
    bool    anyKeyDown_ = false;           // live physical key-held state → $C010 bit7
    // Joystick / paddles.
    uint8_t  paddle_[4] = {128, 128, 128, 128};
    bool     button_[3] = {false, false, false};
    uint64_t paddleReset_ = 0;      // videoCycles_ at the last $C070 strobe
    // Speaker ($C030): absolute cycle-stamps of level toggles this frame.
    std::vector<uint64_t> spkEvents_;
    // Slow-side access penalty accumulator, in master-clock ticks (14.318 MHz).
    long slowPenMaster_ = 0;
    long slowPenSeen_ = 0;    // high-water for tick()'s per-instruction penalty delta (DOC timing)
    bool docIrqLast_ = false; // last DOC IRQ level mirrored to the CPU (edge-only updates)
    uint32_t sp5Calls_ = 0;   // slot-5 device-call counter (diagnostics)
    uint8_t  diskReg_ = 0;    // $C031 DISKREG (b6 = 35SEL, b7 = HDSEL) — mirrored into iwm_
    bool     iwm35_   = false; // 3.5" media on the real IWM Sony drive (vs SmartPort HLE)
    // Single edge-tracked mirror of the DOC IRQ line — EVERY path that can change
    // the pend state must go through this (a direct setIrqLine call elsewhere would
    // desync docIrqLast_ and a later re-assert would be skipped).
    void mirrorDocIrq();
    static constexpr int kSlowExtraMaster = 9;   // 14 (slow cycle) − 5 (fast cycle)
    // Charge one slow-side access (no-op unless in 2.8 MHz fast mode).
    void chargeSlow() { if (speed_ & SPEED_HIGH) slowPenMaster_ += kSlowExtraMaster; }
    // ADB GLU (HLE — $C024-$C027). The ROM's ADB self-test sends command bytes
    // to DATAREG ($C026), waits for CMDFULL ($C027 bit0) to clear, then waits
    // for data-ready ($C027 bit5) and reads the response. We accept commands
    // immediately and queue a trivial response so the handshake completes
    // (real keyboard/mouse routing lands later). See DEV § ADB.
    bool    adbDataReady_ = false;
    uint8_t adbResponse_ = 0;
    // ADB µC command machine (KEGS adb.c HLE): a $C026 write is either a new
    // command byte or a parameter for the pending one; responses queue up and
    // are drained by $C026 reads. $C027 writes latch the interrupt-ENABLE bits
    // (b6 mouse, b4 data, b2 keyboard) — games program these and then take the
    // ADB IRQ instead of polling (Arkanoid writes $30, Battle Chess talks the
    // µC directly; the old stub ignored both → dead in-game mice).
    uint8_t adbIntEn_ = 0;                 // latched $C027 enable bits ($54 mask)
    uint8_t adbMode_ = 0;                  // µC modes byte (cmd $04 set / $05 clear)
    uint8_t adbCmd_ = 0;                   // pending command
    int     adbParamsLeft_ = 0;            // parameter bytes still expected
    uint8_t adbParams_[8] = {0};
    int     adbParamCount_ = 0;
    uint8_t adbResp_[16] = {0};            // response queue
    int     adbRespN_ = 0, adbRespI_ = 0;
    void    adbCommand(uint8_t v);         // handle a $C026 write
    void    adbQueue(uint8_t v) { if (adbRespN_ < 16) adbResp_[adbRespN_++] = v; adbDataReady_ = true; }
    // ADB mouse ($C024 MOUSEDATA / $C027 KMSTATUS) + keyboard modifiers ($C025).
    // Deltas accumulate from the host; a read of $C024 returns X then Y (the
    // $C027 bit1 X/Y toggle), each carrying the button in bit7, and the second
    // (Y) read clears the data-available bit ($C027 bit7). See DEV § ADB.
    int      mouseDX_ = 0, mouseDY_ = 0;   // pending host deltas (clamped on read)
    bool     mouseBtn_ = false;            // button currently down
    bool     mouseDataFull_ = false;       // $C027 bit7 — movement/button pending
    bool     mouseReadY_ = false;          // $C027 bit1 — false: X next, true: Y next
    uint8_t  keyMod_ = 0;                  // $C025 KEYMODREG
    uint64_t mouseSetCycle_ = 0;           // videoCycles_ when mouse data was posted
    bool     kbdIntPending_ = false;       // a key event awaits the ROM handler
    uint8_t  kbdIntStatus_ = 0x40;         // $C026 status byte routing to the kbd handler
    uint64_t kbdSetCycle_ = 0;             // videoCycles_ when the key was posted
    void updateAdbIrq();                   // assert IRQ_SRC_ADB while mouse/key data pending
    // RTC + Battery RAM chip ($C033 CLOCKDATA / $C034 CLOCKCTL, serial protocol).
    // The ROM writes command/address bytes to $C033 and strobes $C034 bit7; the
    // chip serves the real-time-clock seconds (from the host clock) or the
    // 256-byte battery RAM (Control Panel settings — startup slot, date format,
    // …). MAME apple2gs.cpp clock. Without it GS/OS shows a bogus date and the
    // Control Panel can't persist the boot slot.
    enum ClkState { CLK_IDLE, CLK_ADDR, CLK_DATA };
    enum ClkSrc   { SRC_SECONDS, SRC_BRAM, SRC_INTERNAL };
    ClkState clkState_ = CLK_IDLE;
    ClkSrc   clkSrc_ = SRC_SECONDS;        // what the pending data phase reads/writes
    uint8_t  clkData_ = 0, clkCtl_ = 0;
    uint8_t  clkAddr_ = 0;                 // BRAM byte address for the data phase
    uint8_t  clkReg_ = 0;                  // seconds (0-3) or internal (0-1) register index
    uint8_t  clkInternal_[2] = {0, 0};     // test ($0) + write-protect ($1) registers
    bool     clkRead_ = false;
    uint8_t  bram_[256] = {0};
    void     clockStrobe(uint8_t c034);    // advance the $C033/$C034 transaction
    uint8_t  rtcByte(int n) const;         // byte n of the 32-bit RTC seconds count
    // Video / interrupt timing.
    static constexpr int kLineCycles    = 65;                 // slow (1.02 MHz) cycles per line
    static constexpr int kLines         = 262;
    static constexpr int kMasterPerLine = kLineCycles * 14;   // 910 master ticks per line
    uint64_t videoCycles_ = 0;        // MASTER-clock ticks (14.318 MHz — wall time)
    int      lastVpos_ = 0;
    uint8_t  intflag_ = 0;            // $C046 INTFLAG (VBL=0x08, QUARTER=0x10)
    uint8_t  inten_ = 0;              // $C041 INTEN  (VBL=0x08, QUARTER=0x10)
    uint8_t  vgcint_ = 0;             // $C023 VGCINT (IRQ=0x80, 1sec=0x40, scan=0x20, 1sec-en=0x04, scan-en=0x02)
    uint32_t frameCount_ = 0;         // host frames, for the ¼-sec / 1-sec timers
    Iwm      iwm_;                    // on-board 5.25" IWM ($C0E0-$C0EF)
    Es5503   doc_;                    // Ensoniq 5503 DOC (Sound GLU $C03C-$C03F)
    Scc8530  scc_;                    // SCC 8530 serial ($C038-$C03B)
    ProDosHdd hdd_{7};                // ProDOS hard-disk card in slot 7
    ProDosHdd disk35_{5, true};       // 800K 3.5" SmartPort drive in slot 5 (WDM-trap ROM)
    bool      disk35Changed_ = false;  // a 3.5" swap/eject → report "disk switched" in STATUS bit0
    bool      disk35SwitchIo_ = false; // a 3.5" swap/eject → return $2E on the next block READ/WRITE once
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
    uint8_t  slowLcRead(uint8_t bank, uint16_t off);           // Mega II LC ($E0/$E1 $D000+)
    void     slowLcWrite(uint8_t bank, uint16_t off, uint8_t v);
    void     lcSwitch(uint16_t off, bool writing);
    bool     maybeShadow(uint8_t bank, uint16_t off, uint8_t v);   // true = wrote slow side
    // Recompute the shared CPU IRQ lines from the flag/enable registers.
    void updateMega2Irq();   // VBL + ¼-second ($C041 INTEN & $C046 INTFLAG)
    void updateVgcIrq();     // scan-line + 1-second ($C023 VGCINT)
    void clearVgcScanline(); // $C02E/$C02F read side-effect (MAME clear_vgcint)
    // SmartPort HLE (slot-5 3.5" drive).
    void    prodosBlockCall();       // WDM $C6 — ProDOS block via $42-$47
    void    smartportCall();         // WDM $C5 — SmartPort dispatch (inline params)
    uint8_t smartportStatus(uint8_t unit, uint8_t code, uint32_t list, bool ext);
};

#endif // POMIIGS_IIGSMEMORY_H
