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
// 5.25": bit-cell read/write over a POM2 `DiskImage` (.dsk/.do/.po/.nib/
// .d13/.2mg/.woz incl. WOZ2 FLUX) — the latch assembles one nibble at a
// time from the quarter-track bit stream (leading zero cells slip the byte
// boundary exactly like the real LSS, so 10-cell sync $FFs re-align data
// prologues), paced at ~4 µs/cell with elastic delivery (the discipline
// proven on the 3.5" path). Writes collect in a session and splice back as
// flux (DiskImage::writeFlux); dirty media persists via saveDirty().
// 3.5": Sony35 drive model — status/commands via phases+SEL+LSTRB, 800K GCR
// zoned tracks, read AND write (nibble-level; see Sony35.h).
// With no disk, the data line reads weak bits so the ROM's sync search fails
// and it falls through to "Check startup device".
// Source of truth: MAME machine/iwm.cpp; POM2 IWMDevice/DiskImage; KEGS.

#ifndef POMIIGS_IWM_H
#define POMIIGS_IWM_H

#include <cstdint>
#include <string>
#include <vector>

#include "DiskImage.h"
#include "Sony35.h"

class Iwm
{
public:
    Iwm();

    // Mount a 5.25" image by path (.dsk/.do/.po/.nib/.d13/.2mg/.woz —
    // POM2 DiskImage detector). Drive 1 of the internal port.
    bool loadDisk525(const std::string& path);
    bool hasDisk() const { return disk525_.isLoaded(); }
    void eject();                        // flush pending writes, drop media
    void flushDisk525();                 // end write session + saveDirty()
    const std::string& disk525Path() const { return disk525_.getPath(); }
    // $C036 disk-motor-detect coupling senses the 5.25" (slot 6) motor only:
    // with 35SEL set the ENABLE line is routed to the 3.5" drive instead.
    bool motorOn() const { return motorOn_ && !sel35_; }

    // 3.5" Sony drives (internal port, both drives — $C0EA/$C0EB select).
    bool loadDisk35(const std::string& path, int drive = 0, bool readOnly = false) {
        if (!d35_[drive & 1].loadImage(path)) return false;
        if (readOnly) d35_[drive & 1].setWriteProtect(true);
        return true;
    }
    void ejectDisk35(int drive = 0) { d35_[drive & 1].eject(); }
    bool hasDisk35(int drive = 0) const { return d35_[drive & 1].loaded(); }
    void flushDisk35() { d35_[0].flush(); d35_[1].flush(); }
    const std::string& disk35Path(int drive = 0) const { return d35_[drive & 1].path(); }
    Sony35& sony35(int drive = 0) { return d35_[drive & 1]; }

    // $C031 DISKREG (b6 = 35SEL, b7 = HDSEL) — MAME apple2gs.cpp:1995-2006.
    void setDiskReg(uint8_t v) { sel35_ = (v & 0x40) != 0; hdsel_ = (v & 0x80) != 0; }

    // $C0E0-$C0EF access (offset 0..15). Read returns the data/status latch;
    // write latches the data register. `cycle` is the absolute master-clock
    // tick (14.318 MHz) so the bit/nibble streams advance with time.
    uint8_t access(uint8_t offset, bool isWrite, uint8_t writeVal, uint64_t cycle);

private:
    // ── 5.25" drive (Disk II mechanism over POM2 DiskImage) ──────────────
    DiskImage disk525_;

    // Mechanical / controller state.
    int  phase_[4] = {0, 0, 0, 0};   // stepper phases (5.25) / CA0-2+LSTRB (3.5)
    int  halfTrack_ = 0;             // 0..69 head position in half tracks
    bool motorOn_ = false;           // ENABLE line ($C0E8/9)
    int  drive_ = 0;                 // selected drive ($C0EA/B)
    bool q6_ = false, q7_ = false;   // latch state
    uint8_t dataReg_ = 0;
    uint8_t mode_ = 0;               // IWM mode register (written when Q7·Q6·!enable)

    // Read latch (bit-cell walk, elastic ~4 µs/cell pacing).
    size_t   bitPos525_ = 0;         // bit index within the current quarter-track
    uint64_t nibClock525_ = 0;       // master tick of the last assembled nibble
    bool     latchValid525_ = false;
    uint8_t  latch525_ = 0;

    // Write session: bytes collected from data writes, spliced as flux on
    // session close (Q7/ENABLE fall, read, step, eject) — Sony35 pattern.
    std::vector<uint8_t> wr525_;
    size_t wr525StartBit_ = 0;
    int    wr525Qt_ = 0;
    bool   wr525Active_ = false;

    // $C031 routing + the Sony drives.
    bool sel35_ = false;             // DISKREG b6: phases → 3.5" register protocol
    bool hdsel_ = false;             // DISKREG b7: Sony SEL line / head select
    Sony35 d35_[2];

    uint64_t wrBusyUntil_ = 0;       // handshake bit6 window after a data write

    // Sony register index {CA2,CA1,CA0,SEL} from the current phase lines +
    // HDSEL (Neil Parker's addressing; GSSquared Floppy35_woz ordering).
    int sonyIdx() const { return (phase_[2] << 3) | (phase_[1] << 2) | (phase_[0] << 1) | (hdsel_ ? 1 : 0); }

    // ENABLE2 — PH1+PH3 both energised addresses the EXTERNAL SmartPort
    // chain (UniDisk protocol), not the dumb drive (KEGS iwm.c:494-505).
    // The ROM's disk-port probe WRITES command packets in this state; without
    // the gate they landed on the 5.25" surface and wiped sector 0's address
    // field on every boot.
    bool enable2() const { return !sel35_ && phase_[1] && phase_[3]; }

    // Quarter-track for the head: whole tracks land on TMAP's 4t entries,
    // half-track positions on 4t+2 (usually unformatted in WOZ — matching
    // the real head sitting between tracks). True quarter stepping
    // (adjacent-phase pairs) is a follow-up.
    int qt525() const { int qt = halfTrack_ * 2; return qt < DiskImage::kQuarterTracks ? qt : DiskImage::kQuarterTracks - 1; }

    void stepPhase(int magnet, bool on);
    uint8_t readNibble525(uint64_t cycle);
    void    writeNibble525(uint8_t v);
    void    endWrite525();
};

#endif // POMIIGS_IWM_H
