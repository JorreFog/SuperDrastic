/* hr.c: the hi-res pipeline: the 3D scene rendered at 3x (768x576) and supersampled into DraStic's 2x output frame.
 *
 * DraStic's hi-res path is 2x: it transforms vertices to 512x384, bins polygons into 12 bins of 32 lines and renders
 * each bin into a render context of 32 lines x 512 pixels. Here every bin is rendered at 3x instead, 48 lines x 768
 * pixels (plus one line above and below for the edge marking's neighbours), with
 *   - the vertices' 3x screen coordinates, computed by hr_vertices() from the same clip-space values and viewport as
 *     DraStic's geometry_perspective_apply_hires_asm (hooked in rast.c; its math in the comment there);
 *   - our own polygon walker, hr_polygon(): render_polygon_4x's edge walks and span setup from spec/edges.c, with
 *     the line width 768 and span arrays of 64 entries;
 *   - the pixel kernels of fused_asm.c in their hi-res instantiation (rast_kern_h*, strides from the kernel
 *     arguments) through f_run() with layout_3x, including deferred shading and bilinear filtering;
 *   - the bin resolve (edge marking, fog) as spec/resolve.c's, over 768-pixel lines, into a 48 x 768 buffer;
 *   - a 3:2 box downsample of that buffer into the bin's 32 x 512 output block (DraStic's compositing then sees a
 *     normal 2x frame): each output pixel averages a 1.5 x 1.5 block, colour weighted by alpha so transparent
 *     (clear) pixels do not darken polygon edges; the averaged alpha gives anti-aliased edges over the 2D layers.
 * Not rendered at 3x yet: shadow polygons (mode 3). DraStic's sprite path (axis-aligned textured quads, polygon
 * flag bit 14) goes through the general walker here. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"
#include "comp.h"
#include "spec/edges.h"

/* the 3x layout: 50 context lines (bin line l is frame line hy0 - 1 + l), 768 pixels; the span arrays 64 entries */
#define HR_N        3
#define HR_W        768
#define HR_BL       48                  /* bin lines */
#define HR_CL       50                  /* context lines: one above and below */
#define HR_LSTRIDE  (HR_W * 4)
#define HR_ATTR     (HR_CL * HR_LSTRIDE)
#define HR_ID       (2 * HR_ATTR)
#define HR_HDR      (HR_ID + HR_CL * HR_W)
#define HR_CTX_SIZE (HR_HDR + 0x100)
#define HR_SPS      0x100
#define HR_SPANS    (11 * HR_SPS + 0x300)

#define EDGES_FN(name) hr_##name
#define EDGES_ARR  0x100
#define EDGES_XMAX HR_W
#define EDGES_LINKAGE static
#define EDGES_NEON 1
/* the edge heights' reciprocals (0x3fffffff + i) / i: DraStic's table stops at 512 lines (its 2x frame has 384), a 3x
 * edge can be 575 lines tall */
static uint32_t hr_recip[1024];
__attribute__((constructor)) static void hr_recip_init(void) { for (uint32_t i = 1; i < 1024; i++) hr_recip[i] = (0x3fffffffu + i) / i; }
#define EDGES_RECIP_TABLE hr_recip
#include "spec/edges_impl.h"

#define U8(p, o)  (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)U64(p, o))

const layout_t layout_3x = { HR_SPS, HR_LSTRIDE, HR_ATTR, HR_ID, HR_W, HR_W * 2, HR_W, HR_CL, 1, HR_HDR };

typedef struct { uint16_t x, y; } hrv_t;
#define HR_NVTX 6144                    /* vertex records per geometry buffer (GEOM_VERTS_BUF / 16: the DS's vertex RAM) */
static hrv_t hr_vtx[2][HR_NVTX];        /* the vertices' 3x screen coordinates, per geometry buffer */
static hrv_t hr_vtx2[2][HR_NVTX];       /* the 2x ones: the index mapping check against DraStic's records */
int hr_vcheck;
uint32_t *hr_frame;                     /* RAST_DUMP: the resolved 3x frame (576 x 768) */

typedef struct {
    uint8_t *ctx;                       /* colour | attr | ids | header (sys, geom, line mask, fog used) */
    uint8_t *spans;
} hr_t;
static __thread hr_t hr;

static hr_t *hr_get(void) {
    if (!hr.ctx) {
        hr.ctx = aligned_alloc(64, HR_CTX_SIZE + 64); memset(hr.ctx, 0, HR_CTX_SIZE + 64);
        hr.spans = aligned_alloc(64, HR_SPANS); memset(hr.spans, 0, HR_SPANS);
    }
    return &hr;
}

/* ---- vertices: geometry_perspective_apply_hires_asm's screen transform at 3x ----
 * DraStic (per vertex i, clip x y w at geom+0x17f0/0x3070/0x6170, reciprocal r and shift s of w from its caller,
 * viewport width vw, height vh, x1 vx, y1 vy at geom+0x9ab6..):
 *   X2 = 2 vx + (((u32)((x + w) vw)) * r  >> (48 - s)) >> 14,   Y2 = 2 (192 - vy - vh) + (((u32)((w - y) vh)) * r >> (48 - s)) >> 14
 * (u64 products, the u64 >> (48 - s) a ushl by s - 48). At 3x: X3 = 3 vx + (3 P) >> (63 - s) with the same P. */
void hr_vertices(uint8_t *p, const uint32_t *recips, const uint32_t *shifts) {
    unsigned n = U32(p, GEOM_VTX_COUNT), buf = U8(p, GEOM_SWAP_BUF);
    /* geometry_flush_polygons appends this flush's n vertices to the buffer's vertex records: the record of
     * vertex i is total - n + i (the count at GEOM_VERTS + 0x18000 already includes them) */
    unsigned total = U32(p, GEOM_VERTS + buf * GEOM_VERTS_BUF + 0x18000), base = total >= n ? total - n : 0;
    if (base >= HR_NVTX) return;
    if (base + n > HR_NVTX) n = HR_NVTX - base;
    const int32_t *X = (const int32_t *)(p + GEOM_CLIP_X), *Y = (const int32_t *)(p + GEOM_CLIP_Y), *W = (const int32_t *)(p + GEOM_CLIP_W);
    uint32_t vw = U16(p, GEOM_VIEWPORT), vh = U16(p, GEOM_VIEWPORT + 2), vx = U16(p, GEOM_VIEWPORT + 4), vy = U16(p, GEOM_VIEWPORT + 6);
    uint32_t oy = (uint32_t)(192 - (int)vy - (int)vh);
    hrv_t *o = hr_vtx[buf] + base, *o2 = hr_vtx2[buf] + base;
    for (unsigned i = 0; i < n; i++) {
        uint32_t x = (uint32_t)X[i], y = (uint32_t)Y[i], w = (uint32_t)W[i], r = recips[i];
        int s = (int)shifts[i];
        unsigned __int128 px = (unsigned __int128)(uint32_t)((x + w) * vw) * r, py = (unsigned __int128)(uint32_t)((w - y) * vh) * r;
        int e = 63 - s;
        uint32_t x3, y3, x2, y2;
        if (e >= 0) {
            x3 = (uint32_t)((px * HR_N) >> e); y3 = (uint32_t)((py * HR_N) >> e);
            x2 = (uint32_t)((px * 2) >> e); y2 = (uint32_t)((py * 2) >> e);
        } else {
            x3 = (uint32_t)((px * HR_N) << -e); y3 = (uint32_t)((py * HR_N) << -e);
            x2 = (uint32_t)((px * 2) << -e); y2 = (uint32_t)((py * 2) << -e);
        }
        o[i].x = (uint16_t)(HR_N * vx + x3); o[i].y = (uint16_t)(HR_N * oy + y3);
        o2[i].x = (uint16_t)(2 * vx + x2); o2[i].y = (uint16_t)(2 * oy + y2);
    }
}

