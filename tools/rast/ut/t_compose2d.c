/* t_compose2d.c: DraStic's 2D compositor vs src/rast/spec/2d/compose.c and the closed form of spec/2d/pixel.h, in
 * DraStic's process. DraStic's routine runs on copy A, the port on an identical copy B: every output buffer sits
 * between guard bytes and is compared with them; the composite's whole scratch area (render_scanline_2d's frame) and
 * the alpha plane after every call; the engine struct (two full 0x81420-byte structs, the header re-randomised per
 * case) after every call that gets one (the first 0x400 bytes: the header and the layer structs) and whole every 1024
 * cases and after every group. Groups (the analysis' t_compose.c generators, re2d/compose.md): the window masks
 * (update_window_mask, the three inhibit helpers, generate_window_masks with its Y state machine and dirty bits,
 * apply_windows); the compositor's pieces (the double priority encoder, select_blend_enable, shade, the four
 * coefficient setups, apply, apply_offset, the 3D horizontal shift); the composite on every path (random flags, lists,
 * masks, 3D spans, alpha planes, the shifted 3D copy in S); capture; the 32-bit conversions. Then the closed form:
 * inputs shaped as render_scanline_2d makes them (the analysis' pix.c: DraStic-shaped priority lists, lmask, bldcnt'
 * and flags derived as in compose.md 3.2 with DraStic's own set_3d_visibility, OBJ attribute plane and semi / bitmap
 * masks as render_scanline_obj_c leaves them, 3D spans of alpha 0 / 31 / 1..30, effects windows), DraStic's composite,
 * the port, and spec_composite_pixel for every pixel from the inputs before the call.
 * The simple path's helpers one by one (priority_encode_single, the select_pixels merges, expand, binary32,
 * select_pixels) are in t_composite.c; disable_blank_layers (bg.c) in t_bg2d.c.
 * run.sh t_compose2d.c ../../../src/rast/spec/2d/compose.c
 * Env: UT_SEED; UT_N cases per group (default: 20000, the window states 30000, the composite 40000, the closed form
 * 20000). */
#include "ut.h"
#include "spec/2d/compose.h"
#include "spec/2d/pixel.h"

#define N_ITER(n) (getenv("UT_N") ? atoi(getenv("UT_N")) : (n))
#define ENG_SIZE 0x81420
#define HDR      0x400
#define G        64

typedef void (*winmask_fn)(uint8_t *, uint32_t);
typedef void (*inh1_fn)(uint8_t *, uint8_t *, uint32_t, const uint8_t *, uint32_t, uint32_t);
typedef void (*inh2_fn)(uint8_t *, uint8_t *, uint32_t, const uint8_t *, const uint8_t *, uint32_t, uint32_t, uint32_t);
typedef void (*inh3_fn)(uint8_t *, uint8_t *, uint32_t, const uint8_t *, const uint8_t *, const uint8_t *, uint32_t, uint32_t,
                        uint64_t, uint64_t);
typedef void (*genwin_fn)(uint8_t *, uint8_t *, uint8_t *, const uint8_t *, uint32_t, uint32_t);
typedef void (*applywin_fn)(const uint8_t *, uint8_t *, const uint8_t *, uint32_t);
typedef void (*prio2_fn)(const uint8_t *, const uint8_t *, uint8_t *, uint8_t *);
typedef void (*sbe_fn)(uint8_t *, const uint8_t *, uint32_t, uint32_t);
typedef void (*shade_fn)(const uint8_t *, uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*sbb_fn)(uint32_t, uint8_t *, uint8_t *, const uint8_t *);
typedef void (*sab_fn)(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*apply_fn)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*applyo_fn)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*comp_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t **, const uint32_t *, uint8_t *, uint64_t, uint64_t,
                        uint64_t, uint64_t);
typedef void (*hshift_fn)(uint32_t *, const uint32_t *, int32_t);
typedef void (*capd_fn)(const uint8_t *, uint16_t *, const void *);
typedef void (*capb_fn)(const uint8_t *, uint16_t *, const uint16_t *, const void *);
typedef void (*cv1_fn)(const uint8_t *, uint32_t *);
typedef void (*cv2_fn)(const uint8_t *, const uint8_t *, uint32_t *);
typedef void (*cs1_fn)(const uint8_t *, uint32_t *, uint32_t, uint32_t);
typedef void (*cs2_fn)(const uint8_t *, const uint8_t *, uint32_t *, uint32_t, uint32_t);
typedef uint32_t (*vis3d_fn)(uint8_t *, const uint32_t *);

