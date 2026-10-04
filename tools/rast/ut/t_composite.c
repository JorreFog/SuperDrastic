/* t_composite.c: the 3D visibility step of the scanline compositor (src/rast/spec/composite.c) vs DraStic's originals,
 * and the NEON version (src/rast/compvis.h) vs the C port.
 * run.sh t_composite.c ../../../src/rast/spec/composite.c */
#include "ut.h"
#include "spec/composite.h"
#include "compvis.h"

typedef void (*gather_fn)(uint8_t *, const uint32_t *);
typedef uint32_t (*vis_fn)(uint8_t *, const uint32_t *);
#define DS_GATHER 0xa0a48
#define DS_VIS    0x3c2c0

static uint32_t pxbuf[256 + 16] __attribute__((aligned(64)));
static unsigned ncalls[3], nret[3];          /* calls per return value 0 / 0x10 / 2 (coverage of the three outcomes) */

/* alpha byte for the "near 31" mode: values that differ from 0 / 0x1f in one bit or in bits 5-7 */
static uint8_t near31(void) {
    static const uint8_t v[] = { 0x1f, 0x1f, 0, 0, 0x1e, 0x1d, 0x1b, 0x17, 0x0f, 0x3f, 0x5f, 0x9f, 0xff, 0x20, 0x80, 0x01, 0xe0 };
    return v[rnd(sizeof v)];
}

/* 256 pixels: bytes 0..2 random (r6 g6 b6, or any byte), byte 3 by mode */
static void fill(uint32_t *px, int mode, unsigned pos, uint8_t val) {
    int junk = rnd(2);
    unsigned dens = rnd(257);
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = junk ? (uint32_t)rnd64() & 0xffffff : rnd(64) | rnd(64) << 8 | rnd(64) << 16;
        uint8_t a;
        switch (mode) {
        case 0: a = rnd(256); break;                                    /* any byte */
        case 1: a = 0; break;                                           /* nothing visible */
        case 2: a = 1 + rnd(255); break;                                /* all visible, any byte */
        case 3: a = 0x1f; break;                                        /* all opaque */
        case 4: a = rnd(256) < dens ? 0x1f : 0; break;                  /* opaque / transparent mix */
        case 5: a = rnd(256) < dens ? (rnd(40) ? 0x1f : 1 + rnd(30)) : 0; break;  /* a few translucent */
        case 6: a = rnd(32); break;                                     /* 5-bit alpha */
        case 7: a = i == (int)pos ? val : 0; break;                     /* one visible pixel */
        case 8: a = i == (int)pos ? val : 0x1f; break;                  /* one pixel differs from opaque */
        case 9: a = near31(); break;
        default: a = 0; break;
        }
        px[i] = rgb | (uint32_t)a << 24;
    }
    if (mode == 10) {                                                   /* spans, as polygons cover a line */
        int x = 0;
        while (x < 256) {
            int n = 1 + rnd(rnd(2) ? 8 : 80);
            uint8_t a = rnd(3) ? 0x1f : rnd(3) ? 0 : (uint8_t)(1 + rnd(30));
            for (int k = 0; k < n && x < 256; k++, x++) px[x] = (px[x] & 0xffffff) | (uint32_t)a << 24;
        }
    }
}

static void check(const uint32_t *px, const char *what) {
    uint8_t g1[256 + 64], g2[256 + 64], o1[96], o2[96], o3[96];
    /* gather: 256 bytes at a random offset in a guard-filled buffer */
    unsigned go = rnd(33);
    uint8_t pat = rnd(256);
    memset(g1, pat, sizeof g1); memset(g2, pat, sizeof g2);
    DS(gather_fn, DS_GATHER)(g1 + go, px);
    spec_render_scanline_gather_3d_alpha(g2 + go, px);
    if (ut_cmp(what, g1, g2, sizeof g1)) { fprintf(stderr, "   (gather, offset %u)\n", go); return; }
    /* set_3d_visibility: 32 bytes at a random offset, guard bytes around */
    unsigned oo = 16 + rnd(32);
    pat = rnd(256);
    memset(o1, pat, sizeof o1); memset(o2, pat, sizeof o2); memset(o3, pat, sizeof o3);
    uint32_t r1 = DS(vis_fn, DS_VIS)(o1 + oo, px);
    uint32_t r2 = spec_render_scanline_set_3d_visibility(o2 + oo, px);
    uint32_t r3 = comp_vis_neon(o3 + oo, px);
    if (ut_cmp(what, o1, o2, sizeof o1)) { fprintf(stderr, "   (set_3d_visibility vs spec, offset %u)\n", oo); return; }
    if (r1 != r2) { fprintf(stderr, "FAIL %s: set_3d_visibility returns %#x, spec %#x\n", what, r1, r2); ut_fail++; return; }
    if (ut_cmp(what, o2, o3, sizeof o2)) { fprintf(stderr, "   (NEON vs spec, offset %u)\n", oo); return; }
    if (r3 != r2) { fprintf(stderr, "FAIL %s: NEON returns %#x, spec %#x\n", what, r3, r2); ut_fail++; return; }
    int k = r1 == 0 ? 0 : r1 == 0x10 ? 1 : r1 == 2 ? 2 : -1;
    if (k < 0) { fprintf(stderr, "FAIL %s: unexpected return %#x\n", what, r1); ut_fail++; return; }
    nret[k]++;
}

void ut_main(void) {
    char what[64];
    for (int mode = 0; mode <= 10; mode++) {
        int n = mode == 7 || mode == 8 ? 0 : 2000;
        for (int t = 0; t < n && ut_fail < 10; t++) {
            uint32_t *px = pxbuf + rnd(4) * (rnd(2) ? 1 : 4);           /* word and 16-byte offsets */
            fill(px, mode, 0, 0);
            snprintf(what, sizeof what, "mode %d test %d", mode, t);
            check(px, what);
            ncalls[0]++;
        }
    }
    /* one non-zero (mode 7) / one non-opaque (mode 8) pixel at every position, with 31, 1..30 and 32..255 */
    for (int mode = 7; mode <= 8; mode++)
        for (unsigned pos = 0; pos < 256 && ut_fail < 10; pos++)
            for (int v = 0; v < 3; v++) {
                uint8_t val = v == 0 ? (mode == 7 ? 0x1f : 0) : v == 1 ? (uint8_t)(1 + rnd(30)) : (uint8_t)(32 + rnd(224));
                uint32_t *px = pxbuf + rnd(4);
                fill(px, mode, pos, val);
                snprintf(what, sizeof what, "mode %d pos %u value %#x", mode, pos, val);
                check(px, what);
                ncalls[0]++;
            }
    fprintf(stderr, "t_composite: %u inputs; returns 0: %u, 0x10: %u, 2: %u\n", ncalls[0], nret[0], nret[1], nret[2]);
}
