/* ds2d_asm.S: replacements of DraStic's hand-written 2D scanline routines (ds2d.c hooks them), the same bytes in
 * fewer A55 cycles. Each keeps the original's argument registers and clobbers no register outside the original's
 * own clobber set (its callers are C, see ds2d.c), so ds2d.c can point the hook straight at it.
 *
 * Cycle figures: llvm-mca -mcpu=cortex-a55 on the loop (in order, dual issue; a 128-bit NEON op takes both halves of
 * the 64-bit datapath; a load with writeback, ld1 of several registers or ld2/3/4 holds the load pipe for several
 * cycles and its result retires in order, so the original loops stall behind their own loads). */
    .text

/* void ds2d_conv32_2x(const uint8_t *a, const uint8_t *b, uint32_t *out)
 * render_scanline_color_convert_direct_32_2x_asm (0xa0ae0): two 6-bit planar quarters (a, b: bytes 0..255 at +0x200,
 * +0x100, +0) interleaved into one output row of 512 xRGB pixels:
 *     out[2i]   = a[0x200+i] << 2 | a[0x100+i] << 2 << 8 | a[i] << 2 << 16 | alpha(2i) << 24
 *     out[2i+1] = the same from b,                                          alpha(2i+1) << 24
 * (each << 2 within its byte), alpha(p) = 0xff for p % 16 in {0, 2, 4, 6}, else 0: DraStic's st4 of v19 / v23 =
 * movi .4h #0xff (the low 8 bytes ff 00 ff 00 ..., the high 8 bytes zero) as the 4th byte of every 16 pixels.
 * The original's loop (ld1 with writeback, 32 pixels a step) models at 50 cycles a step; this one loads with ldr q at
 * a register offset, one step ahead (the zips of step k beside the loads of step k+1, the shifts of k+1 under the
 * first st4), 23 cycles a step, the last step's zips and stores after the loop. It reads exactly the 6 x 256 input
 * bytes and writes exactly out[0..511]. Clobbers x3-x7, v0-v5, v16-v23 (the original: x0-x7, the same vectors). */
    .p2align 4
    .globl ds2d_conv32_2x
    .hidden ds2d_conv32_2x
    .type ds2d_conv32_2x, %function
ds2d_conv32_2x:
    add   x4, x0, #0x100
    add   x5, x1, #0x100
    add   x6, x0, #0x200
    add   x7, x1, #0x200
    ldr   q0, [x6]
    ldr   q3, [x7]
    ldr   q1, [x4]
    ldr   q4, [x5]
    ldr   q2, [x0]
    ldr   q5, [x1]
    movi  v19.4h, #0xff
    movi  v23.4h, #0xff
    mov   x3, #16
    shl   v0.16b, v0.16b, #2
    shl   v3.16b, v3.16b, #2
    shl   v1.16b, v1.16b, #2
    shl   v4.16b, v4.16b, #2
    shl   v2.16b, v2.16b, #2
    shl   v5.16b, v5.16b, #2
1:  zip1  v16.16b, v0.16b, v3.16b
    zip2  v20.16b, v0.16b, v3.16b
    ldr   q0, [x6, x3]
    zip1  v17.16b, v1.16b, v4.16b
    ldr   q3, [x7, x3]
    zip2  v21.16b, v1.16b, v4.16b
    ldr   q1, [x4, x3]
    zip1  v18.16b, v2.16b, v5.16b
    ldr   q4, [x5, x3]
    zip2  v22.16b, v2.16b, v5.16b
    ldr   q2, [x0, x3]
    ldr   q5, [x1, x3]
    add   x3, x3, #16
    shl   v0.16b, v0.16b, #2
    shl   v3.16b, v3.16b, #2
    st4   {v16.16b, v17.16b, v18.16b, v19.16b}, [x2], #64
    shl   v1.16b, v1.16b, #2
    shl   v4.16b, v4.16b, #2
    shl   v2.16b, v2.16b, #2
    shl   v5.16b, v5.16b, #2
    cmp   x3, #256
    st4   {v20.16b, v21.16b, v22.16b, v23.16b}, [x2], #64
    b.ne  1b
    zip1  v16.16b, v0.16b, v3.16b
    zip2  v20.16b, v0.16b, v3.16b
    zip1  v17.16b, v1.16b, v4.16b
    zip2  v21.16b, v1.16b, v4.16b
    zip1  v18.16b, v2.16b, v5.16b
    zip2  v22.16b, v2.16b, v5.16b
    st4   {v16.16b, v17.16b, v18.16b, v19.16b}, [x2], #64
    st4   {v20.16b, v21.16b, v22.16b, v23.16b}, [x2], #64
    ret
    .size ds2d_conv32_2x, . - ds2d_conv32_2x

/* void ds2d_conv32_1x(const uint8_t *a, uint32_t *out)
 * render_scanline_color_convert_direct_32_1x_asm (0xa0a80): one 6-bit planar line (bytes at +0x200, +0x100, +0 as
 * above) into 256 xRGB pixels, out[i] = a[0x200+i] << 2 | a[0x100+i] << 2 << 8 | a[i] << 2 << 16 | 0xff << 24 (each
 * << 2 within its byte; movi .16b #0xff: every alpha byte set). The original (ld1 with writeback) models at 43
 * cycles per 32 pixels, this loop (ldr q at register offsets into v16-v22, the shifts into v0-v6) at 21. Runs at 1x
 * (hires_3d off) only. Reads exactly the 3 x 256 input bytes, writes exactly out[0..255]. Clobbers x2-x5, v0-v7,
 * v16-v22 (the original: x0-x4, v0-v7). */
    .p2align 4
    .globl ds2d_conv32_1x
    .hidden ds2d_conv32_1x
    .type ds2d_conv32_1x, %function
ds2d_conv32_1x:
    add   x3, x0, #0x100
    add   x4, x0, #0x200
    movi  v3.16b, #0xff
    movi  v7.16b, #0xff
    mov   x2, #0
    mov   x5, #16
1:  ldr   q16, [x4, x2]
    ldr   q17, [x3, x2]
    ldr   q18, [x0, x2]
    ldr   q20, [x4, x5]
    ldr   q21, [x3, x5]
    ldr   q22, [x0, x5]
    shl   v0.16b, v16.16b, #2
    shl   v1.16b, v17.16b, #2
    shl   v2.16b, v18.16b, #2
    add   x2, x2, #32
    shl   v4.16b, v20.16b, #2
    add   x5, x5, #32
    shl   v5.16b, v21.16b, #2
    shl   v6.16b, v22.16b, #2
    st4   {v0.16b, v1.16b, v2.16b, v3.16b}, [x1], #64
    cmp   x2, #256
    st4   {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], #64
    b.ne  1b
    ret
    .size ds2d_conv32_1x, . - ds2d_conv32_1x

