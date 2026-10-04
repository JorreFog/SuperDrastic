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
    uint32_t *out;                      /* the resolved bin, 48 x 768 */
} hr_t;
static __thread hr_t hr;

static hr_t *hr_get(void) {
    if (!hr.ctx) {
        hr.ctx = aligned_alloc(64, HR_CTX_SIZE + 64); memset(hr.ctx, 0, HR_CTX_SIZE + 64);
        hr.spans = aligned_alloc(64, HR_SPANS); memset(hr.spans, 0, HR_SPANS);
        hr.out = aligned_alloc(64, HR_BL * HR_W * 4);
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
        for (int i = 0; i < lines; i++) { U16(span, 8 * HR_SPS + 4 * i) &= 0x7fff; U16(span, 9 * HR_SPS + 4 * i) &= 0x7fff; }
        hr_render_polygon_setup_spans_4x(span, lines);
    }
    unsigned line0 = (unsigned)((int)(y_top > bin_top ? y_top : bin_top) - lb);     /* context line of the first line */
    /* a guard against spans outside the context (should not happen: clipped polygons stay within the viewport) */
    for (int i = 0; i < lines; i++) {
        unsigned X = U16(sp, 8 * HR_SPS + 4 * i), C = U16(sp, 9 * HR_SPS + 4 * i);
        if (X > HR_W || X + C > HR_W || line0 + (unsigned)lines > HR_CL) {
            static unsigned nrep;
            if (nrep++ < 8) {
                fprintf(stderr, "[hr] bad span: poly %08x line %d/%d X %u C %u line0 %u ytop %u ybot %u bin %u..%u\n", a8, i, lines, X, C,
                        line0, y_top, ybot, bin_top, bin_bot);
                for (unsigned k = 0; k < count; k++)
                    fprintf(stderr, "   v%u (%u,%u) w %d z %u\n", k, U16(vptr[k], 4), U16(vptr[k], 6), (int32_t)U32(vptr[k], 0), U16(vptr[k], 8));
            }
            return;
        }
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
 * alpha-only fog leaves r, g, b. A group of 16 without any fog flag is unchanged (k = 0) and skipped. */
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
    for (; c8 != end; c8 += 64) {
        uint8x16x4_t px = vld4q_u8(c8);
        if (__builtin_expect(vmaxvq_u8(px.val[3]) < 0x80, 0)) continue;     /* no fog flag in the group */
        uint8x16x4_t a = vld4q_u8(c8 + ad);
        uint16x8_t d0 = vshrq_n_u16(vreinterpretq_u16_u8(vzip1q_u8(a.val[1], a.val[2])), 1);
        uint16x8_t d1 = vshrq_n_u16(vreinterpretq_u16_u8(vzip2q_u8(a.val[1], a.val[2])), 1);
        uint16x8_t v0 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(vqsubq_u16(d0, off)), sh));
        uint16x8_t v1 = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(vqsubq_u16(d1, off)), sh));
        /* i = v >> 10 (the high bytes >> 2), or >= 0xc0 without the fog flag (tbl gives 0) */
        uint8x16_t i = vsriq_n_u8(vcgezq_s8(vreinterpretq_s8_u8(px.val[3])), vuzp2q_u8(vreinterpretq_u8_u16(v0), vreinterpretq_u8_u16(v1)), 2);
        int8x16_t dl = vreinterpretq_s8_u8(vqtbl2q_u8(dlt, i));
        int16x8_t p0 = vqdmulhq_s16(vshll_n_s8(vget_low_s8(dl), 5), vreinterpretq_s16_u16(v0));
        int16x8_t p1 = vqdmulhq_s16(vshll_high_n_s8(dl, 5), vreinterpretq_s16_u16(v1));
        uint8x16_t k = vsubq_u8(vqtbl2q_u8(ntab, i), vuzp1q_u8(vreinterpretq_u8_s16(p0), vreinterpretq_u8_s16(p1)));   /* -w */
        int8x16_t ks = vreinterpretq_s8_u8(vaddq_u8(k, vceqq_u8(k, x81)));  /* -128 for w = 127 */
        px.val[3] = fog_ch16(vandq_u8(px.val[3], x7f), fa, ks);
        if (full) px.val[0] = fog_ch16(px.val[0], fr, ks), px.val[1] = fog_ch16(px.val[1], fg, ks), px.val[2] = fog_ch16(px.val[2], fb, ks);
        vst4q_u8(c8, px);
    }
}
static __attribute__((noinline)) void fog_line(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full) {
    if (full) fog_line16(c, attr, table, params, fogc, 1);
    else fog_line16(c, attr, table, params, fogc, 0);
}
/* edge marking of a line, NEON over 8 pixels: a pixel C is an edge against a neighbour N (left, right, above,
 * below; the clear attribute beyond the line's ends) when N's 24-bit key is larger and the polygon ids differ
 * (c3 = ((C >> 24) & 0x7f) ^ 0x40 against n3 = (N >> 24) & 0x3f); the edge byte is c3 >> 3, or 0xff when no edge;
 * marked pixels (edge byte < 8, i.e. the polygon's edge flag set) take the edge colour of their id >> 3 and keep
 * alpha bits 0-4; unmarked ones get alpha (ca & 0x1f) | ((e >> 3) & 0xe0) (spec/resolve.c's edge_mark). */
