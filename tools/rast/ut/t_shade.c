/* t_shade.c: render_polygon_shade / shade_untextured and the routines they call, alpha tests, fog flag, edge
 * marking, against src/rast/spec/shade.c. Run: run.sh t_shade.c ../../../src/rast/spec/shade.c */
#include "ut.h"
#include "spec/shade.h"

#define ITERS 4000
#define ASZ 0x2400

static uint8_t A[ASZ], B[ASZ];
static uint8_t *sysbuf, *geom;

static uint32_t gen_count(void) {
    switch (rnd(8)) {
    case 0: return rnd(4);
    case 1: return 16 * rndr(1, 32) + rndr(-1, 1);
    case 2: return 512 - rnd(3);
    default: return rnd(513);
    }
}
static uint32_t gen_pixel(int extreme) {
    if (extreme) return (uint32_t)rnd64();
    return rnd(64) | rnd(64) << 8 | rnd(64) << 16 | rnd(32) << 24 | (rnd(4) == 0) << 31;
}
static void fill_pixels(uint8_t *p, int n, int extreme) {
    for (int i = 0; i < n; i++) { uint32_t v = gen_pixel(extreme); memcpy(p + 4 * i, &v, 4); }
}
static void fill_bytes(uint8_t *p, int n, int lim) { for (int i = 0; i < n; i++) p[i] = rnd(lim); }
static void arena_init(void) { rndfill(A, ASZ); }
static void arena_sync(void) { memcpy(B, A, ASZ); }
static uint32_t gen_stride(uint32_t count) {
    uint32_t n = (2 * count + 29) & ~15u;
    if (rnd(4) == 0) n += 16 * rnd(8);
    return n;
}
static void fill_toon(int extreme) {
    uint8_t *t = geom + SHADE_GEOM_TOON;
    for (int i = 0; i < 96; i++) { uint32_t c = rnd(32); t[i] = extreme ? (uint8_t)rnd64() : (c ? 2 * c + 1 : 0); }
    rndfill(t + 96, 0x200);
}

typedef void (*shade_t)(const void *, const void *, const void *, uint32_t *, const uint32_t *, uint8_t *, uint32_t,
                        uint32_t, uint32_t);
typedef void (*shadeu_t)(const void *, const void *, const void *, uint32_t *, const uint8_t *, uint32_t, uint32_t,
                         uint32_t);

static void test_shade(int untex) {
    char what[160];
    for (int it = 0; it < ITERS; it++) {
        int ext = rnd(4) == 0;
        uint32_t count = gen_count(), stride = gen_stride(count);
        uint32_t alpha = ext ? rnd(256) : rnd(32);
        uint32_t poly[8];
        for (int i = 0; i < 8; i++) poly[i] = (uint32_t)rnd64();
        uint32_t disp = (uint32_t)rnd64();
        memcpy(sysbuf + SHADE_A0_DISP3DCNT, &disp, 4);
        fill_toon(rnd(5) == 0);
        uint32_t mis = rnd(6) == 0 ? 4 * rnd(4) : 0;
        uint32_t odst = 0x100 + mis, otex = rnd(10) < 7 ? odst : 0xa00 + mis, orgb = 0x1400 + (rnd(6) == 0 ? rnd(16) : 0);
        arena_init();
        fill_pixels(A + otex, count + 16, ext);
        fill_bytes(A + orgb, 3 * stride + 16, ext ? 256 : 64);
        arena_sync();
        if (untex) {
            DS(shadeu_t, 0x4a620)(sysbuf, geom, poly, (uint32_t *)(A + odst), A + orgb, stride, alpha, count);
            spec_render_polygon_shade_untextured(sysbuf, geom, poly, (uint32_t *)(B + odst), B + orgb, stride, alpha, count);
        } else {
            DS(shade_t, 0x48cc0)(sysbuf, geom, poly, (uint32_t *)(A + odst), (uint32_t *)(A + otex), A + orgb, stride,
                                 alpha, count);
            spec_render_polygon_shade(sysbuf, geom, poly, (uint32_t *)(B + odst), (uint32_t *)(B + otex), B + orgb,
                                      stride, alpha, count);
        }
        snprintf(what, sizeof what, "%s it %d mode %u hl %u count %u stride %u alias %d ext %d",
                 untex ? "shade_untextured" : "shade", it, (poly[1] >> 4) & 3, (disp >> 1) & 1, count, stride,
                 otex == odst, ext);
        if (ut_cmp(what, A, B, ASZ) && ut_fail > 10) return;
    }
}

