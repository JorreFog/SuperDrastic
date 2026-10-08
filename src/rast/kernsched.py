"""kernsched.py: instruction scheduling of kerngen.py's kernels for an in-order dual-issue core (Cortex-A55).

kerngen.py writes each kernel stage by stage, a chain of dependent instructions at a time (the vertex colour's r,
then g, then b; the texture coordinate's u, then v). An out-of-order core overlaps such chains by itself; the
handhelds' Cortex-A55 issues in program order, two instructions a cycle at best, and an instruction whose operand is
not ready yet stalls everything behind it. schedule() reorders the instructions of each basic block (the straight
code between labels, directives and branches) so that independent chains interleave and a result's consumers come
as late as its latency asks, without changing what the block computes:

  - every register is tracked as written and read per instruction (w and x name the same register; b h s d q and v
    the same vector register; NZCV is one more register), including the instructions that also read their
    destination (accumulates, bit selects, element inserts, the narrowing "2" forms, immediate orr/bic, bfi, movk);
  - a read after a write waits the writer's latency; a write after a read or a write keeps the program order;
  - memory: loads may pass loads; a store keeps its order with every load and store that may touch the same bytes.
    REGIONS (set by kerngen.py) names what the kernels' pointer registers address: read-only memory (the texture,
    the palette, the kernel arguments, the span entries, the constant tables: never stored to, so their loads pass
    any store), distinct buffers (the colour, attribute, owner and id lines and the pass masks: accesses through
    different ones never overlap) and the stack (sp, and x28 = sp + 32: accesses at known offsets that do not overlap
    are independent); any other base register may address anything;
  - the block's branch stays its last instruction, and anything that is not understood (or that moves sp) ends a
    block, so it keeps its place.
Every order of the block that keeps those dependences computes the same; schedule_block() makes several and keeps
the one a machine model runs fastest. The model (Machine, machine()) is llvm-mca's Cortex-A55 model as its in-order
issue runs it (llvm-mca -mcpu=cortex-a55; it reproduces llvm-mca's cycles on the kernels' group loops within a cycle):
  - in program order, at most two micro-ops a cycle (ldp, and a load or store that writes its base back: two);
  - a result is ready after its latency (3 for integer ALU work, LLVM's figures; an ldp's first register after 4);
  - units, each busy for some cycles: two ALUs, the integer multiplier, two FP/NEON pipes, the load pipe, the store
    pipe. A 128-bit NEON operation, and a widening one (umull, uaddl, ushll, addhn...), holds one FP pipe for two
    cycles and must open its issue group (be the first of its cycle); shifts, narrows, dup, tbl, conversions, fmov
    and the by-element fmul hold one for a cycle at any width. ldp q holds the load pipe 6 cycles, ld2 4, ldp d/x 2;
  - results are written back in program order (stores excepted): an instruction whose result would be written
    before an earlier one's waits, so a load or a multiply delays the short operations behind it, and a branch
    (an issue slot) waits for the block's last results to be written.
The candidates: the cycle-driven list schedule of earlier versions (each cycle two instructions, one load or store,
two 64-bit NEON halves and one NEON multiply; the longest latency path first), the order written, and list
schedules on the model under several rankings of the instructions it could issue next (the soonest to issue, the
longest latency path, the soonest result, the most successors, in combinations). The machine state carries over
from the block before on the fall-through path, and the winner is the one whose branch issues soonest; ties go to
the one that gets through the next block (as written) soonest, then to the order of the list above. Identical
blocks in the same state (the 2x and 3x kernel sets) are scheduled once. The result is checked by running the
kernels: RAST=diff and the per-polygon diff compare every pixel with DraStic's (tools/rast/dev/regress.sh)."""
import re

BRANCH = {"b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz"}
STORE = {"str", "stp", "strb", "strh", "stur", "st1", "st2", "st3", "st4"}
LOAD = {"ldr", "ldp", "ldrb", "ldrh", "ldrsw", "ldrsh", "ldrsb", "ldur", "ld1", "ld1r", "ld2", "ld3", "ld4"}
FLAGS_ONLY = {"cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp"}
SET_FLAGS = {"adds", "subs", "ands", "bics", "negs"}
READ_FLAGS = {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg", "adc", "sbc", "ccmp", "ccmn",
              "fcsel"}
