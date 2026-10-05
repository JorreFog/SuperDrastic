# DraStic r2.5.2.2: hi-res ("4x" = 2x) scanline compositing

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. Its C reference of the 3D visibility step is `src/rast/spec/composite.c`, and the
> simple composite path is in `src/rast/spec/2d/compose.c`; `re/verify/vis_model.py` was not kept. The 2D engine's
> analyses are next to this file.

Scope: `render_scanline` (0x404a0) and the per-line helpers that composite the 2D layers with the 3D frame at
512x384. Sources: `re/drastic.dis` (llvm-objdump), first instruction words read from the binary, per-call
counts derived from the per-frame instruction totals you measured (light 3D scene). One semantic claim was
checked by machine: an interpreter for the NEON body of `render_scanline_set_3d_visibility` (built from the
disassembly lines) matches the C reference in A.1 on 400 random inputs (`re/verify/vis_model.py`).

Offsets: `eng` = a 2D engine struct (engine A = video+0x2e78, index 0, has the 3D layer; engine B =
video+0x84298, index 1; `[eng+0xb7]` holds the index, set by `initialize_video_2d` 0x43664). `sys = [eng+0]`,
`cfg = [sys+8]` (`cfg+0x4a0` hires_3d, `cfg+0x468` threaded_3d).

---

## 0. Corrections to the premise

1. **The 3D screen is not composited on `video_render_thread`.** `update_frame` (0x30dc0) is called from
   `event_scanline_start_function` (line 192) on the emulation thread. It calls `video_render_scanlines(video,
   191)` (0x30df0). When the frame had no mid-frame catch-up (`[video+0x458894] == 0`), that function
   (0x30cbc..0x30d4c) signals `video_render_thread` to run **engine B** (`video_2d_render_scanlines(video+0x84298,
   0, 191, NULL)`, 0x2fab8). It runs **engine A** (the 3D screen) itself on the **emulation thread**
   (`video_2d_render_scanlines(video+0x2e78, start, 191, video+0x458820)`, 0x30d04), then waits on the
   `[+0x961]` flag. So the expensive 3D-screen composite runs on the emulator's critical path. The frame-end
   phase lasts max(A, B), and the emulation thread is blocked for all of it. If the frame had a mid-frame
   catch-up (triggered from `event_hblank_start_function` 0x1d65c..0x1d950: DISPCNT display mode 2/3, a flag at sys+0x3b2a9a0 that was not identified, DMA display), both
   engines render on the emulation thread, line by line.
2. **What "768 calls" means.** The per-quarter helpers (`set_3d_visibility`, `gather_3d_alpha`,
   `2d_composite`, `select_pixels*`, `expand_6bit_split`, `priority_encode_single`) run 4 times per DS line,
   for engine A only (192 x 4 = 768). Only `color_convert_direct_32_2x` runs for both screens (2 rows x 192
   lines x 2 engines = 768). In the measured scene engine B is in display mode 0: `render_scanline` memsets
   white planes and converts them, and `render_scanline_2d` is never called for B. That is why there are 192
   calls of `render_scanline_2d` and 384 calls of `render_scanline`.
3. Several asm entry points have separately named internal labels (`priority_encode_single_bg_layer`,
   `_complete`, `_obj_layer`, `render_scanline_select_pixels_binary32_alpha`). Your per-symbol totals for those
   entries cover only the head block. For example, 0.009 M / 768 = 12 is exactly the 12-instruction head of
   `priority_encode_single_asm`.

---

## 1. Call structure

### 1.1 Frame level

```
event_scanline_start_function (line 192)            emulation thread
  update_frame 0x30dc0
    video_render_scanlines(video, 191) 0x30be0
      [no catch-up] signal video_render_thread  ->  video_2d_render_scanlines(engB, 0, 191, NULL)   (render thread)
                    video_2d_render_scanlines(engA, 0, 191, capture=video+0x458820)                  (emu thread)
                    wait for engine B
event_scanline_start_function (line 215): 3D for the next frame
    threaded_3d=0: update_frame_3d (synchronous, bins on the 3 bin threads)
    threaded_3d=1: video_3d_start_rendering; update_frame later calls video_3d_finish_rendering, which
                   publishes [sys+0x34eb58] -> [sys+0x34eb60]; render_scanline_3d reads +0x34eb60 when threaded
```

