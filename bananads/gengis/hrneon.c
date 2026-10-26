/* hrneon.c: the hi-res layouts' batches for the shading modes the assembly kernels do not have (decal, toon,
 * highlight), in NEON, 8 pixels a step: fused.c's line_px, lane for lane (render.gengis_hrneon).
 *
 * line_px is the scalar reference these polygons took (fused_asm.c's asm_batch_for -> neon_batch_for, whose NEON
 * batches are DraStic's 2x layout only). At 4x in Mario Kart DS they were 14% of the Gengis threads' time. The math
 * is line_px's, step for step (fused_neon.c's batch_neon, generalised to the layout's strides, is the template: the
 * perspective weights with the same float operations, the depth, the colour and texture interpolation, the
 * translucent write-back), with each mode's colour as line_px computes it in u32, including its carries between the
 * channels (decal's channels can exceed 8 bits; the toon lookups' indices are not bounded to the table: line_px reads
 * past it, so a group with an index past 31 takes line_px's lookups lane by lane), and shadow polygons (mode 3) with
 * line_px's stencil and line-mask steps. The deferred passes stay on line_px. */
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"

#pragma clang fp contract(off)

#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

int gx_hrneon = 1;                      /* render.gengis_hrneon (gengis_renderer.cpp sets it per scale) */
__thread int gx_ref;                    /* render.gengis_verify's reference render: the old paths */

static const float iota8f[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint32_t iota8u[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint16_t tailm[8][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },                     { 0xffff, 0, 0, 0, 0, 0, 0, 0 },
    { 0xffff, 0xffff, 0, 0, 0, 0, 0, 0 },           { 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0 }, { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0 },
};

typedef struct { uint32x4_t l, h; } u32x8;

static inline u32x8 widen(uint16x8_t m) {
    int16x8_t s = vreinterpretq_s16_u16(m);
    u32x8 r = { vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(s))), vreinterpretq_u32_s32(vmovl_high_s16(s)) };
    return r;
}
#define LERP_C(base, dl, w, k) \
    vmovn_u16(vshrq_n_u16(vcombine_u16(vshrn_n_u32(vreinterpretq_u32_s32(vmlal_laneq_s16(base, vget_low_s16(w), dl, k)), 16), \
                                       vshrn_n_u32(vreinterpretq_u32_s32(vmlal_high_laneq_s16(base, w, dl, k)), 16)), 2))
#define LERP_UV(base, dl, w, k) \
    vshrq_n_s16(vcombine_s16(vshrn_n_s32(vmlal_laneq_s16(base, vget_low_s16(w), dl, k), 16), \
                             vshrn_n_s32(vmlal_high_laneq_s16(base, w, dl, k), 16)), 3)