RMW = {"mla", "mls", "umlal", "umlal2", "smlal", "smlal2", "umlsl", "umlsl2", "smlsl", "smlsl2", "sqdmlal", "sqdmlal2",
       "fmla", "fmls", "bit", "bif", "bsl", "sli", "sri", "ssra", "usra", "srsra", "ursra", "tbx", "bfi", "bfxil",
       "movk", "ins", "uadalp", "sadalp", "saba", "uaba", "sabal", "uabal", "sabal2", "uabal2",
       "xtn2", "shrn2", "rshrn2", "sqxtn2", "uqxtn2", "sqxtun2", "addhn2", "raddhn2", "subhn2", "rsubhn2", "sqshrn2",
       "uqshrn2", "sqrshrn2", "uqrshrn2", "sqshrun2", "sqrshrun2", "fcvtn2", "fcvtxn2"}
# everything else: the first operand is written, the others read
KNOWN = BRANCH | STORE | LOAD | FLAGS_ONLY | SET_FLAGS | READ_FLAGS | RMW | {
    "add", "sub", "and", "orr", "eor", "bic", "orn", "eon", "mov", "mvn", "movi", "mvni", "neg", "mul", "madd", "msub",
    "lsl", "lsr", "asr", "ror", "sbfiz", "ubfiz", "sbfx", "ubfx", "sxtw", "sxth", "sxtb", "uxth", "uxtb", "adrp", "adr",
    "dup", "fmov", "umov", "smov", "abs", "addp", "uzp1", "uzp2", "zip1", "zip2", "trn1", "trn2", "ext", "rev64", "tbl",
    "shl", "sshr", "ushr", "srshr", "urshr", "shrn", "rshrn", "sqshrn", "uqshrn", "sqrshrn", "uqrshrn", "sqshrun",
    "xtn", "sqxtn", "uqxtn", "sqxtun", "sshll", "sshll2", "ushll", "ushll2", "sxtl", "sxtl2", "uxtl", "uxtl2",
    "addhn", "raddhn", "subhn", "uaddl", "uaddl2", "saddl", "saddl2", "uaddw", "uaddw2", "saddw", "saddw2", "usubl",
    "ssubl", "usubw", "ssubw", "umull", "umull2", "smull", "smull2", "sqdmulh", "sqrdmulh", "sqdmull", "pmull",
    "cmeq", "cmhi", "cmhs", "cmgt", "cmge", "cmlt", "cmle", "cmtst", "umax", "umin", "smax", "smin", "uqadd", "uqsub",
    "sqadd", "sqsub", "uhadd", "urhadd", "shadd", "srhadd", "uabd", "sabd", "uminv", "umaxv", "addv", "cnt", "not",
    "fadd", "fsub", "fmul", "fdiv", "frecpe", "frecps", "frsqrte", "frsqrts", "fcvtzs", "fcvtzu", "scvtf", "ucvtf",
    "fmax", "fmin", "fneg", "fabs", "nop"}

REG = re.compile(r"\b(?:([xw])(\d+)|([vqdshb])(\d+)|(sp|wsp)|([xw]zr))\b")

def regs(s):
    """the registers named in an operand string: r<n> (x/w), v<n> (vector/fp), sp"""
    out = []
    for m in REG.finditer(s):
        if m.group(1): out.append("r" + m.group(2))
        elif m.group(3): out.append("v" + m.group(4))
        elif m.group(5): out.append("sp")
    return out

