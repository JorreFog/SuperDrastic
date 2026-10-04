/* composite.c: exact C ports of DraStic r2.5.2.2's 3D visibility step of the 2D scanline compositor. Verified
 * bit-exact against the originals by tools/rast/ut/t_composite.c (the 32 output bytes plus guard bytes around them,
 * the return value; 256-byte alpha array plus guards for the gather).
 *
 * ===================================================================================================================
 * WHERE IT RUNS
 * ===================================================================================================================
 * render_scanline_2d (0x3ef00) composites a DS line of engine A (the screen with the 3D layer as BG0). In the hi-res
 * path it runs once per quarter q = 0..3 of the line (two output rows x the even / odd pixels), with
 *      px = render_scanline_3d(sys, line) + q * 0x400       (frame + line*0x1000 + q*0x400, 256 pixels)
 * or a BG0HOFS-shifted copy of that quarter on its stack; in the 1x path once per line with the 1x line or the
 * downsampled line. The 3D frame's pixel format is r6 | g6<<8 | b6<<16 | a<<24 with a = the 5-bit alpha (0 =
 * transparent: the layer below shows; 31 = opaque; 1..30 = translucent, blended with the layer below).
 *     flags_q = flags_line | render_scanline_set_3d_visibility(S + 0xda0, px)
 * The bitmap at S+0xda0 is BG0's visibility for the priority encoder; flag 2 (bit 1) sends the quarter down the
 * complex (blending) path; 0x10 says the 3D layer shows somewhere, opaque everywhere it shows.
 *
 * ===================================================================================================================
 * render_scanline_gather_3d_alpha_asm(u8 a[256], const u32 px[256])  (0xa0a48, hand-written NEON)
 * ===================================================================================================================
 * Four ld4.16b per 64 pixels (the 4th register holds byte 3 of 16 consecutive pixels), stored in order:
 *      a[i] = px[i] >> 24          for i = 0..255; writes exactly a[0..255], reads exactly px[0..255].
 *
 * ===================================================================================================================
 * render_scanline_set_3d_visibility(u8 bits[32], const u32 px[256]) -> u32  (0x3c2c0, C with NEON intrinsics)
 * ===================================================================================================================
 * Gathers the 256 alpha bytes into a stack array (above), then folds them into bits, 128 pixels per iteration:
 * ld2.4s splits the alpha words into even / odd 4-byte groups; per byte, nz(x) is formed as (x | x >> 4) & 0x0f
 * (even group) and (x | x << 4) & 0xf0 (odd group), folded with >> 2 and >> 1 and masked with 0x11, so byte k of a
 * word holds bit 0 = (alpha[8j+k] != 0) and bit 4 = (alpha[8j+4+k] != 0); >> 7 and >> 14 folds and two xtn
 * narrowings then pack the 8 bits of pixels 8j..8j+7 into bitmap byte j. The same folds run on alpha ^ 0x1f (non-zero
 * where alpha != 31) and are ANDed with the visibility folds before an OR accumulation: the fold maps every bit
 * position of a word to the same pixel in both, so the AND is exact per pixel. Result, for any byte values:
 *      bits[j] bit b = (alpha[8j + b] != 0)                 j = 0..31, b = 0..7 (LSB first); all 32 bytes written
 *      return 2    if some pixel has alpha not in {0, 0x1f} (alpha 0x20..0xff counts as translucent too),
 *             0x10 else if some pixel has alpha != 0 (then every such pixel has alpha 0x1f),
 *             0    else (no pixel visible).
 * Bytes 0..2 of the pixels are never looked at. No other memory is written (the gather array is the original's
 * own stack frame). */
#include <string.h>
#include "composite.h"

void spec_render_scanline_gather_3d_alpha(uint8_t a[256], const uint32_t px[256]) {
    for (int i = 0; i < 256; i++) a[i] = (uint8_t)(px[i] >> 24);
}

uint32_t spec_render_scanline_set_3d_visibility(uint8_t bits[32], const uint32_t px[256]) {
    uint8_t a[256];
    spec_render_scanline_gather_3d_alpha(a, px);
    int any = 0, transl = 0;
    memset(bits, 0, 32);
    for (int i = 0; i < 256; i++) {
        if (!a[i]) continue;
        bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        any = 1;
        transl |= a[i] != 0x1f;
    }
    return transl ? 2 : any ? 0x10 : 0;
}
