/* gx_resolve.h: the bin resolve's fog and edge-marking passes, NEON, over GX_W-pixel lines: hr.c's (SuperDrastic PR #8,
 * a63fe5b, src/rast/hr.c), with its 768-pixel constants made gx.c's. Included by gx.c only. */
/* fog of a line in place: spec/resolve.c's fog_calculate_weights and fog_modulate_full/alpha_intermediate in one
 * pass, NEON over 16 pixels with colours and attributes split into byte planes (ld4), so a pixel's weight is one
 * byte lane and the weights never go to memory.
 * Weight: d = sat(((attr >> 9) & 0x7fff) - off) (attr bytes 1 and 2 zipped to u16 lanes, >> 1), v = sqshl16(d,
 * shift), i = v >> 10, f = v & 0x3ff, w = table[i] + (u8)(((s8)table[32 + i] * f) >> 10). With v = f + 1024 i the
 * product is floor(dl v / 1024) - dl i, the first as sqdmulh(32 dl, v) = (64 dl v) >> 16 (exact: |32 dl| <= 4096,
 * v <= 0x7fff), the second folded into the table: -w = (dl i - table[i]) - (u8)floor(dl v / 1024) from a per-call
 * table of dl i - table[i]. The factor k = -w (and -128 for 127, i.e. -w = 0x81) where the pixel's fog flag (alpha
 * bit 7) is set, else 0: a clear flag sets the index's top bits, so both tbl lookups give 0 and k = 0.
 * Per byte c += ((s8)(c - F) * k) >> 7 (smull, then shrn #7 keeps product bits 7-14); the flag is cleared;
 * alpha-only fog leaves r, g, b. A group of 16 without any fog flag is unchanged (k = 0); a step of two such groups
 * is skipped. */