static inline uint16x8_t texc(int16x8_t x, int16x8_t lo, int16x8_t hi, uint16x8_t and, uint16x8_t flipbit) {
    x = veorq_s16(x, vreinterpretq_s16_u16(vtstq_u16(vreinterpretq_u16_s16(x), flipbit)));
    return vandq_u16(vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(x, lo), hi)), and);
}
/* ((a * b + a + b) & 0xffff) >> sh, & 0xff (a, b bytes: the sum fits 16 bits) */
static inline uint8x8_t mod6(uint8x8_t a, uint8x8_t b) { return vshrn_n_u16(vmlal_u8(vaddl_u8(a, b), a, b), 6); }
static inline uint8x8_t mod5(uint8x8_t a, uint8x8_t b) { return vshrn_n_u16(vmlal_u8(vaddl_u8(a, b), a, b), 5); }
static inline u32x8 pack4(uint8x8_t r, uint8x8_t g, uint8x8_t b, uint8x8_t a) {
    uint16x8_t rg = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(r, r), vcombine_u8(g, g)));
    uint16x8_t ba = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(b, b), vcombine_u8(a, a)));
    u32x8 o = { vreinterpretq_u32_u16(vzip1q_u16(rg, ba)), vreinterpretq_u32_u16(vzip2q_u16(rg, ba)) };
    return o;
}
/* the bytes of 8 u32 lanes */
static inline void unpack4(u32x8 c, uint8x8_t *r, uint8x8_t *g, uint8x8_t *b, uint8x8_t *a) {
    uint16x8_t lo = vuzp1q_u16(vreinterpretq_u16_u32(c.l), vreinterpretq_u16_u32(c.h));
    uint16x8_t hi = vuzp2q_u16(vreinterpretq_u16_u32(c.l), vreinterpretq_u16_u32(c.h));
    *r = vmovn_u16(lo); *g = vshrn_n_u16(lo, 8); *b = vmovn_u16(hi); *a = vshrn_n_u16(hi, 8);
}
static inline void gather(uint32x4_t al, uint32x4_t ah, const uint32_t *tex, uint32x4_t *t0, uint32x4_t *t1) {
    uint32x4_t a = vdupq_n_u32(tex[vgetq_lane_u32(al, 0)]), b = vdupq_n_u32(tex[vgetq_lane_u32(ah, 0)]);
    a = vld1q_lane_u32(tex + vgetq_lane_u32(al, 1), a, 1); b = vld1q_lane_u32(tex + vgetq_lane_u32(ah, 1), b, 1);
    a = vld1q_lane_u32(tex + vgetq_lane_u32(al, 2), a, 2); b = vld1q_lane_u32(tex + vgetq_lane_u32(ah, 2), b, 2);
    a = vld1q_lane_u32(tex + vgetq_lane_u32(al, 3), a, 3); b = vld1q_lane_u32(tex + vgetq_lane_u32(ah, 3), b, 3);
    *t0 = a; *t1 = b;
}
static inline void gather_pal(uint32x4_t al, uint32x4_t ah, const uint8_t *idx, const uint32_t *pal, uint32x4_t *t0,
                              uint32x4_t *t1) {
    uint32x4_t a = vdupq_n_u32(pal[idx[vgetq_lane_u32(al, 0)]]), b = vdupq_n_u32(pal[idx[vgetq_lane_u32(ah, 0)]]);
    a = vld1q_lane_u32(pal + idx[vgetq_lane_u32(al, 1)], a, 1); b = vld1q_lane_u32(pal + idx[vgetq_lane_u32(ah, 1)], b, 1);
    a = vld1q_lane_u32(pal + idx[vgetq_lane_u32(al, 2)], a, 2); b = vld1q_lane_u32(pal + idx[vgetq_lane_u32(ah, 2)], b, 2);
    a = vld1q_lane_u32(pal + idx[vgetq_lane_u32(al, 3)], a, 3); b = vld1q_lane_u32(pal + idx[vgetq_lane_u32(ah, 3)], b, 3);
    *t0 = a; *t1 = b;
}
/* toon[off + (s >> 1)] per lane, as line_px reads it (no bound: the index can pass the 32 entries). tb: the table's
 * 32 entries at off (tbl), used when every lane's index is below 32 */
static inline uint8x8_t toon_at(const uint8_t *toon, unsigned off, uint8x16x2_t tb, uint8x8_t idx, int small) {
    if (small) return vqtbl2_u8(tb, idx);
    uint8_t ix[8], o[8];
    vst1_u8(ix, idx);
    for (int j = 0; j < 8; j++) o[j] = toon[off + ix[j]];
    return vld1_u8(o);
}

/* MODE: 1 decal, 2 toon, 3 highlight (toon with DISP3DCNT bit 1), 4 shadow (polygon mode 3: modulate colour; id 0 the
 * mask, which sets the stencil, attribute bit 31, where its depth test fails; another id draws where the stencil is set
 * and the pixel's id is not its own) */
