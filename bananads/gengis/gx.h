/* gx.h: Gengis Engine at 1x for BananaDS (gx.c): the DS's 3D scene rendered into melonDS's 256x192 3D layer by
 * SuperDrastic's rasterizer (its walker, its pixel kernels, its fog and edge-marking resolve), bin by bin, for a pool
 * of render threads. The adapter (gengis_renderer.cpp) fills a gx_frame from melonDS's GPU3D state. */
#ifndef BANANADS_GX_H
#define BANANADS_GX_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define GX_W 256            /* pixels a line */
#define GX_H 192
#define GX_BL 16            /* lines a bin */
#define GX_NBINS (GX_H / GX_BL)

/* a polygon as the rasterizer takes it */
typedef struct {
    uint8_t rec[32];        /* the polygon record f_run reads: +2 texture wrap bits, +4 POLYGON_ATTR, +8 count |
                             * flags << 8 | ybot << 23, +0x10 the texture entry */
    uint8_t vtx[10][16];    /* the vertex records (w, x, y, z >> 9, BGR555, s, t), vertex 0 the top one, in order */
    uint16_t ytop, ybot;
    uint8_t count, flags;
    uint8_t pad[2];
} gx_poly;

/* polygon flags (rec + 9): */
enum { GXF_TRANSLUCENT = 1, GXF_TEXTURED = 2, GXF_FLAT = 4, GXF_WBUFFER = 8, GXF_CONSTDEPTH = 0x10, GXF_AFFINE = 0x20 };

/* a texture entry (the layout f_run reads: DraStic's texture cache entry's fields) */
typedef struct {
    uint32_t params, pages, pad0[2];
    const void *texels;     /* +0x10: u32 texels (direct, compressed) or u8 palette indices */
    const uint32_t *palette;/* +0x18: u32 colours (paletted formats) */
    uint8_t pad1[0x40 - 0x20];
    uint16_t width, height; /* +0x40 */
    uint16_t bucket, pltt;
    uint8_t dirty, format, paletted, pad2[5];
} gx_texture;

typedef struct {
    uint32_t d3, aref;                  /* DISP3DCNT, ALPHA_TEST_REF */
    uint32_t clear_color, clear_attr;   /* in the rasterizer's colour and attribute formats */
    const uint16_t *rear_col, *rear_dep;/* the rear-plane bitmap (DISP3DCNT bit 14): texture slots 2 and 3 */
    uint8_t rear_x, rear_y;
    uint8_t fog_table[64];              /* 32 densities, 32 deltas */
    uint32_t fog_color, fog_offset;     /* colour format; FOG_OFFSET */
    uint8_t edge[24];                   /* edge colours: r[8], g[8], b[8] (6 bits) */
    uint8_t toon[96];                   /* toon table: r[32], g[32], b[32] (6 bits) */
    const gx_poly *polys;
    unsigned npolys, ntranslucent;      /* the translucent ones last */
    unsigned gen;                       /* the frame's number: the threads' per-frame state */
} gx_frame;

void gx_init(void);
/* the frame's state the pixel pipeline reads from its own places; before any thread renders a bin of the frame */
void gx_begin_frame(const gx_frame *f);
/* render bin `bin` of the frame into out (GX_BL lines of GX_W pixels, melonDS's 3D layer format) */
void gx_render_bin(const gx_frame *f, unsigned bin, uint32_t *out);
static inline uint32_t gx_expand555(uint32_t c) {   /* BGR555 -> r6 | g6 << 8 | b6 << 16 (5 -> 6 as c*2 + (c != 0)) */
    uint32_t p = (c & 0x1f) | ((c >> 5) & 0x1f) << 8 | ((c >> 10) & 0x1f) << 16;
    return (p << 1) + (((p + 0x1f1f1f) >> 5) & 0x010101);
}
extern int gx_defer;                    /* deferred shading of opaque polygons (defer.c) */

#ifdef __cplusplus
}
#endif
#endif
