# Cross-compiling for the handhelds (aarch64) from a PC: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
# With a sysroot (SuperDrastic's build.sh recipe), add -DCMAKE_SYSROOT=<path> to get SDL2/ALSA/libenet too.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# The handhelds' firmwares don't all ship a new enough libstdc++: link it in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
