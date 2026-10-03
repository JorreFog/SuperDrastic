/* fused.c: the polygon pixel pipeline as one pass per pixel (replaces render_polygon_setup_4x).
 *
 * DraStic runs each batch of up to 512 pixels through ~15 stages, each a loop over the batch with its own scratch
 * arrays. Here every pixel goes through all stages at once, in registers, and pixels that fail the depth test skip
 * the rest. The per-pixel math is exactly the stages' (spec/); b0.c is the stage-by-stage reference.
 *
 * Batch structure still matters in a few places and is kept: flat colour comes from the batch's first line, and a
 * quirk in writeback_alpha_asm_4x stores, for a line whose pixel count is 5 mod 8, the next packed pixel's
 * translucent id into the line's last pixel (tail_id). Shadow polygons (mode 3) go through b0. */
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
#include "fused.h"

static inline uint16_t texcoord(uint16_t x, int mode, uint16_t size) {
    uint16_t m = (uint16_t)(size - 1);
    switch (mode) {
    case CLAMP: { int16_t s = (int16_t)x; if (s < 0) s = 0; if (s > (int16_t)m) s = (int16_t)m; return (uint16_t)s; }
    case WRAP: return x & m;
    default: { uint16_t f = size & (uint16_t)~m; if (x & f) x = (uint16_t)~x; return x & m; }
    }
}
static inline uint32_t sel(uint32_t x, uint32_t y, uint32_t k) { return (x & ~k) | (y & k); }


int use_neon = 2;          /* RAST_PIPE: 1 scalar reference lines, 2 C NEON batches, 3 assembly kernels */

/* one line of a batch: s = this line's span entry, bs = the batch's first line's entry, y = line in the bin.
 * id0_out: the new translucent id of the line's first pixel (for the quirk, see f_setup_4x) */
