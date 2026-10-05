/* comp.c: the 3D visibility step of DraStic's scanline compositor, off the emulation thread's critical path.
 *
 * DraStic composites the 3D screen (engine A) on the emulation thread: per DS line and quarter (256 pixels of one
 * output row's even or odd pixels), render_scanline_set_3d_visibility (0x3c2c0) turns the 3D pixels' alpha bytes
 * into BG0's visibility bitmap and the quarter's flags (spec/composite.c): ~425 instructions, 768 calls a frame,
 * ~0.33 M instructions a frame, a third of the 3D screen's compositing. Two steps take it off the critical path:
 *
 * 1. The function is replaced (its 4th instruction is an adrp, so the patch cannot trampoline back: this is a full
 *    replacement) by hook_vis(), which computes the same 32 bytes and return value with NEON (compvis.h, 139
 *    instructions a call) for any pointer: the callers pass the 2x frame, the 1x line, the downsampled line or a
 *    BG0HOFS-shifted copy on their stack.
 * 2. Table (RAST_COMP=2, the default): the result depends only on the 1 KiB of pixels the pointer addresses, and in
 *    the 2x path that is a half-row of the 3D output frame (frame + line*0x1000 + q*0x400). So the render threads
 *    compute it once per finished bin (comp_bin(), called by rast.c's and hr.c's bin loops right after the bin's
 *    output block is written: 64 half-rows of 0x400 bytes, rows y at +y*0x800, parity p at +p*0x400; res2.c's
 *    res2_vis_bin(), compvis.h's results scheduled for the A55; or, but for the fog-only resolve, res2.c's resolves
 *    while they write the block, through comp_bin_table()) into a table per output frame; the hook copies the table
 *    entry when the pointer is a half-row of a frame whose table is valid, and computes with NEON otherwise.
 *
 * The table's validity rule: tvalid[slot] is set only while the frame's bytes are the ones the table was computed
 * from. DraStic writes the two output frames (sys + SYS_FRAMEBUF, 2 x 0xc0000) only in
 *   - update_frame_3d_4x: renders the bins (our hook, which computes the entries) into SYS_OUTPUT, then the edge
 *     marking gap passes re-mark rows 32k-1 and 32k (k = 1..11); or, when it does not render, with threaded_3d
 *     memcpy()s the last rendered frame (SYS_LAST) into SYS_OUTPUT. hook_uf4() wraps it: before, it invalidates
 *     the frame the call may write (the unpublished one with threaded_3d; the bins also invalidate SYS_OUTPUT when
 *     they start); after, it recomputes the 44 gap-row entries and validates the frame if all 12 bins ran, copies
 *     the source's table and validity if it copied, or restores the old validity if nothing was written.
 *   - update_frame_3d_1x (1x frames, whose lines are also 0x400 apart) and reset_video_3d (memset): both
 *     invalidate every table, before and after.
 * Ordering: with threaded_3d=0, rendering (line 215) and compositing (line 192, or line by line) run one after the
 * other on the emulation thread; with threaded_3d=1 the compositor reads only the published frame, never the one
 * being written, and the table writes reach it through the same mutex hand-off (video_3d_finish_rendering) as the
 * pixels. RAST_COMPCHECK=1 checks every call against the C port (and every table when it becomes valid).
 *
 * 3. The quarters that show only the 3D layer and the backdrop (RAST_COMPFUSE, default on). After the visibility
 *    step render_scanline_2d calls render_scanline_2d_composite (0x3c6d0) for the quarter. With (flags & 0xf) == 0
 *    (no blending, no brightness) that is DraStic's priority encoder, then select_pixels: the layers' u16 lines
 *    merged, the backdrop, the 6-bit expansion into the planes, then the 3D pixels' bytes over them where BG0 is on
 *    top (spec/composite.c), ~620 instructions a quarter. hook_composite() runs the same priority encoder (DraStic's
 *    own function, so the masks in S are its bytes) and, when the masks say the quarter is BG0's and the backdrop's
 *    alone (compfuse.h: no other layer of the mask claims a pixel, and every pixel is BG0's or the backdrop's), writes
 *    the planes in one NEON pass: ~45 instructions for a quarter that is all 3D, ~30 all backdrop, ~150 mixed.
 *    Otherwise it calls DraStic's select_pixels with the arguments the original passes it; and for other flags, no
 *    3D layer, another layer mask or overlapping buffers, the original through a trampoline (its first four
 *    instructions are position independent; rast_hook() returns it). Both run the original encoder, and both the
 *    original and the fused pass write only the planes after it (select_pixels' u16 line is its own stack, written
 *    in full before it is read; nothing reads below the stack pointer afterwards), so the masks and the planes are
 *    the whole observable state, and the planes are the same bytes by the derivation in compfuse.h.
 *    RAST_COMPCHECK=1 also checks every call that does not go to the trampoline: our path, then the original through
 *    the trampoline on the same input bytes, comparing render_scanline_2d's whole stack frame (S and the layer lines),
 *    the planes with 0x40 bytes either side, the 3D pixels and the engine's first 0x400 bytes; counts per path every
 *    2 s ("[comp] composite:" lines, "[compcheck]" for a difference).
 *
 * RAST_COMP (or DSFLIP_RAST_COMP): 0 off (DraStic's function), 1 the NEON replacement only, 2 (default) with the
 * table. RAST_COMPFUSE (DSFLIP_RAST_COMPFUSE) 0 leaves render_scanline_2d_composite alone. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ds3d.h"
#include "rast.h"
#include "comp.h"
#include "compvis.h"
#include "compfuse.h"
#include "res2.h"
#include "spec/composite.h"

#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)U64(p, o))

#define FRAME_BYTES (NBINS * BIN_BYTES)             /* 0xc0000: 384 rows of 0x800 */
#define HALF_ROWS   (FRAME_BYTES / 0x400)           /* 768 entries: row y, parity p at index 2y + p */

