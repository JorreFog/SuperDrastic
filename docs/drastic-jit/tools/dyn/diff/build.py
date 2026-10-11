#!/usr/bin/env python3
"""Differential-test ROM for DraStic's interpreter vs JIT (ARM9, ARM mode).
Generates diff9.s: each test presets NZCVQ, loads operands, runs one instruction, then stores the result
register(s) and CPSR (mrs) to RAM at 0x02200100 + 4*k. At the end writes 'DONE' (0x454E4F44) at 0x02200000 and the
number of words at 0x02200004. Also writes out/tests.txt: one line per stored word, describing it, plus the
expected value computed here from the ARMv5TE semantics (so each run can be checked on its own).
Usage: build.py  -> out/diff.nds, out/tests.txt"""
import os, struct, subprocess, sys
EXTRA = "extra" in sys.argv[1:]   # build.py extra -> out_extra/: adds register-aliasing / conditional / Q-setting MSR tests
HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, "out_extra" if EXTRA else "out"); os.makedirs(OUT, exist_ok=True)
M = 0xFFFFFFFF
def s32(x): x &= M; return x - (1 << 32) if x & 0x80000000 else x
def sat(x):
    if x > 0x7FFFFFFF: return 0x7FFFFFFF, 1
    if x < -0x80000000: return 0x80000000, 1
    return x & M, 0
def h(x, top): v = (x >> 16) & 0xFFFF if top else x & 0xFFFF; return v - 0x10000 if v & 0x8000 else v

asm = []; desc = []   # desc: (text, expected or None)
def emit(*l): asm.extend(l)
def ld(r, v): emit("    ldr %s, =0x%08x" % (r, v & M))
def flagsf(nzcvq): emit("    msr cpsr_f, #0x%08x" % (nzcvq << 27))   # bits 31..27 = N Z C V Q
# The JIT's MSR with only the f field cannot clear Q (probe3/4/6), so tests preset flags through cpsr_fc
def flags(nzcvq): emit("    mrs r10, cpsr", "    bic r10, r10, #0xF8000000", "    orr r10, r10, #0x%08x" % (nzcvq << 27), "    msr cpsr_fc, r10")
def store(r, text, exp=None): emit("    str %s, [r11], #4" % r); desc.append((text, exp))
def store_cpsr(text, exp_flags=None):
    emit("    mrs r10, cpsr", "    lsr r10, r10, #27"); store("r10", text + " NZCVQ", exp_flags)

def nzcvq(n, z, c, v, q): return n << 4 | z << 3 | c << 2 | v << 1 | q

# 1. register-specified shifts with S (MOVS rd, rm, <sh> rs): result and C
def shift(kind, x, n, cin):
    x &= M
    if n == 0: return x, cin
    if kind == "lsl":
        if n < 32: return (x << n) & M, (x >> (32 - n)) & 1
        if n == 32: return 0, x & 1
        return 0, 0
    if kind == "lsr":
        if n < 32: return x >> n, (x >> (n - 1)) & 1
        if n == 32: return 0, x >> 31
        return 0, 0
    if kind == "asr":
        if n < 32: return (s32(x) >> n) & M, (x >> (n - 1)) & 1
        return (M if x >> 31 else 0), x >> 31
    if kind == "ror":
        r = n & 31
        if r == 0: return x, x >> 31
        return ((x >> r) | (x << (32 - r))) & M, (x >> (r - 1)) & 1
k = 0
for kind in ("asr", "lsr", "lsl", "ror"):
    for x in (0x1, 0x2, 0x3, 0xF0, 0x80000000, 0x80000001, 0x40000000):
        for n in (0, 1, 2, 4, 31, 32, 33, 0x101):
            for cin in (0, 1):
                ld("r1", x); ld("r2", n); flags(nzcvq(0, 0, cin, 1, 0))
                emit("    movs r0, r1, %s r2" % kind)
                res, c = shift(kind, x, n & 0xFF, cin)
                tag = "movs r0,r1,%s r2  r1=%08x r2=%x Cin=%d" % (kind, x, n, cin)
                store("r0", tag + " result", res)
                store_cpsr(tag, nzcvq(res >> 31, int(res == 0), c, 1, 0))

# Q flag probes: can MSR clear/set Q, before and after a saturating op set it?
flagsf(0); store_cpsr("probe1 msr cpsr_f,#0 (before any Q set)", 0)
ld("r1", 0x7FFFFFFF); ld("r2", 1); flagsf(0); emit("    qadd r0, r1, r2"); store_cpsr("probe2 qadd 7fffffff+1 (sets Q)", 1)
flagsf(0); store_cpsr("probe3 msr cpsr_f,#0 after Q set", 0)
emit("    mov r0, #0", "    msr cpsr_f, r0"); store_cpsr("probe4 msr cpsr_f,r0(=0)", 0)
flagsf(0x1F); store_cpsr("probe5 msr cpsr_f,#0xf8000000", 0x1F)
flagsf(0); store_cpsr("probe6 msr cpsr_f,#0 again", 0)
emit("    mrs r0, cpsr", "    bic r0, r0, #0xF8000000", "    msr cpsr_fc, r0"); store_cpsr("probe7 msr cpsr_fc,r0 (NZCVQ cleared)", 0)

