/* edges_impl.h: the edge walker and span setup routines of edges.c as a template, included with
 *   EDGES_FN(name)   the function name prefix (spec_ for the exact DraStic layout, hr_ for the hi-res pipeline)
 *   EDGES_ARR        bytes per span array (DraStic: 0xb0 = 44 entries; hi-res bins of 48 lines: 0x100)
 *   EDGES_XMAX       the x clamp of setup_spans (DraStic: 0x200, the line width)
 *   EDGES_LINKAGE    empty or static
 *   EDGES_NEON       1: the routines below in NEON (the hi-res pipeline, tools/rast/ut/t_edges.c's spec_neon_ set);
 *                    0 or undefined: the plain C ports. Both write the same bytes (DraStic's overruns included).
 * The math is edges.c's, verified bit-exact against DraStic in the spec_ instantiation; the hi-res one differs only
 * in these constants. */
#ifndef EDGES_LINKAGE
#define EDGES_LINKAGE
#endif
#ifndef EDGES_NEON
#define EDGES_NEON 0
#endif
#define EA(k) ((k) * EDGES_ARR)
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>

#pragma STDC FP_CONTRACT OFF

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr16(uint8_t *p, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(p, &t, 2); }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wrf(uint8_t *p, float v) { memcpy(p, &v, 4); }

EDGES_LINKAGE uint32_t EDGES_FN(edges_reciprocal)(int32_t i) {
    if (i >= 1 && i <= 512) return (uint32_t)(0x3fffffffu + (uint32_t)i) / (uint32_t)i;
    return 0;   /* 0 and 513..1023 are zero in DraStic; outside 0..1023 DraStic reads past the table */
}

/* ---------------------------------------------------------------------------------------------------------------- */
EDGES_LINKAGE void EDGES_FN(render_polygon_edge_perspective_coefficients)(float *outf, vtx_t **pairs, const uint8_t *counts, uint32_t n,
                                                       int32_t skip) {
#if EDGES_NEON
    /* lanes (num, den) of lines j, j+1 in p and j+2, j+3 in q: each lane accumulates d4 as the C port's v0..v3 */
    float *out = outf;
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *a = pairs[2 * e], *b = pairs[2 * e + 1];
        int32_t wa = (int32_t)rd32(a), wb = (int32_t)rd32(b);
        int32_t h = (int32_t)((uint32_t)rd16(b + 6) - rd16(a + 6));
        float A = (float)wa, D = (float)(int32_t)((uint32_t)wa - (uint32_t)wb), B = (float)wb, H = (float)h;
        float n0 = 0.0f * B, d0 = B * H;
        if (e == 0) { float k = (float)skip; n0 = n0 + k * A; d0 = d0 + k * D; }
        float32x2_t v0 = { n0, d0 }, d1 = { A, D };
        float32x2_t d2 = vadd_f32(d1, d1), d3 = vadd_f32(d1, d2), d4 = vadd_f32(d2, d2);
        float32x4_t p = vcombine_f32(v0, vadd_f32(v0, d1)), q = vcombine_f32(vadd_f32(v0, d2), vadd_f32(v0, d3));
        float32x4_t dd = vcombine_f32(d4, d4);
        int32_t cnt = counts[e];
        do {
            vst1q_f32(out, p); vst1q_f32(out + 4, q);
            out += 8;
            p = vaddq_f32(p, dd); q = vaddq_f32(q, dd);
            cnt -= 4;
        } while (cnt > 0);
        out += cnt * 2;
    }
