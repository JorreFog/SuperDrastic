/* fused.c: the polygon pixel pipeline as one pass per pixel (replaces render_polygon_setup_4x).
 *
 * DraStic runs each batch of up to 512 pixels through ~15 stages, each a loop over the batch with its own scratch
 * arrays. Here every pixel goes through all stages at once, in registers, and pixels that fail the depth test skip
 * the rest. The per-pixel math is exactly the stages' (spec/); b0.c is the stage-by-stage reference.
 *
 * Batch structure still matters in a few places and is kept: flat colour comes from the batch's first line, and a
 * quirk in writeback_alpha_asm_4x stores, for a line whose pixel count is 5 mod 8, the next packed pixel's
 * translucent id into the line's last pixel. When that next pixel is past the batch (the stages' padding lanes),
 * the batch goes through b0 instead. Shadow polygons (mode 3) also go through b0. */
#include <arm_neon.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"

#pragma clang fp contract(off)

#define U8(p, o)  (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)U64(p, o))
#define DS_RECIP   0x3f27120
#define DS_RECIP_U 0x3f28520

enum { CLAMP, WRAP, FLIP };
static inline uint16_t texcoord(uint16_t x, int mode, uint16_t size) {
    uint16_t m = (uint16_t)(size - 1);
    switch (mode) {
    case CLAMP: { int16_t s = (int16_t)x; if (s < 0) s = 0; if (s > (int16_t)m) s = (int16_t)m; return (uint16_t)s; }
    case WRAP: return x & m;
    default: { uint16_t f = size & (uint16_t)~m; if (x & f) x = (uint16_t)~x; return x & m; }
    }
}
static inline uint32_t sel(uint32_t x, uint32_t y, uint32_t k) { return (x & ~k) | (y & k); }

#include "fused.h"

int use_neon = 1;          /* RAST_PIPE=2 / 1: NEON lines / scalar reference lines */

/* one line of a batch: s = this line's span entry, bs = the batch's first line's entry, y = line in the bin.
 * id0_out: the new translucent id of the line's first pixel (for the quirk, see f_setup_4x) */