/* ---- the polygon walker: render_polygon_4x (spec/edges.c section 1) at 3x ---- */
static void hr_polygon(hr_t *H, uint8_t *poly, uint8_t *verts, const hrv_t *hv, const hrv_t *hv2, unsigned bin_top,
                       unsigned bin_bot, int lb, uint32_t d3, int defer) {
    uint32_t a8 = U32(poly, 8);
    unsigned count = a8 & 15, flags = (a8 >> 8) & 0xff, oi = (a8 >> 16) & 0x7f, base = U16(poly, 0x1a);
    if (count < 1 || count > 10) return;             /* DraStic walks 1 and 2 vertices too (points, lines) */
    /* the vertex order: DraStic's table entry oi = group + t is the group's base sequence rotated to start at its
     * top vertex t; the top vertex is chosen again from the 3x coordinates (ties keep DraStic's) */
    const uint32_t *orders = (const uint32_t *)(ds_base + DS_VERTEX_ORDERS);
    uint32_t seq = orders[oi & ~7u];
    unsigned t = oi & 7, top = t;
    unsigned idx[10]; uint16_t ys[10];
    unsigned ymin = 0xffff;
    for (unsigned k = 0; k < count; k++) {
        idx[k] = base + ((seq >> (4 * k)) & 15);
        if (idx[k] >= HR_NVTX) return;
        ys[k] = hv[idx[k]].y;
        if (ys[k] < ymin) ymin = ys[k];
    }
    if (ys[t] != ymin) for (unsigned k = 0; k < count; k++) if (ys[k] == ymin) { top = k; break; }
    uint8_t vbuf[10][16]; vtx_t *vptr[12];
    unsigned ybot = 0, bad = 0;
    for (unsigned k = 0; k < count; k++) {
        unsigned vi = idx[(top + k) % count];
        memcpy(vbuf[k], verts + 16 * vi, 16);
        unsigned x3 = hv[vi].x, y3 = hv[vi].y;
        if (U16(vbuf[k], 4) != hv2[vi].x || U16(vbuf[k], 6) != hv2[vi].y) {
            /* DraStic changed the record after its transform (or the mapping is off): scale its 2x coordinates */
            if (hr_vcheck) bad++;
            x3 = (3 * U16(vbuf[k], 4) + 1) / 2; y3 = (3 * U16(vbuf[k], 6) + 1) / 2;
        }
        U16(vbuf[k], 4) = (uint16_t)x3; U16(vbuf[k], 6) = (uint16_t)y3;
        if (y3 > ybot) ybot = y3;
        vptr[k] = vbuf[k];
    }
    vptr[count] = vptr[0]; vptr[count + 1] = vptr[1];
    if (bad) {
        static unsigned nrep;
        if (nrep++ < 6) {
            fprintf(stderr, "[hr] vertex check: %u of %u vertices differ (poly %08x base %u order %08x)\n", bad, count, a8, base, seq);
            for (unsigned k = 0; k < count; k++) {
                unsigned vi = idx[(top + k) % count];
                const uint8_t *v = verts + 16 * vi;
                int found = -1;
                for (unsigned j = 0; j < HR_NVTX && found < 0; j++)
                    if (hv2[j].x == U16(v, 4) && hv2[j].y == U16(v, 6)) found = (int)j;
                fprintf(stderr, "   v%u idx %u: record (%u,%u) ours (%u,%u) w %d; first index with the record's x,y: %d\n", k, vi,
                        U16(v, 4), U16(v, 6), hv2[vi].x, hv2[vi].y, (int32_t)U32(v, 0), found);
            }
        }
    }
    unsigned y_top = U16(vptr[0], 6);
    int top_clip = y_top < bin_top, bot_clip = ybot > bin_bot;
    unsigned y_end = ybot < bin_bot ? ybot : bin_bot;
    int lines = (int)y_end - (int)(y_top > bin_top ? y_top : bin_top);
    if (lines <= 0) return;
    uint8_t *span = H->spans, *scratch = span + 10 * HR_SPS, *sp = span;
    if ((d3 >> 5) & 1) {
        unsigned ys0 = bin_top - top_clip, ye = y_end + bot_clip, nl = lines + top_clip + bot_clip;
        hr_render_polygon_interpolate_edges_constprop_0(span, scratch, &vptr[0], ys0, ye, flags);
        hr_render_polygon_interpolate_edges_constprop_1(span + HR_SPS, scratch, &vptr[count], ys0, ye, flags);
        for (unsigned i = 0; i < nl; i++) {
            unsigned L = U16(span, 8 * HR_SPS + 4 * i), R = U16(span, 9 * HR_SPS + 4 * i), l = L & 0x7fff, r = R & 0x7fff;
            if (l > r) { if (!(L & 0x8000) && l < HR_W) l++; }
            else if (!(R & 0x8000) && r < HR_W) r++;
            U16(span, 8 * HR_SPS + 4 * i) = (uint16_t)l; U16(span, 9 * HR_SPS + 4 * i) = (uint16_t)r;
        }
        hr_render_polygon_setup_spans_4x(span, (int32_t)nl);
        sp = span + 4 * top_clip;
        hr_render_polygon_setup_edge_markers_c(sp, (uint32_t)lines, (uint32_t)(bot_clip << 1 | top_clip));
    } else {
        hr_render_polygon_interpolate_edges_constprop_0(span, scratch, &vptr[0], bin_top, y_end, flags);
        hr_render_polygon_interpolate_edges_constprop_1(span + HR_SPS, scratch, &vptr[count], bin_top, y_end, flags);
        /* (render_polygon_4x clears the vertical-edge bits here; setup_spans masks them itself) */
        hr_render_polygon_setup_spans_4x(span, lines);
    }
    unsigned line0 = (unsigned)((int)(y_top > bin_top ? y_top : bin_top) - lb);     /* context line of the first line */
    /* a guard against lines outside the context (should not happen: clipped polygons stay within the viewport). The
     * spans stay within the line: setup_spans orders and clamps both ends to HR_W, so X + C <= HR_W. */
    if (line0 + (unsigned)lines > HR_CL) {
        static unsigned nrep;
        if (nrep++ < 8) {
            fprintf(stderr, "[hr] bad span: poly %08x lines %d line0 %u ytop %u ybot %u bin %u..%u\n", a8, lines, line0, y_top, ybot,
                    bin_top, bin_bot);
            for (unsigned k = 0; k < count; k++)
                fprintf(stderr, "   v%u (%u,%u) w %d z %u\n", k, U16(vptr[k], 4), U16(vptr[k], 6), (int32_t)U32(vptr[k], 0), U16(vptr[k], 8));
        }
        return;
    }
    if (defer) defer_poly(&layout_3x, H->ctx, sp, poly, 0, line0, (unsigned)lines, flags, vptr[0]);
    else f_run(&layout_3x, H->ctx, sp, poly, 0, line0, (unsigned)lines, flags, vptr[0], 0, 0);
}

