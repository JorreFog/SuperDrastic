# DraStic r2.5.2.2: the 2D sprite (OBJ) renderer

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. The model is `src/rast/spec/2d/obj.c` / `obj.h` (`re/2d/obj/h/obj_model.c`:
> `m_setup_edges`, `m_reorder_obj` and `m_render_obj` are `spec_video_2d_obj_affine_setup_edges`,
> `spec_video_2d_reorder_obj` and `spec_render_scanline_obj_c`; the `OBJ_STATS` counters were dropped).
> `objharness.c`'s generators run in DraStic's own process in `tools/rast/ut/t_obj2d.c`; `objdiff.c` was not kept. The
> other analyses are next to this file; the design is `../2d-engine.md`.

Key: `obj`. Scope: how DraStic draws the sprites of one 2D engine for one DS line: `video_2d_reorder_obj` 0x3e530
(the per-frame OBJ tables), `video_2d_obj_affine_setup_edges` 0x3e390 (the affine span setup) and
`render_scanline_obj_c` 0x36b70 (0x1abc bytes, mapped completely), plus how their output reaches
`render_scanline_2d` / `render_scanline_2d_composite` / the window code. Also which `render_sprite_*` routines
belong to this path (none: they are the 3D renderer's).

Sources and how the claims were checked:

- **[D]** = read in the disassembly (`re/drastic.dis`; the function dumps are in `re/2d/obj/*.s`).
- **[M]** = machine-checked. A C model of the three routines (`re/2d/obj/h/obj_model.c`, written from the
  disassembly, working on DraStic's own engine-struct byte layout) was compared byte for byte with DraStic's own code
  in two ways:
  1. `re/2d/obj/h/objharness.c`: loads the `drastic` ELF into an aarch64 process under `qemu-aarch64`, applies its
     relocations, and calls DraStic's functions directly on random states (random OAM including every shape, size,
     mode and flag, rotation/scale matrices, identity and degenerate matrices, the 12-sprite full-screen image,
     duplicate OBJs at (0,0), random DISPCNT mapping bits, engine A/B, ext palette on/off, random stale table and
     stack contents). Result: **6000 frames, 1.15 M lines, 16.5 M OBJ-line entries, 220 M affine pixels, 0
     mismatches** in the OBJ tables (`eng+0x380..0x21418`), the whole scratch area `S+0xa60..0xf00` after every line,
     and the return value; `setup_edges` alone: 200 000 random cases, 0 mismatches. Mutations of the model (a
     rounding, the list order, the wrap test, the duplicate filter, the full-screen test, the 16-bit offset wrap) are
     caught (the last one as a crash: unwrapped offsets leave VRAM).
  2. `re/2d/obj/h/objdiff.c`: a preload library that hooks both functions in the running emulator (the simulator,
     `run.sh`) and runs the model next to DraStic on every call, on the real state. On the ds2d test scenes (build of
     14:58) T4 (all sizes, 4/8bpp, 1D/2D, flips, ext palettes, a crowded line), T5 (affine, double size, bitmap with
     alpha, semi-transparent), T6 (OBJ window), T8 (OBJ mosaic), T9 (3D + OBJ), 30 s each: about 16 000 reorders and
     1.6 M rendered lines, **0 mismatches**. Then T5 of the 19:43 build (adds the 12-sprite full-screen image, broken
     every other 16 frames), 50 s: 5752 reorders (356 found the image), 540 000 lines, 0 mismatches; a third hook on
     `render_scanline_2d_composite` checked, on all 273 408 composites with the image active, that the OBJ layer
     source is the image line, the vis slot `4+prio` equals the image line's bit-15 mask, flags has 0x20 and the
     layer mask has 0x10: 0 failures (section 6). T9 of the 19:43 build (a captured 3D frame shown as the image,
     2x), 50 s: 5560 reorders, 520 000 lines, 0 mismatches; 236 544 image composites, 59 136 with the 1x VRAM line
     (q = 0) and 177 408 (exactly 3x as many) with a hi-res capture quarter (q = 1..3), 0 failures.
- **[G]** = guess (not verified).

Offsets: `eng` = a 2D engine struct (A = `video+0x2e78`, B = `video+0x84298`, see `re/2d/frame.md` 7).
`S` = render_scanline_2d's scratch area (`sp+0x180` of its frame, see `re/compositing-2x.md` 1.3).
`T` = `eng+0x380`, the OBJ tables.

---

## 0. Summary and corrections to the premise

1. **The `render_sprite_*` routines are not 2D sprites.** `render_sprite_check.isra.0` 0x43e20 is called only by
   the 3D binning (`video_3d_bin_polygons[_y_sort]_1x/4x`, 0x4fcb4, 0x50344, 0x55194, 0x55824): it tests whether a 3D
   quad is a screen-aligned rectangle whose texture coordinates step exactly one texel per pixel (s/t deltas
   `== 16*dx` or `16*dx - 1` in 12.4) inside the texture, and if so sets bit 14 of the polygon attribute word
   (`[poly+8] |= 0x4000`). `render_polygon_1x/4x` then draws it with `render_sprite_block_1x/4x` (calls at
   0x4f14c/0x4f504/0x5461c/0x549d4), which use `render_sprite_load_texels_paletted_c`, `render_sprite_modulate_c`,
   `render_sprite_writeback_c_1x/4x`, `render_sprite_load_depth[_colors_id]_asm_1x/4x`. The other `render_sprite_*`
   symbols (`load_texels_paletted_4bpp_c`, `load_texels_c`, `modulate_alpha_c`, `set_alpha_c`,
   `writeback_alpha_c_*`, `load_depth*_c_*`, `writeback_asm_*`, `..._4bpp_asm_unused`) have no caller and no
   function-pointer relocation: dead code. [D] All of this is the "2D-in-3D" (3D sprite quad) path and belongs to
   the 3D rasterizer, already replaced by ours.
2. **The 2D OBJ path is three routines**: `video_2d_reorder_obj` (decodes all 128 OAM entries into per-OBJ records
   and per-line, per-priority lists), `video_2d_obj_affine_setup_edges` (two calls per affine OBJ) and
   `render_scanline_obj_c` (one call per engine per DS line). There is no NEON assembly version; the C is compiled
   with autovectorised loops. [D]
3. **No OBJ mosaic.** Nothing reads OAM attr0 bit 12 except the full-screen-image test (which requires it clear), and
   nothing reads MOSAIC bits 8-15 (`[eng+0xa8]` is read only by `render_scanline_bg` and the BG bitmap code; no byte
   load of `+0xa9` exists). Mosaic sprites are drawn as normal sprites. [D, M]
4. **No per-line sprite limit**: all 128 OBJs can render on a line; DISPCNT.23 (H-blank free) is never read. [D, M]
5. **OBJ-to-OBJ ordering differs from the hardware.** DraStic keeps one colour line and draws the lists in the order
   window, priority 3, 2, 1, 0, each list from the highest OAM index to the lowest, so a pixel shows the opaque OBJ
   with the lowest priority *number*, then the lowest OAM index. The DS shows the lowest OAM index regardless of
   priority (the well-known "priority inversion" case: a low-index OBJ with priority 3 behind a BG hides a
   high-index priority-0 OBJ; in DraStic the priority-0 OBJ shows). [M for DraStic's order; hardware behaviour from
   GBATEK, not tested here]
6. **Sprites are always rendered at 1x**, also in 2x ("hires") mode, and reused unchanged for the four 2x quarters
   (each OBJ pixel becomes 2x2; rotated sprites are not resampled at 2x). The only OBJ content with 2x data is the
   12-sprite full-screen bitmap image, when it shows a hi-res display capture (section 6). [D, M in situ]
7. **The OBJ mapping mode and ext-palette enable are latched when the tables are built**: DISPCNT bits 4, 5, 6,
   20-22 and 31 are read only by `video_2d_reorder_obj`, which runs at `start_frame` (line 0, per rendered engine),
   after a replayed OAM write (`[eng+0xb6]`, at the next line's `render_scanline_2d`) and at savestate load. A
   mid-frame DISPCNT change of these bits takes effect at the next OAM write or the next frame. [D]
8. **Corrections to `re/2d/frame.md` 7.1 / 15.4**: the OBJ records are 0x58 bytes at `eng+0x380 + i*0x58`, indexed
   by OAM number (not sorted, not stride 0x30); the sorting is done by the per-line lists at `eng+0x2f80` (section
   2.1); `eng+0x20f80` holds the 5 x 192 per-line list counts; the line flag byte has bit 1 = bitmap OBJ besides
   bit 0 = semi-transparent OBJ. [M]
9. **The OBJ cost is large in sprite scenes**: `render_scanline_obj_c` 0.65 M instructions a frame on T4 (27.7% of
   the frame) and 0.68 M on T5 (25.8%), plus 0.02-0.04 M of `video_2d_reorder_obj` (section 8). Engine A's OBJ runs
   on the emulation thread.

---

## 1. Call structure

```
start_frame (line 0, emulation thread), per engine that renders this frame:
    video_2d_reorder_obj(eng)                                   0x30a3c / 0x30b5c
video_2d_load_savestate: tail call video_2d_reorder_obj(eng)    0x43b50
render_scanline_2d(eng, planes, line, capture, hires)           0x3ef00, per DS line
    if [eng+0xb6] (an OAM write was replayed after the previous line): video_2d_reorder_obj(eng); [eng+0xb6] = 0   0x40078
    flagbyte = [eng+0x21340 + line]                             0x3efac (semi-transparent / bitmap OBJ on this line)
    render_scanline_bg(...)
    if DISPCNT.12 (OBJ on):                                     0x3f028
        layers |= render_scanline_obj_c(eng, S+0xa70, S+0xc90, S+0xe20, S+0xec0, S+0xee0, line)    0x3f920
    if [eng+0x21400] (full-screen OBJ image): layers |= 0x10, flags |= 0x20, OBJ vis slot / source replaced (6)
    render_scanline_generate_window_masks(eng, S+0xf00, S+0xfa0, objwin = S+0xea0, layers, line)
    ... 1x composite, or 4 quarters at 2x, all reading the same OBJ outputs
```

`video_2d_obj_affine_setup_edges` is called only by `video_2d_reorder_obj` (0x3eba4, 0x3ebc8). [D]

The OAM read is `[eng+0x30]` = the real OAM array (A: `sys+0x15070`, B: `sys+0x15470`), not the CPU's deferred
shadow: CPU writes during lines 0..191 reach it through the event replay after their line (and set `[eng+0xb6]`),
writes during vblank go to it directly and are picked up by the next `start_frame` (`re/2d/frame.md` 3, 4.2). So OAM
is line-exact: a write during line L is visible from line L+1. [D]

---

## 2. video_2d_reorder_obj(x0 eng) 0x3e530: the OBJ tables

Frame 0xe0 bytes; no return value. Reads: `[eng+0x90]` DISPCNT, `[eng+0x30]` OAM (0x400 B), `[eng+0x18]` palette
(the OBJ palette is `+0x200`), `[eng+0x28]` OBJ ext palette, `[eng+0x08]` VRAM alias, `[eng+0xb7]` engine index,
`[eng+0]` video (full-screen image only), the size table `obj_size_table.13328` 0x11def0. [D, M]

### 2.1 Table layout (T = eng+0x380) [M]

| address | size | contents |
|---|---|---|
| `T + i*0x58`, i = 0..127 | 0x58 each | OBJ record `E(i)` (2.2), indexed by OAM number |
| `T + 0x2c00 + l*0x6000 + y*0x80 + k` = `eng+0x2f80 + ..` | u8 | list `l` (0..3 = priority 0..3, 4 = OBJ window) of line `y`: the k-th OBJ number, in increasing OAM order |
| `T + 0x20c00 + l*0xc0 + y` = `eng+0x20f80 + ..` | u8 | number of entries of list `l` on line `y` (cleared by a 0x3c0-byte memset per call) |
| `eng + 0x21340 + y` | u8 | line flags: bit 0 = a semi-transparent OBJ (mode 1) is registered on line y, bit 1 = a bitmap OBJ (mode 3) is (cleared per call, 0xc0 B) |
| `eng + 0x21400` | ptr | full-screen OBJ image: pointer to its first u16 in the VRAM alias, or NULL (cleared per call) |
| `eng + 0x21408` | ptr | its hi-res capture data (3 x 256 u16 per line) or NULL (written only when the image is found) |
| `eng + 0x21410` | u8 | its priority (written only when the image is found) |

### 2.2 The OBJ record E (0x58 bytes) [M]

| offset | type | contents |
|---|---|---|
| 0x00 / 0x08 / 0x10 | s64 | affine X span: start, length, per-line step (32.32 pixels, section 3) |
| 0x18 / 0x20 / 0x28 | s64 | affine Y span: start, length, per-line step |
| 0x30 | ptr | palette: 4bpp `pal+0x200 + palbank*0x20`; 8bpp `ext ? ext + palbank*0x200 : pal+0x200` (ext = `[eng+0x28]` if DISPCNT.31, else NULL); computed from attr0.13 also for bitmap OBJs (unused there) |
| 0x38 | ptr | `[eng+8] + objbase + off`: the first byte of the OBJ's data in the VRAM alias (objbase 0x400000 engine A, 0x600000 engine B); adjusted for hflip and left clipping (2.4) |
| 0x40 / 0x42 | s16 | affine: texture x / y (8.8, low 16 bits) at box pixel 0 of the centre line, after left clipping |
| 0x44 | u16 | byte pitch of one tile row (tiles) or pixel row (bitmaps), 2.4 |
| 0x46 | s16 | X of the first drawn pixel (-7..255 after clipping) |
| 0x48 | s16 | Y: normal = attr0 y; vflip = y + h - 1; affine = y + bh/2 (box centre); identity double = y + h/2; minus 256 when the registration start is > 191 |
| 0x4a / 0x4c / 0x4e / 0x50 | s16 | PA, PC, PB, PD (note the order), stored for every affine OBJ |
| 0x52 | u8 | kind: bit0 8bpp, bit1 bitmap, bit2 hflip, bit3 affine. Values 0, 1, 4, 5 (tiled), 2, 6 (bitmap), 8, 9, 10 (affine 4bpp, 8bpp, bitmap) |
| 0x53 | u8 | attribute byte: 0 = normal or window, 0x80 = semi-transparent, `2*alpha+1` (3..31) = bitmap |
| 0x54 | u8 | vflip (non-affine only) |
| 0x55 | u8 | drawn width in pixels after clipping (box width for affine) |
| 0x56 | 2 | never written |

Records of OBJs that are skipped keep stale contents (partially written before the skip: 0x48, 0x53, 0x54, the
affine fields). Only records referenced by a list are read. [M]

### 2.3 Per-OBJ decode (exact; the C is `m_reorder_obj` in obj_model.c) [M]

```
dc = DISPCNT; tshift = dc.4 ? 5 + dc[21:20] : 5          (tile number -> byte offset)
excl[] = full-screen-image OBJ numbers if (dc & 0x60) == 0x20 and the image is found (2.6), else empty
last = (0xffff, 0xffff, 0xffff)
for i = 0..127 (a0, a1, a2 = OAM[i]):
    if i is the next entry of excl[]: skip
    if shape (a0>>14) == 3: skip;  if (a0 & 0x300) == 0x200: skip (not affine + "disable")
    (w, h) = size[shape*4 + a1>>14];  dbl = a0 & 0x200;  bw = dbl ? 2w : w;  bh = dbl ? 2h : h
    y = a0 & 0xff;  x9 = a1 & 0x1ff;  ystart = y
    if y > 191 and y + bh <= 255: skip                    (below the screen, not wrapping to the top)
    if y <= 191 and x9 == 0 and y == 0:                   (duplicate filter, output-neutral)
        if (a0, a1, a2) == last: skip;  last = (a0, a1, a2)
    if x9 > 255 and x9 + bw <= 511: skip                  (entirely right of the screen)
    x = sext9(x9)
    E.y = y; E.attr = 0; E.vflip = 0
    if affine (a0.8):
        group = (a1>>9) & 31: PA, PB, PC, PD = s16 OAM[group*32 + 6, +0xe, +0x16, +0x1e]; store them
        if PA == 0x100 and PD == 0x100 and PB == PC == 0:             identity: a plain, unflipped sprite
            kind = 0
            if dbl: E.y = y + h/2;  x = (s16)(x + w/2);  ystart = y + h/2
                    if ystart > 191 and ystart + h <= 255: skip;  if x > 255: skip;  if x + w <= 0: skip
                    bw = w; bh = h
        else:                                                          rotation / scaling
            tx0 = (w/2)*256 - PA*(bw/2);  ty0 = (h/2)*256 - PC*(bw/2)          (32-bit)
            if x < -7: c = (-x) & ~7; x += c; bw -= c; tx0 += PA*c; ty0 += PC*c
            if x + bw > 256: bw = (263 - x) & ~7
            E.tx0 = tx0; E.ty0 = ty0; E.y = y + bh/2
            setup_edges(tx0, PA, w*256 - 1, PB) -> E[0x00, 0x08, 0x10]
            setup_edges(ty0, PC, h*256 - 1, PD) -> E[0x18, 0x20, 0x28]
            kind = 8
    else:
        kind = hflip (a1.12) << 2
        if vflip (a1.13): E.y = y + h - 1; E.vflip = 1
    mode = (a0>>10) & 3;  prio = (a2>>10) & 3
    if mode == 3 (bitmap):
        alpha = a2>>12;  if alpha == 0: skip;  E.attr = 2*alpha + 1
        tile = a2 & 0x3ff
        dc.6 (1D):   off = tile << (7 + dc.22);                           pitch = 2w
        dc.5 (2D):   off = ((tile>>5)*256 + (tile & 0x1f)) * 16;         pitch = 0x200   (256-dot wide)
        else (2D):   off = ((tile>>4)*128 + (tile & 0x0f)) * 16;         pitch = 0x100   (128-dot wide)
        if not affine: if hflip: off += (w-1)*2
                       if x < -7: c = (-x) & ~7; x += c; bw -= c; off += hflip ? -2c : 2c
                       if x + bw > 256: bw = (263 - x) & ~7
        kind |= 2;  list = prio;  lineflag = 2
    else (tiled):
        off = (a2 & 0x3ff) << tshift;  kind |= a0.13 (8bpp)
        mode 1: E.attr = 0x80; list = prio; lineflag = 1
        mode 0: list = prio;  mode 2: list = 4 (window);  lineflag = 0
        2D (dc.4 == 0): pitch = 0x400; if 8bpp: off &= ~0x20 (tile bit 0 ignored); if hflip: off += (w/8 - 1)*(8bpp ? 64 : 32)
        1D:             pitch = (w/8)*(8bpp ? 64 : 32);              if hflip: off += (w/8 - 1)*(8bpp ? 64 : 32)
        if not affine: if x < -7: c = (-x) & ~7; x += c; bw -= c; off += (hflip ? -1 : 1) * c*(8bpp ? 8 : 4)
                       if x + bw > 256: bw = (263 - x) & ~7
    (bw is never 0 here; the code tests it)
    E.x = x; E.vram = [eng+8] + objbase + off; E.kind = kind; E.width = bw; E.pal = (above)
    if ystart > 191: E.y -= 256; ystart -= 256              (sprites starting at y >= 192 wrap to the top)
    for l = ystart .. ystart + bh - 1: yy = l & 0xff; if yy <= 191: append i to list[list][yy]; flags[yy] |= lineflag
```

Notes:
- Clipping is to whole tiles: a sprite partly left of the screen keeps x in -7..-1 and draws its first tile partly
  into the 8-pixel padding of the line buffers; the right clip keeps 1..7 pixels beyond x = 255. The padding is
  ignored downstream (2.5, 4.3). [M]
- The "identity" test uses the full s16 values; an identity matrix is drawn exactly like a non-affine sprite (and
  attr1 bits 12-13, part of the group number, are not taken as flips). [M]
- Bitmap OBJs ignore attr0.13; DISPCNT bits 5 and 6 together mean 1D. Alpha 0 bitmap OBJs are not registered at all
  (no line flag). [M]
- The duplicate filter only drops an OBJ whose (a0, a1, a2) equal those of the last OBJ seen at position (0, 0);
  identical OBJs draw identical pixels and the lower OAM index wins anyway, so the output is unchanged. A
  replacement does not need it (it only matters to reproduce the tables byte for byte). [M]

### 2.4 OBJ VRAM addressing summary [M]

| kind | byte offset of the OBJ's data (`off`) | pitch (next row of tiles / pixels) | within a row |
|---|---|---|---|
| tiled 2D (dc.4 = 0) | `tile*32`, 8bpp: `(tile & ~1)*32` | 0x400 (32 tile slots) | tile k at `+k*32` (4bpp) / `+k*64` (8bpp); pixel row r at `+r*4` / `+r*8` |
| tiled 1D | `tile << (5 + dc[21:20])` | `(w/8)*32` / `(w/8)*64` | same |
| bitmap 2D 128 (dc.5 = 0, dc.6 = 0) | `(tile & 0xf)*16 + (tile >> 4)*0x800` | 0x100 | pixel k at `+2k` |
| bitmap 2D 256 (dc.5 = 1, dc.6 = 0) | `(tile & 0x1f)*16 + (tile >> 5)*0x1000` | 0x200 | same |
| bitmap 1D (dc.6 = 1) | `tile << (7 + dc.22)` | `2w` | same |

All reads go through the VRAM alias `[eng+8] + 0x400000/0x600000 + off` (the DS address minus 0x06000000), with
whatever bank mapping the alias holds at render time (`re/2d/frame.md` 4.3). No masking to the OBJ VRAM size is done
beyond the 10-bit tile number.

### 2.5 Palettes [M]

- 4bpp: `pal + 0x200 + palbank*32`, index 0 transparent, colour = the raw u16 of palette RAM (bit 15 kept as stored).
- 8bpp: with DISPCNT.31 and `[eng+0x28] != NULL`: `[eng+0x28] + palbank*0x200` (the 8 KiB OBJ extended palette in
  VRAM bank memory; set by `remap_vram_body` 0x302ec, cleared at 0x2ff9c [D]); otherwise the normal OBJ palette.
  Index 0 transparent.
- The palette *pointer* is latched at reorder time, the palette *contents* are read when the line is drawn, so
  palette writes are line-exact (they arrive by the event replay).

### 2.6 The 12-sprite full-screen bitmap image (0x3ecdc..0x3ee64) [M]

Only when `(DISPCNT & 0x60) == 0x20` (bitmap OBJ, 2D mapping, 256-dot wide). Scan i = 0..127; an OBJ qualifies when
`y <= 191`, `(a1 & 0x13f) | (a0 & 0x3f) == 0` (y, x multiples of 64, x < 256), `a0 & 0xff00 == 0x0c00` (bitmap,
not affine, no mosaic, 4bpp bit clear, square), `a1 & 0xfe00 == 0xc000` (64x64, no flips), `a2 & 0xf000 == 0xf000`
(alpha 15). `row = y/64, col = x/64, base = (a2 & 0x3ff) - (row*256 + col*8)`, `prio = a2 bits 10-11`. OBJ 0 fixes
(prio, base) **only if it is OBJ number 0** (`i == 0`); every other candidate must match them (so if OBJ 0 does not
qualify, the image needs prio 0 and base tile 0). The first OBJ per cell is recorded; when all 12 cells
(`(1<<col) << row*4` = 0xfff) are filled:

- bank = the last of A, B, C, D (in that order) whose `[video+0x10+b*16] == 6` and `[video+0x18+b*16] << 14 ==
  objbase`; if none: no image (the excluded list is reset).
- `w1 = (base & 0x1f) + (base >> 5)*256`, `off = w1*16`; `[eng+0x21400] = [eng+8] + objbase + off`,
  `[eng+0x21410] = prio`, `[eng+0x21408] = NULL`, and if `w1*8 <= 0x4000` and all six 16 KiB blocks from
  `off >> 14` are valid in `C+0x20+bank` (`(0x3f << (off>>14)) & ~valid == 0`): `[eng+0x21408] = [C + bank*8] +
  w1*48` (C = `video+0x458820`, the hi-res capture buffers, `re/2d/frame.md` 8.2).
- The 12 OBJs are left out of the normal lists (the excl list, in OAM order).

### 2.7 Code map of video_2d_reorder_obj (0x3e530..0x3eefc) [D]

| range | what |
|---|---|
| 0x3e530..0x3e648 | DISPCNT decode (tile shift / 2D mask 0x3ecc8, OBJ base, ext palette), memsets of counts and line flags, image pointer = NULL; `(dc & 0x60) == 0x20` -> 0x3ecdc |
| 0x3e64c..0x3e6a4 | loop setup: size table 0x11def0, bitmap shift, duplicate filter state `[sp+0x6c..0x74]`, record pointer `x9 = E+0x30` |
| 0x3e6a8..0x3e744 | exclusion list, shape 3, disable, size, y range (0x3e700), x range (0x3e72c), sign extension |
| 0x3e748..0x3e7b8 | record header, affine group load, identity test; 0x3e7c0 identity double size |
| 0x3e8e4..0x3e930 | y <= 191: the (0,0) duplicate filter |
| 0x3e950..0x3e978 | non-affine flips |
| 0x3e818..0x3e894, 0x3ec5c.. | bitmap OBJs |
| 0x3e97c..0x3ea2c, 0x3ebd4.. | tiled OBJs: 2D / 1D addressing, hflip, left/right clip |
| 0x3ea30..0x3eb0c | record store, palette pointer, list registration loop (0x3eab8) |
| 0x3eb10..0x3ebd0 | rotation/scaling: texture origin, clip, two `setup_edges` calls |
| 0x3ecdc..0x3eef4 | the 12-sprite full-screen image detection and bank check |

---

## 3. video_2d_obj_affine_setup_edges(w0 t0, w1 dA, w2 lim, w3 dB, x4 start*, x5 step*, x6 len*) 0x3e390 [M]

Leaf, no stack. For one axis (X: `t0 = tx0, dA = PA, lim = w*256-1, dB = PB`; Y: `ty0, PC, h*256-1, PD`) it finds
the box pixels `i` whose texture coordinate `t0 + i*dA + n*dB` is in `[0, lim]`, as a span `[start + n*step,
start + n*step + len]` in 32.32 fixed point (n = line relative to the box centre). All three outputs are s64.

```
dA > 0:  start = ceil(((dA - 1 - t0) << 32) / dA);   len = ceil(((lim - t0) << 32) / dA) - start
dA < 0:  start = ceil(((dA + lim + 1 - t0) << 32) / dA);   len = ceil(((-t0) << 32) / dA) - start
dA != 0: step = ceil(((-dB) << 32) / dA)
dA == 0, dB == 0: inside = 0 <= t0 <= lim:  start = inside ? 0 : -1;  len = inside ? 128<<32 : 0;  step = 0
dA == 0, dB != 0: q1 = trunc((dB > 0 ? lim - t0 : -t0) / dB);  q0 = trunc((dB > 0 ? -t0 : lim - t0) / dB)  (32-bit sdiv)
                  start = (-128*q1) << 32;  len = ((-128*q0) << 32) - start + (128 << 32);  step = 128 << 32
```

`ceil` is the exact ceiling of the 64-bit quotient (DraStic's four sign cases of `sdiv` with `+-(d-1)` all round
up). The numerators are formed in 32 bits, then shifted. The `dA == 0` case encodes a per-line on/off as a span
that moves by 128 pixels a line (128 = the widest box).

**Where this differs from the per-pixel hardware rule** (measured with the model's debug counters over 169 M box
pixels: 7313 extra, 4269 missing pixels, **all on lines above the box centre**, n < 0):
- `step` is rounded up, so `n*step` with n < 0 is rounded *down* (by at most |n| x 2^-32); when the exact edge of
  either axis is an integer, the span's left end moves one pixel left (an extra pixel whose texture coordinate is
  one 8.8 unit outside the texture: -1, or size*256 when the step is negative) and its right end one pixel left (the
  last in-range pixel is not drawn). For n >= 0 the spans are exact (the rounding error stays below 1/|dA|).
- In the `dA == 0` case the span end is inclusive, so on the line just before the first visible line box pixel 0 is
  drawn (coordinate out of range).
An extra pixel's texel is still fetched, through the 16-bit offset wrap of 4.2 (for example `tx>>8 = -1` reads the
previous tile's or the previous row's data). A replacement must use the same 32.32 arithmetic. [M]

