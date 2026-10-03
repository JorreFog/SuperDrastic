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

/* geometry state; buf = the render-side buffer of the double-buffered polygon/vertex RAM */
#define GEOM_SWAP_BUF   0x9ac0      /* u8: geometry-side buffer; render side is ^1 */
#define GEOM_CLRIMG_OFS 0x9aa8      /* u16: CLRIMAGE_OFFSET */
#define GEOM_VERTS      0x9ad4      /* + buf * 0x18004: vertex[] (16 bytes each) */
#define GEOM_VERTS_BUF  0x18004
#define GEOM_POLYS_OPA  0x39ae0     /* + buf * 0x10008: polygon[] (32 bytes each) */
#define GEOM_POLYS_TRL  0x59af0
#define GEOM_POLYS_BUF  0x10008
#define GEOM_TRL_COUNT  0x69af0     /* + buf * 0x10008: u32 */

#define BIN_BYTES 0x10000
#define NBINS 12
#endif