static void hr_render_list(hr_t *H, const uint8_t *list, uint8_t *polys, uint8_t *verts, const hrv_t *hv, const hrv_t *hv2,
                           unsigned bin_top, unsigned bin_bot, int lb, uint32_t d3, int defer) {
    uint32_t n = U32(list, 0x1000);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *poly = polys + 32 * (size_t)((const uint16_t *)list)[i];
        int d = defer && !((U32(poly, 4) >> 4) & 3);
        if (defer && !d) defer_flush(&layout_3x, H->ctx);
        hr_polygon(H, poly, verts, hv, hv2, bin_top, bin_bot, lb, d3, d);
    }
    if (defer) defer_flush(&layout_3x, H->ctx);
}

/* ---- clear: the clear colour, or the rear-plane bitmap (as rast.c's clear_bin, texels 3x3 at 3x) ---- */
static __attribute__((noinline)) void hr_clear_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned hy0) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT), cattr = U32(sys, SYS_CLEAR_ATTR);
    uint32_t *col = (uint32_t *)H->ctx, *att = (uint32_t *)(H->ctx + HR_ATTR);
    if (!(d3 & (1u << 14))) {
        uint32_t c = U32(sys, SYS_CLEAR_COLOR);
        for (int i = 0; i < HR_CL * HR_W; i++) col[i] = c, att[i] = cattr;
        return;
    }
    const uint16_t *ci = (const uint16_t *)PTR(sys, SYS_CLRIMG_COL), *di = (const uint16_t *)PTR(sys, SYS_CLRIMG_DEP);
    uint16_t ofs = U16(geom, GEOM_CLRIMG_OFS);
    unsigned xo = ofs & 0xff;
    uint32_t idattr = cattr & 0x3f000000;
    for (int l = 0; l < HR_CL; l++) {
        int yl = (int)hy0 - 1 + l + (ofs >> 8);
        if (yl < 0) yl = 0;
        unsigned row = (((unsigned)yl / HR_N) & 0xff) << 8;
        uint32_t *c = col + l * HR_W, *a = att + l * HR_W;
        for (int i = 0; i < 256; i++) {
            unsigned ix = row + ((xo + i) & 0xff);
            uint32_t pc = 0, pa;
            if (ci) pc = rast_pixel_embedded_alpha(ci[ix]);
            if (ci && di)  { uint16_t d = di[ix]; pc |= (uint32_t)(d >> 15) << 31; pa = ((d & 0x7fffu) << 9) | idattr; }
            else if (ci)   { pc |= 0x80000000u; pa = idattr | 0xfffe00; }
            else if (di)   { uint16_t d = di[ix]; pc = (uint32_t)(d >> 15) << 31; pa = ((d & 0x7fffu) << 9) | idattr; }
            else           { pc = 0x80000000u; pa = idattr | 0xfffe00; }
            for (int k = 0; k < HR_N; k++) { c[HR_N * i + k] = pc; a[HR_N * i + k] = pa; }
        }
    }
}

/* ---- resolve: spec/resolve.c's math over 768-pixel lines; the neighbours of the bin's first and last line are
 * the overlap lines (the clear attribute at the frame's top and bottom) ---- */
static int32_t sqshl16(int32_t v, int8_t s) {
    if (s >= 0) {
        if (v == 0) return 0;
        if (s >= 16) return v > 0 ? 0x7fff : -0x8000;
        int32_t r = v * (1 << s);
        return r > 0x7fff ? 0x7fff : r < -0x8000 ? -0x8000 : r;
    }
    int n = -s;
    if (n >= 16) return v < 0 ? -1 : 0;
    return v >> n;
}
/* fog of a line in place: spec/resolve.c's fog_calculate_weights and fog_modulate_full/alpha_intermediate in one
 * pass, NEON over 16 pixels with colours and attributes split into byte planes (ld4), so a pixel's weight is one
 * byte lane and the weights never go to memory.
 * Weight: d = sat(((attr >> 9) & 0x7fff) - off) (attr bytes 1 and 2 zipped to u16 lanes, >> 1), v = sqshl16(d,
 * shift), i = v >> 10, f = v & 0x3ff, w = table[i] + (u8)(((s8)table[32 + i] * f) >> 10). With v = f + 1024 i the
 * product is floor(dl v / 1024) - dl i, the first as sqdmulh(32 dl, v) = (64 dl v) >> 16 (exact: |32 dl| <= 4096,
 * v <= 0x7fff), the second folded into the table: -w = (dl i - table[i]) - (u8)floor(dl v / 1024) from a per-call
 * table of dl i - table[i]. The factor k = -w (and -128 for 127, i.e. -w = 0x81) where the pixel's fog flag (alpha
 * bit 7) is set, else 0: a clear flag sets the index's top bits, so both tbl lookups give 0 and k = 0.
 * Per byte c += ((s8)(c - F) * k) >> 7 (smull, then shrn #7 keeps product bits 7-14); the flag is cleared;
 * alpha-only fog leaves r, g, b. A group of 16 without any fog flag is unchanged (k = 0); a step of two such groups
 * is skipped. */