---

## 4. render_scanline_obj_c(x0 eng, x1 col, x2 alpha, x3 vis, x4 semi, x5 bmp, w6 line) -> w0 0x36b70

Frame 0x370: `sp+0xd0` u16 offsets[128], `sp+0x1d0` u8 nibble shifts[128], `sp+0x258` u8 buf[256] (8 spare bytes
on each side), stack guard. Called as `(eng, S+0xa70, S+0xc90, S+0xe20, S+0xec0, S+0xee0, line)`. [D]

### 4.1 Algorithm [M]

```
ret = 0
for l = 4, 3, 2, 1, 0:                       window list first, then priority 3 .. 0
    out = vis + l*32                          (l = 4 -> S+0xea0, the OBJ window mask)
    n = count[l][line]; if n == 0: out[0..31] = 0; continue
    memset(buf, 0, 256)
    for k = n-1 .. 0: draw(E(list[l][line][k]))        highest OAM number first: the lowest one is drawn last
    out = bitmap(buf[i] != 0, i = 0..255)    (bit i = byte i>>3, bit i&7)
    ret = 0x10
if lineflags[line]:                          semi-transparent or bitmap OBJ registered on the line
    semi = bitmap(alpha[i] & 0x80);  bmp = bitmap((alpha[i] & 0x3f) != 0);  alpha[i] &= 0x3f  (all 256)
else:
    semi = 0; bmp = 0                        (alpha untouched)
return ret
```

