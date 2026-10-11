/* ds2d.c: DraStic's 2D scanline routines on the emulation thread, replaced by exact ones that cost fewer A55 cycles.
 *
 * DraStic composites every DS line on the emulation thread (render_scanline -> render_scanline_2d -> the quarter's
 * compositor), so its 2D routines are on the critical path of every frame; at 2x the conversion of the 6-bit planes
 * into the 32-bit output rows alone is 0.62 M modeled A55 cycles a frame in every game (768 calls). Each hook here
 * patches the DraStic function's entry with rast.c's rast_hook() (its first four instructions checked) to jump to
 * ours; ours writes the same bytes and nothing else the callers can see:
 *
 *   H_CONV   render_scanline_color_convert_direct_32_2x_asm (0xa0ae0) -> ds2d_conv32_2x (ds2d_asm.S): the same loop
 *           with ldr q at a register offset, loads a step ahead (23 modeled cycles per 32 pixels, the original 50).
 *           Callers: render_scanline (C) only, twice per output line pair (x0 / x1 = the two quarters' planes, x2 =
 *           the output row); C callers keep nothing in x0-x18 / v0-v7 / v16-v31 across the call.
 *   H_CONV1X render_scanline_color_convert_direct_32_1x_asm (0xa0a80, the same for one quarter at 1x, hires_3d off:
 *           render_scanline, C) -> ds2d_conv32_1x: 21 modeled cycles per 32 pixels against 43. The 16-bit
 *           conversions (_16_1x, _16_2x) do not run in the handhelds' configuration (32-bit output); _16_1x could not
 *           be hooked anyway (its loop branches back into its first four instructions).
 *   H_SELECT render_scanline_select_pixels (0x39330, C: the quarter's layer merge, spec/composite.c) ->
 *           ds2d_select(): DraStic merges the layers' u16 lines pass by pass through a u16 line on its stack
 *           (binary_asm per further layer, binary_scalar_asm for the backdrop), then expands the line into the 6-bit
 *           planes (expand_6bit_split_asm) and writes the 3D bytes over them (binary32_asm, the _alpha variant also
 *           the alpha bytes): ~630 modeled cycles a quarter with one layer, in passes whose loads stall (IPC 0.35 -
 *           0.7). ds2d_select() does it all per 32 pixels in registers (two independent halves of 16): the first
 *           layer's line, each further layer and the backdrop selected by their masks (a zero mask word skips the
 *           layer), the planes from the merged colours, the 3D bytes (ldr q + two levels of uzp) where BG0's mask is
 *           set, and stores the planes once; specialised for 1..5 layers and for the 3D bytes. Callers:
 *           C (render_scanline_2d_composite, also through comp.c's fused path). The original's only other writes
 *           are to its own stack, below the caller's; buffers that overlap (DraStic's never do) go to the original.
 *   H_EFFECTS render_scanline_color_effects_apply_asm (0xa0378, the blend of two planar quarters with per-pixel
 *           weights) -> ds2d_effects_apply (ds2d_asm.S): 32 pixels a step from ldr q at register offsets, 63 modeled
 *           cycles against the original's 89 per 16. Caller: a tail branch from render_scanline_2d_composite (C);
 *           its buffers (the planes and render_scanline_2d's stack copies) never overlap.
 *   H_PRIO   render_scanline_priority_encode_single_asm (0x9ffc0: the quarter's per-layer "top layer" masks from the
 *           visibility bitmaps, layer list front to back) -> ds2d_prio_single(): the same walk with ldr q / stp q
 *           (the original's ld1 / st1 of two registers hold the A55's load and store pipes ~4 cycles each, ~12 a
 *           layer). Callers: render_scanline_2d_composite (C) and comp.c's cf_run() (a C call); the original's
 *           helpers (priority_encode_single_bg_layer etc.) are branched to from inside it only.
 *   H_SPAN4  render_scanline_tiled_span_4bpp_asm (0x9efa0, a 4bpp text BG line: 33 tiles' rows and palette lookups)
 *           -> ds2d_span4 (ds2d_asm.S): each step's scalar loads issued during the step before, the palettes by
 *           ldr q + uzp instead of ld2, the pixels by zip + str q instead of st2 (63 modeled cycles per 4 tiles
 *           against 94). Caller: render_scanline_tiled_ext (C). Its first instruction is an adr, so the check mode
 *           runs the original through ds2d_span4_orig (ds2d_asm.S: its first four instructions' effects, then a jump
 *           to its fifth) instead of the trampoline.
 *   H_BLEND  render_scanline_select_blend_enable_asm (0xa0070: the quarter's 256-bit mask of the pixels
 *           whose top layer is a blending target) -> ds2d_blend_enable(): the original walks all six masks with ld1
 *           of two registers and writeback (~45 modeled cycles); ours skips the layers whose bit in the first
 *           argument is clear (they change nothing) and loads with ldr q. Callers: render_scanline_2d_composite (C).
 *
 * RAST_2D (or DSFLIP_RAST_2D): 0 leaves DraStic's functions alone; otherwise a mask of the hooks (default all):
 * 1 H_CONV, 2 H_SELECT, 4 H_EFFECTS, 8 H_BLEND, 16 H_PRIO, 32 H_CONV1X, 64 H_SPAN4. RAST_2DCHECK=1: every call of a
 * hooked function runs ours, then DraStic's original (through the trampoline rast_hook() returns, or
 * ds2d_span4_orig) on the same input bytes (every region either may write, and the inputs of the routines whose
 * outputs could reach them, is saved before ours and restored before the original), and compares those regions
 * afterwards (the outputs with up to 64 bytes either side, within their pages); ours is left in place.
 * "[ds2d] check:" counts every 2 s, "[ds2dcheck]" lines name the first differing byte. The 2D routines run on the
 * emulation thread only; the check's buffers are under a mutex all the same. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arm_neon.h>
#include "rast.h"
#include "ds2d.h"

enum { H_CONV, H_SELECT, H_EFFECTS, H_BLEND, H_PRIO, H_CONV1X, H_SPAN4, H_N };

/* the originals (trampolines) */
typedef void (*conv_fn)(const uint8_t *a, const uint8_t *b, uint32_t *out);
static conv_fn orig_conv;
typedef void (*select_fn)(uint8_t *eng, uint8_t *out, const uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                          uint8_t *alpha, uint64_t lmask);
static select_fn orig_select;
typedef void (*effects_fn)(uint8_t *out, const uint8_t *planes, const uint8_t *wa, const uint8_t *wb);
static effects_fn orig_effects;
typedef void (*blend_fn)(uint8_t *out, const uint8_t *masks, uint64_t a, uint64_t b);
static blend_fn orig_blend;
typedef void (*prio_fn)(uint8_t *eng, uint8_t *vis, uint8_t *excl);
static prio_fn orig_prio;
typedef void (*conv1_fn)(const uint8_t *a, uint32_t *out);
static conv1_fn orig_conv1;
typedef void (*span4_fn)(uint16_t *dst, uint8_t *rows, const uint16_t *pal, const uint8_t *tiles, const uint16_t *entries,
                         const uint8_t *flags);
