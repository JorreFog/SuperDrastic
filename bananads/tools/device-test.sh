#!/bin/sh
# BananaDS on a handheld: correctness on real hardware, then milestone B's benchmark (docs/PLAN.md).
# Runs on the RG DS / RG DS Plus (ROCKNIX), from the unpacked bananads-device folder:
#
#   sh device-test.sh [--quick] [game.nds ...]
#
# With no games given, it takes up to three .nds files from /storage/roms/nds. It touches no saves: every run
# uses /tmp/bananads-test for saves and states. It sets the CPU clock for the benchmark and puts the
# governor and limits back on exit, Ctrl-C included. The report goes to /storage/bananads-results-<date>.md
# with the raw log beside it. Exit code: 0 when every correctness check passed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/bin
ROMS=$HERE/roms
RUN=${BANANADS_RUN:-}   # a wrapper for the binaries (the PC dry run uses qemu-aarch64)
QUICK=0
[ "${1:-}" = "--quick" ] && { QUICK=1; shift; }
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${BANANADS_OUT:-/storage}
REPORT=$OUT/bananads-results-$STAMP.md
LOG=$OUT/bananads-results-$STAMP.log
WORK=/tmp/bananads-test
rm -rf "$WORK"; mkdir -p "$WORK"
FAILS=0
CPU=/sys/devices/system/cpu/cpufreq/policy0

say() { echo "$*"; echo "$*" >> "$LOG"; }
md() { echo "$*" >> "$REPORT"; }
temp() {  # the hottest thermal zone, in degrees C
    t=0
    for z in /sys/class/thermal/thermal_zone*/temp; do
        [ -r "$z" ] || continue
        v=$(cat "$z" 2>/dev/null || echo 0)
        [ "$v" -gt "$t" ] 2>/dev/null && t=$v
    done
    echo $((t / 1000))
}
mhz() { [ -r $CPU/scaling_cur_freq ] && echo $(( $(cat $CPU/scaling_cur_freq) / 1000 )) || echo "?"; }
COMMON="--frontend headless --set save.dir=$WORK --set state.dir=$WORK --set resume.on_start=off --set resume.on_quit=off --set wifi.dns=off"

# --- the clock: saved now, put back on exit ---
SAVED_GOV=""; SAVED_MIN=""; SAVED_MAX=""
if [ -w $CPU/scaling_governor ]; then
    SAVED_GOV=$(cat $CPU/scaling_governor); SAVED_MIN=$(cat $CPU/scaling_min_freq); SAVED_MAX=$(cat $CPU/scaling_max_freq)
fi
restore() {
    if [ -n "$SAVED_GOV" ]; then
        echo "$SAVED_MIN" > $CPU/scaling_min_freq 2>/dev/null
        echo "$SAVED_MAX" > $CPU/scaling_max_freq 2>/dev/null
        echo "$SAVED_MIN" > $CPU/scaling_min_freq 2>/dev/null
        echo "$SAVED_GOV" > $CPU/scaling_governor 2>/dev/null
        echo "clock put back: $SAVED_GOV, $SAVED_MIN-$SAVED_MAX kHz"
    fi
}
trap restore EXIT
trap 'exit 130' INT TERM
setclock() {  # setclock <kHz>: fixed at that frequency (performance governor capped by max)
    [ -n "$SAVED_GOV" ] || return 0
    echo performance > $CPU/scaling_governor 2>/dev/null
    echo "$1" > $CPU/scaling_max_freq 2>/dev/null
    echo "$1" > $CPU/scaling_min_freq 2>/dev/null
    sleep 1
}
pickfreq() {  # the highest available frequency <= $1 kHz (or the highest of all for 0)
    best=0
    for f in $(cat $CPU/scaling_available_frequencies 2>/dev/null); do
        if [ "$1" = 0 ] || [ "$f" -le "$1" ]; then [ "$f" -gt "$best" ] && best=$f; fi
    done
    echo $best
}