`draw` writes, for every opaque texel of the OBJ at screen pixel p (p from -7 to 263):
`buf[p] = index (or colour>>8 for bitmaps)`, `col[p] = colour (u16)`, `alpha[p] = E.attr`.

### 4.2 The per-pixel paths [M]

Non-affine (`row = E.vflip ? E.y - line : line - E.y`, as u32):

| kind | source pointer | per output pixel k = 0 .. width-1 | opaque when | colour |
|---|---|---|---|---|
| 0 4bpp | `vram + (row>>3)*pitch + (row&7)*4` | tile t = k/8: u32 at `+t*32`, nibble k&7 | index != 0 | `pal[index]` |
| 4 4bpp hflip | same (vram points at the last tile) | u32 at `-t*32`, nibble 7-(k&7) | " | " |
| 1 8bpp | `vram + (row>>3)*pitch + (row&7)*8` | byte k&7 of the 8 at `+t*64` | " | " |
| 5 8bpp hflip | same | byte 7-(k&7) of the 8 at `-t*64` | " | " |
| 2 bitmap | `vram + row*pitch` (u32 product) | u16 at `+2k` | bit 15 | the u16 (bit 15 set) |
| 6 bitmap hflip | same (vram points at the last pixel) | u16 at `-2k` | " | " |

