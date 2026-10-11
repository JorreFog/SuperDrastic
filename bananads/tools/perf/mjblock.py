#!/usr/bin/env python3
# mjblock.py <code dump> <ips> <block start hex>...: a block's disassembly with each instruction's samples
import struct, sys, subprocess, collections, bisect
data = open(sys.argv[1], 'rb').read(); blocks = {}; i = 0
while i + 12 <= len(data):
    at, n = struct.unpack_from('<QI', data, i); i += 12; blocks[at] = data[i:i + n]; i += n
hist = collections.Counter(int(l.split()[0], 16) for l in open(sys.argv[2]) if l.split())
for a in sys.argv[3:]:
    s = int(a, 16); b = blocks[s]
    words = [struct.unpack_from('<I', b, k)[0] for k in range(0, len(b), 4)]
    txt = " ".join("0x%02x 0x%02x 0x%02x 0x%02x" % (w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) for w in words)
    out = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=txt, capture_output=True, text=True).stdout
    ins = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith('.')]
    print("== block %x (%d bytes, %d samples)" % (s, len(b), sum(hist[s + 4 * k] for k in range(len(words)))))
    for k, t in enumerate(ins): print("%4x %4s  %s" % (4 * k, hist[s + 4 * k] or '', t))
