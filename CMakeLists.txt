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
    src/Iwm.cpp
    src/DiskImage.cpp     # POM2 port: 5.25" media container (dsk/po/nib/WOZ+FLUX)
    src/Sony35.cpp        # M5 3.5" Sony drive + 800K GCR codec
    src/Es5503.cpp        # M6
    src/Scc8530.cpp       # M7
    src/ProDosHdd.cpp     # HDD ProDOS
    src/Snapshot.cpp      # save/load state
    src/VGC.cpp           # M3
    src/Audio.cpp         # M6 audio host (miniaudio)
    src/Ui.cpp            # ImGui menu bar / status bar / dialogs
)
set(HEADERS
    src/CPU65816.h
    src/IIgsMemory.h
    src/Iwm.h
    src/DiskImage.h
    src/TwoImg.h
    src/Logger.h
    src/Sony35.h
    src/Es5503.h
    src/Scc8530.h
    src/ProDosHdd.h
    src/Snapshot.h
    src/VGC.h
    src/VGCNtsc.h
    src/Audio.h
    src/Ui.h)

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

# ─── Headless surface (no ImGui/GL — CI, CLI) ────────────────────────────
# POM2 has a dedicated pomiigs_headless target; POMIIGS never needed one — the
# tests/ harnesses below (screenshot, triage, boot_trace, gsos_trace, hdd_trace)
# link the emulator without ImGui/GL and cover the same ground.

