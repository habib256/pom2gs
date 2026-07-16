// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Headless screenshot: run a real ROM for N frames and write the VGC output as
// a PNG. Lets you see the boot screen without a display. Dev tool.
//
//   screenshot <rom> <out.png> [--frames N] [--char roms/iigs-char.rom]

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include "CPU65816.h"
#include "IIgsMemory.h"
#include "VGC.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static std::vector<uint8_t> readFile(const char* p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <rom> <out.png> [--frames N] [--char file]\n", argv[0]); return 2; }
    const char* romPath = argv[1];
    const char* outPath = argv[2];
    long frames = 120; const char* charPath = "roms/iigs-char.rom"; const char* hddPath = nullptr;
    const char* disk35Path = nullptr;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--char") && i + 1 < argc) charPath = argv[++i];
        else if (!std::strcmp(argv[i], "--hdd") && i + 1 < argc) hddPath = argv[++i];
        else if (!std::strcmp(argv[i], "--disk35") && i + 1 < argc) disk35Path = argv[++i];
    }

    std::vector<uint8_t> rom = readFile(romPath);
    IIgsMemory mem; CPU65816 cpu(&mem); VGC vgc;
    if (rom.empty() || !mem.loadRom(rom)) { std::fprintf(stderr, "bad ROM %s\n", romPath); return 2; }
    mem.setCpu(&cpu); mem.reset(); if (hddPath) mem.loadHdd(hddPath);
    // --disk35 alone → boot the 3.5" (eject the HDD). Both → keep both (install
    // scenario: blank HDD on slot 7 as target, boot chains to the slot-5 disk).
    if (disk35Path) { mem.loadDisk35(disk35Path); if (!hddPath) mem.ejectHdd(); }
    cpu.hardReset();
    std::vector<uint8_t> chr = readFile(charPath);
    bool chrOk = !chr.empty() && vgc.setCharRom(chr);

    const char* jig = getenv("MOUSE_JIGGLE");   // dev: inject mouse motion each frame
    const char* dir = getenv("MOUSE_DIR");      // dev: steady directional motion (cursor tracking)
    const char* key = getenv("KEY_INJECT");     // dev: type this char once after boot
    const bool doFrameTick = getenv("FRAMETICK");   // match the interactive emulator's periodic IRQs
    for (long f = 0; f < frames; ++f) {
        long spent = 0;
        while (spent < 46000) { int c = cpu.run(1); mem.tick(c); spent += (c > 0 ? c : 1); }
        if (doFrameTick) mem.frameTick();
        if (jig && f > 14000) mem.mouseMove(((f & 1) ? 3 : -3), (f % 5) - 2);   // wiggle the ADB mouse (after boot)
        if (dir && f > 14000 && f < 14045) mem.mouseMove(5, 0);                 // steady move → cursor tracks
        if (key && f == 14500) { const char* km=getenv("KEY_MOD");             // press a key once
            if (km) mem.setKeyModifiers(uint8_t(strtol(km,0,16))); mem.keyDown(uint8_t(key[0])); }
    }
    const uint32_t* fb = vgc.render(mem);
    if (!stbi_write_png(outPath, vgc.width(), vgc.height(), 4, fb, vgc.width() * 4)) {
        std::fprintf(stderr, "png write failed\n"); return 1;
    }
    std::printf("  $C022=%02X (fg=%X bg=%X) border=%X\n", mem.textColor(), mem.textColor()>>4, mem.textColor()&0xF, mem.borderColor());
    std::printf("wrote %s (%dx%d) after %ld frames — mode=%s, char ROM=%s, CPU $%02X:%04X\n",
                outPath, vgc.width(), vgc.height(), frames,
                mem.shrEnabled() ? "SHR" : (mem.textMode() ? "text" : (mem.hires() ? "HGR" : "LORES")), chrOk ? "yes" : "no",
                cpu.getPBR(), cpu.getPC());
    return 0;
}
