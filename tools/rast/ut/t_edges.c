/* t_edges.c: bit-exact tests of src/rast/spec/edges.c against DraStic's edge walkers and span setup.
 * run: tools/rast/ut/run.sh tools/rast/ut/t_edges.c src/rast/spec/edges.c   (UT_SEED=n) */
#include "ut.h"
#include "spec/edges.h"

#define O_COEFF   0x9abd0
#define O_STEPS   0x9ace4
#define O_W       0x9ad40
#define O_PARAMS  0x9ade8
#define O_XZ      0x4c930
#define O_X       0x4ccd0
#define O_CP0     0x4cf10
#define O_CP1     0x4d0d0
#define O_GEN     0x4d290
#define O_SPANS   0x9cd10
#define O_MARKERS 0x4d680
#define O_RECIP   0x3f27120   /* reciprocal_table (bss, vaddr) */

/* render_polygon_edge_interpolate_parameters_asm stores the stale register v26 into the low halfwords of +0x580;
 * call DraStic through this trampoline, which zeroes v26 (the port writes 0 there). */
__attribute__((visibility("hidden"), used)) void *ut_tramp_fn;
__asm__(".text\n.p2align 2\n.globl ut_tramp\n.hidden ut_tramp\n.type ut_tramp,%function\nut_tramp:\n"
        "adrp x16, ut_tramp_fn\nldr x16, [x16, :lo12:ut_tramp_fn]\nmovi d26, #0\nbr x16\n");
void ut_tramp(void);
#define TRAMP(type, off) (ut_tramp_fn = (void *)(ut_base + (off)), (type)ut_tramp)

static int calls[16];

static int32_t rnd_w(void) {
    switch (rnd(8)) {
    case 0: return rndr(1, 4096);
    case 1: return rndr(1, 1 << 24);
    case 2: return (int32_t)rnd64();
    case 3: return 0;
    case 4: return rndr(-1000, 1000);
    case 5: return 0x1000;
    case 6: return rndr(0x10000, 0x7fffff);
    default: return rndr(1, 1 << 16);
    }
}
static void rnd_vertex(uint8_t *v, uint32_t y) {
    int32_t w = rnd_w(); memcpy(v, &w, 4);
    uint16_t x = rnd(6) ? rnd(512) : rnd(65536), z = rnd(65536), c = rnd(65536);
    uint16_t s = rnd(4) ? (uint16_t)rndr(-4096, 4096) : rnd(65536), t = rnd(4) ? (uint16_t)rndr(-4096, 4096) : rnd(65536);
    uint16_t yy = y;
    memcpy(v + 4, &x, 2); memcpy(v + 6, &yy, 2); memcpy(v + 8, &z, 2);
    memcpy(v + 10, &c, 2); memcpy(v + 12, &s, 2); memcpy(v + 14, &t, 2);
}

static uint8_t verts[64 * 16];
static vtx_t *pairs[32];
static uint8_t counts[16];

/* random edges: n pairs with b->y >= a->y + something; returns n, total lines */
static uint32_t rnd_pairs(uint32_t *ptotal, uint32_t maxtotal, int allow_zero, int dy_extreme) {
    uint32_t n = 1 + rnd(16), total = 0;
    for (uint32_t i = 0; i < 64; i++) rnd_vertex(verts + 16 * i, rnd(512));
    for (uint32_t e = 0; e < n; e++) {
        uint8_t *a = verts + 16 * (2 * e), *b = verts + 16 * (2 * e + 1);
        uint16_t ya = rnd(400), dy = dy_extreme && !rnd(4) ? rnd(1024) : 1 + rnd(dy_extreme ? 512 : 100);
        uint16_t yb = ya + dy;
        memcpy(a + 6, &ya, 2); memcpy(b + 6, &yb, 2);
        pairs[2 * e] = a; pairs[2 * e + 1] = b;
        uint32_t left = maxtotal - total, c = allow_zero && !rnd(6) ? 0 : 1 + rnd(left < 40 ? left + 1 : 40);
        if (c > left) c = left;
        counts[e] = c; total += c;
    }
    *ptotal = total;
    return n;
}