Tiled sprites are processed in whole tiles of 8 pixels (`((width-1)>>3)+1` tiles); width is always a multiple of 8.

Affine (kinds 8, 9, 10), per line:
```
dy = line - E.y; if dy < -192: dy += 256                 (boxes that wrap past line 255)
xs = E.xstart + dy*E.xstep;  ys = E.ystart + dy*E.ystep  (s64)
first = max(xs>>32, ys>>32, 0);  last = min((xs+E.xlen)>>32, (ys+E.ylen)>>32);  if last >= width: last = width-1
if last - first + 1 <= 0: nothing
tx = (s16)(E.tx0 + dy*PB + first*PA);  ty = (s16)(E.ty0 + dy*PD + first*PC)      (all mod 2^16)
for p = first .. last (screen x = E.x + p), then tx += PA, ty += PC (16-bit wrap):
    8  4bpp:   off = (u16)(((tx>>11)<<5) + (ty>>11)*pitch + ((ty>>8)&7)*4 + ((tx>>9)&3));  index = (vram[off] >> (((tx>>8)&1)*4)) & 15
    9  8bpp:   off = (u16)(((tx>>11)<<6) + (ty>>11)*pitch + ((ty>>8)&7)*8 + ((tx>>8)&7));   index = vram[off]
    10 bitmap: off = (u16)(((tx>>8)*2) + (ty>>8)*pitch);                                  c = u16 at vram + off
```
(`>>` arithmetic on the s16 coordinates; `off` is a u16, so the fetch is at most 64 KiB past E.vram, never
before it.) Opaque and colour as above. The offsets are computed 8 or 16 at a time in NEON when the span is longer
than 7 (8bpp/bitmap) or 15 (4bpp) pixels; the results are identical to the scalar tail. [M]

