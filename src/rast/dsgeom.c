/* dsgeom.c: DraStic's geometry stage (the DS 3D geometry engine's emulation, on the emulation thread), the parts of
 * it that a replacement makes cheaper on the A55. Installed by geom_init() when the renderer is hooked; RAST_GEOM=0
 * (or DSFLIP_RAST_GEOM=0) leaves DraStic's functions alone, RAST_GEOMCHECK=1 checks every replaced call against the
 * original (below).
 *
 * The callers assume the AAPCS and nothing more, so plain C functions are valid replacements (from the disassembly):
 * queue_geometry_command is called by store_io_register_arm9_32 (C, bl at 0x16238), update_frame_geometry (C,
 * 0x62aac) and the JIT's 32-bit store helper arm64_store_memory32_arm9 (hand-written, bl at 0x8d6ac), which saves
 * w12-w18, x30 and NZCV in its state block before the call (0x8d640..0x8d658) and reloads x10/x11 and those after it:
 * nothing else of the JIT's is live across the call. geometry_add_vertex: process_geometry_commands' five bl sites
 * (0x60568..0x60f54), each followed by code that touches callee-saved registers only. The y-sort: update_frame_3d_4x
 * (bl 0x59048; callee-saved registers only after it) and video_3d_bin_polygons_4x's tail call (b 0x557e0, after its
 * epilogue). The vertex block: geometry_transform_vertexes (bl 0x5e0a8). GCC's IPA-RA cannot have narrowed any of
 * these: each original's clobber set is "everything" since each calls a function GCC did not compile in that unit
 * (process_geometry -> ... -> the matrix asm; the matrix asm; texture_cache_lookup in another unit; the asm itself).
 * Threads: queue_geometry_command (the CPU's stores and the frame update), geometry_add_vertex and the vertex block
 * (process_geometry) run on the emulation thread; the y-sort runs where update_frame_3d_4x runs (the emulation thread,
 * or DraStic's 3D thread with threaded_3d), whose two calls a frame (the opaque list, then the translucent one through
 * video_3d_bin_polygons_4x) are sequential: DraStic's static sort arrays already assume that, so nothing new is shared.
 *
 * queue_geometry_command(geom, cmd, param) (0x627b0): the direct command ports (0x4000440..0x40005ff:
 * store_io_register_arm9_32 and the JIT's 32-bit store path) and update_frame_geometry end here, one call per 32-bit
 * word: a command with its first parameter, or a further parameter. The command queue in the geometry state:
 *   cmd_cur  (+0x9a68) u8 *:  the next command byte (the queue's bytes start at +0x79b00)
 *   par_cur  (+0x9a70) u32 *: the next parameter word (the parameters start at +0x81b00)
 *   cmd_pend (+0x9a78), par_pend (+0x9a80): the same while a command still waits for parameters
 *   left     (+0x9ac1) u8:    parameters the pending command still needs
 *   swap     (+0x9aca) u8:    SWAP_BUFFERS (0x50) queued: everything after it goes through process_geometry
 * What the original does (the disassembly, 0x627b0..0x6294c):
 *   left != 0 (a parameter): *par_pend++ = param; if left == 1, the command is complete: cmd_cur += 1,
 *     par_cur = par_pend, cmd_pend = cmd_cur, and process_geometry runs when the queue then holds more than 0x400
 *     command bytes (cmd_cur - queue > 0x400, signed); left -= 1 (stored after process_geometry).
 *   left == 0 (a command): with swap set, the 0x9aca path (it writes the system's alert flags); else cmd &= 0x7f and
 *     n = geometry_command_parameters[cmd] (0x11e250, DraStic's table); 0x50 sets swap and continues below, then
 *     runs process_geometry. Otherwise *cmd_cur = cmd; n > 1: *par_cur = param with par_pend = par_cur + 1 (par_cur
 *     itself stays), left = n - 1. n <= 1: the command is complete: cmd_cur += 1, n == 1 stores the parameter
 *     (par_cur = par_pend = par_cur + 1), cmd_pend = cmd_cur, process_geometry when more than 0x400 bytes are queued.
 * queue_fast() does exactly those stores, in the original's order, for the paths that do not call process_geometry
 * and do not touch swap; every other call (swap set, command 0x50, a full queue) goes to the original through the
 * trampoline before anything is written, so it runs on the same state. The original costs ~40 instructions a call
 * (a 48-byte stack frame, its stores with writeback, the table through adrp); queue_fast() 10-20 and no frame.
 * Precondition: the two queues do not overlap the fields (they are fixed arrays in the state, +0x79b00 and +0x81b00,
 * far from +0x9a68), so the fields the original loads after one of its queue stores hold the values queue_fast()
 * loaded first.
 *
 * geometry_add_vertex(geom, x, y, z) (0x5e890, from process_geometry_commands' vertex commands): appends a vertex to
 * the batch of 64 that geometry_transform_vertexes transforms. The original (0x5e890..0x5e9a0):
 *   with the matrix flag (+0x9ad0) set, the clip matrix is recomputed first (geometry_matrix_multiply_4x4_by_4x4_asm)
 *     and the flag cleared;
 *   texture coordinate mode 3 (+0x9ac4 == 3, the "vertex source" transform): s = x*m[0] + y*m[4] + z*m[8] and
 *     t = x*m[1] + y*m[5] + z*m[9] with the texture matrix's words m at +0x9864 (signed 32x32 -> 64-bit products, a
 *     wrapping 64-bit sum), stored as u16 (+0x9aa2 + (s >> 24)) at +0x9a98 and (+0x9aa4 + (t >> 24)) at +0x9a9a;
 *   n = the batch count (+0x300, u32): a full batch (n == 64) is transformed first (geometry_transform_vertexes,
 *     which empties it); then x, y, z go to +4n, +0x100+4n, +0x200+4n, the texture coordinate word (+0x9a98) to
 *     +0x52c+4n, the count to n + 1, and the vertex counter at +0x338 is incremented.
 * add_vertex_fast() does the same stores in the same order when neither call is needed (the matrix flag clear,
 * n != 64), else the original runs through the trampoline before anything is written. ~65 instructions with the
 * 80-byte stack frame -> ~25 (mode 3) or ~15.
 *
 * video_3d_bin_polygons_y_sort_4x(geom, bins, polys, verts, flags) (0x549f0; update_frame_3d_4x calls it for the
 * opaque list, video_3d_bin_polygons_4x tail-calls it for the translucent one unless manual sorting is on): fills
 * the 12 bins' polygon lists (u16 indices, u32 count at +0x1000, 0x1004 apart) the render threads read. The original
 * (0x549f0..0x551f8), which geom_ysort() follows step by step:
 *   1. per polygon with 2..8 vertices (+8 & 0xf): the vertices' y (+6), all but vertex 0's clamped to 0x180 and
 *      stored back; top = the first lowest y, bottom = the highest; skipped when equal; flags from whether the
 *      vertices' +0xa (4), +8 (0x10) and word 0 (0x20) agree (+0x9acc bit 1: 0x30 on word 0 alone, and 8); a
 *      textured polygon (+0 & 0x1c000000, and (+4 >> 4 & 3) != 3) gets flag 2 and the texture_cache_lookup() result
 *      at +0x10 (one lookup per run of equal keys); +8 gets bottom << 23, flags << 8, the top vertex << 16 and the
 *      count << 19 (quads: the sprite variant and render_sprite_check()); then the polygon index goes into row `top`
 *      of DraStic's sort array (polygon_sort_list.15121, 0x181 rows of 0x800, counts in polygon_sort_counts.15122).
 *   2. rows 0x180 down to 0: each row's polygons, last first, into the rows of their bottom y (+8 >> 23, at most
 *      0x180), and the row's count to 0.
 *   3. rows 1..0x180: each row's polygons, last first, into every bin from the top vertex's (its y >> 5) to the
 *      row's ((row - 1) >> 5); the count to 0; then row 0's count to 0.
 * The texture lookups are DraStic's own economy: it compares word 0 and +0x18 with the last pair (0x54e44 cmp w1, w0;
 * 0x54e4c ccmp w2, w19) and calls texture_cache_lookup only on a mismatch (0x54e50 b.ne 0x551a0), so the cache sees
 * the same calls. render_sprite_check.isra.0 (an IPA-SRA clone) takes x0 = the polygon (it reads [x0, #8]) and x1 =
 * its vertex base (vertices at x1 + 16 k, k from the table at 0x11df90 indexed by bits 16..22 of word 8); the original
 * stores word 8 before the call (0x55184) and keeps x11 (the last texture) live across it, which our C does not need.
 * The original walks all 0x181 rows twice whatever they hold; geom_ysort() keeps a byte a row, set while the row's
 * count is not 0 (from the counts at entry, which DraStic leaves at 0: a NEON OR of them decides whether to look),
 * and a bit per 8 rows, and visits only those rows: an empty row's visit only stores 0 over 0. Pass 2 re-reads the 8
 * after each row (an append may land below it, which the original's downward walk then visits) and keeps only the
 * rows below. It uses DraStic's own sort arrays, so every byte the original writes is written the same, the stale
 * list entries included. The per-polygon work is DraStic's in C (no faster on the A55 model than its code): where
 * nearly every row holds polygons (the stress ROM) the replacement costs what the original does.
 *
 * geometry_transform_vertex_block_asm(geom) (0x9dda0, from geometry_transform_vertexes): geom_tvb.S, the same
 * computation scheduled for the A55 (its header has the details).
 *
 * Check mode (RAST_GEOMCHECK=1): every call ours handles runs twice on the same bytes: ours, then the original through
 * the trampoline, and everything it may write is compared. queue_geometry_command: the geometry state's
 * +0x9a00..+0x9b00 (cursors, counts, flags) and 64 bytes around each of the three cursors' targets (the original would
 * also differ there if it called process_geometry, which rewinds the queue); geometry_add_vertex: +0..+0x800 (the
 * batch, its counts, the texture coordinates) and +0x9800..+0x9b00 (the texture matrix, the flags; the original
 * would differ there or in the batch count if it called either function); the y-sort: the polygons, the vertices,
 * the bins and DraStic's sort arrays (its texture lookups run twice: the second finds the first's entry); the vertex
 * block: +0..+0x4000 (the batch, the clip-space vertices and codes). Counts every 2 s ("[geom] queue:" lines,
 * "[geomcheck]" for a difference). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arm_neon.h>
#include "rast.h"
#include "dsgeom.h"

#define DS_QUEUE_GEOMETRY_COMMAND 0x627b0
#define DS_GEOMETRY_COMMAND_PARAMETERS 0x11e250   /* u8[128]: the parameter words of each command */
#define G_CMD_CUR   0x9a68
#define G_PAR_CUR   0x9a70
#define G_CMD_PEND  0x9a78
#define G_PAR_PEND  0x9a80
#define G_LEFT      0x9ac1
#define G_SWAP      0x9aca
#define G_QUEUE     0x79b00
#define G_QUEUE_MAX 0x400

