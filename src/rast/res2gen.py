"""res2gen.py: writes res2_line.S, the fused line of res2.c (the 2x resolve of one colour line with the compositor's
visibility bitmaps of its two half-rows, res2_line_asm), and the bitmaps alone of an output line already written
(comp_vis_line_asm: four ldr q + uzp2 .8h x2 + uzp2 .16b per 16 pixels of a half-row, the translucency term
umin(a, a ^ 31) as compvis.h), list-scheduled for the Cortex-A55 as llvm-mca models it.

The computation is res2.c's (see its header). The line's 512 pixels are 16 sets of 32; per set:
    ldr q x8 (latency 3), bic #0xe0 << 24 x8 (the mask, in place), uzp1 / uzp2 .4s x8 (even and odd pixels, 4),
    stp q x4 (the two half-rows), uzp2 .8h x4 + uzp2 .16b x2 (the 16 alphas of each parity), cmtst x2 (a != 0),
    bic x2 (~a where a != 0: the translucency term) OR-ed into an accumulator per parity, and x2 (the bit weights);
then the addp tree: level 1 per two sets, level 2 per four, level 3 per eight (the bitmap bytes 0..15 and 16..31 of
both half-rows, stored). Loads are ldr q, not ld2: llvm-mca's A55 model retires in order, so an instruction that would
write its result before an earlier one waits (an ld2's 7 cycles stall every short instruction after it: DraStic's loop
spends 68 cycles per 32 pixels so); ldr q (3) and uzp (4) leave little to wait for.

The scheduler is greedy over the whole line's dependence graph: each cycle at most two instructions, one load, one
store, one 128-bit NEON operation; an instruction issues when its operands are ready and, being in order, not before
its write-back would follow the latest one issued; among the candidates the longest path to the end wins. Virtual
registers are then given the 24 caller-saved vector registers (v0-v7, v16-v31) in issue order; a set may only start
loading once every instruction of the set two before it (but its addp tree) has issued, which bounds the live
registers. Run: python3 res2gen.py > res2_line.S"""
import sys

LAT = {"ldr": 3, "bic_i": 1, "uzp": 4, "cmtst": 3, "bic": 1, "and": 1, "orr": 1, "addp": 3, "stp": 0, "str": 0}


class Op:
    def __init__(self, kind, fmt, dst, srcs, unit, rmw=False):
        self.kind, self.fmt, self.dst, self.srcs, self.unit, self.rmw = kind, fmt, dst, srcs, unit, rmw
        self.lat = LAT[kind]
        self.users = []
        self.prio = 0
        self.set = 0


def tree(op, v, lv, w, p, k, ww, bits):
    """the addp tree of parity p after its vector k (weights ww), as soon as a level's pair is complete; level 3 is
    stored: bitmap bytes 0..15 (k = 7) or 16..31 (k = 15), the odd half-row's 32 bytes after the even one's"""
    lvl, idx, node = 1, k, ww
    while idx % 2 == 1:
        left = lv[p].pop((lvl, idx - 1)) if lvl > 1 else w[p][k - 1]
        d = v(); op("addp", "addp  v{d}.16b, v{s0}.16b, v{s1}.16b", d, [left, node], "fp")
        node, idx, lvl = d, idx // 2, lvl + 1
        if lvl == 4:
            op("str", "str   q{s0}, [%s, #%d]" % (bits, (0 if p == "e" else 32) + 16 * idx), None, [d], "st")
            return
    if lvl > 1:
        lv[p][(lvl, idx)] = node


