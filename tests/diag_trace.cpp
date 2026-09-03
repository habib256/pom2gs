// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Headless "boot this disk and show me the text page" runner for the
// cycle-accuracy catalog's guest diagnostics (MiSTer custom tests, TrueGS,
// service diagnostics…). Mounts the same media set as `screenshot`, runs a
// fixed number of frames with the periodic Mega II/VGC interrupts, then prints
// the 40-column text page as `[text NN] |....|` rows so a driver script can
// match verdicts deterministically. Media are host-read-only unless
// --writeback is given (the floppy R/W test needs it).
//
//   diag_trace <rom> [--frames N] [--disk35 img] [--disk35b img]
//                    [--disk525 img] [--hdd img] [--iwm35] [--writeback]
//
// Exit 0 = ran; 2 = usage/bad ROM; 77 = a named media file is missing (SKIP).

#include "CPU65816.h"
#include "IIgsMemory.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool exists(const char* path) { return path && std::ifstream(path).good(); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom> [--frames N] [--disk35 img] [--disk35b img] [--disk525 img] [--hdd img] [--iwm35] [--writeback]\n", argv[0]);
        return 2;
    }
    const char* romPath = argv[1];
    long frames = 600;
    const char* hddPath = nullptr; const char* disk35Path = nullptr; const char* disk35bPath = nullptr;
    const char* disk525Path = nullptr; bool iwm35 = false, writeBack = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--hdd") && i + 1 < argc) hddPath = argv[++i];
        else if (!std::strcmp(argv[i], "--disk35") && i + 1 < argc) disk35Path = argv[++i];
        else if (!std::strcmp(argv[i], "--disk35b") && i + 1 < argc) disk35bPath = argv[++i];
        else if (!std::strcmp(argv[i], "--disk525") && i + 1 < argc) disk525Path = argv[++i];
        else if (!std::strcmp(argv[i], "--iwm35")) iwm35 = true;
        else if (!std::strcmp(argv[i], "--writeback")) writeBack = true;
        else { std::fprintf(stderr, "unknown arg '%s'\n", argv[i]); return 2; }
    }
    for (const char* m : {hddPath, disk35Path, disk35bPath, disk525Path})
        if (m && !exists(m)) { std::fprintf(stderr, "[diag] media missing: %s — SKIP\n", m); return 77; }

    std::vector<uint8_t> rom = readFile(romPath);
    IIgsMemory mem; CPU65816 cpu(&mem);
    if (rom.empty() || !mem.loadRom(rom)) { std::fprintf(stderr, "[diag] bad ROM %s\n", romPath); return 2; }
    mem.setCpu(&cpu); mem.reset(); mem.setMediaWriteBack(writeBack);
    mem.setIwm35(iwm35); if (hddPath) mem.loadHdd(hddPath);
    if (disk35Path) { mem.loadDisk35(disk35Path); if (!hddPath) mem.ejectHdd(); }
    if (disk35bPath) mem.loadDisk35(disk35bPath, 1);
    if (disk525Path) { mem.loadDisk525(disk525Path); if (!hddPath) mem.ejectHdd(); }
    cpu.hardReset();
    std::printf("[diag] rom=%s frames=%ld disk35=%s disk35b=%s disk525=%s hdd=%s iwm35=%d writeback=%d\n", romPath, frames,
                disk35Path ? disk35Path : "-", disk35bPath ? disk35bPath : "-", disk525Path ? disk525Path : "-",
                hddPath ? hddPath : "-", iwm35, writeBack);

    for (long f = 0; f < frames; ++f) {
        uint64_t spent = 0;
        while (spent < uint64_t(mem.masterPerFrame())) spent += mem.tick(cpu.run(1));
        mem.frameTick();
    }
    std::printf("[diag] end $%02X:%04X A=%04X X=%04X Y=%04X SP=%04X P=%02X %s shadow=$%02X speed=$%02X\n",
                cpu.getPBR(), cpu.getPC(), cpu.getA(), cpu.getX(), cpu.getY(), cpu.getSP(), cpu.getP(),
                cpu.getEmulationMode() ? "e" : "n", mem.shadowReg(), mem.speedReg());
    const uint8_t* e0 = mem.slowRam();
    for (int row = 0; row < 24; ++row) {
        const int rbase = 0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28;
        std::string line;
        for (int col = 0; col < 40; ++col) {
            uint8_t c = e0[rbase + col] & 0x7F;
            if (c < 0x20) c += 0x40;
            line += char(c);
        }
        std::printf("[text %02d] |%s|\n", row, line.c_str());
    }
    return 0;
}