geom_queue_fn *geom_queue_orig;
/* a copy of DraStic's geometry_command_parameters (read-only data): one adrp + add from here, no GOT load */
static uint8_t cmd_params[128];
void geom_tables(uintptr_t base) { memcpy(cmd_params, (const void *)(base + DS_GEOMETRY_COMMAND_PARAMETERS), sizeof cmd_params); }

/* the cursor fields go through memcpy (a char access, which may alias the parameter stores) so the stores stay in
 * the original's order */
static inline void *ldp_(const uint8_t *g, unsigned o) { void *p; memcpy(&p, g + o, 8); return p; }
static inline void stp_(uint8_t *g, unsigned o, const void *p) { memcpy(g + o, &p, 8); }

/* the original's effect for the calls that need no process_geometry and no swap; 0 (nothing written) otherwise */
static inline __attribute__((always_inline)) int queue_fast(uint8_t *g, uint32_t cmd, uint32_t param) {
    /* every load first (independent, so they overlap; the paths below are then short blocks of stores) */
    unsigned left = g[G_LEFT], swap = g[G_SWAP];
    uint8_t *c = ldp_(g, G_CMD_CUR);
    uint32_t *pc = ldp_(g, G_PAR_CUR), *pp = ldp_(g, G_PAR_PEND);
    cmd &= 0x7f;
    unsigned n = cmd_params[cmd];
    /* the command completes now: would the queue then hold more than 0x400 bytes (the original's signed compare) */
    int full = (int64_t)((uintptr_t)c + 1 - ((uintptr_t)g + G_QUEUE)) > G_QUEUE_MAX;
    /* keep the compiler from sinking the loads into the branches (an empty asm that "uses" them): the original loads
     * everything before its first store too; the fields and the queues never overlap, so this is for the schedule
     * (the loads overlap each other), not for exactness */
    __asm__("" : "+r"(left), "+r"(swap), "+r"(c), "+r"(pc), "+r"(pp), "+r"(n));
    if (left) {
        if (left != 1) {
            *pp = param;
            stp_(g, G_PAR_PEND, pp + 1);
            g[G_LEFT] = (uint8_t)(left - 1);
            return 1;
        }
        if (full) return 0;
        *pp = param;
        stp_(g, G_PAR_PEND, pp + 1);
        stp_(g, G_CMD_CUR, c + 1);
        stp_(g, G_PAR_CUR, pp + 1);
        stp_(g, G_CMD_PEND, c + 1);
        g[G_LEFT] = 0;
        return 1;
    }
    if (swap | (cmd == 0x50)) return 0;
    if (n > 1) {
        *c = (uint8_t)cmd;
        *pc = param;
        stp_(g, G_PAR_PEND, pc + 1);
        g[G_LEFT] = (uint8_t)(n - 1);
        return 1;
    }
    if (full) return 0;
    *c = (uint8_t)cmd;
    if (n) {
        *pc = param;
        stp_(g, G_PAR_CUR, pc + 1);
        stp_(g, G_PAR_PEND, pc + 1);
    }
    stp_(g, G_CMD_CUR, c + 1);
    stp_(g, G_CMD_PEND, c + 1);
    return 1;
}

