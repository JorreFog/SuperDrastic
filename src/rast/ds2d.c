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
 *   H_PAL8   render_scanline_palette_lookup_8bpp_asm (0x9f504: 8bpp indexes to u16 colours, 8 a step) -> ds2d_pal8
 *           (ds2d_asm.S): software pipelined over two register sets, 16 modeled cycles a step against 26. Callers:
 *           render_scanline_tiled_ext (the 8bpp text BG line) and the affine BG renderers (C, bl). Its loop
 *           re-enters its second instruction (b.gt at +0x68 to +0x4), so the check mode runs a private copy of the
 *           whole original (copy_orig, 112 bytes: register code, that one branch inside the copy, no literal or
 *           pc-relative address).
 *   H_PAL12  render_scanline_palette_lookup_12bpp_asm (0x9f574: the extended palettes' 12-bit indexes, in place)
 *           -> ds2d_pal12, the same way (19 against 28; its copy: 116 bytes, b.gt at +0x6c to +0x4). Callers:
 *           tiled_ext, affine_extended_ext (C).
 *   H_PRIO2  render_scanline_priority_encode_double_asm (0x9fe98: the single encoder with a second output, the layer
 *           below the top one, for the effects path) -> ds2d_prio_double(): as ds2d_prio_single. Caller:
 *           render_scanline_2d_composite (C).
 *   H_ALPHA_BASE, H_ALPHA, H_BLEND_BASE, H_BLEND_SET  render_scanline_color_effects_setup_alpha_base_asm (0xa042c),
 *           _setup_alpha_asm (0xa04b4), _setup_blend_base_asm (0xa0254), _setup_blend_asm (0xa02e0): the blend's
 *           per-pixel weight arrays (wa, wb: 256 bytes each) from the 3D alpha line or BLDALPHA where the enable
 *           mask is set -> ds2d_setup_alpha_base() etc.: the same per 64-pixel block, from ldr q, the all-zero
 *           mask blocks shortcut. Caller: render_scanline_2d_composite (C).
 *   H_SHADE  render_scanline_shade_asm (0xa0108: the brightness effect on three planar quarters where the mask is
 *           set) -> ds2d_shade(): the same umlal / shrn arithmetic per 32 pixels, the unmasked blocks copied. Caller:
 *           a tail branch from render_scanline_2d_composite (C).
 *   H_SPAN8N, H_SPAN8E  render_scanline_tiled_span_8bpp_normal_palette_asm (0x9f0f0) and _ext_palette_asm
 *           (0x9f188): an 8bpp text BG line's 33 tile rows (index bytes; with the palette number as the high byte
 *           for the extended palettes) -> ds2d_span8n() / ds2d_span8e(): the rows loaded straight into vector
 *           lanes instead of through general registers, the interleave by zip + str q. Caller: tiled_ext (C).
 *   H_TME4, H_TME8  render_scanline_tiled_setup_tile_map_entries_4bpp_asm (0x9ee70) / _8bpp_asm (0x9ef08): a text
 *           BG line's 40 map entries from tile column k, through DraStic's table of 32 map-row loaders, as tile-row
 *           byte offsets plus flag bytes -> ds2d_tme4() / ds2d_tme8(): the same rotation by ext, without the indirect
 *           call and the stores with writeback. Caller: tiled_ext (C).
 *   H_DBL    render_scanline_disable_blank_layers_asm (0xa089c: clears the enable bits of BGs whose visibility bitmap
 *           is empty) -> ds2d_disable_blank(). Caller: render_scanline_2d (C), twice a line.
 *   H_VIS4, H_VIS8, H_VIS12  render_scanline_set_visibility_4bpp_asm (0x9f278), _8bpp_asm (0x9f390), _12bpp_asm
 *           (0x9f468): a layer line's 256-bit visibility bitmap from its nibbles / bytes / u16 indexes ->
 *           ds2d_vis4() etc.: the originals' instructions from ldr q loads. Callers: tiled_ext, the affine renderers (C).
 *
 * RAST_2D (or DSFLIP_RAST_2D): 0 leaves DraStic's functions alone; otherwise a mask of the hooks (default all but
 * the five that do not pay, DS2D_DEFAULT below; -1 all):
 * 1 H_CONV, 2 H_SELECT, 4 H_EFFECTS, 8 H_BLEND, 16 H_PRIO, 32 H_CONV1X, 64 H_SPAN4, 128 H_PAL8, 256 H_PAL12,
 * 512 H_PRIO2, 1024 H_ALPHA_BASE, 2048 H_ALPHA, 4096 H_BLEND_BASE, 8192 H_BLEND_SET, 16384 H_SHADE, 32768 H_SPAN8N,
 * 65536 H_SPAN8E, 0x20000 H_TME4, 0x40000 H_TME8, 0x80000 H_DBL, 0x100000 H_VIS4, 0x200000 H_VIS8, 0x400000 H_VIS12.
 * RAST_2DCHECK=1: every call of a hooked function runs ours, then DraStic's original (through the
 * trampoline rast_hook() returns; through an ORIG4 stub of ds2d_asm.S for the entries that start with an adr, as
 * ds2d_span4_orig; through a private copy of the whole routine for the two palette lookups, whose loops re-enter the
 * patched entry: copy_orig(), check mode only, both routines position independent in every instruction) on the same
 * input bytes (every region either may write, and the inputs of the routines whose outputs could reach them, is
 * saved before ours and restored before the original), and compares those regions afterwards (the outputs with up
 * to 64 bytes either side, within their pages); ours is left in place. "[ds2d] check:" counts every 2 s,
 * "[ds2dcheck]" lines name the first differing byte. The 2D routines run on the emulation thread only; the check's
 * buffers are under a mutex all the same. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <arm_neon.h>
#include "rast.h"
#include "ds2d.h"

enum { H_CONV, H_SELECT, H_EFFECTS, H_BLEND, H_PRIO, H_CONV1X, H_SPAN4, H_PAL8, H_PAL12, H_PRIO2, H_ALPHA_BASE, H_ALPHA,
       H_BLEND_BASE, H_BLEND_SET, H_SHADE, H_SPAN8N, H_SPAN8E, H_TME4, H_TME8, H_DBL, H_VIS4, H_VIS8, H_VIS12, H_N };

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
/* the same for the ORIG4 stubs of ds2d_asm.S: { the adr's target, the fifth instruction } per stub */
__attribute__((visibility("hidden"))) uintptr_t ds2d_orig_vars[9][2];
typedef void (*dbl_fn)(const uint8_t *masks, uint8_t *word);
static dbl_fn orig_dbl;
typedef void (*pal8_fn)(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n);
typedef void (*pal12_fn)(uint16_t *dst, const uint16_t *src, const uint16_t *pal, int n);
static pal8_fn orig_pal8;                  /* private copies of the originals (see copy_orig) */
static pal12_fn orig_pal12;
typedef void (*prio2_fn)(uint8_t *eng, uint8_t *vis, uint8_t *excl, uint8_t *excl2);
static prio2_fn orig_prio2;
typedef void (*alpha_fn)(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask);
typedef void (*blendset_fn)(uint32_t bldalpha, uint8_t *wa, uint8_t *wb, const uint8_t *mask);
static blendset_fn orig_blend_base, orig_blend_set;
typedef void (*shade_fn)(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask);
static shade_fn orig_shade;