/* the tables of both output frames, indexed by (pointer - fb0) >> 10: frame 0 (at fb0) entries 0..767, frame 1 (at
 * fb0 + FRAME_BYTES) entries 768..1535 */
static uint8_t tbits[2 * HALF_ROWS][32] __attribute__((aligned(64)));
static uint8_t tflag[2 * HALF_ROWS];
static int tvalid[2];                   /* atomic: frame s's table matches the frame's bytes */
static uintptr_t fb0;                   /* sys + SYS_FRAMEBUF (set by the update hook) */
static unsigned long bins_done;         /* atomic: bins whose entries comp_bin() computed */
static int comp_mode, use_tab, compcheck;

static void (*orig_uf4)(uint8_t *, uint32_t), (*orig_uf1)(uint8_t *, uint32_t), (*orig_reset)(uint8_t *);

/* the table slot of an output frame pointer, or -1 */
static inline int slot_of(const uint8_t *p) {
    uintptr_t b = __atomic_load_n(&fb0, __ATOMIC_RELAXED), off = (uintptr_t)p - b;
    return b && off < 2 * FRAME_BYTES && off % FRAME_BYTES == 0 ? (int)(off / FRAME_BYTES) : -1;
}
static inline void set_valid(int s, int v) { __atomic_store_n(&tvalid[s], v, __ATOMIC_RELEASE); }
static void invalidate_all(void) { set_valid(0, 0); set_valid(1, 0); }

/* ---- check mode ---- */
static pthread_mutex_t cc_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cc_calls, cc_tab, cc_bad, cc_tables, cc_tables_bad, cc_upd[4];  /* updates: rendered, copied, unchanged, 1x */
static struct timespec cc_last;

static void cc_report(void) {                                   /* every 2 s, under cc_lock */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - cc_last.tv_sec < 2) return;
    cc_last = now;
    fprintf(stderr, "[comp] check: %lu calls checked (%lu from the table, %lu NEON), %lu differ; %lu tables checked, %lu differ; "
            "frame updates: %lu rendered, %lu copied, %lu unchanged, %lu at 1x\n",
            cc_calls, cc_tab, cc_calls - cc_tab, cc_bad, cc_tables, cc_tables_bad, cc_upd[0], cc_upd[1], cc_upd[2], cc_upd[3]);
}