static inline __attribute__((always_inline)) uint8x16_t fog_ch16(uint8x16_t c, uint8x16_t f, int8x16_t k) {
    int8x16_t d = vreinterpretq_s8_u8(vsubq_u8(c, f));
    int16x8_t lo = vmull_s8(vget_low_s8(d), vget_low_s8(k)), hi = vmull_high_s8(d, k);
    return vaddq_u8(c, vreinterpretq_u8_s8(vshrn_high_n_s16(vshrn_n_s16(lo, 7), hi, 7)));
}
static inline __attribute__((always_inline)) void fog_line16(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params,
                                                             uint32_t fogc, int full) {
    static const uint8_t idx[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    const uint16x8_t off = vdupq_n_u16((uint16_t)(params >> 16));
    const int16x8_t sh = vdupq_n_s16((int8_t)(params & 0xff));
    const uint8x16x2_t dlt = { { vld1q_u8(table + 32), vld1q_u8(table + 48) } };
    const uint8x16x2_t ntab = { { vsubq_u8(vmulq_u8(dlt.val[0], vld1q_u8(idx)), vld1q_u8(table)),
                                  vsubq_u8(vmulq_u8(dlt.val[1], vld1q_u8(idx + 16)), vld1q_u8(table + 16)) } };
    const uint8x16_t fr = vdupq_n_u8((uint8_t)fogc), fg = vdupq_n_u8((uint8_t)(fogc >> 8)), fb = vdupq_n_u8((uint8_t)(fogc >> 16));
    const uint8x16_t fa = vdupq_n_u8((uint8_t)(fogc >> 24)), x81 = vdupq_n_u8(0x81), x7f = vdupq_n_u8(0x7f);
    uint8_t *c8 = (uint8_t *)c, *end = c8 + 4 * HR_W;
    const ptrdiff_t ad = (const uint8_t *)attr - c8;                        /* one pointer: attr at c8 + ad */
    /* the factor of 16 pixels: their alpha plane t (the flags) and their attribute words at at */
#define FOG_K(t, at) ({ \
        uint8x16x4_t a = vld4q_u8(at); \
        uint16x8_t d0 = vshrq_n_u16(vreinterpretq_u16_u8(vzip1q_u8(a.val[1], a.val[2])), 1); \
        uint16x8_t d1 = vshrq_n_u16(vreinterpretq_u16_u8(vzip2q_u8(a.val[1], a.val[2])), 1); \
        uint16x8_t v0 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(vqsubq_u16(d0, off)), sh)); \
        uint16x8_t v1 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(vqsubq_u16(d1, off)), sh)); \
        /* i = v >> 10 (the high bytes >> 2), or >= 0xc0 without the fog flag (tbl gives 0) */ \
        uint8x16_t i = vsriq_n_u8(vcgezq_s8(vreinterpretq_s8_u8(t)), vuzp2q_u8(vreinterpretq_u8_u16(v0), vreinterpretq_u8_u16(v1)), 2); \
        int8x16_t dl = vreinterpretq_s8_u8(vqtbl2q_u8(dlt, i)); \
        int16x8_t p0 = vqdmulhq_s16(vshll_n_s8(vget_low_s8(dl), 5), vreinterpretq_s16_u16(v0)); \
        int16x8_t p1 = vqdmulhq_s16(vshll_high_n_s8(dl, 5), vreinterpretq_s16_u16(v1)); \
        uint8x16_t k = vsubq_u8(vqtbl2q_u8(ntab, i), vuzp1q_u8(vreinterpretq_u8_s16(p0), vreinterpretq_u8_s16(p1)));   /* -w */ \
        vreinterpretq_s8_u8(vaddq_u8(k, vceqq_u8(k, x81)));                 /* -128 for w = 127 */ \
    })
    /* two groups a step, so that the in-order core has two independent chains to interleave (a group's own chain,
     * from the attribute load to the store, is about 70 cycles long); a step without any fog flag is skipped, and in
     * a step that is fogged a group without flags is unchanged (k = 0) */
    _Static_assert(HR_W % 32 == 0, "fog: 32 pixels a step");
    for (; c8 != end; c8 += 128) {
        uint8x16x4_t px = vld4q_u8(c8), py = vld4q_u8(c8 + 64);
        if (__builtin_expect(vmaxvq_u8(vorrq_u8(px.val[3], py.val[3])) < 0x80, 0)) continue;     /* no fog flag in the step */
        int8x16_t kx = FOG_K(px.val[3], c8 + ad), ky = FOG_K(py.val[3], c8 + 64 + ad);
        px.val[3] = fog_ch16(vandq_u8(px.val[3], x7f), fa, kx);
        py.val[3] = fog_ch16(vandq_u8(py.val[3], x7f), fa, ky);
        if (full) {
            px.val[0] = fog_ch16(px.val[0], fr, kx), px.val[1] = fog_ch16(px.val[1], fg, kx), px.val[2] = fog_ch16(px.val[2], fb, kx);
            py.val[0] = fog_ch16(py.val[0], fr, ky), py.val[1] = fog_ch16(py.val[1], fg, ky), py.val[2] = fog_ch16(py.val[2], fb, ky);
        }
        vst4q_u8(c8, px); vst4q_u8(c8 + 64, py);
    }
#undef FOG_K
}
static __attribute__((noinline)) void fog_line(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full) {
    if (full) fog_line16(c, attr, table, params, fogc, 1);
    else fog_line16(c, attr, table, params, fogc, 0);
}
/* edge marking (spec/resolve.c's edge_identify + edge_mark): a pixel C is an edge against a neighbour N (left, right,
 * above, below; the clear attribute beyond the line's ends) when N's 24-bit key is larger and the polygon ids differ
 * (c3 = ((C >> 24) & 0x7f) ^ 0x40 against n3 = (N >> 24) & 0x3f, never equal when C's bit 30 is clear); the edge byte
 * is c3 >> 3, or 0xff when no edge; pixels with an edge byte < 8 (an edge and bit 30 set) take the edge colour of
 * their id >> 3, all others keep r, g, b, and every alpha byte becomes ca & 0x1f (the edge byte's (e >> 3) & 0xe0 is
 * always 0). So per pixel:
 *   marked = bit 30 of C && some N with (N & 0xffffff) > (C & 0xffffff) and (N ^ C) & 0x3f000000,
 *   out = marked ? ec[(C >> 27) & 7] (r, g, b) | (px & 0x1f000000) : px & 0x1fffffff.
 * Bits 29-31 matter to nothing downstream (the downsample reads the alpha as & 0x1f), so the lines are marked in
 * place and keep them: the colours of a step (16 pixels of two lines) without a marked pixel are not loaded or stored
 * at all, which is most steps (S4 at 3x: edge_lines 3.03 -> 2.59 M modeled cycles a frame; 2.38 M with both lines'
 * tests before the one test for a marked pixel).
 * NEON over 16 pixels on byte planes: ld4 splits 16 attribute words into the key bytes k0, k1, k2 and the top byte t,
 * and 16 colours into r, g, b, a (st4 interleaves them back). N > C is lexicographic over the key planes,
 * k2 == ? (k1 == ? k0 > : k1 >) : k2 >, with cmeq/cmhi and two selects. The id test is folded in: (N > C) & (tN ^ tC)
 * is nonzero in bits 0-5 iff N is an edge for C (bit 30 aside), so the four are ORed and tested against 0x3f once.
 * Neighbours share their compares: from the same equalities, N > C and N >= C, whose complement is C > N, and the same
 * id xor give the test of C against N and the test of N against C. So the pair (x, x+1) gives the right test of x
 * and the left test of x+1 (the left results move one lane on, the lane before pixel 0 being pixel 0's test against
 * the clear attribute; the pixels to the right are an ld4 one pixel on), and two lines are marked at once: the pair
 * (line l, line l+1) gives the below test of l and the above test of l+1. The colour is a tbx on 16-entry tables
 * (entries 2i and 2i+1 = ec[7 - i]) indexed by ~(t >> 2) & 0x1f = 15 - (id >> 2) when bit 30 is set (16 or more when
 * clear) and by 31 where there is no edge: out-of-range indices keep the pixel's colour. */