### 4.3 What the line produced looks like [M]

| output | format | content |
|---|---|---|
| `col` = S+0xa70 (in the 0x220-byte layer buffer S+0xa60: 8 + 256 + 8 u16) | u16[256] | raw colour of the frontmost opaque OBJ pixel: the lowest priority number, then the lowest OAM index; OBJ-window sprites are drawn first and overwritten. Pixels no OBJ covers keep their old (stale) value. Raw palette u16 (bit 15 as stored) or bitmap u16 (bit 15 = 1) |
| `alpha` = S+0xc90 | u8[256] | the attribute byte of that OBJ: 0 normal/window, 0x80 semi-transparent, 2a+1 bitmap; then `&= 0x3f` over all 256 bytes when the line flag is set. Uncovered pixels keep stale values |
| `vis` + p*32 = S+0xe20, S+0xe40, S+0xe60, S+0xe80 (p = 0..3) | 4 x 32 B | bit i = some opaque OBJ of priority p covers pixel i (a pixel can be set in several slots) |
| S+0xea0 | 32 B | the OBJ window: opaque pixels of mode-2 OBJs (any priority) |
| `semi` = S+0xec0 | 32 B | bit i = alpha[i] bit 7 (frontmost OBJ semi-transparent); 0 if the line flag is clear |
| `bmp` = S+0xee0 | 32 B | bit i = (alpha[i] & 0x3f) != 0 (frontmost OBJ is a bitmap OBJ); 0 if the line flag is clear |
| w0 | u32 | 0x10 if any of the 5 lists (window included) has an entry on this line, else 0; ORed into the layer mask |

