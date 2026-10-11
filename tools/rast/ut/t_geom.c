/* t_geom.c: dsgeom.c's replacements against DraStic's originals, on a fake geometry state.
 * queue_geometry_command: random queue states (parameters left 0, 1, 2 or more; the swap flag; the command queue
 * empty, anywhere, and at the 0x400-byte limit where the original calls process_geometry; random cursors and
 * fields around them) and random words (valid commands, 0x50 and 0xd0, any 32-bit value). Where ours handles the
 * call it must leave exactly the original's bytes: the state's +0x9a00..+0x9b00, the command queue and the parameter
 * area are compared after ours and after DraStic's on copies. Where it does not, it must write nothing and pass the
 * call on unchanged (a stub stands in for the trampoline: the original would run process_geometry, which a fake state
 * cannot feed). Then the check mode (geom_queue_command_check, DraStic's function as its "trampoline": no differences
 * reported, the original's bytes left), and DraStic's entry patched as geom_init() patches it (a copy of rast.c's
 * rast_hook), called through that entry against the original through the trampoline.
 * geometry_add_vertex the same way (test_vertex(): the batch, its counts and the texture matrix and coordinates);
 * video_3d_bin_polygons_y_sort_4x in test_ysort(), geometry_transform_vertex_block_asm (geom_tvb.S) in test_tvb(),
 * queue_geometry_command_packed (GXFIFO words) in test_packed(), geometry_transform_vertexes in test_tvx().
 * run.sh t_geom.c ../../../src/rast/geom_tvb.S */
#include "ut.h"
#include <sys/mman.h>
#include "dsgeom.c"

uintptr_t ds_base;
/* rast.c's patch_jump() and rast_hook(), verbatim (keep them identical) */
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

#define GSIZE 0x84000                   /* the queue fields, the command queue (+0x79b00) and parameters (+0x81b00) */
#define PAR_WORDS 0x800
static uint8_t *ga, *gb, *g0;

static const unsigned cursors[4] = { G_CMD_CUR, G_PAR_CUR, G_CMD_PEND, G_PAR_PEND };
/* the regions a call may write, from src to dst, the cursors (pointers into src) moved to dst */
static void copy_state(uint8_t *dst, const uint8_t *src) {
    memcpy(dst + 0x9a00, src + 0x9a00, 0x100);
    memcpy(dst + G_QUEUE - 0x40, src + G_QUEUE - 0x40, 0x500);
    memcpy(dst + 0x81b00 - 0x40, src + 0x81b00 - 0x40, PAR_WORDS * 4 + 0x200);
    for (int i = 0; i < 4; i++) stp_(dst, cursors[i], (uint8_t *)ldp_(src, cursors[i]) - src + dst);
}
/* the regions compared, the cursors as offsets in their state */
static int cmp_state(const char *what, const uint8_t *ref, const uint8_t *ours) {
    uint8_t fr[0x100], fo[0x100];
    memcpy(fr, ref + 0x9a00, 0x100); memcpy(fo, ours + 0x9a00, 0x100);
    for (int i = 0; i < 4; i++) {
        uintptr_t a = (uintptr_t)ldp_(ref, cursors[i]) - (uintptr_t)ref, b = (uintptr_t)ldp_(ours, cursors[i]) - (uintptr_t)ours;
        memcpy(fr + cursors[i] - 0x9a00, &a, 8); memcpy(fo + cursors[i] - 0x9a00, &b, 8);
    }
    return ut_cmp(what, fr, fo, 0x100) ||
           ut_cmp(what, ref + G_QUEUE - 0x40, ours + G_QUEUE - 0x40, 0x500) ||
           ut_cmp(what, ref + 0x81b00 - 0x40, ours + 0x81b00 - 0x40, PAR_WORDS * 4 + 0x200);
}

static void rnd_state(uint8_t *g, uint32_t *cmd, uint32_t *param) {
    rndfill(g + 0x9a00, 0x100);
    rndfill(g + G_QUEUE - 0x40, 0x500);
    rndfill(g + 0x81b00 - 0x40, PAR_WORDS * 4 + 0x200);
    unsigned k = rnd(8);
    g[G_LEFT] = k < 3 ? 0 : k < 5 ? 1 : k < 7 ? 2 + rnd(3) : (uint8_t)rnd64();
    g[G_SWAP] = rnd(4) ? 0 : 1 + rnd(255);
    unsigned q = rnd(3) ? rnd(0x401) : 0x3f8 + rnd(16);     /* queued command bytes; near the limit a third of the time */
    stp_(g, G_CMD_CUR, g + G_QUEUE + q);
    stp_(g, G_CMD_PEND, rnd(2) ? g + G_QUEUE + q : g + G_QUEUE + rnd(0x401));
    uint8_t *pc = g + 0x81b00 + 4 * rnd(PAR_WORDS - 64);
    stp_(g, G_PAR_CUR, pc);
    stp_(g, G_PAR_PEND, rnd(2) ? pc + 4 * rnd(32) : g + 0x81b00 + 4 * rnd(PAR_WORDS - 64));
    k = rnd(8);
    *cmd = k < 4 ? rnd(0x80) : k == 4 ? 0x50 : k == 5 ? 0xd0 : k == 6 ? rnd(0x100) : (uint32_t)rnd64();
    *param = (uint32_t)rnd64();
}

/* the stand-in for the trampoline where ours passes the call on */
static uint8_t *fw_g; static uint32_t fw_cmd, fw_param; static int fw_n;
static void fw_stub(uint8_t *g, uint32_t cmd, uint32_t param) { fw_g = g; fw_cmd = cmd; fw_param = param; fw_n++; }

