#!/usr/bin/env python3
"""blocks.py jit_raw.txt [cpu]: per phase, list translated block PCs (with counts) and flush/ITCM events, in order."""
import sys, collections, re
cpu = sys.argv[2] if len(sys.argv) > 2 else None
cur = None; acc = collections.OrderedDict()
def dump():
    if cur is not None: print("ph=%-4s %s" % (cur, " ".join("%s%s" % (k, "x%d" % v if v > 1 else "") for k, v in acc.items())))
for l in open(sys.argv[1]):
    if l[0] not in "BFI": continue
    f = l.split(); ph = re.search(r"ph=(\d+)", l); ph = ph.group(1) if ph else "?"
    if ph != cur: dump(); cur = ph; acc = collections.OrderedDict()
    if l[0] == "B":
        if cpu and f[1] != cpu: continue
        k = ("T" if f[2] == "1" else "") + f[3] + "(%s)" % f[4] + ("/7" if f[1] == "0" else "")
    elif l[0] == "F": k = "[FLUSH cpu%s %s]" % (f[1], f[2])
    else: k = "[ITCMmod %s %s]" % (f[2], f[3])
    acc[k] = acc.get(k, 0) + 1
dump()
