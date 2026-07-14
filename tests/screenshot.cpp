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
    long frames = 120; const char* charPath = "roms/iigs-char.rom";
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--char") && i + 1 < argc) charPath = argv[++i];
    }

    std::vector<uint8_t> rom = readFile(romPath);
    IIgsMemory mem; CPU65816 cpu(&mem); VGC vgc;
    if (rom.empty() || !mem.loadRom(rom)) { std::fprintf(stderr, "bad ROM %s\n", romPath); return 2; }
    mem.setCpu(&cpu); mem.reset(); cpu.hardReset();
    std::vector<uint8_t> chr = readFile(charPath);
    bool chrOk = !chr.empty() && vgc.setCharRom(chr);

    for (long f = 0; f < frames; ++f) {
        long spent = 0;
        while (spent < 46000) { int c = cpu.run(1); mem.tick(c); spent += (c > 0 ? c : 1); }
    }
    const uint32_t* fb = vgc.render(mem);
    if (!stbi_write_png(outPath, vgc.width(), vgc.height(), 4, fb, vgc.width() * 4)) {
        std::fprintf(stderr, "png write failed\n"); return 1;
    }
    std::printf("wrote %s (%dx%d) after %ld frames — mode=%s, char ROM=%s, CPU $%02X:%04X\n",
                outPath, vgc.width(), vgc.height(), frames,
                mem.shrEnabled() ? "SHR" : "text", chrOk ? "yes" : "no",
                cpu.getPBR(), cpu.getPC());
    return 0;
}
