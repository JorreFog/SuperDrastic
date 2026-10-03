/* fused_asm.c: the batches on the generated assembly kernels (kerngen.py -> rast_kern.S). Fills a kargs_t
 * per line with the same per-line values batch_neon computes, calls the kernel, then does the edge-marking fix-up
 * from the pass masks the kernel leaves in pm. The non-modulate shading modes stay on the C paths. */
#include <math.h>
#include <stdint.h>
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
} kargs_t;
_Static_assert(sizeof(kargs_t) == 0xc0, "kargs_t layout");
_Static_assert(__builtin_offsetof(kargs_t, pid24) == 0x20 && __builtin_offsetof(kargs_t, bytes) == 0x30 &&
               __builtin_offsetof(kargs_t, K) == 0x38 && __builtin_offsetof(kargs_t, tw) == 0x3c &&
               __builtin_offsetof(kargs_t, s_and) == 0x40, "kargs_t layout");

typedef uint64_t kern_fn(const kargs_t *, const uint8_t *bs, uint32_t k, uint32_t line, uint8_t *ctx, uint32_t flags, uint8_t *id0);
#define KD(D, T, R) extern kern_fn rast_kern_##D##T##R##0, rast_kern_##D##T##R##1;
#define KT(D) KD(D, 0, 0) KD(D, 0, 1) KD(D, 1, 0) KD(D, 1, 1) KD(D, 2, 0) KD(D, 2, 1) KD(D, 3, 0) KD(D, 3, 1) KD(D, 4, 0) KD(D, 4, 1)
KT(0) KT(1) KT(2)
#define KR(D, T) { { &rast_kern_##D##T##00, &rast_kern_##D##T##01 }, { &rast_kern_##D##T##10, &rast_kern_##D##T##11 } }
static kern_fn *const kernels[3][5][2][2] = {
    { KR(0, 0), KR(0, 1), KR(0, 2), KR(0, 3), KR(0, 4) },
    { KR(1, 0), KR(1, 1), KR(1, 2), KR(1, 3), KR(1, 4) },
    { KR(2, 0), KR(2, 1), KR(2, 2), KR(2, 3), KR(2, 4) },
};

static void splat16(uint16_t *d, uint16_t v) { for (int i = 0; i < 8; i++) d[i] = v; }

/* the kernel arguments: everything per polygon; the kernels read the per-line values from the span entry */
static void kargs_poly(poly_t *P) {
    kargs_t *a = (kargs_t *)P->kargs;
    a->recip = P->recip; a->recip_u = P->recip_u;
    for (int i = 0; i < 4; i++) a->pid24[i] = P->pid << 24;
    a->bytes[0] = (uint8_t)P->A; a->bytes[1] = (uint8_t)P->aref; a->bytes[2] = (P->attr >> 15) & 1 ? 0x80 : 0;
    a->bytes[3] = (uint8_t)P->pid;
    a->K = P->K; a->tw = P->tw;
    if (P->flags & 2) {
        a->tex = P->paletted ? (const void *)P->idx8 : (const void *)P->texels32; a->pal = P->pal;
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
    kern_fn *kern = kernels[D][T][R][(fl >> 2) & 1];
    kargs_t *a = (kargs_t *)P->kargs;
    unsigned fr = U16(bs, 0x420) >> 3, fg = U16(bs, 0x422) >> 3, fb = U16(bs, 0x582) >> 3;
    a->bytes[4] = (uint8_t)fr; a->bytes[5] = (uint8_t)fg; a->bytes[6] = (uint8_t)fb;
    int flat_white = (fl & 4) && T && P->A == 31 && fr == 63 && fg == 63 && fb == 63;
    const int edges = !R && ((P->d3 >> 5) & 1);
    /* flags: 0 affine steps, 1 depth equal, 2 white (flat batches; the kernels compute it per line otherwise),
     * 6 edge marking; translucent: 3 blend, 4 fog, 5 depth update */
    const unsigned flags = ((fl >> 5) & 1) | ((P->attr >> 14) & 1) << 1 | flat_white << 2 | edges << 6 |
        (R ? ((P->d3 >> 3) & 1) << 3 | ((P->attr >> 15) & 1) << 4 | ((P->attr >> 11) & 1) << 5 : 0);
    uint8_t dummy[32];
    uint64_t anypass = kern(a, bs, k, line, P->ctx, flags, id0 ? id0 : dummy);
    if (anypass) { P->pass = 1; P->fogused |= 1; }
}

/* the batch routine for this polygon: the assembly kernel for opaque modulate-shaded polygons, else the C NEON one */
batch_fn *asm_batch_for(poly_t *P) {
    if (P->mode == 1 || P->mode == 2) return neon_batch_for(P);
    kargs_poly(P);
    return batch_asm;
}
