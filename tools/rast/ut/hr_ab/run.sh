#!/bin/sh
# run.sh [iterations] [seed]: A/B of the 3x pipeline's resolve and downsample (src/rast/hr.c) against an earlier
# version of the file. 3x has no DraStic reference, so a change to these stages must give the same output frame as
# before: the base version (BASE, a git revision, default 450370b) and the worktree's hr.c are compiled side by side
# (ab_tu.c prefixes each one's exported names, its static stage functions get wrappers), and ab_main.c runs both on
# the same random inputs: the downsample alone, the fog alone, and hr_resolve_bin + hr_downsample on random bin
# contexts (fog off/full/alpha-only, edge marking on/off) and on polygon-like ones (rectangles of one id: edge_lines'
# id screen), comparing the output blocks; and the polygon walker. A plain aarch64 program
# under qemu-aarch64 (no DraStic needed), seconds a run. Env: SIM (simulator dir with rtsys/ and qemu-build/), BASE.
HERE=$(cd "$(dirname "$0")" && pwd)
SIM=${SIM:-/tmp/claude-0/-home-user/b019d7e4-f3cd-580d-8132-fb9570fcadd2/scratchpad/sim}
R=$(cd "$HERE/../../../../src/rast" && pwd)
O=$(mktemp -d)
git -C "$R" show "${BASE:-450370b}:src/rast/hr.c" > "$O/hr_base.c" || exit 2
# the base's headers beside it (its quoted includes look in its own directory first): without them the base side took
# the worktree's spec/edges_impl.h, so a change to the NEON walker ran on both sides
git -C "$R" ls-tree -r --name-only "${BASE:-450370b}" src/rast | grep '\.h$' | while read -r f; do
    mkdir -p "$O/$(dirname "${f#src/rast/}")"; git -C "$R" show "${BASE:-450370b}:$f" > "$O/${f#src/rast/}"
done
CC="clang --target=aarch64-linux-gnu --sysroot=$SIM/rtsys -O2 -mtune=cortex-a55 -Wall -Wno-unused-function -I$R"
$CC -c -o "$O/old.o" -DTAG=old_ -DHRFILE="\"$O/hr_base.c\"" "$HERE/ab_tu.c" || exit 2
$CC -c -o "$O/new.o" -DTAG=new_ -DHRFILE="\"${NEWFILE:-$R/hr.c}\"" "$HERE/ab_tu.c" || exit 2
$CC -fuse-ld=lld -o "$O/ab" "$HERE/ab_main.c" "$O/old.o" "$O/new.o" "$HERE/ab_stubs.c" -lm || exit 2
# a base whose walker does not stop a chain at its window (hr.c without EDGES_LINE_CAP): the polygons with such chains
# are counted apart (ab_main.c, chain_overruns)
grep -q EDGES_LINE_CAP "$O/hr_base.c" || export AB_UNCAPPED=1
$SIM/qemu-build/qemu-aarch64 -L $SIM/rtsys "$O/ab" "$@"
