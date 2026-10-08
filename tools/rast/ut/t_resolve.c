/* t_resolve.c: bin resolve / fog / edge marking ports (src/rast/spec/resolve.c) vs DraStic's originals, and the NEON
 * resolve with the compositor's visibility table (src/rast/res2.c) vs DraStic's resolve and the visibility port, and
 * res2.c's fog and edge-marking resolves vs DraStic's drivers.
 * run.sh t_resolve.c ../../../src/rast/spec/resolve.c ../../../src/rast/spec/composite.c ../../../src/rast/res2.c
 *        ../../../src/rast/res2_line.S */
#include "ut.h"
#include "spec/resolve.h"
#include "spec/composite.h"
#include "res2.h"

#define CTX_SZ  0x24100
#define SYS_SZ  0x34f000
#define GEOM_SZ 0xa000
#define OUT_SZ  (12 * 0x10000)
#define GAPS_LO 0x32db40
#define GAPS_HI 0x34eb40
#define VB_OFS  0x1056c0

typedef void (*resolve_fn)(void *, void *);
typedef void (*weights_fn)(const uint32_t *, uint8_t *, const uint8_t *, uint32_t);
typedef void (*mod_fn)(void *, const uint32_t *, const uint8_t *, uint32_t);
typedef void (*mark_fn)(void *, const uint32_t *, const uint8_t *, const uint8_t *);
typedef void (*ident_fn)(uint8_t *, const uint32_t *, const uint32_t *, const uint32_t *, uint32_t);
typedef void (*ident2_fn)(uint8_t *, const uint32_t *, const uint32_t *, uint32_t);
typedef void (*drv_fn)(void *, void *, uint32_t);
typedef void (*gaps_fn)(void *);

static uint8_t *ctx[2], *sys[2], *geom[2], *outf[2];

/* attribute word: random, or structured to give ties / few polygon ids / few depths */
static uint32_t rnd_attr(int mode) {
    switch (mode) {
    case 0: return (uint32_t)rnd64();
    case 1: return rnd(4) << 24 | rnd(2) << 30 | rnd(2) << 31 | rnd(4) << 16 | rnd(2) << 8 | rnd(2);
    case 2: return (rnd(2) ? 0x40000000u : 0) | rnd(64) << 24 | (rnd(3) * 0x3fff) << 9 | rnd(512);
    default: return rnd(2) ? 0x7f001234u : (uint32_t)rnd64() & 0x7fffffff;
    }
}
static void fill_attr(uint32_t *a, size_t n) {
    int mode = rnd(4);
    for (size_t i = 0; i < n; i++) a[i] = (i && rnd(3) == 0) ? a[i - 1] : rnd_attr(mode);
}
static void fill_color(uint32_t *c, size_t n) {
    int mode = rnd(3);
    for (size_t i = 0; i < n; i++)
        c[i] = mode == 0 ? (uint32_t)rnd64() : (rnd(64) | rnd(64) << 8 | rnd(64) << 16 | rnd(32) << 24 | rnd(2) << 31);
}
static void fill_weights(uint8_t *w, size_t n) {
    int mode = rnd(3);
    for (size_t i = 0; i < n; i++) w[i] = mode == 0 ? rnd(256) : mode == 1 ? rnd(128) : (rnd(2) ? 127 : rnd(2) ? 0 : rnd(128));
}
static void fill_edges(uint8_t *e, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (rnd(4)) {
        case 0: e[i] = rnd(256); break;
        case 1: e[i] = rnd(16); break;
        case 2: e[i] = 0xff; break;
        default: e[i] = 120 + rnd(20); break;
        }
    }
}
static void fill_fog_table(uint8_t *t) {
    if (rnd(2)) { rndfill(t, 64); return; }
    uint8_t v = rnd(32);
    for (int i = 0; i < 32; i++) { t[i] = v; v += rnd(8); if (v > 127) v = 127; }
    for (int i = 0; i < 31; i++) t[32 + i] = t[i + 1] - t[i];
    t[63] = rnd(2) ? 0 : rnd(256);
}
static uint32_t fog_params_rnd(void) {
    if (rnd(4) == 0) return (uint32_t)rnd64();
    uint32_t sh = rnd(16);
    return sh | (((uint32_t)rnd(0x8000) + (0x400u >> sh)) << 16);
}

