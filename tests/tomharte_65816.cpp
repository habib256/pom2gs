// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Tom Harte "SingleStepTests/65816" ProcessorTests harness — the CPU65816
// gate. Adapted from POM2's tomharte_cpu_test.cpp (same hand-rolled JSON
// scanner) for the wider 65816 state: 16-bit s/a/x/y, the d (direct page),
// dbr/pbr bank registers, the e (emulation) flag, and 24-bit RAM addresses.
//
//   https://github.com/SingleStepTests/65816  (v1/, one .json per opcode/mode)
//
// CPU65816 is instruction-stepped (run(1) = one opcode) and IIgsMemory is a
// flat 16 MB array, so — exactly as in POM2 — we validate final register file,
// touched RAM, cycle count, and EVERY bus cycle in order: address/data plus
// VDA/VPA/VPB/RWB/E/M/X/MLB for the active transactions, and address plus the
// same pin state for the inactive (internal-operation) cycles, so a
// misplaced internal cycle is diagnosed even when the total count is right.
// `--active-only` restores the transaction-only comparison for triage. Each
// vector's `e` field selects emulation/native mode. P is compared with the
// phantom bits (0x30) masked only in emulation mode; in native mode M/X are
// real flags.
//
// Usage: tomharte_65816 <dir> [--max N] [--only hh,..] [--skip hh,..]
//                        [--no-cycles] [--no-bus] [--active-only] [--verbose] [--examples K]
// Exit 0 = all matched; 1 = mismatch; 2 = usage; 77 = missing data (CTest SKIP).

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RamCell { uint32_t addr; uint8_t val; };

struct ExpectedCycle {
    uint32_t addr = 0;
    uint8_t val = 0;
    bool hasAddr = true;     // WAI/STP halt cycle: address null
    bool hasValue = false;
    std::string outputs;
};

struct CpuState {
    uint16_t pc = 0, s = 0, a = 0, x = 0, y = 0, d = 0;
    uint8_t  p = 0, dbr = 0, pbr = 0, e = 1;
    std::vector<RamCell> ram;
};

struct Vector {
    std::string name;
    CpuState initial, fin;
    int cycleCount = -1;
    std::vector<ExpectedCycle> cycles;
};

