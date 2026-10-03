/* fused.h: shared between fused.c (per-polygon setup, scalar reference line) and fused_neon.c (NEON batches) */
#ifndef FUSED_H
#define FUSED_H
#include <stdint.h>
typedef struct {
    uint8_t *ctx, *sys, *geom, *poly, *tex, *v0;
    const uint8_t *toon;                    /* geom + 0x99cc */
    const uint32_t *recip, *recip_u;
    uint32_t attr, pid, A, flags, d3, aref, mode, K;
    int ms, mt, paletted;
    uint16_t tw, th;
    const uint32_t *texels32, *pal; const uint8_t *idx8;
    /* texcoord() as clamp-and-mask: clamp to [lo, hi] (whole s16 range for wrap/flip), & and, flip where (x & flip) */
    int16_t s_lo, s_hi, t_lo, t_hi;
    uint16_t s_and, t_and, s_flip, t_flip;
    int fogused, pass;      /* pass: some pixel of the batch survived the depth and alpha tests */
    uint8_t kargs[0xc0] __attribute__((aligned(16)));    /* fused_asm.c: the assembly kernels' arguments */
} poly_t;
/* a batch: lines bs[0..k) of the span block, bin lines line.., at most 512 pixels; id0[l] receives the
 * translucent id of each line's first pixel afterwards (translucent polygons) */
typedef void batch_fn(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0);
batch_fn *neon_batch_for(const poly_t *P);
batch_fn *asm_batch_for(poly_t *P);
enum { CLAMP, WRAP, FLIP };
#endif