static void cc_call(const uint8_t *bits, const uint32_t *px, uint32_t r, int slot, unsigned idx) {
    uint8_t ref[32];
    uint32_t rr = spec_render_scanline_set_3d_visibility(ref, px);
    pthread_mutex_lock(&cc_lock);
    cc_calls++; cc_tab += slot >= 0;
    if (rr != r || memcmp(ref, bits, 32)) {
        if (cc_bad++ < 30) {
            int j = 0;
            while (j < 31 && ref[j] == bits[j]) j++;
            fprintf(stderr, "[compcheck] set_3d_visibility(%p) from %s %d entry %u: returns %#x, C port %#x; bitmap byte %d %02x, C port %02x\n",
                    (const void *)px, slot >= 0 ? "table" : "NEON", slot, idx, r, rr, j, bits[j], ref[j]);
        }
    }
    cc_report();
    pthread_mutex_unlock(&cc_lock);
}

/* a table that just became valid against its frame, entry by entry */
static void cc_table(int s) {
    const uint8_t *f = (const uint8_t *)(__atomic_load_n(&fb0, __ATOMIC_RELAXED) + (uintptr_t)s * FRAME_BYTES);
    unsigned bad = 0, first = 0;
    for (unsigned i = 0; i < HALF_ROWS; i++) {
        uint8_t ref[32];
        uint32_t rr = spec_render_scanline_set_3d_visibility(ref, (const uint32_t *)(f + i * 0x400));
        unsigned e = s * HALF_ROWS + i;
        if (rr != tflag[e] || memcmp(ref, tbits[e], 32)) { if (!bad++) first = i; }
    }
    pthread_mutex_lock(&cc_lock);
    cc_tables++;
    if (bad && cc_tables_bad++ < 30)
        fprintf(stderr, "[compcheck] table %d: %u of %u entries differ from the frame, first: row %u parity %u\n", s, bad,
                HALF_ROWS, first / 2, first % 2);
    pthread_mutex_unlock(&cc_lock);
}

/* ---- the replacement of render_scanline_set_3d_visibility ---- */
/* the table entry for px, or -1 */
static inline int tab_entry(const uint32_t *px) {
    uintptr_t off = (uintptr_t)px - __atomic_load_n(&fb0, __ATOMIC_RELAXED);
    if (off >= 2 * FRAME_BYTES || (off & 0x3ff)) return -1;      /* not a half-row of an output frame */
    unsigned e = (unsigned)(off >> 10);
    return __atomic_load_n(&tvalid[e >= HALF_ROWS], __ATOMIC_ACQUIRE) ? (int)e : -1;
}
static __attribute__((noinline)) uint32_t hook_vis_check(uint8_t *bits, const uint32_t *px) {
    int e = tab_entry(px);
    uint32_t r;
    if (e >= 0) { memcpy(bits, tbits[e], 32); r = tflag[e]; }
    else r = comp_vis_neon(bits, px);
    cc_call(bits, px, r, e < 0 ? -1 : e >= HALF_ROWS, e < 0 ? 0 : e % HALF_ROWS);
    return r;
}
static uint32_t hook_vis(uint8_t *bits, const uint32_t *px) {
    if (__builtin_expect(compcheck, 0)) return hook_vis_check(bits, px);
    int e = tab_entry(px);
    if (e >= 0) { memcpy(bits, tbits[e], 32); return tflag[e]; }
    return comp_vis_neon(bits, px);
}

/* ---- the table: per bin on the render threads, and the update wrappers ---- */
void comp_bins_begin(uint8_t *sys) {
    if (!use_tab) return;
    int s = slot_of(PTR(sys, SYS_OUTPUT));
    if (s >= 0) set_valid(s, 0);
}