static void test_queue(int n) {
    geom_queue_fn *orig = DS(geom_queue_fn *, DS_QUEUE_GEOMETRY_COMMAND);
    unsigned handled[6] = { 0 }, fwd = 0;
    for (int i = 0; i < n && !ut_fail; i++) {
        uint32_t cmd, param;
        rnd_state(ga, &cmd, &param);
        copy_state(g0, ga);
        unsigned left = ga[G_LEFT], np = cmd_params[cmd & 0x7f];
        /* the replacement as installed, with the stub as the trampoline */
        geom_queue_orig = fw_stub; fw_n = 0;
        geom_queue_command(ga, cmd, param);
        if (fw_n) {
            if (fw_n != 1 || fw_g != ga || fw_cmd != cmd || fw_param != param) { fprintf(stderr, "FAIL queue: forwarded call's arguments\n"); ut_fail++; }
            cmp_state("queue: forwarded call wrote", g0, ga);
            /* forwarded only where the original runs process_geometry or the swap path */
            uint8_t *c = ldp_(g0, G_CMD_CUR);
            long qn = c + 1 - (g0 + G_QUEUE);
            int need = left == 1 ? qn > 0x400 : left ? 0 : g0[G_SWAP] || (cmd & 0x7f) == 0x50 || (np <= 1 && qn > 0x400);
            if (!need) { fprintf(stderr, "FAIL queue: forwarded a call ours handles (left %u cmd %#x)\n", left, cmd); ut_fail++; }
            fwd++;
            continue;
        }
        handled[left > 1 ? 0 : left ? 1 : np > 1 ? 2 : np ? 3 : 4]++;
        copy_state(gb, g0);
        orig(gb, cmd, param);
        if (cmp_state("queue: ours vs DraStic's", gb, ga))
            fprintf(stderr, "   left %u swap %u cmd %#x params %u queued %ld\n", left, g0[G_SWAP], cmd, np,
                    (long)((uint8_t *)ldp_(g0, G_CMD_CUR) - (g0 + G_QUEUE)));
        /* the check mode on the same input: DraStic's bytes, no difference counted */
        copy_state(ga, g0);
        geom_queue_orig = orig;
        unsigned long bad = qc_bad;
        geom_queue_command_check(ga, cmd, param);
        if (qc_bad != bad) { fprintf(stderr, "FAIL queue: the check mode reported a difference\n"); ut_fail++; }
        cmp_state("queue: check mode's result", gb, ga);
    }
    fprintf(stderr, "queue_geometry_command: %u parameter, %u last parameter, %u command with more, %u with one, %u with none "
            "equal to DraStic's; %u passed on\n", handled[0], handled[1], handled[2], handled[3], handled[4], fwd);
    if (!handled[0] || !handled[1] || !handled[2] || !handled[3] || !handled[4] || !fwd) { fprintf(stderr, "FAIL queue: a path not covered\n"); ut_fail++; }
}


/* queue_geometry_command_packed: the queue states of rnd_state() (cmd_pend apart from cmd_cur half the time, which
 * this path reads) and packed words whose four bytes are command ids with parameters, without (MTX_PUSH, MTX_IDENTITY,
 * END_VTXS), zero, or any byte; where ours handles the call the same regions as the queue test are compared, where
 * it passes the call on nothing may be written and the original would run process_geometry */
static const uint8_t pk_cmds[] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x20, 0x21, 0x22,
                                   0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41, 0x50, 0x60, 0x70, 0x71, 0x72 };
static uint32_t pk_word(void) {
    uint32_t w = 0;
    unsigned k = rnd(8);
    if (k == 7) return (uint32_t)rnd64();
    for (int i = 0; i < 4; i++) {
        unsigned b = k < 4 ? (rnd(3) ? pk_cmds[rnd(sizeof pk_cmds)] : 0) : k == 4 ? pk_cmds[rnd(sizeof pk_cmds)] : rnd(2) ? rnd(0x100) : 0;
        w |= b << (8 * i);
    }
    return w;
}
static void pfw_stub(uint8_t *g, uint32_t w) { fw_g = g; fw_cmd = w; fw_n++; }
static void test_packed(int n) {
    geom_packed_fn *orig = DS(geom_packed_fn *, DS_QUEUE_PACKED);
    unsigned handled[4] = { 0 }, fwd = 0;
    for (int i = 0; i < n && !ut_fail; i++) {
        uint32_t cmd, param;
        rnd_state(ga, &cmd, &param);
        uint32_t w = pk_word();
        copy_state(g0, ga);
        unsigned left = ga[G_LEFT];
        unsigned np = cmd_params[w & 0x7f] + cmd_params[w >> 8 & 0x7f] + cmd_params[w >> 16 & 0x7f] + cmd_params[w >> 24 & 0x7f];
        unsigned nc = (w & 0x7f ? 1 : 0) + (w >> 8 & 0x7f ? 1 : 0) + (w >> 16 & 0x7f ? 1 : 0) + (w >> 24 & 0x7f ? 1 : 0);
        geom_packed_orig = pfw_stub; fw_n = 0;
        geom_queue_packed(ga, w);
        if (fw_n) {
            if (fw_n != 1 || fw_g != ga || fw_cmd != w) { fprintf(stderr, "FAIL packed: forwarded call's arguments\n"); ut_fail++; }
            cmp_state("packed: forwarded call wrote", g0, ga);
            long qp = (uint8_t *)ldp_(g0, G_CMD_PEND) - (g0 + G_QUEUE), qc = (uint8_t *)ldp_(g0, G_CMD_CUR) + nc - (g0 + G_QUEUE);
            int need = left == 1 ? qp > 0x400 : left ? 0 : (np == 0 && qc > 0x400);
            if (!need) { fprintf(stderr, "FAIL packed: forwarded a call ours handles (left %u word %#x)\n", left, w); ut_fail++; }
            fwd++;
            continue;
        }
        handled[left > 1 ? 0 : left ? 1 : np ? 2 : 3]++;
        copy_state(gb, g0);
        orig(gb, w);
        if (cmp_state("packed: ours vs DraStic's", gb, ga))
            fprintf(stderr, "   left %u word %#x params %u commands %u queued %ld\n", left, w, np, nc, (long)((uint8_t *)ldp_(g0, G_CMD_CUR) - (g0 + G_QUEUE)));
        copy_state(ga, g0);
        geom_packed_orig = orig;
        unsigned long bad = pc_bad;
        geom_queue_packed_check(ga, w);
        if (pc_bad != bad) { fprintf(stderr, "FAIL packed: the check mode reported a difference\n"); ut_fail++; }
        cmp_state("packed: check mode's result", gb, ga);
    }
    fprintf(stderr, "queue_geometry_command_packed: %u parameter, %u last parameter, %u command word with parameters, %u without "
            "equal to DraStic's; %u passed on\n", handled[0], handled[1], handled[2], handled[3], fwd);
    if (!handled[0] || !handled[1] || !handled[2] || !handled[3] || !fwd) { fprintf(stderr, "FAIL packed: a path not covered\n"); ut_fail++; }
}