static void line_px(poly_t *P, uint8_t *s, const uint8_t *bs, unsigned y, uint8_t *id0_out) {
    unsigned X = U16(s, 0x580), C = U16(s, 0x630);
    uint32_t *col_l = (uint32_t *)(P->ctx + CTX_COLOR + y * 0x800) + X;
    uint32_t *att_l = (uint32_t *)(P->ctx + CTX_ATTR + y * 0x800) + X;
    uint8_t *id_l = P->ctx + CTX_IDBUF + y * 0x200 + X;
    uint32_t fl = P->flags;
    int32_t W0 = (int32_t)U32(s, 0x000), dW = (int32_t)U32(s, 0x0b0);
    uint32_t Z0 = U32(s, 0x160); int32_t dZ = (int32_t)U32(s, 0x210);
    uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
    uint32_t Rwc = P->recip_u[C];
    float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)(uint32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
    float E0 = 8.0f * fW0, E1 = 8.0f * fD, num[8], den[8];
    for (int j = 0; j < 8; j++) { num[j] = (float)j * fW0; den[j] = fmaf(-(float)j, fD, S); }
    uint32_t rg0 = U32(s, 0x420), drg = U32(s, 0x4d0), xb = U32(s, 0x580), cdb = U32(s, 0x630);
    uint32_t st0 = U32(s, 0x2c0), dst = U32(s, 0x370);
    int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
    uint32_t U0 = ((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0);
    uint32_t V0 = ((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0);
    unsigned EL = U16(s, 0x6e0), ER = U16(s, 0x6e2);
    uint8_t fr = (uint8_t)(U16(bs, 0x420) >> 3), fg = (uint8_t)(U16(bs, 0x422) >> 3), fb = (uint8_t)(U16(bs, 0x582) >> 3);

    for (unsigned i = 0; i < C; i++) {
        unsigned j = i & 7;
        if (i && !j) for (int k = 0; k < 8; k++) { num[k] = num[k] + E0; den[k] = den[k] - E1; }
        int16_t st;
        if (fl & 0x20) st = (int16_t)((uint32_t)(i * Rwc) >> 16);
        else {
            float r = vrecpes_f32(den[j]);
            r = r * vrecpss_f32(r, den[j]);
            r = r * vrecpss_f32(r, den[j]);
            st = (int16_t)vcvts_n_s32_f32(num[j] * r, 15);
        }
        uint32_t dep;
        if (fl & 0x10) dep = P->K;
        else if (fl & 8) dep = (uint32_t)W0 + (uint32_t)(((int64_t)dW * st) >> 15);
        else dep = (uint32_t)((((uint64_t)Z0 << 30) + (uint64_t)i * zstep) >> 30);
        uint32_t da = att_l[i], pass;
        if (P->attr & (1u << 14)) { uint32_t d = dep - (da & 0xffffff); if ((int32_t)d < 0) d = 0u - d; pass = d < 0x100; }
        else pass = (da & 0xffffff) > dep;
        uint8_t m = pass ? 0xff : 0;
        if (!m && !(fl & 1)) continue;       /* opaque: nothing written for a failed pixel */

        uint32_t vr, vg, vb;
        if (fl & 4) { vr = fr; vg = fg; vb = fb; }
        else {
            vr = (uint8_t)((((rg0 & 0xffff) << 15) + (uint32_t)((int16_t)drg * st)) >> 18);
            vg = (uint8_t)((((rg0 >> 16) << 15) + (uint32_t)((int16_t)(drg >> 16) * st)) >> 18);
            vb = (uint8_t)((((xb >> 16) << 15) + (uint32_t)((int16_t)(cdb >> 16) * st)) >> 18);
        }
        uint32_t c, A = P->A;
        if (fl & 2) {
            uint16_t u = (uint16_t)((int16_t)((U0 + (uint32_t)(du * st)) >> 16) >> 3);
            uint16_t v = (uint16_t)((int16_t)((V0 + (uint32_t)(dv * st)) >> 16) >> 3);
            uint16_t k = m ? 0xffff : 0;
            uint32_t a = (uint32_t)(texcoord(u, P->ms, P->tw) & k) + (uint32_t)(texcoord(v, P->mt, P->th) & k) * P->tw;
            uint32_t t = P->paletted ? P->pal[P->idx8[a]] : P->texels32[a];
            uint32_t tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff, ta = t >> 24;
            if (P->mode == 1) {
                uint32_t a1, w;
                if (ta == 31) { a1 = 32; w = 0; } else { a1 = ta; w = ta ? 31 - ta : 32; }
                c = ((tb * a1 + vb * w) >> 5) << 16 | ((tg * a1 + vg * w) >> 5) << 8 | (((tr * a1 + vr * w) >> 5) | A << 24);
            } else if (P->mode == 2 && (P->d3 & 2)) {
                uint32_t r = ((vr * tr + vr + tr) & 0xffff) >> 6, g = ((vr * tg + vr + tg) & 0xffff) >> 6;
                uint32_t b = ((vr * tb + vr + tb) & 0xffff) >> 6, al = ((A * ta + A + ta) & 0xffff) >> 5;
                uint32_t cc = (r & 0xff) | (g & 0xff) << 8 | (b & 0xff) << 16 | (al & 0xff) << 24, sidx = vr >> 1;
                uint32_t hr = P->toon[sidx] + (cc & 63), hg = P->toon[0x20 + sidx] + ((cc >> 8) & 63), hb = P->toon[0x40 + sidx] + ((cc >> 16) & 63);
                if (hr > 63) hr = 63;
                if (hg > 63) hg = 63;
                if (hb > 63) hb = 63;
                c = hb << 16 | hg << 8 | hr | (cc & 0x1f000000);
            } else {
                if (P->mode == 2) {
                    uint32_t ti = vr >> 1;
                    vr = ti < 32 ? P->toon[ti] : 0; vg = ti < 32 ? P->toon[0x20 + ti] : 0; vb = ti < 32 ? P->toon[0x40 + ti] : 0;
                }
                uint32_t r = ((vr * tr + vr + tr) & 0xffff) >> 6, g = ((vg * tg + vg + tg) & 0xffff) >> 6;
                uint32_t b = ((vb * tb + vb + tb) & 0xffff) >> 6, al = ((A * ta + A + ta) & 0xffff) >> 5;
                c = (r & 0xff) | (g & 0xff) << 8 | (b & 0xff) << 16 | (al & 0xff) << 24;
            }
            if (!((c >> 24) > (P->aref & 0xff))) m = 0;
        } else {
            if (P->mode != 2) c = vr | vg << 8 | vb << 16 | (A & 0xff) << 24;
            else {
                uint32_t sidx = vr >> 1;
                if (P->d3 & 2) {
                    uint32_t hr = P->toon[sidx] + vr, hg = P->toon[0x20 + sidx] + vg, hb = P->toon[0x40 + sidx] + vb;
                    if (hr > 63) hr = 63;
                    if (hg > 63) hg = 63;
                    if (hb > 63) hb = 63;
                    c = (hb << 16 | hg << 8) | (hr | A << 24);
                } else c = ((uint32_t)P->toon[0x40 + sidx] << 16 | (uint32_t)P->toon[0x20 + sidx] << 8) | (P->toon[sidx] | A << 24);
            }
        }
        if (m) P->fogused |= 1, P->pass = 1;

        if (fl & 1) {                                           /* translucent */
            uint32_t dc = col_l[i]; uint8_t did = id_l[i];
            uint8_t sa = (uint8_t)(c >> 24), dal = (uint8_t)(dc >> 24) & 0x1f;
            uint32_t out = c & 0x00ffffff;
            if (P->d3 & 8) {
                uint8_t ws = dal ? sa : 0x1f, wd = dal ? (uint8_t)(0x1f - sa) : 0;
                out = 0;
                for (int ch = 0; ch < 3; ch++) {
                    uint16_t cs = (uint8_t)(c >> (8 * ch)), cd = (uint8_t)(dc >> (8 * ch));
                    out |= (uint32_t)(uint8_t)((uint16_t)(cs + cs * ws + cd * wd) >> 5) << (8 * ch);
                }
            }
            c = out | (uint32_t)(sa > dal ? sa : dal) << 24;
            if (did == (uint8_t)P->pid && sa != 0x1f) m = 0;
            uint8_t op = sa == 0x1f ? 0xff : 0, o = m & op, t = m & (uint8_t)~op;
            uint32_t M = m * 0x01010101u, O = o * 0x01010101u, cm = M, src = c;
            unsigned var = ((P->attr >> 11) & 1) | ((P->attr >> 14) & 2);
            if (var & 2) { src |= 0x80000000u; cm = (M & 0x00ffffffu) | (uint32_t)(m & (op | 0x7f)) << 24; }
            uint32_t nw = dep | P->pid << 24, am = (var & 1) ? ((M & 0x00ffffffu) | (O & 0xff000000u)) : O;
            col_l[i] = sel(dc, src, cm);
            att_l[i] = sel(da, nw, am);
            uint8_t nid = (uint8_t)sel(did, (uint8_t)P->pid, t);
            id_l[i] = nid;
            if (i == 0 && id0_out) *id0_out = nid;
        } else {                                                /* opaque */
            if (!m) continue;
            if (P->attr & (1u << 15)) c |= 0x80000000u;
            if ((P->d3 & 0x20) && (i < EL || i >= C - ER)) dep = (dep & 0x00ffffffu) | 0x40000000u;
            col_l[i] = c;
            att_l[i] = dep | P->pid << 24;
        }
    }
}

void b0_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                 unsigned flags, uint8_t *v0);
void b0_flush(uint8_t *ctx, uint8_t *spans, uint8_t *poly, unsigned line0, unsigned nlines, uint8_t *buf,
              unsigned n, unsigned flags, uint8_t *v0);

void f_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0) {
    poly_t P;
    P.ctx = ctx; P.sys = PTR(ctx, CTX_SYS); P.geom = PTR(ctx, CTX_GEOM); P.poly = poly; P.v0 = v0;
    P.attr = U32(poly, 4); P.mode = (P.attr >> 4) & 3;
    if (P.mode == 3) { b0_setup_4x(ctx, spans, poly, buf, line0, nlines, flags, v0); return; }
    P.pid = (P.attr >> 24) & 63; P.A = (P.attr >> 16) & 31; P.flags = flags;
    P.d3 = U32(P.sys, SYS_DISP3DCNT); P.aref = U32(P.sys, 0x34eb44);
    if (!(flags & 2) && P.A <= P.aref) return;
    P.toon = P.geom + 0x99cc;
    P.recip = (const uint32_t *)(ds_base + DS_RECIP); P.recip_u = (const uint32_t *)(ds_base + DS_RECIP_U);
    P.K = (flags & 8) ? U32(v0, 0) : (uint32_t)U16(v0, 8) << 9;
    if (flags & 2) {
        P.tex = PTR(poly, 0x10);
        uint32_t wm = U16(poly, 2) & 0xf;
        P.ms = !(wm & 1) ? CLAMP : (wm & 4) ? FLIP : WRAP;
        P.mt = !(wm & 2) ? CLAMP : (wm & 8) ? FLIP : WRAP;
        P.tw = U16(P.tex, 0x40); P.th = U16(P.tex, 0x42);
        P.paletted = P.tex[0x4a];
        P.texels32 = (const uint32_t *)PTR(P.tex, 0x10); P.idx8 = PTR(P.tex, 0x10); P.pal = (const uint32_t *)PTR(P.tex, 0x18);
    }
    P.fogused = 0;
    line_fn *nl = use_neon ? neon_line_for(&P) : 0;

    unsigned line = line0, left = nlines, i = 0;
    while (left) {
        while (left && !U16(spans, 0x630 + 4 * i)) { i++; line++; left--; }
        if (!left) break;
        unsigned first = i, k = 0, n = 0;
        while (left) {
            unsigned c = U16(spans, 0x630 + 4 * i);
            if (!c || n + c > 512) break;
            n += c; k++; i++; left--;
        }
        uint8_t *bs = spans + 4 * first;
        if ((flags & 1) && U16(bs, 0x630 + 4 * (k - 1)) % 8 == 5) {
            /* the last line's quirk reads the stages' padding: run this batch stage by stage */
            int fu = U32(ctx, CTX_FOGUSED);
            b0_flush(ctx, bs, poly, line, k, buf, n, flags, v0);
            if (U32(ctx, CTX_FOGUSED) != (uint32_t)fu) P.fogused |= 2;
        } else if (flags & 1) {
            /* the quirk: a line with 5 mod 8 pixels gets the next line's first new id as its last id, but only if
             * the batch is written at all (DraStic skips a batch in which no pixel passes) */
            uint8_t id0[32];
            P.pass = 0;
            for (unsigned l = 0; l < k; l++)
                if (nl) { nl(&P, bs + 4 * l, bs, line + l); id0[l] = ctx[CTX_IDBUF + (line + l) * 0x200 + U16(bs, 0x580 + 4 * l)]; }
                else line_px(&P, bs + 4 * l, bs, line + l, &id0[l]);
            if (P.pass)
                for (unsigned l = 0; l + 1 < k; l++) {
                    unsigned c = U16(bs, 0x630 + 4 * l);
                    if (c % 8 == 5) ctx[CTX_IDBUF + (line + l) * 0x200 + U16(bs, 0x580 + 4 * l) + c - 1] = id0[l + 1];
                }
        } else
            for (unsigned l = 0; l < k; l++)
                if (nl) nl(&P, bs + 4 * l, bs, line + l); else line_px(&P, bs + 4 * l, bs, line + l, 0);
        line += k;
    }
    if ((P.fogused & 1) && (((flags & 1) && (P.attr & (1u << 15))) || (!(flags & 1) && (P.attr & (1u << 15)))))
        U32(ctx, CTX_FOGUSED) = 1;
}
