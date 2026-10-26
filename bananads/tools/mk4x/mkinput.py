#!/usr/bin/env python3
# mkinput.py <out> [frames]: the bench's kms.input for /data/mk4x/race.mst (Figure-8 Circuit, frame 1350 of a cold
# boot, before the countdown): A held from GO (frame 300: a clean start) to the end + the steering segments below.
# Segments: (from, to, key, on, period): key held `on` frames of every `period` in [from, to).
import sys
SEG = [(620, 820, 'left', 4, 30)]  # (v1: through the start; the kart ends at the first curve's wall)
out = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 12000
l = [f'300:a:{n}']
for a, b, k, on, per in SEG:
    for f in range(a, b, per): l.append(f'{f}:{k}:{min(on, b - f)}')
open(out, 'w').write('\n'.join(l) + '\n')