/* DraStic's span_4bpp: its constant and its fifth instruction, for ds2d_span4_orig (ds2d_asm.S) */
__attribute__((visibility("hidden"))) uintptr_t ds2d_span4_consts, ds2d_span4_cont;

/* ---- check mode ---- */
typedef struct { uint8_t *p; size_t n; } rgn;
#define CHK_PAD 64
#define CHK_BYTES 0x8000
static pthread_mutex_t chk_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t chk_in[CHK_BYTES], chk_ours[CHK_BYTES], chk_ref[CHK_BYTES];
static unsigned long chk_n[H_N], chk_bad[H_N];
static struct timespec chk_last;
static const char *hname[H_N] = { "color_convert_direct_32_2x", "select_pixels", "color_effects_apply", "select_blend_enable", "priority_encode_single",
                                    "color_convert_direct_32_1x", "tiled_span_4bpp" };
static int ds2d_check, ds2d_mask;

/* a region with up to CHK_PAD bytes either side, as far as the pages of its first and last byte go (always mapped) */
static rgn padded(void *p, size_t n) {
    uintptr_t a = (uintptr_t)p, e = a + n, lo = a - CHK_PAD, hi = e + CHK_PAD;
    uintptr_t plo = a & ~(uintptr_t)4095, phi = ((e - 1) | 4095) + 1;
    if (lo < plo || lo > a) lo = plo;
    if (hi > phi || hi < e) hi = phi;
    return (rgn){ (uint8_t *)lo, hi - lo };
}
static size_t rg_save(const rgn *r, int nr, uint8_t *buf) {
    size_t o = 0;
    for (int i = 0; i < nr; i++) { memcpy(buf + o, r[i].p, r[i].n); o += r[i].n; }
    return o;
}
/* (overlapping regions: every copy of a byte holds the same value, so the order does not matter) */
static void rg_load(const rgn *r, int nr, const uint8_t *buf) {
    size_t o = 0;
    for (int i = 0; i < nr; i++) { memcpy(r[i].p, buf + o, r[i].n); o += r[i].n; }
}

