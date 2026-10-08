/* walk.c: DraStic's polygon walker for the 2x bins (render_polygon_4x, spec/edges.c section 1) on NEON forms of its
 * edge routines: the five routines of each edge chain fused into one pass per edge (walk_chain), the span setup with
 * the edge-marking fix-up (walk_spans) and the edge markers (walk_markers), 4 to 8 lines a step. They write DraStic's
 * values for the lines render_polygon_setup_4x and the markers read; edges_impl.h's NEON forms (DraStic's bytes,
 * overruns included, tools/rast/ut/t_edges.c) are the per-routine reference.
 *
 * walk_polygon_4x() does what render_polygon_4x(ctx, poly, verts, bin_top, bin_bot) does for an ordinary polygon:
 * the vertex pointers from the walk-order table (nibble k of the polygon's entry, the ninth vertex the base one), the
 * clipping against the bin's lines, the two edge chains, with edge marking the extra neighbour lines and the
 * exclusive larger x, the span setup and the edge markers, and then render_polygon_setup_4x's work (the `setup`
 * argument: rast.c's hook). It leaves to DraStic (returns 0) the polygons whose handling differs or is not defined:
 * sprites (the axis-aligned quad path, attr bit 14), shadow polygons (mode 3: b0.c's stage-by-stage path, which wants
 * DraStic's scratch buffer), vertex counts 0 and 10-15 (DraStic fills nine vertex pointers and reads stale stack
 * slots beyond) and records whose bottom y lies below all their vertices (the walks run past the vertices). Not
 * done, as it changes nothing: render_polygon_4x clears the vertical-edge bit of the x values before the span setup
 * when edge marking is off; the span setup masks it itself.
 * Checked by tools/rast/ut/t_walk.c (the span arrays render_polygon_setup_4x gets, against DraStic's walker on random
 * polygons) and RAST=diff, which renders every bin with DraStic's renderer and ours (tools/rast/dev/regress.sh). */
#include <arm_neon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "spec/edges.h"

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

#pragma STDC FP_CONTRACT OFF
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)*(uint64_t *)((uint8_t *)(p) + (o)))
#define EA(k) ((k) * SPAN_ARR)

/* ---- one chain of render_polygon_interpolate_edges, its five routines fused per edge ----
 * walk_chain(spans, vptr, dir, y_start, y_end, flags) writes what constprop_0 (dir +1) / constprop_1 (dir -1) write
 * into the span arrays for the walked lines 0..total-1 (spec/edges.c section 4), but per edge in one pass over its
 * lines, 8 at a time, in registers: no float pairs and Q15 weights through the scratch (span + 0x6e0, which then
 * holds nothing: only the edge markers use it, and they write it afterwards), no per-routine reloads of the edge's
 * vertices. Every value is a pure function of its line's lane, so the lanes give DraStic's values exactly:
 *   coefficients: lane m of 4 holds (num, den) of lines 4h + m, accumulated with d4 one group at a time as DraStic's
 *     four lanes do (each fadd rounded; the second group of an iteration is the first + d4, as DraStic's next one);
 *   steps: frecpe and two frecps steps, num * r to Q15 (fcvtzs #15), the low 16 bits;
 *   w: (s64)(wb - wa) * q >> 15, + wa (32-bit);
 *   s, t: bits 15..30 of base + q * ds (smlal, shrn #15) with base = s_a << 15 (+ 0x800 when ds > 0);
 *   r, g, b: base (c6(a) << 18) + 0x38000 has no bits below 15 and |q * dc| < 2^24, so bits 15..30 of the sum are
 *     (c6(a) << 3) + 7 + floor(q * dc / 2^15): sqdmulh(q, dc) (2 q dc >> 16, no saturation as |dc| <= 504) + that;
 *   x, z: the DDA x + i xs (u32), z + i zs (u64), as interp_x.
 * Beyond each edge's lines up to 7 more are written (the next edge's overwrite them; after the last, garbage in lines
 * total..total+6, below the 44 entries for a bin's at most 34 lines: setup_spans runs over them as over DraStic's
 * overrun, nothing reads them). x and z there are not DraStic's (its x/z routines write exactly cnt lines): only the
 * walked lines are DraStic's bytes, which is what render_polygon_setup_4x and the edge markers read
 * (tools/rast/ut/t_walk.c compares those). */
