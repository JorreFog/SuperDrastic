/* t_composite.c: the scanline compositor routines of src/rast/spec/composite.c vs DraStic's originals: the 3D
 * visibility step (and the NEON version, src/rast/compvis.h, vs the C port), and the simple path of
 * render_scanline_2d_composite with everything under it (priority encoder, the select_pixels merges, the 6-bit
 * expansion, the 3D insertion, select_pixels, the composite itself); then the fused 3D + backdrop pass
 * (src/rast/compfuse.h: the kind decision against a per-pixel reference, the planes against DraStic's select_pixels
 * and the C port). Every output buffer sits between guard bytes; inputs are compared afterwards too (nothing else
 * may change).
 * run.sh t_composite.c ../../../src/rast/spec/composite.c */
#include "ut.h"
#include "spec/composite.h"
#include "compvis.h"
#include "compfuse.h"
#include <stddef.h>

typedef void (*gather_fn)(uint8_t *, const uint32_t *);
typedef uint32_t (*vis_fn)(uint8_t *, const uint32_t *);
#define DS_GATHER 0xa0a48
#define DS_VIS    0x3c2c0

static uint32_t pxbuf[256 + 16] __attribute__((aligned(64)));
static unsigned ncalls[3], nret[3];          /* calls per return value 0 / 0x10 / 2 (coverage of the three outcomes) */

/* alpha byte for the "near 31" mode: values that differ from 0 / 0x1f in one bit or in bits 5-7 */
static uint8_t near31(void) {
    static const uint8_t v[] = { 0x1f, 0x1f, 0, 0, 0x1e, 0x1d, 0x1b, 0x17, 0x0f, 0x3f, 0x5f, 0x9f, 0xff, 0x20, 0x80, 0x01, 0xe0 };
    return v[rnd(sizeof v)];
}

/* 256 pixels: bytes 0..2 random (r6 g6 b6, or any byte), byte 3 by mode */
static void fill(uint32_t *px, int mode, unsigned pos, uint8_t val) {
    int junk = rnd(2);
    unsigned dens = rnd(257);
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = junk ? (uint32_t)rnd64() & 0xffffff : rnd(64) | rnd(64) << 8 | rnd(64) << 16;
        uint8_t a;
        switch (mode) {
        case 0: a = rnd(256); break;                                    /* any byte */
        case 1: a = 0; break;                                           /* nothing visible */
        case 2: a = 1 + rnd(255); break;                                /* all visible, any byte */
        case 3: a = 0x1f; break;                                        /* all opaque */
        case 4: a = rnd(256) < dens ? 0x1f : 0; break;                  /* opaque / transparent mix */
        case 5: a = rnd(256) < dens ? (rnd(40) ? 0x1f : 1 + rnd(30)) : 0; break;  /* a few translucent */
        case 6: a = rnd(32); break;                                     /* 5-bit alpha */
        case 7: a = i == (int)pos ? val : 0; break;                     /* one visible pixel */
        case 8: a = i == (int)pos ? val : 0x1f; break;                  /* one pixel differs from opaque */
        case 9: a = near31(); break;
        default: a = 0; break;
        }
        px[i] = rgb | (uint32_t)a << 24;
    }
    if (mode == 10) {                                                   /* spans, as polygons cover a line */
        int x = 0;
        while (x < 256) {
            int n = 1 + rnd(rnd(2) ? 8 : 80);
            uint8_t a = rnd(3) ? 0x1f : rnd(3) ? 0 : (uint8_t)(1 + rnd(30));
            for (int k = 0; k < n && x < 256; k++, x++) px[x] = (px[x] & 0xffffff) | (uint32_t)a << 24;
        }
    }
}

