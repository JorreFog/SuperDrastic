#!/bin/sh
# run.sh [iterations] [seed]: fused.c's batch_end (f_run's batch splitting, 8 lines a step) against the scalar loop it
# replaced, on random line counts (2x and 3x limits, lines without pixels, full lines, garbage past the last line). The
# function is cut out of src/rast/fused.c and compiled with main.c into a plain aarch64 program run under qemu-aarch64
# (no DraStic needed). Env: SIM (simulator dir with rtsys/ and qemu-build/). Prints PASS or the first mismatches.
HERE=$(cd "$(dirname "$0")" && pwd)
SIM=${SIM:-/tmp/claude-0/-home-user/b019d7e4-f3cd-580d-8132-fb9570fcadd2/scratchpad/sim}
R=$(cd "$HERE/../../../../src/rast" && pwd)
O=$(mktemp -d)
awk '/^static inline unsigned batch_end\(/{f=1} f{print} f&&/^}/{exit}' "$R/fused.c" > "$O/batch_end.h"
[ -s "$O/batch_end.h" ] || { echo "batch_end not found in fused.c"; exit 2; }
clang --target=aarch64-linux-gnu --sysroot=$SIM/rtsys -fuse-ld=lld -O2 -Wall -I"$O" -o "$O/t" "$HERE/main.c" || exit 2
$SIM/qemu-build/qemu-aarch64 -L $SIM/rtsys "$O/t" "$@"
