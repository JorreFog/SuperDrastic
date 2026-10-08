#!/usr/bin/env python3
"""cycles.py <fnprof.txt> <drastic.log> <librast.so> [top]: instructions and modeled Cortex-A55 cycles per frame, by
function, for DraStic and librast together (a run of prof.sh: the fnprof plugin's per-block counts, the log with the
load addresses). Env: SIM (the simulator dir: dsrun/drastic), BLOCKS=<function> NB=<n> also lists that function's n
costliest blocks.

Each executed basic block is disassembled and run through llvm-mca's in-order Cortex-A55 model (the handhelds' core)
once, from an empty pipeline; its cost is the cycle its last instruction issues, plus one. Branches count as an issue
slot. Stalls carried from one block into the next are not counted, so the totals are a lower bound, best for
comparing builds: instruction counts alone miss what an in-order core loses to dependent instructions. Frames:
calls of update_frame_3d_4x/1x. llvm-mca results are cached in $SIM/cyc-cache by block text."""
import bisect, collections, hashlib, os, pickle, re, subprocess, sys

prof, log, lib = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
SIM = os.environ.get("SIM", "/tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad")
CACHE = os.path.join(SIM, "cyc-cache")
os.makedirs(CACHE, exist_ok=True)
txt = open(log, errors="replace").read()
dbase = int(re.search(r"\[sim\] drastic base (0x[0-9a-f]+)", txt).group(1), 16)
m = re.search(r"librast base (0x[0-9a-f]+)", txt)
lbase = int(m.group(1), 16) if m else None

def disasm(path):
    h = hashlib.md5(open(path, "rb").read()).hexdigest()
    cache = os.path.join(CACHE, f"dis-{h}.pkl")
    if os.path.exists(cache): return pickle.load(open(cache, "rb"))
    out = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", path], capture_output=True, text=True).stdout
    d = {}
    for l in out.split("\n"):
        mm = re.match(r"\s*([0-9a-f]+):\s+(.*)", l)
        if mm: d[int(mm.group(1), 16)] = mm.group(2).split("//")[0].strip()
    pickle.dump(d, open(cache, "wb"))
    return d