/* geometry_add_vertex: the batch and the flags random, the count 0..63, 64 (a full batch: passed on) or beyond, the
 * matrix flag set a quarter of the time (passed on), texture coordinate mode 3 half the time, coordinates as the
 * vertex commands give them (sign-extended 16-bit) or any 32-bit value */
static void vtx_state(uint8_t *g, int32_t *v) {
    rndfill(g, VC_LO); rndfill(g + VC_HI_AT, VC_HI);
    unsigned k = rnd(8);
    st32(g, G_BATCH_N, k < 6 ? rnd(64) : k == 6 ? 64 : 65 + rnd(0x200));
    g[G_MTX_DIRTY] = rnd(4) ? 0 : 1 + rnd(255);
    g[G_TEXMODE] = rnd(2) ? 3 : (uint8_t)rnd64();
    for (int i = 0; i < 3; i++) v[i] = rnd(2) ? (int16_t)rnd64() : (int32_t)rnd64();
}
static void vfw_stub(uint8_t *g, int32_t x, int32_t y, int32_t z) { fw_g = g; fw_cmd = x; fw_param = y; fw_n++; (void)z; }
static void test_vertex(int n) {
    geom_vertex_fn *orig = DS(geom_vertex_fn *, DS_GEOMETRY_ADD_VERTEX);
    unsigned handled[2] = { 0 }, fwd = 0;
    for (int i = 0; i < n && !ut_fail; i++) {
        int32_t v[3];
        vtx_state(ga, v);
        memcpy(g0, ga, VC_LO); memcpy(g0 + VC_HI_AT, ga + VC_HI_AT, VC_HI);
        geom_vertex_orig = vfw_stub; fw_n = 0;
        geom_add_vertex(ga, v[0], v[1], v[2]);
        if (fw_n) {
            if (fw_n != 1 || fw_g != ga || fw_cmd != (uint32_t)v[0] || fw_param != (uint32_t)v[1]) { fprintf(stderr, "FAIL vertex: forwarded call's arguments\n"); ut_fail++; }
            ut_cmp("vertex: forwarded call wrote", g0, ga, VC_LO) || ut_cmp("vertex: forwarded call wrote", g0 + VC_HI_AT, ga + VC_HI_AT, VC_HI);
            if (!g0[G_MTX_DIRTY] && ld32(g0, G_BATCH_N) != 64) { fprintf(stderr, "FAIL vertex: forwarded a call ours handles\n"); ut_fail++; }
            fwd++;
            continue;
        }
        handled[g0[G_TEXMODE] == 3]++;
        memcpy(gb, g0, VC_LO); memcpy(gb + VC_HI_AT, g0 + VC_HI_AT, VC_HI);
        orig(gb, v[0], v[1], v[2]);
        if (ut_cmp("vertex: ours vs DraStic's", gb, ga, VC_LO) || ut_cmp("vertex: ours vs DraStic's", gb + VC_HI_AT, ga + VC_HI_AT, VC_HI))
            fprintf(stderr, "   n %u mode %u v %d %d %d\n", (uint32_t)ld32(g0, G_BATCH_N), g0[G_TEXMODE], v[0], v[1], v[2]);
        memcpy(ga, g0, VC_LO); memcpy(ga + VC_HI_AT, g0 + VC_HI_AT, VC_HI);
        geom_vertex_orig = orig;
        unsigned long bad = vc_bad;
        geom_add_vertex_check(ga, v[0], v[1], v[2]);
        if (vc_bad != bad) { fprintf(stderr, "FAIL vertex: the check mode reported a difference\n"); ut_fail++; }
        ut_cmp("vertex: check mode's result", gb, ga, VC_LO) || ut_cmp("vertex: check mode's result", gb + VC_HI_AT, ga + VC_HI_AT, VC_HI);
    }
    fprintf(stderr, "geometry_add_vertex: %u (%u with texture coordinate mode 3) equal to DraStic's; %u passed on\n",
            handled[0] + handled[1], handled[1], fwd);
    if (!handled[0] || !handled[1] || !fwd) { fprintf(stderr, "FAIL vertex: a path not covered\n"); ut_fail++; }
}

/* video_3d_bin_polygons_y_sort_4x: DraStic's texture_cache_lookup and render_sprite_check.isra.0 are patched to
 * stubs (their callers, the original and ours, both go through those entries) that log every call and return or
 * write values derived from their arguments, so the calls' order and arguments are compared too. Random polygons:
 * 0..15 vertices (mostly 3 and 4), vertex y mostly in 0..0x1a0 with some up to 0xffff, vertex fields equal across a
 * polygon or not (the shading flags), texture keys repeating (the lookup cache), the sprite path's bit 6, the
 * polygon word's high bits; sometimes stale rows in DraStic's sort arrays. Everything both may write is compared:
 * the polygons, vertices, bins, the sort arrays and the stubs' logs. */
