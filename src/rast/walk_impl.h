/* walk_impl.h: the fused polygon walker's routines (walk.c's, see there), as a template for both layouts:
 *   WALK_ARR    bytes per span array (DraStic's 2x: SPAN_ARR, 44 entries; hr.c's 3x: 0x100, 64 entries)
 *   WALK_XMAX   setup_spans' x clamp (0x200; 3x: 768)
 *   WALK_RECIP  the edge heights' reciprocal table, a u32[1024] expression (DraStic's; 3x: hr.c's hr_recip)
 *   WALK_CAP    1: a chain's lines stop at its window (edges_impl.h's EDGES_LINE_CAP, the 3x walker)
 *   WALK_HR     1: the edge-marking x fix-up's rule of hr.c (+ 1 unless bit 15 or x >= WALK_XMAX; 2x: bits 15, 9)
 * walk_chain() and walk_spans() / walk_markers() write what edges_impl.h's interpolate_edges, setup_spans and
 * setup_edge_markers write with the same constants, on the lines that render_polygon_setup_4x / f_run and the
 * markers read (tools/rast/ut/t_walk.c at 2x against DraStic's walker; tools/rast/ut/hr_ab at 3x against hr.c's
 * former per-routine walker, edges_impl.h's NEON form). */
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>

static inline uint16_t wk_rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t wk_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wk_wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
#define WK_EA(k) ((k) * WALK_ARR)

#pragma STDC FP_CONTRACT OFF
/* ---- one chain of render_polygon_interpolate_edges, its five routines fused per edge ----
 * walk_chain(spans, vptr, dir, y_start, y_end, flags) writes what constprop_0 (dir +1) / constprop_1 (dir -1) write
 * into the span arrays for the walked lines 0..total-1 (spec/edges.c section 4), but per edge in one pass over its
 * lines, 8 at a time, in registers: no float pairs and Q15 weights through the scratch (span + 0x6e0, which then
 * holds nothing: only the edge markers use it, and they write it afterwards), no per-routine reloads of the edge's
 * vertices. Every value is a pure function of its line's lane, so the lanes give DraStic's values exactly:
 *   coefficients: lane m of 4 holds (num, den) of lines 4h + m, accumulated with d4 one group at a time as DraStic's
 *     four lanes do (each fadd rounded; the second group of an iteration is the first + d4, as DraStic's next one);
 *   steps: frecpe and two frecps steps, num * r to Q15 (fcvtzs #15), the low 16 bits;
 *   w: (s64)(wb - wa) * q >> 15, + wa (32-bit; sqdmulh of q << 16 where it cannot saturate, walk_edge says how);
 *   s, t: bits 15..30 of base + q * ds (smlal, shrn #15, or sqdmull and addhn) with base = s_a << 15 (+ 0x800 when
 *     ds > 0);
 *   r, g, b: base (c6(a) << 18) + 0x38000 has no bits below 15 and |q * dc| < 2^24, so bits 15..30 of the sum are
 *     (c6(a) << 3) + 7 + floor(q * dc / 2^15): sqdmulh(q, dc) (2 q dc >> 16, no saturation as |dc| <= 504) + that;
 *   x, z: the DDA x + i xs (u32), z + i zs (u64), as interp_x.
 * Beyond each edge's lines up to 7 more are written (the next edge's overwrite them; after the last, garbage in lines
 * total..total+6, below the 44 entries for a bin's at most 34 lines, at 3x the 64 for 50: setup_spans runs over them
 * as over DraStic's overrun, nothing reads them). x and z there are not DraStic's (its x/z routines write exactly cnt
 * lines): only the walked lines are DraStic's bytes, which is what render_polygon_setup_4x and the edge markers read
 * (tools/rast/ut/t_walk.c compares those; at 3x tools/rast/ut/hr_ab what f_run receives). Nor is the scratch (span
 * array 10) written: with edge marking off it keeps what it held. */
/* the edge heights' reciprocals (WALK_RECIP: DraStic's reciprocal_table, which initialize_video_3d fills, or hr.c's), 0
 * past it as edges_impl.h */