static uint8_t A[0x2000], B[0x2000];

static void t_coeff(void) {
    for (int it = 0; it < 3000; it++) {
        uint32_t total, n = rnd_pairs(&total, 600, 1, 1);
        int32_t skip = rnd(4) ? (int32_t)rnd(300) : (int32_t)rnd64();
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        DS(void (*)(void *, vtx_t **, const uint8_t *, uint32_t, int32_t), O_COEFF)(A + 64, pairs, counts, n, skip);
        spec_render_polygon_edge_perspective_coefficients((float *)(B + 64), pairs, counts, n, skip);
        if (ut_cmp("coefficients", A, B, sizeof A)) { fprintf(stderr, "  it %d n %u skip %d\n", it, n, skip); return; }
        calls[0]++;
    }
}

static void t_steps(void) {
    for (int it = 0; it < 3000; it++) {
        int32_t total = rnd(8) ? (int32_t)rnd(300) : -(int32_t)rnd(20);
        uint32_t mode = rnd(3);
        rndfill(A, sizeof A);
        if (mode != 2) { /* realistic coefficients */
            uint32_t tt, n = rnd_pairs(&tt, 600, 1, 0);
            spec_render_polygon_edge_perspective_coefficients((float *)A, pairs, counts, n, rnd(40));
        }
        memcpy(B, A, sizeof A);
        int inplace = rnd(2);
        uint8_t *oa = inplace ? A : A + 0x1400, *ob = inplace ? B : B + 0x1400;
        DS(void (*)(void *, const void *, int32_t), O_STEPS)(oa, A, total);
        spec_render_polygon_edge_perspective_steps((int16_t *)ob, (const float *)B, total);
        if (ut_cmp("steps", A, B, sizeof A)) { fprintf(stderr, "  it %d total %d mode %u\n", it, total, mode); return; }
        calls[1]++;
    }
}

static int16_t steps[1024];
static void rnd_steps(void) {
    for (int i = 0; i < 1024; i++) steps[i] = rnd(4) ? (int16_t)rnd(32768) : (int16_t)rnd64();
}

static void t_w_params(void) {
    for (int it = 0; it < 4000; it++) {
        uint32_t total, n = rnd_pairs(&total, 80, 1, 0);
        rnd_steps();
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        DS(void (*)(vtx_t **, void *, const int16_t *, const uint8_t *, uint32_t), O_W)(pairs, A, steps, counts, n);
        spec_render_polygon_edge_interpolate_w(pairs, B, steps, counts, n);
        if (ut_cmp("interpolate_w", A, B, sizeof A)) { fprintf(stderr, "  it %d n %u\n", it, n); return; }
        calls[2]++;
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        TRAMP(void (*)(vtx_t **, void *, const int16_t *, const uint8_t *, uint32_t), O_PARAMS)(pairs, A, steps, counts, n);
        spec_render_polygon_edge_interpolate_parameters(pairs, B, steps, counts, n);
        if (ut_cmp("interpolate_parameters", A, B, sizeof A)) { fprintf(stderr, "  it %d n %u\n", it, n); return; }
        calls[3]++;
    }
}

static void t_x(void) {
    for (int it = 0; it < 4000; it++) {
        uint32_t total, n = rnd_pairs(&total, 80, 1, 1);
        uint32_t skip = rnd(4) ? rnd(64) : (uint32_t)rnd64();
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        DS(void (*)(vtx_t **, void *, const uint8_t *, uint32_t, uint32_t), O_XZ)(pairs, A, counts, n, skip);
        spec_render_polygon_edge_interpolate_xz_c(pairs, B, counts, n, skip);
        if (ut_cmp("interpolate_xz_c", A, B, sizeof A)) { fprintf(stderr, "  it %d n %u\n", it, n); return; }
        calls[4]++;
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        DS(void (*)(vtx_t **, void *, const uint8_t *, uint32_t, uint32_t), O_X)(pairs, A, counts, n, skip);
        spec_render_polygon_edge_interpolate_x_c(pairs, B, counts, n, skip);
        if (ut_cmp("interpolate_x_c", A, B, sizeof A)) { fprintf(stderr, "  it %d n %u\n", it, n); return; }
        calls[5]++;
    }
}

