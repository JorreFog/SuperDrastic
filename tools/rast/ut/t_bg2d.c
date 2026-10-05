/* t_bg2d.c: DraStic's BG layer renderers vs src/rast/spec/2d/bg.c, in DraStic's process: random layer structs, VRAM
 * (4 MiB with 2 MiB of random guard memory each side, inside a +-2 GiB reservation: see vram below), palettes and ext
 * palettes; DraStic's function on engine A, the port on an identical engine B (two full 0x81420-byte engine structs
 * with the same random contents).
 * Compared after every call: the visibility bitmaps with 64 guard bytes around them (byte for byte); every visible
 * pixel; every u16 the port writes into the line buffer of a layer rendered once on the line (it must equal DraStic's);
 * that the port writes nothing outside buf[0..255] (its side of the line buffers byte for byte); the engine's header
 * and layer structs (the clip edges and dirty flags the renderers step). The whole engine struct after every frame and
 * group.
 * DraStic's own padding scribbles (bytes outside buf[0..255], never read: bg.md 3, 4.6) are measured and printed.
 * Groups (the analysis' generators, re2d/bg.md 13): the blank-layer test; the edge math alone (setup_edges, and
 * update_affine_variables on whole layer structs); text BGs (4bpp, 8bpp, 8bpp with ext palettes, NULL ext slots);
 * single affine / extended / bitmap / large bitmap lines with fresh edges; whole frames through render_scanline_bg
 * (render_scanline's per-line stepping, mosaic, mid-frame register writes and render list changes, the null
 * renderer, direct bitmap layers); clip mode with |PA| or |PC| > 2047 and the span on screen.
 * run.sh t_bg2d.c ../../../src/rast/spec/2d/bg.c
 * Env: UT_SEED; BG_N cases per group (default 3000; the groups run 4x-70x that); BG_ONLY = a list of group names
 * (blank edges text affine frames bigclip); BG_EV the frames group's event mask (bit 0 PA, 1 PB, 2 PD, 3 X, 4 the wrap
 * bit, 5 MOSAIC, 6 the render list length); BG_CENSUS=1 also prints DraStic's deviations from a hardware-style
 * per-pixel model of affine and bitmap BGs (the census of re2d/bg.md 4); BG_V=1 names every single-layer case before
 * its calls (to find a crashing one). */
#include "ut.h"
#include <sys/mman.h>
#include "spec/2d/bg.h"

#define DS_BG      0x36880
#define DS_UAV     0x34550
#define DS_EDGES   0x326f0
#define DS_BLANK   0xa089c
#define ENG_SIZE   0x81420
#define HDR        0x400                           /* the engine header and the four layer structs (0xc0..0x380) */

#define BP(p, o)   ((uint8_t *)(p) + (o))
#define BU8(p, o)  (*(uint8_t *)BP(p, o))
#define BU16(p, o) (*(uint16_t *)BP(p, o))
#define BS16(p, o) (*(int16_t *)BP(p, o))
#define BU32(p, o) (*(uint32_t *)BP(p, o))
#define BS32(p, o) (*(int32_t *)BP(p, o))
#define BPTR(p, o) (*(uint8_t **)BP(p, o))

typedef void (*rsbg_fn)(uint8_t *eng, uint8_t *lines, uint8_t *vis, uint32_t line);
typedef void (*uav_fn)(uint8_t *L);
typedef void (*edges_fn)(int32_t, int32_t, int32_t, int32_t, int64_t *, int64_t *, int64_t *);
typedef void (*blank_fn)(const uint8_t *, uint32_t *);

/* VRAM: 4 MiB of random data with 2 MiB of random guard memory each side, inside a reservation of +-2 GiB around the
 * pointer (zero pages, not committed): the bitmap renderers add a sign-extended 32-bit offset to the alias (bg.md 2.2),
 * and the clip edges of a degenerate axis (PA or PC = 0, a small PB / PD, a huge reference) overflow in their 64-bit
 * arithmetic (in DraStic as in the port) and can open a span whose rows lie anywhere in that range. */
static uint8_t *vmem, *vram, *pal, *xpal;       /* palette 0x400; ext palette 0x2000 */
#define VPRE  (2u << 20)
#define VSIZE (4u << 20)

static int nmax = 3000, census, verbose;
static uint8_t *engA, *engB;                    /* A: DraStic, B: the port */

/* ---- random layer states (re2d/bg.md 13: the analysis' t_bg.c) ---- */
/* a cheap cos in 8.8 for rotation-like matrices */
static int sintab_cos(int a) { int x = (a & 255) - 128; int v = 256 - (x * x) / 32; return (a & 128) ? -v : v; }
static int16_t rnd_param(void) {
    switch (rnd(10)) {
    case 0: return 0;
    case 1: return (int16_t)(rnd(2) ? 256 : -256);
    case 2: return (int16_t)rndr(-300, 300);
    case 3: return (int16_t)rndr(-2047, 2047);
    case 4: return (int16_t)(rnd(2) ? rndr(2040, 2100) : -rndr(2040, 2100));
    case 5: return (int16_t)rnd64();
    case 6: return (int16_t)(rnd(2) ? 32767 : -32768);
    case 7: return (int16_t)rndr(-64, 64);
    default: { int a = rnd(256); return (int16_t)((sintab_cos(a) * (64 + rnd(768))) >> 8); }
    }
}
static int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }
static int32_t rnd_ref(uint32_t size) {      /* a reference point near the map, or anywhere */
    switch (rnd(4)) {
    case 0: return sext28((uint32_t)rnd64());
    case 1: return rndr(-(int32_t)size * 256, (int32_t)size * 512);
    case 2: return (int32_t)(rnd(size) << 8) + rnd(256) - (rnd(2) ? (int32_t)size * 128 : 0);
    default: return rndr(-2048, 2048);
    }
}