The stale values are never consumed: every consumer masks the colour and alpha with the OBJ masks of this line (5).
The semi/bmp bitmaps are built from all 256 alpha bytes, so they can have stale bits outside the OBJ pixels; they
too are always ANDed with "OBJ is the top layer".

### 4.4 Code map of render_scanline_obj_c (0x36b70..0x3862c) [D]

| range | what |
|---|---|
| 0x36b70..0x36bf0 | prologue: `x21` = list-4 base - 1 (`eng+0x1af7f+line*0x80`), `x22` = `vis+0x80`, `[sp+0x88]` = `eng+0x21280+line` (list-4 count), callee-saved x19-x28, d8-d11 |
| 0x36bf4..0x36c24 | list loop (count pointer -= 0xc0, list base -= 0x6000, output -= 0x20); an empty list stores 32 zero bytes |
| 0x36c28..0x36c3c | line flag `[eng+0x21340+line]`: 0 -> 0x38578 (zero semi/bmp), else the alpha pass |
| 0x36c40..0x36f94 | alpha pass, NEON (2 x 128 px), after an overlap test; scalar twin 0x38594..0x38608 |
| 0x36f98..0x36fd8 | stack-guard check, epilogue |
| 0x36fdc..0x37014 | per non-empty list: `memset(sp+0x258, 0, 256)`, x5 = last list entry |
| 0x37018..0x37064 | per OBJ: load E fields (x 0x46, kind 0x52, width 0x55, pitch 0x44, attr 0x53), dispatch on kind |
| 0x37068..0x371c4 | kind 4 (4bpp hflip); 0x371c8 next OBJ (x5 -= 1 until x21) |
| 0x371d4..0x37470 | buf -> 32-byte mask (NEON nibble/bit folds), `w0 = 0x10` |
| 0x37474..0x375e8 | kind 1 (8bpp); kind 0 at 0x383c4 |
| 0x375ec..0x379b4 | kind 10 (affine bitmap): span, offsets (NEON 8/iter 0x376d4, scalar 0x377f8), draw 0x37958 |
| 0x379b8..0x37d74 | kind 6 -> 0x37d78; kind 8 (affine 4bpp): span, offsets + nibble shifts (NEON 16/iter 0x37aa4, scalar 0x37cb8), draw 0x37d04 |
| 0x37d78..0x37de0 | kind 6 (bitmap hflip) |
| 0x37de4..0x3824c | kind 9 (affine 8bpp): span, offsets (NEON 8/iter 0x37ec0, scalar unrolled 0x38018), draw 0x381e8 |
| 0x38250..0x383c0 | kind 5 (8bpp hflip) |
| 0x383c4..0x38510 | kind 0 (4bpp) |
| 0x38514..0x38574 | kind 2 (bitmap) |
| 0x38578..0x38590 | no semi/bitmap OBJ on the line: zero the two masks |
| 0x3860c..0x38624 | short spans: enter the scalar offset loops at index 0 |

Kinds 3, 7 and > 10 fall through to "next OBJ" (they cannot be produced by the reorder).

---

## 5. How OBJ priority and alpha reach the compositor

Read in `render_scanline_2d` 0x3ef00 and `render_scanline_2d_composite` 0x3c6d0. [D] (The compositing internals
are in `re/compositing-2x.md`.)

- **Priority.** `video_2d_reorder_layers` builds `[eng+0x84..]`: for p = 0..3, the OBJ slot `4|p` (only if
  DISPCNT.12) then the enabled BGs of priority p. The priority encoder walks that list; an OBJ slot claims the pixels
  of `vis[p]` not yet covered, all OBJ slots merge into one OBJ mask (`spec/composite.c`). The colour is then taken
  from the single OBJ line `S+0xa70` (layer table entry 4 = `S+0xa60`), which holds the frontmost OBJ. So OBJ beats a
  BG of the same priority, and OBJ-vs-OBJ follows section 0.5.
- **Layer mask**: the return value (0x10) sets bit 4 of the layer mask used by `disable_blank_layers`,
  `select_pixels` and the BLDCNT masking (`BLDCNT & (0xf0f0 | m | m<<8)`, which keeps the OBJ target bits anyway).
- **Composite flags**: `flags = [eng+0x21340+line]` (bit 0 semi-transparent OBJ, bit 1 bitmap OBJ), then: bit 0 is
  cleared when BLDCNT has no second target among the enabled layers / OBJ / backdrop; BLDCNT mode 1 with first and
  second targets adds 4; mode 2/3 with first targets and BLDY != 0 adds 8; `set_3d_visibility` ORs in 2 (translucent
  3D) or 0x10; the full-screen OBJ image adds 0x20 (0x3f0a0..0x3f124, 0x3f934). Any of bits 0-2 selects the complex
  composite path.
- **Alpha plane**: when `flags & 2`, `S+0xc90` is passed as the composite's `alpha` plane (1x; at 2x a per-quarter
  copy `S+0xfc0` for q = 0..2 and the original for q = 3), so `select_pixels_binary32_alpha` can overwrite it with
  the 3D alpha where BG0 (3D) is the top layer (0x3f654, 0x3ff74).
- **Semi-transparent OBJ** (complex path, `flags & 5` = 1 or 5): blend mask = `(semi & OBJtop) | firstTarget` (5) or
  `semi & OBJtop` (1, then treated as alpha mode), ANDed with the second-target mask; EVA/EVB from BLDALPHA
  (0x3cd34, 0x3cdd8). A semi-transparent OBJ pixel is a first target regardless of BLDCNT's OBJ bit.
- **Bitmap OBJ alpha** (`flags & 2` and an alpha plane): mask = `OBJtop & bmp & secondTarget` (plus BG0top &
  secondTarget for the 3D when BLDCNT mode is not 2/3), then `color_effects_setup_alpha` gives per pixel EVA = a+1,
  EVB = 31-a out of 32, with a = 2*alpha+1: EVA = 2(alpha+1), EVB = 2(15-alpha), the hardware's (alpha+1)/16 and
  (15-alpha)/16 (0x3c994..0x3ca80). Without a second target the bitmap pixel is drawn opaque.
