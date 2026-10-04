/* t_comp_hook.c: comp.c's hook layer against DraStic's render_scanline_set_3d_visibility, on a fake system struct:
 * the table lookup (which pointers hit, which fall back to NEON), comp_bin()'s entry mapping, and the frame-update
 * wrappers' validity rule (hook_uf4: rendered with the gap rows re-marked after the bins, copied, unchanged;
 * hook_uf1; hook_reset), with threaded_3d on and off and publications in between. The wrapped originals are
 * stubs that write the frames the way the disassembly shows DraStic's do (update_frame_3d_4x 0x58f10: the unpublished
 * frame with threaded_3d, the bins, then rows 32k-1 and 32k; or memcpy(OUTPUT, LAST) when it does not render).
 * After every step: every valid table equals the original on its frame, and hook_vis() equals the original for
 * half-rows of both frames, other offsets inside them and a buffer outside.
 * Why a unit test as well as RAST_COMPCHECK: in the test ROMs the 3D layer is opaque nearly everywhere, so a stale
 * table entry usually still gives the right answer there and the check mode hardly sees the bookkeeping fail (a build
 * without the table copy of the frame-copying update passed the frame-skipping scene ROM's check); here every
 * half-row is random, so any stale entry shows. comp.c is included to reach its static state.
 * run.sh t_comp_hook.c ../../../src/rast/spec/composite.c */
#include "ut.h"
#include "comp.c"

uintptr_t ds_base;
void *rast_hook(uintptr_t off, const uint32_t expect[4], void *to) { (void)off; (void)expect; (void)to; return 0; }

typedef uint32_t (*vis_fn)(uint8_t *, const uint32_t *);
#define DS_VIS 0x3c2c0

static uint8_t *sys, *cfg, *f0, *f1;
static uint32_t outside[256 + 64];
static unsigned n_hit, n_neon, n_checked, n_valid_checked, n_ops[8];

/* a half-row's pixels: alpha by a random mode (empty, opaque, opaque/empty mix, 5-bit, any byte, a few translucent) */
static void fill_half(uint32_t *px) {
    int mode = rnd(6);
    for (int i = 0; i < 256; i++) {
        uint8_t a;
        switch (mode) {
        case 0: a = 0; break;
        case 1: a = 0x1f; break;
        case 2: a = rnd(3) ? 0x1f : 0; break;
        case 3: a = rnd(32); break;
        case 4: a = rnd(256); break;
        default: a = rnd(8) ? 0x1f : (uint8_t)rnd(31); break;
        }
        px[i] = (uint32_t)rnd64() & 0xffffff;
        px[i] |= (uint32_t)a << 24;
    }
}

/* ---- stubs of the wrapped originals ---- */
static int st_render;                 /* the stub update renders (else: copy with threaded_3d, or nothing) */
static void stub_uf4(uint8_t *s, uint32_t skip) {
    int threaded = U32(PTR(s, SYS_CFG), CFG_THREADED_3D) != 0;
    uint8_t *out;
    if (threaded) { out = PTR(s, SYS_PUBLISHED) == f0 ? f1 : f0; U64(s, SYS_OUTPUT) = (uint64_t)(uintptr_t)out; }
    else out = PTR(s, SYS_OUTPUT);
    if (!skip && st_render) {
        U64(s, SYS_LAST) = (uint64_t)(uintptr_t)out;
        comp_bins_begin(s);                                     /* as rast.c's hook_entry */
        for (unsigned b = 0; b < NBINS; b++) {
            for (unsigned h = 0; h < 64; h++) fill_half((uint32_t *)(out + b * BIN_BYTES + h * 0x400));
            comp_bin(s, b, 1);
        }
        /* the gap passes: rows 32k-1 and 32k get new alpha bytes (edge marking) */
        for (unsigned k = 1; k < NBINS; k++)
            for (unsigned y = 32 * k - 1; y <= 32 * k; y++)
                for (unsigned i = 0; i < 512; i++)
                    if (rnd(4) == 0) out[y * 0x800 + i * 4 + 3] = (uint8_t)(rnd(2) ? rnd(32) : 0x1f);
        return;
    }
    if (threaded) { uint8_t *last = PTR(s, SYS_LAST); if (last != out) memcpy(out, last, FRAME_BYTES); }
}
static void stub_uf1(uint8_t *s, uint32_t skip) {                 /* 1x frame lines, 0x400 apart */
    (void)skip;
    uint8_t *out = PTR(s, SYS_OUTPUT);
    for (unsigned l = 0; l < 192; l++) if (rnd(2)) fill_half((uint32_t *)(out + l * 0x400));
}
static void stub_reset(uint8_t *vb) {
    memset(vb, 0, 2 * FRAME_BYTES);
    U64(sys, SYS_OUTPUT) = (uint64_t)(uintptr_t)vb; U64(sys, SYS_PUBLISHED) = (uint64_t)(uintptr_t)(vb + FRAME_BYTES);
    U64(sys, SYS_LAST) = (uint64_t)(uintptr_t)vb;
}