static uint8_t *layer(uint8_t *eng, uint32_t n) { return eng + 0xc0 + n * 0xb0; }

static void layer_base(uint8_t *L, uint32_t n) {
    memset(L + 8, 0, 0xb0 - 8);
    BPTR(L, BGL_ENG) = L - 0xc0 - n * 0xb0;
    BPTR(L, BGL_VRAM) = vram;
    BPTR(L, BGL_PAL) = pal;
    BPTR(L, BGL_EXTPAL) = rnd(10) ? xpal : 0;
    BU8(L, BGL_EXTON) = (uint8_t)rnd(2);
    rndfill(L + BGL_RECA, 0x88 - BGL_RECA);          /* derived affine state: garbage unless recomputed */
}

/* a text BG layer as set_bg_control + set_display_control leave it */
static void setup_text(uint8_t *L) {
    uint32_t cnt = (uint32_t)rnd(65536);
    BU16(L, BGL_CNT) = (uint16_t)cnt;
    BU32(L, BGL_SCRRAW) = ((cnt >> 8) & 31) << 11;
    BU32(L, BGL_CHRRAW) = ((cnt >> 2) & 15) << 14;
    BU32(L, BGL_MAP) = BU32(L, BGL_SCRRAW) + (rnd(8) << 16);
    BU32(L, BGL_CHR) = BU32(L, BGL_CHRRAW) + (rnd(8) << 16);
    BU16(L, BGL_HOFS) = (uint16_t)(rnd(4) ? rnd(512) : rnd(65536));
    BU16(L, BGL_VOFS) = (uint16_t)(rnd(4) ? rnd(512) : rnd(65536));
    BPTR(L, BGL_FN) = (uint8_t *)(ut_base + DS_RENDER_SCANLINE_TILED_EXT);
}

/* an affine / bitmap layer (BG2/BG3) as set_bg_control leaves it: kind 0 normal, 1 extended, 2 bmp16, 3 bmp8,
 * 4 mode 6's large bitmap */
