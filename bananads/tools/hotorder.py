#!/usr/bin/env python3
# hotorder.py <out.ld> <unstripped bananads> <prof.raw>... : a GNU ld --section-ordering-file (CMake BANANADS_TEXT_ORDER)
# with the functions that prof.raw sampled in the bananads module, hottest first (samples summed over the profiles,
# each profile weighted to the same total). Build the binary for the profiles with BANANADS_TEXT_ORDER= (off) or any
# order: names, not addresses, go into the file. Only placement changes; the code is the same.
import subprocess, sys, bisect, collections, os
out, binp, raws = sys.argv[1], sys.argv[2], sys.argv[3:]
NM = os.environ.get('NM', os.path.expanduser('~/BananaLinux/output/rgds-plus/host/bin/aarch64-linux-nm'))
syms = []
for l in subprocess.run([NM, '-S', '--defined-only', binp], capture_output=True, text=True).stdout.split('\n'):
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW' and int(p[1], 16) > 0:
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
syms.sort()
# one name per address: an alias (.localalias) shares its function's section, which is named after the function
byaddr = {}
for a, sz, n in syms:
    byaddr.setdefault(a, []).append((sz, n))
starts = sorted(byaddr)
weight = collections.Counter()
for raw in raws:
    lo = None; c = collections.Counter(); tot = 0
    for l in open(raw):
        if l.startswith('# module') and l.split()[-1] == 'bananads': lo = int(l.split()[2], 16)
        if l.startswith('#'): continue
        a, n, m = l.split()[:3]
        if m != 'bananads': continue
        off = int(a, 16) - lo; i = bisect.bisect_right(starts, off) - 1
        if i < 0: continue
        sz, _ = byaddr[starts[i]][0]
        if off < starts[i] + sz: c[starts[i]] += int(n); tot += int(n)
    for a, n in c.items(): weight[a] += n / tot
names = []
for a, w in weight.most_common():
    ns = [n for _, n in byaddr[a] if not n.endswith('.localalias')] or [n[:-len('.localalias')] for _, n in byaddr[a]]
    for n in ns:
        n = n.replace('.localalias', '')
        if n not in names: names.append(n)
with open(out, 'w') as f:
    f.write('/* made by tools/hotorder.py from %s: the hot functions first */\n.text : {\n' % ' '.join(os.path.basename(r) for r in raws))
    for n in names:
        f.write('  *(.text.%s)\n' % n)
    f.write('}\n')
print(len(names), 'functions')
