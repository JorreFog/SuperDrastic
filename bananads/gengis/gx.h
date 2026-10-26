/* gx.h: Gengis Engine at 1x and 2x for BananaDS (gx.c, gx2.c): the DS's 3D scene rendered into melonDS's 3D layer by
 * SuperDrastic's rasterizer (its walker, its pixel kernels, its fog and edge-marking resolve), bin by bin, for a pool
 * of render threads. The adapter (gengis_renderer.cpp) fills a gx_frame from melonDS's GPU3D state. */
#ifndef BANANADS_GX_H
#define BANANADS_GX_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define GX_W 256            /* pixels a line (at 1x; 2x has twice as many lines of twice as many pixels) */
#define GX_H 192
#define GX_BL 16            /* lines a bin (at 1x) */
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
    const uint32_t *yr;                 /* per polygon ytop | ybot << 16 (the bins' overlap tests) */
    unsigned npolys, ntranslucent;      /* the translucent ones last */
    unsigned gen;                       /* the frame's number: the threads' per-frame state */
} gx_frame;

void gx_init(void);
/* the frame's state the pixel pipeline reads from its own places; before any thread renders a bin of the frame */
void gx_begin_frame(const gx_frame *f);
/* render bin `bin` of the frame into out (GX_BL lines of GX_W pixels, melonDS's 3D layer format) */
void gx_render_bin(const gx_frame *f, unsigned bin, uint32_t *out);
/* the same at 2x (gx2.c), from 2x vertex positions: 2 * GX_BL rows of 2 * GX_W pixels, each row as its even pixels
 * (GX_W) then its odd ones (GX_W), so that a DS line's four 256-pixel lines are rows 2y and 2y + 1, even and odd */
void gx2_render_bin(const gx_frame *f, unsigned bin, uint32_t *out);
/* the same at 3x (gx3.c, render.scale=3, experimental): 3 * GX_BL rows of 3 * GX_W pixels, each row as its pixels 3k,
 * then 3k + 1, then 3k + 2 (GX_W each): a DS line's nine 256-pixel lines are rows 3y + q / 3, third q % 3 */
void gx3_render_bin(const gx_frame *f, unsigned bin, uint32_t *out);
/* 4x (gx4.c, render.scale=4, experimental): 4 * GX_BL rows of 4 * GX_W, each row as its pixels 4k .. 4k + 3 (GX_W
 * each): a DS line's sixteen 256-pixel lines are rows 4y + q / 4, quarter q % 4 */
void gx4_render_bin(const gx_frame *f, unsigned bin, uint32_t *out);
/* one of 4x's 32-line halves of a bin (the frame's 24: bin 2 u / 2's half u % 2), 32 rows of 4 * GX_W, as above */
void gx4_render_unit(const gx_frame *f, unsigned unit, uint32_t *out);
static inline uint32_t gx_expand555(uint32_t c) {   /* BGR555 -> r6 | g6 << 8 | b6 << 16 (5 -> 6 as c*2 + (c != 0)) */
    uint32_t p = (c & 0x1f) | ((c >> 5) & 0x1f) << 8 | ((c >> 10) & 0x1f) << 16;
    return (p << 1) + (((p + 0x1f1f1f) >> 5) & 0x010101);
}
extern int gx_defer;                    /* deferred shading of opaque polygons (defer.c) */
extern int gx_stats;                    /* render.gengis_stats: count the span pixels (per thread: gx_px_*) */
extern __thread unsigned long gx_px_opa, gx_px_trl, gx_lines;
extern int gx_trim;                     /* render.gengis_trim: opaque polygons skip the bins' context lines without edge marking */
extern int gx_lazy;                     /* render.gengis_lazy: the bin context cleared 64 pixels at a time when a polygon reaches them */
extern int gx_hrneon;                   /* render.gengis_hrneon: decal/toon/highlight polygons on NEON batches (hrneon.c) */
extern __thread int gx_ref;             /* render.gengis_verify's reference render (this thread): the paths before the 4x changes */
extern int gx_zva;                      /* render.gengis_zva: the clear and the 2x output allocate lines with dc zva */

#ifdef __cplusplus
}
#endif
#endif
