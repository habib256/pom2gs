// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── IWM (Integrated Woz Machine) — 5.25" Disk II + 3.5" Sony ─────────────
// The IIgs on-board disk controller at $C0E0-$C0EF (phases, enable, drive
// select, Q6/Q7 latches). $C031 DISKREG routes the port: bit 6 (35SEL)
// switches the phase/enable lines from the 5.25" stepper to the Sony 3.5"
// drive's register protocol, bit 7 (HDSEL) is the Sony SEL line (register
// address bit + head select). MAME apple2gs.cpp:268-269 & devsel_w
// (3684-3721); KEGS iwm.c iwm_get_dsk (395-409).
//
// 5.25": GCR nibble stream per track; the CPU's boot code reads nibbles
// looking for the D5 AA 96 address prologue (M5 read path — .dsk/.do/.po
// nibblised 6-and-2, .nib pass-through, 1 bit ≈ 4 CPU cycles).
// 3.5": Sony35 drive model — status/commands via phases+SEL+LSTRB, 800K GCR
// zoned tracks, read AND write (nibble-level; see Sony35.h).
// With no disk, the data line reads weak bits so the ROM's sync search fails
// and it falls through to "Check startup device".
// Source of truth: MAME machine/iwm.cpp; Beneath Apple DOS (GCR); WOZ spec.

#ifndef POMIIGS_IWM_H
#define POMIIGS_IWM_H

#include <cstdint>
#include <string>
#include <vector>

#include "Sony35.h"

class Iwm
{
public:
    Iwm();

    // Load a 5.25" image (143 360-byte .dsk/.do/.po → nibblised, or raw .nib
    // 232 960 B). Returns false on unrecognised size.
    bool loadDisk525(const std::vector<uint8_t>& img, bool prodosOrder);
    bool hasDisk() const { return diskPresent_; }
    void eject() { diskPresent_ = false; }
    // $C036 disk-motor-detect coupling senses the 5.25" (slot 6) motor only:
    // with 35SEL set the ENABLE line is routed to the 3.5" drive instead.
    bool motorOn() const { return motorOn_ && !sel35_; }

    // 3.5" Sony drives (internal port, both drives — $C0EA/$C0EB select).
    bool loadDisk35(const std::string& path, int drive = 0) { return d35_[drive & 1].loadImage(path); }
    void ejectDisk35(int drive = 0) { d35_[drive & 1].eject(); }
    bool hasDisk35(int drive = 0) const { return d35_[drive & 1].loaded(); }
    void flushDisk35() { d35_[0].flush(); d35_[1].flush(); }
    const std::string& disk35Path(int drive = 0) const { return d35_[drive & 1].path(); }
    Sony35& sony35(int drive = 0) { return d35_[drive & 1]; }

    // $C031 DISKREG (b6 = 35SEL, b7 = HDSEL) — MAME apple2gs.cpp:1995-2006.
    void setDiskReg(uint8_t v) { sel35_ = (v & 0x40) != 0; hdsel_ = (v & 0x80) != 0; }

    // $C0E0-$C0EF access (offset 0..15). Read returns the data/status latch;
    // write latches the data register. `cycle` is the absolute master-clock
    // tick (14.318 MHz) so the nibble streams advance with time.
    uint8_t access(uint8_t offset, bool isWrite, uint8_t writeVal, uint64_t cycle);

private:
    // 35 tracks × nibble stream (5.25").
    std::vector<std::vector<uint8_t>> track_;   // nibblised tracks
    bool diskPresent_ = false;
    bool writeProtect_ = false;

    // Mechanical / controller state.
    int  phase_[4] = {0, 0, 0, 0};   // stepper phases (5.25) / CA0-2+LSTRB (3.5)
    int  halfTrack_ = 0;             // 0..69 (quarter tracks folded to half)
    bool motorOn_ = false;           // ENABLE line ($C0E8/9)
    int  drive_ = 0;                 // selected drive ($C0EA/B; only drive 0 modelled)
    bool q6_ = false, q7_ = false;   // latch state
    uint8_t dataReg_ = 0;
    uint8_t mode_ = 0;               // IWM mode register (written when Q7·Q6·!enable)

    // $C031 routing + the Sony drives.
    bool sel35_ = false;             // DISKREG b6: phases → 3.5" register protocol
    bool hdsel_ = false;             // DISKREG b7: Sony SEL line / head select
    Sony35 d35_[2];

    // Read-stream position (5.25").
    uint64_t lastCycle_ = 0;
    size_t   bitPos_ = 0;
    uint64_t wrBusyUntil_ = 0;       // handshake bit6 window after a data write

    // Sony register index {CA2,CA1,CA0,SEL} from the current phase lines +
    // HDSEL (Neil Parker's addressing; GSSquared Floppy35_woz ordering).
    int sonyIdx() const { return (phase_[2] << 3) | (phase_[1] << 2) | (phase_[0] << 1) | (hdsel_ ? 1 : 0); }

    void stepPhase(int magnet, bool on);
    int  curTrack() const { return halfTrack_ / 2; }
    void advance(uint64_t cycle);
    void nibblise(const std::vector<uint8_t>& img, bool prodosOrder);
};

#endif // POMIIGS_IWM_H