static void chk_report(void) {                      /* under chk_lock, every 2 s */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - chk_last.tv_sec < 2) return;
    chk_last = now;
    fprintf(stderr, "[ds2d] check:");
    for (int h = 0; h < H_N; h++) fprintf(stderr, " %s %lu calls, %lu differ;", hname[h], chk_n[h], chk_bad[h]);
    fprintf(stderr, "\n");
}

/* after ours ran with chk_in saved and chk_ours holding its result, and the original with the same input bytes:
 * compare, log, and put our result back. Returns 1 when they differ. Under chk_lock. */
static int chk_finish(int h, const rgn *r, int nr, size_t total) {
    rg_save(r, nr, chk_ref);
    int bad = memcmp(chk_ref, chk_ours, total) != 0;
    chk_n[h]++;
    if (bad && chk_bad[h]++ < 30) {
        size_t o = 0;
        for (int i = 0; i < nr; i++, o += r[i - 1].n)
            for (size_t j = 0; j < r[i].n; j++)
                if (chk_ref[o + j] != chk_ours[o + j]) {
                    fprintf(stderr, "[ds2dcheck] %s: region %d (%p, %zu bytes) byte %zu: DraStic %02x, ours %02x\n",
                            hname[h], i, (void *)r[i].p, r[i].n, j, chk_ref[o + j], chk_ours[o + j]);
                    break;
                }
    }
    rg_load(r, nr, chk_ours);
    chk_report();
    return bad;
}

