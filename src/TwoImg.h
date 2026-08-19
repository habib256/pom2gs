// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
// Ported verbatim from POM2 (the Apple II-family sibling emulator).
//
// 2IMG (.2mg) container — the ONE definition of the flags-word semantics,
// shared by the three loaders that open a .2mg: DiskImage (5.25"), Sony35
// (800K 3.5") and ProDosHdd (HDV) — POM2 names them DiskImage / Disk35Image /
// Block512Backing. Spec: DiskImage_2MG_Info.txt; cross-checked against
// CiderPress (`kFlagLocked = 0x80000000`) and AppleWin.
//
//   bit 31   locked (write-protect)
//   bit  8   volume-number-valid
//   bits 0-7 DOS 3.3 volume number (only meaningful when bit 8 is set)
//
// History: all three loaders independently tested bit 0 for the lock —
// locked images mounted writable and odd volume numbers read as
// write-protected. The subtlety worth keeping in one place: bit 0 is
// retained as a LENIENT extra write-protect signal, but only when no
// volume field is declared (bit 8 clear) — with bit 8 set, bit 0 is just
// the low bit of the volume number.

#ifndef POMIIGS_TWOIMG_H
#define POMIIGS_TWOIMG_H

#include <cstddef>
#include <cstdint>

namespace pom2 {

/// Write-protect ("locked") status of a 2IMG flags word.
inline constexpr bool twoImgWriteProtected(uint32_t flags)
{
    return (flags & (1u << 31)) != 0 ||
           ((flags & 1u) != 0 && (flags & (1u << 8)) == 0);
}

/// DOS 3.3 volume number of a 2IMG flags word (254 when not declared).
inline constexpr uint8_t twoImgVolume(uint32_t flags)
{
    return (flags & (1u << 8)) ? static_cast<uint8_t>(flags & 0xFF)
                               : static_cast<uint8_t>(254);
}

/// Where the disk data actually lives inside a (possibly) 2IMG file.
///
/// The block loaders (ProDosHdd, Sony35) used to assume "2IMG magic ⇒ the
/// payload starts at byte 64 and runs to EOF". Both fields are explicit in the
/// header for a reason:
///   * `dataOffset` (bytes 24-27) need not be 64 — a writer may park a comment
///     or creator chunk between the header and the data. Skipping a fixed 64
///     loads every block shifted, and the write-back path then patches the
///     backing FILE at the same wrong offset, corrupting the user's image.
///   * `dataLength` (bytes 28-31) bounds the payload — a trailing comment /
///     creator chunk otherwise reads back as extra phantom blocks.
///
/// Lenient by design: this is a *repair* of the fixed-64 assumption, not a
/// validator. A header whose own offset/length fields are unusable falls back
/// to the legacy behaviour (skip `headerLength`, else 64; run to EOF), so an
/// image that mounted before still mounts.
struct TwoImgPayload
{
    bool        is2img         = false;  ///< the file carries a 2IMG envelope
    std::size_t offset         = 0;      ///< byte offset of the disk data
    std::size_t length         = 0;      ///< byte length of the disk data
    bool        writeProtected = false;  ///< per twoImgWriteProtected()
};

inline TwoImgPayload twoImgProbe(const uint8_t* p, std::size_t n)
{
    TwoImgPayload r;
    if (n < 64 || p[0] != '2' || p[1] != 'I' || p[2] != 'M' || p[3] != 'G')
        return r;                        // not a 2IMG — caller uses the raw file
    auto rd16 = [&](std::size_t o) {
        return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8);
    };
    auto rd32 = [&](std::size_t o) {
        return rd16(o) | (static_cast<uint32_t>(rd16(o + 2)) << 16);
    };
    r.is2img         = true;
    r.writeProtected = twoImgWriteProtected(rd32(16));

    const uint32_t headerLen = rd16(8);
    const uint32_t dataOff   = rd32(24);
    const uint32_t dataLen   = rd32(28);

    // Header length is only a hint for the fallback; 52 is the shortest form
    // the spec defines (DiskImage::detectFormat refuses anything shorter).
    r.offset = (headerLen >= 52 && headerLen <= n) ? headerLen : 64;
    if (dataOff >= 52 && dataOff <= n) r.offset = dataOff;
    r.length = n - r.offset;
    if (dataLen != 0 && static_cast<std::size_t>(dataLen) <= r.length)
        r.length = dataLen;
    return r;
}

} // namespace pom2

#endif // POMIIGS_TWOIMG_H