static inline __attribute__((always_inline)) uint8x16_t edge_sel(uint8x16_t m, uint8x16_t t, uint8x16_t f) {
    /* m ? t : f as one bif (the bsl intrinsic becomes and/orr pairs when the compiler sees that the operands of the
     * compare chains are exclusive). f must not be an all-ones constant when testing under an unpatched qemu 9.2:
     * its TCG folds a bit select whose false operand is a known -1 to -1 (fold_bitsel_vec's orc case swaps the wrong
     * operands; tools/sim/setup.sh applies the fix to the simulator's qemu). */
    __asm__("bif %0.16b, %1.16b, %2.16b" : "+w"(t) : "w"(f), "w"(m));
    return t;
}
/* N > C on the key planes */
static inline __attribute__((always_inline)) uint8x16_t edge_gt(uint8x16x4_t n, uint8x16x4_t c) {
    return edge_sel(vceqq_u8(n.val[2], c.val[2]), edge_sel(vceqq_u8(n.val[1], c.val[1]), vcgtq_u8(n.val[0], c.val[0]),
                                                         vcgtq_u8(n.val[1], c.val[1])), vcgtq_u8(n.val[2], c.val[2]));
}
/* a pair of neighbours: *nc = the test of C against N ((N > C) & id xor), returns the test of N against C */
static inline __attribute__((always_inline)) uint8x16_t edge_pair(uint8x16x4_t n, uint8x16x4_t c, uint8x16_t *nc) {
    uint8x16_t e1 = vceqq_u8(n.val[1], c.val[1]), e2 = vceqq_u8(n.val[2], c.val[2]), g1 = vcgtq_u8(n.val[1], c.val[1]);
    uint8x16_t g2 = vcgtq_u8(n.val[2], c.val[2]), xid = veorq_u8(n.val[3], c.val[3]);
    uint8x16_t gt = edge_sel(e2, edge_sel(e1, vcgtq_u8(n.val[0], c.val[0]), g1), g2);        /* N > C */
    uint8x16_t ge = edge_sel(e2, edge_sel(e1, vcgeq_u8(n.val[0], c.val[0]), g1), g2);        /* N >= C */
    *nc = vandq_u8(gt, xid);
    return vbicq_u8(xid, ge);
}
/* the colours of 16 pixels in place, with the top bytes t and the tests' edge lanes e (cmtst of the tests against
 * 0x3f): the alpha bytes keep their flag bits 5-7 (the 2x path clears them, ca & 0x1f), which the downsample ignores */
static inline __attribute__((always_inline)) void edge_mark(uint32_t *col, uint8x16_t t, uint8x16_t e, uint8x16_t er, uint8x16_t eg,
                                                            uint8x16_t eb) {
    const uint8x16_t m1f = vdupq_n_u8(0x1f);
    /* ~(t >> 2) & 0x1f = 15 - (id >> 2) + (bit 30 clear ? 16 : 0) where there is an edge, 31 where not */
    uint8x16_t idx = edge_sel(e, vbicq_u8(m1f, vshrq_n_u8(t, 2)), m1f);
    uint8x16x4_t p = vld4q_u8((const uint8_t *)col);
    p.val[0] = vqtbx1q_u8(p.val[0], er, idx); p.val[1] = vqtbx1q_u8(p.val[1], eg, idx); p.val[2] = vqtbx1q_u8(p.val[2], eb, idx);
    vst4q_u8((uint8_t *)col, p);
}
/* 16 pixels of two lines (the last ones of the lines when last: the clear attribute to the right), lp0 and lp1 = the
 * previous block's left tests (lane 15 is the one for this block's pixel 0), replaced by this block's. Both lines'
 * tests first, then one test for a marked pixel (an edge and bit 30 set) in either line: the colours of a block
 * without one are not loaded or stored at all, and the two lines' compare chains share one basic block. */
static inline __attribute__((always_inline)) void edge_block(uint32_t *col0, uint32_t *col1, const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
                                                             int last, uint8x16x4_t k, uint8x16_t *lp0, uint8x16_t *lp1,
                                                             uint8x16_t er, uint8x16_t eg, uint8x16_t eb) {
    const uint8x16_t m3f = vdupq_n_u8(0x3f), m40 = vdupq_n_u8(0x40);
    uint8x16x4_t p0 = vld4q_u8((const uint8_t *)c0), p1 = vld4q_u8((const uint8_t *)c1), r, n;
    uint8x16_t y0, y1, v, lt;
    /* the right neighbours: the planes of an ld4 one pixel on */
    if (!last) r = vld4q_u8((const uint8_t *)(c0 + 1));
    else for (int i = 0; i < 4; i++) r.val[i] = vextq_u8(p0.val[i], k.val[i], 1);
    lt = edge_pair(r, p0, &y0);
    y0 = vorrq_u8(y0, vextq_u8(*lp0, lt, 15)); *lp0 = lt;
    n = vld4q_u8((const uint8_t *)a);
    y0 = vorrq_u8(y0, vandq_u8(edge_gt(n, p0), veorq_u8(n.val[3], p0.val[3])));
    y1 = edge_pair(p1, p0, &v);                                 /* line 1 against line 0 above it, and the reverse */
    y0 = vorrq_u8(y0, v);
    if (!last) r = vld4q_u8((const uint8_t *)(c1 + 1));
    else for (int i = 0; i < 4; i++) r.val[i] = vextq_u8(p1.val[i], k.val[i], 1);
    lt = edge_pair(r, p1, &v);
    y1 = vorrq_u8(vorrq_u8(y1, v), vextq_u8(*lp1, lt, 15)); *lp1 = lt;
    n = vld4q_u8((const uint8_t *)b);
    y1 = vorrq_u8(y1, vandq_u8(edge_gt(n, p1), veorq_u8(n.val[3], p1.val[3])));
    uint8x16_t e0 = vtstq_u8(y0, m3f), e1 = vtstq_u8(y1, m3f);
    uint8x16_t m = vorrq_u8(vandq_u8(e0, p0.val[3]), vandq_u8(e1, p1.val[3]));   /* bit 6: a marked pixel */
    if (__builtin_expect(!vmaxvq_u8(vandq_u8(m, m40)), 1)) return;
    edge_mark(col0, p0.val[3], e0, er, eg, eb);
    edge_mark(col1, p1.val[3], e1, er, eg, eb);
}
/* two lines of the bin, marked in place: col0/c0 and col1/c1 (the line below), a = the line above the first, b = the
 * line below the second */