`video_2d_render_scanlines(eng, first, last, capture)` 0x42a90: for each line it calls
`render_scanline(eng, [eng+0x38] + line*[eng+0x40], line, capture)` (0x42b38), then replays the logged
register writes for that line (BGxCNT, BLDCNT `+0xa0`, BLDY `+0xa2`, BLDALPHA `+0xa4`, MASTER_BRIGHT
`+0xa6`, ...).

The scanout is `[eng+0x38]`, from `get_screen_ptr`. Under dsflip it is a DRM dumb buffer (XRGB8888).
The pitch `[eng+0x40]` comes from `get_screen_pitch` 0x8aa40: (3*hires+1)*bpp*256 = **0x1000 per DS line**
at 2x/32 bpp. DS line L writes rows 2L and 2L+1 at `+0` and `+pitch/2` (0x800 = 512 px x 4 B).

### 1.2 `render_scanline(eng x0, out x1, line w2, capture x3)` 0x404a0

Its stack frame is 0x1aa0 bytes. It holds `planes = sp+0x290` (0xC00 B): four **quarter plane sets** of 0x300
B. Each set is `R6[256] G6[256] B6[256]`: u8, 6-bit values, planar.

- `bpp = get_screen_bytes_per_pixel()`, `hires = [cfg+0x4a0] & 1` (w28).
- Line 191: catch up the DMA display FIFO (`dma_transfer_display`).
- Special case 0x40510: engine A with `[cfg+0x4a4] != 0` (a word that is not in the menu or in
  `drastic.cfg.base`). It runs `render_scanline_3d` and converts the 3D line straight to XRGB, with no 2D at
  all (0x40ad8, `st2` interleave). Your runs do not take this path, because `set_3d_visibility` runs.
- Display mode `DISPCNT[17:16]` (w27):
  - **1 (graphics):** `r = render_scanline_2d(eng, planes, line, capture, hires)` (0x4097c). If r != 0 (2x
    quarters were produced), the convert sources are `q0=planes, q1=planes+0x300, q2=+0x600, q3=+0x900`.
    Otherwise all four are `planes` (the 1x line, pixel-doubled by the convert).
  - **0 (off):** `memset(planes, 0xff, 0x300)` (white), with all four sources = `planes`.
  - 2 (VRAM display) / 3 (main-memory FIFO): `expand_6bit_split` of VRAM/FIFO data, plus hi-res capture
    quarters (0x4120c).
- Convert (0x4067c). MASTER_BRIGHT is `[eng+0xa6]`, mode = bits 15:14, f = (MB&0x1f)*2. At 32 bpp and 2x:
  - mode 0, or f == 0: `color_convert_direct_32_2x(q0, q1, out)` and `(q2, q3, out + pitch/2)` (0x406bc / 0x406d0).
  - modes 1/2: `color_convert_shade_32_2x_asm(qa, qb, dst, 32-f, 63f+16 | 16)`. If f > 31, memset white/black.

### 1.3 `render_scanline_2d(eng x0, planes x1, line w2, capture x3, hires w4)` 0x3ef00

The stack frame is 0x1d30 bytes. Scratch `S = sp+0x180` (x25) holds:

