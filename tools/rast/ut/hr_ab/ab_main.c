/* A/B of hr.c's 3x stage functions (run.sh): the base version (old_) against the worktree's (new_) on random inputs */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void old_ds(const uint32_t *in, uint8_t *out, uint32_t clear);
void new_ds(const uint32_t *in, uint8_t *out, uint32_t clear);
void old_fog(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full);
void new_fog(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full);
void old_rds(const uint8_t *ctx, uint8_t *sys, uint8_t *geom, uint8_t *out);
void new_rds(const uint8_t *ctx, uint8_t *sys, uint8_t *geom, uint8_t *out);
unsigned old_ctx_size(void);

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }
#define W 768
static uint32_t in[48 * W], att[1][W];
static uint8_t o1[32 * 0x800], o2[32 * 0x800];

/* a pixel: colour channels 6-bit (or 8-bit when wide), alpha byte: 5-bit alpha by class, flag bits 5-7 random */
static uint32_t pix(int cls, int wide) {
    uint32_t m = wide ? 0xff : 0x3f, a;
    switch (cls) { case 0: a = 31; break; case 1: a = 0; break; default: a = rnd() % 32; }
    a |= (rnd() & 7) << 5;
    return (rnd() & m) | (rnd() & m) << 8 | (rnd() & m) << 16 | a << 24;
}

static int test_ds(int iters) {
    int bad = 0;
    for (int it = 0; it < iters; it++) {
        int mode = it % 6, wide = (it % 7) == 6;
        /* regions: per group of 3 pixels a class, so groups of 4 triples hit all-opaque, all-clear and mixed */
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < W; x++) {
                int cls;
                switch (mode) {
                case 0: cls = rnd() % 3; break;                                    /* per pixel */
                case 1: cls = ((x / 12 + y / 3 + it) % 5) == 0 ? 2 : ((x / 12 + y / 3) % 2); break;   /* per group */
                case 2: cls = (rnd() % 64) ? 0 : 2; break;                         /* mostly opaque */
                case 3: cls = (rnd() % 64) ? 1 : 2; break;                         /* mostly clear */
                case 4: cls = 0; break;
                default: cls = (x / 12 + y / 3) % 3; break;
                }
                in[y * W + x] = pix(cls, wide);
            }
        uint32_t clear = rnd();
        memset(o1, 0x5a, sizeof o1); memset(o2, 0x5a, sizeof o2);
        old_ds(in, o1, clear); new_ds(in, o2, clear);
        if (memcmp(o1, o2, sizeof o1)) {
            for (size_t i = 0; i < sizeof o1; i += 4)
                if (memcmp(o1 + i, o2 + i, 4)) { if (bad < 5) printf("ds it %d mode %d: byte %zu old %08x new %08x\n", it, mode, i, *(uint32_t *)(o1 + i), *(uint32_t *)(o2 + i)); break; }
            bad++;
        }
    }
    printf("downsample: %d cases, %d differ\n", iters, bad);
    return bad;
}

static int test_fog(int iters) {
    int bad = 0;
    static uint32_t c1[W], c2[W];
    uint8_t table[64];
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < 64; i++) table[i] = rnd();
        if (it & 1) for (int i = 0; i < 32; i++) table[i] = rnd() % 128;
        uint32_t sh = rnd() % 16, off = rnd() & 0x7fff;
        if (it % 4 == 1) off = rnd() % 0x400;
        uint32_t params = sh | (off + (0x400u >> sh)) << 16;
        if (it % 8 == 7) params = (rnd() & 0xffff0000u) | (rnd() & 0xff);   /* any shift byte */
        /* near: table[0] = 0 (as fog density tables have) and steps of 32 pixels whose depths are all at most the
         * offset (fog_line's skip of steps whose factors are all 0), some with one pixel at offset + 1 */
        int mode = it % 4, near = it % 5 >= 3;
        uint32_t uoff = (params >> 16) & 0xffff;
        if (near) table[0] = 0;
        for (int x = 0; x < W; x++) {
            uint32_t c = pix(rnd() % 3, it % 5 == 4);
            if (mode == 1) c &= 0x7fffffff;                                  /* no fog flags */
            if (mode == 2 && (x / 16) % 3) c &= 0x7fffffff;                  /* groups without */
            if (mode == 3) c |= 0x80000000;
            c1[x] = c2[x] = c;
            att[0][x] = rnd();
            if (it % 3 == 0) att[0][x] = (att[0][x] & 0xff0001ff) | ((rnd() % 0x800) << 9) | ((rnd() & 1) ? 0x7f0000 << 1 : 0);
            if (near && (x / 32) % 4 != 3) {
                uint32_t dmax = uoff < 0x7fff ? uoff : 0x7fff, dd = rnd() % 4 ? rnd() % (dmax + 1) : dmax;
                if ((x / 32) % 4 == 2 && x % 32 == (it % 32) && dmax < 0x7fff) dd = dmax + 1;
                att[0][x] = (att[0][x] & 0xff0001ff) | dd << 9;
            }
        }
        uint32_t fogc = rnd();
        int full = (it >> 1) & 1;
        old_fog(c1, att[0], table, params, fogc, full); new_fog(c2, att[0], table, params, fogc, full);
        if (memcmp(c1, c2, sizeof c1)) {
            for (int x = 0; x < W; x++)
                if (c1[x] != c2[x]) { if (bad < 5) printf("fog it %d: x %d old %08x new %08x\n", it, x, c1[x], c2[x]); break; }
            bad++;
        }
    }
    printf("fog: %d cases, %d differ\n", iters, bad);
    return bad;
}

