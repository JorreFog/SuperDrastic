/* fused_neon.c: the fused pixel pipeline (fused.c) in NEON, 8 pixels per step, one call per batch.
 *
 * Same math as fused.c's line_px, lane for lane. Specialised at compile time on the depth source (z, w, constant),
 * texture fetch (none, direct, paletted), translucency and flat colour; shading modes other than modulate
 * (decal, toon, highlight) stay on line_px. A group of 8 pixels in which no pixel passes the depth test stops
 * right there.
 *
 * Shortcuts that give the same bits:
 *   - a group whose 8 pixels all pass is stored without the read-modify-write of the line buffers;
 *   - a line with white vertex colours (6-bit 63 at both ends) on a polygon with alpha 31: modulate gives the
 *     texel back unchanged ((64 * (t + 1) - 1) >> 6 = t, (32 * (ta + 1) - 1) >> 5 = ta), so the colour
 *     interpolation and the modulate are skipped;
 *   - edge marking (the first/last pixels of each line) is a scalar fix-up after the line, from the pass mask. */
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"

#pragma clang fp contract(off)

extern unsigned long st_written; extern int rast_stats;

#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

static const float iota8f[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint32_t iota8u[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
/* tail[n]: the first n of 8 lanes set */
static const uint16_t tail[8][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },                     { 0xffff, 0, 0, 0, 0, 0, 0, 0 },
    { 0xffff, 0xffff, 0, 0, 0, 0, 0, 0 },           { 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0 }, { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0 },
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0 },
};

typedef struct { uint32x4_t l, h; } u32x8;

/* depth tests -> 8 lanes of 0 / 0xffff, in inline asm: clang otherwise turns the compare results into boolean
 * vectors and re-materialises them with xtn/ushll/shl/cmlt (12 instructions instead of 3). da is consumed. */
static inline uint16x8_t depth_less(u32x8 da, u32x8 dep) {           /* (da & 0xffffff) > dep */
    uint16x8_t r;
    __asm__("bic %1.4s, #0xff, lsl #24\n\tbic %2.4s, #0xff, lsl #24\n\t"
            "cmhi %1.4s, %1.4s, %3.4s\n\tcmhi %2.4s, %2.4s, %4.4s\n\tuzp1 %0.8h, %1.8h, %2.8h"
            : "=w"(r), "+w"(da.l), "+w"(da.h) : "w"(dep.l), "w"(dep.h));
    return r;
}
static inline uint16x8_t depth_equal(u32x8 da, u32x8 dep) {          /* |dep - (da & 0xffffff)| < 0x100 */
    uint16x8_t r; uint32x4_t c;
    __asm__("bic %1.4s, #0xff, lsl #24\n\tbic %2.4s, #0xff, lsl #24\n\t"
            "sub %1.4s, %3.4s, %1.4s\n\tsub %2.4s, %4.4s, %2.4s\n\tabs %1.4s, %1.4s\n\tabs %2.4s, %2.4s\n\t"
            "movi %5.4s, #1, lsl #8\n\tcmhi %1.4s, %5.4s, %1.4s\n\tcmhi %2.4s, %5.4s, %2.4s\n\tuzp1 %0.8h, %1.8h, %2.8h"
            : "=w"(r), "+w"(da.l), "+w"(da.h), "=&w"(c) : "w"(dep.l), "w"(dep.h));
    return r;
}

/* 0/0xffff lanes -> 0/0xffffffff lanes */
static inline u32x8 widen(uint16x8_t m) {
    int16x8_t s = vreinterpretq_s16_u16(m);
    u32x8 r = { vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(s))), vreinterpretq_u32_s32(vmovl_high_s16(s)) };
    return r;
}
/* (u8)((u32)(base + delta * w) >> 18), delta = lane `k` of dl */
#define LERP_C(base, dl, w, k) \
    vmovn_u16(vshrq_n_u16(vcombine_u16(vshrn_n_u32(vreinterpretq_u32_s32(vmlal_laneq_s16(base, vget_low_s16(w), dl, k)), 16), \
                                       vshrn_n_u32(vreinterpretq_u32_s32(vmlal_high_laneq_s16(base, w, dl, k)), 16)), 2))