void comp_bin(uint8_t *sys, unsigned bin, int count) {
    if (!use_tab) return;
    const uint8_t *out = PTR(sys, SYS_OUTPUT);
    int s = slot_of(out);
    if (s < 0 || bin >= NBINS) return;
    const uint8_t *blk = out + (size_t)bin * BIN_BYTES;
    unsigned e0 = s * HALF_ROWS + bin * 64;
    res2_vis_bin(blk, &tbits[e0], &tflag[e0]);      /* compvis.h's results, scheduled for the A55 (res2.c) */
    if (count) __atomic_fetch_add(&bins_done, 1, __ATOMIC_RELAXED);
}

/* comp_bin() for a resolve that computes the entries itself (res2.c, while it writes the block): the bin's 64
 * entries, or 0 when the table is off or the output is not one of its frames (then comp_bin() as usual, a no-op) */
int comp_bin_table(uint8_t *sys, unsigned bin, uint8_t (**bits)[32], uint8_t **flags) {
    if (!use_tab || bin >= NBINS) return 0;
    int s = slot_of(PTR(sys, SYS_OUTPUT));
    if (s < 0) return 0;
    *bits = &tbits[s * HALF_ROWS + bin * 64];
    *flags = &tflag[s * HALF_ROWS + bin * 64];
    return 1;
}
void comp_bin_done(void) { __atomic_fetch_add(&bins_done, 1, __ATOMIC_RELAXED); }

void (*comp_uf4_done)(uint8_t *sys);

/* A frame without new geometry (every other frame of a game that draws its 3D at 30 fps, every frame of a still
 * scene): with threaded_3d update_frame_3d_4x then makes the unpublished frame a copy of the last rendered one,
 * memcpy(SYS_OUTPUT, SYS_LAST, 0xc0000) as its last act (a tail call), and video_3d_finish_rendering publishes
 * SYS_OUTPUT. 768 KiB through a 512 KiB cache, 30 times a second in HeartGold (measured on the RG DS Plus), for a
 * frame that already exists: here the copy is not made and SYS_OUTPUT is set to the last rendered frame instead, so
 * that one is published again. The next update still picks "the frame that is not published" to render into, and
 * nothing reads SYS_OUTPUT before that but video_3d_finish_rendering. Done through DraStic's GOT slot of memcpy:
 * only this one call, recognised by its arguments inside hook_uf4's call, is not passed on. RAST_FRAMECOPY=1 (or
 * DSFLIP_RAST_FRAMECOPY=1) keeps DraStic's copy. */
static void *(*ds_memcpy_real)(void *, const void *, size_t);
static __thread uint8_t *uf4_sys;       /* the system struct, while hook_uf4 is inside update_frame_3d_4x */
static unsigned long copies_skipped;
static void *ds_memcpy(void *d, const void *s, size_t n) {
    uint8_t *sys = uf4_sys;
    if (__builtin_expect(n == FRAME_BYTES && sys != 0, 0) && d == PTR(sys, SYS_OUTPUT) && s == PTR(sys, SYS_LAST)) {
        U64(sys, SYS_OUTPUT) = (uint64_t)(uintptr_t)s;
        __atomic_fetch_add(&copies_skipped, 1, __ATOMIC_RELAXED);
        return d;
    }
    return ds_memcpy_real(d, s, n);
}