| S + | contents |
|---|---|
| 0x1e0, 0x400, 0x620, 0x840, 0xa60 | layer line buffers BG0..BG3, OBJ: 0x220 B each = 8 + 256 + 8 u16 (BGR555, bit 15 = opaque) |
| 0x000..0x3ff | BG0-HOFS-shifted 3D quarter (`render_scanline_horizontal_shift_3d`) |
| 0xc90 | OBJ alpha plane (u8[256]); a copy at 0xfc0 per quarter when 3D is translucent |
| 0xda0 | **visibility bitmaps**, 32 B (256 bits, LSB-first) per slot: 0..3 = BG0..BG3, 4..7 = OBJ by priority |
| 0xea0 / 0xf00 / 0xfa0 | window masks |
| 0x10c0 | priority-encoder output: exclusive masks, 32 B per slot: BG0..BG3, OBJ (+0x80), backdrop (+0xa0) |
| 0x1180.. 0x1b90 | complex-path scratch (second layer, blend masks, alpha/EVA/EVB planes) |

Order of work per DS line:
1. If `DISPCNT.3` (BG0 is 3D) and BG0 is enabled: `[sp+0xc0] = render_scanline_3d(sys, line)` (0x3f8e4). At 2x
   this returns `frame + line*0x1000`.
2. `render_scanline_bg(eng, S+0x1e0, vis=S+0xda0, line)` (1x BGs) and, if OBJ is enabled, `render_scanline_obj_c`
   (1x OBJ). Then `render_scanline_generate_window_masks`.
3. Hi-res-capable layers `H` (w27): bit0 = BG0 when it is 3D and hires; bit2 = BG2 if `[eng+0x240]` (direct-colour
   bitmap BG2); bit3 = BG3 if `[eng+0x2f0]`; bit4 = OBJ if `[eng+0x21400]` (full-screen 16-bit OBJ image, priority
   `[eng+0x21410]`). **The 2x path is taken iff `hires && (enabled_layers & H)`** (0x3f0e4).
   - **1x path** (0x3f0f8): `set_3d_visibility(S+0xda0, 3d_line)` once, the OBJ-image bitmap, windows,
     `disable_blank_layers`, then one `render_scanline_2d_composite` into `planes` (0x3f69c). Returns 0.
   - **2x path** (0x3f95c): windows for the non-hi-res layers once, `disable_blank_layers`, then
     **for q in 0..3** (x21; loop end 0x3ffe8):
     - `p3d = 3d_line + q*0x400` (0x3f9fc). If BG0HOFS != 0, `horizontal_shift_3d(S, p3d, hofs)` and `p3d = S`.
     - `flags_q = flags_line | set_3d_visibility(S+0xda0, p3d)` (0x3fa14).
     - Hi-res OBJ / BG2 / BG3 quarter data: `[eng+0x21408]`, `[eng+0x248]`, `[eng+0x2f8]` hold 3 x 256 u16 per line
       for q = 1..3 (hi-res capture data); q = 0 uses the 1x VRAM line. Their bit-15 visibility bitmaps are
       built inline (`ld4` / `ushr` / `xtn` blocks 0x3fa94, 0x3fd20).
     - `apply_windows` for the hi-res layers. With windows off it is a 4-instruction early-out.
     - `render_scanline_2d_composite(eng, planes + q*0x300, S, layer_table, p3d, alpha?, layers, BLDCNT,
       [sp]=flags_q, [sp+8]=line)` (0x3ffd4; 0x3f8a8 for q = 3).
     - After q = 3: hi-res display capture (`render_scanline_capture_blended` per quarter) if capture is active.
       Returns the hi-res mask (non-zero).

**Note:** a direct-colour bitmap BG2/BG3 (`[eng+0x240]` set) forces the 4-quarter path even when no hi-res
data exists (`csel` at 0x3fd0c picks the 1x line). That is 4 identical composites per line, on engine B too.

### 1.4 `render_scanline_2d_composite(eng, out, S, layers, p3d, alpha, lmask w6, bldcnt w7, flags, line)` 0x3c6d0

- **Simple path**, `(flags & 7) == 0` (0x3cab8): `priority_encode_single_asm(eng, S+0xda0, S+0x10c0)`.
  If `flags & 8` (BLDCNT brightness mode with factor != 0): `select_pixels` into S+0x1180,
  `select_blend_enable`, then tail-call `shade_asm`. **Otherwise tail-call
  `select_pixels(eng, out, S+0x10c0, layers, p3d, NULL, lmask)`** (0x3cc0c). This is the path in your profile.
