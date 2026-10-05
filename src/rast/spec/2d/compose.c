/* compose.c: exact C ports of DraStic r2.5.2.2's 2D compositor (key "compose", tools/rast/re2d/compose.md): windows,
 * colour effects, every path of render_scanline_2d_composite with everything under it, the 3D layer's horizontal
 * shift, display capture and the 32-bit scanout conversion, on DraStic's own engine struct and scratch layout. One
 * function per DraStic routine, same arguments, same memory writes, including the stale-scratch behaviour (the masks
 * of unlisted layers, the second planes under EVB = 0). Verified against the originals in DraStic's process by
 * tools/rast/ut/t_compose2d.c (every output between guard bytes, the scratch area and the engine struct after every
 * call; the closed form of pixel.h against the original composite) and t_composite.c (the simple path's helpers one
 * by one); during the analysis also in the running emulator: 3.2 M composites, 2.4 M window mask calls and 9.9 M
 * conversions of the ds2d scenes, 0 differences.
 *
 * ===================================================================================================================
 * WHERE IT RUNS
 * ===================================================================================================================
 * render_scanline_2d (0x3ef00), per DS line of an engine, after the BG and OBJ renderers (bg.c, obj.c):
 *   generate_window_masks(eng, S+0xf00, S+0xfa0, objwin = S+0xea0, lmask, line)
 *   lmask = DISPCNT[11:8] | 0x10 (OBJ on the line, or the OBJ image); bm = BLDCNT & (lmask | lmask << 8 | 0xf0f0)
 *   flags = the OBJ line flags (bit 0 a semi-transparent OBJ, bit 1 a bitmap OBJ); BLDCNT mode 1 with a 1st and a 2nd
 *       target in bm: | 4; mode 2/3 with a 1st target and BLDY != 0: | 8; no 2nd target in bm: & ~1;
 *       | set_3d_visibility(..) (2: a translucent 3D pixel, 0x10: the 3D layer shows, all opaque); | 0x20 (OBJ image)
 *   1x: apply_windows, disable_blank_layers, render_scanline_2d_composite(eng, planes, S, layers, p3d, alpha, lmask,
 *       bm, flags, line) with alpha = flags & 2 ? S+0xc90 : NULL
 *   2x (a hi-res layer: the 3D BG0, a direct BG2 / BG3, the OBJ image): the composite per quarter q = 0..3 into
 *       planes + q*0x300, with the quarter's 3D pixels (p3d + q*256 u32, or a BG0HOFS-shifted copy at S) and, for
 *       q = 0..2, a copy of the alpha plane at S+0xfc0
 *   the display capture of the line, then render_scanline's conversion with MASTER_BRIGHT into the scanout
 * Engine B runs the same code with no 3D layer and no capture. Everything here reads only per-line state; nothing
 * writes back into the engine but the window code (eng+0x44/0x64 masks, eng+0xb4 state, eng+0xb5 dirty bits).
 *
 * Formats. Layer lines: u16 BGR555 (r | g << 5 | b << 10, bit 15 ignored here), 256 pixels at layers[k] + 0x10 (k =
 * 0..3 BG, 4 OBJ; the table holds pointers 0x10 bytes before the first pixel). Bitmaps ("masks"): 256 bits, 32 bytes,
 * pixel i = byte i >> 3, bit i & 7 (as little-endian u32 words: word w bit b = pixel 32w + b). Planes: R6[256] at
 * out, G6 at +0x100, B6 at +0x200; a 2D colour c5 becomes c5 << 1, 3D bytes are copied as they are. 3D pixels: u32
 * r6 | g6 << 8 | b6 << 16 | a << 24 (a = 0 transparent, 31 opaque, 1..30 translucent). Engine fields: *[eng+0x18] the
 * backdrop (palette entry 0); the priority list eng+0x84 (count eng+0xb3; video_2d_reorder_layers: per priority 0..3
 * the OBJ slot 4+p if OBJ is on, then the enabled BGs of that priority); DISPCNT eng+0x90; WININ | WINOUT << 16
 * eng+0x9c (stored & 0x3f3f3f3f); BLDCNT eng+0xa0; BLDY eng+0xa2 (stored & 0x1f); BLDALPHA eng+0xa4; WIN0H / WIN1H
 * eng+0xaa / 0xac; WIN0V / WIN1V eng+0xae / 0xb0 (Y1 the high byte, Y2 the low byte). The scratch area: compose.h.
 * NEON routines work in blocks (32 pixels; setup_alpha 64, apply 16), loading a block's inputs before storing its
 * outputs; the ports keep that order, so they give the same bytes for overlapping buffers too.
 *
 * ===================================================================================================================
 * WINDOWS
 * ===================================================================================================================
 * render_scanline_update_window_mask(mask, WINxH)  (0x3a1c0): left = WINxH >> 8, right = WINxH & 0xff; right == 0:
 *   empty if left == 0, else right = 256; left == right: empty; left < right: bits [left, right); left > right: all
 *   but [right, left). Called by generate_window_masks for eng+0x44 / eng+0x64 when dirty bit 0 / 1 of eng+0xb5 is
 *   set (WIN0H / WIN1H replayed; 3 after a savestate load).
 * render_scanline_window_inhibit_masks_single / _double / _triple(inh, fx, lmask, wins.., ins.., out)  (0x3b060 /
 *   0x3ab10 / 0x3a380): the regions, in priority order, A = winA, B = winB & ~A, C = winC & ~(A|B), O = ~(A|B|C).
 *   For each region R with control bits i (a WININ / WINOUT byte, inverted: bit set = disabled): i bit 5: fx |= R;
 *   for each set bit k of (i & lmask): inh[k] |= R. So WIN0 > WIN1 > OBJ window > outside; bits 0-4 are BG0..BG3,
 *   OBJ, bit 5 the colour effects.
 * render_scanline_generate_window_masks(eng, inh, fx, objwin, lmask, line)  (0x3b360):
 *     fx = 0; en = (DISPCNT >> 13) & 7 (WIN0, WIN1, OBJ window); en == 0: return (inh untouched, no state step)
 *     inh[0..0x9f] = 0; wv = eng[0x9c] ^ 0x3f3f3f3f; dirty masks rebuilt (above)
 *     s = eng[0xb4] | (Y1_0 == line ? 5 : 4); Y2_0 == line: s &= ~1; Y1_1 == line: s |= 2; Y2_1 == line: s &= ~2
 *     eng[0xb4] = s; eng[0xb5] = 0
 *     s & en: 1, 2, 4: single(WIN0 / WIN1 / objwin); 3, 5, 6: double; 7: triple, with WINOUT's byte 0 as "outside";
 *     0 (windows enabled, none active on the line): fx = all if WINOUT bit 5; inh[k] = all for the bits of
 *     lmask & WINOUT
 *   Traps: the Y state machine (bit 0 WIN0, bit 1 WIN1, bit 2 always set here) is stepped only on lines that are
 *   rendered with a window enabled; render_scanline initialises it at line 0 (per window: set if Y1 > 191, then
 *   cleared if Y2 > 191; otherwise it keeps the previous frame's value). The OBJ window mask S+0xea0 is written only
 *   by render_scanline_obj_c (DISPCNT.12): with the OBJ window on and OBJ display off it is stale stack data.
 * render_scanline_apply_windows(eng, vis, inh, lmask)  (0x3b650): if (DISPCNT & 0xe000) == 0 return; vis[k] &= ~inh[k]
 *   for the BGs k of lmask; if lmask bit 4, the four OBJ slots vis[4..7] &= ~inh[4]. A window hides a layer by
 *   removing its pixels from the priority encoding: the layer below shows.
 *
 * ===================================================================================================================
 * render_scanline_2d_composite(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line)  (0x3c6d0, C)
 * ===================================================================================================================
 * flags & 7 == 0, the simple path:
 *     priority_encode_single(eng, S+0xda0, S+0x10c0)
 *     flags & 8 == 0: select_pixels(eng, out, S+0x10c0, layers, p3d, NULL, lmask) (a tail call; alpha is dropped)
 *     flags & 8:      select_pixels(eng, S+0x1180, ..); select_blend_enable(T = S+0x1480, excl, lmask, bldcnt & 0x3f);
 *                     T &= ~fx; shade(eng, out, S+0x1180, T)
 * Otherwise the complex path (both layers of every pixel, the coefficient planes, one apply pass for all 256):
 *     lmask2 = lmask ? lmask & (bldcnt >> 8), minus the frontmost list entry if it is a BG : 0
 *     priority_encode_double(eng, S+0xda0, TOP = S+0x13c0, SEC = S+0x1480)
 *     select_pixels(eng, P = S+0x1540, TOP, layers, p3d, flags & 8 ? NULL : alpha, lmask)       the top planes
 *     select_pixels(eng, S+0x1840, SEC, layers, p3d, NULL, lmask2)                               the second planes
 *     T1 = select_blend_enable(TOP, lmask, bldcnt & 0x3f) & ~fx;  T2 = select_blend_enable(SEC, lmask, bldcnt >> 8 & 0x3f)
 *     flags & 5: 4: M = T1 & T2;  5: M = ((SEMI & TOP.obj) | T1) & T2, f &= ~1;  1: M = SEMI & TOP.obj & T2, f |= 4
 *                (with M: f & 0x10 removes TOP.bg0 from M, f & 0x20 removes TOP.obj)
 *     f & 8 (brightness, fy = BLDY <= 16 ? 2*BLDY : 32): EVA = T1 ? 32 - fy : 32, EVB = 0, brighten (BLDCNT.6 clear)
 *                also OFF = T1 ? fy : 0; then, with f & 4, setup_blend(BLDALPHA, EVA, EVB, M) over it
 *     else f & 4: setup_blend_base(BLDALPHA, EVA, EVB, M)
 *     f & 2 and alpha: M = BMP & TOP.obj & T2, | TOP.bg0 & T2 if !BLDCNT.7 and p3d; setup_alpha(_base)(EVA, EVB, alpha, M)
 *     brighten ? apply_offset_c(out, P, EVA, EVB, OFF) : apply(out, P, EVA, EVB)
 *   (with flags & 7 == 2 and alpha == NULL nothing writes EVA / EVB and apply reads stale bytes; render_scanline_2d
 *   never passes that: alpha is non-NULL exactly when flags & 2.)
 * The output per pixel (t / s = the top / second layer's 6-bit colour; pixel.h has the whole closed form):
 *     plain t;  alpha blend min(63, (t*A + s*B + 16) >> 5) with A = min(2*EVA, 32), B = min(2*EVB + BLDALPHA.7, 32);
 *     3D / bitmap OBJ alpha min(63, (t*(a+1) + s*(31-a) + 16) >> 5);  brighten (t*(32-f) + 63*f + 16) >> 5;
 *     darken (t*(32-f) + 16) >> 5, f = min(2*BLDY, 32)
 * Traps (DraStic's behaviour, kept): BLDALPHA bit 7 leaks into EVB; EVA, EVB and EVY saturate at 16; results are
 * 6-bit, not 5-bit; opaque 3D (0x10) is never alpha-blended with EVA / EVB; translucent 3D blends with a+1 / 31-a
 * whatever BG0's 1st-target bit, but only in BLDCNT modes 0 and 1 (in modes 2 / 3 it is drawn opaque, also with
 * BLDY = 0); a semi-transparent OBJ is a 1st target by itself and in brighten mode also gets the offset when it is
 * T1; the effects window bit clears only T1 (semi OBJ, bitmap OBJ and 3D alpha still blend in an effects-disabled
 * region); the backdrop is a 1st target only for brightness and a 2nd target when exactly one layer covers the pixel;
 * with the OBJ image (0x20) OBJ-top pixels are left out of the BLDALPHA / semi blend, not out of the bitmap-OBJ alpha.
 *
 * The helpers:
 * render_scanline_priority_encode_single_asm(eng, vis, excl)  (0x9ffc0)
 *   covered = objcovered = objex = 0; for k = 0 .. eng[0xb3]-1, s = eng[0x84+k], o = s*32:
 *     BG (s & 4 == 0): excl[o..o+31] = vis[o..] & ~covered;  covered |= vis[o..]
 *     OBJ (s & 4):     v = vis[o..] & ~objcovered; objex |= v & ~covered; covered |= v; objcovered |= v
 *   then excl+0xa0 (backdrop) = ~covered, excl+0x80 (OBJ) = objex. Each covered pixel is in exactly one written mask.
 *   Only those slots are written: the excl slot of a BG that is not in the list keeps its stale bytes, and the
 *   consumers read it all the same.
 * render_scanline_priority_encode_double_asm(eng, vis, top, sec)  (0x9fe98): the same walk with cov2 (covered twice):
 *   BG k: top[k] = v & ~cov; sec[k] = v & ~cov2 & cov; cov2 |= cov & v; cov |= v.  OBJ slots: v &= ~objcov; objtop |=
 *   v & ~cov; objsec |= v & ~cov2 & cov; cov2 |= cov & v; cov |= v; objcov |= v.  Then top.bd = ~cov, sec.bd = cov &
 *   ~cov2, top.obj = objtop, sec.obj = objsec: a pixel with no layer has the backdrop on top and no second layer.
 * render_scanline_select_pixels_binary_asm(dst, src, layer, mask)  (0xa0640): dst[i] = mask bit i ? layer[i] : src[i];
 *   per 32-pixel block: with dst == src a zero mask word skips the block; with dst != src the block is always copied.
 * render_scanline_select_pixels_binary_scalar_asm(dst, src, colour, mask)  (0xa0560): the same with a constant colour.
 * render_scanline_expand_6bit_split_asm(out, c)  (0xa0818): R = (c & 0x1f) << 1, G = (c >> 5 & 0x1f) << 1,
 *   B = (c >> 10 & 0x1f) << 1.
 * render_scanline_select_pixels_binary32_asm(out, alpha, px, mask)  (0xa0730; alpha != NULL: _alpha, 0xa07ac): where
 *   the mask bit is set, R, G, B = bytes 0, 1, 2 of px[i] as they are (and alpha[i] = byte 3); every byte of out (and
 *   alpha) is rewritten, unchanged where the bit is clear.
 * render_scanline_select_pixels(eng, out, excl, layers, p3d, alpha, lmask)  (0x39330, C): tmp = u16[256] (its stack).
 *   For the set bits k of lmask, lowest first: the first layer's buffer becomes src; each later one is merged:
 *   binary(tmp, src, layer k, excl + 32k), src = tmp. Then binary_scalar(tmp, src, backdrop, excl + 0xa0) if a layer
 *   was found, else tmp = the backdrop everywhere; expand_6bit_split(out, tmp); then, if p3d and lmask & 1,
 *   binary32(out, alpha, p3d, excl). So the 3D layer counts as the first layer: its own u16 buffer shows only where
 *   no mask claims the pixel.
 * render_scanline_select_blend_enable_asm(out, excl, lmask, bits)  (0xa0070): acc = 0; for k = 0..4 in lmask: bits.k ?
 *   acc |= excl[k] : acc &= ~excl[k]; then the backdrop always: bits.5 ? acc |= excl[5] : acc &= ~excl[5]; out = acc.
 * render_scanline_shade_asm(eng, out, in, mask)  (0xa0108): f = min(2*BLDY, 32); add = BLDCNT.6 ? 16 : 63*f + 16;
 *   per channel out = mask ? (c*(32-f) + add) >> 5 : c (16-bit sums, the result truncated to 8 bits).
 * render_scanline_color_effects_setup_blend_base_asm / _blend_asm(BLDALPHA, eva, evb, mask)  (0xa0254 / 0xa02e0):
 *   w = 2*BLDALPHA, a = min(w & 0x3f, 32), b = min((w >> 8) & 0x3f, 32); base: eva = mask ? a : 32, evb = mask ? b : 0;
 *   non-base: only where mask.
 * render_scanline_color_effects_setup_alpha_base_asm / _alpha_asm(eva, evb, alpha, mask)  (0xa042c / 0xa04b4): base:
 *   a' = mask ? alpha[i] : 31, eva = a' + 1, evb = 31 - a' (u8); non-base: only where mask, from alpha[i].
 * render_scanline_color_effects_apply_asm(out, P, eva, evb)  (0xa0378): per channel s = (t*eva + s2*evb) mod 2^16,
 *   out = min(63, ((s + 16) >> 5) & 0xff); P = the top planes, then the second planes at +0x300.
 * render_scanline_color_effects_apply_offset_c(out, P, eva, evb, off)  (0x39f00, C): s = t*eva + s2*evb +
 *   ((63*off + 16) & 0xffff); out = s > 0x7ff ? 63 : s >> 5.
 *
 * ===================================================================================================================
 * render_scanline_horizontal_shift_3d(dst, src, hofs)  (0x3c630)
 * ===================================================================================================================
 * hofs = sext9(BG0HOFS), -256..255: dst[i] = src[i + hofs] inside the line, 0 (alpha 0: transparent) outside (libc
 * memcpy + memset). Once per line in the 1x path (dst = S, over BG0's line buffer; the capture reads the shifted copy
 * too), once per quarter in the 2x path (hofs DS pixels = 2*hofs output pixels: exact at 2x).
 *
 * ===================================================================================================================
 * DISPLAY CAPTURE  (after the line's composite, when C && C[0x51] && line < C[0x50])
 * ===================================================================================================================
 * dst = [C+0x28] + 2*((u32[C+0x48] + u16[C+0x4c]*line) & 0xffff); width = u16[C+0x4c] (128 or 256); per quarter at 2x
 * (q = 1..3 into the hi-res buffer [C+0x30] + 6*off + (q-1)*0x200, then q = 0 into VRAM), source B = [C+0x40].
 * render_scanline_capture_direct_asm(C, dst, planes)  (0xa0910): dst[i] = ((G >> 1) << 5) + (R >> 1) | 0x8000 |
 *   (B >> 1) << 10 (u16), in 32-pixel steps.
 * render_scanline_capture_direct_3d_asm(C, dst, px)  (0xa09b0): ((g6 >> 1) << 5) + (r6 >> 1) | ((b6 >> 1) & 0x1f) << 10
 *   | (s8(a) > 0) << 15.
 * render_scanline_capture_blended / _blended_3d(C, dst, srcB, src)  (0x3bc60 / 0x3bdb0, C): eva = C[0x54], evb2 =
 *   2*C[0x55] (both from 0..16); eva == 16 && evb2 == 0: the direct routine. No source B ([C+0x40] == NULL for the 2D
 *   version, srcB == NULL for the 3D one): c = (A6*eva) >> 5 per channel, dst = 0x8000 | R | G << 5 | (B & 0x3f) << 10.
 *   Else v = A6*eva + B5*evb2 per channel (B5 from srcB's BGR555), c = v < 0x400 ? v >> 5 : 31, dst = 0x8000 | ...
 *   Trap: bit 15 is always set when blending, also for 3D pixels of alpha 0 (only the direct 3D capture tests alpha).
 *
 * ===================================================================================================================
 * THE 32-BIT SCANOUT  (render_scanline 0x4067c: MB = MASTER_BRIGHT eng+0xa6, m = MB >> 14, f = (MB & 0x1f) << 1)
 * ===================================================================================================================
 * m = 0 or 3, or f = 0: direct; m = 1 (brighten), f <= 31: shade(32 - f, 63f + 16); m = 2 (darken): shade(32 - f, 16);
 * f > 31: memset white (0xff) / black. At 2x: (q0, q1) -> row 2L, (q2, q3) -> row 2L + 1 (pitch / 2 further).
 * render_scanline_color_convert_direct_32_1x_asm(planes, dst)  (0xa0a80): dst[i] = B<<2 | (G<<2) << 8 | (R<<2) << 16 |
 *   0xff << 24 (each << 2 in 8 bits).
 * render_scanline_color_convert_direct_32_2x_asm(e, o, dst)  (0xa0ae0): dst[2i] from e[i], dst[2i+1] from o[i]; byte 3
 *   = 0xff where (x & 15) is 0, 2, 4 or 6, else 0 (x = the output pixel: a constant register pattern).
 * render_scanline_color_convert_shade_32_1x_asm(planes, dst, factor, add)  (0xa0c90): c' = (u8)(((c*factor + add) mod
 *   2^16) >> 5) << 2 (factor as a byte, add as a u16); byte 3 = 0xff.
 * render_scanline_color_convert_shade_32_2x_asm(e, o, dst, factor, add)  (0xa0d80): c' = (u8)(((((c << 2) & 0xff) *
 *   factor + add) mod 2^16) >> 5): the << 2 comes FIRST. That is DraStic's 2x brighten bug: with add = 63f + 16 the
 *   brightening term is a quarter of what it should be, bright colours get darker (a fade to white goes through dark
 *   grey, then jumps to white at factor 16); darken is right (and finer than 1x). Byte 3 of output pixel x (k = x & 31,
 *   16 input pixels per iteration n = x >> 5) = byte (k & 15) of 8 u16 accumulators: k < 16: ACC(B of e, input pixel
 *   16n + 8 + (k >> 1)), k >= 16: ACC(G of o, input pixel 16n + ((k - 16) >> 1)) (registers reused). */
