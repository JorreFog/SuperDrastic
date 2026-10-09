#!/usr/bin/env python3
"""kerngen.py [out.S]: generates rast_kern.S, the opaque pixel kernels of the fused rasterizer in AArch64 assembly.

One function per variant, rast_kern_<D><T><R><F><B>(const kargs_t *a, const uint8_t *bs, uint32_t k, uint32_t line,
uint8_t *ctx, uint32_t flags, uint8_t *id0) -> anypass (uint64), for a batch: span entries bs[0..k), bin lines line..:
  D: depth source  0 z (DDA in u64), 1 w (dW * step >> 15 + W0), 2 constant
  T: texture       0 none, 1 direct, 2 direct wrap/wrap, 3 paletted, 4 paletted wrap/wrap
  R: 0 opaque, 1 translucent (blend, id test, combine)
  F: flat colour   0 interpolated vertex colour, 1 the batch's flat colour
  B: texture filter 0 nearest (DraStic's), 1 bilinear: four texels blended with 3-bit weights, the texel fraction
     remapped through kargs.fraclut (identity, or a curve that keeps texels sharp away from their edges); not exact
Deferred opaque shading (defer.c) splits the opaque work in two: rast_kern_v<D><T> (visibility) runs the depth test,
the alpha test when the texture can fail it (T, else T = 0), writes the attribute words and marks the passing pixels
with the polygon's index in the owner buffer (kargs.owner, u16 per pixel, 512 per bin line) and does the edge
marking; rast_kern_s<T><F><B> (shade) then colours the pixels whose owner is this polygon (kargs.idx16).
Runtime flags: bit 0 affine steps (w constant), bit 1 depth-equal test, bit 2 white vertex colour (flat batches; with
bit 9 the kernels set it per line), bit 6 edge marking, bit 7 a palette of at most 16 entries (kargs.pal16; the
nearest-filtering paletted kernels then look the texels up with tbl in v18-v21 instead of loading them), bit 8 no
alpha test (the texture's lowest alpha passes it), bit 9 look for white lines (the batch's first line is white and
A is 31; other batches never take the white shortcut, which gives the same bits), bit 10 A is 31 (the alpha
modulate is the identity), bits 11 and 12 the s and t axes clamp (textures not wrapped on both); translucent only:
bit 3 alpha blending (DISP3DCNT bit 3), bit 4 fog (attr bit 15), bit 5 depth update (attr bit 11). The kernels set
bit 13 (w depth: dW fits in s16) and bit 14 (no colour delta is -32768) per line, and the textured nearest-filtering
ones (not deferred) bit 15 per batch: bits 8 and 10 both, the alpha stages' one test (fused_alpha()). Translucent kernels store each
line's first id in id0[].
The per-pixel math is that of fused_neon.c's batch_neon (opaque path), lane for lane: see there and spec/ for
the derivation of every operation. The point of generating assembly is register allocation: the group loop keeps
~20 constants and ~8 temporaries live, and clang spills and shuffles about 40 instructions per 8 pixels.

Register use in the group loop:
  v0-v3   num_l num_h den_l den_h (perspective steps); affine: v0 Rwc, v1 8 Rwc splats
  v4 v5   E0 E1 splats (8 W0, 8 dW), or the affine lanes' products i * Rwc (st = their high halves)
  v6-v9   z: za zb zs4 z8 (u64 pairs)   w: dW W0 splats   const: K splat (per batch)
  v10-v14 r0 g0 b0 (u16 splats) ub vb (s32 splats)   v15  deltas (h lanes) dr dg - db du dv tw (tw per batch)
  v16 v17 texture s and t masks (W-1, H-1)   v18-v21 bilinear or the tbl palette   v22 pid<<24 splat
  v23 bytes A aref fog pid fr fg fb   v24-v31 temporaries (v28 the group's pass mask, but in keep_dep()'s kernels)
  Where a variant leaves some of v7-v14, v16-v21 unused, they hold per-batch constants or more scratch instead
  (roles()). kernsched.py then reorders every basic block for the in-order Cortex-A55.
  x1 colour ptr  x2 attribute ptr (both advance per group)  x3 pass masks (opaque, post-incremented) / id line
  (translucent, x4 the index)  (visibility kernels: x1 the owner line; shade kernels: x2 the owner line)
  x5 the line's pixels left  x6 texels  x7 flags  x8 the pass mask (scratch outside the loop)  x9-x16 gather
  x17 Rwc  x19 kargs  x20 tail-mask table  x21 palette  x22 span entry  x23 lines left  x24 bin line  x25 ctx
  x26 id0  x27 batch flags  x28 the texel buffer (sp + 32)  x0 anypass (nonzero: some pixel passed)
The group loop (0:) starts with one basic block (head(), head_w()): the depth test, the perspective weights beside it
(computed for every group: the in-order core overlaps the two chains only in one block; with w depth, which needs
them first, the attribute words' load) and the next group's steps, so the latch carries no long-latency result into
the next group. With z or constant depth a line starts in a reduced loop (20:) that only tests, until its first
passing group sets up the weights and interpolants (27:, then the weights at 28:). Rare paths are out of line after
the latch (the affine weights 6:, the depth-equal test 24:, which with w depth also takes the affine weights and the
64-bit w products 23:, the 32-bit vertex colour 37:, the alpha modulate and alpha test 46: and the white lines' alpha
test 45:, the partial-group store 1:) so the common path has no taken branch besides the loop's.
Stack: [sp,#32] 8 texels, [sp,#64] the depth words of the group (keep_dep()'s kernels keep them in v25 v26 and
spill them only at a line's first group), [sp,#96..] spills
(bilinear: st, the five bases, the four corner weights, the four nearest-corner masks), [sp,#256] the line's pass masks
(512 bytes; the hi-res set's frame is 1040 bytes for its 768-pixel lines)."""
import os, sys
import kernsched
from kernsched import schedule

# kargs_t layout (fused_asm.c mirrors it): the per-polygon constants; everything per line comes from the span entry
K = dict(recip=0x00, recip_u=0x08, tex=0x10, pal=0x18, pid24=0x20, bytes=0x30, K=0x38, tw=0x3c,
         s_and=0x40, t_and=0x50, s_lo=0x60, t_lo=0x70, s_hi=0x80, t_hi=0x90, s_flip=0xa0, t_flip=0xb0,
         fraclut=0xc0, owner=0xd0, idx16=0xe0, lstride=0xf0, attr_off=0xf4, id_off=0xf8, id_stride=0xfc,
         owner_stride=0x100, pal16=0x110, size=0x150)
# span entry fields (stride 4 per line), see spec/edges.c; SPS = bytes per array (DraStic's 0xb0; the hi-res
# pipeline's 48-line bins use 0x100), HR = the hi-res kernel set: buffer strides from kargs instead of DraStic's
# context layout (line 0x800 bytes, attributes at +0x10000, ids at +0x20000 with 0x200 per line, owners 0x400)
SPS, HR, PFX = 0xb0, False, "rast_kern_"
FRAME = 784             # the stack frame: [sp,#256] holds a line's pass masks (512 pixels; 768 in the hi-res set)
def span_layout(stride):
    global SPS, SP
    SPS = stride
    SP = dict(W0=0, dW=stride, Z0=2 * stride, dZ=3 * stride, st0=4 * stride, dst=5 * stride, rg0=6 * stride,
              drg=7 * stride, xb=8 * stride, cdb=9 * stride, edges=10 * stride)
span_layout(0xb0)
# lanes of the delta register v15: the span's drg (dr dg) and cdb (C db) words land in .s[0] and .s[1], dst (du dv)
# in .s[2]; the texture width (per batch) in .h[6]
L_DR, L_DG, L_DB, L_DU, L_DV, L_TW = 0, 1, 3, 4, 5, 6

out = []
def e(s=""): out.append("\t" + s if s and not s.endswith(":") and not s.startswith(".") else s)

RL = {}         # the current kernel's per-batch constants in otherwise unused registers: roles()
OOLS = []       # out-of-line blocks of the group loop, emitted after its latch
KD = False      # the current kernel's keep_dep() kind: its depth words stay in v25 v26, its pass mask in x8

def keep_dep(D, T, R, F, B, M):
    """The opaque textured kernels with z or w depth and no register for the dep role kept the group's attribute
    words on the stack: spilled at the depth test, reloaded for the store (ldp q, which holds the A55's load pipe 6
    cycles, and the pid orr). "flat": the flat paletted ones (03010 04010 13010 14010; S4's and S7's main kernels,
    whose spares hold the flat colour and two modulate scratches); "lit": the lit direct-textured ones (01000 02000
    11000 12000, whose spares hold the vertex colour and a modulate scratch); "litp": the lit paletted ones (03000
    04000 13000 14000; b's modulate scratch is g's texel register v27, read by then). Instead the
    test's v25 v26 keep them (the store ORs pid in: dep_words()), freed by the pass mask, which lives in x8 only
    (the store and the partial group take it from there; the alpha test narrows x8): v28 is a modulate scratch
    (lit: v24 r's, st being spent by then, and v28 b's, instead of v26 and v25; the slow vertex colour's v26 and the
    texture masks' load, v25, take v28 too). The paletted ones ("flat", "litp") also store with st4 from v28-v31:
    r's modulate takes v24 as its scratch, g's result overwrites its texel's v29 (r's, read by then), a | fog is v31
    (b's texel, read by then) and b's result goes to v30 (the alpha texel) last, in the store's block, from its
    scratch bsc() (the white lines' 44: puts b << 6 there). test()'s callers (the lazy loop's entry, the
    depth-equal heads) spill the words too and 28: reloads them: the line setup at 27: uses v25 v26. kpath, cycles
    a group (256 colours / 16): 04010 140/137 -> 133/130, 14010 148/145 -> 142/139, 03010 157 -> 150, 13010 165 ->
    159; 02000 125 -> 122, 12000 136 -> 133, 04000 141/138 -> 136/133, 14000 149/146 -> 145/142.
    ut/kern_ab checks every kernel against a base."""
    if R or M or B or D == 2 or not T: return False
    if F: return "flat" if T in (3, 4) else False
    return "litp" if T in (3, 4) else "lit"

def uses_st(D, T, F, M):
    """the perspective weights are needed: w depth, texture or vertex colour (not flat; the visibility pass has none)"""
    return D == 1 or T or (not F and M != 1)

