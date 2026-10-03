/* fused_asm.c: the batches on the generated assembly kernels (kerngen.py -> rast_kern.S). Fills a kargs_t
 * per line with the same per-line values batch_neon computes, calls the kernel, then does the edge-marking fix-up
 * from the pass masks the kernel leaves in pm. The non-modulate shading modes stay on the C paths. */
#include <arm_neon.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"

#pragma clang fp contract(off)

#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

typedef struct {                /* layout shared with kerngen.py (K dict) */
    float num[8], den[8], E0, E1;
    uint64_t za0, zstep;
    uint32_t Rwc; int32_t W0, dW; uint32_t K;
    int32_t base[5];
    int16_t dl[8];
    uint32_t C;
    uint32_t *col, *att; uint8_t *pm;
    uint32_t flags;
    uint32_t pad0;
    uint32_t pid24[4];
    uint8_t bytes[8];
    const void *tex; const uint32_t *pal;
    uint64_t pad1;
    uint16_t s_and[8], t_and[8], s_lo[8], t_lo[8], s_hi[8], t_hi[8], s_flip[8], t_flip[8];
} kargs_t;
_Static_assert(sizeof(kargs_t) == 0x160, "kargs_t layout");
_Static_assert(__builtin_offsetof(kargs_t, dl) == 0x7c && __builtin_offsetof(kargs_t, pid24) == 0xb0 &&
               __builtin_offsetof(kargs_t, tex) == 0xc8 && __builtin_offsetof(kargs_t, s_and) == 0xe0, "kargs_t layout");

typedef uint64_t kern_fn(kargs_t *);
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

/* the per-polygon part of the kernel arguments */
static void kargs_poly(poly_t *P) {
    kargs_t *a = (kargs_t *)P->kargs;
    for (int i = 0; i < 4; i++) a->pid24[i] = P->pid << 24;
    a->bytes[0] = (uint8_t)P->A; a->bytes[1] = (uint8_t)P->aref; a->bytes[2] = (P->attr >> 15) & 1 ? 0x80 : 0;
    a->bytes[3] = (uint8_t)P->pid;
    a->K = P->K;
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
    uint8_t pm[520];
    a->pm = pm;
    unsigned fr = U16(bs, 0x420) >> 3, fg = U16(bs, 0x422) >> 3, fb = U16(bs, 0x582) >> 3;
    a->bytes[4] = (uint8_t)fr; a->bytes[5] = (uint8_t)fg; a->bytes[6] = (uint8_t)fb;
    int flat_white = (fl & 4) && P->A == 31 && fr == 63 && fg == 63 && fb == 63;
    const int equal = (P->attr >> 14) & 1, wconst = (fl >> 5) & 1, edges = !R && ((P->d3 >> 5) & 1);
    const unsigned tflags = R ? ((P->d3 >> 3) & 1) << 3 | ((P->attr >> 15) & 1) << 4 | ((P->attr >> 11) & 1) << 5 : 0;
    const float32x4_t io_l = { 0, 1, 2, 3 }, io_h = { 4, 5, 6, 7 };
    uint64_t anypass = 0;

    for (unsigned l = 0; l < k; l++) {
        const uint8_t *s = bs + 4 * l;
        unsigned y = line + l, X = U16(s, 0x580), C = U16(s, 0x630);
        a->col = (uint32_t *)(P->ctx + CTX_COLOR + y * 0x800) + X;
        a->att = (uint32_t *)(P->ctx + CTX_ATTR + y * 0x800) + X;
        a->C = C;
        if (R) a->pm = P->ctx + CTX_IDBUF + y * 0x200 + X;
        int32_t W0 = (int32_t)U32(s, 0x000), dW = (int32_t)U32(s, 0x0b0);
        float fW0 = (float)W0, fD = (float)dW, S = (float)(int32_t)((uint32_t)W0 + (uint32_t)dW) * (float)C;
        vst1q_f32(a->num, vmulq_n_f32(io_l, fW0)); vst1q_f32(a->num + 4, vmulq_n_f32(io_h, fW0));
        vst1q_f32(a->den, vfmsq_n_f32(vdupq_n_f32(S), io_l, fD)); vst1q_f32(a->den + 4, vfmsq_n_f32(vdupq_n_f32(S), io_h, fD));
        a->E0 = 8.0f * fW0; a->E1 = 8.0f * fD;
        a->Rwc = P->recip_u[C]; a->W0 = W0; a->dW = dW;
        uint32_t Z0 = U32(s, 0x160); int32_t dZ = (int32_t)U32(s, 0x210);
        a->za0 = (uint64_t)Z0 << 30;
        a->zstep = (uint64_t)((int64_t)dZ * (int64_t)(int32_t)P->recip[C] + (dZ < 0 ? 0x3fffffff : 0));
        uint32_t rg0 = U32(s, 0x420), drg = U32(s, 0x4d0), xb = U32(s, 0x580), cdb = U32(s, 0x630);
        uint32_t st0 = U32(s, 0x2c0), dst = U32(s, 0x370);
        int16_t du = (int16_t)dst, dv = (int16_t)(dst >> 16);
        a->base[0] = (int32_t)((rg0 & 0xffff) << 15); a->base[1] = (int32_t)((rg0 >> 16) << 15); a->base[2] = (int32_t)((xb >> 16) << 15);
        a->base[3] = (int32_t)(((uint32_t)(int32_t)(int16_t)st0 << 15) + (du > 0 ? 0x400 : 0));
        a->base[4] = (int32_t)(((uint32_t)(int32_t)(int16_t)(st0 >> 16) << 15) + (dv > 0 ? 0x400 : 0));
        a->dl[0] = (int16_t)drg; a->dl[1] = (int16_t)(drg >> 16); a->dl[2] = (int16_t)(cdb >> 16);
        a->dl[3] = du; a->dl[4] = dv; a->dl[5] = (int16_t)P->tw;
        int white = T && ((fl & 4) ? flat_white : (P->A == 31 && rg0 == 0x01ff01ff && drg == 0 && (xb >> 16) == 0x1ff && (cdb >> 16) == 0));
        a->flags = wconst | equal << 1 | white << 2 | tflags;
        anypass |= kern(a);
        if (R) id0[l] = a->pm[0];
        if (edges) {
            unsigned EL = U16(s, 0x6e0), ER = U16(s, 0x6e2), p24 = P->pid << 24;
            uint32_t *att_l = a->att;
            if (EL > C) EL = C;
            if (ER > C) ER = C;
            for (unsigned i = 0; i < EL; i++) if (pm[i]) att_l[i] = (att_l[i] & 0xffffff) | 0x40000000u | p24;
            for (unsigned i = C - ER; i < C; i++) if (pm[i]) att_l[i] = (att_l[i] & 0xffffff) | 0x40000000u | p24;
        }
    }
    if (anypass) { P->pass = 1; P->fogused |= 1; }
}

/* the batch routine for this polygon: the assembly kernel for opaque modulate-shaded polygons, else the C NEON one */
batch_fn *asm_batch_for(poly_t *P) {
    if (P->mode == 1 || P->mode == 2) return neon_batch_for(P);
    kargs_poly(P);
    return batch_asm;
}
