#!/bin/sh
# on the handheld: [CLK=<kHz>] [EXTRA="--set ..."] hgquick.sh <bin>... : HeartGold's overworld, the fps of frames
# 1500-2100 (600 frames, the player walking left and right), each binary in turn, the clock pinned (CLK, default
# 1992000; 1104000 is the one that matters for A/Bs). A run that can't show it measured the scene (the save loaded,
# frame 1500's picture: hgcommon.sh) prints INVALID instead of its fps, and the script exits 1.
. "$(dirname "$0")/hgcommon.sh"
pin_clock ${CLK:-1992000}
rc=0
for B in "$@"; do
  fresh
  $B $COMMON --set headless.frames=2100 --set headless.measure_from=$FROM ${EXTRA:-} "$ROM" > "$W/log" 2>&1
  fps=$(grep "frames $FROM-2100" "$W/log" | grep -o '[0-9.]* fps' | head -1)
  if why=$(scene "$W/log"); then
    echo "$(basename $B): frames $FROM-2100 at ${CLK:-1992000} kHz: ${fps:-no fps line} ${EXTRA:+($EXTRA) }[scene $why]"
    bounds "${fps% fps}"
  else
    echo "$(basename $B): INVALID: $why (fps line: ${fps:-none}; the log: $W/log)"; rc=1
  fi
done
exit $rc
