#!/bin/sh
# run.sh <rom.nds> [seconds] [extra drastic args, e.g. --recompiler]: DraStic under qemu-aarch64 with jitdump.so
# preloaded, on Xvfb :97. Outputs in $OUT (default <this dir>/out): drastic.log, jit_raw.txt, jit_log.txt.
# result.txt: words a test ROM reports (see jitdump.c result_watch); the run ends when the ROM writes DONE.
# Env: JITDUMP_MAX=<n blocks>, JITDUMP_LOG= (empty: skip DraStic's own log).
# Note: the tonistiigi/binfmt qemu-aarch64 is built for binfmt_misc's preserve-argv0 mode and consumes the first
# guest argument as argv[0], so the program name is passed twice.
D=$(cd "$(dirname "$0")" && pwd)
ROM=$(readlink -f "$1"); SECS=${2:-20}; shift; [ $# -gt 0 ] && shift
OUT=${OUT:-$D/out}; mkdir -p "$OUT"
pgrep -f 'Xvfb :97' >/dev/null || { Xvfb :97 -screen 0 1280x1024x24 >/dev/null 2>&1 & sleep 1; }
cd "$D/drastic" || exit 1
DISPLAY=:97 SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy XDG_RUNTIME_DIR=/tmp \
  timeout -s KILL "$SECS" "$D/qemu-aarch64" -L "$D/sysroot" -E LD_PRELOAD="$D/jitdump.so" \
  -E JITDUMP_RAW="$OUT/jit_raw.txt" -E JITDUMP_LOG="${JITDUMP_LOG-$OUT/jit_log.txt}" \
  ${JITDUMP_MAX:+-E JITDUMP_MAX=$JITDUMP_MAX} -E JITDUMP_RESULT="$OUT/result.txt" \
  ./drastic ./drastic "$@" "$ROM" > "$OUT/drastic.log" 2>&1
echo "exit $?"