- **Complex path**, flags bit0 (semi-transparent OBJ), bit1 (translucent 3D) or bit2 (BLDCNT alpha mode):
  `priority_encode_double` (top and second layer), two `select_pixels` (the first with the alpha plane, so
  `select_pixels_binary32_alpha` stores the 3D alpha), `select_blend_enable` x2, `color_effects_setup_alpha`
  (per pixel EVA = a+1, EVB = 31-a) or `_setup_blend`, then `color_effects_apply(_offset)`.

### 1.5 `render_scanline_select_pixels(eng, out, excl, layers, p3d, alpha, lmask)` 0x39330 (C)

1. For each set bit of `lmask`, in layer order: the first layer's u16 line becomes the source. Each later one is
   merged with `select_pixels_binary_asm(tmp, src, layer, excl[idx])`.
2. If any layer was found: `select_pixels_binary_scalar_asm(tmp, src, backdrop=*(u16*)[eng+0x18], excl[5])`.
   Otherwise `tmp` is filled with the backdrop.
3. `expand_6bit_split_asm(out, tmp)`: 256 u16 -> 6-bit R/G/B planes.
4. If `p3d && (lmask & 1)`: `select_pixels_binary32_asm(out, alpha, p3d, excl[0])` writes the 3D pixel's bytes
   into the planes wherever BG0 is the top layer.

In the measured scene `lmask` = BG0 only. So `tmp` = BG0's u16 buffer (garbage, because BG0 is 3D) with the
backdrop where 3D alpha is 0. All of it is expanded, and then 3D overwrites nearly all of it.

---

## 2. Hot helpers

Per-call counts come from your per-frame totals / 768 (render_scanline_2d / 192; render_scanline / 384).
"px" = pixels processed per call.

| helper | addr | per call | px | instr/px | loop width |
|---|---|---|---|---|---|
| set_3d_visibility (self) | 0x3c2c0 | 382 | 256 | 1.49 | 128 px, 2 iterations of 166 |
| gather_3d_alpha_asm | 0xa0a48 | 43 | 256 | 0.17 | 64 px (4 x ld4) |
| color_convert_direct_32_2x_asm | 0xa0ae0 | 361 | 512 out | 0.70 | 32 out px, 22 instr |
| expand_6bit_split_asm | 0xa0818 | 229 | 256 | 0.89 | 32 px, 28 instr |
| select_pixels_binary32_asm | 0xa0730 | 178 | 256 | 0.70 | 32 px, 21 instr (alpha variant 25) |
| select_pixels_binary_scalar_asm | 0xa0560 | 72 | 256 | 0.28 | 32 px: 16 instr, or 8 when the mask word is 0 |
| select_pixels (C, self) | 0x39330 | 70 | - | - | glue |
| 2d_composite (self) | 0x3c6d0 | 42 | - | - | glue |
| priority_encode_single_asm | 0x9ffc0 | 12 head + 10..13 per layer + 7 | 256 | ~0.12 | whole line in 2 Q regs per layer |
| render_scanline_2d (self) | 0x3ef00 | 490 / DS line | - | - | |
| render_scanline (self) | 0x404a0 | 120 / DS line | - | - | |

What each computes:

- **set_3d_visibility(u8 bits[32], const u32 px[256]) -> flags.** Calls gather (alpha byte -> 256 B on its
  stack), then folds bytes into bits with about 83 vector ops per 128 px (`eor 0x1f`, nibble folds, `>>7`,
  `>>14`, `xtn`). Verified semantics (A.1): **bit i = (byte3(px[i]) != 0)**. Return value: **2** if some pixel
  has byte3 not in {0, 0x1f} (translucent); **0x10** if some pixel is visible and all visible pixels are 0x1f;
  **0** if nothing is visible. The caller ORs it into the quarter's flags.