#define YT_MAXP 0x400
#define YT_LOG 4096
static uint64_t yt_log[YT_LOG]; static int yt_nlog;
static void *tex_stub(void *cache, uint32_t key, uint32_t key2) {
    if (yt_nlog < YT_LOG - 3) { yt_log[yt_nlog++] = 1; yt_log[yt_nlog++] = (uintptr_t)cache; yt_log[yt_nlog++] = (uint64_t)key << 32 | key2; }
    return (void *)(uintptr_t)(((uint64_t)key * 0x9e3779b1u) ^ key2 ^ 0x5500000000ull);
}
__attribute__((visibility("hidden"))) void sprite_stub(uint8_t *poly, uint8_t *verts) {
    if (yt_nlog < YT_LOG - 3) { yt_log[yt_nlog++] = 2; yt_log[yt_nlog++] = (uintptr_t)poly; yt_log[yt_nlog++] = (uintptr_t)verts; }
    uint32_t w; memcpy(&w, poly + 8, 4);
    w ^= (uint32_t)ld16(verts, 0x16) << 4 & 0x80;              /* like the real one: a flag from the vertices */
    memcpy(poly + 8, &w, 4);
}
/* the real render_sprite_check.isra.0 writes only x0-x10 (and flags), and the original keeps the last texture in x11
 * across the call: the stub keeps x11-x18 as it does */
__attribute__((naked)) static void sprite_stub_asm(void) {
    __asm__("stp x29, x30, [sp, #-80]!\n mov x29, sp\n stp x11, x12, [sp, #16]\n stp x13, x14, [sp, #32]\n"
            "stp x15, x16, [sp, #48]\n stp x17, x18, [sp, #64]\n bl sprite_stub\n ldp x11, x12, [sp, #16]\n"
            "ldp x13, x14, [sp, #32]\n ldp x15, x16, [sp, #48]\n ldp x17, x18, [sp, #64]\n ldp x29, x30, [sp], #80\n ret");
}
static void ys_state(uint8_t *geom, uint8_t *polys, uint8_t *verts, uint8_t *bins) {
    uint32_t np = rnd(4) ? rnd(YT_MAXP) : rnd(8);
    rndfill(polys, np * 32 + 64); st32(polys, 0x10000, np);
    for (uint32_t i = 0; i < np; i++) {
        uint8_t *P = polys + i * 32;
        uint32_t k = rnd(16), n = k < 7 ? 3 : k < 13 ? 4 : rnd(16);
        uint32_t w8 = (uint32_t)rnd64();
        if (rnd(2)) w8 &= 0x7fffff;
        st32(P, 8, (w8 & ~0xfu) | n);
        st16(P, 0x1a, (uint16_t)rnd(6144 - 16));
        if (rnd(3) == 0) st32(P, 0, (uint32_t)rnd64() & ~0x1c000000u);
        if (i && rnd(2)) { memcpy(P, P - 32, 4); memcpy(P + 0x18, P - 32 + 0x18, 2); }   /* the same texture */
        if (i && rnd(4) == 0) memcpy(P, P - 32, 4);
    }
    for (int v = 0; v < 6144; v++) {
        uint8_t *V = verts + v * 16;
        rndfill(V, 16);
        unsigned k = rnd(16);
        st16(V, 6, k < 12 ? rnd(0x1a0) : k < 15 ? 0x170 + rnd(0x20) : (uint16_t)rnd64());
        if (v && rnd(4)) { memcpy(V, V - 16, 4); memcpy(V + 8, V - 16 + 8, 4); }       /* shared colour, +8, +0xa */
        if (v && rnd(8) == 0) memcpy(V + 6, V - 16 + 6, 2);                            /* flat edges */
    }
    rndfill(bins, YC_BINS);
    geom[G_SORT_FLAGS] = (uint8_t)rnd64();
    stp_(geom, G_TEXCACHE, (void *)(uintptr_t)rnd64());
    int16_t *counts = (int16_t *)(ds_base + DS_SORT_COUNTS);
    uint16_t *list = (uint16_t *)(ds_base + DS_SORT_LIST);
    memset(counts, 0, YS_ROWS * 2);
    if (np && rnd(8) == 0)                                   /* stale rows (never left by DraStic, but exact anyway) */
        for (int k = rnd(4); k >= 0; k--) {
            unsigned y = rnd(YS_ROWS), c = 1 + rnd(3);
            for (unsigned j = 0; j < c; j++) list[y * 0x800 + counts[y] + j] = (uint16_t)rnd(np);
            counts[y] += c;
        }
}
static void test_ysort(int n) {
    geom_ysort_fn *orig = DS(geom_ysort_fn *, DS_YSORT_4X);
    uint8_t *geom = calloc(1, 0xa000), *polys = calloc(1, YC_POLYS + 64), *verts = calloc(1, YC_VERTS + 0x200), *bins = calloc(1, YC_BINS);
    uint8_t *in = malloc(YC_SIZE), *ours = malloc(YC_SIZE), *ref = malloc(YC_SIZE);
    static uint64_t ours_log[YT_LOG];
    if (!rast_hook(DS_TEXTURE_CACHE_LOOKUP, (const uint32_t *)(ds_base + DS_TEXTURE_CACHE_LOOKUP), (void *)tex_stub) ||
        !rast_hook(DS_RENDER_SPRITE_CHECK, (const uint32_t *)(ds_base + DS_RENDER_SPRITE_CHECK), (void *)sprite_stub_asm)) {
        fprintf(stderr, "FAIL ysort: cannot patch the stubs\n"); ut_fail++; return;
    }
    unsigned long polys_n = 0, lookups = 0, sprites = 0, entries = 0;
    for (int it = 0; it < n && !ut_fail; it++) {
        ys_state(geom, polys, verts, bins);
        uint32_t flags = rnd(2);
        yc_take(in, polys, verts, bins);
        yt_nlog = 0;
        geom_ysort(geom, bins, polys, verts, flags);
        int nours = yt_nlog; memcpy(ours_log, yt_log, nours * 8);
        yc_take(ours, polys, verts, bins);
        yc_put(in, polys, verts, bins);
        yt_nlog = 0;
        orig(geom, bins, polys, verts, flags);
        yc_take(ref, polys, verts, bins);
        if (nours != yt_nlog || memcmp(ours_log, yt_log, nours * 8)) { fprintf(stderr, "FAIL ysort: the lookups/sprite checks differ (%d/%d log words)\n", yt_nlog, nours); ut_fail++; }
        if (ut_cmp("ysort: polys", ref, ours, YC_POLYS) || ut_cmp("ysort: verts", ref + YC_POLYS, ours + YC_POLYS, YC_VERTS) ||
            ut_cmp("ysort: bins", ref + YC_POLYS + YC_VERTS, ours + YC_POLYS + YC_VERTS, YC_BINS) ||
            ut_cmp("ysort: sort arrays", ref + YC_POLYS + YC_VERTS + YC_BINS, ours + YC_POLYS + YC_VERTS + YC_BINS, YC_SORT))
            fprintf(stderr, "   iteration %d: %u polygons, flags %u, +0x9acc %#x\n", it, (uint32_t)ld32(in, 0x10000), flags, geom[G_SORT_FLAGS]);
        polys_n += (uint32_t)ld32(in, 0x10000);
        for (int i = 0; i < yt_nlog; i += 3) { lookups += yt_log[i] == 1; sprites += yt_log[i] == 2; }
        for (int k = 0; k < YS_BINS; k++) entries += (uint32_t)ld32(ref + YC_POLYS + YC_VERTS + k * YS_BIN_BYTES, 0x1000);
        /* the check mode on the same input: no difference, DraStic's bytes left */
        yc_put(in, polys, verts, bins);
        geom_ysort_orig = orig;
        unsigned long bad = yc_bad;
        geom_ysort_check(geom, bins, polys, verts, flags);
        if (yc_bad != bad) { fprintf(stderr, "FAIL ysort: the check mode reported a difference\n"); ut_fail++; }
        yc_take(ours, polys, verts, bins);
        ut_cmp("ysort: check mode's result", ref, ours, YC_SIZE);
    }
    fprintf(stderr, "video_3d_bin_polygons_y_sort_4x: %d calls, %lu polygons, %lu texture lookups, %lu sprite checks, %lu bin "
            "entries equal to DraStic's\n", n, polys_n, lookups, sprites, entries);
    if (!lookups || !sprites || !entries) { fprintf(stderr, "FAIL ysort: a path not covered\n"); ut_fail++; }
}