static inline __attribute__((always_inline)) uint8x16_t fog_ch16(uint8x16_t c, uint8x16_t f, int8x16_t k) {
    int8x16_t d = vreinterpretq_s8_u8(vsubq_u8(c, f));
    int16x8_t lo = vmull_s8(vget_low_s8(d), vget_low_s8(k)), hi = vmull_high_s8(d, k);
    return vaddq_u8(c, vreinterpretq_u8_s8(vshrn_high_n_s16(vshrn_n_s16(lo, 7), hi, 7)));
}
static inline __attribute__((always_inline)) void fog_line16(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params,
                                                             uint32_t fogc, int zb, uint8_t *tp, const int planes, int full) {
    static const uint8_t idx[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    const uint16x8_t off = vdupq_n_u16((uint16_t)(params >> 16));
    const int16x8_t sh = vdupq_n_s16((int8_t)(params & 0xff));
    const uint8x16x2_t dlt = { { vld1q_u8(table + 32), vld1q_u8(table + 48) } };
    const uint8x16x2_t ntab = { { vsubq_u8(vmulq_u8(dlt.val[0], vld1q_u8(idx)), vld1q_u8(table)),
                                  vsubq_u8(vmulq_u8(dlt.val[1], vld1q_u8(idx + 16)), vld1q_u8(table + 16)) } };
    const uint8x16_t fog4 = vreinterpretq_u8_u32(vdupq_n_u32(fogc)), x81 = vdupq_n_u8(0x81);
    const uint32x4_t m7f = vdupq_n_u32(0x7fffffff);                         /* the words without their fog flag */
    uint8_t *c8 = (uint8_t *)c, *end = c8 + 4 * GX_W;
    const ptrdiff_t ad = (const uint8_t *)attr - c8;                        /* one pointer: attr at c8 + ad */
    /* a fog colour byte broadcast where the modulate needs it (the fog colour in one register; four held across the
     * step make the compiler spill a colour group) */
#define FOGB(k) ({ uint8x16_t f_; __asm__ volatile("dup %0.16b, %1.b[" #k "]" : "=w"(f_) : "w"(fog4)); f_; })
    /* the factor of 16 pixels in two parts: FOG_D, the saturated depth differences d - off of the attribute words w0,
     * w1 (8 pixels; shrn #8 narrows each word to its bits 8-23, >> 1 gives the 15-bit depth; u16 lanes), and FOG_K,
     * the factor from two of them (pixels 0-7 and 8-15) and the pixels' alpha plane t (the flags) */
#define FOG_D(w0, w1) vqsubq_u16(vshrq_n_u16(vshrn_high_n_u32(vshrn_n_u32(w0, 8), w1, 8), 1), off)
#define FOG_K(t, u0, u1) ({ \
        uint16x8_t v0 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(u0), sh)); \
        uint16x8_t v1 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(u1), sh)); \
        /* i = v >> 10 (the high bytes >> 2), or >= 0xc0 without the fog flag (tbl gives 0) */ \
        uint8x16_t i = vsriq_n_u8(vcgezq_s8(vreinterpretq_s8_u8(t)), vuzp2q_u8(vreinterpretq_u8_u16(v0), vreinterpretq_u8_u16(v1)), 2); \
        int8x16_t dl = vreinterpretq_s8_u8(vqtbl2q_u8(dlt, i)); \
        int16x8_t p0 = vqdmulhq_s16(vshll_n_s8(vget_low_s8(dl), 5), vreinterpretq_s16_u16(v0)); \
        int16x8_t p1 = vqdmulhq_s16(vshll_high_n_s8(dl, 5), vreinterpretq_s16_u16(v1)); \
        uint8x16_t k = vsubq_u8(vqtbl2q_u8(ntab, i), vuzp1q_u8(vreinterpretq_u8_s16(p0), vreinterpretq_u8_s16(p1)));   /* -w */ \
        vreinterpretq_s8_u8(vaddq_u8(k, vceqq_u8(k, x81)));                 /* -128 for w = 127 */ \
    })
    /* two groups a step, so that the in-order core has two independent chains to interleave (a group's own chain,
     * from the attribute load to the store, is about 70 cycles long). The step's 16 words, attributes and colours,
     * come by ldr q at immediate offsets (one asm block each: clang pairs them into ldp q otherwise, which holds the
     * A55's load pipe 6 cycles; an ld4 of the planes holds it 8 and its result comes 11 cycles later), the planes the
     * step needs are gathered from them by tbl (a cycle each): first the attributes' depth bytes (bits 16-23): a step
     * whose 32 are all at most zb (fogzero.h's bound: from the table, the offset and the shift; -1 when there is
     * none) has every factor 0 whatever its flags, so it is its colours with the fog flags cleared, by word, without
     * the factors (S4 at 3x: 84% of the steps; ~40 modeled cycles against 80 with ld4 planes and the depths' u16
     * compare). Otherwise the colours' alpha planes: a step without any fog flag is skipped, in a step that is fogged
     * a group without flags is unchanged (k = 0), and the groups' r, g, b planes are gathered for the modulate and
     * stored interleaved by st4. */
    static const uint8_t plane_idx[32] = { 2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62,
                                           3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63 };
    const uint8x16_t didx = vld1q_u8(plane_idx);
    /* the alpha index (didx + 1) and the colour planes' (didx - 2, - 1, 0) loaded where they are used, not held
     * across the loop (the A55's load pipe is free there; held, they cost a colour group a spill) */