static __attribute__((noinline)) void edge_line(uint32_t *out, const uint32_t *col, const uint32_t *cur, const uint32_t *above, const uint32_t *below,
                      uint32_t clear, const uint8_t *ec) {
    const uint32x4_t key = vdupq_n_u32(0xffffff), x40 = vdupq_n_u32(0x40), clr = vdupq_n_u32(clear);
    const uint8x8_t ecr = vld1_u8(ec), ecg = vld1_u8(ec + 8), ecb = vld1_u8(ec + 16);
    uint32x4_t prev = clr;
    for (int x = 0; x < HR_W; x += 4) {
        uint32x4_t c = vld1q_u32(cur + x);
        uint32x4_t next = x + 4 < HR_W ? vld1q_u32(cur + x + 4) : clr;
        uint32x4_t l = vextq_u32(prev, c, 3), r = vextq_u32(c, next, 1);
        uint32x4_t a = vld1q_u32(above + x), b = vld1q_u32(below + x);
        uint32x4_t ck = vandq_u32(c, key), c3 = veorq_u32(vandq_u32(vshrq_n_u32(c, 24), vdupq_n_u32(0x7f)), x40);
#define EDGE(N) vandq_u32(vcgtq_u32(vandq_u32(N, key), ck), vmvnq_u32(vceqq_u32(vandq_u32(vshrq_n_u32(N, 24), vdupq_n_u32(0x3f)), c3)))
        uint32x4_t any = vorrq_u32(vorrq_u32(EDGE(l), EDGE(r)), vorrq_u32(EDGE(a), EDGE(b)));
#undef EDGE
        uint32x4_t e = vandq_u32(vorrq_u32(vshrq_n_u32(c3, 3), vmvnq_u32(any)), vdupq_n_u32(0xff));   /* the edge byte; 0xff no edge */
        uint32x4_t px = vld1q_u32(col + x);
        uint32x4_t marked = vcltq_u32(e, vdupq_n_u32(8));
        uint8x8_t idx = vmovn_u16(vcombine_u16(vmovn_u32(vandq_u32(e, vdupq_n_u32(7))), vdup_n_u16(0)));
        /* the edge colours of the 4 pixels: tbl on the 8-entry tables, lanes 0-3 */
        uint8x8_t er = vtbl1_u8(ecr, idx), eg = vtbl1_u8(ecg, idx), eb = vtbl1_u8(ecb, idx);
        uint32x4_t ecol = vorrq_u32(vorrq_u32(vmovl_u16(vget_low_u16(vmovl_u8(er))), vshlq_n_u32(vmovl_u16(vget_low_u16(vmovl_u8(eg))), 8)),
                                    vshlq_n_u32(vmovl_u16(vget_low_u16(vmovl_u8(eb))), 16));
        uint32x4_t alpha = vorrq_u32(vandq_u32(px, vdupq_n_u32(0x1f000000)), vshlq_n_u32(vandq_u32(vshrq_n_u32(e, 3), vdupq_n_u32(0xe0)), 24));
        uint32x4_t res = vbslq_u32(marked, ecol, vandq_u32(px, key));
        vst1q_u32(out + x, vorrq_u32(res, alpha));
        prev = c;
    }
}
static __attribute__((noinline)) void hr_resolve_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned bin) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT);
    int edges = (d3 >> 5) & 1, fog = (d3 >> 7) & 1 ? ((d3 >> 6) & 1 ? 2 : 1) : 0;   /* 1 full, 2 alpha only */
    if (fog && !(U32(H->ctx, HR_HDR + 0x14) && U32(sys, 0x34eb50))) fog = 0;
    uint32_t params = 0, fogc = U32(geom, 0x9a9c), clear = U32(sys, SYS_CLEAR_ATTR);
    if (fog) { uint32_t sh = (d3 >> 8) & 0xf; params = sh | ((U16(geom, 0x9aaa) & 0x7fff) + (0x400u >> sh)) << 16; }
    (void)bin;