static void hook_uf4(uint8_t *sys, uint32_t skip) {
    uintptr_t b = (uintptr_t)sys + SYS_FRAMEBUF;
    if (__atomic_load_n(&fb0, __ATOMIC_RELAXED) != b) { invalidate_all(); __atomic_store_n(&fb0, b, __ATOMIC_RELAXED); }
    int threaded = U32(PTR(sys, SYS_CFG), CFG_THREADED_3D) != 0;
    int was[2] = { __atomic_load_n(&tvalid[0], __ATOMIC_ACQUIRE), __atomic_load_n(&tvalid[1], __ATOMIC_ACQUIRE) };
    if (threaded) {                                              /* it writes the frame that is not published */
        int p = slot_of(PTR(sys, SYS_PUBLISHED));
        for (int s = 0; s < 2; s++) if (s != p) set_valid(s, 0);
    }
    unsigned long n0 = __atomic_load_n(&bins_done, __ATOMIC_ACQUIRE);
    uf4_sys = sys; orig_uf4(sys, skip); uf4_sys = 0;
    unsigned long n = __atomic_load_n(&bins_done, __ATOMIC_ACQUIRE) - n0;
    if (comp_uf4_done && n == NBINS) comp_uf4_done(sys);
    uint8_t *o = PTR(sys, SYS_OUTPUT), *last = PTR(sys, SYS_LAST);
    int so = slot_of(o), sl = slot_of(last);
    if (so < 0) { invalidate_all(); return; }
    if (n) {
        if (n != NBINS) { set_valid(so, 0); return; }
        /* rendered: the gap passes may have re-marked rows 32k-1 and 32k after the bins */
        for (unsigned k = 1; k < NBINS; k++)
            for (unsigned h = (32 * k - 1) * 2; h < (32 * k + 1) * 2; h++)
                tflag[so * HALF_ROWS + h] = (uint8_t)comp_vis_neon(tbits[so * HALF_ROWS + h], (const uint32_t *)(o + h * 0x400));
        if (compcheck) { cc_table(so); __atomic_fetch_add(&cc_upd[0], 1, __ATOMIC_RELAXED); }
        set_valid(so, 1);
    } else if (threaded && o != last) {                          /* memcpy(o, last, FRAME_BYTES) */
        if (sl >= 0 && __atomic_load_n(&tvalid[sl], __ATOMIC_ACQUIRE)) {
            memcpy(tbits[so * HALF_ROWS], tbits[sl * HALF_ROWS], sizeof tbits / 2);
            memcpy(tflag + so * HALF_ROWS, tflag + sl * HALF_ROWS, HALF_ROWS);
            if (compcheck) cc_table(so);
            set_valid(so, 1);
        } else set_valid(so, 0);
        if (compcheck) __atomic_fetch_add(&cc_upd[1], 1, __ATOMIC_RELAXED);
    } else {                                                     /* nothing written */
        set_valid(so, was[so]);
        if (compcheck) __atomic_fetch_add(&cc_upd[2], 1, __ATOMIC_RELAXED);
    }
}

static void hook_uf1(uint8_t *sys, uint32_t skip) {
    invalidate_all(); orig_uf1(sys, skip); invalidate_all();
    if (compcheck) __atomic_fetch_add(&cc_upd[3], 1, __ATOMIC_RELAXED);
}
static void hook_reset(uint8_t *vb) { invalidate_all(); orig_reset(vb); invalidate_all(); }

/* ---- render_scanline_2d_composite: the 3D + backdrop quarters in one pass ---- */
/* the original's arguments: x0..x7 and two u32 on the stack (flags, line). The callers write the stack slots and
 * w6, w7 as 32-bit values; the upper halves of those registers and slots are undefined, so the parameters are
 * taken as u64 and cut to 32 bits before use (the original reads them as w registers / 32-bit loads too). */
typedef void (*composite_fn)(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers, const uint32_t *p3d,
                             uint8_t *alpha, uint64_t lmask, uint64_t bldcnt, uint64_t flags, uint64_t line);
typedef void (*prio_fn)(uint8_t *eng, uint8_t *vis, uint8_t *excl);
typedef void (*select_fn)(uint8_t *eng, uint8_t *out, uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                          uint8_t *alpha, uint64_t lmask);
static composite_fn orig_composite;                     /* the trampoline to DraStic's function */
#define DSF(type, off) ((type)(ds_base + (off)))

/* how a call is handled: the fused kinds, DraStic's select_pixels after our encoder call, or the original through
 * the trampoline (and why); also the cf_n[] slot of the counters */
enum { CF_ALL3D, CF_BACKDROP, CF_MIXED, CF_SELECT, CF_FLAGS, CF_NO3D, CF_LAYERS, CF_OVERLAP, CF_N };
#define CF_TRAMPOLINE(r) ((r) >= CF_FLAGS)