static void check(const uint32_t *px, const char *what) {
    uint8_t g1[256 + 64], g2[256 + 64], o1[96], o2[96], o3[96];
    /* gather: 256 bytes at a random offset in a guard-filled buffer */
    unsigned go = rnd(33);
    uint8_t pat = rnd(256);
    memset(g1, pat, sizeof g1); memset(g2, pat, sizeof g2);
    DS(gather_fn, DS_GATHER)(g1 + go, px);
    spec_render_scanline_gather_3d_alpha(g2 + go, px);
    if (ut_cmp(what, g1, g2, sizeof g1)) { fprintf(stderr, "   (gather, offset %u)\n", go); return; }
    /* set_3d_visibility: 32 bytes at a random offset, guard bytes around */
    unsigned oo = 16 + rnd(32);
    pat = rnd(256);
    memset(o1, pat, sizeof o1); memset(o2, pat, sizeof o2); memset(o3, pat, sizeof o3);
    uint32_t r1 = DS(vis_fn, DS_VIS)(o1 + oo, px);
    uint32_t r2 = spec_render_scanline_set_3d_visibility(o2 + oo, px);
    uint32_t r3 = comp_vis_neon(o3 + oo, px);
    if (ut_cmp(what, o1, o2, sizeof o1)) { fprintf(stderr, "   (set_3d_visibility vs spec, offset %u)\n", oo); return; }
    if (r1 != r2) { fprintf(stderr, "FAIL %s: set_3d_visibility returns %#x, spec %#x\n", what, r1, r2); ut_fail++; return; }
    if (ut_cmp(what, o2, o3, sizeof o2)) { fprintf(stderr, "   (NEON vs spec, offset %u)\n", oo); return; }
    if (r3 != r2) { fprintf(stderr, "FAIL %s: NEON returns %#x, spec %#x\n", what, r3, r2); ut_fail++; return; }
    int k = r1 == 0 ? 0 : r1 == 0x10 ? 1 : r1 == 2 ? 2 : -1;
    if (k < 0) { fprintf(stderr, "FAIL %s: unexpected return %#x\n", what, r1); ut_fail++; return; }
    nret[k]++;
}

static void test_visibility(void) {
    char what[64];
    for (int mode = 0; mode <= 10; mode++) {
        int n = mode == 7 || mode == 8 ? 0 : 2000;
        for (int t = 0; t < n && ut_fail < 10; t++) {
            uint32_t *px = pxbuf + rnd(4) * (rnd(2) ? 1 : 4);           /* word and 16-byte offsets */
            fill(px, mode, 0, 0);
            snprintf(what, sizeof what, "mode %d test %d", mode, t);
            check(px, what);
            ncalls[0]++;
        }
    }
    /* one non-zero (mode 7) / one non-opaque (mode 8) pixel at every position, with 31, 1..30 and 32..255 */
    for (int mode = 7; mode <= 8; mode++)
        for (unsigned pos = 0; pos < 256 && ut_fail < 10; pos++)
            for (int v = 0; v < 3; v++) {
                uint8_t val = v == 0 ? (mode == 7 ? 0x1f : 0) : v == 1 ? (uint8_t)(1 + rnd(30)) : (uint8_t)(32 + rnd(224));
                uint32_t *px = pxbuf + rnd(4);
                fill(px, mode, pos, val);
                snprintf(what, sizeof what, "mode %d pos %u value %#x", mode, pos, val);
                check(px, what);
                ncalls[0]++;
            }
    fprintf(stderr, "t_composite: %u inputs; returns 0: %u, 0x10: %u, 2: %u\n", ncalls[0], nret[0], nret[1], nret[2]);
}

/* ===================================================================================================================
 * the simple path of render_scanline_2d_composite
 * =================================================================================================================== */
