#!/bin/sh
# build.sh <sysroot> [shtest]   builds build/libsuperdrastic.so (and with "shtest", build/shtest) for aarch64 Linux.
# build.sh wfc_test             builds and runs the host test for the Wi-Fi NAT/frame code (gcc, no sysroot).
#
# <sysroot>: an aarch64 glibc >= 2.38 sysroot with libdrm's headers and libdrm.so. One way to make it (what CI does,
# see .github/workflows/build.yml): extract the Debian trixie arm64 packages libc6, libc6-dev, linux-libc-dev,
# libdrm-dev, libdrm2, libgcc-14-dev and libgcc-s1 into one directory, then `ln -s usr/lib <sysroot>/lib`.
# Needs clang, lld and llvm-strip on the build machine (any architecture).
set -e
SR=${1:?usage: build.sh <aarch64-sysroot> [shtest] | build.sh wfc_test}
cd "$(dirname "$0")"
mkdir -p build
if [ "$1" = wfc_test ]; then      # src/wifi.c's frame builder and userspace NAT, tested on the build machine (tools/wfc_test.c)
    gcc -O2 -Wall -Wextra -pthread -Isrc -o build/wfc_test src/wfcnet.c src/wififrame.c tools/wfc_test.c && ./build/wfc_test
    exit $?
fi
RC=src/third_party/rcheevos
# src/rast: our 3D rasterizer for DraStic's hi-res mode (off unless DSFLIP_RAST=1; see src/rast/README.md)
# src/wifi.c + wififrame.c + wfcnet.c: Nintendo WFC over an emulated open AP (off unless DSFLIP_WFC / nds.wfc_dns is set)
SRCS="src/dsflip.c src/shader.c src/audio.c src/ra.c src/ui.c src/volume.c src/cpugov.c src/resume.c src/menu.c src/wifi.c src/wififrame.c src/wfcnet.c $(ls $RC/src/rc_client.c $RC/src/rc_compat.c \
      $RC/src/rc_util.c $RC/src/rc_version.c $RC/src/rcheevos/*.c $RC/src/rapi/*.c $RC/src/rhash/*.c) \
      src/rast/rast.c src/rast/walk.c src/rast/b0.c src/rast/fused.c src/rast/fused_neon.c src/rast/fused_asm.c src/rast/defer.c src/rast/hr.c src/rast/comp.c src/rast/res2.c src/rast/res2_line.S src/rast/rast_kern.S $(ls src/rast/spec/*.c)"
python3 src/rast/kerngen.py src/rast/rast_kern.S
python3 src/rast/res2gen.py > src/rast/res2_line.S 2>/dev/null
V=$(cat VERSION 2>/dev/null || echo dev)
# -mtune=cortex-a55: scheduled for the handhelds' in-order cores (the RG DS and RG DS Plus); still plain ARMv8.0 code
CC="clang --target=aarch64-linux-gnu --sysroot=$SR -fuse-ld=lld -O2 -mtune=cortex-a55 -Wall -Wno-unused-function"
$CC -shared -fPIC -DDSFLIP_VERSION="\"$V\"" -DRC_DISABLE_LUA -DRC_CLIENT_SUPPORTS_HASH -I"$SR/usr/include/libdrm" \
    -I$RC/include -I$RC/src -Isrc/third_party/stb -o build/libsuperdrastic.so $SRCS -ldrm -lpthread -lm
llvm-strip --strip-unneeded build/libsuperdrastic.so
ls -l build/libsuperdrastic.so
if [ "$2" = shtest ]; then       # the shader benchmark (tools/shtest.c): runs a shader on a frame without DraStic
    $CC -I"$SR/usr/include/libdrm" -o build/shtest tools/shtest.c src/shader.c -ldrm -ldl
    ls -l build/shtest
fi
