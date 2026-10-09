/* rast.c: our 3D rasterizer in place of DraStic's (video_3d_render_bins_4x and everything under it).
 *
 * DraStic calls video_3d_render_bins_4x(ctx) once per render thread per frame. We patch its entry with a branch to
 * rast_render_bins(). RAST env: "off" leaves DraStic alone, "ours" renders with ours only, "diff" (development)
 * renders every bin with both, compares the resolved output and keeps DraStic's, logging any mismatch. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ds3d.h"
#include "rast.h"
#include "fused.h"
#include "comp.h"
#include "res2.h"

static int mode;                 /* 0 off, 1 ours, 2 diff */
static void (*orig_render_bins)(uint8_t *ctx);
static __thread int in_ours;
static int pipe_sel = 3, pdiff, pdiff_strict, rast_scale = 2;   /* RAST_SCALE: 2 (DraStic's hi-res, exact) or 3 (hr.c) */   /* RAST_PDIFF_STRICT: 1 compares the 5-mod-8 quirk pixel too, 2 the 1-pixel batches too */  /* RAST_PIPE: 0 b0 stage by stage, 1 fused scalar, 2 fused C NEON, 3 assembly kernels */

#define U8(p, o)  (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)U64(p, o))
#define DSFN(type, off) ((type)(ds_base + (off)))

/* ---- the bin loop (Phase A: our structure, DraStic's polygon and resolve routines) ---- */

static void clear_bin(uint8_t *ctx, uint8_t *sys, uint8_t *geom, unsigned y0) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT), cattr = U32(sys, SYS_CLEAR_ATTR);
    uint32_t *col = (uint32_t *)(ctx + CTX_COLOR), *att = (uint32_t *)(ctx + CTX_ATTR);
    if (!(d3 & (1u << 14))) {                                   /* plain clear colour */
        uint32_t c = U32(sys, SYS_CLEAR_COLOR);
        for (int i = 0; i < 32 * 512; i++) col[i] = c, att[i] = cattr;
        return;
    }
    /* rear-plane bitmap: texture slots 2 (colour) and 3 (depth), scrolled by CLRIMAGE_OFFSET */
    const uint16_t *ci = (const uint16_t *)PTR(sys, SYS_CLRIMG_COL), *di = (const uint16_t *)PTR(sys, SYS_CLRIMG_DEP);
    uint16_t ofs = U16(geom, GEOM_CLRIMG_OFS);
    unsigned xo = ofs & 0xff, yl = y0 + (ofs >> 8);
    uint32_t idattr = cattr & 0x3f000000;
    for (int l = 0; l < 32; l++, yl++) {
        unsigned row = ((yl >> 1) & 0xff) << 8;
        uint32_t *c = col + l * 512, *a = att + l * 512;
        for (int i = 0; i < 256; i++) {
            unsigned idx = row + ((xo + i) & 0xff);
            uint32_t pc, pa;
            if (ci) pc = rast_pixel_embedded_alpha(ci[idx]);
            if (ci && di)  { uint16_t d = di[idx]; pc |= (uint32_t)(d >> 15) << 31; pa = ((d & 0x7fffu) << 9) | idattr; }
            else if (ci)   { pc |= 0x80000000u; pa = idattr | 0xfffe00; }
            else if (di)   { uint16_t d = di[idx]; pc = (uint32_t)(d >> 15) << 31; pa = ((d & 0x7fffu) << 9) | idattr; }
            else           { pc = 0x80000000u; pa = idattr | 0xfffe00; }
            c[2 * i] = c[2 * i + 1] = pc; a[2 * i] = a[2 * i + 1] = pa;
        }
    }
}

/* the bin's resolve into its output block and the compositor's visibility table entries for it, while the block is in
 * cache: res2.c's NEON forms of DraStic's resolves (video_3d_resolve_bin_asm_4x, and with fog and / or edge marking
 * its drivers' stages fused per line), the plain resolve's pass writing the block and the entries from the same
 * registers; the fog-only resolve's block (unmasked) then gets its entries from comp_bin() */