# the games for section 2 (before anything reuses the positional parameters)
GAMES=""
if [ $# -gt 0 ]; then
    for g in "$@"; do GAMES="$GAMES
$g"; done
else
    GAMES=$(find /storage/roms/nds -maxdepth 2 -iname '*.nds' 2>/dev/null | sort | head -n 3)
fi

# --- the system ---
: > "$LOG"
md "# BananaDS on a handheld: $STAMP"
md ""
md "BananaDS $(cat "$HERE/VERSION" 2>/dev/null || echo '?'), from \`device-test.sh\`$( [ $QUICK = 1 ] && echo ' --quick')."
md ""
md "| | |"
md "|---|---|"
md "| Device | $(cat /sys/firmware/devicetree/base/model 2>/dev/null | tr -d '\0' || echo '?') |"
md "| Kernel | $(uname -r) |"
md "| OS | $(grep -h '^PRETTY_NAME\|^VERSION=' /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '"' | tr '\n' ' ') |"
md "| CPU frequencies (kHz) | $(cat $CPU/scaling_available_frequencies 2>/dev/null || echo '?') |"
md "| Governor before | ${SAVED_GOV:-? (not writable: clocks not set)} ${SAVED_MIN:+($SAVED_MIN-$SAVED_MAX kHz)} |"
md "| Memory | $(grep MemTotal /proc/meminfo | tr -s ' ' | cut -d' ' -f2-) total, $(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2-) available |"
md "| Temperature at start | $(temp) °C |"
md "| Running | $(for p in drastic bananads emulationstation sway retroarch; do pidof $p >/dev/null 2>&1 && printf '%s ' $p; done) |"
md ""
if pidof drastic >/dev/null 2>&1 || pidof bananads-bananajit >/dev/null 2>&1; then
    say "A game is running: quit it first."; exit 2
fi
for b in bananads-bananajit bananads-melonjit bananads-difftest-bananajit; do
    if ! $RUN "$BIN/$b" --version >> "$LOG" 2>&1 && [ "$b" != bananads-difftest-bananajit ]; then
        say "$b doesn't start (see $LOG)"; md "**$b doesn't start**"; exit 2
    fi
done

FMAX=$(pickfreq 0); F1104=$(pickfreq 1104000)
[ "$FMAX" = 0 ] && FMAX=""; [ "$F1104" = 0 ] && F1104=""
setclock "${FMAX:-0}"

# --- 1. correctness on real hardware ---
md "## 1. Correctness (BananaJIT against the interpreter, on the A55s)"
md ""
md "| Check | Result |"
md "|---|---|"
check() {  # check <name> <command...>
    name=$1; shift
    say "== $name"
    if "$@" >> "$LOG" 2>&1; then md "| $name | PASS |"; say "PASS"
    else md "| $name | **FAIL** |"; say "FAIL"; FAILS=$((FAILS + 1)); fi
}
pictures() {  # pictures <rom>: frames 150/300/450/600 equal with jit=on and jit=off
    for j in on off; do
        rm -rf "$WORK/p_$j"; mkdir -p "$WORK/p_$j"
        $RUN "$BIN/bananads-bananajit" $COMMON --set jit=$j --set headless.frames=600 \
            --set headless.dump_dir="$WORK/p_$j" --set headless.dump_every=150 "$1" || return 1
    done
    for f in 150 300 450 600; do cmp "$WORK/p_on/frame$f.ppm" "$WORK/p_off/frame$f.ppm" || return 1; done
}
gate() {  # gate <switch> <rom>: main RAM identical after every frame with the switch off and on
    $RUN "$BIN/bananads-difftest-bananajit" --every --frames 120 --a jit=on --a "jit.$1=off" \
        --b jit=on --b "jit.$1=on" "$2" | grep -q "^SAME"
}
diffrom() {  # diffrom <binary> <jit on|off> <rom> <tests.txt>: every result as the ARMv5TE says
    n=$(wc -l < "$4"); len=$((n * 4))
    $RUN "$BIN/$1" --frames 60 --region 0x02200000:8 --dump 0x02200000:$((len + 256)) \
        --a "jit=$2" --b "jit=$2" "$3" | grep "run A" > "$WORK/dump.txt" || return 1
    python3 "$HERE/tools/check_results.py" "$4" "$WORK/dump.txt"
}
for v in arm thumb arm7 arm7-thumb; do check "pictures, stress L5 ($v)" pictures "$ROMS/stress/$v/dsstress-L5.nds"; done
for g in fastloads faststores link mul ldm; do check "exact: jit.$g off = on (L5, Thumb)" gate $g "$ROMS/stress/thumb/dsstress-L5.nds"; done
check "exact: jit.fastmem7 off = on (ARM7 workload)" gate fastmem7 "$ROMS/stress/arm7-thumb/dsstress-L5.nds"
check "exact: jit.mul off = on (ARM7 workload)" gate mul "$ROMS/stress/arm7-thumb/dsstress-L5.nds"
check "exact: jit.ldm off = on (ARM7 workload)" gate ldm "$ROMS/stress/arm7-thumb/dsstress-L5.nds"
if command -v python3 >/dev/null 2>&1 && [ -f "$ROMS/diff/diff.nds" ]; then
    for d in "diff.nds tests.txt" "diff_extra.nds tests_extra.txt"; do
        set -- $d
        check "$1: interpreter = ARMv5TE" diffrom bananads-difftest-bananajit off "$ROMS/diff/$1" "$ROMS/diff/$2"
        check "$1: BananaJIT = ARMv5TE" diffrom bananads-difftest-bananajit on "$ROMS/diff/$1" "$ROMS/diff/$2"
        check "$1: melonDS JIT = ARMv5TE" diffrom bananads-difftest-melonjit on "$ROMS/diff/$1" "$ROMS/diff/$2"
    done
else
    md "| differential ROMs | skipped (no python3 or no ROMs in the kit) |"
fi
md ""

# --- 2. speed ---
SF=1200; GF=3600; COOL=15
[ $QUICK = 1 ] && { SF=300; GF=600; COOL=3; }
md "## 2. Speed (headless: frames as fast as the CPUs go; 100% = a DS's 59.83 fps)"
md ""
md "\`headless\` runs flat out with no display or sound, so the percentage is headroom: above 100%, that"
md "configuration can hold full speed with the rest to spare. 3D is melonDS's software renderer at 1x"
md "(DraStic's bar on these handhelds is 60 fps at 2x, through SuperDrastic)."
md ""
md "| ROM | Clock | Interpreter | melonDS JIT | BananaJIT | °C after |"
md "|---|---|---|---|---|---|"
bench() {  # bench <binary> <jit> <frames> <rom> -> "fps (pct%)"
    r=$($RUN "$BIN/$1" $COMMON --set jit=$2 --set headless.frames=$3 "$4" 2>&1 | tee -a "$LOG" | grep "\[headless\]")
    f=$(echo "$r" | sed -n 's/.*: \([0-9.]*\) fps (\([0-9]*\)% of a DS).*/\1 fps (\2%)/p')
    echo "${f:-failed}"
}
row() {  # row <label> <rom> <frames>
    for clk in max 1104; do
        if [ $clk = max ]; then k=$FMAX; else k=$F1104; fi
        [ -z "$k" ] && [ $clk = 1104 ] && continue
        [ -n "$k" ] && setclock "$k"
        say "== speed: $1 at $clk ($(mhz) MHz)"
        a=$(bench bananads-bananajit off "$3" "$2"); sleep $COOL
        b=$(bench bananads-melonjit on "$3" "$2"); sleep $COOL
        c=$(bench bananads-bananajit on "$3" "$2"); t=$(temp); sleep $COOL
        md "| $1 | $(mhz) MHz | $a | $b | $c | $t |"
    done
}
row "stress L1 (light 3D)" "$ROMS/stress/arm/dsstress-L1.nds" $SF
row "stress L5" "$ROMS/stress/arm/dsstress-L5.nds" $SF
row "stress L10 (heaviest 3D)" "$ROMS/stress/arm/dsstress-L10.nds" $SF
row "stress L5, ARM7 busy (Thumb)" "$ROMS/stress/arm7-thumb/dsstress-L5.nds" $SF
echo "$GAMES" | while IFS= read -r g; do
    [ -n "$g" ] || continue
    row "$(basename "$g" .nds)" "$g" $GF
done
setclock "${FMAX:-0}"
md ""
md "### The 3D thread (one ROM, BananaJIT, full clock)"
md ""
md "| render.threaded | fps |"
md "|---|---|"
for t in on off; do
    r=$($RUN "$BIN/bananads-bananajit" $COMMON --set render.threaded=$t --set headless.frames=$SF \
        "$ROMS/stress/arm/dsstress-L10.nds" 2>&1 | tee -a "$LOG" | grep "\[headless\]" | sed -n 's/.*: \([0-9.]*\) fps.*/\1/p')
    md "| $t | ${r:-failed} |"
    sleep $COOL
done
md ""
md "Temperature at the end: $(temp) °C. Raw log: \`$(basename "$LOG")\`."
md ""
md "**Correctness: $FAILS check(s) failed.**"
say "report: $REPORT"
say "log:    $LOG"
[ $FAILS = 0 ]