def roles(D, T, R, F, B, M):
    """Per-batch constants and scratch held in vector registers the variant leaves unused, instead of a dup or a
    load (or a stack spill) per group; in the order of what they save: colw (untextured flat: the colour words, fog
    bit included), caf (untextured: alpha | fog bit), trans (two registers for the translucent blend weights, which
    keeps the blend free of stack spills), idx (the polygon's index, deferred passes), dep (the group's attribute
    words dep | pid << 24, computed at the depth test; constant depth: K | pid), fog (the fog bit byte), flat (fr fg
    fb), a (A, for the alpha modulate), pid (the translucent id test), aref (the alpha test), late (untextured
    translucent: two registers for trans_store()'s tail, which the ex registers give the textured ones). The bilinear
    kernels use every register. For the in-order core (kernsched.py interleaves only what uses distinct registers)
    some kernels trade those for registers that let independent chains overlap: vc + vs (lit direct-textured opaque:
    the vertex colour, computed among the texel gather's loads, and a second modulate scratch), flat + vs (textured
    flat: the three modulates side by side), ex (textured translucent: with the blend weights' registers, which are
    free until the blend, the colour's channels and the modulates' scratch, and a second blend channel). The pool:
    v10-v12 without a vertex colour, the texture's registers without a texture or a palette, and the depth's unused
    ones (w depth v8 v9, constant depth v7-v9, the shade pass v6-v9)."""
    if B: return {}
    if T in (1, 2) and not F and R == 0 and M == 0:
        # lit, direct textures (the pool is v18-v21): the vertex colour in three registers and a second modulate scratch,
        # so the colour is computed among the texel gather's loads and the three modulates run side by side; the
        # depth words, the fog bit and A go to the stack and the bytes register instead
        return {"vc": ["v18", "v19", "v20"], "vs": ["v21"]}
    if T in (3, 4) and not F and R == 0 and M == 0:
        # lit, paletted (the palette holds v18-v21): the same with the texture masks (loaded from the arguments where
        # used) and the depth's spare registers (z: v9, the 8 z steps as 2 x v8, and pid << 24, loaded)
        x = {0: ["v9", "v22"], 1: ["v8", "v9"], 2: ["v7", "v8"]}[D]
        return {"vc": ["v16", "v17", x[0]], "vs": [x[1]]}
    pool = []
    if F or M == 1: pool += ["v10", "v11", "v12"]                               # no vertex colour
    if T == 0: pool += ["v13", "v14", "v16", "v17", "v18", "v19", "v20", "v21"]  # no texture
    elif T in (1, 2): pool += ["v18", "v19", "v20", "v21"]                      # no palette
    if M == 2: pool += ["v6", "v7", "v8", "v9"]                                 # no depth (the shade pass)
    elif D == 1: pool += ["v8", "v9"]                                           # w depth: v6 v7 only
    elif D == 2: pool += ["v7", "v8", "v9"]                                     # constant depth: v6 only
    elif T in (3, 4) and (F or R) and M == 0: pool += ["v9", "v22"]  # z, paletted: v9 (8 z steps: 2 x v8), pid << 24 (loaded)
    if T in (3, 4) and R and M == 0: pool += ["v16", "v17"]     # paletted translucent: the texture masks (loaded)
    want = []
    if T and F and R == 0 and M != 1:
        # textured flat: the flat colour and two more modulate scratches first (the three modulates side by side)
        want += [("flat", 3), ("vs", 2)]
    if T == 0 and R == 0 and M != 1: want.append(("colw", 1) if F else ("caf", 1))
    if R: want.append(("trans", 2))
    if R and T and M == 0:
        # textured translucent: two more, so that the colour's channels (in v25 and the blend weights' registers,
        # free until the blend) modulate with their own scratch registers and two blend channels run side by side
        want.append(("ex", 2))
    if M: want.append(("idx", 1))
    if M != 2: want.append(("dep", 1 if D == 2 else 2))
    if T:
        if R == 0 and M != 1: want.append(("fog", 1))
        if F and M != 1 and R: want.append(("flat", 3))
        want.append(("a", 1))
    elif R:                                                                      # (the colour stays in them)
        want.append(("a", 1))
        if F: want.append(("flat", 3))
    if R: want.append(("pid", 1))
    if T and M != 2: want.append(("aref", 1))
    if R and not T: want.append(("late", 2))    # untextured translucent: trans_store()'s tail as with the ex registers
    r = {}
    for name, n in want:
        if len(pool) >= n: r[name], pool = pool[:n], pool[n:]
    return r

def q(v): return "q" + v[1:]

def taken(reg): return any(reg in regs for regs in RL.values())

def pid24(tmp):
    """the pid << 24 splat: v22, or (v22 given to a role) loaded into tmp"""
    if not taken("v22"): return "v22"
    e(f"ldr {q(tmp)}, [x19, #{K['pid24']}]")
    return tmp

def z_step8(D):
    """the latch's 8 z steps: + v9, or (v9 given to a role) + v8 twice"""
    if D != 0: return
    if not taken("v9"): e("add v6.2d, v6.2d, v9.2d"); e("add v7.2d, v7.2d, v9.2d")
    else: e("add v6.2d, v6.2d, v8.2d"); e("add v7.2d, v7.2d, v8.2d"); e("add v6.2d, v6.2d, v8.2d"); e("add v7.2d, v7.2d, v8.2d")

def flush_ools():
    for f in OOLS:
        if f: f()
    OOLS.clear()

def roles_setup(D):
    r = RL
    if "idx" in r: e(f"ldr {q(r['idx'][0])}, [x19, #{K['idx16']}]")
    if "dep" in r and D == 2: e(f"orr {r['dep'][0]}.16b, v6.16b, v22.16b")
    for name, b in (("fog", 2), ("a", 0), ("aref", 1), ("pid", 3)):
        if name in r: e(f"dup {r[name][0]}.8b, v23.b[{b}]")
    if "flat" in r:
        for ch in range(3): e(f"dup {r['flat'][ch]}.8b, v23.b[{4 + ch}]")
    if "caf" in r:
        c = r["caf"][0]; e(f"dup {c}.8b, v23.b[0]"); e("dup v24.8b, v23.b[2]"); e(f"orr {c}.8b, {c}.8b, v24.8b")
    if "colw" in r:
        e("dup v24.8b, v23.b[0]"); e("dup v25.8b, v23.b[2]"); e("orr v24.8b, v24.8b, v25.8b")       # alpha | fog
        e("dup v25.8b, v23.b[4]"); e("dup v26.8b, v23.b[5]"); e("zip1 v25.16b, v25.16b, v26.16b")  # r g
        e("dup v26.8b, v23.b[6]"); e("zip1 v26.16b, v26.16b, v24.16b")                           # b a
        e(f"zip1 {r['colw'][0]}.8h, v25.8h, v26.8h")

def prologue(name, D, T, R, F, M=0, B=0):
    """M: 0 the kernel, 1 visibility, 2 shade"""
    e(f".globl {name}"); e(f".type {name}, %function"); e(f"{name}:")
    e("stp d8, d9, [sp, #-160]!"); e("stp d10, d11, [sp, #16]"); e("stp d12, d13, [sp, #32]"); e("stp d14, d15, [sp, #48]")
    e("stp x19, x20, [sp, #64]"); e("stp x21, x22, [sp, #80]"); e("stp x23, x24, [sp, #96]"); e("stp x25, x26, [sp, #112]")
    e("stp x27, x28, [sp, #128]")
    e(f"sub sp, sp, #{FRAME}")
    e("mov x19, x0"); e("mov x22, x1"); e("mov w23, w2"); e("mov w24, w3"); e("mov x25, x4"); e("mov w27, w5"); e("mov x26, x6")
    e("adrp x20, rast_kern_tail"); e("add x20, x20, :lo12:rast_kern_tail")
    e("add x28, sp, #32")
    if T:
        if not taken("v16"): e(f"ldr q16, [x19, #{K['s_and']}]"); e(f"ldr q17, [x19, #{K['t_and']}]")
        e(f"ldp x6, x21, [x19, #{K['tex']}]")
        if T in (3, 4) and not B:
            e("tbz w27, #7, 1f"); e(f"add x8, x19, #{K['pal16']}"); e("ld1 {v18.16b, v19.16b, v20.16b, v21.16b}, [x8]"); e("1:")
        e(f"ldrh w8, [x19, #{K['tw']}]"); e(f"mov v15.h[{L_TW}], w8")
    if not taken("v22"): e(f"ldr q22, [x19, #{K['pid24']}]")
    e(f"ldr d23, [x19, #{K['bytes']}]")
    if D == 2 and M != 2: e(f"add x8, x19, #{K['K']}"); e("ld1r {v6.4s}, [x8]")
    roles_setup(D)
    e("mov w7, w27"); e("mov x0, #0")
    if fused_alpha(T, M, B):                                                     # bit 15: bits 8 and 10 (fused_alpha())
        e("and w8, w27, w27, lsr #2"); e("ubfx w8, w8, #8, #1"); e("bfi w7, w8, #15, #1")
    # ---- per line ----
    e("10:")
    e(f"ldrh w5, [x22, #{SP['cdb']}]")                                          # C: x5 counts the pixels left
    e(f"ldrh w8, [x22, #{SP['xb']}]")                                           # X
    if HR:
        e(f"ldr w10, [x19, #{K['lstride']}]"); e("mul x10, x24, x10"); e("add x1, x25, x10"); e("add x1, x1, x8, lsl #2")
        if M != 2: e(f"ldr w10, [x19, #{K['attr_off']}]"); e("add x2, x1, x10")
        if M:
            e(f"ldr w10, [x19, #{K['owner_stride']}]"); e("mul x10, x24, x10"); e(f"ldr x11, [x19, #{K['owner']}]")
            e(f"add x{M}, x11, x10"); e(f"add x{M}, x{M}, x8, lsl #1")
        if R:
            e(f"ldr w10, [x19, #{K['id_stride']}]"); e("mul x10, x24, x10"); e(f"ldr w11, [x19, #{K['id_off']}]")
            e("add x3, x25, x11"); e("add x3, x3, x10"); e("add x3, x3, x8")
        elif M != 2: e("add x3, sp, #256")
    else:
        e("add x1, x25, x24, lsl #11"); e("add x1, x1, x8, lsl #2")
        if M != 2: e("add x2, x1, #0x10, lsl #12")
        if M: e(f"ldr x10, [x19, #{K['owner']}]"); e(f"add x{M}, x10, x24, lsl #10"); e(f"add x{M}, x{M}, x8, lsl #1")
        if R: e("add x3, x25, x24, lsl #9"); e("add x3, x3, #0x20, lsl #12"); e("add x3, x3, x8")
        elif M != 2: e("add x3, sp, #256")
    st = uses_st(D, T, F, M)
    if R and not lazy(D, st): e("mov x4, #0")
    if D == 0: depth_setup_z()
    if not lazy(D, st): line_setup(D, T, F, M)

def lazy(D, st):
    """the weights and interpolants are set up at the line's first group with a passing pixel (not needed by the
    depth test: z or constant depth)"""
    return st and D != 1

def line_setup(D, T, F, M):
    if uses_st(D, T, F, M): steps_setup(D)
    interpolants_setup(T, F, M)
    if T and not F and M != 1: white_test()

