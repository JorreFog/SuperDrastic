#!/usr/bin/env python3
"""fnreport.py <fnprof.txt> <drastic binary> <load base hex> [top]: instructions executed per DraStic function
(from the fnprof QEMU plugin's per-block counts), per emulated frame (frames = calls of update_frame_3d_*)."""
import bisect, collections, subprocess, sys

prof, exe, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
syms = []   # (start, end, name) of functions and data objects (the JIT code cache is a data object)
for l in subprocess.run(["llvm-nm", "-S", "--defined-only", "--no-sort", exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2].lower() in ("t", "w", "b", "d"):
        a, s = int(p[0], 16), int(p[1], 16)
        syms.append((a, a + s, p[3]))
    elif len(p) == 3 and p[1].lower() in ("t", "w"):           # assembler labels: no size
        syms.append((int(p[0], 16), 0, p[2]))
syms.sort()
# a symbol without a size (hand-written assembly) runs to the next symbol
syms = [(a, e if e > a else (syms[i + 1][0] if i + 1 < len(syms) else a + 4), n) for i, (a, e, n) in enumerate(syms)]
starts = [s[0] for s in syms]
tot = collections.Counter(); calls = collections.Counter(); grand = 0
for l in open(prof):
    pc, n, e = l.split(); pc = int(pc, 16); n = int(n); e = int(e)
    off = pc - base; grand += n * e
    i = bisect.bisect_right(starts, off) - 1
    name = syms[i][2] if i >= 0 and off < syms[i][1] else ("[outside drastic]" if not 0 <= off < 0x2000000 else "[drastic, unnamed]")
    tot[name] += n * e
    if i >= 0 and off == syms[i][0]: calls[name] += e
frames = max(calls.get("update_frame_3d_4x", 0) + calls.get("update_frame_3d_1x", 0), 1)
print(f"{grand / 1e9:.2f} G instructions, {frames} emulated frames, {grand / frames / 1e6:.2f} M instructions a frame")
print(f"{'function':48} {'M/frame':>8} {'share':>6} {'calls/frame':>11}")
for name, n in tot.most_common(top):
    print(f"{name[:48]:48} {n / frames / 1e6:8.3f} {100 * n / grand:5.1f}% {calls.get(name, 0) / frames:11.1f}")
