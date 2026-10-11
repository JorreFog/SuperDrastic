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
    sp_tiles = malloc(65536 + 8);
    static uint16_t pal[256], entries[36];
    static uint8_t flags[36], d1[528 + 2 * G], d2[528 + 2 * G], r1[136 + 2 * G], r2[136 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 3000; it++) {
        if (it % 64 == 0) rndfill(sp_tiles, 65536 + 8);
        if (it == 0) { sp_tiles0 = malloc(65536 + 8); memcpy(sp_tiles0, sp_tiles, 65536 + 8); }
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


/* ---- the palette lookups ---- */
#define NPAL 32
typedef void (*pal8_fn_t)(uint16_t *, const uint8_t *, const uint16_t *, int);
typedef void (*pal12_fn_t)(uint16_t *, const uint16_t *, const uint16_t *, int);
static struct { uint8_t buf[0x800 + 2 * G]; uint16_t pal[4096]; int dst, src, n; uint8_t ref[0x800 + 2 * G]; } pal_case[2][NPAL];

/* a count as the callers pass them (256, or a clipped run's length), or any */
static int pal_n(void) {
    switch (rnd(4)) {
    case 0: return 256;
    case 1: return 1 + rnd(256);
    case 2: return rnd(20) - 4;
    default: return 1 + rnd(24);
    }
}
static void test_pal(void) {
    pal8_fn_t ds8 = DS(pal8_fn_t, DS_PAL8);
    pal12_fn_t ds12 = DS(pal12_fn_t, DS_PAL12);
    static uint8_t b1[0x800 + 2 * G], b2[0x800 + 2 * G];
    static uint16_t pal[4096];
    unsigned n8 = 0, n12 = 0, nplain = 0;
    for (int it = 0; it < 6000; it++) {
        int ext = it & 1;
        rndfill(b1, sizeof b1); rndfill(pal, sizeof pal);
        if (rnd(2)) for (int i = 0; i < 0x800; i++) b1[G + i] = rnd(2) ? rnd(16) : (uint8_t)rnd64();     /* small indexes too */
        int n = pal_n(), it8 = pal_iters(n);
        int dst, src, poff = -1;                          /* poff >= 0: the 8bpp palette inside the buffer */
        switch (rnd(7)) {
        case 0: dst = 0; src = ext ? 0 : 0x110 - rnd(8); break;                 /* tiled_ext: in place (12bpp), src above dst (8bpp) */
        case 1: { int s = rnd(256), e = s + rnd(256 - s); n = e - s + 1; it8 = pal_iters(n);
                  dst = 2 * s; src = ext ? 2 * s : 0x100 + s; break; }          /* the affine runs */
        case 2: dst = rnd(0x400); src = dst + rnd(80) - 40; nplain++; break;      /* near aliasing: the plain path */
        case 3: if (!ext && it >= 2 * NPAL) { poff = 2 * rnd(0x2c0); dst = poff + rnd(1100) - 600; if (dst < 0) dst = 0; src = 0x500 + rnd(0x100);
                            nplain++; break; }                  /* dst over the palette: the plain path */
                /* fall through */
        default: dst = rnd(0x400); src = rnd(0x400); break;
        }
        if (src < 0) src = 0;
        if (dst + 16 * it8 > 0x800 || src + (ext ? 16 : 8) * it8 > 0x800) continue;
        memcpy(b2, b1, sizeof b1);
        if (it < 2 * NPAL) {                           /* the inputs, for the patched-entry test */
            memcpy(pal_case[ext][it / 2].buf, b1, sizeof b1); memcpy(pal_case[ext][it / 2].pal, pal, sizeof pal);
            pal_case[ext][it / 2].dst = dst; pal_case[ext][it / 2].src = src; pal_case[ext][it / 2].n = n;
        }
        if (ext) { ds12((uint16_t *)(b1 + G + dst), (const uint16_t *)(b1 + G + src), pal, n); ds2d_pal12((uint16_t *)(b2 + G + dst), (const uint16_t *)(b2 + G + src), pal, n); n12++; }
        else if (poff >= 0) { ds8((uint16_t *)(b1 + G + dst), b1 + G + src, (const uint16_t *)(b1 + G + poff), n);
                              ds2d_pal8((uint16_t *)(b2 + G + dst), b2 + G + src, (const uint16_t *)(b2 + G + poff), n); n8++; }
        else { ds8((uint16_t *)(b1 + G + dst), b1 + G + src, pal, n); ds2d_pal8((uint16_t *)(b2 + G + dst), b2 + G + src, pal, n); n8++; }
        if (ut_cmp(ext ? "palette_lookup_12bpp" : "palette_lookup_8bpp", b1, b2, sizeof b1)) {
            fprintf(stderr, "  dst %d src %d n %d\n", dst, src, n);
            return;
        }
        if (it < 2 * NPAL) memcpy(pal_case[ext][it / 2].ref, b1, sizeof b1);
    }
    /* the private copies of the originals (the check mode's reference), then the check wrappers */
    orig_pal8 = (pal8_fn)copy_orig(DS_PAL8, 112);
    orig_pal12 = (pal12_fn)copy_orig(DS_PAL12, 116);
    if (!orig_pal8 || !orig_pal12) { fprintf(stderr, "FAIL copy_orig\n"); ut_fail++; return; }
    unsigned long bad0 = chk_bad[H_PAL8] + chk_bad[H_PAL12];
    static uint8_t b0[0x800 + 2 * G];
    for (int it = 0; it < 400; it++) {
        int ext = it & 1, n = pal_n();
        rndfill(b0, sizeof b0); rndfill(pal, sizeof pal);
        int dst = rnd(0x200), src = rnd(0x200);
        memcpy(b1, b0, sizeof b0); memcpy(b2, b0, sizeof b0);
        if (ext) {
            ds12((uint16_t *)(b1 + G + dst), (const uint16_t *)(b1 + G + src), pal, n);
            orig_pal12((uint16_t *)(b2 + G + dst), (const uint16_t *)(b2 + G + src), pal, n);
            if (ut_cmp("palette_lookup_12bpp copy of the original", b1, b2, sizeof b1)) return;
            memcpy(b2, b0, sizeof b0);
            chk_pal12((uint16_t *)(b2 + G + dst), (const uint16_t *)(b2 + G + src), pal, n);
            if (ut_cmp("palette_lookup_12bpp check wrapper", b1, b2, sizeof b1)) return;
        } else {
            ds8((uint16_t *)(b1 + G + dst), b1 + G + src, pal, n);
            orig_pal8((uint16_t *)(b2 + G + dst), b2 + G + src, pal, n);
            if (ut_cmp("palette_lookup_8bpp copy of the original", b1, b2, sizeof b1)) return;
            memcpy(b2, b0, sizeof b0);
            chk_pal8((uint16_t *)(b2 + G + dst), b2 + G + src, pal, n);
            if (ut_cmp("palette_lookup_8bpp check wrapper", b1, b2, sizeof b1)) return;
        }
    }
    if (chk_bad[H_PAL8] + chk_bad[H_PAL12] != bad0) { fprintf(stderr, "FAIL palette_lookup check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "palette_lookup: %u 8bpp + %u 12bpp random calls equal (%u near-aliasing through the plain path), 200 each "
            "through the copies and the check wrappers\n", n8, n12, nplain);
}

/* ---- priority_encode_double ---- */
typedef void (*prio2_fn_t)(uint8_t *, uint8_t *, uint8_t *, uint8_t *);
static struct { uint8_t eng[0x100], S[0x900], ref[0x900]; } p2_case[NPR];

static void test_prio2(void) {
    prio2_fn_t ds = DS(prio2_fn_t, DS_PRIO_DOUBLE);
    static uint8_t eng[0x100], S1[0x900], S2[0x900], eng2[0x100];
    unsigned n = 0;
    for (int it = 0; it < 20000; it++) {
        prio_case(eng, S1);                                /* vis at +0x40, excl at +0x380; excl2 at +0x600 */
        rndfill(S1 + 0x700, 0x200);
        memcpy(S2, S1, sizeof S1); memcpy(eng2, eng, sizeof eng);
        if (it < NPR) memcpy(p2_case[it].S, S1, sizeof S1);
        ds(eng, S1 + 0x40, S1 + 0x380, S1 + 0x600);
        ds2d_prio_double(eng2, S2 + 0x40, S2 + 0x380, S2 + 0x600);
        if (ut_cmp("prio_double", S1, S2, sizeof S1) || ut_cmp("prio_double eng", eng, eng2, sizeof eng)) return;
        if (it < NPR) { memcpy(p2_case[it].eng, eng, sizeof eng); memcpy(p2_case[it].ref, S1, sizeof S1); }
        n++;
    }
    /* overlapping vis / excl / excl2: the same order of loads and stores per layer */
    for (int it = 0; it < 3000; it++) {
        prio_case(eng, S1);
        rndfill(S1 + 0x700, 0x200);
        memcpy(S2, S1, sizeof S1);
        int d1 = 32 * rnd(14), d2 = 32 * rnd(14);
        ds(eng, S1 + 0x40, S1 + 0x40 + d1, S1 + 0x40 + d2);
        ds2d_prio_double(eng, S2 + 0x40, S2 + 0x40 + d1, S2 + 0x40 + d2);
        if (ut_cmp("prio_double overlapping", S1, S2, sizeof S1)) return;
    }
    orig_prio2 = ds;
    unsigned long bad0 = chk_bad[H_PRIO2];
    for (int it = 0; it < 200; it++) {
        prio_case(eng, S1);
        rndfill(S1 + 0x700, 0x200);
        memcpy(S2, S1, sizeof S1);
        ds(eng, S1 + 0x40, S1 + 0x380, S1 + 0x600);
        chk_prio2(eng, S2 + 0x40, S2 + 0x380, S2 + 0x600);
        if (ut_cmp("prio_double check wrapper", S1, S2, sizeof S1)) return;
    }
    if (chk_bad[H_PRIO2] != bad0) { fprintf(stderr, "FAIL prio_double check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "prio_double: %u random calls, 3000 with overlapping buffers, 200 through the check wrapper equal\n", n);
}

/* ---- the effects' weight setups and the shade ---- */
typedef void (*alpha_fn_t)(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*blendw_fn_t)(uint32_t, uint8_t *, uint8_t *, const uint8_t *);
typedef void (*shade_fn_t)(const uint8_t *, uint8_t *, const uint8_t *, const uint8_t *);
#define NFX 16
/* one buffer holds wa, wb, alpha and the mask at random (possibly aliasing) offsets */
static struct { uint8_t buf[0x800]; int wa, wb, al, mk; uint32_t w; uint8_t ref[0x800]; } fx_case[4][NFX];
static struct { uint8_t eng[0x100], in[0x300], mask[32], out[0x300 + 2 * G]; } sh_case[NFX];

static void fx_layout(int *wa, int *wb, int *al, int *mk) {
    if (rnd(3)) { *wa = rnd(0x100); *wb = 0x200 + rnd(0x100); *al = 0x400 + rnd(0x100); *mk = 0x600 + rnd(0x100); return; }
    *wa = rnd(0x400); *wb = rnd(0x400); *al = rnd(0x400); *mk = rnd(0x700);     /* aliasing layouts */
}
static void fx_mask(uint8_t *m) {
    fill_mask(m);
    if (rnd(4) == 0) memset(m, 0, 32);
}
static void test_effects_setup(void) {
    alpha_fn_t dsab = DS(alpha_fn_t, DS_SETUP_ALPHA_BASE), dsa = DS(alpha_fn_t, DS_SETUP_ALPHA);
    blendw_fn_t dsbb = DS(blendw_fn_t, DS_SETUP_BLEND_BASE), dsb = DS(blendw_fn_t, DS_SETUP_BLEND);
    static uint8_t b1[0x800], b2[0x800];
    static const char *nm[4] = { "setup_alpha_base", "setup_alpha", "setup_blend_base", "setup_blend" };
    unsigned n[4] = { 0 };
    for (int it = 0; it < 12000; it++) {
        int f = it & 3, wa, wb, al, mk;
        rndfill(b1, sizeof b1);
        fx_layout(&wa, &wb, &al, &mk);
        fx_mask(b1 + mk);
        if (rnd(2)) for (int i = 0; i < 256; i++) b1[al + i] = rnd(32);                 /* 3D alpha values */
        uint32_t w = rnd(2) ? rnd(0x10000) : (uint32_t)rnd64();
        memcpy(b2, b1, sizeof b1);
        switch (f) {
        case 0: dsab(b1 + wa, b1 + wb, b1 + al, b1 + mk); ds2d_setup_alpha_base(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 1: dsa(b1 + wa, b1 + wb, b1 + al, b1 + mk); ds2d_setup_alpha(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 2: dsbb(w, b1 + wa, b1 + wb, b1 + mk); ds2d_setup_blend_base(w, b2 + wa, b2 + wb, b2 + mk); break;
        default: dsb(w, b1 + wa, b1 + wb, b1 + mk); ds2d_setup_blend(w, b2 + wa, b2 + wb, b2 + mk); break;
        }
        if (ut_cmp(nm[f], b1, b2, sizeof b1)) { fprintf(stderr, "  wa %d wb %d alpha %d mask %d w %#x\n", wa, wb, al, mk, w); return; }
        if (it / 4 < NFX) {
            int k = it / 4;
            memcpy(fx_case[f][k].buf, b2, sizeof b2); memcpy(fx_case[f][k].ref, b1, sizeof b1);    /* b2 == b1 now: the inputs are in there */
            fx_case[f][k].wa = wa; fx_case[f][k].wb = wb; fx_case[f][k].al = al; fx_case[f][k].mk = mk; fx_case[f][k].w = w;
        }
        n[f]++;
    }
    /* the inputs of the kept cases: regenerate them (the buffers above hold the outputs) */
    for (int f = 0; f < 4; f++) for (int k = 0; k < NFX; k++) {
        rndfill(fx_case[f][k].buf, 0x800);
        fx_case[f][k].wa = rnd(0x100); fx_case[f][k].wb = 0x200 + rnd(0x100); fx_case[f][k].al = 0x400 + rnd(0x100); fx_case[f][k].mk = 0x600 + rnd(0x100);
        fx_mask(fx_case[f][k].buf + fx_case[f][k].mk);
        memcpy(fx_case[f][k].ref, fx_case[f][k].buf, 0x800);
        uint8_t *r = fx_case[f][k].ref;
        switch (f) {
        case 0: dsab(r + fx_case[f][k].wa, r + fx_case[f][k].wb, r + fx_case[f][k].al, r + fx_case[f][k].mk); break;
        case 1: dsa(r + fx_case[f][k].wa, r + fx_case[f][k].wb, r + fx_case[f][k].al, r + fx_case[f][k].mk); break;
        case 2: dsbb(fx_case[f][k].w, r + fx_case[f][k].wa, r + fx_case[f][k].wb, r + fx_case[f][k].mk); break;
        default: dsb(fx_case[f][k].w, r + fx_case[f][k].wa, r + fx_case[f][k].wb, r + fx_case[f][k].mk); break;
        }
    }
    /* the originals through the ORIG4 stubs, then the check wrappers */
    ds2d_orig_vars[0][0] = ds2d_orig_vars[1][0] = ut_base + DS_SHADE_BITS;
    ds2d_orig_vars[0][1] = ut_base + DS_SETUP_ALPHA_BASE + 16;
    ds2d_orig_vars[1][1] = ut_base + DS_SETUP_ALPHA + 16;
    orig_blend_base = dsbb; orig_blend_set = dsb;
    unsigned long bad0 = chk_bad[H_ALPHA_BASE] + chk_bad[H_ALPHA] + chk_bad[H_BLEND_BASE] + chk_bad[H_BLEND_SET];
    static uint8_t b0[0x800];
    for (int it = 0; it < 800; it++) {
        int f = it & 3, wa = rnd(0x100), wb = 0x200 + rnd(0x100), al = 0x400 + rnd(0x100), mk = 0x600 + rnd(0x100);
        rndfill(b0, sizeof b0); fx_mask(b0 + mk);
        uint32_t w = (uint32_t)rnd64();
        memcpy(b1, b0, sizeof b0); memcpy(b2, b0, sizeof b0);
        switch (f) {
        case 0: dsab(b1 + wa, b1 + wb, b1 + al, b1 + mk); ds2d_alpha_base_orig(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 1: dsa(b1 + wa, b1 + wb, b1 + al, b1 + mk); ds2d_alpha_orig(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 2: dsbb(w, b1 + wa, b1 + wb, b1 + mk); break;
        default: dsb(w, b1 + wa, b1 + wb, b1 + mk); break;
        }
        if (f < 2 && ut_cmp("setup_alpha originals through the stubs", b1, b2, sizeof b1)) return;
        memcpy(b2, b0, sizeof b0);
        switch (f) {
        case 0: chk_alpha_base(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 1: chk_alpha_set(b2 + wa, b2 + wb, b2 + al, b2 + mk); break;
        case 2: chk_blend_base(w, b2 + wa, b2 + wb, b2 + mk); break;
        default: chk_blend_set(w, b2 + wa, b2 + wb, b2 + mk); break;
        }
        if (ut_cmp("effects setup check wrappers", b1, b2, sizeof b1)) return;
    }
    if (chk_bad[H_ALPHA_BASE] + chk_bad[H_ALPHA] + chk_bad[H_BLEND_BASE] + chk_bad[H_BLEND_SET] != bad0) { fprintf(stderr, "FAIL effects setup check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "effects setups: %u %u %u %u random calls (aliasing layouts included), the stubs and 200 each through the check wrappers equal\n", n[0], n[1], n[2], n[3]);
}

static void test_shade(void) {
    shade_fn_t ds = DS(shade_fn_t, DS_SHADE);
    static uint8_t eng[0x100], in[0x300 + 2 * G], mask[32], o1[0x300 + 2 * G], o2[0x300 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 6000; it++) {
        rndfill(eng, sizeof eng);
        if (rnd(2)) { uint16_t v = rnd(2) ? rnd(32) : rnd(0x10000); memcpy(eng + 0xa2, &v, 2); }
        fill_planes(in, sizeof in); fill_mask(mask);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        if (rnd(4) == 0) {                                   /* in place */
            memcpy(o1 + G, in + G, 0x300); memcpy(o2 + G, in + G, 0x300);
            ds(eng, o1 + G, o1 + G, mask); ds2d_shade(eng, o2 + G, o2 + G, mask);
        } else { ds(eng, o1 + G, in + G, mask); ds2d_shade(eng, o2 + G, in + G, mask); }
        if (ut_cmp("shade", o1, o2, sizeof o1)) { fprintf(stderr, "  evy %#x bldcnt %#x\n", eng[0xa2] | eng[0xa3] << 8, eng[0xa0] | eng[0xa1] << 8); return; }
        if (it < NFX) { memcpy(sh_case[it].eng, eng, sizeof eng); memcpy(sh_case[it].in, in + G, 0x300); memcpy(sh_case[it].mask, mask, 32); memcpy(sh_case[it].out, o1, sizeof o1); }
        n++;
    }
    orig_shade = ds;
    unsigned long bad0 = chk_bad[H_SHADE];
    for (int it = 0; it < 200; it++) {
        rndfill(eng, sizeof eng); fill_planes(in, sizeof in); fill_mask(mask);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds(eng, o1 + G, in + G, mask); chk_shade(eng, o2 + G, in + G, mask);
        if (ut_cmp("shade check wrapper", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_SHADE] != bad0) { fprintf(stderr, "FAIL shade check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "shade: %u random quarters (in place too) + 200 through the check wrapper equal\n", n);
}

/* ---- the 8bpp text BG spans ---- */
typedef void (*span8n_fn_t)(uint8_t *, const uint8_t *, const uint16_t *, const uint8_t *);
typedef void (*span8e_fn_t)(uint16_t *, const uint8_t *, const uint16_t *, const uint8_t *);
static struct { uint16_t entries[36]; uint8_t flags[36]; uint8_t dst[528 + 2 * G]; } s8_case[2][NSP];

static void test_span8(void) {
    span8n_fn_t dsn = DS(span8n_fn_t, DS_SPAN8N);
    span8e_fn_t dse = DS(span8e_fn_t, DS_SPAN8E);
    static uint16_t entries[36], pal[256];
    static uint8_t flags[36], d1[528 + 2 * G], d2[528 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 6000; it++) {
        int ext = it & 1;
        if (it % 64 == 0) rndfill(sp_tiles, 65536 + 8);
        const uint8_t *tiles = it < 2 * NSP ? sp_tiles0 : sp_tiles;         /* the kept cases: the first fill */
        span4_inputs(pal, entries, flags);
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        if (ext) { dse((uint16_t *)(d1 + G), tiles, entries, flags); ds2d_span8e((uint16_t *)(d2 + G), tiles, entries, flags); }
        else { dsn(d1 + G, tiles, entries, flags); ds2d_span8n(d2 + G, tiles, entries, flags); }
        if (ut_cmp(ext ? "span8 ext palette" : "span8 normal palette", d1, d2, sizeof d1)) return;
        if (it / 2 < NSP) {
            memcpy(s8_case[ext][it / 2].entries, entries, sizeof entries); memcpy(s8_case[ext][it / 2].flags, flags, sizeof flags);
            memcpy(s8_case[ext][it / 2].dst, d1, sizeof d1);
        }
        n++;
    }
    ds2d_orig_vars[2][0] = ds2d_orig_vars[3][0] = ut_base + DS_SPAN8_CONSTS;
    ds2d_orig_vars[2][1] = ut_base + DS_SPAN8N + 16;
    ds2d_orig_vars[3][1] = ut_base + DS_SPAN8E + 16;
    unsigned long bad0 = chk_bad[H_SPAN8N] + chk_bad[H_SPAN8E];
    for (int it = 0; it < 400; it++) {
        int ext = it & 1;
        span4_inputs(pal, entries, flags);
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        if (ext) { dse((uint16_t *)(d1 + G), sp_tiles, entries, flags); ds2d_span8e_orig((uint16_t *)(d2 + G), sp_tiles, entries, flags); }
        else { dsn(d1 + G, sp_tiles, entries, flags); ds2d_span8n_orig(d2 + G, sp_tiles, entries, flags); }
        if (ut_cmp("span8 originals through the stubs", d1, d2, sizeof d1)) return;
        memcpy(d2, d1, sizeof d1); rndfill(d2 + G, ext ? 528 : 264);
        if (ext) chk_span8e((uint16_t *)(d2 + G), sp_tiles, entries, flags); else chk_span8n(d2 + G, sp_tiles, entries, flags);
        if (ut_cmp("span8 check wrappers", d1, d2, sizeof d1)) return;
    }
    if (chk_bad[H_SPAN8N] + chk_bad[H_SPAN8E] != bad0) { fprintf(stderr, "FAIL span8 check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "span8: %u random lines (normal / extended palettes), 200 each through the stubs and the check wrappers equal\n", n);
}

/* ---- the tile map entries ---- */
typedef void (*tme_fn_t)(uint16_t *, uint8_t *, const uint16_t *, const uint16_t *, uint32_t, uint32_t);
#define NTM 16
static struct { uint16_t row[32], row2[32]; uint32_t k, roff; uint8_t e[80 + 2 * G], f[48 + 2 * G]; } tm_case[2][NTM];

static void test_tme(void) {
    tme_fn_t ds4 = DS(tme_fn_t, DS_TME4), ds8 = DS(tme_fn_t, DS_TME8);
    static uint16_t row[32], row2[32];
    static uint8_t e1[80 + 2 * G], e2[80 + 2 * G], f1[48 + 2 * G], f2[48 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 8000; it++) {
        int b8 = it & 1;
        rndfill(row, sizeof row); rndfill(row2, sizeof row2);
        uint32_t k = rnd(32), roff = rnd(2) ? (b8 ? 8 * rnd(8) : 4 * rnd(8)) : (uint32_t)rnd64();
        rndfill(e1, sizeof e1); memcpy(e2, e1, sizeof e1); rndfill(f1, sizeof f1); memcpy(f2, f1, sizeof f1);
        if (b8) { ds8((uint16_t *)(e1 + G), f1 + G, row, row2, k, roff); ds2d_tme8((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff); }
        else { ds4((uint16_t *)(e1 + G), f1 + G, row, row2, k, roff); ds2d_tme4((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff); }
        /* flags 40..47 are the original's undefined bytes: compared up to 40, then the right guard */
        if (ut_cmp(b8 ? "tile_map_entries_8bpp entries" : "tile_map_entries_4bpp entries", e1, e2, sizeof e1) ||
            ut_cmp("tile_map_entries flags", f1, f2, G + 40) || ut_cmp("tile_map_entries flags guard", f1 + G + 48, f2 + G + 48, G)) {
            fprintf(stderr, "  k %u roff %#x\n", k, roff);
            return;
        }
        if (it / 2 < NTM) {
            memcpy(tm_case[b8][it / 2].row, row, sizeof row); memcpy(tm_case[b8][it / 2].row2, row2, sizeof row2);
            tm_case[b8][it / 2].k = k; tm_case[b8][it / 2].roff = roff;
            memcpy(tm_case[b8][it / 2].e, e1, sizeof e1); memcpy(tm_case[b8][it / 2].f, f1, sizeof f1);
        }
        n++;
    }
    ds2d_orig_vars[4][0] = ds2d_orig_vars[5][0] = ut_base + DS_TME_TABLE;
    ds2d_orig_vars[4][1] = ut_base + DS_TME4 + 16;
    ds2d_orig_vars[5][1] = ut_base + DS_TME8 + 16;
    unsigned long bad0 = chk_bad[H_TME4] + chk_bad[H_TME8];
    for (int it = 0; it < 400; it++) {
        int b8 = it & 1;
        rndfill(row, sizeof row); rndfill(row2, sizeof row2);
        uint32_t k = rnd(32), roff = 4 * rnd(16);
        rndfill(e1, sizeof e1); memcpy(e2, e1, sizeof e1); rndfill(f1, sizeof f1); memcpy(f2, f1, sizeof f1);
        if (b8) { ds8((uint16_t *)(e1 + G), f1 + G, row, row2, k, roff); ds2d_tme8_orig((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff); }
        else { ds4((uint16_t *)(e1 + G), f1 + G, row, row2, k, roff); ds2d_tme4_orig((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff); }
        if (ut_cmp("tile_map_entries originals through the stubs", e1, e2, sizeof e1) || ut_cmp("tile_map_entries stubs flags", f1, f2, G + 40)) return;
        memcpy(e2, e1, sizeof e1); rndfill(e2 + G, 80); memcpy(f2, f1, sizeof f1); rndfill(f2 + G, 40);
        if (b8) chk_tme8((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff); else chk_tme4((uint16_t *)(e2 + G), f2 + G, row, row2, k, roff);
        if (ut_cmp("tile_map_entries check wrappers", e1, e2, sizeof e1) || ut_cmp("tile_map_entries check wrappers flags", f1, f2, G + 40)) return;
    }
    if (chk_bad[H_TME4] + chk_bad[H_TME8] != bad0) { fprintf(stderr, "FAIL tile_map_entries check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "tile_map_entries: %u random calls (4bpp / 8bpp, every column), 200 each through the stubs and the check wrappers equal\n", n);
}

/* ---- disable_blank_layers ---- */
typedef void (*dbl_fn_t)(const uint8_t *, uint8_t *);
static struct { uint8_t m[128]; uint8_t w[4 + 2 * G]; uint8_t ref[4 + 2 * G]; } db_case[NTM];

static void test_dbl(void) {
    dbl_fn_t ds = DS(dbl_fn_t, DS_DISABLE_BLANK);
    static uint8_t m[128], w1[4 + 2 * G], w2[4 + 2 * G];
    unsigned n = 0;
    for (int it = 0; it < 20000; it++) {
        for (int k = 0; k < 4; k++) { fill_mask(m + 32 * k); if (rnd(3) == 0) memset(m + 32 * k, 0, 32); if (rnd(8) == 0) m[32 * k + rnd(32)] = 1 << rnd(8); }
        rndfill(w1, sizeof w1); memcpy(w2, w1, sizeof w1);
        if (it < NTM) { memcpy(db_case[it].m, m, sizeof m); memcpy(db_case[it].w, w1, sizeof w1); }
        ds(m, w1 + G); ds2d_disable_blank(m, w2 + G);
        if (ut_cmp("disable_blank_layers", w1, w2, sizeof w1)) return;
        if (it < NTM) memcpy(db_case[it].ref, w1, sizeof w1);
        n++;
    }
    orig_dbl = ds;
    unsigned long bad0 = chk_bad[H_DBL];
    for (int it = 0; it < 200; it++) {
        for (int k = 0; k < 4; k++) { fill_mask(m + 32 * k); if (rnd(3) == 0) memset(m + 32 * k, 0, 32); }
        rndfill(w1, sizeof w1); memcpy(w2, w1, sizeof w1);
        ds(m, w1 + G); chk_dbl(m, w2 + G);
        if (ut_cmp("disable_blank_layers check wrapper", w1, w2, sizeof w1)) return;
    }
    if (chk_bad[H_DBL] != bad0) { fprintf(stderr, "FAIL disable_blank_layers check wrapper counted differences\n"); ut_fail++; }
    fprintf(stderr, "disable_blank_layers: %u random calls + 200 through the check wrapper equal\n", n);
}

/* ---- the visibility bitmaps ---- */
typedef void (*vis4_fn_t)(uint8_t *, const uint8_t *, uint32_t);
typedef void (*vis8_fn_t)(uint8_t *, const uint8_t *);
typedef void (*vis12_fn_t)(uint8_t *, const uint16_t *);
static struct { uint8_t src[512]; uint32_t shift; uint8_t ref[32 + 2 * G]; } vi_case[3][NTM];

static void vis_src(uint8_t *p, size_t n) {
    switch (rnd(4)) {
    case 0: rndfill(p, n); break;
    case 1: for (size_t i = 0; i < n; i++) p[i] = rnd(4) ? 0 : (uint8_t)rnd64(); break;
    case 2: for (size_t i = 0; i < n; i++) p[i] = rnd(2) ? 0 : (rnd(2) ? 0xf0 : 0x0f); break;
    default: for (size_t i = 0; i < n; i++) p[i] = rnd(6) ? (uint8_t)rnd64() : 0; break;
    }
}
static void test_vis(void) {
    vis4_fn_t ds4 = DS(vis4_fn_t, DS_VIS4); vis8_fn_t ds8 = DS(vis8_fn_t, DS_VIS8); vis12_fn_t ds12 = DS(vis12_fn_t, DS_VIS12);
    static uint8_t src[512 + 2 * G], o1[32 + 2 * G], o2[32 + 2 * G];
    unsigned n[3] = { 0 };
    for (int it = 0; it < 12000; it++) {
        int f = it % 3;
        vis_src(src, sizeof src);
        uint32_t shift = rnd(4) ? rnd(8) : (rnd(2) ? rnd(300) : (uint32_t)rnd64());
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        switch (f) {
        case 0: ds4(o1 + G, src + G, shift); ds2d_vis4(o2 + G, src + G, shift); break;
        case 1: ds8(o1 + G, src + G); ds2d_vis8(o2 + G, src + G); break;
        default: ds12(o1 + G, (const uint16_t *)(src + G)); ds2d_vis12(o2 + G, (const uint16_t *)(src + G)); break;
        }
        if (ut_cmp(f == 0 ? "set_visibility_4bpp" : f == 1 ? "set_visibility_8bpp" : "set_visibility_12bpp", o1, o2, sizeof o1)) { fprintf(stderr, "  shift %#x\n", shift); return; }
        if (it / 3 < NTM) { memcpy(vi_case[f][it / 3].src, src + G, 512); vi_case[f][it / 3].shift = shift; memcpy(vi_case[f][it / 3].ref, o1, sizeof o1); }
        n[f]++;
    }
    ds2d_orig_vars[6][0] = ut_base + DS_VIS4_CONSTS;
    ds2d_orig_vars[6][1] = ut_base + DS_VIS4 + 16;
    ds2d_orig_vars[7][0] = ds2d_orig_vars[8][0] = ut_base + DS_VIS_CONSTS;
    ds2d_orig_vars[7][1] = ut_base + DS_VIS8 + 16;
    ds2d_orig_vars[8][1] = ut_base + DS_VIS12 + 16;
    unsigned long bad0 = chk_bad[H_VIS4] + chk_bad[H_VIS8] + chk_bad[H_VIS12];
    for (int it = 0; it < 600; it++) {
        int f = it % 3;
        vis_src(src, sizeof src);
        uint32_t shift = rnd(8);
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        switch (f) {
        case 0: ds4(o1 + G, src + G, shift); ds2d_vis4_orig(o2 + G, src + G, shift); break;
        case 1: ds8(o1 + G, src + G); ds2d_vis8_orig(o2 + G, src + G); break;
        default: ds12(o1 + G, (const uint16_t *)(src + G)); ds2d_vis12_orig(o2 + G, (const uint16_t *)(src + G)); break;
        }
        if (ut_cmp("set_visibility originals through the stubs", o1, o2, sizeof o1)) return;
        memcpy(o2, o1, sizeof o1); rndfill(o2 + G, 32);
        switch (f) {
        case 0: chk_vis4(o2 + G, src + G, shift); break;
        case 1: chk_vis8(o2 + G, src + G); break;
        default: chk_vis12(o2 + G, (const uint16_t *)(src + G)); break;
        }
        if (ut_cmp("set_visibility check wrappers", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_VIS4] + chk_bad[H_VIS8] + chk_bad[H_VIS12] != bad0) { fprintf(stderr, "FAIL set_visibility check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "set_visibility: %u 4bpp (any shift) / %u 8bpp / %u 12bpp random lines, 200 each through the stubs and the check wrappers equal\n", n[0], n[1], n[2]);
}

/* ---- the blended captures ---- */
typedef void (*capb_fn_t)(uint8_t *, uint16_t *, const uint16_t *, const void *);
#define NCB 16
static struct { uint8_t cap[0x80]; uint8_t a[0x800]; uint16_t b[512]; int hasb, w; uint8_t ref[1024 + 2 * G]; } cb_case[2][NCB];

static void capb_inputs(uint8_t *cap, uint8_t *a, uint16_t *b, int is3d, int *w) {
    rndfill(cap, 0x80);
    switch (rnd(4)) {                                  /* EVA / EVB: DraStic's (0..16), any byte, or the direct case */
    case 0: cap[0x54] = rnd(17); cap[0x55] = rnd(17); break;
    case 1: break;
    case 2: cap[0x54] = 16; cap[0x55] = 0; break;
    default: cap[0x54] = rnd(2) ? 0 : 16; cap[0x55] = rnd(2) ? 16 : rnd(17); break;
    }
    int ww = rnd(3) ? 128 * (1 + rnd(2)) : rnd(4) ? rnd(300) : 0;
    if (cap[0x54] == 16 && cap[0x55] == 0) ww = 32 * (1 + rnd(8));            /* the direct asm's steps of 32 */
    *w = ww;
    uint16_t w16 = (uint16_t)ww; memcpy(cap + 0x4c, &w16, 2);
    rndfill(a, 0x800);
    if (rnd(2)) for (int i = 0; i < 0x800; i++) a[i] &= is3d ? ((i & 3) == 3 ? 0xff : 0x3f) : 0x3f;   /* 6-bit channels */
    rndfill(b, 1024);
}
static void test_capb(void) {
    capb_fn_t ds[2] = { DS(capb_fn_t, DS_CAPB), DS(capb_fn_t, DS_CAPB3D) };
    static uint8_t cap[0x80], a[0x800], o1[1024 + 2 * G], o2[1024 + 2 * G];
    static uint16_t b[512];
    unsigned n[2] = { 0 }, ninplace = 0;
    for (int it = 0; it < 8000; it++) {
        int is3d = it & 1, w;
        capb_inputs(cap, a, b, is3d, &w);
        int hasb = rnd(4) != 0;
        uint64_t hb = hasb ? 1 : 0; memcpy(cap + 0x40, &hb, 8);              /* the 2D one's source B flag */
        const uint16_t *pb = hasb || !is3d ? b : 0;
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        int inplace = hasb && rnd(5) == 0;
        if (inplace) {                                   /* out == srcb: VRAM captured onto itself */
            memcpy(o1 + G, b, 1024); memcpy(o2 + G, b, 1024);
            ds[is3d](cap, (uint16_t *)(o1 + G), (const uint16_t *)(o1 + G), a);
            (is3d ? (capb_fn_t)ds2d_capb3d : (capb_fn_t)ds2d_capb)(cap, (uint16_t *)(o2 + G), (const uint16_t *)(o2 + G), a);
            ninplace++;
        } else {
            ds[is3d](cap, (uint16_t *)(o1 + G), pb, a);
            (is3d ? (capb_fn_t)ds2d_capb3d : (capb_fn_t)ds2d_capb)(cap, (uint16_t *)(o2 + G), pb, a);
        }
        if (ut_cmp(is3d ? "capture_blended_3d" : "capture_blended", o1, o2, sizeof o1)) {
            fprintf(stderr, "  eva %u evb %u w %d srcb %d in place %d\n", cap[0x54], cap[0x55], w, hasb, inplace);
            return;
        }
        if (!inplace && it / 2 < NCB) {
            int k = it / 2;
            memcpy(cb_case[is3d][k].cap, cap, sizeof cap); memcpy(cb_case[is3d][k].a, a, sizeof a); memcpy(cb_case[is3d][k].b, b, sizeof b);
            cb_case[is3d][k].hasb = hasb; cb_case[is3d][k].w = w; memcpy(cb_case[is3d][k].ref, o1, sizeof o1);
        }
        n[is3d]++;
    }
    /* every width 1..256 (the w % 16 tails) with EVA / EVB at their extremes, source B or none, both sources */
    static const uint8_t ev[8][2] = { { 0, 0 }, { 16, 0 }, { 16, 16 }, { 31, 31 }, { 32, 32 }, { 255, 255 }, { 31, 0 }, { 0, 31 } };
    unsigned nsweep = 0;
    for (int w = 1; w <= 256; w++) for (int e = 0; e < 8; e++) for (int hasb = 0; hasb < 2; hasb++) for (int is3d = 0; is3d < 2; is3d++) {
        int ww;
        capb_inputs(cap, a, b, is3d, &ww);
        cap[0x54] = ev[e][0]; cap[0x55] = ev[e][1];
        if (cap[0x54] == 16 && cap[0x55] == 0 && w % 32) continue;      /* the direct asm: steps of 32 pixels */
        uint16_t w16 = (uint16_t)w; memcpy(cap + 0x4c, &w16, 2);
        uint64_t hb = hasb; memcpy(cap + 0x40, &hb, 8);
        const uint16_t *pb = hasb || !is3d ? b : 0;
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds[is3d](cap, (uint16_t *)(o1 + G), pb, a);
        (is3d ? (capb_fn_t)ds2d_capb3d : (capb_fn_t)ds2d_capb)(cap, (uint16_t *)(o2 + G), pb, a);
        if (ut_cmp("capture_blended sweep", o1, o2, sizeof o1)) {
            fprintf(stderr, "  eva %u evb %u w %d srcb %d 3d %d\n", cap[0x54], cap[0x55], w, hasb, is3d);
            return;
        }
        nsweep++;
    }
    /* out over a source: the original's order (the replacement hands these to it) */
    orig_capb = ds[0]; orig_capb3d = ds[1];
    for (int it = 0; it < 400; it++) {
        int is3d = it & 1, w;
        static uint8_t buf1[0x1000], buf2[0x1000];
        capb_inputs(cap, a, b, is3d, &w);
        if (cap[0x54] == 16 && cap[0x55] == 0) cap[0x55] = 1;
        uint64_t hb = 1; memcpy(cap + 0x40, &hb, 8);
        rndfill(buf1, sizeof buf1); memcpy(buf2, buf1, sizeof buf1);
        int ao = rnd(0x400), bo = 0x400 + rnd(0x200), oo = rnd(2) ? ao + 2 * rnd(64) : bo + 2 * (rnd(32) + 1);
        if (oo + 2 * w > 0x1000 || ao + (is3d ? 4 : 1) * w + (is3d ? 0 : 0x200) > 0x1000 || bo + 2 * w > 0x1000) continue;
        ds[is3d](cap, (uint16_t *)(buf1 + oo), (const uint16_t *)(buf1 + bo), buf1 + ao);
        (is3d ? (capb_fn_t)ds2d_capb3d : (capb_fn_t)ds2d_capb)(cap, (uint16_t *)(buf2 + oo), (const uint16_t *)(buf2 + bo), buf2 + ao);
        if (ut_cmp("capture_blended overlapping", buf1, buf2, sizeof buf1)) return;
    }
    unsigned long bad0 = chk_bad[H_CAPB] + chk_bad[H_CAPB3D];
    for (int it = 0; it < 400; it++) {
        int is3d = it & 1, w;
        capb_inputs(cap, a, b, is3d, &w);
        uint64_t hb = rnd(2); memcpy(cap + 0x40, &hb, 8);
        const uint16_t *pb = hb || !is3d ? b : 0;
        rndfill(o1, sizeof o1); memcpy(o2, o1, sizeof o1);
        ds[is3d](cap, (uint16_t *)(o1 + G), pb, a);
        if (is3d) chk_capb3d(cap, (uint16_t *)(o2 + G), pb, (const uint32_t *)a); else chk_capb(cap, (uint16_t *)(o2 + G), pb, a);
        if (ut_cmp("capture_blended check wrappers", o1, o2, sizeof o1)) return;
    }
    if (chk_bad[H_CAPB] + chk_bad[H_CAPB3D] != bad0) { fprintf(stderr, "FAIL capture_blended check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "capture_blended: %u 2D + %u 3D random lines (any EVA / EVB, widths 0..299, %u in place, the direct case), "
            "%u in the sweep of widths 1..256 x EVA / EVB extremes, 400 overlapping, 200 each through the check wrappers "
            "equal\n", n[0], n[1], ninplace, nsweep);
}

/* ---- the affine BG tile renderers ---- */
typedef void (*affnn_fn_t)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, int, const uint8_t *);
typedef void (*affe_fn_t)(void *, const uint8_t *, const uint8_t *, const uint16_t *, const uint8_t *, int, const uint8_t *);
#define NAF 12
static struct { uint8_t offs[1200], widths[80], flips[80]; uint16_t entries[80]; int n, used; uint8_t ref[1600 + 2 * G]; } af_case[3][NAF];

/* runs as DraStic's setup makes them (1-8 pixels, summing to <= 256) or any (0..40, wide ones); n runs */
static int aff_inputs(uint8_t *offs, uint8_t *widths, uint16_t *entries, uint8_t *flips) {
    rndfill(offs, 1200); rndfill(entries, 160); rndfill(flips, 80);
    if (rnd(2)) for (int i = 0; i < 1200; i++) offs[i] &= 0x3f;
    if (rnd(2)) for (int i = 0; i < 80; i++) flips[i] = rnd(4) == 0 ? 0 : rnd(2) ? 0x07 : rnd(2) ? 0x38 : 0x3f;
    int n, sum = 0;
    switch (rnd(4)) {
    case 0: n = 0; for (; n < 70 && sum < 256; n++) { widths[n] = 1 + rnd(8); sum += widths[n]; } break;
    case 1: n = 1 + rnd(40); for (int k = 0; k < n; k++) widths[k] = rnd(41); break;
    case 2: n = rnd(4); for (int k = 0; k < 4; k++) widths[k] = rnd(3) ? 1 + rnd(30) : 0; break;  /* n 0..3, zero widths */
    default: n = 1 + rnd(30); for (int k = 0; k < n; k++) widths[k] = rnd(4) ? 1 + rnd(15) : 16 + rnd(25); break;
    }
    for (int k = n; k < 80; k++) widths[k] = rnd(20);
    if (rnd(8) == 0) widths[0] = 0;                                      /* the whole line from one tile */
    else if (widths[0] == 0) widths[0] = 1;
    return n;
}
static void test_aff(void) {
    affnn_fn_t dnn = DS(affnn_fn_t, DS_AFF_NN);
    affe_fn_t den = DS(affe_fn_t, DS_AFF_EN), dee = DS(affe_fn_t, DS_AFF_EE);
    static uint8_t offs[1200], widths[80], flips[80], d1[1600 + 2 * G], d2[1600 + 2 * G];
    static uint16_t entries[80];
    unsigned nf[3] = { 0 };
    for (int it = 0; it < 9000; it++) {
        int f = it % 3;
        if (it % 300 == 0) rndfill(sp_tiles, 65536 + 8);
        const uint8_t *tiles = it < 3 * NAF ? sp_tiles0 : sp_tiles;
        int n = aff_inputs(offs, widths, entries, flips);
        if (aff_extent(widths, n) * (f == 2 ? 2 : 1) > 1600) continue;
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        if (f == 0) { dnn(d1 + G, offs, widths, (const uint8_t *)entries, n, tiles); ds2d_aff_nn(d2 + G, offs, widths, (const uint8_t *)entries, n, tiles); }
        else if (f == 1) { den(d1 + G, offs, widths, entries, flips, n, tiles); ds2d_aff_en(d2 + G, offs, widths, entries, flips, n, tiles); }
        else { dee(d1 + G, offs, widths, entries, flips, n, tiles); ds2d_aff_ee((uint16_t *)(d2 + G), offs, widths, entries, flips, n, tiles); }
        if (ut_cmp(f == 0 ? "affine_render_tiles_normal_normal" : f == 1 ? "affine_render_tiles_extended_normal" : "affine_render_tiles_extended_extended",
                   d1, d2, sizeof d1)) { fprintf(stderr, "  n %d widths %u %u %u\n", n, widths[0], widths[1], widths[2]); return; }
        if (it / 3 < NAF) {
            int k = it / 3;
            memcpy(af_case[f][k].offs, offs, sizeof offs); memcpy(af_case[f][k].widths, widths, sizeof widths);
            memcpy(af_case[f][k].flips, flips, sizeof flips); memcpy(af_case[f][k].entries, entries, sizeof entries);
            af_case[f][k].n = n; af_case[f][k].used = 1; memcpy(af_case[f][k].ref, d1, sizeof d1);
        }
        nf[f]++;
    }
    ds2d_orig_vars[9][0] = ut_base + DS_AFF_NN + 0xdc; ds2d_orig_vars[9][1] = ut_base + DS_AFF_NN + 16;
    ds2d_orig_vars[10][0] = ut_base + DS_AFF_EN + 0xfc; ds2d_orig_vars[10][1] = ut_base + DS_AFF_EN + 16;
    ds2d_orig_vars[11][0] = ut_base + DS_AFF_EE + 0x11c; ds2d_orig_vars[11][1] = ut_base + DS_AFF_EE + 16;
    unsigned long bad0 = chk_bad[H_AFF_NN] + chk_bad[H_AFF_EN] + chk_bad[H_AFF_EE];
    for (int it = 0; it < 600; it++) {
        int f = it % 3;
        int n = aff_inputs(offs, widths, entries, flips);
        if (aff_extent(widths, n) * (f == 2 ? 2 : 1) > 1600) continue;
        rndfill(d1, sizeof d1); memcpy(d2, d1, sizeof d1);
        if (f == 0) { dnn(d1 + G, offs, widths, (const uint8_t *)entries, n, sp_tiles); ds2d_aff_nn_orig(d2 + G, offs, widths, (const uint8_t *)entries, n, sp_tiles); }
        else if (f == 1) { den(d1 + G, offs, widths, entries, flips, n, sp_tiles); ds2d_aff_en_orig(d2 + G, offs, widths, entries, flips, n, sp_tiles); }
        else { dee(d1 + G, offs, widths, entries, flips, n, sp_tiles); ds2d_aff_ee_orig((uint16_t *)(d2 + G), offs, widths, entries, flips, n, sp_tiles); }
        if (ut_cmp("affine_render_tiles originals through the stubs", d1, d2, sizeof d1)) return;
        memcpy(d2, d1, sizeof d1); rndfill(d2 + G, 1600);
        memcpy(d2 + G + (f == 2 ? 2 : 1) * aff_extent(widths, n), d1 + G + (f == 2 ? 2 : 1) * aff_extent(widths, n), 1600 - (f == 2 ? 2 : 1) * aff_extent(widths, n));
        if (f == 0) chk_aff_nn(d2 + G, offs, widths, (const uint8_t *)entries, n, sp_tiles);
        else if (f == 1) chk_aff_en(d2 + G, offs, widths, entries, flips, n, sp_tiles);
        else chk_aff_ee((uint16_t *)(d2 + G), offs, widths, entries, flips, n, sp_tiles);
        if (ut_cmp("affine_render_tiles check wrappers", d1, d2, sizeof d1)) return;
    }
    if (chk_bad[H_AFF_NN] + chk_bad[H_AFF_EN] + chk_bad[H_AFF_EE] != bad0) { fprintf(stderr, "FAIL affine_render_tiles check wrappers counted differences\n"); ut_fail++; }
    fprintf(stderr, "affine_render_tiles: %u / %u / %u random lines (runs as DraStic's and any widths, n 0..70, the one-tile line), "
            "200 each through the stubs and the check wrappers equal\n", nf[0], nf[1], nf[2]);
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
    /* the ten routines of the second batch through their patched entries */
    {
        pal8_fn_t p8 = DS(pal8_fn_t, DS_PAL8); pal12_fn_t p12 = DS(pal12_fn_t, DS_PAL12);
        static uint8_t b[0x800 + 2 * G];
        for (int ext = 0; ext < 2; ext++) for (int i = 0; i < NPAL; i++) {
            memcpy(b, pal_case[ext][i].buf, sizeof b);
            if (ext) p12((uint16_t *)(b + G + pal_case[1][i].dst), (const uint16_t *)(b + G + pal_case[1][i].src), pal_case[1][i].pal, pal_case[1][i].n);
            else p8((uint16_t *)(b + G + pal_case[0][i].dst), b + G + pal_case[0][i].src, pal_case[0][i].pal, pal_case[0][i].n);
            if (ut_cmp("palette_lookup patched entry", pal_case[ext][i].ref, b, sizeof b)) return;
        }
        prio2_fn_t pp = DS(prio2_fn_t, DS_PRIO_DOUBLE);
        for (int i = 0; i < NPR; i++) {
            static uint8_t S[0x900];
            memcpy(S, p2_case[i].S, sizeof S);
            pp(p2_case[i].eng, S + 0x40, S + 0x380, S + 0x600);
            if (ut_cmp("prio_double patched entry", p2_case[i].ref, S, sizeof S)) return;
            memcpy(S, p2_case[i].S, sizeof S);
            orig_prio2(p2_case[i].eng, S + 0x40, S + 0x380, S + 0x600);
            if (ut_cmp("prio_double trampoline", p2_case[i].ref, S, sizeof S)) return;
        }
        alpha_fn_t fab = DS(alpha_fn_t, DS_SETUP_ALPHA_BASE), fa = DS(alpha_fn_t, DS_SETUP_ALPHA);
        blendw_fn_t fbb = DS(blendw_fn_t, DS_SETUP_BLEND_BASE), fb = DS(blendw_fn_t, DS_SETUP_BLEND);
        for (int f = 0; f < 4; f++) for (int k = 0; k < NFX; k++) {
            static uint8_t x[0x800];
            memcpy(x, fx_case[f][k].buf, sizeof x);
            int wa = fx_case[f][k].wa, wb = fx_case[f][k].wb, al = fx_case[f][k].al, mk = fx_case[f][k].mk;
            switch (f) {
            case 0: fab(x + wa, x + wb, x + al, x + mk); break;
            case 1: fa(x + wa, x + wb, x + al, x + mk); break;
            case 2: fbb(fx_case[f][k].w, x + wa, x + wb, x + mk); break;
            default: fb(fx_case[f][k].w, x + wa, x + wb, x + mk); break;
            }
            if (ut_cmp("effects setup patched entry", fx_case[f][k].ref, x, sizeof x)) return;
            memcpy(x, fx_case[f][k].buf, sizeof x);
            switch (f) {
            case 0: ds2d_alpha_base_orig(x + wa, x + wb, x + al, x + mk); break;
            case 1: ds2d_alpha_orig(x + wa, x + wb, x + al, x + mk); break;
            case 2: orig_blend_base(fx_case[f][k].w, x + wa, x + wb, x + mk); break;
            default: orig_blend_set(fx_case[f][k].w, x + wa, x + wb, x + mk); break;
            }
            if (ut_cmp("effects setup original after the patch", fx_case[f][k].ref, x, sizeof x)) return;
        }
        shade_fn_t fs = DS(shade_fn_t, DS_SHADE);
        for (int k = 0; k < NFX; k++) {
            static uint8_t o[0x300 + 2 * G];
            memcpy(o, sh_case[k].out, sizeof o); memset(o + G, 0x5a, 0x300);
            fs(sh_case[k].eng, o + G, sh_case[k].in, sh_case[k].mask);
            if (ut_cmp("shade patched entry", sh_case[k].out, o, sizeof o)) return;
            memset(o + G, 0xa5, 0x300);
            orig_shade(sh_case[k].eng, o + G, sh_case[k].in, sh_case[k].mask);
            if (ut_cmp("shade trampoline", sh_case[k].out, o, sizeof o)) return;
        }
        span8n_fn_t s8n = DS(span8n_fn_t, DS_SPAN8N); span8e_fn_t s8e = DS(span8e_fn_t, DS_SPAN8E);
        for (int ext = 0; ext < 2; ext++) for (int i = 0; i < NSP; i++) {
            static uint8_t d[528 + 2 * G];
            memcpy(d, s8_case[ext][i].dst, sizeof d); memset(d + G, 0x5a, ext ? 528 : 264);
            if (ext) s8e((uint16_t *)(d + G), sp_tiles0, s8_case[1][i].entries, s8_case[1][i].flags);
            else s8n(d + G, sp_tiles0, s8_case[0][i].entries, s8_case[0][i].flags);
            if (ut_cmp("span8 patched entry", s8_case[ext][i].dst, d, sizeof d)) return;
            memset(d + G, 0xa5, ext ? 528 : 264);
            if (ext) ds2d_span8e_orig((uint16_t *)(d + G), sp_tiles0, s8_case[1][i].entries, s8_case[1][i].flags);
            else ds2d_span8n_orig(d + G, sp_tiles0, s8_case[0][i].entries, s8_case[0][i].flags);
            if (ut_cmp("span8 original after the patch", s8_case[ext][i].dst, d, sizeof d)) return;
        }
    }
    /* the third batch through the patched entries */
    {
        tme_fn_t t4 = DS(tme_fn_t, DS_TME4), t8 = DS(tme_fn_t, DS_TME8);
        static uint8_t e[80 + 2 * G], f[48 + 2 * G];
        for (int b8 = 0; b8 < 2; b8++) for (int i = 0; i < NTM; i++) {
            memcpy(e, tm_case[b8][i].e, sizeof e); memset(e + G, 0x5a, 80); memcpy(f, tm_case[b8][i].f, sizeof f); memset(f + G, 0x5a, 40);
            (b8 ? t8 : t4)((uint16_t *)(e + G), f + G, tm_case[b8][i].row, tm_case[b8][i].row2, tm_case[b8][i].k, tm_case[b8][i].roff);
            if (ut_cmp("tile_map_entries patched entry", tm_case[b8][i].e, e, sizeof e) || ut_cmp("tile_map_entries patched entry flags", tm_case[b8][i].f, f, G + 40)) return;
            memset(e + G, 0xa5, 80); memset(f + G, 0xa5, 40);
            (b8 ? ds2d_tme8_orig : ds2d_tme4_orig)((uint16_t *)(e + G), f + G, tm_case[b8][i].row, tm_case[b8][i].row2, tm_case[b8][i].k, tm_case[b8][i].roff);
            if (ut_cmp("tile_map_entries original after the patch", tm_case[b8][i].e, e, sizeof e) || ut_cmp("tile_map_entries original after the patch flags", tm_case[b8][i].f, f, G + 40)) return;
        }
        dbl_fn_t db = DS(dbl_fn_t, DS_DISABLE_BLANK);
        for (int i = 0; i < NTM; i++) {
            static uint8_t w[4 + 2 * G];
            memcpy(w, db_case[i].w, sizeof w);
            db(db_case[i].m, w + G);
            if (ut_cmp("disable_blank_layers patched entry", db_case[i].ref, w, sizeof w)) return;
            memcpy(w, db_case[i].w, sizeof w);
            orig_dbl(db_case[i].m, w + G);
            if (ut_cmp("disable_blank_layers trampoline", db_case[i].ref, w, sizeof w)) return;
        }
        vis4_fn_t v4 = DS(vis4_fn_t, DS_VIS4); vis8_fn_t v8 = DS(vis8_fn_t, DS_VIS8); vis12_fn_t v12 = DS(vis12_fn_t, DS_VIS12);
        for (int f3 = 0; f3 < 3; f3++) for (int i = 0; i < NTM; i++) {
            static uint8_t o[32 + 2 * G];
            for (int pass = 0; pass < 2; pass++) {
                memcpy(o, vi_case[f3][i].ref, sizeof o); memset(o + G, pass ? 0xa5 : 0x5a, 32);
                switch (f3) {
                case 0: (pass ? ds2d_vis4_orig : v4)(o + G, vi_case[0][i].src, vi_case[0][i].shift); break;
                case 1: (pass ? ds2d_vis8_orig : v8)(o + G, vi_case[1][i].src); break;
                default: (pass ? ds2d_vis12_orig : v12)(o + G, (const uint16_t *)vi_case[2][i].src); break;
                }
                if (ut_cmp(pass ? "set_visibility original after the patch" : "set_visibility patched entry", vi_case[f3][i].ref, o, sizeof o)) return;
            }
        }
    }
    {
        capb_fn_t e[2] = { DS(capb_fn_t, DS_CAPB), DS(capb_fn_t, DS_CAPB3D) };
        for (int is3d = 0; is3d < 2; is3d++) for (int k = 0; k < NCB; k++) {
            static uint8_t o[1024 + 2 * G];
            const uint16_t *pb = cb_case[is3d][k].hasb || !is3d ? cb_case[is3d][k].b : 0;
            for (int pass = 0; pass < 2; pass++) {
                memcpy(o, cb_case[is3d][k].ref, sizeof o);
                memset(o + G, pass ? 0xa5 : 0x5a, 2 * cb_case[is3d][k].w);
                (pass ? (is3d ? orig_capb3d : orig_capb) : e[is3d])(cb_case[is3d][k].cap, (uint16_t *)(o + G), pb, cb_case[is3d][k].a);
                if (ut_cmp(pass ? "capture_blended trampoline" : "capture_blended patched entry", cb_case[is3d][k].ref, o, sizeof o)) return;
            }
        }
    }
    {
        affnn_fn_t enn = DS(affnn_fn_t, DS_AFF_NN);
        affe_fn_t een = DS(affe_fn_t, DS_AFF_EN), eee = DS(affe_fn_t, DS_AFF_EE);
        for (int f = 0; f < 3; f++) for (int k = 0; k < NAF; k++) {
            static uint8_t d[1600 + 2 * G];
            if (!af_case[f][k].used) continue;              /* a case skipped for its extent */
            memcpy(d, af_case[f][k].ref, sizeof d);
            size_t ext = (f == 2 ? 2 : 1) * aff_extent(af_case[f][k].widths, af_case[f][k].n);
            memset(d + G, 0x5a, ext);
            if (f == 0) enn(d + G, af_case[f][k].offs, af_case[f][k].widths, (const uint8_t *)af_case[f][k].entries, af_case[f][k].n, sp_tiles0);
            else (f == 1 ? een : eee)(d + G, af_case[f][k].offs, af_case[f][k].widths, af_case[f][k].entries, af_case[f][k].flips, af_case[f][k].n, sp_tiles0);
            if (ut_cmp("affine_render_tiles patched entry", af_case[f][k].ref, d, sizeof d)) return;
        }
    }
    fprintf(stderr, "patched entries: %d calls each equal (second batch: %d / %d / %d; third: %d; captures %d)\n", NCONV, NPAL, NPR, NFX, NTM, NCB);
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
    test_pal();
    test_prio2();
    test_effects_setup();
    test_shade();
    test_span8();
    test_tme();
    test_dbl();
    test_vis();
    test_capb();
    test_aff();
    test_patched();
}
