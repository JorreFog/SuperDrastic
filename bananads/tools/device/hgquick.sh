#!/bin/sh
# on the handheld: hgquick.sh <bin>... : HeartGold, 600 frames measured after loading (1500-2100), 1992 MHz
CPU=/sys/devices/system/cpu/cpufreq/policy0
G=$(cat $CPU/scaling_governor); MN=$(cat $CPU/scaling_min_freq); MX=$(cat $CPU/scaling_max_freq)
restore() { echo $MN > $CPU/scaling_min_freq; echo $MX > $CPU/scaling_max_freq; echo $MN > $CPU/scaling_min_freq; echo $G > $CPU/scaling_governor; }
trap restore EXIT; trap 'exit 130' INT TERM
echo performance > $CPU/scaling_governor; echo 1992000 > $CPU/scaling_max_freq; echo 1992000 > $CPU/scaling_min_freq
for B in "$@"; do
  rm -rf /tmp/hgb; mkdir -p /tmp/hgb; cp /storage/bananads-dev/hg.sav /tmp/hgb/hg.sav
  $B --frontend headless --set save.dir=/tmp/hgb --set state.dir=/tmp/hgb --set resume.on_start=off --set resume.on_quit=off \
    --set wifi.dns=off --set headless.frames=2100 --set headless.measure_from=1500 --set render.gengis_threads=2 \
    --set headless.input=$(cat /storage/bananads-dev/walk.txt) ${EXTRA:-} /storage/bananads-dev/hg.nds 2>&1 | grep "frames 1500" | sed "s|^|$(basename $B): |"
done
