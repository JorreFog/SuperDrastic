#!/usr/bin/env python3
# mktrace.py <trace.txt> [bananads.log]: per-frame timeline from kms.lowlat_trace (+ [gengis-stats] N=1 lines):
# which stage makes 4x frames late. emu = start->pub (the emulator's frame incl. its Gengis help + 2D fence waits),
# gap = pub->commit (presenter: 2D fence + shader), scan = commit->flip; late = a refresh with no new frame before it.
import sys, statistics as st
rows = [list(map(int, l.split())) for l in open(sys.argv[1]) if l[0] != '#']
rows = [r for r in rows if r[1] and r[2]]
gs = []
if len(sys.argv) > 2:
    for l in open(sys.argv[2], errors='replace'):
        if l.startswith('[gengis-stats]'):
            try: gs.append((float(l.split('bins ')[1].split()[0]), float(l.split('convert ')[1].split()[0]), float(l.split('emu wait ')[1].split()[0])))
            except Exception: pass
emu = [(r[2] - r[1]) / 1000 for r in rows]
per = [(rows[i + 1][1] - rows[i][1]) / 1000 for i in range(len(rows) - 1)]
gap = [(r[6] - r[2]) / 1000 for r in rows if r[6]]
scan = [(r[7] - r[6]) / 1000 for r in rows if r[7] and r[6]]
flips = [r[7] for r in rows if r[7]]
fi = [(flips[i + 1] - flips[i]) / 1000 for i in range(len(flips) - 1)]
def q(v): v = sorted(v); return f"avg {st.mean(v):.2f} p50 {v[len(v)//2]:.2f} p90 {v[int(len(v)*.9)]:.2f} p99 {v[int(len(v)*.99)]:.2f} max {v[-1]:.2f}"
print(f"frames {len(rows)}")
print("emu start->pub ms:", q(emu)); print("start->next start ms:", q(per)); print("pub->commit ms:", q(gap)); print("commit->flip ms:", q(scan))
late = [i for i, x in enumerate(fi) if x > 24]
print(f"flip gaps > 24 ms (a refresh repeated): {len(late)} of {len(fi)}")
# for the late flips: what was slow in the frame shown after the gap
fl = [r for r in rows if r[7]]
ce = cg = 0
for i in late:
    r = fl[i + 1]; e = (r[2] - r[1]) / 1000; g = (r[6] - r[2]) / 1000
    if e > 16.6: ce += 1
    if g > 12: cg += 1
print(f"late frames whose emu part > 16.6 ms: {ce}; whose pub->commit > 12 ms: {cg}")
if gs:
    print("gengis bins CPU ms/frame:", q([g[0] for g in gs])); print("gengis convert ms:", q([g[1] for g in gs])); print("emu wait for 3D ms:", q([g[2] for g in gs]))
