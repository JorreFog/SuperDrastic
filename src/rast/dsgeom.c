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
 * geometry_transform_vertexes(geom) (0x5e060): finishes a batch. 38 bl sites, all C: geometry_add_vertex (a full
 * batch), geometry_load_polygon_count / geometry_load_vertex_count (GXSTAT reads), update_frame_geometry and 34 in
 * process_geometry_commands' handlers; it calls the matrix-multiply and vertex-block asm (outside its unit), so GCC's
 * IPA-RA cannot have narrowed its clobber set for them and an AAPCS replacement is safe. The original (0x5e060..0x5e884):
 *   n = the batch count (+0x300); nothing when 0. With the matrix flag (+0x9ad0) the clip matrix (+0x97e4) is rebuilt
 *   first (geometry_matrix_multiply_4x4_by_4x4_asm of +0x9824 and the position matrix at *+0x9a58), the flag cleared.
 *   The vertex block (above) transforms the batch to the clip-space vertices from index i0 = +0x330; the batch's
 *   texture coordinate words (+0x52c) are copied to +0x14dc + 4 (i0 + k).
 *   Lighting: the batch's normals (+0x33c.., packed 10.10.10, their end at +0x318) each give a colour: the normal
 *   through the direction matrix (*+0x9a60, its first three rows, 64-bit products, bits 12..43 of each sum), then for
 *   each light i of the mask (+0x9ac3, every set bit of the byte): dif = ((ty*ly) >> 9) + ((tx*lx) >> 9) + ((tz*lz)
 *   >> 9) with the light vector at +0x96c8 + 12 i, 32-bit products and sums, 0 when negative; spec the same with the
 *   half vector at +0x96f8 + 12 i, then squared; with the shininess table on (+0x9ace) spec = 2 * table[min(spec >>
 *   11, 127)] (+0x98a4; the clamp is signed: a wrapped square stays negative and indexes below the table, as the
 *   original does), else spec >>= 9; r += dif * (u16 at +0x9728 + 6 i) + spec * (u16 at +0x9740 + 6 i), g and b
 *   with +2 and +4, all starting from the three words at +0x9758 and wrapping; the colour is (b >> 14 at most 31) <<
 *   10 | (g >> 14 ...) << 5 | (r >> 14 ...). geom_light() does this four normals at a time in NEON lanes, lane for
 *   lane the same arithmetic (the lights in a scalar loop, so any number of them; the table read per lane).
 *   Colours: the output colours at +0x1354 + 2 (i0 + k), k < n, come from the colour event stream (+0x4dc.., its end
 *   at +0x328; process_geometry_commands pushes one byte per NORMAL or COLOR command: the batch count then (bits
 *   0..6) and bit 7 for an explicit colour, whose value goes to the list at +0x44c (end +0x320); a command at the
 *   same count replaces the last event): vertices before the first event's index take the current colour (+0x644,
 *   a 32-bit word, its low 16 bits stored), those from an event's index to the next event's its colour (the explicit
 *   list and the lit colours each in order), and the current colour becomes the last event's. The original has three
 *   copies of that fill: with normals and explicit colours it walks the stream to its end and takes bit 7 per event;
 *   with normals only it walks as many events as normals (bit 7 ignored); with explicit colours only as many as
 *   colours. DraStic's streams always have one byte per list entry with strictly rising indices up to the count, so
 *   the three agree; tvx_fast() handles only such a stream (one pass checks it: the counts add up, the indices rise,
 *   the bit-7 count is the colour count) and passes any other state to the original untouched.
 *   Then +0x310 = 0xff, the three list ends reset, the count 0, +0x330 = i0 + n, +0x644 = the current colour, and
 *   when i0 + n > 127 geometry_flush_polygons(geom) (a tail call in the original; a call here).
 * The event fill is one memset-like store per run, the lighting ~90 instructions per 4 vertices with 2 lights
 * against the original's ~45 per vertex and light plus 12 for the normal.
 *
 * Check mode (RAST_GEOMCHECK=1): every call ours handles runs twice on the same bytes: ours, then the original through
 * the trampoline, and everything it may write is compared. queue_geometry_command: the geometry state's
 * +0x9a00..+0x9b00 (cursors, counts, flags) and 64 bytes around each of the three cursors' targets (the original would
 * also differ there if it called process_geometry, which rewinds the queue); geometry_add_vertex: +0..+0x800 (the
 * batch, its counts, the texture coordinates) and +0x9800..+0x9b00 (the texture matrix, the flags; the original
 * would differ there or in the batch count if it called either function); the y-sort: the polygons, the vertices,
 * the bins and DraStic's sort arrays (its texture lookups run twice: the second finds the first's entry); the vertex
 * block: +0..+0x4000 (the batch, the clip-space vertices and codes); the transform: +0..+0x1800 (the batch, the
 * lists, the output vertices) and +0x9700..+0x9b00 (the clip matrix, the flags), with the flush handled so: DraStic's
 * geometry_flush_polygons entry is hooked (geom_flush_check), the input is saved, ours runs with a flush it asks for
 * only recorded, the regions are copied out and the input restored, the original runs (its tail call to the flush
 * lands on the hook and is recorded the same way), the regions and the two flush decisions are compared (one side
 * asking alone is reported as a difference), and a flush the original asked for is then run once, on its bytes.
 * The "to DraStic's" count of the transform says how often the event stream rule passed a call on (never, on
 * consistent streams). Counts every 2 s ("[geom] queue:" lines, "[geomcheck]" for a difference). */
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