def symbols(path):
    ss = []
    for l in subprocess.run(["llvm-nm", "-S", "--defined-only", "--no-sort", path], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 4 and p[2].lower() in ("t", "w", "b", "d"): a, s = int(p[0], 16), int(p[1], 16); ss.append((a, a + s, p[3]))
        elif len(p) == 3 and p[1].lower() in ("t", "w"): ss.append((int(p[0], 16), 0, p[2]))
    ss.sort()
    ss = [(a, e if e > a else (ss[i + 1][0] if i + 1 < len(ss) else a + 4), n) for i, (a, e, n) in enumerate(ss)]
    return ss, [s[0] for s in ss]

objs = [(dbase, os.path.join(SIM, "dsrun/drastic"), "")] + ([(lbase, lib, "R:")] if lbase is not None else [])
info = [(base, disasm(path), symbols(path), tag) for base, path, tag in objs]
BR = re.compile(r"^(b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|b\.\w+)\b")

def clean(ins):
    """an instruction as llvm-mca can take it on its own"""
    if BR.match(ins) or ins.startswith("prfm"): return "nop"
    if re.match(r"^(adrp|adr)\s", ins): return "mov " + ins.split()[1].rstrip(",") + ", #0"
    if re.match(r"^(ld(add|clr|eor|set|smax|smin|umax|umin)|swp|cas)\w*\s", ins):     # atomics (no A55 model): a load
        return re.sub(r"^\S+\s+\S+,\s*(\S+),", r"ldr \1,", ins)
    if re.match(r"^ldr\w*\s+\w+,\s*0x", ins): return re.sub(r",\s*0x.*", ", [sp]", ins)
    return re.sub(r"\s*<[^>]*>", "", ins)

blocks, frames = [], 0
for l in open(prof):
    pc, n, e = l.split(); pc = int(pc, 16); n = int(n); e = int(e)
    for base, dis, (ss, starts), tag in info:
        off = pc - base
        if 0 <= off < 0x4000000 and off in dis:
            i = bisect.bisect_right(starts, off) - 1
            name = ss[i][2] if i >= 0 and off < ss[i][1] else "?"
            if tag == "" and off == ss[i][0] and name in ("update_frame_3d_4x", "update_frame_3d_1x"): frames += e
            ins = tuple(clean(dis[off + 4 * k]) for k in range(n) if off + 4 * k in dis)
            blocks.append((tag + name, ins, n, e))
            break
    else: blocks.append(("[other]", (), n, e))
frames = max(frames, 1)

cpath = os.path.join(CACHE, "mca-cortex-a55.pkl")
memo = pickle.load(open(cpath, "rb")) if os.path.exists(cpath) else {}
todo = sorted({b[1] for b in blocks if b[1] and b[1] not in memo})
for c in range(0, len(todo), 400):
    chunk = todo[c:c + 400]
    src = []
    for j, ins in enumerate(chunk): src += [f"# LLVM-MCA-BEGIN b{j}", *ins, "# LLVM-MCA-END"]
    tmp = os.path.join(CACHE, "tmp.s")
    open(tmp, "w").write("\n".join(src) + "\n")
    out = subprocess.run(["llvm-mca", "-mtriple=aarch64", "-mcpu=cortex-a55", "-mattr=+lse", "-iterations=1", "-timeline",
                          "-timeline-max-cycles=100000", "-instruction-info=false", "-resource-pressure=false", tmp],
                         capture_output=True, text=True)
    if out.returncode: sys.stderr.write(out.stderr[:2000])
    parts = re.split(r"\n\[\d+\] Code Region - b(\d+)", out.stdout)
    got = {}
    for k in range(1, len(parts), 2):
        last = max([mm.group(1).find("D") for mm in re.finditer(r"\[0,\d+\]\s+(\S.*)", parts[k + 1])] or [-1])
        got[int(parts[k])] = last + 1
    for j, ins in enumerate(chunk): memo[ins] = got.get(j) or len(ins)
pickle.dump(memo, open(cpath, "wb"))

I, C = collections.Counter(), collections.Counter()
for name, ins, n, e in blocks:
    I[name] += n * e; C[name] += (memo.get(ins, n) if ins else n) * e
ri = sum(v for k, v in I.items() if k.startswith("R:")); rc = sum(v for k, v in C.items() if k.startswith("R:"))
print(f"{frames} frames; all: {sum(I.values()) / frames / 1e6:.2f} M instructions, {sum(C.values()) / frames / 1e6:.2f} M cycles "
      f"a frame; librast: {ri / frames / 1e6:.2f} M, {rc / frames / 1e6:.2f} M")
print(f"{'function (R: librast)':52} {'M instr':>8} {'M cyc':>8} {'IPC':>5}")
for k, v in C.most_common(top): print(f"{k[:52]:52} {I[k] / frames / 1e6:8.3f} {v / frames / 1e6:8.3f} {I[k] / v:5.2f}")
if os.environ.get("BLOCKS"):
    want, agg, where = os.environ["BLOCKS"], collections.Counter(), {}
    for name, ins, n, e in blocks:
        if name.endswith(want) and ins: agg[ins] += e; where[ins] = name
    for ins, e in sorted(agg.items(), key=lambda kv: -kv[1] * memo.get(kv[0], len(kv[0])))[:int(os.environ.get("NB", 3))]:
        c = memo.get(ins, len(ins))
        print(f"\n{where[ins]}: block of {len(ins)} instructions, {c} cycles, {e / frames:.0f} a frame, {c * e / frames / 1e6:.3f} M cycles")
        for i in ins: print("   ", i)
