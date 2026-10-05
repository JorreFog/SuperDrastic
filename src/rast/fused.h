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
    uint16_t *owner;        /* dmode: the bin's owner buffer (u16 per pixel, one line per bin line; 0xffff none) */
    unsigned idx;           /* dmode: this polygon's index in the owner buffer */
    /* the layouts: the span block's bytes per array (DraStic 0xb0; hi-res 0x100) and the context's line stride,
     * attribute and id buffer offsets, id and owner line strides (DraStic: 0x800, 0x10000, 0x20000, 0x200, 0x400);
     * hr: the hi-res kernel set (strides from kargs) */
    unsigned sps, lstride, attr_off, id_off, id_stride, owner_stride, hr, hdr_off;
    int pal16;              /* fused_asm.c: the palette has at most 16 entries (tbl lookups) */
    int noat;               /* textured: the alpha test cannot fail (the texture's lowest alpha passes it) */
    uint8_t kargs[0x150] __attribute__((aligned(16)));   /* fused_asm.c: the assembly kernels' arguments */
} poly_t;
#define SPO(P, k) ((P)->sps * (k))      /* span array k: 0 W0, 1 dW, 2 Z0, 3 dZ, 4 st, 5 dst, 6 rg, 7 drg, 8 xb, 9 cdb, 10 edges */
/* the layouts (also for the hi-res pipeline, hr.c): per scale, the span block stride and the context strides */
typedef struct { unsigned sps, lstride, attr_off, id_off, id_stride, owner_stride, width, lines, hr, hdr_off; } layout_t;
extern const layout_t layout_2x, layout_3x;     /* hdr_off: the context header (sys +0, geom +8, fog used +0x14) */
/* a batch: lines bs[0..k) of the span block, bin lines line.., at most 512 pixels; id0[l] receives the
 * translucent id of each line's first pixel afterwards (translucent polygons) */
typedef void batch_fn(poly_t *P, const uint8_t *bs, unsigned k, unsigned line, uint8_t *id0);
batch_fn *neon_batch_for(const poly_t *P);
batch_fn *asm_batch_for(poly_t *P);
/* the polygon through the pipeline: dmode 0 immediately, 1 the visibility pass of a deferred opaque polygon with
 * index idx (returns whether any pixel passed), 2 its shade pass */
int f_run_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
             unsigned flags, uint8_t *v0, int dmode, unsigned idx);
int f_run(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0,
          unsigned nlines, unsigned flags, uint8_t *v0, int dmode, unsigned idx);
void f_begin_frame(void);       /* per render thread, per frame: forgets the texture classifications */
/* defer.c */
extern int rast_defer, rast_texfilter;
uint16_t *defer_owner(void);
void defer_poly(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0,
                unsigned nlines, unsigned flags, uint8_t *v0);
void defer_flush(const layout_t *L, uint8_t *ctx);
/* hr.c: the 3x pipeline */
void hr_render_bins(uint8_t *ctx);
void hr_vertices(uint8_t *geom, const uint32_t *recips, const uint32_t *shifts);
extern int hr_vcheck, hr_check, hr_ipcheck, hr_edges;
void hr_check_frame(uint8_t *sys);
extern uint32_t *hr_frame;
enum { CLAMP, WRAP, FLIP };
#endif