typedef void (*prio_fn)(uint8_t *, uint8_t *, uint8_t *);
typedef void (*bin_fn)(uint16_t *, const uint16_t *, const uint16_t *, const uint8_t *);
typedef void (*scal_fn)(uint16_t *, const uint16_t *, uint32_t, const uint8_t *);
typedef void (*exp_fn)(uint8_t *, const uint16_t *);
typedef void (*b32_fn)(uint8_t *, uint8_t *, const uint32_t *, const uint8_t *);
typedef void (*sel_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t **, const uint32_t *, uint8_t *, uint32_t);
typedef void (*comp_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t **, const uint32_t *, uint8_t *, uint64_t, uint64_t,
                        uint64_t, uint64_t);
#define DS_PRIO   0x9ffc0
#define DS_BIN    0xa0640
#define DS_SCAL   0xa0560
#define DS_EXP    0xa0818
#define DS_B32    0xa0730
#define DS_SEL    0x39330
#define DS_COMP   0x3c6d0

static unsigned nport[8];

/* a 256-bit mask by mode: per 32-bit word zero / all ones / random / sparse, or a run like a layer's coverage */
static void fill_mask(uint8_t *m, int mode) {
    if (mode == 4) {                                        /* spans */
        memset(m, 0, 32);
        int x = rnd(256);
        while (x < 256) { int n = 1 + rnd(rnd(2) ? 8 : 100); for (int k = 0; k < n && x < 256; k++, x++) m[x >> 3] |= 1 << (x & 7); x += rnd(80); }
        return;
    }
    for (int w = 0; w < 8; w++) {
        int wm = mode < 4 ? mode : rnd(4);
        uint32_t v = wm == 0 ? 0 : wm == 1 ? ~0u : wm == 2 ? (uint32_t)rnd64() : (1u << rnd(32)) | (rnd(2) ? 1u << rnd(32) : 0);
        memcpy(m + 4 * w, &v, 4);
    }
}
static int rmode(void) { return rnd(8) < 3 ? 5 : (int)rnd(5); }   /* 5 = per word mixed (fill_mask default) */

/* flat test buffers A (original) and B (port) with the same random contents: each payload sits between 64 guard
 * bytes (lay() hands out payload offsets), and the whole span is compared afterwards */
#define G 64
static uint8_t A[0x4000] __attribute__((aligned(64))), B[0x4000] __attribute__((aligned(64)));
static size_t lay_end;
static size_t lay(size_t n) { size_t o = lay_end + G; lay_end = o + n; return o; }
static void lay_fill(void) { lay_end += G; rndfill(A, lay_end); memcpy(B, A, lay_end); }

static void test_prio(void) {
    char what[64];
    for (int t = 0; t < 4000 && ut_fail < 10; t++) {
        /* eng: 0x100 bytes; vis and excl: 16 slots each (list bytes up to 15 address beyond the 8 real ones) */
        lay_end = 0;
        size_t oe = lay(0x100), ov = lay(0x200), ox = lay(0x200);
        lay_fill();
        for (int s = 0; s < 16; s++) { fill_mask(A + ov + 32 * s, rmode()); memcpy(B + ov + 32 * s, A + ov + 32 * s, 32); }
        int kind = rnd(4);
        unsigned n = 0;
        uint8_t list[16];
        if (kind == 0) {                                    /* as video_2d_reorder_layers builds it */
            int obj = rnd(2), on = rnd(16), pr[4] = { rnd(4), rnd(4), rnd(4), rnd(4) };
            for (int p = 0; p < 4; p++) {
                if (obj) list[n++] = 4 + p;
                for (int bg = 0; bg < 4; bg++) if ((on >> bg & 1) && pr[bg] == p) list[n++] = bg;
            }
        } else if (kind == 1) { n = rnd(9); for (unsigned k = 0; k < n; k++) list[k] = rnd(8); }      /* any order, repeats */
        else if (kind == 2) { n = rnd(17); for (unsigned k = 0; k < n; k++) list[k] = rnd(16); }     /* slots 8..15 too */
        else { n = 1; list[0] = rnd(2) ? 0 : 4 + rnd(4); }
        A[oe + 0xb3] = B[oe + 0xb3] = n;
        memcpy(A + oe + 0x84, list, n); memcpy(B + oe + 0x84, list, n);
        DS(prio_fn, DS_PRIO)(A + oe, A + ov, A + ox);
        spec_render_scanline_priority_encode_single(B + oe, B + ov, B + ox);
        snprintf(what, sizeof what, "priority_encode_single test %d (kind %d, %u entries)", t, kind, n);
        ut_cmp(what, A, B, lay_end);
        nport[0]++;
    }
}