static void setup_affine(uint8_t *L, int kind) {
    uint32_t cnt = (uint32_t)rnd(65536), size = (cnt >> 14) & 3;
    if (kind == 1) cnt &= ~0x80u;
    if (kind == 2) cnt |= 0x84;
    if (kind == 3) cnt = (cnt | 0x80) & ~4u;
    BU16(L, BGL_CNT) = (uint16_t)cnt;
    BU32(L, BGL_SCRRAW) = ((cnt >> 8) & 31) << 11;
    BU32(L, BGL_CHRRAW) = ((cnt >> 2) & 15) << 14;
    BU32(L, BGL_MAP) = BU32(L, BGL_SCRRAW) + (rnd(8) << 16);
    BU32(L, BGL_CHR) = BU32(L, BGL_CHRRAW) + (rnd(8) << 16);
    uint32_t w = 1u << (size + 7);
    BU32(L, BGL_BMP) = ((cnt >> 8) & 31) << 14;
    BU8(L, BGL_TMASK) = (uint8_t)((w >> 3) - 1);
    BU8(L, BGL_LOG2T) = (uint8_t)(size + 4);
    if (w > 256) { BU16(L, BGL_WMASK) = 0x1ff; BU16(L, BGL_HMASK) = (uint16_t)((w >> 1) - 1); BU8(L, BGL_LOG2W) = 9; }
    else { BU16(L, BGL_WMASK) = BU16(L, BGL_HMASK) = (uint16_t)(w - 1); BU8(L, BGL_LOG2W) = (uint8_t)(size + 7); }
    if (kind == 4) {
        BU32(L, BGL_BMP) = 0;
        if (cnt & 0x4000) { BU16(L, BGL_WMASK) = 0x3ff; BU16(L, BGL_HMASK) = 0x1ff; BU8(L, BGL_LOG2W) = 10; }
        else { BU16(L, BGL_WMASK) = 0x1ff; BU16(L, BGL_HMASK) = 0x3ff; BU8(L, BGL_LOG2W) = 9; }
    }
    uint32_t sz = kind >= 2 ? (uint32_t)BU16(L, BGL_WMASK) + 1 : w, szy = kind >= 2 ? (uint32_t)BU16(L, BGL_HMASK) + 1 : w;
    if (rnd(2)) {
        BS16(L, BGL_PA) = rnd_param(); BS16(L, BGL_PB) = rnd_param(); BS16(L, BGL_PC) = rnd_param(); BS16(L, BGL_PD) = rnd_param();
        if (rnd(4) == 0) { BS16(L, BGL_PA) = 0x100; BS16(L, BGL_PC) = 0; }        /* identity in x (bitmap fast path) */
        BS32(L, BGL_REFX) = BS32(L, BGL_CURX) = rnd_ref(sz);
        BS32(L, BGL_REFY) = BS32(L, BGL_CURY) = rnd_ref(szy);
    } else {
        /* a game-like rotation / scale about the map centre, the screen centre on the map */
        int a = rnd(256), k = 64 + rnd(rnd(2) ? 448 : 2048);
        if (rnd(4) == 0) a = rnd(4) * 64;                                   /* 0, 90, 180, 270 degrees */
        int c = sintab_cos(a), si = sintab_cos(a - 64);
        int32_t pa = c * k / 256, pb = -si * k / 256, pc = si * k / 256, pd = c * k / 256;
        if (a % 64 == 0 && rnd(2)) { pa = a == 0 ? k : a == 128 ? -k : 0; pd = pa; pc = a == 64 ? k : a == 192 ? -k : 0; pb = -pc; }
        BS16(L, BGL_PA) = (int16_t)pa; BS16(L, BGL_PB) = (int16_t)pb; BS16(L, BGL_PC) = (int16_t)pc; BS16(L, BGL_PD) = (int16_t)pd;
        int32_t cx = (int32_t)(sz * 128) + rndr(-(int32_t)sz * 96, (int32_t)sz * 96);
        int32_t cy = (int32_t)(szy * 128) + rndr(-(int32_t)szy * 96, (int32_t)szy * 96);
        BS32(L, BGL_REFX) = BS32(L, BGL_CURX) = cx - (pa * 128 + pb * 96);
        BS32(L, BGL_REFY) = BS32(L, BGL_CURY) = cy - (pc * 128 + pd * 96);
    }
    BU8(L, BGL_DIRTY) = 1;
    static const uint32_t fns[5] = { DS_RENDER_SCANLINE_AFFINE_NORMAL_EXT, DS_RENDER_SCANLINE_AFFINE_EXTENDED_EXT,
                                     DS_RENDER_SCANLINE_BITMAP_16BPP, DS_RENDER_SCANLINE_BITMAP_8BPP, DS_RENDER_SCANLINE_BITMAP_8BPP };
    BPTR(L, BGL_FN) = (uint8_t *)(ut_base + fns[kind]);
}

/* ---- the comparison ---- */
/* the line buffers (4 slots of 0x220 bytes at +0x40) and the bitmaps (4 x 32 bytes at +0x40), guards around both */
#define LINES_SZ (0x40 + 4 * 0x220 + 0x40)
#define VIS_SZ   (0x40 + 4 * 32 + 0x40)
static uint8_t linesA[LINES_SZ] __attribute__((aligned(16))), linesB[LINES_SZ] __attribute__((aligned(16)));
static uint8_t visA[VIS_SZ] __attribute__((aligned(16))), visB[VIS_SZ] __attribute__((aligned(16)));
static uint8_t pat;
static const char *pathname[8] = { "4bpp", "8bpp", "8bpp-ext", "affine", "ext-affine", "bmp16", "bmp8", "frames" };
static int pad_lo[8], pad_hi[8], pad_seen[8];     /* DraStic's writes outside buf[0..255]: byte offsets from buf */
static unsigned long px_written, px_visible;

static uint16_t *bufA(uint32_t n) { return (uint16_t *)(linesA + 0x40 + n * 0x220 + 0x10); }
static uint16_t *bufB(uint32_t n) { return (uint16_t *)(linesB + 0x40 + n * 0x220 + 0x10); }

static void fill_out(void) {
    pat = (uint8_t)rnd(256);
    memset(linesA, pat, sizeof linesA); memset(linesB, pat, sizeof linesB);
    memset(visA, pat, sizeof visA); memset(visB, pat, sizeof visB);
}

/* after a call that rendered the slots in mask: returns 1 on a mismatch. In the slots of strict (rendered once on the
 * line) every u16 the port wrote must equal DraStic's; a slot the render list names twice is rendered twice into the
 * same buffer, and the second render leaves DraStic's junk past its span (bg.md 4.6) over the first render's pixels:
 * there only the visible pixels count. */
