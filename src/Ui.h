// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Emulator UI (Dear ImGui) ─────────────────────────────────────────────
// The desktop chrome: a top main-menu bar (File / Machine / Video / Audio /
// Help), the screen window, a bottom status bar, and the modal dialogs.
// Structure mirrors POM1's MainWindow_Menu — one file owns the menu so
// main.cpp keeps only the emulation/input/GL loop.
//
// The UI never reads files itself: path resolution + file I/O live in main and
// are injected as onLoadRom / onLoadHdd callbacks. Everything else it drives
// directly on the emulator objects it holds by reference.

#ifndef POMIIGS_UI_H
#define POMIIGS_UI_H

#include <functional>
#include <string>
#include <vector>

class IIgsMemory;
class CPU65816;
class VGC;
class AudioOut;

class Ui
{
public:
    Ui(IIgsMemory& mem, CPU65816& cpu, VGC& vgc, AudioOut& audio);

    // File-load hooks (return true on success). Set by main after construction.
    std::function<bool(const std::string&)> onLoadRom;
    std::function<bool(const std::string&)> onLoadHdd;
    std::function<bool(const std::string&)> onLoadDisk35;   // 800K 3.5" on slot 5 (cold-boot)
    std::function<bool(const std::string&)> onSwapDisk35;   // hot-swap 3.5" (no reset)
    std::function<void()>                   onEjectDisk35;
    std::function<bool()>                   onSaveState;    // quick save state (F7)
    std::function<bool()>                   onLoadState;    // quick load state (F8)

    // Quick-swap menu of 3.5" images (label, full path) — set by main from the
    // directory of the configured install disk. Lets the user change disks during
    // an install without typing a path.
    std::vector<std::pair<std::string, std::string>> disk35Menu;
    std::string disk35Path;                                 // currently-inserted 3.5"

    // Shared with the host loop.
    bool running = false;          // emulation gate (Machine ▸ Run/Pause, F6)
    bool quitRequested = false;    // File ▸ Quit / Ctrl+Q / window close

    // Machine info shown in the menus / status bar (set by main after boot).
    std::string romPath, hddPath;
    bool romOk = false, chrOk = false;

    // Display scale of the screen window (Video ▸ Scale).
    float scale = 1.25f;

    // Draw the whole ImGui frame. `screenTex` is the GL texture holding the
    // current VGC framebuffer; `menuActive` reports whether an ImGui menu is
    // open (so main can suppress Apple II keystrokes while navigating menus).
    void render(unsigned int screenTex);

    // Transient message shown at the left of the status bar.
    void setStatus(const std::string& msg, float seconds = 2.5f);

private:
    IIgsMemory& mem_;
    CPU65816&   cpu_;
    VGC&        vgc_;
    AudioOut&   audio_;

    // Modal state.
    bool  pendingAbout_ = false;
    bool  pendingLoad_  = false;
    int   loadKind_     = 0;             // 0 = ROM, 1 = hard disk, 2 = 3.5" disk, 3 = hot-swap
    char  pathBuf_[512] = {0};
    // Universal media browser (the Load dialog): current directory + entries.
    std::string browseDir_;
    std::vector<std::pair<bool, std::string>> browseEntries_;   // (isDir, name)
    int  browseSel_ = -1;
    void browseTo(const std::string& dir);      // cd + rescan (filtered by loadKind_)
    bool browseAccept(const std::string& path); // dispatch to the right onLoad* hook

    std::string statusMsg_;
    double      statusUntil_ = 0.0;

    void handleShortcuts();
    void menuBar();
    void screenWindow(unsigned int tex);
    void statusBar();
    void dialogs();
    void openLoad(int kind);
    void doReset();
};

#endif // POMIIGS_UI_H
