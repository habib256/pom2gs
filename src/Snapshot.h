// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Machine snapshot (save/load state) ───────────────────────────────────
// One-file save state: a "PGSS" header + version, the 65C816 registers, then
// IIgsMemory::saveState (RAM, MMU, DOC, BRAM, media paths — see IIgsMemory).
// Mirrors POM2's MachineSnapshot split: this wrapper owns the file format,
// the subsystems own their own field rosters, so the two can't drift.
//
// Scope (v1): manual save/load state. The RewindBuffer ring (POM2 port) will
// build on the same serializers with delta compression — the IIgs fast side
// is up to 8 MB per snapshot, too big for POM2's raw ring.

#ifndef POMIIGS_SNAPSHOT_H
#define POMIIGS_SNAPSHOT_H

#include <string>

class CPU65816;
class IIgsMemory;

// Returns false (with an intact machine) on I/O error or format mismatch.
bool saveSnapshot(const std::string& path, const CPU65816& cpu, const IIgsMemory& mem);
bool loadSnapshot(const std::string& path, CPU65816& cpu, IIgsMemory& mem);

#endif // POMIIGS_SNAPSHOT_H
