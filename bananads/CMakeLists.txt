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
# The handheld frontend (frontend=kms: both screens on the panels through DRM/KMS, evdev input; needs libdrm): auto
# builds it when libdrm is there, ON requires it (BananaLinux's package), OFF leaves it out.
set(BANANADS_KMS "auto" CACHE STRING "The KMS frontend: auto, ON or OFF")
set(BANANADS_MELONDS_DIR "" CACHE PATH "A melonDS checkout to build against instead of the pinned one")
set(BANANADS_JIT "melonds" CACHE STRING "The CPU recompiler on aarch64: melonds (melonDS's) or banana (BananaJIT, docs/JIT.md)")

# The core. The commit is pinned: a new one is a reviewed change (docs/CORE.md says how to move it).
set(BANANADS_MELONDS_COMMIT 906e9ebb27da8c6a715cd7abab4abfe8a8d29427)

# The handhelds' CPU: the RG DS and RG DS Plus have Cortex-A55s (in order, ARMv8.2). Tuning for it schedules
# instructions for its pipeline and allows its v8.2 instructions; behaviour is the same. Empty for generic ARMv8.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BANANADS_CPU "cortex-a55" CACHE STRING "-mcpu for aarch64 builds (empty: generic)")
    if(BANANADS_CPU)
        add_compile_options(-mcpu=${BANANADS_CPU})
    endif()
endif()
# Link-time optimization over the core and the frontend (inlining across files): on for the handhelds' aarch64
# builds (membench's main thread -5% under qemu-count, the binary 7% smaller; pictures and RAM identical), off for
# the PC. melonDS's own ENABLE_LTO_RELEASE stays off: this one covers BananaDS's executable too.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BANANADS_LTO_DEFAULT ON)
else()
    set(BANANADS_LTO_DEFAULT OFF)
endif()
option(BANANADS_LTO "Link-time optimization" ${BANANADS_LTO_DEFAULT})
if(BANANADS_LTO)
    add_compile_options(-flto=auto)
    add_link_options(-flto=auto)