static __attribute__((noinline)) void edge_lines(uint32_t *col0, uint32_t *col1, const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
                                                 uint32_t clear, const uint8_t *ec) {
    uint8x8_t ecr = vrev64_u8(vld1_u8(ec)), ecg = vrev64_u8(vld1_u8(ec + 8)), ecb = vrev64_u8(vld1_u8(ec + 16));
    uint8x16_t er = vcombine_u8(ecr, ecr), eg = vcombine_u8(ecg, ecg), eb = vcombine_u8(ecb, ecb);
    er = vzip1q_u8(er, er); eg = vzip1q_u8(eg, eg); eb = vzip1q_u8(eb, eb);       /* entries 2i, 2i+1 = ec[7 - i] */
    /* the left tests of the lines' pixel 0 against the clear attribute, in lane 15 */
    uint8x16_t lp0 = vsetq_lane_u8((clear & 0xffffff) > (c0[0] & 0xffffff) ? (uint8_t)((clear ^ c0[0]) >> 24) : 0, vdupq_n_u8(0), 15);
    uint8x16_t lp1 = vsetq_lane_u8((clear & 0xffffff) > (c1[0] & 0xffffff) ? (uint8_t)((clear ^ c1[0]) >> 24) : 0, vdupq_n_u8(0), 15);
    const uint8x16x4_t k = { { vdupq_n_u8((uint8_t)clear), vdupq_n_u8((uint8_t)(clear >> 8)), vdupq_n_u8((uint8_t)(clear >> 16)),
                               vdupq_n_u8((uint8_t)(clear >> 24)) } };
    int x = 0;
    for (; x < HR_W - 16; x += 16)
        edge_block(col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 0, k, &lp0, &lp1, er, eg, eb);
    edge_block(col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 1, k, &lp0, &lp1, er, eg, eb);
}
/* resolves the bin in place and returns its 48 lines (768 pixels each, contiguous): the context's colour lines 1..48,
 * fogged and edge-marked. The resolve's other work, clearing bits 29-31 of each pixel, is left out: the downsample
 * ignores them (it reads the alpha as & 0x1f and replaces byte 3). The edge marking reads only the attribute lines and
 * each pixel's own colour, so marking the colour lines in place gives the same pixels as a separate output buffer. */
static __attribute__((noinline)) const uint32_t *hr_resolve_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned bin) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT);
    int edges = (d3 >> 5) & 1, fog = (d3 >> 7) & 1 ? ((d3 >> 6) & 1 ? 2 : 1) : 0;   /* 1 full, 2 alpha only */
    if (fog && !(U32(H->ctx, HR_HDR + 0x14) && U32(sys, 0x34eb50))) fog = 0;
    uint32_t params = 0, fogc = U32(geom, 0x9a9c), clear = U32(sys, SYS_CLEAR_ATTR);
    if (fog) { uint32_t sh = (d3 >> 8) & 0xf; params = sh | ((U16(geom, 0x9aaa) & 0x7fff) + (0x400u >> sh)) << 16; }
    (void)bin;
#define COL(l) ((uint32_t *)(H->ctx + (l) * HR_LSTRIDE))
#define ATT(l) ((uint32_t *)(H->ctx + HR_ATTR + (l) * HR_LSTRIDE))
    _Static_assert(HR_BL % 2 == 0, "the resolve takes the bin's lines in pairs");
    for (int l = 1; l <= HR_BL; l += 2) {               /* two lines a step: the edge marking shares their compares */
        if (fog) for (int k = l; k <= l + 1; k++) fog_line(COL(k), ATT(k), geom + 0x9974, params, fogc, fog == 1);
        if (edges) edge_lines(COL(l), COL(l + 1), ATT(l - 1), ATT(l), ATT(l + 1), ATT(l + 2), clear, geom + 0x99b4);
    }
    return COL(1);
#undef COL
#undef ATT
}

/* ---- downsample 3:2 into the output block ----
 * A 2x2 block of output pixels from a 3x3 block of 3x pixels: horizontally the left output is 2a + b and the right
 * b + 2c of each triple (a b c), vertically the top output 2 r0 + r1 and the bottom r1 + 2 r2 of the three rows,
 * so each output is a weighted sum of 9 (weights summing to 9). Colours are weighted by alpha as well (so the
 * clear colour of transparent pixels does not darken polygon edges): with the 5-bit alphas a (the resolve may leave
 * flag bits 5-7 in byte 3), A = sum(w a) and P = sum(w a c) per channel, colour = (P K + 2^15) >> 16 with
 * K = 65536 / A (floor, saturated to 65535 for A = 1) and alpha = (A 7282 + 2^15) >> 16 (A / 9, 7282 / 65536 = 1 / 9);
 * an output whose alpha is 0 (A <= 4) is (nearly) transparent and gets the clear colour (as the 2x output has there
 * in the common case; the compositor does not show it).
 * NEON, four triples (four left and four right outputs in each of the two output rows) at a time: ld3 gives the a,
 * b and c pixels of the triples as vectors. Each of the 9 input vectors has one weight w: a0 4, b0 2, c0 4, a1 2,
 * b1 1, c1 2, a2 4, b2 2, c2 4 (top left: a0 b0 a1 b1, top right: c0 b0 c1 b1, bottom left: a1 b1 a2 b2, bottom
 * right: c1 b1 c2 b2). Three cases by the 36 alphas:
 *  - all 31 (the opaque interior, by far the most common): A = 279 and K = 234 everywhere, so colour =
 *    (31 S 234 + 2^15) >> 16 = (7254 S + 2^15) >> 16 with S = sum(w c), which is sqrdmulh(S, 3627); alpha = 31.
 *    S = 2X + b1 with X = 2a0 + b0 + a1 for the top left output (the others alike), X as u8 (<= 252 for 6-bit
 *    channels), S as u16 (<= 567).
 *  - all 0: every output is the clear colour.
 *  - otherwise the general case: w a of each pixel spread over its four bytes with tbl (<= 124), the pixel's byte 3
 *    replaced by 1, and umull/umlal chains sum an output's four products in u16 lanes: P in the colour lanes and A
 *    in the alpha lane (sums fit: 9 x 31 x 63 < 65536). K per pixel from the alpha lanes (uzp2 and a shift give
 *    four pixels' A in u32 lanes): a float reciprocal (frecpe plus two Newton steps) truncated to 16 fraction bits,
 *    which is floor(65536 / A) for every A <= 279, saturated to u16. tbl spreads K over the pixel's colour lanes and
 *    7282 into its alpha lane, so one umull and rounding narrow give the colours and the alpha (A = 0: P = 0, any K).
 * The left outputs of a row go to the first 0x400 bytes of the output line and the right ones to the second (the
 * even and the odd pixels: the output block's layout). */
