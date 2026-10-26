/* gx.c: Gengis Engine at 1x (gx.h). SuperDrastic's rasterizer, driven by BananaDS instead of DraStic.
 *
 * The frame is cut into GX_NBINS bins of GX_BL lines. A render thread renders a bin into a context of its own (the
 * bin's lines plus one above and one below, the edge marking's neighbours, GX_W pixels each) with
 *   - walk_impl.h's fused polygon walker, in the layout of hr.c's 3x pipeline (span arrays of 64 entries, a chain's
 *     lines stopped at the window) at 256 pixels a line;
 *   - fused.c's f_run and the hi-res set of the assembly kernels (rast_kern_h*: buffer strides from their arguments),
 *     with deferred shading (defer.c) when gx_defer is set;
 *   - hr.c's resolve (fog, then edge marking) over the bin's lines (gx_resolve.h);
 * and writes the bin's lines in melonDS's 3D layer format (r6 | g6 << 8 | b6 << 16 | a5 << 24).
 *
 * Where the copied code reads DraStic's memory it reads ours: fused.c takes the reciprocal tables at ds_base + their
 * DraStic offsets, and DISP3DCNT, the alpha reference and the toon table from the "sys" and "geom" blocks at their
 * DraStic offsets; gx_begin_frame() writes the frame's values there. */
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"
#include "fogzero.h"
#include "spec/edges.h"
/* GX_SCALE 2 (gx2.c): the same pipeline at 512x384, as gx2_render_bin; GX_SCALE 3 (gx3.c): 768x576, gx3_render_bin */
#ifndef GX_SCALE
#define GX_SCALE 1
#endif
#if GX_SCALE == 2
#define gx_render_bin gx2_render_bin
#elif GX_SCALE == 3
#define gx_render_bin gx3_render_bin
#elif GX_SCALE == 4
#define gx_render_bin gx4_render_bin
#endif
#include "gx.h"
/* this pipeline's frame: GX_NBINS bins of GX_BL lines of GX_W pixels */
#undef GX_W
#undef GX_H
#undef GX_BL
#define GX_W (256 * GX_SCALE)
#define GX_H (192 * GX_SCALE)
/* GX_SUB: the bins a frame bin is rendered as, one after the other (4x: two of 32 lines, so that a bin's context of
 * GX_BL + 2 lines fits the walker's and the kernels' 64-line span arrays, the 64-bit shadow line mask and defer.c's
 * owner buffer; 64 + 2 lines would not) */
#if GX_SCALE == 4
#define GX_SUB 2
#else
#define GX_SUB 1
#endif
#define GX_BL (16 * GX_SCALE / GX_SUB)
_Static_assert(GX_H / GX_BL == (192 / 16) * GX_SUB, "the bins");  /* (GX_NBINS expands with this file's GX_H, GX_BL) */

_Static_assert(offsetof(gx_texture, texels) == 0x10 && offsetof(gx_texture, palette) == 0x18 &&
               offsetof(gx_texture, width) == 0x40 && offsetof(gx_texture, height) == 0x42 &&
               offsetof(gx_texture, format) == 0x49 && offsetof(gx_texture, paletted) == 0x4a, "gx_texture layout");
_Static_assert(sizeof(gx_poly) % 4 == 0, "gx_poly");

#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))

/* ---- the reciprocal tables, where fused.c looks for DraStic's ----
 * recip[i] = (0x3fffffff + i) / i (DS_RECIP: the edges and the depth steps), recip_u[i] = (0x7fffffff + i) / i
 * (DS_RECIP_U = DS_RECIP + 0x1400: the affine steps); 0 for i = 0 */
#define GX_RECIP_OFF   0x3f27120u
#define GX_RECIP_U_OFF 0x3f28520u
#define GX_TABS_N ((GX_RECIP_U_OFF - GX_RECIP_OFF) / 4 + 1025)
#if GX_SCALE == 1
uintptr_t ds_base;
uint32_t gx_tabs[GX_TABS_N];
int gx_defer;
int gx_stats;
__thread unsigned long gx_px_opa, gx_px_trl, gx_lines;
int gx_zva;
int gx_trim = 1;
/* rast.c's (RAST_STATS: the opaque pixels written, which fused.c counts when it is set) */
int rast_stats;
unsigned long st_written, st_covered;
#else
extern uint32_t gx_tabs[GX_TABS_N];
#endif