static inline uint32_t wrecip(int32_t i) {
    const int in = (uint32_t)i < 1024;
    uint32_t v = (WALK_RECIP)[in ? i : 0];
    return in ? v : 0;
}

static inline int32x4_t w_step(float32x4_t num, float32x4_t den) {
    float32x4_t r = vrecpeq_f32(den);
    r = vmulq_f32(r, vrecpsq_f32(r, den));
    r = vmulq_f32(r, vrecpsq_f32(r, den));
    return vcvtq_n_s32_f32(vmulq_f32(num, r), 15);
}

/* one edge: lines line.. of the chain's arrays, cnt of them (8 a step), skip = the lines above y_start (edge 0's; 0 for
 * the others, which then add nothing: 0 * A to a zero numerator changes at most its sign, which no result sees).
 * For the A55's in-order issue the first 8 lines' weights (a chain of reciprocal steps: the latency) are computed
 * among the edge's setup, one basic block with the colour, x and z setup (the reciprocal table's bound is a select,
 * not a branch, and walk_polygon_4x inlines walk_chain once per depth source, so that clang does not merge the two
 * setups up to a branch on it; the vertices' z, c, s, t halfwords come as vectors, the colour channels of both in
 * one). A step that continues tests first and computes the next 8 lines' weights among its stores (an edge's last
 * step computes none); that fits in registers with z as one vector of 2 lines and a step and x as one of 4 and a
 * step (clang spilled 13 vectors a step with z as 4 vectors and x as 2), and with w's and s, t's products in fewer
 * instructions (sqdmulh, sqdmull + addhn: the fast forms below). Modeled A55 cycles of a z edge: its setup and first
 * step 188 -> 160, a continuing step 90 -> 68. The edge's five multipliers share a register (by-lane products), the
 * rest are constants of the loop */
