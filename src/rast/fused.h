/* fused.h: shared between fused.c (per-polygon setup, scalar reference line) and fused_neon.c (NEON lines) */
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
    int fogused, pass;      /* pass: some pixel of the batch survived the depth and alpha tests */
} poly_t;
typedef void line_fn(poly_t *P, uint8_t *s, const uint8_t *bs, unsigned y);
line_fn *neon_line_for(const poly_t *P);
#endif