#define DS_UPDATE_WINDOW_MASK   0x3a1c0
#define DS_INHIBIT_SINGLE       0x3b060
#define DS_INHIBIT_DOUBLE       0x3ab10
#define DS_INHIBIT_TRIPLE       0x3a380
#define DS_GENERATE_WINDOWS     0x3b360
#define DS_APPLY_WINDOWS        0x3b650
#define DS_PRIORITY_DOUBLE      0x9fe98
#define DS_BLEND_ENABLE         0xa0070
#define DS_SHADE                0xa0108
#define DS_SETUP_BLEND_BASE     0xa0254
#define DS_SETUP_BLEND          0xa02e0
#define DS_SETUP_ALPHA_BASE     0xa042c
#define DS_SETUP_ALPHA          0xa04b4
#define DS_APPLY                0xa0378
#define DS_APPLY_OFFSET         0x39f00
#define DS_COMPOSITE            0x3c6d0
#define DS_HSHIFT               0x3c630
#define DS_CAPTURE_DIRECT       0xa0910
#define DS_CAPTURE_DIRECT_3D    0xa09b0
#define DS_CAPTURE_BLENDED      0x3bc60
#define DS_CAPTURE_BLENDED_3D   0x3bdb0
#define DS_CONVERT_DIRECT_1X    0xa0a80
#define DS_CONVERT_DIRECT_2X    0xa0ae0
#define DS_CONVERT_SHADE_1X     0xa0c90
#define DS_CONVERT_SHADE_2X     0xa0d80
#define DS_SET_3D_VISIBILITY    0x3c2c0

static uint8_t *engA, *engB;
static unsigned n_cases;

/* the engines: the header of A random (the callers then set fields), B a copy; the rest of both stays as it is */
static void eng_fill(void) { rndfill(engA, HDR); }
static void eng_copy(void) { memcpy(engB, engA, HDR); }
static int eng_cmp(const char *what) {
    if (ut_cmp(what, engA, engB, HDR)) { fprintf(stderr, "   (engine header)\n"); return 1; }
    if (++n_cases % 1024 == 0 && ut_cmp(what, engA, engB, ENG_SIZE)) { fprintf(stderr, "   (engine struct)\n"); return 1; }
    return 0;
}
static int eng_cmp_all(const char *what) { return ut_cmp(what, engA, engB, ENG_SIZE); }

/* a 256-bit mask with a random density / shape */
static void rbits(uint8_t *m) {
    switch (rnd(6)) {
    case 0: memset(m, 0, 32); break;
    case 1: memset(m, 0xff, 32); break;
    case 2: rndfill(m, 32); break;
    case 3: { unsigned d = rnd(257); for (int i = 0; i < 256; i++) { if (rnd(256) < d) m[i >> 3] |= 1 << (i & 7); else m[i >> 3] &= ~(1 << (i & 7)); } break; }
    default: {                                            /* spans */
        int x = 0, on = rnd(2);
        while (x < 256) { int n = 1 + rnd(rnd(2) ? 8 : 90); for (int k = 0; k < n && x < 256; k++, x++) { if (on) m[x >> 3] |= 1 << (x & 7); else m[x >> 3] &= ~(1 << (x & 7)); } on ^= 1; }
    }
    }
}
/* 6-bit planes (or junk bytes, rarely) */
static void rplanes(uint8_t *p, size_t n) { int junk = !rnd(8); for (size_t i = 0; i < n; i++) p[i] = junk ? (uint8_t)rnd64() : rnd(64); }

/* ---------------- windows ---------------- */
static void t_winmask(void) {
    unsigned n = 0;
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++, n++) {
        uint8_t a[G + 32 + G], b[sizeof a];
        rndfill(a, sizeof a); memcpy(b, a, sizeof a);
        uint32_t l = rnd(4) ? rnd(256) : (rnd(2) ? 0 : 255), r = rnd(4) ? rnd(256) : (rnd(3) ? 0 : rnd(2) ? l : 255);
        if (!rnd(8)) r = l;
        uint32_t h = l << 8 | r;
        DS(winmask_fn, DS_UPDATE_WINDOW_MASK)(a + G, h);
        spec_render_scanline_update_window_mask(b + G, h);
        if (ut_cmp("update_window_mask", a, b, sizeof a)) { fprintf(stderr, "   WINxH %04x\n", h); return; }
    }
    fprintf(stderr, "t_compose2d: update_window_mask %u cases\n", n);
}

static void t_inhibit(void) {
    unsigned n[3] = { 0 };
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++) {
        uint8_t A[G + 0xa0 + 32 + G], B[sizeof A], wa[32], wb[32], wc[32];
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        rbits(wa); rbits(wb); rbits(wc);
        uint32_t lmask = rnd(32), i1 = (uint32_t)rnd64(), i2 = (uint32_t)rnd64(), i3 = (uint32_t)rnd64(), io = (uint32_t)rnd64();
        uint8_t *inhA = A + G, *fxA = A + G + 0xa0, *inhB = B + G, *fxB = B + G + 0xa0;
        int which = rnd(3);
        if (which == 0) {
            DS(inh1_fn, DS_INHIBIT_SINGLE)(inhA, fxA, lmask, wa, i1, io);
            spec_render_scanline_window_inhibit_masks_single(inhB, fxB, lmask, wa, i1, io);
        } else if (which == 1) {
            DS(inh2_fn, DS_INHIBIT_DOUBLE)(inhA, fxA, lmask, wa, wb, i1, i2, io);
            spec_render_scanline_window_inhibit_masks_double(inhB, fxB, lmask, wa, wb, i1, i2, io);
        } else {
            DS(inh3_fn, DS_INHIBIT_TRIPLE)(inhA, fxA, lmask, wa, wb, wc, i1, i2, i3, io);
            spec_render_scanline_window_inhibit_masks_triple(inhB, fxB, lmask, wa, wb, wc, i1, i2, i3, io);
        }
        n[which]++;
        if (ut_cmp(which == 0 ? "inhibit_masks_single" : which == 1 ? "inhibit_masks_double" : "inhibit_masks_triple", A, B, sizeof A)) return;
    }
    fprintf(stderr, "t_compose2d: window_inhibit_masks single %u, double %u, triple %u cases\n", n[0], n[1], n[2]);
}