def steps_setup(D, lazy=False):
    """perspective: num_j = j * W0, den_j = (W0 + dW) * C - j * dW and their per-group steps 8 W0, 8 dW, as floats;
    affine (flag bit 0): the lanes' products i * Rwc and their per-group step 8 Rwc (u32, wrapping). D = 1: the
    dW and W0 splats of the depth. lazy: at the line's first group with a passing pixel (x5 pixels left; x8 holds
    the pass mask): the steps of the g groups before it applied as the group loop applies them (perspective: g
    roundings in order; affine: the lanes i + 8 g)."""
    c, w0, dw, t, p = ("w9", "w12", "w13", "w14", "x14") if lazy else ("w5", "w8", "w10", "w11", "x11")
    ja, jb, j8 = ("v29", "v30", "v31") if lazy else ("v28", "v29", "v28")       # (lazy: v28 holds the pass mask)
    if lazy: e(f"ldrh {c}, [x22, #{SP['cdb']}]"); e(f"sub w15, {c}, w5"); e("lsr w15, w15, #3")    # C, g
    e("tbnz w7, #0, 1f")
    e(f"ldr {w0}, [x22, #{SP['W0']}]"); e(f"ldr {dw}, [x22, #{SP['dW']}]")
    e(f"add {t}, {w0}, {dw}"); e(f"scvtf s24, {t}"); e(f"ucvtf s25, {c}"); e("fmul s24, s24, s25")
    e(f"scvtf s26, {w0}"); e(f"scvtf s27, {dw}")
    e(f"ldp {q(ja)}, {q(jb)}, [x20, #144]")                                       # j = 0..7 as floats
    e(f"fmul v0.4s, {ja}.4s, v26.s[0]"); e(f"fmul v1.4s, {jb}.4s, v26.s[0]")
    e("dup v2.4s, v24.s[0]"); e("dup v3.4s, v24.s[0]")
    e(f"fmls v2.4s, {ja}.4s, v27.s[0]"); e(f"fmls v3.4s, {jb}.4s, v27.s[0]")
    e(f"ldr {q(j8)}, [x20, #208]")                                                # 8.0
    e(f"fmul v4.4s, {j8}.4s, v26.s[0]"); e(f"fmul v5.4s, {j8}.4s, v27.s[0]")
    if D == 1: e(f"dup v6.4s, {dw}"); e(f"dup v7.4s, {w0}")
    if lazy:
        e("cbz w15, 2f")
        e("29:"); e("fadd v0.4s, v0.4s, v4.4s"); e("fadd v1.4s, v1.4s, v4.4s"); e("fsub v2.4s, v2.4s, v5.4s"); e("fsub v3.4s, v3.4s, v5.4s")
        e("subs w15, w15, #1"); e("b.ne 29b")
    e("b 2f")
    e("1:")
    e(f"ldr {p}, [x19, #{K['recip_u']}]"); e(f"ldr w17, [{p}, {c}, uxtw #2]")
    e("ldp q4, q5, [x20, #176]")
    if lazy: e("lsl w15, w15, #3"); e("dup v0.4s, w15"); e("add v4.4s, v4.4s, v0.4s"); e("add v5.4s, v5.4s, v0.4s")
    e("dup v0.4s, w17"); e("mul v4.4s, v4.4s, v0.4s"); e("mul v5.4s, v5.4s, v0.4s")
    e("lsl w17, w17, #3"); e("dup v1.4s, w17")
    if D == 1: e(f"add x8, x22, #{SP['dW']}"); e("ld1r {v6.4s}, [x8]"); e("ld1r {v7.4s}, [x22]")
    e("2:")
    if D == 1:                                                                   # bit 13: dW fits in s16
        e("fmov w16, s6"); e("cmp w16, w16, sxth"); e("cset w16, eq"); e("bfi w7, w16, #13, #1")

def depth_setup_z():
    """z: the line's DDA in u64 lanes, z0 << 30 + i * zstep, zstep = dZ * recip[C] (+ 0x3fffffff for dZ < 0)"""
    e(f"ldr w8, [x22, #{SP['Z0']}]"); e(f"ldrsw x10, [x22, #{SP['dZ']}]")
    e(f"ldr x11, [x19, #{K['recip']}]"); e("ldrsw x11, [x11, w5, uxtw #2]")
    e("mul x12, x10, x11"); e("asr x13, x10, #63"); e("add x12, x12, x13, lsr #34")
    e("lsl x8, x8, #30")
    e("dup v25.2d, x12"); e("dup v6.2d, x8"); e("add x10, x8, x12"); e("mov v6.d[1], x10")
    e("shl v8.2d, v25.2d, #1"); e("add v7.2d, v6.2d, v8.2d"); e("shl v8.2d, v25.2d, #2")
    if not taken("v9"): e("shl v9.2d, v25.2d, #3")

def interpolants_setup(T, F, M):
    """the line's vertex colour (not flat, not visibility) and texture coordinate bases (Q15 + rounding) and deltas"""
    if not F and M != 1:
        # r g b (u16 splats) from the halves of the span's rg0 and xb words (x0 b)
        e(f"ldr s10, [x22, #{SP['rg0']}]"); e("dup v11.8h, v10.h[1]"); e("dup v10.8h, v10.h[0]")
        e(f"ldr s12, [x22, #{SP['xb']}]"); e("dup v12.8h, v12.h[1]")
        e(f"ldr w10, [x22, #{SP['drg']}]"); e(f"ldr w12, [x22, #{SP['cdb']}]")
        e("mov v15.s[0], w10"); e("mov v15.s[1], w12")                           # dr dg, C db
        # bit 14: no colour delta is -32768 (vertex_colour's 16-bit products cannot saturate)
        e("mov w16, #0x8000"); e("cmp w16, w10, uxth"); e("lsr w15, w10, #16"); e("ccmp w15, w16, #4, ne")
        e("lsr w15, w12, #16"); e("ccmp w15, w16, #4, ne"); e("cset w15, ne"); e("bfi w7, w15, #14, #1")
    if T:
        # u0 = s0 << 15, + 0x400 where du > 0 (0 - (du << 16) < 0); v0 = t0 << 15, + 0x400 where dv > 0 (dst >= 0x10000)
        e(f"ldr w13, [x22, #{SP['st0']}]"); e(f"ldr w14, [x22, #{SP['dst']}]")
        e("sbfiz w15, w13, #15, #16"); e("add w16, w15, #0x400"); e("cmp wzr, w14, lsl #16"); e("csel w15, w16, w15, lt")
        e("dup v13.4s, w15")
        e("asr w15, w13, #16"); e("lsl w15, w15, #15"); e("add w16, w15, #0x400"); e("cmp w14, #0x10, lsl #12")
        e("csel w15, w16, w15, ge"); e("dup v14.4s, w15")
        e("mov v15.s[2], w14")                                                  # du dv

def white_test():
    """flag bit 9: bit 2 for the lines with a white vertex colour (r g b 0x1ff, deltas 0; A is 31): the texel is the
    colour. Uses w10 w12 = drg cdb from interpolants_setup."""
    e("tbz w27, #9, 1f")
    e(f"ldr w9, [x22, #{SP['rg0']}]"); e(f"ldr w11, [x22, #{SP['xb']}]")
    e("eor w15, w9, #0x1ff01ff"); e("orr w15, w15, w10")                        # rg ^ white | drg
    e("eor w16, w11, #0x1ff0000"); e("orr w16, w16, w12")                       # high halves: b ^ white | db
    e("orr w15, w15, w16, lsr #16"); e("cmp w15, #0"); e("cset w15, eq"); e("bfi w7, w15, #2, #1")
    e("1:")

def line_end(R, lz=False):
    """after a line's group loop: edge marking fix-up (opaque) or the first id (translucent); next line. lz (lazy
    kernels): the pass masks start at the first passing pixel x4"""
    if R:
        e("ldrb w8, [x3]"); e("strb w8, [x26], #1")
    else:
        # mark_edges: byte 3 of the attribute := 0x40 (then the id again) on the first EL and last ER pixels this
        # polygon wrote; EL may be C + 1 on the polygon's last line (DraStic marks its padding). The attribute line
        # starts (C - 1) / 8 groups before x2 (the loop leaves it at its last group), the pass masks at sp + 256.
        e("tbz w27, #6, 15f")
        e(f"ldrh w5, [x22, #{SP['cdb']}]")
        e("sub w8, w5, #1"); e("lsr w8, w8, #3"); e("sub x17, x2, x8, lsl #5"); e("add x3, sp, #256")
        e(f"ldr w13, [x19, #{K['pid24']}]")
        e(f"ldrh w8, [x22, #{SP['edges']}]"); e("cmp w8, w5"); e("csel w8, w8, w5, lo"); e(f"mov x10, {'x4' if lz else '#0'}")
        e("11:"); e("cmp x10, x8"); e("b.hs 12f")
        e("ldrb w12, [x3, x10]"); e("cbz w12, 13f")
        e("ldr w12, [x17, x10, lsl #2]"); e("and w12, w12, #0xffffff"); e("orr w12, w12, #0x40000000"); e("orr w12, w12, w13"); e("str w12, [x17, x10, lsl #2]")
        e("13:"); e("add x10, x10, #1"); e("b 11b")
        e("12:")
        e(f"ldrh w8, [x22, #{SP['edges'] + 2}]"); e("cmp w8, w5"); e("csel w8, w8, w5, lo"); e("sub x10, x5, x8")
        if lz: e("cmp x10, x4"); e("csel x10, x10, x4, hs")
        e("14:"); e("cmp x10, x5"); e("b.hs 15f")
        e("ldrb w12, [x3, x10]"); e("cbz w12, 16f")
        e("ldr w12, [x17, x10, lsl #2]"); e("and w12, w12, #0xffffff"); e("orr w12, w12, #0x40000000"); e("orr w12, w12, w13"); e("str w12, [x17, x10, lsl #2]")
        e("16:"); e("add x10, x10, #1"); e("b 14b")
        e("15:")
    e("add x22, x22, #4"); e("add w24, w24, #1"); e("subs w23, w23, #1"); e("b.ne 10b")

def epilogue():
    e(f"add sp, sp, #{FRAME}")
    e("ldp x27, x28, [sp, #128]"); e("ldp x25, x26, [sp, #112]"); e("ldp x23, x24, [sp, #96]"); e("ldp x21, x22, [sp, #80]")
    e("ldp x19, x20, [sp, #64]")
    e("ldp d14, d15, [sp, #48]"); e("ldp d12, d13, [sp, #32]"); e("ldp d10, d11, [sp, #16]"); e("ldp d8, d9, [sp], #160")
    e("ret")

def steps(r=("v24", "v25", "v26", "v27")):
    """perspective: st -> v24 (8 x s16 Q15 perspective weights); r: v24 and the three scratch registers (the
    group head's form, beside the depth test, uses v29-v31)"""
    a, b, s, t = r
    e(f"frecpe {a}.4s, v2.4s"); e(f"frecpe {b}.4s, v3.4s")
    e(f"frecps {s}.4s, {a}.4s, v2.4s"); e(f"frecps {t}.4s, {b}.4s, v3.4s")
    e(f"fmul {a}.4s, {a}.4s, {s}.4s"); e(f"fmul {b}.4s, {b}.4s, {t}.4s")
    e(f"frecps {s}.4s, {a}.4s, v2.4s"); e(f"frecps {t}.4s, {b}.4s, v3.4s")
    e(f"fmul {a}.4s, {a}.4s, {s}.4s"); e(f"fmul {b}.4s, {b}.4s, {t}.4s")
    e(f"fmul {a}.4s, v0.4s, {a}.4s"); e(f"fmul {b}.4s, v1.4s, {b}.4s")
    e(f"fcvtzs {a}.4s, {a}.4s, #15"); e(f"fcvtzs {b}.4s, {b}.4s, #15")
    e(f"xtn {a}.4h, {a}.4s"); e(f"xtn2 {a}.8h, {b}.4s")

def depth(D):
    """dep -> v25 (pixels 0-3), v26 (4-7); the constant variant reads v6"""
    if D == 0:
        e("shrn v25.2s, v6.2d, #30"); e("shrn2 v25.4s, v7.2d, #30")
        e("add v26.2d, v6.2d, v8.2d"); e("add v27.2d, v7.2d, v8.2d")
        e("shrn v26.2s, v26.2d, #30"); e("shrn2 v26.4s, v27.2d, #30")
    elif D == 1:
        # W0 + (dW * st >> 15): with dW in s16 (flag bit 13, per line) the products fit in 32 bits; else the 64-bit
        # products out of line (23:)
        e("tbz w7, #13, 23f")
        e("smull v25.4s, v24.4h, v6.h[0]"); e("smull2 v26.4s, v24.8h, v6.h[0]")
        e("sshr v25.4s, v25.4s, #15"); e("sshr v26.4s, v26.4s, #15")
        e("add v25.4s, v25.4s, v7.4s"); e("add v26.4s, v26.4s, v7.4s")
        e("22:")

