#!/bin/sh
# run3.sh <rom name> [secs]: runs drastic/roms/<name>.nds with --recompiler and --interpreter, prints outcome and results
D=$(cd "$(dirname "$0")/.." && pwd); n=$1; t=${2:-25}
cp "$D/smc/out/$n.nds" "$D/drastic/roms/"
for m in jit int; do
  f=--recompiler; [ $m = int ] && f=--interpreter
  O=$D/o3_${n}_$m; rm -rf "$O"; mkdir -p "$O"
  OUT=$O JITDUMP_LOG= JITDUMP_PARTIAL=80 "$D/run.sh" "drastic/roms/$n.nds" $t $f > "$O/rc.txt" 2>&1
  if [ -s "$O/result.txt" ]; then st=DONE; r=$(tr '\n' ' ' < "$O/result.txt");
  else st="NO-DONE($(cat $O/rc.txt|tr -d '\n'))"; r="phase=$(sed -n 3p $O/result.txt.partial 2>/dev/null) res: $(sed -n '65,80p' $O/result.txt.partial 2>/dev/null | tr '\n' ' ')"; fi
  sig=$(grep -m1 -i 'signal\|sigill\|sigsegv\|crash\|segmentation' "$O/drastic.log" | cut -c1-100)
  echo "$n $m: $st | $r | $sig"
done