static void t_genwin(void) {
    unsigned n = 0, by_state[8] = { 0 };
    for (int it = 0; it < N_ITER(30000) && ut_fail < 10; it++) {
        uint8_t SA[G + 0xa0 + 0x60 + 32 + G], SB[sizeof SA], objwin[32];   /* inh at +G, fx at +G+0x100 */
        eng_fill(); rndfill(SA, sizeof SA); rbits(objwin);
        uint32_t line = rnd(192);
        for (int k = 0; k < 4; k++) if (rnd(2)) engA[0xae + k] = (uint8_t)(rnd(2) ? line : line + rnd(3) - 1);   /* edges hit the line */
        engA[0xb4] = rnd(8); engA[0xb5] = rnd(4);
        if (rnd(4)) { uint32_t d = *(uint32_t *)(engA + 0x90); d = (d & ~0xe000u) | rnd(8) << 13; *(uint32_t *)(engA + 0x90) = d; }
        *(uint32_t *)(engA + 0x9c) &= 0x3f3f3f3f;
        uint32_t lmask = rnd(32);
        eng_copy(); memcpy(SB, SA, sizeof SA);
        DS(genwin_fn, DS_GENERATE_WINDOWS)(engA, SA + G, SA + G + 0x100, objwin, lmask, line);
        spec_render_scanline_generate_window_masks(engB, SB + G, SB + G + 0x100, objwin, lmask, line);
        if (ut_cmp("generate_window_masks", SA, SB, sizeof SA) || eng_cmp("generate_window_masks")) {
            fprintf(stderr, "   line %u DISPCNT %08x lmask %x\n", line, *(uint32_t *)(engA + 0x90), lmask);
            return;
        }
        by_state[engA[0xb4] & ((*(uint32_t *)(engA + 0x90) >> 13) & 7)]++;
        n++;
    }
    eng_cmp_all("generate_window_masks: engine struct");
    fprintf(stderr, "t_compose2d: generate_window_masks %u cases (by active windows: %u %u %u %u %u %u %u %u)\n", n, by_state[0],
            by_state[1], by_state[2], by_state[3], by_state[4], by_state[5], by_state[6], by_state[7]);
}

static void t_applywin(void) {
    unsigned n = 0;
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++, n++) {
        uint8_t A[G + 0x100 + 0xa0 + G], B[sizeof A];      /* vis at +G (8 slots), inh at +G+0x100 */
        eng_fill(); rndfill(A, sizeof A);
        if (rnd(4)) *(uint32_t *)(engA + 0x90) &= ~0xe000u;
        if (rnd(2)) *(uint32_t *)(engA + 0x90) |= 0x2000u << rnd(3);
        eng_copy(); memcpy(B, A, sizeof A);
        uint32_t lmask = rnd(3) ? rnd(32) : (uint32_t)rnd64();
        DS(applywin_fn, DS_APPLY_WINDOWS)(engA, A + G, A + G + 0x100, lmask);
        spec_render_scanline_apply_windows(engB, B + G, B + G + 0x100, lmask);
        if (ut_cmp("apply_windows", A, B, sizeof A) || eng_cmp("apply_windows")) return;
    }
    eng_cmp_all("apply_windows: engine struct");
    fprintf(stderr, "t_compose2d: apply_windows %u cases\n", n);
}

/* ---------------- the compositor's pieces ---------------- */
/* a priority list: a random sequence of distinct slots 0..7, or DraStic's shape (per priority, the OBJ slot, then BGs) */
static void rlist(uint8_t *eng) {
    uint8_t s[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int i = 7; i > 0; i--) { int j = rnd(i + 1); uint8_t t = s[i]; s[i] = s[j]; s[j] = t; }
    unsigned n = rnd(9);
    if (rnd(3) == 0) {
        n = 0;
        int obj = rnd(2), bgp[4];
        for (int k = 0; k < 4; k++) bgp[k] = rnd(5) ? (int)rnd(4) : -1;
        for (int p = 0; p < 4; p++) { if (obj) s[n++] = 4 | p; for (int k = 0; k < 4; k++) if (bgp[k] == p) s[n++] = k; }
    }
    memcpy(eng + 0x84, s, 8);
    eng[0xb3] = n;
}

