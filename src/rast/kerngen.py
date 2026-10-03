#!/usr/bin/env python3
"""kerngen.py [out.S]: generates rast_kern.S, the opaque pixel kernels of the fused rasterizer in AArch64 assembly.

One function per variant, rast_kern_<D><T><R><F>(kargs_t *a) -> anypass (uint64):
  D: depth source  0 z (DDA in u64), 1 w (dW * step >> 15 + W0), 2 constant
  T: texture       0 none, 1 direct, 2 direct wrap/wrap, 3 paletted, 4 paletted wrap/wrap
  R: 0 opaque, 1 translucent (blend, id test, combine)
  F: flat colour   0 interpolated vertex colour, 1 the batch's flat colour
Runtime flags (kargs.flags): bit 0 affine steps (w constant), bit 1 depth-equal test, bit 2 white vertex colour,
translucent only: bit 3 alpha blending (DISP3DCNT bit 3), bit 4 fog (attr bit 15), bit 5 depth update (attr bit 11).
The per-pixel math is that of fused_neon.c's batch_neon (opaque path), lane for lane: see there and spec/ for
the derivation of every operation. The point of generating assembly is register allocation: the group loop keeps
~20 constants and ~8 temporaries live, and clang spills and shuffles about 40 instructions per 8 pixels.

Register use in the group loop:
  v0-v3   num_l num_h den_l den_h (perspective steps)      v4 v5   E0 E1 splats, or the affine lane counters
  v6-v9   z: za zb zs4 z8 (u64 pairs)   w: dW W0 splats   const: K splat
  v10-v14 rb gb bb ub vb (s32 splats)   v15  deltas dr dg db du dv tw (h lanes 0-5)
  v16 v17 texture s and t masks (W-1, H-1)   v22 pid<<24 splat   v23 bytes A aref fog pid fr fg fb
  v24-v31 temporaries
  x1 colour line ptr  x2 attribute line ptr  x3 pass-mask ptr (opaque) / id line ptr (translucent)  x4 i0  x5 C
  x6 texels  x7 flags  x8 scratch
  x9-x16 gather  x17 Rwc  x19 kargs  x20 tail-mask table  x21 palette  x0 anypass
Stack: [sp] 8 texel addresses, [sp,#32] 8 texels, [sp,#64] the depth words of the group, [sp,#96..] spills."""
import sys

# kargs_t layout (fused_asm.c mirrors it)
K = dict(num=0x00, den=0x20, E0=0x40, E1=0x44, za0=0x48, zstep=0x50, Rwc=0x58, W0=0x5c, dW=0x60, K=0x64,
         base=0x68, dl=0x7c, C=0x8c, col=0x90, att=0x98, pm=0xa0, flags=0xa8,
         pid24=0xb0, bytes=0xc0, tex=0xc8, pal=0xd0,
         s_and=0xe0, t_and=0xf0, s_lo=0x100, t_lo=0x110, s_hi=0x120, t_hi=0x130, s_flip=0x140, t_flip=0x150, size=0x160)

out = []
def e(s=""): out.append("\t" + s if s and not s.endswith(":") and not s.startswith(".") else s)

def prologue(name, D, T, R, F):
    e(f".globl {name}"); e(f".type {name}, %function"); e(f"{name}:")
    e("stp d8, d9, [sp, #-96]!"); e("stp d10, d11, [sp, #16]"); e("stp d12, d13, [sp, #32]"); e("stp d14, d15, [sp, #48]")
    e("stp x19, x20, [sp, #64]"); e("str x21, [sp, #80]")
    e("sub sp, sp, #160")
    e("mov x19, x0")
    e(f"ldr w7, [x19, #{K['flags']}]")
    e(f"ldr w5, [x19, #{K['C']}]")
    e(f"ldp x1, x2, [x19, #{K['col']}]")
    e(f"ldr x3, [x19, #{K['pm']}]")
    e("adrp x20, rast_kern_tail"); e("add x20, x20, :lo12:rast_kern_tail")
    # steps
    e(f"ldp q0, q1, [x19, #{K['num']}]"); e(f"ldp q2, q3, [x19, #{K['den']}]")
    e("tbnz w7, #0, 1f")
    e(f"add x8, x19, #{K['E0']}"); e("ld1r {v4.4s}, [x8]"); e(f"add x8, x19, #{K['E1']}"); e("ld1r {v5.4s}, [x8]")
    e("b 2f")
    e("1:"); e("adrp x8, rast_kern_iota"); e("add x8, x8, :lo12:rast_kern_iota"); e("ldp q4, q5, [x8]")
    e(f"ldr w17, [x19, #{K['Rwc']}]")
    e("2:")
    # depth
    if D == 0:
        e(f"ldp x8, x9, [x19, #{K['za0']}]")
        e("dup v9.2d, x9"); e("dup v6.2d, x8"); e("add x10, x8, x9"); e("mov v6.d[1], x10")
        e("shl v8.2d, v9.2d, #1"); e("add v7.2d, v6.2d, v8.2d"); e("shl v8.2d, v9.2d, #2"); e("shl v9.2d, v9.2d, #3")
    elif D == 1:
        e(f"add x8, x19, #{K['dW']}"); e("ld1r {v6.4s}, [x8]"); e(f"add x8, x19, #{K['W0']}"); e("ld1r {v7.4s}, [x8]")
    else:
        e(f"add x8, x19, #{K['K']}"); e("ld1r {v6.4s}, [x8]")
    # interpolants
    e(f"ldr q24, [x19, #{K['base']}]")
    e("dup v10.4s, v24.s[0]"); e("dup v11.4s, v24.s[1]"); e("dup v12.4s, v24.s[2]"); e("dup v13.4s, v24.s[3]")
    e(f"add x8, x19, #{K['base'] + 16}"); e("ld1r {v14.4s}, [x8]")
    e(f"ldur q15, [x19, #{K['dl']}]")
    if T:
        e(f"ldr q16, [x19, #{K['s_and']}]"); e(f"ldr q17, [x19, #{K['t_and']}]")
        e(f"ldp x6, x21, [x19, #{K['tex']}]")
    e(f"ldr q22, [x19, #{K['pid24']}]"); e(f"ldr d23, [x19, #{K['bytes']}]")
    e("mov x4, #0"); e("mov x0, #0")