/* ---- H_CONV ---- */
static void chk_conv(const uint8_t *a, const uint8_t *b, uint32_t *out) {
    rgn r[3] = { padded(out, 2048), { (uint8_t *)a, 0x300 }, { (uint8_t *)b, 0x300 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ds2d_conv32_2x(a, b, out);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    orig_conv(a, b, out);
    chk_finish(H_CONV, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_CONV1X ---- */
static void chk_conv1(const uint8_t *a, uint32_t *out) {
    rgn r[2] = { padded(out, 1024), { (uint8_t *)a, 0x300 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_conv32_1x(a, out);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    orig_conv1(a, out);
    chk_finish(H_CONV1X, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_SPAN4 ---- */
static void chk_span4(uint16_t *dst, uint8_t *rows, const uint16_t *pal, const uint8_t *tiles, const uint16_t *entries,
                      const uint8_t *flags) {
    rgn r[2] = { padded(dst, 33 * 16), padded(rows, 136) };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_span4(dst, rows, pal, tiles, entries, flags);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    ds2d_span4_orig(dst, rows, pal, tiles, entries, flags);
    chk_finish(H_SPAN4, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_SELECT: render_scanline_select_pixels ---- */
static inline int ov(const void *a, size_t an, const void *b, size_t bn) {
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}

/* 1 when an output buffer overlaps another buffer of the call: the original's passes (each reads a 32-pixel block's
 * inputs before it stores the block) would then see what an earlier pass wrote, which the fused pass does not. */
static int sel_overlap(const uint8_t *eng, const uint8_t *out, const uint8_t *excl, uint8_t **layers,
                       const uint32_t *p3d, const uint8_t *alpha, uint32_t lmask) {
    const uint16_t *bd = *(const uint16_t *const *)(eng + 0x18);
    int a3 = p3d && (lmask & 1) && alpha;                /* alpha is written */
    if (ov(out, 0x300, bd, 2) || ov(out, 0x300, excl, 0xc0)) return 1;
    if (a3 && (ov(alpha, 0x100, out, 0x300) || ov(alpha, 0x100, bd, 2) || ov(alpha, 0x100, excl, 0x20) || ov(alpha, 0x100, p3d, 0x400))) return 1;
    if (p3d && (lmask & 1) && ov(out, 0x300, p3d, 0x400)) return 1;
    unsigned k = 0;
    for (uint32_t m = lmask; m; m >>= 1, k++) {
        if (!(m & 1)) continue;
        if (ov(out, 0x300, excl + 32 * k, 32) || ov(out, 0x300, layers[k] + 0x10, 0x200)) return 1;
        if (a3 && (ov(alpha, 0x100, excl + 32 * k, 32) || ov(alpha, 0x100, layers[k] + 0x10, 0x200))) return 1;
    }
    return 0;
}

/* a q register load as ldr q at base + a constant offset (clang pairs adjacent vld1q into ldp q, which llvm-mca's
 * A55 model holds the load pipe 6 cycles for, against 1 for each ldr q; and a plain "m" operand would put every
 * address in a register of its own, one integer add ahead of the load) */
#define LDQ(base, off) __extension__({                                                                         \
        uint8x16_t v_;                                                                                        \
        __asm__("ldr %q0, [%1, #%2]" : "=w"(v_)                                                               \
                : "r"(base), "n"(off), "m"(*(const uint8_t (*)[16])((const uint8_t *)(base) + (off))));       \
        v_; })
#define LDQ16(base, off) vreinterpretq_u16_u8(LDQ(base, off))
/* cmtst as one instruction (clang turns vtstq into and + cmeq #0 and then selects with the inverted mask) */
static inline uint16x8_t tst16(uint16x8_t a, uint16x8_t b) {
    uint16x8_t r;
    __asm__("cmtst %0.8h, %1.8h, %2.8h" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
static inline uint8x16_t tst8(uint8x16_t a, uint8x16_t b) {
    uint8x16_t r;
    __asm__("cmtst %0.16b, %1.16b, %2.16b" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

/* render_scanline_select_pixels(eng, out, excl, layers, p3d, alpha, lmask), per 32 pixels in registers (see the top;
 * spec/composite.c has the original's passes). lmask arrives in w6 (the upper half undefined), cut to 32 bits as
 * the original reads it. Per pixel i, with the set bits k0 < k1 < ... of lmask and m_k(i) = bit i of excl + 32k:
 *     c = layer k0's u16 [i]; for each later k: m_k(i) ? layer k [i] : c; then m_5(i) (excl + 0xa0) ? backdrop : c;
 *     (no bit set: the backdrop) -> R = (c & 0x1f) << 1, G = ((c >> 5) & 0x1f) << 1, B = ((c >> 10) & 0x1f) << 1;
 *     p3d && lmask & 1 && m_0(i): R, G, B = bytes 0, 1, 2 of p3d[i] and, alpha != NULL, alpha[i] = byte 3.
 * Writes out[0..0x2ff] and, in the last case, alpha[i] where m_0(i) is set (the original rewrites the other alpha
 * bytes with their own values). */
/* the steps of sel_fused(), specialised by the caller for the number of layers (NL = 1..5, so the merge loop unrolls
 * with its pointers in registers; 0: n at run time) and for the 3D bytes (HAS3D; m3 is then non-NULL) */
static inline __attribute__((always_inline)) void sel_steps(uint8_t *out, const uint8_t *excl, const uint16_t *const *lp,
                                                            const uint8_t *const *mp, int n, uint16x8_t bd,
                                                            const uint32_t *p3d, const uint8_t *m3, uint8_t *alpha,
                                                            const int NL, const int HAS3D) {
    static const uint16_t bl16[8] = { 1, 2, 4, 8, 16, 32, 64, 128 }, bh16[8] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
    static const uint8_t b8[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint16x8_t bl = vld1q_u16(bl16), bh = vld1q_u16(bh16);
    const uint8x16_t bb = vld1q_u8(b8), c3e = vdupq_n_u8(0x3e);
    (void)bb; (void)m3; (void)p3d; (void)alpha;
    /* 32 pixels a step, as two independent halves of 16 (the in-order core overlaps them); plain variables, not
     * arrays (clang kept small arrays on the stack) */
#define SEL4(mw, l0, l1, l2, l3) do {                                                                          \
        uint16x8_t m0_ = vdupq_n_u16((uint16_t)(mw)), m1_ = vdupq_n_u16((uint16_t)((mw) >> 16));             \
        c0 = vbslq_u16(tst16(m0_, bl), l0, c0); c2 = vbslq_u16(tst16(m1_, bl), l2, c2);                       \
        c1 = vbslq_u16(tst16(m0_, bh), l1, c1); c3 = vbslq_u16(tst16(m1_, bh), l3, c3);                       \
    } while (0)
#define PLANES(ca, cb, r, g, bb_) do {                                                                         \
        uint8x16_t a_ = vreinterpretq_u8_u16(ca), b_ = vreinterpretq_u8_u16(cb);                              \
        r = vandq_u8(vshlq_n_u8(vuzp1q_u8(a_, b_), 1), c3e);                                                 \
        g = vandq_u8(vcombine_u8(vshrn_n_u16(ca, 4), vshrn_n_u16(cb, 4)), c3e);                               \
        bb_ = vandq_u8(vshrq_n_u8(vuzp2q_u8(a_, b_), 1), c3e);                                               \
    } while (0)
    /* the 16 pixels' bytes 0..3 by two levels of uzp (ld4 holds the load pipe far longer) where mask mh is set */
#define PX3D(mh, i0, r, g, bb_) do {                                                                           \
        uint8x16_t mb_ = tst8(vcombine_u8(vdup_n_u8((uint8_t)(mh)), vdup_n_u8((uint8_t)((mh) >> 8))), bb);      \
        const uint32_t *pp_ = p3d + (i0);                                                                     \
        uint8x16_t q0_ = LDQ(pp_, 0), q1_ = LDQ(pp_, 16), q2_ = LDQ(pp_, 32), q3_ = LDQ(pp_, 48);               \
        uint8x16_t e01_ = vuzp1q_u8(q0_, q1_), o01_ = vuzp2q_u8(q0_, q1_);                                    \
        uint8x16_t e23_ = vuzp1q_u8(q2_, q3_), o23_ = vuzp2q_u8(q2_, q3_);                                    \
        r = vbslq_u8(mb_, vuzp1q_u8(e01_, e23_), r);                                                          \
        g = vbslq_u8(mb_, vuzp1q_u8(o01_, o23_), g);                                                          \
        bb_ = vbslq_u8(mb_, vuzp2q_u8(e01_, e23_), bb_);                                                      \
        if (alpha) vst1q_u8(alpha + (i0), vbslq_u8(mb_, vuzp2q_u8(o01_, o23_), LDQ(alpha + (i0), 0)));          \
    } while (0)
    for (int b = 0; b < 8; b++) {
        uint16x8_t c0, c1, c2, c3;
        if (NL || n) {
            const uint16_t *l = lp[0] + 32 * b;
            c0 = LDQ16(l, 0); c1 = LDQ16(l, 16); c2 = LDQ16(l, 32); c3 = LDQ16(l, 48);
            for (int j = 1; j < (NL ? NL : n); j++) {
                uint32_t mw; memcpy(&mw, mp[j] + 4 * b, 4);
                if (!mw) continue;
                const uint16_t *lj = lp[j] + 32 * b;
                SEL4(mw, LDQ16(lj, 0), LDQ16(lj, 16), LDQ16(lj, 32), LDQ16(lj, 48));
            }
            uint32_t mw; memcpy(&mw, excl + 0xa0 + 4 * b, 4);
            if (mw) SEL4(mw, bd, bd, bd, bd);
        } else c0 = c1 = c2 = c3 = bd;
        uint8x16_t r0, g0, b0, r1, g1, b1;
        PLANES(c0, c1, r0, g0, b0);
        PLANES(c2, c3, r1, g1, b1);
        if (HAS3D) {
            uint32_t mw; memcpy(&mw, m3 + 4 * b, 4);
            if (mw & 0xffff) PX3D(mw, 32 * b, r0, g0, b0);
            if (mw >> 16) PX3D(mw >> 16, 32 * b + 16, r1, g1, b1);
        }
        vst1q_u8(out + 32 * b, r0);
        vst1q_u8(out + 32 * b + 16, r1);
        vst1q_u8(out + 0x100 + 32 * b, g0);
        vst1q_u8(out + 0x100 + 32 * b + 16, g1);
        vst1q_u8(out + 0x200 + 32 * b, b0);
        vst1q_u8(out + 0x200 + 32 * b + 16, b1);
    }
#undef SEL4
#undef PLANES
#undef PX3D
}

static void sel_fused(uint8_t *eng, uint8_t *out, const uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                      uint8_t *alpha, uint32_t lmask) {
    const uint16_t *lp[32];
    const uint8_t *mp[32];
    int n = 0;
    unsigned k = 0;
    for (uint32_t m = lmask; m; m >>= 1, k++)
        if (m & 1) { lp[n] = (const uint16_t *)(layers[k] + 0x10); mp[n] = excl + 32 * k; n++; }
    const uint16x8_t bd = vdupq_n_u16(**(const uint16_t *const *)(eng + 0x18));
    const uint8_t *m3 = p3d && (lmask & 1) ? excl : 0;
#define SEL_CASE(k) case k:                                                                                    \
        if (m3) sel_steps(out, excl, lp, mp, n, bd, p3d, m3, alpha, k, 1);                                       \
        else sel_steps(out, excl, lp, mp, n, bd, p3d, m3, alpha, k, 0);                                          \
        break;
    switch (n) {
    SEL_CASE(1) SEL_CASE(2) SEL_CASE(3) SEL_CASE(4) SEL_CASE(5)
    default:                                                    /* no layer (the backdrop), or bits above 4 */
        if (m3) sel_steps(out, excl, lp, mp, n, bd, p3d, m3, alpha, 0, 1);
        else sel_steps(out, excl, lp, mp, n, bd, p3d, m3, alpha, 0, 0);
    }
#undef SEL_CASE
}

void ds2d_select(uint8_t *eng, uint8_t *out, const uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                 uint8_t *alpha, uint64_t lmask) {
    if (__builtin_expect(sel_overlap(eng, out, excl, layers, p3d, alpha, (uint32_t)lmask), 0)) {
        orig_select(eng, out, excl, layers, p3d, alpha, lmask);
        return;
    }
    sel_fused(eng, out, excl, layers, p3d, alpha, (uint32_t)lmask);
}

static void chk_select(uint8_t *eng, uint8_t *out, const uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                       uint8_t *alpha, uint64_t lmask) {
    rgn r[2] = { padded(out, 0x300), { 0, 0 } };
    int nr = 1;
    if (alpha) r[nr++] = padded(alpha, 0x100);
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, nr, chk_in);
    ds2d_select(eng, out, excl, layers, p3d, alpha, lmask);
    rg_save(r, nr, chk_ours);
    rg_load(r, nr, chk_in);
    orig_select(eng, out, excl, layers, p3d, alpha, lmask);
    chk_finish(H_SELECT, r, nr, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_EFFECTS ---- */
static void chk_effects(uint8_t *out, const uint8_t *planes, const uint8_t *wa, const uint8_t *wb) {
    rgn r[4] = { padded(out, 0x300), { (uint8_t *)planes, 0x600 }, { (uint8_t *)wa, 0x100 }, { (uint8_t *)wb, 0x100 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 4, chk_in);
    ds2d_effects_apply(out, planes, wa, wb);
    rg_save(r, 4, chk_ours);
    rg_load(r, 4, chk_in);
    orig_effects(out, planes, wa, wb);
    chk_finish(H_EFFECTS, r, 4, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_BLEND: render_scanline_select_blend_enable_asm(out[32], masks, a, b) ----
 * masks: the priority encoder's six 256-bit masks (BG0..BG3, OBJ, backdrop at +0xa0); a, b: layer bit sets (w2, w3;
 * the original dups their low bytes and tests bit k of each). With acc = 0, for k = 0..4: bit k of a and of b set:
 * acc |= mask k; of a set, of b clear: acc &= ~mask k; else unchanged. Then bit 5 of b ? acc |= backdrop mask :
 * acc &= ~backdrop mask (a's bit 5 is not consulted). out = acc (32 bytes, stored after every mask is read, as the
 * original's single st1 at its end). */
void ds2d_blend_enable(uint8_t *out, const uint8_t *m, uint64_t a, uint64_t b) {
    uint8x16_t acc0 = vdupq_n_u8(0), acc1 = acc0;
    for (int k = 0; k < 5; k++) {
        if (!(a >> k & 1)) continue;
        const uint8_t *mk = m + 32 * k;
        uint8x16_t m0 = LDQ(mk, 0), m1 = LDQ(mk, 16);
        if (b >> k & 1) { acc0 = vorrq_u8(acc0, m0); acc1 = vorrq_u8(acc1, m1); }
        else { acc0 = vbicq_u8(acc0, m0); acc1 = vbicq_u8(acc1, m1); }
    }
    uint8x16_t m0 = LDQ(m, 0xa0), m1 = LDQ(m, 0xb0);
    if (b >> 5 & 1) { acc0 = vorrq_u8(acc0, m0); acc1 = vorrq_u8(acc1, m1); }
    else { acc0 = vbicq_u8(acc0, m0); acc1 = vbicq_u8(acc1, m1); }
    vst1q_u8(out, acc0);
    vst1q_u8(out + 16, acc1);
}

static void chk_blend(uint8_t *out, const uint8_t *m, uint64_t a, uint64_t b) {
    rgn r[2] = { padded(out, 32), { (uint8_t *)m, 0xc0 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_blend_enable(out, m, a, b);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    orig_blend(out, m, a, b);
    chk_finish(H_BLEND, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_PRIO: render_scanline_priority_encode_single_asm(eng, vis, excl) ----
 * covered = objcovered = objex = 0 (256 bits each); for k = 0 .. [eng+0xb3]-1, s = [eng+0x84+k], o = s*32:
 *   s & 4 == 0 (BG):  excl[o..o+31] = vis[o..] & ~covered;  covered |= vis[o..]
 *   s & 4 != 0 (OBJ): v = vis[o..] & ~objcovered; objex |= v & ~covered; covered |= v; objcovered |= v
 * then excl+0xa0 = ~covered, excl+0x80 = objex (spec/composite.c). Per layer the loads come before its store, in the
 * list's order, as the original's (so the same bytes for any overlap). */
void ds2d_prio_single(uint8_t *eng, uint8_t *vis, uint8_t *excl) {
    unsigned n = eng[0xb3];
    const uint8_t *list = eng + 0x84;
    uint8x16_t c0 = vdupq_n_u8(0), c1 = c0, oc0 = c0, oc1 = c0, ox0 = c0, ox1 = c0;
    for (unsigned k = 0; k < n; k++) {
        uint32_t o = (uint32_t)list[k] << 5;
        const uint8_t *v = vis + o;
        uint8x16_t v0 = LDQ(v, 0), v1 = LDQ(v, 16);
        if (!(o & 0x80)) {
            vst1q_u8(excl + o, vbicq_u8(v0, c0));
            vst1q_u8(excl + o + 16, vbicq_u8(v1, c1));
            c0 = vorrq_u8(c0, v0); c1 = vorrq_u8(c1, v1);
        } else {
            v0 = vbicq_u8(v0, oc0); v1 = vbicq_u8(v1, oc1);
            ox0 = vorrq_u8(ox0, vbicq_u8(v0, c0)); ox1 = vorrq_u8(ox1, vbicq_u8(v1, c1));
            c0 = vorrq_u8(c0, v0); c1 = vorrq_u8(c1, v1);
            oc0 = vorrq_u8(oc0, v0); oc1 = vorrq_u8(oc1, v1);
        }
    }
    vst1q_u8(excl + 0xa0, vmvnq_u8(c0));
    vst1q_u8(excl + 0xb0, vmvnq_u8(c1));
    vst1q_u8(excl + 0x80, ox0);
    vst1q_u8(excl + 0x90, ox1);
}

static void chk_prio(uint8_t *eng, uint8_t *vis, uint8_t *excl) {
    /* the list's slots address vis and excl at up to 255 * 32 + 32 bytes */
    unsigned n = eng[0xb3], hi = 0xc0;
    for (unsigned k = 0; k < n; k++) if (eng[0x84 + k] * 32u + 32 > hi) hi = eng[0x84 + k] * 32u + 32;
    rgn r[3] = { padded(excl, hi), { vis, hi }, { eng, 0x100 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ds2d_prio_single(eng, vis, excl);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    orig_prio(eng, vis, excl);
    chk_finish(H_PRIO, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- install ---- */
static const uint32_t expect_conv[4] = { 0x91040004, 0x91040025, 0x91080006, 0x91080027 };
static const uint32_t expect_select[4] = { 0xd10a03ff, 0xa9007bfd, 0x910003fd, 0xa9025bf5 };
static const uint32_t expect_effects[4] = { 0x4f01e7f9, 0x91040004, 0x91080005, 0x91040026 };
static const uint32_t expect_blend[4] = { 0x4f00e434, 0x528000a4, 0x4e010c42, 0x4f00e400 };
static const uint32_t expect_prio[4] = { 0x3942cc06, 0x91021007, 0x4f000400, 0x4f000401 };
static const uint32_t expect_conv1[4] = { 0x91040003, 0x91080004, 0x4f07e7e3, 0x4f07e7e7 };
static const uint32_t expect_span4[4] = { 0x10ffff87, 0x52800406, 0x4c4078e0, 0x4f00e5e1 };

void ds2d_init(void) {
    const char *e = getenv("DSFLIP_RAST_2D");
    if (!e) e = getenv("RAST_2D");
    ds2d_mask = e ? (int)strtol(e, 0, 0) : -1;
    ds2d_check = getenv("RAST_2DCHECK") && atoi(getenv("RAST_2DCHECK"));
    if (!ds2d_mask) return;
    clock_gettime(CLOCK_MONOTONIC, &chk_last);
    if (ds2d_mask & (1 << H_CONV)) {
        orig_conv = (conv_fn)rast_hook(DS_CONV_32_2X, expect_conv, ds2d_check ? (void *)chk_conv : (void *)ds2d_conv32_2x);
        fprintf(stderr, orig_conv ? "[ds2d] color_convert_direct_32_2x replaced%s\n"
                                  : "[ds2d] unknown color_convert_direct_32_2x, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_SELECT)) {
        orig_select = (select_fn)rast_hook(DS_SELECT_PIXELS_2D, expect_select, ds2d_check ? (void *)chk_select : (void *)ds2d_select);
        fprintf(stderr, orig_select ? "[ds2d] select_pixels replaced (fused merge, expansion and 3D bytes)%s\n"
                                    : "[ds2d] unknown select_pixels, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_EFFECTS)) {
        orig_effects = (effects_fn)rast_hook(DS_EFFECTS_APPLY, expect_effects, ds2d_check ? (void *)chk_effects : (void *)ds2d_effects_apply);
        fprintf(stderr, orig_effects ? "[ds2d] color_effects_apply replaced%s\n" : "[ds2d] unknown color_effects_apply, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_BLEND)) {
        orig_blend = (blend_fn)rast_hook(DS_BLEND_ENABLE, expect_blend, ds2d_check ? (void *)chk_blend : (void *)ds2d_blend_enable);
        fprintf(stderr, orig_blend ? "[ds2d] select_blend_enable replaced%s\n" : "[ds2d] unknown select_blend_enable, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_PRIO)) {
        orig_prio = (prio_fn)rast_hook(DS_PRIO_SINGLE, expect_prio, ds2d_check ? (void *)chk_prio : (void *)ds2d_prio_single);
        fprintf(stderr, orig_prio ? "[ds2d] priority_encode_single replaced%s\n" : "[ds2d] unknown priority_encode_single, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_CONV1X)) {
        orig_conv1 = (conv1_fn)rast_hook(DS_CONV_32_1X, expect_conv1, ds2d_check ? (void *)chk_conv1 : (void *)ds2d_conv32_1x);
        fprintf(stderr, orig_conv1 ? "[ds2d] color_convert_direct_32_1x replaced%s\n" : "[ds2d] unknown color_convert_direct_32_1x, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
    if (ds2d_mask & (1 << H_SPAN4)) {
        /* the trampoline rast_hook() returns is unusable here (an adr in the copied words): the check mode calls
         * ds2d_span4_orig instead */
        ds2d_span4_consts = ds_base + DS_SPAN4_CONSTS;
        ds2d_span4_cont = ds_base + DS_SPAN4 + 16;
        int ok = rast_hook(DS_SPAN4, expect_span4, ds2d_check ? (void *)chk_span4 : (void *)ds2d_span4) != 0;
        fprintf(stderr, ok ? "[ds2d] tiled_span_4bpp replaced%s\n" : "[ds2d] unknown tiled_span_4bpp, not hooked%s\n",
                ds2d_check ? ", checking every call against DraStic's" : "");
    }
}