/* ---- full edge walk ---- */
static vtx_t *vbuf[40];
#define VSTART 20
static uint8_t vcopy[sizeof verts];

/* replicate the walk to reject inputs that would overflow (n > 16, total too large) or be empty */
static int walk_ok(int dir, uint32_t ys, uint32_t ye) {
    vtx_t **q = vbuf + VSTART;
    uint32_t yp, n = 0, total = 0;
    memcpy(&yp, q[0] + 6, 2); yp &= 0xffff;
    if (!(ye > yp)) return 0;
    do {
        q += dir;
        if (q < vbuf + 1 || q >= vbuf + 39) return 0;
        uint32_t y1 = 0; memcpy(&y1, q[0] + 6, 2);
        int32_t len = (int32_t)(y1 - yp);
        if (ys > yp) len += (int32_t)(yp - ys);
        if (y1 > ye) len += (int32_t)(ye - y1);
        if (len > 0) { n++; total += len; }
        yp = y1;
    } while (ye > yp);
    return n >= 1 && n <= 16 && total <= 36;
}

/* forward chain in verts[0..], backward chain in verts[32..] (vertex 0 shared); both end at y = 511 */
static void gen_one(int dir, uint32_t y0, int monotone, uint8_t *base) {
    uint32_t L = 2 + rnd(11), y = y0;
    for (uint32_t k = 1; k < L; k++) {
        uint32_t r = rnd(8);
        if (r == 0) ;
        else if (r == 1 && !monotone) y = y > 5 ? y - rnd(6) : y;
        else y += rnd(r < 4 ? 4 : 30);
        if (y > 511 || k == L - 1) y = 511;
        vtx_t *v = (vtx_t *)(base + 16 * k);
        uint16_t yy = y; memcpy(v + 6, &yy, 2);
        vbuf[VSTART + dir * (int)k] = v;
    }
}
static int gen_chain(int dir, uint32_t *ys, uint32_t *ye, int monotone) {
    for (uint32_t i = 0; i < 64; i++) rnd_vertex(verts + 16 * i, rnd(512));
    for (int i = 0; i < 40; i++) vbuf[i] = (vtx_t *)(verts + 16 * rnd(64));
    uint32_t y0 = rnd(400);
    { uint16_t yy = y0; memcpy(verts + 6, &yy, 2); vbuf[VSTART] = verts; }
    gen_one(1, y0, monotone, verts);
    gen_one(-1, y0, monotone, verts + 16 * 32);
    int32_t s = (int32_t)y0 + rndr(-10, 30); if (s < 0) s = 0;
    uint32_t top = (uint32_t)s > y0 ? (uint32_t)s : y0;
    *ys = s; *ye = top + 1 + rnd(34);
    if (*ye > 511) *ye = 511;
    return walk_ok(dir, *ys, *ye) && walk_ok(-dir, *ys, *ye);
}

/* render_polygon_4x's x fix-up between the walks and setup_spans (edge-marking path) */
static void fixup(uint8_t *s, uint32_t lines) {
    for (uint32_t i = 0; i < lines; i++) {
        uint16_t L, R; memcpy(&L, s + 0x580 + 4 * i, 2); memcpy(&R, s + 0x630 + 4 * i, 2);
        uint16_t l = L & 0x7fff, r = R & 0x7fff;
        if (l > r) { if (!(L & 0x8200)) l++; } else { if (!(R & 0x8200)) r++; }
        memcpy(s + 0x580 + 4 * i, &l, 2); memcpy(s + 0x630 + 4 * i, &r, 2);
    }
}

