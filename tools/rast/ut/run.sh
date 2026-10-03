#!/bin/sh
# run.sh <test.c> [extra sources...]: builds a unit test (see ut.h) and runs it inside DraStic under qemu-aarch64.
# Env: SIM (simulator dir with rtsys/, qemu-build/, dsrun/), UT_SEED.
SIM=${SIM:-/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$1; shift
O=$(mktemp -d)/ut.so
clang --target=aarch64-linux-gnu --sysroot=$SIM/rtsys -fuse-ld=lld -O1 -g -Wall -Wno-unused-function -shared -fPIC \
    -I"$HERE" -I"$HERE/../../../src/rast" -o "$O" "$T" "$@" -lm || exit 2
cd $SIM/dsrun && timeout ${UT_TIMEOUT:-300} $SIM/qemu-build/qemu-aarch64 -L $SIM/rtsys -E UT_SEED=${UT_SEED:-1} -E LD_PRELOAD=$O ./drastic