static void test_binary(void) {
    char what[80];
    for (int t = 0; t < 6000 && ut_fail < 10; t++) {
        int scalar = t & 1, same = rnd(3) == 0;
        lay_end = 0;
        size_t od = lay(0x200), os = lay(0x200), ol = lay(0x200), om = lay(32);
        lay_fill();
        if (same) os = od;
        fill_mask(A + om, rmode()); memcpy(B + om, A + om, 32);
        uint32_t colour = (uint32_t)rnd64();
        if (scalar) {
            DS(scal_fn, DS_SCAL)((uint16_t *)(A + od), (uint16_t *)(A + os), colour, A + om);
            spec_render_scanline_select_pixels_binary_scalar((uint16_t *)(B + od), (uint16_t *)(B + os), colour, B + om);
        } else {
            DS(bin_fn, DS_BIN)((uint16_t *)(A + od), (uint16_t *)(A + os), (uint16_t *)(A + ol), A + om);
            spec_render_scanline_select_pixels_binary((uint16_t *)(B + od), (uint16_t *)(B + os), (uint16_t *)(B + ol), B + om);
        }
        snprintf(what, sizeof what, "%s test %d (dst %s src)", scalar ? "binary_scalar" : "binary", t, same ? "==" : "!=");
        ut_cmp(what, A, B, lay_end);
        nport[1 + scalar]++;
    }
}

static void test_expand_b32(void) {
    char what[80];
    for (int t = 0; t < 6000 && ut_fail < 10; t++) {
        int kind = t % 3;                                   /* expand, binary32, binary32 with alpha */
        lay_end = 0;
        size_t oo = lay(0x300), op = lay(0x400), oa = lay(0x100), om = lay(32);   /* planes, u32 px (or u16 c), alpha, mask */
        lay_fill();
        fill_mask(A + om, rmode()); memcpy(B + om, A + om, 32);
        if (rnd(2)) for (int i = 0; i < 256; i++) { A[op + 4 * i + 3] &= 0x1f; A[op + 4 * i] &= 0x3f; A[op + 4 * i + 1] &= 0x3f; A[op + 4 * i + 2] &= 0x3f; }
        memcpy(B + op, A + op, 0x400);
        if (kind == 0) {
            DS(exp_fn, DS_EXP)(A + oo, (uint16_t *)(A + op));
            spec_render_scanline_expand_6bit_split(B + oo, (uint16_t *)(B + op));
        } else {
            DS(b32_fn, DS_B32)(A + oo, kind == 2 ? A + oa : 0, (uint32_t *)(A + op), A + om);
            spec_render_scanline_select_pixels_binary32(B + oo, kind == 2 ? B + oa : 0, (uint32_t *)(B + op), B + om);
        }
        snprintf(what, sizeof what, "%s test %d", kind == 0 ? "expand_6bit_split" : kind == 1 ? "binary32" : "binary32_alpha", t);
        ut_cmp(what, A, B, lay_end);
        nport[3 + kind]++;
    }
}

/* ---- select_pixels and the composite: an engine, a scratch area S and a quarter's planes ---- */
#define S_SIZE  0x1d30                                      /* render_scanline_2d's frame (S = sp + 0x180) */
#define S_OFS   0x180
struct env {
    uint8_t eng[0x200];
    uint8_t frame[S_SIZE + G];                              /* S = frame + S_OFS */
    uint8_t out[G + 0x300 + G];
    uint8_t alpha[G + 0x100 + G];
    uint32_t px[256 + 16];
    uint16_t bd[8];
    uint8_t *layers[16];
    uint8_t extra[11][0x220];                               /* layer buffers 5..15 for wide lmasks */
};
static struct env EA __attribute__((aligned(64))), EB __attribute__((aligned(64)));

