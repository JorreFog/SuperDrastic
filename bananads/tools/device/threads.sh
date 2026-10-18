#!/bin/sh
# on the handheld: [CLK=<kHz>] threads.sh <bin> [--set ...]: the CPU % of each of BananaDS's threads over 8 s of
# HeartGold's overworld (from frame ~1600; /proc/<pid>/task/*/stat), the clock pinned (CLK, default 1104000). The
# thread at ~100% is the one to work on (headless: the emulator's thread is the main one, "bananads"). The scene is
# checked as hgquick.sh does (hgcommon.sh).
. "$(dirname "$0")/hgcommon.sh"
B=$1; [ -x "$B" ] || die "usage: threads.sh <bin> [--set ...]"; shift
CLK=${CLK:-1104000}; pin_clock $CLK
fresh
$B $COMMON --set headless.frames=6000 --set headless.measure_from=$FROM --set headless.window=100 "$@" "$ROM" > "$W/log" 2>&1 &
pid=$!
# the overworld: the first 100-frame window after frame 1500 logged
n=0; until grep -q "frames $FROM-" "$W/log"; do kill -0 $pid 2>/dev/null || die "it stopped before frame $FROM (the log: $W/log)"; n=$((n+1)); [ $n -gt 300 ] && die "no frame $FROM after 300 s"; sleep 1; done
snap() { for t in /proc/$pid/task/*; do echo "${t##*/} $(sed 's/.*) //' $t/stat | awk '{print $12 + $13}') $(cat $t/comm)"; done; }
s0=$(snap); f0=$(grep -c "frames .*fps" "$W/log"); sleep 8; s1=$(snap); f1=$(grep -c "frames .*fps" "$W/log")
kill $pid 2>/dev/null; wait $pid 2>/dev/null
hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
why=$(scene "$W/log") || { echo "INVALID: $why (the log: $W/log)"; exit 1; }
echo "threads of $(basename $B) $* at $CLK kHz, 8 s of the overworld (about $(( (f1 - f0) * 100 / 8 )) fps) [scene $why]"
echo "$s1" | while read tid t1 name; do
  t0=$(echo "$s0" | awk -v id=$tid '$1 == id { print $2 }')
  awk -v a=${t0:-0} -v b=$t1 -v hz=$hz -v n="$name" -v id=$tid -v p=$pid 'BEGIN { printf "%5.1f%%  %-16s %s%s\n", (b - a) * 100 / hz / 8, n, id, id == p ? " (main: the emulator in headless)" : "" }'
done | sort -rn