#include <arm_neon.h>
static __attribute__((noinline)) void hr_downsample(const uint32_t *in, uint8_t *out, uint32_t clear) {
    static const uint8_t alpha_idx[16] = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };
    /* the K spread: 7282 in bytes 0-7 and K of pixels 0-3 in bytes 8-15 (u16 lanes) -> two pixels' four lanes each */
    static const uint8_t k_idx[32] = { 8, 9, 8, 9, 8, 9, 0, 1, 10, 11, 10, 11, 10, 11, 0, 1,
                                       12, 13, 12, 13, 12, 13, 0, 1, 14, 15, 14, 15, 14, 15, 0, 1 };
    const uint8x16_t aidx = vld1q_u8(alpha_idx), kidx0 = vld1q_u8(k_idx), kidx1 = vld1q_u8(k_idx + 16);
    const uint8x16_t a5 = vdupq_n_u8(0x1f);
    const uint32x4_t a5m = vdupq_n_u32(0x1f000000), amask = vdupq_n_u32(0xff000000), ones = vdupq_n_u32(1);
    const uint32x4_t clr = vdupq_n_u32(clear & 0xffffff);   /* fully transparent outputs carry the clear colour, as at 2x */
    const uint16x4_t k9 = vdup_n_u16(7282);
    uint8x16_t two = vdupq_n_u8(2);
    __asm__("" : "+w"(two));                /* (an opaque 2: mla, not add + shl, for b + 2a) */
    for (unsigned by = 0; by < HR_BL / 3; by++) {
        const uint32_t *r0 = in + 3 * by * HR_W, *r1 = r0 + HR_W, *r2 = r1 + HR_W;
        uint8_t *o0 = out + (2 * by) * 0x800, *oe = o0 + 0x400;
        for (; o0 != oe; o0 += 16) {
            __asm__("" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(o0));     /* (four pointers, post-incremented by the accesses) */
            uint32x4x3_t t0 = vld3q_u32(r0), t1 = vld3q_u32(r1), t2 = vld3q_u32(r2);
            r0 += 12; r1 += 12; r2 += 12;
            uint8x16_t a0 = vreinterpretq_u8_u32(t0.val[0]), b0 = vreinterpretq_u8_u32(t0.val[1]), c0 = vreinterpretq_u8_u32(t0.val[2]);
            uint8x16_t a1 = vreinterpretq_u8_u32(t1.val[0]), b1 = vreinterpretq_u8_u32(t1.val[1]), c1 = vreinterpretq_u8_u32(t1.val[2]);
            uint8x16_t a2 = vreinterpretq_u8_u32(t2.val[0]), b2 = vreinterpretq_u8_u32(t2.val[1]), c2 = vreinterpretq_u8_u32(t2.val[2]);
            uint32x4_t all = vandq_u32(vandq_u32(vandq_u32(t0.val[0], t0.val[1]), vandq_u32(t0.val[2], t1.val[0])),
                                       vandq_u32(vandq_u32(t1.val[1], t1.val[2]), vandq_u32(t2.val[0], t2.val[1])));
            uint32x4_t tl, tr, bl, br;          /* the outputs: top left, top right, bottom left, bottom right */
            if (!vmaxvq_u32(vbicq_u32(a5m, vandq_u32(all, t2.val[2])))) {
                /* all opaque: S = 2X + b1 with X = 2a0 + b0 + a1 (top left), 2c0 + b0 + c1, 2a2 + b2 + a1, 2c2 + b2 + c1
                 * (u8: <= 252), so the u8 stage is an add and an mla per output and the u16 stage a shift and an add;
                 * sli puts 31 (a5's low byte) into byte 3 */
                uint8x16_t xtl = vmlaq_u8(vaddq_u8(b0, a1), a0, two), xtr = vmlaq_u8(vaddq_u8(b0, c1), c0, two);
                uint8x16_t xbl = vmlaq_u8(vaddq_u8(b2, a1), a2, two), xbr = vmlaq_u8(vaddq_u8(b2, c1), c2, two);
#define DIV9(X) vsliq_n_u32(vreinterpretq_u32_u8(vuzp1q_u8( \
                    vreinterpretq_u8_s16(vqrdmulhq_n_s16(vreinterpretq_s16_u16(vaddw_u8(vshll_n_u8(vget_low_u8(X), 1), vget_low_u8(b1))), 3627)), \
                    vreinterpretq_u8_s16(vqrdmulhq_n_s16(vreinterpretq_s16_u16(vaddw_high_u8(vshll_high_n_u8(X, 1), b1)), 3627)))), vreinterpretq_u32_u8(a5), 24)
                tl = DIV9(xtl); tr = DIV9(xtr); bl = DIV9(xbl); br = DIV9(xbr);
