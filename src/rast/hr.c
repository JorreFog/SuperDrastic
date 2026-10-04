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
/* fog weights of a line (spec/resolve.c's fog_calculate_weights), NEON over 8 pixels: d = sat(((attr >> 9) & 0x7fff)
 * - off), v = sqshl16(d, shift), i = v >> 10, f = v & 0x3ff, w = table[i] + ((s8)table[32 + i] * f) >> 10 */
static __attribute__((noinline)) void fog_weights(const uint32_t *attr, uint8_t *w, const uint8_t *table, uint32_t params) {
    const uint16x8_t off = vdupq_n_u16((uint16_t)(params >> 16)), m15 = vdupq_n_u16(0x7fff), m10 = vdupq_n_u16(0x3ff);
    const int16x8_t sh = vdupq_n_s16((int8_t)(params & 0xff));
    const uint8x16x2_t tab = { { vld1q_u8(table), vld1q_u8(table + 16) } }, dlt = { { vld1q_u8(table + 32), vld1q_u8(table + 48) } };
    for (int x = 0; x < HR_W; x += 8) {
        uint32x4_t a0 = vld1q_u32(attr + x), a1 = vld1q_u32(attr + x + 4);
        uint16x8_t d = vandq_u16(vcombine_u16(vshrn_n_u32(a0, 9), vshrn_n_u32(a1, 9)), m15);
        d = vqsubq_u16(d, off);
        uint16x8_t v = vreinterpretq_u16_s16(vqshlq_s16(vreinterpretq_s16_u16(d), sh));
        uint8x8_t i = vmovn_u16(vshrq_n_u16(v, 10));
        uint8x8_t t = vqtbl2_u8(tab, i), dl = vqtbl2_u8(dlt, i);
        int16x8_t f = vreinterpretq_s16_u16(vandq_u16(v, m10)), dl16 = vmovl_s8(vreinterpret_s8_u8(dl));
        int32x4_t p0 = vmull_s16(vget_low_s16(dl16), vget_low_s16(f)), p1 = vmull_s16(vget_high_s16(dl16), vget_high_s16(f));
        int16x8_t p = vcombine_s16(vshrn_n_s32(p0, 10), vshrn_n_s32(p1, 10));
        vst1_u8(w + x, vadd_u8(t, vmovn_u16(vreinterpretq_u16_s16(p))));
    }
}
/* fog of a line in place (fog_modulate_full/alpha_intermediate), NEON over 4 pixels: per byte c += ((s8)(c - F) *
 * k) >> 7 with k = -w (and -128 for 127) where the pixel's fog flag (bit 31) is set, else 0; the flag is cleared;
 * alpha-only fog leaves r, g, b */
static __attribute__((noinline)) void fog_line(uint32_t *c, const uint8_t *w, uint32_t fogc, int full) {
    static const uint8_t kidx[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };
    const uint8x16_t ki = vld1q_u8(kidx), fcol = vreinterpretq_u8_u32(vdupq_n_u32(fogc)), x7f = vdupq_n_u8(0x7f);
    const uint8x16_t chmask = vreinterpretq_u8_u32(vdupq_n_u32(full ? 0xffffffffu : 0xff000000u));
    for (int x = 0; x < HR_W; x += 4) {
        uint8x16_t px = vreinterpretq_u8_u32(vld1q_u32(c + x));
        uint8x8_t w8 = vreinterpret_u8_u32(vdup_n_u32(*(const uint32_t *)(w + x)));
        uint8x8_t k8 = vsub_u8(vdup_n_u8(0), vsub_u8(w8, vceq_u8(w8, vdup_n_u8(0x7f))));   /* -w, -128 for 127 */
        uint8x16_t k = vqtbl1q_u8(vcombine_u8(k8, k8), ki);                                 /* per pixel, 4 lanes */
        uint8x16_t flag = vreinterpretq_u8_u32(vcgtq_u32(vreinterpretq_u32_u8(px), vdupq_n_u32(0x7fffffff)));
        k = vandq_u8(vandq_u8(k, flag), chmask);
        px = vandq_u8(px, vreinterpretq_u8_u32(vdupq_n_u32(0x7fffffff)));
        int8x16_t diff = vreinterpretq_s8_u8(vsubq_u8(px, fcol));
        int16x8_t lo = vmulq_s16(vmovl_s8(vget_low_s8(diff)), vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(k))));
        int16x8_t hi = vmulq_s16(vmovl_s8(vget_high_s8(diff)), vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(k))));
        uint8x16_t add = vcombine_u8(vmovn_u16(vreinterpretq_u16_s16(vshrq_n_s16(lo, 7))), vmovn_u16(vreinterpretq_u16_s16(vshrq_n_s16(hi, 7))));
        vst1q_u32(c + x, vreinterpretq_u32_u8(vaddq_u8(px, add)));
    }
    (void)x7f;
}
/* edge marking (spec/resolve.c's edge_identify + edge_mark): a pixel C is an edge against a neighbour N (left, right,
 * above, below; the clear attribute beyond the line's ends) when N's 24-bit key is larger and the polygon ids differ
 * (c3 = ((C >> 24) & 0x7f) ^ 0x40 against n3 = (N >> 24) & 0x3f, never equal when C's bit 30 is clear); the edge byte
 * is c3 >> 3, or 0xff when no edge; pixels with an edge byte < 8 (an edge and bit 30 set) take the edge colour of
 * their id >> 3, all others keep r, g, b, and every alpha byte becomes ca & 0x1f (the edge byte's (e >> 3) & 0xe0 is
 * always 0). So per pixel:
 *   marked = bit 30 of C && some N with (N & 0xffffff) > (C & 0xffffff) and (N ^ C) & 0x3f000000,
 *   out = marked ? ec[(C >> 27) & 7] (r, g, b) | (px & 0x1f000000) : px & 0x1fffffff.
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
/* the marking of 16 pixels with the top bytes t and the tests y */
static inline __attribute__((always_inline)) void edge_mark(uint32_t *out, const uint32_t *col, uint8x16_t t, uint8x16_t y,
                                                            uint8x16_t er, uint8x16_t eg, uint8x16_t eb) {
    const uint8x16_t m1f = vdupq_n_u8(0x1f), m3f = vdupq_n_u8(0x3f);
    /* ~(t >> 2) & 0x1f = 15 - (id >> 2) + (bit 30 clear ? 16 : 0) where there is an edge, 31 where not */
    uint8x16_t idx = edge_sel(vtstq_u8(y, m3f), vbicq_u8(m1f, vshrq_n_u8(t, 2)), m1f);
    uint8x16x4_t p = vld4q_u8((const uint8_t *)col);
    p.val[0] = vqtbx1q_u8(p.val[0], er, idx); p.val[1] = vqtbx1q_u8(p.val[1], eg, idx); p.val[2] = vqtbx1q_u8(p.val[2], eb, idx);
    p.val[3] = vandq_u8(p.val[3], m1f);
    vst4q_u8((uint8_t *)out, p);
}
/* 16 pixels of two lines (the last ones of the lines when last: the clear attribute to the right), lp0 and lp1 = the
 * previous block's left tests (lane 15 is the one for this block's pixel 0), replaced by this block's */