- **gather_3d_alpha(u8 a[256], const u32 px[256]):** `a[i] = px[i] >> 24` (four `ld4 .16b`, store the 4th register).
- **color_convert_direct_32_2x(planes E, planes O, u32 dst[512]):**
  `dst[2i+p] = (B_p[i]<<2) | (G_p[i]<<2)<<8 | (R_p[i]<<2)<<16 | junk<<24`, where p = 0 takes E and p = 1 takes O
  (`zip1`/`zip2` + `st4`). Byte 3 comes from `movi v19.4h, #0xff`, which clears the upper half. So byte 3 =
  0xff when `x mod 16` is in {0,2,4,6} and 0x00 otherwise (the 1x version writes 0xff). It is harmless for
  XRGB scanout, but must be reproduced if output bytes are compared exactly.
- **expand_6bit_split(planes out, const u16 c[256]):** `R6 = (c&0x1f)<<1`, `G6 = ((c>>5)&0x1f)<<1`,
  `B6 = ((c>>10)&0x1f)<<1` (`xtn` / `shrn` / `shl` / `and 0x3e`).
- **select_pixels_binary32(planes out, u8 *alpha|NULL, const u32 px[256], const u8 mask[32]):** where mask
  bit i is set: `R[i] = byte0, G[i] = byte1, B[i] = byte2` (and `alpha[i] = byte3`). Uses `ld4` de-interleave,
  `ld2r` + `tbl` + `cmtst` to expand mask bits to bytes, and `bit` inserts. The 3D values are inserted as-is (r6/g6/b6).
- **select_pixels_binary_scalar(u16 *dst, const u16 *src, u16 colour, mask):** `dst = mask ? colour : src`.
  When dst != src it always copies, even for zero mask words.
- **priority_encode_single(eng, vis, excl):** walks the `[eng+0xb3]` layer slots in priority order
  (`[eng+0x84..]`): `excl[slot] = vis[slot] & ~covered; covered |= vis`. OBJ slots are merged into
  `excl[4]`. Then `excl[5] = ~covered` (backdrop).
- **2d_composite:** orchestration only (1.4). In the simple path it does no per-pixel work of its own.

---

## 3. Data flow: 3D frame -> scanout (2x, engine A)

```
our bin resolve -> 3D frame (u32 r6|g6<<8|b6<<16|a5<<24; DS line L at +L*0x1000;
                   quarter q = (row r=q>>1, x parity p=q&1) at +q*0x400, 256 px, pixel x = 2i+p)
 per DS line, per quarter q (emu thread):
   render_scanline_3d -> p3d = frame + L*0x1000 + q*0x400 (or a HOFS-shifted copy in S)
   set_3d_visibility:  alpha != 0 -> BG0 vis bitmap (S+0xda0);  0<alpha<31 anywhere -> flag 2
   [1x layers: the same u16 lines + vis bitmaps reused for all 4 quarters = 2x2 nearest-neighbour]
   priority_encode_single: BG0 is top where vis & ~(vis of layers earlier in the [eng+0x84] priority list)
   select_pixels: u16 merge of 2D layers + backdrop -> expand_6bit_split -> planes q
                  -> binary32 writes 3D r6,g6,b6 where BG0 is top
 per row r: color_convert_direct_32_2x(planes 2r, planes 2r+1) -> scanout row 2L+r (XRGB8888, chan<<2)
```

- **Where the 3D alpha is used:** (1) `set_3d_visibility`: a5 = 0 makes the 3D pixel transparent, so the layer
  below or the backdrop shows; a5 != 0 makes BG0 a candidate top layer. (2) The flags: any 1..30 in the quarter
  sends the whole quarter down the complex path. There the 3D alpha (stored by `binary32_alpha`) blends with the
  second target: EVA = a+1, EVB = 31-a, out of 32. a5 = 31 is opaque. Fog and edge marking are already in the
  RGB.