def depth_ool(D):
    if D != 1: return
    e("23:")
    e("sshll v26.4s, v24.4h, #0"); e("sshll2 v27.4s, v24.8h, #0")
    e("smull v25.2d, v26.2s, v6.2s"); e("smull2 v26.2d, v26.4s, v6.4s")
    e("shrn v25.2s, v25.2d, #15"); e("shrn2 v25.4s, v26.2d, #15")
    e("add v25.4s, v25.4s, v7.4s")
    e("smull v26.2d, v27.2s, v6.2s"); e("smull2 v27.2d, v27.4s, v6.4s")
    e("shrn v26.2s, v26.2d, #15"); e("shrn2 v26.4s, v27.2d, #15")
    e("add v26.4s, v26.4s, v7.4s")
    e("b 22b")

def tail(r, free=False):
    """the line's last group: only its x5 (< 8) pixels. free: without a branch (the group head's single block), the
    table's entry 8 (all ones) for the full groups"""
    if free:
        e("mov x9, #8"); e("cmp x5, #8"); e("csel x9, x5, x9, lo")
        e("ldr q28, [x20, x9, lsl #4]"); e(f"and {r}.16b, {r}.16b, v28.16b")
        return
    e("cmp x5, #8"); e("b.hs 3f")
    e("ldr q28, [x20, x5, lsl #4]"); e(f"and {r}.16b, {r}.16b, v28.16b")
    e("3:")

def depth_regs(D): return ("v6", "v6") if D == 2 else ("v25", "v26")

def test(D, fail="8f", eq=24):
    """depth test against the attribute words: m8 -> v28 (and x8); the group's attribute words (dep | pid << 24) into
    the dep registers, or dep spilled to [sp,#64]; fails to `fail`; the depth-equal test out of line at `eq`"""
    dl, dh = depth_regs(D)
    e("ldp q27, q28, [x2]")
    e("bic v27.4s, #0xff, lsl #24"); e("bic v28.4s, #0xff, lsl #24")
    e(f"tbnz w7, #1, {eq}f")                                                    # depth equal: out of line
    e(f"cmhi v27.4s, v27.4s, {dl}.4s"); e(f"cmhi v28.4s, v28.4s, {dh}.4s")
    e(f"{eq + 1}:")
    e("uzp1 v27.8h, v27.8h, v28.8h")
    tail("v27")
    e(f"xtn v28.8b, v27.8h"); e("fmov x8, d28"); e(f"cbz x8, {fail}")
    dep_post(D, True)

def dep_post(D, spill=False):
    """after a passing test: the group's attribute words dep | pid << 24 into the dep registers, or dep spilled;
    keep_dep() kernels: dep stays in v25 v26 (pid ORed in by the store, dep_words()), spilled too with `spill`
    (test()'s callers, the lazy loop's entry and the depth-equal heads, reach 28:, which reloads them)"""
    if D != 2:
        if KD:
            if spill: e("stp q25, q26, [sp, #64]")
        elif "dep" in RL:
            p = pid24("v27"); e(f"orr {RL['dep'][0]}.16b, v25.16b, {p}.16b"); e(f"orr {RL['dep'][1]}.16b, v26.16b, {p}.16b")
        else: e("stp q25, q26, [sp, #64]")

def head(D, M):
    """the group's start in the lazy kernels (z or constant depth; the shade pass's owner test): the test, the
    perspective weights and the next group's steps (formerly the latch's) in one basic block, so that the
    in-order core overlaps their chains (it overlaps only what interleaves in program order, and kernsched.py reorders
    within a block). The weights are computed for every group, also one that fails the test; they use v29-v31 as
    scratch, beside the test's v25-v28, and the tail mask comes without a branch. The affine steps (flag bit 0) take
    an out-of-line copy of the head (6:), the depth-equal test (flag bit 1) one that joins the lazy loop's entry
    (28:), which computes the weights and steps after the test."""
    if M != 2: e("tbnz w7, #1, 24f")                                           # depth equal: out of line
    e("tbnz w7, #0, 6f")                                                       # affine: out of line
    head_block(D, M, lambda: (steps(("v24", "v29", "v30", "v31")), advance(0)), "8f")
    e("5:")
    def ool():
        e("6:"); head_block(D, M, lambda: advance(1), "8b"); e("b 5b")
        if M != 2:
            e("24:"); depth(D); test(D, "39f", 30); e("b 28b")
            test_equal(D, 30)
            e("39:"); advance(); e("b 8b")
    OOLS.append(ool)

def head_w(M):
    """the group's start with w depth (D = 1, the depth is W0 + (dW * st >> 15), so the weights come first): the
    weights, the attribute words' load and the tail mask beside them, the depth test and the next group's steps, one
    block (as head()); the affine steps, the 64-bit products (flag bit 13 clear) and the depth-equal test take the
    former order, with its branches, out of line (24:)"""
    e("tbnz w7, #1, 24f"); e("tbnz w7, #0, 24f"); e("tbz w7, #13, 24f")
    steps(("v24", "v29", "v30", "v31"))
    e("ldp q27, q28, [x2]")
    e("bic v27.4s, #0xff, lsl #24"); e("bic v28.4s, #0xff, lsl #24")
    e("smull v25.4s, v24.4h, v6.h[0]"); e("smull2 v26.4s, v24.8h, v6.h[0]")
    e("sshr v25.4s, v25.4s, #15"); e("sshr v26.4s, v26.4s, #15")
    e("add v25.4s, v25.4s, v7.4s"); e("add v26.4s, v26.4s, v7.4s")
    e("cmhi v27.4s, v27.4s, v25.4s"); e("cmhi v28.4s, v28.4s, v26.4s")
    e("uzp1 v27.8h, v27.8h, v28.8h")
    tail("v27", True)
    e("xtn v28.8b, v27.8h"); e("fmov x8, d28")
    advance(0)
    e("cbz x8, 8f")
    dep_post(1)
    e("5:")
    def ool():
        e("24:")
        e("tbnz w7, #0, 40f"); steps(); advance(0); e("b 41f"); e("40:"); advance(1); e("41:")
        depth(1); test(1, "8b", 30); e("b 5b")
        test_equal(1, 30)
    OOLS.append(ool)

def head_block(D, M, st, fail):
    """head(): the depth (or owner) test with st() beside it, one block; fails to `fail`"""
    if M == 2:
        e("ldr q27, [x2]")
        if "idx" in RL: e(f"cmeq v27.8h, v27.8h, {RL['idx'][0]}.8h")
        else: e(f"ldr q25, [x19, #{K['idx16']}]"); e("cmeq v27.8h, v27.8h, v25.8h")
    else:
        dl, dh = depth_regs(D)
        depth(D)
        e("ldp q27, q28, [x2]")
        e("bic v27.4s, #0xff, lsl #24"); e("bic v28.4s, #0xff, lsl #24")
        e(f"cmhi v27.4s, v27.4s, {dl}.4s"); e(f"cmhi v28.4s, v28.4s, {dh}.4s")
        e("uzp1 v27.8h, v27.8h, v28.8h")
    tail("v27", True)
    e("xtn v28.8b, v27.8h"); e("fmov x8, d28")
    st()
    e(f"cbz x8, {fail}")
    if M != 2: dep_post(D)

def advance(affine=None):
    """the per-group steps, in the group's head (head(), head_w(), the lazy loop's entry 28:): perspective (0) the
    numerators and denominators by 8 W0, 8 dW; affine (1) st from the lanes' products i * Rwc and those by 8 Rwc;
    None: the flag's (labels 40 41: out of line, these come before the partial-group store's 1:)"""
    if affine is None:
        e("tbnz w7, #0, 40f"); advance(0); e("b 41f"); e("40:"); advance(1); e("41:")
    elif affine:
        e("shrn v24.4h, v4.4s, #16"); e("shrn2 v24.8h, v5.4s, #16")
        e("add v4.4s, v4.4s, v1.4s"); e("add v5.4s, v5.4s, v1.4s")
    else:
        e("fadd v0.4s, v0.4s, v4.4s"); e("fadd v1.4s, v1.4s, v4.4s"); e("fsub v2.4s, v2.4s, v5.4s"); e("fsub v3.4s, v3.4s, v5.4s")

def test_equal(D, eq=24):
    """out of line: the depth-equal test (attribute bit 14), |dep - d| < 0x100"""
    dl, dh = depth_regs(D)
    e(f"{eq}:")
    e(f"sub v27.4s, {dl}.4s, v27.4s"); e(f"sub v28.4s, {dh}.4s, v28.4s")
    e("abs v27.4s, v27.4s"); e("abs v28.4s, v28.4s")
    e("movi v29.4s, #1, lsl #8")
    e("cmhi v27.4s, v29.4s, v27.4s"); e("cmhi v28.4s, v29.4s, v28.4s")
    e(f"b {eq + 1}b")

def dep_words(D):
    """the group's attribute words dep | pid << 24 -> two registers"""
    if KD:                                  # (in the store's block: there the pid load and the orrs hide)
        p = pid24({"flat": "v27", "litp": RL["vs"][0]}.get(KD, "v28"))
        e(f"orr v25.16b, v25.16b, {p}.16b"); e(f"orr v26.16b, v26.16b, {p}.16b")
        return "v25", "v26"
    if "dep" in RL: return (RL["dep"][0], RL["dep"][0]) if D == 2 else tuple(RL["dep"])
    if D == 2: e("orr v25.16b, v6.16b, v22.16b"); return "v25", "v25"
    e("ldp q25, q26, [sp, #64]"); p = pid24("v24"); e(f"orr v25.16b, v25.16b, {p}.16b"); e(f"orr v26.16b, v26.16b, {p}.16b")
    return "v25", "v26"

def texcoord(axis, T, r):
    """r (raw s16 coordinate) -> wrapped u16 in r; v29 scratch. Wrap: & (W-1). Otherwise per the axis's mode
    (flag bits 11 s, 12 t: clamp): clamp to [0, W-1] (the sign mask clears negatives, umin the rest; & (W-1) is the
    identity then); flip (or wrap): invert where x & W (kargs flip: W, or 0), & (W-1)"""
    a = "s" if axis == 0 else "t"
    m = f"v{16 + axis}"
    if taken(m):                                       # (v25 is free until the colour; keep_dep(): v28 is)
        m = "v28" if KD else "v25"; e(f"ldr {q(m)}, [x19, #{K[a + '_and']}]")
    if T in (2, 4):
        e(f"and {r}.16b, {r}.16b, {m}.16b")
    else:
        e(f"tbz w7, #{11 + axis}, 1f")
        e(f"sshr v29.8h, {r}.8h, #15"); e(f"bic {r}.16b, {r}.16b, v29.16b"); e(f"umin {r}.8h, {r}.8h, {m}.8h")
        e("b 2f")
        e("1:")
        e(f"ldr q29, [x19, #{K[a + '_flip']}]"); e(f"cmtst v29.8h, {r}.8h, v29.8h"); e(f"eor {r}.16b, {r}.16b, v29.16b")
        e(f"and {r}.16b, {r}.16b, {m}.16b")
        e("2:")

