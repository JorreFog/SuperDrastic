/* bg.h: exact C ports of DraStic r2.5.2.2's BG layer renderers (2D engines A and B): render_scanline_bg with its
 * mosaic, the text / affine / extended affine / bitmap renderers, the affine clip edges, and the blank-layer test the
 * compositor runs on their bitmaps. bg.c documents every routine; the analysis is tools/rast/re2d/bg.md. */
#ifndef SPEC_2D_BG_H
#define SPEC_2D_BG_H
#include <stdint.h>

/* The layer struct L(n) = eng + 0xc0 + n*0xb0 (n = 0..3): byte offsets of the fields the renderers read and write. */
enum {
    BGL_ENG = 0x00,      /* u8 *: the engine struct */
    BGL_VRAM = 0x08,     /* u8 *: DraStic's linear VRAM alias (DS address - 0x06000000) */
    BGL_PAL = 0x10,      /* u16 *: the BG palette (256 entries used) */
    BGL_EXTPAL = 0x18,   /* u16 *: the layer's ext palette slot (16 x 256), or NULL */
    BGL_DIRECT = 0x20,   /* u16 *: direct 16-bit bitmap rows (BG2/BG3, per frame), or NULL: the layer is not rendered */
    BGL_DIRECT2X = 0x28, /* its hi-res capture data, or NULL */
    BGL_FN = 0x30,       /* the renderer, DraStic's code address (see spec_bg_renderer) */
    BGL_MAP = 0x38,      /* u32: map base, VRAM offset (DISPCNT screen base + BGnCNT[12:8] << 11) */
    BGL_CHR = 0x3c,      /* u32: char base, VRAM offset (DISPCNT char base + BGnCNT[5:2] << 14) */
    BGL_BMP = 0x40,      /* u32: bitmap base BGnCNT[12:8] << 14 (0 in mode 6) */
    BGL_SCRRAW = 0x44,   /* u32: BGnCNT[12:8] << 11 */
    BGL_CHRRAW = 0x48,   /* u32: BGnCNT[5:2] << 14 */
    BGL_RECA = 0x4c,     /* u32: ceil(2^31 / |PA|), the X tile-crossing spacing (unchanged while PA = 0) */
    BGL_RECC = 0x50,     /* u32: ceil(2^31 / |PC|) */
    BGL_XS = 0x58,       /* s64: X clip edge, 32.32 screen x: start (stepped per clip line), */
    BGL_XW = 0x60,       /*      width, */
    BGL_XD = 0x68,       /*      step per line */
    BGL_YS = 0x70,       /* s64: Y clip edge: start, width, step */
    BGL_YW = 0x78,
    BGL_YD = 0x80,
    BGL_REFX = 0x88,     /* s32: BGnX / BGnY references (sign-extended 28 bits) */
    BGL_REFY = 0x8c,
    BGL_CURX = 0x90,     /* s32: the current X / Y (line 0: the references; then += PB / PD per line) */
    BGL_CURY = 0x94,
    BGL_CNT = 0x98,      /* u16: BGnCNT */
    BGL_HOFS = 0x9a,     /* u16: HOFS / VOFS (& 0x1ff) */
    BGL_VOFS = 0x9c,
    BGL_PA = 0x9e,       /* s16: PA, PC, PB, PD (in that order) */
    BGL_PC = 0xa0,
    BGL_PB = 0xa2,
    BGL_PD = 0xa4,
    BGL_WMASK = 0xa6,    /* u16: bitmap width - 1 / height - 1 (BG2/BG3) */
    BGL_HMASK = 0xa8,
    BGL_LOG2W = 0xaa,    /* u8: log2 of the bitmap width */
    BGL_TMASK = 0xab,    /* u8: affine map width in tiles - 1 (15, 31, 63, 127) */
    BGL_LOG2T = 0xac,    /* u8: log2 of the affine map width in tiles (4..7) */
    BGL_EXTON = 0xad,    /* u8: DISPCNT.30 (ext palettes on) */
    BGL_DIRTY = 0xae     /* u8: affine parameters changed: recompute the clip edges at the next render */
};

/* DraStic's renderers (file offsets of the values video_2d_update_bg_mode stores at L+0x30) */
#define DS_RENDER_SCANLINE_TILED_EXT           0xa1d20
#define DS_RENDER_SCANLINE_AFFINE_NORMAL_EXT   0xa4730
#define DS_RENDER_SCANLINE_AFFINE_EXTENDED_EXT 0xa4b90
#define DS_RENDER_SCANLINE_BITMAP_16BPP        0x32880
#define DS_RENDER_SCANLINE_BITMAP_8BPP         0x33700
#define DS_RENDER_SCANLINE_NULL                0x31850

/* a layer renderer: (L, buf = the u16 line, vis = its 32-byte visibility bitmap, line) */
typedef void (*spec_bg_fn)(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);

/* render_scanline_tiled_ext (0xa1d20): text BGs, 4bpp / 8bpp / 8bpp with ext palettes */
void spec_render_scanline_tiled_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);
/* render_scanline_affine_normal_ext (0xa4730): affine BGs, 8-bit map entries */
void spec_render_scanline_affine_normal_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);
/* render_scanline_affine_extended_ext (0xa4b90): extended affine BGs, 16-bit map entries */
void spec_render_scanline_affine_extended_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);
/* render_scanline_bitmap_16bpp (0x32880) / render_scanline_bitmap_8bpp (0x33700, also mode 6's large bitmap) */
void spec_render_scanline_bitmap_16bpp(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);
void spec_render_scanline_bitmap_8bpp(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line);
/* render_scanline_update_affine_variables (0x34550): the clip edges and tile spacings from the current state */
void spec_render_scanline_update_affine_variables(uint8_t *L);
/* video_2d_bg_layer_affine_setup_edges (0x326f0): one axis' 32.32 clip edge */
void spec_video_2d_bg_layer_affine_setup_edges(int32_t X, int32_t P, int32_t W, int32_t Q, int64_t *start, int64_t *step,
                                               int64_t *width);
/* the port of the renderer at file offset off (L+0x30 minus DraStic's load address); NULL for render_scanline_null
 * (0x31850, which returns at once) and for any other value */
spec_bg_fn spec_bg_renderer(uintptr_t off);
/* render_scanline_bg (0x36880): every BG of the render list into lines + n*0x220 + 0x10 / vis + n*32, with mosaic;
 * ds_base = DraStic's load address (L+0x30 holds DraStic's renderer) */
void spec_render_scanline_bg(uint8_t *eng, uint8_t *lines, uint8_t *vis, uint32_t line, uintptr_t ds_base);
/* render_scanline_disable_blank_layers_asm (0xa089c): drop the BGs with an empty bitmap from *lmask */
void spec_render_scanline_disable_blank_layers(const uint8_t *vis, uint32_t *lmask);
#endif
