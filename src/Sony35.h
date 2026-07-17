// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Sony 3.5" 800K drive (Apple 3.5 Drive on the IIgs internal port) ─────
// The Sony mechanism has no memory-mapped registers of its own: the IWM's
// phase lines address it. {CA2,CA1,CA0} = IWM phases 2..0, SEL = $C031 bit 7
// (HDSEL), LSTRB = phase 3. A 4-bit register index {CA2,CA1,CA0,SEL} selects
// either a status bit (read back through the IWM SENSE line = status bit 7)
// or, on an LSTRB rising edge, a command (step, motor, eject, …).
//   Register tables: MAME floppy.cpp mac_floppy_device (wpt_r ~3217-3290,
//   seek_phase_w ~3292-3368) and KEGS iwm.c iwm_read_status35 (912-1017) /
//   iwm_do_action35 (1019-1091) — normalized to the {CA2,CA1,CA0,SEL} index
//   (Neil Parker's "Apple 3.5 drive" note, also GSSquared Floppy35_woz).
//
// Media model is nibble-level (KEGS-style): the 800K image (.po/.2mg) is
// nibblised into 160 GCR tracks (80 cylinders × 2 sides, speed zones of 16
// cylinders holding 12/11/10/9/8 × 512-byte sectors). The 6-and-2 codec with
// its 3-byte rolling carry checksum is an exact port of KEGS iwm.c
// iwm_nibblize_track_35 (3125-3345) / iwm_denib_track35 (2409-2725) — itself
// a disassembly of the IIgs ROM's own nibblizer. Writes land on the nibble
// stream and dirty tracks are de-nibblised back to sectors on flush/eject.
// 10-bit sync $FF nibbles are stored as plain bytes (no bit-level model, so
// bit-slip copy protection is out of scope — same simplification as our
// 5.25" path).

#ifndef POMIIGS_SONY35_H
#define POMIIGS_SONY35_H

#include <cstdint>
#include <string>
#include <vector>

class Sony35
{
public:
    static constexpr int kCyls  = 80;    // cylinders per side
    static constexpr int kTracks = 160;  // cyl*2+side
    static constexpr size_t kImageBytes = 819200;   // 800 KB

    // Load an 800K image (.po raw / .2mg with 64-byte header). Marks the
    // disk-switched flag so firmware re-reads media state.
    bool loadImage(const std::string& path);
    void eject();                        // flush, drop media, set switched
    void flush();                        // de-nibblise dirty tracks → file
    bool loaded() const { return present_; }
    // KEGS g_check_nibblization discipline: de-nibblise every track and
    // compare against the loaded image — pins the codec as self-inverse.
    bool checkNibblization();
    bool writeProtected() const { return writeProt_; }
    // Force write-protect (hybrid HLE mount: the SmartPort HLE owns the
    // backing file; this read-only Sony copy must never flush over it).
    void setWriteProtect(bool wp) { writeProt_ = wp; }
    const std::string& path() const { return path_; }
    bool motorOn() const { return motorOn_; }

    // Sony register protocol. `idx` = {CA2,CA1,CA0,SEL}. sense() is the
    // status bit read back through the IWM SENSE line; command() fires on an
    // LSTRB rising edge. `cycle` = 14.318 MHz master ticks (tach/head data).
    int  sense(int idx, uint64_t cycle) const;
    void command(int idx);

    // Head data stream (`side` = HDSEL live value). IWM latch-mode pacing
    // (GSSquared IWM2.hpp:338-364 + NeilA235Floppy.md:284-300): a nibble
    // becomes valid (bit 7 set) once per ~16 µs of platter rotation; polls
    // in between return $00, and reading a valid nibble consumes it. The
    // ROM's address-hunt loops budget POLL iterations assuming this pacing —
    // an always-fresh nibble per read exhausts them instantly (root cause of
    // the first boot stall). Writes advance exactly one nibble per call.
    uint8_t readNibble(int side, uint64_t cycle);
    void    writeNibble(int side, uint8_t v);

    // Writes accumulate in a session buffer; the session closes on Q7/ENABLE
    // falling, a data read, a step, motor-off or eject. A session covering
    // ≥ half the track REPLACES it (the ROM's FORMAT writes whole tracks,
    // whose nibble count differs from ours); anything shorter splices in
    // place at its start position (normal sector write).
    void endWrite();

private:
    // Media / mechanism.
    std::vector<uint8_t> image_;         // 819 200-byte logical image
    std::string path_;                   // backing file ("" = not file-backed)
    size_t headerBytes_ = 0;             // .2mg header to skip on flush
    bool present_   = false;
    bool writeProt_ = false;
    bool switched_  = false;             // disk-switched flag (set on insert/eject)
    bool motorOn_   = false;             // spindle (via command 4/$C)
    bool stepDir_   = false;             // 0 = inward (+1 cyl), 1 = outward (−1)
    int  cyl_       = 0;                 // 0..79

    // Nibble tracks, cyl*2+side. pos_ persists across track changes (mod len).
    std::vector<uint8_t> trk_[kTracks];
    bool     trkDirty_[kTracks] = {false};
    size_t   pos_ = 0;
    uint64_t nibbleClock_ = 0;           // master tick of the last latched nibble
    bool     latchValid_ = false;        // an unconsumed nibble sits in latch_
    uint8_t  latch_ = 0;

    // Write session (see endWrite()).
    std::vector<uint8_t> wrBuf_;
    size_t wrStart_ = 0;                 // track position of the first written nibble
    int    wrTrk_ = 0;                   // track the session belongs to
    bool   wrActive_ = false;

    int curTrk(int side) const { return cyl_ * 2 + side; }

    // GCR codec (KEGS iwm.c ports — see header comment).
    void nibbliseTrack(int trkIdx);
    bool denibbliseTrack(int trkIdx);    // trk_ → image_; false on bad field
    static int  sectorsOnTrack(int trkIdx) { return 12 - (trkIdx >> 5); }
    static size_t imageOffset(int trkIdx);
};

#endif // POMIIGS_SONY35_H