inline void skipWs(const char*& p) { while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') ++p; }
inline bool eat(const char*& p, char c) { skipWs(p); if (*p==c) { ++p; return true; } return false; }
inline uint32_t parseUint(const char*& p) { skipWs(p); uint32_t v=0; while (*p>='0'&&*p<='9'){v=v*10u+uint32_t(*p-'0');++p;} return v; }
inline void skipString(const char*& p) { skipWs(p); if (*p!='"') return; ++p; while (*p&&*p!='"') ++p; if (*p=='"') ++p; }

std::string parseString(const char*& p) {
    skipWs(p);
    if (*p != '"') return {};
    ++p;
    const char* first = p;
    while (*p && *p != '"') ++p;
    std::string value(first, size_t(p - first));
    if (*p == '"') ++p;
    return value;
}

void skipValue(const char*& p) {
    skipWs(p);
    if (*p=='"') { skipString(p); return; }
    if (*p=='['||*p=='{') {
        const char open=*p, close=(open=='[')?']':'}'; int depth=0;
        while (*p) { if (*p=='"'){skipString(p);continue;} if (*p==open)++depth; else if (*p==close){if(--depth==0){++p;return;}} ++p; }
        return;
    }
    while (*p&&*p!=','&&*p!='}'&&*p!=']') ++p;
}

// `[[addr,val], ...]` — addr is 24-bit here.
void parseRam(const char*& p, std::vector<RamCell>& out) {
    out.clear(); eat(p,'['); skipWs(p);
    if (*p==']'){++p;return;}
    while (true) {
        eat(p,'['); const uint32_t a=parseUint(p); eat(p,','); const uint32_t v=parseUint(p); eat(p,']');
        out.push_back({a, uint8_t(v)});
        skipWs(p);
        if (*p==','){++p;continue;} if (*p==']'){++p;break;} break;
    }
}

void parseState(const char*& p, CpuState& st) {
    eat(p,'{');
    while (true) {
        skipWs(p);
        if (*p=='}'){++p;break;}
        if (*p=='"') {
            ++p; const char* k=p; while (*p&&*p!='"') ++p; const size_t kl=size_t(p-k);
            if (*p=='"') ++p; eat(p,':');
            auto key = [&](const char* s){ return kl==std::strlen(s) && !std::strncmp(k,s,kl); };
            if      (key("pc"))  st.pc  = uint16_t(parseUint(p));
            else if (key("s"))   st.s   = uint16_t(parseUint(p));
            else if (key("a"))   st.a   = uint16_t(parseUint(p));
            else if (key("x"))   st.x   = uint16_t(parseUint(p));
            else if (key("y"))   st.y   = uint16_t(parseUint(p));
            else if (key("d"))   st.d   = uint16_t(parseUint(p));
            else if (key("p"))   st.p   = uint8_t(parseUint(p));
            else if (key("dbr")) st.dbr = uint8_t(parseUint(p));
            else if (key("pbr")) st.pbr = uint8_t(parseUint(p));
            else if (key("e"))   st.e   = uint8_t(parseUint(p));
            else if (key("ram")) parseRam(p, st.ram);
            else skipValue(p);
        }
        skipWs(p);
        if (*p==','){++p;continue;} if (*p=='}'){++p;break;}
    }
}

void parseCycles(const char*& p, std::vector<ExpectedCycle>& out) {
    out.clear();
    eat(p,'['); skipWs(p);
    if (*p==']'){++p;return;}
    while (*p) {
        ExpectedCycle cycle;
        if (!eat(p, '[')) break;
        skipWs(p);
        if (!std::strncmp(p, "null", 4)) { p += 4; cycle.hasAddr = false; }
        else cycle.addr = parseUint(p);
        eat(p, ','); skipWs(p);
        if (!std::strncmp(p, "null", 4)) p += 4;
        else { cycle.val = uint8_t(parseUint(p)); cycle.hasValue = true; }
        eat(p, ','); cycle.outputs = parseString(p); eat(p, ']');
        out.push_back(std::move(cycle));
        skipWs(p);
        if (*p==','){++p;continue;}
        if (*p==']'){++p;break;}
        break;
    }
}

bool parseVector(const char*& p, Vector& v) {
    skipWs(p); if (*p!='{') return false; ++p;
    v.name.clear(); v.cycles.clear(); v.cycleCount=-1;
    while (true) {
        skipWs(p);
        if (*p=='}'){++p;break;}
        if (*p=='"') {
            ++p; const char* k=p; while (*p&&*p!='"') ++p; const size_t kl=size_t(p-k); const char* key=k;
            if (*p=='"') ++p; eat(p,':');
            if (kl==4 && !std::strncmp(key,"name",4)) { skipWs(p); if (*p=='"'){++p;const char* s=p;while(*p&&*p!='"')++p;v.name.assign(s,size_t(p-s));if(*p=='"')++p;} }
            else if (kl==7 && !std::strncmp(key,"initial",7)) parseState(p, v.initial);
            else if (kl==5 && !std::strncmp(key,"final",5))   parseState(p, v.fin);
            else if (kl==6 && !std::strncmp(key,"cycles",6))  { parseCycles(p, v.cycles); v.cycleCount = int(v.cycles.size()); }
            else skipValue(p);
        }
        skipWs(p);
        if (*p==','){++p;continue;} if (*p=='}'){++p;break;}
    }
    return true;
}

void loadState(CPU65816& cpu, const CpuState& s) {
    cpu.setEmulationMode(s.e != 0);
    cpu.setPC(s.pc); cpu.setSP(s.s); cpu.setA(s.a); cpu.setX(s.x); cpu.setY(s.y);
    cpu.setD(s.d); cpu.setP(s.p); cpu.setDBR(s.dbr); cpu.setPBR(s.pbr);
}

std::string busOutputs(const CPU65816::BusCycle& cycle) {
    std::string out(8, '-');
    if (cycle.vda) out[0] = 'd';
    if (cycle.vpa) out[1] = 'p';
    if (cycle.vpb) out[2] = 'v';
    out[3] = cycle.halt ? '-' : cycle.write ? 'w' : 'r';
    if (cycle.e) out[4] = 'e';
    if (cycle.m) out[5] = 'm';
    if (cycle.x) out[6] = 'x';
    if (cycle.mlb) out[7] = 'l';
    return out;
}

bool activeBusCycle(const ExpectedCycle& cycle) {
    return cycle.outputs.size() == 8 &&
           (cycle.outputs[0] != '-' || cycle.outputs[1] != '-' || cycle.outputs[2] != '-');
}

// Known corpus errors, carried as exact issue-linked xfails rather than
// opcode-wide skips. Returns the reason when the vector's mismatch is one of
// them, else nullptr.
//  (dp,X) in emulation mode: the pointer's high byte is read within the page
//  of the low byte's address (DL=0: Bruce Clark §5.11, SingleStepTests/65816
//  issue #3 — a single SBC vector was patched; DL!=0: hardware quirk verified
//  by gilyon cputest 0027 and modelled by bsnes readDirectX). The corpus reads
//  it at lo+1, so every vector whose pointer sits at $xxFF differs.
//  JSR (abs,X) in emulation mode pushes through the raw 16-bit stack (a new
//  65816 instruction — 6502.org 65C816 opcodes appendix, bsnes, gilyon cputest
//  0277); the corpus wraps the push inside page 1 (SingleStepTests/65816
//  issue #6), so vectors with S=$xx00 differ.
const char* knownCorpusError(const Vector& v) {
    if (v.cycles.size() < 2 || !v.cycles[0].hasValue || !v.cycles[1].hasValue || !v.initial.e) return nullptr;
    const uint8_t op = v.cycles[0].val;
    if (op == 0xFC) return (v.initial.s & 0xFF) == 0x00 ? "JSR (abs,X) E=1 stack page wrap — SingleStepTests/65816 issue #6" : nullptr;
    if ((op & 0x1F) != 0x01) return nullptr;                       // $01/$21/…/$E1: (dp,X)
    const uint8_t ll = v.cycles[1].val, x = uint8_t(v.initial.x);
    const uint16_t d = v.initial.d;
    const uint16_t lo = (d & 0xFF) == 0 ? uint16_t((d & 0xFF00) | uint8_t(ll + x)) : uint16_t(d + ll + x);
    if ((lo & 0xFF) != 0xFF) return nullptr;
    return (d & 0xFF) == 0 ? "(dp,X) E=1 DL=0 pointer page wrap — SingleStepTests/65816 issue #3"
                           : "(dp,X) E=1 DL!=0 high-byte page wrap — gilyon cputest 0027 / bsnes readDirectX";
}

bool runVector(CPU65816& cpu, IIgsMemory& mem, const Vector& vIn, bool checkCycles, bool checkBus, std::string& why,
               bool activeOnly = false) {
    loadState(cpu, vIn.initial);
    for (const RamCell& c : vIn.initial.ram) mem.write8(c.addr, c.val);

    cpu.setBusTraceEnabled(checkBus);
    // MVN/MVP ($54/$44): the corpus captures at most 100 cycles of a block
    // move — 14 complete 7-cycle iterations plus the opcode and destination-
    // bank fetches of the 15th, leaving PC advanced by two and the registers
    // untouched. POMIIGS steps one byte per run(1) and re-points PC at the
    // opcode until the count wraps, so run whole iterations up to the cap and
    // compare against the vector with those two trailing fetch cycles removed
    // (their addresses and pins are fully determined: PBR:PC opcode, PBR:PC+1
    // operand). Vectors whose move finishes below the cap compare unchanged.
    const bool blockMove = !vIn.cycles.empty() && vIn.cycles[0].hasValue &&
                           (vIn.cycles[0].val == 0x54 || vIn.cycles[0].val == 0x44);
    Vector vAdj;
    const Vector* vp = &vIn;
    int cyc = 0;
    if (blockMove && vIn.cycleCount > 0) {
        while (true) {
            cyc += cpu.run(1);
            if (cpu.getA() == 0xFFFF) break;                       // count wrapped: instruction complete
            if (cyc + 7 > vIn.cycleCount) break;                   // next iteration would exceed the capture
        }
        const int partial = vIn.cycleCount - cyc;
        if (partial > 0) {
            vAdj = vIn;
            vAdj.fin.pc = uint16_t(vIn.fin.pc - partial);
            vAdj.cycleCount = cyc;
            vAdj.cycles.resize(size_t(cyc));
            vp = &vAdj;
        }
    } else {
        cyc = cpu.run(1);
    }
    const Vector& v = *vp;

    bool ok = true; char buf[256];
    auto fail = [&](const char* what, unsigned got, unsigned want) {
        if (ok) { std::snprintf(buf,sizeof buf,"%s got $%X want $%X",what,got,want); why=buf; } ok=false;
    };
    if (cpu.getPC()  != v.fin.pc)  fail("PC", cpu.getPC(),  v.fin.pc);
    if (cpu.getA()   != v.fin.a)   fail("A",  cpu.getA(),   v.fin.a);
    if (cpu.getX()   != v.fin.x)   fail("X",  cpu.getX(),   v.fin.x);
    if (cpu.getY()   != v.fin.y)   fail("Y",  cpu.getY(),   v.fin.y);
    if (cpu.getSP()  != v.fin.s)   fail("SP", cpu.getSP(),  v.fin.s);
    if (cpu.getD()   != v.fin.d)   fail("D",  cpu.getD(),   v.fin.d);
    if (cpu.getDBR() != v.fin.dbr) fail("DBR",cpu.getDBR(), v.fin.dbr);
    if (cpu.getPBR() != v.fin.pbr) fail("PBR",cpu.getPBR(), v.fin.pbr);
    if ((cpu.getEmulationMode()?1:0) != (v.fin.e?1:0)) fail("E", cpu.getEmulationMode(), v.fin.e);
    const uint8_t pmask = v.fin.e ? 0x30 : 0x00;   // phantom B/unused only in emulation
    if (((cpu.getP() ^ v.fin.p) & ~pmask) != 0) fail("P", cpu.getP()&~pmask, v.fin.p&~pmask);
    if (checkCycles && v.cycleCount >= 0 && cyc != v.cycleCount) {
        if (ok) { std::snprintf(buf,sizeof buf,"cycles got %d want %d",cyc,v.cycleCount); why=buf; } ok=false;
    }
    if (checkBus) {
        std::vector<const ExpectedCycle*> expected;
        for (const ExpectedCycle& cycle : v.cycles)
            if (!activeOnly || activeBusCycle(cycle)) expected.push_back(&cycle);
        std::vector<CPU65816::BusCycle> actual;
        for (const CPU65816::BusCycle& cycle : cpu.busTrace())
            if (!activeOnly || cycle.vda || cycle.vpa || cycle.vpb) actual.push_back(cycle);
        if (actual.size() != expected.size()) {
            if (ok) {
                std::snprintf(buf, sizeof buf, "%sbus cycles got %zu want %zu", activeOnly ? "active " : "",
                              actual.size(), expected.size());
                why = buf;
            }
            ok = false;
        }
        const size_t common = std::min(actual.size(), expected.size());
        for (size_t i = 0; i < common; ++i) {
            const ExpectedCycle& want = *expected[i];
            const CPU65816::BusCycle& got = actual[i];
            const std::string gotOutputs = busOutputs(got);
            if ((want.hasAddr && got.address != want.addr) || (want.hasValue && got.value != want.val) || gotOutputs != want.outputs) {
                if (ok) {
                    char wantValue[8] = "null";
                    if (want.hasValue) std::snprintf(wantValue, sizeof wantValue, "$%02X", want.val);
                    std::snprintf(buf, sizeof buf,
                                  "bus[%zu] got [$%06X,$%02X,%s] want [$%06X,%s,%s]",
                                  i, got.address, got.value, gotOutputs.c_str(), want.addr,
                                  wantValue, want.outputs.c_str());
                    why = buf;
                }
                ok = false;
                break;
            }
        }
    }
    for (const RamCell& c : v.fin.ram) {
        const uint8_t got = mem.read8(c.addr);
        if (got != c.val) { if (ok){std::snprintf(buf,sizeof buf,"RAM[$%06X] got $%02X want $%02X",c.addr,got,c.val);why=buf;} ok=false; }
    }
    for (const RamCell& c : v.initial.ram) mem.write8(c.addr, 0);
    for (const RamCell& c : v.fin.ram)     mem.write8(c.addr, 0);
    return ok;
}

int runSelfTest() {
    IIgsMemory mem;
    mem.setTestMode(true);
    CPU65816 cpu(&mem);

    Vector lda;
    lda.name = "selftest LDA immediate";
    lda.initial.pc = 0x1234; lda.initial.s = 0x01FF; lda.initial.p = 0x34;
    lda.initial.a = 0xAB00; lda.initial.pbr = 0x56; lda.initial.e = 1;
    lda.initial.ram = {{0x561234, 0xA9}, {0x561235, 0x80}};
    lda.fin = lda.initial;
    lda.fin.pc = 0x1236; lda.fin.p = 0xB4; lda.fin.a = 0xAB80;
    lda.cycles = {
        {0x561234, 0xA9, true, true, "dp-remx-"},
        {0x561235, 0x80, true, true, "-p-remx-"},
    };
    lda.cycleCount = int(lda.cycles.size());

    Vector brk;
    brk.name = "selftest native BRK vector pull";
    brk.initial.pc = 0x2000; brk.initial.s = 0x01FF; brk.initial.p = 0x30;
    brk.initial.pbr = 0x12; brk.initial.e = 0;
    brk.initial.ram = {{0x122000, 0x00}, {0x122001, 0x42}, {0x00FFE6, 0x34}, {0x00FFE7, 0x12}};
    brk.fin = brk.initial;
    brk.fin.pc = 0x1234; brk.fin.s = 0x01FB; brk.fin.p = 0x34; brk.fin.pbr = 0;
    brk.fin.ram.insert(brk.fin.ram.end(), {
        {0x0001FF, 0x12}, {0x0001FE, 0x20}, {0x0001FD, 0x02}, {0x0001FC, 0x30},
    });
    brk.cycles = {
        {0x122000, 0x00, true, true, "dp-r-mx-"},
        {0x122001, 0x42, true, true, "-p-r-mx-"},
        {0x0001FF, 0x12, true, true, "d--w-mx-"},
        {0x0001FE, 0x20, true, true, "d--w-mx-"},
        {0x0001FD, 0x02, true, true, "d--w-mx-"},
        {0x0001FC, 0x30, true, true, "d--w-mx-"},
        {0x00FFE6, 0x34, true, true, "d-vr-mx-"},
        {0x00FFE7, 0x12, true, true, "d-vr-mx-"},
    };
    brk.cycleCount = int(brk.cycles.size());

    // Emulation-mode INC dp: opcode, DO, read (MLB), the 6502-style dummy
    // write of the unmodified byte with VDA low (MLB), then the real write.
    Vector inc;
    inc.name = "selftest emulation INC dp dummy write";
    inc.initial.pc = 0x3000; inc.initial.s = 0x01FF; inc.initial.p = 0x34;
    inc.initial.d = 0; inc.initial.pbr = 0x02; inc.initial.e = 1;
    inc.initial.ram = {{0x023000, 0xE6}, {0x023001, 0x40}, {0x000040, 0x7F}};
    inc.fin = inc.initial;
    inc.fin.pc = 0x3002; inc.fin.p = 0xB4; inc.fin.ram[2] = {0x000040, 0x80};
    inc.cycles = {
        {0x023000, 0xE6, true, true, "dp-remx-"},
        {0x023001, 0x40, true, true, "-p-remx-"},
        {0x000040, 0x7F, true, true, "d--remxl"},
        {0x000040, 0x7F, true, true, "---wemxl"},
        {0x000040, 0x80, true, true, "d--wemxl"},
    };
    inc.cycleCount = int(inc.cycles.size());

    // Native INX: opcode then one internal cycle with PC+1 on the bus.
    Vector inx;
    inx.name = "selftest native INX internal cycle";
    inx.initial.pc = 0x4000; inx.initial.s = 0x01FF; inx.initial.p = 0x10; // m=0 x=1
    inx.initial.x = 0x00FF; inx.initial.pbr = 0x03; inx.initial.e = 0;
    inx.initial.ram = {{0x034000, 0xE8}};
    inx.fin = inx.initial;
    inx.fin.pc = 0x4001; inx.fin.x = 0x0000; inx.fin.p = 0x12;
    inx.cycles = {
        {0x034000, 0xE8, true, true, "dp-r--x-"},
        {0x034001, 0x00, true, false, "---r--x-"},
    };
    inx.cycleCount = int(inx.cycles.size());

    // Emulation WAI: opcode, two internal cycles at PC+1, then the halt cycle
    // with the address bus and E/M/X outputs released (null address, all
    // pins low) — the shape that used to spin the scanner.
    Vector wai;
    wai.name = "selftest emulation WAI halt cycle";
    wai.initial.pc = 0x5000; wai.initial.s = 0x01FF; wai.initial.p = 0x34;
    wai.initial.pbr = 0x04; wai.initial.e = 1;
    wai.initial.ram = {{0x045000, 0xCB}};
    wai.fin = wai.initial;
    wai.fin.pc = 0x5001;
    wai.cycles = {
        {0x045000, 0xCB, true, true, "dp-remx-"},
        {0x045001, 0x00, true, false, "---remx-"},
        {0x045001, 0x00, true, false, "---remx-"},
        {0x000000, 0x00, false, false, "--------"},
    };
    wai.cycleCount = int(wai.cycles.size());

    for (const Vector* vector : {&lda, &brk, &inc, &inx, &wai}) {
        std::string why;
        if (!runVector(cpu, mem, *vector, true, true, why)) {
            std::fprintf(stderr, "[tomharte65816] SELFTEST FAIL: %s: %s\n", vector->name.c_str(), why.c_str());
            return 1;
        }
    }

    // Prove that a pin mismatch is caught rather than accidentally reduced to
    // the already-existing state/cycle assertions.
    Vector bad = lda;
    bad.cycles[1].outputs = "d--remx-";
    std::string why;
    if (runVector(cpu, mem, bad, true, true, why) || why.find("bus[1]") == std::string::npos) {
        std::fprintf(stderr, "[tomharte65816] SELFTEST FAIL: bus mismatch was not diagnosed (%s)\n", why.c_str());
        return 1;
    }

    // A misplaced internal cycle must be diagnosed even though the total cycle
    // count and the active transactions are unchanged.
    Vector swapped = inc;
    std::swap(swapped.cycles[2], swapped.cycles[3]);
    if (runVector(cpu, mem, swapped, true, true, why) || why.find("bus[2]") == std::string::npos) {
        std::fprintf(stderr, "[tomharte65816] SELFTEST FAIL: misplaced internal cycle was not diagnosed (%s)\n", why.c_str());
        return 1;
    }
    if (!runVector(cpu, mem, swapped, true, true, why, true)) {
        std::fprintf(stderr, "[tomharte65816] SELFTEST FAIL: --active-only should ignore internal order (%s)\n", why.c_str());
        return 1;
    }

    std::puts("[tomharte65816] SELFTEST OK: every bus cycle — active roles, internal placement, data and pins");
    return 0;
}

std::set<int> parseHexList(const char* s) {
    std::set<int> out;
    while (s&&*s) { while (*s==','||*s==' ')++s; if (!*s) break; out.insert(int(std::strtol(s,nullptr,16))); while (*s&&*s!=',')++s; }
    return out;
}
int opcodeFromStem(const std::string& stem) {
    if (stem.size()>=2 && std::isxdigit((unsigned char)stem[0]) && std::isxdigit((unsigned char)stem[1]))
        return int(std::strtol(stem.substr(0,2).c_str(),nullptr,16));
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr,"usage: %s <dir> [--max N] [--only hh,..] [--skip hh,..] [--no-cycles] [--no-bus] [--active-only] [--verbose] [--examples K]\n",argv[0]); return 2; }
    if (!std::strcmp(argv[1], "--self-test")) return runSelfTest();
    const std::string dir = argv[1];
    long maxPerFile=-1; int examples=3; bool verbose=false, checkCycles=true, checkBus=true, activeOnly=false;
    std::set<int> only, skip;
    for (int i=2;i<argc;++i){ std::string a=argv[i];
        if      (a=="--max"&&i+1<argc) maxPerFile=std::strtol(argv[++i],nullptr,10);
        else if (a=="--examples"&&i+1<argc) examples=int(std::strtol(argv[++i],nullptr,10));
        else if (a=="--only"&&i+1<argc) only=parseHexList(argv[++i]);
        else if (a=="--skip"&&i+1<argc) skip=parseHexList(argv[++i]);
        else if (a=="--no-cycles") checkCycles=false;
        else if (a=="--no-bus") checkBus=false;
        else if (a=="--active-only") activeOnly=true;
        else if (a=="--verbose") verbose=true;
        else { std::fprintf(stderr,"unknown arg '%s'\n",a.c_str()); return 2; }
    }

    namespace fs = std::filesystem;
    if (!fs::exists(dir)||!fs::is_directory(dir)) { std::fprintf(stderr,"[tomharte65816] no data at '%s' — SKIP\n",dir.c_str()); return 77; }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) if (e.is_regular_file()&&e.path().extension()==".json") files.push_back(e.path());
    std::sort(files.begin(),files.end());
    if (files.empty()) { std::fprintf(stderr,"[tomharte65816] '%s' holds no .json — SKIP\n",dir.c_str()); return 77; }

    IIgsMemory mem;
    mem.setTestMode(true);   // flat 16 MB bus (Tom Harte models no MMU)
    CPU65816 cpu(&mem);
    std::printf("[tomharte65816] dir=%s files=%zu cycles=%s bus=%s\n", dir.c_str(), files.size(), checkCycles?"on":"off", !checkBus?"off":activeOnly?"active-only":"all-cycles");

    long grandTotal=0, grandPass=0, filesRun=0, xfails=0; bool anyFail=false;
    for (const fs::path& f : files) {
        const std::string stem=f.stem().string(); const int opc=opcodeFromStem(stem);
        if (!only.empty()&&(opc<0||!only.count(opc))) continue;
        if (skip.count(opc)) { std::printf("  %-6s : SKIP\n",stem.c_str()); continue; }
        std::ifstream in(f,std::ios::binary);
        if (!in){ std::fprintf(stderr,"  %-6s: cannot open\n",stem.c_str()); anyFail=true; continue; }
        std::string buf((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        const char* p=buf.c_str(); eat(p,'[');
        int total=0,passed=0; std::vector<std::pair<std::string,std::string>> firstFew; Vector v;
        while (true) {
            skipWs(p); if (*p==']'||*p=='\0') break;
            if (!parseVector(p,v)) break;
            std::string why; bool ok=runVector(cpu,mem,v,checkCycles,checkBus,why,activeOnly);
            if (!ok) { if (const char* reason = knownCorpusError(v)) { ok = true; ++xfails; if (verbose) std::printf("        ~ \"%s\"  XFAIL %s\n", v.name.c_str(), reason); } }
            ++total; if (ok)++passed; else if (int(firstFew.size())<examples) firstFew.push_back({v.name,why});
            if (maxPerFile>0&&total>=maxPerFile) break;
            skipWs(p); if (*p==','){++p;continue;} if (*p==']'){++p;break;}
        }
        ++filesRun; grandTotal+=total; grandPass+=passed;
        const bool fileOk=(passed==total); if (!fileOk) anyFail=true;
        std::printf("  %-6s : %6d/%-6d %s\n",stem.c_str(),passed,total,fileOk?"OK":"FAIL");
        if (!fileOk||verbose) for (auto& mm:firstFew) std::printf("        x \"%s\"  %s\n",mm.first.c_str(),mm.second.c_str());
    }
    std::printf("[tomharte65816] %s: %ld/%ld across %ld file(s), %ld known corpus error(s) carried as xfail%s\n", anyFail?"FAIL":"OK", grandPass, grandTotal, filesRun, xfails, anyFail?"  <<< MISMATCH":"");
    if (filesRun==0) { std::fprintf(stderr,"[tomharte65816] no files matched — SKIP\n"); return 77; }
    return anyFail?1:0;
}
