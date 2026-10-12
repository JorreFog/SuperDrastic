#!/usr/bin/env python3
"""report.py A.tsv A.map B.tsv B.map FRAMES [vcpu]: main-thread instructions per frame, (B - A) / FRAMES, by bucket.
JIT code = no ELF symbol and inside the span of the perf map's blocks. Prints buckets and top symbols / blocks."""
import sys, collections, bisect
def load(tsv, mp, vcpu):
    blocks = []
    for l in open(mp):
        a, n, name = l.split()
        blocks.append((int(a, 16), int(n, 16), name))
    blocks.sort()
    lo = min(b[0] for b in blocks) - 0x100000 if blocks else 0
    hi = max(b[0] + b[1] for b in blocks) + 0x100000 if blocks else 0
    starts = [b[0] for b in blocks]
    syms = collections.Counter(); jit = collections.Counter(); other = collections.Counter(); mem = collections.Counter()
    for l in open(tsv):
        c, kind, key, n, m, br = l.rstrip('\n').split('\t')
        if int(c) != vcpu: continue
        n = int(n); m = int(m)
        if kind == 'sym':
            syms[key] += n; mem['sym:' + key] += m
        else:
            a = int(key, 16)
            if lo <= a < hi:
                i = bisect.bisect_right(starts, a) - 1
                name = blocks[i][2] if i >= 0 and a < blocks[i][0] + blocks[i][1] else 'stub/unknown'
                name = name.rsplit('_', 1)[0] + ':' + name.rsplit('_', 1)[-1] if '_' in name else name
                jit[name] += n; mem['jit'] += m
            else:
                other['lib'] += n
    return syms, jit, other, mem
DISPATCH = ('Execute', 'LookUpBlock', 'JumpTo', 'UpdateMode', 'HaltInterrupted', 'SetupExecutableRegion',
            'ARM_Dispatch', 'ARM_Ret', 'CompileBlock', 'Compile', 'Reset', 'ARM_RestoreContext', 'melonds_jit')
def bucket(sym):
    s = sym
    if any(k in s for k in DISPATCH) and ('ARM' in s or 'melonds' in s or 'Compiler' in s or 'JIT' in s): return 'jit-runtime'
    if 'Interpreter' in s or s.startswith('_ZN7melonDS14ARMInterpreter'): return 'interpreter-fallback'
    if 'Slow' in s or 'ARM9Read' in s or 'ARM9Write' in s or 'ARM7Read' in s or 'ARM7Write' in s or 'IORead' in s or 'IOWrite' in s or 'DataRead' in s or 'DataWrite' in s or 'CodeRead' in s or 'BlockTransfer' in s: return 'memory'
    if 'GPU3D' in s or 'Renderer3D' in s: return '3d'
    if 'GPU2D' in s or 'Banana' in s or 'GPU' in s: return '2d/gpu'
    if 'SPU' in s: return 'audio'
    if 'RunFrame' in s or 'RunSystem' in s or 'RunTimers' in s or 'NDS' in s: return 'core'
    return 'other'
if __name__ == '__main__':
    a_tsv, a_map, b_tsv, b_map, frames = sys.argv[1:6]; frames = int(frames)
    vcpu = int(sys.argv[6]) if len(sys.argv) > 6 else 0
    A = load(a_tsv, a_map, vcpu); B = load(b_tsv, b_map, vcpu)
    def d(i): 
        c = collections.Counter(B[i]); c.subtract(A[i]); return c
    syms, jit, other = d(0), d(1), d(2)
    buckets = collections.Counter()
    for s, n in syms.items(): buckets[bucket(s)] += n
    buckets['jit-code'] = sum(jit.values()); buckets['libs'] = sum(other.values())
    total = sum(buckets.values())
    print(f"main thread: {total/frames/1e3:.1f}k insns/frame")
    for k, n in buckets.most_common(): print(f"  {k:22s} {n/frames/1e3:9.1f}k  {100*n/total:5.1f}%")
    jr = buckets['jit-code'] + buckets['jit-runtime'] + buckets['interpreter-fallback']
    print(f"  JIT total (code+runtime+fallback): {jr/frames/1e3:.1f}k/frame")
    print("top symbols:")
    for s, n in syms.most_common(25): print(f"  {n/frames/1e3:9.1f}k  [{bucket(s)}] {s[:110]}")
    print("top JIT blocks:")
    for s, n in jit.most_common(12): print(f"  {n/frames/1e3:9.1f}k  {s}")
