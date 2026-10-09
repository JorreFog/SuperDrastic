#!/bin/sh
# run.sh [iterations] [seed]: A/B of the generated pixel kernels (src/rast/rast_kern.S) against an earlier version of
# the file. A change to kerngen.py or kernsched.py that only moves work between registers and instructions must leave
# every kernel's output the same, the hi-res set's (3x: no DraStic reference) too: the base version's rast_kern.S
# (BASE, a git revision, default 94f9689; or BASEFILE) and the worktree's (or NEWFILE) are assembled side by side
# (each one's names prefixed) and ab_main.c calls both kernels of every nearest-filtering variant (2x and hi-res
# sets, the deferred passes included) on the same random batches: random spans, depths and attribute words (lines
# that all pass, all fail and mixed), textures, palettes, colours and runtime flags, comparing the context buffer
# (colours, attribute words, ids), the owner buffer, the first ids and the return value. A plain aarch64 program
# under qemu-aarch64 (no DraStic needed). Env: SIM (simulator dir with rtsys/ and qemu-build/), BASE.
HERE=$(cd "$(dirname "$0")" && pwd)
SIM=${SIM:-/tmp/claude-0/-home-user/b019d7e4-f3cd-580d-8132-fb9570fcadd2/scratchpad/sim}
R=$(cd "$HERE/../../../../src/rast" && pwd)
O=$(mktemp -d)
if [ -n "$BASEFILE" ]; then cp "$BASEFILE" "$O/base.S"; else git -C "$R" show "${BASE:-94f9689}:src/rast/rast_kern.S" > "$O/base.S" || exit 2; fi
sed 's/rast_kern_/old_rast_kern_/g' "$O/base.S" > "$O/old.S"
sed 's/rast_kern_/new_rast_kern_/g' "${NEWFILE:-$R/rast_kern.S}" > "$O/new.S"
# the kernels both files have, bilinear ones (B = 1) left out: X(name, hi-res, M (0 kernel, 1 visibility, 2 shade), D, T, R)
grep -o '^\.globl rast_kern_[a-z0-9]*' "$O/base.S" | sed 's/.globl rast_kern_//' | sort > "$O/a"
grep -o '^\.globl rast_kern_[a-z0-9]*' "${NEWFILE:-$R/rast_kern.S}" | sed 's/.globl rast_kern_//' | sort > "$O/b"
comm -12 "$O/a" "$O/b" | awk '{
    n = $0; h = 0; s = n; if (substr(s, 1, 1) == "h") { h = 1; s = substr(s, 2) }
    if (substr(s, 1, 1) == "v") printf "X(%s, %d, 1, %s, %s, 0)\n", n, h, substr(s, 2, 1), substr(s, 3, 1)
    else if (substr(s, 1, 1) == "s") { if (substr(s, 4, 1) == "0") printf "X(%s, %d, 2, 2, %s, 0)\n", n, h, substr(s, 2, 1) }
    else if (substr(s, 5, 1) == "0") printf "X(%s, %d, 0, %s, %s, %s)\n", n, h, substr(s, 1, 1), substr(s, 2, 1), substr(s, 3, 1)
}' > "$O/names.h"
CC="clang --target=aarch64-linux-gnu --sysroot=$SIM/rtsys -O1 -Wall"
$CC -c -o "$O/old.o" "$O/old.S" || exit 2
$CC -c -o "$O/new.o" "$O/new.S" || exit 2
$CC -fuse-ld=lld -I"$O" -o "$O/ab" "$HERE/ab_main.c" "$O/old.o" "$O/new.o" || exit 2
$SIM/qemu-build/qemu-aarch64 -L $SIM/rtsys "$O/ab" "$@"