/* the edge heights' reciprocals: DraStic's reciprocal_table (initialize_video_3d fills it), 0 past it as edges_impl.h */
static inline uint32_t wrecip(int32_t i) { return (uint32_t)i < 1024 ? ((const uint32_t *)(ds_base + DS_RECIP_TABLE))[i] : 0; }

static inline int32x4_t w_step(float32x4_t num, float32x4_t den) {
    float32x4_t r = vrecpeq_f32(den);
    r = vmulq_f32(r, vrecpsq_f32(r, den));
    r = vmulq_f32(r, vrecpsq_f32(r, den));
    return vcvtq_n_s32_f32(vmulq_f32(num, r), 15);
}

static inline uint16_t wc6(uint32_t v) { return (uint16_t)(2 * v + (v != 0)); }

/* one edge: lines line.. of the chain's arrays, cnt of them (8 a step), skip = the lines above y_start (edge 0's; 0 for
 * the others, which then add nothing: 0 * A to a zero numerator changes at most its sign, which no result sees).
 * For the A55's in-order issue the first 8 lines' weights (a chain of reciprocal steps: the latency) are computed
 * among the edge's setup, one basic block with their stores. A step's source computes the next 8's weights before
 * this 8's stores, but clang sinks them past the exit test, after the stores: forced before it (an empty asm on
 * them), a step took 81 modeled cycles instead of 93, but every edge paid for a chain its last step does not use
 * (2.3 steps an edge): +0.1 M a frame on the stress ROM. The edge's five multipliers share a register (by-lane
 * products), the rest are constants of the loop */
