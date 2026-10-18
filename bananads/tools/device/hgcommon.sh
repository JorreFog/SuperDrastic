# hgcommon.sh: sourced by hgquick.sh, hgprofile.sh and threads.sh (on the handheld). They must prove they measure
# HeartGold's overworld: on 2026-10-18 a run "made" 420 fps because the save wasn't found (BananaDS loads
# <save.dir>/<ROM name>.sav) and the game sat in its new-game tutorial, and walk.txt's "eft:" (for ":left:") never
# walked left. So each run here: checks the walk's keys, copies the save under the name BananaDS looks for, checks
# the log says it loaded it, dumps the picture of frame 1500 (just before the measured window) and compares its md5
# with the known one for that scale (rtc.fixed makes it the same every run), and prints fps sanity bounds.
#   KIT=/data/bananads-dev  ROM=$KIT/hg.nds  SAV=<ROM without .nds>.sav  WALK=$KIT/walk-lr.txt  CLK=<kHz>
#   HG_HASH_1X / HG_HASH_2X: frame 1500's md5 for render.scale 1 / 2. Unset: the run prints it and keeps the picture
#   ($KIT/hg-frame1500-<scale>.ppm): look at it once (the overworld, the player standing), then put the md5 here or
#   in the environment. SCENE_CHECK=off skips the comparison (for settings that change the picture on purpose).
HG_HASH_1X=${HG_HASH_1X:-}
HG_HASH_2X=${HG_HASH_2X:-}
KIT=${KIT:-/data/bananads-dev}
ROM=${ROM:-$KIT/hg.nds}
SAV=${SAV:-${ROM%.*}.sav}
WALK=${WALK:-$KIT/walk-lr.txt}
G=$(basename "$ROM"); G=${G%.*}
W=${WORK:-/tmp/hgb}           # saves and states (small); the picture goes to $KIT (/run is a small tmpfs)
DUMP=$KIT/hgdump
FROM=1500                     # the overworld, the player walking left and right from here on
CPU=${CPUFREQ:-/sys/devices/system/cpu/cpufreq/policy0}   # (CPUFREQ: a fake folder, for a test off the device)

die() { echo "FAIL: $*" >&2; exit 1; }
[ -f "$ROM" ] || die "no ROM at $ROM (ROM=...)"
[ -f "$SAV" ] || die "no save at $SAV (SAV=...): without it HeartGold sits in its new-game tutorial"
[ -f "$WALK" ] || die "no walk at $WALK (WALK=...)"
# every press is <frame>:<key>[+<key>...][:<frames held>] with known keys (catches walk.txt's old "1500eft:90")
for p in $(tr ',' ' ' < "$WALK"); do
  k=$(echo "$p" | cut -d: -f2)
  for k1 in $(echo "$k" | tr '+' ' '); do
    case $k1 in a|b|select|start|right|left|up|down|r|l|x|y) ;; *) die "$WALK: '$p' isn't <frame>:<keys>[:<n>]";; esac
  done
done

pin_clock() {  # pin_clock <kHz>: performance governor at that clock; put back on exit
  OG=$(cat $CPU/scaling_governor); OMN=$(cat $CPU/scaling_min_freq); OMX=$(cat $CPU/scaling_max_freq)
  trap 'echo $OMN > $CPU/scaling_min_freq; echo $OMX > $CPU/scaling_max_freq; echo $OMN > $CPU/scaling_min_freq; echo $OG > $CPU/scaling_governor' EXIT
  trap 'exit 130' INT TERM
  echo performance > $CPU/scaling_governor; echo $1 > $CPU/scaling_max_freq; echo $1 > $CPU/scaling_min_freq
  [ "$(cat $CPU/scaling_cur_freq)" = "$1" ] || echo "WARNING: the clock is $(cat $CPU/scaling_cur_freq), not $1" >&2
}
fresh() {  # a clean save folder with the save under the name BananaDS looks for
  rm -rf "$W" "$DUMP"; mkdir -p "$W" "$DUMP"; cp "$SAV" "$W/$G.sav"
}
# the settings every run shares (EXTRA and the scripts' own come after them)
COMMON="--frontend headless --set save.dir=$W --set state.dir=$W --set resume.on_start=off --set resume.on_quit=off
  --set wifi.dns=off --set rtc.fixed=2026-10-11_12:00:00 --set render.gengis_threads=2
  --set headless.dump_dir=$DUMP --set headless.dump_every=$FROM --set headless.input=$(cat "$WALK")"
scene() {  # scene <log>: "ok", or why the run didn't measure the overworld (status 1)
  grep -q "\[emu\] save $W/$G.sav" "$1" || { echo "the save wasn't loaded (no '[emu] save $W/$G.sav' in the log)"; return 1; }
  f=$DUMP/frame$FROM.ppm
  [ -f "$f" ] || { echo "no picture of frame $FROM (did the run reach it?)"; return 1; }
  case "$(head -c 16 "$f" | sed -n 2p)" in "512 768") s=2X;; *) s=1X;; esac
  h=$(md5sum < "$f" | cut -c1-32)
  eval want=\$HG_HASH_$s
  if [ "${SCENE_CHECK:-on}" = off ]; then echo "ok (scene not checked: SCENE_CHECK=off; frame $FROM $h)"; return 0; fi
  if [ -z "$want" ]; then
    cp "$f" "$KIT/hg-frame$FROM-$s.ppm"
    echo "UNCHECKED: HG_HASH_$s unset; frame $FROM is $h ($KIT/hg-frame$FROM-$s.ppm): look at it, then HG_HASH_$s=$h"
    return 0
  fi
  [ "$h" = "$want" ] || { echo "frame $FROM is $h, not HG_HASH_$s=$want: not the scene (save, walk, ROM, settings?)"; return 1; }
  echo "ok"
}
bounds() {  # bounds <fps>: a warning outside what HeartGold's overworld can do at this clock (FPS_LO, FPS_HI)
  c=${1:-0}; lo=${FPS_LO:-35}; hi=${FPS_HI:-$(( ${CLK:-1992000} / 10000 ))}   # (hi: 110 at 1104 MHz, 199 at 1992)
  awk -v f="$c" -v lo="$lo" -v hi="$hi" 'BEGIN { if (f < lo || f > hi) printf "  SUSPECT: %s fps outside [%s, %s]\n", f, lo, hi }'
}