/* (s16)((base + d * w) >> 16) >> 3 */
#define LERP_UV(base, dl, w, k) \
    vshrq_n_s16(vcombine_s16(vshrn_n_s32(vmlal_laneq_s16(base, vget_low_s16(w), dl, k), 16), \
                             vshrn_n_s32(vmlal_high_laneq_s16(base, w, dl, k), 16)), 3)
/* texcoord(): clamp / wrap / flip in one branch-free form: flip inverts where (x & flipbit), then clamp to
 * [lo, hi] (the whole s16 range for wrap/flip) and mask (0xffff for clamp) */
static inline uint16x8_t texc(int16x8_t x, int16x8_t lo, int16x8_t hi, uint16x8_t and, uint16x8_t flipbit) {
    x = veorq_s16(x, vreinterpretq_s16_u16(vtstq_u16(vreinterpretq_u16_s16(x), flipbit)));
    return vandq_u16(vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(x, lo), hi)), and);
}
/* ((a+1)*(b+1)-1) >> sh on bytes, & 0xff */
static inline uint8x8_t mod6(uint8x8_t a, uint8x8_t b) { return vshrn_n_u16(vmlal_u8(vaddl_u8(a, b), a, b), 6); }
static inline uint8x8_t mod5(uint8x8_t a, uint8x8_t b) { return vshrn_n_u16(vmlal_u8(vaddl_u8(a, b), a, b), 5); }
/* r | g<<8 | b<<16 | a<<24 */
static inline u32x8 pack4(uint8x8_t r, uint8x8_t g, uint8x8_t b, uint8x8_t a) {
    uint16x8_t rg = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(r, r), vcombine_u8(g, g)));
    uint16x8_t ba = vreinterpretq_u16_u8(vzip1q_u8(vcombine_u8(b, b), vcombine_u8(a, a)));
    u32x8 o = { vreinterpretq_u32_u16(vzip1q_u16(rg, ba)), vreinterpretq_u32_u16(vzip2q_u16(rg, ba)) };
    return o;
}
/* 8 texels at the addresses in al/ah */
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

