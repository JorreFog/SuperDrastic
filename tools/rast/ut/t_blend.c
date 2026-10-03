/* t_blend.c: alpha blend/pass, alpha combine (8 variants) and 4x writeback vs. DraStic. Run:
 *   run.sh t_blend.c ../../../src/rast/spec/blend.c */
#include "ut.h"
#include "spec/blend.h"

#define NP 1024   /* pixel buffers (blocks of 16 may run past n) */

typedef void (*blend_fn)(uint32_t *, const uint32_t *, int32_t, uint8_t *);
typedef void (*comb_fn)(uint32_t *, uint32_t *, const uint32_t *, const uint32_t *, uint8_t *, uint32_t,
                        const uint8_t *, const uint8_t *, int32_t);
typedef void (*combc_fn)(uint32_t *, uint32_t, const uint32_t *, uint32_t *, uint8_t *, uint32_t,
                         const uint8_t *, const uint8_t *, int32_t);
typedef void (*wb_fn)(const void *, uint32_t *, uint32_t *, uint32_t, uint32_t, const uint32_t *, const uint32_t *,
                      const uint8_t *);
typedef void (*wbap_fn)(const void *, uint32_t *, uint32_t *, uint32_t, uint32_t, const uint32_t *,
                        const uint32_t *);
typedef void (*wba_fn)(const void *, uint32_t *, uint32_t *, uint8_t *, uint32_t, const uint32_t *,
                       const uint32_t *, const uint8_t *);

static int32_t rnd_n(void) {
    switch (rnd(8)) {
    case 0: return rndr(-40, 1);
    case 1: return rndr(1, 40);
    case 2: return 16 * rndr(1, 32);
    default: return rndr(1, 512);
    }
}
/* colour words: realistic (r6 g6 b6 a5 [fog]) or arbitrary */
static uint32_t rnd_color(int mode) {
    if (mode == 0) return (uint32_t)rnd64();
    uint32_t a = rnd(4) == 0 ? (rnd(2) ? 31 : 0) : rnd(32);
    return rnd(64) | rnd(64) << 8 | rnd(64) << 16 | a << 24 | (rnd(4) == 0) << 31;
}
static uint32_t rnd_attr(int mode) {
    if (mode == 0) return (uint32_t)rnd64();
    return rnd(1u << 24) | (rnd(4) == 0 ? 0x40u : 0) << 24;
}

static void test_blend(void) {
    static uint32_t c0[NP], c1[NP], dst[NP];
    static uint8_t a0[NP], a1[NP];
    const char *names[2] = { "alpha_blend", "alpha_pass" };
    blend_fn ds[2] = { DS(blend_fn, 0x9aac8), DS(blend_fn, 0x9ab68) };
    blend_fn us[2] = { spec_render_polygon_alpha_blend, spec_render_polygon_alpha_pass };
    for (int it = 0; it < 6000; it++) {
        int f = it & 1, mode = rnd(3);
        int32_t n = rnd_n();
        for (int i = 0; i < NP; i++) { c0[i] = rnd_color(mode); dst[i] = rnd_color(mode); }
        rndfill(a0, NP);
        memcpy(c1, c0, sizeof c0); memcpy(a1, a0, NP);
        ds[f](c0, dst, n, a0);
        us[f](c1, dst, n, a1);
        if (ut_cmp(names[f], c0, c1, sizeof c0) | ut_cmp(names[f], a0, a1, NP)) {
            fprintf(stderr, "  n=%d mode=%d\n", n, mode);
            if (ut_fail > 10) return;
        }
    }
}

static void test_combine(void) {
    static uint32_t c0[NP], c1[NP], at0[NP], at1[NP], da0[NP], da1[NP], dcol[NP];
    static uint8_t id0[NP], id1[NP], alpha[NP], mask[NP];
    static const char *names[8] = { "combine", "combine_depth", "combine_fog", "combine_depth_fog", "combine_constant",
        "combine_depth_constant", "combine_fog_constant", "combine_depth_fog_constant" };
    static const int offs[8] = { 0x9af2c, 0x9afa0, 0x9b018, 0x9b0a8, 0x9b138, 0x9b1c8, 0x9b258, 0x9b300 };
    static comb_fn us[4] = { spec_render_polygon_alpha_combine, spec_render_polygon_alpha_combine_depth,
        spec_render_polygon_alpha_combine_fog, spec_render_polygon_alpha_combine_depth_fog };
    static combc_fn usc[4] = { spec_render_polygon_alpha_combine_constant,
        spec_render_polygon_alpha_combine_depth_constant, spec_render_polygon_alpha_combine_fog_constant,
        spec_render_polygon_alpha_combine_depth_fog_constant };
    for (int it = 0; it < 16000; it++) {
        int v = it & 7, mode = rnd(3), mmode = rnd(3);
        int32_t n = rnd_n();
        uint32_t pid = mode ? rnd(64) : (uint32_t)rnd64(), cattr = rnd_attr(mode);
        for (int i = 0; i < NP; i++) {
            c0[i] = rnd_color(mode); dcol[i] = rnd_color(mode);
            at0[i] = rnd_attr(mode); da0[i] = rnd_attr(mode) | (mode ? rnd(64) << 24 : 0);
            id0[i] = mode ? (rnd(4) ? rnd(64) : 0xff) : (uint8_t)rnd64();
            alpha[i] = mode ? (rnd(3) ? 31 : rnd(32)) | (rnd(8) == 0) << 7 : (uint8_t)rnd64();
            mask[i] = mmode ? (rnd(4) ? 0xff : 0) : (uint8_t)rnd64();
        }
        memcpy(c1, c0, sizeof c0); memcpy(at1, at0, sizeof at0); memcpy(da1, da0, sizeof da0);
        memcpy(id1, id0, NP);
        if (v < 4) {
            DS(comb_fn, offs[v])(c0, at0, dcol, da0, id0, pid, alpha, mask, n);
            us[v](c1, at1, dcol, da1, id1, pid, alpha, mask, n);
        } else {
            DS(combc_fn, offs[v])(c0, cattr, dcol, da0, id0, pid, alpha, mask, n);
            usc[v - 4](c1, cattr, dcol, da1, id1, pid, alpha, mask, n);
        }
        int e = ut_cmp(names[v], c0, c1, sizeof c0);
        e |= ut_cmp(names[v], at0, at1, sizeof at0);
        e |= ut_cmp(names[v], da0, da1, sizeof da0);
        e |= ut_cmp(names[v], id0, id1, NP);
        if (e) { fprintf(stderr, "  n=%d mode=%d\n", n, mode); if (ut_fail > 10) return; }
    }
}