static int cmp_out(uint32_t slots, uint32_t strict, int path, const char *what) {
    if (ut_cmp(what, visA, visB, sizeof visA)) { fprintf(stderr, "   (bitmaps, path %s)\n", pathname[path]); return 1; }
    uint16_t p16 = (uint16_t)(pat | pat << 8);
    for (uint32_t n = 0; n < 4; n++) {
        if (!(slots >> n & 1)) continue;
        const uint16_t *a = bufA(n), *b = bufB(n);
        const uint8_t *v = visA + 0x40 + 32 * n;
        for (int x = 0; x < 256; x++) {
            int vis = v[x >> 3] >> (x & 7) & 1;
            if (((b[x] != p16 && (strict >> n & 1)) || vis) && a[x] != b[x]) {
                fprintf(stderr, "FAIL %s: slot %u pixel %d (%s) drastic %04x port %04x (path %s)\n", what, n, x,
                        vis ? "visible" : "invisible", a[x], b[x], pathname[path]);
                ut_fail++;
                return 1;
            }
            px_written += b[x] != p16 && (strict >> n & 1);
            px_visible += vis;
        }
    }
    /* the port writes nothing outside the 256 pixels of the slots; DraStic's padding writes are measured */
    for (size_t i = 0; i < sizeof linesB; i++) {
        long o = (long)i - 0x40, n = o >= 0 ? o / 0x220 : 0, r = o - n * 0x220 - 0x10;   /* r: byte offset from buf n */
        int inbuf = o >= 0 && o < 4 * 0x220 && r >= 0 && r < 0x200 && (slots >> n & 1);
        if (!inbuf && linesB[i] != pat) {
            fprintf(stderr, "FAIL %s: the port wrote byte %ld of the line buffers (outside the pixels; path %s)\n", what, o,
                    pathname[path]);
            ut_fail++;
            return 1;
        }
        if (!inbuf && linesA[i] != pat && __builtin_popcount(slots) == 1) {   /* (one slot: unambiguous) */
            long d = o - (long)(__builtin_ctz(slots) * 0x220 + 0x10);
            if (!pad_seen[path] || d < pad_lo[path]) pad_lo[path] = (int)d;
            if (!pad_seen[path] || d > pad_hi[path]) pad_hi[path] = (int)d;
            pad_seen[path] = 1;
        }
    }
    return 0;
}

static void copy_hdr(void) { memcpy(engB, engA, HDR); }
static int cmp_hdr(const char *what) { return ut_cmp(what, engA, engB, HDR); }
static int cmp_eng(const char *what) { return ut_cmp(what, engA, engB, ENG_SIZE); }

/* ---- DraStic vs a hardware-style per-pixel model (BG_CENSUS=1): re2d/bg.md 4.1's census ---- */
static void ideal_affine(uint8_t *L, uint16_t *buf, uint8_t *vis, int extended) {
    uint32_t cnt = BU16(L, BGL_CNT), mask = BU8(L, BGL_TMASK), lg = BU8(L, BGL_LOG2T);
    int32_t X = BS32(L, BGL_CURX), Y = BS32(L, BGL_CURY), PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    int32_t size = (int32_t)(mask + 1) * 8;
    uint8_t *vr = BPTR(L, BGL_VRAM), *map = vr + BU32(L, BGL_MAP), *chr = vr + BU32(L, BGL_CHR);
    int ext = extended && BU8(L, BGL_EXTON);
    const uint16_t *pl = (const uint16_t *)BPTR(L, ext ? BGL_EXTPAL : BGL_PAL);
    memset(vis, 0, 32);
    for (int x = 0; x < 256; x++) {
        int32_t sx = (int32_t)((uint32_t)X + (uint32_t)(PA * x)) >> 8, sy = (int32_t)((uint32_t)Y + (uint32_t)(PC * x)) >> 8;
        if (!(cnt & 0x2000) && (sx < 0 || sy < 0 || sx >= size || sy >= size)) continue;
        sx &= size - 1; sy &= size - 1;
        uint32_t i2 = (uint32_t)(sx >> 3) + ((uint32_t)(sy >> 3) << lg), o = (uint32_t)(sx & 7) | (uint32_t)(sy & 7) << 3;
        uint32_t ix, c;
        if (!extended) { ix = chr[map[i2] * 64 + o]; c = pl[ix]; }
        else {
            uint32_t e = ((const uint16_t *)map)[i2];
            if (e & 0x400) o ^= 7;
            if (e & 0x800) o ^= 0x38;
            ix = chr[((e & 0x3ff) << 6) + o];
            c = ext ? pl[(e >> 12) << 8 | ix] : pl[ix];
        }
        buf[x] = (uint16_t)c;
        if (ix) vis[x >> 3] |= (uint8_t)(1u << (x & 7));
    }
}
static void ideal_bitmap(uint8_t *L, uint16_t *buf, uint8_t *vis, int b16) {
    int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC), X = BS32(L, BGL_CURX), Y = BS32(L, BGL_CURY);
    uint32_t cnt = BU16(L, BGL_CNT), lg = BU8(L, BGL_LOG2W), wm = BU16(L, BGL_WMASK), hm = BU16(L, BGL_HMASK), base = BU32(L, BGL_BMP);
    uint8_t *vr = BPTR(L, BGL_VRAM);
    const uint16_t *pl = (const uint16_t *)BPTR(L, BGL_PAL);
    memset(vis, 0, 32);
    for (int x = 0; x < 256; x++) {
        int32_t sx = (int32_t)((uint32_t)X + (uint32_t)(PA * x)) >> 8, sy = (int32_t)((uint32_t)Y + (uint32_t)(PC * x)) >> 8;
        if (!(cnt & 0x2000) && (sx < 0 || sy < 0 || sx > (int32_t)wm || sy > (int32_t)hm)) continue;
        uint32_t o = (((uint32_t)sy & hm) << lg) + ((uint32_t)sx & wm), c, op;
        if (b16) { c = *(const uint16_t *)(vr + base + 2 * o); op = c >> 15; }
        else { uint32_t ix = vr[base + o]; c = pl[ix]; op = ix != 0; }
        buf[x] = (uint16_t)c;
        if (op) vis[x >> 3] |= (uint8_t)(1u << (x & 7));
    }
}
static unsigned dev_cases[5][2][2][2], dev_bad[5][2][2][2], dev_px[5][2][2][2], cov_vis[5][2][2][2];
static int bigp(int32_t v) { return v < -2047 || v > 2047; }
static void dev_check(uint8_t *L, int kind, const uint16_t *bd, const uint8_t *vd) {
    uint16_t ib[256]; uint8_t iv[32];
    if (!census) return;
    if (kind == 1 && BU8(L, BGL_EXTON) && !BPTR(L, BGL_EXTPAL)) return;   /* stale-line case */
    if (kind <= 1) ideal_affine(L, ib, iv, kind); else ideal_bitmap(L, ib, iv, kind == 2);
    int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    int w = (BU16(L, BGL_CNT) >> 13) & 1, b = bigp(PA) || bigp(PC), z = PA == 0 || PC == 0;
    unsigned bad = 0, nv = 0;
    for (int x = 0; x < 256; x++) {
        int dv = vd[x >> 3] >> (x & 7) & 1, sv = iv[x >> 3] >> (x & 7) & 1;
        nv += dv;
        if (dv != sv || (dv && bd[x] != ib[x])) bad++;
    }
    dev_cases[kind][w][b][z]++; cov_vis[kind][w][b][z] += nv;
    if (bad) { dev_bad[kind][w][b][z]++; dev_px[kind][w][b][z] += bad; }
}
static void dev_report(const char *title) {
    static const char *kn[5] = { "affine", "ext-affine", "bmp16", "bmp8", "large-bmp8" };
    if (!census) return;
    fprintf(stderr, "%s: DraStic vs the per-pixel model (cases / cases differing / pixels differing / avg visible px)\n", title);
    for (int k = 0; k < 5; k++) for (int w = 0; w < 2; w++) for (int b = 0; b < 2; b++) for (int z = 0; z < 2; z++)
        if (dev_cases[k][w][b][z])
            fprintf(stderr, "  %-10s %s %s %s: %6u %6u %8u %6.1f\n", kn[k], w ? "wrap" : "clip", b ? "|PA|or|PC|>2047" : "small          ",
                    z ? "PA=0|PC=0" : "         ", dev_cases[k][w][b][z], dev_bad[k][w][b][z], dev_px[k][w][b][z],
                    (double)cov_vis[k][w][b][z] / dev_cases[k][w][b][z]);
    memset(dev_cases, 0, sizeof dev_cases); memset(dev_bad, 0, sizeof dev_bad); memset(dev_px, 0, sizeof dev_px); memset(cov_vis, 0, sizeof cov_vis);
}