static void resolve_bin(uint8_t *ctx, uint8_t *sys, unsigned bin) {
    uint32_t d3 = U32(sys, SYS_DISP3DCNT);
    uint8_t *out = PTR(sys, SYS_OUTPUT) + (size_t)bin * BIN_BYTES;
    unsigned m = ((d3 >> 5) & 1) << 2 | ((d3 >> 6) & 3);       /* edge marking, fog alpha-only, fog */
    if (U8(ctx, CTX_NO_EDGE)) m &= ~4u;
    uint8_t (*bits)[32], *flags;
    int tab = comp_bin_table(sys, bin, &bits, &flags);
    if (m & 6 ? res2_resolve_fx(ctx, out, bin, m, tab ? bits : 0, tab ? flags : 0)
              : (res2_resolve(out, (const uint32_t *)(ctx + CTX_COLOR), tab ? bits : 0, tab ? flags : 0), 1)) {
        if (tab) comp_bin_done();
        return;
    }
    comp_bin(sys, bin, 1);
}

static __thread int in_defer;    /* hook_setup: the polygon goes to the deferred queue */
static int walk = 1;             /* RAST_WALK=0: DraStic's render_polygon_4x walks every polygon (else walk.c, most) */
static walk_setup_fn hook_setup;
static void render_list(uint8_t *ctx, const uint8_t *list, uint8_t *polys, uint8_t *verts, unsigned y0, int defer) {
    typedef void (*pfn)(void *, void *, void *, unsigned long, unsigned long);
    uint32_t n = U32(list, 0x1000);
    /* walk.c's setup: hook_setup's choice made once (it runs inside our bin loop, so in_ours is set) when nothing
     * per polygon decides it (deferred queueing, RAST_PDIFF); DraStic's own render_polygon_4x calls the hook */
    walk_setup_fn *setup = defer || pdiff ? hook_setup : pipe_sel ? f_setup_4x : b0_setup_4x;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *poly = polys + 32 * (size_t)((const uint16_t *)list)[i];
        /* deferred: modulate-shaded polygons queue up; the others (toon, highlight, shadow) flush and render now */
        if (defer) {
            in_defer = !((U32(poly, 4) >> 4) & 3);
            if (!in_defer) defer_flush(&layout_2x, ctx);
        }
        if (!walk || !walk_polygon_4x(ctx, poly, verts, y0, y0 + 32, setup))
            DSFN(pfn, DS_RENDER_POLYGON_4X)(ctx, poly, verts, y0, y0 + 32);
    }
    in_defer = 0;
    if (defer) defer_flush(&layout_2x, ctx);
}

/* RAST_STATS: opaque pixels written (shaded) vs opaque pixels finally covered (attr id != clear id): the
 * overdraw a deferred opaque pass would remove */
unsigned long st_written, st_covered; int rast_stats;
static void stats_bin(uint8_t *ctx, uint8_t *sys) {
    const uint32_t *att = (const uint32_t *)(ctx + CTX_ATTR);
    uint32_t cid = U32(sys, SYS_CLEAR_ATTR) & 0x3f000000;
    unsigned long cov = 0;
    for (int i = 0; i < 32 * 512; i++) cov += (att[i] & 0x3f000000) != cid;
    __atomic_fetch_add(&st_covered, cov, __ATOMIC_RELAXED);
}

static void render_bins(uint8_t *ctx) {
    uint8_t *sys = PTR(ctx, CTX_SYS), *geom = PTR(ctx, CTX_GEOM);
    unsigned stride = U8(ctx, CTX_BIN_STRIDE), nb = NBINS / stride;
    unsigned buf = U8(geom, GEOM_SWAP_BUF) ^ 1;
    uint8_t *verts = geom + GEOM_VERTS + buf * GEOM_VERTS_BUF;
    uint8_t *opa = geom + GEOM_POLYS_OPA + buf * GEOM_POLYS_BUF, *trl = geom + GEOM_POLYS_TRL + buf * GEOM_POLYS_BUF;
    uint32_t ntrl = U32(geom, GEOM_TRL_COUNT + buf * GEOM_POLYS_BUF);
    f_begin_frame();
    for (unsigned k = 0; k < nb; k++) {
        unsigned bin = U8(ctx, CTX_FIRST_BIN) + k * stride, y0 = bin * 32;
        clear_bin(ctx, sys, geom, y0);
        U64(ctx, CTX_LINEMASK) = 0xffffffffull;
        render_list(ctx, sys + SYS_BINS_OPAQUE + bin * BIN_LIST_SIZE, opa, verts, y0, rast_defer && pipe_sel == 3 && !pdiff);
        if (rast_stats) stats_bin(ctx, sys);
        if (ntrl) {
            memset(ctx + CTX_IDBUF, 0xff, 0x4000);
            render_list(ctx, sys + SYS_BINS_TRANSL + bin * BIN_LIST_SIZE, trl, verts, y0, 0);
        }
        resolve_bin(ctx, sys, bin);     /* and the compositor's visibility table, while the block is in cache */
    }
}