static inline __attribute__((always_inline)) void walk_edge(uint8_t *spans, const uint8_t *a, const uint8_t *b, unsigned line,
                                                            int32_t cnt, uint32_t skip, const int withz) {
    /* coefficients: (num, den) of line j = (j + skip) (A, D) + (0 B, B H), as DraStic's lanes accumulate them */
    int32_t wa = (int32_t)wk_rd32(a), wb = (int32_t)wk_rd32(b);
    int32_t h = (int32_t)((uint32_t)wk_rd16(b + 6) - wk_rd16(a + 6));
    float A = (float)wa, D = (float)(int32_t)((uint32_t)wa - (uint32_t)wb), B = (float)wb, H = (float)h, k = (float)skip;
    float n0 = 0.0f * B, d0 = B * H;
    n0 = n0 + k * A; d0 = d0 + k * D;
    /* lanes {n0, n0 + A, n0 + (A + A), n0 + (A + (A + A))} as n0 + {0, 1, 2, 3} A, one product and one sum a vector,
     * the same bits: j A is exact for j = 0..2, A + 2A rounds the same 3A as 3 A, and n0 + 0 A is n0, its zero's sign
     * too (n0 = 0 B + k A is -0 only when k A is, so A < 0 and 0 A is -0; d0 is never -0: B H is +0 or not 0, and a
     * sum to 0 is +0); 4 A is (A + A) + (A + A) */
    static const float k0123[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
    const float32x4_t kk = vld1q_f32(k0123);
    float32x4_t na = vaddq_f32(vdupq_n_f32(n0), vmulq_n_f32(kk, A)), da = vaddq_f32(vdupq_n_f32(d0), vmulq_n_f32(kk, D));
    const float32x4_t n4 = vdupq_n_f32(4.0f * A), d4 = vdupq_n_f32(4.0f * D);
#define WEIGHTS(q) do { \
        float32x4_t nb_ = vaddq_f32(na, n4), db_ = vaddq_f32(da, d4); \
        q = vcombine_s16(vmovn_s32(w_step(na, da)), vmovn_s32(w_step(nb_, db_))); \
        na = vaddq_f32(nb_, n4); da = vaddq_f32(db_, d4); \
    } while (0)
    int16x8_t q, qn;
    WEIGHTS(q);
    /* w */
    const int32_t dw0 = (int32_t)((uint32_t)wb - (uint32_t)wa);
    const int32x4_t dw = vdupq_n_s32(dw0);
    const uint32x4_t w0 = vdupq_n_u32((uint32_t)wa);
    /* s, t, r, g, b: the vertices' halfwords z, c, s, t (+ 8) as one vector each; the colour channels of both
     * vertices in one vector (c of a in lanes 0-2, of b in 4-6, shifted down by 0, 5, 10), wc6(v) = 2 v + (v != 0)
     * as (v << 1) - (v != 0 ? -1 : 0); dm = b - a for z, c (unused), s, t, then the colours' (wc6 << 3) deltas */
    static const uint8_t cidx[16] = { 2, 3, 2, 3, 2, 3, 255, 255, 10, 11, 10, 11, 10, 11, 255, 255 };
    static const int16_t csh[8] = { 0, -5, -10, 0, 0, -5, -10, 0 };
    const uint16x4_t ha = vld1_u16((const uint16_t *)(a + 8)), hb = vld1_u16((const uint16_t *)(b + 8));
    uint16x8_t cc = vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_u16(vcombine_u16(ha, hb)), vld1q_u8(cidx)));
    cc = vandq_u16(vshlq_u16(cc, vld1q_s16(csh)), vdupq_n_u16(31));
    const uint16x8_t c6 = vshlq_n_u16(vsubq_u16(vshlq_n_u16(cc, 1), vtstq_u16(cc, cc)), 3);
    const int16x4_t cbase = vreinterpret_s16_u16(vadd_u16(vget_low_u16(c6), vdup_n_u16(7)));
    const int16x8_t vr = vdupq_lane_s16(cbase, 0), vg = vdupq_lane_s16(cbase, 1), vb = vdupq_lane_s16(cbase, 2);
    const int16x8_t dm = vreinterpretq_s16_u16(vcombine_u16(vsub_u16(hb, ha), vsub_u16(vget_high_u16(c6), vget_low_u16(c6))));
    int16_t sa = (int16_t)wk_rd16(a + 0xc), ta = (int16_t)wk_rd16(a + 0xe);
    int16_t ds = (int16_t)(uint16_t)(wk_rd16(b + 0xc) - (uint16_t)sa), dt = (int16_t)(uint16_t)(wk_rd16(b + 0xe) - (uint16_t)ta);
    const int32x4_t vs = vdupq_n_s32((int32_t)((uint32_t)((int32_t)sa * 32768) + (ds > 0 ? 0x800 : 0)));
    const int32x4_t vt = vdupq_n_s32((int32_t)((uint32_t)((int32_t)ta * 32768) + (dt > 0 ? 0x800 : 0)));
    /* x, z */
    uint32_t xa = wk_rd16(a + 4);
    int32_t dx = (int32_t)(wk_rd16(b + 4) - xa);
    uint64_t rc = wrecip(h), prod = (uint64_t)(int64_t)dx * rc;
    uint32_t xs = (uint32_t)((dx < 0 ? prod + 0xfff : prod) >> 12), x = (xa << 18) + xs * skip;
    const uint16x8_t vf = vdupq_n_u16(dx == 0 ? 0x8000 : 0);
    static const uint32_t iota[4] = { 0, 1, 2, 3 };
    uint32x4_t xl = vmlaq_n_u32(vdupq_n_u32(x), vld1q_u32(iota), xs);
    const uint32x4_t x4 = vdupq_n_u32(4 * xs);
    uint64x2_t z0 = vdupq_n_u64(0), z2s = z0;
    if (withz) {
        uint32_t za = wk_rd16(a + 8);
        int32_t dz = (int32_t)((wk_rd16(b + 8) - za) << 9);
        uint64_t zs = (uint64_t)(int64_t)dz * rc + (dz < 0 ? 0x40000000u : 0), z = ((uint64_t)za << 39) + (uint64_t)skip * zs;
        z2s = vdupq_n_u64(2 * zs);
        z0 = vaddq_u64(vdupq_n_u64(z), vcombine_u64(vdup_n_u64(0), vdup_n_u64(zs)));
    }

    /* the products' fast forms (F = 1), exact unless a saturating case can occur, which the edge's deltas tell:
     *   w: (s64)dw * q >> 15 is sqdmulh(q << 16, dw) = (2 (q 2^16) dw) >> 32, saturating only for q << 16 = dw =
     *     INT32_MIN (q = -32768: a weight of 1.0 wrapped; DraStic's 2^31 there, sqdmulh's 2^31 - 1);
     *   s, t: bits 15..30 of base + q d are bits 16..31 of 2 (base + q d) mod 2^32: addhn of sqdmull(q, d) (2 q d)
     *     and (s_a << 16) + 0x1000 (when d > 0), sqdmull saturating only for q = d = -32768.
     * Edges with dw = INT32_MIN or ds or dt = -32768 take the plain products (F = 0) */
    int fast = (dw0 != INT32_MIN) & (ds != -32768) & (dt != -32768);
    __asm__("" : "+r"(fast));          /* one branch on the three tests (clang branched on each) */
    const int32x4_t vs2 = vdupq_n_s32((int32_t)(((uint32_t)(uint16_t)sa << 16) + (ds > 0 ? 0x1000 : 0)));
    const int32x4_t vt2 = vdupq_n_s32((int32_t)(((uint32_t)(uint16_t)ta << 16) + (dt > 0 ? 0x1000 : 0)));
    uint8_t *o = spans + 4 * line;