static inline __attribute__((always_inline)) void walk_edge(uint8_t *spans, const uint8_t *a, const uint8_t *b, unsigned line,
                                                            int32_t cnt, uint32_t skip, const int withz) {
    /* coefficients: (num, den) of line j = (j + skip) (A, D) + (0 B, B H), as DraStic's lanes accumulate them */
    int32_t wa = (int32_t)rd32(a), wb = (int32_t)rd32(b);
    int32_t h = (int32_t)((uint32_t)rd16(b + 6) - rd16(a + 6));
    float A = (float)wa, D = (float)(int32_t)((uint32_t)wa - (uint32_t)wb), B = (float)wb, H = (float)h, k = (float)skip;
    float n0 = 0.0f * B, d0 = B * H;
    n0 = n0 + k * A; d0 = d0 + k * D;
    float A2 = A + A, D2 = D + D, A3 = A + A2, D3 = D + D2;
    float32x4_t na = { n0, n0 + A, n0 + A2, n0 + A3 }, da = { d0, d0 + D, d0 + D2, d0 + D3 };
    const float32x4_t n4 = vdupq_n_f32(A2 + A2), d4 = vdupq_n_f32(D2 + D2);
#define WEIGHTS(q) do { \
        float32x4_t nb_ = vaddq_f32(na, n4), db_ = vaddq_f32(da, d4); \
        q = vcombine_s16(vmovn_s32(w_step(na, da)), vmovn_s32(w_step(nb_, db_))); \
        na = vaddq_f32(nb_, n4); da = vaddq_f32(db_, d4); \
    } while (0)
    int16x8_t q, qn;
    WEIGHTS(q);
    /* w */
    const int32x4_t dw = vdupq_n_s32((int32_t)((uint32_t)wb - (uint32_t)wa));
    const uint32x4_t w0 = vdupq_n_u32((uint32_t)wa);
    /* s, t, r, g, b */
    uint32_t ca = rd16(a + 0xa), cb = rd16(b + 0xa);
    uint16_t ra = wc6(ca & 31), ga = wc6((ca >> 5) & 31), ba = wc6((ca >> 10) & 31);
    int16_t dr = (int16_t)((wc6(cb & 31) - ra) << 3), dg = (int16_t)((wc6((cb >> 5) & 31) - ga) << 3);
    int16_t db = (int16_t)((wc6((cb >> 10) & 31) - ba) << 3);
    const int16x8_t vr = vdupq_n_s16((int16_t)((ra << 3) + 7)), vg = vdupq_n_s16((int16_t)((ga << 3) + 7));
    const int16x8_t vb = vdupq_n_s16((int16_t)((ba << 3) + 7));
    int16_t sa = (int16_t)rd16(a + 0xc), ta = (int16_t)rd16(a + 0xe);
    int16_t ds = (int16_t)(uint16_t)(rd16(b + 0xc) - (uint16_t)sa), dt = (int16_t)(uint16_t)(rd16(b + 0xe) - (uint16_t)ta);
    const int32x4_t vs = vdupq_n_s32((int32_t)((uint32_t)((int32_t)sa * 32768) + (ds > 0 ? 0x800 : 0)));
    const int32x4_t vt = vdupq_n_s32((int32_t)((uint32_t)((int32_t)ta * 32768) + (dt > 0 ? 0x800 : 0)));
    const int16x8_t dm = { ds, dt, dr, dg, db, 0, 0, 0 };
    /* x, z */
    uint32_t xa = rd16(a + 4);
    int32_t dx = (int32_t)(rd16(b + 4) - xa);
    uint64_t rc = wrecip(h), prod = (uint64_t)(int64_t)dx * rc;
    uint32_t xs = (uint32_t)((dx < 0 ? prod + 0xfff : prod) >> 12), x = (xa << 18) + xs * skip;
    const uint16x8_t vf = vdupq_n_u16(dx == 0 ? 0x8000 : 0);
    static const uint32_t iota[4] = { 0, 1, 2, 3 };
    uint32x4_t xl = vmlaq_n_u32(vdupq_n_u32(x), vld1q_u32(iota), xs), xh = vaddq_u32(xl, vdupq_n_u32(4 * xs));
    const uint32x4_t x8 = vdupq_n_u32(8 * xs);
    uint64x2_t z0 = vdupq_n_u64(0), z1 = z0, z2 = z0, z3 = z0, z8 = z0;
    if (withz) {
        uint32_t za = rd16(a + 8);
        int32_t dz = (int32_t)((rd16(b + 8) - za) << 9);
        uint64_t zs = (uint64_t)(int64_t)dz * rc + (dz < 0 ? 0x40000000u : 0), z = ((uint64_t)za << 39) + (uint64_t)skip * zs;
        const uint64x2_t z2s = vdupq_n_u64(2 * zs);
        z0 = vaddq_u64(vdupq_n_u64(z), vcombine_u64(vdup_n_u64(0), vdup_n_u64(zs)));
        z1 = vaddq_u64(z0, z2s); z2 = vaddq_u64(z1, z2s); z3 = vaddq_u64(z2, z2s);
        z8 = vdupq_n_u64(8 * zs);
    }

    uint8_t *o = spans + 4 * line;
    for (;;) {
        WEIGHTS(qn);
        int16x4_t q0 = vget_low_s16(q);
        int32x4_t ql = vmovl_s16(q0), qh = vmovl_high_s16(q);
        int32x4_t wl = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(vget_low_s32(dw), vget_low_s32(ql)), 15), vmull_high_s32(dw, ql), 15);
        int32x4_t wh = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(vget_low_s32(dw), vget_low_s32(qh)), 15), vmull_high_s32(dw, qh), 15);
        vst1q_u32((uint32_t *)o, vaddq_u32(vreinterpretq_u32_s32(wl), w0));
        vst1q_u32((uint32_t *)(o + 16), vaddq_u32(vreinterpretq_u32_s32(wh), w0));
        int16x8_t s = vshrn_high_n_s32(vshrn_n_s32(vmlal_laneq_s16(vs, q0, dm, 0), 15), vmlal_high_laneq_s16(vs, q, dm, 0), 15);
        int16x8_t t = vshrn_high_n_s32(vshrn_n_s32(vmlal_laneq_s16(vt, q0, dm, 1), 15), vmlal_high_laneq_s16(vt, q, dm, 1), 15);
        int16x8x2_t st = { { s, t } };
        vst2q_s16((int16_t *)(o + EA(4)), st);
        int16x8x2_t rg = { { vaddq_s16(vr, vqdmulhq_laneq_s16(q, dm, 2)), vaddq_s16(vg, vqdmulhq_laneq_s16(q, dm, 3)) } };
        vst2q_s16((int16_t *)(o + EA(6)), rg);
        uint16x8_t xv = vorrq_u16(vf, vshrq_n_u16(vshrn_high_n_u32(vshrn_n_u32(xl, 16), xh, 16), 2));
        uint16x8x2_t xb = { { xv, vreinterpretq_u16_s16(vaddq_s16(vb, vqdmulhq_laneq_s16(q, dm, 4))) } };
        vst2q_u16((uint16_t *)(o + EA(8)), xb);
        if (withz) {
            vst1q_u32((uint32_t *)(o + EA(2)), vshrn_high_n_u64(vshrn_n_u64(z0, 30), z1, 30));
            vst1q_u32((uint32_t *)(o + EA(2) + 16), vshrn_high_n_u64(vshrn_n_u64(z2, 30), z3, 30));
        }
        if ((cnt -= 8) <= 0) break;
        q = qn;
        xl = vaddq_u32(xl, x8); xh = vaddq_u32(xh, x8);
        if (withz) { z0 = vaddq_u64(z0, z8); z1 = vaddq_u64(z1, z8); z2 = vaddq_u64(z2, z8); z3 = vaddq_u64(z3, z8); }
        o += 32;
    }