/* fills EA (layers point into its S); EB is made a relocated copy by copy_env */
static void fill_env(int pmode) {
    struct env *e = &EA;
    rndfill(e, sizeof *e);
    uint8_t *S = e->frame + S_OFS;
    /* the priority list: as video_2d_reorder_layers builds it (BG0 first among equals), or anything */
    int obj = rnd(2), on = rnd(16) | (rnd(2) ? 1 : 0), pr[4] = { rnd(4), rnd(4), rnd(4), rnd(4) };
    unsigned n = 0;
    if (rnd(6)) {
        for (int p = 0; p < 4; p++) {
            if (obj) e->eng[0x84 + n++] = 4 + p;
            for (int bg = 0; bg < 4; bg++) if ((on >> bg & 1) && pr[bg] == p) e->eng[0x84 + n++] = bg;
        }
    } else { n = rnd(9); for (unsigned k = 0; k < n; k++) e->eng[0x84 + k] = rnd(8); }
    e->eng[0xb3] = n;
    *(uint16_t **)(e->eng + 0x18) = e->bd + rnd(8);
    /* visibility: BG0 from 3D-like alpha patterns, the other layers mostly empty or sparse (an overlay) */
    for (int s = 0; s < 8; s++) {
        int m = s == 0 ? (int)rnd(6) : rnd(3) ? 0 : (int)rnd(6);
        fill_mask(S + 0xda0 + 32 * s, m == 5 ? 5 : m);
    }
    /* layer buffers: the table points 0x10 before the first pixel */
    for (int k = 0; k < 16; k++) e->layers[k] = k < 5 ? S + 0x1e0 + 0x220 * k : e->extra[k - 5];
    /* 3D pixels: 6-bit colour, alpha 0/31 mostly */
    for (int i = 0; i < 256 + 16; i++) {
        uint32_t a = pmode == 0 ? 0x1f : pmode == 1 ? (rnd(4) ? 0x1f : 0) : pmode == 2 ? rnd(32) : (uint32_t)rnd(256);
        e->px[i] = (pmode == 3 ? (uint32_t)rnd64() & 0xffffff : rnd(64) | rnd(64) << 8 | rnd(64) << 16) | a << 24;
    }
}
/* EB = EA with every pointer into EA moved to EB */
static void copy_env(void) {
    memcpy(&EB, &EA, sizeof EA);
    uintptr_t a = (uintptr_t)&EA, b = (uintptr_t)&EB;
    uint8_t **pp = (uint8_t **)(EB.eng + 0x18);
    *pp = *pp - a + b;
    for (int k = 0; k < 16; k++) EB.layers[k] = EB.layers[k] - a + b;
}
static void *rel(void *p) { return p ? (uint8_t *)p - (uintptr_t)&EA + (uintptr_t)&EB : 0; }
/* every byte of the two environments but the pointer fields (which differ by the relocation) */
static void cmp_env(const char *what) {
    uint8_t keep[8];
    memcpy(keep, EB.eng + 0x18, 8); memcpy(EB.eng + 0x18, EA.eng + 0x18, 8);
    if (!ut_cmp(what, &EA, &EB, offsetof(struct env, layers)))
        ut_cmp(what, EA.extra, EB.extra, sizeof EA.extra);
    memcpy(EB.eng + 0x18, keep, 8);
}

static uint32_t rnd_lmask(void) {
    int k = rnd(16);
    if (k < 6) return 1;                                    /* BG0 (3D) only */
    if (k < 9) return 1 | rnd(32);
    if (k < 14) return rnd(32);
    return rnd(2) ? rnd(0x10000) : (uint32_t)rnd64();       /* bits the callers do not set */
}

