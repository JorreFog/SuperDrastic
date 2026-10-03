#!/usr/bin/env python3
"""libreport.py <fnprof.txt> <lib.so> <load base hex> <frames> [top]: instructions per function of a shared library"""
import bisect, collections, subprocess, sys
prof, lib, base, frames = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4])
top = int(sys.argv[5]) if len(sys.argv) > 5 else 30
syms = []
for l in subprocess.run(["llvm-nm", "-S", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2].lower() in ("t", "w"): a, s = int(p[0], 16), int(p[1], 16); syms.append((a, a + s, p[3]))
syms.sort(); starts = [s[0] for s in syms]
tot = collections.Counter(); grand = 0
for l in open(prof):
    pc, n, e = l.split(); off = int(pc, 16) - base; c = int(n) * int(e)
    if not 0 <= off < 0x1000000: continue
    i = bisect.bisect_right(starts, off) - 1
    tot[syms[i][2] if i >= 0 and off < syms[i][1] else "?"] += c; grand += c
print(f"{grand / frames / 1e6:.2f} M/frame in {lib}")
for k, v in tot.most_common(top): print(f"{k[:50]:50} {v / frames / 1e6:8.3f}")