int geom_queue_ours(uint8_t *g, uint32_t cmd, uint32_t param) { return queue_fast(g, cmd, param); }

/* the installed replacement: the fast paths, else the original (through the trampoline, a tail call) */
void geom_queue_command(uint8_t *g, uint32_t cmd, uint32_t param) {
    if (__builtin_expect(!queue_fast(g, cmd, param), 0)) geom_queue_orig(g, cmd, param);
}

/* ---- geometry_add_vertex ---- */
#define DS_GEOMETRY_ADD_VERTEX 0x5e890
#define G_BATCH_N    0x300          /* u32: vertices in the batch */
#define G_VTX_TOTAL  0x338          /* u32 */
#define G_BATCH_TEX  0x52c          /* u32[64]: texture coordinates */
#define G_TEXMTX     0x9864         /* s32[16]: the texture matrix */
#define G_TEXCOORD   0x9a98         /* u16 s, t: the current texture coordinates */
#define G_TEXBASE    0x9aa2         /* u16 s, t: the TEXCOORD command's values, mode 3's offset */
#define G_TEXMODE    0x9ac4         /* u8: texture coordinate transform mode */
#define G_MTX_DIRTY  0x9ad0         /* u8: the clip matrix needs recomputing */

geom_vertex_fn *geom_vertex_orig;