#define PP P
static void line_px(poly_t *P, uint8_t *s, const uint8_t *bs, unsigned y, uint8_t *id0_out) {
    unsigned X = U16(s, SPO(PP, 8)), C = U16(s, SPO(PP, 9));
    uint32_t *col_l = (uint32_t *)(P->ctx + y * P->lstride) + X;
    uint32_t *att_l = (uint32_t *)(P->ctx + P->attr_off + y * P->lstride) + X;
    uint8_t *id_l = P->ctx + P->id_off + y * P->id_stride + X;
    uint32_t fl = P->flags;
    int32_t W0 = (int32_t)U32(s, SPO(PP, 0)), dW = (int32_t)U32(s, SPO(PP, 1));
    uint32_t Z0 = U32(s, SPO(PP, 2)); int32_t dZ = (int32_t)U32(s, SPO(PP, 3));
    uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
    uint32_t Rwc = P->recip_u[C];
    float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)(uint32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
    float E0 = 8.0f * fW0, E1 = 8.0f * fD, num[8], den[8];
    for (int j = 0; j < 8; j++) { num[j] = (float)j * fW0; den[j] = fmaf(-(float)j, fD, S); }
    uint32_t rg0 = U32(s, SPO(PP, 6)), drg = U32(s, SPO(PP, 7)), xb = U32(s, SPO(PP, 8)), cdb = U32(s, SPO(PP, 9));
    uint32_t st0 = U32(s, SPO(PP, 4)), dst = U32(s, SPO(PP, 5));
    int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
    uint32_t U0 = ((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0);
    uint32_t V0 = ((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0);
    unsigned EL = U16(s, SPO(PP, 10)), ER = U16(s, SPO(PP, 10) + 2);
    uint8_t fr = (uint8_t)(U16(bs, SPO(PP, 6)) >> 3), fg = (uint8_t)(U16(bs, SPO(PP, 6) + 2) >> 3), fb = (uint8_t)(U16(bs, SPO(PP, 8) + 2) >> 3);

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

/* The translucent id DraStic's stages compute for pixel index i of a line (i may be C, one past the end: the
 * stages run whole vectors, so that lane holds the line's continuation against the next pixel's buffer state).
 * writeback_alpha_asm_4x stores lane C's id as the last pixel's id of a batch-ending line with C % 8 == 5. */
static uint8_t tail_id(poly_t *P, const uint8_t *s, const uint8_t *bs, unsigned y, unsigned i) {
    unsigned X = U16(s, SPO(PP, 8)), C = U16(s, SPO(PP, 9));
    const uint32_t *col_l = (const uint32_t *)(P->ctx + y * P->lstride) + X;
    const uint32_t *att_l = (const uint32_t *)(P->ctx + P->attr_off + y * P->lstride) + X;
    const uint8_t *id_l = P->ctx + P->id_off + y * P->id_stride + X;
    uint32_t fl = P->flags;
    int32_t W0 = (int32_t)U32(s, SPO(PP, 0)), dW = (int32_t)U32(s, SPO(PP, 1));
    uint32_t Z0 = U32(s, SPO(PP, 2)); int32_t dZ = (int32_t)U32(s, SPO(PP, 3));
    uint64_t zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
    float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)(uint32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
    float num = (float)(i & 7) * fW0, den = fmaf(-(float)(i & 7), fD, S);
    for (unsigned g = 0; g < i / 8; g++) { num = num + 8.0f * fW0; den = den - 8.0f * fD; }
    int16_t st;
    if (fl & 0x20) st = (int16_t)((uint32_t)(i * P->recip_u[C]) >> 16);
    else { float r = vrecpes_f32(den); r = r * vrecpss_f32(r, den); r = r * vrecpss_f32(r, den); st = (int16_t)vcvts_n_s32_f32(num * r, 15); }
    uint32_t dep;
    if (fl & 0x10) dep = P->K;
    else if (fl & 8) dep = (uint32_t)W0 + (uint32_t)(((int64_t)dW * st) >> 15);
    else dep = (uint32_t)((((uint64_t)Z0 << 30) + (uint64_t)i * zstep) >> 30);
    uint32_t da = att_l[i], m;
    if (P->attr & (1u << 14)) { uint32_t d = dep - (da & 0xffffff); if ((int32_t)d < 0) d = 0u - d; m = d < 0x100; }
    else m = (da & 0xffffff) > dep;
    uint32_t sa = P->A;
    if (fl & 2) {
        uint32_t st0 = U32(s, SPO(PP, 4)), dst = U32(s, SPO(PP, 5));
        int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
        uint32_t U0 = ((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0);
        uint32_t V0 = ((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0);
        uint16_t u = (uint16_t)((int16_t)((U0 + (uint32_t)(du * st)) >> 16) >> 3);
        uint16_t v = (uint16_t)((int16_t)((V0 + (uint32_t)(dv * st)) >> 16) >> 3);
        uint16_t k = m ? 0xffff : 0;
        uint32_t a = (uint32_t)(texcoord(u, P->ms, P->tw) & k) + (uint32_t)(texcoord(v, P->mt, P->th) & k) * P->tw;
        uint32_t t = P->paletted ? P->pal[P->idx8[a]] : P->texels32[a], ta = t >> 24;
        if (P->mode == 1) sa = P->A;
        else if (P->mode == 2 && (P->d3 & 2)) sa = (((P->A * ta + P->A + ta) & 0xffff) >> 5) & 0x1f;
        else sa = (((P->A * ta + P->A + ta) & 0xffff) >> 5) & 0xff;
        if (!(sa > (P->aref & 0xff))) m = 0;
    }
    (void)col_l; (void)bs;
    uint8_t did = id_l[i];
    if (did == (uint8_t)P->pid && (uint8_t)sa != 0x1f) m = 0;
    return (m && (uint8_t)sa != 0x1f) ? (uint8_t)P->pid : did;
}

void b0_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                 unsigned flags, uint8_t *v0);
void b0_flush(uint8_t *ctx, uint8_t *spans, uint8_t *poly, unsigned line0, unsigned nlines, uint8_t *buf,
              unsigned n, unsigned flags, uint8_t *v0);

/* ---- deferred shading: can a texture's alpha test fail? ----
 * The lowest texel alpha of each texture used, found once per frame per render thread (the texture cache entries
 * are stable while a frame renders): the texels of direct and compressed textures, the palette entries of paletted
 * ones (spec/texture.c: formats 2/3/4 have 4/16/256 entries, A3I5 and A5I3 256). */
typedef struct { const uint8_t *tex; uint32_t gen; uint8_t min_a; } texinfo_t;
#define TEXINFO_N 1024
static __thread texinfo_t texinfo[TEXINFO_N];
static __thread uint32_t tex_gen = 1;
void f_begin_frame(void) { tex_gen++; }

static unsigned tex_min_alpha_scan(const poly_t *P) {
    unsigned m = 255;
    if (P->paletted) {
        unsigned fmt = P->tex[0x49], np = fmt == 2 ? 4 : fmt == 3 ? 16 : 256;
        for (unsigned i = 0; i < np; i++) { unsigned a = P->pal[i] >> 24; if (a < m) m = a; }
    } else {
        unsigned n = (unsigned)P->tw * P->th;
        const uint8_t *b = (const uint8_t *)P->texels32;
        uint8x16_t acc = vdupq_n_u8(255);
        unsigned i = 0;
        for (; i + 16 <= n; i += 16) acc = vminq_u8(acc, vld4q_u8(b + 4 * i).val[3]);
        m = vminvq_u8(acc);
        for (; i < n; i++) if (b[4 * i + 3] < m) m = b[4 * i + 3];
    }
    return m;
}

static unsigned tex_min_alpha(const poly_t *P) {
    unsigned h = (unsigned)((uintptr_t)P->tex >> 4) & (TEXINFO_N - 1);
    for (unsigned n = 0; n < TEXINFO_N; n++, h = (h + 1) & (TEXINFO_N - 1)) {
        texinfo_t *t = &texinfo[h];
        if (t->gen == tex_gen && t->tex == P->tex) return t->min_a;
        if (t->gen != tex_gen) {
            t->tex = P->tex; t->gen = tex_gen; t->min_a = (uint8_t)tex_min_alpha_scan(P);
            return t->min_a;
        }
    }
    return tex_min_alpha_scan(P);
}

#undef PP
#define PP (&P)
const layout_t layout_2x = { 0xb0, 0x800, CTX_ATTR, CTX_IDBUF, 0x200, 0x400, 512, 32, 0, CTX_SYS };

void f_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0) {
    f_run(&layout_2x, ctx, spans, poly, buf, line0, nlines, flags, v0, 0, 0);
}

int f_run_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
             unsigned flags, uint8_t *v0, int dmode, unsigned idx) {
    return f_run(&layout_2x, ctx, spans, poly, buf, line0, nlines, flags, v0, dmode, idx);
}

int f_run(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0,
          unsigned nlines, unsigned flags, uint8_t *v0, int dmode, unsigned idx) {
    poly_t P;
    P.sps = L->sps; P.lstride = L->lstride; P.attr_off = L->attr_off; P.id_off = L->id_off;
    P.id_stride = L->id_stride; P.owner_stride = L->owner_stride; P.hr = L->hr;
    P.ctx = ctx; P.sys = PTR(ctx, L->hdr_off); P.geom = PTR(ctx, L->hdr_off + 8); P.poly = poly; P.v0 = v0;
    P.attr = U32(poly, 4); P.mode = (P.attr >> 4) & 3;
    if (P.mode == 3) {
        if (L->hr) return 0;                                /* hr.c: shadow polygons are not rendered yet */
        b0_setup_4x(ctx, spans, poly, buf, line0, nlines, flags, v0); return 1;
    }
    P.pid = (P.attr >> 24) & 63; P.A = (P.attr >> 16) & 31; P.flags = flags;
    P.d3 = U32(P.sys, SYS_DISP3DCNT); P.aref = U32(P.sys, 0x34eb44);
    P.dmode = dmode; P.owner = dmode ? defer_owner() : 0; P.idx = idx;
    if (!(flags & 2) && P.A <= P.aref) return 0;
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
    P.fogused = 0; P.pass = 0;
    if (dmode == 1 && (flags & 2)) {
        /* the visibility pass needs the texture only when its alpha test can fail: ca = modulate(A, ta) <= aref */
        unsigned ta = tex_min_alpha(&P), ca = ((P.A + 1) * (ta + 1) - 1) >> 5;
        if (ca > P.aref) P.flags &= ~2u;
    }
    if (flags & 2) {
        /* texcoord() as clamp-and-mask (fused_neon.c): clamp [0, W-1]; wrap: & (W-1); flip: invert where x & W */
        P.s_lo = P.ms == CLAMP ? 0 : -32768; P.s_hi = P.ms == CLAMP ? (int16_t)(P.tw - 1) : 32767;
        P.s_and = P.ms == CLAMP ? 0xffff : (uint16_t)(P.tw - 1); P.s_flip = P.ms == FLIP ? (uint16_t)(P.tw & ~(P.tw - 1)) : 0;
        P.t_lo = P.mt == CLAMP ? 0 : -32768; P.t_hi = P.mt == CLAMP ? (int16_t)(P.th - 1) : 32767;
        P.t_and = P.mt == CLAMP ? 0xffff : (uint16_t)(P.th - 1); P.t_flip = P.mt == FLIP ? (uint16_t)(P.th & ~(P.th - 1)) : 0;
    }
    batch_fn *bf = use_neon >= 3 || L->hr ? asm_batch_for(&P) : use_neon ? neon_batch_for(&P) : 0;

    /* batches: runs of lines with pixels, at most 512 pixels (DraStic's batches: the flat colour and the id quirk
     * follow them); the hi-res pipeline, with lines of up to 768 pixels and no exactness to keep, batches longer */
    const unsigned bmax = L->hr ? 4096 : 512;
    unsigned line = line0, left = nlines, i = 0;
    while (left) {
        while (left && !U16(spans, SPO(PP, 9) + 4 * i)) { i++; line++; left--; }
        if (!left) break;
        unsigned first = i, k = 0, n = 0;
        while (left) {
            unsigned c = U16(spans, SPO(PP, 9) + 4 * i);
            if (!c || (k && n + c > bmax)) break;
            n += c; k++; i++; left--;
        }
        uint8_t *bs = spans + 4 * first;
        if (0) {        } else if (flags & 1) {
            /* the quirk: a line with 5 mod 8 pixels gets the next line's first new id as its last id (the batch's
             * last line: the id the stages compute one past its end), but only if the batch is written at all
             * (DraStic skips a batch in which no pixel passes) */
            uint8_t id0[64];                    /* one per batch line (a hi-res bin has 50 lines) */
            P.pass = 0;
            if (bf) bf(&P, bs, k, line, id0);
            else for (unsigned l = 0; l < k; l++) line_px(&P, bs + 4 * l, bs, line + l, &id0[l]);
            if (P.pass)
                for (unsigned l = 0; l < k; l++) {
                    unsigned c = U16(bs, SPO(PP, 9) + 4 * l);
                    if (c % 8 != 5) continue;
                    uint8_t id = l + 1 < k ? id0[l + 1] : tail_id(&P, bs + 4 * l, bs, line + l, c);
                    ctx[P.id_off + (line + l) * P.id_stride + U16(bs, SPO(PP, 8) + 4 * l) + c - 1] = id;
                }
        } else {
            if (bf) bf(&P, bs, k, line, 0);
            else for (unsigned l = 0; l < k; l++) line_px(&P, bs + 4 * l, bs, line + l, 0);
        }
        line += k;
    }
    if ((P.fogused & 1) && (((flags & 1) && (P.attr & (1u << 15))) || (!(flags & 1) && (P.attr & (1u << 15)))))
        U32(ctx, L->hdr_off + 0x14) = 1;
    return P.pass;
}
