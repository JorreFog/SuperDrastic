#!/bin/sh
# on the handheld: quick.sh <bananads> <difftest>: a short correctness pass (block timing)
B=$1; DT=$2; K=/storage/bananads-device; R=$K/roms; W=/tmp/qc; FAILS=0
COMMON="--frontend headless --set save.dir=$W --set state.dir=$W --set resume.on_start=off --set resume.on_quit=off --set wifi.dns=off"
pictures() { rom=$1; rm -rf $W; mkdir -p $W/on $W/off
  $B $COMMON --set jit=on --set headless.frames=300 --set headless.dump_dir=$W/on --set headless.dump_every=150 $rom >/dev/null 2>&1 || return 1
  $B $COMMON --set jit=off --set headless.frames=300 --set headless.dump_dir=$W/off --set headless.dump_every=150 $rom >/dev/null 2>&1 || return 1
  for f in 150 300; do cmp -s $W/on/frame$f.ppm $W/off/frame$f.ppm || return 1; done; }
for v in arm/dsstress-L5 thumb/dsstress-L5 arm7-thumb/dsstress-L5; do if pictures $R/stress/$v.nds; then echo "PASS $v"; else echo "FAIL $v"; FAILS=$((FAILS+1)); fi; done
for d in "diff.nds tests.txt" "diff_extra.nds tests_extra.txt"; do set -- $d; n=$(wc -l < $R/diff/$2); mkdir -p $W
  if $DT --frames 60 --region 0x02200000:8 --dump 0x02200000:$((n * 4 + 256)) --a jit=on --b jit=on $R/diff/$1 | grep "run A" > $W/dump.txt && python3 $K/tools/check_results.py $R/diff/$2 $W/dump.txt >/dev/null; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS+1)); fi; done
echo "FAILS=$FAILS"