#else
    uint8_t *out = (uint8_t *)outf;
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *a = pairs[2 * e], *b = pairs[2 * e + 1];
        int32_t wa = (int32_t)rd32(a), wb = (int32_t)rd32(b);
        int32_t h = (int32_t)((uint32_t)rd16(b + 6) - rd16(a + 6));
        float A = (float)wa, D = (float)(int32_t)((uint32_t)wa - (uint32_t)wb), B = (float)wb, H = (float)h;
        float v0[2], v1[2], v2[2], v3[2], d1[2] = { A, D }, d2[2], d3[2], d4[2];
        v0[0] = 0.0f * B;
        v0[1] = B * H;
        if (e == 0) {
            float k = (float)skip;
            float s0 = k * A, s1 = k * D;
            v0[0] = v0[0] + s0;
            v0[1] = v0[1] + s1;
        }
        for (int l = 0; l < 2; l++) {
            d2[l] = d1[l] + d1[l];
            d3[l] = d1[l] + d2[l];
            d4[l] = d2[l] + d2[l];
            v1[l] = v0[l] + d1[l];
            v2[l] = v0[l] + d2[l];
            v3[l] = v0[l] + d3[l];
        }
        int32_t cnt = counts[e];
        do {
            wrf(out + 0, v0[0]);  wrf(out + 4, v0[1]);
            wrf(out + 8, v1[0]);  wrf(out + 12, v1[1]);
            wrf(out + 16, v2[0]); wrf(out + 20, v2[1]);
            wrf(out + 24, v3[0]); wrf(out + 28, v3[1]);
            out += 32;
            for (int l = 0; l < 2; l++) {
                v0[l] = v0[l] + d4[l];
                v1[l] = v1[l] + d4[l];
                v2[l] = v2[l] + d4[l];
                v3[l] = v3[l] + d4[l];
            }
            cnt -= 4;
        } while (cnt > 0);
        out += (intptr_t)cnt * 8;
    }
#endif
}

EDGES_LINKAGE void EDGES_FN(render_polygon_edge_perspective_steps)(int16_t *outp, const float *inp, int32_t total) {
    uint8_t *out = (uint8_t *)outp;
    const uint8_t *in = (const uint8_t *)inp;
    do {
        float buf[16];
        memcpy(buf, in, 64);
        in += 64;
        float32x4x2_t p = vld2q_f32(buf), q = vld2q_f32(buf + 8);
        float32x4_t r0 = vrecpeq_f32(p.val[1]), r1 = vrecpeq_f32(q.val[1]);
        r0 = vmulq_f32(r0, vrecpsq_f32(r0, p.val[1]));
        r1 = vmulq_f32(r1, vrecpsq_f32(r1, q.val[1]));
        r0 = vmulq_f32(r0, vrecpsq_f32(r0, p.val[1]));
        r1 = vmulq_f32(r1, vrecpsq_f32(r1, q.val[1]));
        int32x4_t i0 = vcvtq_n_s32_f32(vmulq_f32(p.val[0], r0), 15);
        int32x4_t i1 = vcvtq_n_s32_f32(vmulq_f32(q.val[0], r1), 15);
        int16_t o[8];
        vst1q_s16(o, vcombine_s16(vmovn_s32(i0), vmovn_s32(i1)));
        memcpy(out, o, 16);
        out += 16;
        total -= 8;
    } while (total > 0);
}

EDGES_LINKAGE void EDGES_FN(render_polygon_edge_interpolate_w)(vtx_t **pairs, uint8_t *spans, const int16_t *stepsp,
                                            const uint8_t *counts, uint32_t n) {
    uint8_t *o = spans + EA(0);
    const uint8_t *st = (const uint8_t *)stepsp;
#if EDGES_NEON
    /* the s64 products d * q of 8 lines, their bits 15-46 (shrn) + w_a */
    for (uint32_t e = 0; e < n; e++) {
        uint32_t wa = rd32(pairs[2 * e]), wb = rd32(pairs[2 * e + 1]);
        const int32x2_t d = vdup_n_s32((int32_t)(wb - wa));
        const uint32x4_t w0 = vdupq_n_u32(wa);
        int32_t cnt = counts[e];
        do {
            int16x8_t q = vld1q_s16((const int16_t *)st);
            int32x4_t ql = vmovl_s16(vget_low_s16(q)), qh = vmovl_high_s16(q);
            int32x4_t rl = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(d, vget_low_s32(ql)), 15), vmull_s32(d, vget_high_s32(ql)), 15);
            int32x4_t rh = vshrn_high_n_s64(vshrn_n_s64(vmull_s32(d, vget_low_s32(qh)), 15), vmull_s32(d, vget_high_s32(qh)), 15);
            vst1q_u32((uint32_t *)o, vaddq_u32(vreinterpretq_u32_s32(rl), w0));
            vst1q_u32((uint32_t *)(o + 16), vaddq_u32(vreinterpretq_u32_s32(rh), w0));
            st += 16; o += 32;
            cnt -= 8;
        } while (cnt > 0);
        st += (intptr_t)cnt * 2;
        o += (intptr_t)cnt * 4;
    }