- **How 1x layers become 2x:** there is no resampling. The 2x path composites four 256-pixel quarter lines. The 1x
  BG/OBJ u16 lines and their bitmaps are reused unchanged in each quarter, so 1x pixel i lands on output pixels
  (2i, 2i+1) of both rows. Only the 3D layer (and hi-res capture BG2/BG3/OBJ data) differs per quarter. In the
  1x path the convert gets the same planes for both inputs and both rows (`x22, x22`), which again gives 2x2
  duplication.
- **Format chain per output pixel:** 3D u32 -> 6-bit planes (byte copy) -> `<<2` -> XRGB8888. 2D BGR555 ->
  `<<1` (6-bit) -> `<<2` = `c5<<3`.

---

## 4. Opportunities

### (a) Work the 3D bin threads can do instead

Only data that depends on the 3D frame alone can move, and that is exactly `set_3d_visibility` + `gather`
(0.326 M/frame, the largest item):
- For each hi-res row and parity half (768 per frame = 2 x 384 rows), compute the 32-byte bitmap
  `bit i = byte3 != 0` and the flag {0, 0x10, 2}, and keep them in a side table (24 KiB + 768 B per output
  buffer). Index = `(p3d - frame) >> 10`. Simplest exact form: a **post-pass per finished bin** inside our
  `video_3d_render_bins_4x` wrapper, over the final 0x10000 bytes. This also covers hr.c's 3x downsample,
  `RAST=diff` and DraStic's own renderer. Cost: about 0.45 instr/px, so ~0.09 M/frame split over 3 threads. Folded
  into the resolve kernels it is about 0.02 M.
- Key the table by output-buffer pointer (`[sys+0x34eb58]` / `+0x34eb60` with threaded_3d) and set "valid" after
  the last bin. Consumption order is safe in both modes: threaded_3d=0 renders at line 215, synchronously and
  long before the next composite at line 192; threaded_3d=1 composites only the buffer that
  `video_3d_finish_rendering` published. Invalidate on reset or savestate load.
- **Lighter variant:** only a per-quarter summary byte (empty / full-opaque / mixed). The hook memsets 0x00 or
  0xff and returns 0 or 0x10, and computes only the mixed quarters. That is 768 B of shared state.
- What cannot move: priority, layer selection, windows and blending depend on per-line 2D state.

### (b) Straight NEON rewrites (exact, standalone)

| target | now | rewrite | gain/frame |
|---|---|---|---|
| set_3d_visibility + gather | 425 | ~115-130: 16 `ld4`, `cmtst`, `and` with bit weights, `addp` tree, `orn`+`umin` for the flag | **~0.23 M** |
| expand_6bit_split | 229 | ~180 (`ld2` lo/hi + `shl`/`ushr`/`sli`/`and`) | 0.04 M |
| color_convert_direct_32_2x | 361 | ~330 at best (zip/st4 bound) | small |
| select_pixels_binary32 | 178 | ~150 | small |

The **fused path** is worth more than any single rewrite. When a quarter is "3D + backdrop only", composite +
select + scalar + expand + binary32 (~623 instructions) can be one pass of ~205: DraStic's `priority_encode_single`,
a check that `excl[1..4]` are all zero, then per 32 px `ld2r` / `tbl` / `cmtst` / `ld4` / 6 `bif` / 3 `st1`,
which writes R/G/B = mask0 ? 3D bytes : expand(backdrop). This is exact when:
- `(flags & 0xf) == 0` (simple, non-shade path);
- `p3d != NULL` and `lmask & 1`;
- `excl[1..4] == 0`. Then backdrop = ~mask0, and the planes equal DraStic's even though the BG0 u16 buffer
  holds garbage.

Planes are still produced, so capture and the master-brightness converters work unchanged. Saving is
**~0.32 M/frame** when every quarter qualifies (the stress ROM: only BG0 enabled). Lines with OBJ/BG over the
3D fall back.

Optional later step: when both quarters of a row qualify, convert straight from the 3D frame into the scanout
and skip the planes. That needs cross-function state (a composite hook plus a convert hook keyed by plane
pointer) and is worth about 0.08 M. The scanout is a DRM dumb buffer, so never read it.

