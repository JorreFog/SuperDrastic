cmake_minimum_required(VERSION 3.16)

# BananaDS: a standalone DS emulator for Linux handhelds. The emulation core is melonDS (GPL-3.0-or-later),
# pinned below; the frontend (display, audio, microphone, Wi-Fi, input) is BananaDS's own, built from what
# SuperDrastic learned on the RG DS. See README.md.

project(BananaDS VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(BANANADS_SDL "SDL2 frontend (a desktop window, for development)" ON)
option(BANANADS_ALSA "ALSA audio out and microphone in (the handheld path)" ON)
option(BANANADS_LAN "Local wireless between two handhelds over the network (needs libenet)" OFF)
set(BANANADS_MELONDS_DIR "" CACHE PATH "A melonDS checkout to build against instead of the pinned one")
set(BANANADS_JIT "melonds" CACHE STRING "The CPU recompiler on aarch64: melonds (melonDS's) or banana (BananaJIT, docs/JIT.md)")

# The core. The commit is pinned: a new one is a reviewed change (docs/CORE.md says how to move it).
set(BANANADS_MELONDS_COMMIT 906e9ebb27da8c6a715cd7abab4abfe8a8d29427)

set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE OFF CACHE BOOL "" FORCE)

if(BANANADS_MELONDS_DIR)
    set(MELONDS_SRC ${BANANADS_MELONDS_DIR})
else()
    include(FetchContent)
    FetchContent_Declare(melonds
        GIT_REPOSITORY https://github.com/melonDS-emu/melonDS.git
        GIT_TAG ${BANANADS_MELONDS_COMMIT})
    FetchContent_GetProperties(melonds)
    if(NOT melonds_POPULATED)
        FetchContent_Populate(melonds)
    endif()
    set(MELONDS_SRC ${melonds_SOURCE_DIR})
endif()
add_subdirectory(${MELONDS_SRC} melonds EXCLUDE_FROM_ALL)

# BananaJIT: our own code generator in melonDS's JIT framework, in place of melonDS's ARMJIT_A64 compiler.
# melonDS picks its compiler in src/ARMJIT_Compiler.h; one guarded line there lets BANANADS_JIT pick ours
# (a no-op for builds without it). The define is PUBLIC: every file that sees melonDS's NDS must agree on
# which compiler it holds.
if(BANANADS_JIT STREQUAL "banana")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(FATAL_ERROR "BANANADS_JIT=banana generates AArch64 code: build for aarch64")
    endif()
    set(SEL ${MELONDS_SRC}/src/ARMJIT_Compiler.h)
    file(READ ${SEL} SELTXT)
    if(NOT SELTXT MATCHES "BANANADS_JIT")
        string(REPLACE "#elif defined(__aarch64__)\n#include \"ARMJIT_A64/ARMJIT_Compiler.h\""
            "#elif defined(__aarch64__) && defined(BANANADS_JIT)\n#include \"bananajit.h\"\n#elif defined(__aarch64__)\n#include \"ARMJIT_A64/ARMJIT_Compiler.h\""
            SELTXT2 "${SELTXT}")
        if(SELTXT2 STREQUAL SELTXT)
            message(FATAL_ERROR "melonDS's ARMJIT_Compiler.h changed; BananaJIT's hook needs updating")
        endif()
        file(WRITE ${SEL} "${SELTXT2}")
    endif()
    get_target_property(CORE_SRCS core SOURCES)
    list(FILTER CORE_SRCS EXCLUDE REGEX "ARMJIT_A64/")
    set_property(TARGET core PROPERTY SOURCES ${CORE_SRCS})
    target_sources(core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/jit/bananajit.cpp ${CMAKE_CURRENT_SOURCE_DIR}/jit/linkage.S)
    target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/jit ${MELONDS_SRC}/src)
    target_compile_definitions(core PUBLIC BANANADS_JIT=1)
endif()

# Gengis Engine (gengis/, docs/GENGIS.md): SuperDrastic's 3D rasterizer as melonDS's 3D renderer, at 1x, on aarch64
# (NEON and the generated assembly kernels). render.renderer=gengis picks it at run time.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BANANADS_GENGIS_DEFAULT ON)
else()
    set(BANANADS_GENGIS_DEFAULT OFF)