/* ---- one layer renderer on both engines ---- */
static int run_layer(uint32_t n, uint32_t line, int path, int kind, const char *what) {
    uint8_t *La = layer(engA, n), *Lb = layer(engB, n);
    fill_out();
    if (verbose) { fprintf(stderr, "drastic: %s\n", what); fflush(stderr); }
    ((spec_bg_fn)BPTR(La, BGL_FN))(La, bufA(n), visA + 0x40 + 32 * n, line);
    if (verbose) { fprintf(stderr, "port\n"); fflush(stderr); }
    spec_bg_fn f = spec_bg_renderer((uintptr_t)BPTR(Lb, BGL_FN) - ut_base);
    if (f) f(Lb, bufB(n), visB + 0x40 + 32 * n, line);
    int bad = cmp_out(1u << n, 1u << n, path, what);
    if (!bad && cmp_hdr(what)) { fprintf(stderr, "   (engine header / layer structs, path %s)\n", pathname[path]); bad = 1; }
    if (!bad && kind >= 0) dev_check(La, kind, bufA(n), visA + 0x40 + 32 * n);   /* (the renderers keep X, Y, PA, PC) */
    return bad;
}

static void test_blank(void) {
    char what[64];
    unsigned n = 0;
    for (int t = 0; t < nmax * 5 && ut_fail < 10; t++) {
        uint8_t v[0x40 + 128 + 0x40], vb[sizeof v];
        rndfill(v, sizeof v);
        memset(v + 0x40, 0, 128);
        for (int k = 0; k < 4; k++) if (rnd(2)) { int c = rnd(4); while (c--) v[0x40 + k * 32 + rnd(32)] |= (uint8_t)(1u << rnd(8)); }
        if (rnd(4) == 0) rndfill(v + 0x40 + 32 * rnd(4), 32);
        memcpy(vb, v, sizeof v);
        uint32_t m[3] = { (uint32_t)rnd64(), 0, (uint32_t)rnd64() }, mb[3];
        m[1] = rnd(2) ? m[0] : (uint32_t)rnd64();
        memcpy(mb, m, sizeof m);
        DS(blank_fn, DS_BLANK)(v + 0x40, &m[1]);
        spec_render_scanline_disable_blank_layers(vb + 0x40, &mb[1]);
        snprintf(what, sizeof what, "disable_blank_layers test %d", t);
        if (ut_cmp(what, m, mb, sizeof m) || ut_cmp(what, v, vb, sizeof v)) break;
        n++;
    }
    fprintf(stderr, "t_bg2d: disable_blank_layers %u cases\n", n);
}