/* the sys and geom blocks: the fields fused.c reads, at DraStic's offsets (calloc'd: the untouched pages cost no
 * memory) */
#define GX_SYS_SIZE  0x34ec00
#define GX_GEOM_SIZE 0x9b00
#define SYS_ALPHA_REF 0x34eb44
#define GEOM_TOON     0x99cc
#if GX_SCALE == 1
uint8_t *gx_sys, *gx_geom;

void gx_init(void) {
    {   /* dc zva usable with 64-byte blocks (DZP clear, BS = log2 of the block in words = 4)? else gx_zva stays off */
        uint64_t z; __asm__("mrs %0, dczid_el0" : "=r"(z));
        if (gx_zva && ((z >> 4) & 1 || (z & 15) != 4)) gx_zva = 0;
    }
    if (gx_sys) return;
    /* (to 1024: a span's pixel count C indexes them, and at render.scale=4 a line can be 1024 pixels; the walker's
     * edge heights stop at 1023, wrecip) */
    for (uint32_t i = 1; i <= 1024; i++) {
        gx_tabs[i] = (0x3fffffffu + i) / i;
        gx_tabs[(GX_RECIP_U_OFF - GX_RECIP_OFF) / 4 + i] = (0x7fffffffu + i) / i;
    }
    ds_base = (uintptr_t)gx_tabs - GX_RECIP_OFF;
    gx_sys = calloc(1, GX_SYS_SIZE);
    gx_geom = calloc(1, GX_GEOM_SIZE);
}

/* the frame's values that fused.c reads; before any thread renders a bin of the frame */
void gx_begin_frame(const gx_frame *f) {
    U32(gx_sys, SYS_DISP3DCNT) = f->d3;
    U32(gx_sys, SYS_ALPHA_REF) = f->aref;
    memcpy(gx_geom + GEOM_TOON, f->toon, sizeof f->toon);
}
#else
extern uint8_t *gx_sys, *gx_geom;
#endif

/* ---- the walker: walk_impl.h in this scale's layout ---- */
#define GX_CL       (GX_BL + 2)         /* context lines: one above and one below the bin */
#define GX_LSTRIDE  (GX_W * 4)
#define GX_ATTR     (GX_CL * GX_LSTRIDE)
#define GX_ID       (2 * GX_ATTR)
#define GX_HDR      (GX_ID + GX_CL * GX_W)
#define GX_CTX_SIZE (GX_HDR + 0x100)
#define GX_SPS      0x100               /* span arrays of 64 entries (the hi-res kernels' layout) */
#define GX_SPANS    (11 * GX_SPS + 0x300)

#define WALK_ARR   GX_SPS
#define WALK_XMAX  GX_W
#define WALK_RECIP gx_tabs
#define WALK_CAP   1
#define WALK_HR    1
#include "walk_impl.h"

static const layout_t layout_gx = { GX_SPS, GX_LSTRIDE, GX_ATTR, GX_ID, GX_W, GX_W * 2, GX_W, GX_CL, 1, GX_HDR };

typedef struct {
    uint8_t *ctx;                       /* colour | attributes | translucent ids | header (sys, geom, fog used) */
    uint8_t *spans;
    uint8_t *tp;                        /* the edge marking's id screen (gx_resolve.h's edge_tplanes) */
    unsigned gen;                       /* the frame whose per-frame state this thread has */
} gx_t;
static __thread gx_t gx_ctx;

static gx_t *gx_get(void) {
    gx_t *H = &gx_ctx;
    if (!H->ctx) {
        H->ctx = aligned_alloc(64, GX_CTX_SIZE + 64); memset(H->ctx, 0, GX_CTX_SIZE + 64);
        H->spans = aligned_alloc(64, GX_SPANS); memset(H->spans, 0, GX_SPANS);
        H->tp = aligned_alloc(64, GX_CL * GX_W + 64); memset(H->tp, 0, GX_CL * GX_W + 64);
        H->gen = ~0u;
    }
    return H;
}

/* one polygon into the bin's context: hr.c's hr_polygon on its fused path. bin_top/bin_bot: the frame lines the walk
 * covers (the bin and its neighbour lines), lb: the frame line of context line 0 */
