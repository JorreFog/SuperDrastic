/* ds3d.h: DraStic r2.5.2.2 (ROCKNIX Linux aarch64 build, md5 17550db727f3b59d36b57746ad1944be) 3D renderer layout,
 * as far as the rasterizer needs it. All offsets are from the reverse engineering of video_3d_render_bins_4x and the
 * functions under it; names are ours.
 *
 * Hi-res (2x, "4x" in DraStic's names) rendering: the 512x384 frame is cut into 12 bins of 32 lines. Each render
 * thread owns a render context (0x24100 bytes) and renders bins first, first+stride, ... into its scanline buffers,
 * then resolves each bin into the output frame (0x10000 bytes per bin). */
#ifndef DS3D_H
#define DS3D_H
#include <stdint.h>

/* code offsets in the binary */
#define DS_RENDER_BINS_4X            0x58890
#define DS_RENDER_POLYGON_4X         0x53ef0
#define DS_RENDER_POLYGON_SETUP_4X   0x4bc80
#define DS_BUILD_PIXEL_EMBEDDED_A    0x674b0
#define DS_RESOLVE_BIN_ASM_4X        0x9ce78
#define DS_RESOLVE_FOG_FULL_4X       0x57c70
#define DS_RESOLVE_FOG_ALPHA_4X      0x58280
#define DS_RESOLVE_EDGE_4X           0x56420
#define DS_RESOLVE_EDGE_FOG_FULL_4X  0x57d90
#define DS_RESOLVE_EDGE_FOG_ALPHA_4X 0x583a0

/* render context (per render thread) */
#define CTX_COLOR      0x00000      /* u32 [32][512]: r6 | g6<<8 | b6<<16 | a5<<24 | fog<<31 */
#define CTX_ATTR       0x10000      /* u32 [32][512]: attributes (depth<<9 ...) */
#define CTX_IDBUF      0x20000      /* u8  [32][512]: translucent polygon ids */
#define CTX_SYS        0x24000      /* u64: the emulator's system struct */
#define CTX_GEOM       0x24008      /* u64: geometry/render state */
#define CTX_LINEMASK   0x24010      /* u64: per-line flags (set to 0xffffffff per bin) */
#define CTX_FOGUSED    0x24014
#define CTX_FIRST_BIN  0x240e2      /* u8 */
#define CTX_BIN_STRIDE 0x240e3      /* u8: render threads */
#define CTX_NO_EDGE    0x240e4      /* u8: skip edge marking */
#define CTX_SIZE       0x24100

/* system struct */
#define SYS_DISP3DCNT   0x34eb40
#define SYS_CLEAR_COLOR 0x34eb48    /* in scanline format */
#define SYS_CLEAR_ATTR  0x34eb4c
#define SYS_OUTPUT      0x34eb58    /* u64: output frame, 12 bins x 0x10000 */
#define SYS_CLRIMG_COL  0x2190      /* u64: rear-plane colour (texture slot 2), or 0 */
#define SYS_CLRIMG_DEP  0x2198      /* u64: rear-plane depth (texture slot 3), or 0 */
#define SYS_BINS_OPAQUE 0x2856c0    /* [12] { u16 poly[0x800]; u32 count; } */
#define SYS_BINS_TRANSL 0x2916f0
#define BIN_LIST_SIZE   0x1004
/* the edge marking gap buffers, slot g = the boundary between bins g and g + 1 (spec/resolve.c): attributes 11 x 0x2000
 * (lines 30, 31 of bin g, lines 0, 1 of bin g + 1), colours 11 x 0x1000 (line 31 of bin g, line 0 of bin g + 1) */
#define SYS_ATTR_GAPS   0x32db40
#define SYS_COLOR_GAPS  0x343b40

