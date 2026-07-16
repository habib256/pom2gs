// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Emulator UI. See Ui.h. Menu/status-bar structure inspired by POM1's
// MainWindow_Menu (menu bar + bottom status bar + modal dialogs).

#include "Ui.h"
#include "IIgsMemory.h"
#include "CPU65816.h"
#include "VGC.h"
#include "Audio.h"
#include "Version.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

Ui::Ui(IIgsMemory& mem, CPU65816& cpu, VGC& vgc, AudioOut& audio)
    : mem_(mem), cpu_(cpu), vgc_(vgc), audio_(audio) {}

void Ui::setStatus(const std::string& msg, float seconds) {
    statusMsg_ = msg;
    statusUntil_ = ImGui::GetTime() + double(seconds);
}

void Ui::doReset() {
    mem_.reset();
    cpu_.hardReset();
}

// Per-kind extension filter for the media browser.
static bool mediaMatch(int kind, std::string ext) {
    for (auto& c : ext) c = char(std::tolower((unsigned char)c));
    if (kind == 0) return ext == ".rom" || ext == ".bin";
    if (kind == 1) return ext == ".hdv" || ext == ".po" || ext == ".2mg";
    return ext == ".2mg" || ext == ".po" || ext == ".dsk";      // 3.5" load / hot-swap
}

void Ui::browseTo(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path d = dir.empty() ? fs::current_path(ec) : fs::path(dir);
    if (!fs::is_directory(d, ec)) d = fs::current_path(ec);
    browseDir_ = d.string();
    browseSel_ = -1;
    browseEntries_.clear();
    for (const auto& e : fs::directory_iterator(d, ec)) {
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;          // hidden
        if (e.is_directory(ec)) browseEntries_.emplace_back(true, name);
        else if (e.is_regular_file(ec) && mediaMatch(loadKind_, e.path().extension().string()))
            browseEntries_.emplace_back(false, name);
    }
    std::sort(browseEntries_.begin(), browseEntries_.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first;                 // directories first
        return a.second < b.second;
    });
}

bool Ui::browseAccept(const std::string& path) {
    bool ok = false;
    if      (loadKind_ == 0 && onLoadRom)    ok = onLoadRom(path);
    else if (loadKind_ == 1 && onLoadHdd)    ok = onLoadHdd(path);
    else if (loadKind_ == 2 && onLoadDisk35) ok = onLoadDisk35(path);
    else if (loadKind_ == 3 && onSwapDisk35) { ok = onSwapDisk35(path); if (ok) disk35Path = path; }
    if (ok) {
        setStatus((loadKind_ == 0 ? "Loaded ROM: " : loadKind_ == 3 ? "Inserted: " : "Loaded disk: ") + path);
        running = true;
    } else setStatus("Load failed: " + path, 4.0f);
    return ok;
}

void Ui::openLoad(int kind) {
    loadKind_ = kind;
    // Start in the last directory used for this kind (sticky across the session).
    static std::string lastDir[4];
    if (lastDir[kind].empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string& seed = (kind == 0) ? romPath : (kind >= 2 ? disk35Path : hddPath);
        if (!seed.empty()) lastDir[kind] = fs::path(seed).parent_path().string();
    }
    browseTo(lastDir[kind]);
    lastDir[kind] = browseDir_;
    pathBuf_[0] = 0;
    pendingLoad_ = true;
}

// ── keyboard shortcuts (F-keys / Ctrl combos — chosen to avoid the Apple II
// keyboard, which only receives printables + arrows/Return/Esc) ────────────
void Ui::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) quitRequested = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) vgc_.toggleHgrMode();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && romOk) { doReset(); setStatus("Reset"); }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && romOk) {
        running = !running;
        setStatus(running ? "Running" : "Paused");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false) && onSaveState)
        setStatus(onSaveState() ? "State saved" : "State save FAILED");
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && onLoadState)
        setStatus(onLoadState() ? "State loaded" : "State load FAILED (no save?)");
}

void Ui::render(unsigned int screenTex) {
    handleShortcuts();
    menuBar();
    screenWindow(screenTex);
    statusBar();
    dialogs();
}

