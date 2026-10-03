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
Runtime flags: bit 0 affine steps (w constant), bit 1 depth-equal test, bit 2 white vertex colour (flat batches; the
kernels compute it per line otherwise), bit 6 edge marking; translucent only: bit 3 alpha blending (DISP3DCNT bit 3),
bit 4 fog (attr bit 15), bit 5 depth update (attr bit 11). Translucent kernels store each line's first id in id0[].
The per-pixel math is that of fused_neon.c's batch_neon (opaque path), lane for lane: see there and spec/ for
the derivation of every operation. The point of generating assembly is register allocation: the group loop keeps
~20 constants and ~8 temporaries live, and clang spills and shuffles about 40 instructions per 8 pixels.

Register use in the group loop:
  v0-v3   num_l num_h den_l den_h (perspective steps)      v4 v5   E0 E1 splats, or the affine lane counters
  v6-v9   z: za zb zs4 z8 (u64 pairs)   w: dW W0 splats   const: K splat
  v10-v14 rb gb bb ub vb (s32 splats)   v15  deltas dr dg db tw du dv (h lanes 0-5)
  v16 v17 texture s and t masks (W-1, H-1)   v18-v21 bilinear only   v22 pid<<24 splat   v23 bytes A aref fog pid fr fg fb
  v24-v31 temporaries
  x1 colour ptr  x2 attribute ptr (both advance per group)  x3 pass masks (opaque) / id line (translucent)  x4 i0
  (visibility kernels: x1 the owner line; shade kernels: x2 the owner line)
  x5 C  x6 texels  x7 flags  x8 scratch  x9-x16 gather  x17 Rwc  x19 kargs  x20 tail-mask table  x21 palette
  x22 span entry  x23 lines left  x24 bin line  x25 ctx  x26 id0  x27 batch flags  x28 attribute line base  x0 anypass