#define STORES(F) do { \
        int16x4_t q0 = vget_low_s16(q); \
        int32x4_t wl, wh; \
        int16x8_t s, t; \
        if (F) { \
            wl = vqdmulhq_s32(vreinterpretq_s32_u32(vshll_n_u16(vreinterpret_u16_s16(q0), 16)), dw); \
            wh = vqdmulhq_s32(vreinterpretq_s32_u32(vshll_high_n_u16(vreinterpretq_u16_s16(q), 16)), dw); \
            s = vaddhn_high_s32(vaddhn_s32(vqdmull_laneq_s16(q0, dm, 2), vs2), vqdmull_high_laneq_s16(q, dm, 2), vs2); \
            t = vaddhn_high_s32(vaddhn_s32(vqdmull_laneq_s16(q0, dm, 3), vt2), vqdmull_high_laneq_s16(q, dm, 3), vt2); \
        } else { \
            int32x4_t ql = vmovl_s16(q0), qh = vmovl_high_s16(q); \
            wl = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(vget_low_s32(dw), vget_low_s32(ql)), 15), vmull_high_s32(dw, ql), 15); \
            wh = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(vget_low_s32(dw), vget_low_s32(qh)), 15), vmull_high_s32(dw, qh), 15); \
            s = vshrn_high_n_s32(vshrn_n_s32(vmlal_laneq_s16(vs, q0, dm, 2), 15), vmlal_high_laneq_s16(vs, q, dm, 2), 15); \
            t = vshrn_high_n_s32(vshrn_n_s32(vmlal_laneq_s16(vt, q0, dm, 3), 15), vmlal_high_laneq_s16(vt, q, dm, 3), 15); \
        } \
        vst1q_u32((uint32_t *)o, vaddq_u32(vreinterpretq_u32_s32(wl), w0)); \
        vst1q_u32((uint32_t *)(o + 16), vaddq_u32(vreinterpretq_u32_s32(wh), w0)); \
        int16x8x2_t st = { { s, t } }; \
        vst2q_s16((int16_t *)(o + WK_EA(4)), st); \
        int16x8x2_t rg = { { vaddq_s16(vr, vqdmulhq_laneq_s16(q, dm, 4)), vaddq_s16(vg, vqdmulhq_laneq_s16(q, dm, 5)) } }; \
        vst2q_s16((int16_t *)(o + WK_EA(6)), rg); \
        uint32x4_t xh = vaddq_u32(xl, x4); \
        uint16x8_t xv = vorrq_u16(vf, vshrq_n_u16(vshrn_high_n_u32(vshrn_n_u32(xl, 16), xh, 16), 2)); \
        uint16x8x2_t xb = { { xv, vreinterpretq_u16_s16(vaddq_s16(vb, vqdmulhq_laneq_s16(q, dm, 6))) } }; \
        vst2q_u16((uint16_t *)(o + WK_EA(8)), xb); \
        xl = vaddq_u32(xh, x4); \
        if (withz) { \
            uint64x2_t z1 = vaddq_u64(z0, z2s), z2 = vaddq_u64(z1, z2s), z3 = vaddq_u64(z2, z2s); \
            vst1q_u32((uint32_t *)(o + WK_EA(2)), vshrn_high_n_u64(vshrn_n_u64(z0, 30), z1, 30)); \
            vst1q_u32((uint32_t *)(o + WK_EA(2) + 16), vshrn_high_n_u64(vshrn_n_u64(z2, 30), z3, 30)); \
            z0 = vaddq_u64(z3, z2s); \
        } \
    } while (0)
    /* the exit test first: a step that continues computes the next 8 lines' weights in its stores' block, where
     * their reciprocal chain overlaps the stores; the last step only stores */