static void set_ptrs(int k) {
    *(uint64_t *)(ctx[k] + 0x24000) = (uint64_t)(uintptr_t)sys[k];
    *(uint64_t *)(ctx[k] + 0x24008) = (uint64_t)(uintptr_t)geom[k];
    *(uint64_t *)(sys[k] + 0x34eb58) = (uint64_t)(uintptr_t)outf[k];
    *(uint64_t *)(sys[k] + 0x2c1748) = (uint64_t)(uintptr_t)geom[k];
}
/* randomize the structures the drivers read (copy 0), then mirror to copy 1 */
static void setup(void) {
    fill_color((uint32_t *)ctx[0], 32 * 512);
    fill_attr((uint32_t *)(ctx[0] + 0x10000), 32 * 512);
    rndfill(ctx[0] + 0x20000, CTX_SZ - 0x20000);
    *(uint32_t *)(ctx[0] + 0x24014) = rnd(4) ? 1 : 0;
    rndfill(geom[0] + 0x9900, 0x200);
    fill_fog_table(geom[0] + 0x9974);
    for (int i = 0; i < 24; i++) if (rnd(2)) geom[0][0x99b4 + i] &= 0x3f;
    rndfill(sys[0] + GAPS_LO, GAPS_HI - GAPS_LO + 0x20);
    fill_attr((uint32_t *)(sys[0] + GAPS_LO), 11 * 0x800);
    fill_color((uint32_t *)(sys[0] + 0x343b40), 11 * 0x400);
    *(uint32_t *)(sys[0] + 0x34eb40) &= rnd(2) ? 0xffffffffu : 0xffff;
    *(uint32_t *)(sys[0] + 0x34eb50) = rnd(4) ? (uint32_t)-1 : 0;
    if (rnd(4) == 0) *(uint32_t *)(sys[0] + 0x34eb4c) = rnd_attr(rnd(4));
    rndfill(outf[0], OUT_SZ);
    memcpy(ctx[1], ctx[0], CTX_SZ);
    memcpy(geom[1] + 0x9900, geom[0] + 0x9900, 0x200);
    memcpy(sys[1] + GAPS_LO, sys[0] + GAPS_LO, GAPS_HI - GAPS_LO + 0x20);
    memcpy(outf[1], outf[0], OUT_SZ);
    for (int k = 0; k < 2; k++) set_ptrs(k);
}
static int check(const char *what) {
    int f = ut_fail;
    ut_cmp(what, outf[0], outf[1], OUT_SZ);
    ut_cmp(what, ctx[0], ctx[1], 0x24000);
    ut_cmp(what, ctx[0] + 0x24010, ctx[1] + 0x24010, CTX_SZ - 0x24010);
    ut_cmp(what, sys[0] + GAPS_LO, sys[1] + GAPS_LO, GAPS_HI - GAPS_LO + 0x10);
    ut_cmp(what, geom[0] + 0x9900, geom[1] + 0x9900, 0x200);
    return ut_fail != f;
}