#undef WEIGHTS
}

/* the chain walk of render_polygon_interpolate_edges (spec/edges.c section 4): the edges with lines in
 * [y_start, y_end), each walked as it is found (DraStic collects up to 16, then runs each routine over all) */
static inline __attribute__((always_inline)) void walk_chain(uint8_t *spans, vtx_t **vptr, int dir, uint32_t y_start,
                                                             uint32_t y_end, uint32_t flags) {
    const int withz = !(flags & 0x18);
    vtx_t *prev = vptr[0];
    uint32_t yp = rd16(prev + 6), n = 0, line = 0;
    vtx_t **q = vptr + dir;
    while (y_end > yp) {
        vtx_t *cur = *q;
        uint32_t y1 = rd16(cur + 6);
        int32_t len = (int32_t)(y1 - yp);
        uint32_t sk = 0;
        if (y_start > yp) { len += (int32_t)(yp - y_start); sk = y_start - yp; }
        if (y1 > y_end) len += (int32_t)(y_end - y1);
        if (len > 0) {
            if (n == 16) break;
            if (withz) walk_edge(spans, prev, cur, line, (uint8_t)len, n ? 0 : sk, 1);
            else walk_edge(spans, prev, cur, line, (uint8_t)len, n ? 0 : sk, 0);
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
 * r = x_right & 0x7fff, the larger (r when equal) + 1 unless that x has bit 15 (vertical) or bit 9 (>= 512) set */
static inline __attribute__((always_inline)) void walk_spans(uint8_t *s, int32_t lines, const int adjust) {
    const uint32_t ARR = SPAN_ARR;
    const uint32x4_t m15 = vdupq_n_u32(0x7fff), xmax = vdupq_n_u32(0x200), hi = vdupq_n_u32(0xffff0000u);
    const uint32x4_t mvx = vdupq_n_u32(0x8200);
    uint8_t *p = s;
    do {
        uint32x4_t xl = vld1q_u32((const uint32_t *)(p + 8 * ARR)), xr = vld1q_u32((const uint32_t *)(p + 9 * ARR));
        if (adjust) {
            uint32x4_t l = vandq_u32(xl, m15), r = vandq_u32(xr, m15), gt = vcgtq_u32(l, r);
            /* mask - 1 = + 1: l where l > r and neither bit, r where l <= r and neither bit (no carry into b) */
            l = vsubq_u32(l, vbicq_u32(gt, vtstq_u32(xl, mvx)));
            r = vsubq_u32(r, vbicq_u32(vmvnq_u32(gt), vtstq_u32(xr, mvx)));
            xl = vbslq_u32(hi, xl, l); xr = vbslq_u32(hi, xr, r);
        }
        uint32x4_t sw = vcgeq_u32(vandq_u32(xl, m15), vandq_u32(xr, m15));
#define PAIR(k, L, R) uint32x4_t L##0 = vld1q_u32((const uint32_t *)(p + (k) * ARR)), R##0 = vld1q_u32((const uint32_t *)(p + (k + 1) * ARR)); \
                      uint32x4_t L = vbslq_u32(sw, R##0, L##0), R = vbslq_u32(sw, L##0, R##0)
        PAIR(4, sl, sr); PAIR(0, wl, wr); PAIR(6, rl, rr); PAIR(2, zl, zr);
#undef PAIR
        uint32x4_t L = vbslq_u32(sw, xr, xl), R = vbslq_u32(sw, xl, xr);
        L = vbslq_u32(hi, L, vminq_u32(vandq_u32(L, m15), xmax));
        R = vbslq_u32(hi, R, vminq_u32(vandq_u32(R, m15), xmax));
        vst1q_u32((uint32_t *)(p + 8 * ARR), L);
        vst1q_u32((uint32_t *)(p + 9 * ARR), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(R), vreinterpretq_u16_u32(L))));
        vst1q_u32((uint32_t *)(p + 4 * ARR), sl);
        vst1q_u32((uint32_t *)(p + 5 * ARR), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(sr), vreinterpretq_u16_u32(sl))));
        vst1q_u32((uint32_t *)p, wl); vst1q_u32((uint32_t *)(p + ARR), vsubq_u32(wr, wl));
        vst1q_u32((uint32_t *)(p + 6 * ARR), rl);
        vst1q_u32((uint32_t *)(p + 7 * ARR), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(rr), vreinterpretq_u16_u32(rl))));
        vst1q_u32((uint32_t *)(p + 2 * ARR), zl); vst1q_u32((uint32_t *)(p + 3 * ARR), vsubq_u32(zr, zl));
        p += 16;
        lines -= 4;
    } while (lines > 0);
}

