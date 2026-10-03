/* fused_asm.c: the batches on the generated assembly kernels (kerngen.py -> rast_kern.S). Fills a kargs_t
 * per line with the same per-line values batch_neon computes, calls the kernel, then does the edge-marking fix-up
 * from the pass masks the kernel leaves in pm. The non-modulate shading modes stay on the C paths. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"

#pragma clang fp contract(off)

#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

typedef struct {                /* layout shared with kerngen.py (K dict): the per-polygon constants */
    const uint32_t *recip, *recip_u;
    const void *tex; const uint32_t *pal;
    uint32_t pid24[4];
    uint8_t bytes[8];           /* A aref fog pid fr fg fb */
    uint32_t K; uint16_t tw; uint16_t pad;
    uint16_t s_and[8], t_and[8], s_lo[8], t_lo[8], s_hi[8], t_hi[8], s_flip[8], t_flip[8];
    uint8_t fraclut[16];        /* bilinear: the texel fraction (0..7) -> blend weight (0..8) */
    uint16_t *owner; uint64_t pad2;  /* deferred shading: the owner buffer and the polygon's index, splat */
    uint16_t idx16[8];
    uint32_t lstride, attr_off, id_off, id_stride, owner_stride, pad3[3];   /* the hi-res kernels' buffer layout */
    uint32_t pal16[16];         /* the palette when it has at most 16 entries (flag bit 7): tbl lookups */
} kargs_t;
_Static_assert(sizeof(kargs_t) == 0x150, "kargs_t layout");
_Static_assert(__builtin_offsetof(kargs_t, pid24) == 0x20 && __builtin_offsetof(kargs_t, bytes) == 0x30 &&
               __builtin_offsetof(kargs_t, K) == 0x38 && __builtin_offsetof(kargs_t, tw) == 0x3c &&
               __builtin_offsetof(kargs_t, s_and) == 0x40 && __builtin_offsetof(kargs_t, fraclut) == 0xc0 &&
               __builtin_offsetof(kargs_t, owner) == 0xd0 && __builtin_offsetof(kargs_t, idx16) == 0xe0 &&
               __builtin_offsetof(kargs_t, lstride) == 0xf0 && __builtin_offsetof(kargs_t, owner_stride) == 0x100 &&
               __builtin_offsetof(kargs_t, pal16) == 0x110, "kargs_t layout");