static void test_leaves(int n) {
    static uint32_t a0[512], a1[512], a2[512], c[2][512 + 16], d[2][512 + 16];
    static uint8_t w[2][512 + 32], tbl[64], ec[24], e[2][512 + 32], o[2][0x800 + 64];
    for (int it = 0; it < n && ut_fail < 10; it++) {
        /* weights */
        fill_attr(a0, 512); fill_fog_table(tbl);
        uint32_t p = fog_params_rnd();
        memset(w, 0x5a, sizeof w);
        DS(weights_fn, 0x9cf00)(a0, w[0], tbl, p);
        spec_video_3d_fog_calculate_weights_4x(a0, w[1], tbl, p);
        ut_cmp("fog_calculate_weights", w[0], w[1], sizeof w[0]);

        /* modulate (4 variants), intermediate both in place and out of place */
        fill_color(c[0], 512); memcpy(c[1], c[0], sizeof c[0]);
        fill_weights(w[0], 512);
        uint32_t fc = rnd(2) ? (uint32_t)rnd64() : (rnd(64) | rnd(64) << 8 | rnd(64) << 16 | rnd(32) << 24);
        static const uint32_t moff[4] = {0x9cff8, 0x9d0b0, 0x9d18c, 0x9d220};
        static const char *mname[4] = {"modulate_full_intermediate", "modulate_full_resolve",
                                       "modulate_alpha_intermediate", "modulate_alpha_resolve"};
        for (int m = 0; m < 4; m++) {
            memset(o, 0xa5, sizeof o);
            memcpy(d[0], c[0], sizeof d[0]); memcpy(d[1], c[0], sizeof d[1]);
            int inplace = rnd(2);
            void *dst0 = (m & 1) ? (void *)o[0] : inplace ? (void *)d[0] : (void *)o[0];
            void *dst1 = (m & 1) ? (void *)o[1] : inplace ? (void *)d[1] : (void *)o[1];
            DS(mod_fn, moff[m])(dst0, d[0], w[0], fc);
            switch (m) {
            case 0: spec_video_3d_fog_modulate_full_intermediate_4x(dst1, d[1], w[0], fc); break;
            case 1: spec_video_3d_fog_modulate_full_resolve_4x(dst1, d[1], w[0], fc); break;
            case 2: spec_video_3d_fog_modulate_alpha_intermediate_4x(dst1, d[1], w[0], fc); break;
            default: spec_video_3d_fog_modulate_alpha_resolve_4x(dst1, d[1], w[0], fc); break;
            }
            ut_cmp(mname[m], o[0], o[1], sizeof o[0]);
            ut_cmp(mname[m], d[0], d[1], sizeof d[0]);
        }

        /* edge mark */
        fill_edges(e[0], 512); rndfill(ec, sizeof ec);
        memset(o, 0x3c, sizeof o);
        DS(mark_fn, 0x9d2d8)(o[0], c[0], e[0], ec);
        spec_video_3d_edge_mark_4x(o[1], c[0], e[0], ec);
        ut_cmp("edge_mark", o[0], o[1], sizeof o[0]);

        /* edge identify (3 variants) */
        int mode = rnd(4);
        for (int i = 0; i < 512; i++) { a0[i] = rnd_attr(mode); a1[i] = rnd(2) ? a0[i] : rnd_attr(mode); a2[i] = rnd(2) ? a1[i] : rnd_attr(mode); }
        uint32_t clr = rnd(2) ? rnd_attr(mode) : (uint32_t)rnd64();
        memset(e, 0x77, sizeof e);
        DS(ident_fn, 0x9d368)(e[0], a0, a1, a2, clr);
        spec_video_3d_edge_identify_4x(e[1], a0, a1, a2, clr);
        ut_cmp("edge_identify", e[0], e[1], sizeof e[0]);
        memset(e, 0x77, sizeof e);
        DS(ident2_fn, 0x9d6d0)(e[0], a1, a2, clr);
        spec_video_3d_edge_identify_top_4x(e[1], a1, a2, clr);
        ut_cmp("edge_identify_top", e[0], e[1], sizeof e[0]);
        memset(e, 0x77, sizeof e);
        DS(ident2_fn, 0x9da38)(e[0], a1, a0, clr);
        spec_video_3d_edge_identify_bottom_4x(e[1], a1, a0, clr);
        ut_cmp("edge_identify_bottom", e[0], e[1], sizeof e[0]);
    }
}

