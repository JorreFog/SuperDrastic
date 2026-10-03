#!/bin/sh
# run.sh <rom.nds> [seconds] [profile.txt]: runs ROCKNIX's DraStic on the simulated handheld (setup.sh) for <seconds>
# (default 60) on $DISPLAY (:99 if unset; start one with `Xvfb :99 -screen 0 1280x1024x24 &`). With a profile path,
# the fnprof plugin counts every executed block, and fnreport.py turns it into instructions per DraStic function.
# DraStic's settings: work/drastic/config/drastic.cfg (hires_3d, threaded_3d...). Its output: work/drastic.log.
HERE=$(cd "$(dirname "$0")" && pwd)
W=${SIM_WORK:-$HERE/work}
ROM=$(readlink -f "${1:?usage: run.sh <rom.nds> [seconds] [profile.txt]}"); SECS=${2:-60}; PROF=$3
PLUG=; [ -n "$PROF" ] && PLUG="-plugin $W/fnprof.so,inline=on,out=$(readlink -f "$(dirname "$PROF")")/$(basename "$PROF")"
cd "$W/drastic" || exit 1
DISPLAY=${DISPLAY:-:99} SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy \
    timeout "$SECS" "$W/qemu-build/qemu-aarch64" $PLUG -L "$W/sysroot" -E LD_PRELOAD="$W/simshim.so" ./drastic "$ROM" \
    > "$W/drastic.log" 2>&1
BASE=$(sed -n 's/^\[sim\] drastic base 0x//p' "$W/drastic.log" | head -n1)
if [ -n "$PROF" ] && [ -n "$BASE" ]; then python3 "$HERE/fnreport.py" "$PROF" "$W/drastic/drastic" "$BASE" 40; fi
grep '^\[sim\]' "$W/drastic.log" | grep -v 'drastic base' | sort | uniq -c | head