# 2/3. QADD/QSUB/QDADD/QDSUB
def qop(op, a, b):
    a, b = s32(a), s32(b); q = 0
    if op in ("qdadd", "qdsub"):
        b, q = sat(2 * b); b = s32(b)
    r, q2 = sat(a + b if op in ("qadd", "qdadd") else a - b)
    return r, q | q2
for op in ("qadd", "qsub", "qdadd", "qdsub"):
    for a, b in ((5, -1), (1, 0x10), (-5, 3), (0x7FFFFFFF, 1), (0x80000000, -1), (0x80000000, 1), (0, 0x40000000),
                 (0x10, -0x10), (-1, -1), (0x12345678, 0x01000000), (3, 0xC0000000)):
        ld("r1", a); ld("r2", b); flags(0)
        emit("    %s r0, r1, r2" % op)
        r, q = qop(op, a, b)
        tag = "%s r0,r1,r2  r1=%08x r2=%08x" % (op, a & M, b & M)
        store("r0", tag + " result", r); store_cpsr(tag, q)

# 4. halfword multiplies
vals = [(0x0003FFFE, 0x7FFF0005), (0x80008000, 0x80008000), (0x7FFF7FFF, 0x7FFF7FFF), (0xFFFF0001, 0x0002FFFF)]
for x, y in vals:
    for xy in ("bb", "bt", "tb", "tt"):
        p = h(x, xy[0] == "t") * h(y, xy[1] == "t")
        ld("r1", x); ld("r2", y); flags(0)
        emit("    smul%s r0, r1, r2" % xy)
        tag = "smul%s r0,r1,r2  r1=%08x r2=%08x" % (xy, x, y)
        store("r0", tag + " result", p & M); store_cpsr(tag, 0)
        for acc in (0x7FFFFFF0, 0x10, 0x80000000):
            ld("r1", x); ld("r2", y); ld("r3", acc); flags(0)
            emit("    smla%s r0, r1, r2, r3" % xy)
            r = p + s32(acc); q = int(r != s32(r & M))
            tag = "smla%s r0,r1,r2,r3  r1=%08x r2=%08x r3=%08x" % (xy, x, y, acc)
            store("r0", tag + " result", r & M); store_cpsr(tag, q)
        for lo, hi in ((0xFFFFFFF0, 0x00000000), (0x00000005, 0xFFFFFFFF), (0x7FFFFFFF, 0x7FFFFFFF)):
            ld("r1", x); ld("r2", y); ld("r3", lo); ld("r4", hi); flags(0)
            emit("    smlal%s r3, r4, r1, r2" % xy)
            acc = (hi << 32 | lo) + p; acc &= (1 << 64) - 1
            tag = "smlal%s r3,r4,r1,r2  r1=%08x r2=%08x lo=%08x hi=%08x" % (xy, x, y, lo, hi)
            store("r3", tag + " lo", acc & M); store("r4", tag + " hi", acc >> 32); store_cpsr(tag, 0)
    for wy in ("b", "t"):
        p = (s32(x) * h(y, wy == "t")) >> 16
        ld("r1", x); ld("r2", y); flags(0)
        emit("    smulw%s r0, r1, r2" % wy)
        tag = "smulw%s r0,r1,r2  r1=%08x r2=%08x" % (wy, x, y)
        store("r0", tag + " result", p & M); store_cpsr(tag, 0)
        for acc in (0x7FFFFFF0, 0x80000000):
            ld("r1", x); ld("r2", y); ld("r3", acc); flags(0)
            emit("    smlaw%s r0, r1, r2, r3" % wy)
            r = p + s32(acc); q = int(r != s32(r & M))
            tag = "smlaw%s r0,r1,r2,r3  r1=%08x r2=%08x r3=%08x" % (wy, x, y, acc)
            store("r0", tag + " result", r & M); store_cpsr(tag, q)