/* ---- check mode ---- */
typedef struct { uint8_t *p; size_t n; } rgn;
#define CHK_PAD 64
#define CHK_BYTES 0x8000
static pthread_mutex_t chk_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t chk_in[CHK_BYTES], chk_ours[CHK_BYTES], chk_ref[CHK_BYTES];
static unsigned long chk_n[H_N], chk_bad[H_N];
static struct timespec chk_last;
static const char *hname[H_N] = { "color_convert_direct_32_2x", "select_pixels", "color_effects_apply", "select_blend_enable", "priority_encode_single",
                                    "color_convert_direct_32_1x", "tiled_span_4bpp", "palette_lookup_8bpp", "palette_lookup_12bpp",
                                    "priority_encode_double", "effects_setup_alpha_base", "effects_setup_alpha", "effects_setup_blend_base",
                                    "effects_setup_blend", "shade", "tiled_span_8bpp_normal", "tiled_span_8bpp_ext",
                                    "tile_map_entries_4bpp", "tile_map_entries_8bpp", "disable_blank_layers", "set_visibility_4bpp",
                                    "set_visibility_8bpp", "set_visibility_12bpp" };
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
/* m ? a : b per bit as one bit instruction (clang turns vbslq with a compare's mask into and + bic + orr). bit's
 * operands are (destination = the else value, the then value, the mask), the mask last, unlike vbslq's. */
static inline uint8x16_t sel8(uint8x16_t m, uint8x16_t a, uint8x16_t b) {
    __asm__("bit %0.16b, %1.16b, %2.16b" : "+w"(b) : "w"(a), "w"(m));
    return b;
}
static inline uint16x8_t sel16(uint16x8_t m, uint16x8_t a, uint16x8_t b) {
    return vreinterpretq_u16_u8(sel8(vreinterpretq_u8_u16(m), vreinterpretq_u8_u16(a), vreinterpretq_u8_u16(b)));
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
        c0 = sel16(tst16(m0_, bl), l0, c0); c2 = sel16(tst16(m1_, bl), l2, c2);                               \
        c1 = sel16(tst16(m0_, bh), l1, c1); c3 = sel16(tst16(m1_, bh), l3, c3);                               \
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
        r = sel8(mb_, vuzp1q_u8(e01_, e23_), r);                                                              \
        g = sel8(mb_, vuzp1q_u8(o01_, o23_), g);                                                              \
        bb_ = sel8(mb_, vuzp2q_u8(e01_, e23_), bb_);                                                          \
        if (alpha) vst1q_u8(alpha + (i0), sel8(mb_, vuzp2q_u8(o01_, o23_), LDQ(alpha + (i0), 0)));              \
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


/* ---- H_PAL8, H_PAL12: the palette lookups (ds2d_asm.S) ---- */
/* DraStic's iteration count: n -= 8, another iteration while the result is > 0 */
static inline int pal_iters(int n) { return n <= 8 ? 1 : (n + 7) >> 3; }

/* a private executable copy of one of DraStic's routines, for the check mode: palette_lookup_8bpp / _12bpp loop
 * back into their second instruction, which the patched entry has overwritten, so the trampoline cannot run them;
 * both are position independent (register code and a branch within). Taken before the entry is patched. */
static void *copy_orig(uintptr_t off, size_t len) {
    static uint8_t *page; static size_t used;
    if (!page) page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || used + len > 4096) return 0;
    uint8_t *p = page + used;
    used += (len + 15) & ~(size_t)15;
    memcpy(p, (const void *)(ds_base + off), len);
    __builtin___clear_cache((char *)p, (char *)(p + len));
    return p;
}

