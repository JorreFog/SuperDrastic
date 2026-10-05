/* t_obj2d.c: DraStic's OBJ path vs src/rast/spec/2d/obj.c, in DraStic's process. DraStic's functions run on engine A,
 * the port on engine B: two full 0x81420-byte engine structs, identical at the start of every frame (random stale OBJ
 * tables included), sharing the VRAM alias, palette, ext palette, OAM and a fake video struct (the bank records, the
 * capture validity bits and hi-res buffers the full-screen image reads).
 *   video_2d_obj_affine_setup_edges: random cases of the analysis' four kinds, the three outputs between guards.
 *   frames: random OAM (every shape, size, mode and flag, rotation / scale / identity / degenerate matrices, OBJs at
 *   (0, 0) and duplicates), random DISPCNT mapping bits, engine A or B, ext palettes on / off, the 12-sprite full-screen
 *   image (also broken by one bit, and with or without a matching bank and valid hi-res data), sparse frames with a few
 *   OBJs (lines without any). Then video_2d_reorder_obj on both, the whole engine struct compared;
 *   render_scanline_obj_c for lines 0..191 on both, with render_scanline_2d's scratch frame (random, carried from line
 *   to line as on DraStic's stack) between 64 guard bytes, the whole frame compared after every line, and the return
 *   values; in half the frames a mid-frame OAM rewrite (sometimes with new DISPCNT mapping bits) and a re-sort on
 *   both, as render_scanline_2d runs it after a replayed OAM write; the whole engine struct again before that re-sort
 *   and at the end of the frame.
 * The generators are the analysis' (re2d/obj.md: objharness.c, which ran DraStic's code loaded from the ELF).
 * run.sh t_obj2d.c ../../../src/rast/spec/2d/obj.c
 * Env: UT_SEED; OBJ_N frames (default 1500); OBJ_EDGES setup_edges cases (default 200000). */
#include "ut.h"
#include <math.h>
#include "spec/2d/obj.h"

#define ENG_SIZE   0x81420
#define VIDEO_SIZE 0x459000
#define VRAM_SIZE  (0x800000 + 0x80000)
#define FRAME      0x1d30                           /* render_scanline_2d's stack frame; S = frame + 0x180 */
#define S_OFS      0x180
#define G          64

typedef void (*reorder_fn)(uint8_t *);
typedef uint32_t (*render_fn)(uint8_t *, uint16_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint32_t);
typedef void (*edges_fn)(int32_t, int32_t, int32_t, int32_t, int64_t *, int64_t *, int64_t *);

static uint8_t *engA, *engB, *video, *vram, *pal, *oam, *ext, *hires[4];
static uint8_t frA[G + FRAME + G] __attribute__((aligned(64))), frB[G + FRAME + G] __attribute__((aligned(64)));

static uint32_t r32(void) { return (uint32_t)rnd64(); }

static int16_t rparam(void) {
    switch (rnd(10)) {
    case 0: return 0;
    case 1: return 0x100;
    case 2: return -0x100;
    case 3: return (int16_t)(rnd(0x200) - 0x100);
    case 4: return (int16_t)(rnd(0x80) - 0x40);
    case 5: return (int16_t)r32();
    case 6: return (int16_t)(rnd(0x400) - 0x200);
    case 7: return (int16_t)(0x100 + rnd(16) - 8);
    default: return (int16_t)(rnd(0x300) - 0x180);
    }
}

static void fill_vram(void) {
    for (size_t i = 0; i < VRAM_SIZE; i += 4) {
        uint32_t v = r32();
        if (rnd(4) == 0) v &= 0x0f0f0f0f;          /* some transparent nibbles */
        if (rnd(8) == 0) v = 0;
        if (rnd(6) == 0) v |= 0x80008000;          /* opaque bitmap pixels */
        memcpy(vram + i, &v, 4);
    }
}

/* the OBJ attributes of OAM entry i (kind 1 / 3: mostly affine; 3 with game-like rotation / scale groups; 2 with
 * identity groups) */