static inline __attribute__((always_inline))
void batch_hr(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0, const int DEPTH, const int TEX,
              const int TRANS, const int FLAT, const int MODE) {
    const uint32_t fl = P->flags;
    const unsigned sps = P->sps;
    const int equal = (P->attr >> 14) & 1, fog = (P->attr >> 15) & 1, edges = !TRANS && ((P->d3 >> 5) & 1);
    const int wconst = (fl >> 5) & 1, blend = (P->d3 >> 3) & 1, var = ((P->attr >> 11) & 1) | ((P->attr >> 14) & 2);
    const uint32x4_t pid24 = vdupq_n_u32(P->pid << 24), vK = vdupq_n_u32(P->K), m24 = vdupq_n_u32(0xffffff);
    const uint8x8_t vA = vdup_n_u8((uint8_t)P->A), aref8 = vdup_n_u8((uint8_t)P->aref), pid8 = vdup_n_u8((uint8_t)P->pid);
    const uint32x4_t A24 = vdupq_n_u32((P->A & 0xff) << 24);
    const uint32x4_t fog32 = vdupq_n_u32(fog ? 0x80000000u : 0);
    const float32x4_t io_l = vld1q_f32(iota8f), io_h = vld1q_f32(iota8f + 4);
    const int16x8_t s_lo = vdupq_n_s16(P->s_lo), s_hi = vdupq_n_s16(P->s_hi), t_lo = vdupq_n_s16(P->t_lo), t_hi = vdupq_n_s16(P->t_hi);
    const uint16x8_t s_and = vdupq_n_u16(P->s_and), t_and = vdupq_n_u16(P->t_and), s_flip = vdupq_n_u16(P->s_flip), t_flip = vdupq_n_u16(P->t_flip);
    const uint8x8_t fr = vdup_n_u8((uint8_t)(U16(bs, 6 * sps) >> 3)), fg = vdup_n_u8((uint8_t)(U16(bs, 6 * sps + 2) >> 3)),
                    fb = vdup_n_u8((uint8_t)(U16(bs, 8 * sps + 2) >> 3));
    const uint8_t *toon = P->toon;
    uint8x16x2_t tr_ = { { vld1q_u8(toon), vld1q_u8(toon + 16) } }, tg_ = { { vld1q_u8(toon + 0x20), vld1q_u8(toon + 0x30) } },
                 tb_ = { { vld1q_u8(toon + 0x40), vld1q_u8(toon + 0x50) } };
    const uint8x8_t c31 = vdup_n_u8(31), c63 = vdup_n_u8(63);
    uint64_t anypass = 0;

    for (unsigned l = 0; l < k; l++) {
        const uint8_t *s = bs + 4 * l;
        unsigned y = line + l, X = U16(s, 8 * sps), C = U16(s, 9 * sps);
        uint32_t *col_l = (uint32_t *)(P->ctx + y * P->lstride) + X;
        uint32_t *att_l = (uint32_t *)(P->ctx + P->attr_off + y * P->lstride) + X;
        uint8_t *id_l = P->ctx + P->id_off + y * P->id_stride + X;
        if (MODE == 4) {
            /* line_px's shadow line mask (one bit per bin line, header + 0x18): a mask's first touch of a line after a
             * shadow drew on it resets the line's stencil bits */
            uint64_t *lm = (uint64_t *)(P->ctx + P->hdr_off + 0x18), bit = 1ull << y;
            if (P->pid) *lm &= ~bit;
            else if (!(*lm & bit)) {
                *lm |= bit;
                uint32_t *al = (uint32_t *)(P->ctx + P->attr_off + y * P->lstride);
                for (unsigned i = 0; i < P->lstride / 4; i++) al[i] &= 0x7fffffffu;
            }
        }
        int32_t W0 = (int32_t)U32(s, 0), dW = (int32_t)U32(s, sps);
        float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
        float32x4_t num_l = vmulq_n_f32(io_l, fW0), num_h = vmulq_n_f32(io_h, fW0);
        float32x4_t den_l = vfmsq_n_f32(vdupq_n_f32(S), io_l, fD), den_h = vfmsq_n_f32(vdupq_n_f32(S), io_h, fD);
        const float32x4_t E0 = vdupq_n_f32(8.0f * fW0), E1 = vdupq_n_f32(8.0f * fD);
        const uint32_t Rwc = P->recip_u[C];
        uint32_t Z0 = U32(s, 2 * sps); int32_t dZ = (int32_t)U32(s, 3 * sps);
        uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
        uint64x2_t zs = vdupq_n_u64(zstep), z8 = vshlq_n_u64(zs, 3);
        uint64x2_t za = vaddq_u64(vdupq_n_u64((uint64_t)Z0 << 30), vcombine_u64(vdup_n_u64(0), vdup_n_u64(zstep)));
        uint64x2_t zb = vaddq_u64(za, vshlq_n_u64(zs, 1)), zc = vaddq_u64(za, vshlq_n_u64(zs, 2)), zd = vaddq_u64(zb, vshlq_n_u64(zs, 2));
        uint32_t rg0 = U32(s, 6 * sps), drg = U32(s, 7 * sps), xb = U32(s, 8 * sps), cdb = U32(s, 9 * sps);
        const int32x4_t rb = vdupq_n_s32((int32_t)((rg0 & 0xffff) << 15)), gb = vdupq_n_s32((int32_t)((rg0 >> 16) << 15)), bb = vdupq_n_s32((int32_t)((xb >> 16) << 15));
        uint32_t st0 = U32(s, 4 * sps), dst = U32(s, 5 * sps);
        const int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
        const int32x4_t ub = vdupq_n_s32((int32_t)(((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0)));
        const int32x4_t vb = vdupq_n_s32((int32_t)(((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0)));
        const int16_t dd[8] = { (int16_t)drg, (int16_t)(drg >> 16), (int16_t)(cdb >> 16), du, dv, 0, 0, 0 };
        const int16x8_t dl = vld1q_s16(dd);
        /* edge marks (opaque, DISP3DCNT bit 5): lanes i < EL or i >= C - ER (unsigned, as line_px compares) */
        const uint32x4_t vEL = vdupq_n_u32(U16(s, 10 * sps)), vCR = vdupq_n_u32((uint32_t)C - U16(s, 10 * sps + 2));

        for (unsigned i0 = 0; i0 < C; i0 += 8,
             num_l = vaddq_f32(num_l, E0), num_h = vaddq_f32(num_h, E0), den_l = vsubq_f32(den_l, E1), den_h = vsubq_f32(den_h, E1),
             za = vaddq_u64(za, z8), zb = vaddq_u64(zb, z8), zc = vaddq_u64(zc, z8), zd = vaddq_u64(zd, z8)) {
            int16x8_t st;
            const uint32x4_t iv_l = vaddq_u32(vld1q_u32(iota8u), vdupq_n_u32(i0)), iv_h = vaddq_u32(vld1q_u32(iota8u + 4), vdupq_n_u32(i0));
            if (wconst) {
                st = vreinterpretq_s16_u16(vcombine_u16(vshrn_n_u32(vmulq_n_u32(iv_l, Rwc), 16), vshrn_n_u32(vmulq_n_u32(iv_h, Rwc), 16)));
            } else {
                float32x4_t rl = vrecpeq_f32(den_l), rh = vrecpeq_f32(den_h);
                rl = vmulq_f32(rl, vrecpsq_f32(rl, den_l)); rh = vmulq_f32(rh, vrecpsq_f32(rh, den_h));
                rl = vmulq_f32(rl, vrecpsq_f32(rl, den_l)); rh = vmulq_f32(rh, vrecpsq_f32(rh, den_h));
                st = vcombine_s16(vmovn_s32(vcvtq_n_s32_f32(vmulq_f32(num_l, rl), 15)), vmovn_s32(vcvtq_n_s32_f32(vmulq_f32(num_h, rh), 15)));
            }
            u32x8 dep;
            if (DEPTH == 2) { dep.l = vK; dep.h = vK; }
            else if (DEPTH == 1) {
                int32x4_t sl = vmovl_s16(vget_low_s16(st)), sh = vmovl_high_s16(st), w4 = vdupq_n_s32(dW);
                int32x4_t a = vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(w4), vget_low_s32(sl)), 15), vshrn_n_s64(vmull_high_s32(w4, sl), 15));
                int32x4_t b = vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(w4), vget_low_s32(sh)), 15), vshrn_n_s64(vmull_high_s32(w4, sh), 15));
                dep.l = vaddq_u32(vreinterpretq_u32_s32(a), vdupq_n_u32((uint32_t)W0));
                dep.h = vaddq_u32(vreinterpretq_u32_s32(b), vdupq_n_u32((uint32_t)W0));
            } else {
                dep.l = vcombine_u32(vshrn_n_u64(za, 30), vshrn_n_u64(zb, 30));
                dep.h = vcombine_u32(vshrn_n_u64(zc, 30), vshrn_n_u64(zd, 30));
            }
            u32x8 da = { vld1q_u32(att_l + i0), vld1q_u32(att_l + i0 + 4) };
            uint16x8_t m16;
            {
                uint32x4_t al = vandq_u32(da.l, m24), ah = vandq_u32(da.h, m24);
                uint32x4_t pl, ph;
                if (equal) {
                    /* d = dep - (da & 0xffffff); |d| as s32 (0 - d when negative, mod 2^32) < 0x100 unsigned */
                    uint32x4_t dl_ = vsubq_u32(dep.l, al), dh_ = vsubq_u32(dep.h, ah);
                    dl_ = vreinterpretq_u32_s32(vabsq_s32(vreinterpretq_s32_u32(dl_)));
                    dh_ = vreinterpretq_u32_s32(vabsq_s32(vreinterpretq_s32_u32(dh_)));
                    pl = vcltq_u32(dl_, vdupq_n_u32(0x100)); ph = vcltq_u32(dh_, vdupq_n_u32(0x100));
                } else { pl = vcgtq_u32(al, dep.l); ph = vcgtq_u32(ah, dep.h); }
                m16 = vcombine_u16(vmovn_u32(pl), vmovn_u32(ph));
            }
            const uint16x8_t valid = i0 + 8 > C ? vld1q_u16(tailm[C - i0]) : vdupq_n_u16(0xffff);
            m16 = vandq_u16(m16, valid);
            if (MODE == 4) {
                const uint32x4_t sbit = vdupq_n_u32(0x80000000u);
                if (!P->pid) {
                    /* the mask: the stencil bit where the depth test fails; nothing else */
                    u32x8 f = widen(vbicq_u16(valid, m16));
                    if (vmaxvq_u32(vorrq_u32(f.l, f.h))) {
                        vst1q_u32(att_l + i0, vorrq_u32(da.l, vandq_u32(f.l, sbit)));
                        vst1q_u32(att_l + i0 + 4, vorrq_u32(da.h, vandq_u32(f.h, sbit)));
                    }
                    continue;
                }
                /* a shadow: only where the stencil is set and the pixel's id differs from its own */
                const uint32x4_t p6 = vdupq_n_u32(P->pid), c6 = vdupq_n_u32(63);
                uint32x4_t okl = vbicq_u32(vtstq_u32(da.l, sbit), vceqq_u32(vandq_u32(vshrq_n_u32(da.l, 24), c6), p6));
                uint32x4_t okh = vbicq_u32(vtstq_u32(da.h, sbit), vceqq_u32(vandq_u32(vshrq_n_u32(da.h, 24), c6), p6));
                m16 = vandq_u16(m16, vcombine_u16(vmovn_u32(okl), vmovn_u32(okh)));
            }
            uint8x8_t m8 = vmovn_u16(m16);
            uint64_t mb = vget_lane_u64(vreinterpret_u64_u8(m8), 0);
            if (!mb) continue;          /* nothing written for a group without a passing pixel (line_px's too) */

            /* the vertex colour */
            uint8x8_t vr, vg, vbb;
            if (FLAT) { vr = fr; vg = fg; vbb = fb; }
            else { vr = LERP_C(rb, dl, st, 0); vg = LERP_C(gb, dl, st, 1); vbb = LERP_C(bb, dl, st, 2); }
            uint8x8_t cr, cg, cb, ca;
            if (TEX) {
                uint16x8_t tu = texc(LERP_UV(ub, dl, st, 3), s_lo, s_hi, s_and, s_flip), tv = texc(LERP_UV(vb, dl, st, 4), t_lo, t_hi, t_and, t_flip);
                tu = vandq_u16(tu, m16); tv = vandq_u16(tv, m16);       /* (line_px: texel 0 where the depth failed) */
                uint32x4_t al = vmlal_n_u16(vmovl_u16(vget_low_u16(tu)), vget_low_u16(tv), P->tw);
                uint32x4_t ah = vmlal_high_n_u16(vmovl_high_u16(tu), tv, P->tw);
                uint32x4_t t0, t1;
                if (TEX == 2) gather_pal(al, ah, P->idx8, P->pal, &t0, &t1); else gather(al, ah, P->texels32, &t0, &t1);
                uint8x8_t tr, tg, tb, ta;
                { u32x8 t = { t0, t1 }; unpack4(t, &tr, &tg, &tb, &ta); }
                if (MODE == 1) {
                    /* decal: ta 31 -> a1 32, w 0; else a1 ta, w ta ? 31 - ta : 32; channels (t a1 + v w) >> 5 (up to 9
                     * bits), c = b << 16 | g << 8 | r | A << 24 in u32 (overlapping: as line_px ORs them) */
                    uint8x8_t is31 = vceq_u8(ta, c31), isz = vceq_u8(ta, vdup_n_u8(0));
                    uint8x8_t a1 = vbsl_u8(is31, vdup_n_u8(32), ta);
                    uint8x8_t w = vbsl_u8(is31, vdup_n_u8(0), vbsl_u8(isz, vdup_n_u8(32), vsub_u8(c31, ta)));
                    uint16x8_t r16 = vshrq_n_u16(vmlal_u8(vmull_u8(tr, a1), vr, w), 5);
                    uint16x8_t g16 = vshrq_n_u16(vmlal_u8(vmull_u8(tg, a1), vg, w), 5);
                    uint16x8_t b16 = vshrq_n_u16(vmlal_u8(vmull_u8(tb, a1), vbb, w), 5);
                    u32x8 c;
                    c.l = vorrq_u32(vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(b16)), 16), vshlq_n_u32(vmovl_u16(vget_low_u16(g16)), 8)),
                                    vorrq_u32(vmovl_u16(vget_low_u16(r16)), A24));
                    c.h = vorrq_u32(vorrq_u32(vshlq_n_u32(vmovl_high_u16(b16), 16), vshlq_n_u32(vmovl_high_u16(g16), 8)),
                                    vorrq_u32(vmovl_high_u16(r16), A24));
                    unpack4(c, &cr, &cg, &cb, &ca);
                } else if (MODE == 3) {
                    /* highlight: r g b from vr (all three, as line_px), alpha mod5; + toon[vr >> 1], clamp 63 */
                    uint8x8_t r = mod6(vr, tr), g = mod6(vr, tg), b = mod6(vr, tb), al = mod5(vA, ta);
                    uint8x8_t si = vshr_n_u8(vr, 1);
                    const int small = vmaxv_u8(si) < 32;
                    uint8x8_t hr = vadd_u8(toon_at(toon, 0, tr_, si, small), vand_u8(r, c63));
                    uint8x8_t hg = vadd_u8(toon_at(toon, 0x20, tg_, si, small), vand_u8(g, c63));
                    uint8x8_t hb = vadd_u8(toon_at(toon, 0x40, tb_, si, small), vand_u8(b, c63));
                    /* (the table's entries are 6-bit: the byte sums do not wrap; an index past 31 reads on past the
                     * table, as line_px does, lane by lane) */
                    if (!small) {
                        uint8_t ir[8], ig[8], ib[8], sx[8], o0[8], o1[8], o2[8];
                        vst1_u8(ir, vand_u8(r, c63)); vst1_u8(ig, vand_u8(g, c63)); vst1_u8(ib, vand_u8(b, c63)); vst1_u8(sx, si);
                        for (int j = 0; j < 8; j++) {
                            uint32_t a = toon[sx[j]] + ir[j], b2 = toon[0x20 + sx[j]] + ig[j], c2 = toon[0x40 + sx[j]] + ib[j];
                            o0[j] = (uint8_t)(a > 63 ? 63 : a); o1[j] = (uint8_t)(b2 > 63 ? 63 : b2); o2[j] = (uint8_t)(c2 > 63 ? 63 : c2);
                        }
                        cr = vld1_u8(o0); cg = vld1_u8(o1); cb = vld1_u8(o2);
                    } else { cr = vmin_u8(hr, c63); cg = vmin_u8(hg, c63); cb = vmin_u8(hb, c63); }
                    ca = vand_u8(al, c31);
                } else if (MODE == 4) {
                    cr = mod6(vr, tr); cg = mod6(vg, tg); cb = mod6(vbb, tb); ca = mod5(vA, ta);
                } else {
                    /* toon: the vertex colour := toon[vr >> 1] (0 from index 32 on), then modulate */
                    uint8x8_t si = vshr_n_u8(vr, 1);
                    uint8x8_t xr = vqtbl2_u8(tr_, si), xg = vqtbl2_u8(tg_, si), xb = vqtbl2_u8(tb_, si);
                    cr = mod6(xr, tr); cg = mod6(xg, tg); cb = mod6(xb, tb); ca = mod5(vA, ta);
                }
                m8 = vand_u8(m8, vcgt_u8(ca, aref8));
                mb = vget_lane_u64(vreinterpret_u64_u8(m8), 0);
                if (!mb) continue;
                m16 = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m8)));
            } else {
                if (MODE == 1 || MODE == 4) { cr = vr; cg = vg; cb = vbb; }
                else {
                    uint8x8_t si = vshr_n_u8(vr, 1);
                    const int small = vmaxv_u8(si) < 32;
                    uint8x8_t xr = toon_at(toon, 0, tr_, si, small), xg = toon_at(toon, 0x20, tg_, si, small), xb = toon_at(toon, 0x40, tb_, si, small);
                    if (MODE == 3) {
                        /* toon[] + v (u32 in line_px), min 63 */
                        uint16x8_t a = vaddl_u8(xr, vr), b = vaddl_u8(xg, vg), c = vaddl_u8(xb, vbb);
                        cr = vmovn_u16(vminq_u16(a, vdupq_n_u16(63))); cg = vmovn_u16(vminq_u16(b, vdupq_n_u16(63)));
                        cb = vmovn_u16(vminq_u16(c, vdupq_n_u16(63)));
                    } else { cr = xr; cg = xg; cb = xb; }
                }
                ca = vA;
            }
            anypass |= mb;
            u32x8 aw = { vorrq_u32(dep.l, pid24), vorrq_u32(dep.h, pid24) };

            if (!TRANS) {
                u32x8 c = pack4(cr, cg, cb, ca);
                c.l = vorrq_u32(c.l, fog32); c.h = vorrq_u32(c.h, fog32);
                if (edges) {
                    uint32x4_t el = vorrq_u32(vcltq_u32(iv_l, vEL), vcgeq_u32(iv_l, vCR)), eh = vorrq_u32(vcltq_u32(iv_h, vEL), vcgeq_u32(iv_h, vCR));
                    const uint32x4_t e40 = vdupq_n_u32(0x40000000u);
                    aw.l = vbslq_u32(el, vorrq_u32(vandq_u32(dep.l, m24), vorrq_u32(e40, pid24)), aw.l);
                    aw.h = vbslq_u32(eh, vorrq_u32(vandq_u32(dep.h, m24), vorrq_u32(e40, pid24)), aw.h);
                }
                u32x8 m = widen(m16);
                vst1q_u32(col_l + i0, vbslq_u32(m.l, c.l, vld1q_u32(col_l + i0)));
                vst1q_u32(col_l + i0 + 4, vbslq_u32(m.h, c.h, vld1q_u32(col_l + i0 + 4)));
                vst1q_u32(att_l + i0, vbslq_u32(m.l, aw.l, vld1q_u32(att_l + i0)));
                vst1q_u32(att_l + i0 + 4, vbslq_u32(m.h, aw.h, vld1q_u32(att_l + i0 + 4)));
            } else {
                u32x8 dc = { vld1q_u32(col_l + i0), vld1q_u32(col_l + i0 + 4) };
                uint8x8_t did = vld1_u8(id_l + i0);
                uint16x8_t dlo = vuzp1q_u16(vreinterpretq_u16_u32(dc.l), vreinterpretq_u16_u32(dc.h)), dhi = vuzp2q_u16(vreinterpretq_u16_u32(dc.l), vreinterpretq_u16_u32(dc.h));
                uint8x8_t dal = vand_u8(vshrn_n_u16(dhi, 8), vdup_n_u8(0x1f)), sa = ca;
                if (blend) {
                    uint8x8_t dz = vceq_u8(dal, vdup_n_u8(0));
                    uint8x8_t ws = vbsl_u8(dz, vdup_n_u8(0x1f), sa), wd = vbic_u8(vsub_u8(vdup_n_u8(0x1f), sa), dz);
                    uint8x8_t dr8 = vmovn_u16(dlo), dg8 = vshrn_n_u16(dlo, 8), db8 = vmovn_u16(dhi);
                    cr = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cr), cr, ws), dr8, wd), 5);
                    cg = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cg), cg, ws), dg8, wd), 5);
                    cb = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cb), cb, ws), db8, wd), 5);
                }
                u32x8 c = pack4(cr, cg, cb, vmax_u8(sa, dal));
                uint8x8_t op = vceq_u8(sa, vdup_n_u8(0x1f));
                m8 = vbic_u8(m8, vbic_u8(vceq_u8(did, pid8), op));
                uint8x8_t o8 = vand_u8(m8, op), t8 = vbic_u8(m8, op);
                int16x8_t m16s = vmovl_s8(vreinterpret_s8_u8(m8)), o16s = vmovl_s8(vreinterpret_s8_u8(o8));
                u32x8 M = widen(vreinterpretq_u16_s16(m16s)), O = widen(vreinterpretq_u16_s16(o16s)), cm = M;
                if (var & 2) {
                    c.l = vorrq_u32(c.l, vdupq_n_u32(0x80000000u)); c.h = vorrq_u32(c.h, vdupq_n_u32(0x80000000u));
                    uint16x8_t f16 = vmovl_u8(vand_u8(m8, vorr_u8(op, vdup_n_u8(0x7f))));
                    cm.l = vbslq_u32(m24, M.l, vshlq_n_u32(vmovl_u16(vget_low_u16(f16)), 24));
                    cm.h = vbslq_u32(m24, M.h, vshlq_n_u32(vmovl_high_u16(f16), 24));
                }
                u32x8 am = O;
                if (var & 1) { am.l = vbslq_u32(m24, M.l, O.l); am.h = vbslq_u32(m24, M.h, O.h); }
                vst1q_u32(col_l + i0, vbslq_u32(cm.l, c.l, dc.l)); vst1q_u32(col_l + i0 + 4, vbslq_u32(cm.h, c.h, dc.h));
                vst1q_u32(att_l + i0, vbslq_u32(am.l, aw.l, vld1q_u32(att_l + i0)));
                vst1q_u32(att_l + i0 + 4, vbslq_u32(am.h, aw.h, vld1q_u32(att_l + i0 + 4)));
                vst1_u8(id_l + i0, vbsl_u8(t8, pid8, did));
            }
        }
        if (TRANS) id0[l] = id_l[0];
    }
    if (anypass) { P->pass = 1; P->fogused |= 1; }
}

