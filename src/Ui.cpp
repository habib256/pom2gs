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

#include <cstdio>
#include <cstring>

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

void Ui::openLoad(bool rom) {
    loadIsRom_ = rom;
    const std::string& cur = rom ? romPath : hddPath;
    std::snprintf(pathBuf_, sizeof pathBuf_, "%s", cur.c_str());
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
        if (ImGui::MenuItem("Load ROM...", nullptr, false, bool(onLoadRom)))    openLoad(true);
        if (ImGui::MenuItem("Load Hard Disk...", nullptr, false, bool(onLoadHdd))) openLoad(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quitRequested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        if (ImGui::MenuItem(running ? "Pause" : "Run", "F6", false, romOk)) {
            running = !running;
            setStatus(running ? "Running" : "Paused");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "F5", false, romOk)) { doReset(); setStatus("Reset"); }
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
    if (romOk)
        ImGui::Image((ImTextureID)(intptr_t)tex,
                     ImVec2(vgc_.width() * scale, vgc_.height() * scale));
    else
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

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Load##dlg", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s path:", loadIsRom_ ? "ROM image" : "Hard-disk image (.hdv/.po/.2mg)");
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputText("##path", pathBuf_, sizeof pathBuf_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const bool go = ImGui::Button("Load", ImVec2(120, 0)) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        if (go) {
            std::string path = pathBuf_;
            bool ok = false;
            if (loadIsRom_ && onLoadRom) ok = onLoadRom(path);
            else if (!loadIsRom_ && onLoadHdd) ok = onLoadHdd(path);
            if (ok) {
                setStatus((loadIsRom_ ? "Loaded ROM: " : "Loaded disk: ") + path);
                if (loadIsRom_) running = true;
                ImGui::CloseCurrentPopup();
            } else {
                setStatus("Load failed: " + path, 4.0f);
            }
        }
        ImGui::EndPopup();
    }
}