#define LOOP(F) do { \
        while (cnt > 8) { WEIGHTS(qn); STORES(F); q = qn; cnt -= 8; o += 32; } \
        STORES(F); \
        if (F) __asm__ volatile(""); /* (keeps the last step one block: clang tail-merged it with F = 0's) */ \
    } while (0)
    if (__builtin_expect(fast, 1)) LOOP(1);
    else LOOP(0);
#undef LOOP
#undef STORES
#undef WEIGHTS
}

/* the chain walk of render_polygon_interpolate_edges (spec/edges.c section 4): the edges with lines in
 * [y_start, y_end), each walked as it is found (DraStic collects up to 16, then runs each routine over all); withz:
 * the polygon's depth is z (flags & 0x18 clear), whose arrays the walk then writes too. WALK_CAP (the hi-res walker):
 * at most y_end - y_start lines a chain, as edges_impl.h's EDGES_LINE_CAP (the chain stops at its window) */
static inline __attribute__((always_inline)) void walk_chain(uint8_t *spans, vtx_t **vptr, int dir, uint32_t y_start,
                                                             uint32_t y_end, const int withz) {
    vtx_t *prev = vptr[0];
    uint32_t yp = wk_rd16(prev + 6), n = 0, line = 0;
    vtx_t **q = vptr + dir;
#if WALK_CAP
    const int32_t cap = (int32_t)y_end - (int32_t)y_start;
#endif
    while (y_end > yp) {
        vtx_t *cur = *q;
        uint32_t y1 = wk_rd16(cur + 6);
        int32_t len = (int32_t)(y1 - yp);
        uint32_t sk = 0;
        if (y_start > yp) { len += (int32_t)(yp - y_start); sk = y_start - yp; }
        if (y1 > y_end) len += (int32_t)(y_end - y1);
#if WALK_CAP
        if (len > cap - (int32_t)line) { len = cap - (int32_t)line; if (len <= 0) break; }
#endif
        if (len > 0) {
            if (n == 16) break;
            walk_edge(spans, prev, cur, line, (uint8_t)len, n ? 0 : sk, withz);
            line += (uint8_t)len;
            n++;
        }
        yp = y1;
        prev = cur;
        q += dir;
    }
}

/* render_polygon_setup_spans_4x (spec/edges.c section 4, edges_impl.h's NEON form) over the lines walk_polygon_4x
 * uses: 4 lines a step instead of DraStic's 8 (the lines past them are its overrun, which nothing reads). With adjust
 * (edge marking) render_polygon_4x's fix-up comes first, on the loaded x halves: per line l = x_left & 0x7fff,
 * r = x_right & 0x7fff, the larger (r when equal) + 1 unless that x has bit 15 (vertical) or bit 9 (>= 512) set
 * (WALK_HR, hi-res: unless bit 15 or x >= WALK_XMAX, hr.c's rule).
 * The swap (left x >= right x) is the same on all 4 lines of most steps (the winding decides which chain is left; the
 * lines where the chains meet differ): such a step skips the per-line selects and, when no line swaps, the stores of
 * the 4 left arrays besides x, which keep their values (stress ROM L4: 58 -> 36 modeled A55 cycles a step) */
