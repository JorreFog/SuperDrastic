#!/bin/sh
# measure.sh <bananads (aarch64)> <rom> <label> [--set k=v ...]: the main thread's host instructions per emulated
# frame over frames 100-300, by part (JIT code, JIT runtime, core, memory, 2D, 3D...), counted under qemu with the
# jitcount plugin. Single-threaded (soft 3D on the emulator's thread) so the counts are exactly reproducible.
# Needs a qemu-aarch64 built with --enable-plugins (QEMU=..., README in this directory) and the plugin built.
# Output: $OUT/<label>/report.txt (OUT defaults to ./qcount-runs). Environment variables pass through (MELONDS_*).
set -eu
B=$1; ROM=$2; L=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
Q=${QEMU:-qemu-aarch64}; P=${PLUGIN:-$HERE/libjitcount.so}; W=${OUT:-$PWD/qcount-runs}/$L; mkdir -p "$W"
for n in 100 300; do
  # (the perf map is per process: /tmp/perf-<pid>.map, the pid being qemu's; other runs may be going at once)
  MELONDS_JIT_PERFMAP=1 $Q -L /usr/aarch64-linux-gnu -plugin "$P,out=$W/f$n" "$B" --frontend headless --set jit=on \
    --set render.renderer=soft --set render.threaded=off --set render.2d_thread=off --set rtc.fixed=2026-10-11_12:00:00 \
    --set headless.frames=$n --set save.dir="$W" --set wifi.dns=off --set resume.on_start=off --set resume.on_quit=off \
    "$@" "$ROM" > "$W/log$n" 2>&1 &
  pid=$!
  wait $pid || :
  mv /tmp/perf-$pid.map "$W/f$n.map" 2>/dev/null || : > "$W/f$n.map"
  mv /tmp/melonjit-$pid.code "$W/f$n.code" 2>/dev/null || :
done
python3 "$HERE/report.py" "$W/f100.tsv" "$W/f100.map" "$W/f300.tsv" "$W/f300.map" 200 > "$W/report.txt"
cat "$W/report.txt"
