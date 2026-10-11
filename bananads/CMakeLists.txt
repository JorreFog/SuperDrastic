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

# The differential harness for CPU back-ends (tools/difftest.cpp, docs/JIT.md).
add_executable(bananads-difftest tools/difftest.cpp src/config.cpp src/emu.cpp src/platform.cpp src/mic.cpp src/wifi.cpp)
target_include_directories(bananads-difftest PRIVATE src)
target_link_libraries(bananads-difftest PRIVATE core bananads-net pthread)