/* ---- queue_geometry_command_packed ---- */
#define DS_QUEUE_PACKED 0x62b10
geom_packed_fn *geom_packed_orig;

/* queue_geometry_command_packed(geom, word) (0x62b10; the JIT's 32-bit store helper arm64_store_memory32_arm9 at
 * 0x8d678, which saves w12-w18, x30 and NZCV around the bl as for the plain queue, and store_io_register_arm9_32 (C),
 * for GXFIFO and its mirrors 0x4000400..0x400043f: games' CPU-written packed display lists; AAPCS is enough). The original
 * (0x62b10..0x62c24): a parameter (left != 0): *par_pend++ = word; left == 1 completes the group: cmd_cur = cmd_pend,
 * par_cur = par_pend, then process_geometry when cmd_pend - queue > 0x400 (signed); left -= 1 after it. Else a
 * command word: its four 7-bit command bytes, the non-zero ones appended at cmd_cur in order; cmd_pend = past them;
 * n = the sum of their parameter counts: n != 0 leaves left = n (every parameter of the group comes through the
 * first path); n == 0 completes the group at once: cmd_cur = cmd_pend, process_geometry when past 0x400. The swap
 * flag is not consulted. packed_fast() does those stores in the original's order for the calls that do not run
 * process_geometry; the others go to the original before anything is written (as queue_fast). */
static inline __attribute__((always_inline)) int packed_fast(uint8_t *g, uint32_t w) {
    unsigned left = g[G_LEFT];
    uint8_t *c = ldp_(g, G_CMD_CUR), *cp = ldp_(g, G_CMD_PEND);
    uint32_t *pp = ldp_(g, G_PAR_PEND);
    unsigned c0 = w & 0x7f, c1 = (w >> 8) & 0x7f, c2 = (w >> 16) & 0x7f, c3 = (w >> 24) & 0x7f;
    unsigned n = cmd_params[c0] + cmd_params[c1] + cmd_params[c2] + cmd_params[c3];
    __asm__("" : "+r"(left), "+r"(c), "+r"(cp), "+r"(pp), "+r"(n));
    if (left) {
        if (left != 1) {
            *pp = w;
            stp_(g, G_PAR_PEND, pp + 1);
            g[G_LEFT] = (uint8_t)(left - 1);
            return 1;
        }
        if ((int64_t)((uintptr_t)cp - ((uintptr_t)g + G_QUEUE)) > G_QUEUE_MAX) return 0;
        *pp = w;
        stp_(g, G_PAR_PEND, pp + 1);
        stp_(g, G_CMD_CUR, cp);
        stp_(g, G_PAR_CUR, pp + 1);
        g[G_LEFT] = 0;
        return 1;
    }
    uint8_t *q = c + (c0 != 0) + (c1 != 0) + (c2 != 0) + (c3 != 0);
    if (!n && (int64_t)((uintptr_t)q - ((uintptr_t)g + G_QUEUE)) > G_QUEUE_MAX) return 0;
    if (c0) *c++ = (uint8_t)c0;
    if (c1) *c++ = (uint8_t)c1;
    if (c2) *c++ = (uint8_t)c2;
    if (c3) *c++ = (uint8_t)c3;
    stp_(g, G_CMD_PEND, c);
    if (n) g[G_LEFT] = (uint8_t)n;
    else stp_(g, G_CMD_CUR, c);
    return 1;
}

