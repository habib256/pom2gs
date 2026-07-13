// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// M3 shell: run the 65C816 + IIgs MMU against a real ROM and display the VGC
// framebuffer (Super Hi-Res / text) in the ImGui window. Structure mirrors
// POM2's main.cpp so it grows into the same shape.

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "Version.h"
#include "CPU65816.h"
#include "IIgsMemory.h"
#include "VGC.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void glfwErrorCallback(int e, const char* d) { std::fprintf(stderr, "GLFW error %d: %s\n", e, d); }

int main(int argc, char** argv) {
    std::printf("POMIIGS %s — Apple IIgs emulator\n", pomiigs::kVersionString);

    // ── Emulator ─────────────────────────────────────────────────────────
    IIgsMemory mem;
    CPU65816 cpu(&mem);
    VGC vgc;
    const char* romPath = (argc > 1) ? argv[1] : "roms/iigs-rom03.rom";
    std::vector<uint8_t> rom = readFile(romPath);
    bool romOk = !rom.empty() && mem.loadRom(rom);
    if (romOk) { mem.reset(); cpu.hardReset(); }
    else std::fprintf(stderr, "No/invalid ROM at %s — drop iigs-rom03.rom (256K) or iigs-rom01.rom (128K) in roms/\n", romPath);
    std::vector<uint8_t> chr = readFile("roms/iigs-char.rom");
    bool chrOk = !chr.empty() && vgc.setCharRom(chr);

    // ── Window / ImGui ───────────────────────────────────────────────────
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) { std::fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1000, 720, "POMIIGS — Apple IIgs", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    GLuint screenTex = 0;
    glGenTextures(1, &screenTex);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    bool running = romOk;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Run one video frame worth of emulation (2.8 MHz / 60 ≈ 46 k cycles).
        if (running) {
            long budget = 46000; long spent = 0;
            while (spent < budget) { int c = cpu.run(1); mem.tick(c); spent += (c > 0 ? c : 1); }
        }

        // Upload the VGC framebuffer.
        const uint32_t* fb = vgc.render(mem);
        glBindTexture(GL_TEXTURE_2D, screenTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vgc.width(), vgc.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, fb);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("Apple IIgs");
        ImVec2 sz(vgc.width() * 1.25f, vgc.height() * 1.25f);
        ImGui::Image((ImTextureID)(intptr_t)screenTex, sz);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(840, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("POMIIGS");
        ImGui::Text("v%s", pomiigs::kVersionString);
        ImGui::Separator();
        ImGui::Text("ROM: %s", romOk ? romPath : "MISSING");
        ImGui::Text("char ROM: %s", chrOk ? "loaded" : "absent (text off)");
        ImGui::Text("mode: %s", mem.shrEnabled() ? "Super Hi-Res" : "text/legacy");
        ImGui::Text("CPU $%02X:%04X %s", cpu.getPBR(), cpu.getPC(), cpu.getEmulationMode() ? "e" : "n");
        ImGui::Text("shadow $%02X speed $%02X", mem.shadowReg(), mem.speedReg());
        if (ImGui::Button(running ? "Pause" : "Run")) running = !running && romOk;
        ImGui::SameLine();
        if (ImGui::Button("Reset") && romOk) { mem.reset(); cpu.hardReset(); }
        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    glDeleteTextures(1, &screenTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
