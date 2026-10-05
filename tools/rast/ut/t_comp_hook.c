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
 * Then hook_composite() (the render_scanline_2d_composite hook, with the real DraStic function as its "trampoline")
 * against that function on random engines, scratch areas and quarters: every path (the three fused kinds, DraStic's
 * select_pixels after our encoder call, the original for no 3D layer / another layer mask, which passes all ten
 * arguments on), with the check mode on and off (it must find no difference and leave our result). Last, the real
 * function is patched as comp_init() patches it (a copy of rast.c's rast_hook) and called through its entry: the
 * jump to the hook, the arguments as a caller leaves them, and the trampoline back into the original, against the
 * C port.
 * run.sh t_comp_hook.c ../../../src/rast/spec/composite.c ../../../src/rast/spec/2d/compose.c */
#include "ut.h"
#include <stddef.h>
#include <sys/mman.h>
#include "comp.c"
#include "spec/2d/compose.h"

uintptr_t ds_base;
/* rast.c's rast_hook(), copied (rast.c is not linked here): the 4 original words into an executable buffer with a jump
 * back to the 5th, and the entry rewritten to jump to the hook; returns the trampoline. test_patched_entry() uses it
 * on the real render_scanline_2d_composite, as comp_init() does. */
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

/* ===================================================================================================================
 * hook_composite vs render_scanline_2d_composite
 * =================================================================================================================== */
#define DS_COMP 0x3c6d0
#define G 64
#define S_SIZE 0x1d30                                       /* render_scanline_2d's frame; S = sp + 0x180 */
#define S_OFS 0x180
struct env {
    uint8_t eng[0x400];                                     /* the check mode snapshots 0x400 bytes of it */
    uint8_t frame[S_SIZE + G];
    uint8_t out[G + 0x300 + G];
    uint8_t alpha[G + 0x100 + G];
    uint32_t px[256 + 16];
    uint16_t bd[8];
    uint8_t *layers[16];
    uint8_t extra[11][0x220];
};
static struct env EA __attribute__((aligned(64))), EB __attribute__((aligned(64)));

/* a 256-bit mask: per word zero / all ones / random / sparse, or spans */
static void fill_mask(uint8_t *m, int mode) {
    if (mode == 4) {
        memset(m, 0, 32);
        int x = rnd(256);
        while (x < 256) { int n = 1 + rnd(rnd(2) ? 8 : 100); for (int k = 0; k < n && x < 256; k++, x++) m[x >> 3] |= 1 << (x & 7); x += rnd(80); }
        return;
    }
    for (int w = 0; w < 8; w++) {
        int wm = mode < 4 ? mode : rnd(4);
        uint32_t v = wm == 0 ? 0 : wm == 1 ? ~0u : wm == 2 ? (uint32_t)rnd64() : (1u << rnd(32)) | (rnd(2) ? 1u << rnd(32) : 0);
        memcpy(m + 4 * w, &v, 4);
    }
}

static void fill_env(void) {
    struct env *e = &EA;
    rndfill(e, sizeof *e);
    uint8_t *S = e->frame + S_OFS;
    /* the priority list as video_2d_reorder_layers builds it (BG0 first among equals), or anything */
    int obj = rnd(2), on = rnd(16) | (rnd(2) ? 1 : 0), pr[4] = { rnd(4), rnd(4), rnd(4), rnd(4) };
    unsigned n = 0;
    if (rnd(6)) {
        for (int p = 0; p < 4; p++) {
            if (obj) e->eng[0x84 + n++] = 4 + p;
            for (int bg = 0; bg < 4; bg++) if ((on >> bg & 1) && pr[bg] == p) e->eng[0x84 + n++] = bg;
        }
    } else { n = rnd(9); for (unsigned k = 0; k < n; k++) e->eng[0x84 + k] = rnd(8); }
    e->eng[0xb3] = n;
    *(uint16_t **)(e->eng + 0x18) = e->bd + rnd(8);
    /* visibility: BG0 full / empty / mixed (as 3D quarters are), the other layers mostly empty (an overlay now and then) */
    int m0 = rnd(5);
    for (int s = 0; s < 8; s++) fill_mask(S + 0xda0 + 32 * s, s == 0 ? (m0 == 4 ? 5 : m0) : rnd(4) ? 0 : (int)rnd(6));
    for (int k = 0; k < 16; k++) e->layers[k] = k < 5 ? S + 0x1e0 + 0x220 * k : e->extra[k - 5];
    int pmode = rnd(4);
    for (int i = 0; i < 256 + 16; i++) {
        uint32_t a = pmode == 0 ? 0x1f : pmode == 1 ? (rnd(4) ? 0x1f : 0) : pmode == 2 ? rnd(32) : (uint32_t)rnd(256);
        e->px[i] = (pmode == 3 ? (uint32_t)rnd64() & 0xffffff : rnd(64) | rnd(64) << 8 | rnd(64) << 16) | a << 24;
    }
}
static void copy_env(void) {                                /* EB = EA with every pointer into EA moved to EB */
    memcpy(&EB, &EA, sizeof EA);
    uintptr_t a = (uintptr_t)&EA, b = (uintptr_t)&EB;
    uint8_t **pp = (uint8_t **)(EB.eng + 0x18);
    *pp = *pp - a + b;
    for (int k = 0; k < 16; k++) EB.layers[k] = EB.layers[k] - a + b;
}
static void *rel(void *p) { return p ? (uint8_t *)p - (uintptr_t)&EA + (uintptr_t)&EB : 0; }
static int cmp_env(const char *what) {                      /* every byte but the pointer fields */
    uint8_t keep[8];
    memcpy(keep, EB.eng + 0x18, 8); memcpy(EB.eng + 0x18, EA.eng + 0x18, 8);
    int bad = ut_cmp(what, &EA, &EB, offsetof(struct env, layers)) || ut_cmp(what, EA.extra, EB.extra, sizeof EA.extra);
    memcpy(EB.eng + 0x18, keep, 8);
    return bad;
}

typedef void (*comp_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t **, const uint32_t *, uint8_t *, uint64_t, uint64_t,
                        uint64_t, uint64_t);
static unsigned n_slot[CF_N], n_checkmode;

static void test_hook_composite(void) {
    char what[160];
    ds_base = ut_base;                                      /* DSF(): the encoder and select_pixels are DraStic's */
    orig_composite = DS(comp_fn, DS_COMP);                  /* no patch in this process: the function itself */
    clock_gettime(CLOCK_MONOTONIC, &cf_last);
    for (int t = 0; t < 6000 && ut_fail < 10; t++) {
        fill_env();
        uint8_t *S = EA.frame + S_OFS;
        int k = rnd(16);
        uint32_t lmask = k < 8 ? 1 : k < 12 ? 1 | (rnd(32) & 0x1e) : k < 14 ? rnd(32) : rnd(0x10000);   /* layers[] has 16 entries */
        int hofs = rnd(5) == 0;                             /* BG0HOFS: the quarter is a shifted copy at S + 0 */
        const uint32_t *p3 = rnd(10) ? (hofs ? (uint32_t *)S : EA.px + rnd(16)) : 0;
        if (hofs) memcpy(S, EA.px, 0x400);
        uint8_t *al = rnd(2) ? EA.alpha + G : 0;
        /* flags: the simple path (bit 4 set or not; bits 0-3 take DraStic's blending paths, which read state this
         * environment does not model: the hook passes those calls on untouched, like the no-3D and layer-mask cases
         * tested here), with garbage above bit 31 as the stack slot's upper half may hold */
        uint64_t flags = (uint64_t)rnd(2) << 4;
        if (rnd(2)) flags |= rnd64() << 32;
        uint64_t line = rnd64(), bld = rnd64();
        uint64_t lm64 = lmask | (rnd(2) ? rnd64() << 32 : 0);
        copy_env();
        compcheck = rnd(3) == 0;
        n_checkmode += compcheck;
        unsigned long bad0 = cf_bad, chk0 = cf_checked;
        DS(comp_fn, DS_COMP)(EA.eng, EA.out + G, S, EA.layers, p3, al, lm64, bld, flags, line);
        int why = cf_reason(EB.out + G, EB.frame + S_OFS, rel((void *)p3), lm64, flags);
        int slot = why;
        if (!why) {                                         /* what cf_run will pick, from the masks the encoder leaves */
            uint8_t ex[0xc0];
            memcpy(ex, EB.frame + S_OFS + 0x10c0, 0xc0);     /* (the slots of BGs not in the list keep their bytes) */
            spec_render_scanline_priority_encode_single(EB.eng, EB.frame + S_OFS + 0xda0, ex);
            int kind = comp_fused_kind(ex, lmask);
            slot = kind == CF_KIND_NONE ? CF_SELECT : kind - 1;
        }
        hook_composite(EB.eng, EB.out + G, EB.frame + S_OFS, EB.layers, rel((void *)p3), rel(al), lm64, bld, flags, line);
        n_slot[slot]++;
        snprintf(what, sizeof what, "hook_composite test %d (slot %d, lmask %#x, p3d %s, flags %#llx, check %d)", t, slot, lmask,
                 p3 ? (hofs ? "in S" : "yes") : "NULL", (unsigned long long)flags, compcheck);
        if (cmp_env(what)) continue;
        if (cf_bad != bad0) { fprintf(stderr, "FAIL %s: the check mode reported a difference\n", what); ut_fail++; }
        if (compcheck && !why && cf_checked == chk0) { fprintf(stderr, "FAIL %s: the check mode did not check\n", what); ut_fail++; }
    }
    compcheck = 0;
}

/* ===================================================================================================================
 * the patched entry and the trampoline: comp_init()'s patch on the real render_scanline_2d_composite
 * =================================================================================================================== */
/* The real function is patched as comp_init() patches it (rast_hook above, the same expected words), so a call of
 * DraStic's entry from C now takes the jump to hook_composite() with the ten arguments as a caller leaves them (the
 * last two in the caller's stack slots, with garbage in their upper halves as DraStic's 32-bit stores leave it), and
 * the calls the hook passes on run the original through the trampoline (the 4 copied words, then the jump back to
 * +16, where the original reads the flags from [sp + 0xa0]). Every call is compared against the C port of the simple
 * path (which t_composite.c tests against the unpatched original). The real function is gone afterwards, so this
 * test runs last. */
static unsigned n_pe[CF_N], n_pe_check;
static void test_patched_entry(void) {
    char what[160];
    uint32_t *entry = (uint32_t *)(ds_base + DS_2D_COMPOSITE);
    orig_composite = (composite_fn)rast_hook(DS_2D_COMPOSITE, expect_composite, (void *)hook_composite);
    if (!orig_composite) { fprintf(stderr, "FAIL patched entry: the first words of render_scanline_2d_composite are not the expected ones\n"); ut_fail++; return; }
    /* the patch and the trampoline, word for word */
    const uint32_t *t = (const uint32_t *)orig_composite;
    void *hk = (void *)hook_composite, *back = entry + 4;
    if (entry[0] != 0x58000050 || entry[1] != 0xd61f0200 || memcmp(entry + 2, &hk, 8)) { fprintf(stderr, "FAIL patched entry: entry words\n"); ut_fail++; }
    if (memcmp(t, expect_composite, 16) || t[4] != 0x58000050 || t[5] != 0xd61f0200 || memcmp(t + 6, &back, 8)) { fprintf(stderr, "FAIL patched entry: trampoline words\n"); ut_fail++; }
    for (int tt = 0; tt < 4000 && ut_fail < 10; tt++) {
        fill_env();
        uint8_t *S = EA.frame + S_OFS;
        int k = rnd(16);
        uint32_t lmask = k < 8 ? 1 : k < 12 ? 1 | (rnd(32) & 0x1e) : k < 14 ? rnd(32) : rnd(0x10000);
        int hofs = rnd(5) == 0;
        const uint32_t *p3 = rnd(10) ? (hofs ? (uint32_t *)S : EA.px + rnd(16)) : 0;
        if (hofs) memcpy(S, EA.px, 0x400);
        uint8_t *al = rnd(2) ? EA.alpha + G : 0;
        uint64_t flags = (uint64_t)rnd(2) << 4;
        if (rnd(2)) flags |= rnd64() << 32;
        uint64_t line = rnd64(), bld = rnd64();
        uint64_t lm64 = lmask | (rnd(2) ? rnd64() << 32 : 0);
        copy_env();
        compcheck = rnd(3) == 0;
        n_pe_check += compcheck;
        unsigned long bad0 = cf_bad, chk0 = cf_checked;
        int why = cf_reason(EA.out + G, S, p3, lm64, flags), slot = why;
        if (!why) {
            uint8_t ex[0xc0];
            memcpy(ex, S + 0x10c0, 0xc0);
            spec_render_scanline_priority_encode_single(EA.eng, S + 0xda0, ex);
            int kind = comp_fused_kind(ex, lmask);
            slot = kind == CF_KIND_NONE ? CF_SELECT : kind - 1;
        }
        DS(comp_fn, DS_COMP)(EA.eng, EA.out + G, S, EA.layers, p3, al, lm64, bld, flags, line);   /* the patched entry */
        spec_render_scanline_2d_composite(EB.eng, EB.out + G, EB.frame + S_OFS, EB.layers, rel((void *)p3), rel(al), lmask,
                                          (uint32_t)bld, (uint32_t)flags, (uint32_t)line);
        n_pe[slot]++;
        snprintf(what, sizeof what, "patched entry test %d (slot %d, lmask %#x, p3d %s, flags %#llx, check %d)", tt, slot, lmask,
                 p3 ? (hofs ? "in S" : "yes") : "NULL", (unsigned long long)flags, compcheck);
        if (cmp_env(what)) continue;
        if (cf_bad != bad0) { fprintf(stderr, "FAIL %s: the check mode reported a difference\n", what); ut_fail++; }
        if (compcheck && !why && cf_checked == chk0) { fprintf(stderr, "FAIL %s: the check mode did not check\n", what); ut_fail++; }
    }
    compcheck = 0;
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
    test_hook_composite();
    fprintf(stderr, "t_comp_hook: composite hook: %u all 3D, %u all backdrop, %u mixed, %u select_pixels, %u flags, %u no 3D, "
            "%u layer mask, %u overlap; %u in check mode (%lu checked, %lu differ)\n", n_slot[CF_ALL3D], n_slot[CF_BACKDROP],
            n_slot[CF_MIXED], n_slot[CF_SELECT], n_slot[CF_FLAGS], n_slot[CF_NO3D], n_slot[CF_LAYERS], n_slot[CF_OVERLAP],
            n_checkmode, cf_checked, cf_bad);
    test_patched_entry();
    fprintf(stderr, "t_comp_hook: patched entry: %u all 3D, %u all backdrop, %u mixed, %u select_pixels, %u flags, %u no 3D, "
            "%u layer mask, %u overlap (the last four through the trampoline); %u in check mode (%lu checked, %lu differ)\n",
            n_pe[CF_ALL3D], n_pe[CF_BACKDROP], n_pe[CF_MIXED], n_pe[CF_SELECT], n_pe[CF_FLAGS], n_pe[CF_NO3D], n_pe[CF_LAYERS],
            n_pe[CF_OVERLAP], n_pe_check, cf_checked, cf_bad);
}