static inline int32_t ld32(const uint8_t *g, uintptr_t o) { int32_t v; memcpy(&v, g + o, 4); return v; }
static inline void st32(uint8_t *g, uintptr_t o, uint32_t v) { memcpy(g + o, &v, 4); }
static inline uint16_t ld16(const uint8_t *g, uintptr_t o) { uint16_t v; memcpy(&v, g + o, 2); return v; }
static inline void st16(uint8_t *g, uintptr_t o, uint16_t v) { memcpy(g + o, &v, 2); }

static inline __attribute__((always_inline)) int add_vertex_fast(uint8_t *g, int32_t x, int32_t y, int32_t z) {
    uint32_t n = (uint32_t)ld32(g, G_BATCH_N);
    if (g[G_MTX_DIRTY] | (n == 64)) return 0;
    if (g[G_TEXMODE] == 3) {
        uint64_t s = (uint64_t)((int64_t)x * ld32(g, G_TEXMTX)) + (uint64_t)((int64_t)y * ld32(g, G_TEXMTX + 0x10)) +
                     (uint64_t)((int64_t)z * ld32(g, G_TEXMTX + 0x20));
        uint64_t t = (uint64_t)((int64_t)x * ld32(g, G_TEXMTX + 4)) + (uint64_t)((int64_t)y * ld32(g, G_TEXMTX + 0x14)) +
                     (uint64_t)((int64_t)z * ld32(g, G_TEXMTX + 0x24));
        st16(g, G_TEXCOORD, (uint16_t)(ld16(g, G_TEXBASE) + (uint32_t)((int64_t)s >> 24)));
        st16(g, G_TEXCOORD + 2, (uint16_t)(ld16(g, G_TEXBASE + 2) + (uint32_t)((int64_t)t >> 24)));
    }
    uintptr_t o = (uintptr_t)n * 4;
    st32(g, o, (uint32_t)x);
    st32(g, o + 0x100, (uint32_t)y);
    st32(g, o + 0x200, (uint32_t)z);
    st32(g, o + G_BATCH_TEX, (uint32_t)ld32(g, G_TEXCOORD));
    st32(g, G_BATCH_N, n + 1);
    st32(g, G_VTX_TOTAL, (uint32_t)ld32(g, G_VTX_TOTAL) + 1);
    return 1;
}

int geom_vertex_ours(uint8_t *g, int32_t x, int32_t y, int32_t z) { return add_vertex_fast(g, x, y, z); }

void geom_add_vertex(uint8_t *g, int32_t x, int32_t y, int32_t z) {
    if (__builtin_expect(!add_vertex_fast(g, x, y, z), 0)) geom_vertex_orig(g, x, y, z);
}

/* ---- video_3d_bin_polygons_y_sort_4x ---- */
#define DS_YSORT_4X             0x549f0
#define DS_TEXTURE_CACHE_LOOKUP 0x6b720     /* (cache, u32 key, u32 key2) -> texture */
#define DS_RENDER_SPRITE_CHECK  0x43e20     /* render_sprite_check.isra.0(poly, vertices) */
#define DS_SORT_COUNTS          0x169330    /* polygon_sort_counts.15122: s16[0x181], zero between calls */
#define DS_SORT_LIST            0x169640    /* polygon_sort_list.15121: u16[0x181][0x800] */
#define G_SORT_FLAGS            0x9acc      /* u8: bit 1 (the w-buffer?) changes the shading flags */
#define G_TEXCACHE              0x9a38      /* u64: the texture cache */
#define YS_ROWS                 0x181
#define YS_BINS                 12
#define YS_BIN_BYTES            0x1004      /* u16[0x800] polygon indices, then the u32 count */

geom_ysort_fn *geom_ysort_orig;
typedef void *tex_lookup_fn(void *cache, uint32_t key, uint32_t key2);
typedef void sprite_check_fn(uint8_t *poly, uint8_t *verts);

/* the rows whose count is not 0: a byte a row, 0 or 1 (an append is one store), read 8 rows at a time */
#define YS_WORDS ((YS_ROWS + 7) / 8)
static inline uint64_t ne_word(const uint8_t *ne, int w) { uint64_t v; memcpy(&v, ne + 8 * w, 8); return v; }
/* the highest group below `w` whose bit is set in `wm`, or -1 */
static inline int ys_below(uint64_t wm, int w) { wm &= w >= 64 ? ~0ull : (1ull << w) - 1; return wm ? 63 - __builtin_clzll(wm) : -1; }

