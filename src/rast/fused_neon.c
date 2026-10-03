/* fused_neon.c: the fused pixel pipeline (fused.c) in NEON, 8 pixels per step.
 *
 * Same math as fused.c's line_px, lane for lane. Specialised at compile time on the depth source (z, w, constant),
 * texture fetch (none, direct, paletted), translucency and flat colour; shading modes other than modulate
 * (decal, toon, highlight) use line_px. A group of 8 pixels in which no pixel passes the depth test stops right
 * there: no colour, texture or blending work. */
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"

#pragma clang fp contract(off)

#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))

static const uint32_t iota8[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const float iota8f[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* 8 lanes of u32 */
typedef struct { uint32x4_t l, h; } u32x8;

static inline uint8x8_t mask8(u32x8 m) { return vmovn_u16(vcombine_u16(vmovn_u32(m.l), vmovn_u32(m.h))); }
static inline u32x8 widen8(uint8x8_t m) {
    int16x8_t w = vmovl_s8(vreinterpret_s8_u8(m));
    u32x8 r = { vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(w))), vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(w))) };
    return r;
}
/* (u8)((u32)(base + delta * w) >> 18) for 8 lanes */
static inline uint8x8_t lerp_c(uint32_t base, int16_t delta, int16x8_t w) {
    int32x4_t b = vdupq_n_s32((int32_t)base);
    int32x4_t lo = vmlal_n_s16(b, vget_low_s16(w), delta), hi = vmlal_n_s16(b, vget_high_s16(w), delta);
    uint16x8_t x = vcombine_u16(vshrn_n_u32(vreinterpretq_u32_s32(lo), 16), vshrn_n_u32(vreinterpretq_u32_s32(hi), 16));
    return vmovn_u16(vshrq_n_u16(x, 2));
}
/* (s16)((U + d * w) >> 16) >> 3 */
static inline int16x8_t lerp_uv(uint32_t base, int16_t d, int16x8_t w) {
    int32x4_t b = vdupq_n_s32((int32_t)base);
    int32x4_t lo = vmlal_n_s16(b, vget_low_s16(w), d), hi = vmlal_n_s16(b, vget_high_s16(w), d);
    return vshrq_n_s16(vcombine_s16(vshrn_n_s32(lo, 16), vshrn_n_s32(hi, 16)), 3);
}
static inline uint16x8_t texc(int16x8_t x, int mode, uint16_t size) {
    uint16x8_t m = vdupq_n_u16((uint16_t)(size - 1)), u = vreinterpretq_u16_s16(x);
    if (mode == 0) return vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(x, vdupq_n_s16(0)), vreinterpretq_s16_u16(m)));
    if (mode == 1) return vandq_u16(u, m);
    uint16x8_t f = vtstq_u16(u, vdupq_n_u16(size & (uint16_t)~(size - 1)));
    return vandq_u16(veorq_u16(u, f), m);
}
/* ((a+1)*(b+1)-1) >> sh, & 0xff, on bytes */
static inline uint8x8_t mod8(uint8x8_t a, uint8x8_t b, int sh) {
    uint16x8_t x = vaddq_u16(vmull_u8(a, b), vaddl_u8(a, b));
    return sh == 6 ? vshrn_n_u16(x, 6) : vshrn_n_u16(x, 5);
}
static inline u32x8 pack4(uint8x8_t r, uint8x8_t g, uint8x8_t b, uint8x8_t a) {
    uint16x8_t rg = vorrq_u16(vmovl_u8(r), vshll_n_u8(g, 8)), ba = vorrq_u16(vmovl_u8(b), vshll_n_u8(a, 8));
    u32x8 o = { vreinterpretq_u32_u16(vzip1q_u16(rg, ba)), vreinterpretq_u32_u16(vzip2q_u16(rg, ba)) };
    return o;
}