typedef void (*cp_t)(uint8_t *, uint8_t *, vtx_t **, uint32_t, uint32_t, uint32_t);
typedef void (*gen_t)(void *, uint8_t *, uint8_t *, vtx_t **, uint32_t, uint32_t, int32_t, uint32_t);

static void t_walk(void) {
    for (int it = 0; it < 6000; it++) {
        int which = rnd(3); /* 0: constprop.0, 1: constprop.1, 2: generic */
        int dir = which == 0 ? 1 : which == 1 ? -1 : (rnd(2) ? 1 : -1);
        uint32_t ys, ye;
        if (!gen_chain(dir, &ys, &ye, rnd(4) != 0)) { it--; continue; }
        uint32_t flags = rnd(256);
        memcpy(vcopy, verts, sizeof verts);
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        uint8_t *sa = A + 0x100 + (which == 1 ? 0xb0 : 0), *sb = B + 0x100 + (which == 1 ? 0xb0 : 0);
        if (which < 2) {
            TRAMP(cp_t, which ? O_CP1 : O_CP0)(sa, A + 0x100 + 0x6e0, vbuf + VSTART, ys, ye, flags);
            (which ? spec_render_polygon_interpolate_edges_constprop_1 : spec_render_polygon_interpolate_edges_constprop_0)
                (sb, B + 0x100 + 0x6e0, vbuf + VSTART, ys, ye, flags);
        } else {
            TRAMP(gen_t, O_GEN)(0, sa, A + 0x100 + 0x6e0, vbuf + VSTART, ys, ye, dir, flags);
            spec_render_polygon_interpolate_edges(0, sb, B + 0x100 + 0x6e0, vbuf + VSTART, ys, ye, dir, flags);
        }
        ut_cmp("walk vertices", vcopy, verts, sizeof verts);
        if (ut_cmp(which == 0 ? "constprop.0" : which == 1 ? "constprop.1" : "interpolate_edges", A, B, sizeof A)) {
            fprintf(stderr, "  it %d ys %u ye %u flags %02x dir %d\n", it, ys, ye, flags, dir);
            return;
        }
        calls[6 + which]++;
    }
}

/* both chains + render_polygon_4x's fix-up + setup_spans + edge markers, like the edge-marking path */
static void t_pipeline(void) {
    for (int it = 0; it < 3000; it++) {
        uint32_t ys, ye;
        if (!gen_chain(1, &ys, &ye, 1)) { it--; continue; }
        uint32_t flags = rnd(256), lines = 0;
        { uint32_t yp; memcpy(&yp, vbuf[VSTART] + 6, 2); yp &= 0xffff; lines = ye - (ys > yp ? ys : yp); }
        if (lines < 1) { it--; continue; }
        uint32_t clip = rnd(4);
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        uint8_t *sa = A + 0x100, *sb = B + 0x100;
        TRAMP(cp_t, O_CP0)(sa, sa + 0x6e0, vbuf + VSTART, ys, ye, flags);
        TRAMP(cp_t, O_CP1)(sa + 0xb0, sa + 0x6e0, vbuf + VSTART, ys, ye, flags);
        spec_render_polygon_interpolate_edges_constprop_0(sb, sb + 0x6e0, vbuf + VSTART, ys, ye, flags);
        spec_render_polygon_interpolate_edges_constprop_1(sb + 0xb0, sb + 0x6e0, vbuf + VSTART, ys, ye, flags);
        fixup(sa, lines); fixup(sb, lines);
        DS(void (*)(uint8_t *, int32_t), O_SPANS)(sa, lines);
        spec_render_polygon_setup_spans_4x(sb, lines);
        uint32_t ml = lines - (clip & 1) - ((clip >> 1) & 1);
        if ((int32_t)ml >= ((clip & 2) ? 0 : 1)) {
            DS(void (*)(uint8_t *, uint32_t, uint32_t), O_MARKERS)(sa + 4 * (clip & 1), ml, clip);
            spec_render_polygon_setup_edge_markers_c(sb + 4 * (clip & 1), ml, clip);
        }
        if (ut_cmp("pipeline", A, B, sizeof A)) { fprintf(stderr, "  it %d lines %u clip %u\n", it, lines, clip); return; }
        calls[9]++;
    }
}