#define LINES 32
static uint32_t col0[LINES * 512], col1[LINES * 512], att0[LINES * 512], att1[LINES * 512];
static uint8_t idb0[LINES * 512], idb1[LINES * 512];
static uint8_t spans[0x700];
static uint32_t scol[LINES * 512 + 64], sattr[LINES * 512 + 64];
static uint8_t sids[LINES * 512 + 64], smask[LINES * 512 + 64];

/* random span set; returns total pixels */
static uint32_t rnd_spans(uint32_t nlines, int allow0) {
    rndfill(spans, sizeof spans);
    uint32_t tot = 0, style = rnd(4);
    for (uint32_t l = 0; l < nlines; l++) {
        uint32_t s, c;
        if (style == 0) { s = rnd(512); c = rnd(16); }                       /* short spans: all tails */
        else if (style == 1) { s = rnd(64); c = rndr(0, 512 - s); }          /* wide */
        else { s = rnd(512); c = rndr(0, 512 - s); if (rnd(2)) c = c % 40; }
        if (c > 512 - s) c = 512 - s;
        if (!allow0 && !c) { if (s == 512) s = 511; c = 1; }
        if (allow0 && rnd(10) == 0) c = 0;
        ((uint16_t *)(spans + 0x580))[2 * l] = (uint16_t)s;
        ((uint16_t *)(spans + 0x630))[2 * l] = (uint16_t)c;
        tot += c;
    }
    return tot;
}

static void test_writeback(void) {
    static const char *names[3] = { "writeback_4x", "writeback_all_pass_4x", "writeback_alpha_4x" };
    for (int it = 0; it < 6000; it++) {
        int f = it % 3, mode = rnd(2);
        uint32_t nlines = rnd(4) ? rndr(1, 8) : rndr(1, LINES);
        uint32_t pid = mode ? rnd(64) : (uint32_t)rnd64();
        rnd_spans(nlines, f != 0);
        rndfill(col0, sizeof col0); rndfill(att0, sizeof att0); rndfill(idb0, sizeof idb0);
        memcpy(col1, col0, sizeof col0); memcpy(att1, att0, sizeof att0); memcpy(idb1, idb0, sizeof idb0);
        for (unsigned i = 0; i < sizeof scol / 4; i++) {
            scol[i] = rnd_color(mode); sattr[i] = rnd_attr(mode);
            sids[i] = (uint8_t)rnd64(); smask[i] = rnd(2) ? (uint8_t)rnd64() : 0;
        }
        if (f == 0) {
            DS(wb_fn, 0x9c850)(spans, col0, att0, nlines, pid, scol, sattr, smask);
            spec_render_polygon_writeback_4x(spans, col1, att1, nlines, pid, scol, sattr, smask);
        } else if (f == 1) {
            DS(wbap_fn, 0x9c8b0)(spans, col0, att0, nlines, pid, scol, sattr);
            spec_render_polygon_writeback_all_pass_4x(spans, col1, att1, nlines, pid, scol, sattr);
        } else {
            DS(wba_fn, 0x9ca08)(spans, col0, att0, idb0, nlines, scol, sattr, sids);
            spec_render_polygon_writeback_alpha_4x(spans, col1, att1, idb1, nlines, scol, sattr, sids);
        }
        int e = ut_cmp(names[f], col0, col1, sizeof col0);
        e |= ut_cmp(names[f], att0, att1, sizeof att0);
        e |= ut_cmp(names[f], idb0, idb1, sizeof idb0);
        if (e) { fprintf(stderr, "  nlines=%u\n", nlines); if (ut_fail > 10) return; }
    }
}

void ut_main(void) {
    test_blend();
    test_combine();
    test_writeback();
}
