// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// IWM controller. See Iwm.h. 5.25" media/bit streams come from the POM2
// DiskImage port; the latch/pacing model mirrors the 3.5" Sony path (one
// nibble valid per real interval, $00 between, elastic delivery — see
// DEV.md § Disk for why the ROM's poll-budgeted loops require this).
// Source of truth: MAME machine/iwm.cpp; POM2 IWMDevice; KEGS iwm.c.

#include "Iwm.h"

namespace {
// One 5.25" bit cell = 4 µs = ~57 master ticks (14.318 MHz); a nibble is
// 8 cells ≈ 32 µs (MAME iwm.cpp window sizing; POM2 IWMDevice ÷7 scaling).
constexpr uint64_t kTicksPerCell525 = 57;
// DiskImage flux timestamps: 1 LSS cycle = 0.5 µs → 8 LSS cycles per cell.
constexpr int64_t kLssPerCell = 8;
}

Iwm::Iwm() {}

bool Iwm::loadDisk525(const std::string& path) {
    flushDisk525();                        // don't lose a previous disk's writes
    bool ok = disk525_.loadFile(path);
    // POM2's DiskIICard opts into write-back explicitly; we do the same —
    // the file's own write-protect (WOZ INFO / read-only) still gates it.
    if (ok) disk525_.setWriteBackEnabled(true);
    bitPos525_ = 0; latchValid525_ = false; nibClock525_ = 0;
    return ok;
}

void Iwm::eject() {
    flushDisk525();
    disk525_.eject();
}

void Iwm::flushDisk525() {
    endWrite525();
    if (disk525_.isLoaded() && disk525_.hasUnsavedChanges())
        disk525_.saveDirty();
}

void Iwm::stepPhase(int magnet, bool on) {
    phase_[magnet] = on ? 1 : 0;
    // Move the head toward an energised adjacent phase (2 phases per track,
    // so one phase step = half a track).
    if (!on) return;
    endWrite525();                         // head movement closes a write session
    int cur = halfTrack_ % 4;
    int delta = (magnet - cur + 4) % 4;
    if (delta == 1) halfTrack_++;
    else if (delta == 3) halfTrack_--;
    if (halfTrack_ < 0) halfTrack_ = 0;
    if (halfTrack_ > 68) halfTrack_ = 68;
}

// Assemble one nibble from the quarter-track bit stream, exactly one per
// ~8 cell times (elastic pacing — see Sony35::readNibble). Leading zero
// cells are skipped while the shift register's MSB is clear, which is how
// 10-cell sync $FFs slip the byte boundary and re-align the next prologue
// (POM2 DiskImage.h "LSS bit-cell stream" note).
uint8_t Iwm::readNibble525(uint64_t cycle) {
    endWrite525();                         // a data read closes any write session
    if (!disk525_.isLoaded())              // empty drive: spinning weak bits
        return uint8_t(0x80 | (cycle >> 5));
    const int qt = qt525();
    const int len = disk525_.trackBitLength(qt);
    if (len <= 0)                          // unformatted quarter-track (WOZ gap):
        return uint8_t(0x80 | (cycle >> 5));   // MC3470 AGC noise
    if (cycle - nibClock525_ >= 8 * kTicksPerCell525) {
        uint8_t sh = 0;
        int guard = 10 * 8;                // ≥ one full sync run without data
        while (guard-- > 0) {
            sh = uint8_t((sh << 1) | disk525_.bitAt(qt, int(bitPos525_)));
            bitPos525_ = (bitPos525_ + 1) % size_t(len);
            if (sh & 0x80) break;
        }
        // A window with no set MSB = a long zero span (WOZ weak bits /
        // erased area): the real drive's AGC amplifies noise.
        latch525_ = (sh & 0x80) ? sh : uint8_t(0x80 | (cycle >> 3));
        latchValid525_ = true;
        nibClock525_ = cycle;
    }
    if (latchValid525_) { latchValid525_ = false; return latch525_; }
    return 0x00;                           // byte not assembled yet (MSB clear)
}

void Iwm::writeNibble525(uint8_t v) {
    if (!disk525_.isLoaded() || disk525_.isWriteProtected() || !motorOn_) return;
    if (!wr525Active_) {
        wr525Active_ = true;
        wr525Qt_ = qt525();
        wr525StartBit_ = bitPos525_;
        wr525_.clear();
    }
    wr525_.push_back(v);
    const int len = disk525_.trackBitLength(wr525Qt_);
    if (len > 0) bitPos525_ = (bitPos525_ + 8) % size_t(len);   // head keeps moving
    latchValid525_ = false;
}

// Close the write session: turn the byte stream into flux transitions (one
// per 1-bit at the cell centre) and splice it into the quarter-track
// (DiskImage::writeFlux re-derives nibbles for .dsk/.po and splices bits
// for .woz). Sync $FFs in runs of ≥ kSyncMinRun get 10 cells (8 data + 2
// trailing zero) — the SAME rule as DiskImage::computeCellWidths, whose
// padded timeline writeFlux re-packs against; without this, every written
// gap $FF drifted the following bytes 2 cells and the spliced data field
// never lined up with the old nibble slots. (RWTS writes those self-syncs
// with 40-cycle spacing on real hardware — this reproduces the layout
// without needing cycle-true write pacing.) Unanchored reduction
// (revolutionStart < 0) matches our position-indexed reads (LSS = bit × 8).
void Iwm::endWrite525() {
    if (!wr525Active_) return;
    wr525Active_ = false;
    if (wr525_.empty() || !disk525_.isLoaded()) { wr525_.clear(); return; }
    const size_t n = wr525_.size();
    // Per-byte cell widths (computeCellWidths' sync rule over the session).
    std::vector<uint8_t> width(n, 8);
    for (size_t i = 0; i < n;) {
        if (wr525_[i] != 0xFF) { ++i; continue; }
        size_t j = i;
        while (j < n && wr525_[j] == 0xFF) ++j;
        if (j - i >= 5)
            for (size_t k = i; k < j; ++k) width[k] = 10;
        i = j;
    }
    std::vector<int64_t> flux;
    flux.reserve(n * 8);
    const int64_t start = int64_t(wr525StartBit_) * kLssPerCell;
    int64_t cell = start;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = wr525_[i];
        for (int k = 7; k >= 0; --k) {
            if ((b >> k) & 1) flux.push_back(cell + kLssPerCell / 2);
            cell += kLssPerCell;
        }
        cell += int64_t(width[i] - 8) * kLssPerCell;   // sync padding cells
    }
    disk525_.writeFlux(wr525Qt_, start, cell, int(flux.size()), flux.data());
    wr525_.clear();
}

