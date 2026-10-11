#!/bin/sh
# bench.sh <tag> <game> [VAR=val ...]      (ON the RG DS Plus; refuses while a player's game session runs)
# One measured run of a DS game with a test library: the display is taken from ES/sway (they come back at the end
# unless KEEP=1; `bench.sh restore` brings them back later).
#   game: hg (HeartGold from hg-master.dss, walking)   b2 (Black 2 from its state 0, walking)
#         L3 L6 L10 (the stress ROM's levels)           S<n> (a feature scene, when present)   or a ROM path
# Settings (environment): LIB (library; default the installed one), EXE (DraStic binary), KHZ (fixed CPU clock,
#   default 1104000), WARM (10 s), SECS (30 s), PERF (both|stat|record|none), SHADER (none), WALK (1|0), KEEP (0|1),
#   FREQ (perf record frequency, 1500)
# Output in /storage/dsflip/opt/runs/<tag>.*: .log (libdsflip), .out (DraStic), .threads, .stat, .report, .sum
O=/storage/dsflip/opt/runs; mkdir -p $O
CFGD=/storage/.config/drastic C=/sys/devices/system/cpu/cpufreq/policy0 G=/sys/class/devfreq/fde60000.gpu
ST=/storage/dsflip/opt/.state
restore() {
    [ -f /tmp/bench.pid ] && kill -9 $(cat /tmp/bench.pid) 2>/dev/null; rm -f /tmp/bench.pid
    [ -f $ST/gov ] && cat $ST/gov > $C/scaling_governor; [ -f $ST/max ] && cat $ST/max > $C/scaling_max_freq
    [ -f $ST/ggov ] && cat $ST/ggov > $G/governor
    rm -rf $ST
    systemctl is-active -q sway.service || { systemctl start sway.service; sleep 2; }
    systemctl is-active -q essway.service || systemctl start essway.service
    echo restored
}
[ "$1" = restore ] && { restore; exit 0; }
if systemctl is-active -q dsflip-game; then echo "bench: a game session is running: not starting" >&2; exit 3; fi
TAG=${1:?tag}; GAME=${2:?game}; shift 2
# harness settings may also come as arguments (NAME=value); everything else is DraStic's environment
ENVS=
for a in "$@"; do case "$a" in
  LIB=*|EXE=*|KHZ=*|WARM=*|SECS=*|PERF=*|SHADER=*|WALK=*|KEEP=*|FREQ=*|PEV=*|PCNT=*|PCG=*|SHOT=*|EV=*) eval "${a%%=*}=\"\${a#*=}\"" ;;
  *) ENVS="$ENVS $a" ;;
esac; done
LIB=${LIB:-$CFGD/dsflip/libdsflip.so} EXE=${EXE:-$CFGD/dsflip/drastic} KHZ=${KHZ:-1104000} WARM=${WARM:-10} SECS=${SECS:-30}
PERF=${PERF:-both} SHADER=${SHADER:-none} WALK=${WALK:-1} FREQ=${FREQ:-1500}
SS=/storage/roms/savestates/nds
RES= DSV=
case "$GAME" in
  hg) ROM="/storage/roms/nds/Pokemon - HeartGold Version (USA).nds"; RES=/storage/dsflip/hgtest/hg-master.dss ;;
  b2) ROM="/storage/roms/nds/Pokemon Black Version 2 (DSi Enhanced).nds"; RES="$SS/Pokemon Black Version 2 (DSi Enhanced)_0.dss" ;;
  hgboot) ROM="/storage/roms/nds/Pokemon - HeartGold Version (USA).nds"; WALK=0 ;;
  b2boot) ROM="/storage/roms/nds/Pokemon Black Version 2 (DSi Enhanced).nds"; WALK=0 ;;
  mk) ROM="/storage/roms/nds/Mario Kart DS (USA) (En,Fr,De,Es,It).nds"; WALK=0 ;;
  dq) ROM="/storage/roms/nds/Dragon Quest Monsters - Joker (USA).nds"; WALK=0 ;;
  mph) ROM="/storage/roms/nds/Metroid Prime Hunters (Europe).nds"; WALK=0 ;;
  plat) ROM="/storage/roms/nds/Pokemon Platinum Version.nds"; WALK=0 ;;
  L*) ROM=/storage/dsflip/roms/dsstress-$GAME.nds; WALK=0 ;;
  S*) ROM=/storage/dsflip/roms/dsscenes-$GAME.nds; WALK=0 ;;
  *)  ROM=$GAME; WALK=${WALK:-0} ;;
