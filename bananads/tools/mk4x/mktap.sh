#!/bin/bash
# mktap.sh <tag> <bin> [secs=40] [KEY=value...]: an mkbench run with audio.tap (the emulated sound from the state load,
# 32768 Hz s16 stereo) -> ~/mk4x/runs/<tag>/tap.raw, then compared with REF (default ~/mk4x/runs/tapref-2x/tap.raw,
# base binary at 2x) over the shorter one's length: the exactness gate (identical = render-only change).
T=$1; B=$2; S=${3:-40}; shift 3 2>/dev/null || shift $#
~/mk4x/tools/mkbench.sh "$T" "$B" "$S" BANANADS_AUDIO_TAP=/data/mk4x/runs/$T/tap.raw BANANADS_AUDIO_TAP_SECS=120 "$@"
R=${REF:-$HOME/mk4x/runs/tapref-2x/tap.raw}; A=~/mk4x/runs/$T/tap.raw
[ -f "$A" ] || { echo "$T: no tap"; exit 1; }
[ "$(readlink -f $A)" = "$(readlink -f $R)" ] && { echo "$T: reference tap $(stat -c %s $A) bytes"; exit 0; }
n=$(( $(stat -c %s "$A") < $(stat -c %s "$R") ? $(stat -c %s "$A") : $(stat -c %s "$R") ))
if cmp -n $n "$A" "$R" >/dev/null; then echo "$T: TAP IDENTICAL to $R over $n bytes ($(awk "BEGIN{print $n/4/32768}") s)"
else echo "$T: TAP DIFFERS: $(cmp -n $n "$A" "$R" | head -1) (of $n bytes)"; fi
