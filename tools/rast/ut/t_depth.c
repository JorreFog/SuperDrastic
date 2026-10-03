/* t_depth.c: DraStic's perspective/depth routines vs src/rast/spec/depth.c.
 * run.sh t_depth.c ../../../src/rast/spec/depth.c */
#include "ut.h"
#include "spec/depth.h"

#define RECIP_TABLE   0x3f27120   /* u32 reciprocal_table[1024]:   (0x3fffffff + i) / i, i = 1..512 */
#define RECIP_TABLE_U 0x3f28520   /* u32 reciprocal_table_u[1024]: (0x7fffffff + i) / i, i = 1..512 */

#define MAXPIX 0x30000
#define SPANSZ 0x800

static uint8_t spans[SPANSZ];
static uint32_t *big_tab;          /* random 65536-entry table for extreme counts */
static uint8_t *ia, *ib, *oa, *ob, *oa2, *ob2, *oa3, *ob3;   /* inputs / outputs, drastic (a) vs ours (b) */

static inline uint32_t *sp32(int off, int l) { return (uint32_t *)(spans + off) + l; }
static inline uint16_t *sp16(int off, int l) { return (uint16_t *)(spans + off + 4 * l); }

static int32_t rnd_s32(void) { return (int32_t)rnd64(); }

/* random counts for nlines lines; mode 0: realistic (batch <= ~512 px), 1: any count <= 512, 2: extreme
 * returns total pixel count */
static uint32_t make_spans(uint32_t nlines, int mode) {
    rndfill(spans, SPANSZ);
    uint32_t total = 0;
    uint32_t budget = 512 + rnd(64);
    for (uint32_t l = 0; l < nlines; l++) {
        uint32_t c, x;
        switch (mode) {
        case 0:
            c = rnd(4) == 0 ? rnd(4) : rnd(budget / nlines + 2);
            break;
        case 1:
            c = rnd(513);
            break;
        default:
            c = nlines <= 2 ? (rnd(3) ? rnd(65536) : 65535 - rnd(8)) : rnd(2048);
        }
        x = mode == 2 ? rnd(512) : rnd(513 - (c > 512 ? 512 : c));
        *sp16(0x630, l) = (uint16_t)c;
        *sp16(0x580, l) = (uint16_t)x;
        total += c;
    }
    return total;
}

static uint32_t rnd_lines(int mode) { return mode == 2 ? 1 + rnd(2) : (rnd(4) == 0 ? 1 + rnd(44) : 1 + rnd(32)); }

static void init_tables(void) {
    uint32_t *t = DS(uint32_t *, RECIP_TABLE), *u = DS(uint32_t *, RECIP_TABLE_U);
    for (uint32_t i = 1; i < 0x201; i++) {
        t[i] = (0x3fffffffu + i) / i;
        u[i] = (0x7fffffffu + i) / i;
    }
    big_tab = malloc(65536 * 4);
}

static void fill2(void *a, void *b, size_t n) { rndfill(a, n); memcpy(b, a, n); }

/* ---------------------------------------------------------------------------------------------------------------- */
typedef void coef_fn(const void *, float *, float *, uint32_t);
typedef void steps_fn(int16_t *, const float *, const float *, int32_t);

static void set_w(uint32_t nlines, int kind) {
    for (uint32_t l = 0; l < nlines; l++) {
        int32_t w0, w1;
        switch (kind) {
        case 0: w0 = rndr(1, 0xffffff); w1 = rndr(1, 0xffffff); break;          /* realistic positive w */
        case 1: w0 = rndr(1, 0x1000); w1 = w0 + rndr(-8, 8); if (w1 < 1) w1 = 1; break;  /* near-constant w */
        case 2: w0 = rndr(0x7f000000, 0x7fffffff); w1 = rndr(1, 0x7fffffff); break;
        default: w0 = rnd_s32(); w1 = rnd_s32(); break;
        }
        *sp32(0x000, l) = (uint32_t)w0;
        *sp32(0x0b0, l) = (uint32_t)w1 - (uint32_t)w0;
    }
}