/* geometry_transform_vertex_block_asm against geom_tvb.S: the batch, the matrix (any s32: the 64-bit sums wrap) and
 * the start index random, n 0..64 (the group past a multiple of 4 included) and sometimes larger; the whole state
 * compared; then the check mode (no difference reported, DraStic's bytes left) */
static void test_tvb(int n) {
    geom_tvb_fn *orig = DS(geom_tvb_fn *, DS_TRANSFORM_VERTEX_BLOCK);
    uint8_t *g = calloc(1, 0xa000), *r = calloc(1, 0xa000);
    unsigned long verts = 0;
    for (int it = 0; it < n && !ut_fail; it++) {
        rndfill(g, 0xa000);
        uint32_t nb = rnd(8) ? rnd(65) : 65 + rnd(60), i0 = rnd(8) ? rnd(128) : rnd(300);
        st32(g, G_BATCH_N, nb); st32(g, 0x330, i0);
        for (int k = 0; k < 16; k++) if (rnd(2)) st32(g, 0x97e4 + 4 * k, (uint32_t)(int16_t)rnd64());
        for (int k = 0; k < 0xc0; k++) if (rnd(2)) st32(g, 4 * k, (uint32_t)(int16_t)rnd64());
        memcpy(r, g, 0xa000);
        geom_tvb(g);
        orig(r);
        if (ut_cmp("vertex block: ours vs DraStic's", r, g, 0xa000)) fprintf(stderr, "   n %u from %u\n", nb, i0);
        verts += nb;
    }
    for (int it = 0; it < n / 10 && !ut_fail; it++) {
        rndfill(g, 0xa000);
        st32(g, G_BATCH_N, rnd(65)); st32(g, 0x330, rnd(128));
        memcpy(r, g, 0xa000);
        orig(r);
        geom_tvb_orig = orig;
        unsigned long bad = tc_bad;
        geom_tvb_check(g);
        if (tc_bad != bad) { fprintf(stderr, "FAIL vertex block: the check mode reported a difference\n"); ut_fail++; }
        ut_cmp("vertex block: check mode's result", r, g, 0xa000);
    }
    fprintf(stderr, "geometry_transform_vertex_block_asm: %d batches, %lu vertices equal to DraStic's\n", n, verts);
}

/* geometry_transform_vertexes: a batch of 1..64 vertices from a random output index (the flush threshold of 127
 * output vertices crossed often), the clip matrix sometimes pending (DraStic's matrix multiply runs for both), a
 * consistent colour event stream as process_geometry_commands builds it (strictly rising vertex indices up to the
 * count, one normal or explicit colour per event; all lit, all explicit, mixed, or none), any light mask (bits 4-7
 * included), the shininess table on or off, light and half vectors, matrices and colour tables random (realistic
 * magnitudes when the table is on so its index stays in the state's guard pages: a wrapped square indexes below the
 * table, both read the same bytes), and a few inconsistent streams, which ours must pass on untouched. DraStic's
 * geometry_flush_polygons is patched to geom_flush_check (as geom_init() does in check mode) with a logging stub
 * as the flush, so the tail call's presence and argument are compared too. The whole state is compared; then the
 * check mode. */
