/* compvis.h: render_scanline_set_3d_visibility (spec/composite.c) in NEON, for the compositing hook (comp.c) and the
 * per-bin precompute. Same 32 bytes and return value as the original for any input (tools/rast/ut/t_composite.c
 * tests it against the C port).
 *
 * Per 16 pixels: one ld4.16b de-interleaves the four bytes of the pixels; its 4th register a holds the 16 alphas.
 *   visibility: m = cmtst(a, a) (0xff where a != 0); m & {1, 2, 4, .., 128, 1, 2, .., 128} gives each pixel its bit
 *               weight inside its bitmap byte; three addp levels sum groups of 8 neighbours, which ORs their disjoint
 *               bits: level 1 pairs (one addp per 32 pixels), level 2 quads, level 3 octets = bitmap bytes 0..15 and
 *               16..31 (addp of (x, y) puts x's pair sums before y's, so the bytes come out in pixel order).
 *   translucency: umin(a, a ^ 0x1f) is non-zero exactly when a != 0 and a != 0x1f, for every byte value (a ^ 0x1f is
 *               0 only for a = 0x1f), OR-accumulated over all pixels; non-zero at the end means "return 2".
 *   "some pixel visible" is "some bitmap byte non-zero": umaxv over the OR of the two bitmap vectors.
 * The 16 loads are software-pipelined in two register groups (v0-v7 and v16-v23): the next 32 pixels load while the
 * current 32 are folded, and the 3 ld4 registers that do not hold alpha serve as the fold's temporaries. 124
 * instructions (139 a call through comp.c's hook, with its table check), against the original's ~425 (a gather pass through a stack array, then nibble folds). It is
 * hand-scheduled asm because the compiler hoists all 16 ld4 (64 registers' worth) to the top and spills. */
#ifndef COMPVIS_H
#define COMPVIS_H
#include <stdint.h>

/* one group's fold: alphas A0, A1 (pixels 16k.., 16k+16..), temporaries T0..T3; L = the level-1 addp result */
#define CV_FOLD(A0, A1, T0, T1, T2, T3, L, ACC) \
    "eor   " T0 ".16b, " A0 ".16b, v30.16b\n\t"  "eor   " T1 ".16b, " A1 ".16b, v30.16b\n\t" \
    "cmtst " T2 ".16b, " A0 ".16b, " A0 ".16b\n\t"  "cmtst " T3 ".16b, " A1 ".16b, " A1 ".16b\n\t" \
    ACC(A0, A1, T0, T1) \
    "and   " T2 ".16b, " T2 ".16b, v31.16b\n\t"  "and   " T3 ".16b, " T3 ".16b, v31.16b\n\t" \
    "addp  " L ".16b, " T2 ".16b, " T3 ".16b\n\t"
/* translucency: the first fold writes the accumulators v28, v29, the others OR into them */
#define CV_ACC0(A0, A1, T0, T1) "umin v28.16b, " A0 ".16b, " T0 ".16b\n\t"  "umin v29.16b, " A1 ".16b, " T1 ".16b\n\t"
#define CV_ACC(A0, A1, T0, T1) \
    "umin " T0 ".16b, " A0 ".16b, " T0 ".16b\n\t"  "umin " T1 ".16b, " A1 ".16b, " T1 ".16b\n\t" \
    "orr  v28.16b, v28.16b, " T0 ".16b\n\t"        "orr  v29.16b, v29.16b, " T1 ".16b\n\t"
#define CV_LDA "ld4 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p]], #64\n\t" "ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p]], #64\n\t"
#define CV_LDB "ld4 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[p]], #64\n\t" "ld4 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[p]], #64\n\t"
#define CV_FA(L, ACC) CV_FOLD("v3", "v7", "v0", "v1", "v2", "v4", L, ACC)
#define CV_FB(L, ACC) CV_FOLD("v19", "v23", "v16", "v17", "v18", "v20", L, ACC)

static inline __attribute__((always_inline)) uint32_t comp_vis_neon(uint8_t *bits, const uint32_t *px) {
    static const uint8_t wts[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8_t *p = (const uint8_t *)px;
    uint32_t r, t, v;
    __asm__(
        CV_LDA CV_LDB
        "ldr   q31, [%[w]]\n\t"
        "movi  v30.16b, #0x1f\n\t"
        CV_FA("v24", CV_ACC0) CV_LDA                                   /* pixels 0..31 (level 1 -> v24) */
        CV_FB("v25", CV_ACC) "addp v26.16b, v24.16b, v25.16b\n\t" CV_LDB    /* 32..63; level 2 -> v26 */
        CV_FA("v24", CV_ACC) CV_LDA                                    /* 64..95 */
        CV_FB("v25", CV_ACC) "addp v27.16b, v24.16b, v25.16b\n\t"       /* 96..127; level 2 -> v27 */
        "addp v26.16b, v26.16b, v27.16b\n\t" CV_LDB                    /* bitmap bytes 0..15 -> v26 */
        CV_FA("v24", CV_ACC) CV_LDA                                    /* 128..159 */
        CV_FB("v25", CV_ACC) "addp v27.16b, v24.16b, v25.16b\n\t" CV_LDB    /* 160..191; level 2 -> v27 */
        CV_FA("v24", CV_ACC)                                           /* 192..223 */
        CV_FB("v25", CV_ACC) "addp v24.16b, v24.16b, v25.16b\n\t"       /* 224..255; level 2 -> v24 */
        "addp v27.16b, v27.16b, v24.16b\n\t"                           /* bitmap bytes 16..31 -> v27 */
        "stp   q26, q27, [%[b]]\n\t"
        "orr   v28.16b, v28.16b, v29.16b\n\t"
        "orr   v24.16b, v26.16b, v27.16b\n\t"
        "umaxv b28, v28.16b\n\t"
        "umaxv b24, v24.16b\n\t"
        "fmov  %w[t], s28\n\t"
        "fmov  %w[v], s24\n\t"
        "cmp   %w[v], #0\n\t"
        "cset  %w[r], ne\n\t"
        "lsl   %w[r], %w[r], #4\n\t"                                   /* 0x10 if some pixel is visible */
        "cmp   %w[t], #0\n\t"
        "mov   %w[v], #2\n\t"
        "csel  %w[r], %w[v], %w[r], ne\n\t"                            /* 2 if some pixel is translucent */
        : [r] "=&r"(r), [t] "=&r"(t), [v] "=&r"(v), [p] "+&r"(p), "=m"(*(uint8_t (*)[32])bits)
        : [b] "r"(bits), [w] "r"(wts), "m"(*(const uint8_t (*)[1024])px)
        : "cc", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return r;
}
#undef CV_FOLD
#undef CV_ACC0
#undef CV_ACC
#undef CV_LDA
#undef CV_LDB
#undef CV_FA
#undef CV_FB
#endif