void Ui::menuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Load ROM...", nullptr, false, bool(onLoadRom)))        openLoad(0);
        if (ImGui::MenuItem("Load Hard Disk...", nullptr, false, bool(onLoadHdd)))  openLoad(1);
        if (ImGui::MenuItem("Load 3.5\" Disk...", nullptr, false, bool(onLoadDisk35))) openLoad(2);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quitRequested = true;
        ImGui::EndMenu();
    }

    // 3.5" drive: quick-swap between the install-set disks WITHOUT a reset, so the
    // Installer can prompt "insert disk X" mid-install. The next SmartPort STATUS
    // reports "disk switched" so GS/OS re-reads the new volume.
    if (ImGui::BeginMenu("3.5\" Drive")) {
        if (disk35Menu.empty()) ImGui::TextDisabled("(no 3.5\" images found)");
        for (const auto& d : disk35Menu) {
            const bool current = (d.second == disk35Path);
            if (ImGui::MenuItem(d.first.c_str(), nullptr, current, bool(onSwapDisk35))) {
                if (onSwapDisk35(d.second)) { disk35Path = d.second; setStatus("Inserted: " + d.first); }
                else                          setStatus("Insert failed: " + d.first, 4.0f);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Insert other 3.5\" disk...", nullptr, false, bool(onSwapDisk35))) openLoad(3);
        if (ImGui::MenuItem("Eject", nullptr, false, bool(onEjectDisk35))) {
            onEjectDisk35(); disk35Path.clear(); setStatus("Ejected 3.5\" disk");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        if (ImGui::MenuItem(running ? "Pause" : "Run", "F6", false, romOk)) {
            running = !running;
            setStatus(running ? "Running" : "Paused");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "F5", false, romOk)) { doReset(); setStatus("Reset"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Save State", "F7", false, bool(onSaveState)))
            setStatus(onSaveState() ? "State saved" : "State save FAILED");
        if (ImGui::MenuItem("Load State", "F8", false, bool(onLoadState)))
            setStatus(onLoadState() ? "State loaded" : "State load FAILED (no save?)");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Video")) {
        const bool ntsc = vgc_.hgrMode() == VGC::HgrMode::CompositeNtsc;
        if (ImGui::BeginMenu("HGR / DHGR colour")) {
            if (ImGui::MenuItem("Composite NTSC", "F2", ntsc))
                vgc_.setHgrMode(VGC::HgrMode::CompositeNtsc);
            if (ImGui::MenuItem("Clean RGB", nullptr, !ntsc))
                vgc_.setHgrMode(VGC::HgrMode::RgbClean);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scale")) {
            static const float opt[] = { 1.0f, 1.25f, 1.5f, 2.0f };
            static const char* lbl[] = { "1x", "1.25x", "1.5x", "2x" };
            for (int i = 0; i < 4; ++i)
                if (ImGui::MenuItem(lbl[i], nullptr, scale == opt[i])) scale = opt[i];
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio")) {
        const bool avail = audio_.available();
        bool mute = audio_.muted();
        if (ImGui::MenuItem("Mute", nullptr, &mute, avail)) audio_.setMuted(mute);
        ImGui::BeginDisabled(!avail);
        float volPct = audio_.volume() * 100.0f;
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderFloat("Volume", &volPct, 0.0f, 100.0f, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp))
            audio_.setVolume(volPct / 100.0f);
        ImGui::EndDisabled();
        if (!avail) ImGui::TextDisabled("device unavailable");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About POMIIGS...")) pendingAbout_ = true;
        ImGui::EndMenu();
    }

    // Right-aligned: current video mode + framerate.
    const char* mode = mem_.shrEnabled() ? "Super Hi-Res"
                     : mem_.textMode()   ? "Text"
                     : (mem_.hires() && mem_.dhires()) ? "Double Hi-Res"
                     : mem_.hires()      ? "Hi-Res" : "Lo-Res";
    char right[96];
    std::snprintf(right, sizeof right, "%s  |  %.0f FPS", mode, double(ImGui::GetIO().Framerate));
    float w = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 16.0f);
    ImGui::TextDisabled("%s", right);

    ImGui::EndMainMenuBar();
}

void Ui::screenWindow(unsigned int tex) {
    const float menuH = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_FirstUseEver);
    ImGui::Begin("Apple IIgs", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (romOk) {
        // Draw the active display inside the $C034 border colour (the VGC
        // palette entry doubles as an ImU32 — both are 0xAABBGGRR).
        const float W = vgc_.width() * scale, H = vgc_.height() * scale;
        const float b = 16.0f;
        const ImU32 border = VGC::loresColor(mem_.borderColor());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + W + 2 * b, p.y + H + 2 * b), border);
        dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(p.x + b, p.y + b),
                     ImVec2(p.x + b + W, p.y + b + H));
        ImGui::Dummy(ImVec2(W + 2 * b, H + 2 * b));   // reserve the layout space
    } else
        ImGui::TextDisabled("No ROM loaded — File ▸ Load ROM...");
    ImGui::End();
}

