#!/usr/bin/env python3
"""tgen.py <cpu 9|7> <name> <group,...>: differential-test ROM generator (interpreter vs JIT), run on the ARM9 or the ARM7.
Groups: thumb (Thumb ALU/shifts/BL/BX/pc-rel/LDM/STM/PUSH/POP), storepc (STR/STM with PC), shift (ARM movs Rd,Rm,<sh> Rs),
alu (ARM ALU with flags, multiplies), mem (ARM loads/stores incl. unaligned, LDM/STM base-in-list).
Results: words at 0x02200100.., 'DONE' at 0x02200000, count at 0x02200004 (same as ../diff). Writes out/<name>.tests.txt
(description, expected value for this CPU's architecture: ARMv5TE for the ARM9, ARMv4T for the ARM7; '-' = no model)."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
CPU = sys.argv[1]; NAME = sys.argv[2]; GROUPS = sys.argv[3].split(",")
V5 = CPU == "9"
M = 0xFFFFFFFF
def s32(x): x &= M; return x - (1 << 32) if x & 0x80000000 else x
def ror(x, r): r &= 31; x &= M; return ((x >> r) | (x << (32 - r))) & M if r else x
asm = []; desc = []; tl = [0]
def emit(*l): asm.extend(l)
def ld(r, v): emit("    ldr %s, =0x%08x" % (r, v & M))
def flags(nzcvq): emit("    mrs r10, cpsr", "    bic r10, r10, #0xF8000000", "    orr r10, r10, #0x%08x" % (nzcvq << 27), "    msr cpsr_fc, r10")
def store(r, text, exp=None): emit("    str %s, [r11], #4" % r); desc.append((text, exp))
def store_cpsr(text, exp=None): emit("    mrs r10, cpsr", "    lsr r10, r10, #27"); store("r10", text + " NZCVQ", exp)
def nz(n, z, c, v, q=0): return n << 4 | z << 3 | c << 2 | v << 1 | q
def NZ(res, c, v): res &= M; return nz(res >> 31, int(res == 0), c, v)
def label(): tl[0] += 1; return "L%d" % tl[0]
def thumb_call(body, pre=None):
    """call a Thumb snippet: ARM side ldr r12,=T ; mov lr,pc ; bx r12. Snippet must end by returning with bx lr / bx r7."""
    t = label(); skip = label()
    emit("    ldr r12, =%s" % t, "    mov lr, pc", "    bx r12", "    b %s" % skip, "    .thumb", "    .balign 4",
         "    .thumb_func", "%s:" % t, *["    " + x for x in body], "    .arm", "    .balign 4", "%s:" % skip)
    return t
def shift(kind, x, n, cin):
    x &= M
    if n == 0: return x, cin
    if kind == "lsl":
        if n < 32: return (x << n) & M, (x >> (32 - n)) & 1
        return (0, x & 1) if n == 32 else (0, 0)
    if kind == "lsr":
        if n < 32: return x >> n, (x >> (n - 1)) & 1
        return (0, x >> 31) if n == 32 else (0, 0)
    if kind == "asr":
        if n < 32: return (s32(x) >> n) & M, (x >> (n - 1)) & 1
        return (M if x >> 31 else 0), x >> 31
    r = n & 31
    if r == 0: return x, x >> 31
    return ror(x, r), (x >> (r - 1)) & 1
def addf(a, b, c=0):
    r = (a & M) + (b & M) + c; res = r & M
    v = int(((a ^ res) & (b ^ res) & 0x80000000) != 0)
    return res, int(r >> 32), v
def subf(a, b, c=1): return addf(a, (~b) & M, c)
SHV = (0x1, 0x2, 0x80000000, 0x80000001, 0x40000000, 0xF0)
SHN = (0, 1, 2, 31, 32, 33, 0x101)

if "thumb" in GROUPS:
    # Thumb register shifts (LSL/LSR/ASR/ROR Rd,Rs): converted to ARM movs rd,rd,<sh> rs by convert_thumb_instruction_to_arm
    for kind in ("asr", "lsr", "lsl", "ror"):
        for x in SHV:
            for n in SHN:
                for cin in (0, 1):
                    ld("r0", x); ld("r1", n); flags(nz(0, 0, cin, 1))
                    thumb_call(["%ss r0, r1" % kind, "bx lr"])
                    res, c = shift(kind, x, n & 0xFF, cin)
                    tag = "T %ss r0,r1 r0=%08x r1=%x Cin=%d" % (kind, x, n, cin)
                    store("r0", tag + " result", res); store_cpsr(tag, NZ(res, c, 1))
    # Thumb immediate shifts
    for ins, kind, n in (("lsls r0, r1, #0", "lsl", 0), ("lsls r0, r1, #1", "lsl", 1), ("lsrs r0, r1, #1", "lsr", 1),
                         ("lsrs r0, r1, #32", "lsr", 32), ("asrs r0, r1, #31", "asr", 31), ("asrs r0, r1, #32", "asr", 32)):
        for x in (0x80000001, 0x40000002):
            for cin in (0, 1):
                ld("r1", x); flags(nz(0, 0, cin, 1)); thumb_call([ins, "bx lr"])
                res, c = shift(kind, x, n, cin)
                tag = "T %s r1=%08x Cin=%d" % (ins, x, cin)
                store("r0", tag + " result", res); store_cpsr(tag, NZ(res, c, 1))
    # Thumb ALU with flags
    vals = ((0x7FFFFFFF, 1), (0xFFFFFFFF, 1), (0x80000000, 0x80000000), (5, 7), (0, 0), (0x12345678, 0xEDCBA988))
    for a, b in vals:
        for cin in (0, 1):
            cases = [("adds r0, r0, r1", addf(a, b)), ("subs r0, r0, r1", subf(a, b)), ("adcs r0, r1", addf(a, b, cin)),
                     ("sbcs r0, r1", subf(a, b, cin)), ("negs r0, r1", subf(0, b)), ("cmp r0, r1", subf(a, b)),
                     ("cmn r0, r1", addf(a, b))]
            for ins, (res, c, v) in cases:
                ld("r0", a); ld("r1", b); flags(nz(0, 0, cin, 1 - cin)); thumb_call([ins, "bx lr"])
                tag = "T %s r0=%08x r1=%08x Cin=%d" % (ins, a, b, cin)
                if not ins.startswith(("cmp", "cmn")): store("r0", tag + " result", res)
                store_cpsr(tag, NZ(res, c, v))
            lcases = [("ands r0, r1", a & b), ("eors r0, r1", a ^ b), ("orrs r0, r1", a | b), ("bics r0, r1", a & ~b & M),
                      ("mvns r0, r1", ~b & M), ("tst r0, r1", a & b)]
            if V5: lcases.append(("muls r0, r1", (a * b) & M))     # ARMv5: MULS leaves C (and V) unchanged
            for ins, res in lcases:
                ld("r0", a); ld("r1", b); flags(nz(0, 0, cin, 1 - cin)); thumb_call([ins, "bx lr"])
                tag = "T %s r0=%08x r1=%08x C=%d V=%d" % (ins, a, b, cin, 1 - cin)
                if not ins.startswith("tst"): store("r0", tag + " result", res)
                store_cpsr(tag, NZ(res, cin, 1 - cin))
            for ins, (res, c, v) in (("adds r0, #200", addf(a, 200)), ("subs r0, #200", subf(a, 200)), ("adds r0, r1, #7", addf(b, 7)),
                                     ("subs r0, r1, #7", subf(b, 7)), ("cmp r0, #0x80", subf(a, 0x80))):
                ld("r0", a); ld("r1", b); flags(nz(0, 0, cin, 0)); thumb_call([ins, "bx lr"])
                tag = "T %s r0=%08x r1=%08x" % (ins, a, b)
                if not ins.startswith("cmp"): store("r0", tag + " result", res)
                store_cpsr(tag, NZ(res, c, v))
    # hi registers: add/cmp/mov with r8 (mov/add hi do not touch flags; cmp hi sets them)
    ld("r8", 0x40000000); ld("r1", 0x40000000); ld("r0", 0); flags(nz(1, 0, 1, 0))
    thumb_call(["add r8, r1", "mov r0, r8", "bx lr"]); store("r0", "T add r8,r1 ; mov r0,r8", 0x80000000); store_cpsr("T add/mov hi (flags kept)", nz(1, 0, 1, 0))
    ld("r8", 5); ld("r1", 6); flags(0); thumb_call(["cmp r8, r1", "bx lr"]); store_cpsr("T cmp r8,r1 (5-6)", NZ(-1, 0, 0))
    # PC-relative: ldr r0,[pc,#4], add r0,pc,#4 (adr), mov r0,pc, add r0,pc at word-aligned (A) and halfword (A+2) addresses;
    # the ARM side stores result - T (T = snippet start) or the loaded word
    LIT = [".hword 0x46c0", ".hword 0x46c0", ".word 0x11111111", ".word 0x22222222"]
    for al in (0, 2):
        pad = [".hword 0x46c0"] if al else []       # nop = mov r8,r8
        t = thumb_call(pad + [".hword 0x4801", ".hword 0x4770"] + ([] if al else [".hword 0x46c0"]) + [".hword 0x46c0", ".word 0x11111111", ".word 0x22222222"])
        store("r0", "T ldr r0,[pc,#4] at T+%d" % al, 0x11111111)
        for enc, nm, exp in ((0xA001, "add r0,pc,#4", 8), (0x4678, "mov r0,pc", al + 4), (0x4478, "add r0,pc (r0=0)", al + 4)):
            ld("r0", 0)
            t = thumb_call(pad + [".hword 0x%04x" % enc, ".hword 0x4770", ".hword 0x46c0", ".hword 0x46c0"])
            emit("    ldr r1, =%s" % t, "    bic r1, r1, #1", "    sub r0, r0, r1"); store("r0", "T %s at T+%d: result-T" % (nm, al), exp)
    # BL pair: LR value and return
    ld("r1", 0x100); ld("r2", 0)
    t = thumb_call(["mov r7, lr", "bl 1f", "adds r1, r1, #1", "bx r7", "1: mov r2, lr", "movs r0, #0x5a", "bx lr"])
    emit("    ldr r3, =%s" % t, "    bic r3, r3, #1", "    sub r2, r2, r3")
    store("r0", "T bl pair: callee r0", 0x5a); store("r1", "T bl pair: returned (r1+1)", 0x101); store("r2", "T bl pair: LR-T", 7)
    # lone BL suffix (0xF802: pc = LR + 4, LR = next|1); LR preset by mov lr,r3 with r3 = Tgt-4
    tg = label()
    ld("r0", 0); ld("r2", 0); emit("    ldr r3, =%s" % tg, "    bic r3, r3, #1", "    sub r3, r3, #4")
    t = thumb_call(["mov r7, lr", "mov lr, r3", ".hword 0xF802", "movs r0, #1", "bx r7", "movs r0, #2", "bx r7",
                    "%s: movs r0, #3" % tg, "mov r2, lr", "bx r7"])
    emit("    ldr r3, =%s" % t, "    bic r3, r3, #1", "    sub r2, r2, r3")
    store("r0", "T lone BL suffix: landing (1=next 2=next+4 [JIT bug guess] 3=LR+4 correct)", 3)
    store("r2", "T lone BL suffix: new LR - T (next|1)", 7)
    # lone BL prefix + suffix split by being a branch target: prefix sets LR = pc+4+(off<<12); suffix reached via b
    # (instead: two halves separated by a 'b' over nothing is still adjacent; skip)
    if V5:
        # BLX imm (Thumb -> ARM) and back
        af = label(); tk = label()
        emit("    b %s" % tk, "%s: mov r0, #0x77" % af, "    mov r2, lr", "    bx lr", "%s:" % tk)
        ld("r2", 0)
        t = thumb_call(["mov r7, lr", "blx %s" % af, "adds r0, r0, #1", "bx r7"])
        emit("    ldr r3, =%s" % t, "    bic r3, r3, #1", "    sub r2, r2, r3")
        store("r0", "T blx imm to ARM, back (0x77+1)", 0x78); store("r2", "T blx imm: LR-T", 7)
        # BLX reg to ARM
        ld("r2", 0); emit("    ldr r1, =%s" % af)
        t = thumb_call(["mov r7, lr", "blx r1", "adds r0, r0, #2", "bx r7"])
        emit("    ldr r3, =%s" % t, "    bic r3, r3, #1", "    sub r2, r2, r3")
        store("r0", "T blx r1 (ARM), back (0x77+2)", 0x79); store("r2", "T blx r1: LR-T", 5)
    # BX reg to ARM (bit0=0) and to Thumb (bit0=1)
    af2 = label(); tk = label(); tt = label()
    emit("    b %s" % tk, "%s: mov r0, #0x41" % af2, "    bx r7", "%s:" % tk)
    emit("    ldr r1, =%s" % af2); ld("r0", 0)
    thumb_call(["mov r7, lr", "bx r1", "movs r0, #1", "bx r7"]); store("r0", "T bx r1 -> ARM", 0x41)
    emit("    ldr r1, =%s" % tt); ld("r0", 0)
    thumb_call(["mov r7, lr", "bx r1", "movs r0, #1", "bx r7", ".thumb_func", "%s: movs r0, #0x42" % tt, "bx r7"]); store("r0", "T bx r1 -> Thumb", 0x42)
    # BX PC (aligned): to ARM at A+8 (mov r7,lr at A, nop, bx pc at A+4)
    ld("r0", 0)
    thumb_call(["mov r7, lr", ".hword 0x46c0", "bx pc", ".hword 0x46c0", ".arm", "mov r0, #0x43", "bx r7", ".thumb"]); store("r0", "T bx pc -> ARM", 0x43)
    # MOV PC,Rm (hi) stays Thumb ; ADD PC,Rm
    tm = label(); ld("r0", 0); emit("    ldr r1, =%s" % tm, "    orr r1, r1, #1")
    thumb_call(["mov r7, lr", "mov pc, r1", "movs r0, #1", "bx r7", ".balign 2", "%s: movs r0, #0x44" % tm, "bx r7"]); store("r0", "T mov pc,r1 (stays Thumb)", 0x44)
    ld("r0", 0); ld("r1", 4)
    thumb_call(["mov r7, lr", "add pc, r1", "movs r0, #1", "bx r7", "movs r0, #2", "adds r0, #0x10", "bx r7"])
    store("r0", "T add pc,r1 (r1=4): 0x10 = landed at pc+4+4 (correct), 0x12 = pc+4+2, 1 = no jump", 0x10)
    # LDMIA/STMIA/PUSH/POP
    BUF = 0x02210000
    emit("    ldr r4, =0x%08x" % BUF)
    for i, v in enumerate((0xA0A0A0A0, 0xB1B1B1B1, 0xC2C2C2C2, 0xD3D3D3D3)): ld("r5", v); emit("    str r5, [r4, #%d]" % (4 * i))
    emit("    mov r0, r4"); ld("r1", 0); ld("r2", 0)
    thumb_call(["ldmia r0!, {r1, r2}", "bx lr"])
    emit("    sub r0, r0, r4"); store("r0", "T ldmia r0!,{r1,r2}: wb-base", 8); store("r1", "T ldmia r1", 0xA0A0A0A0); store("r2", "T ldmia r2", 0xB1B1B1B1)
    emit("    mov r0, r4"); ld("r1", 0)
    thumb_call([".hword 0xC803  @ ldmia r0!,{r0,r1}", "bx lr"])
    store("r0", "T ldmia r0!,{r0,r1}: r0 (loaded value wins, no wb)", 0xA0A0A0A0); store("r1", "T ldmia r0!,{r0,r1}: r1", 0xB1B1B1B1)
    emit("    mov r0, r4"); ld("r1", 0x11111111); ld("r2", 0x22222222)
    thumb_call(["stmia r0!, {r1, r2}", "bx lr"])
    emit("    sub r0, r0, r4"); store("r0", "T stmia r0!,{r1,r2}: wb-base", 8)
    emit("    ldr r5, [r4]"); store("r5", "T stmia mem0", 0x11111111); emit("    ldr r5, [r4, #4]"); store("r5", "T stmia mem1", 0x22222222)
    emit("    mov r0, r4"); ld("r1", 0x33333333)
    thumb_call([".hword 0xC003  @ stmia r0!,{r0,r1}", "bx lr"])
    emit("    sub r0, r0, r4"); store("r0", "T stmia r0!,{r0,r1}: wb-base", 8)
    emit("    ldr r5, [r4]", "    sub r5, r5, r4"); store("r5", "T stmia r0!,{r0,r1}: stored r0 - base (0 = original)", 0)
    emit("    ldr r5, [r4, #4]"); store("r5", "T stmia r0!,{r0,r1}: mem1", 0x33333333)
    # push {r0,r1,lr} / pop {r2,r3,r5}: values and SP
    emit("    mov r6, sp"); ld("r0", 0x1234); ld("r1", 0x5678)
    t = thumb_call(["mov r7, lr", "ldr r4, =0xCAFE0001", "mov lr, r4", "push {r0, r1, lr}", "movs r4, #0", "pop {r2, r3, r5}", "mov r0, sp", "bx r7", ".ltorg"])
    emit("    sub r0, r6, r0"); store("r0", "T push/pop: sp delta", 0); store("r2", "T pop r2", 0x1234); store("r3", "T pop r3", 0x5678); store("r5", "T pop r5 (pushed lr)", 0xCAFE0001)
    # pop {r0,pc} to a Thumb target (both arches) and an ARM target (ARMv5 interworks)
    tp = label(); ld("r1", 0x5555); emit("    ldr r2, =%s" % tp, "    orr r2, r2, #1", "    push {r1, r2}"); ld("r3", 0)
    thumb_call(["pop {r0, pc}", "movs r3, #1", "bx lr", ".balign 2", "%s: movs r3, #0x7b" % tp, "bx lr"])
    store("r0", "T pop {r0,pc} (Thumb target): r0", 0x5555); store("r3", "T pop {r0,pc} -> Thumb target", 0x7B)
    if V5:
        ap = label(); tk = label()
        emit("    b %s" % tk, "%s: mov r3, #0xA7" % ap, "    bx lr", "%s:" % tk)
        ld("r1", 0x6666); emit("    ldr r2, =%s" % ap, "    push {r1, r2}"); ld("r3", 0)
        thumb_call(["pop {r0, pc}", "movs r3, #1", "bx lr"])
        store("r0", "T pop {r0,pc} (ARM target): r0", 0x6666); store("r3", "T pop {r0,pc} -> ARM target (v5 interworking)", 0xA7)
    # Thumb unaligned LDR (rotation) and LDRH
    emit("    ldr r4, =0x%08x" % BUF); ld("r5", 0x44332211); emit("    str r5, [r4]"); ld("r5", 0x88776655); emit("    str r5, [r4, #4]")
    for off in (1, 2, 3):
        emit("    add r1, r4, #%d" % off); thumb_call(["ldr r0, [r1]", "bx lr"]); store("r0", "T ldr r0,[r1] unaligned +%d (rotated)" % off, ror(0x44332211, 8 * off))
    emit("    add r1, r4, #1"); thumb_call(["ldrh r0, [r1]", "bx lr"])
    store("r0", "T ldrh r0,[r1] odd addr (v5: aligned 0x2211; v4: ror 8)", 0x2211 if V5 else ror(0x2211, 8))
    emit("    add r1, r4, #1"); ld("r2", 0); thumb_call(["ldrsh r0, [r1, r2]", "bx lr"])
    store("r0", "T ldrsh odd addr (v5: sext 0x2211; v4: ldrsb of 0x22)", 0x2211 if V5 else 0x22)

if "storepc" in GROUPS:
    BUF = 0x02210100
    emit("    ldr r4, =0x%08x" % BUF)
    for nm, body, off in (("str pc,[r4]", ["str pc, [r4]"], 0), ("str pc,[r4,#4]!", ["mov r5, r4", "str pc, [r5, #4]!"], 4),
                          ("stmia r4,{pc}", ["stmia r4, {pc}"], 0), ("stmia r4,{r0,pc}", ["stmia r4, {r0, pc}"], 4),
                          ("stmdb sp!,{r0,pc}", ["mov r5, sp", "stmdb sp!, {r0, pc}", "mov r4, sp", "mov sp, r5"], 4)):
        l = label()
        emit("    ldr r4, =0x%08x" % BUF, *(["    %s" % x for x in body[:-1]] if nm.startswith("str pc,[r4,#4]") or nm.startswith("stmdb") else []))
        if nm.startswith("stmdb"):
            emit("    mov r5, sp", "%s: stmdb sp!, {r0, pc}" % l, "    mov r6, sp", "    mov sp, r5", "    ldr r1, [r6, #4]")
        elif nm.startswith("str pc,[r4,#4]"):
            emit("%s: str pc, [r5, #4]!" % l, "    ldr r1, [r4, #4]")
        else:
            emit("%s: %s" % (l, body[0]), "    ldr r1, [r4, #%d]" % off)
        emit("    ldr r2, =%s" % l, "    sub r1, r1, r2"); store("r1", "STOREPC %s: stored - insn addr (GBATEK: 12)" % nm, 12)
    # reads of PC for comparison
    l = label(); emit("%s: mov r1, pc" % l, "    ldr r2, =%s" % l, "    sub r1, r1, r2"); store("r1", "STOREPC mov r1,pc: value - insn addr", 8)
    l = label(); ld("r3", 0); emit("%s: add r1, pc, r3, lsl r3" % l, "    ldr r2, =%s" % l, "    sub r1, r1, r2")
    store("r1", "STOREPC add r1,pc,r3,lsl r3 (reg shift, ARM ARM unpredictable; ARM7TDMI +12)", None)
    # Thumb push {lr} after bl not relevant; Thumb str of pc impossible (lo regs only)

if "shift" in GROUPS:
    for kind in ("asr", "lsr", "lsl", "ror"):
        for x in SHV:
            for n in SHN:
                for cin in (0, 1):
                    ld("r1", x); ld("r2", n); flags(nz(0, 0, cin, 1))
                    emit("    movs r0, r1, %s r2" % kind)
                    res, c = shift(kind, x, n & 0xFF, cin)
                    tag = "movs r0,r1,%s r2  r1=%08x r2=%x Cin=%d" % (kind, x, n, cin)
                    store("r0", tag + " result", res); store_cpsr(tag, NZ(res, c, 1))

if "alu" in GROUPS:
    vals = ((0x7FFFFFFF, 1), (0xFFFFFFFF, 1), (0x80000000, 0x80000000), (5, 7), (0, 0), (0x12345678, 0xEDCBA988))
    for a, b in vals:
        for cin in (0, 1):
            for ins, (res, c, v) in (("adds", addf(a, b)), ("adcs", addf(a, b, cin)), ("subs", subf(a, b)), ("sbcs", subf(a, b, cin)),
                                     ("rsbs", subf(b, a)), ("rscs", subf(b, a, cin))):
                ld("r1", a); ld("r2", b); flags(nz(0, 0, cin, 1 - cin)); emit("    %s r0, r1, r2" % ins)
                tag = "%s r0,r1,r2 r1=%08x r2=%08x Cin=%d" % (ins, a, b, cin)
                store("r0", tag + " result", res); store_cpsr(tag, NZ(res, c, v))
            for ins, res, sc in (("ands r0, r1, r2, lsl #1", a & ((b << 1) & M), (b >> 31) & 1), ("eors r0, r1, r2, lsr #1", a ^ (b >> 1), b & 1),
                                 ("orrs r0, r1, r2", a | b, cin), ("bics r0, r1, r2, ror #4", a & ~ror(b, 4) & M, (b >> 3) & 1),
                                 ("mvns r0, r2, asr #31", ~((s32(b) >> 31) & M) & M, (b >> 30) & 1), ("movs r0, r2, rrx", (b >> 1) | (cin << 31), b & 1)):
                ld("r1", a); ld("r2", b); flags(nz(0, 0, cin, 1 - cin)); emit("    " + ins)
                tag = "%s r1=%08x r2=%08x C=%d" % (ins, a, b, cin)
                store("r0", tag + " result", res); store_cpsr(tag, NZ(res, sc, 1 - cin))
            for ins, res, sc in (("tst r1, r2", a & b, cin), ("teq r1, r2", a ^ b, cin), ("tst r1, #0xFF", a & 0xFF, cin),
                                 ("tst r1, #0xF0000000", a & 0xF0000000, 1), ("teq r1, r2, lsr #1", a ^ (b >> 1), b & 1),
                                 ("tst r1, r2, lsl #0", a & b, cin)):
                ld("r1", a); ld("r2", b); flags(nz(0, 0, cin, 1 - cin)); emit("    " + ins)
                store_cpsr("%s r1=%08x r2=%08x C=%d V=%d" % (ins, a, b, cin, 1 - cin), NZ(res, sc, 1 - cin))
            # multiplies: results always; flags only N,Z modelled (C: v5 unchanged, v4 unpredictable -> no model)
            for ins, regs in (("muls r0, r1, r2", 1), ("mlas r0, r1, r2, r3", 1), ("umulls r0, r3, r1, r2", 2), ("smulls r0, r3, r1, r2", 2),
                              ("umlals r0, r3, r1, r2", 2), ("smlals r0, r3, r1, r2", 2)):
                ld("r1", a); ld("r2", b); ld("r0", 0x11111111); ld("r3", 0x00000002); flags(nz(0, 0, cin, 1 - cin)); emit("    " + ins)
                op = ins.split()[0]
                if op == "muls": r = (a * b) & M
                elif op == "mlas": r = (a * b + 2) & M
                elif op == "umulls": r = a * b
                elif op == "smulls": r = (s32(a) * s32(b)) & ((1 << 64) - 1)
                elif op == "umlals": r = (a * b + (2 << 32 | 0x11111111)) & ((1 << 64) - 1)
                else: r = (s32(a) * s32(b) + (2 << 32 | 0x11111111)) & ((1 << 64) - 1)
                tag = "%s r1=%08x r2=%08x C=%d" % (ins, a, b, cin)
                store("r0", tag + " lo", r & M)
                if regs == 2: store("r3", tag + " hi", r >> 32)
                n_ = (r >> (63 if regs == 2 else 31)) & 1; z_ = int(r == 0)
                store_cpsr(tag, nz(n_, z_, cin, 1 - cin) if V5 else None)

if "mem" in GROUPS:
    BUF = 0x02210200
    emit("    ldr r4, =0x%08x" % BUF)
    for i, v in enumerate((0x44332211, 0x88776655, 0xCCBBAA99, 0x00FFEEDD)): ld("r5", v); emit("    str r5, [r4, #%d]" % (4 * i))
    w = (0x44332211, 0x88776655, 0xCCBBAA99, 0x00FFEEDD)
    for off in (0, 1, 2, 3):
        emit("    ldr r0, [r4, #%d]" % off); store("r0", "ldr unaligned +%d (rotated)" % off, ror(w[0], 8 * off))
    for off in (0, 1, 2):
        emit("    ldrh r0, [r4, #%d]" % (off + 4)); hw = 0x6655 if off < 2 else 0x8877
        store("r0", "ldrh +%d (odd: v5 aligned, v4 ror 8)" % (off + 4), hw if (V5 or off % 2 == 0) else ror(hw, 8))
        emit("    ldrsh r0, [r4, #%d]" % (off + 4))
        sh = (hw ^ 0x8000) - 0x8000
        b = (w[1] >> (8 * off)) & 0xFF; sb = (b ^ 0x80) - 0x80
        store("r0", "ldrsh +%d (odd: v5 aligned, v4 = ldrsb)" % (off + 4), (sh if (V5 or off % 2 == 0) else sb) & M)
    for off in (0, 3):
        emit("    ldrsb r0, [r4, #%d]" % (off + 8)); b = (w[2] >> (8 * off)) & 0xFF
        store("r0", "ldrsb +%d" % (off + 8), ((b ^ 0x80) - 0x80) & M)
    # swp
    ld("r1", 0xDEADBEEF); emit("    add r2, r4, #16", "    swp r0, r1, [r2]", "    ldr r3, [r2]"); store("r0", "swp old (uninit)", None); store("r3", "swp new", 0xDEADBEEF)
    # LDM/STM base in list
    emit("    mov r0, r4"); emit("    .word 0xE8B00003  @ ldmia r0!,{r0,r1}"); store("r0", "ldmia r0!,{r0,r1} -> r0 (v4: loaded; v5: loaded since r0 not last? see GBATEK)", None)
    emit("    mov r1, r4"); emit("    .word 0xE8B10003  @ ldmia r1!,{r0,r1}"); emit("    sub r1, r1, r4")
    store("r1", "ldmia r1!,{r0,r1} (base last) -> r1-base", None)
    emit("    mov r1, r4", "    .word 0xE8B10006", "    sub r1, r1, r4"); store("r1", "ldmia r1!,{r1,r2} (base first) -> r1-base (v4 loaded val - base; v5 wb 8)", 8 if V5 else (w[0] - BUF) & M)
    emit("    mov r1, r4", "    ldr r0, =0x77777777", "    .word 0xE8A10003", "    ldr r2, [r4, #4]", "    sub r2, r2, r4")
    store("r2", "stmia r1!,{r0,r1} (base not first): stored r1 - base (v4: new base 8; v5: old 0)", 0 if V5 else 8)
    emit("    mov r1, r4", "    .word 0xE8A10006", "    ldr r2, [r4]", "    sub r2, r2, r4"); store("r2", "stmia r1!,{r1,r2} (base first): stored-base", 0)

if "immc" in GROUPS:   # shifter carry-out of rotated immediates (C = imm[31] when rotate != 0) and of imm shifts in TST/TEQ
    for imm in (0x80000000, 0xFF000000, 0xF0000000, 0x000003FC, 0x0000FF00, 0x000000FF, 0x40000000):
        rotated = imm > 0xFF
        for cin in (0, 1):
            for ins, res in (("tst r1, #0x%x" % imm, 0xFFFFFFFF & imm), ("teq r1, #0x%x" % imm, 0xFFFFFFFF ^ imm),
                             ("ands r0, r1, #0x%x" % imm, imm), ("movs r0, #0x%x" % imm, imm), ("orrs r0, r2, #0x%x" % imm, imm),
                             ("mvns r0, #0x%x" % imm, ~imm & M)):
                ld("r1", 0xFFFFFFFF); ld("r2", 0); flags(nz(0, 0, cin, 1)); emit("    " + ins)
                c = (imm >> 31) if rotated else cin
                store_cpsr("%s C=%d" % (ins, cin), NZ(res, c, 1))
    for sh, c in (("lsl #1", 1), ("lsr #1", 1), ("asr #1", 1), ("ror #1", 1), ("lsl #4", 0)):
        for cin in (0, 1):
            for op in ("tst", "teq"):
                ld("r1", 0x7FFFFFFF); ld("r2", 0x88888881 if sh != "lsl #4" else 0x07FFFFFF); flags(nz(0, 0, cin, 1))
                emit("    %s r1, r2, %s" % (op, sh))
                b = {"lsl #1": (0x88888881 << 1) & M, "lsr #1": 0x88888881 >> 1, "asr #1": (s32(0x88888881) >> 1) & M,
                     "ror #1": ror(0x88888881, 1), "lsl #4": (0x07FFFFFF << 4) & M}[sh]
                cc = {"lsl #1": 1, "lsr #1": 1, "asr #1": 1, "ror #1": 1, "lsl #4": 0}[sh]
                res = (0x7FFFFFFF & b) if op == "tst" else (0x7FFFFFFF ^ b)
                store_cpsr("%s r1, r2, %s r2=%08x C=%d" % (op, sh, 0x88888881 if sh != "lsl #4" else 0x07FFFFFF, cin), NZ(res, cc, 1))

# program
out = []; n = 0
for l in asm:
    out.append(l)
    if l.startswith("    str r") and "[r11]" in l: n += 1
    if n >= 30:
        out += ["    b 9f", "    .ltorg", "9:"]; n = 0
init9 = "    msr cpsr_c, #0xDF\n    ldr sp, =0x023F0000\n"
init7 = "    msr cpsr_c, #0xDF\n    ldr sp, =0x0380FE00\n"
src = """    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
""" + (init9 if V5 else init7) + """    ldr r11, =0x02200100
""" + "\n".join(out) + """
    ldr r0, =0x02200000
    ldr r1, =0x02200100
    sub r1, r11, r1
    lsr r1, r1, #2
    str r1, [r0, #4]
    ldr r1, =0x454E4F44
    str r1, [r0]
8:  b 8b
    .ltorg
"""
open(os.path.join(HERE, NAME + ".s"), "w").write(src)
idle = """    .syntax unified\n    .arm\n    .section .text.start, "ax"\n    .global _start\n_start:\n    mov r0, #0x04000000\n    mov r1, #0\n    str r1, [r0, #0x208]\n1:  b 1b\n"""
open(os.path.join(HERE, "idle.s"), "w").write(idle)
with open(os.path.join(OUT, NAME + ".tests.txt"), "w") as f:
    for t, e in desc: f.write("%s\t%s\n" % (t, "-" if e is None else "%08x" % (e & M)))
s9, s7 = (NAME + ".s", "idle.s") if V5 else ("idle.s", NAME + ".s")
subprocess.run(["python3", os.path.join(HERE, "..", "smc", "mkrom.py"), NAME, os.path.abspath(os.path.join(HERE, s9)), os.path.abspath(os.path.join(HERE, s7))], check=True)
print("%d result words" % len(desc))
