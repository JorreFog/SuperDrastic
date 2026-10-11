#!/usr/bin/env python3
"""rawdiff.py A/jit_raw.txt B/jit_raw.txt: compare the first translation of every guest block between two runs;
print each guest instruction whose emitted host words differ
(--ignore-b: compare B/BL by target: DraStic VA for stubs, else 'outside'; host offsets may shift) (and a count of identical blocks)."""
import sys
import os, re
def getbase(p):
    for l in open(os.path.join(os.path.dirname(p), "drastic.log"), errors="replace"):
        m = re.search(r"drastic base (0x[0-9a-f]+)", l)
        if m: return int(m.group(1), 16)
def load(p):
    blocks = {}; cur = None; base = getbase(p)
    for l in open(p):
        t = l.split()
        if not t: continue
        if t[0] == 'B':
            key = (t[1], t[2], t[3]); cur = None
            if key not in blocks: cur = blocks[key] = {'g': [], 'h': [], 'addr': int(t[5], 16), 'base': base}
        elif t[0] == 'G' and cur is not None: cur['g'].append((t[1], t[2], int(t[3])))
        elif t[0] == 'H' and cur is not None: cur['h'] = [int(x, 16) for x in t[1:]]
    return blocks
NOISE = '--ignore-b' in sys.argv
a, b = load(sys.argv[1]), load(sys.argv[2])
same = diff = 0; ops = {}
for k in sorted(set(a) & set(b)):
    A, B = a[k], b[k]
    if A['h'] == B['h']: same += 1; continue
    diff += 1
    g = A['g']
    for i, (pc, op, off) in enumerate(g):
        end = g[i + 1][2] if i + 1 < len(g) else len(A['h']) * 4
        bo = B['g'][i][2] if i < len(B['g']) else -1
        bend = B['g'][i + 1][2] if i + 1 < len(B['g']) else len(B['h']) * 4
        wa, wb = A['h'][off // 4:end // 4], B['h'][bo // 4:bend // 4]
        def norm(ws, blk, o):
            # B/BL: replace imm26 by the target as binary VA (stubs) or 'outside' (cache/other maps, layout-dependent)
            r = []
            for j, w in enumerate(ws):
                if (w >> 26) in (5, 0x25):
                    imm = w & 0x3ffffff; imm -= (1 << 26) if imm & (1 << 25) else 0
                    tgt = blk['addr'] + o + 4 * j + 4 * imm - blk['base']
                    r.append((w >> 26, tgt if 0 <= tgt < 0x150000 else 'x'))
                else: r.append(w)
            return r
        if (norm(wa, A, off) != norm(wb, B, bo) if NOISE else wa != wb):
            print("%s %s pc=%s op=%s\n   A: %s\n   B: %s" % (k[0], k[1], pc, op, ' '.join('%08x' % x for x in wa), ' '.join('%08x' % x for x in wb)))
            ops[op] = ops.get(op, 0) + 1
print("blocks common=%d identical=%d different=%d only_A=%d only_B=%d" % (len(set(a) & set(b)), same, diff, len(set(a) - set(b)), len(set(b) - set(a))))
print("differing guest opcodes:", ' '.join('%s(x%d)' % kv for kv in sorted(ops.items())))
