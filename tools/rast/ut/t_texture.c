/* t_texture.c: texture stages (uv/rgb interpolants, texture addresses, texel loads) vs src/rast/spec/texture.c.
 * run.sh t_texture.c ../../../src/rast/spec/texture.c */
#include "ut.h"
#include "spec/texture.h"

#define SPANS_SIZE 0x800
#define BUF_SIZE (64 * 1024)

typedef void (*setup_fn)(const void *, void *, uint32_t, uint32_t);
typedef void (*interp_fn)(void *, const void *, const int16_t *, uint32_t, uint32_t);
typedef void (*addr_fn)(uint32_t *, const void *, uint32_t, uint32_t, uint32_t, const uint8_t *);
typedef void (*disp_fn)(const void *, uint32_t *, const void *, uint32_t, const uint8_t *);
typedef void (*load_fn)(uint32_t *, const uint32_t *, const uint32_t *, uint32_t);
typedef void (*loadp_fn)(uint32_t *, const uint32_t *, const uint8_t *, const uint32_t *, uint64_t);

static uint8_t spans[SPANS_SIZE];
static uint8_t A[BUF_SIZE], B[BUF_SIZE];
static int16_t wts[1024];
static uint8_t maskb[1024];

static uint32_t stride_for(uint32_t count) { return (2 * count + 29) & ~15u; }

/* random span data with per-line counts; returns total pixel count */
static uint32_t make_spans(uint32_t lines) {
    rndfill(spans, sizeof spans);
    uint32_t total = 0;
    int mode = rnd(4);
    for (uint32_t i = 0; i < lines; i++) {
        uint32_t n;
        switch (mode) {
        case 0: n = rnd(13); break;                 /* small, incl. 0 */
        case 1: n = rnd(512 / lines + 1); break;
        case 2: n = 1 + rnd(40); break;
        default: n = rnd(9) == 0 ? 0 : rnd(64); break;
        }
        if (total + n > 512) n = 512 - total;
        uint16_t v = (uint16_t)n;
        memcpy(spans + 0x630 + 4 * i, &v, 2);
        total += n;
    }
    /* realistic coordinate / colour ranges part of the time */
    if (rnd(2))
        for (uint32_t i = 0; i < 44; i++) {
            int16_t uv[2] = { (int16_t)rndr(-16384, 16383), (int16_t)rndr(-16384, 16383) };
            int16_t d[2] = { (int16_t)rndr(-8192, 8192), (int16_t)rndr(-8192, 8192) };
            uint16_t rgb[2] = { (uint16_t)rnd(64 << 3), (uint16_t)rnd(64 << 3) };
            int16_t drgb[2] = { (int16_t)rndr(-512, 512), (int16_t)rndr(-512, 512) };
            memcpy(spans + 0x2c0 + 4 * i, uv, 4);
            memcpy(spans + 0x370 + 4 * i, d, 4);
            memcpy(spans + 0x420 + 4 * i, rgb, 4);
            memcpy(spans + 0x4d0 + 4 * i, drgb, 4);
            uint16_t b = (uint16_t)rnd(64 << 3); memcpy(spans + 0x580 + 4 * i + 2, &b, 2);
            int16_t db = (int16_t)rndr(-512, 512); memcpy(spans + 0x630 + 4 * i + 2, &db, 2);
        }
    return total;
}

static void make_weights(void) {
    int mode = rnd(3);
    for (int i = 0; i < 1024; i++)
        wts[i] = mode == 0 ? (int16_t)rnd64() : mode == 1 ? (int16_t)rnd(0x8000) : (int16_t)(rnd(2) ? 0x7fff : rnd(0x8001));
}

static void test_setup(const char *name, setup_fn ds, setup_fn sp, int iters) {
    for (int it = 0; it < iters; it++) {
        uint32_t lines = 1 + rnd(44);
        uint32_t total = make_spans(lines);
        uint32_t stride = rnd(4) ? stride_for(total) : stride_for(total) + 16 * rnd(64);
        rndfill(A, sizeof A); memcpy(B, A, sizeof A);
        ds(spans, A, lines, stride);
        sp(spans, B, lines, stride);
        if (ut_cmp(name, A, B, sizeof A)) { fprintf(stderr, "  lines %u total %u stride %u\n", lines, total, stride); return; }
    }
}