#define PIDX(k) ({ uint8x16_t i_; __asm__ volatile("ldr %q0, [%1, #%2]" : "=w"(i_) : "r"(plane_idx), "i"(k)); i_; })
    _Static_assert(GX_W % 32 == 0, "fog: 32 pixels a step");
    for (; c8 != end; c8 += 128) {
        const uint8_t *at = c8 + ad;
        uint32x4_t a0, a1, a2, a3, a4, a5, a6, a7, c0, c1, c2, c3, c4, c5, c6, c7;
        __asm__("ldr %q0, [%8]\n\tldr %q1, [%8, #16]\n\tldr %q2, [%8, #32]\n\tldr %q3, [%8, #48]\n\t"
                "ldr %q4, [%8, #64]\n\tldr %q5, [%8, #80]\n\tldr %q6, [%8, #96]\n\tldr %q7, [%8, #112]"
                : "=&w"(a0), "=&w"(a1), "=&w"(a2), "=&w"(a3), "=&w"(a4), "=&w"(a5), "=&w"(a6), "=&w"(a7)
                : "r"(at), "m"(*(const uint8_t (*)[128])at));
        __asm__("ldr %q0, [%8]\n\tldr %q1, [%8, #16]\n\tldr %q2, [%8, #32]\n\tldr %q3, [%8, #48]\n\t"
                "ldr %q4, [%8, #64]\n\tldr %q5, [%8, #80]\n\tldr %q6, [%8, #96]\n\tldr %q7, [%8, #112]"
                : "=&w"(c0), "=&w"(c1), "=&w"(c2), "=&w"(c3), "=&w"(c4), "=&w"(c5), "=&w"(c6), "=&w"(c7)
                : "r"(c8), "m"(*(const uint8_t (*)[128])c8));
        const uint8x16x4_t wx = { { vreinterpretq_u8_u32(a0), vreinterpretq_u8_u32(a1), vreinterpretq_u8_u32(a2), vreinterpretq_u8_u32(a3) } };
        const uint8x16x4_t wy = { { vreinterpretq_u8_u32(a4), vreinterpretq_u8_u32(a5), vreinterpretq_u8_u32(a6), vreinterpretq_u8_u32(a7) } };
        if (planes) {                   /* the words' top bytes for the edge marking's id screen (two tbl and two str) */
            const uint8x16_t ti = PIDX(16);
            vst1q_u8(tp, vqtbl4q_u8(wx, ti)); vst1q_u8(tp + 16, vqtbl4q_u8(wy, ti));
            tp += 32;
        }
        if (__builtin_expect((int)vmaxvq_u8(vmaxq_u8(vqtbl4q_u8(wx, didx), vqtbl4q_u8(wy, didx))) <= zb, 1)) {
            /* the fog flags (bit 31 of the words) cleared (and with the mask into other registers: the immediate bic
             * is destructive and the words are still wanted on the other path, so clang copied them first) */
            uint32x4_t w0 = vandq_u32(c0, m7f), w1 = vandq_u32(c1, m7f), w2 = vandq_u32(c2, m7f), w3 = vandq_u32(c3, m7f);
            uint32x4_t w4 = vandq_u32(c4, m7f), w5 = vandq_u32(c5, m7f), w6 = vandq_u32(c6, m7f), w7 = vandq_u32(c7, m7f);
            __asm__("str %q1, [%9]\n\tstr %q2, [%9, #16]\n\tstr %q3, [%9, #32]\n\tstr %q4, [%9, #48]\n\t"
                    "str %q5, [%9, #64]\n\tstr %q6, [%9, #80]\n\tstr %q7, [%9, #96]\n\tstr %q8, [%9, #112]"
                    : "=m"(*(uint8_t (*)[128])c8)
                    : "w"(w0), "w"(w1), "w"(w2), "w"(w3), "w"(w4), "w"(w5), "w"(w6), "w"(w7), "r"(c8));
            continue;
        }
        const uint8x16x4_t cx = { { vreinterpretq_u8_u32(c0), vreinterpretq_u8_u32(c1), vreinterpretq_u8_u32(c2), vreinterpretq_u8_u32(c3) } };
        const uint8x16x4_t cy = { { vreinterpretq_u8_u32(c4), vreinterpretq_u8_u32(c5), vreinterpretq_u8_u32(c6), vreinterpretq_u8_u32(c7) } };
        const uint8x16_t aidx = PIDX(16);
        uint8x16_t tx = vqtbl4q_u8(cx, aidx), ty = vqtbl4q_u8(cy, aidx);              /* the alpha planes */
        if (__builtin_expect(vmaxvq_u8(vorrq_u8(tx, ty)) < 0x80, 0)) continue;     /* no fog flag in the step */
        uint16x8_t ux0 = FOG_D(a0, a1), ux1 = FOG_D(a2, a3), uy0 = FOG_D(a4, a5), uy1 = FOG_D(a6, a7);
        int8x16_t kx = FOG_K(tx, ux0, ux1), ky = FOG_K(ty, uy0, uy1);
        uint8x16x4_t px, py;
        px.val[3] = fog_ch16(vshrq_n_u8(vshlq_n_u8(tx, 1), 1), FOGB(3), kx);       /* (the flag cleared: no mask held) */
        py.val[3] = fog_ch16(vshrq_n_u8(vshlq_n_u8(ty, 1), 1), FOGB(3), ky);
        const uint8x16_t ridx = vsubq_u8(didx, vdupq_n_u8(2)), gidx = vsubq_u8(didx, vdupq_n_u8(1));
        px.val[0] = vqtbl4q_u8(cx, ridx); px.val[1] = vqtbl4q_u8(cx, gidx); px.val[2] = vqtbl4q_u8(cx, didx);
        py.val[0] = vqtbl4q_u8(cy, ridx); py.val[1] = vqtbl4q_u8(cy, gidx); py.val[2] = vqtbl4q_u8(cy, didx);
        if (full) {
            const uint8x16_t fr = FOGB(0), fg = FOGB(1), fb = FOGB(2);
            px.val[0] = fog_ch16(px.val[0], fr, kx), px.val[1] = fog_ch16(px.val[1], fg, kx), px.val[2] = fog_ch16(px.val[2], fb, kx);
            py.val[0] = fog_ch16(py.val[0], fr, ky), py.val[1] = fog_ch16(py.val[1], fg, ky), py.val[2] = fog_ch16(py.val[2], fb, ky);
        }
        vst4q_u8(c8, px); vst4q_u8(c8 + 64, py);
    }