#include <string.h>
#include "compose.h"

#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
static inline int mbit(const uint8_t *m, int i) { return m[i >> 3] >> (i & 7) & 1; }

/* ======================================== windows ======================================== */

void spec_render_scanline_update_window_mask(uint8_t *mask, uint32_t winh) {
    uint32_t w[8];
    memcpy(w, mask, 32);
    uint32_t left = (winh >> 8) & 0xff, right = winh & 0xff;
    if (right == 0) {
        if (left == 0) { memset(mask, 0, 32); return; }
        right = 256;
    } else if (left == right) { memset(mask, 0, 32); return; }
    if (left > right) {                                    /* wraps: all but [right, left) */
        for (int i = 0; i < 8; i++) w[i] = ~0u;
        uint32_t lo = right >> 5, hi = (left - 1) >> 5;
        uint32_t keep_lo = ~(~0u << (right & 31)), keep_hi = 0xfffffffeu << ((left - 1) & 31);
        if (lo == hi) w[lo] &= keep_lo | keep_hi;
        else {
            w[lo] &= keep_lo;
            for (uint32_t i = lo + 1; i < hi; i++) w[i] = 0;
            w[hi] &= keep_hi;
        }
    } else {                                               /* [left, right) */
        for (int i = 0; i < 8; i++) w[i] = 0;
        uint32_t lo = left >> 5, hi = (right - 1) >> 5;
        uint32_t set_lo = ~0u << (left & 31), set_hi = ~(0xfffffffeu << ((right - 1) & 31));
        if (lo == hi) w[lo] |= set_lo & set_hi;
        else {
            w[lo] |= set_lo;
            for (uint32_t i = lo + 1; i < hi; i++) w[i] = ~0u;
            w[hi] |= set_hi;
        }
    }
    memcpy(mask, w, 32);
}