#define XT_GUARD 0x400000
static uint64_t fl_log[8]; static int fl_n;
static void flush_stub(uint8_t *g) { if (fl_n < 8) fl_log[fl_n++] = (uintptr_t)g; }
static void tvx_fw_stub(uint8_t *g) { fw_g = g; fw_n++; }
static void xt_state(uint8_t *g, int *consistent) {
    rndfill(g, 0xa000);
    uint32_t n = rnd(16) ? 1 + rnd(64) : (rnd(2) ? 64 : 1 + rnd(3)), i0 = rnd(4) ? rnd(128) : 100 + rnd(32);
    st32(g, G_BATCH_N, n); st32(g, G_OUT_BASE, i0);
    g[G_MTX_DIRTY] = rnd(4) ? 0 : 1 + rnd(255);
    unsigned shin = rnd(2);
    g[G_SHIN_ON] = shin ? 1 + rnd(255) : 0;
    g[G_LIGHT_MASK] = rnd(2) ? rnd(16) : (uint8_t)rnd64();
    stp_(g, G_POS_MTX_PTR, g + 0x9100); stp_(g, G_DIR_MTX_PTR, g + 0x9200);
    for (int k = 0; k < 0xc0; k++) if (rnd(2)) st32(g, 4 * k, (uint32_t)(int16_t)rnd64());
    for (int k = 0; k < 16; k++) {
        if (rnd(2)) st32(g, G_CLIP_MTX + 4 * k, (uint32_t)(int16_t)rnd64());
        if (rnd(2)) st32(g, G_PROJ_MTX + 4 * k, (uint32_t)(int16_t)rnd64());
        if (rnd(2)) st32(g, 0x9100 + 4 * k, (uint32_t)(int16_t)rnd64());
        if (shin || rnd(2)) st32(g, 0x9200 + 4 * k, (uint32_t)rndr(-4096, 4096));
    }
    for (int k = 0; k < 24; k++) if (shin || rnd(2)) st32(g, G_LIGHT_VEC + 4 * k, (uint32_t)rndr(-4096, 4096));
    if (rnd(2)) for (int k = 0; k < 24; k++) st16(g, G_LIGHT_DIF + 2 * k, (uint16_t)rnd(0x400));
    if (rnd(2)) for (int k = 0; k < 3; k++) st32(g, G_LIGHT_BASE + 4 * k, rnd(0x100000));
    if (rnd(2)) st32(g, G_CUR_COLOUR, rnd(0x10000));
    /* the events: ne distinct rising indices in 0..n; kind by a mode */
    unsigned mode = rnd(8), ne = mode == 0 ? 0 : rnd(2) ? rnd(n + 2) : rnd(6);
    int dense = rnd(3) == 0;                         /* an event before every vertex (and maybe one at the count) */
    if (dense && mode) ne = rnd(4) ? n + rnd(2) : 1 + rnd(n);    /* events at the first ne vertices (the rest after the last) */
    if (ne > n + 1) ne = n + 1;
    if (ne > 65) ne = 65;
    uint8_t idx[66];
    if (dense) for (unsigned k = 0; k < ne; k++) idx[k] = (uint8_t)k;
    else { /* ne distinct values of 0..n, rising */
        unsigned left = ne;
        for (unsigned v = 0, k = 0; v <= n && left; v++) if (rnd(n + 1 - v) < left) { idx[k++] = (uint8_t)v; left--; }
    }
    unsigned na = 0, nb = 0;
    for (unsigned k = 0; k < ne; k++) {
        int explicit = mode == 1 ? 1 : mode == 2 ? 0 : rnd(2);
        g[G_EVENTS + k] = (uint8_t)(idx[k] | (explicit ? 0x80 : 0));
        if (explicit) { st16(g, G_COLOURS + 2 * na, (uint16_t)rnd64()); na++; }
        else { st32(g, G_NORMALS + 4 * nb, (uint32_t)rnd64()); nb++; }
    }
    *consistent = 1;
    if (rnd(32) == 0) {        /* an inconsistent stream: a count off, a falling index, a kind count off, an index past n */
        unsigned w = rnd(5);
        if (w == 0) ne += 1;
        else if (w == 1 && ne >= 2) { unsigned j = rnd(ne - 1); uint8_t t = g[G_EVENTS + j]; g[G_EVENTS + j] = g[G_EVENTS + j + 1]; g[G_EVENTS + j + 1] = t; }
        else if (w == 2 && ne >= 2) { unsigned j = 1 + rnd(ne - 1); g[G_EVENTS + j] = (g[G_EVENTS + j] & 0x80) | (g[G_EVENTS + j - 1] & 0x7f); }
        else if (w == 3 && ne) g[G_EVENTS + rnd(ne)] ^= 0x80;
        else if (ne) { unsigned j = ne - 1; g[G_EVENTS + j] = (g[G_EVENTS + j] & 0x80) | (uint8_t)(n + 1 + rnd(126 - n)); }
        else ne = 1;
        *consistent = 0;
    }
    stp_(g, G_NORMAL_CUR, g + G_NORMALS + 4 * nb); stp_(g, G_COLOUR_CUR, g + G_COLOURS + 2 * na); stp_(g, G_EVENT_CUR, g + G_EVENTS + ne);
}
/* a copy of a transform state: its five pointers (the three list cursors, the matrices) moved with it */
static void xt_copy(uint8_t *dst, const uint8_t *src) {
    static const unsigned ptrs[5] = { G_NORMAL_CUR, G_COLOUR_CUR, G_EVENT_CUR, G_POS_MTX_PTR, G_DIR_MTX_PTR };
    memcpy(dst, src, 0xa000);
    for (int i = 0; i < 5; i++) stp_(dst, ptrs[i], (uint8_t *)ldp_(src, ptrs[i]) - src + dst);
}
/* ref (a state at r) against ours (at g): the pointers compared as offsets */
static int xt_cmp(const char *what, const uint8_t *ref, const uint8_t *ours) {
    static uint8_t *tmp;
    if (!tmp) tmp = malloc(0xa000);
    xt_copy(tmp, ref);
    static const unsigned ptrs[5] = { G_NORMAL_CUR, G_COLOUR_CUR, G_EVENT_CUR, G_POS_MTX_PTR, G_DIR_MTX_PTR };
    for (int i = 0; i < 5; i++) stp_(tmp, ptrs[i], (uint8_t *)ldp_(ref, ptrs[i]) - ref + (uint8_t *)ours);
    return ut_cmp(what, tmp, ours, 0xa000);
}
static void test_tvx(int n) {
    geom_tvx_fn *orig = DS(geom_tvx_fn *, DS_TRANSFORM_VERTEXES);
    uint8_t *gb = calloc(1, 2 * XT_GUARD + 0xa000), *rb = calloc(1, 2 * XT_GUARD + 0xa000);
    uint8_t *g = gb + XT_GUARD, *r = rb + XT_GUARD;
    if (!rast_hook(DS_FLUSH_POLYGONS, expect_flush, (void *)geom_flush_check)) { fprintf(stderr, "FAIL transform: cannot patch the flush\n"); ut_fail++; return; }
    geom_flush_orig = flush_stub;
    geom_matmul = DS(geom_matmul_fn *, DS_MATRIX_MUL_4X4_ASM);
    unsigned long verts = 0, lit = 0, flushes = 0, dirty = 0, fwd = 0;
    for (int it = 0; it < n && !ut_fail; it++) {
        int consistent;
        xt_state(g, &consistent);
        int was_dirty = g[G_MTX_DIRTY] != 0;
        uint32_t batch_n = (uint32_t)ld32(g, G_BATCH_N);
        xt_copy(r, g);
        geom_tvx_orig = tvx_fw_stub; fw_n = 0; fl_n = 0;
        geom_transform_vertexes(g);
        int ours_n = fw_n, ours_fl = fl_n; uint64_t ours_log = fl_log[0];
        if (ours_n) {
            if (consistent) { fprintf(stderr, "FAIL transform: passed on a consistent state (iteration %d)\n", it); ut_fail++; }
            if (ours_n != 1 || fw_g != g || ours_fl) { fprintf(stderr, "FAIL transform: forwarded call's arguments or a flush\n"); ut_fail++; }
            xt_cmp("transform: forwarded call wrote", r, g);
            fwd++;
            continue;
        }
        if (!consistent) { fprintf(stderr, "FAIL transform: handled an inconsistent state (iteration %d)\n", it); ut_fail++; }
        fl_n = 0;
        orig(r);
        if (xt_cmp("transform: ours vs DraStic's", r, g) || ours_fl != fl_n || (fl_n && ours_log != fl_log[0]))
            { if (ours_fl != fl_n) ut_fail++; fprintf(stderr, "   iteration %d: n %u from %u mask %#x shin %u dirty %u flush %d/%d\n", it, (uint32_t)ld32(r, G_BATCH_N), (uint32_t)ld32(r, G_OUT_BASE), g[G_LIGHT_MASK], g[G_SHIN_ON], g[G_MTX_DIRTY], fl_n, ours_fl); }
        verts += batch_n;
        flushes += fl_n; dirty += was_dirty;
        /* the check mode on a fresh consistent state (an inconsistent one would reach the original, which, like
         * DraStic itself, never sees one): no difference, DraStic's bytes, one flush */
        do xt_state(g, &consistent); while (!consistent);
        xt_copy(r, g);
        geom_tvx_orig = orig;
        unsigned long bad = xc_bad;
        fl_n = 0;
        geom_tvx_check(g);
        int chk_fl = fl_n;
        fl_n = 0;
        orig(r);
        if (xc_bad != bad) { fprintf(stderr, "FAIL transform: the check mode reported a difference\n"); ut_fail++; }
        xt_cmp("transform: check mode's result", r, g);
        if (chk_fl != fl_n) { fprintf(stderr, "FAIL transform: the check mode flushed %d times, the original %d\n", chk_fl, fl_n); ut_fail++; }
        lit += ((uint8_t *)ldp_(r, G_NORMAL_CUR) - (r + G_NORMALS)) / 4;
    }
    fprintf(stderr, "geometry_transform_vertexes: %d batches, %lu vertices equal to DraStic's (%lu with the matrix pending, %lu flushing); %lu passed on\n", n, verts, dirty, flushes, fwd);
    if (!dirty || !flushes || !fwd) { fprintf(stderr, "FAIL transform: a path not covered\n"); ut_fail++; }
    /* the check mode's flush handling: a flush asked for outside a check window passes straight through; a batch
     * past the threshold against an "original" that never flushes is reported as a difference (one side asking
     * alone), returns, and runs no flush (the original's decision rules) */
    fl_n = 0;
    geom_flush_check(g);
    if (fl_n != 1 || fl_log[0] != (uintptr_t)g) { fprintf(stderr, "FAIL transform: the flush hook did not pass a flush through\n"); ut_fail++; }
    int consistent;
    do xt_state(g, &consistent); while (!consistent || (uint32_t)ld32(g, G_OUT_BASE) + (uint32_t)ld32(g, G_BATCH_N) <= 0x7f);
    geom_tvx_orig = tvx_fw_stub;                                /* does nothing, asks for no flush */
    unsigned long bad = xc_bad; fl_n = 0;
    geom_tvx_check(g);
    if (xc_bad != bad + 1 || fl_n != 0) { fprintf(stderr, "FAIL transform: a one-sided flush was not reported as a difference (%lu) or a flush ran (%d)\n", xc_bad - bad, fl_n); ut_fail++; }
    xc_bad = bad;                                               /* that report was the test's doing */
    (void)lit;
}


