// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── IWM (Integrated Woz Machine) — Disk II 5.25" ─────────────────────────
// The IIgs on-board disk controller. Slot-6 5.25" access lives at $C0E0-$C0EF
// (phases, motor, drive select, Q6/Q7 latches). The controller presents a
// GCR nibble stream from the current track; the CPU's boot code reads nibbles
// looking for the D5 AA 96 address prologue.
//
// M5: 5.25" read path. A .dsk/.do/.po (143 360 B) is nibblised (6-and-2) into
// per-track streams; .nib passes through. Bit position advances with CPU
// cycles (1 bit ≈ 4 CPU cycles). With no disk, the data line reads $00 so the
// ROM's sync search fails and it falls through to "Check startup device".
// SWIM (ROM 03 MFM superset) + 3.5"/SmartPort are staged in next.
// Source of truth: MAME machine/iwm.cpp; Beneath Apple DOS (GCR); WOZ spec.

#ifndef POMIIGS_IWM_H
#define POMIIGS_IWM_H

#include <cstdint>
#include <vector>

class Iwm
{
public:
    Iwm();

    // Load a 5.25" image (143 360-byte .dsk/.do/.po → nibblised, or raw .nib
    // 232 960 B). Returns false on unrecognised size.
    bool loadDisk525(const std::vector<uint8_t>& img, bool prodosOrder);
    bool hasDisk() const { return diskPresent_; }
    void eject() { diskPresent_ = false; }
    bool motorOn() const { return motorOn_; }   // for the $C036 disk-motor-detect speed coupling

    // $C0E0-$C0EF access (offset 0..15). Read returns the data/status latch;
    // write latches the data register. `cycle` is the absolute CPU cycle so
    // the nibble stream advances with time.
    uint8_t access(uint8_t offset, bool isWrite, uint8_t writeVal, uint64_t cycle);

private:
    // 35 tracks × nibble stream.
    std::vector<std::vector<uint8_t>> track_;   // nibblised tracks
    bool diskPresent_ = false;
    bool writeProtect_ = false;

    // Mechanical / controller state.
    int  phase_[4] = {0, 0, 0, 0};   // stepper phase magnets
    int  halfTrack_ = 0;             // 0..69 (quarter tracks folded to half)
    bool motorOn_ = false;
    int  drive_ = 0;                 // selected drive (only drive 0 modelled)
    bool q6_ = false, q7_ = false;   // latch state
    uint8_t dataReg_ = 0;
    uint8_t mode_ = 0;               // IWM mode register (written when Q7·Q6·!motor)

    // Read-stream position.
    uint64_t lastCycle_ = 0;
    size_t   bitPos_ = 0;

    void stepPhase(int magnet, bool on);
    int  curTrack() const { return halfTrack_ / 2; }
    void advance(uint64_t cycle);
    void nibblise(const std::vector<uint8_t>& img, bool prodosOrder);
};

#endif // POMIIGS_IWM_H