static inline __attribute__((always_inline)) void walk_spans(uint8_t *s, int32_t lines, const int adjust) {
    const uint32_t ARR = WALK_ARR;
    const uint32x4_t m15 = vdupq_n_u32(0x7fff), xmax = vdupq_n_u32(WALK_XMAX), hi = vdupq_n_u32(0xffff0000u);
    const uint32x4_t mvx = vdupq_n_u32(WALK_HR ? 0x8000 : 0x8200);
    uint8_t *p = s;
#define LD(k) vld1q_u32((const uint32_t *)(p + (k) * ARR))
#define ST(k, v) vst1q_u32((uint32_t *)(p + (k) * ARR), (v))
#define SUB16(a, b) vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(a), vreinterpretq_u16_u32(b)))
#define XCLAMP(v) vbslq_u32(hi, (v), vminq_u32(vandq_u32((v), m15), xmax))
    do {
        uint32x4_t xl = LD(8), xr = LD(9);
        uint32x4_t sl0 = LD(4), sr0 = LD(5), wl0 = LD(0), wr0 = LD(1), rl0 = LD(6), rr0 = LD(7), zl0 = LD(2), zr0 = LD(3);
        if (adjust) {
            uint32x4_t l = vandq_u32(xl, m15), r = vandq_u32(xr, m15), gt = vcgtq_u32(l, r);
            /* mask - 1 = + 1: l where l > r and neither bit, r where l <= r and neither bit (no carry into b) */
            if (WALK_HR) {      /* (l < xmax: compared before the + 1) */
                uint32x4_t il = vandq_u32(gt, vcltq_u32(l, xmax)), ir = vbicq_u32(vcltq_u32(r, xmax), gt);
                l = vsubq_u32(l, vbicq_u32(il, vtstq_u32(xl, mvx)));
                r = vsubq_u32(r, vbicq_u32(ir, vtstq_u32(xr, mvx)));
            } else {
                l = vsubq_u32(l, vbicq_u32(gt, vtstq_u32(xl, mvx)));
                r = vsubq_u32(r, vbicq_u32(vmvnq_u32(gt), vtstq_u32(xr, mvx)));
            }
            xl = vbslq_u32(hi, xl, l); xr = vbslq_u32(hi, xr, r);
        }
        uint32x4_t sw = vcgeq_u32(vandq_u32(xl, m15), vandq_u32(xr, m15));
        uint64_t sm = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(sw)), 0);
        if (sm == 0) {                      /* no line swaps: the left arrays but x keep their values */
            uint32x4_t L = XCLAMP(xl), R = XCLAMP(xr);
            ST(8, L); ST(9, SUB16(R, L));
            ST(5, SUB16(sr0, sl0)); ST(1, vsubq_u32(wr0, wl0)); ST(7, SUB16(rr0, rl0)); ST(3, vsubq_u32(zr0, zl0));
        } else if (sm == ~(uint64_t)0) {    /* every line swaps */
            uint32x4_t L = XCLAMP(xr), R = XCLAMP(xl);
            ST(8, L); ST(9, SUB16(R, L));
            ST(4, sr0); ST(5, SUB16(sl0, sr0)); ST(0, wr0); ST(1, vsubq_u32(wl0, wr0));
            ST(6, rr0); ST(7, SUB16(rl0, rr0)); ST(2, zr0); ST(3, vsubq_u32(zl0, zr0));
        } else {
#define PAIR(L, R) uint32x4_t L = vbslq_u32(sw, R##0, L##0), R = vbslq_u32(sw, L##0, R##0)
            PAIR(sl, sr); PAIR(wl, wr); PAIR(rl, rr); PAIR(zl, zr);
#undef PAIR
            uint32x4_t L = XCLAMP(vbslq_u32(sw, xr, xl)), R = XCLAMP(vbslq_u32(sw, xl, xr));
            ST(8, L); ST(9, SUB16(R, L));
            ST(4, sl); ST(5, SUB16(sr, sl));
            ST(0, wl); ST(1, vsubq_u32(wr, wl));
            ST(6, rl); ST(7, SUB16(rr, rl));
            ST(2, zl); ST(3, vsubq_u32(zr, zl));
        }
        p += 16;
        lines -= 4;
    } while (lines > 0);
