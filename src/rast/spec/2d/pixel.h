/* pixel.h: the per-pixel closed form of DraStic r2.5.2.2's render_scanline_2d_composite (0x3c6d0; compose.c, and
 * tools/rast/re2d/compose.md 5.6). Everything the composite computes in masks and planes on the way (the encoders'
 * masks, T1 / T2 / M, the EVA / EVB / OFF planes, the second planes) collapses into an independent function of each
 * pixel: given the two front layers of the pixel, their colours, the flags and BLDCNT / BLDALPHA / BLDY, it yields
 * DraStic's 6-bit output, byte for byte, for every input render_scanline_2d produces. This is the contract a fused or
 * NEON composite implements. Header only.
 *
 * Valid for the inputs render_scanline_2d passes (compose.c, WHERE IT RUNS): lmask holds every listed BG whose
 * visibility slot is not empty (disable_blank_layers ran), the OBJ slots are empty when render_scanline_obj_c returned
 * 0, flags and bm are derived as render_scanline_2d derives them, vis[0] is the 3D alpha map when BG0 is 3D, and
 * alpha is non-NULL exactly when flags & 2. Outside that (flags & 7 == 2 with alpha == NULL) DraStic reads stale
 * coefficient bytes. Verified against DraStic's own composite on such inputs by tools/rast/ut/t_compose2d.c; during
 * the analysis on 461 M pixels against the C model and on 1.4 M composites of the ds2d scenes in the running emulator.
 *
 * Per pixel i:
 *   walk the list eng+0x84 (count eng+0xb3) front to back: slot s < 4: BG s is present if vis[s] bit i; slots 4..7:
 *   OBJ is present if vis[s] bit i, counted once (at the first OBJ slot that has the bit)
 *   t = the first present layer, the backdrop if none; u = the second present layer, the backdrop if exactly one is
 *   present, none if no layer is
 *   T1 = bm bit (t == backdrop ? 5 : t) and not fx bit i      t is a 1st target, effects enabled here
 *   T2 = u exists and bm bit (u == backdrop ? 13 : 8 + u)     u is a 2nd target (fx not applied)
 *   fy = BLDY <= 16 ? 2*BLDY : 32;  brighten = !bm.6
 *   flags & 7 == 0 (simple): out = col(t), or with flags & 8 and T1: (col(t)*(32 - fy) + (brighten ? 63*fy : 0) + 16) >> 5
 *   else: A = min((2*BLDALPHA) & 0x3f, 32), B = min(((2*BLDALPHA) >> 8) & 0x3f, 32); semi = SEMI bit i and t == OBJ
 *     M = flags & 5:  4: T1 && T2;  5: (semi || T1) && T2;  1: semi && T2;  0: false.  f = flags, | 4 if flags & 5 == 1
 *     with flags & 5: f & 0x10: M &&= t != BG0;  f & 0x20: M &&= t != OBJ
 *     (EVA, EVB, OFF) = (32, 0, 0);  f & 8: EVA = T1 ? 32 - fy : 32, OFF = brighten && T1 ? fy : 0, (A, B) if f & 4 and M
 *                                    else: (A, B) if f & 4 and M
 *     f & 2 and alpha: a = t == BG0 with the 3D layer on and !(flags & 8) ? the 3D alpha : alpha[i];
 *                      (BMP bit i and t == OBJ and T2) or (!bm.7 and p3d and t == BG0 and T2): (EVA, EVB) = (a+1, 31-a)
 *     out = min(63, (col(t)*EVA + col(u)*EVB + 63*OFF + 16) >> 5) per channel
 *   col(X) = X == backdrop ? E(*[eng+0x18]) : X == BG0 with p3d and lmask bit 0 ? bytes 0..2 of p3d[i] : E(line X [i]);
 *   E(c) = ((c & 31) << 1, (c >> 5 & 31) << 1, (c >> 10 & 31) << 1). Layer numbers: BG0..BG3 = 0..3, OBJ = 4. */
#ifndef SPEC_2D_PIXEL_H
#define SPEC_2D_PIXEL_H
#include <stdint.h>

#define SPEC_PX_BD   5                  /* the backdrop as a layer number */
#define SPEC_PX_NONE 6                  /* no layer */

typedef struct {
    const uint8_t *eng;                 /* the engine: list eng+0x84 / count eng+0xb3, BLDY eng+0xa2, BLDALPHA eng+0xa4,
                                         * the backdrop *[eng+0x18] */
    const uint8_t *vis, *semi, *bmp, *fx;   /* S+0xda0 (8 slots), S+0xec0, S+0xee0, S+0xfa0 */
    uint16_t *const *line;              /* the u16 lines of BG0..BG3 and OBJ (their pixel 0) */
    const uint32_t *p3d;                /* NULL or the 3D line / quarter (BG0 is 3D) */
    const uint8_t *alpha;               /* the alpha plane as passed, before the call (NULL when flags & 2 == 0) */
    uint32_t lmask, bm, flags;          /* lmask as passed, the bldcnt argument, flags */
} spec_composite_in;