static void t_perspective(int iters) {
    coef_fn *dc = DS(coef_fn *, 0x99d50);
    steps_fn *ds = DS(steps_fn *, 0x99df0);
    for (int it = 0; it < iters; it++) {
        int mode = rnd(10) == 0 ? 2 : rnd(3) == 0;
        uint32_t nlines = rnd_lines(mode);
        uint32_t n = make_spans(nlines, mode);
        set_w(nlines, rnd(4));
        size_t fb = ((size_t)n + 64) * 4;
        /* coefficients: num at oa, den at oa2 */
        fill2(oa, ob, fb); fill2(oa2, ob2, fb);
        dc(spans, (float *)oa, (float *)oa2, nlines);
        spec_render_polygon_setup_perspective_coefficients(spans, (float *)ob, (float *)ob2, nlines);
        if (ut_cmp("coefficients num", oa, ob, fb) | ut_cmp("coefficients den", oa2, ob2, fb)) {
            fprintf(stderr, "  it %d nlines %u n %u\n", it, nlines, n);
            return;
        }
        /* steps: in place (as the flush does) or into a separate buffer */
        if (n > 0x7fffff00u) continue;
        int32_t cnt = (int32_t)n;
        if (rnd(4) == 0) cnt = rnd(4) == 0 ? (int32_t)rnd(3) : (int32_t)rnd(n + 1);
        if (rnd(2)) {
            ds((int16_t *)oa, (const float *)oa, (const float *)oa2, cnt);
            spec_render_polygon_setup_perspective_steps((int16_t *)ob, (const float *)ob, (const float *)ob2, cnt);
            if (ut_cmp("steps in place", oa, ob, fb)) { fprintf(stderr, "  it %d n %d\n", it, cnt); return; }
        } else {
            fill2(oa3, ob3, fb);
            ds((int16_t *)oa3, (const float *)oa, (const float *)oa2, cnt);
            spec_render_polygon_setup_perspective_steps((int16_t *)ob3, (const float *)ob, (const float *)ob2, cnt);
            if (ut_cmp("steps", oa3, ob3, fb)) { fprintf(stderr, "  it %d n %d\n", it, cnt); return; }
        }
    }
}

/* steps on arbitrary float inputs (random bits, specials, moderate values) */
static void t_steps_random(int iters) {
    steps_fn *ds = DS(steps_fn *, 0x99df0);
    for (int it = 0; it < iters; it++) {
        int32_t n = rnd(8) == 0 ? (int32_t)rnd(3) : (int32_t)rnd(1100);
        size_t m = (size_t)n + 64;
        float *num = (float *)ia, *den = (float *)ib;
        int kind = rnd(4);
        for (size_t i = 0; i < m; i++) {
            uint32_t r = (uint32_t)rnd64();
            switch (kind) {
            case 0: memcpy(&num[i], &r, 4); r = (uint32_t)rnd64(); memcpy(&den[i], &r, 4); break;
            case 1: num[i] = (float)rndr(-100000, 100000) * 0.37f; den[i] = (float)rndr(-1000000, 1000000) * 1.13f; break;
            case 2: { static const uint32_t sp[] = { 0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00001,
                          0x00000001, 0x807fffff, 0x7f7fffff, 0x3f800000, 0x00800000, 0x7e800000, 0x47000000 };
                    memcpy(&num[i], &sp[rnd(13)], 4); memcpy(&den[i], &sp[rnd(13)], 4);
                    if (rnd(2)) den[i] = (float)rndr(1, 1 << 24); break; }
            default: num[i] = (float)rnd(1 << 20) * (float)rnd(512); den[i] = num[i] + (float)rnd(1 << 24) * (float)rnd(512);
            }
        }
        size_t ob_ = m * 2;
        fill2(oa, ob, ob_);
        ds((int16_t *)oa, num, den, n);
        spec_render_polygon_setup_perspective_steps((int16_t *)ob, num, den, n);
        if (ut_cmp("steps random", oa, ob, ob_)) { fprintf(stderr, "  it %d kind %d n %d\n", it, kind, n); return; }
    }
}