static void test_interp(const char *name, interp_fn ds, interp_fn sp, int iters) {
    for (int it = 0; it < iters; it++) {
        uint32_t count = rnd(8) == 0 ? rnd(9) : rnd(513);
        uint32_t stride = rnd(4) ? stride_for(count) : stride_for(count) + 16 * rnd(64);
        make_weights();
        rndfill(A, sizeof A);
        if (rnd(2)) {   /* realistic: fill via setup from random spans */
            uint32_t lines = 1 + rnd(44);
            uint32_t total = make_spans(lines);
            count = total; stride = stride_for(count);
            (name[12] == 'u' ? DS(setup_fn, 0x9a338) : DS(setup_fn, 0x9a3f0))(spans, A, lines, stride);
        }
        memcpy(B, A, sizeof A);
        int inplace = rnd(3) != 0;
        uint8_t *oa = inplace ? A : A + 0x8000, *ob = inplace ? B : B + 0x8000;
        ds(oa, A, wts, count, stride);
        sp(ob, B, wts, count, stride);
        if (ut_cmp(name, A, B, sizeof A)) { fprintf(stderr, "  count %u stride %u inplace %d\n", count, stride, inplace); return; }
    }
}

static const char *const modes[3] = { "clamp", "wrap", "flip" };

static uint32_t rand_size(void) {
    switch (rnd(6)) {
    case 0: return rnd(0x10000);
    case 1: return rnd(17);
    default: return 8u << rnd(8);
    }
}

static void make_uv(uint32_t count, uint32_t W, uint32_t H, uint8_t *buf) {
    int mode = rnd(3);
    for (uint32_t i = 0; i < count + 8; i++) {
        int16_t u, v;
        if (mode == 0) { u = (int16_t)rnd64(); v = (int16_t)rnd64(); }
        else {
            int32_t w = W ? W : 1, h = H ? H : 1;
            u = (int16_t)rndr(-3 * w, 3 * w); v = (int16_t)rndr(-3 * h, 3 * h);
        }
        memcpy(buf + 4 * i, &u, 2); memcpy(buf + 4 * i + 2, &v, 2);
    }
    int mm = rnd(3);
    for (uint32_t i = 0; i < count + 8; i++)
        maskb[i] = mm == 0 ? (uint8_t)rnd64() : mm == 1 ? 0xff : (rnd(4) ? 0xff : 0);
}

