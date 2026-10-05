# DraStic r2.5.2.2: windows, colour effects, the compositor and the scanout

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. The model is `src/rast/spec/2d/compose.c` / `compose.h`
> (`re/2d/compose/model/compose_spec.c`: `cs_<routine>` is `spec_render_scanline_<DraStic's name without _asm>`, e.g.
> `cs_composite` is `spec_render_scanline_2d_composite`, `cs_apply_offset`
> `spec_render_scanline_color_effects_apply_offset_c`; `cs_disable_blank_layers` is
> `spec_render_scanline_disable_blank_layers` in `bg.c`; the simple path's routines that were in `spec/composite.c`
> moved there too). `pix/pixel.h` is `src/rast/spec/2d/pixel.h` (`px_pixel` / `px_in_t` are `spec_composite_pixel` /
> `spec_composite_in`). `model/t_compose.c` and `pix/pix.c`'s generator are in `tools/rast/ut/t_compose2d.c`;
> `compdiff*.c` and the bench were not kept. The other analyses are next to this file; the design is
> `../2d-engine.md`.

Key: `compose`. Scope: everything between the per-layer lines (BG/OBJ/3D, with their visibility bitmaps) and the
32-bit scanout of one DS line: `render_scanline_generate_window_masks` and the window helpers, `apply_windows`,
`disable_blank_layers`, `render_scanline_2d_composite` for every flag value (the priority encoders, layer
selection, blend-enable masks, coefficient setup, colour-effect application, brightness), the 3D-layer helpers
(`set_3d_visibility`, done in `re/compositing-2x.md`, and `horizontal_shift_3d`), display capture, and the
colour conversion to the scanout with master brightness (`render_scanline`'s convert stage). The frame model,
event replay and engine struct are in `re/2d/frame.md`; the BG and OBJ renderers in `re/2d/bg.md` / `re/2d/obj.md`;
the simple (non-blending) composite path and its scratch layout in `src/rast/spec/composite.c`.

Notation: `eng` = 2D engine struct (A = `video+0x2e78`, B = `video+0x84298`); `S` = `render_scanline_2d`'s
scratch area (`sp+0x180` of its 0x1d30-byte frame); `C` = the capture descriptor `video+0x458820` (frame.md 8.2).
`[p+o]` = memory at p+o. Bitmaps ("masks") are 256 bits = 32 bytes, pixel i = byte i>>3, bit i&7 (LSB first).
Planes = 3 x 256 bytes, R6 at +0, G6 at +0x100, B6 at +0x200 (6-bit values). Registers are AAPCS64.

How the claims were checked:

- **[D]** read in the disassembly (`re/drastic.dis`; per-function dumps in `re/2d/compose/*.s`).
- **[M]** machine-checked: the C model `re/2d/compose/model/compose_spec.c` (one function per DraStic routine,
  written from the disassembly) is compared byte for byte with DraStic's own routine, called in DraStic's process
  under qemu (`model/t_compose.c`, the repo's `tools/rast/ut` harness): random inputs (all flag combinations, random
  priority lists, stale scratch, junk bytes, overlapping windows, edge values), every output buffer between guard
  bytes, the whole scratch area S and the engine struct compared after every call. Per seed: 20 000 cases each for
  update_window_mask, the inhibit masks, apply_windows + disable_blank_layers, the compositor pieces (encoder, blend
  enable, shade, the four setups, apply / apply_offset, horizontal shift), capture and convert; 30 000 for
  generate_window_masks; 40 000 composites (every reachable path: simple, simple + shade, complex with flags & 5 =
  0/1/4/5, each with and without brightness). **0 mismatches**; re-run on 2026-10-05 with seeds 1, 7 and 23, all
  passed (`compose/ut/t_compose-rerun.txt`). Six mutations of the model (a rounding constant, a flag bit, the window
  state machine, the convert's byte-3 pattern, a capture saturation bound, the BLDCNT-mode test of the 3D blend) were
  all caught (previous pass; no log kept). The routines covered: `update_window_mask`, `inhibit_masks_single/double/triple`,
  `generate_window_masks`, `apply_windows`, `disable_blank_layers_asm`, `priority_encode_double_asm`,
  `select_blend_enable_asm`, `shade_asm`, `color_effects_setup_{blend_base,blend,alpha_base,alpha}_asm`,
  `color_effects_apply_asm`, `color_effects_apply_offset_c`, `horizontal_shift_3d`, **`render_scanline_2d_composite`
  (all paths)**, `capture_direct_asm`, `capture_direct_3d_asm`, `capture_blended`, `capture_blended_3d`,
  `color_convert_direct_32_{1x,2x}_asm`, `color_convert_shade_32_{1x,2x}_asm`. (select_pixels, the single encoder,
  expand and binary32 come from `spec/composite.c`, already tested by `t_composite.c`.)
- **[R]** runtime-checked in the simulator: `model/compdiff.so` hooks the composite, `generate_window_masks` and the
  four 32-bit converters in the running emulator (`rast/run.sh`, `RAST=ours`, our compositor hooks off), runs the
  model next to DraStic on every call with the real state and compares everything either may write; in the composite
  hook it also recomputes the `flags`, `bldcnt` and `alpha` arguments that `render_scanline_2d` passes from the engine
  state (the reading of section 3). On the ds2d scenes (`/home/user/wt-scenes2d/stressrom`, build 20:21, copied to
  `model/roms/`), 50 s each: T0 0.4 M composites, T3 0.4 M, T5 0.6 M, T6 0.4 M, T7 0.4 M, T9 0.6 M, cycle 0.4 M
  (**3.2 M composites, 0 mismatches, 0 argument mismatches**; 2.4 M `generate_window_masks` calls, 0 mismatches;
  9.8 M direct and 0.1 M shade converts, 0 mismatches). The paths hit: simple, simple+shade, complex with translucent
  3D / bitmap-OBJ alpha (with and without brightness), alpha mode, alpha + semi-transparent OBJ, semi-transparent OBJ
  + brightness, the OBJ image flag, the 2x per-quarter alpha copies, windows with effects disabled in a region.
- **[C]** the per-pixel closed form of section 5.6 (`re/2d/compose/pix/pix.c`, native) against the model's
  `cs_composite` on inputs shaped the way `render_scanline_2d` produces them (DraStic-shaped priority lists, lmask
  and bldcnt' and flags derived as in 3.2, OBJ attribute plane / semi / bitmap masks as `render_scanline_obj_c`
  makes them, 3D lines with alpha 0 / 31 / 1..30 spans, random windows-effects mask): 5 seeds, 1.8 M lines
  (461 M pixels), every path, **0 differences**; six mutations of the closed form are all caught. And at runtime
  against DraStic itself: `model/compdiff2.c` (compdiff + the closed form computed from the inputs of every
  composite call and compared with the planes DraStic's own routine wrote; logs `model/e2e2-*.log`), 50 s each:
  T5 0.4 M composites (0.14 M with the OBJ image, semi-transparent OBJs with brightness, bitmap OBJs), T9 0.6 M
  (translucent 3D with and without brightness, BLDALPHA, windows with effects disabled, capture, the OBJ image),
  T7 0.4 M (BLDALPHA, semi-transparent OBJ, brightness): **0 differences** in 1.4 M composites (85 360 of the T9
  ones with a non-empty effects-disabled window mask). T6 and T3 were not run this pass (the simulator stayed busy
  with other jobs); DraStic matched the model there under compdiff, and the closed form matches the model on such
  inputs (above).
- **[P]** measured with the block profiler (`rast/prof.sh` + `neon/fnreport.py`) in the running emulator, and
  **[B]** per call with `re/2d/compose/bench/bench.sh` (DraStic's own routines called on fixed scenarios inside
  DraStic's process under the same `fnprof` plugin, the startup run subtracted), section 9.
- **[G]** guess, not verified.

Spot checks of the most load-bearing claims, re-read in the disassembly on 2026-10-05 (this pass): the complex path's
control flow (0x3c728..0x3cab4: lmask2, the `flags & 5` switch, the 0x10 / 0x20 masks at 0x3cc80 / 0x3ccc4, the
inline darken / brighten loops at 0x3c930 / 0x3cb74, the per-pixel alpha mask with the BLDCNT.7 and p3d test at
0x3ca18, apply vs apply_offset by `[sp+0x64]`); the flags and bldcnt' of render_scanline_2d (0x3f078..0x3f0c0,
0x3f934); `color_effects_apply_asm` (umull / umlal .8h, `rshrn #5`, `umin 63`); `apply_offset_c`'s NEON body; the
`2*BLDALPHA` leak in `setup_blend(_base)_asm`; `shade_asm`; the window Y state machine (0x3b414..0x3b450) and the
line-0 initialisation in render_scanline (0x40574..0x405d8); `horizontal_shift_3d`; `capture_blended` (C, 33
instructions a pixel); the capture dispatch (0x3f6a0.., the hi-res quarter sources at 0x4020c, the no-3D case at
0x402d4, the HOFS case at 0x400fc); the convert dispatch (0x4067c, 0x40a40, 0x40be0, the memsets at 0x41288 /
0x412d0); `direct_32_2x_asm` (`movi v19.4h, #0xff` byte-3 pattern) and `shade_32_2x_asm` (`shl #2` before the
multiply); the event replay of BLDCNT / BLDALPHA / BLDY / MASTER_BRIGHT / WININ / WINxH / WINxV (0x42358..,
0x42450.., 0x42628); the dead-code list (5 in section 0: `bench/refs.py` scans every branch, adr / adrp+add pair and
RELATIVE relocation of the whole binary). No discrepancy with the previous pass was found; also checked:
`select_blend_enable_asm` (0xa0070), the 26 hook-point first words of section 11 against the binary, and every
routine address named in this document against the symbol table.

---

## 0. Summary

1. **The pipeline per line** (section 1): layer lines (u16 BGR555) and visibility bitmaps -> windows clear
   visibility bits and build an "effects disabled" mask -> blank BGs dropped -> priority encoder (one or two
   layers deep) -> layer selection into 6-bit planes (3D bytes inserted as they are) -> blend-enable masks ->
   per-pixel coefficient planes EVA/EVB(/offset) -> `out = min(63, (top*EVA + second*EVB [+ 63*off] + 16) >> 5)` ->
   (per DS line, after all quarters) master brightness + `<<2` into XRGB8888.
2. **Two composite paths** (section 5). `flags & 7 == 0`: one layer deep (`spec/composite.c`), optionally the
   brightness shade (flag 8). Otherwise the **complex path**, which always computes both layers and runs the full
   coefficient + apply pass for all 256 pixels. It is selected per line (1x) or per quarter (2x) by: a
   semi-transparent OBJ on the line (bit 0), a bitmap OBJ on the line **or a translucent 3D pixel** (bit 1), BLDCNT
   alpha mode with first and second targets (bit 2). A quarter with one 3D pixel of alpha 1..30 costs 2.5-4x a
   simple quarter even when nothing ends up blended (section 9).
3. **Formulas** (section 5.5), all in the 6-bit domain of the planes, EVA/EVB doubled and clamped to 32:
   alpha blend `min(63, (t*min(2*EVA,32) + s*min(2*EVB,32) + 16) >> 5)`; 3D / bitmap-OBJ alpha
   `min(63, (t*(a+1) + s*(31-a) + 16) >> 5)`; brighten `(c*(32-f) + 63*f + 16) >> 5`; darken `(c*(32-f) + 16) >> 5`
   with `f = min(2*BLDY, 32)`. The scanout byte is the 6-bit value `<< 2`.
4. **DraStic-specific behaviour a replacement must reproduce** (section 10.2), among them: the 2x master-brightness
   *brighten* is wrong (adds `63f+16` to values already shifted to 8 bits: a fade to white goes towards dark grey,
   then jumps to white at MASTER_BRIGHT factor 16; **[M]+[R]**, reached in T7); BLDALPHA bit 7 leaks into EVB; opaque
   3D pixels are never blended with EVA/EVB; translucent 3D is not blended at all while BLDCNT is in a brightness
   mode (even with BLDY = 0); a semi-transparent OBJ over a 2nd target in brighten mode gets the alpha blend *and*,
   if OBJ is also a 1st target, the brightness offset; with the full-screen OBJ image all OBJ-top pixels are excluded
   from BLDALPHA / semi-transparent blending (not from bitmap-OBJ alpha); the window Y state machine only runs on
   rendered lines with windows enabled; the OBJ window mask is stale when OBJ display is off; capture of the 3D
   source needs DISPCNT.3; the 2x scanout's byte 3 is a fixed (direct) or data-dependent (shade) junk pattern.
5. **Dead code**: of the routines named in the brief, the `_c` twins of the NEON routines (`priority_encode_single_c`,
   `priority_encode_double_c`, `select_blend_enable_c`/`_layer`, `shade_c`, `color_effects_setup_*_c`,
   `color_effects_apply_c`, `disable_blank_layers_c`, `capture_direct(_3d)_c`, `gather_3d_alpha_c`,
   `color_convert_*_32_*_c`), `window_inhibit_outside_windows` (inlined into generate_window_masks) and
   `render_scanline_capture` have no caller and no relocation **[D]**. `color_effects_apply_offset_c` is live (it has
   no asm twin); so are `capture_blended(_3d)` (C only) and `color_convert_shade_16_2x_c` (16 bpp only). The darken /
   brighten setups are inlined into the composite (same code as the dead `_c` functions).
6. **Costs** (section 9): on 3D-only scenes engine A's compositing is ~1.07 M instructions a frame, ~5 600 per DS
   line (S7, L4; matches `re/compositing-2x.md`). Per 256-pixel quarter (measured per call, [B]): simple 620 (3D
   only) to 1 100 (3D + 2 BGs + OBJ), simple + shade 1 200-1 700, complex 2 600-2 900, complex with BLDCNT
   brightness 4 900-6 100 (scalar per-pixel loops inside the composite plus the C `apply_offset`). At 2x every
   quarter cost counts four times per line. Windows add 300-700 per line, master brightness at 2x 1 150 per line.
   **Blended display capture is the most expensive single item**: `capture_blended(_3d)` is scalar C, 8 500-8 700
   instructions per 256 pixels, four calls per line at 2x: 34 000 per captured line, up to 6.7 M a frame (T9's
   motion-trail quarter: 1.38 M a frame on average over the scene).
7. **A per-pixel closed form** of the whole composite (section 5.6, [C]) replaces the mask / plane / coefficient
   pipeline: given the two front layers of the pixel, their colours, the flags and the BLDCNT / BLDALPHA / BLDY
   values, it yields DraStic's 6-bit output byte for byte, for every flag combination render_scanline_2d produces
   (checked against the model on 461 M pixels and against DraStic's own planes on 1.4 M composites of the 2D test
   scenes). It is what a fused replacement implements.

---

## 1. The pipeline of one DS line (engine X)

```
render_scanline(eng, out, line, C)                                  0x404a0   (frame.md 5)
 └ render_scanline_2d(eng, planes, line, C, hires) -> 0 / 1           0x3ef00   (section 3)
    ├ [3D]   p3d = render_scanline_3d(video, line)                     u32 r6|g6<<8|b6<<16|a5<<24
    ├ render_scanline_bg     -> S+0x1e0.. u16 lines BG0..3, vis[0..3]  (bg.md)
    ├ render_scanline_obj_c  -> S+0xa70 u16 line, S+0xc90 alpha, vis[4..7], OBJ window, semi, bmp (obj.md)
    ├ generate_window_masks  -> S+0xf00 inhibit[5], S+0xfa0 fx              (section 4)
    ├ flags_line, bldcnt' (section 3.2)
    ├ 1x: set_3d_visibility, direct-layer / OBJ-image bitmaps, apply_windows, disable_blank_layers,
    │     render_scanline_2d_composite -> planes                          (section 5)
    │ 2x: apply_windows + disable_blank_layers for the 1x layers, then per quarter q = 0..3:
    │     3D quarter (+HOFS shift), set_3d_visibility, hi-res layer bitmaps, apply_windows (hi-res layers),
    │     render_scanline_2d_composite -> planes + q*0x300
    └ display capture (section 7)
 └ convert: MASTER_BRIGHT -> color_convert_{direct,shade}_32_{1x,2x} -> scanout   (section 8)
```

Engine B runs the same code (no 3D layer, `C = NULL`). The 1x layer lines, windows, OBJ data and bitmaps are reused
unchanged by all four quarters of the 2x path; only the 3D layer and the "hi-res" layers (direct-colour BG2/BG3 and
the full-screen OBJ image, fed by hi-res capture data) differ per quarter. Quarter q is output row `2L + (q>>1)`,
pixels `x = 2i + (q&1)`.

---

## 2. Data formats

| object | where | format |
|---|---|---|
| layer line | `layers[k] + 0x10` (k = 0..3 BG, 4 OBJ) | u16[256] BGR555 (`r \| g<<5 \| b<<10`); bit 15 ignored by the compositor (it only defines visibility for the direct/OBJ-image bitmaps) |
| 3D line / quarter | `p3d` | u32[256]: byte0 r6, byte1 g6, byte2 b6, byte3 alpha (0 transparent, 31 opaque, 1..30 translucent) |
| visibility | `S+0xda0 + 32*slot` | bitmap; slot 0..3 = BG0..BG3, 4..7 = OBJ priority 0..3 |
| exclusive / top / second masks | `S+0x10c0` (simple), `S+0x13c0` / `S+0x1480` (complex) | 6 bitmaps: BG0..BG3, OBJ (+0x80), backdrop (+0xa0) |
| planes | `out`, `S+0x1180`, `S+0x1540` (top), `S+0x1840` (second) | R6[256] G6[256] B6[256]; 2D colour c5 becomes `c5 << 1`, 3D bytes are copied as they are |
| alpha plane | `S+0xc90` (and the 2x copy `S+0xfc0`) | u8[256]: OBJ attribute (`2a+1` for a bitmap OBJ of alpha a, else 0 after `&0x3f`), overwritten with the 3D alpha where BG0 is the top layer (complex path, `flags & 2` without `flags & 8`) |
| coefficients | `S+0x10c0` EVA, `S+0x11c0` EVB, `S+0x12c0` OFF | u8[256] each (complex path) |
| window masks | `eng+0x44` WIN0, `eng+0x64` WIN1 (persistent), `S+0xea0` OBJ window | bitmaps (the WIN0/WIN1 ones are per engine, recomputed when WINxH changes) |
| inhibit | `S+0xf00 + 32*k`, k = 0..4 (BG0..BG3, OBJ) | bit set = layer k hidden at that pixel |
| fx | `S+0xfa0` | bit set = colour effects disabled at that pixel |
| scanout | `[eng+0x38] + line*[eng+0x40]` | XRGB8888 little-endian `B \| G<<8 \| R<<16 \| X<<24`, 8-bit = 6-bit `<< 2`; 2x: rows at +0 and +pitch/2 |

---

## 3. render_scanline_2d (0x3ef00): what reaches the composite

### 3.1 Order of work [D][R]

```
render_scanline_2d(eng x0, planes x1, line w2, C x3, hires w4) -> w0
  if eng[0xb6]: video_2d_reorder_obj(eng); eng[0xb6] = 0               OAM changed (obj.md)
  disp = [eng+0x90] u32; BLD = [eng+0xa0] u16
  flags = eng[0x21340 + line]          bit 0: semi-transparent OBJ registered on the line, bit 1: bitmap OBJ (obj.md)
  hofs = sext9([eng+0x15a])            BG0HOFS
  img = [eng+0x21400]; d2 = [eng+0x240]; d3 = [eng+0x2f0]             OBJ image, direct BG2 / BG3 (frame.md 7)
  lmask = (disp >> 8) & 0xf;  H = 0;  p3d = NULL
  if disp.3 && (disp.8 || (C && C[0x51] == 2)):                        BG0 is 3D and on, or the capture takes 3D
      H = (hires != 0);  p3d = render_scanline_3d(video, line)
  render_scanline_bg(eng, S+0x1e0, S+0xda0, line)
  if disp.12: lmask |= render_scanline_obj_c(eng, S+0xa70, S+0xc90, S+0xe20, S+0xec0, S+0xee0, line)   0 or 0x10
  if img: H |= 0x10, lmask |= 0x10;   if d2: H |= 4;   if d3: H |= 8
  render_scanline_generate_window_masks(eng, S+0xf00, S+0xfa0, S+0xea0, lmask, line)
  (flags, bldcnt') per 3.2
  layers[5] = { S+0x1e0, S+0x400, S+0x620, S+0x840, S+0xa60 }          (sp+0x158; pointers 0x10 before the pixels)
  if (lmask & H) && hires: goto 2X
  1X:
    if p3d:
        if hofs: horizontal_shift_3d(S, p3d, hofs); p3d = S            (S+0..0x3ff, over BG0's own line buffer)
        flags |= set_3d_visibility(S+0xda0, p3d)                       2 / 0x10 / 0
    if img: l = img + line*512; vis[4 + eng[0x21410]] = bit15(l); flags |= 0x20; layers[4] = l - 0x10
    for k in 2, 3: if d_k: l = d_k + line*512; vis[k] = bit15(l); layers[k] = l - 0x10
    alpha = (flags & 2) ? S+0xc90 : NULL
    apply_windows(eng, S+0xda0, S+0xf00, lmask)
    disable_blank_layers(S+0xda0, &lmask)
    render_scanline_2d_composite(eng, planes, S, layers, p3d, alpha, lmask, bldcnt', flags, line)
    capture (section 7); return 0
  2X:
    Hu = lmask & H
    apply_windows(eng, S+0xda0, S+0xf00, lmask & ~H)
    m = lmask & ~H; disable_blank_layers(S+0xda0, &m); lmq = m | Hu
    for q in 0..3:
        f = flags; pq = NULL
        if p3d: pq = p3d + q*256 (u32); if hofs: horizontal_shift_3d(S, pq, hofs); pq = S
                f |= set_3d_visibility(S+0xda0, pq)
        o = (line*3 + q - 1) * 256                                     u16 index into the hi-res capture data
        if img: l = (q && [eng+0x21408]) ? [eng+0x21408] + 2*o : img + line*512
                vis[4 + eng[0x21410]] = bit15(l); f |= 0x20; layers[4] = l - 0x10
        for k in 2, 3: if d_k: l = (q && hr_k) ? hr_k + 2*o : d_k + line*512; vis[k] = bit15(l); layers[k] = l - 0x10
                       (hr_2 = [eng+0x248], hr_3 = [eng+0x2f8])
        alpha = !(f & 2) ? NULL : q == 3 ? S+0xc90 : (memcpy(S+0xfc0, S+0xc90, 256), S+0xfc0)
        apply_windows(eng, S+0xda0, S+0xf00, Hu)
        render_scanline_2d_composite(eng, planes + q*0x300, S, layers, pq, alpha, lmq, bldcnt', f, line)
    capture; return 1   (the hires argument)
```

`bit15(l)` = the bitmap `bit i = l[i] >> 15` (inline NEON at 0x3f134, 0x3f3b8, 0x3fa94, 0x3fd20; scalar twins
0x40018.., 0x400a4.., 0x40114.., 0x4016c..) **stored over** the whole 32-byte slot. Engine B and 2D-only screens
take the 2x path whenever a direct BG2/BG3 or the OBJ image is enabled (four identical composites when no hi-res
data exists).

### 3.2 The flags and the BLDCNT argument [D][R]

```
bm   = BLD & (lmask | lmask << 8 | 0xf0f0)        lmask as after OBJ / OBJ image (before windows and blank-drop):
                                                   1st/2nd-target bits of disabled BGs cleared; OBJ, BD and the
                                                   mode bits kept
mode = (BLD >> 6) & 3
if mode == 1:      if (bm & 0x3f) && (bm & 0x3f00):           flags |= 4      alpha blending active
elif mode >= 2:    if (bm & 0x3f) && [eng+0xa2] (BLDY) != 0:  flags |= 8      brightness active
if !(bm & 0x3f00): flags &= ~1                    a semi-transparent OBJ needs some 2nd target
flags |= set_3d_visibility(...)                   2 translucent 3D in this line/quarter, 0x10 3D present and all opaque
flags |= 0x20                                      the full-screen OBJ image is on
bldcnt' = bm                                       the composite's w7
```

| bit | meaning | set by |
|---|---|---|
| 0 | semi-transparent OBJ on the line (and some 2nd target enabled) | obj line flags, cleared above |
| 1 | bitmap OBJ on the line, or a 3D pixel with alpha not in {0, 31} | obj line flags, set_3d_visibility |
| 2 | BLDCNT alpha mode with a 1st and a 2nd target | above |
| 3 | BLDCNT brightness mode with a 1st target and BLDY != 0 | above |
| 4 (0x10) | 3D visible and all opaque | set_3d_visibility |
| 5 (0x20) | full-screen OBJ image | render_scanline_2d |

`flags & 7 == 0` -> simple path (with the shade when bit 3); otherwise the complex path. Bits 4, 5 only matter in
the complex path. The windows' "effects" bits do not enter the flags: they act per pixel through `fx`.

---

## 4. Windows

### 4.1 Horizontal masks: render_scanline_update_window_mask(mask x0, WINxH w1) 0x3a1c0 [M]

`left = WINxH >> 8`, `right = WINxH & 0xff`:
- `right == 0`: empty if `left == 0`, else `right = 256`;
- `left == right`: empty;
- `left < right`: bits `[left, right)`;
- `left > right`: all bits except `[right, left)` (wrap).

Written to `eng+0x44` (WIN0) / `eng+0x64` (WIN1) only when the dirty bit `eng[0xb5]` bit 0 / bit 1 is set (set by
the WIN0H / WIN1H event replay, frame.md 3; 3 after a savestate load), by `generate_window_masks`.

### 4.2 Vertical state and region masks: render_scanline_generate_window_masks(eng x0, inh x1, fx x2, objwin x3, lmask w4, line w5) 0x3b360 [M][R]

```
fx[0..31] = 0
en = (DISPCNT >> 13) & 7                      bit 0 WIN0, bit 1 WIN1, bit 2 OBJ window
if en == 0: return                            (inh not written, the state machine not stepped)
inh[0..0x9f] = 0
wv = [eng+0x9c] ^ 0x3f3f3f3f                  WININ | WINOUT<<16, inverted: bit set = layer/effect disabled
if eng[0xb5] & 1: update_window_mask(eng+0x44, WIN0H);  if eng[0xb5] & 2: update_window_mask(eng+0x64, WIN1H)
s = eng[0xb4]
s |= (Y1_0 == line) ? 5 : 4                   Y1 = WINxV >> 8 (top), Y2 = WINxV & 0xff (bottom), bytes eng+0xaf/0xae, 0xb1/0xb0
if Y2_0 == line: s &= ~1
if Y1_1 == line: s |= 2
if Y2_1 == line: s &= ~2
eng[0xb4] = s; eng[0xb5] = 0
switch (s & en):                              out = wv >> 16
  1: single(inh, fx, lmask, eng+0x44, wv,       out)
  2: single(inh, fx, lmask, eng+0x64, wv >> 8,  out)
  4: single(inh, fx, lmask, objwin,   wv >> 24, out)
  3: double(..., eng+0x44, eng+0x64, wv, wv >> 8, out)
  5: double(..., eng+0x44, objwin,   wv, wv >> 24, out)
  6: double(..., eng+0x64, objwin,   wv >> 8, wv >> 24, out)
  7: triple(..., eng+0x44, eng+0x64, objwin, wv, wv >> 8, wv >> 24, out)
  0: if out bit 5: fx = all ones;  for each set bit k of (lmask & out): inh[k] = all ones
```

Region inhibit (`single` 0x3b060, `double` 0x3ab10, `triple` 0x3a380) [M]: the regions are, in priority order,
`A = winA`, `B = winB & ~A`, `C = winC & ~(A|B)`, `O = ~(A|B|C)`. For each region R with inhibit bits `i`:
`if i bit 5: fx |= R`; `for each set bit k of (i & lmask): inh[k] |= R`. So WIN0 > WIN1 > OBJ window > outside;
bits 0-4 of the window control registers are the layers (BG0..BG3, OBJ), bit 5 the effects.

**Per-line state** (`eng[0xb4]`): bit 0 WIN0 active, bit 1 WIN1 active, bit 2 always set here (the OBJ window is
"active" whenever enabled). It is stepped only by this function, i.e. on lines where `render_scanline_2d` runs and
some window is enabled. `render_scanline` initialises it at line 0 (frame.md 5.3): `if Y1 > 191: set; if Y2 > 191:
clear` (per window), otherwise the bit keeps its value from the previous frame. A window whose top edge is in
vblank and bottom edge in the next frame is therefore on from line 0; one with both edges in vblank is off.

### 4.3 apply_windows(eng x0, vis x1, inh x2, lmask w3) 0x3b650 [M]

`if (DISPCNT & 0xe000) == 0: return`; for BG k in lmask: `vis[k] &= ~inh[k]`; if lmask bit 4: the four OBJ slots
`vis[4..7] &= ~inh[4]`. Windows hide a layer by removing its pixels from the priority encoding (the layer below shows).

### 4.4 disable_blank_layers_asm(vis x0, &lmask x1) 0xa089c [M]

`lmask &= m` with `m = 0xfff0 | (BG k has a non-empty vis ? 1 << k : 0) | (any BG non-empty ? 0xffff0000 : 0)`. OBJ
(bit 4) is never dropped. Only an optimisation of the later passes, but the lmask value (stale masks of dropped
layers are then ignored) is visible to the composite.

### 4.5 Edge cases a replacement must copy

- The OBJ window mask `S+0xea0` is written only by `render_scanline_obj_c` (DISPCNT.12 set). With the OBJ window
  enabled and OBJ display off it is whatever the previous line left at that stack address (obj.md 5). Not
  reproducible exactly; pick a rule (empty).
- With all windows disabled `fx` is zero and `inh` is not touched (apply_windows returns early).
- Windows are 1x: at 2x each window pixel covers 2x2 output pixels; the masks are not resampled.
- WININ/WINOUT are stored `& 0x3f3f3f3f` (frame.md 3), the window edges as raw bytes, no clamping of X2 > 256 etc.

---

## 5. render_scanline_2d_composite(eng x0, out x1, S x2, layers x3, p3d x4, alpha x5, lmask w6, bldcnt w7, [sp] flags, [sp+8] line) 0x3c6d0

`line` is not read. `bldcnt` is `bldcnt'` of 3.2; `shade_asm` reads `[eng+0xa0]` (bit 6 only) and BLDY `[eng+0xa2]`,
the blend setups read BLDALPHA `[eng+0xa4]`. Everything [M][R].

### 5.1 Simple path (`flags & 7 == 0`)

```
priority_encode_single(eng, S+0xda0, X = S+0x10c0)                    (spec/composite.c)
if !(flags & 8): select_pixels(eng, out, X, layers, p3d, NULL, lmask); return      (spec/composite.c)
select_pixels(eng, S+0x1180, X, layers, p3d, NULL, lmask)
select_blend_enable(T = S+0x1480, X, lmask, bldcnt & 0x3f);  T &= ~fx
shade(eng, out, S+0x1180, T)
```

`shade_asm` 0xa0108: `f = min(2*BLDY, 32)`; `add = BLDCNT.6 ? 16 : 63*f + 16`; per pixel and channel
`out = T ? (c*(32-f) + add) >> 5 : c` (16-bit arithmetic, `>> 5` truncated to 8 bits; never overflows for c <= 255).

### 5.2 Complex path

```
lmask2 = 0
if lmask: lmask2 = lmask & (bldcnt >> 8);  if !(eng[0x84] & 4): lmask2 &= ~(1 << eng[0x84])
                                            (the frontmost listed BG can never be a second layer)
priority_encode_double(eng, S+0xda0, TOP = S+0x13c0, SEC = S+0x1480)
select_pixels(eng, P = S+0x1540, TOP, layers, p3d, (flags & 8) ? NULL : alpha, lmask)      top planes
select_pixels(eng, S+0x1840,     SEC, layers, p3d, NULL, lmask2)                           second planes
select_blend_enable(T1 = S+0x1b40, TOP, lmask, bldcnt & 0x3f)          top layer is a 1st target
select_blend_enable(T2 = S+0x1b60, SEC, lmask, (bldcnt >> 8) & 0x3f)   second layer is a 2nd target
T1 &= ~fx
M = S+0x1b80; f = flags
switch (flags & 5):
  4: M = T1 & T2                                                       BLDALPHA blend
  5: M = ((SEMI & TOP.obj) | T1) & T2;  f &= ~1                        SEMI = S+0xec0, TOP.obj = TOP+0x80
  1: M = SEMI & TOP.obj & T2;           f = (f & ~1) | 4
  0: (M not written)
if flags & 5:
  if f & 0x10: M &= ~TOP.bg0;  f &= ~0x10                              opaque 3D: no BLDALPHA blend
  if f & 0x20: M &= ~TOP.obj;  f &= ~0x20                              OBJ image: no BLDALPHA blend
init = f & 4; use_off = 0
if f & 8:                                                               brightness
    fy = BLDY <= 16 ? 2*BLDY : 32
    darken (bldcnt.6): EVA = T1 ? 32-fy : 32;  EVB = 0
    brighten:          EVA = T1 ? 32-fy : 32;  EVB = 0;  OFF = T1 ? fy : 0;  use_off = 1
    if init: setup_blend(BLDALPHA, EVA, EVB, M)                         overwrite where M (semi OBJ / alpha mode)
    init = 1
elif init: setup_blend_base(BLDALPHA, EVA, EVB, M)
if (f & 2) && alpha:                                                    per-pixel alpha (bitmap OBJ, 3D)
    M = BMP & TOP.obj & T2                                              BMP = S+0xee0
    if !(bldcnt & 0x80) && p3d: M |= TOP.bg0 & T2                       3D: only in BLDCNT modes 0 and 1
    init ? setup_alpha(EVA, EVB, alpha, M) : setup_alpha_base(EVA, EVB, alpha, M)
use_off ? apply_offset_c(out, P, EVA, EVB, OFF) : apply_asm(out, P, EVA, EVB)
```

`EVA = S+0x10c0`, `EVB = S+0x11c0`, `OFF = S+0x12c0`; P = top planes followed by the second planes (6 x 256 B). If
neither the brightness, the alpha mode nor the per-pixel alpha step runs, EVA/EVB are not written and `apply`
reads stale bytes. That needs `flags & 7 == 2` with `alpha == NULL`, which render_scanline_2d never passes (alpha
is non-NULL exactly when `flags & 2`; [R]).

### 5.3 The helpers [M]

- `priority_encode_double_asm(eng, vis, top, sec)` 0x9fe98: walks the list `eng[0x84 .. 0x84+eng[0xb3])` front to
  back with `cov` (pixels covered by 1+ layers), `cov2` (2+), `objcov`, `objtop`, `objsec`:
  BG slot k: `top[k] = v & ~cov; sec[k] = v & ~cov2 & cov; cov2 |= cov & v; cov |= v`;
  OBJ slot: `v &= ~objcov; objtop |= v & ~cov; objsec |= v & ~cov2 & cov; cov2 |= cov & v; cov |= v; objcov |= v`;
  then `top.bd = ~cov`, `sec.bd = cov & ~cov2`, `top.obj = objtop`, `sec.obj = objsec`. A pixel with no layer has a
  backdrop top and **no** second layer (so no 2nd target: the backdrop is never blended as a 1st target). Slots of
  BGs not in the list keep stale bytes (also in `spec_..._single`).
- `select_blend_enable_asm(out, excl, lmask, bits)` 0xa0070: `acc = 0`; for k = 0..4 with lmask bit k:
  `bits.k ? acc |= excl[k] : acc &= ~excl[k]`; then the backdrop unconditionally: `bits.5 ? acc |= excl[5] :
  acc &= ~excl[5]`; out = acc. With exclusive masks: bit i = the owner of pixel i is selected.
- `setup_blend_base_asm(bldalpha w0, eva, evb, mask)` 0xa0254 / `setup_blend_asm` 0xa02e0: `w = 2*BLDALPHA`,
  `a = min(w & 0x3f, 32)` (= `min(2*EVA, 32)`), `b = min((w >> 8) & 0x3f, 32)` (= `min(2*EVB + BLDALPHA.7, 32)`);
  base: `eva = mask ? a : 32, evb = mask ? b : 0`; non-base: only where mask.
- `setup_alpha_base_asm(eva, evb, alpha, mask)` 0xa042c: `a' = mask ? alpha[i] : 31; eva = a'+1; evb = 31-a'`
  (u8); `setup_alpha_asm` 0xa04b4: only where mask: `eva = alpha+1, evb = 31-alpha`.
- `color_effects_apply_asm(out, P, eva, evb)` 0xa0378: per channel
  `s = (t*eva + s2*evb) mod 2^16; out = min(63, ((s + 16) >> 5) & 0xff)`.
- `color_effects_apply_offset_c(out, P, eva, evb, off)` 0x39f00: `s = t*eva + s2*evb + (63*off + 16)`;
  `out = s > 0x7ff ? 63 : s >> 5` (NEON and scalar halves agree; the scalar runs only for overlapping buffers).

### 5.4 Which pixels get which effect (the semantics of 5.2)

Let t = top layer, s = second layer of the pixel (from the two-deep encoder), T1 = "t is a 1st target and effects are
enabled at the pixel" (`BLDCNT` bits 0-5 of the *enabled* layers, `& ~fx`), T2 = "s exists and is a 2nd target"
(BLDCNT bits 8-13; not masked by fx).

| condition (per pixel) | EVA | EVB | OFF | notes |
|---|---|---|---|---|
| nothing applies | 32 | 0 | 0 | out = min(63, t) |
| alpha mode (flags.2), `T1 & T2`, t not 3D-on-an-opaque-line (0x10), t not the OBJ image (0x20) | min(2EVA,32) | min(2EVB,32) | | BLDALPHA |
| semi-transparent OBJ top (SEMI & t=OBJ), `T2`, no OBJ image | min(2EVA,32) | min(2EVB,32) | (brighten: OFF from the next row stays) | any BLDCNT mode; a semi OBJ is a 1st target by itself |
| brightness (flags.3), `T1` | 32-fy | 0 | brighten: fy | fy = 2*min(BLDY,16); then semi/alpha rows above overwrite EVA/EVB |
| bitmap OBJ top (BMP & t=OBJ), `T2`, flags.1 | a+1 | 31-a | unchanged | a = 2*alpha+1 (alpha 0..15): EVA = 2(alpha+1), EVB = 2(15-alpha) |
| 3D top (t=BG0), `T2`, flags.1, BLDCNT mode 0/1, p3d | a+1 | 31-a | unchanged | a = 3D alpha (31: no blend); overrides BLDALPHA |

The last two rows run after the others and overwrite EVA/EVB where they apply. In the simple path (no semi OBJ,
no bitmap OBJ, no translucent 3D, no alpha mode) the brightness gives the same values (`shade_asm` vs
`apply_offset`/`apply` with EVB = 0: identical for 6-bit inputs).

### 5.5 Output formulas (6-bit planes; scanout = value << 2)

```
plain       out = t                                     (2D: 2*c5; 3D: r6/g6/b6 as rendered)
alpha       out = min(63, (t*A + s*B + 16) >> 5)          A = min(2*EVA, 32), B = min(2*EVB + BLDALPHA.7, 32)
3D / bmp    out = min(63, (t*(a+1) + s*(31-a) + 16) >> 5)
brighten    out = min(63, (t*(32-f) + 63*f + 16) >> 5)    f = min(2*BLDY, 32);   = t + ((63-t)*f + 16) >> 5
darken      out = min(63, (t*(32-f) + 16) >> 5)
semi + brighten (pixel also T1)  out = min(63, (t*A + s*B + 63*f + 16) >> 5)
```

For 2D-on-2D blending with c5 values: `(2t5*2EVA + 2s5*2EVB + 16) >> 5 = (t5*EVA + s5*EVB + 4) >> 3`, a 6-bit
result (DraStic keeps one more bit than the 5-bit hardware formula **[G]** on the hardware part).

### 5.6 The per-pixel closed form of the whole composite [C]

Everything of 5.1-5.5 collapses into an independent function of each pixel. This is the reference a fused
replacement implements (`re/2d/compose/pix/pixel.h`, `px_pixel()`; equal to `cs_composite`, hence to DraStic, on every
input render_scanline_2d can produce: lmask contains every listed BG whose vis slot is not empty, OBJ vis slots are
empty when `render_scanline_obj_c` returned 0, the flags and bm are derived as in 3.2, vis[0] is the 3D alpha map
when BG0 is 3D).

```
inputs of a 256-pixel line / quarter: eng (list [eng+0x84..+0xb3), BLDY [eng+0xa2], BLDALPHA [eng+0xa4], backdrop
  *[eng+0x18]), vis[8] (S+0xda0), the u16 lines L0..L3 and LOBJ, p3d (or NULL), alpha (the plane passed, or NULL),
  SEMI (S+0xec0), BMP (S+0xee0), FX (S+0xfa0), lmask, bm (the bldcnt argument), flags
E(c)    = ((c & 31) << 1, (c >> 5 & 31) << 1, (c >> 10 & 31) << 1)               bit 15 ignored
col(X)  = X == BD ? E(backdrop) : X == BG0 && p3d && lmask.0 ? (byte0, byte1, byte2) of p3d[i] : E(L_X[i])

per pixel i:
  walk the list front to back; slot s < 4: BG s is present if vis[s].i; slots 4..7: OBJ is present if vis[s].i,
    counted once (at the first OBJ slot that has the bit)
  t = the first present layer, BD if none
  u = the second present layer; BD if exactly one layer is present; none if no layer is present
  T1 = bm.(t == BD ? 5 : t) && !FX.i                       t is a 1st target, effects enabled here
  T2 = u != none && bm.(u == BD ? 13 : 8 + u)               u is a 2nd target (FX not applied)
  fy = BLDY <= 16 ? 2*BLDY : 32;   bright = !bm.6
  if (flags & 7) == 0:                                      SIMPLE
      out = col(t)
      if (flags & 8) && T1: out = (col(t)*(32 - fy) + (bright ? 63*fy : 0) + 16) >> 5
      return out                                            (no clamp needed)
  A = min((2*BLDALPHA) & 0x3f, 32);  B = min(((2*BLDALPHA) >> 8) & 0x3f, 32)
  semi = SEMI.i && t == OBJ
  M = { flags & 5 == 4: T1 && T2;   == 5: (semi || T1) && T2;   == 1: semi && T2;   == 0: false }
  f = flags; if (flags & 5) == 1: f |= 4
  if flags & 5: if f & 0x10: M = M && t != BG0;   if f & 0x20: M = M && t != OBJ
  (EVA, EVB, OFF) = (32, 0, 0)
  if f & 8:      EVA = T1 ? 32 - fy : 32;  OFF = bright && T1 ? fy : 0;  if (f & 4) && M: (EVA, EVB) = (A, B)
  else if (f & 4) && M: (EVA, EVB) = (A, B)
  if (f & 2) && alpha:
      a = (t == BG0 && p3d && lmask.0 && !(flags & 8)) ? p3d[i] >> 24 : alpha[i]
      if (BMP.i && t == OBJ && T2) || (!bm.7 && p3d && t == BG0 && T2): (EVA, EVB) = ((a + 1) & 0xff, (31 - a) & 0xff)
  out = min(63, (col(t)*EVA + col(u)*EVB + 63*OFF + 16) >> 5)     per channel; col(u) matters only where T2
```

Notes. Layer numbers: BG0..BG3 = 0..3, OBJ = 4 (so `bm.t` for OBJ is bit 4, `bm.(8+u)` bit 12). The `& 0xffff` wraps
of `apply_asm` / `shade_asm` and the u8 wraps of the setups never trigger for these inputs (6-bit colours, a <= 31).
Where `flags & 7 == 2` and `alpha == NULL` DraStic reads stale EVA / EVB (5.2); render_scanline_2d never passes that.
A pixel whose second layer is not a 2nd target has EVB = 0 in every branch, so the second planes' content there
(whatever `select_pixels` left for layers outside lmask2) never matters. The simple and complex branches agree
whenever no 2nd target is enabled (5.2, `model/native/t_equiv.c`): a quarter forced into the complex path by one
translucent 3D pixel but without any 2nd target can be computed with the simple formula.

### 5.7 The blend and window registers as DraStic stores and reads them [D]

| register | engine field (event replay, frame.md 3) | read by | meaning in DraStic |
|---|---|---|---|
| BLDCNT 0x050 | `eng+0xa0` u16, as written (0x42360; a 32-bit write also sets BLDALPHA, 0x42370) | render_scanline_2d (bm, mode, 3.2), `shade_asm` (bit 6) | bits 0-5 1st targets BG0..BG3, OBJ, BD; bits 6-7 mode (0 none, 1 alpha, 2 brighten, 3 darken); bits 8-13 2nd targets; bits 14-15 ignored. The composite gets `bm = BLDCNT & (lm \| lm<<8 \| 0xf0f0)` (BG bits of disabled BGs cleared) |
| BLDALPHA 0x052 | `eng+0xa4` u16, as written | `setup_blend(_base)_asm` | `A = min(2*(bits 0-4), 32)`, `B = min(2*(bits 8-12) + bit 7, 32)` in 1/32; bits 5, 6, 13-15 ignored; EVA, EVB > 16 saturate at 16/16 |
| BLDY 0x054 | `eng+0xa2` u16 `& 0x1f` (0x42488) | render_scanline_2d (flag 8 only if != 0), the composite, `shade_asm` | `fy = min(2*BLDY, 32)`; BLDY 16..31 = full white / black |
| MASTER_BRIGHT 0x06c | `eng+0xa6` u16, as written (0x42450) | render_scanline's convert (section 8) | bits 14-15 mode (0, 3 none; 1 up; 2 down), bits 0-4 factor, `f = 2*factor`; factor 0 = none; factor 16..31 = memset white / black |
| WININ / WINOUT 0x048 | `eng+0x9c` u32 `& 0x3f3f3f3f` (0x42628) | generate_window_masks (inverted: `^ 0x3f3f3f3f`) | byte 0 WIN0, 1 WIN1, 2 outside, 3 OBJ window; bits 0-4 BG0..BG3, OBJ, bit 5 effects |
| WIN0H / WIN1H 0x040 / 0x042 | `eng+0xaa` / `+0xac` u16 and dirty `eng+0xb5` bit 0 / 1 (0x424e8, 0x424cc) | update_window_mask on the next line with windows on | X1 = bits 8-15 (first pixel inside), X2 = bits 0-7 (first pixel outside); 4.1 |
| WIN0V / WIN1V 0x044 / 0x046 | `eng+0xae` / `+0xb0` u16 | generate_window_masks every rendered line with windows on; render_scanline at line 0 | Y1 = bits 8-15 (line == Y1 activates), Y2 = bits 0-7 (line == Y2 deactivates, tested after Y1); 4.2 |
| DISPCNT 0x000 | `eng+0x90` (engine B `& 0xc0b1fff7`) | render_scanline_2d (bit 3 3D, bits 8-12 layers), windows (13-15), render_scanline (16-17 display mode) | DISPCNT.7 (forced blank) is ignored (frame.md 5) |
| BG0HOFS 0x010 | `L(0)+0x9a = eng+0x15a` u16 `& 0x1ff` | render_scanline_2d: `sext9` -> horizontal_shift_3d | the 3D layer's horizontal shift (5.8); BG0VOFS is never read for the 3D layer |

All of them follow the line-exact event replay: a write during line L takes effect from line L+1, a write during
vblank at once (frame.md 3).

### 5.8 render_scanline_horizontal_shift_3d(dst x0, src x1, hofs w2) 0x3c630 [M][D]

`hofs >= 0`: `memcpy(dst, src + hofs, (256 - hofs)*4); memset(dst + 256 - hofs, 0, hofs*4)`; `hofs < 0`:
`memcpy(dst - hofs, src, (256 + hofs)*4); memset(dst, 0, -hofs*4)` (libc calls; `hofs = sext9(BG0HOFS)`, -256..255).
So `dst[i] = src[i + hofs]` inside the line, 0 (alpha 0: transparent) outside. Called when the 3D line is fetched
and `hofs != 0`: once per line in the 1x path (0x400fc, `dst = S`, and the line pointer itself becomes S, also for
the capture), once per quarter in the 2x path (each 256-entry quarter is shifted by `hofs` entries = `hofs` DS
pixels = `2*hofs` output pixels: exact at 2x). The shifted copy overwrites S+0x000..0x3ff, i.e. the BG0 u16 line
buffer at S+0x1e0 (unused when BG0 is 3D).

---

## 6. The scratch area S (render_scanline_2d's frame: sp .. sp+0x1d30, S = sp+0x180)

Extends `spec/composite.c`'s S_VIS / S_EXCL table. W = written by, R = read by.

| S + | size | contents | W | R |
|---|---|---|---|---|
| 0x000 | 0x400 | BG0HOFS-shifted 3D line/quarter (u32[256]); overlaps BG0's line buffer | horizontal_shift_3d | set_3d_visibility, select_pixels, capture |
| 0x1e0, 0x400, 0x620, 0x840 | 0x220 each | BG0..BG3 line buffers: 8 + 256 + 8 u16 (`layers[k]`, pixels at +0x10) | render_scanline_bg | select_pixels |
| 0xa60 | 0x220 | OBJ line buffer (pixels at 0xa70) | obj_c | select_pixels |
| 0xc90 | 0x100 | alpha plane | obj_c, binary32_alpha | setup_alpha(_base), memcpy to 0xfc0 |
| 0xda0 | 0x100 | vis[8]: BG0..BG3, OBJ prio 0..3 | bg, obj_c, set_3d_visibility, bit15 bitmaps, apply_windows | encoders, disable_blank |
| 0xea0 | 0x20 | OBJ window mask | obj_c | window inhibit |
| 0xec0 | 0x20 | SEMI: top OBJ pixel is semi-transparent | obj_c | composite (cases 1, 5) |
| 0xee0 | 0x20 | BMP: top OBJ pixel is a bitmap OBJ | obj_c | composite (per-pixel alpha) |
| 0xf00 | 0xa0 | inh[5]: BG0..BG3, OBJ | generate_window_masks | apply_windows |
| 0xfa0 | 0x20 | fx: effects disabled | generate_window_masks | composite |
| 0xfc0 | 0x100 | 2x: per-quarter copy of the alpha plane (q = 0..2) | memcpy, binary32_alpha | setup_alpha |
| 0x10c0 | 0xc0 | simple path: excl[6] | priority_encode_single | select_pixels, select_blend_enable |
| 0x1180 | 0x300 | simple + shade: planes before the shade | select_pixels | shade |
| 0x1480 | 0x20 | simple + shade: shade mask | select_blend_enable | shade |
| 0x10c0 | 0x100 | complex: EVA | setups | apply(_offset) |
| 0x11c0 | 0x100 | complex: EVB | setups | apply(_offset) |
| 0x12c0 | 0x100 | complex, brighten: OFF | composite | apply_offset |
| 0x13c0 | 0xc0 | complex: TOP masks (BG0..3, OBJ +0x80, BD +0xa0) | priority_encode_double | select_pixels, blend enable, composite |
| 0x1480 | 0xc0 | complex: SEC masks | priority_encode_double | select_pixels, blend enable |
| 0x1540 | 0x300 | complex: top planes | select_pixels | apply |
| 0x1840 | 0x300 | complex: second planes | select_pixels | apply |
| 0x1b40 / 0x1b60 / 0x1b80 | 0x20 each | complex: T1, T2, M | composite | composite, setups |
| (frame) sp+0x148 | 16 | direct BG2/BG3 line base pointers | | |
| (frame) sp+0x158 | 40 | layers[5] | | composite (x3) |
| (frame) sp+0xb8 / +0x144 / +0xc0 / +0xc8 | | flags_line, lmask, p3d, OBJ image | | |

Only `out` (the planes in render_scanline's frame), the alpha plane and S change in a composite call; nothing in
`eng`. Every S region a call reads is either written earlier in the same line or is stale scratch that cannot
influence the output (masks of unlisted BGs, second-plane garbage under EVB = 0) **[M]** (the model reproduces the
stale reads byte for byte).

---

## 7. Display capture [M] (routines), [D] (dispatch)

Condition (after the composite of the line, 0x3f6a0): `C && C[0x51] && line < C[0x50]`.
`off = (u32[C+0x48] + u16[C+0x4c]*line) & 0xffff` (pixels; wraps in the 128 KiB bank), `dst = [C+0x28] + 2*off`,
`srcB = [C+0x40]` (set per line by render_scanline: VRAM line, FIFO line or NULL). Per quarter; with the `hires`
argument 0 (1x mode) only q = 0, with it set all four, whether the composite took the 1x or the 2x path:

```
capture_q(dst, b, a2d, a3d):
  if C[0x51] == 2:  a3d ? (C[0x53] ? capture_blended_3d(C, dst, b, a3d) : capture_direct_3d(C, dst, a3d)) : nothing
  else:             C[0x53] ? capture_blended(C, dst, b, a2d) : capture_direct(C, dst, a2d)
hires argument set: q = 1, 2, 3 into [C+0x30] + 6*off + (q-1)*0x200, then q = 0 into dst
   a2d = p3d ? planes + q*0x300 : planes (all four!),  a3d = p3d ? p3d + q*0x400 bytes : NULL
   b = srcB for q = 0; for q >= 1 the hi-res copy [C + 8*C[0x4f]] + line*0x600 + (q-1)*0x200 if
       C[0x20 + C[0x4f]] bit (line >> 5) is set (the bank shown in VRAM display mode holds hi-res data), else srcB
```

- `capture_direct_asm(C, dst, planes)` 0xa0910: `dst[i] = (((G>>1)<<5) + (R>>1)) | 0x8000 | ((B>>1) << 10)` (u16),
  i < width = `[C+0x4c]` (128 or 256; the loop works in 32-pixel steps).
- `capture_direct_3d_asm(C, dst, px)` 0xa09b0: `dst = ((g6>>1)<<5) + (r6>>1) | ((b6>>1)&0x1f)<<10 | (s8(a) > 0) << 15`.
- `capture_blended(C, dst, srcB, planes)` 0x3bc60 and `_3d` 0x3bdb0: `eva = C[0x54]`, `evb2 = 2*C[0x55]` (both 0..16);
  `eva == 16 && evb2 == 0` -> the direct routine. Else, if source B is missing (`[C+0x40] == NULL` for the 2D
  version, the `srcB` argument for the 3D version): `c = (A6*eva) >> 5` per channel, `dst = 0x8000 | R | G<<5 |
  (B & 0x3f)<<10`. Otherwise per channel `v = A6*eva + B5*evb2` (B5 from srcB's BGR555, bit 15 ignored),
  `c = v < 0x400 ? v >> 5 : 31`, `dst = 0x8000 | R | G<<5 | B<<10`. **Bit 15 is always set**, also for 3D pixels of
  alpha 0 (only the direct 3D capture tests alpha).
- Hazards [D]: no 3D capture unless DISPCNT.3 (p3d is only fetched then); with BG0HOFS in the 1x path the
  hi-res 3D quarters q1..q3 are read from S+0x400.. (BG1's line buffer), q0 from the shifted copy; in the 2x path
  the capture uses the unshifted 3D; without 3D the hi-res 2D quarters all come from q0's planes.

After the frame `update_frame` marks the written 16 KiB blocks as holding hi-res data (frame.md 1.2).

---

## 8. Scanout conversion (render_scanline 0x4067c..)

`MB = [eng+0xa6]`, `m = MB >> 14`, `f = (MB & 0x1f) << 1` [D]. 32 bpp (the device and the simulator):

| case | 2x (hires) | 1x |
|---|---|---|
| m = 0 or 3, or f = 0 | `direct_32_2x(q0, q1, out)`, `direct_32_2x(q2, q3, out + pitch/2)` | `direct_32_1x(planes, out)` |
| m = 1 (brighten), f <= 31 | `shade_32_2x(q0, q1, out, 32-f, 63f+16)`, same for q2, q3 | `shade_32_1x(planes, out, 32-f, 63f+16)` |
| m = 2 (darken), f <= 31 | `shade_32_2x(.., 32-f, 16)` | `shade_32_1x(.., 32-f, 16)` |
| m = 1, f > 31 (MB factor >= 16) | memset 0xff, 0x800 B per row (white, byte 3 = 0xff) | memset 0xff 0x400 B |
| m = 2, f > 31 | memset 0x00 (black, byte 3 = 0) | memset 0x00 |

(1x without the 2x path: q0 = q1 = q2 = q3 = planes, i.e. 2x2 pixel doubling; display mode 0 / 2 / 3 also give one
plane set, frame.md 5.)

- `direct_32_1x_asm(planes, dst)` 0xa0a80 [M]: `dst[i] = (B<<2) | (G<<2)<<8 | (R<<2)<<16 | 0xff<<24` (each `<<2` in
  8 bits).
- `direct_32_2x_asm(E, O, dst)` 0xa0ae0 [M][R]: `dst[2i] = conv(E[i])`, `dst[2i+1] = conv(O[i])`; byte 3 = 0xff where
  `(x & 15) in {0, 2, 4, 6}`, else 0 (the `movi v19.4h` constant), x = output pixel.
- `shade_32_1x_asm(planes, dst, w2 factor, w3 add)` 0xa0c90 [M]: per channel
  `c' = (u8)(((c*factor + add) mod 2^16) >> 5) << 2` (factor as a byte, add as a u16); byte 3 = 0xff.
- `shade_32_2x_asm(E, O, dst, w3 factor, w4 add)` 0xa0d80 [M][R]: per channel
  `c' = (u8)((((c<<2 & 0xff)*factor + add) mod 2^16) >> 5)`, **the `<<2` is applied first**. Byte 3 of output pixel x
  (k = x & 31, 16 input pixels per iteration n = x >> 5) = byte (k & 15) of 8 u16 accumulators:
  `k < 16`: `acc_j = (((B_E[16n+8+j] << 2) & 0xff)*factor + add) mod 2^16`; `k >= 16`: the same with
  `G_O[16n+j]` (the registers v19 and v23 are reused as accumulators).

**The 2x brighten bug** [M][R]: with `add = 63f+16` the 1x routine computes `c6 + ((63-c6)f+16)>>5` and then shifts
to 8 bits; the 2x one computes `(4*c6*(32-f) + 63f + 16) >> 5` on the already shifted value, so the brightening
term is a quarter of what it should be and bright colours get *darker*. Examples (8-bit output, 1x value in
brackets): MASTER_BRIGHT = 0x4008 (brighten, factor 8, f = 16): white (c6 = 63) -> 0x9e (0xfc), black -> 0x20
(0x80); factor 15 (f = 30): white -> 0x4b (0xfc), black -> 0x3b (0xec); factor 16..31: pure white (both). A fade
to white at 2x therefore goes through dark grey and then jumps to white. Darken (`add = 16`) is right at 2x (and
more precise than 1x: `(4*c6*(32-f)+16)>>5` vs `((c6*(32-f)+16)>>5)<<2`, which differ in the low 2 bits). Reached in
T7 q3 (46 080 brighten calls of `shade_32_2x` in 50 s, all matching the model). An exact replacement reproduces it;
fixing it is a deliberate, visible difference.

### 8.1 End to end: one output pixel

With `o = (R, G, B)` the 6-bit composite output of 5.6 for DS pixel i of quarter q (1x path: the line's output for
all four q), output pixel `(x, y) = (2i + (q & 1), 2L + (q >> 1))` of engine X's screen is the u32 at
`[engX+0x38] + L*pitch + (q >> 1)*pitch/2 + 4*x` (pitch 0x1000 at 2x / 32 bpp), bytes B, G, R, X:

```
MB mode 0 / 3, or factor 0:   c8 = (c6 << 2) & 0xff                                       X = 0xff if (x & 15) in {0,2,4,6} else 0
brighten (1), factor 1..15:   c8 = ((((c6 << 2) & 0xff)*(32 - f) + 63*f + 16) & 0xffff) >> 5   (& 0xff)  X = shade junk (above)
darken (2), factor 1..15:     c8 = ((((c6 << 2) & 0xff)*(32 - f) + 16) & 0xffff) >> 5
factor 16..31:                0xffffffff (brighten) / 0x00000000 (darken)
                              f = 2*factor; at 1x the shade is ((c6*(32-f) + add) >> 5) << 2 and X = 0xff
```

So the scanout of a 2x line is two `direct_32_2x` (or `shade_32_2x`) calls: `(q0, q1)` -> row 2L, `(q2, q3)` ->
row 2L+1, even output pixels from the first plane set, odd from the second. The 0.277 M a frame of `direct_32_2x`
in the profiles is 768 calls = 2 rows x 192 lines x 2 engines at 361 instructions (0.7 per output pixel); engine
A's half is 0.139 M. An engine in display mode 0 (off) converts a white plane set (`memset 0xff` of 0x300 B, which
the convert turns into 0xfc per channel, not 0xff).

16 bpp (not used by dsflip): `direct_16_1x/2x_asm` (RGB565 `R5 | G6 | B5` from the 6-bit planes: `B>>1`, `G` 6-bit,
`R<<2 >> 3`...), `shade_16_1x` inline NEON at 0x40ec0 / 0x40d14, `shade_16_2x_c` 0x3da70 **[D, not analysed]**.

---

## 9. Costs [P][B]

Instructions executed (qemu block profile, `fnprof`). Two sources: **[P]** the running emulator (`compose/prof/`:
`prof.sh` = `rast/prof.sh` with our 3D raster on and our compositor hooks off, `RAST_COMP=0 RAST_COMPFUSE=0`, 90 s,
`fnreport2.py` per function, `stages.py` per stage; 2x, 32 bpp) and **[B]** DraStic's routines called directly on
fixed inputs (`compose/bench/bench.sh` + `model/t_bench.c`: 1 000 calls under the same plugin, minus a 0-call run).
Asm entry labels are folded into their routine (`priority_encode_*_bg_layer` etc.). The profile has no thread
attribution; on a normal frame engine A runs on the emulation thread, engine B on `video_render_thread` (frame.md 2).

### 9.1 Per call, by scenario [B]

Composite of one 256-pixel line or quarter (`render_scanline_2d_composite` with everything under it). Inputs: an
8-entry list (OBJ slot + BG per priority), visibility in spans (40-70 % coverage), 6-bit colours, opaque 3D unless
stated.

| scenario | path | total | main items |
|---|---|---|---|
| 3D only, BG0 everywhere (S7, L4) | simple | **620** | expand 229, binary32 178, binary_scalar 72, select_pixels 70, composite 42, encoder 29 |
| 3D + backdrop | simple | 644 | binary_scalar 96 |
| 3D + 2 BGs + OBJ | simple | 1 094 | select_pixels_binary 301, encoder 129 |
| 4 BGs + OBJ, no 3D | simple | 1 000 | select_pixels_binary 380 |
| 4 BGs + OBJ, BLDCNT brighten | simple + shade | 1 593 | shade 468, blend enable 98 |
| 3D only, BLDCNT brighten | simple + shade | 1 213 | shade 468 |
| 3D + HUD, BLDCNT darken | simple + shade | 1 687 | |
| 3D + HUD, BLDCNT alpha, opaque 3D (flags 0x14) | complex | 2 686 | apply 555, expand x2 458, binary 404, blend enable x2 196, double encoder 226, setup_blend_base 98 |
| translucent 3D, no 2nd target (flags 2) | complex | 2 624 | apply 555, expand 458, binary32_alpha 202, setup_alpha_base 112 |
| translucent 3D, 2nd targets | complex | 2 895 | |
| translucent 3D + BLDCNT brighten (flags 0xa) | complex + offset | **6 102** | composite self 2 395 (the scalar EVA/EVB/OFF loops, ~9 a pixel), apply_offset_c 1 575, setup_alpha 136 |
| 2D BLDALPHA (mode 1) | complex | 2 698 | |
| 2D BLDALPHA + semi-transparent OBJ (flags 5) | complex | 2 723 | |
| semi-transparent OBJ + BLDCNT darken (flags 9) | complex + brightness | 4 854 | composite self 2 306, setup_blend 107 |

Windows, per DS line (`generate_window_masks` with its helpers + one `apply_windows` over 5 layers):

| scenario | total | items |
|---|---|---|
| windows off | 16 + 4 per apply_windows call | early returns |
| WIN0 only, WIN0H rewritten (mask rebuilt) / unchanged | 440 / 317 | apply 167, generate 79-100, update_window_mask 47 per rebuilt mask, inhibit_single 71 |
| WIN0 + WIN1, rebuilt | 515 | inhibit_double 145 |
| WIN0 + WIN1 enabled, neither active on the line | 372 | the "outside only" fill |
| WIN0 + WIN1 + OBJ window, rebuilt / unchanged | 554 / 431 | inhibit_triple 179 |

Capture (256 pixels a call): `capture_direct_asm` 292, `capture_direct_3d_asm` 282, `capture_blended` **8 465**,
`capture_blended_3d` **8 719** (scalar C, 33-34 instructions a pixel; 0x3bc60, 0x3bdb0). Scanout conversion:
`direct_32_2x` 361 per 512-pixel row, `shade_32_2x` 937, `direct_32_1x` 135 per 256-pixel row, `shade_32_1x` 423.

### 9.2 Per DS line of engine A at 2x

Measured in S7 (3D only, every quarter on the simple path; L4 is identical for engine A) [P]:

| stage | calls a line | per call | per line |
|---|---|---|---|
| render_scanline (self) | 1 | 120 | 120 |
| render_scanline_2d (self: flags, layer table, quarter loop) | 1 | 490 | 490 |
| render_scanline_3d, render_scanline_bg (no 2D BG) | 1 + 1 | 10, 26 | 36 |
| generate_window_masks (windows off) | 1 | 16 | 16 |
| apply_windows (off) | 5 | 4 | 20 |
| disable_blank_layers | 1 | 26 | 26 |
| set_3d_visibility + gather_3d_alpha | 4 | 424 | 1 697 |
| composite, simple, 3D only | 4 | 620 | 2 479 |
| color_convert_direct_32_2x | 2 | 361 | 722 |
| **total** | | | **~5 600** (x192 = **1.08 M a frame**, + `video_2d_reorder_obj` ~0.006 M) |

The same line under other conditions, composed from the per-call costs of 9.1 (estimates [B]; the BG / OBJ
renderers of `bg.md` / `obj.md` come on top):

| engine A line at 2x | per line | per frame |
|---|---|---|
| 3D only (above) | 5 600 | 1.08 M |
| 3D + HUD (2 BGs + OBJ), no effects | ~7 500 | 1.44 M |
| 3D + HUD with BLDCNT alpha over the 3D | ~13 900 | 2.7 M |
| translucent 3D somewhere in every quarter, 2nd targets | ~14 700 | 2.8 M |
| translucent 3D + BLDCNT brightness | ~27 500 | 5.3 M |
| + windows (1x masks; per-quarter apply_windows for the 3D layer ~40 each, estimated) | +300..700 | +0.06..0.13 M |
| + master brightness fade (shade_32_2x) | +1 150 | +0.22 M |
| + blended display capture of the line (4 quarters) | +34 900 | +6.7 M |
| + direct display capture | +1 170 | +0.22 M |
| 2D-only screen, 1x composite (no direct layer / OBJ image), BLDALPHA, 2x convert | ~3 800 | 0.73 M |
| 2D screen with a direct 16-bit BG2/BG3 or the OBJ image: 4 identical quarters | ~3 700 (simple) | 0.72 M (L4 engine B on the render thread, measured) |

### 9.3 Per frame in the emulator [P]

M instructions a frame, all engines (engine B is display-off in S7 and the T scenes: its share is
render_scanline + memset + two converts a line, 0.162 M; in L4 it shows a direct 16-bit bitmap through the
4-quarter path).

| stage | S7 | L4 | T6 windows | T7 effects | T9 3D + capture |
|---|---|---|---|---|---|
| render_scanline (self) | 0.046 | 0.046 | 0.046 | 0.046 | 0.048 |
| render_scanline_2d (self) | 0.094 | 0.295 | 0.040 | 0.040 | 0.129 |
| set_3d_visibility + gather | 0.326 | 0.327 | | | 0.319 |
| horizontal_shift_3d | | | | | 0.002 |
| windows (generate + apply) | 0.007 | 0.014 | 0.080 | 0.004 | 0.024 |
| disable_blank_layers | 0.005 | 0.011 | 0.005 | 0.005 | 0.005 |
| composite and everything under it | 0.476 | 0.821 | 0.275 | 0.389 | 1.661 |
| of which shade / apply / apply_offset | | | 0.089 / - / - | 0.021 / 0.080 / - | 0.048 / 0.187 / 0.064 |
| capture | | | | | **1.380** |
| convert (direct / shade 2x) | 0.277 | 0.277 | 0.277 | 0.245 / 0.043 | 0.277 |
| **compose total** | **1.231** | **1.791** | **0.723** | **0.772** | **3.845** |
| of which engine A | 1.07 | 1.07 | 0.56 | 0.61 | 3.68 |
| for scale: render_scanline_obj_c / BG renderers | - / - | - / - | 0.269 / 0.584 | 0.160 / 0.465 | 0.120 / 0.333 |
| whole frame (all threads, JIT included) | 4.37 | 20.34 | 2.36 | 2.17 | 6.22 |

Composites a frame: S7 768 (all simple), L4 1 537 (768 A + 768 B), T6 191 (all simple + shade), T7 191 (25 %
simple, 75 % complex), T9 751 (2x; simple 38 %, simple + shade 13 %, complex 49 %). The composite path mix of the
T scenes is in `model/e2e-*.log` (the `[compdiff]` lines).

### 9.4 What the numbers say for a replacement

- The 3D screen's fixed cost is per quarter: 424 (3D visibility) + >= 620 (composite) four times a line, 75 % of
  the 3D-only line; the fused paths in `compfuse.h` and the bin-thread visibility precompute
  (`re/compositing-2x.md` 4) attack exactly that.
- Anything that sends a quarter to the complex path multiplies its composite cost by 2.5-4.5 (one translucent 3D
  pixel is enough), with BLDCNT brightness by up to 10: the complex path's brightness setup is a scalar per-pixel
  loop in C, and `apply_offset_c` is compiler-vectorised C. A closed-form implementation (5.6) costs the same for
  every path.
- Blended capture is scalar C at 33 instructions a pixel, run four times a line at 2x; a NEON version is an easy
  10x and it is the largest item of any capture-heavy game.
- `render_scanline_2d` itself (490-1 050 a line) is mostly the inline bit-15 maps of the direct layers / OBJ image
  and the per-quarter bookkeeping.

---

## 10. What a replacement needs

### 10.1 Inputs per line, by stage

| stage | reads | writes |
|---|---|---|
| flags / bldcnt' | `eng+0x90` DISPCNT, `+0xa0` BLDCNT, `+0xa2` BLDY, `eng[0x21340+line]` OBJ line flags, OBJ return (any OBJ list entry on the line: `eng[0x20f80 + l*0xc0 + line]`, l = 0..4), `eng+0x21400` OBJ image | locals |
| windows | `eng+0x90`, `+0x9c` WININ/WINOUT, `+0xaa/+0xac` WIN0H/WIN1H, `+0xae..+0xb1` WIN0V/WIN1V, `+0xb4` state, `+0xb5` dirty, `+0x44/+0x64` masks, `S+0xea0` OBJ window | `eng+0x44/0x64` (on dirty), `eng+0xb4`, `eng+0xb5`, `S+0xf00`, `S+0xfa0` |
| apply_windows / blank | `S+0xda0`, `S+0xf00`, DISPCNT | `S+0xda0`, lmask |
| composite | `eng+0x18` -> palette entry 0 (backdrop), `eng+0x84..+0x8b`, `+0xb3` (priority list), `+0xa0`, `+0xa2`, `+0xa4` BLDALPHA, the layer lines, `p3d`, `S+0xda0` vis, `S+0xec0/0xee0/0xfa0`, the alpha plane | planes, S scratch, alpha plane (3D alpha where BG0 is top) |
| capture | `C` (0x58 B), srcB line, hi-res buffers, planes / 3D | VRAM destination, `[C+0x30]` hi-res buffer |
| convert | `eng+0xa6` MASTER_BRIGHT, `eng+0x38/+0x40` scanout / pitch, `cfg+0x4a0` hires, bpp | scanout rows |

Per-frame / per-event state only: the window masks `eng+0x44/0x64` (WINxH events), the priority list (DISPCNT/BGxCNT
events, reorder_layers), the OBJ line flags and image (reorder_obj). Per-line state: `eng+0xb4` (window Y state),
everything in S.

### 10.2 DraStic behaviours to reproduce (all [D]; those marked [M]/[R] are executed by the model checks)

1. 2x master brighten (section 8) [M][R]; MB factor >= 16 is full white/black; mode 3 = no effect.
2. Scanout byte 3: 0xff (1x direct, 1x shade, memsets white), 0 (black memset), pattern (2x direct), junk (2x shade).
3. Clamps: EVA, EVB at 16 (32/32); EVY at 16; BLDALPHA bit 7 adds 1 to the doubled EVB [M]; BLDY is stored
   `& 0x1f`, BLDALPHA / BLDCNT unmasked.
4. Blending results are 6-bit (`(t*A + s*B + 16) >> 5`, rounded, clamped at 63), not 5-bit.
5. Opaque 3D (line or quarter flag 0x10) is removed from the BLDALPHA mask; translucent lines blend BG0-top pixels
   with `a+1 / 31-a` whatever BG0's 1st-target bit (needs only a 2nd target below), but **only in BLDCNT modes 0 and
   1**; in modes 2/3 translucent 3D is drawn opaque (also when BLDY = 0 or there is no 1st target).
6. A bitmap OBJ (flags bit 1) uses `2a+1` as alpha in any BLDCNT mode and needs only a 2nd target below it. The
   OBJ-image flag 0x20 removes OBJ-top pixels from the BLDALPHA / semi mask only; the per-pixel step
   (`BMP & OBJ-top & T2`) runs after it and is not filtered [M].
7. Semi-transparent OBJ: a 1st target by itself (needs T2); flag bit 0 cleared when no enabled 2nd target exists;
   in brighten mode a semi OBJ pixel that is also T1 gets `(t*A + s*B + 63*fy + 16) >> 5`; in darken mode the plain
   alpha blend; the OBJ image (0x20) removes all OBJ-top pixels from the semi/alpha mask.
7b. The window effects bit (fx) only clears T1, i.e. BLDALPHA blending and brightness. Semi-transparent OBJs (cases
   1 and 5 use `SEMI & OBJ-top & T2` without T1), bitmap-OBJ alpha and 3D alpha are still blended inside a window
   region whose effects are disabled [M][R].
8. The backdrop: a 1st target only for brightness (no second layer below it), a 2nd target when exactly one layer
   covers the pixel.
9. The frontmost list entry (if a BG) is excluded from the second-layer selection (no visible effect: it can never be
   second).
10. Windows: state machine of 4.2 (stepped only on rendered lines with windows enabled; line-0 rule), X edges of 4.1,
    priority WIN0 > WIN1 > OBJ > outside, effects bit 5 per region -> fx (clears T1 and the shade mask only: a pixel in
    an effects-disabled region can still be a 2nd target and still gets semi-OBJ, bitmap-OBJ and 3D alpha, which do
    not use T1; see 7b).
11. The OBJ window mask is stale when DISPCNT.12 is clear.
12. Capture: bit 15 always set when blending, 3D needs DISPCNT.3, the hi-res quarter sources of section 7.
13. The 2x path for direct BG2/BG3 or the OBJ image (four composites, hi-res data per quarter); in the 2x path the
    OBJ image's bit15 map and the direct layers' maps are rebuilt per quarter and **stored over** their vis slots.

### 10.3 Exactness notes for a NEON/C replacement

- Model: `re/2d/compose/model/compose_spec.c` (+ `spec/composite.c`) is a complete, tested reference of everything
  from the vis bitmaps and layer lines to the scanout, including the stale-scratch behaviour; it can be dropped into
  `src/rast/spec/` as is. Not modelled: `render_scanline_2d`'s control flow (section 3.1, checked at runtime by
  recomputing its arguments), the capture dispatch, `render_scanline`'s convert dispatch.
- The planes are the only intermediate that must match bit for bit if capture or 2x pixel doubling is involved; the
  rest (masks, coefficient planes) can be fused freely as long as the per-pixel formulas of 5.5 and the masks of 5.2
  are kept, including the "stale but irrelevant" cases (second-plane garbage under EVB = 0).
- The simple path dominates (S7/L4: 100% of quarters; T0: 100%; T6: 100% with the shade; T3: 74%; T9: 51%); a fused
  simple path (compfuse.h idea extended to any layer set: select + expand + 3D insert in one pass) is the main lever,
  then avoiding the complex path for quarters whose only reason is a translucent 3D pixel with no 2nd target
  (`T2 == 0` gives EVA = 32, EVB = 0 everywhere: the planes equal the simple path's, clamped at 63) [M-derivable].
- The closed form of 5.6 is the contract for a fused implementation: per pixel, the two front layers (a walk of at
  most 8 list entries, or the equivalent two-deep bitmap encoder), two colour fetches, the T1 / T2 bits, then one
  multiply-add per channel. Everything DraStic computes in masks and planes on the way (TOP / SEC / T1 / T2 / M,
  EVA / EVB / OFF, the second planes) is internal and needs no reproduction; only the output planes are observable,
  with one exception: the OBJ attribute plane S+0xc90 is never cleared between lines (obj.md 4.1: only drawn OBJ
  pixels are written; q3 / the 1x composite also leave the 3D alpha in it where BG0 was top), and the BMP / SEMI
  masks are built from all 256 of its bytes. Normally they only matter where an OBJ drawn on this line is top, so
  stale bytes are masked out; but with the full-screen OBJ image (flag 0x20) the image pixels are OBJ-top without
  being drawn by `render_scanline_obj_c` (its 12 sprites are left out of the lists), so on a line that also carries
  a semi-transparent or bitmap OBJ (line flag set) an image pixel with a stale non-zero byte there is alpha-blended
  with EVA = stale+1 when its second layer is a 2nd target [D, reasoning; not observed]. On catch-up frames engine
  B renders on the same thread and stack, so the stale bytes may even be engine B's.
- What must stay bit-exact and is easy to get wrong: the 6-bit domain (2D colours `c5 << 1`, 3D bytes as they are,
  scanout `<< 2`); `+16` rounding then `>> 5` then clamp at 63; BLDALPHA bit 7 into B; `fy = min(2*BLDY, 32)`; the
  order of the brightness and the blend overrides (a semi OBJ in brighten mode keeps OFF); the 3D alpha only in
  BLDCNT modes 0/1 and never with brightness; the per-quarter flags (one translucent 3D pixel changes how the
  whole quarter's opaque 3D pixels are masked, but not their result); the 2x master-brightness bug and the byte-3
  patterns of the scanout (section 8).
- Threading: everything here reads only per-line state (S, the engine fields of 10.1 after the line's event
  replay) and the 3D frame; nothing writes back into the engine except `eng+0xb4/0xb5` and the WIN0/WIN1 masks
  (`eng+0x44/0x64`) of the window code, and the capture writes VRAM / the hi-res buffer. A deferred engine-A
  compositor can therefore run on another core from a snapshot of the per-line inputs (frame.md 12), provided the
  window state machine is stepped in line order and capture frames are finished before update_frame returns.

---

## 11. Hook points (16-byte `ldr x16,#8; br x16; .quad` patches)

| function | addr | first 4 words | PC-relative in the patch | args |
|---|---|---|---|---|
| render_scanline_generate_window_masks | 0x3b360 | d10243ff a9017bfd 910043fd a90253f3 | no | x0 eng, x1 inh, x2 fx, x3 objwin, w4 lmask, w5 line |
| render_scanline_update_window_mask | 0x3a1c0 | a9bd7bfd 53087c22 910003fd a90153f3 | no | x0 mask, w1 WINxH |
| render_scanline_window_inhibit_masks_single | 0x3b060 | 0a020086 36280184 91003c24 cb030084 | tbz (2nd) | x0 inh, x1 fx, w2 lmask, x3 win, w4 in, w5 out |
| render_scanline_window_inhibit_masks_double | 0x3ab10 | 0a0200a8 36280185 91003c25 cb0300a5 | tbz (2nd) | + x4 winB, w5 inA, w6 inB, w7 out |
| render_scanline_window_inhibit_masks_triple | 0x3a380 | a9bb7bfd b000092a 910003fd ad400c62 | adrp (2nd) | x3..x5 wins, w6 w7 [sp] in, [sp+8] out |
| render_scanline_apply_windows | 0x3b650 | b9409000 f273081f 540004e0 37000903 | b.eq, tbnz (3rd, 4th): replace fully | x0 eng, x1 vis, x2 inh, w3 lmask |
| render_scanline_disable_blank_layers_asm | 0xa089c | 4cdf2000 4c402004 bd400030 4f07e7f1 | no | x0 vis, x1 &lmask |
| render_scanline_priority_encode_double_asm | 0x9fe98 | 3942cc04 91021005 4f000400 4f000401 | no | x0 eng, x1 vis, x2 top, x3 sec |
| render_scanline_select_blend_enable_asm | 0xa0070 | 4f00e434 528000a4 4e010c42 4f00e400 | no | x0 out, x1 excl, w2 lmask, w3 bits |
| render_scanline_shade_asm | 0xa0108 | fc1f0fe8 79414404 52800408 79414005 | no | x0 eng, x1 out, x2 in, x3 mask |
| render_scanline_color_effects_setup_blend_base_asm | 0xa0254 | 0b000000 4f01e411 53001404 53083405 | no | w0 BLDALPHA, x1 eva, x2 evb, x3 mask |
| render_scanline_color_effects_setup_blend_asm | 0xa02e0 | 0b000000 4f01e411 53001404 53083405 | no | same |
| render_scanline_color_effects_setup_alpha_base_asm | 0xa042c | 10ffe664 4f00e7f9 4c407098 4f00e43a | adr (1st) | x0 eva, x1 evb, x2 alpha, x3 mask |
| render_scanline_color_effects_setup_alpha_asm | 0xa04b4 | 10ffe224 4f00e7fd 4c40709c 4f00e43e | adr (1st) | same |
| render_scanline_color_effects_apply_asm | 0xa0378 | 4f01e7f9 91040004 91080005 91040026 | no | x0 out, x1 P (6 planes), x2 eva, x3 evb |
| render_scanline_color_effects_apply_offset_c | 0x39f00 | 91044005 91144027 eb05003f 9104000d | no | + x4 off |
| render_scanline_2d_composite | 0x3c6d0 | a9b67bfd aa0303e8 910003fd a9025bf5 | no | section 5 |
| render_scanline_horizontal_shift_3d | 0x3c630 | a9bd7bfd 937e7c43 910003fd a90153f3 | no | x0 dst, x1 src, w2 hofs |
| render_scanline_capture_direct_asm | 0xa0910 | 79409803 91040044 91080045 4cdfa040 | no | x0 C, x1 dst, x2 planes |
| render_scanline_capture_direct_3d_asm | 0xa09b0 | 79409803 4cdf0040 4cdf0044 4e208863 | no | x0 C, x1 dst, x2 px |
| render_scanline_capture_blended | 0x3bc60 | 3941540a 39415009 2b0a014a aa0003ec | no | x0 C, x1 dst, x2 srcB, x3 planes |
| render_scanline_capture_blended_3d | 0x3bdb0 | 3941540d 3941500a 2b0d01ad aa0003eb | no | x0 C, x1 dst, x2 srcB, x3 px |
| render_scanline_color_convert_direct_32_1x_asm | 0xa0a80 | 91040003 91080004 4f07e7e3 4f07e7e7 | no | x0 planes, x1 dst |
| render_scanline_color_convert_direct_32_2x_asm | 0xa0ae0 | 91040004 91040025 91080006 91080027 | no | x0 E, x1 O, x2 dst |
| render_scanline_color_convert_shade_32_1x_asm | 0xa0c90 | 4e010c5c 91040005 91080006 4f07e7e3 | no | x0 planes, x1 dst, w2 factor, w3 add |
| render_scanline_color_convert_shade_32_2x_asm | 0xa0d80 | 4e010c7c 91040006 91040027 91080008 | no | x0 E, x1 O, x2 dst, w3 factor, w4 add |

(render_scanline, render_scanline_2d, select_pixels and the simple-path helpers: `re/compositing-2x.md` 4c.)

---

## 12. Open questions / guesses

1. The hardware comparisons in 0.4 / 5.5 / 10.2 (what a real DS does with 3D alpha in brightness modes, the 6-bit
   blend precision, the window line-0 rule) are [G]; they do not affect exactness against DraStic.
2. The 16 bpp converters and `shade_16_2x_c` are not analysed (dsflip is 32 bpp; see 6).
3. Stale reads: the OBJ window with OBJ off (4.5), the BG0 u16 line under the 3D layer (masked out whenever the 3D
   or another layer or the backdrop owns the pixel, which is always) and the OBJ attribute plane in the OBJ-image
   case (10.3) are not reproducible bit for bit by a replacement that does not share DraStic's stack; every other
   stale read is masked out (5.6 is exact without them). Whether the OBJ-image case ever shows in a game is open.
4. `render_scanline_2d`'s capture dispatch (section 7) and the HOFS + hi-res-capture hazards are read (re-read on
   2026-10-05), not tested by a model; the routines they call are [M], and T9's capture quarters ran under compdiff
   with the composite / convert checks only.
5. The per-line estimates of 9.2 other than the S7 / L4 row are sums of per-call costs measured on synthetic
   inputs [B]; real games will differ with layer coverage (the `select_pixels_binary` cost grows with the number of
   enabled layers, the encoders with the list length).
6. Not covered here: the BG / OBJ layer renderers (bg.md, obj.md), the display modes 2 / 3 (VRAM / main-memory
   display, frame.md 5), the 16 bpp scanout (`color_convert_*_16_*`, `shade_16_2x_c` 0x3da70: dsflip uses
   32 bpp).

---

## 13. Routine index and files

Live routines of this subsystem (address, size in bytes from the symbol table; asm routines without a size run to the
next global symbol):

| routine | addr | size | where |
|---|---|---|---|
| `render_scanline` | 0x404a0 | 0xe58 | convert dispatch, MASTER_BRIGHT (section 8) |
| `render_scanline_2d` | 0x3ef00 | 0x159c | flags, bldcnt', windows, quarters, capture dispatch (3, 7) |
| `render_scanline_generate_window_masks` | 0x3b360 | 0x2e8 | window Y state, regions (4.2) |
| `render_scanline_update_window_mask` | 0x3a1c0 | 0x1b4 | WINxH -> 256-bit mask (4.1) |
| `render_scanline_window_inhibit_masks_single` | 0x3b060 | 0x2b0 | 4.2 |
| `render_scanline_window_inhibit_masks_double` | 0x3ab10 | 0x544 | 4.2 |
| `render_scanline_window_inhibit_masks_triple` | 0x3a380 | 0x784 | 4.2 |
| `render_scanline_apply_windows` | 0x3b650 | 0x44c | 4.3 |
| `render_scanline_disable_blank_layers_asm` | 0xa089c | 0x74 | 4.4 |
| `render_scanline_set_3d_visibility` | 0x3c2c0 | 0x36c | compositing-2x.md A.1 |
| `render_scanline_gather_3d_alpha_asm` | 0xa0a48 | 0x38 | called by set_3d_visibility |
| `render_scanline_horizontal_shift_3d` | 0x3c630 | 0x94 | 5.8 |
| `render_scanline_2d_composite` | 0x3c6d0 | 0xc5c | 5 |
| `render_scanline_priority_encode_single_asm` | 0x9ffc0 | 0x30 | spec/composite.c |
| `render_scanline_priority_encode_double_asm` | 0x9fe98 | 0x40 | 5.3 |
| `render_scanline_select_pixels` | 0x39330 | 0x1e0 | spec/composite.c (C) |
| `render_scanline_select_pixels_binary_asm` | 0xa0640 | 0xe0 | spec/composite.c |
| `render_scanline_select_pixels_binary_scalar_asm` | 0xa0560 | 0xd0 | spec/composite.c |
| `render_scanline_expand_6bit_split_asm` | 0xa0818 | 0x84 | spec/composite.c |
| `render_scanline_select_pixels_binary32_asm` | 0xa0730 | 0x7c | spec/composite.c (+ the _alpha entry 0xa07ac) |
| `render_scanline_select_blend_enable_asm` | 0xa0070 | 0x88 | 5.3 |
| `render_scanline_shade_asm` | 0xa0108 | 0x14c | 5.1 |
| `render_scanline_color_effects_setup_blend_base_asm` | 0xa0254 | 0x8c | 5.3 |
| `render_scanline_color_effects_setup_blend_asm` | 0xa02e0 | 0x98 | 5.3 |
| `render_scanline_color_effects_setup_alpha_base_asm` | 0xa042c | 0x88 | 5.3 |
| `render_scanline_color_effects_setup_alpha_asm` | 0xa04b4 | 0xac | 5.3 |
| `render_scanline_color_effects_apply_asm` | 0xa0378 | 0xb4 | 5.3 |
| `render_scanline_color_effects_apply_offset_c` | 0x39f00 | 0x2bc | 5.3 (C, no asm twin) |
| `render_scanline_capture_direct_asm` | 0xa0910 | 0xa0 | 7 |
| `render_scanline_capture_direct_3d_asm` | 0xa09b0 | 0x98 | 7 |
| `render_scanline_capture_blended` | 0x3bc60 | 0x144 | 7 (C only) |
| `render_scanline_capture_blended_3d` | 0x3bdb0 | 0x140 | 7 (C only) |
| `render_scanline_color_convert_direct_32_1x_asm` | 0xa0a80 | 0x60 | 8 |
| `render_scanline_color_convert_direct_32_2x_asm` | 0xa0ae0 | 0x7c | 8 |
| `render_scanline_color_convert_shade_32_1x_asm` | 0xa0c90 | 0xf0 | 8 |
| `render_scanline_color_convert_shade_32_2x_asm` | 0xa0d80 | 0x110 | 8 |

Dead (no branch, no adr / adrp+add, no RELATIVE relocation anywhere in the binary; `compose/bench/refs.py`):
`render_scanline_priority_encode_single_c` 0x38630, `render_scanline_priority_encode_double_c` 0x38990,
`render_scanline_select_blend_enable_layer` 0x39510, `render_scanline_select_blend_enable_c` 0x39660,
`render_scanline_shade_c` 0x398c0, `render_scanline_color_effects_setup_darken_c` 0x399a0,
`render_scanline_color_effects_setup_brighten_c` 0x39a10, `render_scanline_color_effects_setup_blend_base_c` 0x39a90,
`render_scanline_color_effects_setup_blend_c` 0x39b10, `render_scanline_color_effects_setup_alpha_base_c` 0x39b80,
`render_scanline_color_effects_setup_alpha_c` 0x39c20, `render_scanline_color_effects_apply_c` 0x39c90,
`render_scanline_window_inhibit_outside_windows` 0x3b310, `render_scanline_disable_blank_layers_c` 0x3baa0,
`render_scanline_capture_direct_c` 0x3bbb0, `render_scanline_capture_direct_3d_c` 0x3bc10,
`render_scanline_capture` 0x3bef0, `render_scanline_gather_3d_alpha_c` 0x3bf30,
`render_scanline_color_convert_direct_32_1x_c` 0x3d330, `render_scanline_color_convert_direct_32_2x_c` 0x3d440,
`render_scanline_color_convert_shade_32_1x_c` 0x3ddd0, `render_scanline_color_convert_shade_32_2x_c` 0x3dfa0.

Scratch files (`re/2d/compose/`): `model/compose_spec.c` + `.h` (the C model, one function per routine),
`model/t_compose.c` (unit test in DraStic's process; `ut/t_compose-rerun.txt`), `model/compdiff.c` (runtime check of
the model; logs `model/e2e-*.log`), `model/compdiff2.c` (+ the closed form; logs `model/e2e2-*.log`),
`pix/pixel.h` (the closed form of 5.6), `pix/pix.c` (closed form vs model, native), `model/native/t_equiv.c` (simple vs
complex without 2nd targets), `bench/bench.sh` + `bench.py` + `model/t_bench.c` (per-call costs, `bench/all.txt`),
`prof/*.fn` + `prof/stages.py` (emulator profiles), `*.s` (per-function disassembly dumps).