typedef void stepsw_fn(int16_t *, const void *, uint32_t, const uint32_t *);
static void t_steps_w_constant(int iters) {
    stepsw_fn *d = DS(stepsw_fn *, 0x99eb0);
    for (int it = 0; it < iters; it++) {
        int mode = rnd(10) == 0 ? 2 : rnd(3) == 0;
        uint32_t nlines = rnd_lines(mode);
        uint32_t n = make_spans(nlines, mode);
        const uint32_t *tab = DS(uint32_t *, RECIP_TABLE_U);
        if (mode == 2 || rnd(5) == 0) { rndfill(big_tab, 65536 * 4); tab = big_tab; }
        size_t b = ((size_t)n + 64) * 2;
        fill2(oa, ob, b);
        d((int16_t *)oa, spans, nlines, tab);
        spec_render_polygon_setup_perspective_steps_w_constant((int16_t *)ob, spans, nlines, tab);
        if (ut_cmp("steps_w_constant", oa, ob, b)) { fprintf(stderr, "  it %d nlines %u\n", it, nlines); return; }
    }
}

typedef void iw_fn(uint32_t *, const void *, const int16_t *, uint32_t);
static void t_interpolate_w(int iters) {
    iw_fn *d = DS(iw_fn *, 0x99f04);
    coef_fn *dc = DS(coef_fn *, 0x99d50);
    steps_fn *ds = DS(steps_fn *, 0x99df0);
    for (int it = 0; it < iters; it++) {
        int mode = rnd(10) == 0 ? 2 : rnd(3) == 0;
        uint32_t nlines = rnd_lines(mode);
        uint32_t n = make_spans(nlines, mode);
        set_w(nlines, rnd(4));
        int16_t *steps = (int16_t *)ia;
        if (rnd(2) && mode != 2) {   /* realistic steps from the real setup */
            dc(spans, (float *)ia, (float *)ib, nlines);
            ds(steps, (const float *)ia, (const float *)ib, (int32_t)n);
        } else rndfill(steps, ((size_t)n + 64) * 2);
        size_t b = ((size_t)n + 64) * 4;
        fill2(oa, ob, b);
        d((uint32_t *)oa, spans, steps, nlines);
        spec_render_polygon_interpolate_w((uint32_t *)ob, spans, steps, nlines);
        if (ut_cmp("interpolate_w", oa, ob, b)) { fprintf(stderr, "  it %d nlines %u\n", it, nlines); return; }
    }
}

