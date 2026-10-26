#!/bin/sh
# mkbench-dev.sh <tag> <bin> [secs] [KEY=value...]   (on the RG DS Plus as /data/mk4x/mkbench.sh; run ONLY via ~/mk4x/tools/mkbench.sh)
# Mario Kart DS on the panels through the real launcher (menu launch hook, run-bananads, KMS, the card's settings),
# <bin> bind-mounted over /usr/bin/bananads, a COPY of the user's save, STATE (default /data/mk4x/race.mst) loaded as
# the resume state (STATE=none: cold boot), INPUT (a kms.input file on the device: frame-exact presses, the same
# emulated frames at every scale), cpugov memory empty. Measures kms.stats=60 windows for <secs> after a SETTLE (6 s);
# screenshots every SHOTS s (default 0 = start+end only), temps every 5 s, per-thread CPU %. PIN=1 cpugov off.
# Logs: /data/mk4x/runs/<tag>/
T=$1; B=$2; S=${3:-60}; shift 3 2>/dev/null || shift $#
D=/data/mk4x
[ -s /run/banana/game.pid ] && { echo "$T GAME-RUNNING: abort"; exit 1; }
pidof bananads >/dev/null && { echo "$T BANANADS-RUNNING: abort"; exit 1; }
pidof banana-ui >/dev/null || { echo "$T: no menu"; exit 1; }
[ -x "$B" ] || { echo "$T: no binary $B"; exit 1; }
[ $(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo) -ge 400 ] || { echo "$T: MemAvailable < 400 MB"; exit 1; }
i=0; while [ $(cat /sys/class/thermal/thermal_zone0/temp) -ge ${MAXTEMP:-50000} ]; do sleep 3; i=$((i+3)); [ $i -gt 400 ] && { echo "$T: too hot"; exit 1; }; done
T0=$(($(cat /sys/class/thermal/thermal_zone0/temp)/1000))
G="Mario Kart DS (USA) (En,Fr,De,Es,It)"; ROM="/data/roms/nds/$G.nds"; [ -f "$ROM" ] || { echo "$T: no rom"; exit 1; }
ST=${STATE:-$D/race.mst}
W=$D/runs/$T; rm -rf $W; mkdir -p $W/st $W/sv $W/gov $W/shots
cp "/data/saves/nds/$G.sav" $W/sv/; RS=off; [ "$ST" != none ] && { [ -f "$ST" ] || { echo "$T: no state $ST"; exit 1; }; cp "$ST" "$W/st/$G.resume.mst"; RS=on; }
{ echo "BANANADS_SAVE_DIR=$W/sv"; echo "BANANADS_STATE_DIR=$W/st"; echo "BANANADS_RESUME_ON_START=$RS"; echo "BANANADS_RESUME_ON_QUIT=off"
  echo "BANANADS_CPUGOV_DIR=$W/gov"; echo "BANANADS_KMS_STATS=60"; echo "BANANADS_CPUGOV_LOG=on"; echo "BANANADS_AUDIO_STATS=on"
  echo "BANANADS_RA_USER="; [ -n "$INPUT" ] && echo "BANANADS_KMS_INPUT=@$INPUT"
  [ "$PIN" = 1 ] && echo "BANANADS_CPUGOV=off"; for kv in "$@"; do echo "$kv"; done; } > /data/banana/bananads.env
PG=/data/banana/nds-per-game.conf; cp $PG $W/per-game.bak
{ grep -v "^$G.nds " $W/per-game.bak; echo "$G.nds = bananads"; } > $PG
cleanup() { [ -n "$TP" ] && kill $TP 2>/dev/null; umount /usr/bin/bananads 2>/dev/null; rm -f /data/banana/bananads.env /run/banana/launch; cp $W/per-game.bak $PG; }
trap cleanup EXIT; trap 'exit 130' INT TERM
echo "# $(date +%H:%M:%S) $T: $B secs=$S pin=${PIN:-0} state=$ST input=${INPUT:-none} temp=${T0}C env: $*" > $W/run.txt
cp /data/banana/bananads.env $W/env.txt
mount --bind $B /usr/bin/bananads
echo "$ROM" > /run/banana/launch
i=0; until pidof bananads >/dev/null; do pidof drastic >/dev/null && { kill -TERM $(pidof drastic); echo "$T: DRASTIC STARTED INSTEAD: abort"; exit 1; }; sleep 0.2; i=$((i+1)); [ $i -gt 100 ] && { echo "$T: the menu didn't start the game"; exit 1; }; done
U0=$(cut -d' ' -f1 /proc/uptime)
( while :; do echo "$(cut -d' ' -f1 /proc/uptime) $(cat /sys/class/thermal/thermal_zone0/temp) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) $(cat /sys/class/devfreq/*gpu*/cur_freq 2>/dev/null | head -1)"; sleep 5; done > $W/temps.txt ) & TP=$!
sleep ${SETTLE:-6}; M=$(grep -c "^\[kms\] 60 frames" /data/banana/logs/bananads/bananads.log)
python3 /data/bananads-dev/s3/grab.py $W/shots/start.png >/dev/null 2>&1
P=$(pidof bananads); tsnap() { for t in /proc/$P/task/*; do echo "$(cat $t/comm | tr ' ' _) $(awk '{print $14+$15}' $t/stat)"; done; }
tsnap > $W/t0; S0=$(cut -d' ' -f1 /proc/uptime)
if [ "${SHOTS:-0}" -gt 0 ]; then e=0; while [ $e -lt $S ]; do sleep $SHOTS; e=$((e+SHOTS)); pidof bananads >/dev/null || break; python3 /data/bananads-dev/s3/grab.py $W/shots/t$(awk "BEGIN{printf \"%03d\", $(cut -d' ' -f1 /proc/uptime)-$U0}").png 1 >/dev/null 2>&1; done
else sleep $S; fi
tsnap > $W/t1; S1=$(cut -d' ' -f1 /proc/uptime)
awk -v w=$(awk "BEGIN{print $S1-$S0}") 'NR==FNR{a[$1]+=$2; next} {b[$1]+=$2} END{for (k in b) if (b[k]-a[k]>0) printf "%s %.1f%%\n", k, (b[k]-a[k])/w; }' $W/t0 $W/t1 | sort -k2 -rn > $W/threads.txt
python3 /data/bananads-dev/s3/grab.py $W/shots/end.png >/dev/null 2>&1
T1=$(($(cat /sys/class/thermal/thermal_zone0/temp)/1000))
kill -TERM $(pidof bananads)
i=0; while { pidof bananads >/dev/null || [ -s /run/banana/game.pid ]; } && [ $i -lt 150 ]; do sleep 0.2; i=$((i+1)); done
sleep 1; cp /data/banana/logs/bananads/bananads.log $W/bananads.log
echo "$M" > $W/mark; echo "temp ${T0}-${T1}C" >> $W/run.txt
sh $D/mksum.sh $T
