cmake_minimum_required(VERSION 3.16)
project(POMIIGS VERSION 0.1 LANGUAGES CXX)

# Single source of truth for the version string = project(VERSION) above.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─── Optional sanitizer build (POM2 convention) ──────────────────────────
set(POMIIGS_SANITIZE "" CACHE STRING "Build with -fsanitize=<value>. Empty = off.")
if(POMIIGS_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${POMIIGS_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options   (-fsanitize=${POMIIGS_SANITIZE})
endif()

# ─── Dear ImGui (vendored, fetched by setup_imgui.sh) ────────────────────
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found in ${IMGUI_DIR}. Run ./setup_imgui.sh first.")
endif()
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

# ─── Generated version header ────────────────────────────────────────────
set(POMIIGS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in
    ${POMIIGS_GENERATED_DIR}/Version.h
    @ONLY)

# ─── POMIIGS sources ─────────────────────────────────────────────────────
# One concern per file. As milestones land, add each subsystem's .cpp here.
set(SOURCES
    src/main.cpp
    src/CPU65816.cpp      # M1
    src/IIgsMemory.cpp    # M2
    src/Iwm.cpp           # M5
    src/Es5503.cpp        # M6
    src/Scc8530.cpp       # M7
    src/ProDosHdd.cpp     # HDD ProDOS
    src/VGC.cpp           # M3
    # src/Es5503.cpp        # M6
    # ...
)
set(HEADERS
    src/CPU65816.h
    src/IIgsMemory.h
    src/Iwm.h
    src/Es5503.h
    src/Scc8530.h
    src/ProDosHdd.h
    src/VGC.h
    src/VGCNtsc.h)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${IMGUI_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${POMIIGS_GENERATED_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)

if(EMSCRIPTEN)
    # ─── WebAssembly (single-threaded, static-host-friendly — POM2 model) ──
    target_compile_definitions(${PROJECT_NAME} PRIVATE __EMSCRIPTEN__)
    set(EM_LINK "-sUSE_GLFW=3 -sUSE_WEBGL2=1 -sFULL_ES3=1 \
-sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1 \
-lidbfs.js -sFORCE_FILESYSTEM=1")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "${EM_LINK}"
        SUFFIX ".html")            # emit POMIIGS.html + .js + .wasm (default shell)
else()
    find_package(OpenGL REQUIRED)
    find_package(glfw3 3.3 QUIET CONFIG)
    if(glfw3_FOUND)
        set(GLFW_TARGET glfw)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
        set(GLFW_TARGET PkgConfig::GLFW3)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLFW_TARGET} OpenGL::GL)
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
    endif()
endif()

# ─── Headless target (no ImGui/GL — CI, CLI, tests) — POM2 convention ────
# add_executable(pomiigs_headless src/pomiigs_headless.cpp ...)   # enable in M2

# ─── Tests (CTest) ───────────────────────────────────────────────────────
# The CPU816 gate. Test data is user-fetched (large): run
#   tests/fetch_tomharte_65816.sh <dir>
# and point the test at it via POMIIGS_TOMHARTE_DIR. Absent data → soft skip.
option(POMIIGS_TESTS "Build the test targets" ON)
if(POMIIGS_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(tomharte_65816 tests/tomharte_65816.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(tomharte_65816 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set(POMIIGS_TOMHARTE_DIR "" CACHE PATH "Directory of SingleStepTests/65816 .json vectors")
    # MVN/MVP ($54/$44) are excluded: Tom Harte caps block moves at 100 cycles
    # (partial execution), which an instruction-stepped core can't match — see
    # DEV.md § CPU. Their per-byte semantics are covered by the emulator loop.
    add_test(NAME tomharte_65816
             COMMAND tomharte_65816 "${POMIIGS_TOMHARTE_DIR}" --skip 44,54)

    # Dev tool (not a gate): load a real ROM and trace how far it boots.
    add_executable(boot_trace tests/boot_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(boot_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # VGC Super Hi-Res render smoke test (M3 gate).
    add_executable(vgc_test tests/vgc_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(vgc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME vgc_test COMMAND vgc_test)

    # Ensoniq 5503 DOC sound smoke test (M6 gate).
    add_executable(doc_test tests/doc_test.cpp src/Es5503.cpp)
    target_include_directories(doc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME doc_test COMMAND doc_test)

    # SCC 8530 serial loopback test (M7 gate).
    add_executable(scc_test tests/scc_test.cpp src/Scc8530.cpp)
    target_include_directories(scc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME scc_test COMMAND scc_test)

    # Headless PNG screenshot of a booting ROM (dev tool).
    add_executable(screenshot tests/screenshot.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/VGC.cpp src/Iwm.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(screenshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