static void t_pieces(void) {
    static uint8_t A[G + 0x800 + G], B[sizeof A], oa[G + 0x300 + G], ob[sizeof oa];
    static uint8_t src[G + 0x900 + G], srcb[sizeof src];
    static uint32_t s3[256], da[G / 4 + 256 + G / 4], db[sizeof da / 4];
    unsigned n[12] = { 0 };
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++) {
        uint8_t *a = A + G, *b = B + G;
        /* the double priority encoder: vis at +0, top at +0x100, sec at +0x200 */
        eng_fill(); rlist(engA); eng_copy();
        rndfill(A, sizeof A);
        for (int k = 0; k < 8; k++) rbits(a + 32 * k);
        memcpy(B, A, sizeof A);
        DS(prio2_fn, DS_PRIORITY_DOUBLE)(engA, a, a + 0x100, a + 0x200);
        spec_render_scanline_priority_encode_double(engB, b, b + 0x100, b + 0x200);
        if (ut_cmp("priority_encode_double", A, B, sizeof A) || eng_cmp("priority_encode_double")) return;
        n[0]++;
        /* select_blend_enable from those masks into +0x300 */
        uint32_t lm = (uint32_t)rnd64(), bits = (uint32_t)rnd64();
        if (rnd(2)) { lm &= 0x1f; bits &= 0x3f; }
        DS(sbe_fn, DS_BLEND_ENABLE)(a + 0x300, a + 0x100, lm, bits);
        spec_render_scanline_select_blend_enable(b + 0x300, b + 0x100, lm, bits);
        if (ut_cmp("select_blend_enable", A, B, sizeof A)) return;
        n[1]++;
        /* shade: in planes at +0x400, mask at +0x320, out separate */
        *(uint16_t *)(engA + 0xa2) = rnd(3) ? rnd(32) : (uint16_t)rnd64();
        eng_copy();
        rplanes(a + 0x400, 0x300); rbits(a + 0x320);
        memcpy(B, A, sizeof A);
        rndfill(oa, sizeof oa); memcpy(ob, oa, sizeof oa);
        DS(shade_fn, DS_SHADE)(engA, oa + G, a + 0x400, a + 0x320);
        spec_render_scanline_shade(engB, ob + G, b + 0x400, b + 0x320);
        if (ut_cmp("shade", oa, ob, sizeof oa) || ut_cmp("shade (inputs)", A, B, sizeof A) || eng_cmp("shade")) {
            fprintf(stderr, "   BLDY %x BLDCNT %x\n", *(uint16_t *)(engA + 0xa2), *(uint16_t *)(engA + 0xa0));
            return;
        }
        n[2]++;
        /* the coefficient setups: eva at +0x100, evb at +0x200, alpha at +0x300, mask at +0x400 */
        uint32_t ba = (uint32_t)rnd64();
        rndfill(A, sizeof A); rbits(a + 0x400); memcpy(B, A, sizeof A);
        int w = rnd(4);
        static const char *const sn[4] = { "setup_blend_base", "setup_blend", "setup_alpha_base", "setup_alpha" };
        if (w == 0) { DS(sbb_fn, DS_SETUP_BLEND_BASE)(ba, a + 0x100, a + 0x200, a + 0x400); spec_render_scanline_color_effects_setup_blend_base(ba, b + 0x100, b + 0x200, b + 0x400); }
        if (w == 1) { DS(sbb_fn, DS_SETUP_BLEND)(ba, a + 0x100, a + 0x200, a + 0x400); spec_render_scanline_color_effects_setup_blend(ba, b + 0x100, b + 0x200, b + 0x400); }
        if (w == 2) { DS(sab_fn, DS_SETUP_ALPHA_BASE)(a + 0x100, a + 0x200, a + 0x300, a + 0x400); spec_render_scanline_color_effects_setup_alpha_base(b + 0x100, b + 0x200, b + 0x300, b + 0x400); }
        if (w == 3) { DS(sab_fn, DS_SETUP_ALPHA)(a + 0x100, a + 0x200, a + 0x300, a + 0x400); spec_render_scanline_color_effects_setup_alpha(b + 0x100, b + 0x200, b + 0x300, b + 0x400); }
        if (ut_cmp(sn[w], A, B, sizeof A)) { fprintf(stderr, "   BLDALPHA %x\n", ba); return; }
        n[3 + w]++;
        /* apply / apply_offset: P (6 planes) at +0, eva +0x600, evb +0x700, off +0x800 */
        uint8_t *sp = src + G;
        rndfill(src, sizeof src);
        rplanes(sp, 0x600);
        for (int i = 0; i < 256; i++) {
            int mode = rnd(4);
            uint8_t ea = mode == 0 ? 32 : mode == 1 ? (uint8_t)rnd(33) : mode == 2 ? (uint8_t)(1 + rnd(32)) : (uint8_t)rnd64();
            uint8_t eb = mode == 0 ? 0 : mode == 1 ? (uint8_t)rnd(33) : mode == 2 ? (uint8_t)(32 - ea) : (uint8_t)rnd64();
            sp[0x600 + i] = ea; sp[0x700 + i] = eb; sp[0x800 + i] = rnd(2) ? (uint8_t)rnd(33) : (uint8_t)rnd64();
        }
        memcpy(srcb, src, sizeof src);
        rndfill(oa, sizeof oa); memcpy(ob, oa, sizeof oa);
        if (rnd(2)) {
            DS(apply_fn, DS_APPLY)(oa + G, sp, sp + 0x600, sp + 0x700);
            spec_render_scanline_color_effects_apply(ob + G, srcb + G, srcb + G + 0x600, srcb + G + 0x700);
            if (ut_cmp("color_effects_apply", oa, ob, sizeof oa)) return;
            n[7]++;
        } else {
            DS(applyo_fn, DS_APPLY_OFFSET)(oa + G, sp, sp + 0x600, sp + 0x700, sp + 0x800);
            spec_render_scanline_color_effects_apply_offset_c(ob + G, srcb + G, srcb + G + 0x600, srcb + G + 0x700, srcb + G + 0x800);
            if (ut_cmp("color_effects_apply_offset_c", oa, ob, sizeof oa)) return;
            n[8]++;
        }
        if (ut_cmp("apply (inputs)", src, srcb, sizeof src)) return;
        /* the 3D horizontal shift */
        rndfill(s3, sizeof s3); rndfill(da, sizeof da); memcpy(db, da, sizeof da);
        int32_t h = rnd(3) ? rndr(-256, 255) : (rnd(2) ? -256 : rnd(2) ? 255 : 0);
        DS(hshift_fn, DS_HSHIFT)(da + G / 4, s3, h);
        spec_render_scanline_horizontal_shift_3d(db + G / 4, s3, h);
        if (ut_cmp("horizontal_shift_3d", da, db, sizeof da)) { fprintf(stderr, "   hofs %d\n", h); return; }
        n[9]++;
    }
    eng_cmp_all("compositor pieces: engine struct");
    fprintf(stderr, "t_compose2d: priority_encode_double %u, select_blend_enable %u, shade %u, setup_blend_base %u, setup_blend %u, "
            "setup_alpha_base %u, setup_alpha %u, apply %u, apply_offset %u, horizontal_shift_3d %u cases\n",
            n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9]);
}

