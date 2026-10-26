#!/bin/bash
# mkbench.sh <tag> <local bananads> [secs=60] [KEY=value...]   MKDS race bench on the Plus (under ~/bdnight/plus.lock)
# Env: STATE (device path, default /data/mk4x/race.mst; none = cold boot), INPUT (local kms.input file; default
# ~/mk4x/tools/race.input = A held + the fixed steering pattern), SHOTS (s between screenshots), PIN=1, SETTLE, MAXTEMP.
# Settings per run as BANANADS_<KEY>=value, e.g. BANANADS_RENDER_SCALE=4 BANANADS_KMS_SHADER=none.
# Fetches log, threads, temps, shots to ~/mk4x/runs/<tag>/. Needs a binary with kms.input (mk4x/bench a8941d6).
T=$1; B=$2; S=${3:-60}; shift 3 2>/dev/null || shift $#
while [ -e ~/bdnight/DEVICE-PAUSED ]; do sleep 15; done
# M1 priority: while ~/mk4x/M1-NEXT exists, other callers wait before queueing on the lock (M1 runs with M1=1)
[ "$M1" = 1 ] || while [ -e ~/mk4x/M1-NEXT ]; do sleep 10; done
[ -x "$B" ] || { echo "no binary $B"; exit 1; }
IN=${INPUT-$HOME/mk4x/tools/race.input}
K=~/bananaos-0.4.3/dev.sh; L=~/bdnight/plus.lock; R=~/mk4x/runs/$T; rm -rf "$R"; mkdir -p "$R"
flock -w ${LOCKWAIT:-3600} $L bash -c '
  T=$1; B=$2; K=$3; IN=$4; S=$5; shift 5
  $K plus "mkdir -p /data/mk4x/bin /data/mk4x/in; cat > /data/mk4x/bin/$T.tmp && chmod +x /data/mk4x/bin/$T.tmp && mv /data/mk4x/bin/$T.tmp /data/mk4x/bin/$T" < "$B" || exit 1
  $K plus "cat > /data/mk4x/mkbench.sh" < ~/mk4x/tools/mkbench-dev.sh; $K plus "cat > /data/mk4x/mksum.sh" < ~/mk4x/tools/mksum-dev.sh
  I=; [ -n "$IN" ] && { $K plus "cat > /data/mk4x/in/$T.input" < "$IN"; I=/data/mk4x/in/$T.input; }
  $K plus "STATE=${STATE:-} INPUT=$I SHOTS=${SHOTS:-0} SETTLE=${SETTLE:-6} PIN=${PIN:-0} MAXTEMP=${MAXTEMP:-50000} sh /data/mk4x/mkbench.sh $T /data/mk4x/bin/$T $S $*"
  $K plus "rm -f /data/mk4x/bin/$T"' _ "$T" "$B" "$K" "$IN" "$S" "$@"
$K plus "cd /data/mk4x/runs/$T && tar cf - run.txt env.txt threads.txt temps.txt bananads.log shots mark trace.txt prof.txt tap.raw 2>/dev/null" | tar xf - -C "$R"