- With the full-screen image (flag 0x20), OBJ-top pixels are removed from the BLDALPHA blend mask (0x3ccdc).
- **OBJ window**: `render_scanline_generate_window_masks(eng, S+0xf00, S+0xfa0, S+0xea0, layers, line)`: with
  DISPCNT.15 the OBJ window is always "vertically active" (bit 2 of the state), its mask is the 32 bytes at S+0xea0
  and its layer enables are WINOUT bits 8-13 (`[eng+0x9c] >> 24`, XOR 0x3f) (0x3b414..). It is combined with the
  WIN0/WIN1 masks (`eng+0x44`, `eng+0x64`) by `inhibit_masks_single/double/triple`; that these give the hardware
  order WIN0 > WIN1 > OBJ window > outside is a [G] (window code not analysed here).
  Hazard: `render_scanline_obj_c` is called only when DISPCNT.12, and nothing clears S+0xea0 otherwise, so with the
  OBJ window on and OBJ display off the window mask is stale stack data. [D] (Exact reproduction is impossible; a
  replacement should pick a rule, for example an empty mask, and accept the difference.)

---

## 6. 2x ("hires") behaviour and capture

- `render_scanline_obj_c` runs once per DS line at 256 pixels whatever the resolution [D]. In the 2x path of
  `render_scanline_2d` the same colour line, alpha plane, vis slots and window mask are used for all four quarters,
  so each OBJ pixel covers 2x2 output pixels; affine sprites are sampled at 1x. The alpha plane is copied per quarter
  (`memcpy(S+0xfc0, S+0xc90, 256)`, 0x3ff80) only because the 3D alpha is written into it.
- The 2x path is entered for OBJ only through the **full-screen image**: `H` bit 4 is `[eng+0x21400] != NULL`
  (0x3f02c). Per quarter: q = 0 uses the 1x VRAM line `[eng+0x21400] + line*512`; q = 1..3 use
  `[eng+0x21408] + (line*768 + (q-1)*256)*2` when the hi-res data exists, else the 1x line (0x3fa40..0x3fa64; M in
  situ on T9: one 1x composite for three hi-res ones per image line). The
  bitmap `bit i = line[i] >> 15` is **stored over** the vis slot `4 + [eng+0x21410]` (the sprites of that priority
  vanish from it), and layer table entry 4 becomes `line - 0x10`, so the OBJ colour of *every* OBJ-top pixel (also
  those of other sprites at other priorities) comes from the image line (0x3f124..0x3f3b4 at 1x, per quarter at 2x).
  Normal sprites still render into S+0xa70, unused. [D; the composite inputs M in situ, section 0 top] Visible in
  the T5 dumps (`re/2d/obj/f212_230.png`, from `dumpT5/a00212.ppm` and `a00230.ppm`): with the image detected, the
  moving 32x32 sprites and the sprite-drawn title take the image's colours (a square of image stripes cuts into the
  BG circle); in the "broken" frames (one image sprite moved by a pixel, no detection) they show their own pixels.
- Display capture works on the composited planes; OBJ has no special case there. The hi-res capture buffers are
  the source of `[eng+0x21408]` (validity bits `C+0x20+bank`, set after a captured frame, cleared by CPU stores).

---

## 7. DraStic behaviours an exact replacement must reproduce

| behaviour | where |
|---|---|
| OBJ ordering: priority number first, then OAM index (not OAM index first) | 4.1 |
| no OBJ mosaic; no per-line limit; DISPCNT.23 ignored | 0 |
| mapping/ext-palette bits latched at reorder time (line 0, next OAM write, savestate load) | 0.7 |
| tile-granular clipping, colour/alpha written into the line padding (harmless) | 2.3 |
| 32.32 affine spans: an extra left pixel / a missing right pixel on lines above the centre when the edge is an integer; the `PA==0` / `PC==0` case draws pixel 0 on the line before the visible range | 3 |
| affine texture coordinates in 16 bits, texel offsets wrapped to u16 (extra pixels read real VRAM data) | 4.2 |
| 8bpp 2D-mapped tiles ignore tile bit 0; bitmap 1D wins over the 256-wide bit | 2.4 |
| sprites whose box crosses line 255 wrap to the top (registration `& 0xff`, `dy += 256`) | 2.3, 4.2 |
| identity matrices drawn as plain sprites (double size: centred, unscaled) | 2.3 |
| the 12-sprite full-screen image: OBJ-0-only prio/base rule, last-matching bank, overwritten vis slot, layer source replaced for all OBJ pixels, OBJ excluded from BLDALPHA | 2.6, 6, 5 |
| alpha-0 bitmap OBJs not registered; semi-transparent flag cleared without a second target | 2.3, 5 |
| OBJ window + OBJ off: stale mask (choose a rule) | 5 |

---

## 8. Costs (instructions, measured)

In the emulator (fnprof plugin, `rast/prof.sh`, ds2d scenes of the 14:58 build, 30 s, about 1130 frames each, 2x
mode; `re/2d/obj/prof-T4.rep`, `prof-T5.rep`):

| scene | frame total | render_scanline_obj_c | video_2d_reorder_obj | setup_edges |
|---|---|---|---|---|
| T4 tile sprites | 2.32 M | **0.645 M** (27.7%, 191 calls) | 0.040 M (2 calls) | - |
| T5 affine/bitmap/semi | 2.63 M | **0.679 M** (25.8%, 191 calls) | 0.024 M (2 calls) | 0.001 M (28 calls) |

DraStic's own code on controlled inputs (`objharness bench`, fnprof; `h/bench_all.sh`), 64x64 sprites, per line and
per sprite row (one row = one OBJ on one line):

| item | instructions |
|---|---|
| line without any OBJ entry | 128 (five empty lists, two empty bitmaps) |
| line with entries (one list) | ~310 + a libc memset of 256 B (a few instructions); each further non-empty list ~180 |
| line with a semi-transparent or bitmap OBJ registered | +~400 (the alpha-plane pass) |
| 4bpp row, 64 px, all / half opaque | 543 / 401 (about 2 per transparent, 7 per opaque pixel, 6 per 8-px tile) |
| 8bpp | 594 / 473 |
| 4bpp hflip / 8bpp hflip | 594 / 452, 603 / 482 |
| bitmap | 671 / 544 |
| affine 4bpp / 8bpp / bitmap, 64x64 at 30 deg (box 64, 54 px drawn a row) | 1339 / 1059 / 963 (all opaque): about 25 / 20 / 18 per drawn pixel |
| affine 4bpp double size (box 128, 32 px drawn a row) | 804 per box row |
| reorder, per call | ~2000 (128 entries scanned, the memsets) + ~100 per visible OBJ + ~16 per line of its box; affine OBJs +~50, plus 2 x setup_edges of ~26 each (3 64-bit `sdiv` each, slow on the A55) |

The reorder runs per frame per rendered engine, and again before any line that follows a replayed OAM write: a game
that writes OAM every line during the display pays a full re-sort (tens of thousands of instructions) per line.

---