static void test_drivers(int n) {
    static const struct { const char *name; uint32_t off; drv_fn spec; int kind; } drv[] = {
        {"resolve_bin", 0x9ce78, 0, 0},
        {"resolve_bin_fog_full", 0x57c70, spec_video_3d_resolve_bin_fog_full_4x, 1},
        {"resolve_bin_fog_alpha", 0x58280, spec_video_3d_resolve_bin_fog_alpha_4x, 1},
        {"resolve_bin_edge_mark", 0x56420, spec_video_3d_resolve_bin_edge_mark_4x, 1},
        {"resolve_bin_edge_mark_fog_full", 0x57d90, spec_video_3d_resolve_bin_edge_mark_fog_full_4x, 1},
        {"resolve_bin_edge_mark_fog_alpha", 0x583a0, spec_video_3d_resolve_bin_edge_mark_fog_alpha_4x, 1},
        {"resolve_bin_edge_mark_gaps", 0x56630, (drv_fn)spec_video_3d_resolve_bin_edge_mark_gaps_4x, 2},
        {"resolve_bin_edge_mark_fog_full_gaps", 0x580c0, (drv_fn)spec_video_3d_resolve_bin_edge_mark_fog_full_gaps_4x, 2},
        {"resolve_bin_edge_mark_fog_alpha_gaps", 0x586d0, (drv_fn)spec_video_3d_resolve_bin_edge_mark_fog_alpha_gaps_4x, 2},
    };
    for (int it = 0; it < n && ut_fail < 10; it++) {
        for (unsigned d = 0; d < sizeof drv / sizeof drv[0]; d++) {
            setup();
            uint32_t bin = rnd(12);
            uint32_t ob = bin * 0x10000;
            switch (drv[d].kind) {
            case 0:
                DS(resolve_fn, drv[d].off)(outf[0] + ob, ctx[0]);
                spec_video_3d_resolve_bin_4x(outf[1] + ob, ctx[1]);
                break;
            case 1:
                DS(drv_fn, drv[d].off)(ctx[0], outf[0] + ob, bin);
                drv[d].spec(ctx[1], outf[1] + ob, bin);
                break;
            default:
                DS(gaps_fn, drv[d].off)(sys[0] + VB_OFS);
                ((gaps_fn)drv[d].spec)(sys[1] + VB_OFS);
                break;
            }
            if (check(drv[d].name)) fprintf(stderr, "  (iteration %d, bin %u)\n", it, bin);
        }
    }
}

/* res2_resolve vs video_3d_resolve_bin_asm_4x (the block) and render_scanline_set_3d_visibility's port (each of the
 * block's 64 half-rows); colour lines whose alphas are random, 0 or 31 only, all 0, all 31 (the three flags) */
static void test_res2(int n) {
    static uint8_t bits[64][32], flags[64], rbits[32];
    for (int it = 0; it < n && ut_fail < 10; it++) {
        setup();
        uint32_t *c = (uint32_t *)ctx[0];
        for (int y = 0; y < 32; y++) {
            int mode = rnd(5);
            for (int x = 0; x < 512; x++) {
                uint32_t v = c[y * 512 + x], a = (v >> 24) & 0x1f;
                if (mode == 1) a = rnd(2) ? 31 : 0;
                else if (mode == 2) a = 0;
                else if (mode == 3) a = 31;
                else if (mode == 4) a = rnd(8) ? (rnd(2) ? 31 : 0) : rnd(32);
                c[y * 512 + x] = (v & 0xe0ffffffu) | a << 24;
            }
        }
        uint32_t bin = rnd(12), ob = bin * 0x10000;
        memset(bits, 0x5a, sizeof bits); memset(flags, 0x5a, sizeof flags);
        DS(resolve_fn, 0x9ce78)(outf[0] + ob, ctx[0]);
        int tab = rnd(4) != 0;
        res2_resolve(outf[1] + ob, (const uint32_t *)ctx[0], tab ? bits : 0, tab ? flags : 0);
        if (ut_cmp("res2_resolve", outf[0], outf[1], OUT_SZ)) fprintf(stderr, "  (iteration %d, table %d)\n", it, tab);
        if (!tab) continue;
        for (int h = 0; h < 64; h++) {
            uint32_t r = spec_render_scanline_set_3d_visibility(rbits, (const uint32_t *)(outf[0] + ob + h * 0x400));
            if (ut_cmp("res2_resolve bitmap", rbits, bits[h], 32) || r != flags[h]) {
                if (r != flags[h]) { fprintf(stderr, "FAIL res2_resolve flag: half-row %d %#x, port %#x\n", h, flags[h], r); ut_fail++; }
                fprintf(stderr, "  (iteration %d, half-row %d)\n", it, h);
                break;
            }
        }
    }
}