/* the edge math alone: video_2d_bg_layer_affine_setup_edges, then render_scanline_update_affine_variables */
static void test_edges(void) {
    char what[128];
    unsigned n1 = 0, n2 = 0;
    for (int t = 0; t < nmax * 50 && ut_fail < 10; t++) {
        int32_t X = rnd(3) ? rnd_ref(1u << (7 + rnd(4))) : (int32_t)rnd64(), P = rnd_param(), Q = rnd_param();
        int32_t W = rnd(2) ? (int32_t)((rnd(1024) << 8) + 0xff) : (int32_t)((rnd(128) << 11) + 0x7ff);
        int64_t d[5], s[5];                         /* guards at [0] and [4] */
        rndfill(d, sizeof d); memcpy(s, d, sizeof d);
        DS(edges_fn, DS_EDGES)(X, P, W, Q, &d[1], &d[2], &d[3]);
        spec_video_2d_bg_layer_affine_setup_edges(X, P, W, Q, &s[1], &s[2], &s[3]);
        snprintf(what, sizeof what, "setup_edges X %d P %d W %d Q %d", X, P, W, Q);
        if (ut_cmp(what, d, s, sizeof d)) break;
        n1++;
    }
    for (int t = 0; t < nmax * 20 && ut_fail < 10; t++) {
        uint32_t n = 2 + rnd(2);
        uint8_t *L = layer(engA, n);
        layer_base(L, n);
        setup_affine(L, (int)rnd(2));
        copy_hdr();
        DS(uav_fn, DS_UAV)(L);
        spec_render_scanline_update_affine_variables(layer(engB, n));
        snprintf(what, sizeof what, "update_affine_variables test %d", t);
        if (cmp_hdr(what)) break;
        n2++;
    }
    snprintf(what, sizeof what, "edges group: engine struct");
    cmp_eng(what);
    fprintf(stderr, "t_bg2d: setup_edges %u cases, update_affine_variables %u\n", n1, n2);
}

static void test_text(void) {
    char what[96];
    unsigned cnt[3] = { 0 }, nullext = 0;
    for (int t = 0; t < nmax * 4 && ut_fail < 10; t++) {
        uint32_t n = rnd(4);
        uint8_t *L = layer(engA, n);
        layer_base(L, n);
        setup_text(L);
        copy_hdr();
        uint32_t line = rnd(192), cntv = BU16(L, BGL_CNT);
        int path = !(cntv & 0x80) ? 0 : BU8(L, BGL_EXTON) ? 2 : 1;
        cnt[path]++;
        nullext += path == 2 && !BPTR(L, BGL_EXTPAL);
        snprintf(what, sizeof what, "text test %d cnt %04x h %x v %x line %u", t, cntv, BU16(L, BGL_HOFS), BU16(L, BGL_VOFS), line);
        run_layer(n, line, path, -1, what);
    }
    cmp_eng("text group: engine struct");
    fprintf(stderr, "t_bg2d: text 4bpp %u, 8bpp %u, 8bpp ext %u (%u with a NULL ext slot)\n", cnt[0], cnt[1], cnt[2], nullext);
}

/* single affine / bitmap lines with fresh edges (L+0xae = 1) */
static void test_affine(void) {
    char what[160];
    unsigned cnt[5] = { 0 };
    for (int t = 0; t < nmax * 10 && ut_fail < 10; t++) {
        int kind = (int)rnd(5);
        uint32_t n = 2 + rnd(2);
        uint8_t *L = layer(engA, n);
        layer_base(L, n);
        setup_affine(L, kind);
        copy_hdr();
        cnt[kind]++;
        snprintf(what, sizeof what, "affine kind %d test %d cnt %04x pa %d pb %d pc %d pd %d x %d y %d", kind, t, BU16(L, BGL_CNT),
                 BS16(L, BGL_PA), BS16(L, BGL_PB), BS16(L, BGL_PC), BS16(L, BGL_PD), BS32(L, BGL_CURX), BS32(L, BGL_CURY));
        run_layer(n, rnd(192), kind == 0 ? 3 : kind == 1 ? 4 : kind == 2 ? 5 : 6, kind == 4 ? 4 : kind, what);
    }
    cmp_eng("affine group: engine struct");
    dev_report("single lines, fresh edges");
    fprintf(stderr, "t_bg2d: affine single lines: normal %u, extended %u, bmp16 %u, bmp8 %u, large bmp8 %u\n", cnt[0], cnt[1],
            cnt[2], cnt[3], cnt[4]);
}

/* whole frames through render_scanline_bg: render_scanline's per-line affine stepping, mosaic, mid-frame register
 * writes (as the event replay applies them), render list changes, the null renderer */