def addrs(lo, hi):
    """the 8 texel addresses (u32 lanes of lo, hi) -> w9-w16"""
    for i in range(8): e(f"umov w{9 + i}, {lo if i < 4 else hi}.s[{i % 4}]")

def gather(T):
    """8 texels at the u32 addresses in v29 (0-3) and v27 (4-7) -> channels tr v29, tg v27, tb v31, ta v30 (8 x u8).
    Paletted textures with a palette of at most 16 entries (flag bit 7) look the channels up with tbl from the
    palette held in v18-v21, one channel per register (kargs.pal16: r[16] g[16] b[16] a[16]), instead of loading
    each texel's palette entry. The addresses go to w9-w16 with umov (not through the stack: a 128-bit store read
    back as 32-bit words waits on the store's forwarding; the model also prefers umov), the texels back through
    [sp,#32] and ld2 (lane inserts are a serial chain)."""
    addrs("v29", "v27")
    if T in (3, 4):
        for r in range(9, 17): e(f"ldrb w{r}, [x6, w{r}, uxtw]")
        e("tbz w7, #7, 1f")
        e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
        e("ld2 {v29.8h, v30.8h}, [x28]"); e("xtn v29.8b, v29.8h")                    # the 8 indices
        e("tbl v30.8b, {v21.16b}, v29.8b"); e("tbl v31.8b, {v20.16b}, v29.8b"); e("tbl v27.8b, {v19.16b}, v29.8b")
        e("tbl v29.8b, {v18.16b}, v29.8b")
        e("b 2f")
        e("1:")
        for r in range(9, 17): e(f"ldr w{r}, [x21, w{r}, uxtw #2]")
    else:
        for r in range(9, 17): e(f"ldr w{r}, [x6, w{r}, uxtw #2]")
    e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
    e("ld2 {v29.8h, v30.8h}, [x28]")                                              # r | g << 8, b | a << 8
    e("shrn v27.8b, v29.8h, #8"); e("xtn v29.8b, v29.8h")
    e("xtn v31.8b, v30.8h"); e("shrn v30.8b, v30.8h, #8")
    if T in (3, 4): e("2:")

def texture(T, pre_gather=None):
    """texel channels -> tr v29, tg v27, tb v31, ta v30 (8 x u8 each)"""
    # u -> v30
    e(f"smull v30.4s, v24.4h, v15.h[{L_DU}]"); e(f"smull2 v29.4s, v24.8h, v15.h[{L_DU}]")
    e("addhn v30.4h, v30.4s, v13.4s"); e("addhn2 v30.8h, v29.4s, v13.4s")
    e("sshr v30.8h, v30.8h, #3")
    texcoord(0, T, "v30")
    # v -> v27
    e(f"smull v27.4s, v24.4h, v15.h[{L_DV}]"); e(f"smull2 v29.4s, v24.8h, v15.h[{L_DV}]")
    e("addhn v27.4h, v27.4s, v14.4s"); e("addhn2 v27.8h, v29.4s, v14.4s")
    e("sshr v27.8h, v27.8h, #3")
    texcoord(1, T, "v27")
    if pre_gather: pre_gather()
    # address = u + v * W
    e(f"umull v29.4s, v27.4h, v15.h[{L_TW}]"); e(f"umull2 v27.4s, v27.8h, v15.h[{L_TW}]")
    e("uaddw v29.4s, v29.4s, v30.4h"); e("uaddw2 v27.4s, v27.4s, v30.8h")
    gather(T)

def texture_bilinear(T):
    """bilinear: the same outputs as texture(). The coordinate has 3 fraction bits (1/8 texel); the sample point is
    half a texel back so integer coordinates are texel centres; the four texels around it are blended with
    weights (8-fu)(8-fv), fu(8-fv), (8-fu)fv, fu fv (sum 64), rounded. Where one of the four is transparent
    (alpha 0: a cut-out edge) the pixel takes the nearest texel instead, all four channels, so cut-outs keep the
    shape and colours of nearest filtering (no dark fringes, no change of coverage). Uses v10-v14 and v24 as
    scratch (saved) and v18-v21: v18 the cut-out mask, v19 v20 the nearest texel (lo, hi halves), v21 scratch."""
    e(f"smull v30.4s, v24.4h, v15.h[{L_DU}]"); e(f"smull2 v29.4s, v24.8h, v15.h[{L_DU}]")
    e("addhn v30.4h, v30.4s, v13.4s"); e("addhn2 v30.8h, v29.4s, v13.4s")
    e(f"smull v27.4s, v24.4h, v15.h[{L_DV}]"); e(f"smull2 v29.4s, v24.8h, v15.h[{L_DV}]")
    e("addhn v27.4h, v27.4s, v14.4s"); e("addhn2 v27.8h, v29.4s, v14.4s")
    e("str q24, [sp, #96]"); e("stp q10, q11, [sp, #112]"); e("stp q12, q13, [sp, #144]"); e("str q14, [sp, #176]")
    e("movi v24.8h, #4"); e("sub v30.8h, v30.8h, v24.8h"); e("sub v27.8h, v27.8h, v24.8h")
    e("movi v24.8h, #7"); e("and v10.16b, v30.16b, v24.16b"); e("and v11.16b, v27.16b, v24.16b")
    e("sshr v12.8h, v30.8h, #3"); e("sshr v13.8h, v27.8h, #3")
    e("movi v24.8h, #1"); e("add v14.8h, v12.8h, v24.8h"); e("add v31.8h, v13.8h, v24.8h")
    # the nearest texel is corner (fu >= 4, fv >= 4): one 8-bit mask per corner at [sp, #224..248]
    e("shl v21.8h, v10.8h, #13"); e("sshr v21.8h, v21.8h, #15"); e("shl v18.8h, v11.8h, #13"); e("sshr v18.8h, v18.8h, #15")
    e("orr v19.16b, v21.16b, v18.16b"); e("mvn v19.16b, v19.16b"); e("xtn v19.8b, v19.8h"); e("str d19, [sp, #224]")
    e("bic v19.16b, v21.16b, v18.16b"); e("xtn v19.8b, v19.8h"); e("str d19, [sp, #232]")
    e("bic v19.16b, v18.16b, v21.16b"); e("xtn v19.8b, v19.8h"); e("str d19, [sp, #240]")
    e("and v19.16b, v21.16b, v18.16b"); e("xtn v19.8b, v19.8h"); e("str d19, [sp, #248]")
    e("movi v18.2d, #0")
    texcoord(0, T, "v12"); texcoord(0, T, "v14"); texcoord(1, T, "v13"); texcoord(1, T, "v31")
    e("xtn v10.8b, v10.8h"); e("xtn v11.8b, v11.8h")
    e(f"ldr q24, [x19, #{K['fraclut']}]"); e("tbl v10.8b, {v24.16b}, v10.8b"); e("tbl v11.8b, {v24.16b}, v11.8b")
    e("movi v24.8b, #8"); e("sub v29.8b, v24.8b, v10.8b"); e("sub v30.8b, v24.8b, v11.8b")
    e("umull v24.8h, v29.8b, v30.8b"); e("xtn v24.8b, v24.8h"); e("str d24, [sp, #192]")
    e("umull v24.8h, v10.8b, v30.8b"); e("xtn v24.8b, v24.8h"); e("str d24, [sp, #200]")
    e("umull v24.8h, v29.8b, v11.8b"); e("xtn v24.8b, v24.8h"); e("str d24, [sp, #208]")
    e("umull v24.8h, v10.8b, v11.8b"); e("xtn v24.8b, v24.8h"); e("str d24, [sp, #216]")
    e("movi v24.2d, #0"); e("movi v25.2d, #0"); e("movi v26.2d, #0"); e("movi v27.2d, #0")
    for u, v, w in (("v12", "v13", 192), ("v14", "v13", 200), ("v12", "v31", 208), ("v14", "v31", 216)):
        e(f"umull v29.4s, {v}.4h, v15.h[{L_TW}]"); e(f"umull2 v30.4s, {v}.8h, v15.h[{L_TW}]")
        e(f"uaddw v29.4s, v29.4s, {u}.4h"); e(f"uaddw2 v30.4s, v30.4s, {u}.8h")
        # gather() wants the addresses in v29 and v27; v27 is an accumulator here, so inline it
        addrs("v29", "v30")
        if T in (3, 4):
            for r in range(9, 17): e(f"ldrb w{r}, [x6, w{r}, uxtw]")
            for r in range(9, 17): e(f"ldr w{r}, [x21, w{r}, uxtw #2]")
        else:
            for r in range(9, 17): e(f"ldr w{r}, [x6, w{r}, uxtw #2]")
        e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
        e("ld2 {v10.8h, v11.8h}, [x28]")
        e(f"ldr d29, [sp, #{w}]")
        e("xtn v30.8b, v10.8h"); e("umlal v24.8h, v30.8b, v29.8b")
        e("shrn v30.8b, v10.8h, #8"); e("umlal v25.8h, v30.8b, v29.8b")
        e("xtn v30.8b, v11.8h"); e("umlal v26.8h, v30.8b, v29.8b")
        e("shrn v30.8b, v11.8h, #8"); e("umlal v27.8h, v30.8b, v29.8b")
        e("cmeq v21.8b, v30.8b, #0"); e("orr v18.8b, v18.8b, v21.8b")                # a transparent corner
        e(f"ldr d21, [sp, #{w + 32}]"); e("sxtl v21.8h, v21.8b")                      # this corner where nearest
        e("bit v19.16b, v10.16b, v21.16b"); e("bit v20.16b, v11.16b, v21.16b")
    e("rshrn v30.8b, v27.8h, #6"); e("rshrn v27.8b, v25.8h, #6"); e("rshrn v29.8b, v24.8h, #6"); e("rshrn v31.8b, v26.8h, #6")
    e("xtn v21.8b, v19.8h"); e("bit v29.8b, v21.8b, v18.8b"); e("shrn v21.8b, v19.8h, #8"); e("bit v27.8b, v21.8b, v18.8b")
    e("xtn v21.8b, v20.8h"); e("bit v31.8b, v21.8b, v18.8b"); e("shrn v21.8b, v20.8h, #8"); e("bit v30.8b, v21.8b, v18.8b")
    e("ldr q24, [sp, #96]"); e("ldp q10, q11, [sp, #112]"); e("ldp q12, q13, [sp, #144]"); e("ldr q14, [sp, #176]")


def vertex_colour(ch, F, dst, slow=False):
    """vertex colour channel ch (0 r, 1 g, 2 b) -> dst (8 x u8): ((c0 << 15) + d * st) >> 18 with the base c0 (u16
    splat v10-v12) and the delta d (v15) of the line. That is (c0 + floor(d * st / 2^15)) >> 3 in 16 bits (bits 18-25
    of the 32-bit sum are bits 3-10 of c0 + its high part), and sqdmulh gives floor(d * st / 2^15) unless it
    saturates (d = st = -32768: flag bit 14 clear, slow: the 32-bit sums). v26 scratch"""
    if F:
        e(f"dup {dst}.8b, v23.b[{4 + ch}]")
        return
    l = (L_DR, L_DG, L_DB)[ch]
    if not slow:
        e(f"sqdmulh {dst}.8h, v24.8h, v15.h[{l}]"); e(f"add {dst}.8h, {dst}.8h, v{10 + ch}.8h"); e(f"shrn {dst}.8b, {dst}.8h, #3")
    else:
        w = "v28" if KD else "v26"                                              # (keep_dep(): v26 holds depth)
        e(f"ushll {dst}.4s, v{10 + ch}.4h, #15"); e(f"mov {w}.16b, {dst}.16b")
        e(f"smlal {dst}.4s, v24.4h, v15.h[{l}]"); e(f"smlal2 {w}.4s, v24.8h, v15.h[{l}]")
        e(f"uzp2 {dst}.8h, {dst}.8h, {w}.8h"); e(f"shrn {dst}.8b, {dst}.8h, #2")