# 5. (extra) aliasing, high registers, conditional execution, MSR fields that set Q
if EXTRA:
    for op in ("qdadd", "qdsub", "qadd"):
        for a, b in ((1, 0x10), (-1, 0x40000000), (0x7FFFFFFF, -0x40000001), (0x12345678, -0x50000000), (-5, 3)):
            for form, rd, rm, rn, va, vb in (("rd=rm", "r1", "r1", "r2", a, b), ("rd=rn", "r2", "r1", "r2", a, b),
                                             ("rm=rn", "r0", "r1", "r1", a, a), ("hi", "r12", "r9", "r8", a, b),
                                             ("lr", "lr", "r8", "r12", a, b)):
                ld(rm, va); ld(rn, vb) if rn != rm else None; flags(0)
                emit("    %s %s, %s, %s" % (op, rd, rm, rn))
                r, q = qop(op, va, vb)
                tag = "x%s %s,%s,%s [%s] a=%08x b=%08x" % (op, rd, rm, rn, form, va & M, vb & M)
                store(rd, tag + " result", r); store_cpsr(tag, q)
        for cond, z, run in (("ne", 1, 0), ("eq", 1, 1)):
            ld("r0", 0xDEADBEEF); ld("r1", 1); ld("r2", 0x50000000); flags(nzcvq(0, z, 0, 0, 0))
            emit("    %s%s r0, r1, r2" % (op, cond))
            r, q = qop(op, 1, 0x50000000)
            tag = "x%s%s r0,r1,r2 Z=%d" % (op, cond, z)
            store("r0", tag + " result", r if run else 0xDEADBEEF); store_cpsr(tag, nzcvq(0, z, 0, 0, q if run else 0))
    # MSR: flags-only fields must write Q (set and clear), immediate and register forms, f+s / f+x, conditional
    for txt, ins, pre, exp in (("msr cpsr_f,#0x08000000", ["    msr cpsr_f, #0x08000000"], 0, 0x01),
                               ("msr cpsr_f,#0x48000000", ["    msr cpsr_f, #0x48000000"], 0x10, 0x09),
                               ("msr cpsr_f,r5=0xa8000000", ["    ldr r5, =0xa8000000", "    msr cpsr_f, r5"], 0, 0x15),
                               ("msr cpsr_f,r5=0 (Q was 1)", ["    mov r5, #0", "    msr cpsr_f, r5"], 0x1F, 0),
                               ("msr cpsr_fs,#0x08000000", ["    msr cpsr_fs, #0x08000000"], 0, 0x01),
                               ("msr cpsr_fx,r5=0", ["    mov r5, #0", "    msr cpsr_fx, r5"], 0x01, 0),
                               ("msrne cpsr_f,#0x08000000 Z=1 (skipped)", ["    msrne cpsr_f, #0x08000000"], 0x08, 0x08),
                               ("msreq cpsr_f,#0x08000000 Z=1", ["    msreq cpsr_f, #0x08000000"], 0x08, 0x01)):
        flags(pre); emit(*ins); store_cpsr("x" + txt, exp)
    # Q set by MSR, then a non-saturating QADD must keep it (sticky)
    flags(0); emit("    msr cpsr_f, #0x08000000"); ld("r1", 1); ld("r2", 2); emit("    qadd r0, r1, r2")
    store("r0", "xmsrQ then qadd 1+2 result", 3); store_cpsr("xmsrQ then qadd 1+2", 1)
    # SMLAxy / SMLALxy with aliased registers and negative accumulators
    for x, y in ((0xFFFF0001, 0x0002FFFF), (0x80007FFF, 0x7FFF8000)):
        for xy in ("bb", "tt", "bt", "tb"):
            p = h(x, xy[0] == "t") * h(y, xy[1] == "t")
            for acc in (0x80000000, 0xFFFFFFF0, 0x7FFFFFFF):
                ld("r1", x); ld("r2", y); ld("r3", acc); flags(0)
                emit("    smla%s r3, r1, r2, r3" % xy)
                r = p + s32(acc); q = int(r != s32(r & M))
                tag = "xsmla%s r3,r1,r2,r3 r1=%08x r2=%08x r3=%08x" % (xy, x, y, acc)
                store("r3", tag + " result", r & M); store_cpsr(tag, q)
            for lo, hi in ((0xFFFFFFF0, 0xFFFFFFFF), (0x80000000, 0x7FFFFFFF)):
                ld("r5", x); ld("r6", y); flags(0)
                emit("    smlal%s r5, r6, r5, r6" % xy)   # RdLo=Rm, RdHi=Rs: acc = (Rs:Rm) + p
                accv = ((y << 32 | x) + p) & ((1 << 64) - 1)
                tag = "xsmlal%s r5,r6,r5,r6 r5=%08x r6=%08x" % (xy, x, y)
                store("r5", tag + " lo", accv & M); store("r6", tag + " hi", accv >> 32); store_cpsr(tag, 0)
                ld("r9", x); ld("r12", y); ld("r8", lo); ld("lr", hi); flags(0x1F)
                emit("    smlal%s r8, lr, r9, r12" % xy)
                accv = ((hi << 32 | lo) + p) & ((1 << 64) - 1)
                tag = "xsmlal%s r8,lr,r9,r12 r9=%08x r12=%08x lo=%08x hi=%08x" % (xy, x, y, lo, hi)
                store("r8", tag + " lo", accv & M); store("lr", tag + " hi", accv >> 32); store_cpsr(tag, 0x1F)
    # MOVS ASR by register with aliasing
    for x, n in ((0x80000001, 1), (0x40000000, 31), (0xF0, 4), (0x2, 2)):
        for cin in (0, 1):
            ld("r1", x); ld("r2", n); flags(nzcvq(0, 0, cin, 0, 0)); emit("    movs r2, r1, asr r2")
            res, c = shift("asr", x, n, cin)
            tag = "xmovs r2,r1,asr r2 r1=%08x n=%d Cin=%d" % (x, n, cin)
            store("r2", tag + " result", res); store_cpsr(tag, nzcvq(res >> 31, int(res == 0), c, 0, 0))
            ld("r9", x); ld("r12", n); flags(nzcvq(0, 0, cin, 0, 0)); emit("    movs r9, r9, asr r12")
            store("r9", tag.replace("r2,r1,asr r2", "r9,r9,asr r12") + " result", res)
            store_cpsr(tag.replace("r2,r1,asr r2", "r9,r9,asr r12"), nzcvq(res >> 31, int(res == 0), c, 0, 0))