#undef PIDX
#undef FOGB
#undef FOG_D
#undef FOG_K
}
/* fog of a colour line in place with the weights of the attribute line attr (full: r, g, b and alpha; else alpha);
 * zb from fog_zero_b2() */
static __attribute__((noinline)) void fog_line_zb(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc,
                                                  int zb, int full) {
    if (full) fog_line16(c, attr, table, params, fogc, zb, 0, 0, 1);
    else fog_line16(c, attr, table, params, fogc, zb, 0, 0, 0);
}
/* the same, writing the attribute line's top-byte plane tp as it goes (the edge marking's id screen; a function of
 * its own: in one function clang merges the two calls and tests tp in the loop) */
static __attribute__((noinline)) void fog_line_zbp(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc,
                                                   int zb, uint8_t *tp, int full) {
    if (full) fog_line16(c, attr, table, params, fogc, zb, tp, 1, 1);
    else fog_line16(c, attr, table, params, fogc, zb, tp, 1, 0);
}
/* the same with the bound derived here and no planes (ut/hr_ab's fog test; the resolve derives it once per bin) */
static void fog_line(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full) {
    fog_line_zb(c, attr, table, params, fogc, fog_zero_b2(table, params), full);
}
/* edge marking (spec/resolve.c's edge_identify + edge_mark): a pixel C is an edge against a neighbour N (left, right,
 * above, below; the clear attribute beyond the line's ends) when N's 24-bit key is larger and the polygon ids differ
 * (c3 = ((C >> 24) & 0x7f) ^ 0x40 against n3 = (N >> 24) & 0x3f, never equal when C's bit 30 is clear); the edge byte
 * is c3 >> 3, or 0xff when no edge; pixels with an edge byte < 8 (an edge and bit 30 set) take the edge colour of
 * their id >> 3, all others keep r, g, b, and every alpha byte becomes ca & 0x1f (the edge byte's (e >> 3) & 0xe0 is
 * always 0). So per pixel:
 *   marked = bit 30 of C && some N with (N & 0xffffff) > (C & 0xffffff) and (N ^ C) & 0x3f000000,
 *   out = marked ? ec[(C >> 27) & 7] (r, g, b) | (px & 0x1f000000) : px & 0x1fffffff.
 * Bits 29-31 matter to nothing downstream (the downsample reads the alpha as & 0x1f), so the lines are marked in
 * place and keep them: the colours of a step (16 pixels of two lines) without a marked pixel are not loaded or stored
 * at all, which is most steps (S4 at 3x: edge_lines 3.03 -> 2.59 M modeled cycles a frame; 2.38 M with both lines'
 * tests before the one test for a marked pixel).
 * NEON over 16 pixels on byte planes: ld4 splits 16 attribute words into the key bytes k0, k1, k2 and the top byte t,
 * and 16 colours into r, g, b, a (st4 interleaves them back). N > C is lexicographic over the key planes,
 * k2 == ? (k1 == ? k0 > : k1 >) : k2 >, with cmeq/cmhi and two selects. The id test is folded in: (N > C) & (tN ^ tC)
 * is nonzero in bits 0-5 iff N is an edge for C (bit 30 aside), so the four are ORed and tested against 0x3f once.
 * Neighbours share their compares: from the same equalities, N > C and N >= C, whose complement is C > N, and the same
 * id xor give the test of C against N and the test of N against C. So the pair (x, x+1) gives the right test of x
 * and the left test of x+1 (the left results move one lane on, the lane before pixel 0 being pixel 0's test against
 * the clear attribute; the pixels to the right are an ld4 one pixel on), and two lines are marked at once: the pair
 * (line l, line l+1) gives the below test of l and the above test of l+1. The colour is a tbx on 16-entry tables
 * (entries 2i and 2i+1 = ec[7 - i]) indexed by ~(t >> 2) & 0x1f = 15 - (id >> 2) when bit 30 is set (16 or more when
 * clear) and by 31 where there is no edge: out-of-range indices keep the pixel's colour. */
