#!/usr/bin/env python3
"""Checks a result-block test ROM's output against its expected values.

    check_results.py <tests.txt> <dump.txt>

tests.txt: one line per result word, "description<TAB>expected hex" (the format of the DraStic JIT study's
differential ROMs, SuperDrastic PR #10, docs/drastic-jit/tools/dyn/diff). dump.txt: `bananads-difftest --dump
0x02200000:<len>` output, starting at the block's base: 'DONE' at +0, the count at +4, results from +0x100.
Exit code 0 when DONE is set, the count matches and every word is as expected."""
import sys

tests = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])]
raw = open(sys.argv[2]).read().split(":", 1)[1].split()
b = bytes(int(x, 16) for x in raw)
done = b[0:4] == b"DONE"
count = int.from_bytes(b[4:8], "little")
bad = []
for i, t in enumerate(tests):
    v = int.from_bytes(b[0x100 + 4 * i:0x104 + 4 * i], "little")
    if len(t) > 1 and t[1] not in ("", "None") and int(t[1], 16) != v:
        bad.append("%s: expected %s, got %08x" % (t[0], t[1], v))
print("DONE=%s count=%d/%d, %d wrong" % (done, count, len(tests), len(bad)))
for x in bad[:20]:
    print("  " + x)
sys.exit(0 if done and count == len(tests) and not bad else 1)