static inline __attribute__((always_inline))
void line_neon(poly_t *P, uint8_t *s, const uint8_t *bs, unsigned y, const int DEPTH, const int TEX, const int TRANS,
               const int FLAT) {
    unsigned X = U16(s, 0x580), C = U16(s, 0x630);
    uint32_t *col_l = (uint32_t *)(P->ctx + CTX_COLOR + y * 0x800) + X;
    uint32_t *att_l = (uint32_t *)(P->ctx + CTX_ATTR + y * 0x800) + X;
    uint8_t *id_l = P->ctx + CTX_IDBUF + y * 0x200 + X;
    uint32_t fl = P->flags;
    const int need_steps = !FLAT || TEX || DEPTH == 1;
    int32_t W0 = (int32_t)U32(s, 0x000), dW = (int32_t)U32(s, 0x0b0);
    uint32_t Z0 = U32(s, 0x160); int32_t dZ = (int32_t)U32(s, 0x210);
    uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
    uint32_t Rwc = P->recip_u[C];
    float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)(uint32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
    float32x4_t io_l = vld1q_f32(iota8f), io_h = vld1q_f32(iota8f + 4);
    float32x4_t num_l = vmulq_n_f32(io_l, fW0), num_h = vmulq_n_f32(io_h, fW0);
    float32x4_t den_l = vfmsq_n_f32(vdupq_n_f32(S), io_l, fD), den_h = vfmsq_n_f32(vdupq_n_f32(S), io_h, fD);
    float32x4_t E0 = vdupq_n_f32(8.0f * fW0), E1 = vdupq_n_f32(8.0f * fD);
    uint64x2_t z0 = vdupq_n_u64(((uint64_t)Z0 << 30)), zs = vdupq_n_u64(zstep);
    uint64x2_t za = vaddq_u64(z0, vcombine_u64(vdup_n_u64(0), vdup_n_u64(zstep)));
    uint64x2_t zb = vaddq_u64(za, vshlq_n_u64(zs, 1)), zc = vaddq_u64(za, vshlq_n_u64(zs, 2)), zd = vaddq_u64(zb, vshlq_n_u64(zs, 2));
    uint64x2_t z8 = vshlq_n_u64(zs, 3);
    uint32x4_t iv_l = vld1q_u32(iota8), iv_h = vld1q_u32(iota8 + 4), eight = vdupq_n_u32(8);
    uint32x4_t vC = vdupq_n_u32(C), vEL = vdupq_n_u32(U16(s, 0x6e0)), vER = vdupq_n_u32(C - U16(s, 0x6e2));
    uint32_t rg0 = U32(s, 0x420), drg = U32(s, 0x4d0), xb = U32(s, 0x580), cdb = U32(s, 0x630);
    uint32_t st0 = U32(s, 0x2c0), dst = U32(s, 0x370);
    int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
    uint32_t U0 = ((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0);
    uint32_t V0 = ((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0);
    uint8x8_t fr = vdup_n_u8((uint8_t)(U16(bs, 0x420) >> 3)), fg = vdup_n_u8((uint8_t)(U16(bs, 0x422) >> 3)), fb = vdup_n_u8((uint8_t)(U16(bs, 0x582) >> 3));
    uint32x4_t vK = vdupq_n_u32(P->K), m24 = vdupq_n_u32(0xffffff), pid24 = vdupq_n_u32(P->pid << 24);
    uint8x8_t vA = vdup_n_u8((uint8_t)P->A), aref8 = vdup_n_u8((uint8_t)P->aref), pid8 = vdup_n_u8((uint8_t)P->pid);
    const int equal = (P->attr >> 14) & 1, fog = (P->attr >> 15) & 1, edges = (P->d3 >> 5) & 1, wconst = (fl >> 5) & 1;
    const int blend = (P->d3 >> 3) & 1, var = ((P->attr >> 11) & 1) | ((P->attr >> 14) & 2);

    for (unsigned i0 = 0; i0 < C; i0 += 8) {
        if (i0) {
            num_l = vaddq_f32(num_l, E0); num_h = vaddq_f32(num_h, E0);
            den_l = vsubq_f32(den_l, E1); den_h = vsubq_f32(den_h, E1);
            za = vaddq_u64(za, z8); zb = vaddq_u64(zb, z8); zc = vaddq_u64(zc, z8); zd = vaddq_u64(zd, z8);
            iv_l = vaddq_u32(iv_l, eight); iv_h = vaddq_u32(iv_h, eight);
        }
        int16x8_t st = vdupq_n_s16(0);
        if (need_steps) {
            if (wconst) {
                st = vreinterpretq_s16_u16(vcombine_u16(vshrn_n_u32(vmulq_n_u32(iv_l, Rwc), 16), vshrn_n_u32(vmulq_n_u32(iv_h, Rwc), 16)));
            } else {
                float32x4_t rl = vrecpeq_f32(den_l), rh = vrecpeq_f32(den_h);
                rl = vmulq_f32(rl, vrecpsq_f32(rl, den_l)); rh = vmulq_f32(rh, vrecpsq_f32(rh, den_h));
                rl = vmulq_f32(rl, vrecpsq_f32(rl, den_l)); rh = vmulq_f32(rh, vrecpsq_f32(rh, den_h));
                int32x4_t cl = vcvtq_n_s32_f32(vmulq_f32(num_l, rl), 15), ch = vcvtq_n_s32_f32(vmulq_f32(num_h, rh), 15);
                st = vcombine_s16(vmovn_s32(cl), vmovn_s32(ch));
            }
        }
        u32x8 dep;
        if (DEPTH == 2) { dep.l = vK; dep.h = vK; }
        else if (DEPTH == 1) {
            int32x4_t sl = vmovl_s16(vget_low_s16(st)), sh = vmovl_s16(vget_high_s16(st));
            int32x2_t w2 = vdup_n_s32(dW);
            int32x4_t a = vcombine_s32(vshrn_n_s64(vmull_s32(w2, vget_low_s32(sl)), 15), vshrn_n_s64(vmull_high_s32(vdupq_n_s32(dW), sl), 15));
            int32x4_t b = vcombine_s32(vshrn_n_s64(vmull_s32(w2, vget_low_s32(sh)), 15), vshrn_n_s64(vmull_high_s32(vdupq_n_s32(dW), sh), 15));
            dep.l = vaddq_u32(vreinterpretq_u32_s32(a), vdupq_n_u32((uint32_t)W0));
            dep.h = vaddq_u32(vreinterpretq_u32_s32(b), vdupq_n_u32((uint32_t)W0));
        } else {
            dep.l = vcombine_u32(vshrn_n_u64(za, 30), vshrn_n_u64(zb, 30));
            dep.h = vcombine_u32(vshrn_n_u64(zc, 30), vshrn_n_u64(zd, 30));
        }
        u32x8 da = { vld1q_u32(att_l + i0), vld1q_u32(att_l + i0 + 4) }, m;
        if (equal) {
            uint32x4_t dl = vreinterpretq_u32_s32(vabsq_s32(vreinterpretq_s32_u32(vsubq_u32(dep.l, vandq_u32(da.l, m24)))));
            uint32x4_t dh = vreinterpretq_u32_s32(vabsq_s32(vreinterpretq_s32_u32(vsubq_u32(dep.h, vandq_u32(da.h, m24)))));
            m.l = vcltq_u32(dl, vdupq_n_u32(0x100)); m.h = vcltq_u32(dh, vdupq_n_u32(0x100));
        } else { m.l = vcgtq_u32(vandq_u32(da.l, m24), dep.l); m.h = vcgtq_u32(vandq_u32(da.h, m24), dep.h); }
        m.l = vandq_u32(m.l, vcltq_u32(iv_l, vC)); m.h = vandq_u32(m.h, vcltq_u32(iv_h, vC));
        uint8x8_t m8 = mask8(m);
        if (!vget_lane_u64(vreinterpret_u64_u8(m8), 0)) continue;

        uint8x8_t vr, vg, vb;
        if (FLAT) { vr = fr; vg = fg; vb = fb; }
        else {
            vr = lerp_c((rg0 & 0xffff) << 15, (int16_t)drg, st);
            vg = lerp_c((rg0 >> 16) << 15, (int16_t)(drg >> 16), st);
            vb = lerp_c((xb >> 16) << 15, (int16_t)(cdb >> 16), st);
        }
        u32x8 c;
        if (TEX) {
            uint16x8_t k = vmovl_u8(m8);
            k = vorrq_u16(k, vshlq_n_u16(k, 8));
            uint16x8_t tu = vandq_u16(texc(lerp_uv(U0, du, st), P->ms, P->tw), k);
            uint16x8_t tv = vandq_u16(texc(lerp_uv(V0, dv, st), P->mt, P->th), k);
            uint32x4_t al = vmlal_n_u16(vmovl_u16(vget_low_u16(tu)), vget_low_u16(tv), P->tw);
            uint32x4_t ah = vmlal_n_u16(vmovl_u16(vget_high_u16(tu)), vget_high_u16(tv), P->tw);
            uint32_t a[8], t[8];
            vst1q_u32(a, al); vst1q_u32(a + 4, ah);
            if (TEX == 2) for (int l = 0; l < 8; l++) t[l] = P->pal[P->idx8[a[l]]];
            else for (int l = 0; l < 8; l++) t[l] = P->texels32[a[l]];
            uint16x8_t t0 = vreinterpretq_u16_u32(vld1q_u32(t)), t1 = vreinterpretq_u16_u32(vld1q_u32(t + 4));
            uint16x8_t lo = vuzp1q_u16(t0, t1), hi = vuzp2q_u16(t0, t1);
            uint8x8_t tr = vmovn_u16(lo), tg = vshrn_n_u16(lo, 8), tb = vmovn_u16(hi), ta = vshrn_n_u16(hi, 8);
            uint8x8_t oa = mod8(vA, ta, 5);
            c = pack4(mod8(vr, tr, 6), mod8(vg, tg, 6), mod8(vb, tb, 6), oa);
            m8 = vand_u8(m8, vcgt_u8(oa, aref8));
            if (!vget_lane_u64(vreinterpret_u64_u8(m8), 0)) continue;
            m = widen8(m8);
        } else c = pack4(vr, vg, vb, vA);
        P->fogused |= 1; P->pass = 1;

        u32x8 aw = { vorrq_u32(dep.l, pid24), vorrq_u32(dep.h, pid24) };
        int full = i0 + 8 <= C;
        if (!TRANS) {
            if (fog) { c.l = vorrq_u32(c.l, vdupq_n_u32(0x80000000u)); c.h = vorrq_u32(c.h, vdupq_n_u32(0x80000000u)); }
            if (edges) {
                uint32x4_t el = vorrq_u32(vcltq_u32(iv_l, vEL), vcgeq_u32(iv_l, vER)), eh = vorrq_u32(vcltq_u32(iv_h, vEL), vcgeq_u32(iv_h, vER));
                uint32x4_t ml = vorrq_u32(vandq_u32(dep.l, m24), vdupq_n_u32(0x40000000u)), mh = vorrq_u32(vandq_u32(dep.h, m24), vdupq_n_u32(0x40000000u));
                aw.l = vorrq_u32(vbslq_u32(el, ml, dep.l), pid24); aw.h = vorrq_u32(vbslq_u32(eh, mh, dep.h), pid24);
            }
            if (full) {
                uint32x4_t cl = vld1q_u32(col_l + i0), chh = vld1q_u32(col_l + i0 + 4);
                vst1q_u32(col_l + i0, vbslq_u32(m.l, c.l, cl)); vst1q_u32(col_l + i0 + 4, vbslq_u32(m.h, c.h, chh));
                vst1q_u32(att_l + i0, vbslq_u32(m.l, aw.l, da.l)); vst1q_u32(att_l + i0 + 4, vbslq_u32(m.h, aw.h, da.h));
            } else {
                uint32_t cc[8], ww[8]; uint8_t mm[8];
                vst1q_u32(cc, c.l); vst1q_u32(cc + 4, c.h); vst1q_u32(ww, aw.l); vst1q_u32(ww + 4, aw.h); vst1_u8(mm, m8);
                for (unsigned l = 0; l < 8; l++) if (mm[l]) { col_l[i0 + l] = cc[l]; att_l[i0 + l] = ww[l]; }
            }
        } else {
            u32x8 dc = { vld1q_u32(col_l + i0), vld1q_u32(col_l + i0 + 4) };
            uint8x8_t did = vld1_u8(id_l + i0);
            uint16x8_t c0 = vreinterpretq_u16_u32(c.l), c1 = vreinterpretq_u16_u32(c.h), d0 = vreinterpretq_u16_u32(dc.l), d1 = vreinterpretq_u16_u32(dc.h);
            uint16x8_t clo = vuzp1q_u16(c0, c1), chi = vuzp2q_u16(c0, c1), dlo = vuzp1q_u16(d0, d1), dhi = vuzp2q_u16(d0, d1);
            uint8x8_t sa = vshrn_n_u16(chi, 8), dal = vand_u8(vshrn_n_u16(dhi, 8), vdup_n_u8(0x1f));
            uint8x8_t cr = vmovn_u16(clo), cg = vshrn_n_u16(clo, 8), cb = vmovn_u16(chi);
            if (blend) {
                uint8x8_t dz = vceq_u8(dal, vdup_n_u8(0));
                uint8x8_t ws = vbsl_u8(dz, vdup_n_u8(0x1f), sa), wd = vbic_u8(vsub_u8(vdup_n_u8(0x1f), sa), dz);
                uint8x8_t dr = vmovn_u16(dlo), dg = vshrn_n_u16(dlo, 8), db = vmovn_u16(dhi);
                cr = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cr), cr, ws), dr, wd), 5);
                cg = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cg), cg, ws), dg, wd), 5);
                cb = vshrn_n_u16(vmlal_u8(vmlal_u8(vmovl_u8(cb), cb, ws), db, wd), 5);
            }
            c = pack4(cr, cg, cb, vmax_u8(sa, dal));
            uint8x8_t op = vceq_u8(sa, vdup_n_u8(0x1f));
            m8 = vbic_u8(m8, vbic_u8(vceq_u8(did, pid8), op));
            uint8x8_t o8 = vand_u8(m8, op), t8 = vbic_u8(m8, op);
            u32x8 M = widen8(m8), O = widen8(o8), cm = M;
            if (var & 2) {
                c.l = vorrq_u32(c.l, vdupq_n_u32(0x80000000u)); c.h = vorrq_u32(c.h, vdupq_n_u32(0x80000000u));
                uint16x8_t f16 = vmovl_u8(vand_u8(m8, vorr_u8(op, vdup_n_u8(0x7f))));
                u32x8 F = { vshlq_n_u32(vmovl_u16(vget_low_u16(f16)), 24), vshlq_n_u32(vmovl_u16(vget_high_u16(f16)), 24) };
                cm.l = vbslq_u32(m24, M.l, F.l); cm.h = vbslq_u32(m24, M.h, F.h);
            }
            u32x8 am = O;
            if (var & 1) { am.l = vbslq_u32(m24, M.l, O.l); am.h = vbslq_u32(m24, M.h, O.h); }
            u32x8 nc = { vbslq_u32(cm.l, c.l, dc.l), vbslq_u32(cm.h, c.h, dc.h) };
            u32x8 na = { vbslq_u32(am.l, aw.l, da.l), vbslq_u32(am.h, aw.h, da.h) };
            uint8x8_t ni = vbsl_u8(t8, pid8, did);
            if (full) {
                vst1q_u32(col_l + i0, nc.l); vst1q_u32(col_l + i0 + 4, nc.h);
                vst1q_u32(att_l + i0, na.l); vst1q_u32(att_l + i0 + 4, na.h);
                vst1_u8(id_l + i0, ni);
            } else {
                uint32_t cc[8], ww[8]; uint8_t ii[8];
                vst1q_u32(cc, nc.l); vst1q_u32(cc + 4, nc.h); vst1q_u32(ww, na.l); vst1q_u32(ww + 4, na.h); vst1_u8(ii, ni);
                for (unsigned l = 0; l < C - i0; l++) { col_l[i0 + l] = cc[l]; att_l[i0 + l] = ww[l]; id_l[i0 + l] = ii[l]; }
            }
        }
    }
}

