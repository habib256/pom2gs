// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Machine snapshot wrapper. See Snapshot.h.

#include "Snapshot.h"
#include "CPU65816.h"
#include "IIgsMemory.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace {
constexpr char     kMagic[4] = {'P', 'G', 'S', 'S'};
constexpr uint32_t kVersion  = 2;   // v2: + ADB GLU int-enables / µC modes byte

template <typename T> void put(std::ostream& os, const T& v) { os.write((const char*)&v, sizeof v); }
template <typename T> void get(std::istream& is, T& v)       { is.read((char*)&v, sizeof v); }
}

bool saveSnapshot(const std::string& path, const CPU65816& cpu, const IIgsMemory& mem) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os.write(kMagic, 4);
    put(os, kVersion);
    // 65C816 registers.
    put(os, cpu.getA()); put(os, cpu.getX()); put(os, cpu.getY());
    put(os, cpu.getSP()); put(os, cpu.getD());
    put(os, cpu.getDBR()); put(os, cpu.getPBR());
    put(os, cpu.getPC()); put(os, cpu.getP());
    const bool e = cpu.getEmulationMode(); put(os, e);
    mem.saveState(os);
    return os.good();
}

bool loadSnapshot(const std::string& path, CPU65816& cpu, IIgsMemory& mem) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;
    char magic[4] = {0}; is.read(magic, 4);
    uint32_t ver = 0; get(is, ver);
    if (std::memcmp(magic, kMagic, 4) != 0 || ver != kVersion) return false;
    uint16_t a, x, y, sp, d, pc; uint8_t dbr, pbr, p; bool e = false;
    get(is, a); get(is, x); get(is, y); get(is, sp); get(is, d);
    get(is, dbr); get(is, pbr); get(is, pc); get(is, p); get(is, e);
    if (!is.good()) return false;
    if (!mem.loadState(is)) return false;
    cpu.setA(a); cpu.setX(x); cpu.setY(y); cpu.setSP(sp); cpu.setD(d);
    cpu.setDBR(dbr); cpu.setPBR(pbr); cpu.setPC(pc); cpu.setP(p);
    cpu.setEmulationMode(e);
    return true;
}