def modulate(v, t, sh, w="v26", o=None):
    """o (t) = ((v+1)*(t+1)-1) >> sh, bytes; w (v26) scratch"""
    e(f"uaddl {w}.8h, {v}.8b, {t}.8b"); e(f"umlal {w}.8h, {v}.8b, {t}.8b"); e(f"shrn {o or t}.8b, {w}.8h, #{sh}")

def st4_store():
    """the opaque textured kernels with four registers in a row free for the colour bytes (st4_quad()) store a full
    group's colours with st4 from the channels' bytes (the modulates write them there): no zips for the common case
    (in llvm-mca's A55 model the pack-and-store 14 -> 6 cycles); the partial group zips them"""
    return st4_quad() is not None

def st4_quad():
    """the four registers in a row the st4 kernels' colour bytes go to: lit direct-textured opaque v18-v21 (vc, vs);
    flat textured opaque with its depth words in registers v24-v27 (free after the modulates' inputs); keep_dep()'s
    paletted kernels v28-v31"""
    if KD in ("flat", "litp"): return ["v28", "v29", "v30", "v31"]
    if RL.get("vc") == ["v18", "v19", "v20"] and RL.get("vs") == ["v21"]: return ["v18", "v19", "v20", "v21"]
    if "flat" in RL and len(RL.get("vs", ())) == 2 and "dep" in RL and "trans" not in RL and "idx" not in RL:
        return ["v24", "v25", "v26", "v27"]
    return None

def bsc():
    """keep_dep()'s st4 kernels: b's modulate scratch, whose shrn into v30 waits for the store's block (flat: the
    second modulate scratch; lit paletted: g's texel register v27, read by then)"""
    return "v27" if KD == "litp" else RL["vs"][1]

def modulate_alpha(skip, s=("v25", "v26")):
    """ta (v30) -> ca = modulate(A, ta); with flag bit 10 (A is 31) ca = (32 (ta + 1) - 1) >> 5 = ta: to skip;
    s: the scratch registers"""
    e(f"tbnz w7, #10, {skip}")
    if "a" in RL: modulate(RL["a"][0], "v30", 5, s[1])
    else: e(f"dup {s[0]}.8b, v23.b[0]"); modulate(s[0], "v30", 5, s[1])

def alpha_test(fail="8f", s="v25"):
    """ca v30 > aref -> narrows the mask v28; fails to `fail`; s: the scratch register"""
    if "aref" in RL: e(f"cmhi {s}.8b, v30.8b, {RL['aref'][0]}.8b")
    else: e(f"dup {s}.8b, v23.b[1]"); e(f"cmhi {s}.8b, v30.8b, {s}.8b")
    if KD: e(f"fmov x9, d{s[1:]}"); e("and x8, x8, x9")               # (keep_dep(): the mask in x8 only)
    else: e(f"and v28.8b, v28.8b, {s}.8b"); e("fmov x8, d28")
    e(f"cbz x8, {fail}")

def fused_alpha(T, M, B):
    """the textured kernels' alpha stages behind one flag test: bit 15 (set in the prologue) is bits 8 and 10 both
    (no alpha test, A is 31), the common case; then the modulates and the store are one block. Otherwise the alpha
    modulate and the alpha test, and the white lines' alpha test, out of line (46:, 45:, back to 47:)"""
    return T and M == 0 and not B

def white_branch(T, M, B):
    """white vertex colour, alpha 31: the texel is the colour (the rgb modulates and the alpha modulate skipped; the
    st4 kernels copy the texels into the colour's registers out of line, 44:)"""
    e(("tbnz w7, #2, 44f" if st4_store() else "tbnz w7, #2, 45f") if fused_alpha(T, M, B) else "tbnz w7, #2, 1f")

def alpha_stage(T, M, B):
    """after the rgb modulates: the alpha modulate (flag bit 10 clear) and the alpha test (bit 8 clear); the white
    branch joins before the test"""
    if not fused_alpha(T, M, B):
        modulate_alpha("1f")
        e("1:")
        if M != 2:
            e("tbnz w7, #8, 2f"); alpha_test(); e("2:")
        return
    e("tbz w7, #15, 46f")
    e("47:")
    # (the st4 kernels' colour bytes in v24-v27 keep the alpha stages' scratch in the texels' registers)
    # (keep_dep(): v24, r's modulate scratch, and g's texel v27 or (g's result in v27: no st4) b's scratch v28)
    sc = (("v29", "v31") if st4_quad() == ["v24", "v25", "v26", "v27"] else
          (("v24", RL["vs"][0]) if KD == "litp" else ("v24", "v27") if st4_store() else ("v24", "v28")) if KD else
          ("v25", "v26"))
    def ool():
        e("46:"); modulate_alpha("45f", sc)
        if st4_store():
            e("b 45f")
            qd = st4_quad(); e("44:"); e(f"mov {qd[0]}.8b, v29.8b"); e(f"mov {qd[1]}.8b, v27.8b")
            if KD in ("flat", "litp"): e(f"ushll {bsc()}.8h, v31.8b, #6")    # b, as the store's shrn takes it
            else: e(f"mov {qd[2]}.8b, v31.8b")
        e("45:"); e("tbnz w7, #8, 47b"); alpha_test("8b", sc[0]); e("b 47b")
    OOLS.append(ool)

def colour(T, F, B, M=0):
    """shaded colour -> (cr, cg, cb, ca) registers, 8 x u8 (textured: v29 v27 v31 v30; untextured flat with its
    colour words per batch: None); applies the alpha test (textured) unless shading deferred pixels (M = 2: the
    visibility pass applied it) or the texture's lowest alpha passes it (flag bit 8); fails to 8f"""
    if T and "ex" in RL and "trans" in RL:
        # translucent: the three channels' colour (vertex or flat) in v25 and the blend weights' registers, computed
        # among the gather's loads; the modulates with v26 and the ex registers as their scratch
        tr, ex = RL["trans"], RL["ex"]
        cv = RL["flat"] if F and "flat" in RL else ["v25", tr[0], tr[1]]
        ws = ["v26", ex[0], ex[1]]
        texture(T, None if F and "flat" in RL else (lambda: [vertex_colour(ch, F, cv[ch]) for ch in range(3)]))
        white_branch(T, M, B)
        if not F:
            e("tbz w7, #14, 37f")
            e("38:")
            def ool():
                e("37:")
                for ch in range(3): vertex_colour(ch, F, cv[ch], True)
                e("b 38b")
            OOLS.append(ool)
        for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")): modulate(cv[ch], t, 6, ws[ch])
        alpha_stage(T, M, B)
        return "v29", "v27", "v31", "v30"
    if T and "vc" in RL:
        # the vertex colour among the gather's loads (the fast form; the out-of-line slow form replaces it), then the
        # three modulates with their own scratch registers
        vc = RL["vc"]
        texture(T, lambda: [vertex_colour(ch, F, vc[ch]) for ch in range(3)])
        white_branch(T, M, B)
        e("tbz w7, #14, 37f")
        e("38:")
        o = st4_quad() if st4_store() else [None] * 3
        ws = ("v24", RL["vs"][0], "v27" if KD == "litp" else "v28") if KD else ("v26", RL["vs"][0], "v25")
        for ch, t, w in zip(range(3), ("v29", "v27", "v31"), ws):           # (keep_dep(): v25 v26 the depth words)
            if KD == "litp" and ch == 2: e(f"uaddl {w}.8h, {vc[ch]}.8b, {t}.8b"); e(f"umlal {w}.8h, {vc[ch]}.8b, {t}.8b")
            else: modulate(vc[ch], t, 6, w, o[ch])
        def ool():
            e("37:")
            for ch in range(3): vertex_colour(ch, F, vc[ch], True)
            e("b 38b")
        OOLS.append(ool)
        alpha_stage(T, M, B)
        return (*st4_quad()[:3], "v30") if st4_store() else ("v29", "v27", "v31", "v30")
    if T:
        if B: texture_bilinear(T)
        else: texture(T)
        white_branch(T, M, B)
        def rgb(slow=False):
            # modulate scratch registers: with the flat colour in its registers v25 is free too (two side by side),
            # with the two vs registers all three
            vs = RL.get("vs", ())
            ws = (["v26", *vs] if len(vs) == 2 else ["v26", "v25", "v26"]) if F and "flat" in RL else ["v26"] * 3
            o = st4_quad() if st4_store() and M == 0 else [None] * 3
            if KD == "flat": ws[0] = "v24"                                                 # (v25 v26: the depth words)
            for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")):
                if F and "flat" in RL: v = RL["flat"][ch]
                else: v = "v25"; vertex_colour(ch, F, v, slow)
                if KD == "flat" and ch == 2:        # b's result to v30 in the store's block, after a | fog left it
                    e(f"uaddl {ws[2]}.8h, {v}.8b, {t}.8b"); e(f"umlal {ws[2]}.8h, {v}.8b, {t}.8b")
                else: modulate(v, t, 6, ws[ch], o[ch])
        if not F:
            e("tbz w7, #14, 37f")
            rgb(); e("38:")
            def ool(): e("37:"); rgb(True); e("b 38b")
            OOLS.append(ool)
        else: rgb()
        alpha_stage(T, M, B)
        if st4_store() and M == 0: return (*st4_quad()[:3], "v30")
        return "v29", "v27", "v31", "v30"
    if "colw" in RL: return None
    if "flat" in RL: c = RL["flat"]
    else:
        c = ("v29", "v27", "v31")
        if not F:
            e("tbz w7, #14, 37f")
            for ch in range(3): vertex_colour(ch, F, c[ch])
            e("38:")
            def ool():
                e("37:")
                for ch in range(3): vertex_colour(ch, F, c[ch], True)
                e("b 38b")
            OOLS.append(ool)
        else:
            for ch in range(3): vertex_colour(ch, F, c[ch])
    if "caf" in RL: return (*c, RL["caf"][0])
    if "a" in RL: return (*c, RL["a"][0])
    e("dup v30.8b, v23.b[0]")
    return (*c, "v30")

def pack(cols):
    """the colour bytes and the fog bit -> the colour words c.l, c.h (v27 v29, or the batch's)"""
    if cols is None: return RL["colw"][0], RL["colw"][0]
    cr, cg, cb, ca = cols
    if "caf" in RL: a = ca
    else:
        if "fog" in RL: f = RL["fog"][0]
        else: f = "v24" if KD else "v25"; e(f"dup {f}.8b, v23.b[2]")
        e(f"orr v30.8b, {ca}.8b, {f}.8b"); a = "v30"
    e(f"zip1 v29.16b, {cr}.16b, {cg}.16b"); e(f"zip1 v31.16b, {cb}.16b, {a}.16b")
    e("zip1 v27.8h, v29.8h, v31.8h"); e("zip2 v29.8h, v29.8h, v31.8h")
    return "v27", "v29"