On the A55 at 600 MHz the 0.65 M instructions of T4 are roughly 1 ms of the emulation thread per frame (at about
one instruction a cycle) [G].

### 8.1 Where a replacement can win (exact)

- **Move it off the emulation thread**: the OBJ line depends only on the tables, VRAM, palette and the line number
  (section 10), so engine A's OBJ lines can be produced by another core from a frame snapshot, like the rest of the
  deferred 2D engine; nothing in it depends on the 3D layer.
- **Per-tile work in NEON**: a 4bpp tile row is one u32; nibble split (`ushr`/`and`), `cmtst` for opacity, `tbl` on a
  16-entry palette (two 16-byte tables for the u16 colours), and `bit` inserts into the colour/alpha rows give 8
  pixels in roughly 12-16 instructions [G: estimate, not written] instead of DraStic's 22..62; 8bpp needs a scalar palette fetch per opaque pixel (256-entry
  palette) or a 4-register `tbl` pass. The per-list index buffer + 150-instruction mask packing can become a direct
  OR into the 32-byte mask (one `cmtst` + bit-weight `addp` per 8 pixels).
- **Affine**: keep DraStic's 32.32 span (it is cheap: two multiply-adds a line) and its 16-bit coordinate stepping;
  vectorise the offset computation (DraStic already does) and the opacity/colour selection; the texel loads stay
  scalar (no gather).
- **Reorder**: rebuild only the OBJs whose OAM bytes changed (an OAM event names the byte), instead of all 128 per
  OAM write; keep the exact registration semantics (2.3).

## 9. Hook points (16-byte `ldr x16,#8; br x16; .quad` patch)

| function | addr | first 4 words | PC-relative in the patch? | args |
|---|---|---|---|---|
| video_2d_reorder_obj | 0x3e530 | a9b27bfd aa0003e1 910003fd a90153f3 | none: trampoline OK | x0 eng |
| render_scanline_obj_c | 0x36b70 | d10dc3ff 2a0603e9 910d6126 b0000947 | adrp x7 (4th): rewrite it as `ldr x7, =base+0x15f000` in the trampoline (objdiff.c does this) or replace fully | x0 eng, x1 col, x2 alpha, x3 vis, x4 semi, x5 bmp, w6 line -> w0 |
| video_2d_obj_affine_setup_edges | 0x3e390 | 7100003f 350002a1 7100007f 340005a3 | cbnz/cbz (2nd, 4th): replace fully | w0 t0, w1 dA, w2 lim, w3 dB, x4 start*, x5 step*, x6 len* |
| render_scanline_obj_c call site | 0x3f920 | `bl` | - | the only caller |

`render_scanline_obj_c`'s callee-saved use: x19-x28, d8-d11 (saved). The only other readers of the OBJ tables are
`render_scanline_2d` (line flags `eng+0x21340`, full-screen image `eng+0x21400/08/10`); nothing else touches
`eng+0x380..0x21418` (event log aside), and savestates do not store them (load rebuilds them). So a replacement of
both routines may keep its own private tables, but must keep those two fields in DraStic's layout as long as
DraStic's `render_scanline_2d` runs. [D]

---

## 10. What a replacement needs

Inputs (per engine):

| input | read when (DraStic) | notes |
|---|---|---|
| OAM 0x400 B at `[eng+0x30]` | reorder: line 0 and after replayed OAM writes | line-exact through the event log; affine groups are OAM too |
| DISPCNT bits 4, 5, 6, 20-22, 31 | reorder | latched (0.7) |
| DISPCNT.12 (OBJ on), .15 (OBJ window) | per line, render_scanline_2d / window code | |
| engine index `[eng+0xb7]` | reorder | OBJ base 0x400000 / 0x600000 in the alias |
| VRAM alias `[eng+8]` | per pixel at render time | live bank memory, mapping as of the render call |
| palette `[eng+0x18]+0x200` | per pixel at render time | line-exact via the replay |
| OBJ ext palette `[eng+0x28]` | pointer at reorder, contents at render time | 16 x 256 colours |
| video `[eng]`: bank records `+0x10+b*16`, `C+0x20+b`, `C+b*8` | reorder, full-screen image only | |

Outputs per line: the seven items of 4.3 (colour line and alpha plane need to be exact only where an OBJ mask bit
is set; the four priority masks, the OBJ window mask, semi/bmp masks within the OBJ pixels, and the return value
exactly), plus the per-line flag byte and the full-screen image fields of 2.1 for `render_scanline_2d`.

Threads: engine B's OBJ runs on `video_render_thread` while engine A's runs on the emulation thread (normal
frames), so all replacement state must be per engine. [D, re/2d/frame.md 2]

State: none carried between lines except the tables (which depend only on OAM, DISPCNT bits and the bank records);
everything else is recomputed per line.

| state | lifetime | changes when |
|---|---|---|
| OBJ records, lists, counts, line flags, image fields (`eng+0x380..0x21418`) | per frame | `start_frame` (line 0), the first rendered line after a replayed OAM write, savestate load |
| the seven per-line outputs (S area), the 256-byte index buffer | per line | every `render_scanline_obj_c` call (stale parts are never consumed) |
| texels, palette and ext-palette colours | per pixel, at render time | CPU/DMA writes to VRAM (any time), palette events (per line) | A per-frame snapshot for a deferred renderer is therefore: OAM at line 0
plus the OAM events, DISPCNT, the palette and its events, VRAM (shared with the BG renderer's snapshot) and the
ext-palette pointer.

Reference implementation: `re/2d/obj/h/obj_model.c` (`m_reorder_obj`, `m_setup_edges`, `m_render_obj`): DraStic's
exact semantics on DraStic's own data layout, checked as described at the top. Rerun:
`objharness <drastic> edges|frame <n> <seed>` under `qemu-aarch64 -L rtsys`; `objdiff.so` with
`LIB=$S/rast/librast.so:$H/objdiff.so sh $S/rast/run.sh <rom> <secs> ours`.

## 11. Open questions

1. What `[video+0x10+b*16] == 6` means exactly (the full-screen image requires it; `re/2d/frame.md` 15.2 has the same
   question). Observed: bank B mapped as engine-A OBJ at offset 0 (VRAMCNT_B = 0x82) satisfies it (T5/T9 found the
   image); the other mapping kinds were not tested.
2. The hardware OBJ window rule when DISPCNT.12 is clear (DraStic uses stale data).
3. Whether the full-screen image's vis-slot *overwrite* (rather than OR) ever matters: the in-situ check compares the
   slot with the image's bit-15 mask, but the test image is opaque everywhere, so an OR with same-priority sprite
   bits would give the same bytes there. The disassembly (`str`, 0x3f298/0x3f3a0) says overwrite.
4. Rounding: the 32.32 edge artefacts of section 3 happen about once per 10 000 affine pixels in random tests; how
   visible they are in games was not measured (exactness requires them anyway).