/* render_polygon_setup_edge_markers_c (spec/edges.c section 4) in NEON, 4 lines a step: line i's markers depend only
 * on x0 and the width of lines i - 1, i, i + 1, loaded as the words at -4, 0, +4 (their low halves). The first line's
 * previous neighbour without a line above is DraStic's (x0[0], x0[0] + wd[1]); with one, the line above. The steps
 * write up to 3 lines past the m DraStic computes: the last line's store (no line below) then overwrites line m,
 * and the lines beyond are below the polygon's (nothing reads them; the array has room) */
static void walk_markers(uint8_t *p, uint32_t lines, uint32_t clip) {
    const uint32_t ARR = SPAN_ARR;
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
            int32_t x0 = rd16(p + 8 * ARR);
            px = vsetq_lane_s32(x0, px, 0);
            pe = vsetq_lane_s32(x0 + (int32_t)rd16(p + 9 * ARR + 4), pe, 0);
        }
        int32x4_t ce = vaddq_s32(cx, vreinterpretq_s32_u32(w));
        int32x4_t lft = vmaxq_s32(vmaxq_s32(vaddq_s32(cx, vreinterpretq_s32_u32(one)), nx), px);
        int32x4_t rgt = vminq_s32(vminq_s32(vsubq_s32(ce, vreinterpretq_s32_u32(one)), ne), pe);
        uint32x4_t L = vminq_u32(w, vreinterpretq_u32_s32(vsubq_s32(lft, cx)));
        uint32x4_t R = vminq_u32(w, vreinterpretq_u32_s32(vsubq_s32(ce, rgt)));
        vst1q_u32((uint32_t *)(p + 10 * ARR + 4 * i), vsliq_n_u32(L, R, 16));
    }
    if (clip & 2) return;
    wr32(p + 10 * ARR + 4 * m, (uint32_t)(uint16_t)(rd16(p + 9 * ARR + 4 * m) + 1u));
}

