#!/usr/bin/env python3
"""savdump.py <dsreal.sav> [other.sav]: the per-scene records (loop, scene, frames, picture crc, state crc, ARM7 hash,
ARM9 load %, lag max/total). With two saves: SAME/DIFF per record (the exactness check between two builds)."""
import struct, sys
NAMES = ["race", "action", "town", "ohg"]
def recs(path):
    d = open(path, "rb").read(); out = {}
    for i in range(32):
        m, ls, fr, cp, cs, a7, load, lag = struct.unpack_from("<8I", d, i * 32)
        if m != 0x43455252: continue
        out[(ls >> 8, ls & 0xFF)] = (fr, cp, cs, a7, load, lag)
    if not out:   # DraStic takes the EEPROM writes as FLASH (3-byte addresses): each record's first byte is lost
        i = d.find(b"REC")
        while i >= 0:
            m, ls, fr, cp, cs, a7, load, lag = struct.unpack("<8I", b"R" + d[i:i + 31])
            if 0 < fr <= 4000: out[(ls >> 8, ls & 0xFF)] = (fr, cp, cs, a7, load, lag)
            i = d.find(b"REC", i + 3)
    return out
a = recs(sys.argv[1]); b = recs(sys.argv[2]) if len(sys.argv) > 2 else None
for k in sorted(a):
    fr, cp, cs, a7, load, lag = a[k]
    line = "loop %d %-6s frames %d pic %08x state %08x arm7 %08x load %d%% lag max %d total %d" % (k[0], NAMES[k[1]] if k[1] < 4 else k[1], fr, cp, cs, a7, load, lag >> 16, lag & 0xFFFF)
    if b is not None:
        o = b.get(k)
        line += "  " + ("MISSING" if o is None else "SAME" if o[1:4] == (cp, cs, a7) else "DIFF(%s)" % ",".join(n for n, x, y in zip(("pic", "state", "arm7"), (cp, cs, a7), o[1:4]) if x != y))
    print(line)