/* ---------------- the composite, every path ---------------- */
#define FRAME 0x1d30                                     /* render_scanline_2d's frame: S - 0x180 .. S + 0x1bb0 */
#define S_OFS 0x180
static uint8_t frA[G + FRAME + G] __attribute__((aligned(64))), frB[sizeof frA] __attribute__((aligned(64)));
static uint8_t outA[G + 0x300 + G], outB[sizeof outA], pal[16];
static uint32_t px3d[256];
static unsigned path_n[64];

static uint8_t *rel(uint8_t *p) { return p ? p - frA + frB : 0; }   /* a pointer into frame A -> the same in frame B */

static void t_composite(void) {
    uint8_t *S = frA + G + S_OFS;
    for (int it = 0; it < N_ITER(40000) && ut_fail < 10; it++) {
        eng_fill(); rlist(engA);
        *(uint8_t **)(engA + 0x18) = pal;
        rndfill(pal, sizeof pal);
        *(uint16_t *)(engA + 0xa2) = rnd(4) ? rnd(17) : rnd(32);
        uint32_t bldcnt = (uint32_t)rnd64() & 0xffff;
        *(uint16_t *)(engA + 0xa0) = (uint16_t)bldcnt;
        if (rnd(2)) *(uint16_t *)(engA + 0xa4) &= 0x1f1f;
        eng_copy();
        rndfill(frA, sizeof frA);
        for (int k = 0; k < 8; k++) rbits(S + SCR_VIS + 32 * k);
        rbits(S + SCR_FX); rbits(S + SCR_SEMI); rbits(S + SCR_BMP);
        if (rnd(2)) memset(S + SCR_FX, 0, 32);
        for (int k = 0; k < 5; k++) for (int i = 0; i < 0x110; i++) ((uint16_t *)(S + SCR_LINES + 0x220 * k))[i] &= rnd(8) ? 0xffff : 0x7fff;
        /* the 3D pixels: 6-bit colour, alpha 0 / 31 / 1..30 by spans; in S (the BG0HOFS-shifted copy) or outside */
        int has3d = rnd(4) != 0, p3d_in_S = rnd(4) == 0;
        uint32_t *p3d = p3d_in_S ? (uint32_t *)S : px3d;
        { int x = 0; while (x < 256) { int n = 1 + rnd(60); uint8_t al = rnd(3) ? 31 : rnd(2) ? 0 : 1 + rnd(30); for (int k = 0; k < n && x < 256; k++, x++) p3d[x] = rnd(64) | rnd(64) << 8 | rnd(64) << 16 | (uint32_t)al << 24; } }
        /* the OBJ attribute plane (0, or 2a+1), at S+0xc90 or the 2x copy S+0xfc0 */
        uint8_t *alpha0 = rnd(2) ? S + SCR_ALPHA : S + SCR_ALPHA2X;
        for (int i = 0; i < 256; i++) alpha0[i] = rnd(2) ? 0 : (uint8_t)(2 * rnd(16) + 1);
        uint32_t lmask = rnd(32);
        uint32_t flags = rnd(4) ? (rnd(64) | (rnd(2) ? 8 : 0)) : (uint32_t)rnd(16);
        uint8_t *alpha = (flags & 2) ? alpha0 : (rnd(4) ? 0 : alpha0);
        uint8_t *layersA[5], *layersB[5];
        for (int k = 0; k < 5; k++) { layersA[k] = S + SCR_LINES + 0x220 * k; layersB[k] = rel(layersA[k]); }
        rndfill(outA, sizeof outA);
        memcpy(frB, frA, sizeof frA); memcpy(outB, outA, sizeof outA);
        uint32_t line = rnd(192);
        DS(comp_fn, DS_COMPOSITE)(engA, outA + G, S, layersA, has3d ? p3d : 0, alpha, lmask, bldcnt, flags, line);
        spec_render_scanline_2d_composite(engB, outB + G, rel(S), layersB, has3d ? (p3d_in_S ? (uint32_t *)rel(S) : px3d) : 0,
                                          rel(alpha), lmask, bldcnt, flags, line);
        path_n[(flags & 7 ? 1 : 0) | (flags & 8 ? 2 : 0) | (flags & 5) << 2]++;
        if (ut_cmp("composite planes", outA, outB, sizeof outA) || ut_cmp("composite frame", frA, frB, sizeof frA) ||
            eng_cmp("composite")) {
            fprintf(stderr, "   flags %x lmask %x bldcnt %04x BLDY %d BLDALPHA %04x list n=%d 3d=%d alpha=%d\n", flags, lmask,
                    bldcnt, *(uint16_t *)(engA + 0xa2), *(uint16_t *)(engA + 0xa4), engA[0xb3], has3d, alpha != 0);
            return;
        }
    }
    eng_cmp_all("composite: engine struct");
    fprintf(stderr, "t_compose2d: composite paths (index: complex | shade << 1 | (flags & 5) << 2):");
    for (int i = 0; i < 32; i++) if (path_n[i]) fprintf(stderr, " %d:%u", i, path_n[i]);
    fprintf(stderr, "\n");
}