/* res2_resolve_fx (the fog and edge-marking resolves fused, res2.c) vs DraStic's drivers: the output block, the gap
 * buffers and the attribute lines (the colour lines are not compared: res2.c fogs and marks them in place), with and
 * without the table; the table entries of the written half-rows vs the visibility port. Random contexts as setup()'s
 * (attributes with ties, few ids and depths, or polygon ids in rectangles; the fog flag random, or clear in whole
 * 32-pixel steps; fog tables (zero at small depths or everywhere: steps whose weights are all 0), fog
 * colours and offsets, DISP3DCNT's fog shift, the clear attribute and the edge colours random; fog on or off by the
 * context and system flags), every bin (bin 0's top line, bin 11's bottom line, the gap copies of the others). */
static void test_fx(int n) {
    static const struct { const char *name; uint32_t off; unsigned m; } drv[] = {
        {"res2_resolve_fx fog_full", 0x57c70, 2}, {"res2_resolve_fx fog_alpha", 0x58280, 3},
        {"res2_resolve_fx edge_mark", 0x56420, 4}, {"res2_resolve_fx edge_mark (5)", 0x56420, 5},
        {"res2_resolve_fx edge_mark_fog_full", 0x57d90, 6}, {"res2_resolve_fx edge_mark_fog_alpha", 0x583a0, 7},
    };
    static uint8_t bits[64][32], flags[64], rbits[32];
    for (int it = 0; it < n && ut_fail < 10; it++) {
        for (unsigned d = 0; d < sizeof drv / sizeof drv[0]; d++) {
            setup();
            if (rnd(3) == 0) {                                  /* steps of 32 pixels without a fog flag */
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 512; x += 32)
                        if (rnd(2)) for (int i = 0; i < 32; i++) ((uint32_t *)ctx[0])[y * 512 + x + i] &= 0x7fffffffu;
                memcpy(ctx[1], ctx[0], 0x10000);
            }
            if (rnd(2)) {                                       /* polygon ids in rectangles (steps without an id edge) */
                uint32_t *a = (uint32_t *)(ctx[0] + 0x10000), ids[4][4];
                int xs = 3 + rnd(6), ys = rnd(5);
                for (int i = 0; i < 16; i++) ids[i >> 2][i & 3] = rnd(64) << 24 | rnd(4) << 30;
                if (rnd(2)) ids[rnd(4)][rnd(4)] = *(uint32_t *)(sys[0] + 0x34eb4c) & 0xff000000u;
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 512; x++)
                        a[y * 512 + x] = (a[y * 512 + x] & 0xffffff) | ids[(y >> ys) & 3][(x >> xs) & 3];
                memcpy(ctx[1] + 0x10000, ctx[0] + 0x10000, 0x10000);
            }
            if (rnd(4) == 0) {                                  /* weights 0 at small depths, or everywhere */
                int n0 = rnd(2) ? 64 : 1 + rnd(4);
                for (int k = 0; k < 2; k++) memset(geom[k] + 0x9974, 0, n0), memset(geom[k] + 0x9994, 0, n0 < 32 ? n0 : 32);
            }
            uint32_t bin = rnd(12), ob = bin * 0x10000;
            int tab = rnd(4) != 0;
            memset(bits, 0x5a, sizeof bits); memset(flags, 0x5a, sizeof flags);
            DS(drv_fn, drv[d].off)(ctx[0], outf[0] + ob, bin);
            int r = res2_resolve_fx(ctx[1], outf[1] + ob, bin, drv[d].m, tab ? bits : 0, tab ? flags : 0);
            int f = ut_fail;
            ut_cmp(drv[d].name, outf[0], outf[1], OUT_SZ);
            ut_cmp(drv[d].name, ctx[0] + 0x10000, ctx[1] + 0x10000, 0x14000);
            ut_cmp(drv[d].name, ctx[0] + 0x24010, ctx[1] + 0x24010, CTX_SZ - 0x24010);
            ut_cmp(drv[d].name, sys[0] + GAPS_LO, sys[1] + GAPS_LO, GAPS_HI - GAPS_LO + 0x10);
            if (ut_fail != f) fprintf(stderr, "  (iteration %d, bin %u, table %d)\n", it, bin, tab);
            if (!tab || !r) continue;
            int edge = drv[d].m & 4, h0 = edge && bin ? 2 : 0, h1 = edge && bin != 11 ? 62 : 64;
            for (int h = h0; h < h1; h++) {
                uint32_t rv = spec_render_scanline_set_3d_visibility(rbits, (const uint32_t *)(outf[0] + ob + h * 0x400));
                if (ut_cmp("res2_resolve_fx bitmap", rbits, bits[h], 32) || rv != flags[h]) {
                    if (rv != flags[h]) { fprintf(stderr, "FAIL res2_resolve_fx flag: half-row %d %#x, port %#x\n", h, flags[h], rv); ut_fail++; }
                    fprintf(stderr, "  (%s, iteration %d, bin %u, half-row %d)\n", drv[d].name, it, bin, h);
                    break;
                }
            }
        }
    }
}

