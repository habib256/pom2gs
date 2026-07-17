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
#include "Snapshot.h"
#include "Ui.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <filesystem>
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

// ── Config file (pomiigs.cfg) ────────────────────────────────────────────
// Plain `key = value`, `#` comments. Keys: boot (gsos|finder|hdd), rom, hdd,
// disk35, iwm35 (0/1: 3.5" media on the real IWM Sony drive instead of the
// SmartPort HLE). Sits next to run_emulator.sh (repo root). CLI args/flags
// override it.
struct Config { std::string boot, rom, hdd, disk35, disk35b, disk525; bool iwm35 = false; };
static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static Config readConfig(std::string& usedPath) {
    Config c;
    usedPath = findPath("pomiigs.cfg");
    if (usedPath.empty()) return c;
    std::ifstream f(usedPath); std::string line;
    while (std::getline(f, line)) {
        auto h = line.find('#'); if (h != std::string::npos) line.erase(h);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if      (k == "boot")   c.boot   = v;
        else if (k == "rom")    c.rom    = v;
        else if (k == "hdd")    c.hdd    = v;
        else if (k == "disk35")  c.disk35 = v;
        else if (k == "disk525") c.disk525 = v;
        else if (k == "disk35b") c.disk35b = v;   // second Sony drive (needs iwm35)
        else if (k == "iwm35")   c.iwm35  = (v == "1" || v == "true" || v == "yes");
    }
    return c;
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
    // ── Config file + args (precedence: CLI > pomiigs.cfg > built-in) ────
    // Boot target: `boot = gsos|finder` in pomiigs.cfg (or `--gsos`) boots the
    // Finder from a slot-5 3.5" disk; `boot = hdd` (or default / `--hdd`) boots
    // the slot-7 ProDOS HDD. Paths: `disk35`/`hdd`/`rom` in the config, or the
    // CLI positionals (ROM, then HDD) / `--gsos <disk>`.
    std::string cfgPath; Config cfg = readConfig(cfgPath);
    if (!cfgPath.empty()) std::printf("Config: %s (boot=%s)\n", cfgPath.c_str(),
                                      cfg.boot.empty() ? "default" : cfg.boot.c_str());

    bool forceGsos = false, forceHdd = false; std::string gsosDisk;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gsos" || a == "--finder") { forceGsos = true; if (i + 1 < argc && argv[i + 1][0] != '-') gsosDisk = argv[++i]; }
        else if (a == "--hdd") forceHdd = true;
        else if (a == "--iwm35") cfg.iwm35 = true;
        else pos.push_back(a);
    }
    mem.setIwm35(cfg.iwm35);   // before any loadDisk35 below
    if (cfg.iwm35) std::printf("3.5\" drive: real IWM/Sony (internal ROM firmware)\n");
    if (!cfg.disk525.empty()) {
        std::string rp = findPath(cfg.disk525);
        if (!rp.empty() && mem.loadDisk525(rp)) std::printf("5.25\" drive (slot 6): %s\n", rp.c_str());
        else std::fprintf(stderr, "5.25\" drive: cannot load '%s'\n", cfg.disk525.c_str());
    }
    if (cfg.iwm35 && !cfg.disk35b.empty()) {
        std::string rp = findPath(cfg.disk35b);
        if (!rp.empty() && mem.loadDisk35(rp, 1))
            std::printf("3.5\" drive 2: %s\n", rp.c_str());
        else std::fprintf(stderr, "3.5\" drive 2: cannot load '%s'\n", cfg.disk35b.c_str());
    }
    const bool bootGsos = forceGsos ||
        (!forceHdd && (cfg.boot == "gsos" || cfg.boot == "finder"));

    // ROM: CLI positional > config `rom` > auto-find (ROM 03 then ROM 01).
    std::string matched;
    std::vector<uint8_t> rom;
    std::string romArg = !pos.empty() ? pos[0] : cfg.rom;
    if (!romArg.empty()) rom = findResource(romArg, matched);
    if (rom.empty()) { rom = findResource("roms/iigs-rom03.rom", matched);
                       if (rom.empty()) rom = findResource("roms/iigs-rom01.rom", matched); }
    static std::string romPathStr = matched;
    const char* romPath = romPathStr.c_str();
    bool romOk = !rom.empty() && mem.loadRom(rom);
    mem.setCpu(&cpu);
    if (romOk) { mem.reset(); cpu.hardReset(); std::printf("Loaded ROM: %s (%zu KB)\n", romPath, rom.size() / 1024); }
    else std::fprintf(stderr, "No ROM found — drop iigs-rom03.rom (256K) or iigs-rom01.rom (128K) in roms/\n");
    std::string chrMatched;
    std::vector<uint8_t> chr = findResource("roms/iigs-char.rom", chrMatched);
    bool chrOk = !chr.empty() && vgc.setCharRom(chr);

    // Boot disk: Finder (slot-5 3.5") or ProDOS HDD (slot 7).
    std::string hddPath, disk35Path; bool hddOk = false;
    if (bootGsos) {
        disk35Path = !gsosDisk.empty() ? gsosDisk
                   : (!cfg.disk35.empty() ? cfg.disk35
                                          : std::string("disks35/System 6.0.1/Disk 2 of 7 System Disk.2mg"));
        std::string rp = findPath(disk35Path); if (!rp.empty()) disk35Path = rp;
        if (mem.loadDisk35(disk35Path)) {
            std::printf("GS/OS Finder boot — 3.5\" (slot 5): %s\n", disk35Path.c_str());
            // Mount the slot-7 hard disk too, if configured, so the install
            // target is present while the Installer runs. The ROM boot scan
            // finds the HDD first (slot 7) but our slot-7 firmware re-points the
            // boot to slot 5 for a non-GS-bootable volume (see ProDosHdd), and
            // GS/OS now boots to the Finder with the HDD mounted (writes persist
            // to the image, so a format / install sticks).
            if (!cfg.hdd.empty()) {
                hddPath = cfg.hdd; std::string hp = findPath(hddPath); if (!hp.empty()) hddPath = hp;
                hddOk = mem.loadHdd(hddPath);
                // Only keep an install-target (non-GS-bootable) HDD in GS/OS mode:
                // the slot-7 boot scan runs first, so a *bootable* HDD (e.g. Total
                // Replay) would hijack the boot instead of loading the 3.5" GS/OS.
                // A blank/data volume (block-0 byte 0 != $01) chains to slot 5.
                if (hddOk && mem.hddBootable()) {
                    std::printf("GS/OS: '%s' is bootable — leaving slot 7 empty so GS/OS boots"
                                " (use `boot = hdd` to boot it).\n", hddPath.c_str());
                    mem.ejectHdd(); hddOk = false; hddPath.clear();
                } else if (hddOk) {
                    std::printf("           + hard disk (slot 7): %s\n", hddPath.c_str());
                } else {
                    std::fprintf(stderr, "GS/OS: hard disk '%s' not found (set `hdd =` in pomiigs.cfg)\n", hddPath.c_str());
                }
            } else {
                mem.ejectHdd();              // no HDD configured → slot 7 empty
            }
        } else std::fprintf(stderr, "GS/OS: no 3.5\" disk at '%s' — set `disk35 =` in pomiigs.cfg\n", disk35Path.c_str());
    } else {
        hddPath = (pos.size() > 1) ? pos[1] : (!cfg.hdd.empty() ? cfg.hdd : std::string("hdv/Total Replay v6.0.hdv"));
        std::string rp = findPath(hddPath); if (!rp.empty()) hddPath = rp;
        hddOk = mem.loadHdd(hddPath);
        if (hddOk) std::printf("HDD (slot 7): %s\n", hddPath.c_str());
        // Mount the configured 3.5" too (the slot-7 bootable HDD still wins the
        // boot scan). GS/OS only auto-detects LATER menu swaps on a drive that was
        // ONLINE at boot — an empty-at-boot SmartPort drive is enumerated offline
        // and never re-polled (its $2E disk-switched signal needs a volume access
        // that never comes). With a disk in at boot, hot-swapping to any other
        // disk works (the online volume's next access returns $2E → remount).
        if (!cfg.disk35.empty()) {
            disk35Path = cfg.disk35;
            std::string dp = findPath(disk35Path); if (!dp.empty()) disk35Path = dp;
            if (mem.loadDisk35(disk35Path)) std::printf("3.5\" (slot 5): %s\n", disk35Path.c_str());
            else disk35Path.clear();
        }
    }

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
        mem.reset(); cpu.hardReset();
        return true;
    };
    // Load an 800K 3.5" disk on slot 5. Eject the slot-7 HDD so the ROM boots
    // the 3.5" (it scans the higher slots first), then cold-reset.
    ui.onLoadDisk35 = [&](const std::string& p) -> bool {
        std::string rp = findPath(p); if (rp.empty()) rp = p;
        if (!mem.loadDisk35(rp)) return false;
        mem.ejectHdd();
        ui.hddPath = rp;
        mem.reset(); cpu.hardReset();
        return true;
    };
    // Hot-swap a 3.5" disk mid-run (no reset) — for the Installer's disk prompts.
    ui.onSwapDisk35 = [&](const std::string& p) -> bool {
        std::string rp = findPath(p); if (rp.empty()) rp = p;
        return mem.swapDisk35(rp);
    };
    ui.onEjectDisk35 = [&]() { mem.ejectDisk35(); };
    // Load a 5.25" disk on the slot-6 IWM (.dsk/.do/.po/.nib/.d13/.2mg/.woz).
    // Eject the HDD + 3.5" so the ROM's boot scan reaches slot 6, then
    // cold-reset. Writes persist back to the image (DiskImage::saveDirty).
    ui.onLoadDisk525 = [&](const std::string& p) -> bool {
        std::string rp = findPath(p); if (rp.empty()) rp = p;
        if (!mem.loadDisk525(rp)) return false;
        mem.ejectHdd(); mem.ejectDisk35();
        ui.disk525Path = rp;
        mem.reset(); cpu.hardReset();
        return true;
    };
    ui.onEjectDisk525 = [&]() { mem.ejectDisk525(); };
    // Quick save/load state (F7/F8) → states/quick.pgss next to the config.
    ui.onSaveState = [&]() -> bool {
        std::error_code ec; std::filesystem::create_directories("states", ec);
        return saveSnapshot("states/quick.pgss", cpu, mem);
    };
    ui.onLoadState = [&]() -> bool { return loadSnapshot("states/quick.pgss", cpu, mem); };
    // Populate the "3.5\" Drive" quick-swap menu in EVERY boot mode (it used to be
    // gsos-only, which left the menu empty when booting from the HDD — no way to
    // insert e.g. the synthLAB disk into the running Finder). Source folder: the
    // boot 3.5" if there is one, else the folder of the configured `disk35 =`,
    // else `disks35/` (and its immediate subfolders, e.g. "System 6.0.1/").
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!disk35Path.empty()) ui.disk35Path = disk35Path;
        std::string src = !disk35Path.empty() ? disk35Path : cfg.disk35;
        fs::path dir;
        if (!src.empty()) { std::string rp = findPath(src); dir = fs::path(rp.empty() ? src : rp).parent_path(); }
        if (dir.empty() || !fs::is_directory(dir, ec)) { std::string rp = findPath("disks35"); dir = rp.empty() ? "disks35" : rp; }
        std::vector<std::pair<std::string, std::string>> items;
        auto scan = [&](const fs::path& d) {
            for (const auto& e : fs::directory_iterator(d, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string ext = e.path().extension().string();
                for (auto& c : ext) c = char(std::tolower((unsigned char)c));
                if (ext == ".2mg" || ext == ".po" || ext == ".dsk")
                    items.emplace_back(e.path().filename().string(), e.path().string());
            }
        };
        scan(dir);
        if (items.empty())                                 // bare disks35/ → look one level down
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_directory(ec)) scan(e.path());
        std::sort(items.begin(), items.end());
        ui.disk35Menu = std::move(items);
    }
    ui.romOk = romOk; ui.romPath = romPathStr; ui.chrOk = chrOk;
    ui.hddPath = (hddOk && !hddPath.empty()) ? hddPath : (bootGsos ? disk35Path : std::string());
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
    if (romOk) ui.setStatus("Press Del to capture the mouse", 8.0f);   // needs the ImGui ctx

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
        // Mouse capture (Del toggles, below). While captured, ImGui ignores the
        // pointer so clicks/hover don't leak into the menus — the raw relative
        // motion drives the GS ADB mouse instead. Set before NewFrame samples it.
        static bool mouseCaptured = false;
        if (mouseCaptured) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else               ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        // ── Host input → emulator (before this frame's emulation runs) ──
        // Suppressed while an ImGui widget wants the keyboard (menu open, Load
        // dialog text field) so UI typing doesn't leak into the $C000 latch.
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput)
            for (ImWchar ch : io.InputQueueCharacters)      // typed chars → $C000
                if (ch >= 0x20 && ch < 0x7F) c.mem.keyDown(uint8_t(ch));  // verbatim case: the IIgs ADB keyboard latches real ASCII (lowercase too)
        static const struct { ImGuiKey k; uint8_t code; } kSpecial[] = {  // Apple II key codes
            {ImGuiKey_Enter,0x0D},{ImGuiKey_KeypadEnter,0x0D},{ImGuiKey_Escape,0x1B},
            {ImGuiKey_LeftArrow,0x08},{ImGuiKey_RightArrow,0x15},
            {ImGuiKey_DownArrow,0x0A},{ImGuiKey_UpArrow,0x0B},{ImGuiKey_Backspace,0x7F} };
        if (!io.WantCaptureKeyboard)
            for (auto& s : kSpecial) if (ImGui::IsKeyPressed(s.k, false)) c.mem.keyDown(s.code);
        // Any-key-down → $C010 bit7: reflect the live physical key state (the
        // char-event path above is edge-only and can't model a held key).
        bool akd = false;
        if (!io.WantCaptureKeyboard) {
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z && !akd; ++k) akd = ImGui::IsKeyDown(ImGuiKey(k));
            for (int k = ImGuiKey_0; k <= ImGuiKey_9 && !akd; ++k) akd = ImGui::IsKeyDown(ImGuiKey(k));
            if (!akd) akd = ImGui::IsKeyDown(ImGuiKey_Space)   || ImGui::IsKeyDown(ImGuiKey_Enter)     ||
                            ImGui::IsKeyDown(ImGuiKey_Backspace)|| ImGui::IsKeyDown(ImGuiKey_Escape)    ||
                            ImGui::IsKeyDown(ImGuiKey_LeftArrow)|| ImGui::IsKeyDown(ImGuiKey_RightArrow)||
                            ImGui::IsKeyDown(ImGuiKey_UpArrow)  || ImGui::IsKeyDown(ImGuiKey_DownArrow);
        }
        c.mem.setAnyKeyDown(akd);
        // Command/Option/Control shortcuts: these modifiers suppress ImGui's
        // InputQueueCharacters, so the base key never reaches the $C000 path
        // above. Deliver it directly here (the $C025 modifiers set below make the
        // ROM build a command-key event → GS/OS menu shortcuts fire, e.g. ⌘-A).
        const bool shortcutMod = ImGui::IsKeyDown(ImGuiKey_LeftAlt) ||
                                 ImGui::IsKeyDown(ImGuiKey_RightAlt) || io.KeyCtrl;
        if (shortcutMod && !io.WantCaptureKeyboard) {
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
                if (ImGui::IsKeyPressed(ImGuiKey(k), false)) c.mem.keyDown(uint8_t('A' + (k - ImGuiKey_A)));
            for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k)
                if (ImGui::IsKeyPressed(ImGuiKey(k), false)) c.mem.keyDown(uint8_t('0' + (k - ImGuiKey_0)));
        }
        // Joystick 1 → paddles ($C064/5) + buttons ($C061/2, also open/solid-apple).
        int na = 0; const float* ax = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &na);
        if (ax && na >= 2) {
            c.mem.setPaddle(0, uint8_t((ax[0] * 0.5f + 0.5f) * 255.f));
            c.mem.setPaddle(1, uint8_t((ax[1] * 0.5f + 0.5f) * 255.f));
        }
        int nb = 0; const unsigned char* bt = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &nb);
        c.mem.setButton(0, (nb > 0 && bt[0]) || ImGui::IsKeyDown(ImGuiKey_LeftAlt));
        c.mem.setButton(1, (nb > 1 && bt[1]) || ImGui::IsKeyDown(ImGuiKey_RightAlt));

        // ── Mouse → ADB GLU ($C024). Del enters/exits capture ────────────
        // The GS cursor is drawn by GS/OS, so a usable mouse must be *captured*:
        // GLFW locks + hides the OS pointer (GLFW_CURSOR_DISABLED) and we feed the
        // raw relative motion to the ADB mouse. Released mode hands the pointer
        // back to ImGui (menu bar, dialogs). Del toggles.
        static double lastX = 0, lastY = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            mouseCaptured = !mouseCaptured;
            glfwSetInputMode(c.window, GLFW_CURSOR,
                             mouseCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            glfwGetCursorPos(c.window, &lastX, &lastY);      // reset the delta baseline
            c.ui.setStatus(mouseCaptured ? "Mouse captured — press Del to release"
                                         : "Mouse released — press Del to capture");
        }
        if (mouseCaptured) {
            double mx, my; glfwGetCursorPos(c.window, &mx, &my);
            int dx = int(mx - lastX), dy = int(my - lastY);
            lastX = mx; lastY = my;
            if (dx || dy) c.mem.mouseMove(dx, dy);
            c.mem.mouseButton(glfwGetMouseButton(c.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        } else if (!io.WantCaptureMouse) {                   // windowed fallback (no capture)
            if (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f)
                c.mem.mouseMove(int(io.MouseDelta.x), int(io.MouseDelta.y));
            c.mem.mouseButton(ImGui::IsMouseDown(ImGuiMouseButton_Left));
        }
        // Host modifier keys → ADB KEYMODREG ($C025). ⌘/open-apple = LeftAlt,
        // option/solid-apple = RightAlt (matches the joystick-button mapping).
        c.mem.setKeyModifiers(
            uint8_t((ImGui::IsKeyDown(ImGuiKey_LeftAlt)   ? 0x80 : 0) |   // command
                    (ImGui::IsKeyDown(ImGuiKey_RightAlt)  ? 0x40 : 0) |   // option
                    (ImGui::IsKeyDown(ImGuiKey_CapsLock)  ? 0x04 : 0) |
                    (io.KeyCtrl                            ? 0x02 : 0) |
                    (io.KeyShift                           ? 0x01 : 0)));

        if (c.ui.running) {                    // one video frame, in master-clock ticks
            const long target = c.mem.masterPerFrame();     // 238420 (@ 60 Hz)
            long master = 0;                   // per-step cost tracks live $C036 + slow-side stall
            while (master < target) {
                int cy = c.cpu.run(1); c.mem.tick(cy);
                master += long(cy > 0 ? cy : 1) * (c.mem.speedFast() ? 5 : 14) + c.mem.takeSlowPenalty();
            }
            c.mem.frameTick();                 // ¼-sec / 1-sec / scan-line / DOC interrupts
        }
        c.audio.mixFrame(c.mem);               // speaker ($C030) + DOC → miniaudio
        // POMDBG=1: once-per-second health line on stderr — wall FPS (60 = keeping
        // up), slot-5 device calls (the GS/OS removable poll's liveness — ~1-4/s at
        // an idle Finder; 0 = the poll is dead, disk swaps won't mount), DOC native
        // samples/s (~26320 with 32 oscillators), audio-device underruns (each one
        // is an audible crackle).
        static const bool pomdbg = std::getenv("POMDBG") != nullptr;
        if (pomdbg) {
            static int nf = 0; static double t0 = glfwGetTime();
            static uint32_t sp0 = 0, un0 = 0; static uint64_t pr0 = 0;
            if (++nf >= 60) {
                const double t1 = glfwGetTime();
                const uint32_t sp = c.mem.sp5Calls(), un = c.audio.underruns();
                const uint64_t pr = c.mem.docChip().producedTotal();
                static uint32_t dr0 = 0; const uint32_t dr = c.audio.drops();
                std::fprintf(stderr, "[dbg] fps=%5.1f  sp5=%3u/s  doc=%6llu smp/s  underruns=+%u  drops=+%u  ring=%u\n",
                             nf / (t1 - t0), sp - sp0,
                             (unsigned long long)(pr - pr0), un - un0, dr - dr0, c.audio.ringFill());
                dr0 = dr;
                nf = 0; t0 = t1; sp0 = sp; un0 = un; pr0 = pr;
            }
        }
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
    mem.iwm().flushDisk35();    // commit pending 3.5" sector writes (real-IWM path)
    mem.iwm().flushDisk525();   // commit pending 5.25" writes (DiskImage::saveDirty)
    glDeleteTextures(1, &screenTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
#endif
    return 0;
}