static void chk_pal8(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n) {
    size_t it = pal_iters(n);
    rgn r[2] = { padded(dst, 16 * it), { (uint8_t *)src, 8 * it } };     /* the palette is only read */
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_pal8(dst, src, pal, n);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    orig_pal8(dst, src, pal, n);
    chk_finish(H_PAL8, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_pal12(uint16_t *dst, const uint16_t *src, const uint16_t *pal, int n) {
    size_t it = pal_iters(n);
    rgn r[2] = { padded(dst, 16 * it), { (uint8_t *)src, 16 * it } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_pal12(dst, src, pal, n);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    orig_pal12(dst, src, pal, n);
    chk_finish(H_PAL12, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_PRIO2: render_scanline_priority_encode_double_asm(eng, vis, excl, excl2) ----
 * The single encoder's walk (ds2d_prio_single) with a second output: per listed layer k, s = [eng+0x84+k], o = s*32,
 * with covered c, covered-twice d, objcovered oc, objex ox, objex2 oy all 0 at the start:
 *   BG:  excl[o] = v & ~c;  excl2[o] = v & ~d & c;  d |= c & v;  c |= v           (v = vis[o..o+31])
 *   OBJ: v = vis[o] & ~oc;  ox |= v & ~c;  oy |= v & ~d & c;  d |= c & v;  c |= v;  oc |= v
 * then excl+0xa0 = ~c, excl2+0xa0 = c & ~d, excl+0x80 = ox, excl2+0x80 = oy (that order). excl2 is the layer
 * below the top one (the blend's second target). Caller: render_scanline_2d_composite (C), the effects path. */
void ds2d_prio_double(uint8_t *eng, uint8_t *vis, uint8_t *ea, uint8_t *eb) {
    unsigned n = eng[0xb3];
    const uint8_t *list = eng + 0x84;
    uint8x16_t z = vdupq_n_u8(0), c0 = z, c1 = z, d0 = z, d1 = z, oc0 = z, oc1 = z, ox0 = z, ox1 = z, oy0 = z, oy1 = z;
    for (unsigned k = 0; k < n; k++) {
        uint32_t o = (uint32_t)list[k] << 5;
        const uint8_t *v = vis + o;
        uint8x16_t v0 = LDQ(v, 0), v1 = LDQ(v, 16);
        if (!(o & 0x80)) {
            vst1q_u8(ea + o, vbicq_u8(v0, c0));
            vst1q_u8(ea + o + 16, vbicq_u8(v1, c1));
            vst1q_u8(eb + o, vandq_u8(vbicq_u8(v0, d0), c0));
            vst1q_u8(eb + o + 16, vandq_u8(vbicq_u8(v1, d1), c1));
        } else {
            v0 = vbicq_u8(v0, oc0); v1 = vbicq_u8(v1, oc1);
            ox0 = vorrq_u8(ox0, vbicq_u8(v0, c0)); ox1 = vorrq_u8(ox1, vbicq_u8(v1, c1));
            oy0 = vorrq_u8(oy0, vandq_u8(vbicq_u8(v0, d0), c0)); oy1 = vorrq_u8(oy1, vandq_u8(vbicq_u8(v1, d1), c1));
            oc0 = vorrq_u8(oc0, v0); oc1 = vorrq_u8(oc1, v1);
        }
        d0 = vorrq_u8(d0, vandq_u8(c0, v0)); d1 = vorrq_u8(d1, vandq_u8(c1, v1));
        c0 = vorrq_u8(c0, v0); c1 = vorrq_u8(c1, v1);
    }
    vst1q_u8(ea + 0xa0, vmvnq_u8(c0));
    vst1q_u8(ea + 0xb0, vmvnq_u8(c1));
    vst1q_u8(eb + 0xa0, vbicq_u8(c0, d0));
    vst1q_u8(eb + 0xb0, vbicq_u8(c1, d1));
    vst1q_u8(ea + 0x80, ox0);
    vst1q_u8(ea + 0x90, ox1);
    vst1q_u8(eb + 0x80, oy0);
    vst1q_u8(eb + 0x90, oy1);
}

static void chk_prio2(uint8_t *eng, uint8_t *vis, uint8_t *ea, uint8_t *eb) {
    unsigned n = eng[0xb3], hi = 0xc0;
    for (unsigned k = 0; k < n; k++) if (eng[0x84 + k] * 32u + 32 > hi) hi = eng[0x84 + k] * 32u + 32;
    rgn r[4] = { padded(ea, hi), padded(eb, hi), { vis, hi }, { eng, 0x100 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 4, chk_in);
    ds2d_prio_double(eng, vis, ea, eb);
    rg_save(r, 4, chk_ours);
    rg_load(r, 4, chk_in);
    orig_prio2(eng, vis, ea, eb);
    chk_finish(H_PRIO2, r, 4, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- the colour effects' per-pixel weights ----
 * The masks are the compositor's 256-bit bitmaps (bit i = pixel i); the originals expand a u16 of one (ld4r / ld2r,
 * tbl to bytes, cmtst with 1, 2, 4, ... 128) into 16 enable bytes; here a block's mask bytes are loaded by one ld1r
 * (replicated) and each 16 enable bytes made by tbl + cmtst: EN_BLOCK. Each original loads a block's inputs (64
 * pixels, 32 for the shade; its mask bytes included) and then stores it, so a block is handled the same way here
 * (the callers' buffers do not alias, but a block's order is kept all the same: the unit test aliases them). */
static const uint8_t en_bits[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
static const uint8_t en_idx[4][16] = { { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 }, { 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3 },
                                       { 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5 }, { 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7 } };
/* mask bytes p[0..7] (64 pixels) or p[0..3] (32: S = "4s") replicated over a q register by one ld1r */
#define LD1R(p, S) __extension__({                                                                             \
        uint8x16_t v_;                                                                                        \
        __asm__("ld1r {%0." S "}, [%1]" : "=w"(v_) : "r"(p), "m"(*(const uint8_t (*)[8])(p)));              \
        v_; })
#define EN16(t, k) tst8(vqtbl1q_u8(t, vld1q_u8(en_idx[k])), vld1q_u8(en_bits))

/* render_scanline_color_effects_setup_alpha_base_asm(wa, wb, alpha, mask): per pixel a = enabled ? alpha[i] : 31;
 * wa[i] = a + 1, wb[i] = 31 - a (bytes). The 3D layer's per-pixel alpha where it is the blend's top layer. A block
 * with no enabled pixel reads no alpha (the constants). */
void ds2d_setup_alpha_base(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask) {
    const uint8x16_t c1f = vdupq_n_u8(0x1f), c1 = vdupq_n_u8(1);
    for (int b = 0; b < 4; b++) {
        uint32_t m0, m1;
        memcpy(&m0, mask + 8 * b, 4); memcpy(&m1, mask + 8 * b + 4, 4);
        uint8_t *pa = wa + 64 * b, *pb = wb + 64 * b;
        if (!(m0 | m1)) {
            uint8x16_t a = vdupq_n_u8(0x20), z = vdupq_n_u8(0);
            vst1q_u8(pa, a); vst1q_u8(pa + 16, a); vst1q_u8(pa + 32, a); vst1q_u8(pa + 48, a);
            vst1q_u8(pb, z); vst1q_u8(pb + 16, z); vst1q_u8(pb + 32, z); vst1q_u8(pb + 48, z);
            continue;
        }
        const uint8_t *ap = alpha + 64 * b;
        uint8x16_t a0 = LDQ(ap, 0), a1 = LDQ(ap, 16), a2 = LDQ(ap, 32), a3 = LDQ(ap, 48);
        if (~(m0 & m1)) {                                 /* not all set: the select */
            uint8x16_t t = LD1R(mask + 8 * b, "2d");
            a0 = sel8(EN16(t, 0), a0, c1f); a1 = sel8(EN16(t, 1), a1, c1f);
            a2 = sel8(EN16(t, 2), a2, c1f); a3 = sel8(EN16(t, 3), a3, c1f);
        }
        vst1q_u8(pa, vaddq_u8(a0, c1)); vst1q_u8(pa + 16, vaddq_u8(a1, c1));
        vst1q_u8(pa + 32, vaddq_u8(a2, c1)); vst1q_u8(pa + 48, vaddq_u8(a3, c1));
        vst1q_u8(pb, vsubq_u8(c1f, a0)); vst1q_u8(pb + 16, vsubq_u8(c1f, a1));
        vst1q_u8(pb + 32, vsubq_u8(c1f, a2)); vst1q_u8(pb + 48, vsubq_u8(c1f, a3));
    }
}

/* render_scanline_color_effects_setup_alpha_asm(wa, wb, alpha, mask): the same over existing weights: enabled
 * pixels get alpha[i] + 1 and 31 - alpha[i], the others keep wa[i], wb[i] (a block with no enabled pixel is left). */
void ds2d_setup_alpha(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask) {
    const uint8x16_t c1f = vdupq_n_u8(0x1f), c1 = vdupq_n_u8(1);
    for (int b = 0; b < 4; b++) {
        uint32_t m0, m1;
        memcpy(&m0, mask + 8 * b, 4); memcpy(&m1, mask + 8 * b + 4, 4);
        if (!(m0 | m1)) continue;
        uint8_t *pa = wa + 64 * b, *pb = wb + 64 * b;
        const uint8_t *ap = alpha + 64 * b;
        uint8x16_t a0 = LDQ(ap, 0), a1 = LDQ(ap, 16), a2 = LDQ(ap, 32), a3 = LDQ(ap, 48);
        if (!~(m0 & m1)) {                                /* all set: the new weights only */
            vst1q_u8(pa, vaddq_u8(a0, c1)); vst1q_u8(pa + 16, vaddq_u8(a1, c1));
            vst1q_u8(pa + 32, vaddq_u8(a2, c1)); vst1q_u8(pa + 48, vaddq_u8(a3, c1));
            vst1q_u8(pb, vsubq_u8(c1f, a0)); vst1q_u8(pb + 16, vsubq_u8(c1f, a1));
            vst1q_u8(pb + 32, vsubq_u8(c1f, a2)); vst1q_u8(pb + 48, vsubq_u8(c1f, a3));
            continue;
        }
        uint8x16_t t = LD1R(mask + 8 * b, "2d");
        uint8x16_t e0 = EN16(t, 0), e1 = EN16(t, 1), e2 = EN16(t, 2), e3 = EN16(t, 3);
        uint8x16_t w0 = LDQ(pa, 0), w1 = LDQ(pa, 16), w2 = LDQ(pa, 32), w3 = LDQ(pa, 48);
        uint8x16_t x0 = LDQ(pb, 0), x1 = LDQ(pb, 16), x2 = LDQ(pb, 32), x3 = LDQ(pb, 48);
        vst1q_u8(pa, sel8(e0, vaddq_u8(a0, c1), w0)); vst1q_u8(pa + 16, sel8(e1, vaddq_u8(a1, c1), w1));
        vst1q_u8(pa + 32, sel8(e2, vaddq_u8(a2, c1), w2)); vst1q_u8(pa + 48, sel8(e3, vaddq_u8(a3, c1), w3));
        vst1q_u8(pb, sel8(e0, vsubq_u8(c1f, a0), x0)); vst1q_u8(pb + 16, sel8(e1, vsubq_u8(c1f, a1), x1));
        vst1q_u8(pb + 32, sel8(e2, vsubq_u8(c1f, a2), x2)); vst1q_u8(pb + 48, sel8(e3, vsubq_u8(c1f, a3), x3));
    }
}

/* BLDALPHA's two weights as the originals take them from w0: eva = (w0 << 1) & 0x3f, evb = ((w0 << 1) >> 8) &
 * 0x3f (32-bit shifts: bit 7 of w0 reaches evb's bit 0), each at most 32 */
static inline void bld_weights(uint32_t w, uint8x16_t *eva, uint8x16_t *evb) {
    uint32_t a = (w << 1) & 0x3f, b = ((w << 1) >> 8) & 0x3f;
    *eva = vdupq_n_u8(a > 32 ? 32 : a);
    *evb = vdupq_n_u8(b > 32 ? 32 : b);
}

/* render_scanline_color_effects_setup_blend_base_asm(bldalpha, wa, wb, mask): wa[i] = enabled ? eva : 32,
 * wb[i] = enabled ? evb : 0 (wb's block stored before wa's, as the original). */
void ds2d_setup_blend_base(uint32_t bldalpha, uint8_t *wa, uint8_t *wb, const uint8_t *mask) {
    uint8x16_t eva, evb;
    bld_weights(bldalpha, &eva, &evb);
    const uint8x16_t c20 = vdupq_n_u8(0x20);
    for (int b = 0; b < 4; b++) {
        uint8_t *pa = wa + 64 * b, *pb = wb + 64 * b;
        uint64_t m;
        memcpy(&m, mask + 8 * b, 8);
        if (!m || !~m) {                                  /* none or all set: two constants */
            uint8x16_t x = m ? evb : vdupq_n_u8(0), y = m ? eva : c20;
            vst1q_u8(pb, x); vst1q_u8(pb + 16, x); vst1q_u8(pb + 32, x); vst1q_u8(pb + 48, x);
            vst1q_u8(pa, y); vst1q_u8(pa + 16, y); vst1q_u8(pa + 32, y); vst1q_u8(pa + 48, y);
            continue;
        }
        uint8x16_t t = LD1R(mask + 8 * b, "2d");
        uint8x16_t e0 = EN16(t, 0), e1 = EN16(t, 1), e2 = EN16(t, 2), e3 = EN16(t, 3);
        vst1q_u8(pb, vandq_u8(e0, evb)); vst1q_u8(pb + 16, vandq_u8(e1, evb));
        vst1q_u8(pb + 32, vandq_u8(e2, evb)); vst1q_u8(pb + 48, vandq_u8(e3, evb));
        vst1q_u8(pa, sel8(e0, eva, c20)); vst1q_u8(pa + 16, sel8(e1, eva, c20));
        vst1q_u8(pa + 32, sel8(e2, eva, c20)); vst1q_u8(pa + 48, sel8(e3, eva, c20));
    }
}

/* render_scanline_color_effects_setup_blend_asm(bldalpha, wa, wb, mask): enabled pixels get eva / evb, the others
 * keep their weights (wa's block stored first). */
void ds2d_setup_blend(uint32_t bldalpha, uint8_t *wa, uint8_t *wb, const uint8_t *mask) {
    uint8x16_t eva, evb;
    bld_weights(bldalpha, &eva, &evb);
    for (int b = 0; b < 4; b++) {
        uint32_t m0, m1;
        memcpy(&m0, mask + 8 * b, 4); memcpy(&m1, mask + 8 * b + 4, 4);
        if (!(m0 | m1)) continue;
        uint8_t *pa = wa + 64 * b, *pb = wb + 64 * b;
        if (!~(m0 & m1)) {                                /* all set: the constants */
            vst1q_u8(pa, eva); vst1q_u8(pa + 16, eva); vst1q_u8(pa + 32, eva); vst1q_u8(pa + 48, eva);
            vst1q_u8(pb, evb); vst1q_u8(pb + 16, evb); vst1q_u8(pb + 32, evb); vst1q_u8(pb + 48, evb);
            continue;
        }
        uint8x16_t t = LD1R(mask + 8 * b, "2d");
        uint8x16_t e0 = EN16(t, 0), e1 = EN16(t, 1), e2 = EN16(t, 2), e3 = EN16(t, 3);
        uint8x16_t w0 = LDQ(pa, 0), w1 = LDQ(pa, 16), w2 = LDQ(pa, 32), w3 = LDQ(pa, 48);
        uint8x16_t x0 = LDQ(pb, 0), x1 = LDQ(pb, 16), x2 = LDQ(pb, 32), x3 = LDQ(pb, 48);
        vst1q_u8(pa, sel8(e0, eva, w0)); vst1q_u8(pa + 16, sel8(e1, eva, w1));
        vst1q_u8(pa + 32, sel8(e2, eva, w2)); vst1q_u8(pa + 48, sel8(e3, eva, w3));
        vst1q_u8(pb, sel8(e0, evb, x0)); vst1q_u8(pb + 16, sel8(e1, evb, x1));
        vst1q_u8(pb + 32, sel8(e2, evb, x2)); vst1q_u8(pb + 48, sel8(e3, evb, x3));
    }
}

static void chk_alpha(int h, alpha_fn ours, alpha_fn orig, uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask) {
    rgn r[4] = { padded(wa, 256), padded(wb, 256), { (uint8_t *)alpha, 256 }, { (uint8_t *)mask, 32 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 4, chk_in);
    ours(wa, wb, alpha, mask);
    rg_save(r, 4, chk_ours);
    rg_load(r, 4, chk_in);
    orig(wa, wb, alpha, mask);
    chk_finish(h, r, 4, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_alpha_base(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask) {
    chk_alpha(H_ALPHA_BASE, ds2d_setup_alpha_base, ds2d_alpha_base_orig, wa, wb, alpha, mask);
}
static void chk_alpha_set(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask) {
    chk_alpha(H_ALPHA, ds2d_setup_alpha, ds2d_alpha_orig, wa, wb, alpha, mask);
}
static void chk_blendw(int h, blendset_fn ours, blendset_fn orig, uint32_t w, uint8_t *wa, uint8_t *wb, const uint8_t *mask) {
    rgn r[3] = { padded(wa, 256), padded(wb, 256), { (uint8_t *)mask, 32 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ours(w, wa, wb, mask);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    orig(w, wa, wb, mask);
    chk_finish(h, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_blend_base(uint32_t w, uint8_t *wa, uint8_t *wb, const uint8_t *mask) {
    chk_blendw(H_BLEND_BASE, ds2d_setup_blend_base, orig_blend_base, w, wa, wb, mask);
}
static void chk_blend_set(uint32_t w, uint8_t *wa, uint8_t *wb, const uint8_t *mask) {
    chk_blendw(H_BLEND_SET, ds2d_setup_blend, orig_blend_set, w, wa, wb, mask);
}

/* ---- H_SHADE: render_scanline_shade_asm(eng, out, in, mask) ----
 * The brightness effect on three planar quarters (256 bytes each at +0, +0x100, +0x200): with e = min(2 * BLDY
 * ([eng+0xa2], u16), 32), and e' = 0 when BLDCNT ([eng+0xa0]) bit 6 is set (the darken effect), else e:
 *     out[i] = low byte of ((enabled ? e' * 63 + 16 : 0) + in[i] * (enabled ? 32 - e : 32)) >> 5
 * (umlal into 16 bits and shrn #5: the original's instructions). Disabled pixels come out unchanged, so a 32-pixel
 * block with no enabled pixel is copied. Caller: a tail branch from render_scanline_2d_composite (C). */
void ds2d_shade(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask) {
    uint32_t evy, bldcnt;
    memcpy(&evy, eng + 0xa2, 2); evy &= 0xffff;
    memcpy(&bldcnt, eng + 0xa0, 2); bldcnt &= 0xffff;
    uint32_t e = evy * 2;
    if (e > 32) e = 32;
    uint32_t base = ((bldcnt & 0x40) ? 0 : e) * 63 + 16;
    const uint8x16_t ven = vdupq_n_u8((uint8_t)(32 - e)), v32 = vdupq_n_u8(32);
    const uint16x8_t vb = vdupq_n_u16((uint16_t)base);
    for (int b = 0; b < 8; b++) {
        uint32_t mw;
        memcpy(&mw, mask + 4 * b, 4);
        const uint8_t *ip = in + 32 * b;
        uint8_t *op = out + 32 * b;
        uint8x16_t p0 = LDQ(ip, 0), p1 = LDQ(ip, 16), p2 = LDQ(ip, 0x100), p3 = LDQ(ip, 0x110), p4 = LDQ(ip, 0x200), p5 = LDQ(ip, 0x210);
        if (!mw) {
            vst1q_u8(op, p0); vst1q_u8(op + 16, p1); vst1q_u8(op + 0x100, p2); vst1q_u8(op + 0x110, p3);
            vst1q_u8(op + 0x200, p4); vst1q_u8(op + 0x210, p5);
            continue;
        }
        uint8x16_t t = LD1R(mask + 4 * b, "4s");
        uint8x16_t e0 = EN16(t, 0), e1 = EN16(t, 1);
        uint8x16_t w0 = sel8(e0, ven, v32), w1 = sel8(e1, ven, v32);
        uint16x8_t b0l = vandq_u16(vb, vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vget_low_u8(e0)))));
        uint16x8_t b0h = vandq_u16(vb, vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vget_high_u8(e0)))));
        uint16x8_t b1l = vandq_u16(vb, vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vget_low_u8(e1)))));
        uint16x8_t b1h = vandq_u16(vb, vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vget_high_u8(e1)))));
#define SHADE1(p, bl, bh, w) vcombine_u8(vshrn_n_u16(vmlal_u8(bl, vget_low_u8(p), vget_low_u8(w)), 5),            \
                                         vshrn_n_u16(vmlal_u8(bh, vget_high_u8(p), vget_high_u8(w)), 5))
        vst1q_u8(op, SHADE1(p0, b0l, b0h, w0)); vst1q_u8(op + 16, SHADE1(p1, b1l, b1h, w1));
        vst1q_u8(op + 0x100, SHADE1(p2, b0l, b0h, w0)); vst1q_u8(op + 0x110, SHADE1(p3, b1l, b1h, w1));
        vst1q_u8(op + 0x200, SHADE1(p4, b0l, b0h, w0)); vst1q_u8(op + 0x210, SHADE1(p5, b1l, b1h, w1));
#undef SHADE1
    }
}