/* void ds2d_effects_apply(uint8_t *out, const uint8_t *planes, const uint8_t *wa, const uint8_t *wb)
 * render_scanline_color_effects_apply_asm (0xa0378): the blend of two 6-bit planar quarters (planes: the first
 * layer's R, G, B at +0, +0x100, +0x200, the second's at +0x300, +0x400, +0x500) with per-pixel weights:
 *     out[c * 0x100 + i] = umin(63, (u8)((p1[c][i] * wa[i] + p2[c][i] * wb[i] + 16) >> 5))      c = 0, 1, 2
 * (umull / umlal into 16 bits, rshrn #5 narrowing to the low 8 bits, umin 63: the original's instructions, so the
 * same bytes for any inputs). The original's loop (eight ld1 with writeback, 16 pixels a step) models at 89 cycles a
 * step; this one does 32 pixels a step with ldr q / str q at register offsets, the second half's products beside the
 * first half's narrowing and stores: 63 cycles. Reads the 6 x 256 plane bytes and both weight arrays, writes exactly
 * out[0..0x2ff], each 32-pixel block after its inputs are loaded (the original's order per 16; DraStic's buffers do
 * not overlap). Called by a tail branch from render_scanline_2d_composite (C). Clobbers x4-x12, v0-v7, v16-v31. */
    .p2align 4
    .globl ds2d_effects_apply
    .hidden ds2d_effects_apply
    .type ds2d_effects_apply, %function
ds2d_effects_apply:
    movi   v25.16b, #0x3f
    add     x4, x0, #0x100
    add     x5, x0, #0x200
    add     x6, x1, #0x100
    add     x7, x1, #0x200
    add     x8, x1, #0x300
    add     x9, x1, #0x400
    add     x10, x1, #0x500
    mov     x11, #0
    mov     x12, #16
1:  ldr   q0, [x2, x11]
    ldr   q2, [x1, x11]
    ldr   q3, [x6, x11]
    ldr   q4, [x7, x11]
    umull v16.8h, v2.8b, v0.8b
    ldr   q1, [x3, x11]
    umull2 v17.8h, v2.16b, v0.16b
    ldr   q5, [x8, x11]
    umull v18.8h, v3.8b, v0.8b
    ldr   q6, [x9, x11]
    umull2 v19.8h, v3.16b, v0.16b
    ldr   q7, [x10, x11]
    umull v20.8h, v4.8b, v0.8b
    ldr   q22, [x2, x12]
    umull2 v21.8h, v4.16b, v0.16b
    ldr   q24, [x1, x12]
    umlal v16.8h, v5.8b, v1.8b
    ldr   q26, [x6, x12]
    umlal2 v17.8h, v5.16b, v1.16b
    ldr   q27, [x7, x12]
    umlal v18.8h, v6.8b, v1.8b
    ldr   q23, [x3, x12]
    umlal2 v19.8h, v6.16b, v1.16b
    ldr   q28, [x8, x12]
    umlal v20.8h, v7.8b, v1.8b
    ldr   q29, [x9, x12]
    umlal2 v21.8h, v7.16b, v1.16b
    ldr   q30, [x10, x12]
    umull v2.8h, v24.8b, v22.8b
    umull2 v3.8h, v24.16b, v22.16b
    rshrn v0.8b, v16.8h, #0x5
    umull v4.8h, v26.8b, v22.8b
    rshrn2 v0.16b, v17.8h, #0x5
    umull2 v5.8h, v26.16b, v22.16b
    rshrn v1.8b, v18.8h, #0x5
    umull v6.8h, v27.8b, v22.8b
    rshrn2 v1.16b, v19.8h, #0x5
    umull2 v7.8h, v27.16b, v22.16b
    umin  v0.16b, v0.16b, v25.16b
    umlal v2.8h, v28.8b, v23.8b
    rshrn v31.8b, v20.8h, #0x5
    umlal2 v3.8h, v28.16b, v23.16b
    rshrn2 v31.16b, v21.8h, #0x5
    str   q0, [x0, x11]
    umlal v4.8h, v29.8b, v23.8b
    umin  v1.16b, v1.16b, v25.16b
    umlal2 v5.8h, v29.16b, v23.16b
    str   q1, [x4, x11]
    umlal v6.8h, v30.8b, v23.8b
    umin  v31.16b, v31.16b, v25.16b
    umlal2 v7.8h, v30.16b, v23.16b
    str   q31, [x5, x11]
    rshrn v22.8b, v2.8h, #0x5
    rshrn2 v22.16b, v3.8h, #0x5
    rshrn v23.8b, v4.8h, #0x5
    rshrn2 v23.16b, v5.8h, #0x5
    umin  v22.16b, v22.16b, v25.16b
    rshrn v24.8b, v6.8h, #0x5
    rshrn2 v24.16b, v7.8h, #0x5
    umin  v23.16b, v23.16b, v25.16b
    str   q22, [x0, x12]
    umin  v24.16b, v24.16b, v25.16b
    str   q23, [x4, x12]
    add   x11, x11, #32
    str   q24, [x5, x12]
    add   x12, x12, #32
    cmp   x11, #256
    b.ne   1b
    ret
    .size ds2d_effects_apply, . - ds2d_effects_apply

/* void ds2d_span4(uint16_t *dst, uint8_t *rows, const uint16_t *pal, const uint8_t *tiles, const uint16_t *entries,
 *                 const uint8_t *flags)
 * render_scanline_tiled_span_4bpp_asm (0x9efa0): a 4bpp text BG line, 33 tiles of 8 pixels. Tile t (t = 0..31):
 *     row = *(u32 *)(tiles + entries[t]);   f = flags[t]   (bit 2: h-flip, bits 4-7: the palette number)
 *     h-flip: row = its 8 nibbles reversed (nibbles swapped in each byte, rev32)
 *     rows[4t..4t+3] = row;   dst[8t + j] = pal[16 * (f >> 4) + nibble j of row]   (nibble 0 = the low one of byte 0)
 * and the 33rd tile as DraStic's tail does it: dst[256..263] as above from entries[32], flags[32]; rows[128..135] =
 * row (flipped per flags[32]) then the same tile row flipped per bit 2 of flags[33] (its .2s cmtst of the dup'd
 * words). The original's step (four ldr w + four ins, four ld2 of the palettes, four st2 .8b, ld1 / ldr with
 * writeback) models at 94 cycles per 4 tiles; here each step's scalar loads (flags, entries, the four rows by ldr s +
 * ld1 lanes, the palette pointers) are issued during the step before, the palettes split by ldr q + uzp, the pixels
 * interleaved by zip and stored by str q: 63 cycles. Its tail is the original's, verbatim. Reads what the original
 * reads (the tail's 8-byte entry load included), writes exactly dst[0..263] and rows[0..135]; its buffers (the BG's
 * line, render_scanline_tiled_ext's stack, VRAM, the palette) never overlap. Caller: render_scanline_tiled_ext (C).
 * Clobbers x6-x17, v0-v7, v16-v26 (the original: x0-x12, v0-v7, v16-v25). */