esac
[ -f "$ROM" ] || { echo "bench: no ROM $ROM" >&2; exit 2; }
DSV="${ROM%.*}.dsv"
[ -f "$DSV" ] && cp "$DSV" /tmp/bench.dsv
# the display: once per series
if systemctl is-active -q essway.service || systemctl is-active -q sway.service; then
    mkdir -p $ST; cat $C/scaling_governor > $ST/gov; cat $C/scaling_max_freq > $ST/max; cat $G/governor > $ST/ggov
    S=$(ls /var/run/0-runtime-dir/sway-ipc.*.sock 2>/dev/null | head -n1)
    [ -n "$S" ] && XDG_RUNTIME_DIR=/var/run/0-runtime-dir swaymsg -s "$S" output '*' power off >/dev/null 2>&1
    systemctl stop essway.service sway.service; sleep 2
fi
echo performance > $C/scaling_governor; cat $C/cpuinfo_max_freq > $C/scaling_max_freq
case "$SHADER" in none|bilinear) echo powersave > $G/governor ;; *) echo performance > $G/governor ;; esac
RENV=
if [ -n "$RES" ]; then cp "$RES" /tmp/bench-run.dss; RENV="DSFLIP_RESUME_FILE=/tmp/bench-run.dss DSFLIP_RESUME_LOAD=1"; fi
rm -f $O/$TAG.*
cd $CFGD
setsid env SDL_VIDEODRIVER=dummy XDG_RUNTIME_DIR=/var/run/0-runtime-dir DSFLIP_LOG=$O/$TAG.log DSFLIP_CPUGOV=0 \
    DSHOOK_SHADER=$SHADER DSFLIP_QUEUE=1 DSFLIP_QUEUE_WAIT=0 DSFLIP_LATCH_MARGIN=3000 DSFLIP_CPUGOV_MEMORY=0 $RENV $ENVS \
    LD_PRELOAD=$LIB $EXE "$ROM" > $O/$TAG.out 2>&1 &
P=$!; echo $P > /tmp/bench.pid
sleep 3; echo $KHZ > $C/scaling_max_freq
sleep $WARM
if ! kill -0 $P 2>/dev/null; then echo "bench: DraStic died during warm-up (see $O/$TAG.out)"; tail -n 5 $O/$TAG.out; [ "$KEEP" = 1 ] || restore; exit 1; fi
snap() { for t in /proc/$P/task/[0-9]*; do echo "${t##*/} $(awk '{print $14 + $15, $39}' $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)"; done; }
WPID=
if [ "$WALK" = 1 ]; then ( while :; do for k in 545 546 547 544; do python3 /storage/dsflip/opt/padkey.py $k 1.2; done; done ) & WPID=$!; sleep 1; fi
n0=$(grep -c "present/s" $O/$TAG.log); snap > /tmp/bench.th0; t0=$(date +%s%N)
if [ -n "$SHOT" ]; then ( sleep $SHOT; rm -f /storage/dsflip/logs/scan0.raw /storage/dsflip/logs/scan1.raw; kill -USR2 $P; sleep 1.5
    cp /storage/dsflip/logs/scan0.raw $O/$TAG.scan0.raw; cp /storage/dsflip/logs/scan1.raw $O/$TAG.scan1.raw ) & fi
EV=${EV:-cycles,instructions,l1d_cache_refill,l2d_cache_refill,l1d_cache,branch-misses}
case "$PERF" in
  stat)   perf stat -e $EV --per-thread -p $P -o $O/$TAG.stat -- sleep $SECS >/dev/null 2>&1 ;;
  record) perf record -q -F $FREQ $PCG -p $P -o /tmp/bench.perf -- sleep $SECS >/dev/null 2>&1 ;;
  event)  perf record -q -e ${PEV:-l2d_cache_refill} -c ${PCNT:-300} -p $P -o /tmp/bench.perf -- sleep $SECS >/dev/null 2>&1 ;;
  both)   H=$((SECS / 2))
          perf stat -e $EV --per-thread -p $P -o $O/$TAG.stat -- sleep $H >/dev/null 2>&1
          perf record -q -F $FREQ -p $P -o /tmp/bench.perf -- sleep $((SECS - H)) >/dev/null 2>&1 ;;
  *)      sleep $SECS ;;
