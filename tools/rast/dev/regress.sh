#!/bin/sh
# regress.sh [lib.so]: the rasterizer's regression in the simulator (SIM: the simulator dir, see tools/sim/):
#   2x exactness: per-polygon diff on stress L10 and the scenes (0 lines), frame diff on the scenes at its 7200- and
#   14400-bin checkpoints (47 and 662 known bins, with and without deferred shading), per-polygon diff on each single scene
#   S3-S9 (dsscenes-cycle.nds sometimes dies in the simulator at frame 720, the scene 2 -> 3 transition, a SIGILL in
#   DraStic's JIT cache that also happens with RAST=off, mostly under load: then the cycle runs cover scenes 0-2 only);
#   3x stability: S7, S4, S2 and L4 keep rendering.
# One simulator job at a time (each 0.5-3 minutes, about 20 minutes in all). Prints one line per check with PASS/FAIL.
SIM=${SIM:-/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad}
HERE=$(cd "$(dirname "$0")" && pwd)
ROMS=${ROMS:-/home/user/ROCKNIXDS/stressrom/out}
export LIB=${1:-$SIM/rast/librast.so}
L=$(mktemp -d)
fail=0
check() { if [ "$2" = "$3" ]; then echo "PASS $1 ($2)"; else echo "FAIL $1 (got '$2', want '$3')"; fail=1; fi; }
sh $HERE/run.sh $ROMS/dsstress-L10.nds 120 ours -E RAST_PDIFF=1 > $L/pd-l10.log 2>&1
check "pdiff L10" "$(grep -ac '\[pdiff\]' $L/pd-l10.log)" 0
sh $HERE/run.sh $ROMS/dsscenes-cycle.nds 150 ours -E RAST_PDIFF=1 > $L/pd-cycle.log 2>&1
check "pdiff scenes" "$(grep -ac '\[pdiff\]' $L/pd-cycle.log)" 0
# the frame diff's counts at its checkpoints: 7200 bins (scenes 0-2: 47, the one-pixel translucent batches of the README) and,
# when the run gets that far (the cycle ROM sometimes dies at its scene 2 -> 3 transition in the simulator, and a starved run
# is slow), 14400 bins (one full cycle of the ten scenes: 662; scenes 5-8 add quirk pixels that the per-polygon check masks,
# state-dependent but deterministic, and the same count with the pre-rast4 kernels and with deferred shading)
cycle_counts() { c7=$(grep -a '\[rast\] diff: 7200 bins' $1 | grep -o '[0-9]* differ' | head -1)
                 c14=$(grep -a '\[rast\] diff: 14400 bins' $1 | grep -o '[0-9]* differ' | head -1)
                 echo "7200: ${c7:-not reached}${c14:+, 14400: $c14}"; }
cycle_want() { c14=$(grep -a '\[rast\] diff: 14400 bins' $1 | head -1); echo "7200: 47 differ${c14:+, 14400: 662 differ}"; }
sh $HERE/run.sh $ROMS/dsscenes-cycle.nds 150 diff > $L/dd-cycle.log 2>&1
check "diff scenes" "$(cycle_counts $L/dd-cycle.log)" "$(cycle_want $L/dd-cycle.log)"
sh $HERE/run.sh $ROMS/dsscenes-cycle.nds 150 diff -E RAST_DEFER=1 > $L/dd-cycle-d.log 2>&1
check "diff scenes deferred" "$(cycle_counts $L/dd-cycle-d.log)" "$(cycle_want $L/dd-cycle-d.log)"
for n in 3 4 5 6 7 8 9; do
    sh $HERE/run.sh $ROMS/dsscenes-S$n.nds 30 ours -E RAST_PDIFF=1 -E RAST_FRAMES=1 > $L/pd-s$n.log 2>&1
    f=$(grep -a '\[rast\] frames' $L/pd-s$n.log | tail -1 | grep -o '[0-9]*$')
    check "pdiff S$n (${f:-0} frames)" "$(grep -ac '\[pdiff\]' $L/pd-s$n.log)$([ "${f:-0}" -ge 100 ] || echo ' (starved: under 100 frames)')" 0
done
for r in dsscenes-S7 dsscenes-S4 dsscenes-S2 dsstress-L4; do
    sh $HERE/run.sh $ROMS/$r.nds 40 ours -E RAST_SCALE=3 -E RAST_FRAMES=1 > $L/3x-$r.log 2>&1
    n=$(grep -a '\[rast\] frames' $L/3x-$r.log | tail -1 | grep -o '[0-9]*$'); s=$(grep -ac '\[sim\] signal' $L/3x-$r.log)
    if [ "${n:-0}" -ge 100 ] && [ "$s" = 0 ]; then echo "PASS 3x $r ($n frames)"; else echo "FAIL 3x $r (${n:-0} frames, $s faults)"; fail=1; fi
done
echo "logs in $L"
exit $fail