/* geometry state; buf = the render-side buffer of the double-buffered polygon/vertex RAM */
#define GEOM_SWAP_BUF   0x9ac0      /* u8: geometry-side buffer; render side is ^1 */
#define GEOM_SORT_MODE  0x9acc      /* u8: bit 0 = translucent polygons in their own order (SWAP_BUFFERS bit 0), not y-sorted */
/* the geometry side's clip-space vertex arrays and viewport (geometry_perspective_apply_hires_asm's inputs) */
#define GEOM_VTX_COUNT  0x64c       /* u32 */
#define GEOM_CLIP_X     0x17f0      /* s32[1568] clip x (screen x after the transform) */
#define GEOM_CLIP_Y     0x3070      /* s32[1568] */
#define GEOM_CLIP_W     0x6170      /* s32[1568] */
#define GEOM_VIEWPORT   0x9ab6      /* u16 width, height, x1, y1 */
#define DS_PERSP_APPLY_HIRES 0x9e648    /* geometry_perspective_apply_hires_asm(geom, recips, shifts) */
#define DS_VERTEX_ORDERS 0x11df90       /* u32[128]: vertex walk orders, nibbles; entry count*8 + top vertex */
#define GEOM_CLRIMG_OFS 0x9aa8      /* u16: CLRIMAGE_OFFSET */
#define GEOM_VERTS      0x9ad4      /* + buf * 0x18004: vertex[] (16 bytes each) */
#define GEOM_VERTS_BUF  0x18004
#define GEOM_POLYS_OPA  0x39ae0     /* + buf * 0x10008: polygon[] (32 bytes each) */
#define GEOM_POLYS_TRL  0x59af0
#define GEOM_POLYS_BUF  0x10008
#define GEOM_TRL_COUNT  0x69af0     /* + buf * 0x10008: u32 */

#define BIN_BYTES 0x10000
#define NBINS 12

/* the output frames, as the 2D compositor reads them (comp.c). update_frame_3d_4x(sys, skip) picks the frame to
 * write (SYS_OUTPUT): with threaded_3d the one that is not published, else the current one. It renders the bins into
 * it, then (edge marking on) re-marks the rows 32k-1 and 32k of each bin boundary (the gap passes, spec/resolve.c);
 * or, when it does not render (no new geometry), with threaded_3d copies the last rendered frame into it.
 * render_scanline_3d returns SYS_OUTPUT + line*0x1000 (SYS_PUBLISHED with threaded_3d). */
#define SYS_CFG         0x8         /* u64: configuration */
#define CFG_THREADED_3D 0x468       /* u32 */
#define SYS_FRAMEBUF    0x1056c0    /* two output frames of NBINS * BIN_BYTES (reset_video_3d clears both) */
#define SYS_PUBLISHED   0x34eb60    /* u64: the frame the compositor reads with threaded_3d */
#define SYS_LAST        0x34eb68    /* u64: the frame last rendered (the source of the copy) */
#define DS_SET_3D_VISIBILITY  0x3c2c0   /* render_scanline_set_3d_visibility(u8 bits[32], const u32 px[256]) */
#define DS_UPDATE_FRAME_3D_4X 0x58f10   /* update_frame_3d_4x(sys, u32 skip) */
#define DS_UPDATE_FRAME_3D_1X 0x52870   /* update_frame_3d_1x(sys, u32 skip) */
#define DS_RESET_VIDEO_3D     0x59a60   /* reset_video_3d(sys + SYS_FRAMEBUF) */

/* the 2D compositor's quarter (comp.c, spec/composite.c): render_scanline_2d_composite(eng, out, S, layers, p3d, alpha,
 * lmask, bldcnt, flags, line) and the two routines its simple path calls. S = render_scanline_2d's scratch area
 * (its stack frame + 0x180; the frame is 0x1d30 bytes). */
#define DS_2D_COMPOSITE       0x3c6d0
#define DS_PRIORITY_ENCODE_SINGLE 0x9ffc0   /* (eng, S + S_VIS, S + S_EXCL); clobbers x3-x7, v0-v7, v16, v17 only */
#define DS_SELECT_PIXELS      0x39330   /* (eng, out, excl, layers, p3d, alpha, lmask) */
#define ENG_BACKDROP    0x18        /* u64: pointer to the backdrop colour (u16 BGR555) */
#define S_VIS           0xda0       /* u8 [8][32]: visibility bitmaps, BG0..BG3, OBJ by priority */
#define S_EXCL          0x10c0      /* u8 [6][32]: the priority encoder's masks, BG0..BG3, OBJ, backdrop */
#define S_FRAME_BELOW   0x180       /* render_scanline_2d's frame: S - 0x180 .. S + 0x1bb0 */
#define S_FRAME_SIZE    0x1d30
#endif