void geom_ysort(uint8_t *geom, uint8_t *bins, uint8_t *polys, uint8_t *verts, uint32_t flags) {
    int16_t *counts = (int16_t *)(ds_base + DS_SORT_COUNTS);
    uint16_t *list = (uint16_t *)(ds_base + DS_SORT_LIST);
    tex_lookup_fn *lookup = (tex_lookup_fn *)(ds_base + DS_TEXTURE_CACHE_LOOKUP);
    sprite_check_fn *sprite = (sprite_check_fn *)(ds_base + DS_RENDER_SPRITE_CHECK);
    for (int k = 0; k < YS_BINS; k++) st32(bins + k * YS_BIN_BYTES, 0x1000, 0);
    /* the rows whose count is not 0 (none, between DraStic's calls: every pass zeroes the rows it reads; the original
     * visits every row, but an empty row's visit only stores 0 over 0) */
    uint8_t ne[YS_WORDS * 8] __attribute__((aligned(8)));
    /* rows 0..0x17f as 48 vectors of 8 counts, ORed in four chains, and row 0x180 */
    uint16x8_t o0 = vld1q_u16((const uint16_t *)counts), o1 = vld1q_u16((const uint16_t *)counts + 8);
    uint16x8_t o2 = vld1q_u16((const uint16_t *)counts + 16), o3 = vld1q_u16((const uint16_t *)counts + 24);
    for (int k = 32; k < YS_ROWS - 1; k += 32) {
        o0 = vorrq_u16(o0, vld1q_u16((const uint16_t *)counts + k));
        o1 = vorrq_u16(o1, vld1q_u16((const uint16_t *)counts + k + 8));
        o2 = vorrq_u16(o2, vld1q_u16((const uint16_t *)counts + k + 16));
        o3 = vorrq_u16(o3, vld1q_u16((const uint16_t *)counts + k + 24));
    }
    uint16_t any = vmaxvq_u16(vorrq_u16(vorrq_u16(o0, o1), vorrq_u16(o2, o3))) | (uint16_t)counts[YS_ROWS - 1];
    /* and a bit per 8 rows (49), set with the rows' bytes: the passes skip empty groups of 8 in a step */
    uint64_t wm = 0;
    memset(ne, 0, sizeof ne);
    if (__builtin_expect(any != 0, 0))
        for (int y = 0; y < YS_ROWS; y++) if (counts[y]) { ne[y] = 1; wm |= 1ull << (y >> 3); }

    /* the polygons: y range, flags, texture, then by top y into the rows */
    uint32_t key = 0xffffffff, key2 = 0xffffffff;
    void *tex = 0;
    for (uint32_t i = 0; i < (uint32_t)ld32(polys, 0x10000); i++) {
        uint8_t *P = polys + (uintptr_t)i * 32;
        uint32_t w8 = (uint32_t)ld32(P, 8), n = w8 & 0xf;
        if (n > 8 || !(w8 & 0xe)) continue;
        uint8_t *V = verts + (uintptr_t)ld16(P, 0x1a) * 16;
        /* vertex 0's y as it is, the others' clamped to 0x180 and stored back; the top is the first lowest y */
        uint32_t top = ld16(V, 6), bot = top, topi = 0;
        uint32_t wa = (uint32_t)ld32(V, 0), wo = wa, h8a = ld16(V, 8), h8o = h8a, haa = ld16(V, 0xa), hao = haa;
        for (uint32_t k = 1; k < n; k++) {
            uint8_t *W = V + 16 * k;
            uint32_t y = ld16(W, 6);
            if (y > 0x180) y = 0x180;
            st16(W, 6, (uint16_t)y);
            if (y < top) { top = y; topi = k; }
            if (y > bot) bot = y;
            uint32_t w = (uint32_t)ld32(W, 0), h8 = ld16(W, 8), ha = ld16(W, 0xa);
            wa &= w; wo |= w; h8a &= h8; h8o |= h8; haa &= ha; hao |= ha;
        }
        if (bot == top) continue;
        /* the same +0xa (4), +8 (0x10) or word 0 (0x20) at every vertex; bit 1 of +0x9acc: 0x30 on word 0, and 8 */
        uint32_t f = hao == haa ? flags | 4 : flags;
        if (geom[G_SORT_FLAGS] & 2) { if (wa == wo) f |= 0x30; f |= 8; }
        else { if (h8a == h8o) f |= 0x10; if (wa == wo) f |= 0x20; }
        uint32_t tw = (uint32_t)ld32(P, 0);
        if ((tw & 0x1c000000) && ((uint32_t)ld32(P, 4) >> 4 & 3) != 3) {
            uint32_t t2 = ld16(P, 0x18);
            f |= 2;
            if (tw != key || t2 != key2) {           /* the last lookup's texture when the keys repeat */
                key2 = t2; key = tw;
                tex = lookup(ldp_(geom, G_TEXCACHE), tw, t2);
                w8 = (uint32_t)ld32(P, 8);            /* the original reloads it after the call */
            }
            stp_(P, 0x10, tex);
        }
        uint32_t w = (w8 | bot << 23) | f << 8;
        if (n == 4) {
            uint32_t ti = topi << 16;
            if (!(w & 0x40)) w |= 0x200000;
            else {
                if (topi == 2) ti = ld16(V, 0x36) != top ? 0x20000 : 0x30000;
                w |= 0x480000;
            }
            st32(P, 8, w | ti);
            sprite(P, V);
        } else st32(P, 8, w | topi << 16 | n << 19);
        int16_t c = counts[top];
        list[(intptr_t)c + ((intptr_t)top << 11)] = (uint16_t)i;
        counts[top] = (int16_t)(c + 1);
        ne[top] = 1;
        wm |= 1ull << (top >> 3);
    }

    /* rows 0x180 down to 0: each row's polygons, last first, into the rows of their bottom y (clamped) */
    for (int w = ys_below(wm, 64); w >= 0; w = ys_below(wm, w))
        for (uint64_t m = ne_word(ne, w); m; ) {
            int y = 8 * w + (63 - __builtin_clzll(m)) / 8;              /* the highest such row of the 8 */
            const uint16_t *row = list + ((intptr_t)y << 11);
            for (int32_t j = counts[y] - 1; j >= 0; j--) {
                uint16_t idx = row[j];
                uint32_t b = (uint32_t)ld32(polys, (uintptr_t)idx * 32 + 8) >> 23;
                if (b > 0x180) b = 0x180;
                int16_t c = counts[b];
                counts[b] = (int16_t)(c + 1);
                list[(intptr_t)c + ((intptr_t)b << 11)] = idx;
                ne[b] = 1;
                wm |= 1ull << (b >> 3);
            }
            counts[y] = 0;
            ne[y] = 0;
            /* the rows below y in these 8, as they are now (appends may have set some; those above y are visited by
             * the third pass, as the original's downward walk leaves them) */
            m = ne_word(ne, w) & ((1ull << (8 * (y - 8 * w))) - 1);
        }

    /* rows 1..0x180 (bottom y), each row's polygons last first, into the bins from the top vertex's to the row's */
    ne[0] = 0;
    for (uint64_t ws = wm; ws; ws &= ws - 1) {
        int w = __builtin_ctzll(ws);
        for (uint64_t m = ne_word(ne, w); m; m &= m - 1) {
            int r = 8 * w + __builtin_ctzll(m) / 8;
            uint32_t y = r - 1, range = 0xfffu >> (11 - (y >> 5));
            const uint16_t *row = list + ((intptr_t)r << 11);
            for (int32_t j = counts[r] - 1; j >= 0; j--) {
                uint16_t idx = row[j];
                const uint8_t *P = polys + (uintptr_t)idx * 32;
                uint32_t vi = ((ld16(P, 0xa) & 7) + ld16(P, 0x1a)) & 0x1ffff;
                uint32_t bm = (0xfffu << ((ld16(verts, vi * 16 + 6) >> 5) & 31)) & range;
                while (bm) {
                    uint8_t *B = bins + __builtin_ctz(bm) * YS_BIN_BYTES;
                    uint32_t cnt = (uint32_t)ld32(B, 0x1000);
                    st16(B, (uintptr_t)cnt * 2, idx);
                    st32(B, 0x1000, cnt + 1);
                    bm &= bm - 1;
                }
            }
            counts[r] = 0;
        }
    }
    counts[0] = 0;
}