int geom_packed_ours(uint8_t *g, uint32_t w) { return packed_fast(g, w); }
void geom_queue_packed(uint8_t *g, uint32_t w) { if (__builtin_expect(!packed_fast(g, w), 0)) geom_packed_orig(g, w); }

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

/* ---- geometry_transform_vertexes ---- */
#define DS_TRANSFORM_VERTEXES  0x5e060
#define DS_FLUSH_POLYGONS      0x5aef0     /* geometry_flush_polygons(geom): static, x0 only */
#define DS_MATRIX_MUL_4X4_ASM  0x9df08     /* geometry_matrix_multiply_4x4_by_4x4_asm(dst, a, b) */
#define G_LAST_EVENT   0x310    /* u32: the last event byte pushed (0xff after a transform: no event matches a count) */
#define G_NORMAL_CUR   0x318    /* s32 *: the end of the batch's normals at +0x33c (packed 10.10.10) */
#define G_NORMALS      0x33c
#define G_COLOUR_CUR   0x320    /* u16 *: the end of the batch's explicit colours at +0x44c */
#define G_COLOURS      0x44c
#define G_EVENT_CUR    0x328    /* u8 *: the end of the colour events at +0x4dc */
#define G_EVENTS       0x4dc
#define G_OUT_BASE     0x330    /* u32: the index of the batch's first output vertex */
#define G_CUR_COLOUR   0x644    /* u32: the colour the next vertices take */
#define G_OUT_COLOUR   0x1354   /* u16[196]: the output vertices' colours */
#define G_OUT_TEX      0x14dc   /* u32[196]: their texture coordinates */
#define G_CLIP_MTX     0x97e4
#define G_PROJ_MTX     0x9824
#define G_POS_MTX_PTR  0x9a58   /* s32 (*)[16]: the position matrix the clip matrix is rebuilt from */
#define G_LIGHT_VEC    0x96c8   /* s32[4][3]: the lights' directions through the direction matrix */
#define G_HALF_VEC     0x96f8   /* s32[4][3]: their half vectors */
#define G_LIGHT_DIF    0x9728   /* u16[4][3]: light colour x material diffuse, r g b */
#define G_LIGHT_SPE    0x9740   /* u16[4][3]: light colour x material specular */
#define G_LIGHT_BASE   0x9758   /* u32[3]: the emission + ambient sums every vertex starts from */
#define G_SHIN_TABLE   0x98a4   /* u8[128]: the shininess table */
#define G_LIGHT_MASK   0x9ac3   /* u8: the enabled lights */
#define G_SHIN_ON      0x9ace   /* u8: the shininess table enabled */
#define G_DIR_MTX_PTR  0x9a60   /* s32 (*)[16]: the direction (light) matrix */
#define TVX_MAX_NORMALS 68      /* the original's stack array holds 64 colours and 4 words of slack */

geom_tvx_fn *geom_tvx_orig;
geom_flush_fn *geom_flush_orig;
static geom_matmul_fn *geom_matmul;
/* check mode: a flush the transform asks for while the two runs are compared is held and run once afterwards
 * (plain statics: the transform and the flush run on the emulation thread only) */