static inline int spec_px_bit(const uint8_t *m, int i) { return m[i >> 3] >> (i & 7) & 1; }

/* the 6-bit colour of layer L at pixel i */
static inline void spec_composite_colour(const spec_composite_in *in, int L, int i, int c[3]) {
    uint16_t v;
    if (L == 0 && in->p3d && (in->lmask & 1)) {
        uint32_t p = in->p3d[i];
        c[0] = p & 0xff; c[1] = p >> 8 & 0xff; c[2] = p >> 16 & 0xff;
        return;
    }
    v = L == SPEC_PX_BD ? **(uint16_t *const *)(in->eng + 0x18) : in->line[L][i];
    c[0] = (v & 0x1f) << 1; c[1] = (v >> 5 & 0x1f) << 1; c[2] = (v >> 10 & 0x1f) << 1;
}

/* pixel i of the composite's planes: out[0..2] = R6, G6, B6 */
static inline void spec_composite_pixel(const spec_composite_in *in, int i, uint8_t out[3]) {
    const uint8_t *eng = in->eng;
    /* 1. the two front layers: the list front to back; the first OBJ slot with the bit stands for OBJ */
    int top = SPEC_PX_NONE, sec = SPEC_PX_NONE, objseen = 0;
    for (int k = 0; k < eng[0xb3]; k++) {
        int s = eng[0x84 + k], L;
        if (!spec_px_bit(in->vis + 32 * s, i)) continue;
        if (s & 4) { if (objseen) continue; objseen = 1; L = 4; } else L = s;
        if (top == SPEC_PX_NONE) top = L; else if (sec == SPEC_PX_NONE) sec = L;
    }
    if (top == SPEC_PX_NONE) top = SPEC_PX_BD; else if (sec == SPEC_PX_NONE) sec = SPEC_PX_BD;
    /* 2. the target bits */
    int t[3], s[3] = { 0, 0, 0 };
    spec_composite_colour(in, top, i, t);
    uint32_t bm = in->bm, f = in->flags;
    int T1 = (top == SPEC_PX_BD ? bm >> 5 & 1 : bm >> top & 1) && !spec_px_bit(in->fx, i);
    int T2 = sec != SPEC_PX_NONE && (sec == SPEC_PX_BD ? bm >> 13 & 1 : bm >> (8 + sec) & 1);
    if (T2) spec_composite_colour(in, sec, i, s);
    uint32_t y = (uint32_t)eng[0xa2] | (uint32_t)eng[0xa3] << 8, fy = y <= 16 ? 2 * y : 32;
    int brighten = !(bm & 0x40);
    if (!(f & 7)) {                                            /* the simple path (+ the shade) */
        for (int c = 0; c < 3; c++)
            out[c] = (uint8_t)((f & 8) && T1 ? (t[c] * (32 - (int)fy) + (brighten ? 63 * (int)fy : 0) + 16) >> 5 : t[c]);
        return;
    }
    /* 3. the complex path: the coefficients */
    uint32_t ba = 2u * ((uint32_t)eng[0xa4] | (uint32_t)eng[0xa5] << 8), A = ba & 0x3f, B = ba >> 8 & 0x3f;
    if (A > 32) A = 32;
    if (B > 32) B = 32;
    int semi = spec_px_bit(in->semi, i) && top == 4;
    int M = 0;
    switch (f & 5) {
    case 4: M = T1 && T2; break;
    case 5: M = (semi || T1) && T2; f &= ~1u; break;
    case 1: M = semi && T2; f = (f & ~1u) | 4; break;
    }
    if (in->flags & 5) {
        if (f & 0x10) M = M && top != 0;
        if (f & 0x20) M = M && top != 4;
    }
    int eva = 32, evb = 0, off = 0, use_off = 0, init = f & 4;
    if (f & 8) {
        eva = T1 ? 32 - (int)fy : 32; evb = 0;
        if (brighten) { off = T1 ? (int)fy : 0; use_off = 1; }
        if (init && M) { eva = (int)A; evb = (int)B; }
        init = 1;
    } else if (init && M) { eva = (int)A; evb = (int)B; }
    if ((f & 2) && in->alpha) {
        int M2 = (spec_px_bit(in->bmp, i) && top == 4 && T2) || (!(bm & 0x80) && in->p3d && top == 0 && T2);
        int a = in->alpha[i];
        if (top == 0 && in->p3d && (in->lmask & 1) && !(in->flags & 8)) a = in->p3d[i] >> 24;   /* binary32_alpha */
        if (M2) { eva = (uint8_t)(a + 1); evb = (uint8_t)(31 - a); }
        else if (!init) { eva = 32; evb = 0; }
    }
    /* 4. one multiply-add per channel */
    for (int c = 0; c < 3; c++) {
        uint32_t v = (uint32_t)(t[c] * eva + s[c] * evb + (use_off ? 63 * off : 0) + 16) >> 5;
        out[c] = (uint8_t)(v > 63 ? 63 : v);
    }
}
#endif