Stack: [sp] 8 texel addresses, [sp,#32] 8 texels, [sp,#64] the depth words of the group, [sp,#96..] spills
(bilinear: st, the five bases, the four corner weights, the four nearest-corner masks), [sp,#256] the line's pass masks."""
import sys

# kargs_t layout (fused_asm.c mirrors it): the per-polygon constants; everything per line comes from the span entry
K = dict(recip=0x00, recip_u=0x08, tex=0x10, pal=0x18, pid24=0x20, bytes=0x30, K=0x38, tw=0x3c,
         s_and=0x40, t_and=0x50, s_lo=0x60, t_lo=0x70, s_hi=0x80, t_hi=0x90, s_flip=0xa0, t_flip=0xb0,
         fraclut=0xc0, owner=0xd0, idx16=0xe0, lstride=0xf0, attr_off=0xf4, id_off=0xf8, id_stride=0xfc,
         owner_stride=0x100, size=0x110)
# span entry fields (stride 4 per line), see spec/edges.c; SPS = bytes per array (DraStic's 0xb0; the hi-res
# pipeline's 48-line bins use 0x100), HR = the hi-res kernel set: buffer strides from kargs instead of DraStic's
# context layout (line 0x800 bytes, attributes at +0x10000, ids at +0x20000 with 0x200 per line, owners 0x400)
SPS, HR, PFX = 0xb0, False, "rast_kern_"
def span_layout(stride):
    global SPS, SP
    SPS = stride
    SP = dict(W0=0, dW=stride, Z0=2 * stride, dZ=3 * stride, st0=4 * stride, dst=5 * stride, rg0=6 * stride,
              drg=7 * stride, xb=8 * stride, cdb=9 * stride, edges=10 * stride)
span_layout(0xb0)
# lanes of the delta register v15
L_DR, L_DG, L_DB, L_TW, L_DU, L_DV = 0, 1, 2, 3, 4, 5

out = []
def e(s=""): out.append("\t" + s if s and not s.endswith(":") and not s.startswith(".") else s)

def prologue(name, D, T, R, F, M=0):
    """M: 0 the kernel, 1 visibility, 2 shade"""
    e(f".globl {name}"); e(f".type {name}, %function"); e(f"{name}:")
    e("stp d8, d9, [sp, #-160]!"); e("stp d10, d11, [sp, #16]"); e("stp d12, d13, [sp, #32]"); e("stp d14, d15, [sp, #48]")
    e("stp x19, x20, [sp, #64]"); e("stp x21, x22, [sp, #80]"); e("stp x23, x24, [sp, #96]"); e("stp x25, x26, [sp, #112]")
    e("stp x27, x28, [sp, #128]")
    e("sub sp, sp, #784")
    e("mov x19, x0"); e("mov x22, x1"); e("mov w23, w2"); e("mov w24, w3"); e("mov x25, x4"); e("mov w27, w5"); e("mov x26, x6")
    e("adrp x20, rast_kern_tail"); e("add x20, x20, :lo12:rast_kern_tail")
    if T:
        e(f"ldr q16, [x19, #{K['s_and']}]"); e(f"ldr q17, [x19, #{K['t_and']}]")
        e(f"ldp x6, x21, [x19, #{K['tex']}]")
    e(f"ldr q22, [x19, #{K['pid24']}]"); e(f"ldr d23, [x19, #{K['bytes']}]")
    e("mov x0, #0")
    # ---- per line ----
    e("10:")
    e("mov x9, x22"); e("mov w7, w27")
    e(f"ldrh w5, [x9, #{SP['cdb']}]")                                           # C
    e(f"ldrh w8, [x9, #{SP['xb']}]")                                            # X
    if HR:
        e(f"ldr w10, [x19, #{K['lstride']}]"); e("mul x10, x24, x10"); e("add x1, x25, x10"); e("add x1, x1, x8, lsl #2")
        e(f"ldr w10, [x19, #{K['attr_off']}]"); e("add x2, x1, x10"); e("mov x28, x2")
        if M:
            e(f"ldr w10, [x19, #{K['owner_stride']}]"); e("mul x10, x24, x10"); e(f"ldr x11, [x19, #{K['owner']}]")
            e(f"add x{M}, x11, x10"); e(f"add x{M}, x{M}, x8, lsl #1")
        if R:
            e(f"ldr w10, [x19, #{K['id_stride']}]"); e("mul x10, x24, x10"); e(f"ldr w11, [x19, #{K['id_off']}]")
            e("add x3, x25, x11"); e("add x3, x3, x10"); e("add x3, x3, x8")
        else: e("add x3, sp, #256")
    else:
        e("lsl x10, x24, #11"); e("add x1, x25, x10"); e("add x1, x1, x8, lsl #2")
        e("add x2, x1, #0x10, lsl #12"); e("mov x28, x2")
        if M == 1: e(f"ldr x10, [x19, #{K['owner']}]"); e("add x1, x10, x24, lsl #10"); e("add x1, x1, x8, lsl #1")
        if M == 2: e(f"ldr x10, [x19, #{K['owner']}]"); e("add x2, x10, x24, lsl #10"); e("add x2, x2, x8, lsl #1")
        if R: e("lsl x10, x24, #9"); e("add x3, x25, x10"); e("add x3, x3, #0x20, lsl #12"); e("add x3, x3, x8")
        else: e("add x3, sp, #256")
    # perspective steps: num_j = j * W0, den_j = (W0 + dW) * C - j * dW, as floats; or the affine lane counters
    e(f"ldr w8, [x9, #{SP['W0']}]"); e(f"ldr w10, [x9, #{SP['dW']}]")
    e("add w11, w8, w10"); e("scvtf s24, w11"); e("ucvtf s25, w5"); e("fmul s24, s24, s25")
    e("scvtf s26, w8"); e("scvtf s27, w10")
    e("ldp q28, q29, [x20, #128]")                                               # j = 0..7 as floats
    e("fmul v0.4s, v28.4s, v26.s[0]"); e("fmul v1.4s, v29.4s, v26.s[0]")
    e("dup v2.4s, v24.s[0]"); e("dup v3.4s, v24.s[0]")
    e("fmls v2.4s, v28.4s, v27.s[0]"); e("fmls v3.4s, v29.4s, v27.s[0]")
    e("tbnz w7, #0, 1f")
    e("fmov s28, #8.0"); e("fmul s26, s26, s28"); e("fmul s27, s27, s28"); e("dup v4.4s, v26.s[0]"); e("dup v5.4s, v27.s[0]")
    e("b 2f")
    e("1:"); e("ldp q4, q5, [x20, #160]"); e(f"ldr x11, [x19, #{K['recip_u']}]"); e("ldr w17, [x11, w5, uxtw #2]")
    e("2:")
    # depth
    if D == 0:
        e(f"ldr w8, [x9, #{SP['Z0']}]"); e(f"ldrsw x10, [x9, #{SP['dZ']}]")
        e(f"ldr x11, [x19, #{K['recip']}]"); e("ldrsw x11, [x11, w5, uxtw #2]")
        e("mul x12, x10, x11"); e("mov w13, #0x3fffffff"); e("cmp x10, #0"); e("csel x13, x13, xzr, lt"); e("add x12, x12, x13")
        e("lsl x8, x8, #30")
        e("dup v9.2d, x12"); e("dup v6.2d, x8"); e("add x10, x8, x12"); e("mov v6.d[1], x10")
        e("shl v8.2d, v9.2d, #1"); e("add v7.2d, v6.2d, v8.2d"); e("shl v8.2d, v9.2d, #2"); e("shl v9.2d, v9.2d, #3")
    elif D == 1:
        e(f"add x8, x9, #{SP['dW']}"); e("ld1r {v6.4s}, [x8]"); e("ld1r {v7.4s}, [x9]")
    else:
        e(f"add x8, x19, #{K['K']}"); e("ld1r {v6.4s}, [x8]")
    # interpolants
    e(f"ldr w8, [x9, #{SP['rg0']}]"); e(f"ldr w10, [x9, #{SP['drg']}]"); e(f"ldr w11, [x9, #{SP['xb']}]")
    e(f"ldr w12, [x9, #{SP['cdb']}]"); e(f"ldr w13, [x9, #{SP['st0']}]"); e(f"ldr w14, [x9, #{SP['dst']}]")
    e("ubfiz w15, w8, #15, #16"); e("dup v10.4s, w15")
    e("lsr w15, w8, #16"); e("lsl w15, w15, #15"); e("dup v11.4s, w15")
    e("lsr w15, w11, #16"); e("lsl w15, w15, #15"); e("dup v12.4s, w15")
    e("sbfiz w15, w13, #15, #16"); e("sxth w16, w14"); e("cmp w16, #0"); e("cset w16, gt"); e("add w15, w15, w16, lsl #10"); e("dup v13.4s, w15")
    e("asr w15, w13, #16"); e("lsl w15, w15, #15"); e("asr w16, w14, #16"); e("cmp w16, #0"); e("cset w16, gt"); e("add w15, w15, w16, lsl #10"); e("dup v14.4s, w15")
    e("mov v15.s[0], w10"); e("lsr w15, w12, #16"); e(f"mov v15.h[{L_DB}], w15")
    e(f"ldrh w15, [x19, #{K['tw']}]"); e(f"mov v15.h[{L_TW}], w15"); e("mov v15.s[2], w14")
    if T and not F and M != 1:
        # white vertex colour with alpha 31: the texel is the colour
        e("mov w15, #0x1ff"); e("movk w15, #0x1ff, lsl #16"); e("cmp w8, w15")
        e("ccmp w10, #0, #0, eq")
        e("lsr w16, w11, #16"); e("mov w15, #0x1ff"); e("ccmp w16, w15, #0, eq")
        e("lsr w16, w12, #16"); e("ccmp w16, #0, #0, eq")
        e(f"ldrb w16, [x19, #{K['bytes']}]"); e("ccmp w16, #31, #0, eq")
        e("cset w15, eq"); e("orr w7, w7, w15, lsl #2")
    e("mov x4, #0")

def line_end(R):
    """after a line's group loop: edge marking fix-up (opaque) or the first id (translucent); next line"""
    if R:
        e("ldrb w8, [x3]"); e("strb w8, [x26], #1")
    else:
        # mark_edges: byte 3 of the attribute := 0x40 (then the id again) on the first EL and last ER pixels this
        # polygon wrote; EL may be C + 1 on the polygon's last line (DraStic marks its padding)
        e("tbz w27, #6, 15f")
        e(f"ldr w13, [x19, #{K['pid24']}]")
        e(f"ldrh w8, [x22, #{SP['edges']}]"); e("cmp w8, w5"); e("csel w8, w8, w5, lo"); e("mov x10, #0")
        e("11:"); e("cmp x10, x8"); e("b.hs 12f")
        e("ldrb w12, [x3, x10]"); e("cbz w12, 13f")
        e("ldr w12, [x28, x10, lsl #2]"); e("and w12, w12, #0xffffff"); e("orr w12, w12, #0x40000000"); e("orr w12, w12, w13"); e("str w12, [x28, x10, lsl #2]")
        e("13:"); e("add x10, x10, #1"); e("b 11b")
        e("12:")
        e(f"ldrh w8, [x22, #{SP['edges'] + 2}]"); e("cmp w8, w5"); e("csel w8, w8, w5, lo"); e("sub x10, x5, x8")
        e("14:"); e("cmp x10, x5"); e("b.hs 15f")
        e("ldrb w12, [x3, x10]"); e("cbz w12, 16f")
        e("ldr w12, [x28, x10, lsl #2]"); e("and w12, w12, #0xffffff"); e("orr w12, w12, #0x40000000"); e("orr w12, w12, w13"); e("str w12, [x28, x10, lsl #2]")
        e("16:"); e("add x10, x10, #1"); e("b 14b")
        e("15:")
    e("add x22, x22, #4"); e("add w24, w24, #1"); e("subs w23, w23, #1"); e("b.ne 10b")

def epilogue():
    e("add sp, sp, #784")
    e("ldp x27, x28, [sp, #128]"); e("ldp x25, x26, [sp, #112]"); e("ldp x23, x24, [sp, #96]"); e("ldp x21, x22, [sp, #80]")
    e("ldp x19, x20, [sp, #64]")
    e("ldp d14, d15, [sp, #48]"); e("ldp d12, d13, [sp, #32]"); e("ldp d10, d11, [sp, #16]"); e("ldp d8, d9, [sp], #160")
    e("ret")

def steps():
    """st -> v24 (8 x s16 Q15 perspective weights)"""
    e("tbnz w7, #0, 1f")
    e("frecpe v24.4s, v2.4s"); e("frecpe v25.4s, v3.4s")
    e("frecps v26.4s, v24.4s, v2.4s"); e("frecps v27.4s, v25.4s, v3.4s")
    e("fmul v24.4s, v24.4s, v26.4s"); e("fmul v25.4s, v25.4s, v27.4s")
    e("frecps v26.4s, v24.4s, v2.4s"); e("frecps v27.4s, v25.4s, v3.4s")
    e("fmul v24.4s, v24.4s, v26.4s"); e("fmul v25.4s, v25.4s, v27.4s")
    e("fmul v24.4s, v0.4s, v24.4s"); e("fmul v25.4s, v1.4s, v25.4s")
    e("fcvtzs v24.4s, v24.4s, #15"); e("fcvtzs v25.4s, v25.4s, #15")
    e("xtn v24.4h, v24.4s"); e("xtn2 v24.8h, v25.4s")
    e("b 2f")
    e("1:")
    e("dup v26.4s, w17"); e("mul v24.4s, v4.4s, v26.4s"); e("mul v25.4s, v5.4s, v26.4s")
    e("shrn v24.4h, v24.4s, #16"); e("shrn2 v24.8h, v25.4s, #16")
    e("2:")

def depth(D):
    """dep -> v25 (pixels 0-3), v26 (4-7); the constant variant reads v6"""
    if D == 0:
        e("shrn v25.2s, v6.2d, #30"); e("shrn2 v25.4s, v7.2d, #30")
        e("add v26.2d, v6.2d, v8.2d"); e("add v27.2d, v7.2d, v8.2d")
        e("shrn v26.2s, v26.2d, #30"); e("shrn2 v26.4s, v27.2d, #30")
    elif D == 1:
        e("sshll v26.4s, v24.4h, #0"); e("sshll2 v27.4s, v24.8h, #0")
        e("smull v25.2d, v26.2s, v6.2s"); e("smull2 v26.2d, v26.4s, v6.4s")
        e("shrn v25.2s, v25.2d, #15"); e("shrn2 v25.4s, v26.2d, #15")
        e("add v25.4s, v25.4s, v7.4s")
        e("smull v26.2d, v27.2s, v6.2s"); e("smull2 v27.2d, v27.4s, v6.4s")
        e("shrn v26.2s, v26.2d, #15"); e("shrn2 v26.4s, v27.2d, #15")
        e("add v26.4s, v26.4s, v7.4s")

def test(D):
    """depth test against the attribute words: m8 -> v28; spills dep to [sp,#64]; fails to 8f"""
    dl, dh = ("v6", "v6") if D == 2 else ("v25", "v26")
    e("ldp q27, q28, [x2]")
    e("bic v27.4s, #0xff, lsl #24"); e("bic v28.4s, #0xff, lsl #24")
    e("tbnz w7, #1, 1f")
    e(f"cmhi v27.4s, v27.4s, {dl}.4s"); e(f"cmhi v28.4s, v28.4s, {dh}.4s")
    e("b 2f")
    e("1:")
    e(f"sub v27.4s, {dl}.4s, v27.4s"); e(f"sub v28.4s, {dh}.4s, v28.4s")
    e("abs v27.4s, v27.4s"); e("abs v28.4s, v28.4s")
    e("movi v29.4s, #1, lsl #8")
    e("cmhi v27.4s, v29.4s, v27.4s"); e("cmhi v28.4s, v29.4s, v28.4s")
    e("2:")
    e("uzp1 v27.8h, v27.8h, v28.8h")
    e("add x8, x4, #8"); e("cmp x8, x5"); e("b.ls 3f")
    e("sub x8, x5, x4"); e("ldr q28, [x20, x8, lsl #4]"); e("and v27.16b, v27.16b, v28.16b")
    e("3:")
    e("xtn v28.8b, v27.8h"); e("fmov x8, d28"); e("cbz x8, 8f")
    if D != 2: e("stp q25, q26, [sp, #64]")

def texcoord(axis, T, r):
    """r (raw s16 coordinate) -> wrapped u16 in r; v29 scratch"""
    a = "s" if axis == 0 else "t"
    if T in (2, 4):
        e(f"and {r}.16b, {r}.16b, v{16 + axis}.16b")
    else:
        e(f"ldr q29, [x19, #{K[a + '_flip']}]"); e(f"cmtst v29.8h, {r}.8h, v29.8h"); e(f"eor {r}.16b, {r}.16b, v29.16b")
        e(f"ldr q29, [x19, #{K[a + '_lo']}]"); e(f"smax {r}.8h, {r}.8h, v29.8h")
        e(f"ldr q29, [x19, #{K[a + '_hi']}]"); e(f"umin {r}.8h, {r}.8h, v29.8h")
        e(f"and {r}.16b, {r}.16b, v{16 + axis}.16b")

def gather(T):
    """8 texels at the u32 addresses in v29 (0-3) and v27 (4-7) -> v29 (texels 0-3), v30 (4-7)"""
    e("stp q29, q27, [sp]")
    e("ldp w9, w10, [sp]"); e("ldp w11, w12, [sp, #8]"); e("ldp w13, w14, [sp, #16]"); e("ldp w15, w16, [sp, #24]")
    if T in (3, 4):
        for r in range(9, 17): e(f"ldrb w{r}, [x6, w{r}, uxtw]")
        for r in range(9, 17): e(f"ldr w{r}, [x21, w{r}, uxtw #2]")
    else:
        for r in range(9, 17): e(f"ldr w{r}, [x6, w{r}, uxtw #2]")
    e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
    e("ldp q29, q30, [sp, #32]")

def texture(T):
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
    # address = u + v * W
    e(f"umull v29.4s, v27.4h, v15.h[{L_TW}]"); e(f"umull2 v27.4s, v27.8h, v15.h[{L_TW}]")
    e("uaddw v29.4s, v29.4s, v30.4h"); e("uaddw2 v27.4s, v27.4s, v30.8h")
    gather(T)
    e("uzp1 v27.8h, v29.8h, v30.8h"); e("uzp2 v30.8h, v29.8h, v30.8h")
    e("xtn v29.8b, v27.8h"); e("shrn v27.8b, v27.8h, #8")
    e("xtn v31.8b, v30.8h"); e("shrn v30.8b, v30.8h, #8")

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
        e("stp q29, q30, [sp]")
        e("ldp w9, w10, [sp]"); e("ldp w11, w12, [sp, #8]"); e("ldp w13, w14, [sp, #16]"); e("ldp w15, w16, [sp, #24]")
        if T in (3, 4):
            for r in range(9, 17): e(f"ldrb w{r}, [x6, w{r}, uxtw]")
            for r in range(9, 17): e(f"ldr w{r}, [x21, w{r}, uxtw #2]")
        else:
            for r in range(9, 17): e(f"ldr w{r}, [x6, w{r}, uxtw #2]")
        e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
        e("ldp q29, q30, [sp, #32]")
        e("uzp1 v10.8h, v29.8h, v30.8h"); e("uzp2 v11.8h, v29.8h, v30.8h")
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

def vertex_colour(ch, F, dst):
    """vertex colour channel ch (0 r, 1 g, 2 b) -> dst (8 x u8); v26 scratch"""
    if F:
        e(f"dup {dst}.8b, v23.b[{4 + ch}]")
    else:
        e(f"smull {dst}.4s, v24.4h, v15.h[{ch}]"); e(f"smull2 v26.4s, v24.8h, v15.h[{ch}]")
        e(f"addhn {dst}.4h, {dst}.4s, v{10 + ch}.4s"); e(f"addhn2 {dst}.8h, v26.4s, v{10 + ch}.4s")
        e(f"shrn {dst}.8b, {dst}.8h, #2")

def modulate(v, t, sh):
    """t = ((v+1)*(t+1)-1) >> sh, bytes; v26 scratch"""
    e(f"uaddl v26.8h, {v}.8b, {t}.8b"); e(f"umlal v26.8h, {v}.8b, {t}.8b"); e(f"shrn {t}.8b, v26.8h, #{sh}")

def alpha_test():
    """ca v30 > aref -> narrows the mask v28; fails to 8f"""
    e("dup v25.8b, v23.b[1]"); e("cmhi v25.8b, v30.8b, v25.8b"); e("and v28.8b, v28.8b, v25.8b")
    e("fmov x8, d28"); e("cbz x8, 8f")

def colour(T, F, B, M=0):
    """shaded colour -> cr v29, cg v27, cb v31, ca v30; applies the alpha test (textured) unless shading deferred
    pixels (M = 2: the visibility pass applied it); fails to 8f"""
    if T:
        if B: texture_bilinear(T)
        else: texture(T)
        e("tbnz w7, #2, 1f")                        # white vertex colour, alpha 31: the texel is the colour
        for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")):
            vertex_colour(ch, F, "v25"); modulate("v25", t, 6)
        e("dup v25.8b, v23.b[0]"); modulate("v25", "v30", 5)
        e("1:")
        if M != 2: alpha_test()
    else:
        for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")): vertex_colour(ch, F, t)
        e("dup v30.8b, v23.b[0]")

def store(D):
    """opaque: pack, fog bit, store the group (all 8 lanes straight when they all pass)"""
    e("str d28, [x3, x4]"); e("orr x0, x0, x8")
    e("dup v25.8b, v23.b[2]"); e("orr v30.8b, v30.8b, v25.8b")                   # fog bit
    e("zip1 v29.16b, v29.16b, v27.16b"); e("zip1 v31.16b, v31.16b, v30.16b")
    e("zip1 v27.8h, v29.8h, v31.8h"); e("zip2 v29.8h, v29.8h, v31.8h")           # c.l v27, c.h v29
    if D == 2: e("orr v25.16b, v6.16b, v22.16b"); e("orr v26.16b, v6.16b, v22.16b")
    else: e("ldp q25, q26, [sp, #64]"); e("orr v25.16b, v25.16b, v22.16b"); e("orr v26.16b, v26.16b, v22.16b")
    e("cmn x8, #1"); e("b.ne 1f")
    e("stp q27, q29, [x1]"); e("stp q25, q26, [x2]")
    e("b 7f")
    e("1:")
    e("sshll v28.8h, v28.8b, #0"); e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
    e("ldp q24, q28, [x1]"); e("bit v24.16b, v27.16b, v30.16b"); e("bit v28.16b, v29.16b, v31.16b"); e("stp q24, q28, [x1]")
    e("ldp q24, q28, [x2]"); e("bit v24.16b, v25.16b, v30.16b"); e("bit v28.16b, v26.16b, v31.16b"); e("stp q24, q28, [x2]")
    e("b 7f")
    e("8:"); e("str d28, [x3, x4]")                                                  # no pixel of the group: mask 0
    e("7:")

def trans_store(D):
    """translucent: blend with the destination, id test, combine, store (spec/blend.c, fused_neon.c's TRANS path).
    In: cr v29, cg v27, cb v31, ca v30, m8 v28, dep at [sp,#64]. x3 = id line pointer."""
    e("orr x0, x0, x8")
    e("stp d27, d31, [sp, #96]")                                                 # cg, cb
    e("stp d30, d28, [sp, #112]")                                                # sa, m8
    e("ldp q25, q26, [x1]")
    e("uzp2 v24.8h, v25.8h, v26.8h"); e("uzp1 v25.8h, v25.8h, v26.8h")           # dhi (b|a<<8), dlo (r|g<<8)
    e("str q24, [sp, #128]")
    e("shrn v26.8b, v24.8h, #8"); e("movi v24.8b, #0x1f"); e("and v26.8b, v26.8b, v24.8b")   # dal = dst alpha & 0x1f
    e("tbz w7, #3, 1f")
    # blend: c' = (c + c*ws + d*wd) >> 5, ws = dal ? sa : 31, wd = dal ? 31 - sa : 0
    e("ldr d30, [sp, #112]")
    e("cmeq v28.8b, v26.8b, #0"); e("movi v27.8b, #0x1f")
    e("bsl v28.8b, v27.8b, v30.8b")                                              # ws
    e("sub v27.8b, v27.8b, v30.8b"); e("cmeq v30.8b, v26.8b, #0"); e("bic v27.8b, v27.8b, v30.8b")   # wd
    e("xtn v30.8b, v25.8h")
    e("ushll v31.8h, v29.8b, #0"); e("umlal v31.8h, v29.8b, v28.8b"); e("umlal v31.8h, v30.8b, v27.8b"); e("shrn v29.8b, v31.8h, #5")
    e("ldr d31, [sp, #96]"); e("shrn v30.8b, v25.8h, #8")
    e("ushll v24.8h, v31.8b, #0"); e("umlal v24.8h, v31.8b, v28.8b"); e("umlal v24.8h, v30.8b, v27.8b"); e("shrn v31.8b, v24.8h, #5")
    e("ldr d25, [sp, #104]"); e("ldr q30, [sp, #128]"); e("xtn v30.8b, v30.8h")
    e("ushll v24.8h, v25.8b, #0"); e("umlal v24.8h, v25.8b, v28.8b"); e("umlal v24.8h, v30.8b, v27.8b"); e("shrn v25.8b, v24.8h, #5")
    e("b 2f")
    e("1:"); e("ldr d31, [sp, #96]"); e("ldr d25, [sp, #104]")
    e("2:")                                                                      # cr v29, cg v31, cb v25
    e("ldr d30, [sp, #112]")                                                     # sa
    e("umax v24.8b, v30.8b, v26.8b")                                             # a' = max(sa, dal)
    e("zip1 v29.16b, v29.16b, v31.16b"); e("zip1 v25.16b, v25.16b, v24.16b")
    e("zip1 v27.8h, v29.8h, v25.8h"); e("zip2 v29.8h, v29.8h, v25.8h")           # c.l v27, c.h v29
    e("ldr d28, [sp, #120]"); e("ldr d31, [x3, x4]")                                 # m8, dst ids
    e("movi v24.8b, #0x1f"); e("cmeq v24.8b, v30.8b, v24.8b")                   # op = sa == 31
    e("dup v25.8b, v23.b[3]")                                                    # pid
    e("cmeq v26.8b, v31.8b, v25.8b"); e("bic v26.8b, v26.8b, v24.8b"); e("bic v28.8b, v28.8b, v26.8b")  # drop: same id, not opaque
    e("and v26.8b, v28.8b, v24.8b")                                              # o8 = m & op
    e("bic v30.8b, v28.8b, v24.8b")                                              # t8 = m & ~op
    e("bsl v30.8b, v25.8b, v31.8b"); e("str d30, [x3, x4]")                          # ids = t ? pid : old
    e("sshll v30.8h, v28.8b, #0"); e("sshll v31.8h, v26.8b, #0")
    e("sshll v25.4s, v30.4h, #0"); e("sshll2 v26.4s, v30.8h, #0")               # M32 v25 v26
    e("sshll v30.4s, v31.4h, #0"); e("sshll2 v31.4s, v31.8h, #0")               # O32 v30 v31
    e("tbz w7, #4, 3f")
    # fog: colour bit 31 set; the colour mask keeps the destination's bit 31 where the pixel is not opaque
    e("orr v27.4s, #0x80, lsl #24"); e("orr v29.4s, #0x80, lsl #24")
    e("sshll v24.8h, v24.8b, #0"); e("sshll v28.4s, v24.4h, #0"); e("sshll2 v24.4s, v24.8h, #0")
    e("mvn v28.16b, v28.16b"); e("ushr v28.4s, v28.4s, #31"); e("shl v28.4s, v28.4s, #31"); e("bic v25.16b, v25.16b, v28.16b")
    e("mvn v24.16b, v24.16b"); e("ushr v24.4s, v24.4s, #31"); e("shl v24.4s, v24.4s, #31"); e("bic v26.16b, v26.16b, v24.16b")
    e("3:")
    e("ldp q24, q28, [x1]"); e("bit v24.16b, v27.16b, v25.16b"); e("bit v28.16b, v29.16b, v26.16b"); e("stp q24, q28, [x1]")
    e("tbz w7, #5, 4f")
    # depth update: the attribute's depth bits follow the whole mask, its id byte only the opaque pixels
    e("mvni v24.4s, #0xff, lsl #24"); e("bsl v24.16b, v25.16b, v30.16b")
    e("mvni v28.4s, #0xff, lsl #24"); e("bsl v28.16b, v26.16b, v31.16b")
    e("mov v30.16b, v24.16b"); e("mov v31.16b, v28.16b")
    e("4:")
    if D == 2: e("orr v25.16b, v6.16b, v22.16b"); e("orr v26.16b, v6.16b, v22.16b")
    else: e("ldp q25, q26, [sp, #64]"); e("orr v25.16b, v25.16b, v22.16b"); e("orr v26.16b, v26.16b, v22.16b")
    e("ldp q24, q28, [x2]"); e("bit v24.16b, v25.16b, v30.16b"); e("bit v28.16b, v26.16b, v31.16b"); e("stp q24, q28, [x2]")
    e("8:")

def latch(D, M=0):
    e(f"add x1, x1, #{16 if M == 1 else 32}"); e(f"add x2, x2, #{16 if M == 2 else 32}"); e("add x4, x4, #8")
    e("cmp x4, x5"); e("b.hs 9f")
    e("tbnz w7, #0, 1f")
    e("fadd v0.4s, v0.4s, v4.4s"); e("fadd v1.4s, v1.4s, v4.4s"); e("fsub v2.4s, v2.4s, v5.4s"); e("fsub v3.4s, v3.4s, v5.4s")
    e("b 2f")
    e("1:"); e("movi v24.4s, #8"); e("add v4.4s, v4.4s, v24.4s"); e("add v5.4s, v5.4s, v24.4s")
    e("2:")
    if D == 0: e("add v6.2d, v6.2d, v9.2d"); e("add v7.2d, v7.2d, v9.2d")
    e("b 0b")

def vis_store(D):
    """visibility: the attribute words (as store()) and the polygon index into the owner line (x1)"""
    e("str d28, [x3, x4]"); e("orr x0, x0, x8")
    if D == 2: e("orr v25.16b, v6.16b, v22.16b"); e("orr v26.16b, v6.16b, v22.16b")
    else: e("ldp q25, q26, [sp, #64]"); e("orr v25.16b, v25.16b, v22.16b"); e("orr v26.16b, v26.16b, v22.16b")
    e(f"ldr q27, [x19, #{K['idx16']}]")
    e("cmn x8, #1"); e("b.ne 1f")
    e("stp q25, q26, [x2]"); e("str q27, [x1]")
    e("b 7f")
    e("1:")
    e("sshll v28.8h, v28.8b, #0")
    e("ldr q24, [x1]"); e("bit v24.16b, v27.16b, v28.16b"); e("str q24, [x1]")
    e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
    e("ldp q24, q28, [x2]"); e("bit v24.16b, v25.16b, v30.16b"); e("bit v28.16b, v26.16b, v31.16b"); e("stp q24, q28, [x2]")
    e("b 7f")
    e("8:"); e("str d28, [x3, x4]")
    e("7:")

def owner_test():
    """the pixels of the group this polygon owns (owner line x2 == kargs.idx16): m8 -> v28; none -> 8f"""
    e("ldr q27, [x2]"); e(f"ldr q25, [x19, #{K['idx16']}]"); e("cmeq v27.8h, v27.8h, v25.8h")
    e("add x8, x4, #8"); e("cmp x8, x5"); e("b.ls 3f")
    e("sub x8, x5, x4"); e("ldr q28, [x20, x8, lsl #4]"); e("and v27.16b, v27.16b, v28.16b")
    e("3:")
    e("xtn v28.8b, v27.8h"); e("fmov x8, d28"); e("cbz x8, 8f")

def shade_store():
    """shade: pack, fog bit, store the colour words (all 8 straight when the polygon owns them all)"""
    e("dup v25.8b, v23.b[2]"); e("orr v30.8b, v30.8b, v25.8b")                   # fog bit
    e("zip1 v29.16b, v29.16b, v27.16b"); e("zip1 v31.16b, v31.16b, v30.16b")
    e("zip1 v27.8h, v29.8h, v31.8h"); e("zip2 v29.8h, v29.8h, v31.8h")           # c.l v27, c.h v29
    e("cmn x8, #1"); e("b.ne 1f")
    e("stp q27, q29, [x1]")
    e("b 8f")
    e("1:")
    e("sshll v28.8h, v28.8b, #0"); e("sshll v30.4s, v28.4h, #0"); e("sshll2 v31.4s, v28.8h, #0")
    e("ldp q24, q28, [x1]"); e("bit v24.16b, v27.16b, v30.16b"); e("bit v28.16b, v29.16b, v31.16b"); e("stp q24, q28, [x1]")
    e("8:")

def kernel_vis(D, T):
    """visibility pass of a deferred opaque polygon; T only for textures whose alpha test can fail"""
    name = f"{PFX}v{D}{T}"
    prologue(name, D, T, 0, 0, 1)
    e("0:")
    if D == 1 or T: steps()
    depth(D); test(D)
    if T:
        texture(T)
        e("dup v25.8b, v23.b[0]"); modulate("v25", "v30", 5)
        alpha_test()
    vis_store(D)
    latch(D, 1)
    e("9:")
    line_end(0)
    epilogue()
    e(f".size {name}, .-{name}")
    e()

def kernel_shade(T, F, B):
    """shade pass of a deferred opaque polygon: the pixels it owns"""
    name = f"{PFX}s{T}{F}{B}"
    prologue(name, 2, T, 0, F, 2)
    e("0:")
    steps(); owner_test(); colour(T, F, B, 2); shade_store()
    latch(2, 2)
    e("9:")
    e("add x22, x22, #4"); e("add w24, w24, #1"); e("subs w23, w23, #1"); e("b.ne 10b")
    epilogue()
    e(f".size {name}, .-{name}")
    e()

def kernel(D, T, R, F, B):
    name = f"{PFX}{D}{T}{R}{F}{B}"
    prologue(name, D, T, R, F)
    e("0:")
    steps(); depth(D); test(D); colour(T, F, B)
    if R: trans_store(D)
    else: store(D)
    latch(D)
    e("9:")
    line_end(R)
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
span_layout(0x100); HR = True; PFX = "rast_kern_h"  # the hi-res pipeline (hr.c)
all_kernels()
e(".section .rodata")
e(".balign 16")
e("rast_kern_tail:")
for n in range(8): e(".hword " + ", ".join("0xffff" if i < n else "0" for i in range(8)))
e(".float 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0")
e(".word 0, 1, 2, 3, 4, 5, 6, 7")
e('.section .note.GNU-stack,"",%progbits')
open(sys.argv[1] if len(sys.argv) > 1 else "rast_kern.S", "w").write("\n".join(out) + "\n")