static int flush_hold, flush_wanted;
static inline void flush_call(uint8_t *g) { if (flush_hold) flush_wanted = 1; else geom_flush_orig(g); }
void geom_flush_check(uint8_t *g) { flush_call(g); }

/* the lit colours of the batch's nb normals, four at a time in NEON lanes (the lights in a scalar loop, so any
 * number of them): DraStic's arithmetic lane for lane (32-bit wrapping products and sums, the 64-bit normal
 * transform narrowed to bits 12..43, the signed clamps, the table lookups per lane). lit holds nb rounded up to 4. */
static void geom_light(const uint8_t *g, uint32_t nb, uint16_t *lit) {
    const uint8_t *M = ldp_(g, G_DIR_MTX_PTR);
    int32_t m0 = ld32(M, 0), m1 = ld32(M, 4), m2 = ld32(M, 8), m4 = ld32(M, 0x10), m5 = ld32(M, 0x14), m6 = ld32(M, 0x18),
            m8 = ld32(M, 0x20), m9 = ld32(M, 0x24), m10 = ld32(M, 0x28);
    uint32x4_t base_r = vdupq_n_u32((uint32_t)ld32(g, G_LIGHT_BASE)), base_g = vdupq_n_u32((uint32_t)ld32(g, G_LIGHT_BASE + 4)),
               base_b = vdupq_n_u32((uint32_t)ld32(g, G_LIGHT_BASE + 8));
    unsigned mask = g[G_LIGHT_MASK], shin = g[G_SHIN_ON];
    for (uint32_t v = 0; v < nb; v += 4) {
        int32x4_t nw;
        if (v + 4 <= nb) nw = vld1q_s32((const int32_t *)(g + G_NORMALS + 4 * v));
        else { int32_t t[4] = { 0, 0, 0, 0 }; memcpy(t, g + G_NORMALS + 4 * v, 4 * (nb - v)); nw = vld1q_s32(t); }
        /* the three 10-bit fields, sign-extended (sbfx) */
        int32x4_t nx = vshrq_n_s32(vshlq_n_s32(nw, 22), 22), ny = vshrq_n_s32(vshlq_n_s32(nw, 12), 22),
                  nz = vshrq_n_s32(vshlq_n_s32(nw, 2), 22);
        int32x2_t nxl = vget_low_s32(nx), nyl = vget_low_s32(ny), nzl = vget_low_s32(nz);
        /* (ny*m4 + nx*m0 + nz*m8) >> 12, the low 32 bits of a wrapping 64-bit sum (ubfx #12, #32): shrn */
        int64x2_t a = vmlal_n_s32(vmlal_n_s32(vmull_n_s32(nyl, m4), nxl, m0), nzl, m8);
        int64x2_t b = vmlal_high_n_s32(vmlal_high_n_s32(vmull_high_n_s32(ny, m4), nx, m0), nz, m8);
        int32x4_t tx = vshrn_high_n_s64(vshrn_n_s64(a, 12), b, 12);
        a = vmlal_n_s32(vmlal_n_s32(vmull_n_s32(nyl, m5), nxl, m1), nzl, m9);
        b = vmlal_high_n_s32(vmlal_high_n_s32(vmull_high_n_s32(ny, m5), nx, m1), nz, m9);
        int32x4_t ty = vshrn_high_n_s64(vshrn_n_s64(a, 12), b, 12);
        a = vmlal_n_s32(vmlal_n_s32(vmull_n_s32(nyl, m6), nxl, m2), nzl, m10);
        b = vmlal_high_n_s32(vmlal_high_n_s32(vmull_high_n_s32(ny, m6), nx, m2), nz, m10);
        int32x4_t tz = vshrn_high_n_s64(vshrn_n_s64(a, 12), b, 12);
        uint32x4_t r = base_r, gg = base_g, bb = base_b;
        for (unsigned i = 0, m = mask; m; i++, m >>= 1) {
            if (!(m & 1)) continue;
            const uint8_t *L = g + G_LIGHT_VEC + 12 * i, *H = g + G_HALF_VEC + 12 * i;
            /* ((ty*ly) >> 9) + ((tx*lx) >> 9) + ((tz*lz) >> 9), each product's low 32 bits, 0 when the sum is negative */
            int32x4_t d = vaddq_s32(vshrq_n_s32(vmulq_n_s32(ty, ld32(L, 4)), 9), vshrq_n_s32(vmulq_n_s32(tx, ld32(L, 0)), 9));
            d = vmaxq_s32(vaddq_s32(d, vshrq_n_s32(vmulq_n_s32(tz, ld32(L, 8)), 9)), vdupq_n_s32(0));
            int32x4_t s = vaddq_s32(vshrq_n_s32(vmulq_n_s32(ty, ld32(H, 4)), 9), vshrq_n_s32(vmulq_n_s32(tx, ld32(H, 0)), 9));
            s = vmaxq_s32(vaddq_s32(s, vshrq_n_s32(vmulq_n_s32(tz, ld32(H, 8)), 9)), vdupq_n_s32(0));
            s = vmulq_s32(s, s);
            if (shin) {
                /* (s >> 11) at most 127 (signed: a wrapped square stays negative) indexes the table; twice the byte */
                int32_t t[4];
                vst1q_s32(t, vminq_s32(vshrq_n_s32(s, 11), vdupq_n_s32(127)));
                for (int k = 0; k < 4; k++) t[k] = 2 * g[G_SHIN_TABLE + (intptr_t)t[k]];
                s = vld1q_s32(t);
            } else s = vshrq_n_s32(s, 9);
            const uint8_t *D = g + G_LIGHT_DIF + 6 * i, *P = g + G_LIGHT_SPE + 6 * i;
            uint32x4_t du = vreinterpretq_u32_s32(d), su = vreinterpretq_u32_s32(s);
            r = vmlaq_n_u32(vmlaq_n_u32(r, du, ld16(D, 0)), su, ld16(P, 0));
            gg = vmlaq_n_u32(vmlaq_n_u32(gg, du, ld16(D, 2)), su, ld16(P, 2));
            bb = vmlaq_n_u32(vmlaq_n_u32(bb, du, ld16(D, 4)), su, ld16(P, 4));
        }
        uint32x4_t c31 = vdupq_n_u32(31);
        r = vminq_u32(vshrq_n_u32(r, 14), c31); gg = vminq_u32(vshrq_n_u32(gg, 14), c31); bb = vminq_u32(vshrq_n_u32(bb, 14), c31);
        vst1_u16(lit + v, vmovn_u32(vorrq_u32(vorrq_u32(vshlq_n_u32(bb, 10), vshlq_n_u32(gg, 5)), r)));
    }
}