static void test_select(void) {
    char what[96];
    for (int t = 0; t < 5000 && ut_fail < 10; t++) {
        fill_env(rnd(4));
        uint8_t *S = EA.frame + S_OFS;
        for (int s = 0; s < 16 && s * 32 < 0x100 + 0x40; s++) if (rnd(2)) fill_mask(S + 0x10c0 + 32 * s, rmode());
        uint32_t lmask = rnd_lmask();
        if (lmask > 0xffff) lmask &= 0xffff;                /* layers[] has 16 entries */
        copy_env();
        const uint32_t *p3 = rnd(5) ? EA.px + rnd(16) : 0;
        uint8_t *al = rnd(2) ? EA.alpha + G : 0;
        DS(sel_fn, DS_SEL)(EA.eng, EA.out + G, S + 0x10c0, EA.layers, p3, al, lmask);
        spec_render_scanline_select_pixels(EB.eng, EB.out + G, EB.frame + S_OFS + 0x10c0, EB.layers, rel((void *)p3), rel(al), lmask);
        snprintf(what, sizeof what, "select_pixels test %d (lmask %#x, p3d %s, alpha %s)", t, lmask, p3 ? "yes" : "NULL", al ? "yes" : "NULL");
        cmp_env(what);
        nport[6]++;
    }
}

static unsigned ncomp[4];
static void test_composite(void) {
    char what[128];
    for (int t = 0; t < 6000 && ut_fail < 10; t++) {
        fill_env(rnd(4));
        uint8_t *S = EA.frame + S_OFS;
        uint32_t lmask = rnd_lmask() & 0x1f;
        int hofs = rnd(4) == 0;                             /* BG0HOFS: the quarter is a shifted copy in S+0x000 */
        const uint32_t *p3 = rnd(8) ? (hofs ? (uint32_t *)S : EA.px + rnd(16)) : 0;
        if (hofs) memcpy(S, EA.px, 0x400);
        uint8_t *al = rnd(2) ? EA.alpha + G : 0;
        uint64_t flags = (rnd64() & ~(uint64_t)0xf) | (rnd(3) ? 0 : (uint64_t)rnd(2) << 4), line = rnd64();
        if (rnd(2)) flags &= 0x10;
        copy_env();
        uint64_t bld = rnd64();
        DS(comp_fn, DS_COMP)(EA.eng, EA.out + G, S, EA.layers, p3, al, lmask, bld, flags, line);
        spec_render_scanline_2d_composite_simple(EB.eng, EB.out + G, EB.frame + S_OFS, EB.layers, rel((void *)p3), lmask);
        snprintf(what, sizeof what, "composite test %d (lmask %#x, p3d %s, flags %#llx)", t, lmask, p3 ? "yes" : "NULL",
                 (unsigned long long)flags);
        cmp_env(what);
        ncomp[p3 ? 1 : 0]++;
    }
}

/* ===================================================================================================================
 * the fused 3D + backdrop pass (compfuse.h) vs DraStic's select_pixels and the C port
 * =================================================================================================================== */
/* the kind by the per-pixel rule: 3D when excl[0] claims every pixel; none when another layer of the mask claims one
 * or a pixel is neither BG0's nor the backdrop's; else backdrop (excl[0] empty) or mixed */
static int kind_ref(const uint8_t *excl, uint32_t lmask) {
    int all = 1, any = 0;
    for (int i = 0; i < 256; i++) {
        int b0 = excl[i >> 3] >> (i & 7) & 1;
        all &= b0; any |= b0;
    }
    if (all) return CF_KIND_3D;
    for (int i = 0; i < 256; i++) {
        for (int k = 1; k <= 4; k++)
            if ((lmask >> k & 1) && (excl[32 * k + (i >> 3)] >> (i & 7) & 1)) return CF_KIND_NONE;
        if (!((excl[i >> 3] | excl[0xa0 + (i >> 3)]) >> (i & 7) & 1)) return CF_KIND_NONE;
    }
    return any ? CF_KIND_MIXED : CF_KIND_BACKDROP;
}