static void test_frames(void) {
    char what[128];
    unsigned nlines = 0, nframes = 0, nmosaic = 0, nnull = 0, ndirect = 0;
    int evmask = getenv("BG_EV") ? atoi(getenv("BG_EV")) : 0x7f;
    for (int fr = 0; fr < nmax / 4 && ut_fail < 10; fr++) {
        memset(engA, 0, HDR);
        int kinds[4];
        for (uint32_t n = 0; n < 4; n++) {
            uint8_t *L = layer(engA, n);
            layer_base(L, n);
            if (n < 2 || rnd(4) == 0) { setup_text(L); kinds[n] = -1; }
            else { kinds[n] = (int)rnd(5); setup_affine(L, kinds[n]); }
            if (rnd(8) == 0) { BPTR(L, BGL_FN) = (uint8_t *)(ut_base + DS_RENDER_SCANLINE_NULL); nnull++; }
            if (n >= 2 && rnd(16) == 0) { BPTR(L, BGL_DIRECT) = vram; ndirect++; }   /* a direct bitmap: skipped */
        }
        BU16(engA, 0xa8) = (uint16_t)(rnd(3) ? 0 : rnd(65536));
        BU8(engA, 0xb2) = (uint8_t)rnd(5);
        for (uint32_t i = 0; i < 4; i++) BU8(engA, 0x8c + i) = (uint8_t)rnd(4);
        if (rnd(2)) { uint8_t p[4] = { 0, 1, 2, 3 }; for (int i = 3; i > 0; i--) { int j = rnd(i + 1); uint8_t x = p[i]; p[i] = p[j]; p[j] = x; } memcpy(engA + 0x8c, p, 4); }
        copy_hdr();
        for (uint32_t line = 0; line < 192 && ut_fail < 10; line++) {
            /* render_scanline's affine stepping (lines 1..191) and line-0 reload, on both */
            for (uint8_t *E = engA;; E = engB) {
                for (int n = 2; n < 4; n++) {
                    uint8_t *L = layer(E, n);
                    if (line == 0) { BS32(L, BGL_CURX) = BS32(L, BGL_REFX); BS32(L, BGL_CURY) = BS32(L, BGL_REFY); BU8(L, BGL_DIRTY) = 1; }
                    else { BS32(L, BGL_CURX) += BS16(L, BGL_PB); BS32(L, BGL_CURY) += BS16(L, BGL_PD); }
                }
                if (E == engB) break;
            }
            /* a random mid-frame register write, as the replay applies it */
            if (rnd(24) == 0) {
                uint32_t n = 2 + rnd(2), ev = rnd(6);
                if (!(evmask >> ev & 1)) ev = 99;
                int16_t v = rnd_param(); int32_t r = rnd_ref(256); uint16_t m = (uint16_t)rnd(65536);
                for (int k = 0; k < 2; k++) {
                    uint8_t *E = k ? engB : engA, *L = layer(E, n);
                    switch (ev) {
                    case 0: BS16(L, BGL_PA) = v; BU8(L, BGL_DIRTY) = 1; break;
                    case 1: BS16(L, BGL_PB) = v; BU8(L, BGL_DIRTY) = 1; break;
                    case 2: BS16(L, BGL_PD) = v; BU8(L, BGL_DIRTY) = 1; break;
                    case 3: BS32(L, BGL_REFX) = BS32(L, BGL_CURX) = r; BU8(L, BGL_DIRTY) = 1; break;
                    case 4: BU16(L, BGL_CNT) ^= 0x2000; break;                     /* the wrap bit */
                    case 5: BU16(E, 0xa8) = m; break;                               /* MOSAIC */
                    }
                }
            }
            if ((evmask & 0x40) && rnd(32) == 0) { uint8_t c = (uint8_t)rnd(5); BU8(engA, 0xb2) = c; BU8(engB, 0xb2) = c; }
            fill_out();
            DS(rsbg_fn, DS_BG)(engA, linesA + 0x40, visA + 0x40, line);
            spec_render_scanline_bg(engB, linesB + 0x40, visB + 0x40, line, ut_base);
            nlines++;
            uint32_t slots = 0, twice = 0;
            for (uint32_t i = 0; i < BU8(engA, 0xb2); i++) {
                uint32_t b = 1u << BU8(engA, 0x8c + i);
                twice |= slots & b; slots |= b;
            }
            for (uint32_t n = 0; n < 4; n++) if ((slots >> n & 1) && (BU16(layer(engA, n), BGL_CNT) & 0x40) && (BU16(engA, 0xa8) & 0xff)) { nmosaic++; break; }
            snprintf(what, sizeof what, "frame %d line %u mosaic %04x list %u", fr, line, BU16(engA, 0xa8), BU8(engA, 0xb2));
            if (cmp_out(slots, slots & ~twice, 7, what)) break;
            if (cmp_hdr(what)) { fprintf(stderr, "   (engine header / layer structs)\n"); break; }
            for (uint32_t i = 0; i < BU8(engA, 0xb2) && census; i++) {
                uint32_t n = BU8(engA, 0x8c + i);
                uint8_t *L = layer(engA, n);
                int twice = 0;
                for (uint32_t j = 0; j < BU8(engA, 0xb2); j++) twice += BU8(engA, 0x8c + j) == n;
                if (twice > 1 || n < 2 || kinds[n] < 0 || (BU16(L, BGL_CNT) & 0x40) ||
                    !spec_bg_renderer((uintptr_t)BPTR(L, BGL_FN) - ut_base)) continue;
                dev_check(L, kinds[n], bufA(n), visA + 0x40 + n * 32);
            }
        }
        snprintf(what, sizeof what, "frame %d: engine struct", fr);
        cmp_eng(what);
        nframes++;
    }
    dev_report("frames (stepped edges)");
    fprintf(stderr, "t_bg2d: frames %u (%u lines, %u with mosaic; %u null renderers, %u direct layers)\n", nframes, nlines,
            nmosaic, nnull, ndirect);
}