static uint32_t attr_word(int mode) {
    uint32_t w = rnd();
    switch (mode) {
    case 1: w = (w & 0xc0000000u) | (rnd() % 3) << 24 | (rnd() % 3) * 0x010101u; break;       /* few ids, keys */
    case 2: w = (w & 0x40000000u) | 0x40000000u | (rnd() % 4) << 24 | (rnd() % 2) << 16 | (rnd() % 2) << 8 | (rnd() % 2); break;
    case 3: w &= ~0x40000000u; break;                                                         /* no bit 30 */
    case 4: w = 0x45123456; break;                                                            /* uniform */
    default: break;
    }
    return w;
}

/* the bin resolve (fog, edge marking) and the downsample together, on random contexts: the output blocks compared */
static int test_rds(int iters) {
    int bad = 0;
    unsigned csz = old_ctx_size();
    uint8_t *ctx = malloc(csz), *sys = calloc(1, 0x350000), *geom = calloc(1, 0x10000);
    for (int it = 0; it < iters; it++) {
        int mode = it % 6, cmode = (it / 6) % 4;
        uint32_t *colp = (uint32_t *)ctx, *attp = (uint32_t *)(ctx + 50 * W * 4);
        for (unsigned i = 0; i < csz; i++) ctx[i] = rnd();
        for (int l = 0; l < 50; l++)
            for (int x = 0; x < W; x++) {
                int m = mode == 5 ? ((x / 16 + l / 2 + it) % 5) : mode;
                if (mode == 0 && (x / 48 + l / 3) % 4 == 0) m = 4;          /* uniform areas among random ones */
                attp[l * W + x] = attr_word(m);
                int cls = cmode == 0 ? (int)(rnd() % 3) : cmode == 1 ? 0 : cmode == 2 ? (int)((x / 12 + l / 3) % 3) : (rnd() % 16 ? 0 : 2);
                colp[l * W + x] = pix(cls, 0);
                if (it % 3 == 0 && (x / 16) % 4 == 0) colp[l * W + x] |= 0x80000000u;   /* fog flags */
            }
        uint32_t d3 = rnd() & ~0xf0u;
        d3 |= (it % 2) << 5;                                                /* edge marking */
        d3 |= ((it / 2) % 3 == 0 ? 0 : (it / 2) % 3 == 1 ? 0x80 : 0xc0);   /* fog: off, full, alpha only */
        *(uint32_t *)(sys + 0x34eb40) = d3;
        *(uint32_t *)(sys + 0x34eb48) = rnd();
        *(uint32_t *)(sys + 0x34eb4c) = it % 4 == 3 ? attp[W] : attr_word(mode == 5 ? 0 : mode);
        *(uint32_t *)(sys + 0x34eb50) = rnd() % 4;
        *(uint32_t *)(ctx + 2 * 50 * W * 4 + 50 * W + 0x14) = rnd() % 4;   /* the bin's fog used flag */
        for (int i = 0x9900; i < 0x9b00; i++) geom[i] = rnd();
        for (int i = 0x99b4; i < 0x99cc; i++) geom[i] &= 0x3f;             /* edge colours: 6-bit */
        if (it & 4) for (int i = 0; i < 32; i++) geom[0x9974 + i] %= 128;
        if (it % 5 >= 3) {                  /* table[0] = 0 and a high offset: fog_line's skip of steps without factors */
            geom[0x9974] = 0;
            *(uint16_t *)(geom + 0x9aaa) = it % 5 == 3 ? 0x7fff : 0x4000 + rnd() % 0x4000;
        }
        memset(o1, 0x5a, sizeof o1); memset(o2, 0x5a, sizeof o2);
        old_rds(ctx, sys, geom, o1); new_rds(ctx, sys, geom, o2);
        if (memcmp(o1, o2, sizeof o1)) {
            for (size_t i = 0; i < sizeof o1; i += 4)
                if (memcmp(o1 + i, o2 + i, 4)) { if (bad < 5) printf("rds it %d mode %d d3 %08x: byte %zu old %08x new %08x\n", it, mode, d3, i, *(uint32_t *)(o1 + i), *(uint32_t *)(o2 + i)); break; }
            bad++;
        }
    }
    printf("resolve + downsample: %d cases, %d differ\n", iters, bad);
    return bad;
}