#undef LD
#undef ST
#undef SUB16
#undef XCLAMP
}

/* render_polygon_setup_edge_markers_c (spec/edges.c section 4) in NEON, 4 lines a step: line i's markers depend only
 * on x0 and the width of lines i - 1, i, i + 1, loaded as the words at -4, 0, +4 (their low halves). The first line's
 * previous neighbour without a line above is DraStic's (x0[0], x0[0] + wd[1]); with one, the line above. The steps
 * write up to 3 lines past the m DraStic computes: the last line's store (no line below) then overwrites line m,
 * and the lines beyond are below the polygon's (nothing reads them; the array has room) */
static void walk_markers(uint8_t *p, uint32_t lines, uint32_t clip) {
    const uint32_t ARR = WALK_ARR;
    const uint32x4_t lo = vdupq_n_u32(0xffff), one = vdupq_n_u32(1);
    int32_t m = (int32_t)(clip & 2 ? lines : lines - 1);
    for (int32_t i = 0; i < m; i += 4) {
        const uint8_t *x = p + 8 * ARR + 4 * i, *wd = p + 9 * ARR + 4 * i;
        int32x4_t cx = vreinterpretq_s32_u32(vandq_u32(vld1q_u32((const uint32_t *)x), lo));
        uint32x4_t w = vandq_u32(vld1q_u32((const uint32_t *)wd), lo);
        int32x4_t nx = vreinterpretq_s32_u32(vandq_u32(vld1q_u32((const uint32_t *)(x + 4)), lo));
        int32x4_t ne = vaddq_s32(nx, vreinterpretq_s32_u32(vandq_u32(vld1q_u32((const uint32_t *)(wd + 4)), lo)));
        int32x4_t px = vreinterpretq_s32_u32(vandq_u32(vld1q_u32((const uint32_t *)(x - 4)), lo));
        int32x4_t pe = vaddq_s32(px, vreinterpretq_s32_u32(vandq_u32(vld1q_u32((const uint32_t *)(wd - 4)), lo)));
        if (i == 0 && !(clip & 1)) {
            int32_t x0 = wk_rd16(p + 8 * ARR);
            px = vsetq_lane_s32(x0, px, 0);
            pe = vsetq_lane_s32(x0 + (int32_t)wk_rd16(p + 9 * ARR + 4), pe, 0);
        }
        int32x4_t ce = vaddq_s32(cx, vreinterpretq_s32_u32(w));
        int32x4_t lft = vmaxq_s32(vmaxq_s32(vaddq_s32(cx, vreinterpretq_s32_u32(one)), nx), px);
        int32x4_t rgt = vminq_s32(vminq_s32(vsubq_s32(ce, vreinterpretq_s32_u32(one)), ne), pe);
        uint32x4_t L = vminq_u32(w, vreinterpretq_u32_s32(vsubq_s32(lft, cx)));
        uint32x4_t R = vminq_u32(w, vreinterpretq_u32_s32(vsubq_s32(ce, rgt)));
        vst1q_u32((uint32_t *)(p + 10 * ARR + 4 * i), vsliq_n_u32(L, R, 16));
    }
    if (clip & 2) return;
    wk_wr32(p + 10 * ARR + 4 * m, (uint32_t)(uint16_t)(wk_rd16(p + 9 * ARR + 4 * m) + 1u));
}

#undef WK_EA
