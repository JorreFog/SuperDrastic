/* obj.h: exact C ports of DraStic r2.5.2.2's 2D sprite (OBJ) path: the per-frame OBJ tables (video_2d_reorder_obj),
 * the affine span setup (video_2d_obj_affine_setup_edges) and the per-line renderer (render_scanline_obj_c). obj.c
 * documents them; the analysis is tools/rast/re2d/obj.md. */
#ifndef SPEC_2D_OBJ_H
#define SPEC_2D_OBJ_H
#include <stdint.h>

/* The OBJ tables in the engine struct (eng + offset), written by video_2d_reorder_obj only. */
enum {
    OBJT_RECORDS = 0x380,     /* 128 records of OBJE_SIZE bytes, indexed by OAM number */
    OBJT_LISTS = 0x2f80,      /* u8 [5][192][128]: list l (0..3 = priority 0..3, 4 = OBJ window) of line y: OBJ numbers
                               * in increasing OAM order, at OBJT_LISTS + l*0x6000 + y*0x80 + k */
    OBJT_COUNTS = 0x20f80,    /* u8 [5][192]: entries of list l on line y, at OBJT_COUNTS + l*0xc0 + y */
    OBJT_LINEFLAGS = 0x21340, /* u8 [192]: bit 0 a semi-transparent OBJ is registered on the line, bit 1 a bitmap OBJ */
    OBJT_IMAGE = 0x21400,     /* u16 *: the 12-sprite full-screen bitmap image in the VRAM alias, or NULL */
    OBJT_IMAGE2X = 0x21408,   /* u16 *: its hi-res capture data (3 x 256 u16 a line), or NULL */
    OBJT_IMAGEPRIO = 0x21410, /* u8: its priority */
    OBJT_END = 0x21418        /* the event log follows */
};
/* An OBJ record E = eng + OBJT_RECORDS + i*OBJE_SIZE. */
enum {
    OBJE_XS = 0x00,     /* s64: affine X span: start, length, per-line step (32.32 box pixels) */
    OBJE_XLEN = 0x08,
    OBJE_XSTEP = 0x10,
    OBJE_YS = 0x18,     /* s64: affine Y span: start, length, per-line step */
    OBJE_YLEN = 0x20,
    OBJE_YSTEP = 0x28,
    OBJE_PAL = 0x30,    /* u8 *: the palette (4bpp: its 16-colour bank) */
    OBJE_VRAM = 0x38,   /* u8 *: the OBJ's first data byte in the VRAM alias, adjusted for hflip and left clipping */
    OBJE_TX0 = 0x40,    /* s16: affine texture x / y (8.8) at box pixel 0 of the centre line, after left clipping */
    OBJE_TY0 = 0x42,
    OBJE_PITCH = 0x44,  /* u16: bytes from one tile row (tiles) or pixel row (bitmaps) to the next */
    OBJE_X = 0x46,      /* s16: x of the first drawn pixel (-7..255) */
    OBJE_Y = 0x48,      /* s16: the reference line (see obj.c) */
    OBJE_PA = 0x4a,     /* s16: PA, PC, PB, PD (in that order) */
    OBJE_PC = 0x4c,
    OBJE_PB = 0x4e,
    OBJE_PD = 0x50,
    OBJE_KIND = 0x52,   /* u8: bit 0 8bpp, bit 1 bitmap, bit 2 hflip, bit 3 affine */
    OBJE_ATTR = 0x53,   /* u8: 0 normal or window, 0x80 semi-transparent, 2*alpha + 1 bitmap */
    OBJE_VFLIP = 0x54,  /* u8: vflip (non-affine) */
    OBJE_WIDTH = 0x55,  /* u8: drawn width in pixels after clipping (the box width for affine) */
    OBJE_SIZE = 0x58
};

#define DS_VIDEO_2D_REORDER_OBJ             0x3e530
#define DS_VIDEO_2D_OBJ_AFFINE_SETUP_EDGES  0x3e390
#define DS_RENDER_SCANLINE_OBJ_C            0x36b70

/* video_2d_obj_affine_setup_edges (0x3e390): one axis of an affine OBJ's span */
void spec_video_2d_obj_affine_setup_edges(int32_t t0, int32_t dA, int32_t lim, int32_t dB, int64_t *start, int64_t *step,
                                          int64_t *len);
/* video_2d_reorder_obj (0x3e530): the OBJ tables from OAM, DISPCNT and (for the full-screen image) the bank records */
void spec_video_2d_reorder_obj(uint8_t *eng);
/* render_scanline_obj_c (0x36b70): one line's colour, alpha plane, priority / window / semi / bitmap masks;
 * returns 0x10 if any list has an entry on the line */
uint32_t spec_render_scanline_obj_c(uint8_t *eng, uint16_t *col, uint8_t *alpha, uint8_t *vis, uint8_t *semi, uint8_t *bmp,
                                    uint32_t line);
#endif