static inline void fill16(uint8_t *p, uint16_t c, uint32_t cnt) {
    uint16x8_t v = vdupq_n_u16(c);
    for (; cnt >= 8; cnt -= 8, p += 16) vst1q_u16((uint16_t *)p, v);
    for (; cnt; cnt--, p += 2) st16(p, 0, c);
}

/* the original's effect; 0 (nothing written) for a state the original's three fill variants would not read alike
 * (DraStic never builds one: the event stream is one byte per normal or colour, the vertex indices strictly rising) */
static inline __attribute__((always_inline)) int tvx_fast(uint8_t *g) {
    uint32_t n = (uint32_t)ld32(g, G_BATCH_N);
    if (!n) return 1;
    const uint8_t *ncur = ldp_(g, G_NORMAL_CUR), *ccur = ldp_(g, G_COLOUR_CUR), *ecur = ldp_(g, G_EVENT_CUR);
    uintptr_t nb = (uintptr_t)(ncur - (g + G_NORMALS)) >> 2, na = (uintptr_t)(ccur - (g + G_COLOURS)) >> 1, ne = ecur - (g + G_EVENTS);
    if (ne != na + nb || nb > TVX_MAX_NORMALS || na > 72) return 0;
    const uint8_t *ev = g + G_EVENTS;
    for (uintptr_t k = 0, prev = 0, nc = 0; k < ne; k++) {
        uint32_t e = ev[k], idx = e & 0x7f;
        if ((k && idx <= prev) || idx > n) return 0;
        nc += e >> 7;
        prev = idx;
        if (k + 1 == ne && nc != na) return 0;
    }
    uint32_t i0 = (uint32_t)ld32(g, G_OUT_BASE);
    if (g[G_MTX_DIRTY]) { geom_matmul(g + G_CLIP_MTX, g + G_PROJ_MTX, ldp_(g, G_POS_MTX_PTR)); g[G_MTX_DIRTY] = 0; }
    geom_tvb(g);
    uint32_t end = i0 + n;
    memcpy(g + G_OUT_TEX + 4 * (uintptr_t)i0, g + G_BATCH_TEX, 4 * (size_t)n);
    uint16_t lit[TVX_MAX_NORMALS + 4];
    if (nb) geom_light(g, (uint32_t)nb, lit);
    /* the colours: the current one up to each event's vertex, then that event's (explicit or lit) */
    uint8_t *out = g + G_OUT_COLOUR + 2 * (uintptr_t)i0;
    const uint8_t *ex = g + G_COLOURS;
    const uint16_t *li = lit;
    uint32_t cur = (uint32_t)ld32(g, G_CUR_COLOUR), prev = 0;
    for (uintptr_t k = 0; k < ne; k++) {
        uint32_t e = ev[k], idx = e & 0x7f, cnt = idx - prev;
        fill16(out, (uint16_t)cur, cnt);
        out += 2 * cnt;
        if (e & 0x80) { cur = ld16(ex, 0); ex += 2; } else cur = *li++;
        prev = idx;
    }
    fill16(out, (uint16_t)cur, n - prev);
    st32(g, G_LAST_EVENT, 0xff);
    stp_(g, G_NORMAL_CUR, g + G_NORMALS);
    stp_(g, G_COLOUR_CUR, g + G_COLOURS);
    st32(g, G_BATCH_N, 0);
    stp_(g, G_EVENT_CUR, g + G_EVENTS);
    st32(g, G_OUT_BASE, end);
    st32(g, G_CUR_COLOUR, cur);
    if (end > 0x7f) flush_call(g);
    return 1;
}