/* the same on polygon-like contexts: rectangles of one polygon id each over a background (keys a gradient and noise),
 * so most blocks of 16 pixels have one id in all their neighbour pairs and edge_lines' id screen passes them, and its
 * transitions between screened and full blocks (the left tests carried across, the forced block after a full one) and
 * the rectangles' edges at and next to block boundaries are exercised; the random contexts above give few such blocks */
static int test_rds_rect(int iters) {
    int bad = 0;
    unsigned csz = old_ctx_size();
    uint8_t *ctx = malloc(csz), *sys = calloc(1, 0x350000), *geom = calloc(1, 0x10000);
    for (int it = 0; it < iters; it++) {
        uint32_t *colp = (uint32_t *)ctx, *attp = (uint32_t *)(ctx + 50 * W * 4);
        for (unsigned i = 0; i < csz; i++) ctx[i] = rnd();
        uint32_t bg = rnd();
        for (int i = 0; i < 50 * W; i++) attp[i] = bg + (it & 1 ? (rnd() & 3) : 0);
        for (int r = 0, nr = 1 + rnd() % 12; r < nr; r++) {
            int x0 = rnd() % W, y0 = rnd() % 50, w = 1 + rnd() % (it & 2 ? 40 : 400), h = 1 + rnd() % 50;
            if (rnd() % 4 == 0) x0 = (x0 & ~15) + (int)(rnd() % 3) - 1;                 /* at block boundaries */
            if (x0 < 0) x0 = 0;
            uint32_t top = (rnd() & 0xc0000000u) | (rnd() % 64) << 24 | (rnd() % 3 ? 0 : 0x40000000u);
            uint32_t key = rnd() & 0xffffff;
            int gx = (int)(rnd() % 7) - 3, gy = (int)(rnd() % 7) - 3;
            for (int y = y0; y < y0 + h && y < 50; y++)
                for (int x = x0; x < x0 + w && x < W; x++)
                    attp[y * W + x] = top | ((key + (uint32_t)(gx * x + gy * y) + (rnd() & 1)) & 0xffffff);
        }
        for (int i = 0; i < 50 * W; i++) colp[i] = pix(rnd() % 3, 0);
        uint32_t d3 = (rnd() & ~0xf0u) | 0x20;                                          /* edge marking */
        d3 |= ((it / 4) % 3 == 0 ? 0 : (it / 4) % 3 == 1 ? 0x80 : 0xc0);               /* fog: off, full, alpha only */
        *(uint32_t *)(sys + 0x34eb40) = d3;
        *(uint32_t *)(sys + 0x34eb48) = rnd();
        *(uint32_t *)(sys + 0x34eb4c) = it % 4 == 3 ? attp[W] : rnd() % 2 ? bg : rnd();
        *(uint32_t *)(sys + 0x34eb50) = rnd() % 4;
        *(uint32_t *)(ctx + 2 * 50 * W * 4 + 50 * W + 0x14) = rnd() % 4;
        for (int i = 0x9900; i < 0x9b00; i++) geom[i] = rnd();
        for (int i = 0x99b4; i < 0x99cc; i++) geom[i] = (geom[i] & 0x3f) | 1;          /* edge colours: 6-bit, not 0 */
        if (it % 5 >= 3) {                  /* table[0] = 0, the offset at or just above the background's depth */
            geom[0x9974] = 0;
            *(uint16_t *)(geom + 0x9aaa) = (uint16_t)(((bg >> 9) & 0x7fff) + rnd() % 8);
        }
        memset(o1, 0x5a, sizeof o1); memset(o2, 0x5a, sizeof o2);
        old_rds(ctx, sys, geom, o1); new_rds(ctx, sys, geom, o2);
        if (memcmp(o1, o2, sizeof o1)) {
            for (size_t i = 0; i < sizeof o1; i += 4)
                if (memcmp(o1 + i, o2 + i, 4)) { if (bad < 5) printf("rds rect it %d d3 %08x: byte %zu old %08x new %08x\n", it, d3, i, *(uint32_t *)(o1 + i), *(uint32_t *)(o2 + i)); break; }
            bad++;
        }
    }
    printf("resolve + downsample, polygon-like contexts: %d cases, %d differ\n", iters, bad);
    return bad;
}