static inline __attribute__((always_inline)) uint8x16_t edge_sel(uint8x16_t m, uint8x16_t t, uint8x16_t f) {
    /* m ? t : f as one bif (the bsl intrinsic becomes and/orr pairs when the compiler sees that the operands of the
     * compare chains are exclusive). f must not be an all-ones constant when testing under an unpatched qemu 9.2:
     * its TCG folds a bit select whose false operand is a known -1 to -1 (fold_bitsel_vec's orc case swaps the wrong
     * operands; tools/sim/setup.sh applies the fix to the simulator's qemu). */
    __asm__("bif %0.16b, %1.16b, %2.16b" : "+w"(t) : "w"(f), "w"(m));
    return t;
}
/* N > C on the key planes */
static inline __attribute__((always_inline)) uint8x16_t edge_gt(uint8x16x4_t n, uint8x16x4_t c) {
    return edge_sel(vceqq_u8(n.val[2], c.val[2]), edge_sel(vceqq_u8(n.val[1], c.val[1]), vcgtq_u8(n.val[0], c.val[0]),
                                                         vcgtq_u8(n.val[1], c.val[1])), vcgtq_u8(n.val[2], c.val[2]));
}
/* a pair of neighbours: *nc = the test of C against N ((N > C) & id xor), returns the test of N against C */
static inline __attribute__((always_inline)) uint8x16_t edge_pair(uint8x16x4_t n, uint8x16x4_t c, uint8x16_t *nc) {
    uint8x16_t e1 = vceqq_u8(n.val[1], c.val[1]), e2 = vceqq_u8(n.val[2], c.val[2]), g1 = vcgtq_u8(n.val[1], c.val[1]);
    uint8x16_t g2 = vcgtq_u8(n.val[2], c.val[2]), xid = veorq_u8(n.val[3], c.val[3]);
    uint8x16_t gt = edge_sel(e2, edge_sel(e1, vcgtq_u8(n.val[0], c.val[0]), g1), g2);        /* N > C */
    uint8x16_t ge = edge_sel(e2, edge_sel(e1, vcgeq_u8(n.val[0], c.val[0]), g1), g2);        /* N >= C */
    *nc = vandq_u8(gt, xid);
    return vbicq_u8(xid, ge);
}
/* the colours of 16 pixels in place, with the top bytes t and the tests' edge lanes e (cmtst of the tests against
 * 0x3f): the alpha bytes keep their flag bits 5-7 (the 2x path clears them, ca & 0x1f), which the downsample ignores */
