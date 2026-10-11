#!/bin/sh
# Builds the handheld test kit: bananads-device-<commit>.tar.gz, unpacked on the device and run with
# device-test.sh (docs/DEVICE-TEST.md). Run from the SuperDrastic checkout on an x86-64 Linux PC (or CI) with:
#   g++-aarch64-linux-gnu, cmake, clang, lld, llvm (stress ROMs), and arm64 SDL2/ALSA dev packages for the
#   window frontend (Debian/Ubuntu multiarch: libsdl2-dev:arm64 libasound2-dev:arm64). Without them the kit is
#   headless-only, which is all device-test.sh needs.
#   sh bananads/tools/package-device.sh [outdir]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(cd "${1:-$ROOT}" && pwd)
SHA=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
KIT=$OUT/bananads-device
B=$OUT/build-device
rm -rf "$KIT"; mkdir -p "$KIT/bin" "$KIT/roms/stress" "$KIT/roms/diff" "$KIT/tools"

SDL=OFF
if [ -f /usr/lib/aarch64-linux-gnu/pkgconfig/sdl2.pc ] && [ -f /usr/lib/aarch64-linux-gnu/pkgconfig/alsa.pc ]; then SDL=ON; fi
for jit in banana melonds; do
    PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
        cmake -S "$ROOT/bananads" -B "$B-$jit" -DCMAKE_TOOLCHAIN_FILE="$ROOT/bananads/cmake/aarch64-linux-gnu.cmake" \
        -DBANANADS_SDL=$SDL -DBANANADS_ALSA=$SDL -DBANANADS_JIT=$jit > "$B-$jit.log"
    cmake --build "$B-$jit" -j"$(nproc)" --target bananads bananads-difftest >> "$B-$jit.log"
    name=$([ $jit = banana ] && echo bananajit || echo melonjit)
    cp "$B-$jit/bananads" "$KIT/bin/bananads-$name"
    cp "$B-$jit/bananads-difftest" "$KIT/bin/bananads-difftest-$name"
done
aarch64-linux-gnu-strip "$KIT"/bin/*

for v in arm thumb arm7 arm7-thumb; do
    case $v in
        arm) env="" d=out ;;
        thumb) env="STRESS_THUMB=1" d=out-thumb ;;
        arm7) env="STRESS_ARM7=arm" d=out-arm7 ;;
        arm7-thumb) env="STRESS_ARM7=thumb" d=out-arm7-thumb ;;
    esac
    env $env python3 "$ROOT/stressrom/build.py" > /dev/null
    mkdir -p "$KIT/roms/stress/$v"
    cp "$ROOT/stressrom/$d"/dsstress-L1.nds "$ROOT/stressrom/$d"/dsstress-L5.nds "$ROOT/stressrom/$d"/dsstress-L10.nds \
       "$ROOT/stressrom/$d"/dsswap.nds "$KIT/roms/stress/$v/"
done

# The DraStic JIT study's differential ROMs (SuperDrastic PR #10, its own clean-room builder), when reachable.
T=$(mktemp -d)
if git -C "$ROOT" fetch -q --depth 1 origin drastic-jit-re 2>/dev/null; then
    git -C "$ROOT" archive FETCH_HEAD docs/drastic-jit/tools/dyn/diff | tar -x -C "$T"
    D=$T/docs/drastic-jit/tools/dyn/diff
    python3 "$D/build.py" > /dev/null && python3 "$D/build.py" extra > /dev/null
    cp "$D/out/diff.nds" "$D/out/tests.txt" "$KIT/roms/diff/"
    cp "$D/out_extra/diff_extra.nds" "$KIT/roms/diff/"
    cp "$D/out_extra/tests.txt" "$KIT/roms/diff/tests_extra.txt"
else
    echo "(drastic-jit-re not reachable: the kit has no differential ROMs; device-test.sh skips them)"
fi
rm -rf "$T"

cp "$ROOT/bananads/tools/device-test.sh" "$KIT/"
cp "$ROOT/bananads/tools/check_results.py" "$KIT/tools/"
cp "$ROOT/bananads/docs/DEVICE-TEST.md" "$KIT/README.md"
cp "$ROOT/bananads/docs/SETTINGS.md" "$KIT/SETTINGS.md"
echo "$SHA (SDL window: $SDL)" > "$KIT/VERSION"
tar -C "$OUT" -czf "$OUT/bananads-device-$SHA.tar.gz" bananads-device
echo "$OUT/bananads-device-$SHA.tar.gz"