static void random_obj(int i, int kind) {
    uint16_t *o = (uint16_t *)oam;
    uint32_t y = rnd(4) ? rnd(192) : rnd(256);
    uint32_t mode = rnd(10);
    mode = mode < 5 ? 0 : mode < 7 ? 1 : mode < 8 ? 2 : 3;
    uint32_t aff = rnd(10) < 4, b9 = rnd(10) < 3, shape = rnd(16) == 0 ? 3 : rnd(3);
    if (kind == 1 || kind == 3) { aff = rnd(4) != 0; b9 = rnd(2); }
    uint32_t a0 = y | aff << 8 | b9 << 9 | mode << 10 | rnd(2) << 12 | rnd(2) << 13 | shape << 14;
    uint32_t x = rnd(4) ? rnd(256) : rnd(512);
    uint32_t a1 = x | rnd(32) << 9 | rnd(4) << 14;
    uint32_t a2 = rnd(1024) | rnd(4) << 10 | rnd(16) << 12;
    if (rnd(20) == 0) { a0 &= ~0xffu; a1 &= ~0x1ffu; }               /* at (0, 0): the duplicate filter */
    if (rnd(30) == 0 && i > 0) { a0 = o[(i - 1) * 4]; a1 = o[(i - 1) * 4 + 1]; a2 = o[(i - 1) * 4 + 2]; }
    o[i * 4] = (uint16_t)a0; o[i * 4 + 1] = (uint16_t)a1; o[i * 4 + 2] = (uint16_t)a2;
    o[i * 4 + 3] = (uint16_t)rparam();
}
static void random_groups(int kind) {
    uint16_t *o = (uint16_t *)oam;
    if (kind == 3) {                                   /* rotation + scale matrices, as games use them */
        for (int g = 0; g < 32; g++) {
            double a = rnd(3600) * 3.14159265358979 / 1800.0, sx = 0.25 + rnd(400) / 100.0, sy = rnd(2) ? sx : 0.25 + rnd(400) / 100.0;
            double c = cos(a), sn = sin(a);
            o[g * 16 + 3] = (uint16_t)(int16_t)(c * 256 / sx); o[g * 16 + 7] = (uint16_t)(int16_t)(-sn * 256 / sx);
            o[g * 16 + 11] = (uint16_t)(int16_t)(sn * 256 / sy); o[g * 16 + 15] = (uint16_t)(int16_t)(c * 256 / sy);
            if (rnd(8) == 0) {                         /* the four 90-degree turns */
                static const int16_t v[4][4] = { { 256, 0, 0, 256 }, { 0, -256, 256, 0 }, { -256, 0, 0, -256 }, { 0, 256, -256, 0 } };
                int q = rnd(4);
                o[g * 16 + 3] = (uint16_t)v[q][0]; o[g * 16 + 7] = (uint16_t)v[q][1];
                o[g * 16 + 11] = (uint16_t)v[q][2]; o[g * 16 + 15] = (uint16_t)v[q][3];
            }
        }
    }
    if (kind == 2)                                     /* a few identity groups */
        for (int g = 0; g < 32; g++) if (rnd(3) == 0) { o[g * 16 + 3] = 0x100; o[g * 16 + 7] = 0; o[g * 16 + 11] = 0; o[g * 16 + 15] = 0x100; }
}
static void random_oam(int kind) {
    for (int i = 0; i < 128; i++) random_obj(i, kind);
    random_groups(kind);
}

/* the 12 sprites of a 256x192 bitmap image (2D 256-wide bitmap mapping), optionally broken by one bit */
static void fullscreen_oam(int base_tile, int prio, int first_index, int breakit) {
    uint16_t *o = (uint16_t *)oam;
    int k = first_index;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++, k++) {
            int i = k & 127;
            o[i * 4] = (uint16_t)(r * 64 | 0x0c00);
            o[i * 4 + 1] = (uint16_t)(c * 64 | 0xc000);
            o[i * 4 + 2] = (uint16_t)(((base_tile + r * 256 + c * 8) & 0x3ff) | prio << 10 | 0xf000);
        }
    if (breakit) o[((first_index + rnd(12)) & 127) * 4 + rnd(3)] ^= (uint16_t)(1 << rnd(16));
}

static void setup_eng(uint8_t *eng, uint32_t dispcnt, int index, int with_ext) {
    rndfill(eng, 0x380);                               /* the registers the OBJ code does not read are anything */
    *(uint8_t **)(eng + 0x00) = video;
    *(uint8_t **)(eng + 0x08) = vram;
    *(uint8_t **)(eng + 0x18) = pal;
    *(uint8_t **)(eng + 0x28) = with_ext ? ext : NULL;
    *(uint8_t **)(eng + 0x30) = oam;
    *(uint32_t *)(eng + 0x90) = dispcnt;
    eng[0xb7] = (uint8_t)index;
}