static void or_region(uint8_t *dst, const uint8_t *r) { for (int j = 0; j < 32; j++) dst[j] |= r[j]; }
/* fx |= region if bit 5 of the control bits; inh[k] |= region for every set bit k of (bits & lmask) */
static void inhibit_region(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *r, uint32_t bits) {
    if (bits >> 5 & 1) or_region(fx, r);
    uint32_t m = bits & lmask;
    for (unsigned k = 0; m; k++, m >>= 1) if (m & 1) or_region(inh + 32 * k, r);
}

void spec_render_scanline_window_inhibit_masks_single(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *w,
                                                      uint32_t win_inh, uint32_t out_inh) {
    uint8_t in[32], out[32];
    for (int j = 0; j < 32; j++) { in[j] = w[j]; out[j] = (uint8_t)~w[j]; }
    inhibit_region(inh, fx, lmask, in, win_inh);
    inhibit_region(inh, fx, lmask, out, out_inh);
}

void spec_render_scanline_window_inhibit_masks_double(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *a,
                                                      const uint8_t *b, uint32_t ia, uint32_t ib, uint32_t out_inh) {
    uint8_t ra[32], rb[32], ro[32];
    for (int j = 0; j < 32; j++) { ra[j] = a[j]; rb[j] = b[j] & (uint8_t)~a[j]; ro[j] = (uint8_t)~(a[j] | b[j]); }
    inhibit_region(inh, fx, lmask, ra, ia);
    inhibit_region(inh, fx, lmask, rb, ib);
    inhibit_region(inh, fx, lmask, ro, out_inh);
}

