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
#include "Audio.h"
#include "Ui.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#ifdef __linux__
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Directory of the running executable (so `roms/` resolves regardless of the
// working directory). Empty on platforms where it can't be determined.
static std::string execDir() {
#ifdef __linux__
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; std::string p(buf); auto s = p.find_last_of('/'); if (s != std::string::npos) return p.substr(0, s + 1); }
#endif
    return {};
}

// Find a resource by trying the CWD, the exec dir, and the exec dir's parent
// (so it works whether you run from the repo root or build/). Returns the
// contents + the path that matched.
static std::vector<uint8_t> findResource(const std::string& rel, std::string& matched) {
    std::string ed = execDir();
    for (const std::string& base : { std::string(), ed, ed + "../" }) {
        std::string p = base + rel;
        auto data = readFile(p);
        if (!data.empty()) { matched = p; return data; }
    }
    matched = rel;
    return {};
}

// Resolve a path (CWD / exec dir / parent) without reading the file.
static std::string findPath(const std::string& rel) {
    std::string ed = execDir();
    for (const std::string& base : { std::string(), ed, ed + "../" }) {
        std::ifstream f(base + rel, std::ios::binary);
        if (f) return base + rel;
    }
    return {};
}

static void glfwErrorCallback(int e, const char* d) { std::fprintf(stderr, "GLFW error %d: %s\n", e, d); }

int main(int argc, char** argv) {
    std::printf("POMIIGS %s — Apple IIgs emulator\n", pomiigs::kVersionString);

    // ── Emulator ─────────────────────────────────────────────────────────
    // static so they outlive main() under Emscripten's callback main loop
    // (Ctx below holds references to them).
    static IIgsMemory mem;
    static CPU65816 cpu(&mem);
    static VGC vgc;
    static AudioOut audio;
    std::string matched;
    std::vector<uint8_t> rom;
    if (argc > 1) { rom = readFile(argv[1]); matched = argv[1]; }
    else {                                   // try ROM 03 then ROM 01, CWD + exec-relative
        rom = findResource("roms/iigs-rom03.rom", matched);
        if (rom.empty()) rom = findResource("roms/iigs-rom01.rom", matched);
    }
    static std::string romPathStr = matched;
    const char* romPath = romPathStr.c_str();
    bool romOk = !rom.empty() && mem.loadRom(rom);
    mem.setCpu(&cpu);
    if (romOk) { mem.reset(); cpu.hardReset(); std::printf("Loaded ROM: %s (%zu KB)\n", romPath, rom.size() / 1024); }
    else std::fprintf(stderr, "No ROM found — drop iigs-rom03.rom (256K) or iigs-rom01.rom (128K) in roms/\n");
    std::string chrMatched;
    std::vector<uint8_t> chr = findResource("roms/iigs-char.rom", chrMatched);
    bool chrOk = !chr.empty() && vgc.setCharRom(chr);
    // Optional ProDOS hard disk on slot 7: argv[2], else auto-find hdv/*.hdv.
    std::string hddPath = (argc > 2) ? argv[2] : findPath("hdv/Total Replay v6.0.hdv");
    bool hddOk = !hddPath.empty() && mem.loadHdd(hddPath);
    if (hddOk) std::printf("HDD (slot 7): %s\n", hddPath.c_str());

    // ── UI (menu bar / status bar / dialogs) ─────────────────────────────
    // File I/O stays here; the UI drives loads through these callbacks.
    static Ui ui(mem, cpu, vgc, audio);
    ui.onLoadRom = [&](const std::string& p) -> bool {
        std::string m; std::vector<uint8_t> data = findResource(p, m);
        if (data.empty() || !mem.loadRom(data)) return false;
        mem.reset(); cpu.hardReset();
        ui.romOk = true; ui.romPath = m;
        return true;
    };
    ui.onLoadHdd = [&](const std::string& p) -> bool {
        std::string rp = findPath(p); if (rp.empty()) rp = p;
        if (!mem.loadHdd(rp)) return false;
        ui.hddPath = rp;
        return true;
    };
    ui.romOk = romOk; ui.romPath = romPathStr; ui.chrOk = chrOk;
    ui.hddPath = hddOk ? hddPath : std::string();
    ui.running = romOk;

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

    static GLuint screenTex = 0;
    glGenTextures(1, &screenTex);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Heap-allocated so the state outlives main() under Emscripten's callback
    // main loop (main returns immediately there).
    struct Ctx {
        GLFWwindow* window; IIgsMemory& mem; CPU65816& cpu; VGC& vgc; AudioOut& audio; Ui& ui; GLuint tex;
    };
    static Ctx ctx{window, mem, cpu, vgc, audio, ui, screenTex};

    auto frame = [](void* p) {
        Ctx& c = *static_cast<Ctx*>(p);
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        // ── Host input → emulator (before this frame's emulation runs) ──
        // Suppressed while an ImGui widget wants the keyboard (menu open, Load
        // dialog text field) so UI typing doesn't leak into the $C000 latch.
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput)
            for (ImWchar ch : io.InputQueueCharacters)      // typed chars → $C000
                if (ch >= 0x20 && ch < 0x7F) { uint8_t a = uint8_t(ch); if (a >= 'a' && a <= 'z') a -= 0x20; c.mem.keyDown(a); }
        static const struct { ImGuiKey k; uint8_t code; } kSpecial[] = {  // Apple II key codes
            {ImGuiKey_Enter,0x0D},{ImGuiKey_KeypadEnter,0x0D},{ImGuiKey_Escape,0x1B},
            {ImGuiKey_LeftArrow,0x08},{ImGuiKey_RightArrow,0x15},
            {ImGuiKey_DownArrow,0x0A},{ImGuiKey_UpArrow,0x0B},{ImGuiKey_Backspace,0x7F} };
        if (!io.WantCaptureKeyboard)
            for (auto& s : kSpecial) if (ImGui::IsKeyPressed(s.k, false)) c.mem.keyDown(s.code);
        // Joystick 1 → paddles ($C064/5) + buttons ($C061/2, also open/solid-apple).
        int na = 0; const float* ax = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &na);
        if (ax && na >= 2) {
            c.mem.setPaddle(0, uint8_t((ax[0] * 0.5f + 0.5f) * 255.f));
            c.mem.setPaddle(1, uint8_t((ax[1] * 0.5f + 0.5f) * 255.f));
        }
        int nb = 0; const unsigned char* bt = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &nb);
        c.mem.setButton(0, (nb > 0 && bt[0]) || ImGui::IsKeyDown(ImGuiKey_LeftAlt));
        c.mem.setButton(1, (nb > 1 && bt[1]) || ImGui::IsKeyDown(ImGuiKey_RightAlt));

        if (c.ui.running) {                    // one video frame; budget follows $C036
            const long budget = c.mem.frameCycleBudget();   // 47684 fast / 17030 slow
            long spent = 0;
            while (spent < budget) { int cy = c.cpu.run(1); c.mem.tick(cy); spent += (cy > 0 ? cy : 1); }
        }
        c.audio.mixFrame(c.mem);               // speaker ($C030) + DOC → miniaudio
        const uint32_t* fb = c.vgc.render(c.mem);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c.vgc.width(), c.vgc.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, fb);

        c.ui.render(c.tex);                    // menu bar + screen + status bar + dialogs
        if (c.ui.quitRequested) glfwSetWindowShouldClose(c.window, 1);

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(c.window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(c.window);
    };

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(frame, &ctx, 0, 1);   // browser drives the loop
#else
    while (!glfwWindowShouldClose(window)) frame(&ctx);
    glDeleteTextures(1, &screenTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
#endif
    return 0;
}