/* clip mode with |PA| or |PC| > 2047 and the span on screen: the tile-run method with large steps */
static void test_bigclip(void) {
    char what[160];
    unsigned vis_total = 0, n_cases = 0;
    for (int t = 0; t < nmax * 10 && ut_fail < 10; t++) {
        int kind = (int)rnd(2);
        uint32_t n = 2 + rnd(2);
        uint8_t *L = layer(engA, n);
        layer_base(L, n);
        setup_affine(L, kind);
        uint32_t cnt = BU16(L, BGL_CNT) & ~0x2000u;                 /* clip */
        if (rnd(2)) cnt |= 0xc000;                                  /* 1024 x 1024 */
        BU16(L, BGL_CNT) = (uint16_t)cnt;
        uint32_t size = (cnt >> 14) & 3, w = 1u << (size + 7);
        BU8(L, BGL_TMASK) = (uint8_t)((w >> 3) - 1); BU8(L, BGL_LOG2T) = (uint8_t)(size + 4);
        int32_t big = rndr(2048, 32767) * (rnd(2) ? 1 : -1), sm = rndr(-2047, 2047);
        if (rnd(2)) { BS16(L, BGL_PA) = (int16_t)big; BS16(L, BGL_PC) = (int16_t)(rnd(2) ? sm : rndr(2048, 32767) * (rnd(2) ? 1 : -1)); }
        else { BS16(L, BGL_PC) = (int16_t)big; BS16(L, BGL_PA) = (int16_t)sm; }
        int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
        int32_t x0 = rnd(256), sx = (int32_t)rnd(w << 8), sy = (int32_t)rnd(w << 8);   /* screen x0 lands at (sx, sy) */
        BS32(L, BGL_CURX) = sx - PA * x0; BS32(L, BGL_CURY) = sy - PC * x0;
        BU8(L, BGL_DIRTY) = 1;
        copy_hdr();
        snprintf(what, sizeof what, "bigclip kind %d test %d cnt %04x pa %d pc %d x %d y %d", kind, t, cnt, PA, PC,
                 BS32(L, BGL_CURX), BS32(L, BGL_CURY));
        run_layer(n, rnd(192), kind ? 4 : 3, kind, what);
        for (int i = 0; i < 32; i++) vis_total += (unsigned)__builtin_popcount(visA[0x40 + 32 * n + i]);
        n_cases++;
    }
    cmp_eng("bigclip group: engine struct");
    dev_report("clip, large |PA| or |PC|, span on screen");
    fprintf(stderr, "t_bg2d: bigclip %u cases, %u visible pixels\n", n_cases, vis_total);
}

void ut_main(void) {
    const char *s = getenv("BG_N"); if (s) nmax = atoi(s);
    const char *only = getenv("BG_ONLY");
    census = getenv("BG_CENSUS") != 0;
    verbose = getenv("BG_V") != 0;
    uint8_t *res = mmap(0, ((size_t)1 << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    pal = malloc(0x400); xpal = malloc(0x2000);
    engA = malloc(ENG_SIZE); engB = malloc(ENG_SIZE);
    if (res == MAP_FAILED || !pal || !xpal || !engA || !engB) { fprintf(stderr, "FAIL: no memory\n"); ut_fail++; return; }
    vram = res + ((size_t)1 << 31);
    vmem = vram - VPRE;
    rndfill(vmem, VPRE + VSIZE + VPRE); rndfill(pal, 0x400); rndfill(xpal, 0x2000);
    rndfill(engA, ENG_SIZE); memcpy(engB, engA, ENG_SIZE);
    if (!only || strstr(only, "blank")) test_blank();
    if (!only || strstr(only, "edges")) test_edges();
    if (!only || strstr(only, "text")) test_text();
    if (!only || strstr(only, "affine")) test_affine();
    if (!only || strstr(only, "frames")) test_frames();
    if (!only || strstr(only, "bigclip")) test_bigclip();
    cmp_eng("the end: engine struct");
    fprintf(stderr, "t_bg2d: %lu visible pixels and %lu port-written pixels compared\n", px_visible, px_written);
    for (int p = 0; p < 8; p++)
        if (pad_seen[p]) fprintf(stderr, "t_bg2d: DraStic's writes outside buf[0..255], path %s: byte offsets %d .. %d from buf\n",
                                 pathname[p], pad_lo[p], pad_hi[p]);
}