void spec_render_scanline_window_inhibit_masks_triple(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *a,
                                                      const uint8_t *b, const uint8_t *c, uint32_t ia, uint32_t ib,
                                                      uint32_t ic, uint32_t out_inh) {
    uint8_t ra[32], rb[32], rc[32], ro[32];
    for (int j = 0; j < 32; j++) {
        uint8_t ab = a[j] | b[j];
        ra[j] = a[j]; rb[j] = b[j] & (uint8_t)~a[j]; rc[j] = c[j] & (uint8_t)~ab; ro[j] = (uint8_t)~(c[j] | ab);
    }
    inhibit_region(inh, fx, lmask, ra, ia);
    inhibit_region(inh, fx, lmask, rb, ib);
    inhibit_region(inh, fx, lmask, rc, ic);
    inhibit_region(inh, fx, lmask, ro, out_inh);
}

void spec_render_scanline_generate_window_masks(uint8_t *eng, uint8_t *inh, uint8_t *fx, const uint8_t *objwin,
                                                uint32_t lmask, uint32_t line) {
    memset(fx, 0, 32);
    uint32_t en = (U32(eng, 0x90) >> 13) & 7;
    if (!en) return;
    uint32_t y1_0 = eng[0xaf], y2_0 = eng[0xae], y1_1 = eng[0xb1], y2_1 = eng[0xb0];
    uint32_t state = eng[0xb4], dirty = eng[0xb5];
    memset(inh, 0, 0xa0);
    uint32_t wv = U32(eng, 0x9c) ^ 0x3f3f3f3f;
    if (dirty & 1) spec_render_scanline_update_window_mask(eng + 0x44, U16(eng, 0xaa));
    if (dirty & 2) spec_render_scanline_update_window_mask(eng + 0x64, U16(eng, 0xac));
    state |= y1_0 == line ? 5 : 4;
    if (y2_0 == line) state &= ~1u;
    if (y1_1 == line) state |= 2;
    if (y2_1 == line) state &= ~2u;
    eng[0xb4] = (uint8_t)state;
    eng[0xb5] = 0;
    uint32_t out = wv >> 16;
    uint8_t *w0 = eng + 0x44, *w1 = eng + 0x64;
    switch (state & en) {
    case 1: spec_render_scanline_window_inhibit_masks_single(inh, fx, lmask, w0, wv, out); break;
    case 2: spec_render_scanline_window_inhibit_masks_single(inh, fx, lmask, w1, wv >> 8, out); break;
    case 4: spec_render_scanline_window_inhibit_masks_single(inh, fx, lmask, objwin, wv >> 24, out); break;
    case 3: spec_render_scanline_window_inhibit_masks_double(inh, fx, lmask, w0, w1, wv, wv >> 8, out); break;
    case 5: spec_render_scanline_window_inhibit_masks_double(inh, fx, lmask, w0, objwin, wv, wv >> 24, out); break;
    case 6: spec_render_scanline_window_inhibit_masks_double(inh, fx, lmask, w1, objwin, wv >> 8, wv >> 24, out); break;
    case 7: spec_render_scanline_window_inhibit_masks_triple(inh, fx, lmask, w0, w1, objwin, wv, wv >> 8, wv >> 24, out); break;
    default: {                                             /* windows enabled, none active on this line: all "outside" */
        if (out >> 5 & 1) memset(fx, 0xff, 32);
        uint32_t m = lmask & out;
        for (unsigned k = 0; m; k++, m >>= 1) if (m & 1) memset(inh + 32 * k, 0xff, 32);
    }
    }
}

