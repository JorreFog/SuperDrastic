# The RG DS's own toolchain and flags (what BananaLinux's Buildroot 2026.02.3 passes; .github/workflows/
# bananads-device.yml): the Bootlin aarch64 glibc "stable 2025.08-1" toolchain (GCC 14.3.0, binutils 2.43.1,
# glibc 2.41: BR2_TOOLCHAIN_EXTERNAL_BOOTLIN_AARCH64_GLIBC_STABLE), from
# https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--glibc--stable-2025.08-1.tar.xz
# Where it is unpacked: BANANADS_BOOTLIN in the environment (or -DBANANADS_BOOTLIN=...).
# Buildroot calls the toolchain's real compilers (.br_real) through its own wrapper, which with BananaLinux's
# config adds --sysroot, -mcpu=cortex-a55 and -Wl,-z,relro and nothing else (no PIE, no stack protector), and builds
# CMake packages as Release with the type's own flags emptied and CMAKE_C_FLAGS = TARGET_CFLAGS: -O2 -g0
# -D_FORTIFY_SOURCE=1 plus the large-file defines. Configure also with -DBUILD_SHARED_LIBS=ON (Buildroot's
# cmake-package does) and bananads.mk's options (-DBANANADS_JIT=melonds -DBANANADS_LTO=ON -DBANANADS_CPU=cortex-a55).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(NOT BANANADS_BOOTLIN)
    set(BANANADS_BOOTLIN "$ENV{BANANADS_BOOTLIN}")
endif()
if(NOT BANANADS_BOOTLIN)
    message(FATAL_ERROR "bootlin-a55.cmake: set BANANADS_BOOTLIN to the unpacked toolchain")
endif()
set(ENV{BANANADS_BOOTLIN} "${BANANADS_BOOTLIN}")  # (for try_compile's own runs of this file)
set(CMAKE_SYSROOT ${BANANADS_BOOTLIN}/aarch64-buildroot-linux-gnu/sysroot)
set(CMAKE_C_COMPILER ${BANANADS_BOOTLIN}/bin/aarch64-linux-gcc.br_real)
set(CMAKE_CXX_COMPILER ${BANANADS_BOOTLIN}/bin/aarch64-linux-g++.br_real)
set(CMAKE_ASM_COMPILER ${BANANADS_BOOTLIN}/bin/aarch64-linux-gcc.br_real)
set(CMAKE_AR ${BANANADS_BOOTLIN}/bin/aarch64-linux-gcc-ar)
set(CMAKE_RANLIB ${BANANADS_BOOTLIN}/bin/aarch64-linux-gcc-ranlib)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_RELEASE " -DNDEBUG" CACHE STRING "Buildroot's Release CFLAGS")
set(CMAKE_CXX_FLAGS_RELEASE " -DNDEBUG" CACHE STRING "Buildroot's Release CXXFLAGS")
set(BR_TARGET_CFLAGS "-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -g0 -D_FORTIFY_SOURCE=1")
set(CMAKE_C_FLAGS "${BR_TARGET_CFLAGS}" CACHE STRING "Buildroot's TARGET_CFLAGS")
set(CMAKE_CXX_FLAGS "${BR_TARGET_CFLAGS}" CACHE STRING "Buildroot's TARGET_CXXFLAGS")
set(CMAKE_ASM_FLAGS "${BR_TARGET_CFLAGS}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-mcpu=cortex-a55 -Wl,-z,relro" CACHE STRING "Buildroot's wrapper link flags")
set(CMAKE_SHARED_LINKER_FLAGS "-mcpu=cortex-a55 -Wl,-z,relro" CACHE STRING "")