/* res2_vis_bin (comp_bin's entries of a written block) vs the visibility port: alpha bytes of any value (the fog
 * resolves keep bits 5 and 6), 0 or 31, all 0, all 31, few translucent */
static void test_vis(int n) {
    static uint8_t bits[64][32], flags[64], rbits[32];
    for (int it = 0; it < n && ut_fail < 10; it++) {
        uint32_t *b = (uint32_t *)outf[0];
        for (int h = 0; h < 64; h++) {
            int mode = rnd(6);
            for (int x = 0; x < 256; x++) {
                uint32_t v = (uint32_t)rnd64(), a = v >> 24;
                if (mode == 1) a = rnd(2) ? 31 : 0;
                else if (mode == 2) a = 0;
                else if (mode == 3) a = 31;
                else if (mode == 4) a = rnd(16) ? (rnd(2) ? 31 : 0) : rnd(256);
                else if (mode == 5) a &= 0x1f;
                b[h * 256 + x] = (v & 0xffffff) | a << 24;
            }
        }
        memset(bits, 0x5a, sizeof bits); memset(flags, 0x5a, sizeof flags);
        res2_vis_bin(outf[0], bits, flags);
        for (int h = 0; h < 64; h++) {
            uint32_t r = spec_render_scanline_set_3d_visibility(rbits, b + h * 256);
            if (ut_cmp("res2_vis_bin bitmap", rbits, bits[h], 32) || r != flags[h]) {
                if (r != flags[h]) { fprintf(stderr, "FAIL res2_vis_bin flag: half-row %d %#x, port %#x\n", h, flags[h], r); ut_fail++; }
                fprintf(stderr, "  (iteration %d, half-row %d)\n", it, h);
                break;
            }
        }
    }
}

void ut_main(void) {
    for (int k = 0; k < 2; k++) {
        ctx[k] = calloc(1, CTX_SZ); sys[k] = calloc(1, SYS_SZ); geom[k] = calloc(1, GEOM_SZ); outf[k] = calloc(1, OUT_SZ);
    }
    rndfill(sys[0], SYS_SZ); memcpy(sys[1], sys[0], SYS_SZ);
    rndfill(geom[0], GEOM_SZ); memcpy(geom[1], geom[0], GEOM_SZ);
    const char *s = getenv("UT_N");
    int n = s ? atoi(s) : 1000;
    test_leaves(n * 4);
    test_drivers(n);
    test_res2(n);
    test_fx(n);
    test_vis(n);
    set_ptrs(0); set_ptrs(1);
    *(uint64_t *)(sys[1] + 0x34eb58) = *(uint64_t *)(sys[0] + 0x34eb58);
    *(uint64_t *)(sys[1] + 0x2c1748) = *(uint64_t *)(sys[0] + 0x2c1748);
    ut_cmp("sys (whole)", sys[0], sys[1], SYS_SZ);
    ut_cmp("geom (whole)", geom[0], geom[1], GEOM_SZ);
    fprintf(stderr, "t_resolve: %d leaf iterations, %d driver iterations x 9 drivers, %d res2, %d fx (x 6 modes) and %d vis iterations\n", n * 4, n, n, n, n);
}
