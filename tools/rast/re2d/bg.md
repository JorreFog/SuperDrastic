# DraStic r2.5.2.2: the BG layer renderers (key `bg`)

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. The port is `src/rast/spec/2d/bg.c` / `bg.h` (`re/2d/bg/bgspec.c`: the same `spec_`
> names; the layer offsets `L_*` are `BGL_*`; `spec_render_scanline_bg` takes DraStic's load address instead of a
> function map; the mutation and per-pixel knobs were dropped). `re/2d/bg/t_bg.c` is `tools/rast/ut/t_bg2d.c`, which
> also keeps the ideal models `ideal_affine` / `ideal_bitmap` and the census of section 4 (`BG_CENSUS=1`). `bgdiff.c`
> and `bench.sh` were not kept. The other analyses are next to this file; the design is `../2d-engine.md`.

Scope: everything that turns the BG registers, VRAM and palettes into one BG layer line: `render_scanline_bg`
0x36880 and the per-layer renderers it calls through `[L+0x30]` (text, affine, extended affine, 8/16-bit bitmaps,
the large bitmap), their asm helpers, the clipping-edge state (`render_scanline_update_affine_variables`,
`video_2d_bg_layer_affine_setup_edges`), mosaic, `render_scanline_disable_blank_layers_asm`, and the "direct"
bitmap layers that bypass the renderers. Engine A unless noted; engine B runs the same code.

Sources: `re/drastic.dis`, the function dumps in `re/2d/fn/*.s` and `re/2d/dis/*.s`. Frame model and engine
struct: `re/2d/frame.md` (key `frame`; corrections in 0.5). Compositing: `re/compositing-2x.md`.

**How the claims were checked.** The routines were ported to C (`re/2d/bg/bgspec.c`, ~450 lines) and compared with
DraStic's own code in DraStic's process:

1. Unit test `re/2d/bg/t_bg.c` (SuperDrastic's `tools/rast/ut` harness, nothing in the repos changed): random
   layer structs, VRAM (4 MiB + 2 MiB guards each side), palettes and ext palettes; DraStic's function and the
   port run on identical copies; compared: the 32-byte visibility bitmap, every visible pixel, the whole engine
   struct afterwards (so the clipping-edge state is checked too). Groups: text BGs (4,000 cases/seed), single
   affine/bitmap lines with fresh edges (10,000), whole frames through `render_scanline_bg` with per-line affine
   stepping, mosaic and random mid-frame register changes (48,000 lines), clip mode with large scale factors
   (10,000), the edge math alone (50,000 + 20,000), the blank-layer test (5,000). **Seeds 1, 11, 22, 33: 0
   mismatches.** Mutation check: 5 of 6 deliberately wrong variants of the port are caught (the 6th is a proof
   that DraStic's tile-run method equals per-pixel evaluation, see 4.4).
2. End to end `re/2d/bg/bgdiff.c`: preloaded into DraStic running the ds2d test scenes
   (`/home/user/wt-scenes2d/stressrom/out/ds2d-T*.nds`), it hooks `render_scanline_bg` and runs the port on copies
   of the engine struct, line buffers and bitmaps for every line, comparing as above. Results in section 13.

Marked **guess** = not verified by either the disassembly or a test.

---

## 0. Summary and corrections to the premise

1. **The C renderers named in the task are dead code.** `render_scanline_tiled_c` 0x320c0,
   `render_scanline_affine_normal_c` 0x34880, `render_scanline_affine_extended_c` 0x35170,
   `render_scanline_update_affine_variables_wrap` 0x344f0 / `_bitmap` 0x36370, `render_scanline_set_visibility_8bpp`
   0x31860 / `_16bpp` 0x31b00 / `_full_16bpp_c` 0x31dc0, `render_scanline_apply_mosaic` 0x36830 /
   `_visibility` 0x36670, `render_scanline_disable_blank_layers_c` 0x3baa0, `render_scanline_clip_visibility_c`,
   the `*_c` twins of every asm helper, `render_scanline_affine_setup_tile_widths*`, `_remove_zero_tile_widths`,
   `_setup_arrays_b_*`, `_setup_map_indexes_b_*`: no branch, no adrp+add pair and no relocation refers to them
   (script over the whole disassembly + `llvm-readelf -r`). The live renderers are `render_scanline_tiled_ext`
   0xa1d20, `render_scanline_affine_normal_ext` 0xa4730, `render_scanline_affine_extended_ext` 0xa4b90,
   `render_scanline_bitmap_16bpp` 0x32880, `render_scanline_bitmap_8bpp` 0x33700 and `render_scanline_null`
   0x31850, installed in `[L+0x30]` by `video_2d_update_bg_mode` (pointer slots 0x15fe28 / 0x15fde0 / 0x15fdf8,
   R_AARCH64_RELATIVE). Mosaic is inlined in `render_scanline_bg`.
2. **Output format** (verified): every BG layer produces, once per DS line, a u16[256] colour line and a 256-bit
   visibility bitmap. Colour = the palette RAM entry (or VRAM pixel for 16-bit bitmaps) **as stored, bit 15
   included**; bitmap bit x = pixel x opaque. Some paths go through an 8-bit index line or a 12-bit
   `(pal<<8)|index` line first and convert in place (sections 3, 4). There is no BGR555 conversion, no 6-bit step
   and no "bit 15 = opaque" convention in BG lines (that is OBJ / the 3D layer).
3. **2x ("hires"):** BG lines are always rendered at 256 pixels, once per DS line (`render_scanline_2d` calls
   `render_scanline_bg` once, before choosing the 1x or the 4-quarter path, 0x3f024). The 4-quarter composite
   reuses the same line and bitmap for all four quarters, i.e. 2x2 nearest-neighbour (compositing-2x.md 3). The
   only BG data with real 2x detail is the hi-res display-capture copy of a "direct" 16-bit bitmap BG2/BG3
   (`[L+0x28]`, section 6).
4. **Exactness:** the port reproduces DraStic bit for bit. Compared with an ideal per-pixel hardware model DraStic
   is exact except for the quirks listed in section 12 (garbage map entries for pixels 0..15 at scale factors >= 8
   with wrap; history-dependent clip windows; one-line errors at the edge of degenerate (90-degree) transforms;
   stale lines when no ext palette slot is mapped or in mode 6 for BG1/BG3; per-frame "direct" layers). A
   replacement that wants DraStic's pixels must reproduce them; the C port shows how.
5. **Corrections to frame.md:** `L+0x44` is the raw **screen** (map) base `BGnCNT[12:8]<<11` and `L+0x48` the raw
   **char** base `BGnCNT[5:2]<<14` (frame.md has them swapped); `eng+0x94` = DISPCNT **screen** base
   `DISPCNT[29:27]<<16`, `eng+0x98` = DISPCNT **char** base `DISPCNT[26:24]<<16`. Verified in
   `video_2d_set_bg_control` 0x41cc0..0x41d2c and `video_2d_set_display_control` 0x41f58..0x41fe0 (ushl {-8,-2},
   and {0x1f,0xf}, mul {0x800,0x4000}, stored at L+0x44; L+0x38 = eng+0x94 + L+0x44, L+0x3c = eng+0x98 + L+0x48),
   and by the unit test, which sets the fields that way and matches DraStic.
6. Costs (section 11): text 4bpp 682 instructions per layer line, text 8bpp 1,265, 8bpp with ext palettes 1,387,
   affine 2,400-3,425, bitmaps 2,561-3,852, H-mosaic +1,890; all at 1x, independent of the 2x setting, on the
   thread that renders the engine (engine A: the emulation thread).

---

## 1. Call structure

```
render_scanline_2d(eng, ...)                    0x3ef00, once per DS line (display mode 1, or capture active)
  render_scanline_bg(eng, S+0x1e0, S+0xda0, line)                      0x3f024
    for i in 0 .. eng[0xb2]-1:  n = eng[0x8c+i]; L = eng + 0xc0 + n*0xb0     (BGs in priority order)
      if [L+0x20] != NULL: skip                                        (direct layer, section 6)
      buf = S+0x1e0 + n*0x220 + 0x10;   vis = S+0xda0 + n*32
      mosaic off (BGnCNT.6 = 0):  [L+0x30](L, buf, vis, line)
      mosaic on:   adjust VOFS / affine counters by line % (V+1), call, restore, H-mosaic (section 7)
  (later) render_scanline_disable_blank_layers_asm(S+0xda0, &enabled)  0x3f670 (1x) / 0x3f98c (2x)
```

`render_scanline_bg(x0 eng, x1 lines, x2 vis, w3 line)`: `lines` = S+0x1e0 (slot of BG0), `vis` = S+0xda0. Slot n
= 0x220 bytes = 8 u16 padding + u16[256] + 8 u16 padding. BG0 is in the list only when DISPCNT.3 = 0 (3D BG0 is
handled by the compositor). The list `eng+0x8c` / count `eng+0xb2` (built by `video_2d_reorder_layers` 0x41300
from DISPCNT bits 8-11 and the BGnCNT priorities; within a priority by BG number; **independent of the BG mode**).

### 1.1 Which renderer a layer gets (`video_2d_update_bg_mode` 0x41b60, verified)

| DISPCNT mode | BG0, BG1 | BG2 | BG3 |
|---|---|---|---|
| 0 | text | text | text |
| 1 | text | text | affine |
| 2 | text | affine | affine |
| 3 | text | text | ext(BG3CNT) |
| 4 | text | affine | ext(BG3CNT) |
| 5 | text | ext(BG2CNT) | ext(BG3CNT) |
| 6 | BG0 text, **BG1 null** | bitmap_8bpp (large) | **null** |
| 7 | text | unchanged | unchanged |

text = `render_scanline_tiled_ext`, affine = `render_scanline_affine_normal_ext`, ext(c) = `c.7 ? (c.2 ?
bitmap_16bpp : bitmap_8bpp) : render_scanline_affine_extended_ext`, null = `render_scanline_null` (ret). Called on
DISPCNT mode changes and on BGnCNT bit 2/7 changes (`set_bg_control` 0x41dc0), and by `reset_video_2d`. Engine B
keeps the mode bits (`DISPCNT & 0xc0b1fff7`), so a game setting mode 6 on engine B gets the large bitmap at offset
0 of **engine A's** BG VRAM (L+0x40 = 0) (**guess** that any game does this).

---

## 2. Inputs: the layer struct and what derives it

### 2.1 Layer struct `L = eng + 0xc0 + n*0xb0` (fields the renderers read/write)

| off | type | meaning | set by |
|---|---|---|---|
| 0x08 | ptr | VRAM linear alias `[sys+0xfd500]` (16 MiB image of 0x06000000..) | initialize_video_2d |
| 0x10 | ptr | BG palette (engine A: sys+0x16070, 0x200 B used) | reset_video_2d (`[video+0x2e50+8*idx]`) |
| 0x18 | ptr | BG ext palette slot (8 KiB, 16 x 256 u16) or NULL | BG0/1: set_bg_control (slot n + 2*BGnCNT.13) and remap_vram_body; BG2/3: remap_vram_body only (slots 2/3) |
| 0x20 / 0x28 | ptr | direct 16-bit bitmap (section 6) / its 2x capture data | map_bg_direct_layers, per frame |
| 0x30 | fnptr | renderer (1.1) | update_bg_mode |
| 0x38 / 0x3c | u32 | map base / char base, VRAM-alias offsets: `eng+0x94 + L+0x44` / `eng+0x98 + L+0x48` (engine B: `(0x200000 + raw) & ~0x1e0000`) | set_bg_control, set_display_control |
| 0x40 | u32 | bitmap base `BGnCNT[12:8]<<14` (+0x200000 on B; 0 in mode 6) | set_bg_control (n >= 2) |
| 0x44 / 0x48 | u32 | raw screen base `[12:8]<<11` / raw char base `[5:2]<<14` | set_bg_control |
| 0x4c / 0x50 | u32 | `ceil(2^31/|PA|)` / `ceil(2^31/|PC|)` (affine tile run spacing), unchanged when PA/PC = 0 | update_affine_variables |
| 0x58 / 0x60 / 0x68 | s64 | X axis clip edge: start, width, per-line step, 32.32 screen x | update_affine_variables / setup_edges; start stepped by the renderer |
| 0x70 / 0x78 / 0x80 | s64 | Y axis clip edge, same | same |
| 0x88 / 0x8c | s32 | BGnX / BGnY reference (sext 28) | event replay |
| 0x90 / 0x94 | s32 | current X / Y (line 0: = ref; lines 1..191: += PB / PD before rendering, `render_scanline` 0x405dc / 0x40910) | render_scanline, event replay |
| 0x98 | u16 | BGnCNT | event replay |
| 0x9a / 0x9c | u16 | HOFS / VOFS (& 0x1ff) | event replay |
| 0x9e / 0xa0 / 0xa2 / 0xa4 | s16 | PA / PC / PB / PD | event replay |
| 0xa6 / 0xa8 / 0xaa | u16, u16, u8 | bitmap width mask, height mask, log2 width | set_bg_control (n >= 2) |
| 0xab / 0xac | u8 | affine map tiles-1 (15/31/63/127), log2 tiles (4..7) | set_bg_control (n >= 2) |
| 0xad | u8 | DISPCNT.30 (ext palettes on) | set_display_control |
| 0xae | u8 | "affine parameters changed": recompute the clip edges at the next affine render | render_scanline line 0 (BG2, BG3), PA..PD / X / Y event replay, savestate load; cleared by the renderer |

Bitmap sizes (`set_bg_control` 0x41d70.., n >= 2): size 0 128x128 (masks 127/127, log2 7), 1 256x256, 2 512x256
(0x1ff/0xff, 9), 3 512x512 (0x1ff/0x1ff, 9); affine map tiles 16/32/64/128. Mode 6 (0x41e04): BGnCNT.14 ? 1024x512
(0x3ff/0x1ff, 10) : 512x1024 (0x1ff/0x3ff, 9), base 0. **BG0/BG1 get none of these** (text sizes are read from
BGnCNT at render time). Engine-level inputs: `eng+0x8c/0xb2` (list), `eng+0xa8` (MOSAIC). Nothing else: the
renderers do not read the I/O register file, OAM, windows or the 3D state.

### 2.2 Memory the renderers read

All VRAM reads are `[L+8] + offset` with **no masking to the engine's 512 KiB/128 KiB BG area**: char base up to
0x70000+0x3c000 plus 64 KiB of 8bpp tiles, map up to 0x7f800 + 8 KiB, bitmap up to 0x7c000 + 512 KiB, the affine
wrap bug (4.5) up to map + 0x3f3f, bitmap offsets are 32-bit sign-extended. What an offset beyond the mapped banks
returns is whatever DraStic's page mapping of the alias holds (**guess**: mirrors or the dummy page); a
replacement must read through the same alias (or reproduce its mapping). Palettes: `[L+0x10]` (512 B used), ext
palettes `[L+0x18]` (8 KiB, bank memory directly, not through the alias).

### 2.3 What is per frame / per line / per event

- Per event (replayed after the line it was written on, frame.md 3): BGnCNT -> L+0x38..0x48, sizes, renderer,
  ext slot (BG0/1), list; DISPCNT -> bases, renderer, L+0xad, list; offsets, PA..PD, X/Y (+ L+0xae); MOSAIC.
- Per line: the current X/Y counters (render_scanline), the clip edges (stepped by the renderer itself, 4.2).
- Per VRAM remap (at the next render call): ext palette slot pointers.
- Per frame (start_frame): the direct-layer decision.
- No caches: no decoded tiles, no per-frame map copies; each line re-reads map entries, tiles and palettes.

---

## 3. Text BGs: `render_scanline_tiled_ext(x0 L, x1 buf, x2 vis, w3 line)` 0xa1d20

Algorithm (verified, `spec_render_scanline_tiled_ext`):

```
y = VOFS + line                    only bits 0..8 are used, so y is effectively (VOFS + line) mod 512
m = L+0x38;  if (CNT.15 && y & 0x100): m += CNT.14 ? 0x1000 : 0x800        (lower half of a 512-tall map)
row = m + ((y & 0xf8) << 3)                                               (32 entries x 2 B per tile row)
A = B = row;  if CNT.14 (512 wide): HOFS < 256 ? B = row + 0x800 : A = row + 0x800
t = (HOFS >> 3) & 31,  fine = HOFS & 7,  fy = y & 7
for k in 0..32 (33 tiles):  e = t+k < 32 ? A[t+k] : B[t+k-32]            (u16 map entries)
   tile = e & 0x3ff, hflip = e.10, vflip = e.11, pal = e >> 12, r = vflip ? 7-fy : fy
   4bpp: d = u32 at chr + tile*32 + r*4;  pixel p: idx = nibble (hflip ? 7-p : p) of d, colour = PAL[pal*16 + idx]
   8bpp: idx = chr[(tile*64 + r*8 + (hflip ? 7-p : p)) & 0xffff]
         ext off: colour = PAL[idx];  ext on: colour = EXT[(pal << 8) | idx]
   tile pixel 8k+p lands on screen x = 8k + p - fine
vis bit x = idx(x) != 0;  buf[x] = colour(x)  (colour of index 0 = PAL[pal*16] / PAL[0] / EXT[pal<<8]; not observable)
```

`chr = [L+8] + [L+0x3c]`, PAL = `[L+0x10]` (u16), EXT = `[L+0x18]`. Ext palettes apply iff CNT.7 and `L+0xad`;
**if the slot pointer is NULL the function returns without writing anything** (0xa1dcc): buf and vis keep what the
previous use of that slot left (section 12.4).

Implementation (all verified; for a memory-exact port of the helpers see `re/2d/fn/*.s`):

| step | function | args | does |
|---|---|---|---|
| map | `render_scanline_tiled_setup_tile_map_entries_4bpp_asm` 0x9ee70 / `_8bpp_asm` 0x9ef08 | x0 u16 out[40], x1 u8 out[48], x2 block A row, x3 block B row, w4 t, w5 r-offset (fy*4 / fy*8) | jump table 0x9e9e0 + t*4 loads 40 entries (`ext` shifts), out u16 = `tile*32 + (vflip ? 28-w5 : w5)` (8bpp: `tile*64 + (vflip ? 56-w5 : w5)`, 16-bit), out u8 = `e >> 8` (entries 33..39 junk; bytes 40..47 zero) |
| 4bpp | `render_scanline_tiled_span_4bpp_asm` 0x9efa0 | x0 buf - fine (u16), x1 u32 rows[34], x2 PAL, x3 chr, x4 offsets, x5 high bytes | 4 tiles per iteration x 8 + tail: row word, nibble-reversed if hflip (`ushr/sli/rev32/bit`), stored to x1 for the bitmap; colours by `tbl` on the 16-entry palette bank (`ld2` lo/hi bytes) |
| 4bpp | `render_scanline_set_visibility_4bpp_asm` 0x9f278 | x0 vis, x1 rows, w2 fine | per tile byte mask of non-zero nibbles (`cmtst` 0x0f/0xf0, insert masks 0x9f270), then `(m[t] >> fine) | (m[t+1] << (8-fine))` |
| 8bpp | `render_scanline_tiled_span_8bpp_normal_palette_asm` 0x9f0f0 | x0 index line `buf + 0x110 - fine` (bytes), x1 chr, x2 offsets, x3 high bytes | 8 index bytes per tile, `rev64` if hflip (mask table 0x9f0e0 = {4,4,0x400,0x400}) |
| 8bpp | `render_scanline_set_visibility_8bpp_asm` 0x9f390 | x0 vis, x1 u8[256] | bit = byte != 0 (`cmeq`, weights 1..128, 3 `addp`) |
| 8bpp | `render_scanline_palette_lookup_8bpp_asm` 0x9f504 | x0 u16 dst, x1 u8 src, x2 PAL, w3 count | `dst[i] = PAL[src[i]]`, 8 per iteration (rounds count up to 8) |
| ext | `render_scanline_tiled_span_8bpp_ext_palette_asm` 0x9f188 | x0 buf - fine (u16), x1 chr, x2 offsets, x3 high bytes | u16 `idx | pal<<8`; tail tile: `idx | (pal32 | (e33 >> 8 & 0xf) << 4) << 8` (a `dup v3.8b` of an unmasked byte) - harmless because the lookup masks with 0xfff |
| ext | `render_scanline_set_visibility_12bpp_asm` 0x9f468 | x0 vis, x1 u16[256] | bit = low byte != 0 |
| ext | `render_scanline_palette_lookup_12bpp_asm` 0x9f574 | x0 dst, x1 src (in place), x2 EXT, w3 count | `dst[i] = EXT[src[i] & 0xfff]` |

Writes outside buf[0..255] (measured with guard bytes): 4bpp and 8bpp-ext: bytes -14..527 relative to buf (the
33rd tile and `-fine`); 8bpp: bytes 512..535 (the index line left in the post-padding and up to 8 bytes into the
**next slot's** pre-padding). None of it is read by anything (**guess** for consumers outside this subsystem;
select_pixels reads only buf[0..255]).

---

## 4. Affine BGs: `render_scanline_affine_normal_ext` 0xa4730 and `render_scanline_affine_extended_ext` 0xa4b90

Both: `(x0 L, x1 buf, x2 vis, w3 line)`; the line number is not used. Normal = 8-bit map entries (tile 0..255, no
flips, standard palette, BGnCNT.7 ignored); extended = 16-bit map entries (tile 0..1023, hflip/vflip, palette
number used only with ext palettes; BG2 slot 2 / BG3 slot 3 via `[L+0x18]`).

### 4.1 What a pixel is (verified; `ideal_affine` in bgspec.c is the hardware-style reference)

```
X = L+0x90, Y = L+0x94 (current counters, 32-bit), PA, PC (s16)
for screen x: sx = (X + PA*x) >> 8, sy = (Y + PC*x) >> 8   (arithmetic, 32-bit)
size = (L+0xab + 1) * 8 pixels;  wrap (CNT.13): sx, sy &= size-1;  clip: outside [0,size) -> transparent
normal:   e = map[(sx>>3) + ((sy>>3) << L+0xac)] (u8);  idx = chr[e*64 + (sy&7)*8 + (sx&7)]
extended: e = map16[...];  o = (sx&7)|(sy&7)<<3, o ^= 7 if e.10, o ^= 0x38 if e.11;  idx = chr[(e&0x3ff)*64 + o]
colour = PAL[idx], or with ext palettes (extended only, L+0xad): EXT[(e>>12)<<8 | idx];  vis = idx != 0
```

DraStic equals this per-pixel rule in wrap mode for |PA|, |PC| <= 2047 and in clip mode whenever the clip edges
are current and the transform is not degenerate (4.2), for every pixel. Unit-test census (DraStic vs `ideal_affine`
/ `ideal_bitmap`): 0 differing pixels in those classes over ~8,100 fresh-edge single lines (three seeds), 38,400
whole-frame lines with stepped edges and no disturbing events (BG_EV=0), and ~30,000 large-scale clip lines.

### 4.2 Clip mode: the edge state machine (verified, `spec_render_scanline_update_affine_variables`,
`spec_video_2d_bg_layer_affine_setup_edges`, `clip_span`)

The visible span of a clip-mode line is **not** computed from the current coordinates. It comes from per-layer
32.32 fixed-point "edges":

```
if L+0xae:  update_affine_variables(L)          0x34550 (called at the top of every affine render, wrap or not)
   W = (L+0xab << 11) + 0x7ff                   (largest in-range coordinate, 1/256 px)
   X axis: edges(X = L+0x90, P = PA, W, Q = PB) -> L+0x58 start, L+0x68 step, L+0x60 width
   Y axis: edges(Y = L+0x94, P = PC, W, Q = PD) -> L+0x70, L+0x80, L+0x78
   L+0x4c = (2^31 + |PA| - 1) / |PA| if PA != 0;  L+0x50 likewise for PC;  L+0xae = 0
edges(X, P, W, Q):                               (= video_2d_bg_layer_affine_setup_edges 0x326f0)
   P > 0: start = ceil(2^32 (P-1-X) / P),  end = ceil(2^32 (W-X) / P)
   P < 0: start = ceil(2^32 (P+W+1-X) / P), end = ceil(2^32 (-X) / P)
          width = end - start,  step = ceil(2^32 (-Q) / P)          (exact ceilings, 64-bit)
   P = 0, Q = 0: X in [0,W] ? (start 0, width 256<<32) : (start -1, width 0);  step 0
   P = 0, Q != 0: a = trunc((W-X-1)/Q), b = trunc(-X/Q)  (Q > 0; swapped for Q < 0)
          start = (-256 a) << 32,  width = (-256 b << 32) - start,  step = 256 << 32
per clip-mode line (0xa4954):
   lo = max(hi32(L+0x58), hi32(L+0x70)),  hi = min(hi32(L+0x58 + L+0x60), hi32(L+0x70 + L+0x78))
   L+0x58 += L+0x68;  L+0x70 += L+0x80            (always, before any early exit)
   if hi < 0 or lo > 255: vis = 0, buf untouched;  clamp to [0,255];  if hi < lo: vis = 0
   render pixels lo..hi from X' = X + lo*PA, Y' = Y + lo*PC;  vis = bits lo..hi only
```

For P != 0 the start/end at the recompute line are the exact first/last in-range screen x, and stepping keeps
them exact on later lines (error < 193 x 2^-32 against a granularity of 1/|P|; the census over 38,400 whole-frame
lines with no disturbing events found no difference). Deviations come from **when** the edges are recomputed and
stepped:

- recomputed only when `L+0xae` is set (line 0 for BG2/BG3, a write to PA..PD or X/Y, savestate load);
- stepped only on lines where this layer is rendered in clip mode. A line where the layer is not in the render
  list (disabled mid-frame), is in wrap mode, or returns early for a NULL ext palette leaves the edges behind
  while X/Y keep moving: the clip window lags by (missed lines) x step until the next recompute. (The affine tile
  renderers consume L+0xae on every call, wrap or clip; the bitmap renderers only on general clip lines, so for a
  bitmap a wrap-to-clip switch after line 0 still recomputes, for an affine tile BG it does not.) Measured:
  random list-length changes and wrap toggles produce 1..100-pixel window errors (BG_EV=64 / 16 census);
  parameter writes alone produce none (BG_EV=15);
- with vertical mosaic, a recompute on a mosaic line uses the mosaic-adjusted X/Y (section 7);
- **degenerate axes** (PA = 0 or PC = 0, e.g. 90-degree rotations) use integer line counts with truncating
  division: with `a = trunc((W-X-1)/Q)`, `b = trunc(-X/Q)` (Q > 0; the two swapped for Q < 0) and n = lines since
  the recompute, the axis is fully open on lines `b < n <= a`, shows **only screen pixel 0** on line `n = b` (if
  b <= a), and is closed otherwise. The hardware rule is `0 <= X+nQ <= W`, i.e. `ceil(-X/Q) <= n <= floor((W-X)/Q)`.
  So line b shows pixel 0 alone whether the hardware would show nothing there (X+bQ < 0) or the whole line
  (X+bQ = 0, or 0 < X < Q at n = 0); the last line is dropped when W-X is a multiple of Q; and a recompute with
  X in (W, W+Q) opens line 0 although it is out of range. Unit census: 13 of ~270 random degenerate single lines
  differ, 100+ pixels each.

### 4.3 Wrap mode, |PA| and |PC| <= 2047: the tile-run pipeline (verified)

`render_scanline_affine_setup_arrays_normal` 0xa3df0 / `_extended` 0xa3780 `(x0 L, x1 u8 runs/widths (A+0x340), x2
u16 map idx/entries (A+0x100), x3 map ptr, x4 u8 offsets (A), x5 u8 flip masks (A+0x460, extended), w6 X0, w7 Y0,
[sp] count-1) -> w0 runs`, with A = sp+0xa0 of the renderer (0x630-byte frame):

1. Tile crossings per axis: `n = ((X0 + count*PA) >> 11) - (X0 >> 11)` (negated for PA < 0); positions
   `(v + k*R) >> 20`, `v = ((PA + 0x7ff - (X0&0x7ff)) * R) >> 11` (PA > 0) or `(((X0&0x7ff) - PA) * R) >> 11`
   (PA < 0), R = L+0x4c; same for Y with PC, L+0x50. Lists of bytes (stack arrays of 0x120).
2. `render_scanline_affine_merge_tile_widths_c` 0xa2610 `(a, b, out, na, nb)`: sorted merge, equal values once;
   then a duplicate-removal pass. b boundaries -> b+1 runs.
3. `setup_map_indexes_{normal,extended}_asm` 0x9f5e8 / 0x9f754: per run start s, `((X0+s*PA)>>11 & mask) +
   (((Y0+s*PC)>>11 & mask) << log2)` (u16; x2 for extended); `load_tile_map_entries_*_asm` 0x9fac8 / 0x9fb40:
   map bytes / u16 entries; `setup_flip_masks_asm` 0x9fbb0: `(e.10 ? 7 : 0) | (e.11 ? 0x38 : 0)`.
4. `diff_tile_widths_asm` 0x9f8c4: widths = boundary differences, last = count+1 - last boundary (mod 256: a single
   run of 256 has width 0, handled as "256" by the tile renderer).
5. `setup_tile_offsets_asm` 0x9f930 `(out, X0, Y0, PA, PC, count-1)`: per pixel `((X0+PA*x)>>8 & 7) |
   ((Y0+PC*x)>>8 & 7) << 3` (16-bit lanes; writes `(floor((count-1)/32)+1)*32` bytes).
6. `render_tiles_{normal_normal,extended_normal,extended_extended}_asm` 0x9f9bc / 0x9fbf8 / 0x9fd30: per run, the
   64-byte tile in 4 registers, `tbl` by the (flip-XORed) offsets; 16 bytes per run written and the pointer
   advanced by the width (later runs overwrite the excess). Output: index bytes at `buf + 0x100 + x` (normal and
   extended without ext palettes), or u16 `idx | pal<<8` at `buf + 2x` (extended with ext palettes).
7. `set_visibility_8bpp_asm` / `_12bpp_asm` over the whole 256-entry line, then `palette_lookup_8bpp_asm(buf+lo,
   buf+0x100+lo, PAL, count)` or `palette_lookup_12bpp_asm` in place. In clip mode the bitmap is then ANDed with
   the span mask [lo, hi] (inline, 0xa4a3c); bytes outside the span are stale and masked out.

### 4.4 The run method is exact

The map entry of a run is evaluated at the run's first pixel and the 6-bit offset per pixel, so the result equals
per-pixel evaluation iff every pixel whose tile differs from its left neighbour starts a run. With `R =
ceil(2^31/|P|)` the computed crossing is never early and is late by less than `(|P|/2048 + 2 + k)` units of
2^-20 px (k = crossing number); a late crossing changes a pixel only if it is that pixel's only crossing, which
needs |P| < 4096, where the error stays below the 2^20/|P| granularity. Machine check: replacing the run method by
per-pixel evaluation in the port (mutation 2) still matches DraStic in every test, including 10,000 clip-mode
lines with |PA| or |PC| in 2048..32767 and the span on screen (332,811 visible pixels). **A replacement can
evaluate affine BGs per pixel** (with the clip edges and the general-path bug of 4.5 kept).

### 4.5 Wrap mode, |PA| or |PC| > 2047: the general path and its bug (verified)

Condition `((PA + 0x7ff) & 0xffff) > 0xffe || ((PC + 0x7ff) & 0xffff) > 0xffe` (0xa47c4). Per-pixel map indexes for
all 256 pixels (vector loop 0xa4888; extended: x2), `memset(widths, 1, 256)`, then
`setup_tile_offsets_asm(A, X, Y, PA, PC, 256)`: with count 256 it writes 9 x 32 = **288 bytes** into the 256-byte
offset array and overwrites the first 32 bytes of the map index array at A+0x100. Result: **pixels 0..15 use map
index `off(256+2i) | off(257+2i) << 8`** (off(p) = the in-tile offset formula of 4.3.5 for p = 256..287; a byte
offset for the extended variant) - a garbage map entry, read from up to map + 0x3f3f. Pixels 16..255 are correct.
Unit census: every such line differs from the ideal model in about 16 pixels (e.g. 2262 pixels over 143 lines).
Applies to both affine renderers, not to bitmaps, not to clip mode (clip mode always uses the run method).

### 4.6 Writes outside buf[0..255]

normal: bytes 512..526; extended with ext palettes: 512..541 (14 bytes into the next slot's pre-padding) - from
the 16-byte run stores and the 8-pixel lookup rounding. Not observable (**guess** for consumers outside).

---

## 5. Bitmap BGs: `render_scanline_bitmap_16bpp` 0x32880, `render_scanline_bitmap_8bpp` 0x33700

`(x0 L, x1 buf, x2 vis, w3 line)` (line unused). Compiled C, vectorised. 16bpp: pixel = u16 at `VRAM + (s32)(base
+ 2*o)`, buf = pixel as stored, vis = bit 15. 8bpp: idx = byte at `VRAM + (s32)(base + o)`, buf = PAL[idx], vis =
idx != 0. base = L+0x40, `o = (row << L+0xaa) + col`, masks wm = L+0xa6, hm = L+0xa8 (verified,
`spec_render_scanline_bitmap_*`):

| case | rows/cols | visible |
|---|---|---|
| PA = 0x100 && PC = 0, wrap | row = (Y>>8) & hm, col = (X>>8) & wm then +1 & wm per pixel | all 256 |
| same, clip | row = Y>>8 (must be in [0,hm], else nothing), col from max(0, X>>8) | screen x in [max(0,-(X>>8)), min(255, wm-(X>>8))] |
| general, wrap | (Y+PC*x)>>8 & hm, (X+PA*x)>>8 & wm | all 256 |
| general, clip | unmasked `(Y'>>8, X'>>8)` stepping from lo | the edge span of 4.2, with `setup_edges(X, PA, (wm<<8)+0xff, PB, L+0x58, L+0x68, L+0x60)` and the Y twin (hm, PC, PD) computed only here when L+0xae, then cleared |

The identity case ignores the edges and does not clear L+0xae. Mode 6 is the same 8bpp function with the large
sizes and base 0. Note that bitmaps have no general-path bug, and their clip edges have the history behaviour of
4.2 (unit census: same deviations in frames with list changes). Width/height masks are the bitmap's own (512x256
etc.), not the affine map size.

---

## 6. Direct 16-bit bitmap layers (BG2/BG3, per frame)

`video_2d_map_bg_direct_layers(eng)` 0x41700, called from `start_frame` for rendered engines (frame.md 1.1),
modes 3..5: for BG2 (mode 5) and BG3: if `(BGnCNT & 0xc0fc) == 0x4084` (16-bit bitmap, 256x256, no mosaic, bits
3-5 = 0), refs X = Y = 0 (L+0x88 as one u64), PA = 0x100, PC = 0, PB = 0, PD = 0x100 (L+0xa0 u64 masked) and the
bitmap base lies in a VRAM bank A..D whose mapping kind `[video+0x10+16*bank]` is 6 at the matching 128 KiB
offset: `L+0x20 = VRAM + base`; `L+0x28` = the bank's hi-res capture buffer + offset if all six 16 KiB blocks are
valid (`C+0x20+bank`), else 0 (not cleared when `base & 0x1ffff > 0x8001`, so it can be stale). Otherwise
L+0x20 = 0 (verified for the conditions; bank-kind meaning **guess**, from frame.md).

Then `render_scanline_bg` skips the layer and `render_scanline_2d` uses the VRAM row directly (0x3f3c8..0x3f638):
line pointer `[L+0x20] + line*512`, layer table entry = pointer - 0x10, visibility = bit 15 of each pixel
(inline NEON into S+0xda0 + n*32), and the layer forces the 4-quarter path at 2x (compositing-2x.md 1.3). Because
the decision is per frame and the row is the screen line, **mid-frame writes to BGnX/Y/PA..PD, the counters and
mosaic are ignored for such a layer until the next frame**; an exact replacement must make the same decision at
start_frame and use row = line.

---

## 7. Mosaic (inlined in `render_scanline_bg`, verified)

For a layer with BGnCNT.6, with MOSAIC = `eng+0xa8`, H = bits 0-3, V = bits 4-7:

- Vertical: `m = V ? line % (V+1) : 0`. Before the call: `VOFS -= m` (u16), `X -= PB*m`, `Y -= PD*m` (32-bit, with
  the PB/PD current now); restored after the call (all layer kinds get all three adjustments). So the line shown
  is `line - m` for text BGs, and for affine/bitmap BGs `current counters - m*(PB,PD)` (equal to the counters of
  line `line-m` only if PB/PD and the references did not change in between). A clip-edge recompute on such a line
  uses the adjusted X/Y.
- Horizontal (when H != 0, after the call): `buf[x] = buf[x - x%(H+1)]` and the same for the visibility bit, for
  the whole line (`mosaic_masks` table 0x11deb0 = bits at multiples of H+1, with a carry between 32-bit words).

---

## 8. `render_scanline_disable_blank_layers_asm(x0 vis, x1 u32 *enabled)` 0xa089c (verified)

For BG n = 0..3: if the 32-byte bitmap at vis + 32n is all zero, clear bit n of *enabled. Bits 4..15 kept; bits
16..31 are cleared only when all four bitmaps are empty (`orr #0xf0` / `#0xff00` mask). Called by
`render_scanline_2d` after windows (1x 0x3f670, 2x 0x3f98c) with the BG0 slot holding the 3D bitmap when BG0 is
3D. Pure optimisation (a layer with no visible pixel contributes nothing), **guess** that the compositor never
needs the bit of an empty layer (e.g. as a blend target it has no pixels either).

---

## 9. Interplay with 2x, capture, windows

- 2x: none inside the BG renderers (section 0.3). Direct layers carry 3 extra quarter rows from display capture.
- Capture: the BG renderers never capture; a captured frame displayed as a bitmap BG is read from VRAM like any
  bitmap (q0 = the 1x capture) unless it qualifies as a direct layer (then the hi-res quarters are used).
- Windows are applied later to the bitmaps (`render_scanline_apply_windows`), not by the renderers.

---

## 10. State a replacement must keep, and when it changes

| state | where | changes |
|---|---|---|
| per-layer line buffer + bitmap | S+0x1e0.. / S+0xda0.. on render_scanline_2d's stack | every rendered line; **not** cleared between lines (stale content is visible in 12.4) |
| clip edges, reciprocals, dirty flag | L+0x4c..0x87, L+0xae | recompute on dirty; step per rendered clip line |
| affine counters | L+0x90/0x94 | line 0 reload, += PB/PD per line in render_scanline, event writes |
| everything else in 2.1 | | per event / per frame as listed |

The renderers have side effects on the engine struct (edges, L+0xae); `render_scanline_bg` restores VOFS/X/Y after
mosaic. A replacement running off the emulation thread needs the struct snapshot + event log of frame.md 12 and
must carry the edge state itself.

---

## 11. Costs (measured: instructions, qemu block profile)

Per layer and DS line, DraStic code only, including the 58 instructions of `render_scanline_bg` itself (one-layer
list); `re/2d/bg/bench.sh` (t_bg.c bench mode, 1,920 calls each, game-like parameters, 256x256):

| layer | instr/line | biggest parts |
|---|---|---|
| text 4bpp | 682 | span 451, visibility 66, tiled_ext 65, map setup 38+4 |
| text 8bpp | 1,265 | palette_lookup_8bpp 834, span 212 |
| text 8bpp ext palettes | 1,387 | palette_lookup_12bpp 866, span 282 |
| affine, rotated 30 deg, wrap | 3,153 | palette lookup 834, setup_arrays 821, render_tiles 436, merge 349, map indexes 165, entries 165, offsets 154 |
| affine, rotated, clip (240 px visible) | 3,060 | |
| affine, identity, wrap | 2,400 | |
| affine, scale 1/16, wrap (general path) | 5,412 | render_tiles 2,567 (256 one-pixel runs) |
| extended affine, rotated | 3,244 (3,425 with ext palettes) | |
| bitmap 16bpp identity / rotated / rotated clip | 2,561 / 3,324 / 3,294 | one function |
| bitmap 8bpp identity / rotated, large bitmap | 3,087 / 3,852 / 3,852 | |
| + mosaic H=3,V=3 (text 4bpp) | +1,891 | scalar H-mosaic in render_scanline_bg |

Per frame (another agent's profiles of the ds2d scenes, `re/2d/prof-2dT*.rep`, summing the BG functions): T0 (four
text BGs) ~0.61 M instructions/frame, T1 (ext palettes) ~0.87 M, T2 (affine + extended + bitmaps) ~1.50 M, T3
(bitmap modes) ~0.96 M, T9 (3D + text + extended affine) ~0.76 M, against 2.3..3.1 M for the whole frame (5.4 M
for T9). The existing 3D test ROMs (dsstress, dsscenes) set
DISPCNT = mode 0 with only BG0 = 3D: they exercise none of the BG renderers.

Where the time goes: 3.3 instructions per pixel in each palette lookup (scalar `ldrh` per pixel, 8 per iteration),
the run setup (~1,500 per affine line before any pixel is drawn), and the scalar bitmap loops. All of it is per DS
line at 1x, so it does not grow at 2x, but it sits on the emulation thread for engine A.

---

## 12. What an exact replacement must reproduce (checklist)

1. Formats of 0.2; renderer choice of 1.1 (mode 7 keeps BG2/BG3's previous renderer; engine B mode bits kept).
2. Text BGs (3): 9-bit offsets, block layout, 33 tiles, flips, palette numbers, ext slot selection (BG0/1: n +
   2*BGnCNT.13, BG2/3: fixed 2/3), raw palette bit 15 copied into the line.
3. Affine (4): per-pixel rule + clip edges with their exact arithmetic and history (4.2, including which renderers
   consume L+0xae when) + the 16-pixel garbage at |PA| or |PC| > 2047 in wrap mode (4.5). Engine B uses the same
   code.
4. **Stale lines:** (a) text 8bpp / extended affine with ext palettes on and `[L+0x18] == NULL` (only possible
   before any ext-palette bank was mapped since reset: `reset_video` zeroes the slot table, unmapping later stores
   a dummy pointer) write nothing; (b) mode 6 with BG1/BG3 enabled calls `render_scanline_null`. The compositor
   then uses whatever the slot held: the same layer's previous
   line on that thread, or other stack data at line 0 (not reproducible; treat as "keep the previous line"). No
   other renderer path leaves the bitmap unwritten (clip-mode lines with nothing visible write vis = 0).
5. Bitmaps (5) with the identity fast path and their own edge behaviour.
6. Direct layers (6): per-frame decision, row = screen line.
7. Mosaic (7) including the adjusted-counter semantics.
8. VRAM through the linear alias without masking (2.2); palette/OAM line-exact via the event replay (frame.md).
9. Not needed: the padding writes (3, 4.6), the u16 values at invisible pixels (only visible pixels and the
   bitmaps are consumed downstream, **guess** for consumers outside this subsystem).

Notes for a faster exact replacement (estimates, **guess** until built): the observable output is (bitmap, colours
at visible pixels), so a replacement may produce index lines plus bitmaps and resolve colours later, as long as it
uses the palette/ext-palette contents of that line (palette writes are line-exact through the event replay, so the
lookup must happen before the next line's events are applied, or on a per-line palette snapshot). DraStic spends
3.3 instructions per pixel in each 8-bit palette lookup and ~1,500 per affine line on run setup that 4.4 shows is
unnecessary; per-pixel affine evaluation with NEON and `tbl`-based 4bpp/8bpp decoding should cost on the order of
1 instruction per pixel. The clip-edge state (4.2) is cheap to replicate exactly with the port's 64-bit code.

---

## 13. Verification record

- `re/2d/bg/bgspec.c` / `bgspec.h`: the port (`spec_render_scanline_bg`, `spec_render_scanline_tiled_ext`,
  `spec_render_scanline_affine_{normal,extended}_ext`, `spec_render_scanline_bitmap_{16,8}bpp`,
  `spec_render_scanline_update_affine_variables`, `spec_video_2d_bg_layer_affine_setup_edges`,
  `spec_render_scanline_disable_blank_layers`) and the ideal models (`ideal_affine`, `ideal_bitmap`).
- `re/2d/bg/t_bg.c`: `sh /home/user/SuperDrastic/tools/rast/ut/run.sh t_bg.c bgspec.c` (env: UT_SEED, BG_N,
  BG_ONLY=blank|edges|text|affine|frames|bigclip, BG_EV event mask, BG_MUT mutation 1..6, BG_DEVDUMP). Seeds 1,
  11, 22, 33 at BG_N=1000 (seed 1 at 400): all passed; log `re/2d/bg/seeds.log`.
- Mutations: 1 (no 16-pixel bug), 3 (truncating step), 4 (NULL ext slot ignored), 5 (mosaic carry), 6 (Y edge not
  stepped) all fail as they must; 2 (per-pixel instead of runs) passes (4.4).
- `re/2d/bg/bgdiff.c` end to end (`LIB=bgdiff.so sh rast/run.sh <rom> 45 off`): see the table below
  (`re/2d/bg/e2e-*.log`).
- `re/2d/bg/bench.sh` + `bench.py`: the costs of 11 (`bench-all.txt`).

End to end, ds2d scenes (`/home/user/wt-scenes2d/stressrom/out`, 45 s each under qemu, DraStic's own renderers vs
the port on copies, every line of engine A; engine B is off in these ROMs). Layer-lines = (line, layer) pairs
compared by kind; "direct" = layer-lines skipped by both because the layer is a direct bitmap (T3/T9 rerun for
30 s with that counter):

| ROM | lines | mismatching | text 4bpp | text 8bpp | 8bpp ext | affine | ext-affine | bmp16 | bmp8 | visible px compared |
|---|---|---|---|---|---|---|---|---|---|---|
| T0 text BGs | 434,000 | 0 | 1,301,997 | 433,999 | 0 | 0 | 0 | 0 | 0 | 281 M |
| T1 ext palettes | 498,000 | 0 | 497,999 | 734,238 | 759,759 | 0 | 0 | 0 | 0 | 420 M |
| T2 affine | 494,000 | 0 | 987,998 | 0 | 0 | 493,999 | 241,733 | 125,546 | 126,720 | 353 M |
| T3 bitmap modes | 504,000 | 0 | 506,506 | 0 | 0 | 126,720 | 0 | 253,253 | 377,466 | 223 M |
| T4 sprites | 408,000 | 0 | 816,000 | 408,000 | 0 | 0 | 0 | 0 | 0 | 158 M |
| T5 affine OBJ | 400,000 | 0 | 800,000 | 0 | 0 | 0 | 0 | 303,280 | 0 | 204 M |
| T6 windows | 422,000 | 0 | 1,266,000 | 422,000 | 0 | 0 | 0 | 0 | 0 | 271 M |
| T7 colour effects | 428,000 | 0 | 856,000 | 428,000 | 0 | 0 | 0 | 0 | 0 | 166 M |
| T8 mosaic | 422,000 | 0 | 735,505 | 313,505 | 0 | 103,680 | 0 | 56,655 | 0 | 168 M |
| T9 3D + capture | 468,000 | 0 | 884,160 | 243,360 | 0 | 0 | 0 | 0 | 0 | 156 M |
| cycle (all scenes) | 480,000 | 0 | 944,448 | 304,320 | 103,104 | 56,448 | 17,280 | 73,152 | 27,840 | 206 M |

Direct layer-lines: T3 43,776 (the mode 3 16-bit bitmap qualifies), T9 31,104 (the captured frame shown as BG3).
`render_scanline_null` with an enabled layer: none in these scenes. Total: about 4.96 M lines, 0 mismatches.

Which DraStic-specific paths the scenes reach (30 s reruns with path counters, `e2e3-*.log`, all 0 mismatches):
T2: 1,612 affine layer-lines on the wrap general path (4.5, the 16-pixel bug: matched), 184,912 clip-mode
affine/bitmap layer-lines of which 25,152 have PA = 0 or PC = 0; T3: 159,584 clip layer-lines; T5: 234,752
clip 16-bit bitmap layer-lines (identity); T8: 646,336 mosaic layer-lines. Not reached by any scene: a NULL ext
palette slot and an enabled null renderer (mode 6 BG1/BG3) were not reached (counters 0); whether any scene leaves
clip edges behind by a mid-frame enable is not known (the unit test covers that case).

---

## Appendix A: hook points (16-byte `ldr x16,#8; br x16; .quad` patches)

| function | addr | first 4 words | PC-relative in the first 16 bytes? | args |
|---|---|---|---|---|
| render_scanline_bg | 0x36880 | a9b67bfd 910003fd 79415008 a90153f3 | no: trampoline OK (bgdiff.c uses it) | x0 eng, x1 lines (S+0x1e0), x2 vis (S+0xda0), w3 line |
| render_scanline_tiled_ext | 0xa1d20 | a9a97bfd 910003fd a90363f7 d00005f8 | adrp (4th): replace fully | x0 L, x1 buf, x2 vis, w3 line |
| render_scanline_affine_normal_ext | 0xa4730 | d118c3ff a9017bfd 910043fd a90463f7 | no (the adrp is the 5th) | same |
| render_scanline_affine_extended_ext | 0xa4b90 | d118c3ff a9017bfd 910043fd a9035bf5 | no | same |
| render_scanline_bitmap_16bpp | 0x32880 | d10a83ff b000096f aa0203ee a9007bfd | adrp (2nd): replace fully | same |
| render_scanline_bitmap_8bpp | 0x33700 | a9aa7bfd 9000096d 910003fd f9001bf7 | adrp (2nd): replace fully | same |
| render_scanline_update_affine_variables | 0x34550 | 3942ac03 79c13c01 79c14404 53155063 | no | x0 L |
| video_2d_bg_layer_affine_setup_edges | 0x326f0 | 7100003f 35000281 7100007f 34000583 | cbnz, cbz (2nd, 4th): replace fully | w0 X, w1 P, w2 W, w3 Q, x4 &start, x5 &step, x6 &width |
| video_2d_map_bg_direct_layers | 0x41700 | b9409001 f901201f f901781f 12000821 | no | x0 eng |
| video_2d_update_bg_mode | 0x41b60 | d00008e2 9105c004 f9471442 f9007802 | adrp (1st): replace fully | x0 eng |

`render_scanline_bg` has exactly two call sites, both in `render_scanline_2d` and mutually exclusive (0x3f024; 0x3f8fc
after `render_scanline_3d` when BG0 is 3D), with the same arguments.

Alternatively, replace the per-layer function pointers `[L+0x30]` instead of patching code: in the 2D code they are
written only by `video_2d_update_bg_mode` (called by set_display_control, set_bg_control and reset_video_2d), so a
hook there can substitute pointers.

## Appendix B: helper argument summary

Listed in sections 3 and 4.3; full instruction listings in `re/2d/fn/<name>.s`. Loop-count conventions of the asm
helpers (what they write past the logical count): `setup_tile_offsets` floor(n/32)+1 groups of 32 (n = count-1),
`load_tile_map_entries_*` floor(n/8)+1 groups of 8, `setup_flip_masks` floor(n/32)+1 groups of 32,
`setup_map_indexes_*` max(1, ceil(n/32)) groups of 32 plus one leading entry (stored as a 32-bit word),
`palette_lookup_*` ceil(count/8) groups of 8, `diff_tile_widths` ceil((n+2)/16) groups of 16.