/* ---- geometry_transform_vertex_block_asm: geom_tvb.S ---- */
#define DS_TRANSFORM_VERTEX_BLOCK 0x9dda0
geom_tvb_fn *geom_tvb_orig;

/* ---- check mode ---- */
#define QC_AROUND 32
static unsigned long qc_n[2], qc_bad;      /* [0] handled by ours (checked), [1] to the original */
static unsigned long vc_n[2], vc_bad, yc_n, yc_bad, tc_n, tc_bad;
static struct timespec qc_last;

static void qc_report(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - qc_last.tv_sec < 2) return;
    qc_last = now;
    fprintf(stderr, "[geom] queue: %lu checked, %lu differ, %lu to DraStic's; add_vertex: %lu checked, %lu differ, %lu to "
            "DraStic's; y-sort: %lu checked, %lu differ; vertex block: %lu checked, %lu differ\n", qc_n[0], qc_bad, qc_n[1],
            vc_n[0], vc_bad, vc_n[1], yc_n, yc_bad, tc_n, tc_bad);
}

/* the bytes a call may write: the state's queue fields and 32 bytes either side of the three cursors' targets (the
 * queues start at +0x79b00 and +0x81b00, so the 32 bytes before a cursor are inside the state even at a queue's start) */
struct qc_img { uint8_t f[0x100], c[2 * QC_AROUND], p[2 * QC_AROUND], q[2 * QC_AROUND]; };
static void qc_take(struct qc_img *m, uint8_t *g, uint8_t *c, uint8_t *p, uint8_t *q) {
    memcpy(m->f, g + 0x9a00, 0x100);
    memcpy(m->c, c - QC_AROUND, 2 * QC_AROUND); memcpy(m->p, p - QC_AROUND, 2 * QC_AROUND); memcpy(m->q, q - QC_AROUND, 2 * QC_AROUND);
}
static void qc_put(const struct qc_img *m, uint8_t *g, uint8_t *c, uint8_t *p, uint8_t *q) {
    /* the four regions of one image were taken at one moment, so where they overlap they hold the same bytes */
    memcpy(q - QC_AROUND, m->q, 2 * QC_AROUND); memcpy(p - QC_AROUND, m->p, 2 * QC_AROUND); memcpy(c - QC_AROUND, m->c, 2 * QC_AROUND);
    memcpy(g + 0x9a00, m->f, 0x100);
}