uint8_t Iwm::access(uint8_t offset, bool isWrite, uint8_t writeVal, uint64_t cycle) {
    // $C0E0-$C0EF: even/odd pairs toggle a line low/high.
    switch (offset) {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x4: case 0x5: case 0x6: case 0x7: {
            const int magnet = offset >> 1;
            const bool on = offset & 1;
            phase_[magnet] = on ? 1 : 0;
            if (sel35_) {
                // Phases are the Sony register address; PH3 is LSTRB — a
                // rising edge with the drive enabled fires a command (KEGS
                // iwm.c:494-499 iwm_do_action35 gate; MAME seek_phase_w).
                if (magnet == 3 && on && motorOn_)
                    d35_[drive_ & 1].command(sonyIdx());
            } else {
                stepPhase(magnet, on);        // 5.25" head stepper
            }
            break;
        }
        case 0x8:
            motorOn_ = false;
            if (sel35_) { d35_[0].flush(); d35_[1].flush(); }
            else flushDisk525();
            break;
        case 0x9: motorOn_ = true;  break;
        case 0xA: if (drive_ != 0) d35_[1].endWrite(); drive_ = 0; break;
        case 0xB: if (drive_ != 1) d35_[0].endWrite(); drive_ = 1; break;
        case 0xC: q6_ = false; break;
        case 0xD: q6_ = true;  break;
        case 0xE:                                     // Q7 falling closes a write session
            if (q7_) { if (sel35_) d35_[drive_ & 1].endWrite(); else endWrite525(); }
            q7_ = false; break;
        case 0xF: q7_ = true;  break;
    }

    if (isWrite) {
        dataReg_ = writeVal;
        if (q7_ && q6_) {
            if (!motorOn_) mode_ = writeVal & 0x1F;             // write MODE register
            else {
                if (sel35_)                                     // write data → Sony head
                    d35_[drive_ & 1].writeNibble(hdsel_ ? 1 : 0, writeVal); // (async, handshake below)
                else if (drive_ == 0 && !enable2())             // write data → 5.25" head
                    writeNibble525(writeVal);                   // (ENABLE2 = SmartPort chain packet)
                // The write shifter drains one 8-bit nibble (~16 µs on the
                // 3.5", ~32 µs on the 5.25"); the handshake's bit 6 stays
                // high while it still holds data.
                wrBusyUntil_ = cycle + 2 * 229;                 // buffer + shifter
            }
        }
        return 0;
    }

    // Read: the returned latch depends on Q7:Q6.
    if (!q7_ && !q6_) {                       // read data
        if (!motorOn_) return 0xFF;           // enable off → floating-high bus (MAME iwm.cpp / KEGS)
        if (sel35_)                           // Sony head data (latch-paced)
            return d35_[drive_ & 1].readNibble(hdsel_ ? 1 : 0, cycle);
        if (enable2()) return 0xFF;           // external SmartPort chain: no device
        if (drive_ != 0)                      // no second 5.25" drive attached
            return uint8_t(0x80 | (cycle >> 5));
        return readNibble525(cycle);
    }
    if (!q7_ && q6_) {                        // read status: low 5 = mode, hi = sense
        uint8_t st = mode_ & 0x1F;
        if (sel35_) {
            // bit7 = Sony SENSE, addressed by {CA2,CA1,CA0,SEL}; both
            // internal drives are installed ($C0EA/$C0EB select).
            if (d35_[drive_ & 1].sense(sonyIdx(), cycle)) st |= 0x80;
        } else if (enable2()) {
            st |= 0x80;                       // no external chain device (KEGS: 1)
        } else {
            // bit7 = sense line: write-protect, OR phase 1 energised — the
            // drive-detection trick (MAME floppy.cpp:799-805 wpt_r; the
            // IIgs internal 5.25" driver's probe at $FF:581C polls status
            // with PH1 on and hangs unless the line answers). An empty
            // drive reads protected.
            if (!disk525_.isLoaded() || disk525_.isWriteProtected() || phase_[1]) st |= 0x80;
        }
        if (motorOn_) st |= 0x20;             // bit5 = enable on
        return st;
    }
    if (q7_ && !q6_)                          // read handshake
        // bit7 = register ready (we accept every byte instantly); bit6 = the
        // shifter still holds data. The ROM's write-flush loop at $FF:57B7
        // (LDA $C0EC / AND #$40 / BNE) waits for bit6 to DROP after the last
        // byte — KEGS parity: bit6 set only within ~8 bit-times of the last
        // write (iwm.c:1147-1162), not MAME's constant 0xC0.
        return uint8_t(0x80 | (cycle < wrBusyUntil_ ? 0x40 : 0x00));
    return dataReg_;
}
