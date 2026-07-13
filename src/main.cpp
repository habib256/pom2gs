// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Milestone 0 shell: opens the Dear ImGui + GLFW/OpenGL window POM2 uses and
// shows a status panel. The emulation core (CPU65816, IIgsMemory, VGC, …) is
// wired in from Milestone 1 onward — see TODO.md. Structure mirrors POM2's
// main.cpp so it grows into the same shape.

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "Version.h"

#include <GLFW/glfw3.h>
#include <cstdio>

static void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main(int, char**) {
    std::printf("POMIIGS %s — Apple IIgs emulator (Milestone 0: foundation)\n",
                pomiigs::kVersionString);

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }

    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(
        960, 640, "POMIIGS — Apple IIgs", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);   // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::Begin("POMIIGS");
        ImGui::Text("Apple IIgs emulator — v%s", pomiigs::kVersionString);
        ImGui::Separator();
        ImGui::TextWrapped(
            "Milestone 0: foundation. Build system, docs, and subsystem map "
            "are in place. Source of truth: MAME apple2gs.cpp.");
        ImGui::BulletText("M1: 65C816 core (Tom Harte 65816 gate)");
        ImGui::BulletText("M2: FPI + Mega II MMU (24-bit, shadow/speed)");
        ImGui::BulletText("M3: VGC Super Hi-Res + legacy video");
        ImGui::BulletText("M4-8: ADB, RTC, IWM/SWIM, DOC, serial, polish");
        ImGui::Separator();
        ImGui::Text("Drop roms/iigs-rom03.rom to boot (not yet wired).");
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
