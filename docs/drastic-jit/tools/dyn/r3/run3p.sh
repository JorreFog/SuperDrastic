#!/bin/sh
# run3p.sh <rom name> [secs]: like dyn2's run3.sh, but three modes: int (--interpreter), jit (--recompiler, no JITFIX),
# fix (--recompiler, JITFIX=${FIXMASK:-1}). Prints DONE/crash plus the result words (or the partial phase/results).
D=$(cd "$(dirname "$0")/.." && pwd); n=$1; t=${2:-25}
cp "$D/smc/out/$n.nds" "$D/drastic/roms/"
for m in int jit fix; do
  f=--recompiler; [ $m = int ] && f=--interpreter
  O=$D/o3_${n}_$m; rm -rf "$O"; mkdir -p "$O"
  if [ $m = fix ]; then JF=${FIXMASK:-1}; else JF=; fi
  JITFIX=$JF OUT=$O JITDUMP_LOG= JITDUMP_PARTIAL=80 "$D/run.sh" "drastic/roms/$n.nds" $t $f > "$O/rc.txt" 2>&1
  if [ -s "$O/result.txt" ]; then st=DONE; r=$(tr '\n' ' ' < "$O/result.txt");
  else st="NO-DONE($(cat $O/rc.txt|tr -d '\n'))"; r="phase=$(sed -n 3p $O/result.txt.partial 2>/dev/null) res: $(sed -n '65,72p' $O/result.txt.partial 2>/dev/null | tr '\n' ' ')"; fi
  sig=$(grep -a -m1 -i 'signal\|sigill\|sigsegv\|crash\|segmentation' "$O/drastic.log" | cut -c1-80)
  echo "$n $m: $st | $r | $sig"
done
