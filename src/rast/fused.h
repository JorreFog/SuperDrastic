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
    int dmode;              /* deferred shading (defer.c): 0 no, 1 the visibility pass, 2 the shade pass */
    uint16_t *owner;        /* dmode: the bin's owner buffer (u16 per pixel, 512 per line; 0xffff none) */
    unsigned idx;           /* dmode: this polygon's index in the owner buffer */
    uint8_t kargs[0xf0] __attribute__((aligned(16)));    /* fused_asm.c: the assembly kernels' arguments */
} poly_t;
/* a batch: lines bs[0..k) of the span block, bin lines line.., at most 512 pixels; id0[l] receives the
 * translucent id of each line's first pixel afterwards (translucent polygons) */
typedef void batch_fn(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0);
batch_fn *neon_batch_for(const poly_t *P);
batch_fn *asm_batch_for(poly_t *P);
/* the polygon through the pipeline: dmode 0 immediately, 1 the visibility pass of a deferred opaque polygon with
 * index idx (returns whether any pixel passed), 2 its shade pass */
int f_run_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
             unsigned flags, uint8_t *v0, int dmode, unsigned idx);
void f_begin_frame(void);       /* per render thread, per frame: forgets the texture classifications */
/* defer.c */
extern int rast_defer;
uint16_t *defer_owner(void);
void defer_poly(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0);
void defer_flush(uint8_t *ctx);
enum { CLAMP, WRAP, FLIP };
#endif
