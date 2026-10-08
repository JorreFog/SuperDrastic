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
        int mode = it % 4;
        for (int x = 0; x < W; x++) {
            uint32_t c = pix(rnd() % 3, it % 5 == 4);
            if (mode == 1) c &= 0x7fffffff;                                  /* no fog flags */
            if (mode == 2 && (x / 16) % 3) c &= 0x7fffffff;                  /* groups without */
            if (mode == 3) c |= 0x80000000;
            c1[x] = c2[x] = c;
            att[0][x] = rnd();
            if (it % 3 == 0) att[0][x] = (att[0][x] & 0xff0001ff) | ((rnd() % 0x800) << 9) | ((rnd() & 1) ? 0x7f0000 << 1 : 0);
        }
        uint32_t sh = rnd() % 16, off = rnd() & 0x7fff;
        if (it % 4 == 1) off = rnd() % 0x400;
        uint32_t params = sh | (off + (0x400u >> sh)) << 16;
        if (it % 8 == 7) params = (rnd() & 0xffff0000u) | (rnd() & 0xff);   /* any shift byte */
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

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200;
    if (argc > 2) rs ^= strtoull(argv[2], 0, 0) * 0x9e3779b97f4a7c15ull;
    int bad = test_ds(n) + test_fog(n * 10) + test_rds(n);
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