void geom_queue_command_check(uint8_t *g, uint32_t cmd, uint32_t param) {
    uint8_t *c = ldp_(g, G_CMD_CUR), *p = ldp_(g, G_PAR_CUR), *q = ldp_(g, G_PAR_PEND);
    struct qc_img in, ours, ref;
    qc_take(&in, g, c, p, q);
    if (!queue_fast(g, cmd, param)) {
        geom_queue_orig(g, cmd, param);
        qc_n[1]++;
        qc_report();
        return;
    }
    qc_take(&ours, g, c, p, q);
    qc_put(&in, g, c, p, q);
    geom_queue_orig(g, cmd, param);
    qc_take(&ref, g, c, p, q);
    qc_n[0]++;
    if (memcmp(&ours, &ref, sizeof ours) && qc_bad++ < 30) {
        fprintf(stderr, "[geomcheck] queue_geometry_command(%p, %#x, %#x) left %u swap %u queued %ld differs from DraStic's:",
                (void *)g, cmd, param, in.f[G_LEFT - 0x9a00], in.f[G_SWAP - 0x9a00], (long)(c - (g + G_QUEUE)));
        const uint8_t *a = (const uint8_t *)&ref, *b = (const uint8_t *)&ours;
        for (unsigned i = 0, k = 0; i < sizeof ours && k < 8; i++)
            if (a[i] != b[i]) { fprintf(stderr, " [%u] %02x/%02x", i, a[i], b[i]); k++; }
        fprintf(stderr, " (DraStic/ours; 0..255 the fields from +0x9a00, then the cmd, par, pend areas)\n");
    }
    /* the original's bytes stay: they are DraStic's */
    qc_report();
}

#define VC_LO 0x800
#define VC_HI_AT 0x9800
#define VC_HI 0x300
static uint8_t vc_in[VC_LO + VC_HI], vc_ours[VC_LO + VC_HI];

void geom_add_vertex_check(uint8_t *g, int32_t x, int32_t y, int32_t z) {
    memcpy(vc_in, g, VC_LO); memcpy(vc_in + VC_LO, g + VC_HI_AT, VC_HI);
    if (!add_vertex_fast(g, x, y, z)) { geom_vertex_orig(g, x, y, z); vc_n[1]++; qc_report(); return; }
    memcpy(vc_ours, g, VC_LO); memcpy(vc_ours + VC_LO, g + VC_HI_AT, VC_HI);
    memcpy(g, vc_in, VC_LO); memcpy(g + VC_HI_AT, vc_in + VC_LO, VC_HI);
    geom_vertex_orig(g, x, y, z);
    vc_n[0]++;
    int bad = memcmp(vc_ours, g, VC_LO) || memcmp(vc_ours + VC_LO, g + VC_HI_AT, VC_HI);
    if (bad && vc_bad++ < 30) {
        fprintf(stderr, "[geomcheck] geometry_add_vertex(%p, %d, %d, %d) n %u mode %u differs from DraStic's:", (void *)g, x, y, z,
                (uint32_t)ld32(vc_in, G_BATCH_N), vc_in[VC_LO + G_TEXMODE - VC_HI_AT]);
        for (unsigned i = 0, k = 0; i < VC_LO + VC_HI && k < 8; i++) {
            uint8_t r = i < VC_LO ? g[i] : g[VC_HI_AT + i - VC_LO];
            if (r != vc_ours[i]) { fprintf(stderr, " [+%#x] %02x/%02x", i < VC_LO ? i : VC_HI_AT + i - VC_LO, r, vc_ours[i]); k++; }
        }
        fprintf(stderr, " (DraStic/ours)\n");
    }
    /* the original's bytes stay */
    qc_report();
}

/* y-sort: the polygons (and their count), the vertices, the bins and DraStic's two sort arrays, before and after
 * (three images of ~1.6 MB memcpy'd per call: fine for a check mode); the texture lookups run twice (the second finds
 * the first's entry: the same texture) */
#define YC_POLYS 0x10004
#define YC_VERTS 0x18004
#define YC_BINS (YS_BINS * YS_BIN_BYTES)
#define YC_SORT (DS_SORT_LIST + YS_ROWS * 0x1000 - DS_SORT_COUNTS)
static uint8_t *yc_buf;

static void yc_take(uint8_t *to, const uint8_t *polys, const uint8_t *verts, const uint8_t *bins) {
    memcpy(to, polys, YC_POLYS); memcpy(to + YC_POLYS, verts, YC_VERTS); memcpy(to + YC_POLYS + YC_VERTS, bins, YC_BINS);
    memcpy(to + YC_POLYS + YC_VERTS + YC_BINS, (const void *)(ds_base + DS_SORT_COUNTS), YC_SORT);
}
static void yc_put(const uint8_t *from, uint8_t *polys, uint8_t *verts, uint8_t *bins) {
    memcpy(polys, from, YC_POLYS); memcpy(verts, from + YC_POLYS, YC_VERTS); memcpy(bins, from + YC_POLYS + YC_VERTS, YC_BINS);
    memcpy((void *)(ds_base + DS_SORT_COUNTS), from + YC_POLYS + YC_VERTS + YC_BINS, YC_SORT);
}
#define YC_SIZE (YC_POLYS + YC_VERTS + YC_BINS + YC_SORT)