/* the span block (DraStic's is on render_polygon_4x's stack at sp + 0x140) and the buffer it passes to
 * render_polygon_setup_4x (span + 0x840 up to the end of its 0xdcd0-byte frame: the scratch of DraStic's own setup,
 * which RAST_PDIFF runs on it) */
#define WALK_SPANS 0x840
#define WALK_BUF   0xd500
static __thread uint8_t *walk_mem;

int walk_polygon_4x(uint8_t *ctx, uint8_t *poly, uint8_t *verts, unsigned bin_top, unsigned bin_bot, walk_setup_fn *setup) {
    uint32_t a8 = U32(poly, 8);
    unsigned count = a8 & 15, flags = (a8 >> 8) & 0xff, ybot = a8 >> 23;
    if (!count || count > 9 || (flags & 0x40) || ((U32(poly, 4) >> 4) & 3) == 3) return 0;
    if (!walk_mem) { walk_mem = aligned_alloc(64, WALK_SPANS + WALK_BUF); if (!walk_mem) return 0; memset(walk_mem, 0, WALK_SPANS + WALK_BUF); }
    uint8_t *span = walk_mem, *buf = walk_mem + WALK_SPANS;

    const uint32_t seq = ((const uint32_t *)(ds_base + DS_VERTEX_ORDERS))[(a8 >> 16) & 0x7f];
    const size_t base = U16(poly, 0x1a);
    /* vp[k] for the walk order's k = 0..count-1, and vp[count] = vp[0] as DraStic's. Both chains stop at the lowest
     * vertex at the latest, within vp[0..count], when the record's bottom y is not below every vertex; a record whose
     * is walks on (up to 16 edges either way) into stale stack slots in DraStic: left to DraStic */
    vtx_t *vp[10];
    unsigned ymax = 0;
    for (unsigned k = 0; k < count; k++) {
        vp[k] = verts + 16 * (base + (k < 8 ? (seq >> (4 * k)) & 15 : 0));
        unsigned y = U16(vp[k], 6);
        if (y > ymax) ymax = y;
    }
    if (ybot > ymax) return 0;
    vp[count] = vp[0];

    unsigned y_top = U16(vp[0], 6), y_end = ybot, skip = 0, clip = 0;
    int lines = (int)(ybot - y_top);
    if (y_top < bin_top) { skip = bin_top - y_top; lines -= (int)skip; clip = 1; }
    if (ybot > bin_bot) { lines += (int)(bin_bot - ybot); y_end = bin_bot; clip |= 2; }
    if (lines <= 0) return 1;
    unsigned line0 = y_top - bin_top + skip;           /* the bin line of the first drawn line */

    uint8_t *sys = PTR(ctx, CTX_SYS);
    /* edge marking: a neighbour line above (top clipped) and below (bottom clipped) for the markers; per line the
     * larger x becomes exclusive (+1) unless that edge is vertical (bit 15) or x has bit 9 set (x >= 512) */
    const int marks = (U32(sys, SYS_DISP3DCNT) >> 5) & 1;
    unsigned ys = bin_top, ye = y_end, nl = (unsigned)lines;
    uint8_t *sp = span;
    if (marks && (clip & 1)) { ys--; nl++; sp = span + 4; }
    if (marks && (clip & 2)) { ye++; nl++; }
    /* the forward chain into the left arrays, the backward one into the right (one inlined copy of the walk) */
    for (int c = 0; c < 2; c++) walk_chain(span + c * SPAN_ARR, c ? &vp[count] : &vp[0], c ? -1 : 1, ys, ye, flags);
    if (!marks) {
        walk_spans(span, lines, 0);
        setup(ctx, span, poly, buf, line0, (unsigned)lines, flags, vp[0]);
        return 1;
    }
    walk_spans(span, (int32_t)nl, 1);         /* (with the larger x made exclusive first) */
    walk_markers(sp, (uint32_t)lines, clip);
    setup(ctx, sp, poly, buf, line0, (unsigned)lines, flags, vp[0]);
    return 1;
}
