#!/bin/sh
S=${SIM:-/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad}
R=${R:-/home/user/SuperDrastic/src/rast}
clang --target=aarch64-linux-gnu --sysroot=$S/rtsys -fuse-ld=lld ${OPT:--O2 -mtune=cortex-a55} -Wall -Wno-unused-function -shared -fPIC -I$R -o ${OUT:-$S/rast/librast.so} $R/rast.c $R/walk.c $R/b0.c $R/fused.c $R/fused_neon.c $R/fused_asm.c $R/defer.c $R/hr.c $R/rast_kern.S $R/spec/depth.c $R/spec/texture.c $R/spec/shade.c $R/spec/blend.c $R/spec/resolve.c $R/spec/edges.c $R/comp.c $R/spec/composite.c $EXTRA -lpthread -lm