/* 0 when the call takes the simple path with the 3D layer, else the reason it goes to the original */
static inline int cf_reason(const uint8_t *out, const uint8_t *S, const uint32_t *p3d, uint64_t lmask, uint64_t flags) {
    if ((uint32_t)flags & 0xf) return CF_FLAGS;              /* blending (bits 0-2) or brightness (bit 3) */
    if (!p3d) return CF_NO3D;                                /* no 3D layer (engine B, BG0 not 3D) */
    if (((uint32_t)lmask & ~0x1eu) != 1) return CF_LAYERS;   /* BG0 off, or bits above OBJ (DraStic never sets them) */
    /* the original writes the planes (expand) before it reads the 3D pixels and BG0's mask (binary32): with
     * overlapping buffers the fused pass, which reads first, would differ. DraStic's buffers never overlap. */
    uintptr_t o = (uintptr_t)out, p = (uintptr_t)p3d, x = (uintptr_t)S + S_EXCL;
    if ((o < p + 0x400 && p < o + 0x300) || (o < x + 0xc0 && x < o + 0x300)) return CF_OVERLAP;
    return 0;
}

/* the simple path: DraStic's priority encoder (so the masks in S are its bytes), then the fused planes, or
 * DraStic's select_pixels with the arguments the original passes it. Returns the cf_n[] slot. */
static inline int cf_run(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers, const uint32_t *p3d, uint64_t lmask) {
    DSF(prio_fn, DS_PRIORITY_ENCODE_SINGLE)(eng, S + S_VIS, S + S_EXCL);
    int kind = comp_fused_kind(S + S_EXCL, (uint32_t)lmask);
    if (kind == CF_KIND_NONE) {
        DSF(select_fn, DS_SELECT_PIXELS)(eng, out, S + S_EXCL, layers, p3d, 0, (uint32_t)lmask);
        return CF_SELECT;
    }
    comp_fused_planes(out, p3d, S + S_EXCL, **(const uint16_t **)(eng + ENG_BACKDROP), kind);
    return kind - 1;                                         /* CF_ALL3D, CF_BACKDROP, CF_MIXED */
}

/* check mode: every call that does not go to the trampoline runs twice on the same input bytes: ours, then the
 * original through the trampoline, and everything either may write is compared: render_scanline_2d's whole stack
 * frame (S and the layer lines; the original writes only the encoder's masks in it), the planes with 0x40 bytes
 * either side, the 3D pixels (which may lie in S: a BG0HOFS-shifted copy) and the engine's first 0x400 bytes.
 * Counts per slot, printed every 2 s. */
static pthread_mutex_t cf_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cf_n[CF_N], cf_checked, cf_bad;
static struct timespec cf_last;
#define CF_OUT_PAD 0x40
#define CF_ENG_BYTES 0x400
static uint8_t cf_in_fr[S_FRAME_SIZE], cf_in_out[0x300 + 2 * CF_OUT_PAD], cf_in_px[0x400], cf_in_eng[CF_ENG_BYTES];
static uint8_t cf_our_fr[S_FRAME_SIZE], cf_our_out[0x300 + 2 * CF_OUT_PAD], cf_our_px[0x400], cf_our_eng[CF_ENG_BYTES];

static void cf_report(void) {                           /* under cf_lock, every 2 s */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - cf_last.tv_sec < 2) return;
    cf_last = now;
    fprintf(stderr, "[comp] composite: %lu fused (%lu all 3D, %lu all backdrop, %lu mixed), %lu via select_pixels "
            "(another layer shows), %lu to DraStic's (%lu flags, %lu no 3D, %lu layer mask, %lu overlap); "
            "%lu checked, %lu differ\n", cf_n[CF_ALL3D] + cf_n[CF_BACKDROP] + cf_n[CF_MIXED], cf_n[CF_ALL3D],
            cf_n[CF_BACKDROP], cf_n[CF_MIXED], cf_n[CF_SELECT], cf_n[CF_FLAGS] + cf_n[CF_NO3D] + cf_n[CF_LAYERS] + cf_n[CF_OVERLAP],
            cf_n[CF_FLAGS], cf_n[CF_NO3D], cf_n[CF_LAYERS], cf_n[CF_OVERLAP], cf_checked, cf_bad);
}