/* 1 when one of the polygon's two chains, walked as hr_polygon and edges_impl.h's interpolate_edges walk them, covers
 * more lines than its window y_end - y_start: a chain that goes down, up and down again (these random polygons are
 * mostly self-intersecting) covers lines twice. hr.c's walker stops such a chain at the window (EDGES_LINE_CAP); before
 * it, the chain's lines ran on in the span arrays, past 64 the right chain's overwrote the left chain's first entries
 * and past 128 the perspective steps ran out of the span block (what made two copies of the same hr.c differ with the
 * previous polygons' spans in their heap blocks). Against a base without the cap (AB_UNCAPPED=1, set by run.sh) these
 * polygons are counted apart; every other polygon must hash the same. */
static int chain_overruns(unsigned count, const uint32_t *orders, unsigned oi, unsigned base, uint16_t (*hv)[2], uint16_t (*hv2)[2],
                          const uint8_t *verts, unsigned bin_top, unsigned bin_bot, int edges) {
    uint32_t seq = orders[oi & ~7u];                /* (t >= 8 of 9 and 10 vertices: the next group's entry, as hr_polygon) */
    unsigned t = oi & 7, idx[10], ys[10], y[12], ymin = 0xffff, top = t, ybot = 0;
    for (unsigned k = 0; k < count; k++) {
        idx[k] = base + ((seq >> (4 * (k & 7))) & 15);
        ys[k] = hv[idx[k]][1];
        if (ys[k] < ymin) ymin = ys[k];
    }
    if (ys[t] != ymin) for (unsigned k = 0; k < count; k++) if (ys[k] == ymin) { top = k; break; }
    for (unsigned k = 0, j = top % count; k < count; k++, j = j + 1 < count ? j + 1 : 0) {
        unsigned vi = idx[j], y3 = hv[vi][1], rx = *(const uint16_t *)(verts + 16 * vi + 4), ry = *(const uint16_t *)(verts + 16 * vi + 6);
        if (rx != hv2[vi][0] || ry != hv2[vi][1]) y3 = (3 * ry + 1) / 2;       /* hr_polygon's fallback */
        y[k] = y3;
        if (y3 > ybot) ybot = y3;
    }
    y[count] = y[0]; y[count + 1] = y[1];
    int top_clip = y[0] < bin_top, bot_clip = ybot > bin_bot;
    unsigned y_end = ybot < bin_bot ? ybot : bin_bot;
    if ((int)y_end - (int)(y[0] > bin_top ? y[0] : bin_top) <= 0) return 0;
    unsigned ys0 = edges ? bin_top - top_clip : bin_top, ye = edges ? y_end + bot_clip : y_end;
    for (int dir = 1; dir >= -1; dir -= 2) {
        int pos = dir > 0 ? 0 : (int)count;
        unsigned yp = y[pos], n = 0;
        int total = 0;
        if (ye > yp) do {
            pos += dir;
            unsigned y1 = y[pos];
            int len = (int)(y1 - yp);
            if (ys0 > yp) len += (int)(yp - ys0);
            if (y1 > ye) len += (int)(ye - y1);
            if (len > 0) { if (n == 16) break; total += len; n++; }
            yp = y1;
        } while (ye > yp);
        if (total > (int)ye - (int)ys0) return 1;
    }
    return 0;
}