static inline __attribute__((always_inline)) void edge_mark(uint32_t *col, uint8x16_t t, uint8x16_t e, uint8x16_t er, uint8x16_t eg,
                                                            uint8x16_t eb) {
    const uint8x16_t m1f = vdupq_n_u8(0x1f);
    /* ~(t >> 2) & 0x1f = 15 - (id >> 2) + (bit 30 clear ? 16 : 0) where there is an edge, 31 where not */
    uint8x16_t idx = edge_sel(e, vbicq_u8(m1f, vshrq_n_u8(t, 2)), m1f);
    uint8x16x4_t p = vld4q_u8((const uint8_t *)col);
    p.val[0] = vqtbx1q_u8(p.val[0], er, idx); p.val[1] = vqtbx1q_u8(p.val[1], eg, idx); p.val[2] = vqtbx1q_u8(p.val[2], eb, idx);
    vst4q_u8((uint8_t *)col, p);
}
/* 16 pixels of two lines (the last ones of the lines when last: the clear attribute to the right), lp0 and lp1 = the
 * previous block's left tests (lane 15 is the one for this block's pixel 0), replaced by this block's. Both lines'
 * tests first, then one test for a marked pixel (an edge and bit 30 set) in either line: the colours of a block
 * without one are not loaded or stored at all, and the two lines' compare chains share one basic block. */
static inline __attribute__((always_inline)) void edge_block(uint32_t *col0, uint32_t *col1, const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
                                                             int last, uint8x16x4_t k, uint8x16_t *lp0, uint8x16_t *lp1,
                                                             uint8x16_t er, uint8x16_t eg, uint8x16_t eb) {
    const uint8x16_t m3f = vdupq_n_u8(0x3f), m40 = vdupq_n_u8(0x40);
    uint8x16x4_t p0 = vld4q_u8((const uint8_t *)c0), p1 = vld4q_u8((const uint8_t *)c1), r, n;
    uint8x16_t y0, y1, v, lt;
    /* the right neighbours: the planes of an ld4 one pixel on */
    if (!last) r = vld4q_u8((const uint8_t *)(c0 + 1));
    else for (int i = 0; i < 4; i++) r.val[i] = vextq_u8(p0.val[i], k.val[i], 1);
    lt = edge_pair(r, p0, &y0);
    y0 = vorrq_u8(y0, vextq_u8(*lp0, lt, 15)); *lp0 = lt;
    n = vld4q_u8((const uint8_t *)a);
    y0 = vorrq_u8(y0, vandq_u8(edge_gt(n, p0), veorq_u8(n.val[3], p0.val[3])));
    y1 = edge_pair(p1, p0, &v);                                 /* line 1 against line 0 above it, and the reverse */
    y0 = vorrq_u8(y0, v);
    if (!last) r = vld4q_u8((const uint8_t *)(c1 + 1));
    else for (int i = 0; i < 4; i++) r.val[i] = vextq_u8(p1.val[i], k.val[i], 1);
    lt = edge_pair(r, p1, &v);
    y1 = vorrq_u8(vorrq_u8(y1, v), vextq_u8(*lp1, lt, 15)); *lp1 = lt;
    n = vld4q_u8((const uint8_t *)b);
    y1 = vorrq_u8(y1, vandq_u8(edge_gt(n, p1), veorq_u8(n.val[3], p1.val[3])));
    uint8x16_t e0 = vtstq_u8(y0, m3f), e1 = vtstq_u8(y1, m3f);
    uint8x16_t m = vorrq_u8(vandq_u8(e0, p0.val[3]), vandq_u8(e1, p1.val[3]));   /* bit 6: a marked pixel */
    if (__builtin_expect(!vmaxvq_u8(vandq_u8(m, m40)), 1)) return;
    edge_mark(col0, p0.val[3], e0, er, eg, eb);
    edge_mark(col1, p1.val[3], e1, er, eg, eb);
}
/* the top bytes of n attribute lines (the polygon ids and flags) as byte planes: the edge marking's id screen reads
 * each line's once instead of twice (a line is c0 or c1 of one pair and a or b of another) and its right neighbours
 * with one unaligned load. 64 pixels a step: 16 ldr q at immediate offsets from one pointer (asm: the compiler makes
 * ldp, which holds the A55's load pipe longer in the model, or post-incremented loads, which hold it and write the
 * base back in order, of them otherwise) and a tbl of each four words' byte 3 (a cycle, latency 4, whatever the
 * table's size in llvm-mca's A55; two levels of uzp2 were 6 four-cycle operations): 13 modeled cycles a step of 32
 * pixels against 22. */