### (c) Hook points (16-byte `ldr x16,#8; br x16; .quad`)

| function | addr | first 4 words | PC-relative in the patch? | args | replicate |
|---|---|---|---|---|---|
| render_scanline_set_3d_visibility | 0x3c2c0 | a9aa7bfd 910003fd a90153f3 f0000914 | adrp (4th): replace fully | x0 u8[32] out, x1 const u32[256] -> w0 | all 32 bytes; return exactly 0/0x10/2; AAPCS. Callers: 0x3f110 (1x), 0x3fa14 (2x). The pointer may be 1x, downsample (hires=2) or HOFS scratch, so fall back to compute |
| render_scanline_gather_3d_alpha_asm | 0xa0a48 | 52802002 d503201f 4cdf0020 4cdf0024 | loop head at +8 inside the patch: replace only | x0 u8[256], x1 u32[256] | only caller is set_3d_visibility |
| render_scanline_2d_composite | 0x3c6d0 | a9b67bfd aa0303e8 910003fd a9025bf5 | none: trampoline OK | x0 eng, x1 planes, x2 S, x3 u64 layers[5] (ptr-0x10 to u16[256]), x4 u32 p3d or NULL, x5 u8 alpha or NULL, w6 layer mask, w7 BLDCNT, [sp] u32 flags, [sp+8] u32 line | planes for the quarter; S+0x10c0 is scratch only |
| render_scanline_select_pixels | 0x39330 | d10a03ff a9007bfd 910003fd a9025bf5 | none | x0 eng, x1 planes, x2 excl[6][32], x3 layers, x4 p3d, x5 alpha, w6 lmask | see 1.5 |
| render_scanline_priority_encode_single_asm | 0x9ffc0 | 3942cc06 91021007 4f000400 4f000401 | none | x0 eng, x1 vis base, x2 excl out | callable as a helper; clobbers v0-v7, v16, v17, x3-x7 |
| render_scanline_expand_6bit_split_asm | 0xa0818 | 91040002 91080003 52802004 4f01e7d2 | none (loop head at +0x10) | x0 planes, x1 u16[256] | 2.x |
| render_scanline_select_pixels_binary32_asm | 0xa0730 | 91040006 91080007 10ffff45 4f00e43b | adr (3rd) | x0 planes, x1 alpha/NULL, x2 u32[256], x3 mask | both variants |
| render_scanline_select_pixels_binary_scalar_asm | 0xa0560 | 10000685 4e020c54 4c4070a0 52802004 | adr (1st) | x0 u16 dst, x1 src, w2 colour, x3 mask | |
| render_scanline_color_convert_direct_32_2x_asm | 0xa0ae0 | 91040004 91040025 91080006 91080027 | none (loop at +0x20) | x0 planes E, x1 planes O, x2 u32[512] | byte-3 pattern (2.x) |
| render_scanline | 0x404a0 | d283540c cb2c63ff a9007bfd 910003fd | none | x0 eng, x1 scanout line, w2 line, x3 capture/NULL | |
| render_scanline_2d | 0x3ef00 | d283a60c cb2c63ff b0000905 a9017bfd | adrp (3rd) | x0 eng, x1 planes[4], w2 line, x3 capture, w4 hires -> w0 | |
| render_scanline_3d | 0x59950 | f9400402 b944a043 b9446842 34000103 | cbz (4th) | x0 sys, w1 line -> u32* | |
| video_2d_render_scanlines | 0x42a90 | a9b97bfd d2800187 910003fd a90573fb | none | x0 eng, w1 first, w2 last, x3 capture | |

"Replace fully" means no trampoline to the original, so the replacement must carry its own fallback. A.2 has
the NEON sketch.

---

## 5. Cost model (light 3D scene, instructions/frame)