static void test_texaddr(int iters) {
    static const uint32_t offs[3][3] = {   /* [t][s] */
        { 0x9a52c, 0x9a58c, 0x9a5e4 }, { 0x9a64c, 0x9a6a4, 0x9a6f4 }, { 0x9a754, 0x9a7bc, 0x9a81c } };
    static const addr_fn ports[3][3] = {
        { spec_render_polygon_generate_texture_addresses_clamp_clamp, spec_render_polygon_generate_texture_addresses_wrap_clamp,
          spec_render_polygon_generate_texture_addresses_flip_clamp },
        { spec_render_polygon_generate_texture_addresses_clamp_wrap, spec_render_polygon_generate_texture_addresses_wrap_wrap,
          spec_render_polygon_generate_texture_addresses_flip_wrap },
        { spec_render_polygon_generate_texture_addresses_clamp_flip, spec_render_polygon_generate_texture_addresses_wrap_flip,
          spec_render_polygon_generate_texture_addresses_flip_flip } };
    char name[96];
    for (int t = 0; t < 3; t++)
        for (int s = 0; s < 3; s++) {
            snprintf(name, sizeof name, "generate_texture_addresses_%s_%s", modes[s], modes[t]);
            for (int it = 0; it < iters; it++) {
                uint32_t count = rnd(8) == 0 ? rnd(9) : rnd(513);
                uint32_t W = rand_size(), H = rand_size();
                if (rnd(8) == 0) { W |= rnd64() << 16; H |= rnd64() << 16; }   /* garbage upper bits */
                rndfill(A, 0x2000);
                make_uv(count, W & 0xffff, H & 0xffff, A);
                memcpy(B, A, 0x2000);
                int inplace = rnd(2);
                DS(addr_fn, offs[t][s])((uint32_t *)(inplace ? A : A + 0x1000), A, count, W, H, maskb);
                ports[t][s]((uint32_t *)(inplace ? B : B + 0x1000), B, count, W, H, maskb);
                if (ut_cmp(name, A, B, 0x2000)) { fprintf(stderr, "  count %u W %x H %x\n", count, W, H); break; }
            }
        }
    /* C dispatcher with a fake polygon / texture cache entry */
    uint8_t poly[32], tex[80];
    for (int it = 0; it < iters * 4; it++) {
        rndfill(poly, sizeof poly); rndfill(tex, sizeof tex);
        void *tp = tex; memcpy(poly + 0x10, &tp, sizeof tp);
        uint16_t W = (uint16_t)rand_size(), H = (uint16_t)rand_size();
        memcpy(tex + 0x40, &W, 2); memcpy(tex + 0x42, &H, 2);
        uint32_t count = rnd(8) == 0 ? rnd(9) : rnd(513);
        rndfill(A, 0x2000);
        make_uv(count, W, H, A);
        memcpy(B, A, 0x2000);
        DS(disp_fn, 0x47b90)(poly, (uint32_t *)A, A, count, maskb);
        spec_render_polygon_generate_texture_addresses(poly, (uint32_t *)B, B, count, maskb);
        if (ut_cmp("generate_texture_addresses (dispatch)", A, B, 0x2000)) {
            fprintf(stderr, "  mode %x count %u W %x H %x\n", poly[2] & 15, count, W, H); break;
        }
    }
}

static uint32_t texels[1 << 16];
static uint8_t indices[1 << 16];
static uint32_t palette[256];

static void test_load(int iters) {
    for (int it = 0; it < iters; it++) {
        uint32_t count = rnd(8) == 0 ? rnd(9) : rnd(513);
        rndfill(texels, sizeof texels); rndfill(indices, sizeof indices); rndfill(palette, sizeof palette);
        uint32_t lim = 1u << (3 + rnd(14));
        rndfill(A, 0x2000);
        for (uint32_t i = 0; i < count + 8; i++) { uint32_t a = rnd(lim); memcpy(A + 4 * i, &a, 4); }
        memcpy(B, A, 0x2000);
        int inplace = rnd(2);
        int pal = it & 1;
        uint32_t *oa = (uint32_t *)(inplace ? A : A + 0x1000), *ob = (uint32_t *)(inplace ? B : B + 0x1000);
        if (pal) {
            DS(loadp_fn, 0x9a88c)(oa, (uint32_t *)A, indices, palette, (uint64_t)count);
            spec_render_polygon_load_texels_paletted(ob, (uint32_t *)B, indices, palette, count);
        } else {
            DS(load_fn, 0x9a8fc)(oa, (uint32_t *)A, texels, count);
            spec_render_polygon_load_texels(ob, (uint32_t *)B, texels, count);
        }
        if (ut_cmp(pal ? "load_texels_paletted" : "load_texels", A, B, 0x2000)) { fprintf(stderr, "  count %u\n", count); return; }
    }
}

void ut_main(void) {
    int n = 4000;
    test_setup("setup_uv_interpolants", DS(setup_fn, 0x9a338), spec_render_polygon_setup_uv_interpolants, n);
    test_setup("setup_rgb_interpolants", DS(setup_fn, 0x9a3f0), spec_render_polygon_setup_rgb_interpolants, n);
    test_interp("interpolate_uv", DS(interp_fn, 0x9a3a0), spec_render_polygon_interpolate_uv, n);
    test_interp("interpolate_rgb", DS(interp_fn, 0x9a4a0), spec_render_polygon_interpolate_rgb, n);
    test_texaddr(3000);
    test_load(2 * n);
}