/* ---- diff mode ---- */

static pthread_mutex_t stat_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long st_bins, st_bad, st_calls;

static void diff_render_bins(uint8_t *ctx) {
    uint8_t *sys = PTR(ctx, CTX_SYS);
    unsigned stride = U8(ctx, CTX_BIN_STRIDE), nb = NBINS / stride, first = U8(ctx, CTX_FIRST_BIN);
    orig_render_bins(ctx);
    uint8_t *out = PTR(sys, SYS_OUTPUT);
    static __thread uint8_t *ref;
    if (!ref) ref = malloc(NBINS * BIN_BYTES);
    for (unsigned k = 0; k < nb; k++) { unsigned b = first + k * stride; memcpy(ref + b * BIN_BYTES, out + b * BIN_BYTES, BIN_BYTES); }
    in_ours = 1; render_bins(ctx); in_ours = 0;
    unsigned long bad = 0;
    for (unsigned k = 0; k < nb; k++) {
        unsigned b = first + k * stride;
        const uint32_t *r = (const uint32_t *)(ref + b * BIN_BYTES), *o = (const uint32_t *)(out + b * BIN_BYTES);
        unsigned nd = 0, fi = 0;
        for (unsigned i = 0; i < BIN_BYTES / 4; i++) if (r[i] != o[i]) { if (!nd++) fi = i; }
        if (nd) {
            bad++;
            pthread_mutex_lock(&stat_lock);
            if (st_bad < 40)
                fprintf(stderr, "[rast] call %lu bin %u: %u words differ, first word %u: drastic %08x ours %08x\n",
                        st_calls, b, nd, fi, r[fi], o[fi]);
            pthread_mutex_unlock(&stat_lock);
            memcpy(out + b * BIN_BYTES, ref + b * BIN_BYTES, BIN_BYTES);
            comp_bin(sys, b, 0);        /* the block changed after render_bins computed its entries */
        }
    }
    pthread_mutex_lock(&stat_lock);
    st_calls++; st_bins += nb; st_bad += bad;
    if (st_calls % 600 == 0) fprintf(stderr, "[rast] diff: %lu bins compared, %lu differ\n", st_bins, st_bad);
    if (rast_stats && st_calls % 600 == 0)
        fprintf(stderr, "[rast] opaque: %lu pixels shaded, %lu covered: overdraw %.2f\n", st_written, st_covered, (double)st_written / (st_covered ? st_covered : 1));
    pthread_mutex_unlock(&stat_lock);
}

/* RAST_DUMP=<dir>: every RAST_DUMP_EVERY (default 60) frames, the thread that renders bin 0 writes the output
 * frame as <dir>/fNNNNN.ppm (other threads' bins may be mid-frame) */