#define V(D, T, R, F) static void line_##D##T##R##F(poly_t *P, uint8_t *s, const uint8_t *bs, unsigned y) { line_neon(P, s, bs, y, D, T, R, F); }
#define V2(D, T, R) V(D, T, R, 0) V(D, T, R, 1)
#define V3(D, T) V2(D, T, 0) V2(D, T, 1)
#define V4(D) V3(D, 0) V3(D, 1) V3(D, 2)
V4(0) V4(1) V4(2)
#undef V
#define V(D, T, R, F) line_##D##T##R##F,
#define V2(D, T, R) V(D, T, R, 0) V(D, T, R, 1)
static line_fn *const table[3][3][2][2] = {
    { { { V(0,0,0,0) V(0,0,0,1) }, { V(0,0,1,0) V(0,0,1,1) } }, { { V(0,1,0,0) V(0,1,0,1) }, { V(0,1,1,0) V(0,1,1,1) } }, { { V(0,2,0,0) V(0,2,0,1) }, { V(0,2,1,0) V(0,2,1,1) } } },
    { { { V(1,0,0,0) V(1,0,0,1) }, { V(1,0,1,0) V(1,0,1,1) } }, { { V(1,1,0,0) V(1,1,0,1) }, { V(1,1,1,0) V(1,1,1,1) } }, { { V(1,2,0,0) V(1,2,0,1) }, { V(1,2,1,0) V(1,2,1,1) } } },
    { { { V(2,0,0,0) V(2,0,0,1) }, { V(2,0,1,0) V(2,0,1,1) } }, { { V(2,1,0,0) V(2,1,0,1) }, { V(2,1,1,0) V(2,1,1,1) } }, { { V(2,2,0,0) V(2,2,0,1) }, { V(2,2,1,0) V(2,2,1,1) } } },
};

/* the NEON line routine for this polygon, or 0 when it needs line_px (shading other than modulate) */
line_fn *neon_line_for(const poly_t *P) {
    if (P->mode == 1 || P->mode == 2) return 0;
    unsigned fl = P->flags;
    int D = (fl & 0x10) ? 2 : (fl & 8) ? 1 : 0, T = (fl & 2) ? (P->paletted ? 2 : 1) : 0;
    return table[D][T][fl & 1][(fl >> 2) & 1];
}