#else
    for (uint32_t e = 0; e < n; e++) {
        uint32_t wa = rd32(pairs[2 * e]), wb = rd32(pairs[2 * e + 1]);
        int32_t d = (int32_t)(wb - wa);
        int32_t cnt = counts[e];
        do {
            for (int k = 0; k < 8; k++) {
                int64_t prod = (int64_t)d * (int16_t)rd16(st + 2 * k);
                wr32(o + 4 * k, (uint32_t)((uint64_t)prod >> 15) + wa);
            }
            st += 16; o += 32;
            cnt -= 8;
        } while (cnt > 0);
        st += (intptr_t)cnt * 2;
        o += (intptr_t)cnt * 4;
    }
#endif
}

static inline uint16_t c6(uint32_t v) { return (uint16_t)(2 * v + (v != 0)); }

EDGES_LINKAGE void EDGES_FN(render_polygon_edge_interpolate_parameters)(vtx_t **pairs, uint8_t *spans, const int16_t *stepsp,
                                                     const uint8_t *counts, uint32_t n) {
    uint8_t *ost = spans + EA(4), *org = spans + EA(6), *oxb = spans + EA(8);
    const uint8_t *st = (const uint8_t *)stepsp;
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *a = pairs[2 * e], *b = pairs[2 * e + 1];
        uint32_t ca = rd16(a + 0xa), cb = rd16(b + 0xa);
        uint16_t ea[3] = { c6(ca & 31), c6((ca >> 5) & 31), c6((ca >> 10) & 31) };
        uint16_t eb[3] = { c6(cb & 31), c6((cb >> 5) & 31), c6((cb >> 10) & 31) };
        int16_t dc[3];
        uint32_t cbase[3];
        for (int k = 0; k < 3; k++) {
            dc[k] = (int16_t)(uint16_t)((uint16_t)(eb[k] - ea[k]) << 3);
            cbase[k] = ((uint32_t)ea[k] << 18) + 0x38000;
        }
        int16_t sa = (int16_t)rd16(a + 0xc), ta = (int16_t)rd16(a + 0xe);
        int16_t ds = (int16_t)(uint16_t)(rd16(b + 0xc) - (uint16_t)sa), dt = (int16_t)(uint16_t)(rd16(b + 0xe) - (uint16_t)ta);
        uint32_t sbase = (uint32_t)((int32_t)sa * 32768) + (ds > 0 ? 0x800 : 0);
        uint32_t tbase = (uint32_t)((int32_t)ta * 32768) + (dt > 0 ? 0x800 : 0);
        int32_t cnt = counts[e];
#if EDGES_NEON
        /* 4 lines: s16 x s16 products + the u32 bases, bits 15-30 (shrn), interleaved as the halves of the words */
        const uint32x4_t vs = vdupq_n_u32(sbase), vt = vdupq_n_u32(tbase), vr = vdupq_n_u32(cbase[0]);
        const uint32x4_t vg = vdupq_n_u32(cbase[1]), vb = vdupq_n_u32(cbase[2]);
        const uint16x4_t zero = vdup_n_u16(0);
#define PAR(base, d) vshrn_n_u32(vaddq_u32(base, vreinterpretq_u32_s32(vmull_n_s16(q, d))), 15)
        do {
            int16x4_t q = vld1_s16((const int16_t *)st);
            uint16x4x2_t vst = { { PAR(vs, ds), PAR(vt, dt) } }, vrg = { { PAR(vr, dc[0]), PAR(vg, dc[1]) } };
            uint16x4x2_t vxb = { { zero, PAR(vb, dc[2]) } };                    /* DraStic: stale v26.h[k] */
            vst2_u16((uint16_t *)ost, vst); vst2_u16((uint16_t *)org, vrg); vst2_u16((uint16_t *)oxb, vxb);
            st += 8; ost += 16; org += 16; oxb += 16;
            cnt -= 4;
        } while (cnt > 0);
#undef PAR
#else
        do {
            for (int k = 0; k < 4; k++) {
                int32_t q = (int16_t)rd16(st + 2 * k);
                uint32_t s = (sbase + (uint32_t)(q * ds)) >> 15, t = (tbase + (uint32_t)(q * dt)) >> 15;
                uint32_t r = (cbase[0] + (uint32_t)(q * dc[0])) >> 15, g = (cbase[1] + (uint32_t)(q * dc[1])) >> 15;
                uint32_t bb = (cbase[2] + (uint32_t)(q * dc[2])) >> 15;
                wr16(ost + 4 * k, s); wr16(ost + 4 * k + 2, t);
                wr16(org + 4 * k, r); wr16(org + 4 * k + 2, g);
                wr16(oxb + 4 * k, 0); /* DraStic: stale v26.h[k] */
                wr16(oxb + 4 * k + 2, bb);
            }
            st += 8; ost += 16; org += 16; oxb += 16;
            cnt -= 4;
        } while (cnt > 0);
#endif
        ost += (intptr_t)cnt * 4; org += (intptr_t)cnt * 4; oxb += (intptr_t)cnt * 4;
        st += (intptr_t)cnt * 2;
    }
}