static inline __attribute__((always_inline)) void edge_block(uint32_t *out0, uint32_t *out1, const uint32_t *col0, const uint32_t *col1,
                                                             const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
                                                             int last, uint8x16x4_t k, uint8x16_t *lp0, uint8x16_t *lp1,
                                                             uint8x16_t er, uint8x16_t eg, uint8x16_t eb) {
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
    edge_mark(out0, col0, p0.val[3], vorrq_u8(y0, v), er, eg, eb);
    if (!last) r = vld4q_u8((const uint8_t *)(c1 + 1));
    else for (int i = 0; i < 4; i++) r.val[i] = vextq_u8(p1.val[i], k.val[i], 1);
    lt = edge_pair(r, p1, &v);
    y1 = vorrq_u8(vorrq_u8(y1, v), vextq_u8(*lp1, lt, 15)); *lp1 = lt;
    n = vld4q_u8((const uint8_t *)b);
    y1 = vorrq_u8(y1, vandq_u8(edge_gt(n, p1), veorq_u8(n.val[3], p1.val[3])));
    edge_mark(out1, col1, p1.val[3], y1, er, eg, eb);
}
/* two lines of the bin, out0/col0/c0 and out1/col1/c1 (the line below), a = the line above the first, b = the line
 * below the second */
static __attribute__((noinline)) void edge_lines(uint32_t *out0, uint32_t *out1, const uint32_t *col0, const uint32_t *col1,
                                                 const uint32_t *a, const uint32_t *c0, const uint32_t *c1, const uint32_t *b,
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
        edge_block(out0 + x, out1 + x, col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 0, k, &lp0, &lp1, er, eg, eb);
    edge_block(out0 + x, out1 + x, col0 + x, col1 + x, a + x, c0 + x, c1 + x, b + x, 1, k, &lp0, &lp1, er, eg, eb);
}
static __attribute__((noinline)) void hr_resolve_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned bin) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT);
    int edges = (d3 >> 5) & 1, fog = (d3 >> 7) & 1 ? ((d3 >> 6) & 1 ? 2 : 1) : 0;   /* 1 full, 2 alpha only */
    if (fog && !(U32(H->ctx, HR_HDR + 0x14) && U32(sys, 0x34eb50))) fog = 0;
    uint32_t params = 0, fogc = U32(geom, 0x9a9c), clear = U32(sys, SYS_CLEAR_ATTR);
    if (fog) { uint32_t sh = (d3 >> 8) & 0xf; params = sh | ((U16(geom, 0x9aaa) & 0x7fff) + (0x400u >> sh)) << 16; }
    uint8_t w[HR_W];
    (void)bin;
#define COL(l) ((uint32_t *)(H->ctx + (l) * HR_LSTRIDE))
#define ATT(l) ((uint32_t *)(H->ctx + HR_ATTR + (l) * HR_LSTRIDE))
    _Static_assert(HR_BL % 2 == 0, "the resolve takes the bin's lines in pairs");
    for (int l = 1; l <= HR_BL; l += 2) {               /* two lines a step: the edge marking shares their compares */
        uint32_t *o = H->out + (l - 1) * HR_W;
        if (fog) for (int k = l; k <= l + 1; k++) { fog_weights(ATT(k), w, geom + 0x9974, params); fog_line(COL(k), w, fogc, fog == 1); }
        if (edges) edge_lines(o, o + HR_W, COL(l), COL(l + 1), ATT(l - 1), ATT(l), ATT(l + 1), ATT(l + 2), clear, geom + 0x99b4);
        else for (int x = 0; x < 2 * HR_W; x++) o[x] = COL(l)[x] & 0x1fffffff;
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