endif()
option(BANANADS_GENGIS "Gengis Engine 3D renderer (aarch64)" ${BANANADS_GENGIS_DEFAULT})
if(BANANADS_GENGIS)
    set(GX ${CMAKE_CURRENT_SOURCE_DIR}/gengis)
    add_library(gengis STATIC ${GX}/gx.c ${GX}/fused.c ${GX}/fused_neon.c ${GX}/fused_asm.c ${GX}/defer.c ${GX}/rast_kern.S)
    target_include_directories(gengis PUBLIC ${GX})
    # the rasterizer's float steps are exact only without contraction (its sources say so with a clang pragma)
    target_compile_options(gengis PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -ffp-contract=off -Wno-unused-function>)
    set_target_properties(gengis PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
endif()

# melonDS's network helpers, without its Qt frontend. LAN and netplay need ENet; slirp (Nintendo WFC) doesn't.
find_package(PkgConfig REQUIRED)
set(NET ${MELONDS_SRC}/src/net)
add_subdirectory(${NET}/libslirp ${CMAKE_BINARY_DIR}/libslirp EXCLUDE_FROM_ALL)
add_library(bananads-net STATIC ${NET}/Net.cpp ${NET}/Net_Slirp.cpp ${NET}/PacketDispatcher.cpp ${NET}/LocalMP.cpp)
target_include_directories(bananads-net PUBLIC ${NET} ${MELONDS_SRC}/src)
target_link_libraries(bananads-net PUBLIC core slirp)
if(BANANADS_LAN)
    pkg_check_modules(ENet REQUIRED IMPORTED_TARGET libenet)
    target_sources(bananads-net PRIVATE ${NET}/LAN.cpp ${NET}/Netplay.cpp ${NET}/MPInterface.cpp)
    target_link_libraries(bananads-net PUBLIC PkgConfig::ENet)
    target_compile_definitions(bananads-net PUBLIC BANANADS_LAN=1)
endif()

add_executable(bananads
    src/main.cpp
    src/config.cpp
    src/emu.cpp
    src/platform.cpp
    src/mic.cpp
    src/wifi.cpp
    src/video_headless.cpp)
target_include_directories(bananads PRIVATE src)
target_link_libraries(bananads PRIVATE core bananads-net pthread)
if(BANANADS_GENGIS)
    target_sources(bananads PRIVATE src/gengis_renderer.cpp)
    target_link_libraries(bananads PRIVATE gengis)
    target_compile_definitions(bananads PRIVATE BANANADS_GENGIS=1)
endif()
target_compile_definitions(bananads PRIVATE BANANADS_VERSION="${PROJECT_VERSION}")

if(BANANADS_SDL)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_sources(bananads PRIVATE src/frontend_sdl.cpp)
    target_link_libraries(bananads PRIVATE PkgConfig::SDL2)
    target_compile_definitions(bananads PRIVATE BANANADS_SDL=1)
endif()
if(BANANADS_ALSA)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    target_sources(bananads PRIVATE src/alsa.cpp)
    target_link_libraries(bananads PRIVATE PkgConfig::ALSA)
    target_compile_definitions(bananads PRIVATE BANANADS_ALSA=1)
endif()

# Host tests: no ROM needed (tests/tests.cpp).
enable_testing()
add_executable(bananads-tests tests/tests.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-tests PRIVATE src)
target_link_libraries(bananads-tests PRIVATE core bananads-net pthread)
add_test(NAME host COMMAND bananads-tests)

# BananaJIT's encoder against LLVM's assembler (runs on any host: it only encodes).
add_executable(bananads-jit-encodings tests/jit_encodings.cpp)
target_include_directories(bananads-jit-encodings PRIVATE jit)
add_test(NAME jit-encodings COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_encodings.sh $<TARGET_FILE:bananads-jit-encodings>)

# The differential harness for CPU back-ends (tools/difftest.cpp, docs/JIT.md).
add_executable(bananads-difftest tools/difftest.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-difftest PRIVATE src)
target_link_libraries(bananads-difftest PRIVATE core bananads-net pthread)
if(BANANADS_GENGIS)
    target_sources(bananads-difftest PRIVATE src/gengis_renderer.cpp)
    target_link_libraries(bananads-difftest PRIVATE gengis)
    target_compile_definitions(bananads-difftest PRIVATE BANANADS_GENGIS=1)
endif()