.macro SPAN4_STEP pref
    ldr   q5, [x9]
    ldr   q6, [x9, #16]
    dup   v4.4s, w7
    ushr  v3.16b, v2.16b, #4
.if \pref
    ldr   w13, [x5, #4]
    ldr   x8, [x4, #8]
.endif
    sli   v3.16b, v2.16b, #4
    ldr   q16, [x10]
    cmtst v4.4s, v4.4s, v0.4s
    ldr   q17, [x10, #16]
    rev32 v3.16b, v3.16b
.if \pref
    and   x14, x8, #0xffff
    ubfx  x15, x8, #16, #16
.endif
    uzp1  v7.16b, v5.16b, v6.16b
    ldr   q18, [x11]
    bit   v2.16b, v3.16b, v4.16b
    ldr   q19, [x11, #16]
    uzp2  v5.16b, v5.16b, v6.16b
.if \pref
    ubfx  x16, x8, #32, #16
    add   x15, x3, x15
.endif
    ushr  v4.16b, v2.16b, #4
    ldr   q20, [x12]
    and   v3.16b, v2.16b, v1.16b
    ldr   q21, [x12, #16]
    uzp1  v6.16b, v16.16b, v17.16b
    str   q2, [x1]
    zip1  v22.16b, v3.16b, v4.16b
.if \pref
    ldr   s26, [x3, x14]
    lsr   x17, x8, #48
.endif
    zip2  v24.16b, v3.16b, v4.16b
.if \pref
    ld1   {v26.s}[1], [x15]
    add   x16, x3, x16
.endif
    uzp2  v16.16b, v16.16b, v17.16b
.if \pref
    ld1   {v26.s}[2], [x16]
    add   x17, x3, x17
.endif
    ext   v23.16b, v22.16b, v22.16b, #8
.if \pref
    ld1   {v26.s}[3], [x17]
    ubfx  x14, x13, #4, #4
.endif
    tbl   v7.8b, {v7.16b}, v22.8b
.if \pref
    ubfx  x15, x13, #12, #4
.endif
    tbl   v5.8b, {v5.16b}, v22.8b
.if \pref
    ubfx  x16, x13, #20, #4
.endif
    ext   v25.16b, v24.16b, v24.16b, #8
.if \pref
    ubfx  x17, x13, #28, #4
.endif
    uzp1  v17.16b, v18.16b, v19.16b
    tbl   v6.8b, {v6.16b}, v23.8b
    tbl   v16.8b, {v16.16b}, v23.8b
    uzp2  v18.16b, v18.16b, v19.16b
    zip1  v7.16b, v7.16b, v5.16b
    uzp1  v19.16b, v20.16b, v21.16b
    zip1  v6.16b, v6.16b, v16.16b
    uzp2  v20.16b, v20.16b, v21.16b
    tbl   v17.8b, {v17.16b}, v24.8b
    str   q7, [x0]
    tbl   v18.8b, {v18.16b}, v24.8b
    str   q6, [x0, #16]
    tbl   v19.8b, {v19.16b}, v25.8b
.if \pref
    add   x9, x2, x14, lsl #5
    add   x10, x2, x15, lsl #5
.endif
    tbl   v20.8b, {v20.16b}, v25.8b
.if \pref
    add   x11, x2, x16, lsl #5
    add   x12, x2, x17, lsl #5
.endif
    zip1  v17.16b, v17.16b, v18.16b
.if \pref
    mov   w7, w13
    add   x4, x4, #8
.endif
    zip1  v19.16b, v19.16b, v20.16b
.if \pref
    add   x5, x5, #4
    mov   v2.16b, v26.16b
.endif
    str   q17, [x0, #32]
    add   x1, x1, #16
    str   q19, [x0, #48]
    add   x0, x0, #64
.endm

    .p2align 4
    .globl ds2d_span4
    .hidden ds2d_span4
    .type ds2d_span4, %function
ds2d_span4:
    adr   x7, ds2d_hflip_bits
    ldr   w13, [x5]
    ldr   x8, [x4]
    ld1   {v0.4s}, [x7]
    movi  v1.16b, #0xf
    and   x14, x8, #0xffff
    ubfx  x15, x8, #16, #16
    ubfx  x16, x8, #32, #16
    lsr   x17, x8, #48
    ldr   s2, [x3, x14]
    add   x15, x3, x15
    add   x16, x3, x16
    add   x17, x3, x17
    ld1   {v2.s}[1], [x15]
    ubfx  x14, x13, #4, #4
    ld1   {v2.s}[2], [x16]
    ubfx  x15, x13, #12, #4
    ld1   {v2.s}[3], [x17]
    ubfx  x16, x13, #20, #4
    ubfx  x17, x13, #28, #4
    add   x9, x2, x14, lsl #5
    add   x10, x2, x15, lsl #5
    add   x11, x2, x16, lsl #5
    add   x12, x2, x17, lsl #5
    mov   w7, w13
    mov   w6, #7
1:  SPAN4_STEP 1
    subs  w6, w6, #1
    b.ne  1b
    SPAN4_STEP 0
    /* the 33rd tile: DraStic's tail (x4, x5 point at the last step's entries and flags) */
    ldr   w7, [x5, #4]
    ldr   x8, [x4, #8]
    dup   v4.4s, w7
    ubfx  x9, x8, #0, #16
    ldr   w9, [x3, x9]
    dup   v2.4s, w9
    ushr  v3.8b, v2.8b, #4
    cmtst v4.2s, v4.2s, v0.2s
    ubfx  x9, x7, #4, #4
    sli   v3.8b, v2.8b, #4
    rev32 v3.8b, v3.8b
    add   x9, x2, x9, lsl #5
    bit   v2.8b, v3.8b, v4.8b
    st1   {v2.8b}, [x1]
    ushr  v4.16b, v2.16b, #4
    and   v3.16b, v2.16b, v1.16b
    ld2   {v5.16b, v6.16b}, [x9]
    zip1  v22.16b, v3.16b, v4.16b
    zip2  v24.16b, v3.16b, v4.16b
    tbl   v5.8b, {v5.16b}, v22.8b
    tbl   v6.8b, {v6.16b}, v22.8b
    st2   {v5.8b, v6.8b}, [x0]
    ret
    .size ds2d_span4, . - ds2d_span4
    .p2align 4
ds2d_hflip_bits:                        /* bit 2 of byte k in lane k (the original's const_hflip_test_mask_bits) */
    .word 0x00000004, 0x00000400, 0x00040000, 0x04000000

/* void ds2d_span4_orig(same arguments): DraStic's render_scanline_tiled_span_4bpp_asm for the check mode. Its first
 * instruction is an adr, so rast_hook()'s trampoline cannot run it; this does what its first four instructions do
 * (x7 = its constant, w6 = 32, v0 = the constant, v1 = 0x0f bytes) and jumps to its fifth (ds2d_span4_cont, set by
 * ds2d.c to DraStic's 0x9efb0; ds2d_span4_consts to its 0x9ef90). */
    .p2align 4
    .globl ds2d_span4_orig
    .hidden ds2d_span4_orig
    .type ds2d_span4_orig, %function
ds2d_span4_orig:
    adrp  x16, ds2d_span4_consts
    ldr   x7, [x16, :lo12:ds2d_span4_consts]
    adrp  x16, ds2d_span4_cont
    ldr   x16, [x16, :lo12:ds2d_span4_cont]
    mov   w6, #0x20
    ld1   {v0.4s}, [x7]
    movi  v1.16b, #0xf
    br    x16
    .size ds2d_span4_orig, . - ds2d_span4_orig

/* void ds2d_pal8(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n)
 * render_scanline_palette_lookup_8bpp_asm (0x9f504): dst[i] = pal[src[i]] for 8 pixels an iteration, as many
 * iterations as DraStic's loop runs: n -= 8 first, another iteration while the result is still > 0 (n <= 8: one
 * iteration; 256: 32). Callers (C, through bl): render_scanline_tiled_ext (dst = the layer's u16 line, src = its
 * 8bpp index line at line + 0x110, n = 256) and render_scanline_affine_normal_ext / _extended_ext (dst = line +
 * 2 * x0, src = line + 0x100 + x0, n = the clipped run's pixels + 1). The original's iteration (ldr x with
 * writeback, eight ands, eight ldrh, four orr, three stores, one dependence chain) models at 26 cycles; this one is
 * software pipelined over two register sets: an iteration's word is loaded and its eight indexes computed while the
 * previous iteration's gathers are in flight and their halfwords stored (16 modeled cycles per 8 pixels). Relative
 * to the original, that moves one iteration's source load ahead of the previous iteration's stores, which the
 * callers' buffers never make visible (the tiled source lies 0x110 bytes above the destination; an affine run's
 * source lies 256 bytes above its start and the run ends before reaching it); arguments where it could (dst
 * within 8 * iterations - 9 bytes below or 15 above src), and dst over the palette (each iteration's halfword
 * stores are interleaved with its own gathers; DraStic's palettes never overlap a line buffer), go to
 * ds2d_pal8_plain, the original's order. Reads exactly what the original reads; writes exactly dst[0 ..
 * 8 * iterations). Clobbers x4-x17 (the original: x4-x13) and saves x19-x22 on the stack (a frame the original
 * does not have; all callers are C). */
    .p2align 4
    .globl ds2d_pal8
    .hidden ds2d_pal8
    .type ds2d_pal8, %function
ds2d_pal8:
    add   w7, w3, #7
    asr   w7, w7, #3                 /* the iterations (n > 8), else <= 1 */
    sxtw  x7, w7
    sub   x6, x0, x1                 /* d = dst - src: a store of iteration k meets the load of k + 1 iff */
    add   x6, x6, x7, lsl #3         /* -7 <= d + 8k <= 15 for some k <= iterations - 2 */
    sub   x6, x6, #9
    lsl   x9, x7, #3
    add   x9, x9, #6
    cmp   x6, x9
    b.ls  ds2d_pal8_plain
    cmp   x7, #1                     /* dst over the palette (an iteration's stores among its own gathers): */
    csinc x8, x7, xzr, ge            /* dst - pal in (-16 * iterations, 512) */
    sub   x6, x0, x2
    add   x6, x6, x8, lsl #4
    sub   x6, x6, #1
    lsl   x9, x8, #4
    add   x9, x9, #510
    cmp   x6, x9
    b.ls  ds2d_pal8_plain
    mov   x4, #0x1fe
    stp   x19, x20, [sp, #-32]!
    stp   x21, x22, [sp, #16]
    ldr   x5, [x1], #8
    subs  w3, w3, #8
    and   x6, x4, x5, lsl #1
    and   x7, x4, x5, lsr #7
    and   x8, x4, x5, lsr #15
    and   x9, x4, x5, lsr #23
    and   x10, x4, x5, lsr #31
    and   x11, x4, x5, lsr #39
    and   x12, x4, x5, lsr #47
    and   x13, x4, x5, lsr #55
    b.le  9f                         /* one iteration */
1:
    ldr   x5, [x1], #8
    ldrh  w6, [x2, x6]
    subs  w3, w3, #8
    ldrh  w7, [x2, x7]
    add   x0, x0, #16
    ldrh  w8, [x2, x8]
    and   x14, x4, x5, lsl #1
    ldrh  w9, [x2, x9]
    and   x15, x4, x5, lsr #7
    ldrh  w10, [x2, x10]
    strh  w6, [x0, #-16]
    ldrh  w11, [x2, x11]
    and   x16, x4, x5, lsr #15
    ldrh  w12, [x2, x12]
    strh  w7, [x0, #-14]
    ldrh  w13, [x2, x13]
    and   x17, x4, x5, lsr #23
    strh  w8, [x0, #-12]
    and   x19, x4, x5, lsr #31
    strh  w9, [x0, #-10]
    and   x20, x4, x5, lsr #39
    strh  w10, [x0, #-8]
    and   x21, x4, x5, lsr #47
    strh  w11, [x0, #-6]
    and   x22, x4, x5, lsr #55
    strh  w12, [x0, #-4]
    strh  w13, [x0, #-2]
    b.le  7f
    ldr   x5, [x1], #8
    ldrh  w14, [x2, x14]
    subs  w3, w3, #8
    ldrh  w15, [x2, x15]
    add   x0, x0, #16
    ldrh  w16, [x2, x16]
    and   x6, x4, x5, lsl #1
    ldrh  w17, [x2, x17]
    and   x7, x4, x5, lsr #7
    ldrh  w19, [x2, x19]
    strh  w14, [x0, #-16]
    ldrh  w20, [x2, x20]
    and   x8, x4, x5, lsr #15
    ldrh  w21, [x2, x21]
    strh  w15, [x0, #-14]
    ldrh  w22, [x2, x22]
    and   x9, x4, x5, lsr #23
    strh  w16, [x0, #-12]
    and   x10, x4, x5, lsr #31
    strh  w17, [x0, #-10]
    and   x11, x4, x5, lsr #39
    strh  w19, [x0, #-8]
    and   x12, x4, x5, lsr #47
    strh  w20, [x0, #-6]
    and   x13, x4, x5, lsr #55
    strh  w21, [x0, #-4]
    strh  w22, [x0, #-2]
    b.gt  1b
9:
    ldrh  w6, [x2, x6]
    ldrh  w7, [x2, x7]
    ldrh  w8, [x2, x8]
    ldrh  w9, [x2, x9]
    strh  w6, [x0, #0]
    ldrh  w10, [x2, x10]
    strh  w7, [x0, #2]
    ldrh  w11, [x2, x11]
    strh  w8, [x0, #4]
    ldrh  w12, [x2, x12]
    strh  w9, [x0, #6]
    ldrh  w13, [x2, x13]
    strh  w10, [x0, #8]
    strh  w11, [x0, #10]
    strh  w12, [x0, #12]
    strh  w13, [x0, #14]
    ldp   x21, x22, [sp, #16]
    ldp   x19, x20, [sp], #32
    ret
7:
    ldrh  w14, [x2, x14]
    ldrh  w15, [x2, x15]
    ldrh  w16, [x2, x16]
    ldrh  w17, [x2, x17]
    strh  w14, [x0, #0]
    ldrh  w19, [x2, x19]
    strh  w15, [x0, #2]
    ldrh  w20, [x2, x20]
    strh  w16, [x0, #4]
    ldrh  w21, [x2, x21]
    strh  w17, [x0, #6]
    ldrh  w22, [x2, x22]
    strh  w19, [x0, #8]
    strh  w20, [x0, #10]
    strh  w21, [x0, #12]
    strh  w22, [x0, #14]
    ldp   x21, x22, [sp, #16]
    ldp   x19, x20, [sp], #32
    ret
    .size ds2d_pal8, . - ds2d_pal8

/* the original's order (its loop, instruction for instruction) for the overlapping arguments above */
    .p2align 4
    .globl ds2d_pal8_plain
    .hidden ds2d_pal8_plain
    .type ds2d_pal8_plain, %function
ds2d_pal8_plain:
    mov   x4, #0x1fe
1:  ldr   x5, [x1], #8
    subs  w3, w3, #8
    and   x6, x4, x5, lsl #1
    and   x7, x4, x5, lsr #7
    and   x8, x4, x5, lsr #15
    and   x9, x4, x5, lsr #23
    ldrh  w6, [x2, x6]
    and   x10, x4, x5, lsr #31
    ldrh  w7, [x2, x7]
    and   x11, x4, x5, lsr #39
    ldrh  w8, [x2, x8]
    and   x12, x4, x5, lsr #47
    ldrh  w9, [x2, x9]
    and   x13, x4, x5, lsr #55
    ldrh  w10, [x2, x10]
    orr   w6, w6, w7, lsl #16
    ldrh  w11, [x2, x11]
    orr   w7, w8, w9, lsl #16
    ldrh  w12, [x2, x12]
    str   w6, [x0], #4
    ldrh  w13, [x2, x13]
    orr   w8, w10, w11, lsl #16
    str   w7, [x0], #4
    orr   w9, w12, w13, lsl #16
    stp   w8, w9, [x0], #8
    b.gt  1b
    ret
    .size ds2d_pal8_plain, . - ds2d_pal8_plain

/* void ds2d_pal12(uint16_t *dst, const uint16_t *src, const uint16_t *pal, int n)
 * render_scanline_palette_lookup_12bpp_asm (0x9f574): dst[i] = pal[src[i] & 0xfff] (the extended palettes: a
 * palette number in the index's high byte), 8 pixels an iteration from two 8-byte loads, the iteration count as
 * ds2d_pal8's. Callers (C, bl): render_scanline_tiled_ext (in place: dst = src = the layer's line, n = 256) and
 * render_scanline_affine_extended_ext (in place at line + 2 * x0, n = the run + 1). Pipelined as ds2d_pal8 (the
 * original 28 modeled cycles an iteration, this 17); the reordering is visible only with dst 1 to 31 bytes above
 * src: those arguments, and dst over the palette, take ds2d_pal12_plain. Clobbers x4-x17 (the original: x4-x14),
 * saves x19-x22 on the stack. */
    .p2align 4
    .globl ds2d_pal12
    .hidden ds2d_pal12
    .type ds2d_pal12, %function
ds2d_pal12:
    sub   x6, x0, x1
    sub   x6, x6, #1
    cmp   x6, #31
    b.lo  ds2d_pal12_plain
    add   w7, w3, #7                 /* dst over the 8 KiB palette, as ds2d_pal8 */
    asr   w7, w7, #3
    sxtw  x7, w7
    cmp   x7, #1
    csinc x8, x7, xzr, ge
    sub   x6, x0, x2
    add   x6, x6, x8, lsl #4
    sub   x6, x6, #1
    add   x9, x8, #512
    lsl   x9, x9, #4
    sub   x9, x9, #2
    cmp   x6, x9
    b.ls  ds2d_pal12_plain
    mov   x4, #0x1ffe
    stp   x19, x20, [sp, #-32]!
    stp   x21, x22, [sp, #16]
    ldr   x5, [x1], #8
    ldr   x10, [x1], #8
    subs  w3, w3, #8
    and   x6, x4, x5, lsl #1
    and   x7, x4, x5, lsr #15
    and   x8, x4, x5, lsr #31
    and   x9, x4, x5, lsr #47
    and   x11, x4, x10, lsr #15
    and   x12, x4, x10, lsr #31
    and   x13, x4, x10, lsr #47
    and   x10, x4, x10, lsl #1
    b.le  9f
1:
    ldr   x5, [x1], #8
    ldr   x19, [x1], #8
    ldrh  w6, [x2, x6]
    subs  w3, w3, #8
    ldrh  w7, [x2, x7]
    add   x0, x0, #16
    ldrh  w8, [x2, x8]
    and   x14, x4, x5, lsl #1
    ldrh  w9, [x2, x9]
    and   x15, x4, x5, lsr #15
    ldrh  w10, [x2, x10]
    strh  w6, [x0, #-16]
    ldrh  w11, [x2, x11]
    and   x16, x4, x5, lsr #31
    ldrh  w12, [x2, x12]
    strh  w7, [x0, #-14]
    ldrh  w13, [x2, x13]
    and   x17, x4, x5, lsr #47
    strh  w8, [x0, #-12]
    and   x20, x4, x19, lsr #15
    strh  w9, [x0, #-10]
    and   x21, x4, x19, lsr #31
    strh  w10, [x0, #-8]
    and   x22, x4, x19, lsr #47
    strh  w11, [x0, #-6]
    and   x19, x4, x19, lsl #1
    strh  w12, [x0, #-4]
    strh  w13, [x0, #-2]
    b.le  7f
    ldr   x5, [x1], #8
    ldr   x10, [x1], #8
    ldrh  w14, [x2, x14]
    subs  w3, w3, #8
    ldrh  w15, [x2, x15]
    add   x0, x0, #16
    ldrh  w16, [x2, x16]
    and   x6, x4, x5, lsl #1
    ldrh  w17, [x2, x17]
    and   x7, x4, x5, lsr #15
    ldrh  w19, [x2, x19]
    strh  w14, [x0, #-16]
    ldrh  w20, [x2, x20]
    and   x8, x4, x5, lsr #31
    ldrh  w21, [x2, x21]
    strh  w15, [x0, #-14]
    ldrh  w22, [x2, x22]
    and   x9, x4, x5, lsr #47
    strh  w16, [x0, #-12]
    and   x11, x4, x10, lsr #15
    strh  w17, [x0, #-10]
    and   x12, x4, x10, lsr #31
    strh  w19, [x0, #-8]
    and   x13, x4, x10, lsr #47
    strh  w20, [x0, #-6]
    and   x10, x4, x10, lsl #1
    strh  w21, [x0, #-4]
    strh  w22, [x0, #-2]
    b.gt  1b
9:
    ldrh  w6, [x2, x6]
    ldrh  w7, [x2, x7]
    ldrh  w8, [x2, x8]
    ldrh  w9, [x2, x9]
    strh  w6, [x0, #0]
    ldrh  w10, [x2, x10]
    strh  w7, [x0, #2]
    ldrh  w11, [x2, x11]
    strh  w8, [x0, #4]
    ldrh  w12, [x2, x12]
    strh  w9, [x0, #6]
    ldrh  w13, [x2, x13]
    strh  w10, [x0, #8]
    strh  w11, [x0, #10]
    strh  w12, [x0, #12]
    strh  w13, [x0, #14]
    ldp   x21, x22, [sp, #16]
    ldp   x19, x20, [sp], #32
    ret
7:
    ldrh  w14, [x2, x14]
    ldrh  w15, [x2, x15]
    ldrh  w16, [x2, x16]
    ldrh  w17, [x2, x17]
    strh  w14, [x0, #0]
    ldrh  w19, [x2, x19]
    strh  w15, [x0, #2]
    ldrh  w20, [x2, x20]
    strh  w16, [x0, #4]
    ldrh  w21, [x2, x21]
    strh  w17, [x0, #6]
    ldrh  w22, [x2, x22]
    strh  w19, [x0, #8]
    strh  w20, [x0, #10]
    strh  w21, [x0, #12]
    strh  w22, [x0, #14]
    ldp   x21, x22, [sp, #16]
    ldp   x19, x20, [sp], #32
    ret
    .size ds2d_pal12, . - ds2d_pal12

    .p2align 4
    .globl ds2d_pal12_plain
    .hidden ds2d_pal12_plain
    .type ds2d_pal12_plain, %function
ds2d_pal12_plain:
    mov   x4, #0x1ffe
1:  ldr   x5, [x1], #8
    subs  w3, w3, #8
    ldr   x6, [x1], #8
    and   x7, x4, x5, lsl #1
    and   x8, x4, x5, lsr #15
    and   x9, x4, x5, lsr #31
    and   x10, x4, x5, lsr #47
    ldrh  w7, [x2, x7]
    and   x11, x4, x6, lsl #1
    ldrh  w8, [x2, x8]
    and   x12, x4, x6, lsr #15
    ldrh  w9, [x2, x9]
    and   x13, x4, x6, lsr #31
    ldrh  w10, [x2, x10]
    and   x14, x4, x6, lsr #47
    ldrh  w11, [x2, x11]
    orr   w7, w7, w8, lsl #16
    ldrh  w12, [x2, x12]
    orr   w8, w9, w10, lsl #16
    ldrh  w13, [x2, x13]
    str   w7, [x0], #4
    ldrh  w14, [x2, x14]
    orr   w9, w11, w12, lsl #16
    str   w8, [x0], #4
    orr   w10, w13, w14, lsl #16
    stp   w9, w10, [x0], #8
    b.gt  1b
    ret
    .size ds2d_pal12_plain, . - ds2d_pal12_plain

/* The check mode's entries to the originals whose first instruction is an adr (rast_hook()'s trampoline would copy
 * it with a wrong target): each does what the original's first four instructions do, with the adr's address from a
 * variable ds2d.c sets (ds2d_orig_vars: DraStic's constant and the original's fifth instruction, per routine), and
 * jumps to the fifth. */
.macro ORIG4 name, idx, i1, i2, i3
    .p2align 4
    .globl \name
    .hidden \name
    .type \name, %function
\name:
    adrp  x16, ds2d_orig_vars
    add   x16, x16, :lo12:ds2d_orig_vars
    ldp   x17, x16, [x16, #(\idx) * 16]
    \i1
    \i2
    \i3
    br    x16
    .size \name, . - \name
.endm
/* render_scanline_color_effects_setup_alpha_base_asm (0xa042c): adr x4, bits; movi v25, 0x1f; ld1 {v24}, [x4]; movi v26, 1 */
ORIG4 ds2d_alpha_base_orig, 0, "mov x4, x17", "movi v25.16b, #0x1f", "ld1 {v24.16b}, [x4]; movi v26.16b, #1"
/* render_scanline_color_effects_setup_alpha_asm (0xa04b4): adr x4, bits; movi v29, 0x1f; ld1 {v28}, [x4]; movi v30, 1 */
ORIG4 ds2d_alpha_orig, 1, "mov x4, x17", "movi v29.16b, #0x1f", "ld1 {v28.16b}, [x4]; movi v30.16b, #1"
/* render_scanline_tiled_span_8bpp_normal_palette_asm (0x9f0f0): adr x5, c; mov w4, 32; ld1 {v0.4s}, [x5]; shl v1.4s, v0.4s, 16 */
ORIG4 ds2d_span8n_orig, 2, "mov x5, x17", "mov w4, #0x20", "ld1 {v0.4s}, [x5]; shl v1.4s, v0.4s, #16"
/* render_scanline_tiled_span_8bpp_ext_palette_asm (0x9f188): adr x5, c; movi v18, 1; ld1 {v0.4s}, [x5]; mov v18.d[0], xzr */
ORIG4 ds2d_span8e_orig, 3, "mov x5, x17", "movi v18.16b, #1", "ld1 {v0.4s}, [x5]; mov v18.d[0], xzr"
/* render_scanline_tiled_setup_tile_map_entries_4bpp_asm (0x9ee70): adr x7, table; mov x8, x30; mov w6, 28; movi v5.8h, 0x800 */
ORIG4 ds2d_tme4_orig, 4, "mov x7, x17", "mov x8, x30", "mov w6, #0x1c; movi v5.8h, #0x8, lsl #8"
/* render_scanline_tiled_setup_tile_map_entries_8bpp_asm (0x9ef08): the same with w6 = 56 */
ORIG4 ds2d_tme8_orig, 5, "mov x7, x17", "mov x8, x30", "mov w6, #0x38; movi v5.8h, #0x8, lsl #8"
/* render_scanline_set_visibility_4bpp_asm (0x9f278): adr x4, masks; mov w3, 8; sub w3, w3, w2; neg w2, w2 */
ORIG4 ds2d_vis4_orig, 6, "mov x4, x17", "mov w3, #8", "sub w3, w3, w2; neg w2, w2"
/* render_scanline_set_visibility_8bpp_asm (0x9f390): adr x2, bits; ld1 {v24.16b}, [x2]; ld1 x4 [x1], #64; ld1 x4 [x1], #64 */
ORIG4 ds2d_vis8_orig, 7, "mov x2, x17", "ld1 {v24.16b}, [x2]", "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x1], #64; ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x1], #64"
/* render_scanline_set_visibility_12bpp_asm (0x9f468): adr x2, bits; ld1 {v24.16b}, [x2]; mov w3, 2; nop */
ORIG4 ds2d_vis12_orig, 8, "mov x2, x17", "ld1 {v24.16b}, [x2]", "mov w3, #2"

/* The affine BG tile renderers: render_scanline_affine_render_tiles_normal_normal_asm (0x9f9bc),
 * _extended_normal_asm (0x9fbf8), _extended_extended_asm (0x9fd30). A line of an affine BG comes as runs of pixels
 * within one tile (count runs: widths[k] pixels from the 8x8 8bpp tile of entry k, each pixel's byte offset in the
 * tile from offs[]); a run's 16 offsets are loaded, looked up in the tile's 64 bytes by tbl and 16 bytes stored, the
 * pointers then advanced by the run's width (the next run's store overwrites the excess; widths of 16 or more loop
 * in 16s). normal_normal: entries are bytes (tile * 64 & 0x3fc0), the output 8bpp indexes; extended_normal: u16
 * entries (tile * 64 & 0xffc0), each run's offsets XORed with a flip byte (flips[k]); extended_extended: the same
 * with the entries' palette numbers (bits 12-15) as the high byte of u16 output pixels (st2). The originals load a
 * tile by ld1 of four registers (the load pipe held 8 modeled cycles), two tiles a step: 51 modeled cycles per two
 * runs (normal_normal). These are the originals instruction for instruction except: the tiles by four ldr q, the
 * narrow path's offsets by ldr q at [x1] and [x1, w0] and its stores by str q at [x0] and [x0, w0] with the pointer
 * adds after them, ld2r of the flip bytes as two ld1r: the same loads and stores in the same order (every load of a
 * step before its stores), so the same bytes for any arguments; 31 cycles per two runs. Callers: the affine BG
 * renderers render_scanline_affine_normal_ext / _extended_ext (C). The first four instructions include a cbz, so the
 * check mode reaches the originals through ds2d_aff_*_orig (below). Clobbers the originals' registers and x16, x17. */
.macro LDTILE t0, t1, t2, t3, base
    ldr   \t0, [\base]
    ldr   \t1, [\base, #16]
    ldr   \t2, [\base, #32]
    ldr   \t3, [\base, #48]
.endm

    .p2align 4
    .globl ds2d_aff_nn
    .hidden ds2d_aff_nn
    .type ds2d_aff_nn, %function
ds2d_aff_nn:                            /* (dst, offs, widths, entries, count, tiles) */
    ldrb  w7, [x2]
    mov   w12, #0x3fc0
    cbz   w7, 8f                        /* widths[0] == 0: 256 pixels from entries[0]'s tile */
    subs  w4, w4, #1
    b.eq  6f
1:  ldrh  w9, [x3], #2
    ldrh  w6, [x2], #2
    and   w10, w12, w9, lsl #6
    and   w11, w12, w9, lsr #2
    and   x7, x6, #0xff
    lsr   x8, x6, #8
    add   x10, x5, w10, uxtw
    add   x11, x5, w11, uxtw
    tst   w6, #0xf0f0f0f0
    LDTILE q0, q1, q2, q3, x10
    LDTILE q4, q5, q6, q7, x11
    b.ne  3f
    ldr   q16, [x1]
    ldr   q17, [x1, x7]
    add   x1, x1, x7
    add   x1, x1, x8
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v19.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  w4, w4, #2
    str   q18, [x0]
    str   q19, [x0, x7]
    add   x0, x0, x7
    add   x0, x0, x8
    b.gt  1b
    b.eq  6f
    ret
3:  ld1   {v16.16b}, [x1], #16          /* a run of 16 or more: the original's loops */
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  x7, x7, #16
    st1   {v18.16b}, [x0], #16
    b.gt  3b
    add   x1, x1, w7, sxtw
    add   x0, x0, w7, sxtw
4:  ld1   {v17.16b}, [x1], #16
    tbl   v19.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  x8, x8, #16
    st1   {v19.16b}, [x0], #16
    b.gt  4b
    subs  w4, w4, #2
    add   x1, x1, w8, sxtw
    add   x0, x0, w8, sxtw
    b.gt  1b
    b.ne  7f
6:  ldrb  w10, [x3]                     /* the last (odd) run */
    ldrb  w7, [x2]
    and   w10, w12, w10, lsl #6
    add   x10, x5, w10, uxtw
    LDTILE q0, q1, q2, q3, x10
5:  ld1   {v16.16b}, [x1], #16
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  w7, w7, #16
    st1   {v18.16b}, [x0], #16
    b.gt  5b
7:  ret
8:  ldrb  w10, [x3]
    mov   x7, #0x100
    and   w10, w12, w10, lsl #6
    add   x10, x5, w10, uxtw
    LDTILE q0, q1, q2, q3, x10
9:  ld1   {v16.16b, v17.16b}, [x1], #32
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v19.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v17.16b
    subs  x7, x7, #0x20
    st1   {v18.16b, v19.16b}, [x0], #32
    b.ne  9b
    ret
    .size ds2d_aff_nn, . - ds2d_aff_nn

    .p2align 4
    .globl ds2d_aff_en
    .hidden ds2d_aff_en
    .type ds2d_aff_en, %function
ds2d_aff_en:                            /* (dst, offs, widths, entries, flips, count, tiles) */
    ldrb  w8, [x2]
    mov   w13, #0xffc0
    cbz   w8, 8f
    subs  w5, w5, #1
    b.eq  6f
1:  ldr   w10, [x3], #4
    ldrh  w7, [x2], #2
    and   w11, w13, w10, lsl #6
    and   w12, w13, w10, lsr #10
    and   x8, x7, #0xff
    lsr   x9, x7, #8
    add   x11, x6, w11, uxtw
    add   x12, x6, w12, uxtw
    tst   w7, #0xf0f0f0f0
    add   x16, x4, #1
    ld1r  {v20.16b}, [x4]
    ld1r  {v21.16b}, [x16]
    add   x4, x4, #2
    LDTILE q0, q1, q2, q3, x11
    LDTILE q4, q5, q6, q7, x12
    b.ne  3f
    ldr   q16, [x1]
    ldr   q17, [x1, x8]
    add   x1, x1, x8
    add   x1, x1, x9
    eor   v16.16b, v16.16b, v20.16b
    eor   v17.16b, v17.16b, v21.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v19.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  w5, w5, #2
    str   q18, [x0]
    str   q19, [x0, x8]
    add   x0, x0, x8
    add   x0, x0, x9
    b.gt  1b
    b.eq  6f
    ret
3:  ld1   {v16.16b}, [x1], #16
    eor   v16.16b, v16.16b, v20.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  x8, x8, #16
    st1   {v18.16b}, [x0], #16
    b.gt  3b
    add   x1, x1, w8, sxtw
    add   x0, x0, w8, sxtw
4:  ld1   {v17.16b}, [x1], #16
    eor   v17.16b, v17.16b, v21.16b
    tbl   v19.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  x9, x9, #16
    st1   {v19.16b}, [x0], #16
    b.gt  4b
    subs  w5, w5, #2
    add   x1, x1, w9, sxtw
    add   x0, x0, w9, sxtw
    b.gt  1b
    b.ne  7f
6:  ldrh  w11, [x3]
    ldrb  w8, [x2]
    and   w11, w13, w11, lsl #6
    add   x11, x6, w11, uxtw
    ld1r  {v20.16b}, [x4]
    LDTILE q0, q1, q2, q3, x11
5:  ld1   {v16.16b}, [x1], #16
    eor   v16.16b, v16.16b, v20.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  w8, w8, #16
    st1   {v18.16b}, [x0], #16
    b.gt  5b
7:  ret
8:  ldrh  w11, [x3]
    mov   x8, #0x100
    and   w11, w13, w11, lsl #6
    ld1r  {v20.16b}, [x4]
    add   x11, x6, w11, uxtw
    LDTILE q0, q1, q2, q3, x11
9:  ld1   {v16.16b, v17.16b}, [x1], #32
    eor   v16.16b, v16.16b, v20.16b
    eor   v17.16b, v17.16b, v20.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v19.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v17.16b
    subs  x8, x8, #0x20
    st1   {v18.16b, v19.16b}, [x0], #32
    b.ne  9b
    ret
    .size ds2d_aff_en, . - ds2d_aff_en

    .p2align 4
    .globl ds2d_aff_ee
    .hidden ds2d_aff_ee
    .type ds2d_aff_ee, %function
ds2d_aff_ee:                            /* (u16 dst, offs, widths, entries, flips, count, tiles) */
    ldrb  w8, [x2]
    mov   w15, #0xffc0
    cbz   w8, 8f
    subs  w5, w5, #1
    b.eq  6f
1:  ldr   w10, [x3], #4
    ldrh  w7, [x2], #2
    and   w11, w15, w10, lsl #6
    and   w12, w15, w10, lsr #10
    and   x8, x7, #0xff
    lsr   x9, x7, #8
    add   x11, x6, w11, uxtw
    add   x12, x6, w12, uxtw
    ubfx  w13, w10, #12, #4
    lsr   w14, w10, #28
    tst   w7, #0xf0f0f0f0
    add   x16, x4, #1
    ld1r  {v22.16b}, [x4]
    ld1r  {v23.16b}, [x16]
    add   x4, x4, #2
    dup   v19.16b, w13
    dup   v21.16b, w14
    LDTILE q0, q1, q2, q3, x11
    LDTILE q4, q5, q6, q7, x12
    b.ne  3f
    ldr   q16, [x1]
    ldr   q17, [x1, x8]
    add   x1, x1, x8
    add   x1, x1, x9
    add   x8, x8, x8
    add   x9, x9, x9
    eor   v16.16b, v16.16b, v22.16b
    eor   v17.16b, v17.16b, v23.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v20.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  w5, w5, #2
    st2   {v18.16b, v19.16b}, [x0], x8
    st2   {v20.16b, v21.16b}, [x0], x9
    b.gt  1b
    b.eq  6f
    ret
3:  ld1   {v16.16b}, [x1], #16
    eor   v16.16b, v16.16b, v22.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  x8, x8, #16
    st2   {v18.16b, v19.16b}, [x0], #32
    b.gt  3b
    add   x1, x1, w8, sxtw
    add   x0, x0, w8, sxtw #1
4:  ld1   {v17.16b}, [x1], #16
    eor   v17.16b, v17.16b, v23.16b
    tbl   v20.16b, {v4.16b, v5.16b, v6.16b, v7.16b}, v17.16b
    subs  x9, x9, #16
    st2   {v20.16b, v21.16b}, [x0], #32
    b.gt  4b
    subs  w5, w5, #2
    add   x1, x1, w9, sxtw
    add   x0, x0, w9, sxtw #1
    b.gt  1b
    b.ne  7f
6:  ldrh  w11, [x3]
    ldrb  w8, [x2]
    ubfx  w13, w11, #12, #4
    and   w11, w15, w11, lsl #6
    add   x11, x6, w11, uxtw
    dup   v19.16b, w13
    ld1r  {v22.16b}, [x4]
    LDTILE q0, q1, q2, q3, x11
5:  ld1   {v16.16b}, [x1], #16
    eor   v16.16b, v16.16b, v22.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    subs  w8, w8, #16
    st2   {v18.16b, v19.16b}, [x0], #32
    b.gt  5b
7:  ret
8:  ldrh  w11, [x3]
    mov   x8, #0x100
    ubfx  w13, w11, #12, #4
    and   w11, w15, w11, lsl #6
    ld1r  {v22.16b}, [x4]
    dup   v19.16b, w13
    dup   v21.16b, w13
    add   x11, x6, w11, uxtw
    LDTILE q0, q1, q2, q3, x11
9:  ld1   {v16.16b, v17.16b}, [x1], #32
    eor   v16.16b, v16.16b, v22.16b
    eor   v17.16b, v17.16b, v22.16b
    tbl   v18.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v16.16b
    tbl   v20.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v17.16b
    subs  x8, x8, #0x20
    st2   {v18.16b, v19.16b}, [x0], #32
    st2   {v20.16b, v21.16b}, [x0], #32
    b.ne  9b
    ret
    .size ds2d_aff_ee, . - ds2d_aff_ee

/* the check mode's entries to the three originals: their first four instructions (the third a cbz on widths[0]),
 * then a jump to the fifth (ds2d_orig_vars[i] = { the cbz's target, the fifth instruction }) */
.macro AFFORIG name, idx, i1, i2, w, i4
    .p2align 4
    .globl \name
    .hidden \name
    .type \name, %function
\name:
    adrp  x16, ds2d_orig_vars
    add   x16, x16, :lo12:ds2d_orig_vars
    ldp   x17, x16, [x16, #(\idx) * 16]
    \i1
    \i2
    cbz   \w, 1f
    \i4
    br    x16
1:  br    x17
    .size \name, . - \name
.endm
AFFORIG ds2d_aff_nn_orig, 9, "ldrb w7, [x2]", "mov w12, #0x3fc0", w7, "subs w4, w4, #1"
AFFORIG ds2d_aff_en_orig, 10, "ldrb w8, [x2]", "mov w13, #0xffc0", w8, "subs w5, w5, #1"
AFFORIG ds2d_aff_ee_orig, 11, "ldrb w8, [x2]", "mov w15, #0xffc0", w8, "subs w5, w5, #1"