static void interp_x(vtx_t **pairs, uint8_t *spans, const uint8_t *counts, uint32_t n, uint32_t skip, int withz) {
    uint8_t *oz = spans + EA(2), *ox = spans + EA(8);
    for (uint32_t e = 0; e < n; e++) {
        const uint8_t *a = pairs[2 * e], *b = pairs[2 * e + 1];
        uint32_t xa = rd16(a + 4), xb = rd16(b + 4);
        int32_t dy = (int32_t)((uint32_t)rd16(b + 6) - rd16(a + 6));
        int32_t dx = (int32_t)(xb - xa);
        uint64_t r = EDGES_FN(edges_reciprocal)(dy);
        uint64_t prod = (uint64_t)(int64_t)dx * r;
        uint32_t xs = (uint32_t)((dx < 0 ? prod + 0xfff : prod) >> 12);
        uint32_t vflag = dx == 0 ? 0x8000 : 0;
        uint32_t x = xa << 18;
        uint32_t za = rd16(a + 8), zb = rd16(b + 8);
        int32_t dz = (int32_t)((zb - za) << 9);
        uint64_t zs = (uint64_t)(int64_t)dz * r + (dz < 0 ? 0x40000000u : 0);
        uint64_t z = (uint64_t)za << 39;
        if (e == 0) { x += xs * skip; z += (uint64_t)skip * zs; }
        uint32_t cnt = counts[e];
#if EDGES_NEON
        /* 4 lines a step: x + i xs (u32) and z + i zs (u64) wrap as the sums do; the x halfwords merged into the
         * words (their high halves, b, are interpolate_parameters') */
        if (cnt >= 4) {
            static const uint32_t iota[4] = { 0, 1, 2, 3 };
            uint32x4_t xv = vmlaq_n_u32(vdupq_n_u32(x), vld1q_u32(iota), xs);
            const uint32x4_t x4 = vdupq_n_u32(xs * 4), vf = vdupq_n_u32(vflag), lo = vdupq_n_u32(0xffff);
            uint64x2_t za = { z, z + zs }, zb = { z + 2 * zs, z + 3 * zs };
            const uint64x2_t z4 = vdupq_n_u64(4 * zs);
            for (; cnt >= 4; cnt -= 4) {
                uint32x4_t wv = vld1q_u32((const uint32_t *)ox);
                vst1q_u32((uint32_t *)ox, vbslq_u32(lo, vorrq_u32(vf, vshrq_n_u32(xv, 18)), wv));
                if (withz) vst1q_u32((uint32_t *)oz, vshrn_high_n_u64(vshrn_n_u64(za, 30), zb, 30));
                xv = vaddq_u32(xv, x4); za = vaddq_u64(za, z4); zb = vaddq_u64(zb, z4);
                oz += 16; ox += 16;
            }
            x = vgetq_lane_u32(xv, 0); z = vgetq_lane_u64(za, 0);
        }
#endif
        for (uint32_t i = 0; i < cnt; i++) {
            if (withz) wr32(oz, (uint32_t)(z >> 30));
            wr16(ox, vflag | (x >> 18));
            oz += 4; ox += 4;
            x += xs; z += zs;
        }
    }
}