endif()

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
# BananaDS's changes to melonDS itself (patches/melonds/*.patch, in order; docs/PERF.md says what they do): applied
# once to the melonDS tree, left alone where they already are. They go on in order, so the tree holds the first k
# of them: k is the last patch that comes off cleanly (git apply --reverse --check, newest first; an earlier one
# can fail that check once a later one has changed its context), and the rest are applied. OFF builds stock
# melonDS, for A/B runs (in a build directory of its own: the fetched tree is per build directory).
option(BANANADS_MELONDS_PATCHES "Apply patches/melonds/*.patch to melonDS" ON)
if(BANANADS_MELONDS_PATCHES)
    file(GLOB MELONDS_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/melonds/*.patch)
    list(SORT MELONDS_PATCHES)
    list(LENGTH MELONDS_PATCHES NPATCHES)
    set(APPLIED 0)
    set(I ${NPATCHES})
    while(I GREATER 0)
        math(EXPR J "${I} - 1")
        list(GET MELONDS_PATCHES ${J} P)
        execute_process(COMMAND git apply --reverse --check ${P} WORKING_DIRECTORY ${MELONDS_SRC}
            RESULT_VARIABLE ALREADY OUTPUT_QUIET ERROR_QUIET)
        if(ALREADY EQUAL 0)
            set(APPLIED ${I})
            break()
        endif()
        set(I ${J})
    endwhile()
    set(I 0)
    foreach(P ${MELONDS_PATCHES})
        if(I GREATER_EQUAL APPLIED)
            execute_process(COMMAND git apply ${P} WORKING_DIRECTORY ${MELONDS_SRC} RESULT_VARIABLE R)
            if(NOT R EQUAL 0)
                message(FATAL_ERROR "${P} doesn't apply to ${MELONDS_SRC}")
            endif()
            message(STATUS "melonDS: applied ${P}")
        endif()
        math(EXPR I "${I} + 1")
    endforeach()
endif()
add_subdirectory(${MELONDS_SRC} melonds EXCLUDE_FROM_ALL)
# melonDS builds everything position-independent (CMAKE_POSITION_INDEPENDENT_CODE), so GCC may not inline or
# call directly any of the core's non-inline functions (another module could interpose them). BananaDS links the
# core statically into one executable: nothing can, so say so. The same code, fewer calls (BANANADS_CORE_NOINTERPOSE).
option(BANANADS_CORE_NOINTERPOSE "Build melonDS's core with -fno-semantic-interposition" ON)
if(BANANADS_CORE_NOINTERPOSE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(core PRIVATE -fno-semantic-interposition)
endif()

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
    # ...and in ARMJIT.cpp, where the framework retires or invalidates a block, a call that unlinks the exits
    # jumping straight to it (BananaJIT's block chaining, docs/JIT.md)
    set(FW ${MELONDS_SRC}/src/ARMJIT.cpp)
    file(READ ${FW} FWTXT)
    if(NOT FWTXT MATCHES "UnlinkBlock")
        set(FWTXT2 "${FWTXT}")
        string(REPLACE "void ARMJIT::RetireJitBlock(JitBlock* block) noexcept\n{\n"
            "void ARMJIT::RetireJitBlock(JitBlock* block) noexcept\n{\n#ifdef BANANADS_JIT\n    JITCompiler.UnlinkBlock(block->EntryPoint);\n#endif\n"
            FWTXT2 "${FWTXT2}")
        string(REPLACE "        FastBlockLookupRegions[block->StartAddrLocal >> 27][(block->StartAddrLocal & 0x7FFFFFF) / 2] = (u64)UINT32_MAX << 32;"
            "#ifdef BANANADS_JIT\n        JITCompiler.UnlinkBlock(block->EntryPoint);\n#endif\n        FastBlockLookupRegions[block->StartAddrLocal >> 27][(block->StartAddrLocal & 0x7FFFFFF) / 2] = (u64)UINT32_MAX << 32;"
            FWTXT2 "${FWTXT2}")
        string(REGEX MATCHALL "UnlinkBlock" HOOKS "${FWTXT2}")
        list(LENGTH HOOKS NHOOKS)
        if(NOT NHOOKS EQUAL 2)
            message(FATAL_ERROR "melonDS's ARMJIT.cpp changed; BananaJIT's unlink hooks need updating")
        endif()
        file(WRITE ${FW} "${FWTXT2}")
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
    add_library(gengis STATIC ${GX}/gx.c ${GX}/gx2.c ${GX}/fused.c ${GX}/fused_neon.c ${GX}/fused_asm.c ${GX}/defer.c ${GX}/rast_kern.S)
    target_include_directories(gengis PUBLIC ${GX})
    # the rasterizer's float steps are exact only without contraction (its sources say so with a clang pragma)
    target_compile_options(gengis PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -ffp-contract=off -Wno-unused-function>)
    set_target_properties(gengis PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
endif()

# The Banana 2D engine (gpu2d/, from ROCKNIXDS's bananads/ at 99cac23, its PR #66): melonDS's 2D renderer
# replaced, pixel for pixel (render.2d). It needs two friend declarations in melonDS's GPU_Soft.h, its
# melonds-core.patch.
option(BANANADS_2D "Banana 2D engine in place of melonDS's 2D renderer" ON)
if(BANANADS_2D)
    set(SOFTH ${MELONDS_SRC}/src/GPU_Soft.h)
    file(READ ${SOFTH} SOFTTXT)
    if(NOT SOFTTXT MATCHES "friend class BananaRenderer2D")
        string(REPLACE "    friend class SoftRenderer3D;\n"
            "    friend class SoftRenderer3D;\n    friend class BananaRenderer2D;\n    friend class BananaRenderer;\n"
            SOFTTXT2 "${SOFTTXT}")
        if(SOFTTXT2 STREQUAL SOFTTXT)
            message(FATAL_ERROR "melonDS's GPU_Soft.h changed; the Banana 2D engine's friend declarations need updating")
        endif()
        file(WRITE ${SOFTH} "${SOFTTXT2}")
    endif()
    add_library(banana2d STATIC gpu2d/GPU2D_Banana.cpp gpu2d/GPU_Banana.cpp)
    target_include_directories(banana2d PUBLIC gpu2d)
    target_link_libraries(banana2d PUBLIC core)
    target_compile_definitions(banana2d PUBLIC BANANADS_2D=1)
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
if(BANANADS_2D)
    target_link_libraries(bananads PRIVATE banana2d)
endif()
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
if(BANANADS_KMS STREQUAL "auto")
    pkg_check_modules(DRM QUIET IMPORTED_TARGET libdrm)
    if(NOT DRM_FOUND)
        message(STATUS "BananaDS: no libdrm, so no KMS frontend (BANANADS_KMS=ON requires it)")
    endif()
elseif(BANANADS_KMS)
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
endif()
if(DRM_FOUND AND BANANADS_KMS)
    target_sources(bananads PRIVATE src/frontend_kms.cpp src/cpugov.cpp src/menu.c src/shader.c src/cheats.cpp src/screenshot.cpp)
    target_link_libraries(bananads PRIVATE PkgConfig::DRM)
    target_compile_definitions(bananads PRIVATE BANANADS_KMS=1)
    # RetroAchievements (src/ra.c): SuperDrastic's rcheevos (../src/third_party/rcheevos, the same repository), a
    # library of its own so its C keeps its own warnings; libcurl is dlopen()ed at run time
    set(RC ${CMAKE_CURRENT_SOURCE_DIR}/../src/third_party/rcheevos)
    file(GLOB RC_SRCS ${RC}/src/rc_client.c ${RC}/src/rc_compat.c ${RC}/src/rc_util.c ${RC}/src/rc_version.c
        ${RC}/src/rcheevos/*.c ${RC}/src/rapi/*.c ${RC}/src/rhash/*.c)
    add_library(rcheevos STATIC ${RC_SRCS})
    target_include_directories(rcheevos PUBLIC ${RC}/include)
    target_compile_definitions(rcheevos PUBLIC RC_DISABLE_LUA RC_CLIENT_SUPPORTS_HASH)
    set_target_properties(rcheevos PROPERTIES C_VISIBILITY_PRESET hidden)
    target_sources(bananads PRIVATE src/ra.c)
    target_link_libraries(bananads PRIVATE rcheevos ${CMAKE_DL_LIBS})
endif()
if(BANANADS_ALSA)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    target_sources(bananads PRIVATE src/alsa.cpp src/stretch.cpp)
    target_link_libraries(bananads PRIVATE PkgConfig::ALSA)
    target_compile_definitions(bananads PRIVATE BANANADS_ALSA=1)
endif()

# Host tests: no ROM needed (tests/tests.cpp).
enable_testing()
add_executable(bananads-tests tests/tests.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-tests PRIVATE src)
target_link_libraries(bananads-tests PRIVATE core bananads-net pthread)
add_test(NAME host COMMAND bananads-tests)

# The CPU governor against a simulated game on a made-up cpufreq folder (tests/cpugov_test.cpp, ~90 s).
add_executable(bananads-cpugov-test tests/cpugov_test.cpp src/cpugov.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-cpugov-test PRIVATE src)
target_link_libraries(bananads-cpugov-test PRIVATE core bananads-net pthread)
add_test(NAME cpugov COMMAND bananads-cpugov-test ${CMAKE_CURRENT_BINARY_DIR}/cpugov-test HeartGold.nds)

# The sound thread's time stretch: pitch kept, length and joins checked on test tones (tests/stretch_test.cpp).
add_executable(bananads-stretch-test tests/stretch_test.cpp src/stretch.cpp)
target_include_directories(bananads-stretch-test PRIVATE src)
add_test(NAME stretch COMMAND bananads-stretch-test)

# BananaJIT's encoder against LLVM's assembler (runs on any host: it only encodes).
add_executable(bananads-jit-encodings tests/jit_encodings.cpp)
target_include_directories(bananads-jit-encodings PRIVATE jit)
add_test(NAME jit-encodings COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_encodings.sh $<TARGET_FILE:bananads-jit-encodings>)

# The differential harness for CPU back-ends (tools/difftest.cpp, docs/JIT.md).
add_executable(bananads-difftest tools/difftest.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-difftest PRIVATE src)
target_link_libraries(bananads-difftest PRIVATE core bananads-net pthread)
if(BANANADS_2D)
    target_link_libraries(bananads-difftest PRIVATE banana2d)
endif()
if(BANANADS_GENGIS)
    target_sources(bananads-difftest PRIVATE src/gengis_renderer.cpp)
    target_link_libraries(bananads-difftest PRIVATE gengis)
    target_compile_definitions(bananads-difftest PRIVATE BANANADS_GENGIS=1)
endif()