typedef void iz_fn(uint32_t *, const void *, uint32_t, const uint32_t *);
static void t_interpolate_z(int iters) {
    iz_fn *d = DS(iz_fn *, 0x99f78);
    for (int it = 0; it < iters; it++) {
        int mode = rnd(10) == 0 ? 2 : rnd(3) == 0;
        uint32_t nlines = rnd_lines(mode);
        uint32_t n = make_spans(nlines, mode);
        int kind = rnd(3);
        for (uint32_t l = 0; l < nlines; l++) {
            uint32_t z0 = kind == 2 ? (uint32_t)rnd64() : rnd(1 << 24);
            int32_t dz = kind == 2 ? rnd_s32() : kind == 1 ? rndr(-0x1000000, 0x1000000) : rndr(-0x3ff, 0x3ff);
            if (rnd(8) == 0) dz = 0;
            *sp32(0x160, l) = z0;
            *sp32(0x210, l) = (uint32_t)dz;
        }
        const uint32_t *tab = DS(uint32_t *, RECIP_TABLE);
        if (mode == 2 || rnd(5) == 0) { rndfill(big_tab, 65536 * 4); tab = big_tab; }
        size_t b = ((size_t)n + 64) * 4;
        fill2(oa, ob, b);
        d((uint32_t *)oa, spans, nlines, tab);
        spec_render_polygon_interpolate_z((uint32_t *)ob, spans, nlines, tab);
        if (ut_cmp("interpolate_z", oa, ob, b)) { fprintf(stderr, "  it %d nlines %u kind %d\n", it, nlines, kind); return; }
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
typedef void cmp_fn(uint8_t *, const uint32_t *, const uint32_t *, int32_t, uint32_t *);
typedef void cmpc_fn(uint8_t *, uint32_t, const uint32_t *, int32_t, uint32_t *);

static void t_compare(int iters) {
    static const char *names[4] = { "compare_equal", "compare_equal_constant", "compare_less_than",
                                    "compare_less_than_constant" };
    cmp_fn *dv[2] = { DS(cmp_fn *, 0x99ff0), DS(cmp_fn *, 0x9a150) };
    cmpc_fn *dk[2] = { DS(cmpc_fn *, 0x9a0a0), DS(cmpc_fn *, 0x9a1e0) };
    cmp_fn *sv[2] = { spec_render_polygon_depth_compare_equal, spec_render_polygon_depth_compare_less_than };
    cmpc_fn *sk[2] = { spec_render_polygon_depth_compare_equal_constant,
                       spec_render_polygon_depth_compare_less_than_constant };
    for (int it = 0; it < iters; it++) {
        int which = rnd(4), eq = which < 2, cst = which & 1;
        int32_t n = rnd(10) == 0 ? (int32_t)rnd(5000) : rnd(6) == 0 ? (int32_t)rnd(17) : (int32_t)rnd(600);
        size_t m = (size_t)n + 64;
        uint32_t *z = (uint32_t *)ia, *at = (uint32_t *)ib;
        uint32_t zc = 0;
        int kind = rnd(4);
        uint32_t base = rnd(1 << 24);
        for (size_t i = 0; i < m; i++) {
            uint32_t zi, ai;
            switch (kind) {
            case 0: /* realistic: 24-bit depths, stored word with random id/flag byte */
                zi = rnd(4) ? base + rndr(-2000, 2000) : rnd(1 << 24); zi &= 0xffffff;
                ai = rnd(2) ? zi + rndr(-300, 300) : rnd(1 << 24);
                ai = (ai & 0xffffff) | (uint32_t)rnd64() << 24;
                break;
            case 1: /* equal-boundary: differences right at +-0x100 */
                zi = rnd(1 << 24); ai = zi + rndr(-0x101, 0x101);
                if (rnd(4) == 0) ai = rnd(2) ? 0 : 0xffffff;
                ai = (ai & 0xffffff) | (uint32_t)rnd64() << 24;
                break;
            case 2: /* new depth with high bits / wrap of the difference */
                zi = rnd(2) ? 0x80000000u + rnd(1 << 24) : (uint32_t)rnd64();
                ai = (uint32_t)rnd64();
                break;
            default:
                zi = (uint32_t)rnd64(); ai = (uint32_t)rnd64();
            }
            z[i] = zi; at[i] = ai;
        }
        if (cst) {
            zc = kind == 2 ? (uint32_t)rnd64() : z[rnd(m)];
            if (eq && rnd(2)) for (size_t i = 0; i < m; i += 1 + rnd(3))
                at[i] = ((zc + rndr(-0x101, 0x101)) & 0xffffff) | (at[i] & 0xff000000);
            if (!eq && rnd(4) == 0) for (size_t i = 0; i < m; i++) at[i] = (at[i] & 0xff000000) | (zc + 1 + rnd(16));
        }
        if (rnd(8) == 0) for (size_t i = 0; i < m; i++) at[i] = rnd(2) ? 0xffffffff : 0x00ffffff; /* all pass */
        size_t b = m;
        fill2(oa, ob, b);
        uint32_t ca = 0xdeadbeef, cb = 0xdeadbeef;
        if (cst) { dk[!eq]((uint8_t *)oa, zc, at, n, &ca); sk[!eq]((uint8_t *)ob, zc, at, n, &cb); }
        else { dv[!eq]((uint8_t *)oa, z, at, n, &ca); sv[!eq]((uint8_t *)ob, z, at, n, &cb); }
        if (ut_cmp(names[which], oa, ob, b) | ut_cmp(names[which], &ca, &cb, 4)) {
            fprintf(stderr, "  it %d n %d kind %d count drastic %u ours %u\n", it, n, kind, ca, cb);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
typedef void ld_fn(uint32_t *, const uint32_t *, const void *, uint32_t);
typedef void ldc_fn(uint32_t *, uint32_t *, uint8_t *, const uint32_t *, const uint32_t *, const uint8_t *, const void *,
    uint32_t);
static void t_load(int iters) {
    ld_fn *d = DS(ld_fn *, 0x9c7b0);
    ldc_fn *dcol = DS(ldc_fn *, 0x9c7f0);
    uint32_t *al = (uint32_t *)ia, *cl = (uint32_t *)ib;
    uint8_t *il = ia + 0x20000 * 4;
    for (int it = 0; it < iters; it++) {
        int mode = rnd(10) == 0 ? 2 : rnd(3) == 0;
        uint32_t nlines = rnd_lines(mode);
        if (mode == 2) mode = 1;   /* source lines: keep x + count within the line buffers (+ slack) */
        uint32_t n = make_spans(nlines, mode);
        size_t lw = (size_t)(nlines + 2) * 512;
        rndfill(al, lw * 4); rndfill(cl, lw * 4); rndfill(il, lw);
        size_t b = ((size_t)n + 64) * 4;
        if (rnd(2)) {
            fill2(oa, ob, b);
            d((uint32_t *)oa, al, spans, nlines);
            spec_render_polygon_load_depth_4x((uint32_t *)ob, al, spans, nlines);
            if (ut_cmp("load_depth", oa, ob, b)) { fprintf(stderr, "  it %d nlines %u\n", it, nlines); return; }
        } else {
            fill2(oa, ob, b); fill2(oa2, ob2, b); fill2(oa3, ob3, b / 4);
            dcol((uint32_t *)oa, (uint32_t *)oa2, oa3, al, cl, il, spans, nlines);
            spec_render_polygon_load_depth_colors_id_4x((uint32_t *)ob, (uint32_t *)ob2, ob3, al, cl, il, spans, nlines);
            if (ut_cmp("load_depth_colors_id attrs", oa, ob, b) | ut_cmp("load_depth_colors_id colors", oa2, ob2, b) |
                ut_cmp("load_depth_colors_id ids", oa3, ob3, b / 4)) {
                fprintf(stderr, "  it %d nlines %u\n", it, nlines);
                return;
            }
        }
    }
}

typedef void sb_fn(void *, uint32_t, int32_t);
static void t_set(int iters) {
    sb_fn *d8 = DS(sb_fn *, 0x9ab98), *d32 = DS(sb_fn *, 0x9abb0);
    for (int it = 0; it < iters; it++) {
        int32_t n = rnd(6) == 0 ? (int32_t)rnd(18) : (int32_t)rnd(2100);
        uint32_t v = (uint32_t)rnd64();
        size_t b = ((size_t)n + 64) * 4;
        fill2(oa, ob, b);
        if (rnd(2)) {
            d8(oa, v, n); spec_render_polygon_set_buffer8(ob, v, n);
            if (ut_cmp("set_buffer8", oa, ob, b)) { fprintf(stderr, "  n %d\n", n); return; }
        } else {
            d32(oa, v, n); spec_render_polygon_set_buffer32((uint32_t *)ob, v, n);
            if (ut_cmp("set_buffer32", oa, ob, b)) { fprintf(stderr, "  n %d\n", n); return; }
        }
    }
}

void ut_main(void) {
    init_tables();
    size_t sz = (size_t)MAXPIX * 4 + 4096;
    uint8_t **bufs[] = { &ia, &ib, &oa, &ob, &oa2, &ob2, &oa3, &ob3 };
    for (int i = 0; i < 8; i++) *bufs[i] = calloc(1, sz);
    int k = getenv("UT_ITERS") ? atoi(getenv("UT_ITERS")) : 3000;
    t_perspective(k);    fprintf(stderr, "perspective coefficients+steps: %d calls, fail %d\n", k, ut_fail);
    t_steps_random(k);   fprintf(stderr, "steps (random floats): %d calls, fail %d\n", k, ut_fail);
    t_steps_w_constant(k); fprintf(stderr, "steps_w_constant: %d calls, fail %d\n", k, ut_fail);
    t_interpolate_w(k);  fprintf(stderr, "interpolate_w: %d calls, fail %d\n", k, ut_fail);
    t_interpolate_z(k);  fprintf(stderr, "interpolate_z: %d calls, fail %d\n", k, ut_fail);
    t_compare(4 * k);    fprintf(stderr, "depth compares: %d calls, fail %d\n", 4 * k, ut_fail);
    t_load(2 * k);       fprintf(stderr, "load_depth(+colors_id): %d calls, fail %d\n", 2 * k, ut_fail);
    t_set(2 * k);        fprintf(stderr, "set_buffer8/32: %d calls, fail %d\n", 2 * k, ut_fail);
}