typedef uint64_t kern_fn(const kargs_t *, const uint8_t *bs, uint32_t k, uint32_t line, uint8_t *ctx, uint32_t flags, uint8_t *id0);
/* the kernel tables, for the exact 2x set (rast_kern_) and the hi-res set (rast_kern_h, strides from kargs) */
#define KD(P, D, T, R) extern kern_fn P##D##T##R##00, P##D##T##R##10;
#define KB(P, D, T, R) extern kern_fn P##D##T##R##00, P##D##T##R##10, P##D##T##R##01, P##D##T##R##11;
#define KT(P, D) KD(P, D, 0, 0) KD(P, D, 0, 1) KB(P, D, 1, 0) KB(P, D, 1, 1) KB(P, D, 2, 0) KB(P, D, 2, 1) KB(P, D, 3, 0) KB(P, D, 3, 1) KB(P, D, 4, 0) KB(P, D, 4, 1)
#define KV(P, D) extern kern_fn P##v##D##0, P##v##D##1, P##v##D##2, P##v##D##3, P##v##D##4;
#define KS(P, T) extern kern_fn P##s##T##00, P##s##T##01, P##s##T##10, P##s##T##11;
#define KALL(P) KT(P, 0) KT(P, 1) KT(P, 2) KV(P, 0) KV(P, 1) KV(P, 2) extern kern_fn P##s000, P##s010; KS(P, 1) KS(P, 2) KS(P, 3) KS(P, 4)
KALL(rast_kern_) KALL(rast_kern_h)
/* [D][T][R][F][B]: B (bilinear) exists for textured variants only */
#define KR0(P, D, T) { { { &P##D##T##000, &P##D##T##000 }, { &P##D##T##010, &P##D##T##010 } }, \
                       { { &P##D##T##100, &P##D##T##100 }, { &P##D##T##110, &P##D##T##110 } } }
#define KR(P, D, T) { { { &P##D##T##000, &P##D##T##001 }, { &P##D##T##010, &P##D##T##011 } }, \
                      { { &P##D##T##100, &P##D##T##101 }, { &P##D##T##110, &P##D##T##111 } } }
#define KVR(P, D) { &P##v##D##0, &P##v##D##1, &P##v##D##2, &P##v##D##3, &P##v##D##4 }
#define KSR(P, T) { { &P##s##T##00, &P##s##T##01 }, { &P##s##T##10, &P##s##T##11 } }
typedef struct {
    kern_fn *const kernels[3][5][2][2][2];
    kern_fn *const vis[3][5];               /* visibility kernels [D][T] */
    kern_fn *const shade[5][2][2];          /* shade kernels [T][F][B] */
} kset_t;
#define KSET(P) { \
    { { KR0(P, 0, 0), KR(P, 0, 1), KR(P, 0, 2), KR(P, 0, 3), KR(P, 0, 4) }, \
      { KR0(P, 1, 0), KR(P, 1, 1), KR(P, 1, 2), KR(P, 1, 3), KR(P, 1, 4) }, \
      { KR0(P, 2, 0), KR(P, 2, 1), KR(P, 2, 2), KR(P, 2, 3), KR(P, 2, 4) } }, \
    { KVR(P, 0), KVR(P, 1), KVR(P, 2) }, \
    { { { &P##s000, &P##s000 }, { &P##s010, &P##s010 } }, KSR(P, 1), KSR(P, 2), KSR(P, 3), KSR(P, 4) } }
static const kset_t ksets[2] = { KSET(rast_kern_), KSET(rast_kern_h) };

int rast_texfilter;             /* 0 nearest (exact), 1 bilinear, 2 sharp bilinear */

static void splat16(uint16_t *d, uint16_t v) { for (int i = 0; i < 8; i++) d[i] = v; }

/* the kernel arguments: everything per polygon; the kernels read the per-line values from the span entry */
static void kargs_poly(poly_t *P) {
    kargs_t *a = (kargs_t *)P->kargs;
    a->recip = P->recip; a->recip_u = P->recip_u;
    for (int i = 0; i < 4; i++) a->pid24[i] = P->pid << 24;
    a->bytes[0] = (uint8_t)P->A; a->bytes[1] = (uint8_t)P->aref; a->bytes[2] = (P->attr >> 15) & 1 ? 0x80 : 0;
    a->bytes[3] = (uint8_t)P->pid;
    a->K = P->K; a->tw = P->tw;
    a->owner = P->owner; splat16(a->idx16, (uint16_t)P->idx);
    a->lstride = P->lstride; a->attr_off = P->attr_off; a->id_off = P->id_off; a->id_stride = P->id_stride;
    a->owner_stride = P->owner_stride;
    static const uint8_t lut_linear[16] = { 0, 1, 2, 3, 4, 5, 6, 7 }, lut_sharp[16] = { 0, 0, 0, 2, 4, 6, 8, 8 };
    memcpy(a->fraclut, rast_texfilter == 2 ? lut_sharp : lut_linear, 16);
    P->pal16 = 0;
    if (P->flags & 2) {
        a->tex = P->paletted ? (const void *)P->idx8 : (const void *)P->texels32; a->pal = P->pal;
        if (P->paletted) {
            unsigned fmt = P->tex[0x49], np = fmt == 2 ? 4 : fmt == 3 ? 16 : 0;    /* I2, I4: 4 and 16 colours */
            if (np) { memcpy(a->pal16, P->pal, np * 4); P->pal16 = 1; }
        }
        /* texcoord(): flip where x & flip; clamp [lo, hi] (identity for wrap/flip); & (W-1) (identity for clamp) */
        splat16(a->s_and, P->tw - 1); splat16(a->t_and, P->th - 1);
        splat16(a->s_lo, P->ms == CLAMP ? 0 : 0x8000); splat16(a->t_lo, P->mt == CLAMP ? 0 : 0x8000);
        splat16(a->s_hi, P->ms == CLAMP ? P->tw - 1 : 0xffff); splat16(a->t_hi, P->mt == CLAMP ? P->th - 1 : 0xffff);
        splat16(a->s_flip, P->ms == FLIP ? (uint16_t)(P->tw & ~(P->tw - 1)) : 0);
        splat16(a->t_flip, P->mt == FLIP ? (uint16_t)(P->th & ~(P->th - 1)) : 0);
    }
}

/* a batch through the assembly kernel of this polygon's variant */
static void batch_asm(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0) {
    unsigned fl = P->flags;
    int D = (fl & 0x10) ? 2 : (fl & 8) ? 1 : 0, T = 0, R = fl & 1;
    if (fl & 2) T = (P->paletted ? 3 : 1) + (P->ms == WRAP && P->mt == WRAP);
    kern_fn *kern = ksets[P->hr].kernels[D][T][R][(fl >> 2) & 1][T && rast_texfilter ? 1 : 0];
    kargs_t *a = (kargs_t *)P->kargs;
    unsigned fr = U16(bs, SPO(P, 6)) >> 3, fg = U16(bs, SPO(P, 6) + 2) >> 3, fb = U16(bs, SPO(P, 8) + 2) >> 3;
    a->bytes[4] = (uint8_t)fr; a->bytes[5] = (uint8_t)fg; a->bytes[6] = (uint8_t)fb;
    int flat_white = (fl & 4) && T && P->A == 31 && fr == 63 && fg == 63 && fb == 63;
    const int edges = !R && ((P->d3 >> 5) & 1);
    /* flags: 0 affine steps, 1 depth equal, 2 white (flat batches; the kernels compute it per line otherwise),
     * 6 edge marking; translucent: 3 blend, 4 fog, 5 depth update */
    const unsigned flags = ((fl >> 5) & 1) | ((P->attr >> 14) & 1) << 1 | flat_white << 2 | edges << 6 | P->pal16 << 7 |
        (R ? ((P->d3 >> 3) & 1) << 3 | ((P->attr >> 15) & 1) << 4 | ((P->attr >> 11) & 1) << 5 : 0);
    uint8_t dummy[32];
    uint64_t anypass = kern(a, bs, k, line, P->ctx, flags, id0 ? id0 : dummy);
    if (anypass) { P->pass = 1; P->fogused |= 1; }
}

static int tex_variant(const poly_t *P) {
    return (P->flags & 2) ? (P->paletted ? 3 : 1) + (P->ms == WRAP && P->mt == WRAP) : 0;
}

/* the visibility pass of a deferred opaque polygon: depth and alpha tests, attribute words, owner indices, edges;
 * the texture only matters (P->flags bit 1, kept by f_run_4x) when its alpha test can fail */
static void batch_vis(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0) {
    unsigned fl = P->flags;
    int D = (fl & 0x10) ? 2 : (fl & 8) ? 1 : 0;
    kargs_t *a = (kargs_t *)P->kargs;
    const int edges = (P->d3 >> 5) & 1;
    const unsigned flags = ((fl >> 5) & 1) | ((P->attr >> 14) & 1) << 1 | edges << 6 | P->pal16 << 7;
    uint8_t dummy[32];
    if (ksets[P->hr].vis[D][tex_variant(P)](a, bs, k, line, P->ctx, flags, dummy)) { P->pass = 1; P->fogused |= 1; }
    (void)id0;
}

/* the shade pass: the colour of the pixels the polygon owns */
static void batch_shade(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0) {
    unsigned fl = P->flags;
    int T = tex_variant(P);
    kargs_t *a = (kargs_t *)P->kargs;
    unsigned fr = U16(bs, SPO(P, 6)) >> 3, fg = U16(bs, SPO(P, 6) + 2) >> 3, fb = U16(bs, SPO(P, 8) + 2) >> 3;
    a->bytes[4] = (uint8_t)fr; a->bytes[5] = (uint8_t)fg; a->bytes[6] = (uint8_t)fb;
    int flat_white = (fl & 4) && T && P->A == 31 && fr == 63 && fg == 63 && fb == 63;
    const unsigned flags = ((fl >> 5) & 1) | flat_white << 2 | P->pal16 << 7;
    uint8_t dummy[32];
    ksets[P->hr].shade[T][(fl >> 2) & 1][T && rast_texfilter ? 1 : 0](a, bs, k, line, P->ctx, flags, dummy);
    (void)id0;
}

/* the batch routine for this polygon: the assembly kernel for opaque modulate-shaded polygons, else the C NEON one */
batch_fn *asm_batch_for(poly_t *P) {
    if (P->mode == 1 || P->mode == 2) return neon_batch_for(P);
    kargs_poly(P);
    return P->dmode == 1 ? batch_vis : P->dmode == 2 ? batch_shade : batch_asm;
}