def epilogue():
    e("9:")
    e("add sp, sp, #160")
    e("ldr x21, [sp, #80]"); e("ldp x19, x20, [sp, #64]")
    e("ldp d14, d15, [sp, #48]"); e("ldp d12, d13, [sp, #32]"); e("ldp d10, d11, [sp, #16]"); e("ldp d8, d9, [sp], #96")
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

def texcoord(axis, T):
    """v27 (raw s16 coordinate) -> wrapped u16 in v27; v29 scratch"""
    a = "s" if axis == 0 else "t"
    if T in (2, 4):
        e(f"and v27.16b, v27.16b, v{16 + axis}.16b")
    else:
        e(f"ldr q29, [x19, #{K[a + '_flip']}]"); e("cmtst v29.8h, v27.8h, v29.8h"); e("eor v27.16b, v27.16b, v29.16b")
        e(f"ldr q29, [x19, #{K[a + '_lo']}]"); e("smax v27.8h, v27.8h, v29.8h")
        e(f"ldr q29, [x19, #{K[a + '_hi']}]"); e("umin v27.8h, v27.8h, v29.8h")
        e(f"and v27.16b, v27.16b, v{16 + axis}.16b")

def texture(T):
    """texel channels -> tr v29, tg v27, tb v31, ta v30 (8 x u8 each)"""
    # u
    e("smull v27.4s, v24.4h, v15.h[3]"); e("smull2 v29.4s, v24.8h, v15.h[3]")
    e("addhn v27.4h, v27.4s, v13.4s"); e("addhn2 v27.8h, v29.4s, v13.4s")
    e("sshr v27.8h, v27.8h, #3")
    texcoord(0, T)
    e("mov v30.16b, v27.16b")                       # tu
    # v
    e("smull v27.4s, v24.4h, v15.h[4]"); e("smull2 v29.4s, v24.8h, v15.h[4]")
    e("addhn v27.4h, v27.4s, v14.4s"); e("addhn2 v27.8h, v29.4s, v14.4s")
    e("sshr v27.8h, v27.8h, #3")
    texcoord(1, T)                                  # tv in v27
    # address = u + v * W
    e("umull v29.4s, v27.4h, v15.h[5]"); e("umull2 v27.4s, v27.8h, v15.h[5]")
    e("uaddw v29.4s, v29.4s, v30.4h"); e("uaddw2 v27.4s, v27.4s, v30.8h")
    e("stp q29, q27, [sp]")
    e("ldp w9, w10, [sp]"); e("ldp w11, w12, [sp, #8]"); e("ldp w13, w14, [sp, #16]"); e("ldp w15, w16, [sp, #24]")
    if T in (3, 4):
        for r in range(9, 17): e(f"ldrb w{r}, [x6, w{r}, uxtw]")
        for r in range(9, 17): e(f"ldr w{r}, [x21, w{r}, uxtw #2]")
    else:
        for r in range(9, 17): e(f"ldr w{r}, [x6, w{r}, uxtw #2]")
    e("stp w9, w10, [sp, #32]"); e("stp w11, w12, [sp, #40]"); e("stp w13, w14, [sp, #48]"); e("stp w15, w16, [sp, #56]")
    e("ldp q29, q30, [sp, #32]")
    e("uzp1 v27.8h, v29.8h, v30.8h"); e("uzp2 v30.8h, v29.8h, v30.8h")
    e("xtn v29.8b, v27.8h"); e("shrn v27.8b, v27.8h, #8")
    e("xtn v31.8b, v30.8h"); e("shrn v30.8b, v30.8h, #8")