#undef DIV9
            } else if (!vmaxvq_u32(vandq_u32(a5m, vorrq_u32(vorrq_u32(vorrq_u32(vorrq_u32(t0.val[0], t0.val[1]), vorrq_u32(t0.val[2], t1.val[0])),
                                                                     vorrq_u32(vorrq_u32(t1.val[1], t1.val[2]), vorrq_u32(t2.val[0], t2.val[1]))), t2.val[2])))) {
                tl = tr = bl = br = clr;        /* all transparent */
            } else {
#define WA(v, s) vshlq_n_u8(vandq_u8(vqtbl1q_u8(v, aidx), a5), s)
                uint8x16_t wa0 = WA(a0, 2), wb0 = WA(b0, 1), wc0 = WA(c0, 2), wa1 = WA(a1, 1), wb1 = vandq_u8(vqtbl1q_u8(b1, aidx), a5);
                uint8x16_t wc1 = WA(c1, 1), wa2 = WA(a2, 2), wb2 = WA(b2, 1), wc2 = WA(c2, 2);
#undef WA
#define ONE(v) v = vreinterpretq_u8_u32(vsliq_n_u32(vreinterpretq_u32_u8(v), ones, 24))
                ONE(a0); ONE(b0); ONE(c0); ONE(a1); ONE(b1); ONE(c1); ONE(a2); ONE(b2); ONE(c2);
#undef ONE
#define MAC4(X0, W0, X1, W1, X2, W2, X3, W3) \
                vmlal_u8(vmlal_u8(vmlal_u8(vmull_u8(vget_low_u8(X0), vget_low_u8(W0)), vget_low_u8(X1), vget_low_u8(W1)), \
                                  vget_low_u8(X2), vget_low_u8(W2)), vget_low_u8(X3), vget_low_u8(W3)), \
                vmlal_high_u8(vmlal_high_u8(vmlal_high_u8(vmull_high_u8(X0, W0), X1, W1), X2, W2), X3, W3)
                uint16x8_t ptl[2] = { MAC4(a0, wa0, b0, wb0, a1, wa1, b1, wb1) }, ptr[2] = { MAC4(c0, wc0, b0, wb0, c1, wc1, b1, wb1) };
                uint16x8_t pbl[2] = { MAC4(a1, wa1, b1, wb1, a2, wa2, b2, wb2) }, pbr[2] = { MAC4(c1, wc1, b1, wb1, c2, wc2, b2, wb2) };
#undef MAC4
#define DIV(P, OUT) do { \
                    float32x4_t f = vcvtq_f32_u32(vshrq_n_u32(vuzp2q_u32(vreinterpretq_u32_u16(P[0]), vreinterpretq_u32_u16(P[1])), 16)); \
                    float32x4_t r = vrecpeq_f32(f); \
                    r = vmulq_f32(r, vrecpsq_f32(r, f)); r = vmulq_f32(r, vrecpsq_f32(r, f)); \
                    uint8x16_t k = vreinterpretq_u8_u16(vqmovn_high_u32(k9, vcvtq_n_u32_f32(r, 16))); \
                    uint16x8_t k01 = vreinterpretq_u16_u8(vqtbl1q_u8(k, kidx0)), k23 = vreinterpretq_u16_u8(vqtbl1q_u8(k, kidx1)); \
                    uint16x8_t c01 = vcombine_u16(vrshrn_n_u32(vmull_u16(vget_low_u16(P[0]), vget_low_u16(k01)), 16), \
                                                  vrshrn_n_u32(vmull_high_u16(P[0], k01), 16)); \
                    uint16x8_t c23 = vcombine_u16(vrshrn_n_u32(vmull_u16(vget_low_u16(P[1]), vget_low_u16(k23)), 16), \
                                                  vrshrn_n_u32(vmull_high_u16(P[1], k23), 16)); \
                    uint32x4_t px = vreinterpretq_u32_u8(vuzp1q_u8(vreinterpretq_u8_u16(c01), vreinterpretq_u8_u16(c23))); \
                    OUT = vbslq_u32(vtstq_u32(px, amask), px, clr); \
                } while (0)
                DIV(ptl, tl); DIV(ptr, tr); DIV(pbl, bl); DIV(pbr, br);
#undef DIV
            }
            vst1q_u32((uint32_t *)o0, tl); vst1q_u32((uint32_t *)(o0 + 0x400), tr);
            vst1q_u32((uint32_t *)(o0 + 0x800), bl); vst1q_u32((uint32_t *)(o0 + 0xc00), br);
        }
    }
}

/* ---- the bin loop (in place of rast.c's render_bins when the scale is 3) ---- */
void hr_render_bins(uint8_t *ctx) {
    uint8_t *sys = PTR(ctx, CTX_SYS), *geom = PTR(ctx, CTX_GEOM);
    unsigned stride = U8(ctx, CTX_BIN_STRIDE), nb = NBINS / stride;
    unsigned buf = U8(geom, GEOM_SWAP_BUF) ^ 1;
    uint8_t *verts = geom + GEOM_VERTS + buf * GEOM_VERTS_BUF;
    uint8_t *opa = geom + GEOM_POLYS_OPA + buf * GEOM_POLYS_BUF, *trl = geom + GEOM_POLYS_TRL + buf * GEOM_POLYS_BUF;
    uint32_t ntrl = U32(geom, GEOM_TRL_COUNT + buf * GEOM_POLYS_BUF), d3 = U32(sys, SYS_DISP3DCNT);
    const hrv_t *hv = hr_vtx[buf], *hv2 = hr_vtx2[buf];
    hr_t *H = hr_get();
    U64(H->ctx, HR_HDR) = (uint64_t)(uintptr_t)sys; U64(H->ctx, HR_HDR + 8) = (uint64_t)(uintptr_t)geom;
    f_begin_frame();
    for (unsigned k = 0; k < nb; k++) {
        unsigned bin = U8(ctx, CTX_FIRST_BIN) + k * stride, hy0 = bin * HR_BL;
        /* context line l is frame line lb + l; the walks cover the bin and its neighbour lines */
        int lb = (int)hy0 - 1;
        unsigned bin_top = hy0 ? hy0 - 1 : 0, bin_bot = hy0 + HR_BL + 1;
        if (bin_bot > HR_BL * NBINS) bin_bot = HR_BL * NBINS;
        hr_clear_bin(H, sys, geom, hy0);
        U32(H->ctx, HR_HDR + 0x10) = 0xffffffffu; U32(H->ctx, HR_HDR + 0x14) = 0;
        U64(H->ctx, HR_HDR + 0x18) = ~0ull;             /* the shadow line mask (fused.c line_px): all lines clean */
        hr_render_list(H, sys + SYS_BINS_OPAQUE + bin * BIN_LIST_SIZE, opa, verts, hv, hv2, bin_top, bin_bot, lb, d3, rast_defer);
        if (ntrl) {
            memset(H->ctx + HR_ID, 0xff, HR_CL * HR_W);
            hr_render_list(H, sys + SYS_BINS_TRANSL + bin * BIN_LIST_SIZE, trl, verts, hv, hv2, bin_top, bin_bot, lb, d3, 0);
        }
        const uint32_t *res = hr_resolve_bin(H, sys, geom, bin);
        if (hr_frame) memcpy(hr_frame + hy0 * HR_W, res, HR_BL * HR_W * 4);
        hr_downsample(res, PTR(sys, SYS_OUTPUT) + (size_t)bin * BIN_BYTES, U32(sys, SYS_CLEAR_COLOR));
        comp_bin(sys, bin, 1);          /* the compositor's visibility table (comp.c) */
    }
}