static uint32_t random_dispcnt(void) { return r32() & (0x10 | 0x20 | 0x40 | 0x300000 | 0x400000 | 0x80000000u); }

static unsigned n_edges, n_frames, n_lines, n_resorts, n_image, n_image2x, n_ret;
static unsigned long n_entries;

static void test_edges(int iters) {
    char what[128];
    for (int it = 0; it < iters && ut_fail < 10; it++) {
        int32_t t0, dA, lim, dB;
        switch (rnd(4)) {
        case 0: t0 = (int32_t)rnd(1 << 23) - (1 << 22); dA = rparam(); lim = (int32_t)(8 << rnd(4)) * 256 - 1; dB = rparam(); break;
        case 1: t0 = (int32_t)rnd(0x4000) - 0x2000; dA = (int32_t)rnd(3) - 1; lim = 0x1fff; dB = rparam(); break;
        case 2: t0 = (int32_t)r32() >> rnd(16); dA = (int16_t)r32(); lim = (int32_t)rnd(0x4000); dB = (int16_t)r32(); break;
        default: t0 = (int32_t)rnd(0x10000) - 0x8000; dA = 0; lim = (int32_t)rnd(0x4000); dB = rnd(2) ? 0 : rparam(); break;
        }
        int64_t a[5], b[5];                            /* guards at [0] and [4] */
        rndfill(a, sizeof a); memcpy(b, a, sizeof a);
        DS(edges_fn, DS_VIDEO_2D_OBJ_AFFINE_SETUP_EDGES)(t0, dA, lim, dB, &a[1], &a[2], &a[3]);
        spec_video_2d_obj_affine_setup_edges(t0, dA, lim, dB, &b[1], &b[2], &b[3]);
        snprintf(what, sizeof what, "obj setup_edges t0 %d dA %d lim %d dB %d", t0, dA, lim, dB);
        if (ut_cmp(what, a, b, sizeof a)) return;
        n_edges++;
    }
}

static int reorder_both(const char *what) {
    DS(reorder_fn, DS_VIDEO_2D_REORDER_OBJ)(engA);
    spec_video_2d_reorder_obj(engB);
    return ut_cmp(what, engA, engB, ENG_SIZE);
}

