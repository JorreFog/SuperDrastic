#!/usr/bin/env python3
# ff-clicks.py <dump>.out.raw ...: the crackle when fast-forward ends (BananaLinux #159), from audio.dump's output
# (48 kHz s16 stereo). Finds the longest silent stretch (kms.ff_mute's fast-forward) and counts discontinuities
# (second difference over 1.5x the local 10 ms RMS) per second: 4 s before it, the first 0.3 s after it, 0.3-1 s and
# 1-4 s after. A clean run has the same rate (0-1/s, peak ~1.6) after as before. Needs numpy; run with python3 -I.
import sys
import numpy as np

SR, BLK = 48000, 240
for path in sys.argv[1:]:
    x = np.fromfile(path, dtype='<i2').astype(np.float64)[0::2]
    nb = len(x) // BLK
    quiet = np.sqrt((x[:nb * BLK].reshape(nb, BLK) ** 2).mean(1)) < 30
    best, s = (0, 0), None
    for i, q in enumerate(quiet):
        if q and s is None: s = i
        if (not q or i == nb - 1) and s is not None:
            if i - s > best[1] - best[0]: best = (s, i)
            s = None
    m0, m1 = best[0] * BLK, best[1] * BLK
    d2 = np.abs(x[2:] - 2 * x[1:-1] + x[:-2])
    loc = np.sqrt(np.convolve(x ** 2, np.ones(480) / 480, 'same'))[1:-1] + 50
    r = d2 / loc
    out = []
    for name, a, b in (('before', max(0, m0 - 4 * SR), m0 - SR // 2), ('after 0-0.3 s', m1, m1 + int(0.3 * SR)),
                       ('0.3-1 s', m1 + int(0.3 * SR), m1 + SR), ('1-4 s', m1 + SR, min(len(r), m1 + 4 * SR))):
        if b > a:
            seg = r[a:b]
            out.append('%s %.0f/s (peak %.1f)' % (name, (seg > 1.5).sum() / ((b - a) / SR), seg.max()))
    print('%s: silent %.2f-%.2f s | %s' % (path, m0 / SR, m1 / SR, ' | '.join(out)))