static void chk_shade(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask) {
    rgn r[3] = { padded(out, 0x300), { (uint8_t *)in, 0x300 }, { (uint8_t *)mask, 32 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ds2d_shade(eng, out, in, mask);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    orig_shade(eng, out, in, mask);
    chk_finish(H_SHADE, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_SPAN8N, H_SPAN8E: the 8bpp text BG spans ----
 * render_scanline_tiled_span_8bpp_normal_palette_asm(dst, tiles, entries, flags): 33 tiles' rows of 8 index bytes,
 * tile t's from tiles + entries[t] (a u16 byte offset), reversed when bit 2 of flags[t] is set, to dst[8t..8t+7];
 * four tiles a step (the flags as one word tested per 32-bit lane with the original's constants), the 33rd as the
 * original's tail (entries[32], flags[32]). The original moves each row through a general register into a vector
 * lane (mov v.d[i], x: ~30 modeled cycles a step); here the rows load straight into the lanes (ldr d / ld1 lane).
 * Caller: render_scanline_tiled_ext (C), dst = the layer's index line (line + 0x110 - the sub-tile shift). */
static inline uint8x16_t rows2(const uint8_t *tiles, uint64_t e, int sh) {
    uint64_t a, b;                                   /* ldr d + ld1 {v.d}[1] (vcombine of two loads costs a mov + ins) */
    memcpy(&a, tiles + ((e >> sh) & 0xffff), 8);
    memcpy(&b, tiles + ((e >> (sh + 16)) & 0xffff), 8);
    uint64x2_t v = vcombine_u64(vcreate_u64(a), vdup_n_u64(0));
    v = vld1q_lane_u64(&b, v, 1);
    return vreinterpretq_u8_u64(v);
}
void ds2d_span8n(uint8_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags) {
    static const uint32_t fb[4] = { 4, 4, 0x400, 0x400 };
    const uint32x4_t fb0 = vld1q_u32(fb), fb1 = vshlq_n_u32(fb0, 16);
    for (int t = 0; t < 32; t += 4) {
        uint32_t f; uint64_t e;
        memcpy(&f, flags + t, 4); memcpy(&e, entries + t, 8);
        uint8x16_t v2 = rows2(tiles, e, 0), v3 = rows2(tiles, e, 32);
        uint32x4_t fd = vdupq_n_u32(f);
        v2 = sel8(vreinterpretq_u8_u32(vtstq_u32(fd, fb0)), vrev64q_u8(v2), v2);
        v3 = sel8(vreinterpretq_u8_u32(vtstq_u32(fd, fb1)), vrev64q_u8(v3), v3);
        vst1q_u8(dst + 8 * t, v2);
        vst1q_u8(dst + 8 * t + 16, v3);
    }
    uint64_t r;
    memcpy(&r, tiles + entries[32], 8);
    if (flags[32] & 4) r = __builtin_bswap64(r);
    memcpy(dst + 256, &r, 8);
}

/* render_scanline_tiled_span_8bpp_ext_palette_asm(dst, tiles, entries, flags): the same rows as u16 pixels,
 * index | palette << 8 with the palette number from flags[t] >> 4 (the extended palettes' 12-bit index); the 33rd
 * tile's palette byte is the original's: (flags[32] >> 4 | flags[33] << 4) & 0xff, from its 32-bit load of
 * flags[32..35] (and it loads entries[32..35] as 8 bytes). Writes dst[0..263]. Caller: render_scanline_tiled_ext (C). */
void ds2d_span8e(uint16_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags) {
    static const uint32_t fb[4] = { 4, 4, 0x400, 0x400 };
    static const uint8_t t01[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };
    const uint32x4_t fb0 = vld1q_u32(fb), fb1 = vshlq_n_u32(fb0, 16);
    const uint8x16_t s01 = vld1q_u8(t01), s23 = vaddq_u8(s01, vdupq_n_u8(2));
    uint8_t *d = (uint8_t *)dst;
    for (int t = 0; t < 32; t += 4) {
        uint32_t f; uint64_t e;
        memcpy(&f, flags + t, 4); memcpy(&e, entries + t, 8);
        uint8x16_t v2 = rows2(tiles, e, 0), v3 = rows2(tiles, e, 32);
        uint32x4_t fd = vdupq_n_u32(f);
        uint8x16_t pn = vreinterpretq_u8_u32(vdupq_n_u32((f >> 4) & 0x0f0f0f0f));
        uint8x16_t p2 = vqtbl1q_u8(pn, s01), p3 = vqtbl1q_u8(pn, s23);
        v2 = sel8(vreinterpretq_u8_u32(vtstq_u32(fd, fb0)), vrev64q_u8(v2), v2);
        v3 = sel8(vreinterpretq_u8_u32(vtstq_u32(fd, fb1)), vrev64q_u8(v3), v3);
        vst1q_u8(d + 16 * t, vzip1q_u8(v2, p2));
        vst1q_u8(d + 16 * t + 16, vzip2q_u8(v2, p2));
        vst1q_u8(d + 16 * t + 32, vzip1q_u8(v3, p3));
        vst1q_u8(d + 16 * t + 48, vzip2q_u8(v3, p3));
    }
    uint32_t f; uint64_t e, r;
    memcpy(&f, flags + 32, 4); memcpy(&e, entries + 32, 8);
    memcpy(&r, tiles + (e & 0xffff), 8);
    if (f & 4) r = __builtin_bswap64(r);
    uint8x8_t row = vcreate_u8(r), pal = vdup_n_u8((uint8_t)(f >> 4));
    vst1q_u8(d + 512, vcombine_u8(vzip1_u8(row, pal), vzip2_u8(row, pal)));
}

static void chk_span8n(uint8_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags) {
    rgn r[3] = { padded(dst, 264), { (uint8_t *)entries, 66 }, { (uint8_t *)flags, 33 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ds2d_span8n(dst, tiles, entries, flags);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    ds2d_span8n_orig(dst, tiles, entries, flags);
    chk_finish(H_SPAN8N, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_span8e(uint16_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags) {
    rgn r[3] = { padded(dst, 528), { (uint8_t *)entries, 72 }, { (uint8_t *)flags, 36 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 3, chk_in);
    ds2d_span8e(dst, tiles, entries, flags);
    rg_save(r, 3, chk_ours);
    rg_load(r, 3, chk_in);
    ds2d_span8e_orig(dst, tiles, entries, flags);
    chk_finish(H_SPAN8E, r, 3, total);
    pthread_mutex_unlock(&chk_lock);
}


/* a region with the pad on its left only (the bytes after it are another output of the same call) */
static rgn padded_l(void *p, size_t n) {
    rgn r = padded(p, n);
    return (rgn){ r.p, (size_t)((uint8_t *)p - r.p) + n };
}

/* ---- H_TME4, H_TME8: render_scanline_tiled_setup_tile_map_entries_{4,8}bpp_asm(entries, flags, row, row2, k, roff) ----
 * The 40 map entries a text BG line needs from tile column k (0..31, the caller's 5-bit field) of a 32-tile map row
 * (row: the 64-byte row of the first screen block, row2: of the next one), through DraStic's table of 32 loaders:
 * with b = k & ~7, r = k & 7 and S = row[b..31] ++ row2[0..b+7], the entries are e[0..31] = S[r..r+31] and e[32..39]
 * = row2[b + ((r + i) & 7)] (the loader's last register is rotated, not shifted: entry 39 wraps to row2[b]). Per
 * entry: entries[i] = (e & 0x3ff) << 5 (8bpp: << 6) | (e & 0x800 ? 28 - roff : roff) (8bpp: 56 - roff): the tile
 * row's byte offset in the character data, roff = (vofs & 7) * 4 (8bpp: * 8); flags[i] = e >> 8 (bit 2 h-flip, 3
 * v-flip, 4-7 the palette). The original stores 48 flag bytes, the last 8 from a register half it never wrote (whatever
 * the caller's code left in v23's upper half); their only readers, the span routine tiled_ext calls next, read at
 * most flags[0..35] (span_4bpp and span_8bpp_ext: 32 a word at a time, then the 33rd tile's 4-byte load of
 * flags[32..35], of which bytes 32 and 33 are used; span_8bpp_normal: flags[0..32]) and entries[0..35], so flag
 * bytes 40..47 are not an output: this writes flags[0..39]. Reads row[b..31],
 * row2[0..b+7] (64 bytes at most each), as the loaders do. Caller: render_scanline_tiled_ext (C), the buffers on its
 * stack. The original models at ~47 cycles (an indirect call into the table, st1 of four registers); this at ~30. */
static inline __attribute__((always_inline)) void tme(uint16_t *entries, uint8_t *flags, const uint16_t *row,
                                                      const uint16_t *row2, uint32_t k, uint32_t roff, const int BPP8) {
    k &= 31;
    unsigned b = k & ~7u, r = k & 7;
    uint8x16_t v0, v1, v2, v3, v4;                   /* S[0..39] as the loaders have them */
    const uint8_t *p = (const uint8_t *)(row + b), *q = (const uint8_t *)row2;
    switch (b) {
    case 0: v0 = LDQ(p, 0); v1 = LDQ(p, 16); v2 = LDQ(p, 32); v3 = LDQ(p, 48); v4 = LDQ(q, 0); break;
    case 8: v0 = LDQ(p, 0); v1 = LDQ(p, 16); v2 = LDQ(p, 32); v3 = LDQ(q, 0); v4 = LDQ(q, 16); break;
    case 16: v0 = LDQ(p, 0); v1 = LDQ(p, 16); v2 = LDQ(q, 0); v3 = LDQ(q, 16); v4 = LDQ(q, 32); break;
    default: v0 = LDQ(p, 0); v1 = LDQ(q, 0); v2 = LDQ(q, 16); v3 = LDQ(q, 32); v4 = LDQ(q, 48); break;
    }
#define ROT(n) v0 = vextq_u8(v0, v1, n); v1 = vextq_u8(v1, v2, n); v2 = vextq_u8(v2, v3, n); v3 = vextq_u8(v3, v4, n); \
               v4 = vextq_u8(v4, v4, n); break;
    switch (r) {
    case 1: ROT(2) case 2: ROT(4) case 3: ROT(6) case 4: ROT(8) case 5: ROT(10) case 6: ROT(12) case 7: ROT(14)
    default: break;
    }
#undef ROT
    const uint16x8_t vflip = vdupq_n_u16(0x800), ro = vdupq_n_u16((uint16_t)roff), rf = vdupq_n_u16((uint16_t)((BPP8 ? 56 : 28) - roff));
#define ONE(v, o) do {                                                                                              \
        uint16x8_t e_ = vreinterpretq_u16_u8(v);                                                                   \
        uint16x8_t s_ = sel16(tst16(e_, vflip), rf, ro);                                                           \
        s_ = BPP8 ? vsliq_n_u16(s_, e_, 6) : vbicq_u16(vsliq_n_u16(s_, e_, 5), vdupq_n_u16(0x8000));               \
        vst1q_u16(entries + (o), s_);                                                                              \
    } while (0)
    ONE(v0, 0); ONE(v1, 8); ONE(v2, 16); ONE(v3, 24); ONE(v4, 32);
#undef ONE
    vst1q_u8(flags, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_u8(v0), 8), vshrn_n_u16(vreinterpretq_u16_u8(v1), 8)));
    vst1q_u8(flags + 16, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_u8(v2), 8), vshrn_n_u16(vreinterpretq_u16_u8(v3), 8)));
    vst1_u8(flags + 32, vshrn_n_u16(vreinterpretq_u16_u8(v4), 8));
}
void ds2d_tme4(uint16_t *entries, uint8_t *flags, const uint16_t *row, const uint16_t *row2, uint32_t k, uint32_t roff) {
    tme(entries, flags, row, row2, k, roff, 0);
}
void ds2d_tme8(uint16_t *entries, uint8_t *flags, const uint16_t *row, const uint16_t *row2, uint32_t k, uint32_t roff) {
    tme(entries, flags, row, row2, k, roff, 1);
}
typedef void (*tme_fn)(uint16_t *, uint8_t *, const uint16_t *, const uint16_t *, uint32_t, uint32_t);
static void chk_tme(int h, tme_fn ours, tme_fn orig, uint16_t *entries, uint8_t *flags, const uint16_t *row, const uint16_t *row2,
                    uint32_t k, uint32_t roff) {
    /* the 8 undefined flag bytes after the 40 are left out; the pads stay on the left: tiled_ext's flags follow
     * its entries directly (the entries' right pad is the flags' region), and after the flags come the original's
     * 8 undefined bytes */
    rgn r[4] = { padded_l(entries, 80), padded_l(flags, 40), { (uint8_t *)row, 64 }, { (uint8_t *)row2, 64 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 4, chk_in);
    ours(entries, flags, row, row2, k, roff);
    rg_save(r, 4, chk_ours);
    rg_load(r, 4, chk_in);
    orig(entries, flags, row, row2, k, roff);
    chk_finish(h, r, 4, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_tme4(uint16_t *e, uint8_t *f, const uint16_t *row, const uint16_t *row2, uint32_t k, uint32_t roff) {
    chk_tme(H_TME4, ds2d_tme4, ds2d_tme4_orig, e, f, row, row2, k, roff);
}
static void chk_tme8(uint16_t *e, uint8_t *f, const uint16_t *row, const uint16_t *row2, uint32_t k, uint32_t roff) {
    chk_tme(H_TME8, ds2d_tme8, ds2d_tme8_orig, e, f, row, row2, k, roff);
}

/* ---- H_DBL: render_scanline_disable_blank_layers_asm(masks, word) ----
 * masks: the four BGs' 256-bit visibility bitmaps (32 bytes each); word: a 32-bit layer-enable word. With f_k = 1
 * when any byte of mask k is set: word &= (f_0 ? 0xfffffff1 : 0) | (f_1 ? 0xfffffff2 : 0) | (f_2 ? 0xfffffff4 : 0) |
 * (f_3 ? 0xfffffff8 : 0) | 0xfff0 (the original's cmtst / umaxv / bic / orr: a BG with no visible pixel loses its
 * enable bit, and with none visible bits 16-31 go too). Caller: render_scanline_2d (C), twice a line. */
void ds2d_disable_blank(const uint8_t *m, uint8_t *word) {
    uint32_t w, keep = 0xfff0;
    memcpy(&w, word, 4);
    for (int k = 0; k < 4; k++) {
        uint8x16_t v = vorrq_u8(LDQ(m + 32 * k, 0), LDQ(m + 32 * k, 16));
        uint64x2_t d = vreinterpretq_u64_u8(v);
        if (vgetq_lane_u64(d, 0) | vgetq_lane_u64(d, 1)) keep |= 0xfffffff0 | 1u << k;
    }
    w &= keep;
    memcpy(word, &w, 4);
}
static void chk_dbl(const uint8_t *m, uint8_t *word) {
    rgn r[2] = { padded(word, 4), { (uint8_t *)m, 128 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_disable_blank(m, word);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    orig_dbl(m, word);
    chk_finish(H_DBL, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}

/* ---- H_VIS4, H_VIS8, H_VIS12: a layer line's 256-bit visibility bitmap ----
 * set_visibility_4bpp_asm(out, src, shift): src holds 272 nibbles (136 bytes: 34 tile rows, the low nibble first);
 * bit i of out = nibble i + shift is not zero, shift = hofs & 7 (the caller's; any value gives the original's ushl
 * results: the bytes are shifted right by shift and the next byte left by 8 - shift, both as the original's
 * per-lane ushl of the low byte of -shift / 8 - shift). The original's instructions in order: two cmtst per 16 bytes
 * (the nibbles), bit + and with its insert masks (two bits per byte), three addp levels (eight pixels a byte), ext,
 * ushl, orr. Loads by ldr q instead of ld1 with writeback. Caller: render_scanline_tiled_ext (C). */
void ds2d_vis4(uint8_t *out, const uint8_t *src, uint32_t shift) {
    const uint8x16_t lo = vdupq_n_u8(0x0f), hi = vdupq_n_u8(0xf0);
    const uint8x16_t ins = vreinterpretq_u8_u32(vdupq_n_u32(0x80200802)), keep = vreinterpretq_u8_u32(vdupq_n_u32(0xc0300c03));
    const int8x16_t sr = vdupq_n_s8((int8_t)(0u - shift)), sl = vdupq_n_s8((int8_t)(8u - shift));
#define NIB(v) vandq_u8(sel8(ins, tst8(v, hi), tst8(v, lo)), keep)
    uint8x16_t a0 = NIB(LDQ(src, 0)), a1 = NIB(LDQ(src, 16)), a2 = NIB(LDQ(src, 32)), a3 = NIB(LDQ(src, 48));
    uint8x16_t a4 = NIB(LDQ(src, 64)), a5 = NIB(LDQ(src, 80)), a6 = NIB(LDQ(src, 96)), a7 = NIB(LDQ(src, 112));
    uint8x16_t a8 = NIB(vcombine_u8(vld1_u8(src + 128), vdup_n_u8(0)));
#undef NIB
    a0 = vpaddq_u8(a0, a1); a1 = vpaddq_u8(a2, a3); a2 = vpaddq_u8(a4, a5); a3 = vpaddq_u8(a6, a7); a4 = vpaddq_u8(a8, a8);
    a0 = vpaddq_u8(a0, a1); a1 = vpaddq_u8(a2, a3); a2 = vpaddq_u8(a4, a4);
    uint8x16_t n0 = vextq_u8(a0, a1, 1), n1 = vextq_u8(a1, a2, 1);
    vst1q_u8(out, vorrq_u8(vshlq_u8(a0, sr), vshlq_u8(n0, sl)));
    vst1q_u8(out + 16, vorrq_u8(vshlq_u8(a1, sr), vshlq_u8(n1, sl)));
}
/* set_visibility_8bpp_asm(out, src): bit i = src[i] != 0 over 256 bytes (cmeq #0, bic of the bit masks, three addp
 * levels). Callers: render_scanline_tiled_ext, the affine renderers (C). */
void ds2d_vis8(uint8_t *out, const uint8_t *src) {
    static const uint8_t b8[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8x16_t bits = vld1q_u8(b8);
#define NZ(o) vbicq_u8(bits, vceqzq_u8(LDQ(src, o)))
    uint8x16_t a0 = vpaddq_u8(NZ(0), NZ(16)), a1 = vpaddq_u8(NZ(32), NZ(48)), a2 = vpaddq_u8(NZ(64), NZ(80)), a3 = vpaddq_u8(NZ(96), NZ(112));
    uint8x16_t a4 = vpaddq_u8(NZ(128), NZ(144)), a5 = vpaddq_u8(NZ(160), NZ(176)), a6 = vpaddq_u8(NZ(192), NZ(208)), a7 = vpaddq_u8(NZ(224), NZ(240));
#undef NZ
    a0 = vpaddq_u8(a0, a1); a1 = vpaddq_u8(a2, a3); a2 = vpaddq_u8(a4, a5); a3 = vpaddq_u8(a6, a7);
    vst1q_u8(out, vpaddq_u8(a0, a1));
    vst1q_u8(out + 16, vpaddq_u8(a2, a3));
}
/* set_visibility_12bpp_asm(out, src): the same over the low bytes of 256 u16 (the original's ld2 takes the even
 * bytes: an extended-palette index's low byte; a non-zero palette number alone does not count), 128 pixels a half. */
void ds2d_vis12(uint8_t *out, const uint16_t *src) {
    static const uint8_t b8[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8x16_t bits = vld1q_u8(b8);
    const uint8_t *s = (const uint8_t *)src;
    for (int h = 0; h < 2; h++, s += 256) {
#define NZ(o) vbicq_u8(bits, vceqzq_u8(vuzp1q_u8(LDQ(s, o), LDQ(s, (o) + 16))))
        uint8x16_t a0 = vpaddq_u8(NZ(0), NZ(32)), a1 = vpaddq_u8(NZ(64), NZ(96)), a2 = vpaddq_u8(NZ(128), NZ(160)), a3 = vpaddq_u8(NZ(192), NZ(224));
#undef NZ
        vst1q_u8(out + 16 * h, vpaddq_u8(vpaddq_u8(a0, a1), vpaddq_u8(a2, a3)));
    }
}
static void chk_vis4(uint8_t *out, const uint8_t *src, uint32_t shift) {
    rgn r[2] = { padded(out, 32), { (uint8_t *)src, 136 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_vis4(out, src, shift);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    ds2d_vis4_orig(out, src, shift);
    chk_finish(H_VIS4, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_vis8(uint8_t *out, const uint8_t *src) {
    rgn r[2] = { padded(out, 32), { (uint8_t *)src, 256 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_vis8(out, src);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    ds2d_vis8_orig(out, src);
    chk_finish(H_VIS8, r, 2, total);
    pthread_mutex_unlock(&chk_lock);
}
static void chk_vis12(uint8_t *out, const uint16_t *src) {
    rgn r[2] = { padded(out, 32), { (uint8_t *)src, 512 } };
    pthread_mutex_lock(&chk_lock);
    size_t total = rg_save(r, 2, chk_in);
    ds2d_vis12(out, src);
    rg_save(r, 2, chk_ours);
    rg_load(r, 2, chk_in);
    ds2d_vis12_orig(out, src);
    chk_finish(H_VIS12, r, 2, total);
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
static const uint32_t expect_pal8[4] = { 0xd2803fc4, 0xf8408425, 0x71002063, 0x8a050486 };
static const uint32_t expect_pal12[4] = { 0xd283ffc4, 0xf8408425, 0x71002063, 0xf8408426 };
static const uint32_t expect_prio2[4] = { 0x3942cc04, 0x91021005, 0x4f000400, 0x4f000401 };
static const uint32_t expect_alpha_base[4] = { 0x10ffe664, 0x4f00e7f9, 0x4c407098, 0x4f00e43a };
static const uint32_t expect_alpha[4] = { 0x10ffe224, 0x4f00e7fd, 0x4c40709c, 0x4f00e43e };
static const uint32_t expect_blendw[4] = { 0x0b000000, 0x4f01e411, 0x53001404, 0x53083405 };   /* _blend_base and _blend */
static const uint32_t expect_shade[4] = { 0xfc1f0fe8, 0x79414404, 0x52800408, 0x79414005 };
static const uint32_t expect_span8n[4] = { 0x10ffff85, 0x52800404, 0x4c4078a0, 0x4f305401 };
static const uint32_t expect_span8e[4] = { 0x10fffac5, 0x4f00e432, 0x4c4078a0, 0x4e081ff2 };
static const uint32_t expect_tme4[4] = { 0x10ffdb87, 0xaa1e03e8, 0x52800386, 0x4f00a505 };
static const uint32_t expect_tme8[4] = { 0x10ffd6c7, 0xaa1e03e8, 0x52800706, 0x4f00a505 };
static const uint32_t expect_dbl[4] = { 0x4cdf2000, 0x4c402004, 0xbd400030, 0x4f07e7f1 };
static const uint32_t expect_vis4[4] = { 0x10ffffc4, 0x52800103, 0x4b020063, 0x4b0203e2 };
static const uint32_t expect_vis8[4] = { 0x10ffff82, 0x4c407058, 0x4cdf2020, 0x4cdf2024 };
static const uint32_t expect_vis12[4] = { 0x10fff8c2, 0x4c407058, 0x52800043, 0xd503201f };

/* one hook: the message names the routine; `orig` receives the trampoline (or stays as set) */
static void *hook1(int h, uintptr_t off, const uint32_t expect[4], void *ours, void *chk) {
    if (!(ds2d_mask & (1 << h))) return 0;
    void *t = rast_hook(off, expect, ds2d_check ? chk : ours);
    fprintf(stderr, t ? "[ds2d] %s replaced%s\n" : "[ds2d] unknown %s, not hooked%s\n", hname[h],
            ds2d_check ? ", checking every call against DraStic's" : "");
    return t;
}

/* the default: every hook but those measured no faster than DraStic's (cycles.py on the 2D scene cycle, modeled A55
 * cycles a call, the patched entry's jump included): the tile map entries (DraStic's table loaders: 44 against 74),
 * disable_blank_layers (45 against ~60), set_visibility_4bpp (95 against ~98) and setup_blend_base (176 against ~200:
 * both are bound by the same 16 128-bit operations per 64 pixels). RAST_2D=-1 (or their bits) enables them. */
#define DS2D_DEFAULT (~(1 << H_TME4 | 1 << H_TME8 | 1 << H_DBL | 1 << H_VIS4 | 1 << H_BLEND_BASE))

void ds2d_init(void) {
    const char *e = getenv("DSFLIP_RAST_2D");
    if (!e) e = getenv("RAST_2D");
    ds2d_mask = e ? (int)strtol(e, 0, 0) : DS2D_DEFAULT;
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
    /* the palette lookups: the check mode runs private copies of the originals (their loops re-enter the patched
     * entry), taken before the patch; without a copy they stay DraStic's */
    if (ds2d_check) {
        orig_pal8 = (pal8_fn)copy_orig(DS_PAL8, 112);
        orig_pal12 = (pal12_fn)copy_orig(DS_PAL12, 116);
        if (!orig_pal8 || !orig_pal12) { fprintf(stderr, "[ds2d] no room for the palette lookups' copies, not hooked\n"); ds2d_mask &= ~(1 << H_PAL8 | 1 << H_PAL12); }
    }
    hook1(H_PAL8, DS_PAL8, expect_pal8, (void *)ds2d_pal8, (void *)chk_pal8);
    hook1(H_PAL12, DS_PAL12, expect_pal12, (void *)ds2d_pal12, (void *)chk_pal12);
    orig_prio2 = (prio2_fn)hook1(H_PRIO2, DS_PRIO_DOUBLE, expect_prio2, (void *)ds2d_prio_double, (void *)chk_prio2);
    /* the ORIG4 stubs' constants and continuation addresses (ds2d_asm.S) */
    ds2d_orig_vars[0][0] = ds2d_orig_vars[1][0] = ds_base + DS_SHADE_BITS;
    ds2d_orig_vars[0][1] = ds_base + DS_SETUP_ALPHA_BASE + 16;
    ds2d_orig_vars[1][1] = ds_base + DS_SETUP_ALPHA + 16;
    ds2d_orig_vars[2][0] = ds2d_orig_vars[3][0] = ds_base + DS_SPAN8_CONSTS;
    ds2d_orig_vars[2][1] = ds_base + DS_SPAN8N + 16;
    ds2d_orig_vars[3][1] = ds_base + DS_SPAN8E + 16;
    hook1(H_ALPHA_BASE, DS_SETUP_ALPHA_BASE, expect_alpha_base, (void *)ds2d_setup_alpha_base, (void *)chk_alpha_base);
    hook1(H_ALPHA, DS_SETUP_ALPHA, expect_alpha, (void *)ds2d_setup_alpha, (void *)chk_alpha_set);
    orig_blend_base = (blendset_fn)hook1(H_BLEND_BASE, DS_SETUP_BLEND_BASE, expect_blendw, (void *)ds2d_setup_blend_base, (void *)chk_blend_base);
    orig_blend_set = (blendset_fn)hook1(H_BLEND_SET, DS_SETUP_BLEND, expect_blendw, (void *)ds2d_setup_blend, (void *)chk_blend_set);
    orig_shade = (shade_fn)hook1(H_SHADE, DS_SHADE, expect_shade, (void *)ds2d_shade, (void *)chk_shade);
    hook1(H_SPAN8N, DS_SPAN8N, expect_span8n, (void *)ds2d_span8n, (void *)chk_span8n);
    hook1(H_SPAN8E, DS_SPAN8E, expect_span8e, (void *)ds2d_span8e, (void *)chk_span8e);
    ds2d_orig_vars[4][0] = ds2d_orig_vars[5][0] = ds_base + DS_TME_TABLE;
    ds2d_orig_vars[4][1] = ds_base + DS_TME4 + 16;
    ds2d_orig_vars[5][1] = ds_base + DS_TME8 + 16;
    ds2d_orig_vars[6][0] = ds_base + DS_VIS4_CONSTS;
    ds2d_orig_vars[6][1] = ds_base + DS_VIS4 + 16;
    ds2d_orig_vars[7][0] = ds2d_orig_vars[8][0] = ds_base + DS_VIS_CONSTS;
    ds2d_orig_vars[7][1] = ds_base + DS_VIS8 + 16;
    ds2d_orig_vars[8][1] = ds_base + DS_VIS12 + 16;
    hook1(H_TME4, DS_TME4, expect_tme4, (void *)ds2d_tme4, (void *)chk_tme4);
    hook1(H_TME8, DS_TME8, expect_tme8, (void *)ds2d_tme8, (void *)chk_tme8);
    orig_dbl = (dbl_fn)hook1(H_DBL, DS_DISABLE_BLANK, expect_dbl, (void *)ds2d_disable_blank, (void *)chk_dbl);
    hook1(H_VIS4, DS_VIS4, expect_vis4, (void *)ds2d_vis4, (void *)chk_vis4);
    hook1(H_VIS8, DS_VIS8, expect_vis8, (void *)ds2d_vis8, (void *)chk_vis8);
    hook1(H_VIS12, DS_VIS12, expect_vis12, (void *)ds2d_vis12, (void *)chk_vis12);
}