static void gx_polygon(gx_t *H, const gx_poly *p, unsigned bin_top, unsigned bin_bot, int lb, uint32_t d3, int defer,
                       unsigned trim) {
    const unsigned count = p->count, flags = p->flags;
    if (count < 1 || count > 10) return;
    vtx_t *vptr[12];
    for (unsigned k = 0; k < count; k++) vptr[k] = (vtx_t *)p->vtx[k];
    vptr[count] = vptr[0]; vptr[count + 1] = vptr[1];
    const unsigned y_top = p->ytop, ybot = p->ybot;
    const int top_clip = y_top < bin_top, bot_clip = ybot > bin_bot;
    const unsigned y_end = ybot < bin_bot ? ybot : bin_bot, y_first = y_top > bin_top ? y_top : bin_top;
    const int lines = (int)y_end - (int)y_first;
    if (lines <= 0) return;
    uint8_t *span = H->spans, *sp = span;
    const int marks = (d3 >> 5) & 1;
    const unsigned ys0 = marks ? bin_top - top_clip : bin_top, ye = marks ? y_end + bot_clip : y_end;
    if (!(flags & 0x18)) {
        walk_chain(span, &vptr[0], 1, ys0, ye, 1);
        walk_chain(span + GX_SPS, &vptr[count], -1, ys0, ye, 1);
    } else {
        walk_chain(span, &vptr[0], 1, ys0, ye, 0);
        walk_chain(span + GX_SPS, &vptr[count], -1, ys0, ye, 0);
    }
    /* the spans with the right edge's pixel (walk_spans' adjust), edge marking on or off: the DS fills a span up to
     * and including its right edge's pixel, except a vertical right edge's (melonDS's RenderPolygonScanline: xend
     * inclusive, a vertical right edge pushed 1 pixel left). DraStic adjusts only with edge marking on, and without
     * it a polygon stopped a pixel short of the DS's: where two polygons' shared edge is a fraction of a pixel apart
     * (HeartGold's map blocks, each drawn with its own matrix) the background showed through between them, a dashed
     * black line down the ground, on about every other line at 2x */
    if (marks) {
        walk_spans(span, lines + top_clip + bot_clip, 1);
        sp = span + 4 * top_clip;
        walk_markers(sp, (uint32_t)lines, (uint32_t)(bot_clip << 1 | top_clip));
    } else walk_spans(span, lines, 1);
    unsigned line0 = (unsigned)((int)y_first - lb);
    if (line0 + (unsigned)lines > GX_CL) return;     /* (cannot happen: the walk is within the window) */
    /* trim (edge marking off, opaque polygons; gx_render_bin says when): the context lines above and below the bin
     * are walked as before (the spans' values depend on where the walk starts) but not rasterized. Nothing reads
     * them then (the resolve and the output take the bin's lines; edge marking, their reader, is off), and an opaque
     * polygon's pixels in the bin's lines do not depend on its batches (the flat colour is every line's colour, the
     * white-line shortcut gives the same bits) */
    int nl = lines;
    if (trim & 1 && line0 == 0) { sp += 4; line0 = 1; nl--; }
    if (trim & 2 && line0 + (unsigned)nl == GX_CL) nl--;
    if (nl <= 0) return;
    uint8_t *rec = (uint8_t *)p->rec;
    if (gx_stats) {             /* render.gengis_stats: the span pixels rasterized (the counts: array 9's low halves) */
        const uint16_t *cnt = (const uint16_t *)(sp + 9 * GX_SPS);
        unsigned long n = 0;
        for (int k = 0; k < nl; k++) n += cnt[2 * k];
        if (flags & GXF_TRANSLUCENT) gx_px_trl += n; else gx_px_opa += n;
        gx_lines++;
    }
    if (defer) defer_poly(&layout_gx, H->ctx, sp, rec, 0, line0, (unsigned)nl, flags, vptr[0]);
    else f_run(&layout_gx, H->ctx, sp, rec, 0, line0, (unsigned)nl, flags, vptr[0], 0, 0);
}

/* dc zva: the 64-byte line at p (64-aligned) zeroed in the cache without a read (gx_zva: DCZID_EL0 says 64 bytes) */
#define GX_ZVA(p) __asm__ volatile("dc zva, %0" : : "r"(p) : "memory")