void spec_render_scanline_apply_windows(const uint8_t *eng, uint8_t *vis, const uint8_t *inh, uint32_t lmask) {
    if (!(U32(eng, 0x90) & 0xe000)) return;
    for (int k = 0; k < 4; k++)
        if (lmask >> k & 1) for (int j = 0; j < 32; j++) vis[32 * k + j] &= (uint8_t)~inh[32 * k + j];
    if (lmask >> 4 & 1)
        for (int s = 4; s < 8; s++) for (int j = 0; j < 32; j++) vis[32 * s + j] &= (uint8_t)~inh[0x80 + j];
}

/* ======================================== the encoders and layer selection ======================================== */

void spec_render_scanline_priority_encode_single(const uint8_t *eng, const uint8_t *vis, uint8_t *excl) {
    uint8_t cov[32] = { 0 }, objcov[32] = { 0 }, objex[32] = { 0 };
    unsigned n = eng[0xb3];
    for (unsigned k = 0; k < n; k++) {
        uint32_t o = (uint32_t)eng[0x84 + k] << 5;
        uint8_t v[32];
        memcpy(v, vis + o, 32);
        if (o & 0x80) {                                     /* OBJ slot (4..7): merged into the one OBJ mask */
            for (int j = 0; j < 32; j++) {
                v[j] &= (uint8_t)~objcov[j];
                objex[j] |= v[j] & (uint8_t)~cov[j];
                cov[j] |= v[j];
                objcov[j] |= v[j];
            }
        } else {
            uint8_t e[32];
            for (int j = 0; j < 32; j++) e[j] = v[j] & (uint8_t)~cov[j];
            memcpy(excl + o, e, 32);
            for (int j = 0; j < 32; j++) cov[j] |= v[j];
        }
    }
    uint8_t bd[32];
    for (int j = 0; j < 32; j++) bd[j] = (uint8_t)~cov[j];
    memcpy(excl + 0xa0, bd, 32);
    memcpy(excl + 0x80, objex, 32);
}

void spec_render_scanline_priority_encode_double(const uint8_t *eng, const uint8_t *vis, uint8_t *top, uint8_t *sec) {
    uint8_t cov[32] = { 0 }, cov2[32] = { 0 }, objcov[32] = { 0 }, objtop[32] = { 0 }, objsec[32] = { 0 };
    unsigned n = eng[0xb3];
    for (unsigned k = 0; k < n; k++) {
        uint32_t o = (uint32_t)eng[0x84 + k] << 5;
        uint8_t v[32];
        memcpy(v, vis + o, 32);
        if (o & 0x80) {
            for (int j = 0; j < 32; j++) {
                v[j] &= (uint8_t)~objcov[j];
                objtop[j] |= v[j] & (uint8_t)~cov[j];
                objsec[j] |= v[j] & (uint8_t)~cov2[j] & cov[j];
                cov2[j] |= cov[j] & v[j];
                cov[j] |= v[j];
                objcov[j] |= v[j];
            }
        } else {
            uint8_t t[32], s[32];
            for (int j = 0; j < 32; j++) { t[j] = v[j] & (uint8_t)~cov[j]; s[j] = v[j] & (uint8_t)~cov2[j] & cov[j]; }
            memcpy(top + o, t, 32);
            memcpy(sec + o, s, 32);
            for (int j = 0; j < 32; j++) { cov2[j] |= cov[j] & v[j]; cov[j] |= v[j]; }
        }
    }
    uint8_t a[32], b[32];
    for (int j = 0; j < 32; j++) { a[j] = (uint8_t)~cov[j]; b[j] = cov[j] & (uint8_t)~cov2[j]; }
    memcpy(top + 0xa0, a, 32);
    memcpy(sec + 0xa0, b, 32);
    memcpy(top + 0x80, objtop, 32);
    memcpy(sec + 0x80, objsec, 32);
}

/* the 32-bit mask word of block b (pixels 32b..32b+31) */
static inline uint32_t mask_word(const uint8_t *mask, int b) {
    uint32_t m;
    memcpy(&m, mask + 4 * b, 4);
    return m;
}

void spec_render_scanline_select_pixels_binary(uint16_t *dst, const uint16_t *src, const uint16_t *layer,
                                               const uint8_t *mask) {
    int same = src == dst;
    for (int b = 0; b < 8; b++, src += 32, dst += 32, layer += 32) {
        uint32_t m = mask_word(mask, b);
        if (same && !m) continue;
        uint16_t s[32], l[32];
        memcpy(s, src, 64);
        if (m) {
            memcpy(l, layer, 64);
            for (int j = 0; j < 32; j++) if (m >> j & 1) s[j] = l[j];
        }
        memcpy(dst, s, 64);
    }
}

void spec_render_scanline_select_pixels_binary_scalar(uint16_t *dst, const uint16_t *src, uint32_t colour,
                                                      const uint8_t *mask) {
    int same = src == dst;
    for (int b = 0; b < 8; b++, src += 32, dst += 32) {
        uint32_t m = mask_word(mask, b);
        if (same && !m) continue;
        uint16_t s[32];
        memcpy(s, src, 64);
        for (int j = 0; j < 32; j++) if (m >> j & 1) s[j] = (uint16_t)colour;
        memcpy(dst, s, 64);
    }
}

void spec_render_scanline_expand_6bit_split(uint8_t *out, const uint16_t *c) {
    for (int b = 0; b < 8; b++) {
        uint16_t s[32];
        uint8_t r[32], g[32], bl[32];
        memcpy(s, c + 32 * b, 64);
        for (int j = 0; j < 32; j++) {
            r[j] = (uint8_t)((s[j] & 0x1f) << 1);
            g[j] = (uint8_t)(((s[j] >> 5) & 0x1f) << 1);
            bl[j] = (uint8_t)(((s[j] >> 10) & 0x1f) << 1);
        }
        memcpy(out + 32 * b, r, 32);
        memcpy(out + 0x100 + 32 * b, g, 32);
        memcpy(out + 0x200 + 32 * b, bl, 32);
    }
}