int geom_tvx_ours(uint8_t *g) { return tvx_fast(g); }
void geom_transform_vertexes(uint8_t *g) { if (__builtin_expect(!tvx_fast(g), 0)) geom_tvx_orig(g); }

/* ---- check mode ---- */
#define QC_AROUND 32
static unsigned long qc_n[2], qc_bad, pc_n[2], pc_bad;      /* [0] handled by ours (checked), [1] to the original */
static unsigned long vc_n[2], vc_bad, yc_n, yc_bad, tc_n, tc_bad, xc_n[2], xc_bad;
static struct timespec qc_last;

static void qc_report(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - qc_last.tv_sec < 2) return;
    qc_last = now;
    fprintf(stderr, "[geom] queue: %lu checked, %lu differ, %lu to DraStic's; add_vertex: %lu checked, %lu differ, %lu to "
            "DraStic's; y-sort: %lu checked, %lu differ; vertex block: %lu checked, %lu differ; transform: %lu checked, %lu "
            "differ, %lu to DraStic's; packed: %lu checked, %lu differ, %lu to DraStic's\n", qc_n[0], qc_bad, qc_n[1], vc_n[0],
            vc_bad, vc_n[1], yc_n, yc_bad, tc_n, tc_bad, xc_n[0], xc_bad, xc_n[1], pc_n[0], pc_bad, pc_n[1]);
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