/* ---------------- capture ---------------- */
static void t_capture(void) {
    static uint8_t C[0x60], planes[0x300];
    static uint16_t srcb[256], dA[G / 2 + 256 + G / 2], dB[sizeof dA / 2];
    static uint32_t px[256];
    unsigned n[4] = { 0 };
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++) {
        rndfill(C, sizeof C);
        *(uint16_t *)(C + 0x4c) = rnd(2) ? 128 : 256;
        C[0x54] = rnd(3) ? rnd(17) : 16; C[0x55] = rnd(3) ? rnd(17) : 0;
        int nob = rnd(4) == 0;
        *(uint64_t *)(C + 0x40) = nob ? 0 : (uint64_t)(uintptr_t)srcb;
        rplanes(planes, sizeof planes); rndfill(srcb, sizeof srcb);
        for (int i = 0; i < 256; i++) px[i] = rnd(64) | rnd(64) << 8 | rnd(64) << 16 | (uint32_t)(rnd(2) ? rnd(32) : (uint8_t)rnd64()) << 24;
        if (rnd(8) == 0) rndfill(px, sizeof px);
        rndfill(dA, sizeof dA); memcpy(dB, dA, sizeof dA);
        int w = rnd(4);
        static const char *const nm[4] = { "capture_direct", "capture_direct_3d", "capture_blended", "capture_blended_3d" };
        uint16_t *a = dA + G / 2, *b = dB + G / 2;
        if (w == 0) { DS(capd_fn, DS_CAPTURE_DIRECT)(C, a, planes); spec_render_scanline_capture_direct(C, b, planes); }
        if (w == 1) { DS(capd_fn, DS_CAPTURE_DIRECT_3D)(C, a, px); spec_render_scanline_capture_direct_3d(C, b, px); }
        if (w == 2) { DS(capb_fn, DS_CAPTURE_BLENDED)(C, a, srcb, planes); spec_render_scanline_capture_blended(C, b, srcb, planes); }
        if (w == 3) { const uint16_t *sb = nob ? 0 : srcb; DS(capb_fn, DS_CAPTURE_BLENDED_3D)(C, a, sb, px); spec_render_scanline_capture_blended_3d(C, b, sb, px); }
        n[w]++;
        if (ut_cmp(nm[w], dA, dB, sizeof dA)) { fprintf(stderr, "   eva %d evb %d width %d\n", C[0x54], C[0x55], *(uint16_t *)(C + 0x4c)); return; }
    }
    fprintf(stderr, "t_compose2d: capture_direct %u, capture_direct_3d %u, capture_blended %u, capture_blended_3d %u cases\n",
            n[0], n[1], n[2], n[3]);
}

