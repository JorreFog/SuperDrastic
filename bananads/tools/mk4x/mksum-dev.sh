#!/bin/sh
# hgsum.sh <tag>...: per run (after the measure mark): 60-frame windows' fps, windows < 59, frames lost, repeats,
# drops, cpugov clocks, audio short chunks/underruns, temps. Lines from /data/mk4x/runs/<tag>/
for t in "$@"; do L=/data/mk4x/runs/$t/bananads.log; [ -f $L ] || { echo "$t: no log"; continue; }
  M=$(cat /data/mk4x/runs/$t/mark 2>/dev/null || echo 0); awk -v M=$M -v t=$t -v tmp="$(tail -1 /data/mk4x/runs/$t/run.txt)" '/^\[kms\] 60 frames shown/ {k++} /^\[kms\] 60 frames shown/ && k>M {v=$8; gsub(/[^0-9.]/,"",v); e=$9; gsub(/[^0-9.]/,"",e); es+=e; n++; sum+=v; f[n]=v; if (v<59) {bad++; lost+=60-v}
      for (i=1;i<=NF;i++) { if ($i=="refreshes" && $(i-1) ~ /^[0-9]+$/) rep+=$(i-1); if ($i=="dropped" && $(i-1) ~ /^[0-9]+$/) drp+=$(i-1) } }
    /^\[cpugov\] [0-9]+ -> / && m {clk=clk" "$4}
    /^\[alsa\] 10 s:/ && m {sc+=$4; al=al" |"$0}
    END {printf "%-14s %s windows %d avg %.2f fps (emulated %.1f/s), under 59: %d (lost ~%.0f), repeats %d, dropped %d, audio short %d, clocks:%s\n", t, tmp, n, (n?sum/n:0), (n?es/n:0), bad, lost, rep, drp, sc, clk; s=""; for(i=1;i<=n;i++) s=s sprintf("%.0f ", f[i]); print "  fps: " s}' $L; echo "  threads: $(head -6 /data/mk4x/runs/$t/threads.txt 2>/dev/null | tr '\n' ' ')"
done
