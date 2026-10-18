#!/bin/sh
# on the handheld: [CLK=<kHz>] [HZ=1000] [SYMS=<unstripped bin>] hgprofile.sh <bin> [--set ...]: where the emulator's
# thread spends HeartGold's overworld (frames 1500-2700), from BananaDS's own sampler (prof.hz: no perf needed): JIT
# code's share, modules, top symbols, by class, the hottest JIT blocks (MELONDS_JIT_PERFMAP). The device's binary is
# stripped: put the unstripped build of the same binary at <bin>.debug (or SYMS=) for names. The clock pinned (CLK,
# default 1104000). Writes $KIT/prof-<date>.txt (to paste back) and prints its head. The scene is checked as in
# hgquick.sh (hgcommon.sh).
. "$(dirname "$0")/hgcommon.sh"
B=$1; [ -x "$B" ] || die "usage: hgprofile.sh <bin> [--set ...]"; shift
SYMS=${SYMS:-$B.debug}; [ -f "$SYMS" ] && SY="--set prof.syms=$SYMS" || { SY=; echo "(no $SYMS: symbol names only if $B isn't stripped)"; }
CLK=${CLK:-1104000}; pin_clock $CLK
OUT=$KIT/prof-$(date +%Y%m%d-%H%M%S).txt
fresh
MELONDS_JIT_PERFMAP=1 MELONDS_JIT_PERFDIR=$W $B $COMMON --set headless.frames=2700 --set headless.measure_from=$FROM \
  --set prof.hz=${HZ:-1000} --set prof.from=$FROM --set prof.out=$OUT.tmp $SY "$@" "$ROM" > "$W/log" 2>&1
why=$(scene "$W/log") || { echo "INVALID: $why (the log: $W/log)"; exit 1; }
{ echo "# $(basename $B) $* at $CLK kHz, $(cat /proc/device-tree/model 2>/dev/null | tr -d '\0'), $(date) [scene $why]"
  grep "frames $FROM-" "$W/log"; cat $OUT.tmp; } > $OUT
rm -f $OUT.tmp $W/perf-*.map $W/melonjit-*.code
head -45 $OUT; echo "written: $OUT"