void geom_ysort_check(uint8_t *geom, uint8_t *bins, uint8_t *polys, uint8_t *verts, uint32_t flags) {
    if (!yc_buf) yc_buf = malloc(3 * YC_SIZE);
    uint8_t *in = yc_buf, *ours = yc_buf + YC_SIZE, *ref = yc_buf + 2 * YC_SIZE;
    yc_take(in, polys, verts, bins);
    geom_ysort(geom, bins, polys, verts, flags);
    yc_take(ours, polys, verts, bins);
    yc_put(in, polys, verts, bins);
    geom_ysort_orig(geom, bins, polys, verts, flags);
    yc_take(ref, polys, verts, bins);
    yc_n++;
    if (memcmp(ours, ref, YC_SIZE) && yc_bad++ < 30) {
        static const char *what[] = { "polys", "verts", "bins", "sort arrays" };
        static const unsigned at[] = { 0, YC_POLYS, YC_POLYS + YC_VERTS, YC_POLYS + YC_VERTS + YC_BINS, YC_SIZE };
        fprintf(stderr, "[geomcheck] video_3d_bin_polygons_y_sort_4x(bins %p, polys %p (%u), flags %u) differs from DraStic's:",
                (void *)bins, (void *)polys, (uint32_t)ld32(in, 0x10000), flags);
        for (int r = 0; r < 4; r++)
            for (unsigned i = at[r]; i < at[r + 1]; i++)
                if (ours[i] != ref[i]) { fprintf(stderr, " %s +%#x: %02x/%02x", what[r], i - at[r], ref[i], ours[i]); break; }
        fprintf(stderr, " (DraStic/ours)\n");
    }
    qc_report();
}

/* the vertex block: +0..+0x4000 holds the batch, the matrix's page is elsewhere (read only), the clip-space vertices
 * and clip codes (+0x650 + 16i, +0x1290 + i for i < 196 and the group past the batch) */
#define TC_BYTES 0x4000
static uint8_t tc_in[TC_BYTES], tc_ours[TC_BYTES];
void geom_tvb_check(uint8_t *g) {
    memcpy(tc_in, g, TC_BYTES);
    geom_tvb(g);
    memcpy(tc_ours, g, TC_BYTES);
    memcpy(g, tc_in, TC_BYTES);
    geom_tvb_orig(g);
    tc_n++;
    if (memcmp(tc_ours, g, TC_BYTES) && tc_bad++ < 30) {
        fprintf(stderr, "[geomcheck] geometry_transform_vertex_block_asm(%p) n %u from %u differs from DraStic's:", (void *)g,
                (uint32_t)ld32(tc_in, G_BATCH_N), (uint32_t)ld32(tc_in, 0x330));
        for (unsigned i = 0, k = 0; i < TC_BYTES && k < 8; i++)
            if (g[i] != tc_ours[i]) { fprintf(stderr, " [+%#x] %02x/%02x", i, g[i], tc_ours[i]); k++; }
        fprintf(stderr, " (DraStic/ours)\n");
    }
    qc_report();
}

static const uint32_t expect_queue[4] = { 0xa9bd7bfd, 0x910003fd, 0xf90013f5, 0x91402415 };
static const uint32_t expect_vertex[4] = { 0xa9bb7bfd, 0x910003fd, 0xa90363f7, 0x91402417 };
static const uint32_t expect_tvb[4] = { 0x91402401, 0xb9433003, 0x911f9021, 0xb9430002 };
static const uint32_t expect_ysort[4] = { 0xa9af7bfd, 0x910003fd, 0xa90153f3, 0xaa0103f4 };

void geom_init(void) {
    const char *e = getenv("DSFLIP_RAST_GEOM");
    if (!e) e = getenv("RAST_GEOM");
    if (e && !atoi(e)) return;
    int check = getenv("RAST_GEOMCHECK") && atoi(getenv("RAST_GEOMCHECK"));
    geom_tables(ds_base);
    if (check) clock_gettime(CLOCK_MONOTONIC, &qc_last);
    geom_queue_orig = (geom_queue_fn *)rast_hook(DS_QUEUE_GEOMETRY_COMMAND, expect_queue,
                                                 check ? (void *)geom_queue_command_check : (void *)geom_queue_command);
    if (!geom_queue_orig) { fprintf(stderr, "[geom] unknown queue_geometry_command, not replaced\n"); return; }
    geom_vertex_orig = (geom_vertex_fn *)rast_hook(DS_GEOMETRY_ADD_VERTEX, expect_vertex,
                                                   check ? (void *)geom_add_vertex_check : (void *)geom_add_vertex);
    geom_ysort_orig = (geom_ysort_fn *)rast_hook(DS_YSORT_4X, expect_ysort, check ? (void *)geom_ysort_check : (void *)geom_ysort);
    geom_tvb_orig = (geom_tvb_fn *)rast_hook(DS_TRANSFORM_VERTEX_BLOCK, expect_tvb, check ? (void *)geom_tvb_check : (void *)geom_tvb);
    fprintf(stderr, "[geom] queue_geometry_command%s%s%s replaced%s\n", geom_vertex_orig ? ", geometry_add_vertex" : "",
            geom_ysort_orig ? ", video_3d_bin_polygons_y_sort_4x" : "", geom_tvb_orig ? ", geometry_transform_vertex_block_asm" : "",
            check ? ", checking every call against DraStic's" : "");
}