esac
snap > /tmp/bench.th1; t1=$(date +%s%N); n1=$(grep -c "present/s" $O/$TAG.log)
[ -n "$WPID" ] && { kill $WPID 2>/dev/null; wait $WPID 2>/dev/null; }
cat /sys/class/thermal/thermal_zone0/temp > /tmp/bench.temp
kill -9 $P 2>/dev/null; wait $P 2>/dev/null; rm -f /tmp/bench.pid
for k in 544 545 546 547; do python3 -c "
import glob, os, struct
dev = next(d for d in sorted(glob.glob('/sys/class/input/event*')) if open(d + '/device/name').read().strip() == 'retrogame_joypad')
fd = os.open('/dev/input/' + os.path.basename(dev), os.O_WRONLY)
os.write(fd, struct.pack('llHHi', 0, 0, 1, $k, 0)); os.write(fd, struct.pack('llHHi', 0, 0, 0, 0, 0))" 2>/dev/null; done
[ -f /tmp/bench.dsv ] && { cp /tmp/bench.dsv "$DSV"; rm -f /tmp/bench.dsv; }
# the summary: fps over the window, per-thread ms per frame
W=$(grep "present/s" $O/$TAG.log | sed -n "$((n0 + 1)),${n1}p")
FPS=$(echo "$W" | sed 's/.*present\/s=\([0-9.]*\).*/\1/' | awk '{s+=$1;n++} END {if(n) printf "%.2f", s/n; else print 0}')
MINF=$(echo "$W" | sed 's/.*present\/s=\([0-9.]*\).*/\1/' | sort -n | head -n1)
DROP=$(echo "$W" | sed 's/.* dropped=\([0-9]*\).*/\1/' | awk '{s+=$1} END {print s+0}')
REP=$(echo "$W" | sed 's/.* repeat=\([0-9]*\).*/\1/' | awk '{s+=$1} END {print s+0}')
awk -v ns=$((t1 - t0)) -v fps=$FPS 'NR==FNR {a[$1]=$2; next} { d = ($2 - a[$1]) * 10; fr = fps * ns / 1e9;
      if (d > 0) printf "%6.2f ms/frame %5.1f%% cpu%s %s %s\n", (fr > 0 ? d / fr : 0), d / (ns / 1e8), $3, $1, $4 }' /tmp/bench.th0 /tmp/bench.th1 | sort -rn > $O/$TAG.threads
MAIN=$(grep " $P " $O/$TAG.threads | awk '{print $1}')
R3D=$(grep -E "rast-3d|drastic$" $O/$TAG.threads | grep -v " $P " | awk '{s+=$1} END {printf "%.2f", s}')
TOT=$(awk '{s+=$1} END {printf "%.2f", s}' $O/$TAG.threads)
echo "$TAG game=$GAME khz=$KHZ fps=$FPS min=$MINF drop=$DROP repeat=$REP main=${MAIN:-?} 3d=$R3D total=$TOT temp=$(cat /tmp/bench.temp) lib=$(basename $LIB) env=$ENVS" | tee $O/$TAG.sum
if [ -f /tmp/bench.perf ]; then
    perf report -i /tmp/bench.perf --stdio --sort comm,dso,sym 2>/dev/null | grep -v "^#" | grep -v "^$" | head -n 220 > $O/$TAG.report
    perf report -i /tmp/bench.perf --stdio --sort comm,dso 2>/dev/null | grep -v "^#" | grep -v "^$" | head -n 40 > $O/$TAG.dso
    perf report -i /tmp/bench.perf --stdio --tid $P --sort dso,sym 2>/dev/null | grep -v "^#" | grep -v "^$" | head -n 160 > $O/$TAG.main
    mv /tmp/bench.perf $O/$TAG.perf
fi
[ "$KEEP" = 1 ] || restore >/dev/null