/* ---- clear: the clear colour, or the rear-plane bitmap ---- */
static void gx_clear(gx_t *H, const gx_frame *f, int y0) {
    uint32_t *col = (uint32_t *)H->ctx, *att = (uint32_t *)(H->ctx + GX_ATTR);
    if (!(f->d3 & (1u << 14))) {
        const uint32x4_t c = vdupq_n_u32(f->clear_color), a = vdupq_n_u32(f->clear_attr);
        if (gx_zva) {
            /* each 64-byte line zeroed by dc zva first (allocated in L1 without reading it from the shared cache
             * or DRAM: the bin context is far larger than L1, so the plain stores missed on every line), then
             * written: the same bytes. One buffer after the other (sequential streams) */
            for (int i = 0; i < GX_CL * GX_W; i += 16) {
                GX_ZVA(col + i);
                vst1q_u32(col + i, c); vst1q_u32(col + i + 4, c); vst1q_u32(col + i + 8, c); vst1q_u32(col + i + 12, c);
            }
            for (int i = 0; i < GX_CL * GX_W; i += 16) {
                GX_ZVA(att + i);
                vst1q_u32(att + i, a); vst1q_u32(att + i + 4, a); vst1q_u32(att + i + 8, a); vst1q_u32(att + i + 12, a);
            }
            return;
        }
        for (int i = 0; i < GX_CL * GX_W; i += 4) { vst1q_u32(col + i, c); vst1q_u32(att + i, a); }
        return;
    }
    const uint16_t *ci = f->rear_col, *di = f->rear_dep;
    const uint32_t idattr = f->clear_attr & 0x3f000000;
    for (int l = 0; l < GX_CL; l++) {
        int yl = y0 + l;
        if (yl < 0) yl = 0;
        const unsigned row = ((unsigned)(yl / GX_SCALE + f->rear_y) & 0xff) << 8;
        uint32_t *c = col + l * GX_W, *a = att + l * GX_W;
        for (int i = 0; i < GX_W; i++) {
            const unsigned ix = row + ((f->rear_x + i / GX_SCALE) & 0xff);
            const uint16_t cv = ci[ix], d = di[ix];
            c[i] = gx_expand555(cv) | ((cv >> 15) & 1) * 31u << 24 | (uint32_t)(d >> 15) << 31;
            a[i] = ((d & 0x7fffu) << 9) | idattr;
        }
    }
}

/* ---- the resolve: fog and edge marking over the bin's lines (gx_resolve.h: hr.c's at 256 pixels) ---- */
#include "gx_resolve.h"

static const uint32_t *gx_resolve(gx_t *H, const gx_frame *f) {
    const uint32_t d3 = f->d3;
    const int edges = (d3 >> 5) & 1;
    int fog = (d3 >> 7) & 1 ? ((d3 >> 6) & 1 ? 2 : 1) : 0;     /* 1 full, 2 alpha only */
    if (fog && !U32(H->ctx, GX_HDR + 0x14)) fog = 0;            /* no fog-flagged pixel was drawn */
    uint32_t params = 0;
    if (fog) { uint32_t sh = (d3 >> 8) & 0xf; params = sh | ((f->fog_offset & 0x7fff) + (0x400u >> sh)) << 16; }
    const int zb = fog ? fog_zero_b2(f->fog_table, params) : -1;
#define COL(l) ((uint32_t *)(H->ctx + (l) * GX_LSTRIDE))
#define ATT(l) ((uint32_t *)(H->ctx + GX_ATTR + (l) * GX_LSTRIDE))
    _Static_assert(GX_BL % 2 == 0, "the resolve takes the bin's lines in pairs");
    if (edges && fog) {
        edge_tplanes(H->tp, ATT(0), 1); edge_tplanes(H->tp + (GX_CL - 1) * GX_W, ATT(GX_CL - 1), 1);
        for (int k = 1; k <= 2; k++) fog_line_zbp(COL(k), ATT(k), f->fog_table, params, f->fog_color, zb, H->tp + k * GX_W, fog == 1);
        for (int l = 1; l <= GX_BL; l += 2) {
            if (l + 2 <= GX_BL)
                for (int k = l + 2; k <= l + 3; k++)
                    fog_line_zbp(COL(k), ATT(k), f->fog_table, params, f->fog_color, zb, H->tp + k * GX_W, fog == 1);
            edge_lines(COL(l), COL(l + 1), ATT(l - 1), ATT(l), ATT(l + 1), ATT(l + 2), H->tp + (l - 1) * GX_W, f->clear_attr, f->edge);
        }
        return COL(1);
    }
    if (edges) edge_tplanes(H->tp, ATT(0), GX_CL);
    for (int l = 1; l <= GX_BL; l += 2) {
        if (fog) for (int k = l; k <= l + 1; k++) fog_line_zb(COL(k), ATT(k), f->fog_table, params, f->fog_color, zb, fog == 1);
        if (edges) edge_lines(COL(l), COL(l + 1), ATT(l - 1), ATT(l), ATT(l + 1), ATT(l + 2), H->tp + (l - 1) * GX_W, f->clear_attr, f->edge);
    }
    return COL(1);
#undef COL
#undef ATT
}

