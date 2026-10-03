#!/bin/sh
# run.sh <rom> <secs> <RAST mode> [extra env]: DraStic in the simulator with librast preloaded
S=/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad
ROM=$1; SECS=${2:-60}; M=${3:-diff}; shift 3
cd $S/dsrun && cp $S/re/drastic.cfg.base config/drastic.cfg
DISPLAY=:99 SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy timeout -s KILL $SECS $S/qemu-build/qemu-aarch64 "$@" -L $S/rtsys -E RAST=$M -E LD_PRELOAD=$S/neon/simshim.so:$S/rast/librast.so ./drastic "$ROM"