static void test_frames(int iters) {
    char what[160];
    uint8_t *SA = frA + G + S_OFS, *SB = frB + G + S_OFS;
    fill_vram();
    for (int it = 0; it < iters && ut_fail < 10; it++) {
        rndfill(pal, 0x400); rndfill(ext, 0x2000);
        if (it % 16 == 15) fill_vram();
        int kind = rnd(4), sparse = rnd(6) == 0;
        random_oam(kind);
        if (sparse)                                    /* a few OBJs: lines without any, empty lists, return 0 */
            for (int i = 0; i < 128; i++) if (rnd(32)) ((uint16_t *)oam)[i * 4] = (uint16_t)(0x200 | rnd(256));
        uint32_t dc = random_dispcnt();
        int index = rnd(4) == 0;
        int image = rnd(4) == 0;
        if (image) {                                   /* the full-screen bitmap image */
            dc = (dc & ~0x60u) | 0x20;
            /* base tile 256 puts the image at w1 = 0x800, the edge of the hi-res data rule (w1*8 <= 0x4000), 257
             * just past it: rare with a uniform tile */
            fullscreen_oam(rnd(8) ? (int)rnd(1024) : 256 + (int)rnd(2), rnd(4), rnd(3) ? 0 : (int)rnd(116), rnd(4) == 0);
            memset(video, 0, 0x100);
            int bank = rnd(5);
            uint32_t objbase = index ? 0x600000 : 0x400000;
            if (bank < 4) { *(uint32_t *)(video + 0x10 + bank * 16) = 6; *(uint32_t *)(video + 0x18 + bank * 16) = objbase >> 14; }
            if (rnd(2)) { int b2 = rnd(4); *(uint32_t *)(video + 0x10 + b2 * 16) = 6; *(uint32_t *)(video + 0x18 + b2 * 16) = rnd(2) ? objbase >> 14 : 0x80; }
            for (int b = 0; b < 4; b++) { video[0x458840 + b] = (uint8_t)(rnd(2) ? 0xff : r32()); *(uint8_t **)(video + 0x458820 + b * 8) = hires[b]; }
        }
        setup_eng(engA, dc, index, rnd(3) != 0);
        rndfill(engA + OBJT_RECORDS, OBJT_END - OBJT_RECORDS);    /* stale tables */
        memcpy(engB, engA, ENG_SIZE);
        snprintf(what, sizeof what, "frame %d reorder (kind %d dispcnt %08x engine %c image %d)", it, kind, dc, index ? 'B' : 'A', image);
        if (reorder_both(what)) return;
        n_image += *(uint8_t **)(engA + OBJT_IMAGE) != 0;
        n_image2x += *(uint8_t **)(engA + OBJT_IMAGE2X) != 0 && *(uint8_t **)(engA + OBJT_IMAGE) != 0;
        rndfill(frA, sizeof frA); memcpy(frB, frA, sizeof frA);
        int resort_line = rnd(2) ? 1 + (int)rnd(191) : -1;
        for (uint32_t line = 0; line < 192; line++) {
            if ((int)line == resort_line) {            /* an OAM write replayed after the previous line, then the re-sort */
                snprintf(what, sizeof what, "frame %d line %u before the re-sort", it, line);
                if (ut_cmp(what, engA, engB, ENG_SIZE)) return;
                int n = 1 + rnd(8);
                while (n--) random_obj(rnd(128), kind);
                if (rnd(4) == 0) random_groups(3);
                if (rnd(3) == 0) {                     /* the mapping bits are latched by the re-sort */
                    uint32_t d = (dc & ~(0x10 | 0x20 | 0x40 | 0x300000 | 0x400000 | 0x80000000u)) | random_dispcnt();
                    *(uint32_t *)(engA + 0x90) = *(uint32_t *)(engB + 0x90) = d;
                }
                snprintf(what, sizeof what, "frame %d line %u re-sort", it, line);
                if (reorder_both(what)) return;
                n_resorts++;
            }
            uint32_t ra = DS(render_fn, DS_RENDER_SCANLINE_OBJ_C)(engA, (uint16_t *)(SA + 0xa70), SA + 0xc90, SA + 0xe20, SA + 0xec0, SA + 0xee0, line);
            uint32_t rb = spec_render_scanline_obj_c(engB, (uint16_t *)(SB + 0xa70), SB + 0xc90, SB + 0xe20, SB + 0xec0, SB + 0xee0, line);
            n_lines++;
            n_ret += ra != 0;
            snprintf(what, sizeof what, "frame %d line %u (kind %d dispcnt %08x engine %c)", it, line, kind,
                     *(uint32_t *)(engA + 0x90), index ? 'B' : 'A');
            if (ra != rb) { fprintf(stderr, "FAIL %s: render_scanline_obj_c returns %#x, port %#x\n", what, ra, rb); ut_fail++; return; }
            if (ut_cmp(what, frA, frB, sizeof frA)) { fprintf(stderr, "   (render_scanline_2d's frame, S at +%#x)\n", G + S_OFS); return; }
        }
        for (int y = 0; y < 192; y++) for (int l = 0; l < 5; l++) n_entries += engA[OBJT_COUNTS + l * 0xc0 + y];
        snprintf(what, sizeof what, "frame %d end: engine struct", it);
        if (ut_cmp(what, engA, engB, ENG_SIZE)) return;
        n_frames++;
    }
}

void ut_main(void) {
    int frames = getenv("OBJ_N") ? atoi(getenv("OBJ_N")) : 1500;
    int edges = getenv("OBJ_EDGES") ? atoi(getenv("OBJ_EDGES")) : 200000;
    engA = malloc(ENG_SIZE); engB = malloc(ENG_SIZE); video = calloc(1, VIDEO_SIZE);
    vram = malloc(VRAM_SIZE); pal = malloc(0x400); oam = malloc(0x400); ext = malloc(0x2000);
    for (int b = 0; b < 4; b++) hires[b] = malloc(0x60000);
    if (!engA || !engB || !video || !vram || !pal || !oam || !ext || !hires[3]) { fprintf(stderr, "FAIL: no memory\n"); ut_fail++; return; }
    rndfill(engA, ENG_SIZE);                           /* the event log and the rest: random, the same in both */
    test_edges(edges);
    fprintf(stderr, "t_obj2d: video_2d_obj_affine_setup_edges %u cases\n", n_edges);
    test_frames(frames);
    fprintf(stderr, "t_obj2d: %u frames (%u with the full-screen image, %u of them with hi-res data), %u mid-frame re-sorts, "
            "%u lines (%u with OBJs), %lu OBJ-line entries\n", n_frames, n_image, n_image2x, n_resorts, n_lines, n_ret, n_entries);
}