def store(D, cols):
    """opaque: pack, fog bit, store the group (all 8 lanes straight when they all pass) and its pass mask. The full
    group and a group without a pixel (8:) fall through into the latch (7:); returns the out-of-line partial group"""
    e("orr x0, x0, x8")
    if st4_store() and cols == (*st4_quad()[:3], "v30"):
        # the bytes r g b (a | fog) in four registers in a row: st4 interleaves them into the colour words; the
        # partial group zips
        r, g, b, a = st4_quad(); t, u = ("v24", "v27") if r in ("v18", "v28") else ("v29", "v25")
        if "fog" in RL: f = RL["fog"][0]
        else: e(f"dup {t}.8b, v23.b[2]"); f = t
        e(f"orr {a}.8b, v30.8b, {f}.8b")
        if KD in ("flat", "litp"): e(f"shrn {b}.8b, {bsc()}.8h, #6")             # b (keep_dep(): last, into v30)
        d0, d1 = dep_words(D)
        e("cmn x8, #1"); e("b.ne 1f")
        e(f"st4 {{{r}.8b, {g}.8b, {b}.8b, {a}.8b}}, [x1]"); e(f"stp {q(d0)}, {q(d1)}, [x2]")
        cl, ch = (("v29", "v27") if r == "v18" else RL["vs"] if KD == "flat" else (RL["vs"][0], "v29") if KD == "litp"
                  else ("v27", "v29"))
        zips = lambda: (e(f"zip1 {t}.16b, {r}.16b, {g}.16b"), e(f"zip1 {u}.16b, {b}.16b, {a}.16b"),
                        e(f"zip1 {cl}.8h, {t}.8h, {u}.8h"), e(f"zip2 {ch}.8h, {t}.8h, {u}.8h"))
    else:
        cl, ch = pack(cols)
        d0, d1 = dep_words(D)
        e("cmn x8, #1"); e("b.ne 1f")
        e(f"stp {q(cl)}, {q(ch)}, [x1]"); e(f"stp {q(d0)}, {q(d1)}, [x2]")
        zips = lambda: None
    m = "x8" if KD else "d28"                                                   # (keep_dep(): the mask in x8 only)
    e("8:"); e(f"str {m}, [x3], #8")                                             # the pass mask (edge marking)
    e("7:")
    def ool():
        e("1:"); e(f"str {m}, [x3], #8"); zips()
        if KD: e("fmov d28, x8")
        e("sshll v28.8h, v28.8b, #0"); e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
        e(f"ldp q24, q28, [x1]"); e(f"bit v24.16b, {cl}.16b, v30.16b"); e(f"bit v28.16b, {ch}.16b, v31.16b"); e("stp q24, q28, [x1]")
        e(f"ldp q24, q28, [x2]"); e(f"bit v24.16b, {d0}.16b, v30.16b"); e(f"bit v28.16b, {d1}.16b, v31.16b"); e("stp q24, q28, [x2]")
        e("b 7b")
    return ool

def trans_store(D, cols):
    """translucent: blend with the destination, id test, combine, store (spec/blend.c, fused_neon.c's TRANS path).
    In: cols = cr cg cb sa (v29 v27 v31 v30, or per-batch registers with the trans role), m8 v28 (and x8), the
    depth words (dep_words). x3 = id line pointer, x4 index."""
    cr, cg, cb, sa = cols
    e("orr x0, x0, x8")
    if "trans" in RL:
        ws, wd = RL["trans"]
        # the destination's halves split by the load (ld2: one load and no uzp pair; in llvm-mca's A55 model 3
        # cycles a group less than ldp q and uzp1/uzp2): dlo (r|g<<8) v25, dhi (b|a<<8) v26, its b byte to v24
        e("ld2 {v25.8h, v26.8h}, [x1]")
        e(f"movi {ws}.8b, #0x1f"); e("xtn v24.8b, v26.8h")
        e("shrn v26.8b, v26.8h, #8"); e(f"and v26.8b, v26.8b, {ws}.8b")       # dal = dst alpha & 0x1f
        e("tbz w7, #3, 1f")
        # blend: c' = (c + c*ws + d*wd) >> 5, ws = dal ? sa : 31, wd = dal ? 31 - sa : 0 (m8 is in x8), into v29 v27 v31
        e("cmeq v28.8b, v26.8b, #0")
        e(f"sub {wd}.8b, {ws}.8b, {sa}.8b"); e(f"bic {wd}.8b, {wd}.8b, v28.8b"); e(f"bif {ws}.8b, {sa}.8b, v28.8b")
        # (with the ex registers the r channel blends in them, beside b; g after b)
        xa, xd = RL["ex"] if "ex" in RL else ("v24", "v28")
        for c, o, d, a, t in ((cb, "v31", None, "v28", "v24"), (cr, "v29", f"xtn {xd}.8b, v25.8h", xa, xd),
                              (cg, "v27", "shrn v28.8b, v25.8h, #8", "v24", "v28")):
            if d: e(d)
            e(f"ushll {a}.8h, {c}.8b, #0"); e(f"umlal {a}.8h, {c}.8b, {ws}.8b"); e(f"umlal {a}.8h, {t}.8b, {wd}.8b")
            e(f"shrn {o}.8b, {a}.8h, #5")
        moves = [(o, c) for c, o in ((cr, "v29"), (cg, "v27"), (cb, "v31")) if c != o]
        if moves:
            e("b 2f"); e("1:")
            for o, c in moves: e(f"mov {o}.8b, {c}.8b")
            e("2:")
        else: e("1:")                                                            # cr v29, cg v27, cb v31
        e(f"umax v24.8b, {sa}.8b, v26.8b")                                       # a' = max(sa, dal)
        e("zip1 v29.16b, v29.16b, v27.16b"); e("zip1 v25.16b, v31.16b, v24.16b")
        e("zip1 v27.8h, v29.8h, v25.8h"); e("zip2 v29.8h, v29.8h, v25.8h")       # c.l v27, c.h v29
        e("fmov d28, x8"); e("ldr d31, [x3, x4]")                                # m8, dst ids
    else:
        assert cols == ("v29", "v27", "v31", "v30")
        e("stp d27, d31, [sp, #96]")                                             # cg, cb
        e("str d30, [sp, #112]")                                                 # sa (m8 is in x8)
        e("ldp q25, q26, [x1]")
        e("uzp2 v24.8h, v25.8h, v26.8h"); e("uzp1 v25.8h, v25.8h, v26.8h")       # dhi (b|a<<8), dlo (r|g<<8)
        e("str q24, [sp, #128]")
        e("shrn v26.8b, v24.8h, #8"); e("movi v24.8b, #0x1f"); e("and v26.8b, v26.8b, v24.8b")   # dal = dst alpha & 0x1f
        e("tbz w7, #3, 1f")
        # blend: c' = (c + c*ws + d*wd) >> 5, ws = dal ? sa : 31, wd = dal ? 31 - sa : 0
        e("cmeq v28.8b, v26.8b, #0"); e("movi v27.8b, #0x1f")
        e("bsl v28.8b, v27.8b, v30.8b")                                          # ws
        e("sub v27.8b, v27.8b, v30.8b"); e("cmeq v30.8b, v26.8b, #0"); e("bic v27.8b, v27.8b, v30.8b")   # wd
        e("xtn v30.8b, v25.8h")
        e("ushll v31.8h, v29.8b, #0"); e("umlal v31.8h, v29.8b, v28.8b"); e("umlal v31.8h, v30.8b, v27.8b"); e("shrn v29.8b, v31.8h, #5")
        e("ldr d31, [sp, #96]"); e("shrn v30.8b, v25.8h, #8")
        e("ushll v24.8h, v31.8b, #0"); e("umlal v24.8h, v31.8b, v28.8b"); e("umlal v24.8h, v30.8b, v27.8b"); e("shrn v31.8b, v24.8h, #5")
        e("ldr d25, [sp, #104]"); e("ldr q30, [sp, #128]"); e("xtn v30.8b, v30.8h")
        e("ushll v24.8h, v25.8b, #0"); e("umlal v24.8h, v25.8b, v28.8b"); e("umlal v24.8h, v30.8b, v27.8b"); e("shrn v25.8b, v24.8h, #5")
        e("ldr d30, [sp, #112]")                                                 # sa
        e("b 2f")
        e("1:"); e("mov v25.8b, v31.8b"); e("mov v31.8b, v27.8b")
        e("2:")                                                                  # cr v29, cg v31, cb v25
        e("umax v24.8b, v30.8b, v26.8b")                                         # a' = max(sa, dal)
        e("zip1 v29.16b, v29.16b, v31.16b"); e("zip1 v25.16b, v25.16b, v24.16b")
        e("zip1 v27.8h, v29.8h, v25.8h"); e("zip2 v29.8h, v29.8h, v25.8h")       # c.l v27, c.h v29
        e("fmov d28, x8"); e("ldr d31, [x3, x4]")                                # m8, dst ids
    e("movi v24.8b, #0x1f"); e(f"cmeq v24.8b, {sa}.8b, v24.8b")                 # op = sa == 31
    if "pid" in RL: p = RL["pid"][0]
    else: e("dup v25.8b, v23.b[3]"); p = "v25"                                  # pid
    e(f"cmeq v26.8b, v31.8b, {p}.8b"); e("bic v26.8b, v26.8b, v24.8b"); e("bic v28.8b, v28.8b, v26.8b")  # drop: same id, not opaque
    e("and v26.8b, v28.8b, v24.8b")                                              # o8 = m & op
    e("bic v30.8b, v28.8b, v24.8b")                                              # t8 = m & ~op
    e(f"bsl v30.8b, {p}.8b, v31.8b"); e("str d30, [x3, x4]")                         # ids = t ? pid : old
    e("sshll v30.8h, v28.8b, #0"); e("sshll v31.8h, v26.8b, #0")
    e("sshll v25.4s, v30.4h, #0"); e("sshll2 v26.4s, v30.8h, #0")               # M32 v25 v26
    e("sshll v30.4s, v31.4h, #0"); e("sshll2 v31.4s, v31.8h, #0")               # O32 v30 v31
    late = "trans" in RL and ("ex" in RL or "late" in RL)
    if late:
        # the blend's four registers are free from here: the destination colours and the depth words (or, with
        # them in a role, the attribute words) load here, before the fog and depth-update branches, and both
        # stores come after them in one block (kernsched.py then interleaves the two read-modify-writes)
        (c0, c1), (a0, a1) = RL["trans"], RL["ex"] if "ex" in RL else RL["late"]
        if "dep" in RL: d0, d1 = dep_words(D); e(f"ldp {q(a0)}, {q(a1)}, [x2]")
        else:
            e(f"ldp {q(a0)}, {q(a1)}, [sp, #64]"); p = pid24(c0)
            e(f"orr {a0}.16b, {a0}.16b, {p}.16b"); e(f"orr {a1}.16b, {a1}.16b, {p}.16b")
            d0, d1 = a0, a1
        e(f"ldp {q(c0)}, {q(c1)}, [x1]")
    e("tbz w7, #4, 3f")
    # fog: colour bit 31 set; the colour mask keeps the destination's bit 31 where the pixel is not opaque
    # (op32 is 0 or -1: (op32 == 0) << 31 is the bit to keep)
    e("orr v27.4s, #0x80, lsl #24"); e("orr v29.4s, #0x80, lsl #24")
    e("sshll v24.8h, v24.8b, #0"); e("sshll v28.4s, v24.4h, #0"); e("sshll2 v24.4s, v24.8h, #0")
    e("cmeq v28.4s, v28.4s, #0"); e("shl v28.4s, v28.4s, #31"); e("bic v25.16b, v25.16b, v28.16b")
    e("cmeq v24.4s, v24.4s, #0"); e("shl v24.4s, v24.4s, #31"); e("bic v26.16b, v26.16b, v24.16b")
    e("3:")
    if not late:
        e("ldp q24, q28, [x1]"); e("bit v24.16b, v27.16b, v25.16b"); e("bit v28.16b, v29.16b, v26.16b"); e("stp q24, q28, [x1]")
    e("tbz w7, #5, 4f")
    # depth update: the attribute's depth bits follow the whole mask, its id byte only the opaque pixels (the colour
    # store's masks v25 v26 lose only bit 31 to the fog, so this may come before or after it)
    e("mvni v24.4s, #0xff, lsl #24"); e("bit v30.16b, v25.16b, v24.16b"); e("bit v31.16b, v26.16b, v24.16b")
    e("4:")
    if late:
        e(f"bit {c0}.16b, v27.16b, v25.16b"); e(f"bit {c1}.16b, v29.16b, v26.16b"); e(f"stp {q(c0)}, {q(c1)}, [x1]")
        if "dep" in RL:
            e(f"bit {a0}.16b, {d0}.16b, v30.16b"); e(f"bit {a1}.16b, {d1}.16b, v31.16b"); e(f"stp {q(a0)}, {q(a1)}, [x2]")
        else:
            e("ldp q24, q28, [x2]"); e(f"bit v24.16b, {d0}.16b, v30.16b"); e(f"bit v28.16b, {d1}.16b, v31.16b"); e("stp q24, q28, [x2]")
    else:
        d0, d1 = dep_words(D)
        e("ldp q24, q28, [x2]"); e(f"bit v24.16b, {d0}.16b, v30.16b"); e(f"bit v28.16b, {d1}.16b, v31.16b"); e("stp q24, q28, [x2]")
    e("8:")

