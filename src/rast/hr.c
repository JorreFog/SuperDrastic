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
static hrv_t hr_vtx[2][1568];           /* the vertices' 3x screen coordinates, per geometry buffer */
static hrv_t hr_vtx2[2][1568];          /* RAST_VCHECK: the 2x ones, to check the index mapping against DraStic's */
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
    if (n > 1568) n = 1568;
    const int32_t *X = (const int32_t *)(p + GEOM_CLIP_X), *Y = (const int32_t *)(p + GEOM_CLIP_Y), *W = (const int32_t *)(p + GEOM_CLIP_W);
    uint32_t vw = U16(p, GEOM_VIEWPORT), vh = U16(p, GEOM_VIEWPORT + 2), vx = U16(p, GEOM_VIEWPORT + 4), vy = U16(p, GEOM_VIEWPORT + 6);
    uint32_t oy = (uint32_t)(192 - (int)vy - (int)vh);
    hrv_t *o = hr_vtx[buf], *o2 = hr_vtx2[buf];
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
        if (hr_vcheck) { o2[i].x = (uint16_t)(2 * vx + x2); o2[i].y = (uint16_t)(2 * oy + y2); }
    }
}

/* ---- the polygon walker: render_polygon_4x (spec/edges.c section 1) at 3x ---- */
static void hr_polygon(hr_t *H, uint8_t *poly, uint8_t *verts, const hrv_t *hv, const hrv_t *hv2, unsigned bin_top,
                       unsigned bin_bot, int lb, uint32_t d3, int defer) {
    uint32_t a8 = U32(poly, 8);
    unsigned count = a8 & 15, flags = (a8 >> 8) & 0xff, oi = (a8 >> 16) & 0x7f, base = U16(poly, 0x1a);
    if (count < 3 || count > 10) return;
    /* the vertex order: DraStic's table entry oi = group + t is the group's base sequence rotated to start at its
     * top vertex t; the top vertex is chosen again from the 3x coordinates (ties keep DraStic's) */
    const uint32_t *orders = (const uint32_t *)(ds_base + DS_VERTEX_ORDERS);
    uint32_t seq = orders[oi & ~7u];
    unsigned t = oi & 7, top = t;
    unsigned idx[10]; uint16_t ys[10];
    unsigned ymin = 0xffff;
    for (unsigned k = 0; k < count; k++) {
        idx[k] = base + ((seq >> (4 * k)) & 15);
        if (idx[k] >= 1568) return;
        ys[k] = hv[idx[k]].y;
        if (ys[k] < ymin) ymin = ys[k];
    }
    if (ys[t] != ymin) for (unsigned k = 0; k < count; k++) if (ys[k] == ymin) { top = k; break; }
    uint8_t vbuf[10][16]; vtx_t *vptr[12];
    unsigned ybot = 0, bad = 0;
    for (unsigned k = 0; k < count; k++) {
        unsigned vi = idx[(top + k) % count];
        memcpy(vbuf[k], verts + 16 * vi, 16);
        if (hr_vcheck && (U16(vbuf[k], 4) != hv2[vi].x || U16(vbuf[k], 6) != hv2[vi].y)) bad++;
        U16(vbuf[k], 4) = hv[vi].x; U16(vbuf[k], 6) = hv[vi].y;
        if (hv[vi].y > ybot) ybot = hv[vi].y;
        vptr[k] = vbuf[k];
    }
    vptr[count] = vptr[0]; vptr[count + 1] = vptr[1];
    if (bad) {
        static unsigned nrep;
        if (nrep++ < 10) fprintf(stderr, "[hr] vertex check: %u of %u vertices differ (poly %08x)\n", bad, count, a8);
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
static void hr_clear_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned hy0) {
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
static void fog_weights(const uint32_t *attr, uint8_t *w, const uint8_t *table, uint32_t params) {
    uint32_t off = params >> 16;
    int8_t sh = (int8_t)(params & 0xff);
    for (int x = 0; x < HR_W; x++) {
        uint32_t d = (attr[x] >> 9) & 0x7fff;
        d = d > off ? d - off : 0;
        uint16_t v = (uint16_t)sqshl16((int32_t)d, sh);
        uint32_t i = (uint32_t)(v >> 10) & 0x3f;
        int32_t f = v & 0x3ff, a = (int32_t)(int8_t)table[32 + i] * 32, p = (2 * a * f) >> 16;
        w[x] = (uint8_t)(table[i] + (uint8_t)p);
    }
}
static inline uint8_t fog_ch(uint8_t c, uint8_t f, uint8_t k) {
    int16_t p = (int16_t)((int8_t)(uint8_t)(c - f) * (int8_t)k);
    return (uint8_t)(c + (uint8_t)((uint16_t)p >> 7));
}
static void fog_line(uint32_t *c, const uint8_t *w, uint32_t fogc, int full) {
    for (int x = 0; x < HR_W; x++) {
        uint32_t px = c[x];
        uint8_t c3 = px >> 24, k = (uint8_t)(-w[x] + (w[x] == 0x7f ? 0xff : 0));
        if (c3 <= 0x7f) k = 0;
        c3 &= 0x7f;
        if (full) {
            uint8_t r = fog_ch(px, fogc, k), g = fog_ch(px >> 8, fogc >> 8, k), b = fog_ch(px >> 16, fogc >> 16, k);
            px = r | g << 8 | b << 16;
        } else px &= 0xffffff;
        c[x] = px | (uint32_t)fog_ch(c3, fogc >> 24, k) << 24;
    }
}
static inline int edge_vs(uint32_t c, uint32_t n) {
    uint8_t c3 = (uint8_t)(((c >> 24) & 0x7f) ^ 0x40), n3 = (n >> 24) & 0x3f;
    return (n & 0xffffff) > (c & 0xffffff) && c3 != n3;
}
static void edge_line(uint32_t *out, const uint32_t *col, const uint32_t *cur, const uint32_t *above, const uint32_t *below,
                      uint32_t clear, const uint8_t *ec) {
    for (int x = 0; x < HR_W; x++) {
        uint32_t c = cur[x], l = x ? cur[x - 1] : clear, r = x < HR_W - 1 ? cur[x + 1] : clear;
        int any = edge_vs(c, l) | edge_vs(c, r) | edge_vs(c, above[x]) | edge_vs(c, below[x]);
        uint8_t e = (uint8_t)(((((c >> 24) & 0x7f) ^ 0x40) >> 3) | (any ? 0 : 0xff)), j = (uint8_t)(e * 2);
        uint32_t px = col[x];
        uint8_t cr = px, cg = px >> 8, cb = px >> 16, ca = px >> 24;
        if (j < 16) { cr = ec[j >> 1]; cg = ec[8 + (j >> 1)]; cb = ec[16 + (j >> 1)]; }
        ca = (uint8_t)((ca & 0x1f) | ((e >> 3) & 0xe0));
        out[x] = cr | cg << 8 | cb << 16 | (uint32_t)ca << 24;
    }
}
static void hr_resolve_bin(hr_t *H, uint8_t *sys, uint8_t *geom, unsigned bin) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT);
    int edges = (d3 >> 5) & 1, fog = (d3 >> 7) & 1 ? ((d3 >> 6) & 1 ? 2 : 1) : 0;   /* 1 full, 2 alpha only */
    if (fog && !(U32(H->ctx, HR_HDR + 0x14) && U32(sys, 0x34eb50))) fog = 0;
    uint32_t params = 0, fogc = U32(geom, 0x9a9c), clear = U32(sys, SYS_CLEAR_ATTR);
    if (fog) { uint32_t sh = (d3 >> 8) & 0xf; params = sh | ((U16(geom, 0x9aaa) & 0x7fff) + (0x400u >> sh)) << 16; }
    uint8_t w[HR_W];
    (void)bin;
#define COL(l) ((uint32_t *)(H->ctx + (l) * HR_LSTRIDE))
#define ATT(l) ((uint32_t *)(H->ctx + HR_ATTR + (l) * HR_LSTRIDE))
    for (int l = 1; l <= HR_BL; l++) {
        uint32_t *o = H->out + (l - 1) * HR_W;
        if (fog) { fog_weights(ATT(l), w, geom + 0x9974, params); fog_line(COL(l), w, fogc, fog == 1); }
        if (edges) edge_line(o, COL(l), ATT(l), ATT(l - 1), ATT(l + 1), clear, geom + 0x99b4);
        else for (int x = 0; x < HR_W; x++) o[x] = COL(l)[x] & 0x1fffffff;
    }
#undef COL
#undef ATT
}