static inline __attribute__((always_inline))
void batch_neon(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0, const int DEPTH, const int TEX,
                const int WRAP, const int TRANS, const int FLAT) {
    const uint32_t fl = P->flags;
    const int equal = (P->attr >> 14) & 1, fog = (P->attr >> 15) & 1, edges = !TRANS && ((P->d3 >> 5) & 1);
    const int wconst = (fl >> 5) & 1, blend = (P->d3 >> 3) & 1, var = ((P->attr >> 11) & 1) | ((P->attr >> 14) & 2);
    const uint32x4_t pid24 = vdupq_n_u32(P->pid << 24), vK = vdupq_n_u32(P->K), m24 = vdupq_n_u32(0xffffff);
    const uint8x8_t vA = vdup_n_u8((uint8_t)P->A), aref8 = vdup_n_u8((uint8_t)P->aref), pid8 = vdup_n_u8((uint8_t)P->pid);
    const uint8x8_t fog8 = vdup_n_u8(fog ? 0x80 : 0);               /* colour bit 31 for fogged opaque polygons */
    const float32x4_t io_l = vld1q_f32(iota8f), io_h = vld1q_f32(iota8f + 4);
    const int16x8_t s_lo = vdupq_n_s16(P->s_lo), s_hi = vdupq_n_s16(P->s_hi), t_lo = vdupq_n_s16(P->t_lo), t_hi = vdupq_n_s16(P->t_hi);
    const uint16x8_t s_and = vdupq_n_u16(P->s_and), t_and = vdupq_n_u16(P->t_and), s_flip = vdupq_n_u16(P->s_flip), t_flip = vdupq_n_u16(P->t_flip);
    const uint8x8_t fr = vdup_n_u8((uint8_t)(U16(bs, 0x420) >> 3)), fg = vdup_n_u8((uint8_t)(U16(bs, 0x422) >> 3)), fb = vdup_n_u8((uint8_t)(U16(bs, 0x582) >> 3));
    const int flat_white = FLAT && P->A == 31 && (U16(bs, 0x420) >> 3) == 63 && (U16(bs, 0x422) >> 3) == 63 && (U16(bs, 0x582) >> 3) == 63;
    uint64_t anypass = 0;
    uint8_t pm[520];

    for (unsigned l = 0; l < k; l++) {
        const uint8_t *s = bs + 4 * l;
        unsigned y = line + l, X = U16(s, 0x580), C = U16(s, 0x630);
        uint32_t *col_l = (uint32_t *)(P->ctx + CTX_COLOR + y * 0x800) + X;
        uint32_t *att_l = (uint32_t *)(P->ctx + CTX_ATTR + y * 0x800) + X;
        uint8_t *id_l = P->ctx + CTX_IDBUF + y * 0x200 + X;
        /* perspective steps: num_i = i*W0, den_i = (W0+dW)*C - i*dW, t = num/den in Q15 */
        int32_t W0 = (int32_t)U32(s, 0x000), dW = (int32_t)U32(s, 0x0b0);
        float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
        float32x4_t num_l = vmulq_n_f32(io_l, fW0), num_h = vmulq_n_f32(io_h, fW0);
        float32x4_t den_l = vfmsq_n_f32(vdupq_n_f32(S), io_l, fD), den_h = vfmsq_n_f32(vdupq_n_f32(S), io_h, fD);
        const float32x4_t E0 = vdupq_n_f32(8.0f * fW0), E1 = vdupq_n_f32(8.0f * fD);
        const uint32_t Rwc = P->recip_u[C];
        /* z: (Z0 << 30 + i * step) >> 30 in u64 */
        uint32_t Z0 = U32(s, 0x160); int32_t dZ = (int32_t)U32(s, 0x210);
        uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
        uint64x2_t zs = vdupq_n_u64(zstep), z8 = vshlq_n_u64(zs, 3);
        uint64x2_t za = vaddq_u64(vdupq_n_u64((uint64_t)Z0 << 30), vcombine_u64(vdup_n_u64(0), vdup_n_u64(zstep)));
        uint64x2_t zb = vaddq_u64(za, vshlq_n_u64(zs, 1)), zc = vaddq_u64(za, vshlq_n_u64(zs, 2)), zd = vaddq_u64(zb, vshlq_n_u64(zs, 2));
        /* vertex colour and uv interpolants */
        uint32_t rg0 = U32(s, 0x420), drg = U32(s, 0x4d0), xb = U32(s, 0x580), cdb = U32(s, 0x630);
        const int32x4_t rb = vdupq_n_s32((int32_t)((rg0 & 0xffff) << 15)), gb = vdupq_n_s32((int32_t)((rg0 >> 16) << 15)), bb = vdupq_n_s32((int32_t)((xb >> 16) << 15));
        uint32_t st0 = U32(s, 0x2c0), dst = U32(s, 0x370);
        const int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
        const int32x4_t ub = vdupq_n_s32((int32_t)(((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0)));
        const int32x4_t vb = vdupq_n_s32((int32_t)(((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0)));
        /* the five interpolation deltas in one register: r, g, b, u, v */
        const int16_t dd[8] = { (int16_t)drg, (int16_t)(drg >> 16), (int16_t)(cdb >> 16), du, dv, 0, 0, 0 };
        const int16x8_t dl = vld1q_s16(dd);
        const int white = TEX && (FLAT ? flat_white : (P->A == 31 && rg0 == 0x01ff01ff && drg == 0 && (xb >> 16) == 0x1ff && (cdb >> 16) == 0));

        for (unsigned i0 = 0; i0 < C; i0 += 8,
             num_l = vaddq_f32(num_l, E0), num_h = vaddq_f32(num_h, E0), den_l = vsubq_f32(den_l, E1), den_h = vsubq_f32(den_h, E1),
             za = vaddq_u64(za, z8), zb = vaddq_u64(zb, z8), zc = vaddq_u64(zc, z8), zd = vaddq_u64(zd, z8)) {
            int16x8_t st;
            if (wconst) {
                uint32x4_t iv_l = vaddq_u32(vld1q_u32(iota8u), vdupq_n_u32(i0)), iv_h = vaddq_u32(vld1q_u32(iota8u + 4), vdupq_n_u32(i0));
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
            /* depth test -> u16 lanes 0 / 0xffff */
            u32x8 da = { vld1q_u32(att_l + i0), vld1q_u32(att_l + i0 + 4) };
            uint16x8_t m16 = equal ? depth_equal(da, dep) : depth_less(da, dep);
            if (i0 + 8 > C) m16 = vandq_u16(m16, vld1q_u16(tail[C - i0]));
            uint8x8_t m8 = vmovn_u16(m16);
            uint64_t mb = vget_lane_u64(vreinterpret_u64_u8(m8), 0);
            if (!mb) { if (edges) vst1_u8(pm + i0, m8); continue; }

            /* colour: (vertex colour x texel) or vertex colour */
            uint8x8_t cr, cg, cb, ca;
            if (TEX) {
                uint16x8_t tu, tv;
                if (WRAP) { tu = vandq_u16(vreinterpretq_u16_s16(LERP_UV(ub, dl, st, 3)), s_and); tv = vandq_u16(vreinterpretq_u16_s16(LERP_UV(vb, dl, st, 4)), t_and); }
                else { tu = texc(LERP_UV(ub, dl, st, 3), s_lo, s_hi, s_and, s_flip); tv = texc(LERP_UV(vb, dl, st, 4), t_lo, t_hi, t_and, t_flip); }
                uint32x4_t al = vmlal_n_u16(vmovl_u16(vget_low_u16(tu)), vget_low_u16(tv), P->tw);
                uint32x4_t ah = vmlal_high_n_u16(vmovl_high_u16(tu), tv, P->tw);
                uint32x4_t t0, t1;
                if (TEX == 2) gather_pal(al, ah, P->idx8, P->pal, &t0, &t1); else gather(al, ah, P->texels32, &t0, &t1);
                uint16x8_t lo = vuzp1q_u16(vreinterpretq_u16_u32(t0), vreinterpretq_u16_u32(t1)), hi = vuzp2q_u16(vreinterpretq_u16_u32(t0), vreinterpretq_u16_u32(t1));
                uint8x8_t tr = vmovn_u16(lo), tg = vshrn_n_u16(lo, 8), tb = vmovn_u16(hi), ta = vshrn_n_u16(hi, 8);
                if (white) { cr = tr; cg = tg; cb = tb; ca = ta; }
                else {
                    uint8x8_t vr, vg, vbb;
                    if (FLAT) { vr = fr; vg = fg; vbb = fb; }
                    else { vr = LERP_C(rb, dl, st, 0); vg = LERP_C(gb, dl, st, 1); vbb = LERP_C(bb, dl, st, 2); }
                    cr = mod6(vr, tr); cg = mod6(vg, tg); cb = mod6(vbb, tb); ca = mod5(vA, ta);
                }
                m8 = vand_u8(m8, vcgt_u8(ca, aref8));
                mb = vget_lane_u64(vreinterpret_u64_u8(m8), 0);
                if (!mb) { if (edges) vst1_u8(pm + i0, m8); continue; }
                m16 = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m8)));
            } else {
                if (FLAT) { cr = fr; cg = fg; cb = fb; }
                else { cr = LERP_C(rb, dl, st, 0); cg = LERP_C(gb, dl, st, 1); cb = LERP_C(bb, dl, st, 2); }
                ca = vA;
            }
            anypass |= mb;
            if (edges) vst1_u8(pm + i0, m8);
            if (!TRANS && rast_stats) __atomic_fetch_add(&st_written, (unsigned long)__builtin_popcountll(mb) / 8, __ATOMIC_RELAXED);
            u32x8 aw = { vorrq_u32(dep.l, pid24), vorrq_u32(dep.h, pid24) };

            if (!TRANS) {
                u32x8 c = pack4(cr, cg, cb, vorr_u8(ca, fog8));
                if (mb == ~0ull) {
                    vst1q_u32(col_l + i0, c.l); vst1q_u32(col_l + i0 + 4, c.h);
                    vst1q_u32(att_l + i0, aw.l); vst1q_u32(att_l + i0 + 4, aw.h);
                } else {
                    u32x8 m = widen(m16);
                    vst1q_u32(col_l + i0, vbslq_u32(m.l, c.l, vld1q_u32(col_l + i0)));
                    vst1q_u32(col_l + i0 + 4, vbslq_u32(m.h, c.h, vld1q_u32(col_l + i0 + 4)));
                    vst1q_u32(att_l + i0, vbslq_u32(m.l, aw.l, vld1q_u32(att_l + i0)));
                    vst1q_u32(att_l + i0 + 4, vbslq_u32(m.h, aw.h, vld1q_u32(att_l + i0 + 4)));
                }
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
        if (edges) {
            /* mark_edges: byte 3 of the attribute := 0x40 on the first EL and the last ER pixels of the line that
             * this polygon wrote (then the id is ORed in again) */
            unsigned EL = U16(s, 0x6e0), ER = U16(s, 0x6e2), p24 = P->pid << 24;
            if (EL > C) EL = C;          /* the polygon's last line gets EL = C + 1: DraStic marks its padding */
            if (ER > C) ER = C;
            for (unsigned i = 0; i < EL; i++) if (pm[i]) att_l[i] = (att_l[i] & 0xffffff) | 0x40000000u | p24;
            for (unsigned i = C - ER; i < C; i++) if (pm[i]) att_l[i] = (att_l[i] & 0xffffff) | 0x40000000u | p24;
        }
        if (TRANS) id0[l] = id_l[0];
    }
    if (anypass) { P->pass = 1; P->fogused |= 1; }
}

#define V(D, T, W, R, F) \
    static void batch_##D##T##W##R##F(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0) { batch_neon(P, bs, k, line, id0, D, T, W, R, F); }
#define V2(D, T, W) V(D, T, W, 0, 0) V(D, T, W, 0, 1) V(D, T, W, 1, 0) V(D, T, W, 1, 1)
#define V3(D) V2(D, 0, 0) V2(D, 1, 0) V2(D, 1, 1) V2(D, 2, 0) V2(D, 2, 1)
V3(0) V3(1) V3(2)
#undef V
#undef V2
#define V(D, T, W, R, F) batch_##D##T##W##R##F,
#define ROW(D, T, W) { { V(D, T, W, 0, 0) V(D, T, W, 0, 1) }, { V(D, T, W, 1, 0) V(D, T, W, 1, 1) } }
static batch_fn *const table[3][5][2][2] = {
    { ROW(0, 0, 0), ROW(0, 1, 0), ROW(0, 1, 1), ROW(0, 2, 0), ROW(0, 2, 1) },
    { ROW(1, 0, 0), ROW(1, 1, 0), ROW(1, 1, 1), ROW(1, 2, 0), ROW(1, 2, 1) },
    { ROW(2, 0, 0), ROW(2, 1, 0), ROW(2, 1, 1), ROW(2, 2, 0), ROW(2, 2, 1) },
};

/* the NEON batch routine for this polygon, or 0 when it needs line_px (shading other than modulate) */
batch_fn *neon_batch_for(const poly_t *P) {
    if (P->mode == 1 || P->mode == 2) return 0;
    unsigned fl = P->flags;
    int D = (fl & 0x10) ? 2 : (fl & 8) ? 1 : 0, T = 0;
    if (fl & 2) T = (P->paletted ? 3 : 1) + (P->s_flip == 0 && P->t_flip == 0 && P->s_and != 0xffff && P->t_and != 0xffff);
    return table[D][T][fl & 1][(fl >> 2) & 1];
}
