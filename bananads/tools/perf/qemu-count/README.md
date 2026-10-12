# Counting host instructions per emulated frame under qemu

`perf` needs the handheld. This counts, under qemu, how many AArch64 instructions BananaDS's main thread executes
per emulated frame, split by part (JIT code, JIT runtime, the scheduler, memory paths, 2D, 3D...), exactly and
reproducibly. Instruction counts aren't A55 time (cache misses and stalls are missing), but a change that removes
instructions from a hot path shows here first, without a device.

1. A qemu with TCG plugins (Ubuntu's has none), about 5 minutes on 4 cores:
   ```sh
   git clone --depth 1 --branch v9.2.4 https://github.com/qemu/qemu.git qemu-src
   cd qemu-src && mkdir build && cd build
   ../configure --target-list=aarch64-linux-user --enable-plugins --disable-docs --disable-werror --disable-tools
   ninja qemu-aarch64
   ```
2. The plugin: `gcc -O2 -shared -fPIC -I<qemu-src>/include/qemu $(pkg-config --cflags glib-2.0) jitcount.c -o libjitcount.so`
3. `QEMU=<qemu-src>/build/qemu-aarch64 sh measure.sh <aarch64 bananads> <rom> <label> [--set ...]`

`jitcount.c` counts per function symbol of the binary and, for code without one, per translation block; `report.py`
puts the blocks the JIT's perf map names (`MELONDS_JIT_PERFMAP`, which measure.sh sets) into "jit-code" (the exit
stubs show as `stub/unknown`) and takes frames 100-300 as the difference of a 100-frame and a 300-frame run. The
ROM to use for the CPU is `bananads/tests/cpubench/` (the stress ROMs are 3D-bound: the JIT is 2-3% of them).