void geom_queue_packed_check(uint8_t *g, uint32_t w) {
    uint8_t *c = ldp_(g, G_CMD_CUR), *p = ldp_(g, G_PAR_CUR), *q = ldp_(g, G_PAR_PEND);
    struct qc_img in, ours, ref;
    qc_take(&in, g, c, p, q);
    if (!packed_fast(g, w)) {
        geom_packed_orig(g, w);
        pc_n[1]++;
        qc_report();
        return;
    }
    qc_take(&ours, g, c, p, q);
    qc_put(&in, g, c, p, q);
    geom_packed_orig(g, w);
    qc_take(&ref, g, c, p, q);
    pc_n[0]++;
    if (memcmp(&ours, &ref, sizeof ours) && pc_bad++ < 30) {
        fprintf(stderr, "[geomcheck] queue_geometry_command_packed(%p, %#x) left %u queued %ld differs from DraStic's:",
                (void *)g, w, in.f[G_LEFT - 0x9a00], (long)(c - (g + G_QUEUE)));
        const uint8_t *a = (const uint8_t *)&ref, *b = (const uint8_t *)&ours;
        for (unsigned i = 0, k = 0; i < sizeof ours && k < 8; i++)
            if (a[i] != b[i]) { fprintf(stderr, " [%u] %02x/%02x", i, a[i], b[i]); k++; }
        fprintf(stderr, " (DraStic/ours)\n");
    }
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

/* transform_vertexes: the batch, its lists and counts, the output vertices (+0..+0x1800) and the clip matrix and
 * flags (+0x9700..+0x9b00). The hook on geometry_flush_polygons (0x5aef0) exists in check mode only; its entry's
 * first four instructions (mov x12 / sub sp / mov x1 / add x10) are position independent; its callers (the
 * transform's tail call 0x5e834, geometry_load_polygon_count 0x5f7d8, geometry_load_vertex_count 0x5f828,
 * process_geometry_commands 0x5fe94 and 0x60338, update_frame_geometry 0x62a04) touch callee-saved registers only
 * after it, and its clobber set is "everything" (it calls the perspective asm), so a C wrapper is a valid stand-in;
 * with no check window open (flush_hold 0: a flush asked for outside a transform) geom_flush_check passes the call
 * straight to the original. Sequence: save; ours with flush_hold set (its flush only recorded in flush_wanted);
 * copy out; restore; the original with the hold still set (its tail call reaches geom_flush_check through the hooked
 * entry and is recorded the same way); compare the regions and the two decisions; then a flush the original asked for
 * runs once, on its bytes. Ours asking alone (or the original alone) counts as a difference and is printed. */
#define XC_LO 0x1800
#define XC_HI_AT 0x9700
#define XC_HI 0x400
static uint8_t xc_in[XC_LO + XC_HI], xc_ours[XC_LO + XC_HI];
void geom_tvx_check(uint8_t *g) {
    memcpy(xc_in, g, XC_LO); memcpy(xc_in + XC_LO, g + XC_HI_AT, XC_HI);
    flush_hold = 1; flush_wanted = 0;
    if (!tvx_fast(g)) { flush_hold = 0; geom_tvx_orig(g); xc_n[1]++; qc_report(); return; }
    int ours_flush = flush_wanted;
    memcpy(xc_ours, g, XC_LO); memcpy(xc_ours + XC_LO, g + XC_HI_AT, XC_HI);
    memcpy(g, xc_in, XC_LO); memcpy(g + XC_HI_AT, xc_in + XC_LO, XC_HI);
    flush_wanted = 0;
    geom_tvx_orig(g);
    flush_hold = 0;
    xc_n[0]++;
    int bad = memcmp(xc_ours, g, XC_LO) || memcmp(xc_ours + XC_LO, g + XC_HI_AT, XC_HI) || ours_flush != flush_wanted;
    if (bad && xc_bad++ < 30) {
        fprintf(stderr, "[geomcheck] geometry_transform_vertexes(%p) n %u from %u normals %ld colours %ld events %ld flush %d/%d differs from DraStic's:",
                (void *)g, (uint32_t)ld32(xc_in, G_BATCH_N), (uint32_t)ld32(xc_in, G_OUT_BASE),
                (long)(((uint8_t *)ldp_(xc_in, G_NORMAL_CUR) - (g + G_NORMALS)) / 4), (long)(((uint8_t *)ldp_(xc_in, G_COLOUR_CUR) - (g + G_COLOURS)) / 2),
                (long)((uint8_t *)ldp_(xc_in, G_EVENT_CUR) - (g + G_EVENTS)), flush_wanted, ours_flush);
        for (unsigned i = 0, k = 0; i < XC_LO + XC_HI && k < 8; i++) {
            uint8_t r = i < XC_LO ? g[i] : g[XC_HI_AT + i - XC_LO];
            if (r != xc_ours[i]) { fprintf(stderr, " [+%#x] %02x/%02x", i < XC_LO ? i : XC_HI_AT + i - XC_LO, r, xc_ours[i]); k++; }
        }
        fprintf(stderr, " (DraStic/ours)\n");
    }
    if (flush_wanted) geom_flush_orig(g);
    qc_report();
}

static const uint32_t expect_queue[4] = { 0xa9bd7bfd, 0x910003fd, 0xf90013f5, 0x91402415 };
static const uint32_t expect_vertex[4] = { 0xa9bb7bfd, 0x910003fd, 0xa90363f7, 0x91402417 };
static const uint32_t expect_packed[4] = { 0xa9be7bfd, 0xaa0003e2, 0x910003fd, 0xa90153f3 };
static const uint32_t expect_tvb[4] = { 0x91402401, 0xb9433003, 0x911f9021, 0xb9430002 };
static const uint32_t expect_ysort[4] = { 0xa9af7bfd, 0x910003fd, 0xa90153f3, 0xaa0103f4 };
static const uint32_t expect_tvx[4] = { 0xa9aa7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3 };
static const uint32_t expect_flush[4] = { 0xd287720c, 0xcb2c63ff, 0xd28f3f01, 0x8b01000a };

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
    geom_packed_orig = (geom_packed_fn *)rast_hook(DS_QUEUE_PACKED, expect_packed, check ? (void *)geom_queue_packed_check : (void *)geom_queue_packed);
    geom_vertex_orig = (geom_vertex_fn *)rast_hook(DS_GEOMETRY_ADD_VERTEX, expect_vertex,
                                                   check ? (void *)geom_add_vertex_check : (void *)geom_add_vertex);
    geom_ysort_orig = (geom_ysort_fn *)rast_hook(DS_YSORT_4X, expect_ysort, check ? (void *)geom_ysort_check : (void *)geom_ysort);
    geom_tvb_orig = (geom_tvb_fn *)rast_hook(DS_TRANSFORM_VERTEX_BLOCK, expect_tvb, check ? (void *)geom_tvb_check : (void *)geom_tvb);
    /* the transform needs the vertex block (ours calls geom_tvb) and, in check mode, the flush held */
    geom_matmul = (geom_matmul_fn *)(ds_base + DS_MATRIX_MUL_4X4_ASM);
    geom_flush_orig = (geom_flush_fn *)(ds_base + DS_FLUSH_POLYGONS);
    if (check) geom_flush_orig = (geom_flush_fn *)rast_hook(DS_FLUSH_POLYGONS, expect_flush, (void *)geom_flush_check);
    if (geom_tvb_orig && geom_flush_orig)
        geom_tvx_orig = (geom_tvx_fn *)rast_hook(DS_TRANSFORM_VERTEXES, expect_tvx, check ? (void *)geom_tvx_check : (void *)geom_transform_vertexes);
    fprintf(stderr, "[geom] queue_geometry_command%s%s%s%s%s replaced%s\n", geom_packed_orig ? ", queue_geometry_command_packed" : "",
            geom_vertex_orig ? ", geometry_add_vertex" : "", geom_ysort_orig ? ", video_3d_bin_polygons_y_sort_4x" : "",
            geom_tvb_orig ? ", geometry_transform_vertex_block_asm" : "", geom_tvx_orig ? ", geometry_transform_vertexes" : "",
            check ? ", checking every call against DraStic's" : "");
}
