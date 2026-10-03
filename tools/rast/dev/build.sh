#!/bin/sh
S=/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad
R=/home/user/SuperDrastic/src/rast
clang --target=aarch64-linux-gnu --sysroot=$S/rtsys -fuse-ld=lld ${OPT:--O2} -Wall -Wno-unused-function -shared -fPIC -I$R -o $S/rast/librast.so $R/rast.c $R/b0.c $R/fused.c $R/fused_neon.c $R/spec/depth.c $R/spec/texture.c $R/spec/shade.c $R/spec/blend.c $R/spec/resolve.c $R/spec/edges.c $EXTRA -lpthread -lm