#define V(D, T, R, F, M) \
    static void hb_##D##T##R##F##M(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0) { batch_hr(P, bs, k, line, id0, D, T, R, F, M); }
#define V4(D, T, R, F) V(D, T, R, F, 1) V(D, T, R, F, 2) V(D, T, R, F, 3) V(D, T, R, F, 4)
#define V3(D, T) V4(D, T, 0, 0) V4(D, T, 0, 1) V4(D, T, 1, 0) V4(D, T, 1, 1)
#define V2(D) V3(D, 0) V3(D, 1) V3(D, 2)
V2(0) V2(1) V2(2)
#undef V
#define V(D, T, R, F, M) hb_##D##T##R##F##M,
#define ROW(D, T, R, F) { V(D, T, R, F, 1) V(D, T, R, F, 2) V(D, T, R, F, 3) V(D, T, R, F, 4) }
#define RT(D, T) { { ROW(D, T, 0, 0), ROW(D, T, 0, 1) }, { ROW(D, T, 1, 0), ROW(D, T, 1, 1) } }
static batch_fn *const table[3][3][2][2][4] = {
    { RT(0, 0), RT(0, 1), RT(0, 2) }, { RT(1, 0), RT(1, 1), RT(1, 2) }, { RT(2, 0), RT(2, 1), RT(2, 2) },
};

/* the batch for a decal / toon / highlight / shadow polygon on a hi-res layout, or 0 (line_px): not for the deferred
 * passes, the reference render of render.gengis_verify, or with render.gengis_hrneon off */
batch_fn *hrneon_batch_for(const poly_t *P) {
    if (!gx_hrneon || gx_ref || !P->hr || P->dmode || !P->mode) return 0;
    unsigned fl = P->flags;
    int D = (fl & 0x10) ? 2 : (fl & 8) ? 1 : 0, T = (fl & 2) ? (P->paletted ? 2 : 1) : 0;
    int M = P->mode == 1 ? 1 : P->mode == 3 ? 4 : (P->d3 & 2) ? 3 : 2;
    return table[D][T][fl & 1][(fl >> 2) & 1][M - 1];
}