/* ---- downsample 3:2 into the output block: a 2x2 block of output pixels from a 3x3 block of 3x pixels with the
 * weights (4 2 . / 2 1 . / . . .) for the top-left one and so on (sum 9); colour weighted by alpha ---- */
static uint32_t recip9[9 * 31 + 1];     /* 2^24 / n, rounded */
static void hr_downsample(const uint32_t *in, uint8_t *out) {
    if (!recip9[1]) for (unsigned i = 1; i < sizeof recip9 / sizeof *recip9; i++) recip9[i] = (uint32_t)((1u << 24) + i / 2) / i;
    static const uint8_t wt[2][3] = { { 2, 1, 0 }, { 0, 1, 2 } };
    for (unsigned by = 0; by < HR_BL / 3; by++)
        for (unsigned bx = 0; bx < HR_W / 3; bx++) {
            const uint32_t *p = in + 3 * by * HR_W + 3 * bx;
            for (unsigned oy = 0; oy < 2; oy++)
                for (unsigned ox = 0; ox < 2; ox++) {
                    uint32_t sa = 0, sr = 0, sg = 0, sb = 0, cr = 0, cg = 0, cb = 0;
                    for (unsigned y = 0; y < 3; y++)
                        for (unsigned x = 0; x < 3; x++) {
                            unsigned wgt = wt[oy][y] * wt[ox][x];
                            if (!wgt) continue;
                            uint32_t px = p[y * HR_W + x], a = (px >> 24) & 0x1f, wa = wgt * a;
                            sa += wa;
                            sr += wa * (px & 0xff); sg += wa * ((px >> 8) & 0xff); sb += wa * ((px >> 16) & 0xff);
                            cr += wgt * (px & 0xff); cg += wgt * ((px >> 8) & 0xff); cb += wgt * ((px >> 16) & 0xff);
                        }
                    uint32_t r, g, b, a = (sa * recip9[9] + (1u << 23)) >> 24;
                    if (sa) {
                        uint32_t k = recip9[sa];
                        r = (uint32_t)(((uint64_t)sr * k + (1u << 23)) >> 24);
                        g = (uint32_t)(((uint64_t)sg * k + (1u << 23)) >> 24);
                        b = (uint32_t)(((uint64_t)sb * k + (1u << 23)) >> 24);
                    } else {
                        uint32_t k = recip9[9];
                        r = (cr * k + (1u << 23)) >> 24; g = (cg * k + (1u << 23)) >> 24; b = (cb * k + (1u << 23)) >> 24;
                    }
                    unsigned X = 2 * bx + ox, Y = 2 * by + oy;
                    *(uint32_t *)(out + Y * 0x800 + (X & 1) * 0x400 + (X >> 1) * 4) = r | g << 8 | b << 16 | a << 24;
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
        hr_render_list(H, sys + SYS_BINS_OPAQUE + bin * BIN_LIST_SIZE, opa, verts, hv, hv2, bin_top, bin_bot, lb, d3, rast_defer);
        if (ntrl) {
            memset(H->ctx + HR_ID, 0xff, HR_CL * HR_W);
            hr_render_list(H, sys + SYS_BINS_TRANSL + bin * BIN_LIST_SIZE, trl, verts, hv, hv2, bin_top, bin_bot, lb, d3, 0);
        }
        hr_resolve_bin(H, sys, geom, bin);
        if (hr_frame) memcpy(hr_frame + hy0 * HR_W, H->out, HR_BL * HR_W * 4);
        hr_downsample(H->out, PTR(sys, SYS_OUTPUT) + (size_t)bin * BIN_BYTES);
    }
}