void spec_render_scanline_select_pixels_binary32(uint8_t *out, uint8_t *alpha, const uint32_t *px, const uint8_t *mask) {
    for (int b = 0; b < 8; b++) {
        uint32_t m = mask_word(mask, b), p[32];
        uint8_t r[32], g[32], bl[32], a[32];
        memcpy(p, px + 32 * b, 128);
        memcpy(r, out + 32 * b, 32);
        memcpy(g, out + 0x100 + 32 * b, 32);
        memcpy(bl, out + 0x200 + 32 * b, 32);
        if (alpha) memcpy(a, alpha + 32 * b, 32);
        for (int j = 0; j < 32; j++) {
            if (!(m >> j & 1)) continue;
            r[j] = (uint8_t)p[j]; g[j] = (uint8_t)(p[j] >> 8); bl[j] = (uint8_t)(p[j] >> 16); a[j] = (uint8_t)(p[j] >> 24);
        }
        memcpy(out + 32 * b, r, 32);
        memcpy(out + 0x100 + 32 * b, g, 32);
        memcpy(out + 0x200 + 32 * b, bl, 32);
        if (alpha) memcpy(alpha + 32 * b, a, 32);
    }
}

void spec_render_scanline_select_pixels(uint8_t *eng, uint8_t *out, uint8_t *excl, uint8_t **layers,
                                        const uint32_t *p3d, uint8_t *alpha, uint32_t lmask) {
    uint16_t tmp[256];
    const uint16_t *src = 0;
    int found = 0;
    unsigned k = 0;
    for (uint32_t m = lmask; m; m >>= 1, k++) {
        if (!(m & 1)) continue;
        const uint16_t *l = (const uint16_t *)(layers[k] + 0x10);
        if (found) { spec_render_scanline_select_pixels_binary(tmp, src, l, excl + 32 * k); src = tmp; }
        else src = l;
        found++;
    }
    const uint16_t *bd = *(const uint16_t **)(eng + 0x18);
    if (found) spec_render_scanline_select_pixels_binary_scalar(tmp, src, *bd, excl + 0xa0);
    else for (int i = 0; i < 256; i++) tmp[i] = *bd;
    spec_render_scanline_expand_6bit_split(out, tmp);
    if (p3d && (lmask & 1)) spec_render_scanline_select_pixels_binary32(out, alpha, p3d, excl);
}

/* ======================================== colour effects ======================================== */

void spec_render_scanline_select_blend_enable(uint8_t *out, const uint8_t *excl, uint32_t lmask, uint32_t bits) {
    uint8_t acc[32] = { 0 };
    for (int k = 0; k < 5; k++) {
        if (!(lmask >> k & 1)) continue;
        const uint8_t *e = excl + 32 * k;
        if (bits >> k & 1) for (int j = 0; j < 32; j++) acc[j] |= e[j];
        else for (int j = 0; j < 32; j++) acc[j] &= (uint8_t)~e[j];
    }
    const uint8_t *e = excl + 0xa0;
    if (bits >> 5 & 1) for (int j = 0; j < 32; j++) acc[j] |= e[j];
    else for (int j = 0; j < 32; j++) acc[j] &= (uint8_t)~e[j];
    memcpy(out, acc, 32);
}

void spec_render_scanline_shade(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask) {
    uint32_t f = 2 * U16(eng, 0xa2);
    if (f > 32) f = 32;
    uint32_t factor = 32 - f, add = (U16(eng, 0xa0) & 0x40) ? 16 : f * 63 + 16;
    for (int b = 0; b < 8; b++) {                          /* 32 pixels a block: loads before stores */
        uint8_t r[3][32];
        for (int c = 0; c < 3; c++) for (int j = 0; j < 32; j++) {
            int i = 32 * b + j, m = mbit(mask, i);
            uint32_t s = ((uint32_t)in[0x100 * c + i] * (m ? factor : 32) + (m ? add : 0)) & 0xffff;
            r[c][j] = (uint8_t)(s >> 5);
        }
        for (int c = 0; c < 3; c++) memcpy(out + 0x100 * c + 32 * b, r[c], 32);
    }
}

/* A = min(2*EVA, 32), B = min(2*EVB + BLDALPHA.7, 32): the doubling moves bit 7 into EVB */
static inline void blend_coeffs(uint32_t bldalpha, uint8_t *a, uint8_t *b) {
    uint32_t w = 2 * bldalpha;
    uint32_t x = w & 0x3f, y = (w >> 8) & 0x3f;
    *a = (uint8_t)(x < 32 ? x : 32);
    *b = (uint8_t)(y < 32 ? y : 32);
}
void spec_render_scanline_color_effects_setup_blend_base(uint32_t bldalpha, uint8_t *eva, uint8_t *evb, const uint8_t *mask) {
    uint8_t a, b;
    blend_coeffs(bldalpha & 0xffff, &a, &b);
    for (int i = 0; i < 256; i++) { int m = mbit(mask, i); eva[i] = m ? a : 32; evb[i] = m ? b : 0; }
}
void spec_render_scanline_color_effects_setup_blend(uint32_t bldalpha, uint8_t *eva, uint8_t *evb, const uint8_t *mask) {
    uint8_t a, b;
    blend_coeffs(bldalpha & 0xffff, &a, &b);
    for (int i = 0; i < 256; i++) if (mbit(mask, i)) { eva[i] = a; evb[i] = b; }
}
void spec_render_scanline_color_effects_setup_alpha_base(uint8_t *eva, uint8_t *evb, const uint8_t *alpha,
                                                         const uint8_t *mask) {
    for (int b = 0; b < 4; b++) {                          /* 64 pixels a block: loads before stores */
        uint8_t ea[64], eb[64];
        for (int j = 0; j < 64; j++) {
            int i = 64 * b + j;
            uint8_t a = mbit(mask, i) ? alpha[i] : 31;
            ea[j] = (uint8_t)(a + 1); eb[j] = (uint8_t)(31 - a);
        }
        memcpy(eva + 64 * b, ea, 64); memcpy(evb + 64 * b, eb, 64);
    }
}
void spec_render_scanline_color_effects_setup_alpha(uint8_t *eva, uint8_t *evb, const uint8_t *alpha, const uint8_t *mask) {
    for (int b = 0; b < 4; b++) {
        uint8_t ea[64], eb[64], al[64];
        memcpy(ea, eva + 64 * b, 64); memcpy(eb, evb + 64 * b, 64); memcpy(al, alpha + 64 * b, 64);
        for (int j = 0; j < 64; j++)
            if (mbit(mask, 64 * b + j)) { ea[j] = (uint8_t)(al[j] + 1); eb[j] = (uint8_t)(31 - al[j]); }
        memcpy(eva + 64 * b, ea, 64); memcpy(evb + 64 * b, eb, 64);
    }
}
void spec_render_scanline_color_effects_apply(uint8_t *out, const uint8_t *src6, const uint8_t *eva, const uint8_t *evb) {
    for (int b = 0; b < 16; b++) {                         /* 16 pixels a block */
        uint8_t r[3][16];
        for (int c = 0; c < 3; c++) for (int j = 0; j < 16; j++) {
            int i = 16 * b + j;
            uint32_t s = ((uint32_t)src6[0x100 * c + i] * eva[i] + (uint32_t)src6[0x300 + 0x100 * c + i] * evb[i]) & 0xffff;
            uint32_t v = ((s + 16) >> 5) & 0xff;
            r[c][j] = (uint8_t)(v < 63 ? v : 63);
        }
        for (int c = 0; c < 3; c++) memcpy(out + 0x100 * c + 16 * b, r[c], 16);
    }
}
void spec_render_scanline_color_effects_apply_offset_c(uint8_t *out, const uint8_t *src6, const uint8_t *eva,
                                                       const uint8_t *evb, const uint8_t *off) {
    for (int b = 0; b < 16; b++) {
        uint8_t r[3][16];
        for (int c = 0; c < 3; c++) for (int j = 0; j < 16; j++) {
            int i = 16 * b + j;
            uint32_t s = (uint32_t)src6[0x100 * c + i] * eva[i] + (uint32_t)src6[0x300 + 0x100 * c + i] * evb[i] +
                         ((63u * off[i] + 16) & 0xffff);
            r[c][j] = (uint8_t)(s > 0x7ff ? 63 : s >> 5);
        }
        for (int c = 0; c < 3; c++) memcpy(out + 0x100 * c + 16 * b, r[c], 16);
    }
}