/* the sub-routines on their own (also covered through shade) */
typedef void (*mod_t)(uint32_t *, const uint32_t *, const uint8_t *, uint32_t, uint32_t, uint32_t);
typedef void (*modred_t)(uint32_t *, const uint32_t *, const uint8_t *, uint32_t, uint64_t);
typedef void (*toon_t)(const uint8_t *, uint8_t *, uint32_t, uint32_t);
typedef void (*comb_t)(uint32_t *, const uint8_t *, uint32_t, uint32_t, uint32_t);

static void test_parts(void) {
    char what[160];
    for (int it = 0; it < ITERS; it++) {
        int ext = rnd(4) == 0, which = rnd(5);
        uint32_t count = gen_count(), stride = gen_stride(count);
        uint32_t alpha = ext ? (uint32_t)rnd64() : rnd(32);
        uint32_t odst = 0x100, otex = rnd(2) ? odst : 0xa00, orgb = 0x1400;
        fill_toon(ext);
        arena_init();
        fill_pixels(A + otex, count + 16, ext);
        fill_bytes(A + orgb, 3 * stride + 16, ext ? 256 : 64);
        arena_sync();
        uint32_t *da = (uint32_t *)(A + odst), *db = (uint32_t *)(B + odst);
        uint32_t *ta = (uint32_t *)(A + otex), *tb = (uint32_t *)(B + otex);
        switch (which) {
        case 0:
            DS(mod_t, 0x9a94c)(da, ta, A + orgb, stride, alpha, count);
            spec_render_polygon_modulate(db, tb, B + orgb, stride, alpha, count);
            break;
        case 1:
            DS(modred_t, 0x9aa1c)(da, ta, A + orgb, alpha, count);
            spec_render_polygon_modulate_red(db, tb, B + orgb, alpha, count);
            break;
        case 2:
            DS(toon_t, 0x9a9d8)(geom + SHADE_GEOM_TOON, A + orgb, stride, count);
            spec_render_polygon_toon_load(geom + SHADE_GEOM_TOON, B + orgb, stride, count);
            break;
        case 3:
            DS(comb_t, 0x9aa98)(da, A + orgb, stride, count, alpha);
            spec_render_polygon_combine_colors(db, B + orgb, stride, count, alpha);
            break;
        case 4:
            DS(mod_t, 0x486c0)(da, ta, A + orgb, stride, alpha, count);
            spec_render_polygon_decal_c(db, tb, B + orgb, stride, alpha, count);
            break;
        }
        snprintf(what, sizeof what, "part %d it %d count %u stride %u alias %d ext %d", which, it, count, stride,
                 otex == odst, ext);
        if (ut_cmp(what, A, B, ASZ) && ut_fail > 10) return;
    }
}

typedef void (*atest_t)(uint8_t *, const uint32_t *, uint32_t, uint32_t, uint32_t *);
typedef void (*aidtest_t)(uint8_t *, const uint8_t *, const uint8_t *, uint32_t, uint32_t);
typedef void (*fog_t)(uint32_t *, uint64_t, uint32_t);

