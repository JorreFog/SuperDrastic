/* edges.c: exact C ports of DraStic r2.5.2.2's polygon edge walking and per-line span setup (hi-res "4x" path).
 * Verified bit-exact against the originals by tools/rast/ut/t_edges.c.
 *
 * ================================================================================================================
 * 1. CONTEXT: how render_polygon_4x (0x53ef0) uses these routines
 * ================================================================================================================
 * polygon record (32 bytes): +0x08 u32 attr, +0x1a u16 base vertex index, +0x10 u64 (texture state, not used here).
 *   attr & 0xf           vertex count (1..9 slots are resolved; 0 = none)
 *   (attr >> 8) & 0xff   "flags" handed to the edge walkers (w26); flags & 0x18 != 0 -> x-only edges (no z array,
 *                        render_polygon_edge_interpolate_x_c), else x+z (render_polygon_edge_interpolate_xz_c).
 *                        attr bit 14 ((attr>>8) & 0x40) selects the sprite (axis-aligned quad) path instead.
 *   (attr >> 16) & 0x7f  index into the u32 table at 0x11df90 whose nibbles give the vertex walk order starting at
 *                        the TOP vertex (e.g. 0x3210, 0x0321, ... 0x2310); vertex k = verts[base + nibble k].
 *   attr >> 23           bottom y of the polygon (9 bits)
 * vptr[k] (k < count) = &verts[base + nibble k], and vptr[count] = vptr[0] again (cyclic), all on the stack.
 * y_top = vptr[0]->y.  With bin lines [bin_top, bin_bot) (args 4, 5 of render_polygon_4x):
 *   top_clip  = y_top < bin_top;   skip = bin_top - y_top (or 0)
 *   y_end     = min(y_bottom, bin_bot);  lines = y_end - max(y_top, bin_top);  nothing is drawn if lines <= 0
 *   bot_clip  = y_bottom > bin_bot
 * Edge marking OFF (DISP3DCNT bit 5 clear):
 *   constprop_0(span, span+0x6e0, &vptr[0],     bin_top, y_end, flags)   forward chain  -> "left"  arrays
 *   constprop_1(span+0xb0, span+0x6e0, &vptr[count], bin_top, y_end, flags) backward chain -> "right" arrays
 *   then for every line i < lines: x16[0x580+4i] &= 0x7fff, x16[0x630+4i] &= 0x7fff (vertical-edge flag dropped)
 *   then setup_spans_4x(span, lines); render_polygon_setup_4x(ctx, span, poly, span+0x840, first line, lines, ...)
 * Edge marking ON: one extra line is walked above (when top_clip) and/or below (when bot_clip):
 *   y_start' = bin_top - top_clip, y_end' = y_end + bot_clip, lines' = lines + top_clip + bot_clip,
 *   same two constprop calls with y_start'/y_end'; then for every line i < lines' with L = x16[0x580+4i],
 *   R = x16[0x630+4i], l = L & 0x7fff, r = R & 0x7fff:
 *       if (l > r) { if (!(L & 0x8200)) l++; } else { if (!(R & 0x8200)) r++; }   (stores l, r as u16)
 *   (i.e. the larger x becomes exclusive unless the edge is vertical (0x8000) or x has bit 9 set (>= 512));
 *   then setup_spans_4x(span, lines'); setup_edge_markers_c(span + 4*top_clip, lines, (bot_clip<<1)|top_clip);
 *   render_polygon_setup_4x(ctx, span + 4*top_clip, ...).  So the extra lines are only neighbours for the markers.
 * The span block is on render_polygon_4x's stack (span = sp+0x140); span+0x840 is a separate per-pixel buffer.
 * render_polygon_1x (0x4ea30) uses the same two constprop clones with its own span setup (not ported here).
 * The generic render_polygon_interpolate_edges (0x4d290) is never called.
 *
 * ================================================================================================================
 * 2. VERTEX RECORD (16 bytes) as the edge routines read it
 * ================================================================================================================
 *   +0x0 s32 w      perspective w (only used as a 32-bit integer; converted to float for the weights)
 *   +0x4 u16 x      screen x, hi-res pixels (0..511 normally)
 *   +0x6 u16 y      screen y, hi-res lines
 *   +0x8 u16 z      depth (z-buffer value; becomes z<<9 in the span)
 *   +0xa u16 colour BGR555 (r = bits 0-4, g = 5-9, b = 10-14; bit 15 ignored)
 *   +0xc s16 s      texture s (as stored; the sprite path treats them as 12.4)
 *   +0xe s16 t      texture t
 *
 * ================================================================================================================
 * 3. SPAN BLOCK LAYOUT (struct of arrays; 44 entries of 4 bytes = 0xb0 bytes per array; line i at +4*i)
 * ================================================================================================================
 * Each quantity has a LEFT array (forward vertex chain, written by constprop_0 with spans = base) immediately
 * followed by a RIGHT array (backward chain, constprop_1 with spans = base+0xb0). "Left/right" are chain names: per
 * line the chains are only ordered by x in setup_spans_4x.  The edge walkers write lines 0..total-1 of their
 * array (line 0 = y_start of the walk); NEON stores overrun by up to 7 (w) / 3 (s,t,r,g,b) entries per edge, the
 * overrun being overwritten by the next edge, so the last edge leaves up to 7 garbage entries after `total`.
 *
 *  off    | after the edge walkers (per chain)                     | after setup_spans_4x (left .. right)
 *  -------+--------------------------------------------------------+---------------------------------------------
 *  0x000 L| s32 w (perspective-correct, integer)                   | s32 w_start  (of the smaller-x chain)
 *  0x0b0 R| s32 w                                                  | s32 w_end - w_start
 *  0x160 L| u32 z<<9 (screen-linear; xz mode only, else untouched) | u32 z_start
 *  0x210 R| u32 z<<9                                               | u32 z_end - z_start
 *  0x2c0 L| u16 s | u16 t<<16  (perspective-correct, same units    | s_start | t_start<<16
 *  0x370 R|   as the vertex s,t)                                   | (s_end-s_start) | (t_end-t_start)<<16 (16b)
 *  0x420 L| u16 r | u16 g<<16  (6.3 fixed: c6*8 + 7 + delta,       | r_start | g_start<<16
 *  0x4d0 R|   c6 = 5->6 bit expanded vertex colour)                | (r_end-r_start) | (g_end-g_start)<<16
 *  0x580 L| u16 x | u16 b<<16; x = integer x (14 bits) | 0x8000 if  | x_start (min(x,0x200)) | b_start<<16
 *  0x630 R|   the edge is vertical (dx == 0); b as r,g             | width (x_end - x_start, x clamped to 512)
 *         |                                                        |   | (b_end-b_start)<<16
 *  0x6e0  | scratch of the edge walkers (both chains share it):    | (setup_edge_markers_c, with p = span or
 *         | float [num,den] pairs, then Q15 s16 weights at the      | span+4): u16 left-marker | u16 right-marker
 *         | start (see perspective_coefficients/steps below)        | <<16 per line
 * All the "_end - _start" deltas are 16-bit (or 32-bit for w, z) wrapping differences. setup_spans_4x swaps a line's
 * left and right values (all quantities together) when x_left >= x_right (bit 15 ignored), so afterwards:
 *   x_start <= x_end, width = min(x_end,512) - min(x_start,512).
 *
 * ================================================================================================================
 * 4. ROUTINES
 * ================================================================================================================
 * render_polygon_interpolate_edges(unused, spans, scratch, vptr, y_start, y_end, dir, flags)          [C, 0x4d290]
 * .constprop.0(spans, scratch, vptr, y_start, y_end, flags) = dir +1                                  [C, 0x4cf10]
 * .constprop.1(spans, scratch, vptr, y_start, y_end, flags) = dir -1                                  [C, 0x4d0d0]
 *   Walks the chain vptr[0], vptr[dir], vptr[2*dir], ... (y compares unsigned):
 *     prev = vptr[0]; yp = prev->y; n = total = skip0 = 0;
 *     if (y_end > yp) do { cur = next; y1 = cur->y;
 *         len = y1 - yp; sk = 0; if (y_start > yp) { len -= y_start - yp; sk = y_start - yp; }
 *         if (y1 > y_end) len -= y1 - y_end;
 *         if (len > 0) { counts[n] = (u8)len; if (n == 0) skip0 = sk; total += len; pairs[n] = (prev, cur); n++; }
 *         prev = cur; yp = y1; } while (y_end > yp);
 *     (it also reads the pointer after the last vertex used). Up to 16 edges (stack arrays).
 *   Then: perspective_coefficients_asm(scratch, pairs, counts, n, skip0)
 *         perspective_steps_asm(scratch, scratch, total)
 *         interpolate_w_asm(pairs, spans, scratch, counts, n)
 *         interpolate_parameters_asm(pairs, spans, scratch, counts, n)
 *         (flags & 0x18) ? interpolate_x_c(pairs, spans, counts, n, skip0)
 *                        : interpolate_xz_c(pairs, spans, counts, n, skip0)
 *   Precondition: n >= 1 (with n == 0 the asm routines run away; render_polygon_4x never does that).
 *   So line j of edge e (j counted from the edge's first walked line) is at y = y_a + j (+ skip0 for e == 0),
 *   and the edge's lines are stored consecutively.
 *
 * render_polygon_edge_perspective_coefficients_asm(out, pairs, counts, n, skip)                     [NEON, 0x9abd0]
 *   For each edge (a, b), with cnt = counts[e], float conversions of s32 (round to nearest):
 *     A = (float)a->w, D = (float)(s32)(a->w - b->w), B = (float)b->w, H = (float)(s32)(b->y - a->y)
 *     v0 = (0.0f * B, B * H); for e == 0 only: v0 += ((float)skip * A, (float)skip * D)
 *     d1 = (A, D), d2 = d1 + d1, d3 = d1 + d2, d4 = d2 + d2
 *     v1 = v0 + d1, v2 = v0 + d2, v3 = v0 + d3
 *     do { store v0,v1,v2,v3 (8 floats); v0 += d4; v1 += d4; v2 += d4; v3 += d4; cnt -= 4; } while (cnt > 0)
 *     out += 2 * cnt (cnt <= 0 now: rewinds the overrun)
 *   i.e. per line j: (num, den) = ((j+skip)*A, B*H + (j+skip)*D), computed by float accumulation in 4 lanes.
 *   Every fmul/fadd is separately rounded (no fma).  n must be >= 1.
 *
 * render_polygon_edge_perspective_steps_asm(out, in, total)                                        [NEON, 0x9ace4]
 *   do { 8 pairs (num, den) from in (64 bytes); r = frecpe(den); r *= frecps(r, den); r *= frecps(r, den);
 *        out[k] = (s16) fcvtzs(num * r, #15) (saturating to s32, then low 16 bits); in += 16; out += 8;
 *        total -= 8 } while (total > 0).   Used in place (out == in): weight q_j = num/den in Q15
 *   = perspective-correct weight of vertex b: q = t*wa / ((1-t)*wb + t*wa), t = (j+skip)/H.  1.0 -> 0x8000 (s16
 *   -32768) would wrap; it cannot occur for walked lines (j+skip < H) unless the float math rounds up.
 *
 * render_polygon_edge_interpolate_w_asm(pairs, spans, steps, counts, n)                            [NEON, 0x9ad40]
 *   Writes spans+0x000 (s32 per line). Per edge: d = b->w - a->w (32-bit wrap);
 *     w_j = (s32)(((s64)d * q_j) >> 15) + a->w     (low 32 bits, wrapping add)
 *   8 lines per iteration (overrun up to 7), then rewinds; steps pointer advances by cnt per edge.
 *
 * render_polygon_edge_interpolate_parameters_asm(pairs, spans, steps, counts, n)                   [NEON, 0x9ade8]
 *   Writes spans+0x2c0 (s|t), +0x420 (r|g), +0x580 (high half: b; low half: the stale NEON register v26, which
 *   the following x/xz routine overwrites for every real line; the port writes 0 there -- DraStic's v26 content is
 *   undefined). 4 lines per iteration (overrun up to 3).  Per edge, with q = q_j (s16):
 *     colour: c6(v) = 2*v + (v != 0) for each 5-bit component; dc = (s16)((c6(b) - c6(a)) << 3) (16-bit)
 *             c_j = (u16)(((c6(a) << 18) + 0x38000 + q * dc) >> 15)      (32-bit wrap, bits 15..30)
 *             = 8*c6(a) + 7 + floor(q * dc / 32768) ... i.e. 6.3 fixed point with a +7/8 bias
 *     tex:    ds = (s16)(b->s - a->s); s_j = (u16)(((s32)a->s * 32768 + (ds > 0 ? 0x800 : 0) + q * ds) >> 15)
 *             (same for t). The 0x800 is a rounding bias (1/16 of a unit) applied only on increasing coordinates.
 *
 * render_polygon_edge_interpolate_xz_c(pairs, spans, counts, n, skip)                                 [C, 0x4c930]
 * render_polygon_edge_interpolate_x_c(pairs, spans, counts, n, skip)                                  [C, 0x4ccd0]
 *   Screen-linear (not perspective) DDA, exactly cnt lines per edge (no overrun), edges with cnt == 0 skipped.
 *     dy = (s32)(b->y - a->y), r = reciprocal_table[dy] = (0x3fffffff + dy) / dy  (u32; 0 for dy = 0)
 *     dx = (s32)(b->x - a->x); P = (s64)dx * r (64-bit)
 *     xstep = (u32)((dx < 0 ? P + 0xfff : P) >> 12)  (14.18 fixed, truncated toward zero); x = a->x << 18 (u32)
 *     vflag = dx == 0 ? 0x8000 : 0
 *     xz only: dz = (s32)((u32)(b->z - a->z) << 9); zstep = (u64)((s64)dz * r) + (dz < 0 ? 0x40000000 : 0);
 *              z = (u64)a->z << 39
 *     edge 0 only: x += xstep * skip (u32); z += (u64)skip * zstep
 *     per line: u16 [0x580 + 4i] = (u16)(vflag | (x >> 18));   xz only: u32 [0x160 + 4i] = (u32)(z >> 30)
 *               x += xstep; z += zstep   (u32 / u64 wrapping)
 *   (z>>30 is z<<9 interpolated: depth with 9 fraction bits.) The x variant leaves 0x160 untouched.
 *
 * render_polygon_setup_spans_asm_4x(spans, lines)                                                 [NEON, 0x9cd10]
 *   Groups of 8 lines: do { ... lines -= 8 } while (lines > 0) (so always >= 8 lines processed, ceil8(lines)).
 *   Per line i: xl = x16[0x580+4i] & 0x7fff, xr = x16[0x630+4i] & 0x7fff; swap = xl >= xr;
 *   if swap: exchange left/right of x, b (0x582/0x632), s,t (0x2c0/0x370), w (0x000/0x0b0), r,g (0x420/0x4d0),
 *   z (0x160/0x210). Then xl = min(xl, 0x200), xr = min(xr, 0x200) and writes
 *     0x580: xl | bl<<16          0x630: (xr - xl) | (br - bl)<<16      (16-bit lanes)
 *     0x2c0: sl | tl<<16          0x370: (sr - sl) | (tr - tl)<<16
 *     0x000: wl                   0x0b0: wr - wl                         (32-bit)
 *     0x420: rl | gl<<16          0x4d0: (rr - rl) | (gr - gl)<<16
 *     0x160: zl                   0x210: zr - zl                         (32-bit)
 *
 * render_polygon_setup_edge_markers_c(p, lines, clip)                                                 [C, 0x4d680]
 *   p = span (+4 when the walk included an extra top line); clip bit0 = extra line above (line -1 valid),
 *   bit1 = extra line below (line `lines` valid). Reads x0[i] = u16 p[0x580+4i], wd[i] = u16 p[0x630+4i]
 *   (after setup_spans), writes u16 p[0x6e0+4i] = left marker count, p[0x6e2+4i] = right marker count:
 *     px = clip&1 ? x0[-1] : x0[0];  pe = clip&1 ? x0[-1] + wd[-1] : x0[0] + wd[1]   (sic: wd[1])
 *     cx = x0[0]; ce = cx + wd[0]; w = wd[0]
 *     iterations m = clip&2 ? lines : lines - 1   (lines >= 1 when bit1 clear; returns if m == 0 and bit1 set)
 *     for i in 0..m-1:  nx = x0[i+1]; nw = wd[i+1]; ne = nx + nw;     (all signed 32-bit)
 *        lo = max(max(cx + 1, nx), px);  hi = min(min(ce - 1, ne), pe);
 *        L = lo - cx; R = ce - hi;  store min_u(w, L), min_u(w, R)   (unsigned min: negatives become w)
 *        px = cx; pe = ce; cx = nx; ce = ne; w = nw;
 *     if !(clip & 2): last line (i = m): store wd[m] + 1, 0   (whole line is left edge).
 *   i.e. the left marker covers the pixels of this line left of both neighbours' spans (+1), the right marker
 *   those right of them; both are clamped to the line width.
 */
#include <arm_neon.h>
#include <string.h>
#include "edges.h"
#define EDGES_FN(name) spec_##name
#define EDGES_ARR  SPAN_ARR
#define EDGES_XMAX 0x200
#include "edges_impl.h"
