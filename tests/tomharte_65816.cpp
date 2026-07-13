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
// flat 16 MB array, so — exactly as in POM2 — we validate final register file
// + touched RAM (+ cycle count, unless --no-cycles). Each vector's `e` field
// selects emulation/native mode. P is compared with the phantom bits (0x30)
// masked only in emulation mode; in native mode M/X are real flags.
//
// Usage: tomharte_65816 <dir> [--max N] [--only hh,..] [--skip hh,..]
//                        [--no-cycles] [--verbose] [--examples K]
// Exit 0 = all matched / no data (soft skip); 1 = mismatch; 2 = usage.

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
#include <vector>

namespace {

struct RamCell { uint32_t addr; uint8_t val; };

struct CpuState {
    uint16_t pc = 0, s = 0, a = 0, x = 0, y = 0, d = 0;
    uint8_t  p = 0, dbr = 0, pbr = 0, e = 1;
    std::vector<RamCell> ram;
};

struct Vector {
    std::string name;
    CpuState initial, fin;
    int cycleCount = -1;
};

inline void skipWs(const char*& p) { while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') ++p; }
inline bool eat(const char*& p, char c) { skipWs(p); if (*p==c) { ++p; return true; } return false; }
inline uint32_t parseUint(const char*& p) { skipWs(p); uint32_t v=0; while (*p>='0'&&*p<='9'){v=v*10u+uint32_t(*p-'0');++p;} return v; }
inline void skipString(const char*& p) { skipWs(p); if (*p!='"') return; ++p; while (*p&&*p!='"') ++p; if (*p=='"') ++p; }

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

int countCycles(const char*& p) {
    eat(p,'['); skipWs(p);
    if (*p==']'){++p;return 0;}
    int n=0, depth=1;
    while (*p&&depth>0) { if (*p=='"'){skipString(p);continue;} if (*p=='['){if(depth==1)++n;++depth;++p;continue;} if (*p==']'){--depth;++p;continue;} ++p; }
    return n;
}

bool parseVector(const char*& p, Vector& v) {
    skipWs(p); if (*p!='{') return false; ++p;
    v.name.clear(); v.cycleCount=-1;
    while (true) {
        skipWs(p);
        if (*p=='}'){++p;break;}
        if (*p=='"') {
            ++p; const char* k=p; while (*p&&*p!='"') ++p; const size_t kl=size_t(p-k); const char* key=k;
            if (*p=='"') ++p; eat(p,':');
            if (kl==4 && !std::strncmp(key,"name",4)) { skipWs(p); if (*p=='"'){++p;const char* s=p;while(*p&&*p!='"')++p;v.name.assign(s,size_t(p-s));if(*p=='"')++p;} }
            else if (kl==7 && !std::strncmp(key,"initial",7)) parseState(p, v.initial);
            else if (kl==5 && !std::strncmp(key,"final",5))   parseState(p, v.fin);
            else if (kl==6 && !std::strncmp(key,"cycles",6))  v.cycleCount = countCycles(p);
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

bool runVector(CPU65816& cpu, IIgsMemory& mem, const Vector& v, bool checkCycles, std::string& why) {
    loadState(cpu, v.initial);
    for (const RamCell& c : v.initial.ram) mem.write8(c.addr, c.val);

    const int cyc = cpu.run(1);

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
    for (const RamCell& c : v.fin.ram) {
        const uint8_t got = mem.read8(c.addr);
        if (got != c.val) { if (ok){std::snprintf(buf,sizeof buf,"RAM[$%06X] got $%02X want $%02X",c.addr,got,c.val);why=buf;} ok=false; }
    }
    for (const RamCell& c : v.initial.ram) mem.write8(c.addr, 0);
    for (const RamCell& c : v.fin.ram)     mem.write8(c.addr, 0);
    return ok;
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
    if (argc < 2) { std::fprintf(stderr,"usage: %s <dir> [--max N] [--only hh,..] [--skip hh,..] [--no-cycles] [--verbose] [--examples K]\n",argv[0]); return 2; }
    const std::string dir = argv[1];
    long maxPerFile=-1; int examples=3; bool verbose=false, checkCycles=true;
    std::set<int> only, skip;
    for (int i=2;i<argc;++i){ std::string a=argv[i];
        if      (a=="--max"&&i+1<argc) maxPerFile=std::strtol(argv[++i],nullptr,10);
        else if (a=="--examples"&&i+1<argc) examples=int(std::strtol(argv[++i],nullptr,10));
        else if (a=="--only"&&i+1<argc) only=parseHexList(argv[++i]);
        else if (a=="--skip"&&i+1<argc) skip=parseHexList(argv[++i]);
        else if (a=="--no-cycles") checkCycles=false;
        else if (a=="--verbose") verbose=true;
        else { std::fprintf(stderr,"unknown arg '%s'\n",a.c_str()); return 2; }
    }

    namespace fs = std::filesystem;
    if (!fs::exists(dir)||!fs::is_directory(dir)) { std::fprintf(stderr,"[tomharte65816] no data at '%s' — soft skip\n",dir.c_str()); return 0; }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) if (e.is_regular_file()&&e.path().extension()==".json") files.push_back(e.path());
    std::sort(files.begin(),files.end());
    if (files.empty()) { std::fprintf(stderr,"[tomharte65816] '%s' holds no .json — soft skip\n",dir.c_str()); return 0; }

    IIgsMemory mem;
    mem.setTestMode(true);   // flat 16 MB bus (Tom Harte models no MMU)
    CPU65816 cpu(&mem);
    std::printf("[tomharte65816] dir=%s files=%zu cycles=%s\n", dir.c_str(), files.size(), checkCycles?"on":"off");

    long grandTotal=0, grandPass=0, filesRun=0; bool anyFail=false;
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
            std::string why; const bool ok=runVector(cpu,mem,v,checkCycles,why);
            ++total; if (ok)++passed; else if (int(firstFew.size())<examples) firstFew.push_back({v.name,why});
            if (maxPerFile>0&&total>=maxPerFile) break;
            skipWs(p); if (*p==','){++p;continue;} if (*p==']'){++p;break;}
        }
        ++filesRun; grandTotal+=total; grandPass+=passed;
        const bool fileOk=(passed==total); if (!fileOk) anyFail=true;
        std::printf("  %-6s : %6d/%-6d %s\n",stem.c_str(),passed,total,fileOk?"OK":"FAIL");
        if (!fileOk||verbose) for (auto& mm:firstFew) std::printf("        x \"%s\"  %s\n",mm.first.c_str(),mm.second.c_str());
    }
    std::printf("[tomharte65816] %s: %ld/%ld across %ld file(s)%s\n", anyFail?"FAIL":"OK", grandPass, grandTotal, filesRun, anyFail?"  <<< MISMATCH":"");
    if (filesRun==0) { std::fprintf(stderr,"[tomharte65816] no files matched — soft skip\n"); return 0; }
    return anyFail?1:0;
}