def build(mode):
    """mode "res": res2_line_asm (resolve + bitmaps); "vis": comp_vis_line_asm (the bitmaps of a written line)"""
    ops, vid, cur = [], [0], [0]

    def v():
        vid[0] += 1
        return vid[0]

    def op(kind, fmt, dst, srcs, unit, rmw=False):
        o = Op(kind, fmt, dst, srcs, unit, rmw)
        o.set = cur[0]
        ops.append(o)
        return o

    acc = {}
    wt = v()
    op("ldr", "ld1   {{v{d}.16b}}, [%s]" % ("x4" if mode == "res" else "x2"), wt, [], "ld")
    if mode == "vis":
        c31 = v()
        op("bic_i", "movi  v{d}.16b, #0x1f", c31, [], "fp")
    for p in "eo":
        acc[p] = v()
        op("bic_i", "movi  v{d}.16b, #0", acc[p], [], "fp")
    w = {"e": [], "o": []}
    lv = {"e": {}, "o": {}}
    for k in range(16):
        cur[0] = k
        if mode == "vis":
            # the two half-rows' pixels 16k..16k+15, as written: the alphas are byte 3, any value
            for p in "eo":
                X = []
                for j in range(4):
                    d = v()
                    op("ldr", "ldr   q{d}, [x0, #%d]" % ((0 if p == "e" else 0x400) + 64 * k + 16 * j), d, [], "ld")
                    X.append(d)
                h0 = v(); op("uzp", "uzp2  v{d}.8h, v{s0}.8h, v{s1}.8h", h0, [X[0], X[1]], "fp")
                h1 = v(); op("uzp", "uzp2  v{d}.8h, v{s0}.8h, v{s1}.8h", h1, [X[2], X[3]], "fp")
                a = v(); op("uzp", "uzp2  v{d}.16b, v{s0}.16b, v{s1}.16b", a, [h0, h1], "fp")
                c = v(); op("cmtst", "cmtst v{d}.16b, v{s0}.16b, v{s0}.16b", c, [a], "fp")
                t = v(); op("bic", "eor   v{d}.16b, v{s0}.16b, v{s1}.16b", t, [a, c31], "fp")
                u = v(); op("bic", "umin  v{d}.16b, v{s0}.16b, v{s1}.16b", u, [a, t], "fp")
                na = v(); op("orr", "orr   v{d}.16b, v{s0}.16b, v{s1}.16b", na, [acc[p], u], "fp"); acc[p] = na
                ww = v(); op("and", "and   v{d}.16b, v{s0}.16b, v{s1}.16b", ww, [c, wt], "fp")
                w[p].append(ww)
                tree(op, v, lv, w, p, k, ww, "x1")
            continue
        r = []
        for j in range(8):
            d = v()
            op("ldr", "ldr   q{d}, [x0, #%d]" % (128 * k + 16 * j), d, [], "ld")
            r.append(d)
        m = []
        for j in range(8):
            d = v()
            op("bic_i", "bic   v{d}.4s, #0xe0, lsl #24", d, [r[j]], "fp", rmw=True)
            m.append(d)
        E, O = [], []
        for i in range(4):
            d = v(); op("uzp", "uzp1  v{d}.4s, v{s0}.4s, v{s1}.4s", d, [m[2 * i], m[2 * i + 1]], "fp"); E.append(d)
            d = v(); op("uzp", "uzp2  v{d}.4s, v{s0}.4s, v{s1}.4s", d, [m[2 * i], m[2 * i + 1]], "fp"); O.append(d)
        for p, X, base in (("e", E, "x1"), ("o", O, "x2")):
            op("stp", "stp   q{s0}, q{s1}, [%s, #%d]" % (base, 64 * k), None, [X[0], X[1]], "st")
            op("stp", "stp   q{s0}, q{s1}, [%s, #%d]" % (base, 64 * k + 32), None, [X[2], X[3]], "st")
        for p, X in (("e", E), ("o", O)):
            h0 = v(); op("uzp", "uzp2  v{d}.8h, v{s0}.8h, v{s1}.8h", h0, [X[0], X[1]], "fp")
            h1 = v(); op("uzp", "uzp2  v{d}.8h, v{s0}.8h, v{s1}.8h", h1, [X[2], X[3]], "fp")
            a = v(); op("uzp", "uzp2  v{d}.16b, v{s0}.16b, v{s1}.16b", a, [h0, h1], "fp")
            c = v(); op("cmtst", "cmtst v{d}.16b, v{s0}.16b, v{s0}.16b", c, [a], "fp")
            t = v(); op("bic", "bic   v{d}.16b, v{s0}.16b, v{s1}.16b", t, [c, a], "fp")
            na = v(); op("orr", "orr   v{d}.16b, v{s0}.16b, v{s1}.16b", na, [acc[p], t], "fp"); acc[p] = na
            ww = v(); op("and", "and   v{d}.16b, v{s0}.16b, v{s1}.16b", ww, [c, wt], "fp")
            w[p].append(ww)
            tree(op, v, lv, w, p, k, ww, "x3")
    for p in "eo":
        if mode == "res":
            d = v(); op("addp", "shl   v{d}.16b, v{s0}.16b, #3", d, [acc[p]], "fp"); acc[p] = d
        d = v(); op("addp", "umaxv b{d}, v{s0}.16b", d, [acc[p]], "fp"); acc[p] = d
    op("str", "fmov  w0, s{s0}", None, [acc["e"]], "x")
    op("str", "fmov  w5, s{s0}", None, [acc["o"]], "x")
    return ops


