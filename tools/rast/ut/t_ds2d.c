/* t_ds2d.c: ds2d.c's replacements of DraStic's 2D scanline routines against the originals, in DraStic's process.
 * For each routine: random inputs (any byte values, edge values, aliased lines), DraStic's function and ours on
 * identical copies, every output byte and 64 guard bytes either side compared. Then the check-mode wrappers (with
 * the real function as their "trampoline": no difference, our result left in place), and last the real entries
 * patched as ds2d_init() patches them (a copy of rast.c's rast_hook) and called through DraStic's own addresses,
 * against results the originals gave before the patch.
 * run.sh t_ds2d.c ../../../src/rast/ds2d_asm.S ../../../src/rast/spec/composite.c */
#include "ut.h"
#include <sys/mman.h>
#include "ds2d.c"
#include "spec/composite.h"

uintptr_t ds_base;
static void patch_jump(uint32_t *at, void *to) {
    at[0] = 0x58000050;                   /* ldr x16, #8 */
    at[1] = 0xd61f0200;                   /* br x16 */
    memcpy(at + 2, &to, 8);
}
void *rast_hook(uintptr_t off, const uint32_t expect[4], void *to) {
    uint32_t *entry = (uint32_t *)(ds_base + off);
    if (memcmp(entry, expect, 16)) return 0;
    static uint32_t *tramp; static int used;
    if (!tramp) tramp = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t *t = tramp + used; used += 8;
    memcpy(t, entry, 16);
    patch_jump(t + 4, entry + 4);
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    uintptr_t pg = (uintptr_t)entry & ~4095ul;
    mprotect((void *)pg, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    patch_jump(entry, to);
    __builtin___clear_cache((char *)entry, (char *)(entry + 4));
    mprotect((void *)pg, 8192, PROT_READ | PROT_EXEC);
    return t;
}

#define G 64
/* a plane block: any bytes, 6-bit values, or extremes */
static void fill_planes(uint8_t *p, size_t n) {
    switch (rnd(4)) {
    case 0: rndfill(p, n); break;
    case 1: for (size_t i = 0; i < n; i++) p[i] = rnd64() & 0x3f; break;
    case 2: for (size_t i = 0; i < n; i++) p[i] = rnd(2) ? 0xff : 0; break;
    default: for (size_t i = 0; i < n; i++) p[i] = rnd(3) ? (uint8_t)rnd64() & 0x3e : (uint8_t)rnd64(); break;
    }
}

/* ---- the 2x line conversion ---- */
#define NCONV 64
static uint8_t conv_in[NCONV][2][0x300];
static uint8_t conv_ref[NCONV][2048 + 2 * G];

static void test_conv(void) {
    conv_fn ds = DS(conv_fn, DS_CONV_32_2X);
    static uint8_t a[0x300 + 2 * G], b[0x300 + 2 * G], o1[2048 + 2 * G], o2[2048 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 3000; it++) {
        fill_planes(a + G, 0x300); fill_planes(b + G, 0x300);
        rndfill(a, G); rndfill(a + G + 0x300, G); rndfill(b, G); rndfill(b + G + 0x300, G);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        int same = rnd(8) == 0;                        /* both quarters the same buffer */
        const uint8_t *pb = same ? a + G : b + G;
        ds(a + G, pb, (uint32_t *)(o1 + G));
        ds2d_conv32_2x(a + G, pb, (uint32_t *)(o2 + G));
        if (ut_cmp("conv32_2x", o1, o2, sizeof o1)) return;
        if (it < NCONV) {                              /* kept for the patched-entry test */
            memcpy(conv_in[it][0], a + G, 0x300); memcpy(conv_in[it][1], pb, 0x300);
            memcpy(conv_ref[it], o1, sizeof o1);
        }
        n++;
    }
    /* the check wrapper: the original as the trampoline, our result left in place, no difference counted */
    orig_conv = ds;
    unsigned long bad0 = chk_bad[H_CONV];
    for (int it = 0; it < 200; it++) {
        fill_planes(a + G, 0x300); fill_planes(b + G, 0x300);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(a + G, b + G, (uint32_t *)(o1 + G));
        chk_conv(a + G, b + G, (uint32_t *)(o2 + G));
        if (ut_cmp("conv32_2x check wrapper", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_CONV] != bad0) { fprintf(stderr, "FAIL conv32_2x check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "conv32_2x: %u random lines + 200 through the check wrapper equal\n", n);
}

/* ---- the 1x line conversion ---- */
typedef void (*conv1_fn_t)(const uint8_t *, uint32_t *);
static uint8_t conv1_in[NCONV][0x300], conv1_ref[NCONV][1024 + 2 * G];

static void test_conv1(void) {
    conv1_fn_t ds = DS(conv1_fn_t, DS_CONV_32_1X);
    static uint8_t a[0x300 + 2 * G], o1[1024 + 2 * G], o2[1024 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 3000; it++) {
        fill_planes(a + G, 0x300); rndfill(a, G); rndfill(a + G + 0x300, G);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(a + G, (uint32_t *)(o1 + G));
        ds2d_conv32_1x(a + G, (uint32_t *)(o2 + G));
        if (ut_cmp("conv32_1x", o1, o2, sizeof o1)) return;
        if (it < NCONV) { memcpy(conv1_in[it], a + G, 0x300); memcpy(conv1_ref[it], o1, sizeof o1); }
        n++;
    }
    orig_conv1 = ds;
    unsigned long bad0 = chk_bad[H_CONV1X];
    for (int it = 0; it < 200; it++) {
        fill_planes(a + G, 0x300);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(a + G, (uint32_t *)(o1 + G));
        chk_conv1(a + G, (uint32_t *)(o2 + G));
        if (ut_cmp("conv32_1x check wrapper", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_CONV1X] != bad0) { fprintf(stderr, "FAIL conv32_1x check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "conv32_1x: %u random lines + 200 through the check wrapper equal\n", n);
}

/* ---- the 4bpp text BG span ---- */
#define NSP 32
typedef void (*span4_fn_t)(uint16_t *, uint8_t *, const uint16_t *, const uint8_t *, const uint16_t *, const uint8_t *);
static uint8_t *sp_tiles, *sp_tiles0;                           /* 64 KiB + 4: any u16 offset; the first fill */
static struct { uint16_t pal[256], entries[36]; uint8_t flags[36]; uint8_t dst[528 + 2 * G], rows[136 + 2 * G]; } sp_case[NSP];

static void span4_inputs(uint16_t *pal, uint16_t *entries, uint8_t *flags) {
    rndfill(pal, 512);
    rndfill(entries, 72);
    rndfill(flags, 36);
    if (rnd(2)) for (int i = 0; i < 36; i++) entries[i] = (uint16_t)(rnd(1024) * 32 + 4 * rnd(8));  /* tile rows */
    if (rnd(3) == 0) for (int i = 0; i < 36; i++) flags[i] &= 0xf4;                                  /* map bits */
}

static void test_span4(void) {
    span4_fn_t ds = DS(span4_fn_t, DS_SPAN4);
    sp_tiles = malloc(65536 + 4);
    static uint16_t pal[256], entries[36];
    static uint8_t flags[36], d1[528 + 2 * G], d2[528 + 2 * G], r1[136 + 2 * G], r2[136 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 3000; it++) {
        if (it % 64 == 0) rndfill(sp_tiles, 65536 + 4);
        if (it == 0) { sp_tiles0 = malloc(65536 + 4); memcpy(sp_tiles0, sp_tiles, 65536 + 4); }
        span4_inputs(pal, entries, flags);
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        rndfill(r1, sizeof r1); memcpy(r2, r1, sizeof r1);
        ds((uint16_t *)(d1 + G), r1 + G, pal, sp_tiles, entries, flags);
        ds2d_span4((uint16_t *)(d2 + G), r2 + G, pal, sp_tiles, entries, flags);
        if (ut_cmp("span4 pixels", d1, d2, sizeof d1) || ut_cmp("span4 rows", r1, r2, sizeof r1)) return;
        if (it < NSP) {
            memcpy(sp_case[it].pal, pal, sizeof pal); memcpy(sp_case[it].entries, entries, sizeof entries);
            memcpy(sp_case[it].flags, flags, sizeof flags); memcpy(sp_case[it].dst, d1, sizeof d1); memcpy(sp_case[it].rows, r1, sizeof r1);
        }
        n++;
    }
    /* the check mode's way to the original (its first four instructions' effects, then a jump to its fifth) */
    ds2d_span4_consts = ut_base + DS_SPAN4_CONSTS;
    ds2d_span4_cont = ut_base + DS_SPAN4 + 16;
    unsigned long bad0 = chk_bad[H_SPAN4];
    for (int it = 0; it < 200; it++) {
        span4_inputs(pal, entries, flags);
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        rndfill(r1, sizeof r1); memcpy(r2, r1, sizeof r1);
        ds((uint16_t *)(d1 + G), r1 + G, pal, sp_tiles, entries, flags);
        ds2d_span4_orig((uint16_t *)(d2 + G), r2 + G, pal, sp_tiles, entries, flags);
        if (ut_cmp("span4 original through ds2d_span4_orig", d1, d2, sizeof d1) || ut_cmp("span4 rows through ds2d_span4_orig", r1, r2, sizeof r1)) return;
        memcpy(d2, d1, G); rndfill(d2 + G, 528); memcpy(r2, r1, G); rndfill(r2 + G, 136);
        chk_span4((uint16_t *)(d2 + G), r2 + G, pal, sp_tiles, entries, flags);
        if (ut_cmp("span4 check wrapper", d1, d2, sizeof d1) || ut_cmp("span4 check wrapper rows", r1, r2, sizeof r1)) return;
    }
    if (chk_bad[H_SPAN4] != bad0) { fprintf(stderr, "FAIL span4 check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "span4: %u random lines, 200 through ds2d_span4_orig and the check wrapper equal\n", n);
}

/* ---- select_pixels ---- */
/* a mask: empty, full, random density, or runs */
static void fill_mask(uint8_t *m) {
    switch (rnd(5)) {
    case 0: memset(m, 0, 32); break;
    case 1: memset(m, 0xff, 32); break;
    case 2: { unsigned d = rnd(9); for (int i = 0; i < 32; i++) { uint8_t v = 0; for (int b = 0; b < 8; b++) v |= (rnd(8) < d) << b; m[i] = v; } break; }
    case 3: { int a = rnd(256), e = a + rnd(256 - a + 1); memset(m, 0, 32); for (int i = a; i < e; i++) m[i >> 3] |= 1 << (i & 7); break; }
    default: rndfill(m, 32); for (int i = 0; i < 32; i++) if (rnd(3)) m[i] = 0; break;
    }
}

#define NSEL 64
typedef void (*sel_fn)(uint8_t *, uint8_t *, const uint8_t *, uint8_t **, const uint32_t *, uint8_t *, uint64_t);
static struct sel_case {
    uint8_t eng[0x100]; uint16_t bd; uint8_t excl[0x400]; uint8_t lbuf[8][0x210]; uint32_t px[256];
    uint64_t lmask; int has3d, hasalpha; uint8_t alpha_in[256 + 2 * G];
    uint8_t out_ref[0x300 + 2 * G], alpha_ref[256 + 2 * G];
} *sc;

static void sel_args(struct sel_case *c, uint8_t **layers) {
    for (int k = 0; k < 8; k++) layers[k] = c->lbuf[k];
    *(uint16_t **)(c->eng + 0x18) = &c->bd;
}

static void test_select(void) {
    sel_fn ds = DS(sel_fn, DS_SELECT_PIXELS_2D);
    sc = calloc(NSEL + 1, sizeof *sc);
    struct sel_case *c = &sc[NSEL];
    static uint8_t o1[0x300 + 2 * G], o2[0x300 + 2 * G], a1[256 + 2 * G], a2[256 + 2 * G], o3[0x300 + 2 * G];
    unsigned n = 0, nk[8] = { 0 };
    orig_select = ds;
    for (int it = 0; it < 20000; it++) {
        rndfill(c, sizeof *c);
        /* the layer mask: DraStic's bits 0-4 (BG0-3, OBJ), sometimes 0 or bit 5 / 6 too */
        uint32_t lm = rnd(32);
        if (!rnd(16)) lm |= 1u << (5 + rnd(2));
        if (!rnd(20)) lm = 0;
        c->lmask = lm | (rnd64() << 32);                 /* the register's upper half: undefined */
        for (int k = 0; k < 8; k++) fill_mask(c->excl + 32 * k);
        for (int k = 0; k < 8; k++) if (rnd(2)) for (int i = 0; i < 0x210; i += 2) c->lbuf[k][i + 1] &= 0x7f;
        if (rnd(2)) c->bd &= 0x7fff;
        if (rnd(3) == 0) for (int i = 0; i < 256; i++) c->px[i] = (c->px[i] & 0x003f3f3f) | (rnd(2) ? 0x1f000000 : 0);
        c->has3d = rnd(4) != 0; c->hasalpha = rnd(2);
        uint8_t *layers[8];
        sel_args(c, layers);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1); memcpy(o3, o1, sizeof o1);
        memcpy(a1, c->alpha_in, sizeof a1); memcpy(a2, a1, sizeof a1);
        const uint32_t *p3d = c->has3d ? c->px : 0;
        ds(c->eng, o1 + G, c->excl, layers, p3d, c->hasalpha ? a1 + G : 0, c->lmask);
        ds2d_select(c->eng, o2 + G, c->excl, layers, p3d, c->hasalpha ? a2 + G : 0, c->lmask);
        if (ut_cmp("select_pixels planes", o1, o2, sizeof o1) || ut_cmp("select_pixels alpha", a1, a2, sizeof a1)) {
            fprintf(stderr, "  lmask %#x 3d %d alpha %d\n", (uint32_t)c->lmask, c->has3d, c->hasalpha);
            return;
        }
        /* the C port (spec/composite.c) agrees with both */
        spec_render_scanline_select_pixels(c->eng, o3 + G, c->excl, layers, p3d, 0, (uint32_t)c->lmask);
        if (!c->hasalpha && ut_cmp("select_pixels C port", o1, o3, sizeof o1)) return;
        if (it < NSEL) {
            sc[it] = *c;
            memcpy(sc[it].out_ref, o1, sizeof o1); memcpy(sc[it].alpha_ref, a1, sizeof a1);
        }
        n++; nk[__builtin_popcount((uint32_t)c->lmask & 0xff)]++;
    }
    /* overlapping buffers go to the original (here DraStic's function itself) */
    {
        uint8_t *layers[8];
        rndfill(c, sizeof *c); c->lmask = 0x13; c->has3d = 1;
        sel_args(c, layers);
        uint8_t *buf = c->lbuf[1] + 0x10;               /* out = layer 1's line */
        static uint8_t save[0x210];
        memcpy(save, c->lbuf[1], sizeof save);
        ds(c->eng, buf, c->excl, layers, c->px, 0, c->lmask);
        memcpy(o1, c->lbuf[1], 0x210); memcpy(c->lbuf[1], save, sizeof save);
        ds2d_select(c->eng, buf, c->excl, layers, c->px, 0, c->lmask);
        if (ut_cmp("select_pixels overlapping", o1, c->lbuf[1], 0x210)) return;
    }
    /* the check wrapper: no difference, our result in place */
    unsigned long bad0 = chk_bad[H_SELECT];
    for (int it = 0; it < 500; it++) {
        rndfill(c, sizeof *c); c->lmask = rnd(32);
        for (int k = 0; k < 8; k++) fill_mask(c->excl + 32 * k);
        uint8_t *layers[8];
        sel_args(c, layers);
        int ha = rnd(2);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1); memcpy(a2, a1, sizeof a1);
        ds(c->eng, o1 + G, c->excl, layers, c->px, ha ? a1 + G : 0, c->lmask);
        chk_select(c->eng, o2 + G, c->excl, layers, c->px, ha ? a2 + G : 0, c->lmask);
        if (ut_cmp("select_pixels check wrapper", o1, o2, sizeof o1) || ut_cmp("select_pixels check wrapper alpha", a1, a2, sizeof a1)) return;
    }
    if (chk_bad[H_SELECT] != bad0) { fprintf(stderr, "FAIL select_pixels check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "select_pixels: %u random quarters equal (layers 0..5: %u %u %u %u %u %u), the overlapping case, 500 "
            "through the check wrapper\n", n, nk[0], nk[1], nk[2], nk[3], nk[4], nk[5]);
}

/* ---- color_effects_apply ---- */
#define NEFF 64
static uint8_t eff_in[NEFF][0x800], eff_ref[NEFF][0x300 + 2 * G];
typedef void (*eff_fn)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);

/* weights: DraStic's (0..16 / 0..31, one pair a pixel), any byte, or extremes */
static void fill_weights(uint8_t *w) {
    switch (rnd(3)) {
    case 0: for (int i = 0; i < 0x200; i++) w[i] = rnd(17); break;
    case 1: rndfill(w, 0x200); break;
    default: for (int i = 0; i < 0x200; i++) w[i] = rnd(2) ? 0xff : rnd(33); break;
    }
}

static void test_effects(void) {
    eff_fn ds = DS(eff_fn, DS_EFFECTS_APPLY);
    static uint8_t in[0x800], o1[0x300 + 2 * G], o2[0x300 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 3000; it++) {
        fill_planes(in, 0x600); fill_weights(in + 0x600);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(o1 + G, in, in + 0x600, in + 0x700);
        ds2d_effects_apply(o2 + G, in, in + 0x600, in + 0x700);
        if (ut_cmp("effects_apply", o1, o2, sizeof o1)) return;
        if (it < NEFF) { memcpy(eff_in[it], in, sizeof in); memcpy(eff_ref[it], o1, sizeof o1); }
        n++;
    }
    orig_effects = ds;
    unsigned long bad0 = chk_bad[H_EFFECTS];
    for (int it = 0; it < 200; it++) {
        fill_planes(in, 0x600); fill_weights(in + 0x600);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(o1 + G, in, in + 0x600, in + 0x700);
        chk_effects(o2 + G, in, in + 0x600, in + 0x700);
        if (ut_cmp("effects_apply check wrapper", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_EFFECTS] != bad0) { fprintf(stderr, "FAIL effects_apply check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "effects_apply: %u random quarters + 200 through the check wrapper equal\n", n);
}

/* ---- select_blend_enable ---- */
#define NBL 64
typedef void (*blend_fn_t)(uint8_t *, const uint8_t *, uint64_t, uint64_t);
static struct { uint8_t m[0xc0]; uint64_t a, b; uint8_t ref[32 + 2 * G]; } bl_case[NBL];

static void test_blend(void) {
    blend_fn_t ds = DS(blend_fn_t, DS_BLEND_ENABLE);
    static uint8_t m[0xc0], o1[32 + 2 * G], o2[32 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 20000; it++) {
        for (int k = 0; k < 6; k++) fill_mask(m + 32 * k);
        uint64_t a = rnd64(), b = rnd64();                 /* any bits, the upper ones too */
        if (rnd(2)) { a &= 0x3f; b &= 0x3f; }
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(o1 + G, m, a, b);
        ds2d_blend_enable(o2 + G, m, a, b);
        if (ut_cmp("blend_enable", o1, o2, sizeof o1)) { fprintf(stderr, "  a %#lx b %#lx\n", (unsigned long)a, (unsigned long)b); return; }
        if (it < NBL) { memcpy(bl_case[it].m, m, sizeof m); bl_case[it].a = a; bl_case[it].b = b; memcpy(bl_case[it].ref, o1, sizeof o1); }
        n++;
    }
    /* the output on the masks (the caller's buffers are apart, but the original reads every mask before its store) */
    {
        static uint8_t buf[0xc0 + 2 * G], buf2[0xc0 + 2 * G];
        for (int it = 0; it < 200; it++) {
            rndfill(buf, sizeof buf); memcpy(buf2, buf, sizeof buf);
            uint64_t a = rnd(64), b = rnd(64);
            int off = rnd(0xa0 / 16 + 1) * 16;
            ds(buf + G + off, buf + G, a, b);
            ds2d_blend_enable(buf2 + G + off, buf2 + G, a, b);
            if (ut_cmp("blend_enable over its masks", buf, buf2, sizeof buf)) return;
        }
    }
    orig_blend = ds;
    unsigned long bad0 = chk_bad[H_BLEND];
    for (int it = 0; it < 200; it++) {
        for (int k = 0; k < 6; k++) fill_mask(m + 32 * k);
        uint64_t a = rnd(64), b = rnd(64);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(o1 + G, m, a, b);
        chk_blend(o2 + G, m, a, b);
        if (ut_cmp("blend_enable check wrapper", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_BLEND] != bad0) { fprintf(stderr, "FAIL blend_enable check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "blend_enable: %u random calls, 200 over its own masks, 200 through the check wrapper equal\n", n);
}

/* ---- priority_encode_single ---- */
#define NPR 64
typedef void (*prio_fn_t)(uint8_t *, uint8_t *, uint8_t *);
static struct { uint8_t eng[0x100], S[0x700], ref[0x700]; } pr_case[NPR];

/* a scratch area: vis at +0x40 (8 slots and beyond), excl at +0x380; random bitmaps, a layer list as
 * video_2d_reorder_layers builds them (per priority: OBJ slot 4 + p, then BGs) or random slots 0..7, any length */
static void prio_case(uint8_t *eng, uint8_t *S) {
    rndfill(eng, 0x100); rndfill(S, 0x700);
    for (int k = 0; k < 16; k++) fill_mask(S + 0x40 + 32 * k);
    unsigned n = 0;
    if (rnd(2)) {
        for (unsigned p = 0; p < 4; p++) {
            if (rnd(2)) eng[0x84 + n++] = 4 + p;
            for (unsigned bg = 0; bg < 4; bg++) if (rnd(3) == 0) eng[0x84 + n++] = bg;
        }
    } else { n = rnd(9); for (unsigned k = 0; k < n; k++) eng[0x84 + k] = rnd(8); }
    eng[0xb3] = n;
}

static void test_prio(void) {
    prio_fn_t ds = DS(prio_fn_t, DS_PRIO_SINGLE);
    static uint8_t eng[0x100], S1[0x700], S2[0x700], eng2[0x100];
    unsigned n = 0;
    for (int it = 0; it < 20000; it++) {
        prio_case(eng, S1);
        memcpy(S2, S1, sizeof S1); memcpy(eng2, eng, sizeof eng);
        if (it < NPR) memcpy(pr_case[it].S, S1, sizeof S1);
        ds(eng, S1 + 0x40, S1 + 0x380);
        ds2d_prio_single(eng2, S2 + 0x40, S2 + 0x380);
        if (ut_cmp("prio_single", S1, S2, sizeof S1) || ut_cmp("prio_single eng", eng, eng2, sizeof eng)) return;
        if (it < NPR) {
            memcpy(pr_case[it].eng, eng, sizeof eng); memcpy(pr_case[it].ref, S1, sizeof S1);
        }
        n++;
    }
    /* overlapping vis and excl (a list slot's vis is an excl slot): the same per-layer order of loads and stores */
    for (int it = 0; it < 2000; it++) {
        prio_case(eng, S1);
        memcpy(S2, S1, sizeof S1);
        int d = 32 * rnd(12);
        ds(eng, S1 + 0x40, S1 + 0x40 + d);
        ds2d_prio_single(eng, S2 + 0x40, S2 + 0x40 + d);
        if (ut_cmp("prio_single overlapping", S1, S2, sizeof S1)) return;
    }
    orig_prio = ds;
    unsigned long bad0 = chk_bad[H_PRIO];
    for (int it = 0; it < 200; it++) {
        prio_case(eng, S1);
        memcpy(S2, S1, sizeof S1);
        ds(eng, S1 + 0x40, S1 + 0x380);
        chk_prio(eng, S2 + 0x40, S2 + 0x380);
        if (ut_cmp("prio_single check wrapper", S1, S2, sizeof S1)) return;
    }
    if (chk_bad[H_PRIO] != bad0) { fprintf(stderr, "FAIL prio_single check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "prio_single: %u random calls, 2000 with overlapping vis / excl, 200 through the check wrapper equal\n", n);
}

static void test_patched(void) {
    setenv("RAST_2D", "-1", 1);
    unsetenv("RAST_2DCHECK");
    ds2d_init();
    if (!orig_conv) { fprintf(stderr, "FAIL ds2d_init did not hook\n"); ut_fail++; return; }
    conv_fn entry = DS(conv_fn, DS_CONV_32_2X);
    static uint8_t o[2048 + 2 * G];
    for (int i = 0; i < NCONV; i++) {
        memcpy(o, conv_ref[i], G); memcpy(o + G + 2048, conv_ref[i] + G + 2048, G);
        memset(o + G, 0x5a, 2048);
        entry(conv_in[i][0], conv_in[i][1], (uint32_t *)(o + G));
        if (ut_cmp("conv32_2x patched entry", conv_ref[i], o, sizeof o)) return;
        /* and the trampoline is DraStic's function */
        memset(o + G, 0xa5, 2048);
        orig_conv(conv_in[i][0], conv_in[i][1], (uint32_t *)(o + G));
        if (ut_cmp("conv32_2x trampoline", conv_ref[i], o, sizeof o)) return;
    }
    if (!orig_select) { fprintf(stderr, "FAIL ds2d_init did not hook select_pixels\n"); ut_fail++; return; }
    sel_fn sentry = DS(sel_fn, DS_SELECT_PIXELS_2D);
    static uint8_t so[0x300 + 2 * G], sa[256 + 2 * G];
    for (int i = 0; i < NSEL; i++) {
        struct sel_case *c = &sc[i];
        uint8_t *layers[8];
        sel_args(c, layers);
        memcpy(so, c->out_ref, G); memcpy(so + G + 0x300, c->out_ref + G + 0x300, G); memset(so + G, 0x5a, 0x300);
        memcpy(sa, c->alpha_in, sizeof sa);
        sentry(c->eng, so + G, c->excl, layers, c->has3d ? c->px : 0, c->hasalpha ? sa + G : 0, c->lmask);
        if (ut_cmp("select_pixels patched entry", c->out_ref, so, sizeof so) || ut_cmp("select_pixels patched entry alpha", c->alpha_ref, sa, sizeof sa)) return;
        memset(so + G, 0xa5, 0x300); memcpy(sa, c->alpha_in, sizeof sa);
        orig_select(c->eng, so + G, c->excl, layers, c->has3d ? c->px : 0, c->hasalpha ? sa + G : 0, c->lmask);
        if (ut_cmp("select_pixels trampoline", c->out_ref, so, sizeof so) || ut_cmp("select_pixels trampoline alpha", c->alpha_ref, sa, sizeof sa)) return;
    }
    if (!orig_effects) { fprintf(stderr, "FAIL ds2d_init did not hook color_effects_apply\n"); ut_fail++; return; }
    eff_fn eentry = DS(eff_fn, DS_EFFECTS_APPLY);
    for (int i = 0; i < NEFF; i++) {
        memcpy(so, eff_ref[i], G); memcpy(so + G + 0x300, eff_ref[i] + G + 0x300, G); memset(so + G, 0x5a, 0x300);
        eentry(so + G, eff_in[i], eff_in[i] + 0x600, eff_in[i] + 0x700);
        if (ut_cmp("effects_apply patched entry", eff_ref[i], so, sizeof so)) return;
        memset(so + G, 0xa5, 0x300);
        orig_effects(so + G, eff_in[i], eff_in[i] + 0x600, eff_in[i] + 0x700);
        if (ut_cmp("effects_apply trampoline", eff_ref[i], so, sizeof so)) return;
    }
    if (!orig_blend) { fprintf(stderr, "FAIL ds2d_init did not hook select_blend_enable\n"); ut_fail++; return; }
    blend_fn_t bentry = DS(blend_fn_t, DS_BLEND_ENABLE);
    for (int i = 0; i < NBL; i++) {
        static uint8_t bo[32 + 2 * G];
        memcpy(bo, bl_case[i].ref, sizeof bo); memset(bo + G, 0x5a, 32);
        bentry(bo + G, bl_case[i].m, bl_case[i].a, bl_case[i].b);
        if (ut_cmp("blend_enable patched entry", bl_case[i].ref, bo, sizeof bo)) return;
        memset(bo + G, 0xa5, 32);
        orig_blend(bo + G, bl_case[i].m, bl_case[i].a, bl_case[i].b);
        if (ut_cmp("blend_enable trampoline", bl_case[i].ref, bo, sizeof bo)) return;
    }
    if (!orig_prio) { fprintf(stderr, "FAIL ds2d_init did not hook priority_encode_single\n"); ut_fail++; return; }
    prio_fn_t pentry = DS(prio_fn_t, DS_PRIO_SINGLE);
    for (int i = 0; i < NPR; i++) {
        static uint8_t S[0x700];
        memcpy(S, pr_case[i].S, sizeof S);
        pentry(pr_case[i].eng, S + 0x40, S + 0x380);
        if (ut_cmp("prio_single patched entry", pr_case[i].ref, S, sizeof S)) return;
        memcpy(S, pr_case[i].S, sizeof S);
        orig_prio(pr_case[i].eng, S + 0x40, S + 0x380);
        if (ut_cmp("prio_single trampoline", pr_case[i].ref, S, sizeof S)) return;
    }
    if (!orig_conv1) { fprintf(stderr, "FAIL ds2d_init did not hook color_convert_direct_32_1x\n"); ut_fail++; return; }
    conv1_fn_t c1entry = DS(conv1_fn_t, DS_CONV_32_1X);
    for (int i = 0; i < NCONV; i++) {
        static uint8_t o1[1024 + 2 * G];
        memcpy(o1, conv1_ref[i], sizeof o1); memset(o1 + G, 0x5a, 1024);
        c1entry(conv1_in[i], (uint32_t *)(o1 + G));
        if (ut_cmp("conv32_1x patched entry", conv1_ref[i], o1, sizeof o1)) return;
        memset(o1 + G, 0xa5, 1024);
        orig_conv1(conv1_in[i], (uint32_t *)(o1 + G));
        if (ut_cmp("conv32_1x trampoline", conv1_ref[i], o1, sizeof o1)) return;
    }
    span4_fn_t s4entry = DS(span4_fn_t, DS_SPAN4);
    for (int i = 0; i < NSP; i++) {
        static uint8_t d[528 + 2 * G], r[136 + 2 * G];
        memcpy(d, sp_case[i].dst, sizeof d); memset(d + G, 0x5a, 528);
        memcpy(r, sp_case[i].rows, sizeof r); memset(r + G, 0x5a, 136);
        s4entry((uint16_t *)(d + G), r + G, sp_case[i].pal, sp_tiles0, sp_case[i].entries, sp_case[i].flags);
        if (ut_cmp("span4 patched entry", sp_case[i].dst, d, sizeof d) || ut_cmp("span4 patched entry rows", sp_case[i].rows, r, sizeof r)) return;
        memset(d + G, 0xa5, 528); memset(r + G, 0xa5, 136);
        ds2d_span4_orig((uint16_t *)(d + G), r + G, sp_case[i].pal, sp_tiles0, sp_case[i].entries, sp_case[i].flags);
        if (ut_cmp("span4 original after the patch", sp_case[i].dst, d, sizeof d) || ut_cmp("span4 original after the patch rows", sp_case[i].rows, r, sizeof r)) return;
    }
    fprintf(stderr, "patched entries: %d calls each equal\n", NCONV);
}

void ut_main(void) {
    ds_base = ut_base;
    test_conv();
    test_conv1();
    test_span4();
    test_select();
    test_effects();
    test_blend();
    test_prio();
    test_patched();
}
