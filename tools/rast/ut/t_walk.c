/* t_walk.c: src/rast/walk.c's walk_polygon_4x against DraStic's render_polygon_4x. Random ordinary polygons (1 to 9
 * vertices in one of DraStic's walk orders, y rising from the top vertex to the bottom one and falling back, as the
 * geometry stage makes them; vertical and horizontal edges, x beyond 511, zero heights) are walked into random bins
 * with edge marking off and on. DraStic's render_polygon_setup_4x is patched to a capture and walk.c gets the same
 * capture as its setup: per polygon, the arguments and every span array line the setup reads must match (the z
 * arrays only for polygons with z depth: the x-only walk leaves them as they were; the edge markers only with edge
 * marking).
 * run: tools/rast/ut/run.sh tools/rast/ut/t_walk.c src/rast/walk.c   (UT_SEED=n) */
#include "ut.h"
#include <sys/mman.h>
#include "ds3d.h"
#include "rast.h"
#include "spec/edges.h"

#define O_SETUP   0x4bc80
#define O_POLY    0x53ef0
#define O_RECIP   0x3f27120
#define U16X(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))

uintptr_t ds_base;                          /* (rast.c's, for walk.c) */

typedef struct { int called; unsigned line0, nlines, flags; uint8_t *poly, *v0; uint8_t arr[11][SPAN_ARR]; } cap_t;
static cap_t cap;
static void capture(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                    unsigned flags, uint8_t *v0) {
    cap.called++; cap.line0 = line0; cap.nlines = nlines; cap.flags = flags; cap.poly = poly; cap.v0 = v0;
    for (int k = 0; k < 11; k++) memcpy(cap.arr[k], spans + k * SPAN_ARR, SPAN_ARR);
    (void)ctx; (void)buf;
}
static void patch(uintptr_t at, void *to) {
    uint32_t *p = (uint32_t *)at;
    mprotect((void *)(at & ~4095ul), 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    p[0] = 0x58000050; p[1] = 0xd61f0200; memcpy(p + 2, &to, 8);          /* ldr x16, #8; br x16 */
    __builtin___clear_cache((char *)p, (char *)(p + 4));
}

static uint8_t verts[512 * 16], polyrec[32];
static int calls[4];

void ut_main(void) {
    ds_base = ut_base;
    uint32_t *rt = DS(uint32_t *, O_RECIP);
    for (uint32_t i = 1; i <= 512; i++) rt[i] = (0x3fffffffu + i) / i;
    patch(ut_base + O_SETUP, (void *)capture);
    uint8_t *ctx = calloc(1, CTX_SIZE), *sys = calloc(1, SYS_DISP3DCNT + 0x100), *geom = calloc(1, 0x10000);
    memcpy(ctx + CTX_SYS, &sys, 8); memcpy(ctx + CTX_GEOM, &geom, 8);
    const uint32_t *orders = DS(const uint32_t *, DS_VERTEX_ORDERS);
    for (int it = 0; it < 20000; it++) {
        unsigned count = 1 + rnd(9), oi = rnd(128), base = rnd(400), vi[9];
        uint32_t seq = orders[oi];
        int ok = 1;
        for (unsigned k = 0; k < count && ok; k++) {
            vi[k] = base + (k < 8 ? (seq >> (4 * k)) & 15 : 0);
            for (unsigned j = 0; j < k; j++) if (vi[j] == vi[k]) ok = 0;
        }
        if (!ok) { it--; continue; }
        /* y by walk position: the top first, rising to the bottom at position b, falling back */
        unsigned b = count > 1 ? rnd(count) : 0, ytop = rnd(5) ? rnd(384) : rnd(500), ys[9];
        unsigned span_y = rnd(4) ? rnd(64) : rnd(3) ? rnd(200) : 0;
        if (ytop + span_y > 511) span_y = 511 - ytop;
        ys[0] = ytop;
        for (unsigned k = 1; k <= b; k++) ys[k] = ys[k - 1] + (k == b ? 0 : rnd(span_y / (b ? b : 1) + 1));
        if (b) ys[b] = ytop + span_y > ys[b - 1] ? ytop + span_y : ys[b - 1];
        for (unsigned k = b + 1; k < count; k++) ys[k] = ys[k - 1] - rnd(ys[k - 1] - ytop + 1);
        unsigned ybot = 0;
        for (unsigned k = 0; k < count; k++) if (ys[k] > ybot) ybot = ys[k];
        if (!rnd(50)) ybot = ys[0] + rnd(ybot - ys[0] + 1);  /* a bottom above the lowest vertex: the walks stop there */
        if (ybot > 511) ybot = 511;
        for (unsigned k = 0; k < count; k++) {
            uint8_t *v = verts + 16 * vi[k];
            int32_t w = rnd(3) ? rndr(1, 1 << 20) : rnd(2) ? rndr(1, 4096) : (int32_t)rnd64();
            uint16_t x = rnd(8) ? rnd(512) : rnd(2) ? 511 + rnd(4) : rnd(65536), y = ys[k], z = rnd(65536), c = rnd(65536);
            uint16_t s = rnd(4) ? rndr(-4096, 4096) : rnd(65536), t = rnd(4) ? rndr(-4096, 4096) : rnd(65536);
            if (k && !rnd(6)) x = U16X(verts + 16 * vi[k - 1], 4);                       /* vertical edges */
            memcpy(v, &w, 4); memcpy(v + 4, &x, 2); memcpy(v + 6, &y, 2); memcpy(v + 8, &z, 2);
            memcpy(v + 10, &c, 2); memcpy(v + 12, &s, 2); memcpy(v + 14, &t, 2);
        }
        unsigned flags = rnd(256) & ~0x40u, mode = rnd(3);
        uint32_t attr = (uint32_t)rnd64() & ~0x30u, a8 = count | flags << 8 | oi << 16 | ybot << 23;
        attr |= mode << 4;
        rndfill(polyrec, sizeof polyrec);
        memcpy(polyrec + 4, &attr, 4); memcpy(polyrec + 8, &a8, 4);
        uint16_t b16 = base; memcpy(polyrec + 0x1a, &b16, 2);
        uint32_t d3 = rnd(2) ? 0x20 : 0; memcpy(sys + SYS_DISP3DCNT, &d3, 4);
        unsigned bin_top = 32 * rnd(12);
        if (rnd(4)) bin_top = 32 * ((ytop + rnd(ybot - ytop + 1)) / 32);                 /* a bin the polygon crosses */
        if (!rnd(8)) bin_top = ytop > 16 ? ytop - rnd(16) : 0;                          /* (not on a bin boundary) */
        unsigned bin_bot = bin_top + 32;

        memset(&cap, 0, sizeof cap);
        DS(void (*)(uint8_t *, uint8_t *, uint8_t *, unsigned long, unsigned long), O_POLY)(ctx, polyrec, verts, bin_top, bin_bot);
        cap_t ref = cap;
        memset(&cap, 0, sizeof cap);
        int handled = walk_polygon_4x(ctx, polyrec, verts, bin_top, bin_bot, capture);
        char what[160];
        snprintf(what, sizeof what, "walk it %d count %u oi %u flags %02x ytop %u ybot %u bin %u edges %u", it, count, oi, flags,
                 ytop, ybot, bin_top, d3 >> 5);
        if (!handled) { fprintf(stderr, "FAIL %s: not handled\n", what); ut_fail++; return; }
        if (ref.called != cap.called || ref.line0 != cap.line0 || ref.nlines != cap.nlines || ref.flags != cap.flags ||
            ref.poly != cap.poly || ref.v0 != cap.v0) {
            fprintf(stderr, "FAIL %s: setup calls %d/%d line0 %u/%u nlines %u/%u flags %x/%x v0 %p/%p\n", what, ref.called, cap.called,
                    ref.line0, cap.line0, ref.nlines, cap.nlines, ref.flags, cap.flags, (void *)ref.v0, (void *)cap.v0);
            ut_fail++; return;
        }
        calls[0]++;
        if (!ref.called) continue;
        calls[1 + (d3 >> 5)]++;
        for (int k = 0; k < 11; k++) {
            if ((k == 2 || k == 3) && (flags & 0x18)) continue;
            if (k == 10 && !d3) continue;
            char aw[200]; snprintf(aw, sizeof aw, "%s array %d", what, k);
            if (ut_cmp(aw, ref.arr[k], cap.arr[k], 4 * ref.nlines)) return;
        }
    }
    fprintf(stderr, "  polygons %d, drawn without edge marking %d, with %d\n", calls[0], calls[1], calls[2]);
}