static const char *dump_dir;
static unsigned dump_every = 60;
static int alpha_hist;          /* RAST_ALPHAHIST=1: with each dump, a histogram of the output frame's byte 3 */
static void dump_frame(uint8_t *ctx) {
    static unsigned long n;
    if (U8(ctx, CTX_FIRST_BIN) != 0 || n++ % dump_every) return;
    const uint8_t *o = PTR(PTR(ctx, CTX_SYS), SYS_OUTPUT);
    if (alpha_hist) {
        unsigned long h[256] = { 0 };
        for (int y = 0; y < 384; y++) for (int x = 0; x < 512; x++) h[o[(2 * y + (x & 1)) * 0x400 + (x >> 1) * 4 + 3]]++;
        fprintf(stderr, "[rast] frame %lu clear colour %08x byte3:", n - 1, U32(PTR(ctx, CTX_SYS), SYS_CLEAR_COLOR));
        for (int v = 0; v < 256; v++) if (h[v]) fprintf(stderr, " %02x:%lu", v, h[v]);
        fprintf(stderr, "\n");
    }
    char fn[512]; snprintf(fn, sizeof fn, "%s/f%05lu.ppm", dump_dir, n - 1);
    FILE *f = fopen(fn, "wb"); if (!f) return;
    fprintf(f, "P6\n512 384\n255\n");
    for (int y = 0; y < 384; y++) for (int x = 0; x < 512; x++) {
        const uint8_t *p = o + (2 * y + (x & 1)) * 0x400 + (x >> 1) * 4;
        uint8_t px[3] = { (uint8_t)(p[0] << 2), (uint8_t)(p[1] << 2), (uint8_t)(p[2] << 2) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    if (!hr_frame) return;
    snprintf(fn, sizeof fn, "%s/h%05lu.ppm", dump_dir, n - 1);
    f = fopen(fn, "wb"); if (!f) return;
    fprintf(f, "P6\n768 576\n255\n");
    for (int i = 0; i < 768 * 576; i++) {
        uint32_t c = hr_frame[i];
        uint8_t px[3] = { (uint8_t)(c << 2), (uint8_t)(c >> 6), (uint8_t)(c >> 14) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static int print_frames;         /* RAST_FRAMES=1: the frame count every 10 frames (profiling runs) */
static void hook_entry(uint8_t *ctx) {
    static __thread int named;
    if (!named) { named = 1; pthread_setname_np(pthread_self(), "rast-3d"); }  /* the performance log's per-thread CPU */
    comp_bins_begin(PTR(ctx, CTX_SYS));
    if (mode == 2) diff_render_bins(ctx); else { in_ours = 1; (rast_scale == 3 ? hr_render_bins : render_bins)(ctx); in_ours = 0; }
    if (dump_dir) dump_frame(ctx);
    if (print_frames && U8(ctx, CTX_FIRST_BIN) == 0) {
        static unsigned long n;
        if (++n % 10 == 0) fprintf(stderr, "[rast] frames %lu\n", n);
    }
}

/* ---- install ---- */

static const uint32_t expect_bins[4] = { 0x91408001, 0xa9b17bfd, 0x52800183, 0x910003fd };
static const uint32_t expect_setup[4] = { 0xd10243ff, 0xa9017bfd, 0x910043fd, 0xa90253f3 };

/* render_polygon_setup_4x: ours while our bin loop runs, DraStic's otherwise (diff mode runs both) */
/* the 3x pipeline's vertex hook: the 3x screen coordinates from the same inputs, then DraStic's transform */
static void (*orig_persp)(uint8_t *, const uint32_t *, const uint32_t *);
static void hook_persp(uint8_t *geom, const uint32_t *recips, const uint32_t *shifts) {
    hr_vertices(geom, recips, shifts);
    orig_persp(geom, recips, shifts);
}
static const uint32_t expect_persp[4] = { 0x91402403, 0x79557864, 0x5280180d, 0xd280188e };

typedef void (*setup_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, unsigned, unsigned, unsigned, uint8_t *);
static setup_fn orig_setup;
static void hook_setup(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                       unsigned flags, uint8_t *v0) {
    if (!in_ours) { orig_setup(ctx, spans, poly, buf, line0, nlines, flags, v0); return; }
    if (in_defer) { defer_poly(&layout_2x, ctx, spans, poly, buf, line0, nlines, flags, v0); return; }
    if (!pdiff) { (pipe_sel ? f_setup_4x : b0_setup_4x)(ctx, spans, poly, buf, line0, nlines, flags, v0); return; }
    /* RAST_PDIFF: per polygon, run DraStic's and ours on the same buffers; report the first differences */
    static __thread uint8_t *save, *ref;
    if (!save) { save = malloc(0x24020); ref = malloc(0x24020); }
    uint8_t spans_copy[0x800];
    memcpy(spans_copy, spans, sizeof spans_copy);
    memcpy(save, ctx, 0x24020);
    orig_setup(ctx, spans, poly, buf, line0, nlines, flags, v0);
    memcpy(ref, ctx, 0x24020);
    memcpy(ctx, save, 0x24020);
    memcpy(spans, spans_copy, sizeof spans_copy);
    (pipe_sel ? f_setup_4x : b0_setup_4x)(ctx, spans, poly, buf, line0, nlines, flags, v0);
    if (flags & 1) {
        /* DraStic stores, as the last pixel's translucent id of a batch-ending line with 5 mod 8 pixels, a value
         * computed from stale scratch memory (render_polygon_4x's stack): not reproducible, so not compared */
        unsigned left = nlines, i = 0, line = line0;
        while (left) {
            while (left && !*(uint16_t *)(spans + 0x630 + 4 * i)) { i++; line++; left--; }
            if (!left) break;
            unsigned k = 0, n = 0;
            while (left) {
                unsigned c = *(uint16_t *)(spans + 0x630 + 4 * i);
                if (!c || n + c > 512) break;
                n += c; k++; i++; left--;
            }
            unsigned c = *(uint16_t *)(spans + 0x630 + 4 * (i - 1)), x = *(uint16_t *)(spans + 0x580 + 4 * (i - 1));
            if (c % 8 == 5 && pdiff_strict < 1) { unsigned o = 0x20000 + (line + k - 1) * 0x200 + x + c - 1; ctx[o] = ref[o]; }
            /* a 1-pixel batch: alpha_id_test's 32-byte mask store runs into the id array and ANDs it with a test
             * of stale bytes */
            if (n == 1 && pdiff_strict < 2) { unsigned o = 0x20000 + line * 0x200 + x; ctx[o] = ref[o]; }
            line += k;
        }
    }
    if (memcmp(ref, ctx, 0x24020)) {
        static int nrep;
        pthread_mutex_lock(&stat_lock);
        if (nrep++ < 30) {
            const char *what[3] = { "color", "attr", "id" };
            fprintf(stderr, "[pdiff] poly attr %08x flags %02x line0 %u nlines %u tex %p", *(uint32_t *)(poly + 4), flags, line0, nlines,
                    *(void **)(poly + 0x10));
            for (unsigned l = 0; l < nlines; l++) fprintf(stderr, " [%u:%u]", *(uint16_t *)(spans + 0x580 + 4 * l), *(uint16_t *)(spans + 0x630 + 4 * l));
            fprintf(stderr, "\n");
            int shown = 0;
            for (int b = 0; b < 3 && shown < 6; b++) {
                unsigned base = b == 0 ? 0 : b == 1 ? 0x10000 : 0x20000, sz = b == 2 ? 0x4000 : 0x10000, w = b == 2 ? 1 : 4;
                for (unsigned o = 0; o < sz && shown < 6; o += w)
                    if (memcmp(ref + base + o, ctx + base + o, w)) {
                        unsigned px = o / w, y = px / 512, x = px % 512;
                        uint32_t a = 0, c = 0; memcpy(&a, ref + base + o, w); memcpy(&c, ctx + base + o, w);
                        fprintf(stderr, "   %s line %u x %u: drastic %08x ours %08x (before %08x)\n", what[b], y, x, a, c,
                                b == 2 ? save[base + o] : *(uint32_t *)(save + base + o));
                        if (b == 2 && y + 1 >= line0 && y + 1 < line0 + nlines) {
                            unsigned l = y + 1 - line0, nx = *(uint16_t *)(spans + 0x580 + 4 * l);
                            unsigned oo = 0x20000 + (y + 1) * 0x200 + nx;
                            fprintf(stderr, "     next line %u x %u: before %02x drastic %02x ours %02x; this line ids before:", y + 1, nx, save[oo], ref[oo], ctx[oo]);
                            for (int q = -6; q <= 2; q++) fprintf(stderr, " %02x/%02x/%02x", save[base + o + q], ref[base + o + q], ctx[base + o + q]);
                            fprintf(stderr, "\n");
                        }
                        shown++;
                    }
            }
            if (memcmp(ref + 0x24000, ctx + 0x24000, 0x20)) fprintf(stderr, "   ctx header differs\n");
        }
        pthread_mutex_unlock(&stat_lock);
        memcpy(ctx, ref, 0x24020);
    }
}

/* the 3D resolution Gengis Engine draws at (2 or 3), 0 when it isn't drawing (off, or an unknown DraStic build);
 * cpugov.c keeps 3x's clock memory apart from 2x's */
int rast_active_scale(void) { return mode == 1 && orig_render_bins ? rast_scale : 0; }

__attribute__((constructor)) static void rast_init(void) {
    /* DSFLIP_RAST=1 (the handheld) or RAST=ours|diff (the simulator); off otherwise */
    const char *e = getenv("RAST"), *d = getenv("DSFLIP_RAST");
    mode = e ? (!strcmp(e, "ours") ? 1 : !strcmp(e, "diff") ? 2 : 0) : (d && *d == '1');
    if (!mode) return;
    if (!e) e = "ours";
    { const char *t = getenv("DSFLIP_RAST_TEXFILTER"); if (!t) t = getenv("RAST_TEXFILTER"); if (t) rast_texfilter = atoi(t); }
    /* deferred shading pays only when shading is expensive (bilinear filtering): on with it unless set */
    { const char *t = getenv("DSFLIP_RAST_DEFER"); if (!t) t = getenv("RAST_DEFER"); rast_defer = t ? atoi(t) : rast_texfilter != 0; }
    { const char *t = getenv("DSFLIP_RAST_SCALE"); if (!t) t = getenv("RAST_SCALE"); if (t) rast_scale = atoi(t) == 3 ? 3 : 2; }
    if (mode == 2) rast_scale = 2;
    hr_vcheck = getenv("RAST_VCHECK") != 0;
    hr_ipcheck = getenv("RAST_HRIPCHECK") != 0;
    if (getenv("RAST_HR_EDGES")) hr_edges = atoi(getenv("RAST_HR_EDGES"));
    dump_dir = getenv("RAST_DUMP");
    rast_stats = getenv("RAST_STATS") != 0;
    print_frames = getenv("RAST_FRAMES") != 0;
    alpha_hist = getenv("RAST_ALPHAHIST") != 0;
    pdiff = getenv("RAST_PDIFF") != 0;
    if (getenv("RAST_PDIFF_STRICT")) pdiff_strict = atoi(getenv("RAST_PDIFF_STRICT"));
    if (getenv("RAST_PIPE")) pipe_sel = atoi(getenv("RAST_PIPE"));
    if (getenv("RAST_WALK")) walk = atoi(getenv("RAST_WALK"));
    { extern int use_neon; use_neon = pipe_sel >= 1 ? pipe_sel : 1; }
    if (getenv("RAST_DUMP_EVERY")) dump_every = atoi(getenv("RAST_DUMP_EVERY"));
    ds_find_base();
    orig_render_bins = (void (*)(uint8_t *))rast_hook(DS_RENDER_BINS_4X, expect_bins, (void *)hook_entry);
    if (!orig_render_bins) { fprintf(stderr, "[rast] Gengis Engine: unknown DraStic build, not hooking\n"); return; }
    orig_setup = (setup_fn)rast_hook(DS_RENDER_POLYGON_SETUP_4X, expect_setup, (void *)hook_setup);
    if (rast_scale == 3) {
        orig_persp = (void (*)(uint8_t *, const uint32_t *, const uint32_t *))rast_hook(DS_PERSP_APPLY_HIRES, expect_persp, (void *)hook_persp);
        if (!orig_persp) { fprintf(stderr, "[rast] cannot hook the vertex transform, 3x off\n"); rast_scale = 2; }
        else if (dump_dir) hr_frame = calloc(768 * 576, 4);
        /* RAST_HRCHECK=1: the rows the bins wrote against the frame after DraStic's gap passes (needs RAST_COMP=2) */
        if (rast_scale == 3 && getenv("RAST_HRCHECK")) { hr_check = 1; comp_uf4_done = hr_check_frame; }
    }
    comp_init();                         /* the compositor's 3D visibility step (comp.c) */
    { Dl_info di; if (dladdr((void *)rast_init, &di)) fprintf(stderr, "[rast] librast base %p\n", di.dli_fbase); }
    fprintf(stderr, "[rast] Gengis Engine: hooked video_3d_render_bins_4x, mode %s, scale %d\n", e, rast_scale);
}