def latch(D, M=0, R=0):
    """the next group (x5 pixels left): pointers, the z DDA (the perspective steps or the affine lanes' products
    advance in the group's head)"""
    e("subs x5, x5, #8"); e("b.ls 9f")
    e(f"add x1, x1, #{16 if M == 1 else 32}"); e(f"add x2, x2, #{16 if M == 2 else 32}")
    if R: e("add x4, x4, #8")
    z_step8(D)
    e("b 0b")

def vis_store(D):
    """visibility: the attribute words (as store()) and the polygon index into the owner line (x1); returns the
    out-of-line partial group"""
    e("orr x0, x0, x8")
    d0, d1 = dep_words(D)
    if "idx" in RL: ix = RL["idx"][0]
    else: e(f"ldr q27, [x19, #{K['idx16']}]"); ix = "v27"
    e("cmn x8, #1"); e("b.ne 1f")
    e(f"stp {q(d0)}, {q(d1)}, [x2]"); e(f"str {q(ix)}, [x1]")
    e("8:"); e("str d28, [x3], #8")
    e("7:")
    def ool():
        e("1:"); e("str d28, [x3], #8")
        e("sshll v28.8h, v28.8b, #0")
        e(f"ldr q24, [x1]"); e(f"bit v24.16b, {ix}.16b, v28.16b"); e("str q24, [x1]")
        e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
        e(f"ldp q24, q28, [x2]"); e(f"bit v24.16b, {d0}.16b, v30.16b"); e(f"bit v28.16b, {d1}.16b, v31.16b"); e("stp q24, q28, [x2]")
        e("b 7b")
    return ool

def owner_test(fail="8f"):
    """the pixels of the group this polygon owns (owner line x2 == kargs.idx16): m8 -> v28; none -> `fail`"""
    e("ldr q27, [x2]")
    if "idx" in RL: e(f"cmeq v27.8h, v27.8h, {RL['idx'][0]}.8h")
    else: e(f"ldr q25, [x19, #{K['idx16']}]"); e("cmeq v27.8h, v27.8h, v25.8h")
    tail("v27")
    e("xtn v28.8b, v27.8h"); e("fmov x8, d28"); e(f"cbz x8, {fail}")

def lazy_loop(D, T, R, F, M):
    """the group loop until the line's first group with a passing pixel (20:): the depth (or owner) test and the
    attribute pointer and depth steps only (the colour pointer, the id index and the pass masks catch up at the
    first pass: no pass mask before it is set, and a line without one skips the edge marking); then (27:) the
    line's weights and interpolants (v28, x8: the pass mask kept), and on into the full loop at the weights (28:)"""
    e("20:")
    if M == 2: owner_test("21f")
    else: depth(D); test(D, "21f", 34)
    e("b 27f")
    e("21:")
    e("subs x5, x5, #8"); e(f"b.ls {'9f' if R or M == 2 else '15f'}")
    if M: e(f"add x1, x1, #{16 if M == 1 else 32}")
    e(f"add x2, x2, #{16 if M == 2 else 32}")
    z_step8(D)
    e("b 20b")
    if M != 2: test_equal(D, 34)
    e("27:")
    steps_setup(D, True)                                                         # (w9 = C)
    if M == 0:                                                                   # the colour line pointer
        if HR: e(f"ldr w16, [x19, #{K['attr_off']}]"); e("sub x1, x2, x16")
        else: e("sub x1, x2, #0x10, lsl #12")
    if R: e("sub x4, x9, x5")                                                    # the id index
    elif M != 2:                                                                 # edge marking: the pass masks
        e("tbz w27, #6, 1f"); e("sub x4, x9, x5"); e("add x3, x3, x4"); e("1:")  # start at the first passing pixel
    interpolants_setup(T, F, M)
    if T and not F and M != 1: white_test()
    e("28:")                                                                     # (and the depth-equal head's)
    if KD:                                  # the attribute words back (the line setup used v25 v26), st beside them
        e("ldp q25, q26, [sp, #64]"); e("tbnz w7, #0, 40f"); steps(("v24", "v29", "v30", "v31")); advance(0); e("b 5f")
    else: e("tbnz w7, #0, 40f"); steps(); advance(0); e("b 5f")
    e("40:"); advance(1); e("b 5f")

def shade_store(cols):
    """shade: pack, fog bit, store the colour words (all 8 straight when the polygon owns them all); the latch at
    8: (the owner test's none); returns the out-of-line partial group"""
    cl, ch = pack(cols)
    e("cmn x8, #1"); e("b.ne 1f")
    e(f"stp {q(cl)}, {q(ch)}, [x1]")
    e("8:")
    def ool():
        e("1:")
        e("sshll v28.8h, v28.8b, #0"); e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
        e(f"ldp q24, q28, [x1]"); e(f"bit v24.16b, {cl}.16b, v30.16b"); e(f"bit v28.16b, {ch}.16b, v31.16b"); e("stp q24, q28, [x1]")
        e("b 8b")
    return ool

def kernel_vis(D, T):
    """visibility pass of a deferred opaque polygon; T only for textures whose alpha test can fail"""
    global RL, KD
    name = f"{PFX}v{D}{T}"
    RL = roles(D, T, 0, 0, 0, 1); KD = False
    st = uses_st(D, T, 0, 1)
    prologue(name, D, T, 0, 0, 1)
    if lazy(D, st): lazy_loop(D, T, 0, 0, 1)
    e("0:")
    if D == 1: head_w(1)
    elif lazy(D, st): head(D, 1)
    else: depth(D); test(D)
    if T:
        texture(T); modulate_alpha("1f"); e("1:"); alpha_test()
    OOLS.append(vis_store(D))
    latch(D, 1)
    flush_ools()
    if not st: test_equal(D)
    depth_ool(D)
    e("9:")
    line_end(0, lazy(D, st))
    epilogue()
    e(f".size {name}, .-{name}")
    e()

def kernel_shade(T, F, B):
    """shade pass of a deferred opaque polygon: the pixels it owns"""
    global RL, KD
    name = f"{PFX}s{T}{F}{B}"
    RL = roles(2, T, 0, F, B, 2); KD = False
    st = uses_st(2, T, F, 2)
    prologue(name, 2, T, 0, F, 2, B)
    if lazy(2, st): lazy_loop(2, T, 0, F, 2)
    e("0:")
    if lazy(2, st): head(2, 2)
    else: owner_test()
    OOLS.append(shade_store(colour(T, F, B, 2)))
    latch(2, 2)
    flush_ools()
    e("9:")
    e("add x22, x22, #4"); e("add w24, w24, #1"); e("subs w23, w23, #1"); e("b.ne 10b")
    epilogue()
    e(f".size {name}, .-{name}")
    e()

def kernel(D, T, R, F, B):
    global RL, KD
    name = f"{PFX}{D}{T}{R}{F}{B}"
    RL = roles(D, T, R, F, B, 0)
    KD = keep_dep(D, T, R, F, B, 0)
    st = uses_st(D, T, F, 0)
    prologue(name, D, T, R, F, 0, B)
    if lazy(D, st): lazy_loop(D, T, R, F, 0)
    e("0:")
    if D == 1: head_w(0)
    elif lazy(D, st): head(D, 0)
    else: depth(D); test(D)
    cols = colour(T, F, B)
    OOLS.append(trans_store(D, cols) if R else store(D, cols))
    latch(D, 0, R)
    flush_ools()
    if not st: test_equal(D)
    depth_ool(D)
    e("9:")
    line_end(R, lazy(D, st))
    epilogue()
    e(f".size {name}, .-{name}")
    e()

e("// generated by kerngen.py: do not edit")
e(".text")
def all_kernels():
    for D in range(3):
        for T in range(5):
            for R in range(2):
                for F in range(2):
                    for B in range(2 if T else 1):
                        kernel(D, T, R, F, B)
    for D in range(3):
        for T in range(5): kernel_vis(D, T)
    for T in range(5):
        for F in range(2):
            for B in range(2 if T else 1): kernel_shade(T, F, B)
all_kernels()                                       # DraStic's layout (the exact 2x path)
span_layout(0x100); HR = True; PFX = "rast_kern_h"; FRAME = 1040  # the hi-res pipeline (hr.c): lines of 768
all_kernels()
e(".section .rodata")
e(".balign 16")
e("rast_kern_tail:")
# the masks of n = 0..8 pixels (a line of C = 0 runs one group with an empty mask; 8: tail(free=True)'s full groups)
for n in range(9): e(".hword " + ", ".join("0xffff" if i < n else "0" for i in range(8)))
e(".float 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0")
e(".word 0, 1, 2, 3, 4, 5, 6, 7")
e(".float 8.0, 8.0, 8.0, 8.0")
e('.section .note.GNU-stack,"",%progbits')
# the scheduler's memory facts (kernsched.py): the registers' roles, the same in every kernel. x6 x21 the texture and
# the palette, x19 the kernel arguments, x20 the constant tables, x22 the span entries: read-only. x1 x2 two distinct
# lines (colour and attributes; the deferred kernels' owner line in x1 or x2). x3 the pass masks (the frame's bytes
# from 256 on: every sp + offset access is below) or a line of ids. x28 = sp + 32.
kernsched.REGIONS.update({"x6": "ro", "x21": "ro", "x19": "ro", "x20": "ro", "x22": "ro", "x1": ("buf", "x1"),
                          "x2": ("buf", "x2"), "x3": ("window", 256, 1 << 20), "x28": ("stack", 32)})
if os.environ.get("KERNSCHED", "1") != "0": out = schedule(out)    # (KERNSCHED=0: in the order written, to compare)
open(sys.argv[1] if len(sys.argv) > 1 else "rast_kern.S", "w").write("\n".join(out) + "\n")