#define COL(l) ((uint32_t *)(H->ctx + (l) * HR_LSTRIDE))
#define ATT(l) ((uint32_t *)(H->ctx + HR_ATTR + (l) * HR_LSTRIDE))
    for (int l = 1; l <= HR_BL; l++) {
        uint32_t *o = H->out + (l - 1) * HR_W;
        if (fog) fog_line(COL(l), ATT(l), geom + 0x9974, params, fogc, fog == 1);
        if (edges) edge_line(o, COL(l), ATT(l), ATT(l - 1), ATT(l + 1), clear, geom + 0x99b4);
        else for (int x = 0; x < HR_W; x++) o[x] = COL(l)[x] & 0x1fffffff;
    }
#undef COL
#undef ATT
}

/* ---- downsample 3:2 into the output block ----
 * A 2x2 block of output pixels from a 3x3 block of 3x pixels: horizontally the left output is 2a + b and the right
 * b + 2c of each triple (a b c), vertically the top output 2 r0 + r1 and the bottom r1 + 2 r2 of the three rows,
 * so each output is a weighted sum of 9 (weights summing to 9). Colours are weighted by alpha as well (so the
 * clear colour of transparent pixels does not darken polygon edges): colour = sum(w a c) / sum(w a), alpha =
 * sum(w a) / 9, both rounded; a pixel whose sum(w a) is 0 is fully transparent and gets the clear colour (as the 2x
 * output has there in the common case; the compositor does not show it).
 * NEON, two triples (four output pixels) at a time: ld3 gives the a, b and c pixels of the triples as vectors;
 * the four channels of a pixel are u16 lanes (sums fit: 9 x 31 x 63 < 65536); the alpha of each pixel is spread
 * over its four lanes with tbl to be the per-channel divisor. The division is a float reciprocal (frecpe plus one
 * Newton step, exact enough for these ranges) scaled to 16 bits and applied with umull. The even output pixels of
 * a row are the left outputs and the odd ones the right outputs, which is the output block's layout. */