/* the first differing byte of a region, for the log */
static void cf_diff(const char *what, const uint8_t *ref, const uint8_t *ours, size_t n, long base) {
    for (size_t i = 0; i < n; i++)
        if (ref[i] != ours[i]) {
            fprintf(stderr, "   %s %+ld: DraStic %02x, ours %02x\n", what, (long)i + base, ref[i], ours[i]);
            return;
        }
}

static __attribute__((noinline)) void hook_composite_check(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers,
                                                           const uint32_t *p3d, uint8_t *alpha, uint64_t lmask,
                                                           uint64_t bldcnt, uint64_t flags, uint64_t line) {
    int why = cf_reason(out, S, p3d, lmask, flags);
    if (why) {
        orig_composite(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line);
        pthread_mutex_lock(&cf_lock);
        cf_n[why]++;
        cf_report();
        pthread_mutex_unlock(&cf_lock);
        return;
    }
    uint8_t *fr = S - S_FRAME_BELOW, *ow = out - CF_OUT_PAD, *px = (uint8_t *)p3d;
    pthread_mutex_lock(&cf_lock);
    memcpy(cf_in_fr, fr, S_FRAME_SIZE); memcpy(cf_in_out, ow, sizeof cf_in_out);
    memcpy(cf_in_px, px, 0x400); memcpy(cf_in_eng, eng, CF_ENG_BYTES);
    int slot = cf_run(eng, out, S, layers, p3d, lmask);
    memcpy(cf_our_fr, fr, S_FRAME_SIZE); memcpy(cf_our_out, ow, sizeof cf_our_out);
    memcpy(cf_our_px, px, 0x400); memcpy(cf_our_eng, eng, CF_ENG_BYTES);
    /* the same input bytes again (p3d may lie in the frame: both copies hold the same bytes there) */
    memcpy(fr, cf_in_fr, S_FRAME_SIZE); memcpy(ow, cf_in_out, sizeof cf_in_out);
    memcpy(px, cf_in_px, 0x400); memcpy(eng, cf_in_eng, CF_ENG_BYTES);
    orig_composite(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line);
    int bad = memcmp(fr, cf_our_fr, S_FRAME_SIZE) || memcmp(ow, cf_our_out, sizeof cf_our_out) ||
              memcmp(px, cf_our_px, 0x400) || memcmp(eng, cf_our_eng, CF_ENG_BYTES);
    cf_n[slot]++; cf_checked++;
    if (bad && cf_bad++ < 30) {
        static const char *kn[] = { "all 3D", "all backdrop", "mixed", "select_pixels" };
        fprintf(stderr, "[compcheck] render_scanline_2d_composite(out %p, S %p, p3d %p, lmask %#x, flags %#x) %s path "
                "differs from DraStic's:\n", (void *)out, (void *)S, (const void *)p3d, (uint32_t)lmask, (uint32_t)flags, kn[slot]);
        cf_diff("planes", ow, cf_our_out, sizeof cf_our_out, -CF_OUT_PAD);
        cf_diff("S", fr, cf_our_fr, S_FRAME_SIZE, -S_FRAME_BELOW);
        cf_diff("p3d", px, cf_our_px, 0x400, 0);
        cf_diff("eng", eng, cf_our_eng, CF_ENG_BYTES, 0);
    }
    /* leave our result in place, as without the check */
    memcpy(fr, cf_our_fr, S_FRAME_SIZE); memcpy(ow, cf_our_out, sizeof cf_our_out);
    memcpy(px, cf_our_px, 0x400); memcpy(eng, cf_our_eng, CF_ENG_BYTES);
    cf_report();
    pthread_mutex_unlock(&cf_lock);
}