void Ui::statusBar() {
    ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("##status", nullptr, flags)) {
        // Left: transient message, else the run state.
        if (!statusMsg_.empty() && ImGui::GetTime() < statusUntil_)
            ImGui::TextUnformatted(statusMsg_.c_str());
        else
            ImGui::TextColored(running ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                       : ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                               running ? "RUN" : "PAUSE");
        // Middle: CPU + shadow/speed + audio, laid out left-to-right.
        ImGui::SameLine(0, 24);
        ImGui::Text("CPU $%02X:%04X %s", cpu_.getPBR(), cpu_.getPC(),
                    cpu_.getEmulationMode() ? "e" : "n");
        ImGui::SameLine(0, 16);
        ImGui::Text("shadow $%02X  speed $%02X", mem_.shadowReg(), mem_.speedReg());
        ImGui::SameLine(0, 16);
        if (!audio_.available())  ImGui::TextDisabled("AUDIO OFF");
        else if (audio_.muted())  ImGui::TextDisabled("MUTED");
        else                      ImGui::Text("AUDIO %.0f%%", double(audio_.volume() * 100.0f));
        // Right: ROM name.
        if (romOk && !romPath.empty()) {
            const char* base = std::strrchr(romPath.c_str(), '/');
            base = base ? base + 1 : romPath.c_str();
            float w = ImGui::CalcTextSize(base).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
            ImGui::TextDisabled("%s", base);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Ui::dialogs() {
    // Deferred OpenPopup so it shares the ID stack with the modal below.
    if (pendingAbout_) { ImGui::OpenPopup("About POMIIGS"); pendingAbout_ = false; }
    if (pendingLoad_)  { ImGui::OpenPopup("Load##dlg");     pendingLoad_  = false; }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About POMIIGS", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::Text("POMIIGS %s", pomiigs::kVersionString);
        ImGui::TextDisabled("Apple IIgs emulator");
        ImGui::Separator();
        ImGui::TextWrapped(
            "16-bit sibling of POM2. 65C816 CPU, FPI + Mega II MMU, VGC video "
            "(Super Hi-Res + legacy text/HGR/DHGR), Ensoniq 5503 DOC + 1-bit "
            "speaker audio, IWM disk, ADB, SCC.");
        ImGui::Spacing();
        ImGui::TextDisabled("VERHILLE Arnaud - Copyright (C) 2026 - GPLv3");
        ImGui::TextDisabled("Source of truth: MAME apple2gs.cpp");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Universal media browser ──────────────────────────────────────────
    // Directory navigation (Places bar, Up, double-click), filtered by the
    // media kind; a manual path field remains for exotic locations.
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Load##dlg", nullptr, 0)) {
        namespace fs = std::filesystem;
        const char* label = loadKind_ == 0 ? "ROM image (.rom/.bin)"
                          : loadKind_ == 1 ? "Hard-disk image (.hdv/.po/.2mg)"
                          : loadKind_ == 3 ? "3.5\" disk to insert now (no reset)"
                                           : "3.5\" boot disk, slot 5 (cold reset)";
        ImGui::TextUnformatted(label);

        // Places: quick jumps that exist on this machine.
        struct Place { const char* name; const char* path; };
        static const Place places[] = {
            { "disks35",  "disks35" },
            { "hdv",      "hdv" },
            { "roms",     "roms" },
            { "Games",    "/media/gistarcade/SHARE/roms/apple2gs" },
            { "Home",     nullptr },
        };
        for (const auto& p : places) {
            std::error_code ec;
            std::string tgt = p.path ? p.path : std::string(std::getenv("HOME") ? std::getenv("HOME") : "/");
            if (!fs::is_directory(tgt, ec)) continue;
            if (ImGui::Button(p.name)) browseTo(tgt);
            ImGui::SameLine();
        }
        if (ImGui::Button("Up")) {
            fs::path parent = fs::path(browseDir_).parent_path();
            if (!parent.empty()) browseTo(parent.string());
        }
        ImGui::TextDisabled("%s", browseDir_.c_str());
        ImGui::Separator();

        // Entry list: directories first; double-click enters/loads.
        std::string accepted;
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.4f;
        if (ImGui::BeginChild("##files", ImVec2(0, -footer), true)) {
            for (int i = 0; i < int(browseEntries_.size()); ++i) {
                const auto& [isDir, name] = browseEntries_[size_t(i)];
                std::string row = (isDir ? "[+] " : "     ") + name;
                if (ImGui::Selectable(row.c_str(), browseSel_ == i,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    browseSel_ = i;
                    if (!isDir)
                        std::snprintf(pathBuf_, sizeof pathBuf_, "%s",
                                      (fs::path(browseDir_) / name).string().c_str());
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (isDir) { browseTo((fs::path(browseDir_) / name).string()); break; }
                        accepted = (fs::path(browseDir_) / name).string();
                    }
                }
            }
            if (browseEntries_.empty()) ImGui::TextDisabled("(no matching media here)");
        }
        ImGui::EndChild();

        // Manual path + buttons.
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputText("##path", pathBuf_, sizeof pathBuf_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool go = ImGui::Button("Load", ImVec2(120, 0)) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        if (go && pathBuf_[0]) {
            std::error_code ec;
            if (fs::is_directory(pathBuf_, ec)) browseTo(pathBuf_);   // typed a folder → navigate
            else accepted = pathBuf_;
        }
        if (!accepted.empty() && browseAccept(accepted)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
