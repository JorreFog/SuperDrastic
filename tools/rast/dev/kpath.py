#!/usr/bin/env python3
"""kpath.py <rast_kern.S> <kernel> [bit=0|1 ...]: a kernel's group loop for full groups that all pass (from its 0:
label back to `b 0b`), the runtime flag tests (tbz/tbnz on w7) resolved by the given flag bits (default: perspective,
not depth-equal, not white, fast vertex colour, A = 31, no alpha test, the fast w-depth products; bit 15 follows bits 8
and 10 unless given), through llvm-mca's Cortex-A55 model as a loop of 200 iterations: cycles per group of 8 pixels.
Branches count as an issue slot. Compare KERNSCHED=0 and scheduled kernels, or two generators."""
import re, subprocess, sys, tempfile

f, name = sys.argv[1], sys.argv[2]
bits = {0: 0, 1: 0, 2: 0, 8: 1, 10: 1, 13: 1, 14: 1, 11: 0, 12: 0, 7: 0}
for a in sys.argv[3:]:
    k, v = a.split("="); bits[int(k)] = int(v)
bits.setdefault(15, bits[8] & bits[10])     # the kernels' bit 15: bits 8 and 10 (kerngen.py's fused_alpha())
lines, on = [], False
for l in open(f):
    l = l.rstrip("\n")
    if l == name + ":": on = True; continue
    if on and l.startswith(".size"): break
    if on: lines.append(l)
labels = {}
for i, l in enumerate(lines):
    if re.match(r"^\w+:$", l): labels.setdefault(l[:-1], []).append(i)

def target(i, lab):
    n, d = lab[:-1], lab[-1]
    return min(j for j in labels[n] if j > i) if d == "f" else max(j for j in labels[n] if j < i)

i, path = labels["0"][0] + 1, []
for _ in range(4000):
    t = lines[i].strip()
    if not t or t.endswith(":"): i += 1; continue
    mn, _, ops = t.partition(" ")
    o = [x.strip() for x in ops.split(",")]
    take = None
    if mn in ("tbz", "tbnz") and o[0] == "w7": take = (bits.get(int(o[1][1:]), 0) == 1) == (mn == "tbnz")
    elif mn in ("tbz", "tbnz"): take = mn == "tbz"
    elif mn == "b.hs": take = True          # a full group (the tail test)
    elif mn == "cbz": take = False          # some pixel passes
    elif mn == "b.ne": take = False         # all pass: the straight store
    elif mn == "b.ls": take = False         # more groups follow
    elif mn == "b": take = True
    if take is None: path.append(t); i += 1; continue
    if mn == "b" and o[-1] == "0b": break
    path.append("nop")
    i = target(i, o[-1]) if take else i + 1
with tempfile.NamedTemporaryFile("w", suffix=".s") as tf:
    tf.write("\n".join(path) + "\n"); tf.flush()
    out = subprocess.run(["llvm-mca", "-mtriple=aarch64", "-mcpu=cortex-a55", "-iterations=200", tf.name], capture_output=True, text=True).stdout
cyc = int(re.search(r"Total Cycles:\s+(\d+)", out).group(1)) / 200
print(f"{name}: {len(path)} instructions ({path.count('nop')} branches), {cyc:.1f} cycles a group, IPC {len(path) / cyc:.2f}")