/* ---------------- the 32-bit scanout ---------------- */
static void t_convert(void) {
    static uint8_t e[0x300], o[0x300];
    static uint32_t dA[G / 4 + 512 + G / 4], dB[sizeof dA / 4];
    unsigned n[4] = { 0 };
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++) {
        rplanes(e, sizeof e); rplanes(o, sizeof o);
        rndfill(dA, sizeof dA); memcpy(dB, dA, sizeof dA);
        int w = rnd(4);
        uint32_t f = rnd(3) ? 2 * rnd(16) : rnd(64), fac = 32 - f, add = rnd(2) ? 63 * f + 16 : 16;
        if (rnd(8) == 0) { fac = (uint32_t)rnd64(); add = (uint32_t)rnd64(); }
        static const char *const nm[4] = { "convert_direct_32_1x", "convert_direct_32_2x", "convert_shade_32_1x", "convert_shade_32_2x" };
        uint32_t *a = dA + G / 4, *b = dB + G / 4;
        if (w == 0) { DS(cv1_fn, DS_CONVERT_DIRECT_1X)(e, a); spec_render_scanline_color_convert_direct_32_1x(e, b); }
        if (w == 1) { DS(cv2_fn, DS_CONVERT_DIRECT_2X)(e, o, a); spec_render_scanline_color_convert_direct_32_2x(e, o, b); }
        if (w == 2) { DS(cs1_fn, DS_CONVERT_SHADE_1X)(e, a, fac, add); spec_render_scanline_color_convert_shade_32_1x(e, b, fac, add); }
        if (w == 3) { DS(cs2_fn, DS_CONVERT_SHADE_2X)(e, o, a, fac, add); spec_render_scanline_color_convert_shade_32_2x(e, o, b, fac, add); }
        n[w]++;
        if (ut_cmp(nm[w], dA, dB, sizeof dA)) { fprintf(stderr, "   factor %u add %u\n", fac, add); return; }
    }
    fprintf(stderr, "t_compose2d: convert direct_32_1x %u, direct_32_2x %u, shade_32_1x %u, shade_32_2x %u cases\n", n[0], n[1], n[2], n[3]);
}

/* ---------------- the closed form (pixel.h) against DraStic's composite ---------------- */
static int bit(const uint8_t *m, int i) { return m[i >> 3] >> (i & 7) & 1; }
static void setb(uint8_t *m, int i, int v) { if (v) m[i >> 3] |= 1 << (i & 7); else m[i >> 3] &= ~(1 << (i & 7)); }
static void spans(uint8_t *m, int pct) {
    int x = 0, on = rnd(2);
    while (x < 256) { int n = 1 + rnd(rnd(2) ? 6 : 50); for (int k = 0; k < n && x < 256; k++, x++) setb(m, x, on); on = (int)rnd(100) < pct; }
}

