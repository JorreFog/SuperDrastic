#!/bin/sh
# pace.sh <tag> <library | installed> <game> <secs> [NAME=value ...]     (ON the RG DS Plus, with ES running)
# A DS game through the real path (ES's API -> start_drastic.sh -> session.sh: the player's power profile, shader,
# CPU placement and governor), walking, and what libdsflip logged about its pacing. No perf, no frame read-backs.
# The library under test is put in the installed one's place for the run and the installed one put back after it.
# game: hg | b2 (from the test savestates). NAME=value: environment for the game (systemctl set-environment).
O=/storage/dsflip/opt/runs; D=/storage/.config/drastic/dsflip; KEEP=/storage/dsflip/opt/lib/installed.so
TAG=${1:?tag}; LIB=${2:?library}; GAME=${3:?game}; SECS=${4:-60}; shift 4
systemctl is-active -q dsflip-game && { echo "pace: a game session is running: not starting"; exit 3; }
curl -s localhost:1234/isIdle 2>/dev/null | grep -q true || { echo "pace: ES is not idle"; exit 4; }
SS=/storage/roms/savestates/nds
case "$GAME" in
  hg) ROM="/storage/roms/nds/Pokemon - HeartGold Version (USA).nds"; RES=/storage/dsflip/hgtest/hg-master.dss ;;
  b2) ROM="/storage/roms/nds/Pokemon Black Version 2 (DSi Enhanced).nds"; RES="$SS/Pokemon Black Version 2 (DSi Enhanced)_0.dss" ;;
  *) echo "pace: game?"; exit 2 ;;
esac
DSV="${ROM%.*}.dsv"; [ -f "$DSV" ] && cp "$DSV" /tmp/pace.dsv
NAMES=; for a in "$@"; do NAMES="$NAMES ${a%%=*}"; done
if [ "$LIB" != installed ]; then
    [ -f $KEEP ] || cp $D/libdsflip.so $KEEP
    cat "$LIB" > $D/libdsflip.so
fi
cleanup() {
    killall -9 drastic 2>/dev/null
    [ -n "$WPID" ] && kill $WPID 2>/dev/null
    i=0; while systemctl is-active -q dsflip-game && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$LIB" != installed ] && cat $KEEP > $D/libdsflip.so
    systemctl unset-environment DSFLIP_RESUME_FILE DSFLIP_RESUME_LOAD $NAMES
    rm -f /tmp/rocknixds-testing
    [ -f /tmp/pace.dsv ] && { cp /tmp/pace.dsv "$DSV"; rm -f /tmp/pace.dsv; }
}
trap cleanup EXIT
touch /tmp/rocknixds-testing
cp "$RES" /tmp/pace-run.dss
systemctl set-environment DSFLIP_RESUME_FILE=/tmp/pace-run.dss DSFLIP_RESUME_LOAD=1 "$@"
curl -s -X POST --data-binary "$ROM" localhost:1234/launch >/dev/null
i=0; while ! systemctl is-active -q dsflip-game && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
systemctl is-active -q dsflip-game || { echo "pace: the game did not start"; exit 1; }
sleep 14                                 # the start, the state load, the governor and the pinner settling
P=$(pidof drastic | tr ' ' '\n' | head -n1)
[ -n "$P" ] || { echo "pace: no DraStic"; exit 1; }
python3 /storage/dsflip/opt/walk.py ${PACE_HOLD:-1.2} ${PACE_WALK:-square} & WPID=$!
sleep 2
LOGF=$D/dsflip.log
n0=$(grep -c "present/s" $LOGF)
snap() { for t in /proc/$P/task/[0-9]*; do echo "${t##*/} $(awk '{print $14 + $15, $39}' $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)"; done; }
snap > /tmp/pace.th0; t0=$(date +%s%N)
sleep $SECS
snap > /tmp/pace.th1; t1=$(date +%s%N)
n1=$(grep -c "present/s" $LOGF)
kill $WPID 2>/dev/null; WPID=
cp $LOGF $O/$TAG.log; cp $D/last-session.log $O/$TAG.session 2>/dev/null
W=$(grep "present/s" $O/$TAG.log | sed -n "$((n0 + 1)),${n1}p")
echo "$W" > $O/$TAG.sec
FPS=$(echo "$W" | sed 's/.*present\/s=\([0-9.]*\).*/\1/' | awk '{s+=$1;n++} END {if(n) printf "%.2f", s/n; else print 0}')
echo "$W" | awk -v tag=$TAG -v fps=$FPS '{ for (i = 1; i <= NF; i++) { split($i, a, "="); if (a[1] == "dropped") d += a[2]; if (a[1] == "repeat") { r += a[2]; if (a[2] > 0) rs++ }
        if (a[1] == "top" && $(i-1) == "max-iv") { if (a[2] > mx) mx = a[2]; if (a[2] > 25000) late++ } } n++ }
    END { printf "%s secs=%d fps=%s repeats=%d (%.2f/s, in %d of the seconds) dropped=%d longest-interval=%d us\n", tag, n, fps, r, n ? r / n : 0, rs, d, mx }'
awk -v ns=$((t1 - t0)) -v fps=$FPS 'NR==FNR {a[$1]=$2; next} { d = ($2 - a[$1]) * 10; fr = fps * ns / 1e9;
      if (d > 0) printf "%6.2f ms/frame %5.1f%% cpu%s %s %s\n", (fr > 0 ? d / fr : 0), d / (ns / 1e8), $3, $1, $4 }' /tmp/pace.th0 /tmp/pace.th1 | sort -rn > $O/$TAG.threads
head -n 8 $O/$TAG.threads
grep -E "^\[dsflip\] libdsflip|\[shader\]|power profile|\[direct\]|\[cpugov\]|\[pace\]|\[audio\] (pump|the device)" $O/$TAG.log | awk '!seen[substr($0,1,14)]++ || /cpugov|pace\]/' | tail -n 9 | cut -c1-230