static __attribute__((noinline)) void edge_tplanes(uint8_t *tp, const uint32_t *att, unsigned n) {
    static const uint8_t top_idx[16] = { 3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63 };
    const uint8x16_t tidx = vld1q_u8(top_idx);
    const uint8_t *p = (const uint8_t *)att, *end = p + (size_t)n * GX_LSTRIDE;
    _Static_assert(GX_W % 64 == 0, "edge_tplanes: 64 pixels a step");
    for (; p != end; p += 256, tp += 64) {
        uint8x16_t q0, q1, q2, q3, q4, q5, q6, q7, q8, q9, q10, q11, q12, q13, q14, q15;
        __asm__("ldr %q0, [%16]\n\tldr %q1, [%16, #16]\n\tldr %q2, [%16, #32]\n\tldr %q3, [%16, #48]\n\t"
                "ldr %q4, [%16, #64]\n\tldr %q5, [%16, #80]\n\tldr %q6, [%16, #96]\n\tldr %q7, [%16, #112]\n\t"
                "ldr %q8, [%16, #128]\n\tldr %q9, [%16, #144]\n\tldr %q10, [%16, #160]\n\tldr %q11, [%16, #176]\n\t"
                "ldr %q12, [%16, #192]\n\tldr %q13, [%16, #208]\n\tldr %q14, [%16, #224]\n\tldr %q15, [%16, #240]"
                : "=&w"(q0), "=&w"(q1), "=&w"(q2), "=&w"(q3), "=&w"(q4), "=&w"(q5), "=&w"(q6), "=&w"(q7),
                  "=&w"(q8), "=&w"(q9), "=&w"(q10), "=&w"(q11), "=&w"(q12), "=&w"(q13), "=&w"(q14), "=&w"(q15)
                : "r"(p), "m"(*(const uint8_t (*)[256])p));
        const uint8x16x4_t w0 = { { q0, q1, q2, q3 } }, w1 = { { q4, q5, q6, q7 } }, w2 = { { q8, q9, q10, q11 } }, w3 = { { q12, q13, q14, q15 } };
        vst1q_u8(tp, vqtbl4q_u8(w0, tidx)); vst1q_u8(tp + 16, vqtbl4q_u8(w1, tidx));
        vst1q_u8(tp + 32, vqtbl4q_u8(w2, tidx)); vst1q_u8(tp + 48, vqtbl4q_u8(w3, tidx));
    }
}
/* two lines of the bin, marked in place: col0/c0 and col1/c1 (the line below), a = the line above the first, b = the
 * line below the second; tp = the top-byte planes of a, c0, c1 and b (consecutive lines of GX_W bytes) */