EDGES_LINKAGE void EDGES_FN(render_polygon_edge_interpolate_xz_c)(vtx_t **pairs, uint8_t *spans, const uint8_t *counts, uint32_t n,
                                               uint32_t skip) {
    interp_x(pairs, spans, counts, n, skip, 1);
}

EDGES_LINKAGE void EDGES_FN(render_polygon_edge_interpolate_x_c)(vtx_t **pairs, uint8_t *spans, const uint8_t *counts, uint32_t n,
                                              uint32_t skip) {
    interp_x(pairs, spans, counts, n, skip, 0);
}

/* ---------------------------------------------------------------------------------------------------------------- */
EDGES_LINKAGE void EDGES_FN(render_polygon_interpolate_edges)(void *unused, uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                           uint32_t y_start, uint32_t y_end, int32_t dir, uint32_t flags) {
    (void)unused;
    vtx_t *pairs[32];
    uint8_t counts[16];
    uint32_t n = 0, total = 0, skip0 = 0;
    vtx_t *prev = vptr[0];
    uint32_t yp = rd16(prev + 6);
    if (y_end > yp) {
        vtx_t **q = vptr + dir;
        do {
            vtx_t *cur = *q;
            uint32_t y1 = rd16(cur + 6);
            int32_t len = (int32_t)(y1 - yp);
            uint32_t sk = 0;
            if (y_start > yp) { len += (int32_t)(yp - y_start); sk = y_start - yp; }
            if (y1 > y_end) len += (int32_t)(y_end - y1);
            if (len > 0) {
                if (n == 16) break;                     /* the arrays hold 16 edges (never reached by DraStic's polygons) */
                counts[n] = (uint8_t)len;
                if (n == 0) skip0 = sk;
                total += (uint32_t)len;
                pairs[2 * n] = prev;
                pairs[2 * n + 1] = cur;
                n++;
            }
            yp = y1;
            prev = cur;
            q += dir;
        } while (y_end > yp);
    }
    if (n == 0) return; /* DraStic's asm helpers would run away here */
    EDGES_FN(render_polygon_edge_perspective_coefficients)((float *)scratch, pairs, counts, n, (int32_t)skip0);
    EDGES_FN(render_polygon_edge_perspective_steps)((int16_t *)scratch, (const float *)scratch, (int32_t)total);
    EDGES_FN(render_polygon_edge_interpolate_w)(pairs, spans, (const int16_t *)scratch, counts, n);
    EDGES_FN(render_polygon_edge_interpolate_parameters)(pairs, spans, (const int16_t *)scratch, counts, n);
    if (flags & 0x18) EDGES_FN(render_polygon_edge_interpolate_x_c)(pairs, spans, counts, n, skip0);
    else EDGES_FN(render_polygon_edge_interpolate_xz_c)(pairs, spans, counts, n, skip0);
}