| | M instr | share of engine A |
|---|---|---|
| set_3d_visibility + gather | 0.326 | 31% |
| expand_6bit_split | 0.176 | 17% |
| color_convert_direct_32_2x (A's half) | 0.139 | 13% |
| select_pixels_binary32 | 0.137 | 13% |
| render_scanline_2d | 0.094 | 9% |
| binary_scalar + select_pixels + composite + prio | 0.150 | 14% |
| render_scanline (A's half) | 0.023 | 2% |
| **engine A total (emulation thread)** | **~1.04** | about 5,450 instr per DS line, 5.3 per output pixel |
| engine B, display off (render thread) | ~0.16 + libc memset | runs in parallel |

All listed functions together are 1.21 M: 32% of the 3.8 M frame and about 60% of the ~2.0 M spent inside
DraStic. Engine A's ~1.04 M sits on the emulation thread's frame-end critical path.

| step | emu-thread saving | engine A after |
|---|---|---|
| 1. NEON set_3d_visibility | ~0.23 M | ~0.81 M |
| 2. per-bin precompute (1 stays as fallback; +~0.02-0.09 M on the bin threads) | ~0.31 M | ~0.73 M |
| 3. fused 3D+backdrop quarters (with 2) | +~0.32 M (when every quarter qualifies) | **~0.41 M** |
| 4. optional row-level direct convert | +~0.08 M | ~0.33 M |

Steps 2 and 3 together save about **0.63 M/frame**: roughly 17% of the 3.8 M frame and 60% of engine A's
compositing. In a game with a HUD over the 3D, step 2 still applies in full and step 3 only to quarters with no
2D overlay. Device note: the counts understate `ld4`/`st4` (multi-cycle on the A55). The fused path also drops
about 3 KiB of L1 round trips per quarter (gather buffer, u16 tmp, planes read-modify-write), so the device gain
should be at least proportional. Side item: engine B's display-off path (memset + 2 converts) could become a
constant fill, but that saves render-thread time only (~0.16 M), off the critical path while A is the larger side.

---

## Appendix A.1: reference semantics, set_3d_visibility (machine-checked)

```c
uint32_t set_3d_visibility(uint8_t bits[32], const uint32_t px[256]) {
    int any = 0, transl = 0;
    memset(bits, 0, 32);
    for (int i = 0; i < 256; i++) {
        uint8_t a = px[i] >> 24;
        if (a) { bits[i >> 3] |= 1u << (i & 7); any = 1; transl |= (a != 0x1f); }
    }
    return transl ? 2 : any ? 0x10 : 0;
}
```

Checked on 400 inputs (0/31 only; 0..31; empty; single pixel; arbitrary bytes 0..255) by interpreting
0x3c2fc..0x3c5dc from the disassembly: `re/verify/vis_model.py`, 0 mismatches.

## Appendix A.2: NEON sketch, ~115 instructions per call

Per 64 px:
- `ld4 {v0-v3}.16b` x4 (alpha in the 4th register of each);
- `cmtst m,a,a` (vis);
- `orn t,a,m` and `umin acc,acc,t` (translucency: a byte below 0x1f after the `orn` means translucent; for exact
  results with alpha bytes > 31, use `cmeq a,#31` plus `bic`/`orr` instead, +1 instr per 16 px);
- `and m,m,weights{1,2,..,128}`;
- `addp` x3; plus 1 `addp` per 128 px; `str q` per 128 px.

End: `uminv` for the translucent test, `umaxv` over the OR of the bitmaps for "any visible", then select 2/0x10/0.

Fused 3D+backdrop select (step 3), per 32 px:
- `ld2r {m0.8h,m1.8h}` of the mask, `tbl` x2 with {0 x8, 1 x8}, `cmtst` x2 against {1,2,..,128};
- `ld4 {r,g,b,a}.16b` x2;
- `bif r,bdR,m` (x3 per 16 px);
- `st1` to R, G, B (+0, +0x100, +0x200).

Backdrop: `bd = *(u16*)[eng+0x18]`, with `bdR = (bd&0x1f)<<1`, `bdG = ((bd>>5)&0x1f)<<1`, `bdB = ((bd>>10)&0x1f)<<1`.