/* geometry_clipped_vertex_interpolate_edge: vertex pairs with coordinates in clip-space ranges (and any 32-bit value
 * sometimes), w of either sign, the pair straddling the plane or not, equal axis distances (den 0), s t colours
 * random; planes 0..7; out separate or the same as a or b. The 24 bytes of all three compared; then the check mode */
static int32_t ct_val(void) { unsigned k = rnd(8); return k < 5 ? rndr(-0x40000, 0x40000) : k < 7 ? (int32_t)(int16_t)rnd64() : (int32_t)rnd64(); }
static void test_clip(int n) {
    geom_clip_fn *orig = DS(geom_clip_fn *, DS_CLIP_EDGE);
    uint8_t buf[2][3][24];
    unsigned long den0 = 0, alias = 0;
    for (int it = 0; it < n && !ut_fail; it++) {
        uint8_t *v = buf[0][0];
        rndfill(v, sizeof buf[0]);
        for (int k = 0; k < 2; k++) for (int c = 0; c < 4; c++) st32(buf[0][k], 4 * c, (uint32_t)ct_val());
        uint32_t plane = rnd(8) ? rnd(6) : 6 + rnd(2), axis = plane >> 1;
        if (rnd(8) == 0) {                         /* den 0: the same distance to the plane at both ends */
            int32_t d = ct_val();
            st32(buf[0][1], 12, (uint32_t)ld32(buf[0][0], 12) + (uint32_t)d);
            st32(buf[0][1], 4 * axis, (uint32_t)ld32(buf[0][0], 4 * axis) + (uint32_t)((plane & 1) ? -d : d));
            if (axis == 3) st32(buf[0][1], 12, (uint32_t)ld32(buf[0][0], 12) + (uint32_t)d);
        }
        unsigned o = rnd(8) == 0 ? rnd(2) : 2;     /* out: a, b or the third slot */
        memcpy(buf[1], buf[0], sizeof buf[0]);
        geom_clip_edge(buf[0][o], buf[0][0], buf[0][1], plane);
        orig(buf[1][o], buf[1][0], buf[1][1], plane);
        if (ut_cmp("clip edge: ours vs DraStic's", buf[1], buf[0], sizeof buf[0]))
            fprintf(stderr, "   iteration %d plane %u out %u\n", it, plane, o);
        int32_t wa = ld32(buf[1][0], 12), wb = ld32(buf[1][1], 12);
        den0 += (plane & 1 ? -(int64_t)wb + wa : (int64_t)wb - wa) == 0;
        alias += o < 2;
    }
    for (int it = 0; it < n / 10 && !ut_fail; it++) {
        rndfill(buf[0], sizeof buf[0]);
        uint32_t plane = rnd(6);
        memcpy(buf[1], buf[0], sizeof buf[0]);
        orig(buf[1][2], buf[1][0], buf[1][1], plane);
        geom_clip_orig = orig;
        unsigned long bad = ec_bad;
        geom_clip_edge_check(buf[0][2], buf[0][0], buf[0][1], plane);
        if (ec_bad != bad) { fprintf(stderr, "FAIL clip edge: the check mode reported a difference\n"); ut_fail++; }
        ut_cmp("clip edge: check mode's result", buf[1], buf[0], sizeof buf[0]);
    }
    fprintf(stderr, "geometry_clipped_vertex_interpolate_edge: %d edges equal to DraStic's (%lu with out = a or b)\n", n, alias);
    (void)den0;
}