static unsigned nkind[4], nkind_ds;
static void test_fused(void) {
    char what[128];
    static uint8_t out3[G + 0x300 + G] __attribute__((aligned(64)));
    for (int t = 0; t < 8000 && ut_fail < 10; t++) {
        fill_env(rnd(4));
        uint8_t *S = EA.frame + S_OFS, *excl = S + 0x10c0;
        /* the masks: BG0's by mode, the backdrop's its complement (mostly), the others empty (mostly) */
        int m0 = rnd(7);
        fill_mask(excl, m0 == 6 ? 5 : m0 < 6 ? m0 : 0);
        if (rnd(8)) for (int j = 0; j < 32; j++) excl[0xa0 + j] = (uint8_t)~excl[j];
        else fill_mask(excl + 0xa0, rmode());
        for (int k = 1; k <= 4; k++) {
            if (rnd(6) == 0) fill_mask(excl + 32 * k, rmode());
            else memset(excl + 32 * k, 0, 32);
        }
        if (rnd(10) == 0) { int b = rnd(0xc0); excl[b] ^= (uint8_t)(1u << rnd(8)); }   /* one stray bit */
        uint32_t lmask = 1 | (rnd(2) ? rnd(32) & 0x1e : 0);
        copy_env();
        const uint32_t *p3 = EA.px + rnd(16);
        uint32_t bd = **(uint16_t **)(EA.eng + 0x18);
        int kind = comp_fused_kind(excl, lmask), ref = kind_ref(excl, lmask);
        if (kind != ref) {
            fprintf(stderr, "FAIL fused test %d: kind %d, reference %d (lmask %#x)\n", t, kind, ref, lmask);
            ut_fail++;
            continue;
        }
        nkind[kind]++;
        if (kind == CF_KIND_NONE) continue;
        /* DraStic's select_pixels on A, the C port on B, the fused pass on a third buffer with the same guards */
        DS(sel_fn, DS_SEL)(EA.eng, EA.out + G, excl, EA.layers, p3, 0, lmask);
        spec_render_scanline_select_pixels(EB.eng, EB.out + G, EB.frame + S_OFS + 0x10c0, EB.layers, rel((void *)p3), 0, lmask);
        memcpy(out3, EB.out, sizeof out3);                  /* the guards and the planes' old bytes, as before the call */
        rndfill(out3 + G, 0x300);                           /* the pass must not depend on the planes' old bytes */
        comp_fused_planes(out3 + G, rel((void *)p3), EB.frame + S_OFS + 0x10c0, bd, kind);
        snprintf(what, sizeof what, "fused test %d (kind %d, lmask %#x)", t, kind, lmask);
        cmp_env(what);
        if (ut_cmp(what, EA.out, out3, sizeof out3)) fprintf(stderr, "   (fused pass vs DraStic's select_pixels)\n");
        nkind_ds++;
    }
}

void ut_main(void) {
    test_visibility();
    test_prio();
    test_binary();
    test_expand_b32();
    test_select();
    test_composite();
    test_fused();
    fprintf(stderr, "t_composite: priority_encode_single %u, binary %u, binary_scalar %u, expand_6bit_split %u, binary32 %u, "
            "binary32_alpha %u, select_pixels %u, composite %u (%u with p3d) calls; fused: %u none, %u all 3D, %u all backdrop, "
            "%u mixed (%u planes compared)\n", nport[0], nport[1], nport[2],
            nport[3], nport[4], nport[5], nport[6], ncomp[0] + ncomp[1], ncomp[1], nkind[0], nkind[1], nkind[2], nkind[3], nkind_ds);
}