# pools must stay within 4 KB of the ldr: flush one every ~60 tests
out = []; n = 0
for l in asm:
    out.append(l)
    if l.startswith("    mrs"): n += 1
    if n >= 40:
        out += ["    b 1f", "    .ltorg", "1:"]; n = 0
src = """    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    msr cpsr_c, #0xDF
    ldr sp, =0x023F0000
    ldr r11, =0x02200100
""" + "\n".join(out) + """
    ldr r0, =0x02200000
    ldr r1, =0x02200100
    sub r1, r11, r1
    lsr r1, r1, #2
    str r1, [r0, #4]
    ldr r1, =0x454E4F44
    str r1, [r0]
2:  b 2b
    .ltorg
"""
open(os.path.join(HERE, "diff9.s"), "w").write(src)
with open(os.path.join(OUT, "tests.txt"), "w") as f:
    for t, e in desc: f.write("%s\t%s\n" % (t, "-" if e is None else "%08x" % (e & M)))

LD = "ENTRY(_start)\nSECTIONS { . = 0x02000000; .text : { *(.text.start) *(.text*) } }\n"
open(os.path.join(OUT, "ld9.ld"), "w").write(LD)
def run(cmd): subprocess.run(cmd, check=True, cwd=HERE)
O = os.path.basename(OUT)
run(["clang", "--target=armv5te-none-eabi", "-c", "diff9.s", "-o", O + "/diff9.o"])
run(["ld.lld", "-T", O + "/ld9.ld", O + "/diff9.o", "-o", O + "/arm9.elf"])
run(["llvm-objcopy", "-O", "binary", O + "/arm9.elf", O + "/arm9.bin"])
arm9 = bytearray(open(os.path.join(OUT, "arm9.bin"), "rb").read())
arm7 = struct.pack("<5I", 0xE59F0008, 0xE3A01080, 0xE5C01001, 0xEAFFFFFB, 0x04000300)
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
a9 = arm9 + b"\0" * (-len(arm9) % 0x200)
a9off = 0x8000; a7off = a9off + len(a9)
body = bytearray(0x8000) + a9 + arm7
body += b"\0" * (-len(body) % 0x200)
hd = bytearray(0x200)
hd[0:12] = b"JITDIFF".ljust(12, b"\0"); hd[12:16] = b"JDIF"; hd[16:18] = b"00"
chip = 0x20000
while chip < len(body): chip *= 2
hd[0x14] = (chip // 0x20000).bit_length() - 1
struct.pack_into("<8I", hd, 0x20, a9off, 0x02000000, 0x02000000, len(a9), a7off, 0x02380000, 0x02380000, len(arm7))
struct.pack_into("<2I", hd, 0x60, 0x00586000, 0x001808F8)
struct.pack_into("<2I", hd, 0x80, len(body), 0x200)
struct.pack_into("<H", hd, 0x15E, crc16(hd[:0x15E]))
body[0:0x200] = hd
body += b"\xff" * (chip - len(body))
open(os.path.join(OUT, "diff_extra.nds" if EXTRA else "diff.nds"), "wb").write(body)
print("arm9 %d bytes, %d result words -> %s" % (len(arm9), len(desc), os.path.join(OUT, "diff_extra.nds" if EXTRA else "diff.nds")))