static void rnd_span_block(uint8_t *s) {
    rndfill(s, 0x900);
    for (int i = 0; i < 48; i++) {
        uint16_t xl = rnd(4) ? rnd(600) | (rnd(4) ? 0 : 0x8000) : rnd(65536);
        uint16_t xr = rnd(4) ? rnd(600) | (rnd(4) ? 0 : 0x8000) : rnd(65536);
        if (!rnd(8)) xr = xl;
        memcpy(s + 0x580 + 4 * i, &xl, 2); memcpy(s + 0x630 + 4 * i, &xr, 2);
    }
}

static void t_setup_spans(void) {
    for (int it = 0; it < 5000; it++) {
        int32_t lines = rnd(10) ? (int32_t)rnd(41) : -(int32_t)rnd(10);
        rndfill(A, sizeof A);
        rnd_span_block(A + 0x100);
        memcpy(B, A, sizeof A);
        DS(void (*)(uint8_t *, int32_t), O_SPANS)(A + 0x100, lines);
        spec_render_polygon_setup_spans_4x(B + 0x100, lines);
        if (ut_cmp("setup_spans_4x", A, B, sizeof A)) { fprintf(stderr, "  it %d lines %d\n", it, lines); return; }
        calls[10]++;
    }
}

static void t_markers(void) {
    for (int it = 0; it < 5000; it++) {
        uint32_t clip = rnd(4), lines = (clip & 2) ? rnd(36) : 1 + rnd(35);
        rndfill(A, sizeof A);
        uint8_t *s = A + 0x100;
        rndfill(s, 0x900);
        for (int i = -1; i < 40; i++) {
            uint16_t x = rnd(6) ? rnd(513) : rnd(65536), w = rnd(6) ? rnd(513 - (x < 513 ? x : 0)) : rnd(65536);
            memcpy(s + 0x580 + 4 * i, &x, 2); memcpy(s + 0x630 + 4 * i, &w, 2);
        }
        memcpy(B, A, sizeof A);
        uint32_t off = rnd(2) ? 4 * (clip & 1) : 4 * rnd(2);
        DS(void (*)(uint8_t *, uint32_t, uint32_t), O_MARKERS)(A + 0x100 + off, lines, clip);
        spec_render_polygon_setup_edge_markers_c(B + 0x100 + off, lines, clip);
        if (ut_cmp("setup_edge_markers_c", A, B, sizeof A)) { fprintf(stderr, "  it %d lines %u clip %u\n", it, lines, clip); return; }
        calls[11]++;
    }
}

void ut_main(void) {
    uint32_t *rt = DS(uint32_t *, O_RECIP);
    for (uint32_t i = 1; i <= 512; i++) rt[i] = (0x3fffffffu + i) / i;
    for (int32_t i = 0; i < 1024; i++)
        if (rt[i] != spec_edges_reciprocal(i)) { fprintf(stderr, "FAIL recip %d\n", i); ut_fail++; break; }
    t_coeff();
    t_steps();
    t_w_params();
    t_x();
    t_walk();
    t_pipeline();
    t_setup_spans();
    t_markers();
    static const char *nm[] = { "coefficients", "steps", "interpolate_w", "interpolate_parameters", "interpolate_xz_c",
        "interpolate_x_c", "constprop.0", "constprop.1", "interpolate_edges", "pipeline", "setup_spans_4x",
        "setup_edge_markers_c" };
    for (int i = 0; i < 12; i++) fprintf(stderr, "  %-24s %d calls\n", nm[i], calls[i]);
}