# ─── Tests (CTest) ───────────────────────────────────────────────────────
# The CPU816 gate. Test data is user-fetched (large): run
#   tests/fetch_tomharte_65816.sh <dir>
# and point the test at it via POMIIGS_TOMHARTE_DIR. Absent data → CTest SKIP.
option(POMIIGS_TESTS "Build the test targets" ON)
if(POMIIGS_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cycle_accuracy_catalog
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cycle_suite.py)

    add_executable(tomharte_65816 tests/tomharte_65816.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(tomharte_65816 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME tomharte_65816_selftest COMMAND tomharte_65816 --self-test)
    set(POMIIGS_TOMHARTE_DIR "" CACHE PATH "Directory of SingleStepTests/65816 .json vectors")
    # MVN/MVP ($54/$44): the corpus caps block moves at 100 cycles; the harness
    # runs whole iterations to the cap and drops the two trailing partial
    # fetches from the expectation (see tests/tomharte_65816.cpp runVector).
    add_test(NAME tomharte_65816
             COMMAND tomharte_65816 "${POMIIGS_TOMHARTE_DIR}")
    set_tests_properties(tomharte_65816 PROPERTIES SKIP_RETURN_CODE 77)

    # Dev tool (not a gate): load a real ROM and trace how far it boots.
    add_executable(boot_trace tests/boot_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(boot_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Guest-diagnostic disk runner (text-page dump) used by tools/mister_diags.py.
    # The MiSTer diagnostics gate SKIPs (77) until the disks are built with
    # `tools/cycle_suite.py build --merlin-root … --cp2 …`.
    add_executable(diag_trace tests/diag_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(diag_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME mister_diagnostics
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mister_diags.py --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(mister_diagnostics PROPERTIES SKIP_RETURN_CODE 77)

    # ROM built-in self-diagnostic (⌘+Option held through RESET) at native
    # speed — catalog entry rom01-rom03-selftest. ROMs are user-provided, so a
    # missing image is a CTest SKIP (exit 77), never a false PASS.
    add_executable(selftest_trace tests/selftest_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(selftest_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Sequential run gates tests 01-08 (the RAM tests 02/04 cannot be launched
    # alone — they overwrite the launcher); 09 (ADB) SKIPs without the µC
    # firmware; the remaining diagnostics run individually.
    set(_rom03 ${CMAKE_CURRENT_SOURCE_DIR}/roms/iigs-rom03.rom)
    set(_rom01 ${CMAKE_CURRENT_SOURCE_DIR}/roms/iigs-rom01.rom)
    add_test(NAME rom03_selftest_01_08 COMMAND selftest_trace ${_rom03} --through 8)
    add_test(NAME rom01_selftest_01_08 COMMAND selftest_trace ${_rom01} --through 8)
    set(_selftests rom03_selftest_01_08 rom01_selftest_01_08)
    foreach(n 9 10 11 12)
        add_test(NAME rom03_selftest_${n} COMMAND selftest_trace ${_rom03} --test ${n})
        list(APPEND _selftests rom03_selftest_${n})
    endforeach()
    foreach(n 9 10 11)
        add_test(NAME rom01_selftest_${n} COMMAND selftest_trace ${_rom01} --test ${n})
        list(APPEND _selftests rom01_selftest_${n})
    endforeach()
    set_tests_properties(${_selftests} PROPERTIES SKIP_RETURN_CODE 77)

    # Dev tool (not a gate): boot an 800K GS/OS disk from slot 5 and trace the
    # toolbox dispatch / SHR timeline / hang detection. Used to pin the ROM
    # interrupt-vector fix (see CHANGELOG "GS toolbox / interrupt vectors").
    add_executable(gsos_trace tests/gsos_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(gsos_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # GS/OS + hard-disk derail microscope (boots 3.5" with a HDD mounted).
    add_executable(hdd_trace tests/hdd_trace.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(hdd_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # VGC Super Hi-Res render smoke test (M3 gate).
    add_executable(vgc_test tests/vgc_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
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

    # Double Hi-Res colour decode gate (header-only VGCNtsc.h).
    add_executable(dhgr_test tests/dhgr_test.cpp)
    target_include_directories(dhgr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME dhgr_test COMMAND dhgr_test)

    # 80STORE display-page quirk regression (Total Replay DHGR fade fix).
    add_executable(dhgr_page_test tests/dhgr_page_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(dhgr_page_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME dhgr_page_test COMMAND dhgr_page_test)

    # 80-column text: aux/main interleave + $C022 text colour.
    add_executable(text80_test tests/text80_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(text80_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME text80_test COMMAND text80_test)

    # SPEED register ($C036) → per-frame CPU cycle budget (//e slow-mode fix).
    add_executable(speed_test tests/speed_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(speed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME speed_test COMMAND speed_test)

    # Phase-aware slow-side penalty (Mega II PH0 sync in fast mode).
    add_executable(slowside_test tests/slowside_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(slowside_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME slowside_test COMMAND slowside_test)

    # 14 MHz Mega II/FPI timing: 912-tick line, PH0 sync and DRAM refresh.
    add_executable(scanout_test tests/scanout_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(scanout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME scanout_test COMMAND scanout_test)

    add_executable(fpi_shadowall_test tests/fpi_shadowall_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(fpi_shadowall_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME fpi_shadowall_test COMMAND fpi_shadowall_test)

    add_executable(megaii_timing_test tests/megaii_timing_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(megaii_timing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME megaii_timing_test COMMAND megaii_timing_test)

    # Interrupt set: ¼-sec / 1-sec / scan-line / DOC (assert / mask / clear).
    add_executable(irq_test tests/irq_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(irq_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME irq_test COMMAND irq_test)

    # ADB GLU mouse + keyboard-modifier register protocol ($C024/$C025/$C027).
    add_executable(adb_test tests/adb_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(adb_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME adb_test COMMAND adb_test)

    # Super Hi-Res color-fill (SCB bit5) + $C034 border colour.
    add_executable(shr_test tests/shr_test.cpp src/VGC.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(shr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME shr_test COMMAND shr_test)

    # Slot-5 3.5" SmartPort/block device (ROM signature + block streaming).
    add_executable(disk35_test tests/disk35_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(disk35_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME disk35_test COMMAND disk35_test)

    # Real IWM 3.5" Sony drive (LLE): GCR codec round-trip, Sony register
    # protocol, address-field reads, firmware-style sector write.
    add_executable(iwm35_test tests/iwm35_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(iwm35_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME iwm35_test COMMAND iwm35_test)

    # 5.25" IWM over the POM2 DiskImage port: bit-cell reads, RWTS-style
    # sector write + flush, seek, WOZ prologue streaming (soft).
    add_executable(iwm525_test tests/iwm525_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(iwm525_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME iwm525_test COMMAND iwm525_test)

    # SmartPort extended dispatch (READBLOCK / STATUS via the WDM-trap ROM).
    add_executable(smartport_test tests/smartport_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(smartport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME smartport_test COMMAND smartport_test)

    # 2IMG envelope (dataOffset / dataLength / lock flag) on the BLOCK loaders.
    add_executable(twoimg_test tests/twoimg_test.cpp src/ProDosHdd.cpp src/Sony35.cpp)
    target_include_directories(twoimg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME twoimg_test COMMAND twoimg_test)

    # Hard-disk boot fixes: emulation-mode ROM vector pull + slot-7 chain +
    # block-write persistence (GS/OS boots to the Finder with a HDD mounted).
    add_executable(hdd_test tests/hdd_test.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(hdd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME hdd_test COMMAND hdd_test)

    add_executable(snapshot_test tests/snapshot_test.cpp src/Snapshot.cpp src/IIgsMemory.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/CPU65816.cpp src/ProDosHdd.cpp)
    target_include_directories(snapshot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME snapshot_test COMMAND snapshot_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Headless PNG screenshot of a booting ROM (dev tool).
    add_executable(screenshot tests/screenshot.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/VGC.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(screenshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Headless compatibility triage (boots a disk, classifies how far it got).
    add_executable(triage tests/triage.cpp src/CPU65816.cpp src/IIgsMemory.cpp src/VGC.cpp src/Iwm.cpp src/DiskImage.cpp src/Sony35.cpp src/Es5503.cpp src/Scc8530.cpp src/ProDosHdd.cpp)
    target_include_directories(triage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