EDGES_LINKAGE void EDGES_FN(render_polygon_interpolate_edges_constprop_0)(uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                                       uint32_t y_start, uint32_t y_end, uint32_t flags) {
    EDGES_FN(render_polygon_interpolate_edges)(0, spans, scratch, vptr, y_start, y_end, 1, flags);
}

EDGES_LINKAGE void EDGES_FN(render_polygon_interpolate_edges_constprop_1)(uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                                       uint32_t y_start, uint32_t y_end, uint32_t flags) {
    EDGES_FN(render_polygon_interpolate_edges)(0, spans, scratch, vptr, y_start, y_end, -1, flags);
}

/* ---------------------------------------------------------------------------------------------------------------- */
EDGES_LINKAGE void EDGES_FN(render_polygon_setup_spans_4x)(uint8_t *s, int32_t lines) {
#if EDGES_NEON
    /* 4 lines a step (8 per iteration, as DraStic's): each left/right pair selected by the swap mask, the x halves
     * masked and clamped, the right arrays rewritten as 16-bit (x b, s t, r g) or 32-bit (w, z) differences */
    const uint32x4_t m15 = vdupq_n_u32(0x7fff), xmax = vdupq_n_u32(EDGES_XMAX), hi = vdupq_n_u32(0xffff0000u);
    uint8_t *p = s;
    do {
        for (int h = 0; h < 2; h++, p += 16) {
            uint32x4_t xl = vld1q_u32((const uint32_t *)(p + EA(8))), xr = vld1q_u32((const uint32_t *)(p + EA(9)));
            uint32x4_t sw = vcgeq_u32(vandq_u32(xl, m15), vandq_u32(xr, m15));
#define PAIR(k, L, R) uint32x4_t L##0 = vld1q_u32((const uint32_t *)(p + EA(k))), R##0 = vld1q_u32((const uint32_t *)(p + EA(k + 1))); \
                      uint32x4_t L = vbslq_u32(sw, R##0, L##0), R = vbslq_u32(sw, L##0, R##0)
            PAIR(4, sl, sr); PAIR(0, wl, wr); PAIR(6, rl, rr); PAIR(2, zl, zr);
#undef PAIR
            uint32x4_t L = vbslq_u32(sw, xr, xl), R = vbslq_u32(sw, xl, xr);
            L = vbslq_u32(hi, L, vminq_u32(vandq_u32(L, m15), xmax));
            R = vbslq_u32(hi, R, vminq_u32(vandq_u32(R, m15), xmax));
            vst1q_u32((uint32_t *)(p + EA(8)), L);
            vst1q_u32((uint32_t *)(p + EA(9)), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(R), vreinterpretq_u16_u32(L))));
            vst1q_u32((uint32_t *)(p + EA(4)), sl);
            vst1q_u32((uint32_t *)(p + EA(5)), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(sr), vreinterpretq_u16_u32(sl))));
            vst1q_u32((uint32_t *)(p + EA(0)), wl); vst1q_u32((uint32_t *)(p + EA(1)), vsubq_u32(wr, wl));
            vst1q_u32((uint32_t *)(p + EA(6)), rl);
            vst1q_u32((uint32_t *)(p + EA(7)), vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(rr), vreinterpretq_u16_u32(rl))));
            vst1q_u32((uint32_t *)(p + EA(2)), zl); vst1q_u32((uint32_t *)(p + EA(3)), vsubq_u32(zr, zl));
        }
        lines -= 8;
    } while (lines > 0);
