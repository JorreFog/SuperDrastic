#!/bin/sh
# prof.sh <rom> <secs> <tag> [-E VAR=..]: DraStic + librast under qemu with the block profiler; then the per-function
# report of librast per frame (prof-<tag>.txt/.log/.rep)
S=${SIM:-/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad}
ROM=$1; SECS=$2; TAG=$3; shift 3
cd $S/dsrun && cp $S/re/drastic.cfg.base config/drastic.cfg
DISPLAY=:99 SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy timeout -s KILL $SECS $S/qemu-build/qemu-aarch64 -plugin $S/neon/fnprof.so,inline=on,out=$S/rast/prof-$TAG.txt "$@" -L $S/rtsys -E RAST=ours -E RAST_FRAMES=1 -E LD_PRELOAD=$S/neon/simshim.so:${LIB:-$S/rast/librast.so} ./drastic "$ROM" > $S/rast/prof-$TAG.log 2>&1
BASE=$(grep -o "librast base 0x[0-9a-f]*" $S/rast/prof-$TAG.log | head -1 | awk '{print $3}')
FRAMES=$(grep -o "\[rast\] frames [0-9]*" $S/rast/prof-$TAG.log | tail -1 | awk '{print $3}')
echo "$BASE $FRAMES" > $S/rast/prof-$TAG.meta
python3 $S/neon/libreport.py $S/rast/prof-$TAG.txt ${LIB:-$S/rast/librast.so} $BASE $FRAMES 25 > $S/rast/prof-$TAG.rep
cat $S/rast/prof-$TAG.rep