def vertex_colour(ch, F, dst):
    """vertex colour channel ch (0 r, 1 g, 2 b) -> dst (8 x u8); v26 scratch"""
    if F:
        e(f"dup {dst}.8b, v23.b[{4 + ch}]")
    else:
        e(f"smull {dst}.4s, v24.4h, v15.h[{ch}]"); e(f"smull2 v26.4s, v24.8h, v15.h[{ch}]")
        e(f"addhn {dst}.4h, {dst}.4s, v{10 + ch}.4s"); e(f"addhn2 {dst}.8h, v26.4s, v{10 + ch}.4s")
        e(f"ushr {dst}.8h, {dst}.8h, #2"); e(f"xtn {dst}.8b, {dst}.8h")

def modulate(v, t, sh):
    """t = ((v+1)*(t+1)-1) >> sh, bytes; v26 scratch"""
    e(f"uaddl v26.8h, {v}.8b, {t}.8b"); e(f"umlal v26.8h, {v}.8b, {t}.8b"); e(f"shrn {t}.8b, v26.8h, #{sh}")

def colour(T, F):
    """shaded colour -> cr v29, cg v27, cb v31, ca v30; applies the alpha test (textured); fails to 8f"""
    if T:
        texture(T)
        e("tbnz w7, #2, 1f")                        # white vertex colour, alpha 31: the texel is the colour
        for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")):
            vertex_colour(ch, F, "v25"); modulate("v25", t, 6)
        e("dup v25.8b, v23.b[0]"); modulate("v25", "v30", 5)
        e("1:")
        e("dup v25.8b, v23.b[1]"); e("cmhi v25.8b, v30.8b, v25.8b"); e("and v28.8b, v28.8b, v25.8b")
        e("fmov x8, d28"); e("cbz x8, 8f")
    else:
        for ch, t in ((0, "v29"), (1, "v27"), (2, "v31")): vertex_colour(ch, F, t)
        e("dup v30.8b, v23.b[0]")

def store(D):
    """opaque: pack, fog bit, store the group (all 8 lanes straight when they all pass)"""
    e("str d28, [x3]"); e("orr x0, x0, x8")
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
    e("8:"); e("str d28, [x3]")                                                  # no pixel of the group: mask 0
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
    e("ldr d28, [sp, #120]"); e("ldr d31, [x3]")                                 # m8, dst ids
    e("movi v24.8b, #0x1f"); e("cmeq v24.8b, v30.8b, v24.8b")                   # op = sa == 31
    e("dup v25.8b, v23.b[3]")                                                    # pid
    e("cmeq v26.8b, v31.8b, v25.8b"); e("bic v26.8b, v26.8b, v24.8b"); e("bic v28.8b, v28.8b, v26.8b")  # drop: same id, not opaque
    e("and v26.8b, v28.8b, v24.8b")                                              # o8 = m & op
    e("bic v30.8b, v28.8b, v24.8b")                                              # t8 = m & ~op
    e("bsl v30.8b, v25.8b, v31.8b"); e("str d30, [x3]")                          # ids = t ? pid : old
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

def latch(D):
    e("add x1, x1, #32"); e("add x2, x2, #32"); e("add x3, x3, #8"); e("add x4, x4, #8")
    e("cmp x4, x5"); e("b.hs 9f")
    e("tbnz w7, #0, 1f")
    e("fadd v0.4s, v0.4s, v4.4s"); e("fadd v1.4s, v1.4s, v4.4s"); e("fsub v2.4s, v2.4s, v5.4s"); e("fsub v3.4s, v3.4s, v5.4s")
    e("b 2f")
    e("1:"); e("movi v24.4s, #8"); e("add v4.4s, v4.4s, v24.4s"); e("add v5.4s, v5.4s, v24.4s")
    e("2:")
    if D == 0: e("add v6.2d, v6.2d, v9.2d"); e("add v7.2d, v7.2d, v9.2d")
    e("b 0b")

def kernel(D, T, R, F):
    name = f"rast_kern_{D}{T}{R}{F}"
    prologue(name, D, T, R, F)
    e("0:")
    steps(); depth(D); test(D); colour(T, F)
    if R: trans_store(D)
    else: store(D)
    latch(D)
    epilogue()
    e(f".size {name}, .-{name}")
    e()

e("// generated by kerngen.py: do not edit")
e(".text")
for D in range(3):
    for T in range(5):
        for R in range(2):
            for F in range(2):
                kernel(D, T, R, F)
e(".section .rodata")
e(".balign 16")
e("rast_kern_tail:")
for n in range(8): e(".hword " + ", ".join("0xffff" if i < n else "0" for i in range(8)))
e("rast_kern_iota:")
e(".word 0, 1, 2, 3, 4, 5, 6, 7")
e('.section .note.GNU-stack,"",%progbits')
open(sys.argv[1] if len(sys.argv) > 1 else "rast_kern.S", "w").write("\n".join(out) + "\n")