static void test_tests(void) {
    char what[160];
    for (int it = 0; it < ITERS; it++) {
        int ext = rnd(4) == 0, which = rnd(3);
        uint32_t count = gen_count();
        uint32_t omask = 0x40, ocol = 0x400, oid = 0x1200, oal = 0x1600, opass = 0x20;
        arena_init();
        if (ext) rndfill(A + omask, 0x240);
        else for (int i = 0; i < 0x240; i++) A[omask + i] = rnd(4) ? 0xff : 0;
        fill_pixels(A + ocol, count + 16, ext && rnd(2));
        uint32_t ref = ext ? (uint32_t)rnd64() : rnd(32);
        uint32_t id = ext ? (uint32_t)rnd64() : rnd(64);
        for (int i = 0; i < 0x240; i++) {
            A[oid + i] = rnd(2) ? (id & 0xff) : ext ? (uint8_t)rnd64() : rnd(64);
            A[oal + i] = ext ? (uint8_t)rnd64() : (rnd(2) ? 31 : rnd(32));
        }
        arena_sync();
        switch (which) {
        case 0:
            DS(atest_t, 0x9a270)(A + omask, (uint32_t *)(A + ocol), ref, count, (uint32_t *)(A + opass));
            spec_render_polygon_alpha_test(B + omask, (uint32_t *)(B + ocol), ref, count, (uint32_t *)(B + opass));
            break;
        case 1:
            DS(aidtest_t, 0x9a2f0)(A + omask, A + oid, A + oal, count, id);
            spec_render_polygon_alpha_id_test(B + omask, B + oid, B + oal, count, id);
            break;
        case 2:
            DS(fog_t, 0x9b3a8)((uint32_t *)(A + ocol), count, 1);
            spec_render_polygon_apply_fog((uint32_t *)(B + ocol), count);
            break;
        }
        snprintf(what, sizeof what, "test %d it %d count %u ref %u id %u ext %d", which, it, count, ref, id, ext);
        if (ut_cmp(what, A, B, ASZ) && ut_fail > 10) return;
    }
}

#define ESZ 0x10000
static uint8_t EA[ESZ], EB[ESZ], SP[0x800];
typedef void (*edge_t)(const void *, void *, uint32_t);

static void test_edges(void) {
    char what[160];
    for (int it = 0; it < ITERS; it++) {
        int ext = rnd(4) == 0;
        uint32_t lines = rnd(3) == 0 ? 1 : rndr(1, ext ? 44 : 32);
        rndfill(SP, sizeof SP);
        for (uint32_t l = 0; l < 44; l++) {
            uint16_t c, L, R;
            if (ext) { c = rnd(48); L = rnd(24); R = rnd(24); }
            else { c = rnd(5) == 0 ? 0 : rnd(41); L = c ? rnd(c + 1 < 8 ? c + 1 : 8) : 0; R = c - L ? rnd((c - L) + 1 < 8 ? (c - L) + 1 : 8) : 0; }
            memcpy(SP + 0x630 + 4 * l, &c, 2);
            memcpy(SP + 0x6e0 + 4 * l, &L, 2);
            memcpy(SP + 0x6e2 + 4 * l, &R, 2);
        }
        rndfill(EA, ESZ);
        memcpy(EB, EA, ESZ);
        DS(edge_t, 0x4a460)(SP, EA + 0x8000, lines);
        spec_render_polygon_mark_edges_c(SP, EB + 0x8000, lines);
        snprintf(what, sizeof what, "mark_edges it %d lines %u ext %d", it, lines, ext);
        if (ut_cmp(what, EA, EB, ESZ) && ut_fail > 10) return;
    }
}

void ut_main(void) {
    sysbuf = calloc(1, SHADE_A0_DISP3DCNT + 0x100);
    geom = calloc(1, SHADE_GEOM_TOON + 0x400);
    test_parts();
    fprintf(stderr, "parts done (%d fails)\n", ut_fail);
    test_shade(0);
    fprintf(stderr, "shade done (%d fails)\n", ut_fail);
    test_shade(1);
    fprintf(stderr, "shade_untextured done (%d fails)\n", ut_fail);
    test_tests();
    fprintf(stderr, "alpha/id/fog done (%d fails)\n", ut_fail);
    test_edges();
    fprintf(stderr, "mark_edges done (%d fails)\n", ut_fail);
}
