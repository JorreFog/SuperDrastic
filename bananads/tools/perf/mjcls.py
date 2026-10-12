#!/usr/bin/env python3
# mjcls.py <code dump> <perf script "ip sym" lines>: melonDS ARMJIT_A64 code's samples by kind of instruction,
# the hottest instruction forms, and the hottest blocks (x29 = CPU, w28 = cycles, w27 = CPSR, x26 = fastmem base)
import struct, sys, subprocess, collections, bisect, re
data = open(sys.argv[1], 'rb').read()
blocks = {}; i = 0
while i + 12 <= len(data):
    at, n = struct.unpack_from('<QI', data, i); i += 12
    blocks[at] = data[i:i + n]; i += n
starts = sorted(blocks)
hist = collections.Counter(); outside = 0; names = {}
for line in open(sys.argv[2]):
    p = line.split()
    if not p: continue
    ip = int(p[0], 16)
    k = bisect.bisect_right(starts, ip) - 1
    if k >= 0 and ip < starts[k] + len(blocks[starts[k]]): hist[ip] += 1
    else: outside += 1
ips = sorted(hist)
def word(ip):
    s = starts[bisect.bisect_right(starts, ip) - 1]
    return struct.unpack_from('<I', blocks[s], ip - s)[0]
def dis(words):
    txt = " ".join("0x%02x 0x%02x 0x%02x 0x%02x" % (w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) for w in words)
    out = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=txt, capture_output=True, text=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith('.')]
ins = dis([word(ip) for ip in ips])
assert len(ins) == len(ips), (len(ins), len(ips))
def cls(t):
    if re.search(r'\[x26', t) or re.search(r'x26', t): return 'fast memory access'
    if re.search(r'NZCV|w27', t): return 'flags/CPSR'
    if re.search(r'w28', t): return 'cycles'
    if re.search(r'\[x29', t): return 'CPU state load/store (guest regs etc.)'
    if t.startswith('bl') or re.search(r'\bsp\b|\[sp', t): return 'calls (slow paths, helpers)'
    if re.search(r'^(b\.|b\t|cbz|cbnz|tbz|tbnz|br|ret)', t): return 'branches'
    if re.search(r'^(mov|movk|movz|orr)\tw0, #|^mov\tw0, #', t): return 'constants'
    return 'guest work (ALU, mul, etc.)'
c = collections.Counter(); forms = collections.Counter()
for ip, t in zip(ips, ins):
    c[cls(t)] += hist[ip]
    forms[re.sub(r'#-?0x[0-9a-f]+|#-?\d+', '#N', t)] += hist[ip]
tot = sum(c.values())
print("samples in JIT blocks: %d (outside the dump: %d)" % (tot, outside))
for k, v in c.most_common(): print("%6d  %5.1f%%  %s" % (v, 100 * v / tot, k))
print("\nhottest instruction forms:")
for t, n in forms.most_common(25): print("%6d  %5.1f%%  %s" % (n, 100 * n / tot, t))
bc = collections.Counter()
for ip in ips: bc[starts[bisect.bisect_right(starts, ip) - 1]] += hist[ip]
print("\nhottest blocks (start, size, samples, cumulative %):")
cum = 0
for s, n in bc.most_common(12):
    cum += n; print("  %x %5d bytes %6d  %5.1f%%" % (s, len(blocks[s]), n, 100 * cum / tot))
print("blocks with samples: %d; 50%% of samples in %d blocks, 90%% in %d" % (len(bc),
    next(i for i, x in enumerate(__import__('itertools').accumulate(n for _, n in bc.most_common())) if x >= tot * 0.5) + 1,
    next(i for i, x in enumerate(__import__('itertools').accumulate(n for _, n in bc.most_common())) if x >= tot * 0.9) + 1))