def schedule(ops, window):
    prod = {o.dst: o for o in ops if o.dst}
    for o in ops:
        for s in o.srcs:
            prod[s].users.append(o)
    for o in reversed(ops):
        o.prio = o.lat + max((u.prio for u in o.users), default=0)
    # sets: a load of set k may issue only once set k - window has issued all but its addp tree (bounds the live registers)
    done, ready_at, order = set(), {}, []
    t, last_wb = 0, 0
    pending = list(ops)
    set_of = {id(o): o.set for o in ops}
    started = {}
    left = {}                       # per set: its instructions (but the addp tree's) not yet issued
    for o in ops:
        if o.kind != "addp":
            left[set_of[id(o)]] = left.get(set_of[id(o)], 0) + 1
    while pending:
        issued, units = 0, set()
        while issued < 2:
            best = None
            for i, o in enumerate(pending[:400]):
                if o.unit in units and o.unit != "x":
                    continue
                if any(s not in done or ready_at[s] > t for s in o.srcs):
                    continue
                if o.kind == "ldr" and o.fmt.startswith("ldr"):
                    sk = set_of[id(o)]
                    if sk - window >= 0 and left[sk - window]:
                        continue
                if o.lat and t + o.lat < last_wb:
                    continue
                if best is None or o.prio > best[1].prio:
                    best = (i, o)
            if not best:
                break
            i, o = best
            pending.pop(i)
            started[id(o)] = t
            if o.kind != "addp":
                left[set_of[id(o)]] -= 1
            order.append(o)
            units.add(o.unit)
            if o.dst:
                done.add(o.dst)
                ready_at[o.dst] = t + o.lat
            if o.lat:
                last_wb = max(last_wb, t + o.lat)
            issued += 1
        t += 1
    return order, t


def allocate(order):
    free = list(range(0, 8)) + list(range(16, 32))
    last = {}
    for i, o in enumerate(order):
        for s in o.srcs:
            last[s] = i
    phys, out = {}, []
    for i, o in enumerate(order):
        srcs = [phys[s] for s in o.srcs]
        for s in set(o.srcs):
            if last[s] == i and not (o.rmw and s == o.srcs[0]):
                free.append(phys[s])
        if o.dst:
            if o.rmw:
                phys[o.dst] = phys[o.srcs[0]]
                if last.get(o.dst, -1) < i:
                    free.append(phys[o.dst])
            else:
                if not free:
                    raise SystemExit("out of registers at %d" % i)
                phys[o.dst] = free.pop(0)
                if o.dst not in last:
                    free.append(phys[o.dst])
        d = phys.get(o.dst)
        out.append(o.fmt.format(d=d, **{"s%d" % n: r for n, r in enumerate(srcs)}))
    return out


FUNCS = {
    "res": ("res2_line_asm", "uint32_t res2_line_asm(const uint32_t *col_line, uint32_t *even, uint32_t *odd, uint8_t bits[64],\n"
            " *                        const uint8_t weights[16]): the resolve of a line and its two half-rows' bitmaps; returns\n"
            " * the translucency accumulators' low 5 bits (moved to the top, umaxv), even | odd << 8"),
    "vis": ("comp_vis_line_asm", "uint32_t comp_vis_line_asm(const uint32_t *out_line, uint8_t bits[64], const uint8_t weights[16]): the\n"
            " * bitmaps of a written output line's two half-rows; returns umaxv(umin(a, a ^ 31)) of each, even | odd << 8"),
}


def main():
    print("/* res2_line.S: generated by res2gen.py (python3 res2gen.py > res2_line.S); see res2.c and res2gen.py. */")
    for mode, window in (("res", 2), ("vis", 2)):
        name, doc = FUNCS[mode]
        ops = build(mode)
        order, cycles = schedule(ops, window)
        lines = allocate(order)
        print("\n/* " + doc + " */")
        print("    .text\n    .p2align 4\n    .globl %s\n    .hidden %s\n    .type %s, %%function\n%s:" % (name, name, name, name))
        for l in lines:
            print("    " + l)
        print("    orr   w0, w0, w5, lsl #8\n    ret\n    .size %s, . - %s" % (name, name))
        print("// %s: scheduler estimate %d cycles" % (name, cycles), file=sys.stderr)


main()