/* ======================================== the composite ======================================== */

void spec_render_scanline_2d_composite(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers, const uint32_t *p3d,
                                       uint8_t *alpha, uint32_t lmask, uint32_t bldcnt, uint32_t flags, uint32_t line) {
    (void)line;
    if (!(flags & 7)) {                                    /* the simple path */
        spec_render_scanline_priority_encode_single(eng, S + SCR_VIS, S + SCR_EXCL);
        if (!(flags & 8)) { spec_render_scanline_select_pixels(eng, out, S + SCR_EXCL, layers, p3d, 0, lmask); return; }
        spec_render_scanline_select_pixels(eng, S + SCR_SHIN, S + SCR_EXCL, layers, p3d, 0, lmask);
        spec_render_scanline_select_blend_enable(S + SCR_SHMASK, S + SCR_EXCL, lmask, bldcnt & 0x3f);
        for (int j = 0; j < 32; j++) S[SCR_SHMASK + j] &= (uint8_t)~S[SCR_FX + j];
        spec_render_scanline_shade(eng, out, S + SCR_SHIN, S + SCR_SHMASK);
        return;
    }
    uint32_t lmask2 = 0;                                   /* the layers that may be second: 2nd targets of lmask */
    if (lmask) {
        uint32_t first = eng[0x84];
        lmask2 = lmask & (bldcnt >> 8);
        if (!(first & 4)) lmask2 &= ~(1u << (first & 31));
    }
    uint8_t *top = S + SCR_TOP, *sec = S + SCR_SEC, *t1 = S + SCR_T1, *t2 = S + SCR_T2, *M = S + SCR_M;
    uint8_t *eva = S + SCR_EVA, *evb = S + SCR_EVB, *off = S + SCR_OFF;
    spec_render_scanline_priority_encode_double(eng, S + SCR_VIS, top, sec);
    spec_render_scanline_select_pixels(eng, S + SCR_PLANES2, top, layers, p3d, (flags & 8) ? 0 : alpha, lmask);
    spec_render_scanline_select_pixels(eng, S + SCR_PLANES2 + 0x300, sec, layers, p3d, 0, lmask2);
    spec_render_scanline_select_blend_enable(t1, top, lmask, bldcnt & 0x3f);
    spec_render_scanline_select_blend_enable(t2, sec, lmask, (bldcnt >> 8) & 0x3f);
    for (int j = 0; j < 32; j++) t1[j] &= (uint8_t)~S[SCR_FX + j];
    uint32_t f = flags;
    const uint8_t *objtop = top + 0x80;
    int have_mask = 0;
    switch (flags & 5) {
    case 4: for (int j = 0; j < 32; j++) M[j] = t1[j] & t2[j]; have_mask = 1; break;
    case 5:
        for (int j = 0; j < 32; j++) M[j] = ((S[SCR_SEMI + j] & objtop[j]) | t1[j]) & t2[j];
        f = flags & ~1u; have_mask = 1; break;
    case 1:
        for (int j = 0; j < 32; j++) M[j] = t2[j] & S[SCR_SEMI + j] & objtop[j];
        f = (flags & ~1u) | 4; have_mask = 1; break;
    default: break;
    }
    if (have_mask) {
        if (f & 0x10) { for (int j = 0; j < 32; j++) M[j] &= (uint8_t)~top[j]; f &= ~0x10u; }      /* opaque 3D */
        if (f & 0x20) { for (int j = 0; j < 32; j++) M[j] &= (uint8_t)~objtop[j]; f &= ~0x20u; }   /* OBJ image */
    }
    int init = (f & 4) != 0, use_off = 0;
    uint32_t bldalpha = U16(eng, 0xa4);
    if (f & 8) {                                           /* brightness: the setups of the dead *_c twins, inlined */
        uint32_t y = U16(eng, 0xa2), fy = y <= 16 ? 2 * y : 32;
        if (bldcnt >> 6 & 1) {                            /* darken */
            for (int i = 0; i < 256; i++) { eva[i] = (uint8_t)(mbit(t1, i) ? 32 - fy : 32); evb[i] = 0; }
        } else {                                           /* brighten */
            for (int i = 0; i < 256; i++) {
                int m = mbit(t1, i);
                eva[i] = (uint8_t)(m ? 32 - fy : 32); off[i] = (uint8_t)(m ? fy : 0); evb[i] = 0;
            }
            use_off = 1;
        }
        if (init) spec_render_scanline_color_effects_setup_blend(bldalpha, eva, evb, M);
        init = 1;
    } else if (init) spec_render_scanline_color_effects_setup_blend_base(bldalpha, eva, evb, M);
    if ((f & 2) && alpha) {                                /* bitmap OBJ alpha and translucent 3D */
        const uint8_t *bmp = S + SCR_BMP;
        for (int j = 0; j < 32; j++) M[j] = bmp[j] & objtop[j] & t2[j];
        if (!(bldcnt & 0x80) && p3d) for (int j = 0; j < 32; j++) M[j] |= top[j] & t2[j];
        if (init) spec_render_scanline_color_effects_setup_alpha(eva, evb, alpha, M);
        else spec_render_scanline_color_effects_setup_alpha_base(eva, evb, alpha, M);
    }
    if (use_off) spec_render_scanline_color_effects_apply_offset_c(out, S + SCR_PLANES2, eva, evb, off);
    else spec_render_scanline_color_effects_apply(out, S + SCR_PLANES2, eva, evb);
}

void spec_render_scanline_horizontal_shift_3d(uint32_t *dst, const uint32_t *src, int32_t hofs) {
    if (hofs >= 0) {
        memcpy(dst, src + hofs, (size_t)(256 - hofs) * 4);
        memset(dst + (256 - hofs), 0, (size_t)hofs * 4);
    } else {
        memcpy(dst - hofs, src, (size_t)(256 + hofs) * 4);
        memset(dst, 0, (size_t)(-hofs) * 4);
    }
}

/* ======================================== capture ======================================== */

