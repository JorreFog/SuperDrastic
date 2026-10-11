#!/bin/sh
# on the handheld: hgprofile.sh <bananads>: where HeartGold's time goes with this build, for choosing the next
# optimization by the device instead of by qemu counts (docs/PERF.md). About 8 minutes; writes
# /storage/bananads-profile-<date>.txt to paste back. Needs /storage/bananads-dev/{hg.nds,hg.sav,walk.txt} (as
# hgquick.sh) and perf. Clock pinned at 1992 MHz (performance governor), put back on exit.
B=$1; [ -x "$B" ] || { echo "usage: hgprofile.sh <bananads binary>"; exit 1; }
D=/storage/bananads-dev; OUT=/storage/bananads-profile-$(date +%Y%m%d-%H%M%S).txt
CPU=/sys/devices/system/cpu/cpufreq/policy0
G=$(cat $CPU/scaling_governor); MN=$(cat $CPU/scaling_min_freq); MX=$(cat $CPU/scaling_max_freq)
restore() { echo $MN > $CPU/scaling_min_freq; echo $MX > $CPU/scaling_max_freq; echo $MN > $CPU/scaling_min_freq; echo $G > $CPU/scaling_governor; }
trap restore EXIT; trap 'exit 130' INT TERM
echo performance > $CPU/scaling_governor; echo 1992000 > $CPU/scaling_max_freq; echo 1992000 > $CPU/scaling_min_freq
COMMON="--frontend headless --set save.dir=/tmp/hgb --set state.dir=/tmp/hgb --set resume.on_start=off --set resume.on_quit=off --set wifi.dns=off --set render.gengis_threads=2 --set headless.input=$(cat $D/walk.txt)"
fresh() { rm -rf /tmp/hgb /tmp/hgt; mkdir -p /tmp/hgb /tmp/hgt; cp $D/hg.sav /tmp/hgb/hg.sav; }
{
echo "# BananaDS HeartGold profile, $(date), $(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')"
echo "binary: $B ($($B --version 2>&1 | head -1))"
echo
echo "## fps, frames 1500-2100, and the pictures at frames 1200/2100 (a hang shows as a different hash)"
echo "frame 1200 of a good run: 12e33782..."
echo
echo "| settings | fps | frame 1200 | frame 2100 |"
echo "|---|---|---|---|"
} > $OUT
run() {   # run <label> [--set ...]: one hgquick-like run
  label=$1; shift; fresh
  fps=$($B $COMMON --set headless.frames=2100 --set headless.measure_from=1500 --set headless.dump_dir=/tmp/hgt \
    --set headless.dump_every=300 "$@" $D/hg.nds 2>&1 | grep "frames 1500" | grep -o '[0-9.]* fps')
  h1=$(md5sum < /tmp/hgt/frame1200.ppm 2>/dev/null | cut -c1-8); h2=$(md5sum < /tmp/hgt/frame2100.ppm 2>/dev/null | cut -c1-8)
  echo "| $label | $fps | $h1 | $h2 |" >> $OUT
  echo "$label: $fps"
}
run "defaults"
run "render.2d_thread=on" --set render.2d_thread=on
run "render.2d_thread=on, gengis_threads=1" --set render.2d_thread=on --set render.gengis_threads=1
run "core.slice=auto (timing trade)" --set core.slice=auto
run "core.fast_gxdma=on core.fast_spu=on" --set core.fast_gxdma=on --set core.fast_spu=on
run "jit.codegen=old" --set jit.codegen=old
run "render.geom_fast=off" --set render.geom_fast=off
run "render.2d_fast=off" --set render.2d_fast=off
run "render.2d_fast=off, render.2d_thread=on" --set render.2d_fast=off --set render.2d_thread=on
run "core.fast_mix=off core.fast_dma=off" --set core.fast_mix=off --set core.fast_dma=off
run "all new parts off" --set jit.codegen=old --set core.fast_loop=off --set core.fast_timers=off --set core.fast_sched=off \
  --set jit.vramfast=off --set jit.iodirect=off --set render.vramtrack_skip=off --set jit.iofast=off \
  --set render.geom_fast=off --set render.2d_fast=off --set core.fast_mix=off --set core.fast_dma=off

echo >> $OUT
if command -v perf > /dev/null; then
  # the overworld only: attach once the scene is running (about 30 s in), 15 s of samples, all threads
  fresh
  MELONDS_JIT_PERFMAP=1 $B $COMMON --set headless.frames=5000 $D/hg.nds > /dev/null 2>&1 &
  pid=$!
  sleep 30
  perf record -F 1999 -p $pid -o /tmp/hg.perf -- sleep 15 > /dev/null 2>&1
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  {
  echo "## perf, the overworld, defaults: share by thread"
  echo '```'
  perf report -i /tmp/hg.perf --stdio --sort comm,tid 2>/dev/null | grep -v '^#' | grep -v '^$' | head -12
  echo '```'
  echo
  echo "## perf, the emulator's main thread: top 60 symbols (JIT blocks are mj9_/mj7_<guest address>)"
  echo '```'
  perf report -i /tmp/hg.perf --stdio --tid $pid --sort dso,sym 2>/dev/null | grep -v '^#' | grep -v '^$' | head -60
  echo '```'
  } >> $OUT
  rm -f /tmp/perf-$pid.map /tmp/melonjit-$pid.code
else
  echo "(perf not found: no profile)" >> $OUT
fi
echo "written: $OUT"