/* ---- a bin ---- */
#if GX_SUB > 1
static void gx_render_sub(const gx_frame *f, unsigned bin, uint32_t *out)
#else
void gx_render_bin(const gx_frame *f, unsigned bin, uint32_t *out)
#endif
{
    gx_t *H = gx_get();
    if (H->gen != f->gen) { f_begin_frame(); H->gen = f->gen; }     /* (the texture entries are the frame's) */
    U64(H->ctx, GX_HDR) = (uint64_t)(uintptr_t)gx_sys; U64(H->ctx, GX_HDR + 8) = (uint64_t)(uintptr_t)gx_geom;
    const unsigned hy0 = bin * GX_BL;
    const int lb = (int)hy0 - 1;                                    /* context line l is frame line lb + l */
    const unsigned bin_top = hy0 ? hy0 - 1 : 0;
    unsigned bin_bot = hy0 + GX_BL + 1;
    if (bin_bot > GX_H) bin_bot = GX_H;
    gx_clear(H, f, lb);
    U32(H->ctx, GX_HDR + 0x10) = 0xffffffffu; U32(H->ctx, GX_HDR + 0x14) = 0;
    U64(H->ctx, GX_HDR + 0x18) = ~0ull;                             /* the shadow line mask: all lines clean */
    const int defer = gx_defer && !gx_ref;
    const unsigned nopa = f->npolys - f->ntranslucent;
    const uint32_t *yr = f->yr;                                     /* ytop | ybot << 16: one cache line a 16 polygons */
    /* the opaque polygons' trim (gx_polygon): with edge marking off, the context line above the bin always (it only
     * feeds what is below it to the line above), the one below unless a translucent polygon reaches it (its first
     * pixel's new id there is the id quirk's for the bin's last line, and it depends on that line's depths) */
    unsigned trim = 0;
    if (gx_trim && !((f->d3 >> 5) & 1) && !defer) {
        trim = hy0 ? 1 : 0;
        const unsigned below = hy0 + GX_BL;                          /* the frame line of the context line below */
        if (bin_bot == below + 1) {
            trim |= 2;
            for (unsigned i = nopa; i < f->npolys; i++)
                if ((yr[i] >> 16) > below && (yr[i] & 0xffff) <= below) { trim &= ~2u; break; }
        }
    }
    for (unsigned i = 0; i < nopa; i++) {
        const uint32_t y = yr[i];
        if ((y >> 16) <= bin_top || (y & 0xffff) >= bin_bot) continue;
        const gx_poly *p = &f->polys[i];
        const int d = defer && !((U32(p->rec, 4) >> 4) & 3);       /* modulate-shaded ones are deferred */
        if (defer && !d) defer_flush(&layout_gx, H->ctx);
        gx_polygon(H, p, bin_top, bin_bot, lb, f->d3, d, trim);
    }
    if (defer) defer_flush(&layout_gx, H->ctx);
    if (f->ntranslucent) {
        memset(H->ctx + GX_ID, 0xff, GX_CL * GX_W);
        for (unsigned i = nopa; i < f->npolys; i++) {
            const uint32_t y = yr[i];
            if ((y >> 16) <= bin_top || (y & 0xffff) >= bin_bot) continue;
            gx_polygon(H, &f->polys[i], bin_top, bin_bot, lb, f->d3, 0, 0);
        }
    }
    const uint32_t *res = gx_resolve(H, f);
    /* the layer's format: alpha 5 bits, no flags */
    const uint32x4_t m = vdupq_n_u32(0x1f3f3f3f);
#if GX_SCALE == 1
    for (int i = 0; i < GX_BL * GX_W; i += 4) vst1q_u32(out + i, vandq_u32(vld1q_u32(res + i), m));
#elif GX_SCALE == 4
    /* each row as its pixels 4k, 4k + 1, 4k + 2, 4k + 3: the 2D compositor's sixteen 256-pixel lines of a DS line */
    for (int l = 0; l < GX_BL; l++) {
        const uint32_t *r = res + l * GX_W;
        uint32_t *o0 = out + l * GX_W;
        if (gx_zva && !gx_ref)  /* (the quarters' 64-byte lines allocated without a read, then written: the same bytes) */
            for (int i = 0; i < GX_W; i += 64) {
                for (int k = 0; k < 4; k++) GX_ZVA(o0 + k * (GX_W / 4) + i / 4);
                for (int j = i; j < i + 64; j += 16) {
                    const uint32x4x4_t v = vld4q_u32(r + j);
                    for (int k = 0; k < 4; k++) vst1q_u32(o0 + k * (GX_W / 4) + j / 4, vandq_u32(v.val[k], m));
                }
            }
        else
        for (int i = 0; i < GX_W; i += 16) {
            const uint32x4x4_t v = vld4q_u32(r + i);
            for (int k = 0; k < 4; k++) vst1q_u32(o0 + k * (GX_W / 4) + i / 4, vandq_u32(v.val[k], m));
        }
    }
#elif GX_SCALE == 3
    /* each row as its pixels 3k, then 3k + 1, then 3k + 2: the 2D compositor's nine 256-pixel lines of a DS line */
    for (int l = 0; l < GX_BL; l++) {
        const uint32_t *r = res + l * GX_W;
        uint32_t *e = out + l * GX_W, *o = e + GX_W / 3, *t = o + GX_W / 3;
        for (int i = 0; i < GX_W; i += 12) {
            const uint32x4x3_t v = vld3q_u32(r + i);
            vst1q_u32(e + i / 3, vandq_u32(v.val[0], m));
            vst1q_u32(o + i / 3, vandq_u32(v.val[1], m));
            vst1q_u32(t + i / 3, vandq_u32(v.val[2], m));
        }
    }
#else
    /* each row as its even pixels, then its odd ones: the 2D compositor's four 256-pixel lines of a DS line */
    for (int l = 0; l < GX_BL; l++) {
        const uint32_t *r = res + l * GX_W;
        uint32_t *e = out + l * GX_W, *o = e + GX_W / 2;
        if (gx_zva)         /* (out's rows are 64-byte aligned: OutBuf; gx_clear says why) */
            for (int i = 0; i < GX_W; i += 32) {
                GX_ZVA(e + i / 2); GX_ZVA(o + i / 2);
                for (int j = i; j < i + 32; j += 8) {
                    const uint32x4x2_t v = vld2q_u32(r + j);
                    vst1q_u32(e + j / 2, vandq_u32(v.val[0], m));
                    vst1q_u32(o + j / 2, vandq_u32(v.val[1], m));
                }
            }
        else
            for (int i = 0; i < GX_W; i += 8) {
                const uint32x4x2_t v = vld2q_u32(r + i);
                vst1q_u32(e + i / 2, vandq_u32(v.val[0], m));
                vst1q_u32(o + i / 2, vandq_u32(v.val[1], m));
            }
    }
#endif
}

#if GX_SUB > 1
/* a frame bin: its GX_SUB bins in turn */
void gx_render_bin(const gx_frame *f, unsigned bin, uint32_t *out) {
    for (unsigned s = 0; s < GX_SUB; s++) gx_render_sub(f, bin * GX_SUB + s, out + s * GX_BL * GX_W);
}
#endif

#if GX_SCALE == 1
/* fused.c's stage-by-stage path for shadow polygons at 2x (b0.c, DraStic's scratch buffers): the 1x layout is a
 * hi-res one, whose shadow polygons take the scalar line path, so it is never called */
void b0_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                 unsigned flags, uint8_t *v0) {
    (void)ctx; (void)spans; (void)poly; (void)buf; (void)line0; (void)nlines; (void)flags; (void)v0;
    abort();
}
#endif