void spec_render_scanline_capture_direct(const uint8_t *C, uint16_t *dst, const uint8_t *planes) {
    uint32_t w = U16(C, 0x4c);
    for (uint32_t i = 0; i < w; i++) {
        uint32_t r = planes[i] >> 1, g = planes[0x100 + i] >> 1, b = planes[0x200 + i] >> 1;
        dst[i] = (uint16_t)((((g << 5) + r) & 0xffff) | 0x8000 | ((b << 10) & 0xffff));
    }
}
void spec_render_scanline_capture_direct_3d(const uint8_t *C, uint16_t *dst, const uint32_t *px) {
    uint32_t w = U16(C, 0x4c);
    for (uint32_t i = 0; i < w; i++) {
        uint32_t p = px[i];
        uint32_t r = (p & 0xff) >> 1, g = (p >> 8 & 0xff) >> 1, b = (p >> 16 & 0xff) >> 1;
        int8_t a = (int8_t)(p >> 24);
        dst[i] = (uint16_t)((((g << 5) + r) & 0xffff) | (b & 0x1f) << 10 | (a > 0 ? 0x8000 : 0));
    }
}
/* one blended pixel: source A's 6-bit channels, source B's BGR555 pixel s */
static inline uint16_t cap_blend(uint32_t ar, uint32_t ag, uint32_t ab, uint16_t s, uint32_t eva, uint32_t evb2) {
    uint32_t sr = ar * eva + (s & 0x1f) * evb2, sg = ag * eva + (s >> 5 & 0x1f) * evb2, sb = ab * eva + (s >> 10 & 0x1f) * evb2;
    uint32_t r = sr < 0x400 ? sr >> 5 : 31;
    uint32_t bv = sb < 0x400 ? ((sb >> 5) & 0x3f) << 10 : 0x7c00;
    uint32_t g = sg > 0x3ff ? 0x3e0 : (sg >> 5) << 5;
    return (uint16_t)(r | g | bv | 0x8000);
}
void spec_render_scanline_capture_blended(const uint8_t *C, uint16_t *dst, const uint16_t *srcb, const uint8_t *planes) {
    uint32_t eva = C[0x54], evb2 = 2u * C[0x55];
    if (evb2 == 0 && eva == 16) { spec_render_scanline_capture_direct(C, dst, planes); return; }
    uint32_t w = U16(C, 0x4c);
    if (!*(void *const *)(C + 0x40)) {                    /* no source B line: A alone, scaled */
        for (uint32_t i = 0; i < w; i++) {
            uint32_t r = planes[i] * eva >> 5, g = planes[0x100 + i] * eva >> 5, b = planes[0x200 + i] * eva >> 5;
            dst[i] = (uint16_t)(r | 0x8000 | g << 5 | (b & 0x3f) << 10);
        }
        return;
    }
    for (uint32_t i = 0; i < w; i++) dst[i] = cap_blend(planes[i], planes[0x100 + i], planes[0x200 + i], srcb[i], eva, evb2);
}
void spec_render_scanline_capture_blended_3d(const uint8_t *C, uint16_t *dst, const uint16_t *srcb, const uint32_t *px) {
    uint32_t eva = C[0x54], evb2 = 2u * C[0x55];
    if (evb2 == 0 && eva == 16) { spec_render_scanline_capture_direct_3d(C, dst, px); return; }
    uint32_t w = U16(C, 0x4c);
    if (!srcb) {
        for (uint32_t i = 0; i < w; i++) {
            uint32_t p = px[i];
            uint32_t r = (p & 0xff) * eva >> 5, g = (p >> 8 & 0xff) * eva >> 5, b = (p >> 16 & 0xff) * eva >> 5;
            dst[i] = (uint16_t)(r | 0x8000 | g << 5 | (b & 0x3f) << 10);
        }
        return;
    }
    for (uint32_t i = 0; i < w; i++) {
        uint32_t p = px[i];
        dst[i] = cap_blend(p & 0xff, p >> 8 & 0xff, p >> 16 & 0xff, srcb[i], eva, evb2);
    }
}

/* ======================================== the 32-bit scanout ======================================== */

static inline uint32_t xrgb(uint8_t r, uint8_t g, uint8_t b, uint8_t x) {
    return (uint32_t)b | (uint32_t)g << 8 | (uint32_t)r << 16 | (uint32_t)x << 24;
}
void spec_render_scanline_color_convert_direct_32_1x(const uint8_t *p, uint32_t *dst) {
    for (int i = 0; i < 256; i++)
        dst[i] = xrgb((uint8_t)(p[i] << 2), (uint8_t)(p[0x100 + i] << 2), (uint8_t)(p[0x200 + i] << 2), 0xff);
}
void spec_render_scanline_color_convert_direct_32_2x(const uint8_t *e, const uint8_t *o, uint32_t *dst) {
    for (int x = 0; x < 512; x++) {
        const uint8_t *p = (x & 1) ? o : e;
        int i = x >> 1;
        dst[x] = xrgb((uint8_t)(p[i] << 2), (uint8_t)(p[0x100 + i] << 2), (uint8_t)(p[0x200 + i] << 2),
                      ((x & 15) < 8 && !(x & 1)) ? 0xff : 0);
    }
}
void spec_render_scanline_color_convert_shade_32_1x(const uint8_t *p, uint32_t *dst, uint32_t factor, uint32_t add) {
    uint32_t fa = factor & 0xff, ad = add & 0xffff;
    for (int i = 0; i < 256; i++) {
        uint8_t c[3];
        for (int k = 0; k < 3; k++) c[k] = (uint8_t)((uint8_t)((((uint32_t)p[0x100 * k + i] * fa + ad) & 0xffff) >> 5) << 2);
        dst[i] = xrgb(c[0], c[1], c[2], 0xff);
    }
}
void spec_render_scanline_color_convert_shade_32_2x(const uint8_t *e, const uint8_t *o, uint32_t *dst, uint32_t factor,
                                                    uint32_t add) {
    uint32_t fa = factor & 0xff, ad = add & 0xffff;
#define ACC(c) ((((uint32_t)(uint8_t)((c) << 2)) * fa + ad) & 0xffff)
    for (int x = 0; x < 512; x++) {
        const uint8_t *p = (x & 1) ? o : e;
        int i = x >> 1;
        uint8_t c[3];
        for (int k = 0; k < 3; k++) c[k] = (uint8_t)(ACC(p[0x100 * k + i]) >> 5);
        int it = x >> 5, k = x & 31;                       /* iteration (16 input pixels), output index in it */
        uint32_t lane;
        if (k < 16) lane = ACC(e[0x200 + 16 * it + 8 + (k >> 1)]);    /* v19: B of e, input pixels 8..15 */
        else lane = ACC(o[0x100 + 16 * it + ((k - 16) >> 1)]);       /* v23: G of o, input pixels 0..7 */
        dst[x] = xrgb(c[0], c[1], c[2], (uint8_t)(lane >> ((k & 1) * 8)));
    }
#undef ACC
}
