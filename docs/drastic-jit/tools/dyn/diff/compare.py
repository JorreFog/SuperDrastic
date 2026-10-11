#!/usr/bin/env python3
"""compare.py tests.txt int/result.txt jit/result.txt: per stored word, expected (ARM ARM model in build.py),
interpreter and recompiler values; prints mismatches and a summary per instruction class."""
import sys, collections
t = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])]
a = [l.strip() for l in open(sys.argv[2])]; b = [l.strip() for l in open(sys.argv[3])]
stats = collections.defaultdict(lambda: [0, 0, 0, 0])   # n, int!=exp, jit!=exp, int!=jit
for (desc, exp), i, j in zip(t, a, b):
    cls = desc.split()[0] + ("" if not desc.split()[0] == "movs" else " " + desc.split()[1].split(",")[2])
    s = stats[cls]; s[0] += 1
    bad = []
    if exp != "-" and i != exp: s[1] += 1; bad.append("INT")
    if exp != "-" and j != exp: s[2] += 1; bad.append("JIT")
    if i != j: s[3] += 1
    if bad or i != j: print("%-75s exp=%s int=%s jit=%s  %s" % (desc, exp, i, j, "/".join(bad) + "!=exp"))
print("\nclass                 words  int!=exp  jit!=exp  int!=jit")
for k, (n, x, y, z) in sorted(stats.items()): print("%-20s %6d %9d %9d %9d" % (k, n, x, y, z))