static __attribute__((noinline)) void edge_lines(uint32_t *col0, uint32_t *col1, const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
                                                 const uint8_t *tp, uint32_t clear, const uint8_t *ec) {
    uint8x8_t ecr = vrev64_u8(vld1_u8(ec)), ecg = vrev64_u8(vld1_u8(ec + 8)), ecb = vrev64_u8(vld1_u8(ec + 16));
    uint8x16_t er = vcombine_u8(ecr, ecr), eg = vcombine_u8(ecg, ecg), eb = vcombine_u8(ecb, ecb);
    er = vzip1q_u8(er, er); eg = vzip1q_u8(eg, eg); eb = vzip1q_u8(eb, eb);       /* entries 2i, 2i+1 = ec[7 - i] */
    /* the left tests of the lines' pixel 0 against the clear attribute, in lane 15 */
    uint8x16_t lp0 = vsetq_lane_u8((clear & 0xffffff) > (c0[0] & 0xffffff) ? (uint8_t)((clear ^ c0[0]) >> 24) : 0, vdupq_n_u8(0), 15);
    uint8x16_t lp1 = vsetq_lane_u8((clear & 0xffffff) > (c1[0] & 0xffffff) ? (uint8_t)((clear ^ c1[0]) >> 24) : 0, vdupq_n_u8(0), 15);
    const uint8x16x4_t k = { { vdupq_n_u8((uint8_t)clear), vdupq_n_u8((uint8_t)(clear >> 8)), vdupq_n_u8((uint8_t)(clear >> 16)),
                               vdupq_n_u8((uint8_t)(clear >> 24)) } };
    const uint8x16_t m3f = vdupq_n_u8(0x3f);
    /* the id screen: a block whose neighbour pairs all have the same polygon id (bits 24-29) has no edge (an edge needs
     * (N ^ C) & 0x3f000000), so it is left as it is. The pairs: line 0 with the line above and with line 1, line 1 with
     * the line below, and each pixel of both lines with the one to its right; the left pair of pixel 0 is the previous
     * block's right pair of its pixel 15: a screened block's left tests lp are 0 (no id differs), and a block after a
     * full one is full when that one's lane 15 can be an edge (force; also the lines' pixel 0 against the clear
     * attribute). The ids come from tp, the four lines' top bytes as byte planes (edge_tplanes, once per line for the
     * bin; the right neighbours an unaligned load one byte on): 6 loads and 11 operations a block (~25 modeled cycles)
     * instead of edge_block's ~160, and in the stress ROM's edge-marking scene 96% of the blocks pass. While no block
     * is forced two blocks are screened at once (one max and branch: a step of 32 pixels ~55 modeled cycles instead of
     * ~68), and a pair that does not pass is taken a block at a time. The last block's right neighbours are the clear
     * attribute, its top byte in the screen's place. */
    const uint8_t *ta = tp, *t0p = tp + GX_W, *t1p = tp + 2 * GX_W, *tbp = tp + 3 * GX_W;
    int force = ((vgetq_lane_u8(lp0, 15) | vgetq_lane_u8(lp1, 15)) & 0x3f) != 0;
    int x = 0;
#define SCREEN(x, r0, r1) ({ \
        uint8x16_t t0 = vld1q_u8(t0p + (x)), t1 = vld1q_u8(t1p + (x)); \
        vorrq_u8(vorrq_u8(veorq_u8(vld1q_u8(ta + (x)), t0), veorq_u8(t0, t1)), \
                 vorrq_u8(veorq_u8(t1, vld1q_u8(tbp + (x))), vorrq_u8(veorq_u8(t0, r0), veorq_u8(t1, r1)))); \
    })
    while (x < GX_W - 16) {
        if (__builtin_expect(!force, 1) && x < GX_W - 32 &&
            !vmaxvq_u8(vandq_u8(vorrq_u8(SCREEN(x, vld1q_u8(t0p + x + 1), vld1q_u8(t1p + x + 1)),
                                         SCREEN(x + 16, vld1q_u8(t0p + x + 17), vld1q_u8(t1p + x + 17))), m3f))) {
            lp0 = lp1 = vdupq_n_u8(0); x += 32; continue;            /* two blocks without an id edge */
        }
        uint8x16_t d = SCREEN(x, vld1q_u8(t0p + x + 1), vld1q_u8(t1p + x + 1));
        if (__builtin_expect(!force, 1) && !vmaxvq_u8(vandq_u8(d, m3f))) { lp0 = lp1 = vdupq_n_u8(0); x += 16; continue; }
        edge_block(col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 0, k, &lp0, &lp1, er, eg, eb);
        force = ((vgetq_lane_u8(lp0, 15) | vgetq_lane_u8(lp1, 15)) & 0x3f) != 0;
        x += 16;
    }
    /* the last block: its right neighbours are the clear attribute */
    uint8x16_t d = SCREEN(x, vextq_u8(vld1q_u8(t0p + x), k.val[3], 1), vextq_u8(vld1q_u8(t1p + x), k.val[3], 1));
    if (__builtin_expect(!force, 1) && !vmaxvq_u8(vandq_u8(d, m3f))) return;
    edge_block(col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 1, k, &lp0, &lp1, er, eg, eb);
#undef SCREEN
}
