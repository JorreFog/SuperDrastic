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
 *    output block is written: 64 half-rows of 0x400 bytes, rows y at +y*0x800, parity p at +p*0x400) into a table
 *    per output frame; the hook copies the table entry when the pointer is a half-row of a frame whose table is
 *    valid, and computes with NEON otherwise.
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
 * RAST_COMP (or DSFLIP_RAST_COMP): 0 off (DraStic's function), 1 the NEON replacement only, 2 (default) with the
 * table. */
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
    for (unsigned h = 0; h < 64; h++)
        tflag[e0 + h] = (uint8_t)comp_vis_neon(tbits[e0 + h], (const uint32_t *)(blk + h * 0x400));
    if (count) __atomic_fetch_add(&bins_done, 1, __ATOMIC_RELAXED);
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
    orig_uf4(sys, skip);
    unsigned long n = __atomic_load_n(&bins_done, __ATOMIC_ACQUIRE) - n0;
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

static const uint32_t expect_vis[4] = { 0xa9aa7bfd, 0x910003fd, 0xa90153f3, 0xf0000914 };
static const uint32_t expect_uf[4] = { 0xa9ba7bfd, 0x914d3402, 0x911b0042, 0x910003fd };    /* both update_frame_3d_* */
static const uint32_t expect_reset[4] = { 0xa9bd7bfd, 0x52800001, 0x910003fd, 0xa90153f3 };

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
    if (compcheck) clock_gettime(CLOCK_MONOTONIC, &cc_last);
    fprintf(stderr, "[comp] set_3d_visibility replaced (NEON%s)%s\n", use_tab ? " + per-bin table" : "",
            compcheck ? ", checking every call" : "");
}