/* the polygon walker (hr_polygon) on random polygons around random bins: what it hands the kernels compared */
void old_poly(uint8_t *poly, uint8_t *verts, const void *hv, const void *hv2, unsigned bin_top, unsigned bin_bot, int lb, uint32_t d3, int defer);
void new_poly(uint8_t *poly, uint8_t *verts, const void *hv, const void *hv2, unsigned bin_top, unsigned bin_bot, int lb, uint32_t d3, int defer);
extern uint64_t ab_hash;
extern unsigned ab_calls;
extern uintptr_t ds_base;
static int test_poly(int iters) {
    enum { NV = 6144 };
    static uint16_t hv[NV][2], hv2[NV][2];
    static uint8_t verts[NV * 16], poly[32];
    uint32_t *orders = calloc(1, 0x11df90 + 512);
    ds_base = (uintptr_t)orders;                        /* the orders table at ds_base + DS_VERTEX_ORDERS */
    orders = (uint32_t *)((uint8_t *)orders + 0x11df90);
    int bad = 0, calls = 0, over = 0, overbad = 0, uncapped = getenv("AB_UNCAPPED") && atoi(getenv("AB_UNCAPPED"));
    for (int it = 0; it < iters; it++) {
        unsigned bin = rnd() % 12, hy0 = bin * 48, bin_top = hy0 ? hy0 - 1 : 0, bin_bot = hy0 + 49 > 576 ? 576 : hy0 + 49;
        unsigned count = it % 13 == 0 ? 1 + rnd() % 2 : 3 + rnd() % 8, base = rnd() % (NV - 16), t = rnd() % count;
        /* the group's base order: a random permutation of 0..count-1 */
        unsigned perm[10];
        for (unsigned k = 0; k < count; k++) perm[k] = k;
        for (unsigned k = count; k > 1; k--) { unsigned j = rnd() % k, x = perm[k - 1]; perm[k - 1] = perm[j]; perm[j] = x; }
        uint32_t seq = 0;
        for (unsigned k = 0; k < count; k++) seq |= perm[k] << (4 * k);
        orders[count * 8] = seq;
        int span = it % 4 == 0 ? 600 : it % 4 == 1 ? 120 : 30;             /* tall, medium and small polygons */
        int yc = (int)hy0 + (int)(rnd() % 80) - 16, xc = (int)(rnd() % 768);
        for (unsigned k = 0; k < 16; k++) {
            unsigned vi = base + k;
            int y = yc + (int)(rnd() % (unsigned)span) - span / 2, x = xc + (int)(rnd() % (unsigned)span) - span / 2;
            if (it % 5 == 0 && k < 4) y = yc;                                     /* tied top vertices */
            y = y < 0 ? 0 : y > 575 ? 575 : y; x = x < 0 ? 0 : x > 767 ? 767 : x;
            hv[vi][0] = (uint16_t)x; hv[vi][1] = (uint16_t)y;
            hv2[vi][0] = (uint16_t)(2 * x / 3); hv2[vi][1] = (uint16_t)(2 * y / 3);
            for (int i = 0; i < 16; i++) verts[16 * vi + i] = rnd();
            *(uint16_t *)(verts + 16 * vi + 4) = hv2[vi][0]; *(uint16_t *)(verts + 16 * vi + 6) = hv2[vi][1];
            if (it % 7 == 0 && k == 1) *(uint16_t *)(verts + 16 * vi + 6) ^= 1;   /* a changed record: the fallback */
        }
        for (int i = 0; i < 32; i++) poly[i] = rnd();
        *(uint32_t *)(poly + 8) = count | (rnd() & 0xff) << 8 | (count * 8 + t) << 16;
        *(uint16_t *)(poly + 0x1a) = (uint16_t)base;
        uint32_t d3 = rnd() & ~0x20u;
        if (it & 1) d3 |= 0x20;                                                 /* edge marking */
        int defer = (it >> 1) & 1;
        uint64_t h0 = ab_hash = 14695981039346656037ull; unsigned c0 = ab_calls = 0;
        old_poly(poly, verts, hv, hv2, bin_top, bin_bot, (int)hy0 - 1, d3, defer);
        uint64_t h1 = ab_hash; unsigned c1 = ab_calls;
        ab_hash = h0; ab_calls = c0;
        new_poly(poly, verts, hv, hv2, bin_top, bin_bot, (int)hy0 - 1, d3, defer);
        calls += c1;
        int ov = chain_overruns(count, orders, (count * 8 + t) & 0x7f, base, hv, hv2, verts, bin_top, bin_bot, (d3 >> 5) & 1);
        over += ov;
        if (h1 != ab_hash || c1 != ab_calls) {
            if (ov && uncapped) overbad++;
            else { if (bad < 5) printf("poly it %d: count %u t %u bin %u calls %u/%u\n", it, count, t, bin, c1, ab_calls); bad++; }
        }
    }
    printf("polygon walker: %d polygons (%d reaching the kernels), %d differ", iters, calls, bad);
    if (uncapped) printf(" (and %d of the %d with a chain longer than its window: the base walks them uncapped)", overbad, over);
    printf("\n");
    return bad;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200;
    if (argc > 2) rs ^= strtoull(argv[2], 0, 0) * 0x9e3779b97f4a7c15ull;
    int bad = test_ds(n) + test_fog(n * 10) + test_rds(n) + test_poly(n * 50) + test_rds_rect(n);
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