#include <arm_neon.h>
static __attribute__((noinline)) void hr_downsample(const uint32_t *in, uint8_t *out, uint32_t clear) {
    static const uint8_t alpha_idx[16] = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };
    const uint8x16_t aidx = vld1q_u8(alpha_idx);
    const uint8x8_t amask = vreinterpret_u8_u32(vdup_n_u32(0xff000000));
    const uint8x16_t a5 = vdupq_n_u8(0x1f);     /* the 5-bit alpha only: the resolve may leave flag bits 5-7 in byte 3 */
    const uint32x2_t clr = vdup_n_u32(clear & 0xffffff);    /* fully transparent outputs carry the clear colour, as at 2x */
    const uint32x4_t zero = vdupq_n_u32(0), k9 = vdupq_n_u32(7282);           /* 7282 / 65536 = 1 / 9 */
    for (unsigned by = 0; by < HR_BL / 3; by++) {
        const uint32_t *r0 = in + 3 * by * HR_W, *r1 = r0 + HR_W, *r2 = r1 + HR_W;
        uint8_t *o0 = out + (2 * by) * 0x800, *o1 = o0 + 0x800;
        for (unsigned bx = 0; bx < HR_W / 3; bx += 2, r0 += 6, r1 += 6, r2 += 6, o0 += 8, o1 += 8) {
            /* per row: the left (2a + b) and right (b + 2c) sums of the two triples, premultiplied colours (P) and
             * alpha weights spread over the channels (A); then top = 2 H(r0) + H(r1), bottom = H(r1) + 2 H(r2) */
            uint16x8_t pt, at, prt, art, pb, ab, prb, arb, p2, a2, pr2, ar2;
#define ROW(r, PL, AL, PR, AR) do { \
                uint32x2x3_t t = vld3_u32(r); \
                uint8x16_t ab16 = vcombine_u8(vreinterpret_u8_u32(t.val[0]), vreinterpret_u8_u32(t.val[1])); \
                uint8x16_t cb16 = vcombine_u8(vreinterpret_u8_u32(t.val[2]), vreinterpret_u8_u32(t.val[1])); \
                uint8x16_t aa = vandq_u8(vqtbl1q_u8(ab16, aidx), a5), ca = vandq_u8(vqtbl1q_u8(cb16, aidx), a5); \
                uint16x8_t pa = vmull_u8(vget_low_u8(ab16), vget_low_u8(aa)), pbb = vmull_u8(vget_high_u8(ab16), vget_high_u8(aa)); \
                uint16x8_t pc = vmull_u8(vget_low_u8(cb16), vget_low_u8(ca)); \
                uint16x8_t wa = vmovl_u8(vget_low_u8(aa)), wb = vmovl_u8(vget_high_u8(aa)), wc = vmovl_u8(vget_low_u8(ca)); \
                PL = vaddq_u16(vaddq_u16(pa, pa), pbb); PR = vaddq_u16(vaddq_u16(pc, pc), pbb); \
                AL = vaddq_u16(vaddq_u16(wa, wa), wb); AR = vaddq_u16(vaddq_u16(wc, wc), wb); \
            } while (0)
            ROW(r0, pt, at, prt, art);
            pt = vaddq_u16(pt, pt); at = vaddq_u16(at, at); prt = vaddq_u16(prt, prt); art = vaddq_u16(art, art);
            ROW(r1, pb, ab, prb, arb);
            pt = vaddq_u16(pt, pb); at = vaddq_u16(at, ab); prt = vaddq_u16(prt, prb); art = vaddq_u16(art, arb);
            ROW(r2, p2, a2, pr2, ar2);
            pb = vaddq_u16(pb, vaddq_u16(p2, p2)); ab = vaddq_u16(ab, vaddq_u16(a2, a2));
            prb = vaddq_u16(prb, vaddq_u16(pr2, pr2)); arb = vaddq_u16(arb, vaddq_u16(ar2, ar2));
#undef ROW
            /* colour = P / A per lane (K = 65536 / A as u16, 0 for A = 0; c = (P K + 2^15) >> 16), alpha = A / 9 */
#define DIV(P, A, OUT) do { \
                uint32x4_t al32 = vmovl_u16(vget_low_u16(A)), ah32 = vmovl_u16(vget_high_u16(A)); \
                float32x4_t fl = vcvtq_f32_u32(al32), fh = vcvtq_f32_u32(ah32); \
                float32x4_t rl = vrecpeq_f32(fl), rh = vrecpeq_f32(fh); \
                rl = vmulq_f32(rl, vrecpsq_f32(rl, fl)); rh = vmulq_f32(rh, vrecpsq_f32(rh, fh)); \
                rl = vmulq_f32(rl, vrecpsq_f32(rl, fl)); rh = vmulq_f32(rh, vrecpsq_f32(rh, fh)); \
                uint32x4_t kl = vbicq_u32(vcvtq_n_u32_f32(rl, 16), vceqq_u32(al32, zero)); \
                uint32x4_t kh = vbicq_u32(vcvtq_n_u32_f32(rh, 16), vceqq_u32(ah32, zero)); \
                uint16x8_t k = vcombine_u16(vqmovn_u32(kl), vqmovn_u32(kh)); \
                uint32x4_t cl = vmull_u16(vget_low_u16(P), vget_low_u16(k)), ch = vmull_u16(vget_high_u16(P), vget_high_u16(k)); \
                uint16x8_t c = vcombine_u16(vrshrn_n_u32(cl, 16), vrshrn_n_u32(ch, 16)); \
                uint16x8_t a9 = vcombine_u16(vrshrn_n_u32(vmulq_u32(al32, k9), 16), vrshrn_n_u32(vmulq_u32(ah32, k9), 16)); \
                uint32x2_t px = vreinterpret_u32_u8(vbsl_u8(amask, vmovn_u16(a9), vmovn_u16(c))); \
                OUT = vbsl_u32(vceq_u32(px & vdup_n_u32(0xff000000), vdup_n_u32(0)), clr, px); \
            } while (0)
            uint32x2_t out00, out01, out10, out11;
            DIV(pt, at, out00); DIV(prt, art, out01); DIV(pb, ab, out10); DIV(prb, arb, out11);
#undef DIV
            vst1_u32((uint32_t *)o0, out00); vst1_u32((uint32_t *)(o0 + 0x400), out01);
            vst1_u32((uint32_t *)o1, out10); vst1_u32((uint32_t *)(o1 + 0x400), out11);
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
        hr_resolve_bin(H, sys, geom, bin);
        if (hr_frame) memcpy(hr_frame + hy0 * HR_W, H->out, HR_BL * HR_W * 4);
        hr_downsample(H->out, PTR(sys, SYS_OUTPUT) + (size_t)bin * BIN_BYTES, U32(sys, SYS_CLEAR_COLOR));
    }
}