static void hook_composite(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers, const uint32_t *p3d,
                           uint8_t *alpha, uint64_t lmask, uint64_t bldcnt, uint64_t flags, uint64_t line) {
    if (__builtin_expect(compcheck, 0)) { hook_composite_check(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line); return; }
    if (cf_reason(out, S, p3d, lmask, flags)) { orig_composite(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line); return; }
    cf_run(eng, out, S, layers, p3d, lmask);
}
static const uint32_t expect_vis[4] = { 0xa9aa7bfd, 0x910003fd, 0xa90153f3, 0xf0000914 };
static const uint32_t expect_uf[4] = { 0xa9ba7bfd, 0x914d3402, 0x911b0042, 0x910003fd };    /* both update_frame_3d_* */
static const uint32_t expect_reset[4] = { 0xa9bd7bfd, 0x52800001, 0x910003fd, 0xa90153f3 };
static const uint32_t expect_composite[4] = { 0xa9b67bfd, 0xaa0303e8, 0x910003fd, 0xa9025bf5 };

void comp_init(void) {
    const char *e = getenv("DSFLIP_RAST_COMP");
    if (!e) e = getenv("RAST_COMP");
    comp_mode = e ? atoi(e) : 2;
    compcheck = getenv("RAST_COMPCHECK") && atoi(getenv("RAST_COMPCHECK"));
    if (comp_mode <= 0) return;
    if (comp_mode >= 2) {
        /* the table needs all three wrappers; any unknown entry leaves it off */
        orig_uf4 = (void (*)(uint8_t *, uint32_t))rast_hook(DS_UPDATE_FRAME_3D_4X, expect_uf, (void *)hook_uf4);
        if (orig_uf4) orig_uf1 = (void (*)(uint8_t *, uint32_t))rast_hook(DS_UPDATE_FRAME_3D_1X, expect_uf, (void *)hook_uf1);
        if (orig_uf1) orig_reset = (void (*)(uint8_t *))rast_hook(DS_RESET_VIDEO_3D, expect_reset, (void *)hook_reset);
        if (!orig_uf4 || !orig_uf1 || !orig_reset) {
            fprintf(stderr, "[comp] cannot hook the frame updates, table off\n");
            comp_mode = 1;
            /* a hooked update wrapper without the table is a plain pass-through: use_tab stays 0 */
        }
    }
    if (!rast_hook(DS_SET_3D_VISIBILITY, expect_vis, (void *)hook_vis)) {
        fprintf(stderr, "[comp] unknown render_scanline_set_3d_visibility, compositing hooks off\n");
        return;
    }
    use_tab = comp_mode >= 2;
    if (use_tab) {
        const char *k = getenv("DSFLIP_RAST_FRAMECOPY");
        if (!k) k = getenv("RAST_FRAMECOPY");
        if (!(k && atoi(k))) {
            ds_memcpy_real = (void *(*)(void *, const void *, size_t))ds_got_patch(DS_GOT_MEMCPY, (void *)ds_memcpy);
            if (ds_memcpy_real) fprintf(stderr, "[comp] a frame without new geometry publishes the last one again (no copy)\n");
        }
    }
    if (compcheck) { clock_gettime(CLOCK_MONOTONIC, &cc_last); cf_last = cc_last; }
    fprintf(stderr, "[comp] set_3d_visibility replaced (NEON%s)%s\n", use_tab ? " + per-bin table" : "",
            compcheck ? ", checking every call" : "");
    const char *f = getenv("DSFLIP_RAST_COMPFUSE");
    if (!f) f = getenv("RAST_COMPFUSE");
    if (f && !atoi(f)) return;
    orig_composite = (composite_fn)rast_hook(DS_2D_COMPOSITE, expect_composite, (void *)hook_composite);
    if (!orig_composite) { fprintf(stderr, "[comp] unknown render_scanline_2d_composite, fused path off\n"); return; }
    fprintf(stderr, "[comp] render_scanline_2d_composite hooked: 3D + backdrop quarters fused%s\n",
            compcheck ? ", checking every call against DraStic's" : "");
}