def split_ops(s):
    ops, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{": depth += 1
        elif ch in "]}": depth -= 1
        if ch == "," and depth == 0: ops.append(cur.strip()); cur = ""
        else: cur += ch
    if cur.strip(): ops.append(cur.strip())
    return ops

# what the kernels' base registers address: "ro" read-only memory, ("buf", name) a buffer no other register reaches,
# ("stack", offset) the stack frame at sp + offset, ("window", lo, hi) somewhere in the stack frame's bytes lo..hi
# (or in a buffer: never overlapping the frame's other bytes); anything else may address anything
REGIONS = {"sp": ("stack", 0)}

def region(memop, size, post):
    """an address operand -> ("ro",), ("buf", reg), ("stack", lo, hi) with hi None when the bytes are not known, or
    ("any",)"""
    mm = re.match(r"\[(\w+)(?:,\s*(#-?\d+|[wx]\d+.*))?\]!?$", memop)
    if not mm: return ("any",)
    r = REGIONS.get(mm.group(1))
    if r is None: return ("any",)
    if r == "ro": return ("ro",)
    if r[0] == "buf": return ("buf", r[1])
    if r[0] == "window": return ("stack", r[1], r[2])
    off = mm.group(2)
    if off is None or off.startswith("#"):
        lo = r[1] + (int(off[1:]) if off else 0)
        if size and not post and not memop.endswith("!"): return ("stack", lo, lo + size)
    return ("stack", 0, None)

def may_alias(a, b):
    """two accesses' regions (one of them a store) may touch the same bytes"""
    if a[0] == "ro" or b[0] == "ro": return False            # never stored to
    if a[0] == "any" or b[0] == "any": return True
    if a[0] != b[0]: return False                             # a buffer and the stack
    if a[0] == "buf": return a[1] == b[1]
    if a[2] is None or b[2] is None: return True
    return not (a[2] <= b[1] or b[2] <= a[1])

VBYTES = {"16b": 16, "8h": 16, "4s": 16, "2d": 16, "8b": 8, "4h": 8, "2s": 8, "1d": 8}

def reg_bytes(op, mn):
    """bytes a load/store register operand moves"""
    m = re.match(r"([xwqdshb])\d+$", op)
    if m:
        if mn in ("strb", "ldrb", "ldrsb"): return 1
        if mn in ("strh", "ldrh", "ldrsh"): return 2
        if mn == "ldrsw": return 4
        return {"x": 8, "w": 4, "q": 16, "d": 8, "s": 4, "h": 2, "b": 1}[m.group(1)]
    m = re.match(r"v\d+\.(\w+)$", op)
    if m: return VBYTES.get(m.group(1))
    return None

class Ins:
    __slots__ = ("text", "mn", "defs", "uses", "mem", "lat", "kind", "width", "mul", "order", "uops", "unit", "occ",
                 "bg", "wb", "wlat", "dlat")

def parse(text, order):
    """an instruction line -> Ins, or None when it is not understood (it then ends the block)"""
    t = text.strip()
    if not t or t.endswith(":") or t.startswith(".") or t.startswith("//"): return None
    mn, _, rest = t.partition(" ")
    mn = mn.lower()
    base = mn.split(".")[0]
    if base not in KNOWN: return None
    I = Ins(); I.text = text; I.mn = mn; I.order = order; I.mem = None
    ops = split_ops(rest)
    defs, uses = [], []
    if base in BRANCH: return None
    if base in STORE or base in LOAD:
        mi = next((i for i, o in enumerate(ops) if o.startswith("[")), None)
        if mi is None: return None                                              # (a literal load)
        memop = ops[mi]; post = ops[mi + 1:]
        data = ops[:mi]
        dregs = []
        for o in data:
            if o.startswith("{"): dregs += regs(o)
            else: dregs += regs(o)
        areg = regs(memop)
        wb = memop.endswith("!") or bool(post)
        if "sp" in areg and wb: return None                                     # moves sp: prologue/epilogue
        if base in STORE: uses += dregs + areg
        else:
            defs += dregs; uses += areg
            if re.search(r"\}\[\d+\]", data[0] if data else ""): uses += dregs   # ld1 {v.s}[i]: a lane
        if wb: defs.append(areg[0])
        # the address: stack accesses at a known offset are told apart, anything else may alias anything
        size = None
        if base in ("ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4", "ld1r"):
            lst = re.findall(r"v\d+\.(\w+)", data[0])
            if base == "ld1r": size = {"b": 1, "h": 2, "s": 4, "d": 8}.get(lst[0][-1]) if lst else None
            elif lst and re.search(r"\}\[\d+\]", data[0]) is None: size = sum(VBYTES.get(a, 0) for a in lst) or None
        else:
            sz = [reg_bytes(o, base) for o in data]
            if all(sz): size = sum(sz)
        I.mem = ("st" if base in STORE else "ld", region(memop, size, post))
    else:
        rs = [regs(o) for o in ops]
        if base in FLAGS_ONLY:
            uses += sum(rs, []); defs.append("nzcv")
        else:
            if not rs or not rs[0]:
                if base == "nop": pass
                else: return None
            else:
                defs += rs[0]; uses += sum(rs[1:], [])
                rmw = base in RMW
                if base in ("mov", "ins") and re.search(r"\]\s*$", ops[0]): rmw = True   # element insert
                if base in ("orr", "bic") and len(ops) >= 2 and ops[1].startswith("#"): rmw = True  # immediate forms
                if rmw: uses += rs[0]
            if base in SET_FLAGS: defs.append("nzcv")
        if base in READ_FLAGS: uses.append("nzcv")
    if "sp" in defs: return None
    I.defs = [d for d in defs]; I.uses = uses
    q = bool(re.search(r"\.(16b|8h|4s|2d)\b|\bq\d", rest))
    vec = bool(re.search(r"\bv\d|\b[qdsbh]\d", rest))
    I.kind = "mem" if I.mem else ("vec" if vec else "int")
    I.width = (2 if q else 1) if I.kind == "vec" else 0
    I.mul = base in ("mul", "mla", "mls", "umull", "umull2", "smull", "smull2", "umlal", "umlal2", "smlal", "smlal2",
                     "sqdmulh", "sqrdmulh", "sqdmull") and I.kind == "vec"
    I.lat = latency(base, rest, q, I)
    machine(I, base, rest, q, ops)
    return I

def latency(b, rest, q, I):
    """result latency, after LLVM's Cortex-A55 scheduling model"""
    if I.mem:
        if I.mem[0] == "st": return 1
        if b in ("ld2", "ld3", "ld4"): return 7
        if b == "ld1": return 4 if "}[" in rest else {1: 4, 2: 5, 3: 6}.get(len(re.findall(r"v\d+", rest)), 11)
        if b == "ldp" and q: return 6
        if b == "ldp": return 5 if re.match(r"\s*[dsqx]", rest) else 4
        if b == "ld1r": return 4
        return 4 if re.search(r"\[\w+,\s*[wx]\d+", rest) else 3
    if I.kind == "int":
        return 4 if b in ("mul", "madd", "msub") else 3
    if b in ("fadd", "fsub", "fmul", "fmla", "fmls", "frecpe", "frecps", "frsqrte", "frsqrts", "fcvtzs", "fcvtzu",
             "scvtf", "ucvtf", "fmax", "fmin"): return 4
    if b in ("mul", "mla", "mls", "umull", "umull2", "smull", "smull2", "umlal", "umlal2", "smlal", "smlal2", "sqdmulh",
             "sqrdmulh", "sqdmull", "zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2", "tbl", "tbx", "bit", "bif", "bsl",
             "movi"): return 4
    if b == "dup": return 2 if q else 4
    if b in ("fmov", "umov", "smov"): return 3
    if b in ("mov", "ins") and re.search(r"\]\s*,\s*[wx]", rest): return 4        # insert from a general register
    if b in ("and", "orr", "eor", "bic", "orn", "mov", "mvn", "mvni", "not"): return 1
    if b in ("uaddl", "uaddl2", "saddl", "uaddw", "uaddw2", "saddw", "addhn", "addhn2", "raddhn", "subhn", "abs",
             "cmtst", "rshrn", "rshrn2", "uabd", "sabd"): return 3
    return 2

# NEON forms that hold an FP pipe for one cycle however wide their operands (the model's 64-bit class), and forms
# that hold it for two with 64-bit operands (they widen); everything else holds it two cycles when 128 bits wide
VD = {"shl", "sshr", "ushr", "srshr", "urshr", "shrn", "shrn2", "rshrn", "rshrn2", "sqshrn", "uqshrn", "sqrshrn",
      "uqrshrn", "sqshrun", "xtn", "xtn2", "sqxtn", "uqxtn", "sqxtun", "fcvtzs", "fcvtzu", "scvtf", "ucvtf", "dup",
      "tbl", "tbx", "fmov", "umov", "smov", "ins"}
WIDE = {"umull", "umull2", "smull", "smull2", "umlal", "umlal2", "smlal", "smlal2", "uaddl", "uaddl2", "saddl",
        "saddl2", "uaddw", "uaddw2", "saddw", "saddw2", "usubl", "ssubl", "usubw", "ssubw", "ushll", "ushll2", "sshll",
        "sshll2", "uxtl", "uxtl2", "sxtl", "sxtl2", "sqdmull", "addhn", "addhn2", "raddhn", "subhn"}

def machine(I, b, rest, q, ops):
    """the resources of llvm-mca's Cortex-A55 model (in-order issue, two micro-ops a cycle): I.uops micro-ops, the
    unit (alu x2, imac, fp x2, mac x2, ld, st) held for I.occ cycles, I.bg when it must open an issue group (the
    128-bit NEON class), I.wb when it takes part in the in-order writeback (every result written in program order:
    stores are exempt), I.wlat the latency of its first result (a base register's writeback: 1), I.dlat per
    register written"""
    I.uops, I.occ, I.bg, I.wb = 1, 1, False, True
    wbase = bool(I.mem) and (ops[-1].endswith("!") or not ops[-1].startswith("["))
    I.dlat = {d: I.lat for d in I.defs}
    if I.mem:
        if I.mem[0] == "st":
            I.unit, I.wb = "st", False
            if wbase: I.uops = 2
        else:
            I.unit = "ld"
            if b == "ldp":
                I.uops = 2
                if q: I.occ = 6
                elif re.match(r"\s*[dx]", rest): I.occ = 2
            elif b in ("ld2", "ld3", "ld4"): I.occ = {"ld2": 4, "ld3": 6, "ld4": 8}[b]
            elif b == "ld1": I.occ = 1 if "}[" in rest else {1: 1, 2: 2, 3: 3}.get(len(re.findall(r"v\d+", rest)), 8)
            if wbase: I.uops += 1
        if b == "ldp" and I.defs: I.dlat[regs(ops[0])[0]] = 4                  # (the pair's first register)
        if wbase:
            m = re.match(r"\[(\w+)", next(o for o in ops if o.startswith("[")))
            if m and regs(m.group(1)): I.dlat[regs(m.group(1))[0]] = 1
    elif I.kind == "int": I.unit = "imac" if b in ("mul", "madd", "msub") else "alu"
    else:
        I.unit = "fp"
        if b in ("fmls", "fmla") and "[" in rest or re.match(r"fmul\s+s", I.mn + " " + rest.strip()): I.unit = "mac"
        elif b in VD or (b == "fmul" and "[" in rest) or (b in ("mov", "ins") and "[" in rest): pass
        elif q or b in WIDE: I.occ, I.bg = 2, True
    I.wlat = min(I.dlat.values()) if I.dlat else I.lat
    I.dlat, I.unit, I.uops = tuple(I.dlat.items()), UNITS[I.unit], min(I.uops, 2)

UNITS = {"alu": (0, 1), "imac": (2,), "fp": (3, 4), "mac": (5, 6), "ld": (7,), "st": (8,), "b": (9,)}

class Machine:
    """the in-order issue state: the current cycle and its issued micro-ops, when each unit and register is free, the
    last writeback"""
    __slots__ = ("cycle", "used", "free", "ready", "lastwb")
    def __init__(self):
        self.cycle, self.used, self.lastwb = 0, 0, 0
        self.free = [0] * 10
        self.ready = {}
    def copy(self):
        m = Machine.__new__(Machine)
        m.cycle, m.used, m.lastwb = self.cycle, self.used, self.lastwb
        m.free, m.ready = list(self.free), dict(self.ready)
        return m
    def when(self, I):
        """the cycle I would issue in"""
        t = self.cycle
        ready = self.ready
        for r in I.uses:
            x = ready.get(r, 0)
            if x > t: t = x
        if I.wb and self.lastwb - I.wlat > t: t = self.lastwb - I.wlat
        f = self.free
        u = min(f[k] for k in I.unit)
        if u > t: t = u
        if t == self.cycle and (self.used + I.uops > 2 or (I.bg and self.used)): t += 1
        return t
    def issue(self, I, t=None):
        if t is None: t = self.when(I)
        if t > self.cycle: self.cycle, self.used = t, 0
        self.used += I.uops
        f = self.free
        k = min(I.unit, key=f.__getitem__); f[k] = t + I.occ
        for d, l in I.dlat: self.ready[d] = t + l
        if I.wb: self.lastwb = t + I.lat
        return t
    def key(self, regs):
        """the state relative to the current cycle, as far as instructions reading regs can tell"""
        c = self.cycle
        return (self.used, max(self.lastwb - c, 1), tuple(max(x - c, 0) for x in self.free),
                tuple(max(self.ready.get(r, 0) - c, 0) for r in regs))

BR = Ins(); BR.uses, BR.defs, BR.dlat, BR.uops, BR.unit, BR.occ, BR.bg, BR.wb, BR.lat, BR.wlat, BR.text = (
    [], [], (), 1, UNITS["b"], 1, False, True, 1, 1, "")          # a branch (an issue slot, in the writeback order)

def cost(m, order):
    """issue order on a copy of the machine state -> (the cycle the block's branch issues in, the cycle its last result
    is ready)"""
    m = m.copy()
    for I in order: m.issue(I)
    return (m.when(BR), max(m.ready.values(), default=0)), m

def dag(ins):
    """the dependence graph of a block: (preds, succs) as (index, latency) lists, the latency path to the block's end"""
    n = len(ins)
    preds = [[] for _ in range(n)]                    # (pred index, latency)
    last_def, readers = {}, {}
    mems = []
    for i, I in enumerate(ins):
        for r in I.uses:
            if r in last_def: preds[i].append((last_def[r], dict(ins[last_def[r]].dlat).get(r, ins[last_def[r]].lat)))
        for r in I.defs:
            for j in readers.get(r, ()):
                if j != i: preds[i].append((j, 0))
            if r in last_def: preds[i].append((last_def[r], 0))
        if I.mem:
            kind, rng = I.mem
            for j in mems:
                k2, r2 = ins[j].mem
                if kind == "ld" and k2 == "ld": continue
                if not may_alias(rng, r2): continue
                preds[i].append((j, 1 if k2 == "st" else 0))
            mems.append(i)
        for r in I.uses: readers.setdefault(r, []).append(i)
        for r in I.defs: last_def[r] = i; readers[r] = []
    succs = [[] for _ in range(n)]
    for i in range(n):
        for j, l in preds[i]: succs[j].append((i, l))
    prio = [0] * n
    for i in range(n - 1, -1, -1):
        prio[i] = max([ins[i].lat] + [l + prio[j] for j, l in succs[i] if l]) if succs[i] else ins[i].lat
    return preds, succs, prio

def list_cycles(ins, preds, succs, prio):
    """the cycle-driven list scheduler: each cycle up to two instructions, at most one load or store, NEON work up to
    two 64-bit halves and one NEON multiply; among the ready ones the longest latency path first"""
    n = len(ins)
    npred = [len(p) for p in preds]
    ready_at = [0] * n
    avail = [i for i in range(n) if not npred[i]]
    done = []
    cycle = 0
    while len(done) < n:
        slots, memu, vecw, mulu = 2, 1, 2, 1
        progressed = True
        while slots and progressed:
            progressed = False
            cand = [i for i in avail if ready_at[i] <= cycle]
            cand.sort(key=lambda i: (-prio[i], i))
            for i in cand:
                I = ins[i]
                if I.kind == "mem" and not memu: continue
                if I.kind == "vec" and I.width > vecw: continue
                if I.mul and not mulu: continue
                slots -= 1
                if I.kind == "mem": memu -= 1
                if I.kind == "vec": vecw -= I.width
                if I.mul: mulu -= 1
                avail.remove(i); done.append(i)
                for j, l in succs[i]:
                    ready_at[j] = max(ready_at[j], cycle + l)
                    npred[j] -= 1
                    if not npred[j]: avail.append(j)
                progressed = True
                break                                  # (re-evaluate: a 0-latency successor may issue alongside)
        if not avail and len(done) < n: raise AssertionError("schedule: cycle in the dependence graph")
        cycle += 1
    return [ins[i] for i in done]

def list_machine(ins, preds, succs, prio, m, key):
    """the list scheduler on the machine model: from the state m, each step issues the instruction that key ranks
    first among those whose predecessors have issued (key(i, cycle it would issue in))"""
    m = m.copy()
    n = len(ins)
    npred = [len(p) for p in preds]
    avail = [i for i in range(n) if not npred[i]]
    done = []
    while avail:
        best = min(avail, key=lambda i: key(i, m.when(ins[i])))
        m.issue(ins[best])
        avail.remove(best); done.append(best)
        for j, l in succs[best]:
            npred[j] -= 1
            if not npred[j]: avail.append(j)
    if len(done) < n: raise AssertionError("schedule: cycle in the dependence graph")
    return [ins[i] for i in done]

MEMO = {}

def schedule_block(ins, m=None, after=()):
    """ins: [Ins] of one block (no branch), m: the machine state it starts in, after: what follows (the branch and the
    next block, as they stand) -> the issue order: of the candidates (the cycle-driven list schedule, the order written,
    the machine list schedules under several rankings) the one whose branch issues soonest on the model, then the one
    that gets through the next block soonest"""
    if m is None: m = Machine()
    if len(ins) < 2: return ins
    after = list(after) + [BR]
    memo = (tuple(I.text for I in ins), tuple(I.text for I in after),
            m.key(sorted({r for I in ins + after for r in I.uses})))
    if memo in MEMO: return [ins[i] for i in MEMO[memo]]
    preds, succs, prio = dag(ins)
    lat = [I.lat for I in ins]
    keys = (lambda i, t: (t, -prio[i], i),                    # the soonest to issue, the longest path first
            lambda i, t: (t - prio[i], t, i),                 # the latest start the path allows
            lambda i, t: (2 * t - prio[i], t, i),
            lambda i, t: (t, -len(succs[i]), -prio[i], i),    # the most successors
            lambda i, t: (t + lat[i], -prio[i], i),           # the soonest result (the in-order writeback)
            lambda i, t: (t + lat[i] - prio[i], t, i),
            lambda i, t: (t, t + lat[i], -prio[i], i))
    cands, seen = [], set()
    for o in [ins, list_cycles(ins, preds, succs, prio)] + [list_machine(ins, preds, succs, prio, m, k) for k in keys]:
        ids = tuple(I.order for I in o)
        if ids not in seen: seen.add(ids); cands.append(o)
    if len(cands) > 1:
        c = [cost(m, o + [BR])[0] for o in cands]
        best = min(c)
        cands = [o for o, x in zip(cands, c) if x == best]
    if len(cands) > 1: cands.sort(key=lambda o: cost(m, o + after)[0])
    pos = {I.order: k for k, I in enumerate(ins)}
    MEMO[memo] = [pos[I.order] for I in cands[0]]
    return cands[0]

def schedule(lines):
    """the kernel file's lines, each basic block reordered"""
    items = [parse(l, k) for k, l in enumerate(lines)]
    out, m, k = [], Machine(), 0
    while k < len(lines):
        if items[k] is None:
            l = lines[k]; out.append(l); k += 1
            t = l.strip()
            mn = t.split(" ")[0].lower()
            if mn.split(".")[0] in BRANCH:
                m.issue(BR)
                if mn in ("b", "br", "ret"): m = Machine()               # no fall-through: the next block starts afresh
            elif t and not t.endswith(":"): m = Machine()               # (directives, functions)
            continue
        e = k
        while e < len(lines) and items[e] is not None: e += 1
        block = items[k:e]
        # what follows on the fall-through path: the branch (if any), then the next block as it stands
        after, j = [], e
        while j < len(lines) and items[j] is None:
            t = lines[j].strip(); mn = t.split(" ")[0].lower()
            if mn.split(".")[0] in BRANCH:
                after.append(BR)
                if mn in ("b", "br", "ret"): j = None; break
            elif t and not t.endswith(":"): j = None; break
            j += 1
        if j is not None:
            while j < len(lines) and items[j] is not None: after.append(items[j]); j += 1
        order = schedule_block(block, m, after)
        out.extend(I.text for I in order)
        for I in order: m.issue(I)
        k = e
    return out