#else
    uint32_t i = 0;
    do {
        for (uint32_t k = 0; k < 8; k++, i++) {
            uint32_t o = 4 * i;
            uint32_t xl = rd16(s + EA(8) + o) & 0x7fff, xr = rd16(s + EA(9) + o) & 0x7fff;
            uint32_t bl = rd16(s + EA(8) + 2 + o), br = rd16(s + EA(9) + 2 + o);
            uint32_t sl = rd16(s + EA(4) + o), tl = rd16(s + EA(4) + 2 + o), sr = rd16(s + EA(5) + o), tr = rd16(s + EA(5) + 2 + o);
            uint32_t wl = rd32(s + EA(0) + o), wr = rd32(s + EA(1) + o);
            uint32_t rl = rd16(s + EA(6) + o), gl = rd16(s + EA(6) + 2 + o), rr = rd16(s + EA(7) + o), gr = rd16(s + EA(7) + 2 + o);
            uint32_t zl = rd32(s + EA(2) + o), zr = rd32(s + EA(3) + o);
            uint32_t t;
#define SWP(a, b) (t = a, a = b, b = t)
            if (xl >= xr) {
                SWP(xl, xr); SWP(bl, br); SWP(sl, sr); SWP(tl, tr);
                SWP(wl, wr); SWP(rl, rr); SWP(gl, gr); SWP(zl, zr);
            }
#undef SWP
            if (xl > EDGES_XMAX) xl = EDGES_XMAX;
            if (xr > EDGES_XMAX) xr = EDGES_XMAX;
            wr16(s + EA(8) + o, xl); wr16(s + EA(8) + 2 + o, bl);
            wr16(s + EA(9) + o, xr - xl); wr16(s + EA(9) + 2 + o, br - bl);
            wr16(s + EA(4) + o, sl); wr16(s + EA(4) + 2 + o, tl);
            wr16(s + EA(5) + o, sr - sl); wr16(s + EA(5) + 2 + o, tr - tl);
            wr32(s + EA(0) + o, wl); wr32(s + EA(1) + o, wr - wl);
            wr16(s + EA(6) + o, rl); wr16(s + EA(6) + 2 + o, gl);
            wr16(s + EA(7) + o, rr - rl); wr16(s + EA(7) + 2 + o, gr - gl);
            wr32(s + EA(2) + o, zl); wr32(s + EA(3) + o, zr - zl);
        }
        lines -= 8;
    } while (lines > 0);
#endif
}

EDGES_LINKAGE void EDGES_FN(render_polygon_setup_edge_markers_c)(uint8_t *p, uint32_t lines, uint32_t clip) {
    int32_t cx = rd16(p + EA(8)), px, pe;
    if (clip & 1) { px = rd16(p + EA(8) - 4); pe = (int32_t)rd16(p + EA(9) - 4) + px; }
    else { px = cx; pe = (int32_t)rd16(p + EA(9) + 4) + cx; }
    uint32_t w = rd16(p + EA(9));
    int32_t ce = (int32_t)w + cx;
    uint32_t m;
    if (clip & 2) { if (lines == 0) return; m = lines; }
    else m = lines - 1;
    uint32_t i = 0;
    for (; i < m; i++) {
        int32_t nx = rd16(p + EA(8) + 4 + 4 * i);
        uint32_t nw = rd16(p + EA(9) + 4 + 4 * i);
        int32_t ne = (int32_t)nw + nx;
        int32_t lo = cx + 1, hi = ce - 1;
        if (!(lo >= nx)) lo = nx;
        if (!(hi <= ne)) hi = ne;
        if (!(lo >= px)) lo = px;
        if (!(hi <= pe)) hi = pe;
        uint32_t L = (uint32_t)(lo - cx), R = (uint32_t)(ce - hi);
        if (w <= L) L = w;
        if (w <= R) R = w;
        wr16(p + EA(10) + 4 * i, L);
        wr16(p + EA(10) + 2 + 4 * i, R);
        px = cx; pe = ce; cx = nx; ce = ne; w = nw;
    }
    if (clip & 2) return;
    wr16(p + EA(10) + 4 * i, rd16(p + EA(9) + 4 * i) + 1u);
    wr16(p + EA(10) + 2 + 4 * i, 0);
}

#undef EA