/* DraStic's entry patched to the replacement, as geom_init() does: called through the entry, against the original
 * through the trampoline, on the calls ours handles */
static void test_patched(int n) {
    geom_queue_fn *entry = DS(geom_queue_fn *, DS_QUEUE_GEOMETRY_COMMAND);
    geom_tables(ut_base);
    geom_queue_fn *tramp = (geom_queue_fn *)rast_hook(DS_QUEUE_GEOMETRY_COMMAND, expect_queue, (void *)geom_queue_command);
    if (!tramp) { fprintf(stderr, "FAIL patched: queue_geometry_command's first words differ\n"); ut_fail++; return; }
    int done = 0;
    for (int i = 0; i < 50 * n && done < n && !ut_fail; i++) {
        uint32_t cmd, param;
        rnd_state(ga, &cmd, &param);
        copy_state(gb, ga);
        copy_state(g0, ga);
        if (!queue_fast(g0, cmd, param)) continue;             /* calls ours passes on would run process_geometry */
        geom_queue_orig = tramp;
        entry(ga, cmd, param);
        tramp(gb, cmd, param);
        cmp_state("patched entry vs trampoline", gb, ga);
        done++;
    }
    fprintf(stderr, "patched queue_geometry_command: %d calls through the entry equal to the original's\n", done);
    geom_vertex_fn *ventry = DS(geom_vertex_fn *, DS_GEOMETRY_ADD_VERTEX);
    geom_vertex_fn *vtramp = (geom_vertex_fn *)rast_hook(DS_GEOMETRY_ADD_VERTEX, expect_vertex, (void *)geom_add_vertex);
    if (!vtramp) { fprintf(stderr, "FAIL patched: geometry_add_vertex's first words differ\n"); ut_fail++; return; }
    done = 0;
    for (int i = 0; i < 50 * n && done < n && !ut_fail; i++) {
        int32_t v[3];
        vtx_state(ga, v);
        if (ga[G_MTX_DIRTY] || ld32(ga, G_BATCH_N) == 64) continue;     /* these run DraStic's transforms */
        memcpy(gb, ga, VC_LO); memcpy(gb + VC_HI_AT, ga + VC_HI_AT, VC_HI);
        geom_vertex_orig = vtramp;
        ventry(ga, v[0], v[1], v[2]);
        vtramp(gb, v[0], v[1], v[2]);
        ut_cmp("patched add_vertex vs trampoline", gb, ga, VC_LO) || ut_cmp("patched add_vertex vs trampoline", gb + VC_HI_AT, ga + VC_HI_AT, VC_HI);
        done++;
    }
    fprintf(stderr, "patched geometry_add_vertex: %d calls through the entry equal to the original's\n", done);
    geom_packed_fn *pentry = DS(geom_packed_fn *, DS_QUEUE_PACKED);
    geom_packed_fn *ptramp = (geom_packed_fn *)rast_hook(DS_QUEUE_PACKED, expect_packed, (void *)geom_queue_packed);
    if (!ptramp) { fprintf(stderr, "FAIL patched: queue_geometry_command_packed's first words differ\n"); ut_fail++; return; }
    done = 0;
    for (int i = 0; i < 50 * n && done < n && !ut_fail; i++) {
        uint32_t cmd, param;
        rnd_state(ga, &cmd, &param);
        uint32_t w = pk_word();
        copy_state(gb, ga);
        copy_state(g0, ga);
        if (!packed_fast(g0, w)) continue;
        geom_packed_orig = ptramp;
        pentry(ga, w);
        ptramp(gb, w);
        cmp_state("patched packed entry vs trampoline", gb, ga);
        done++;
    }
    fprintf(stderr, "patched queue_geometry_command_packed: %d calls through the entry equal to the original's\n", done);
}

void ut_main(void) {
    ds_base = ut_base;
    geom_tables(ut_base);
    ga = calloc(1, GSIZE); gb = calloc(1, GSIZE); g0 = calloc(1, GSIZE);
    test_queue(200000);
    test_packed(200000);
    test_vertex(200000);
    test_ysort(1500);
    test_tvb(20000);
    test_tvx(20000);
    test_clip(300000);
    test_patched(20000);
}
