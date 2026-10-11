#!/usr/bin/env python3
"""jitdis.py <out dir> [--pc HEX,...] [--all] [--cpu 9|7]
Merges jitdump.so's jit_raw.txt with LLVM disassembly: guest ARMv5TE/Thumb (llvm-mc) and emitted AArch64 (llvm-mc),
with host branch targets resolved to DraStic symbols (base from drastic.log, names from $S/bin/syms.txt) or to
other translated blocks. Default: first translation of every distinct guest PC."""
import bisect, os, re, subprocess, sys

S = os.environ.get("RE_WORK", os.path.expanduser("~/drastic-re"))   # needs $RE_WORK/bin/drastic and bin/syms.txt
out = sys.argv[1]
args = sys.argv[2:]
want = None; allx = "--all" in args; cpuf = None
if "--pc" in args: want = {int(x, 16) for x in args[args.index("--pc") + 1].split(",")}
if "--cpu" in args: cpuf = {"9": 1, "7": 0}[args[args.index("--cpu") + 1]]

base = None
for l in open(os.path.join(out, "drastic.log"), errors="replace"):
    m = re.search(r"drastic base (0x[0-9a-f]+)", l)
    if m: base = int(m.group(1), 16); break
syms = []
for l in open(os.path.join(S, "bin/syms.txt")):
    p = l.split()
    if len(p) == 4 and p[2] in "tTdDbB": syms.append((int(p[0], 16), p[3]))
    elif len(p) == 3 and p[1] in "tT": syms.append((int(p[0], 16), p[2]))
syms.sort(); saddr = [a for a, _ in syms]
def symname(va):
    i = bisect.bisect_right(saddr, va) - 1
    if i < 0: return None
    a, n = syms[i]
    return n if va == a else "%s+%#x" % (n, va - a)

blocks = []; cur = None
for l in open(os.path.join(out, "jit_raw.txt")):
    p = l.split()
    if not p: continue
    if p[0] == "B":
        cur = dict(cpu=int(p[1]), thumb=int(p[2]), pc=int(p[3], 16), n=int(p[4]), host=int(p[5], 16),
                   nbytes=int(p[6]), g=[], h=[]); blocks.append(cur)
    elif p[0] == "G":
        cur["g"].append((int(p[1], 16), int(p[2], 16)))
        cur.setdefault("off", []).append(int(p[3]) if len(p) > 3 else None)
    elif p[0] == "H": cur["h"] = [int(x, 16) for x in p[1:]]
hostmap = {}
for b in blocks: hostmap.setdefault(b["host"], b["pc"])

def mc(triple, words, size):
    def run(ws):
        txt = " ".join(" ".join("0x%02x" % ((w >> (8 * k)) & 0xff) for k in range(size)) for w in ws)
        r = subprocess.run(["llvm-mc", "--disassemble", "-triple=" + triple], input=txt, capture_output=True, text=True)
        return [x.strip().replace("\t", " ") for x in r.stdout.splitlines() if x.strip() and x.strip() != ".text"]
    res = run(words)
    if len(res) == len(words): return res
    return [(run([w]) or ["<undefined>"])[0] for w in words]

def thumb_words(g):
    # BL/BLX pairs are two halfwords; disassemble pairs together (as one 32-bit little-endian unit: hw0 | hw1<<16)
    # guest ARM uses the armv6 triple: llvm-mc's armv5te rejects MUL/MLA/UMULL with Rd==Rm-style v5 constraints
    out = []; i = 0
    while i < len(g):
        a, w = g[i]
        if (w >> 11) == 0x1e and i + 1 < len(g):
            out.append((a, w | (g[i + 1][1] << 16), 4)); i += 2
        else:
            out.append((a, w, 2)); i += 1
    return out

seen = set()
for b in blocks:
    if cpuf is not None and b["cpu"] != cpuf: continue
    if want is not None and b["pc"] not in want: continue
    key = (b["cpu"], b["pc"])
    if not allx and key in seen: continue
    seen.add(key)
    print("=" * 100)
    print("%s %s block at %08x: %d guest insns -> %d host bytes, host code at %#x" % (
        "ARM9" if b["cpu"] == 1 else "ARM7", "Thumb" if b["thumb"] else "ARM", b["pc"], b["n"], b["nbytes"], b["host"]))
    print("-- guest")
    if b["thumb"]:
        tw = thumb_words(b["g"])
        dis = []
        for a, w, sz in tw:
            dis.append(mc("thumbv5te", [w & 0xffff], 2) if sz == 2 else (mc("thumbv5te", [w], 4)))
        for (a, w, sz), d in zip(tw, dis):
            print("  %08x: %-9s %s" % (a, ("%04x" % w) if sz == 2 else ("%04x %04x" % (w & 0xffff, w >> 16)), " ; ".join(d) if len(d) > 1 else d[0]))
    else:
        for (a, w), d in zip(b["g"], mc("armv6", [w for _, w in b["g"]], 4)):
            print("  %08x: %08x  %s" % (a, w, d))
    print("-- host (AArch64)" + ("; [guest pc] marks where each guest instruction's code starts" if b.get("off") else ""))
    starts = {}
    for (ga, gw), o in zip(b["g"], b.get("off", [])):
        if o is not None: starts.setdefault(o, []).append(ga)
    for i, (w, d) in enumerate(zip(b["h"], mc("aarch64", b["h"], 4))):
        for ga in starts.get(4 * i, []): print("        [%08x]" % ga)
        here = b["host"] + 4 * i
        m = re.search(r"#(-?\d+)$", d)
        note = ""
        if m and re.match(r"(b|bl|b\.\w+|cbn?z|tbn?z)\s", d):
            tgt = here + int(m.group(1))
            if b["host"] <= tgt < b["host"] + b["nbytes"]: note = "-> +%#x" % (tgt - b["host"])
            elif base and base <= tgt < base + 0x400000: note = "-> " + str(symname(tgt - base))
            elif tgt in hostmap: note = "-> block %08x" % hostmap[tgt]
            else: note = "-> %#x (outside this block: stub/other code)" % tgt
        print("  +%03x %08x  %-40s %s" % (4 * i, w, d, note))