static void check_ptr(const uint32_t *px, const char *what) {
    uint8_t o1[96], o2[96];
    unsigned oo = 16 + rnd(32);
    uint8_t pat = rnd(256);
    memset(o1, pat, sizeof o1); memset(o2, pat, sizeof o2);
    int e = tab_entry(px);
    uint32_t r1 = DS(vis_fn, DS_VIS)(o1 + oo, px);
    uint32_t r2 = hook_vis(o2 + oo, px);
    if (e >= 0) n_hit++; else n_neon++;
    n_checked++;
    if (ut_cmp(what, o1, o2, sizeof o1) || r1 != r2) {
        if (r1 != r2) { fprintf(stderr, "FAIL %s: returns %#x, hook %#x\n", what, r1, r2); ut_fail++; }
        fprintf(stderr, "   (%s, table entry %d)\n", what, e);
    }
}

static void check_all(const char *step) {
    char what[96];
    /* invariant: a valid table equals the original on every half-row of its frame */
    for (int s = 0; s < 2; s++) {
        if (!__atomic_load_n(&tvalid[s], __ATOMIC_ACQUIRE)) continue;
        const uint8_t *f = s ? f1 : f0;
        for (unsigned i = 0; i < HALF_ROWS && ut_fail < 10; i++) {
            uint8_t ref[32];
            uint32_t r = DS(vis_fn, DS_VIS)(ref, (const uint32_t *)(f + i * 0x400));
            n_valid_checked++;
            if (r != tflag[s * HALF_ROWS + i] || memcmp(ref, tbits[s * HALF_ROWS + i], 32)) {
                fprintf(stderr, "FAIL %s: table %d entry %u (row %u parity %u) differs from the frame\n", step, s, i, i / 2, i % 2);
                ut_fail++;
                break;
            }
        }
    }
    /* the hook on pointers DraStic passes: half-rows (2x quarters and 1x lines), other offsets, outside */
    for (int k = 0; k < 24 && ut_fail < 10; k++) {
        const uint8_t *f = rnd(2) ? f1 : f0;
        const uint32_t *px;
        switch (rnd(4)) {
        case 0: case 1: px = (const uint32_t *)(f + rnd(HALF_ROWS) * 0x400); break;
        case 2: px = (const uint32_t *)(f + rnd(HALF_ROWS - 1) * 0x400 + 4 * (1 + rnd(255))); break;
        default: fill_half(outside + 16); px = outside + 16; break;
        }
        snprintf(what, sizeof what, "%s ptr %+ld", step, (long)((const uint8_t *)px - f0));
        check_ptr(px, what);
    }
    /* the last half-row of frame 1 and the first of frame 0 */
    check_ptr((const uint32_t *)(f1 + FRAME_BYTES - 0x400), "last half-row");
    check_ptr((const uint32_t *)f0, "first half-row");
}

void ut_main(void) {
    size_t sz = SYS_LAST + 0x1000;
    sys = aligned_alloc(4096, sz); cfg = calloc(1, 0x1000);
    if (!sys || !cfg) { fprintf(stderr, "FAIL: no memory\n"); ut_fail++; return; }
    memset(sys, 0, sz);
    U64(sys, SYS_CFG) = (uint64_t)(uintptr_t)cfg;
    f0 = sys + SYS_FRAMEBUF; f1 = f0 + FRAME_BYTES;
    for (unsigned i = 0; i < 2 * HALF_ROWS; i++) fill_half((uint32_t *)(f0 + i * 0x400));
    U64(sys, SYS_OUTPUT) = (uint64_t)(uintptr_t)f0; U64(sys, SYS_PUBLISHED) = (uint64_t)(uintptr_t)f1;
    U64(sys, SYS_LAST) = (uint64_t)(uintptr_t)f0;
    orig_uf4 = stub_uf4; orig_uf1 = stub_uf1; orig_reset = stub_reset;
    comp_mode = 2; use_tab = 1; compcheck = 0;
    check_all("before any update");                         /* fb0 unset: every call computes */
    char step[64];
    for (int t = 0; t < 400 && ut_fail < 10; t++) {
        int op = rnd(16);
        if (op < 8) {                                            /* update_frame_3d_4x: render, skip or no change */
            st_render = rnd(3) != 0;
            uint32_t skip = rnd(5) == 0;
            hook_uf4(sys, skip);
            n_ops[st_render && !skip ? 0 : 1]++;
            snprintf(step, sizeof step, "step %d uf4 render %d skip %u thr %u", t, st_render, skip, U32(cfg, CFG_THREADED_3D));
        } else if (op < 12) {                                    /* video_3d_finish_rendering: publish OUTPUT */
            U64(sys, SYS_PUBLISHED) = U64(sys, SYS_OUTPUT);
            n_ops[2]++;
            snprintf(step, sizeof step, "step %d publish", t);
        } else if (op < 14) {                                    /* the menu toggles threaded_3d */
            U32(cfg, CFG_THREADED_3D) ^= 1;
            n_ops[3]++;
            snprintf(step, sizeof step, "step %d threaded_3d %u", t, U32(cfg, CFG_THREADED_3D));
        } else if (op == 14) {
            hook_uf1(sys, 0);
            n_ops[4]++;
            snprintf(step, sizeof step, "step %d uf1", t);
        } else {
            hook_reset(f0);
            n_ops[5]++;
            snprintf(step, sizeof step, "step %d reset", t);
        }
        check_all(step);
    }
    fprintf(stderr, "t_comp_hook: %u ops (%u rendered, %u not rendered, %u publish, %u toggles, %u 1x, %u resets); "
            "%u hook calls (%u from the table, %u computed); %u valid entries checked\n", 400, n_ops[0], n_ops[1], n_ops[2],
            n_ops[3], n_ops[4], n_ops[5], n_checked, n_hit, n_neon, n_valid_checked);
}