static void t_closed(void) {
    uint8_t *S = frA + G + S_OFS;
    static uint8_t ref[0x300], al0[256];
    static uint16_t bd[4];
    unsigned long paths[64] = { 0 }, px_blend = 0;
    for (int it = 0; it < N_ITER(20000) && ut_fail < 10; it++) {
        rndfill(frA, sizeof frA);
        eng_fill();
        *(uint16_t **)(engA + 0x18) = bd; bd[0] = (uint16_t)rnd64();
        /* DISPCNT-like state: the BG enables, OBJ on, BG0 = 3D, the priorities; the list as reorder_layers builds it */
        uint32_t bgon = rnd(16), objon = rnd(4) != 0, is3d = (bgon & 1) && rnd(3) != 0, prio[4];
        for (int k = 0; k < 4; k++) prio[k] = rnd(4);
        unsigned n = 0;
        for (unsigned p = 0; p < 4; p++) { if (objon) engA[0x84 + n++] = 4 | p; for (int k = 0; k < 4; k++) if ((bgon >> k & 1) && prio[k] == p) engA[0x84 + n++] = k; }
        engA[0xb3] = n;
        uint8_t *vis = S + SCR_VIS;
        for (int k = 0; k < 8; k++) { memset(vis + 32 * k, 0, 32); if (rnd(5)) spans(vis + 32 * k, rnd(100)); }
        uint32_t *p3d = 0;
        if (is3d) {
            int x = 0;
            while (x < 256) { int m = 1 + rnd(60); uint32_t a = rnd(3) ? 31 : rnd(2) ? 0 : 1 + rnd(30); for (int k = 0; k < m && x < 256; k++, x++) px3d[x] = rnd(64) | rnd(64) << 8 | rnd(64) << 16 | a << 24; }
            p3d = px3d;
        }
        /* lmask before windows and the blank-layer drop, and the flags of render_scanline_2d (compose.md 3.2) */
        uint32_t objret = objon && rnd(4) ? 0x10 : 0, img = rnd(10) == 0;
        if (objon && !objret) memset(vis + 0x80, 0, 0x80);         /* render_scanline_obj_c found no OBJ: empty slots */
        if (img) spans(vis + 32 * (4 + rnd(4)), 60);                /* the image's bit-15 map over its slot */
        uint32_t lm0 = bgon | objret | (img ? 0x10 : 0);
        uint32_t BLD = (uint32_t)rnd64() & 0xffff;
        *(uint16_t *)(engA + 0xa0) = (uint16_t)BLD;
        *(uint16_t *)(engA + 0xa2) = rnd(4) ? rnd(17) : rnd(32);
        if (rnd(2)) *(uint16_t *)(engA + 0xa4) &= 0x1f1f;
        uint32_t lineflags = rnd(2) ? rnd(4) : 0, flags = lineflags;
        uint32_t bm = BLD & (lm0 | lm0 << 8 | 0xf0f0), mode = BLD >> 6 & 3;
        if (mode == 1) { if ((bm & 0x3f) && (bm & 0x3f00)) flags |= 4; }
        else if (mode >= 2) { if ((bm & 0x3f) && *(uint16_t *)(engA + 0xa2)) flags |= 8; }
        if (!(bm & 0x3f00)) flags &= ~1u;
        if (p3d) flags |= DS(vis3d_fn, DS_SET_3D_VISIBILITY)(vis, p3d);
        if (img) flags |= 0x20;
        uint32_t lmask = lm0;
        for (int k = 0; k < 4; k++) if (lmask >> k & 1) { int z = 1; for (int j = 0; j < 32; j++) z &= vis[32 * k + j] == 0; if (z && rnd(4)) lmask &= ~(1u << k); }
        /* the OBJ attribute plane and the semi / bitmap masks, as render_scanline_obj_c leaves them */
        uint8_t *semi = S + SCR_SEMI, *bmpm = S + SCR_BMP, *fx = S + SCR_FX, *al = S + SCR_ALPHA;
        if (lineflags) {
            for (int x = 0; x < 256; x++) {
                int kind = rnd(4);
                setb(semi, x, kind == 2); setb(bmpm, x, kind == 3); al[x] = kind == 3 ? (uint8_t)(2 * rnd(16) + 1) : 0;
            }
        } else { memset(semi, 0, 32); memset(bmpm, 0, 32); }
        memset(fx, 0, 32); if (rnd(3) == 0) spans(fx, 40);
        memcpy(al0, al, 256);
        uint8_t *alpha = (flags & 2) ? al : 0;
        uint8_t *layersA[5], *layersB[5];
        uint16_t *lines[5];
        for (int k = 0; k < 5; k++) { layersA[k] = S + SCR_LINES + 0x220 * k; layersB[k] = rel(layersA[k]); lines[k] = (uint16_t *)(layersA[k] + 0x10); }
        eng_copy();
        /* the closed form from the inputs before the call */
        spec_composite_in in = { engA, vis, semi, bmpm, fx, lines, p3d, alpha ? al0 : 0, lmask, bm, flags };
        for (int x = 0; x < 256; x++) {
            uint8_t o[3];
            spec_composite_pixel(&in, x, o);
            ref[x] = o[0]; ref[0x100 + x] = o[1]; ref[0x200 + x] = o[2];
            if (flags & 7) px_blend++;
        }
        rndfill(outA, sizeof outA);
        memcpy(frB, frA, sizeof frA); memcpy(outB, outA, sizeof outA);
        uint32_t line = rnd(192);
        DS(comp_fn, DS_COMPOSITE)(engA, outA + G, S, layersA, p3d, alpha, lmask, bm, flags, line);
        spec_render_scanline_2d_composite(engB, outB + G, rel(S), layersB, p3d, rel(alpha), lmask, bm, flags, line);
        paths[(flags & 7 ? 1 : 0) | (flags & 8 ? 2 : 0) | (flags & 5) << 2]++;
        char what[128];
        snprintf(what, sizeof what, "closed form test %d (flags %#x bm %04x lmask %#x)", it, flags, bm, lmask);
        if (ut_cmp(what, outA, outB, sizeof outA) || ut_cmp(what, frA, frB, sizeof frA) || eng_cmp(what)) { fprintf(stderr, "   (DraStic vs the port)\n"); return; }
        if (ut_cmp(what, outA + G, ref, sizeof ref)) {
            int x = 0;
            while (outA[G + x] == ref[x] && outA[G + 0x100 + x] == ref[0x100 + x] && outA[G + 0x200 + x] == ref[0x200 + x]) x++;
            fprintf(stderr, "   (DraStic vs the closed form: pixel %d DraStic %d,%d,%d closed form %d,%d,%d)\n", x, outA[G + x],
                    outA[G + 0x100 + x], outA[G + 0x200 + x], ref[x], ref[0x100 + x], ref[0x200 + x]);
            return;
        }
    }
    eng_cmp_all("closed form: engine struct");
    fprintf(stderr, "t_compose2d: closed form: %lu pixels on the complex path; paths:", px_blend);
    for (int k = 0; k < 64; k++) if (paths[k]) fprintf(stderr, " %d:%lu", k, paths[k]);
    fprintf(stderr, "\n");
}

void ut_main(void) {
    engA = malloc(ENG_SIZE); engB = malloc(ENG_SIZE);
    if (!engA || !engB) { fprintf(stderr, "FAIL: no memory\n"); ut_fail++; return; }
    rndfill(engA, ENG_SIZE); memcpy(engB, engA, ENG_SIZE);
    t_winmask();
    t_inhibit();
    t_genwin();
    t_applywin();
    t_pieces();
    t_composite();
    t_capture();
    t_convert();
    t_closed();
    eng_cmp_all("the end: engine struct");
}
