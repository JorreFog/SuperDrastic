# DraStic r2.5.2.2: an exact, deferred 2D engine for engine A (design and plan)

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. The analyses it cites are next to it: `re/2d/frame.md`, `bg.md`, `obj.md`,
> `compose.md` and `re/compositing-2x.md` are `re2d/*.md`. The reference ports of 3.1 (P1) are in `src/rast/spec/2d/`:
> `re/2d/bg/bgspec.c` is `bg.c`, `re/2d/obj/h/obj_model.c` `obj.c`, `re/2d/compose/model/compose_spec.c` `compose.c`
> (with the simple path that was in `spec/composite.c`), `re/2d/compose/pix/pixel.h` `pixel.h`; their unit tests are
> `tools/rast/ut/t_bg2d.c`, `t_obj2d.c` and `t_compose2d.c` (`tools/rast/README.md`, "The 2D engine").

Scope: replace DraStic's 2D rendering of engine A (BGs, OBJ, windows, colour effects, the composite with the 3D layer,
display capture, the scanout conversion) with our own renderer. It must produce the same bytes and run off the
emulation thread. Sources: the four analyses `re/2d/frame.md`, `bg.md`, `obj.md` and `compose.md` (cited as frame 3,
bg 4.2 and so on, with their own [D]/[M]/[R]/[G] marks), `re/compositing-2x.md`, and the repository (`src/rast/comp.c`,
`compfuse.h`, `spec/composite.c`, `src/dsflip.c`). For this document I also read the disassembly of `update_frame`
0x30dc0, `update_screen` 0x8a120, `update_screens` 0x8a690, `get_screen_ptr` 0x8a9c0, `set_screen_hires_mode` 0x8a2c0,
the head of `video_2d_render_scanlines` 0x42a90, `update_frame_3d` 0x59580, `video_3d_run_thread` 0x59430 and
`save_state` 0x74da0. Those claims are marked **[D]**. **[G]** marks an estimate or guess of this document.

Notation (frame.md's naming): `video` = DraStic's video struct (`st+0x36d1ec0`; `ds3d.h` and `comp.c` call it
`sys`), `sys = [video+0]`, `eng` = an engine struct of 0x81420 bytes (A = `video+0x2e78`, B = `video+0x84298`),
`L(n) = eng+0xc0+n*0xb0`, `S` = `render_scanline_2d`'s scratch area (sp+0x180), `C = video+0x458820` (capture
descriptor). A "line" is a DS line 0..191; a "quarter" is the 256 even or odd pixels of one 2x output row. The
**job** is one deferred frame of engine A; the **worker** is the thread that renders it.

---

## 0. Findings that shape the design

1. **On a normal frame, engine A's output is a pure function of state that exists at line 192** (frame 10, 12).
   - DraStic renders nothing before `update_frame`.
   - Registers, palette and OAM reach the renderer only through the line-tagged write log, replayed after each
     line.
   - VRAM contents and mapping are read as they are at line 192.
   - The 3D layer is the output frame that `render_scanline_3d` returns at 192.

   So a snapshot taken at 192, together with the log, reproduces the frame exactly, whenever it is rendered.
2. **Only two bytes of render-side state carry over into the next frame.** One is the window Y state `eng+0xb4`:
   line 0 initialises it only for edges in vblank (compose 4.2). The other is the OAM-dirty byte `eng+0xb6`: an OAM
   write logged on the last rendered line makes the next frame re-sort at its line 0. Everything else the renderers
   change is either set up again at line 0 or a function of the registers: the affine counters, the clip edges and
   `L+0xae`, the window masks and `eng+0xb5`, the OBJ tables (bg 4.2, frame 5). Stale `S` stack data is garbage in
   DraStic as well.
3. **DraStic's per-stage routines can run on more than one thread, one engine each.** Engine B already runs all of
   them on `video_render_thread` while engine A runs on the emulation thread (frame 2).
   - They read only the engine struct, the layer structs and the pointers stored in them.
   - Two exceptions reach `video` through `[eng+0]`. `render_scanline_3d` reads the 3D frame pointer.
     `video_2d_reorder_obj` reads the VRAM bank records and the capture validity bits, for the full-screen image only.
   - `video_2d_process_event` reads only constant bank-base pointers through `video`, plus the platform screen table
     for POWCNT1 (key 0x305) (frame 3, 7.2). That is a reading of the trigger map; P2 must confirm it over all 0x92c
     bytes [G].

   So a worker can run DraStic's own stage routines on a private engine copy whose pointers lead to snapshot copies.
4. **Correction to frame 8.3 and compositing-2x 1.1: the scanout `[eng+0x38]` is not a DRM dumb buffer** [D].
   - `get_screen_ptr` returns `[entry+8]` of the platform screen table (`[0x15fef8]`, entries of 0x28 bytes).
     `set_screen_hires_mode` mallocs that buffer: 0x30000*bpp bytes, 768 KiB at 2x and 32 bpp, the same buffer every
     frame.
   - `update_frame` calls `update_screen(screen)` for each rendered engine. That function runs `SDL_LockTexture`
     (dsflip hands out a dumb buffer), one `memcpy` of 384 x 2048 bytes (row by row, with a printf, if the pitch
     differs) and `SDL_UnlockTexture`.
   - `update_screens` then runs `SDL_RenderCopy` x2 and `SDL_RenderPresent`.

   So the emulation thread also copies 1.5 MB a frame into scanout memory (DRM dumb buffers, likely write-combined
   [G]). A deferred engine A can write its rows straight into the dumb buffer and drop its half of that copy.
5. **Engine A costs 1.0-4.1 M instructions a frame on the emulation thread**: 1.0-1.9 M in the ds2d scenes, 4.1 M in
   T9 with capture, ~1.1 M in a 3D-only scene (~0.4 M with our compositor hooks). At 600 MHz and about one
   instruction a cycle [G] that is 2-7 ms of the 16.7 ms frame (1.5).

---

## 1. The 2D engine as DraStic implements it

### 1.1 Frame model

| when | what (emulation thread unless noted) | ref |
|---|---|---|
| line 0 (`event_scanline_start_function` 0x1d960) | `start_frame` 0x30870: palette/OAM switched to deferred mode; scanout pointer and pitch in `[eng+0x38/0x40]` (NULL plus `[eng+0xb8]=1` when frame skip drops the engine); `video_2d_reorder_obj` and `video_2d_map_bg_direct_layers` (bank records read now); event queues reset; `[video+0x458894]=0`; C filled from DISPCAPCNT (`C+0x56` = capture this frame) | frame 1.1 |
| lines 0..191 | The JIT runs. `video_2d_queue_event` 0x429f0 appends stores to I/O 0x000-0x003, 0x008-0x055, 0x06c-0x06f and 0x305, and every changed palette/OAM byte, as 12-byte entries `{u32 key, u32 value, u8 line, u8 size}` at `eng+0x21418`. The renderer's palette, OAM and registers do not change. VRAM stores land at once. | frame 3, 4 |
| hblank of line L <= 191 | Catch-up if engine A is in display mode 2/3, an hblank DMA targets VRAM, or the per-game hack byte is set: `video_render_scanlines(video, L)` renders lines `[0x894..L]` of A, then B, now | frame 1, 10 |
| line 192 (`update_frame`) | `video_render_scanlines(video,191)` 0x30be0: apply pending VRAM remaps to the alias; signal `video_render_thread` (B: `video_2d_render_scanlines(engB,0,191,NULL)`); `video_2d_render_scanlines(engA,0,191,C)` here; wait for B. Then: if `C+0x56`, clear DISPCAPCNT.31 and mark the hi-res validity bits `C+0x20..`; `remap_palette_oam_direct`; `update_screen(A)`, `update_screen(B)` (memcpy), `update_screens` (present); `video_3d_finish_rendering` (threaded_3d); `update_frame_geometry`. After that: input, auto-saves, vblank DMAs | frame 1.2, 2; [D] |
| lines 192..262 | vblank: `video_2d_process_event` applies register writes at once; palette/OAM are written directly | frame 3 |
| line 215 | 3D of the next frame. threaded_3d=0: `update_frame_3d` 0x59580 on this thread, which also renders a third of the bins. threaded_3d=1: `video_3d_start_rendering`, then `video_3d_run_thread` 0x59430 calls `update_frame_3d_4x` 0x58f10. Both write the output frame that line 192's composite read (threaded: the formerly published one) | [D], comp.c |

`video_2d_render_scanlines(eng, first, last, C)` loops over the lines:
- `if [eng+0x38]: render_scanline(eng, [eng+0x38]+line*[eng+0x40], line, C)`. The pointer is re-read every line
  (`ldr x1,[x27,#0x38]; cbz` at 0x42b18 [D]).
- Then it replays the entries with `line <= this line`: the inlined switch of `video_2d_process_event` 0x420c0, which
  also runs `set_display_control` 0x41ef0, `set_bg_control` 0x41cb0, `update_bg_mode` 0x41b60 and `reorder_layers`
  0x41300.

A write during line L therefore takes effect at line L+1. With `[eng+0x38] = NULL` only the replay runs. **That is
DraStic's own replay-only path, the one frame skip uses.**

### 1.2 State and when it changes

| state | changes | carried to the next frame? |
|---|---|---|
| Registers in the engine header: DISPCNT `eng+0x90`, WININ/OUT `+0x9c`, BLDCNT/BLDY/BLDALPHA/MASTER_BRIGHT/MOSAIC `+0xa0..0xa8`, WINxH/V `+0xaa..0xb0`. Per layer: BGnCNT `L+0x98`, HOFS/VOFS `+0x9a/0x9c`, PA/PC/PB/PD `+0x9e..0xa4`, X/Y references `+0x88/0x8c` | Replay after each line; `process_event` in vblank | yes (DraStic state) |
| Derived: lists `eng+0x84/0xb3` and `+0x8c/0xb2`, bases `L+0x38..0x48`, sizes `L+0xa6..0xac`, renderer `L+0x30`, ext slot `L+0x18`, `eng+0x10/0x94/0x98` | Recomputed at replay | yes (recomputable) |
| Palette `[eng+0x18]` (A: sys+0x16070, 0x400 B) and OAM `[eng+0x30]` (sys+0x15070, 0x400 B) | Byte events at replay (OAM also sets `eng+0xb6`); direct stores in vblank | yes |
| VRAM through the linear alias `[eng+8]=[sys+0xfd500]` (DS address - 0x06000000: A BG +0, A OBJ +0x400000); ext palettes in bank memory (`[eng+0x20]` slot table, `[eng+0x28]`) | Contents: any time. Mapping: at each render call | shared memory |
| OBJ tables `eng+0x380..0x21418`: 128 records of 0x58 B, per-line lists `eng+0x2f80`, counts `+0x20f80`, line flags `+0x21340`, image `+0x21400/08/10` | `start_frame`, and the first rendered line after an OAM event (bank records and validity read at that moment) | no (rebuilt) |
| Direct layers `eng+0x240/0x248`, `0x2f0/0x2f8` | `start_frame` | no |
| Affine counters `L+0x90/0x94` | Reloaded at line 0; `+= PB/PD` on lines 1..191 (`render_scanline`); reloaded on X/Y writes | no (savestate bytes only) |
| Clip edges `L+0x4c..0x87`, `L+0xae` | Recomputed when `L+0xae` is set (line 0, PA..Y writes); stepped on each rendered clip line | no |
| Window masks `eng+0x44/0x64`, dirty bits `eng+0xb5` | Rebuilt on the next windowed line after a WINxH write | no (function of WINxH) |
| **Window Y state `eng+0xb4`** | Line-0 rule (set if Y1>191, then cleared if Y2>191); stepped by `generate_window_masks` on rendered lines that have a window enabled | **yes** |
| **OAM dirty `eng+0xb6`** | Set by an OAM replay; cleared by `render_scanline_2d` after a re-sort | **yes** |
| C; hi-res buffers `[C+bank*8]` (0x600 B a line, q1..q3); validity `C+0x20+bank` | `start_frame`; `update_frame` after a capture; CPU VRAM stores (validity cleared) | yes |
| 3D frame `[video+0x34eb58]` (threaded_3d=0) / `[video+0x34eb60]` (=1) | Written from line 215 | per frame |
| `S` (layer lines, vis bitmaps, alpha plane, window/SEMI/BMP masks) | Every line; parts left stale on the stack | garbage |

### 1.3 The per-line pipeline (engine A, 2x, 32 bpp)

```
render_scanline(eng x0, out x1, line w2, C x3) 0x404a0         frame 0x1aa0; planes sp+0x290 = 4 x {R6,G6,B6}[256]
  any DMA with start mode 4: dma_transfer_display 0x1f190 (FIFO line; line 191 disables non-repeat channels, IRQ)
  line 0: window Y init, L2/L3 counters = refs, L2/L3+0xae = 1;  lines 1..191: counters += (PB, PD)
  mode 1 (or capture): r = render_scanline_2d(eng, planes, line, C, hires) 0x3ef00      r != 0: 4 quarters
    eng+0xb6 ? video_2d_reorder_obj 0x3e530
    p3d = render_scanline_3d(video, line) 0x59950 = 3D frame + line*0x1000   if DISPCNT.3 && (BG0 on || C+0x51==2)
    render_scanline_bg 0x36880: per BG of eng+0x8c (skipping direct layers), mosaic inline, [L+0x30](L, buf, vis, line):
        tiled_ext 0xa1d20 | affine_normal_ext 0xa4730 | affine_extended_ext 0xa4b90 | bitmap_16bpp 0x32880 |
        bitmap_8bpp 0x33700 | null 0x31850  ->  u16[256] at S+0x1e0+n*0x220+0x10, 256-bit vis at S+0xda0+n*32
    DISPCNT.12: render_scanline_obj_c 0x36b70 -> colour S+0xa70, alpha S+0xc90, vis[4..7] S+0xe20, OBJ window
        S+0xea0, SEMI S+0xec0, BMP S+0xee0; returns 0x10
    OBJ image ([eng+0x21400]) / direct BG2,BG3: bit-15 maps stored over their vis slots
    render_scanline_generate_window_masks 0x3b360 -> inh[5] S+0xf00, fx S+0xfa0      (steps eng+0xb4)
    flags = line flags | 4 (alpha) | 8 (brightness) | 2/0x10 (3D) | 0x20 (image); bm = BLDCNT & (lm|lm<<8|0xf0f0)
    1x: set_3d_visibility 0x3c2c0, apply_windows 0x3b650, disable_blank_layers 0xa089c,
        render_scanline_2d_composite 0x3c6d0 -> planes
    2x (hires && enabled layers & {3D BG0, direct BG2/3, image}): per q: 3D quarter (+ horizontal_shift_3d 0x3c630),
        set_3d_visibility, hi-res layer maps, apply_windows, render_scanline_2d_composite(.., planes+q*0x300, ..)
    capture (C+0x51 && line < C+0x50): capture_direct 0xa0910 / _3d 0xa09b0 / blended 0x3bc60 / _3d 0x3bdb0,
        q0 -> VRAM (C+0x28 + ((C+0x48 + C+0x4c*line) & 0xffff)*2), q1..q3 -> hi-res buffer
  mode 0: white planes (2D still rendered when capturing);  mode 2: VRAM line [eng+0x10]+line*512;  mode 3: FIFO line
  convert: MASTER_BRIGHT [eng+0xa6]: color_convert_direct_32_2x 0xa0ae0 (q0,q1)->row 2L, (q2,q3)->row 2L+1 |
           shade_32_2x 0xa0d80 | memset white/black (factor >= 16)
```

The composite has two paths (compose 5).
- **Simple path, `flags & 7 == 0`:** one-deep priority encode, then `select_pixels`: u16 merge, backdrop, 6-bit
  expansion, 3D bytes inserted. Optionally the BLDCNT shade.
- **Complex path:** two-deep encode, top and second planes, T1/T2 target masks, EVA/EVB/OFF planes, then
  `min(63, (t*EVA + s*EVB [+63*OFF] + 16) >> 5)`.

Both collapse into the per-pixel closed form of compose 5.6 (`re/2d/compose/pix/pixel.h`). It is exact against
DraStic's planes on 1.4 M composites at runtime.

Formats:
- Layer line: u16 holding the raw palette RAM or VRAM value, bit 15 as stored.
- Visibility: 256 bits, LSB first.
- 3D pixel: u32 `r6|g6<<8|b6<<16|a5<<24`.
- Planes: 6-bit.
- Scanout: XRGB8888 with `c6<<2`, plus the byte-3 pattern of compositing-2x 2.

DraStic-specific behaviour the replacement must keep:
- OBJ order: priority first, then OAM index.
- No OBJ mosaic and no per-line OBJ limit.
- The 16-pixel garbage map entry at |PA| or |PC| > 2047 in wrap mode.
- Clip windows that depend on history.
- The 2x master-brighten bug.
- BLDALPHA bit 7 leaks into EVB.
- Forced blank (DISPCNT.7) is ignored.

The full lists are bg 12, obj 7 and compose 10.2.

### 1.4 Scanout and presentation [D]

- Engine X writes DS line L to rows 2L and 2L+1 at `[engX+0x38] + L*0x1000` and `+0x800`. This is DraStic's own
  768 KiB screen buffer (0.4).
- `update_frame` (0x30e60..0x30f9c) calls `update_screen(A's screen)` when `[engA+0xb8]==0`, and
  `update_screen(B's screen)` when `[engB+0xb8]==0`. It then calls `update_screens`.
- A's screen index: 0 when bit 15 of the halfword at `sys+0x1b374` is set (presumably POWCNT1's display swap),
  otherwise 1, or 0 with the screen-swap option `cfg+0x498` (0x30f50..0x30f80). `get_screen_ptr` then XORs
  `[tab+0xac]`.
- `update_screen` is a lock, memcpy and unlock. Under dsflip, `SDL_LockTexture` takes a FREE dumb buffer
  (state WRITING), `SDL_UnlockTexture` marks it WRITTEN, and `SDL_RenderPresent` enqueues the WRITTEN buffers of
  both panels as one frame for the presenter thread (one commit per emulated frame, `dsflip.c` 1298-1460).

### 1.5 Costs (instructions, qemu block profile, 2x, DraStic's own code unless noted)

| stage | routines | per call / per line | per frame, measured | ref |
|---|---|---|---|---|
| log append | `video_2d_queue_event` | ~40 per logged write | 0.004 M (T0, 98 events) | prof T0 |
| line loop + replay | `video_2d_render_scanlines` | ~15 per line, ~10 per event | 0.005-0.008 M | frame 14 |
| OBJ tables | `video_2d_reorder_obj` | ~2,000 + ~100 per OBJ + ~16 per box line | 0.008-0.042 M (re-sorts again after every OAM event) | obj 8 |
| line drivers | `render_scanline` / `render_scanline_2d` self | 120 / 190-490 per line (more with direct layers or the image) | 0.023 / 0.037-0.13 M (A) | compose 9 |
| BG | `render_scanline_bg` + renderers | per layer line: text 4bpp 682, 8bpp 1,265, ext 1,387, affine 2,400-3,425, general affine 5,412, bitmaps 2,561-3,852; H-mosaic +1,891 | 0.60 (T0), 0.87 (T1), 0.96 (T2), 0.73 (T3), 1.34 M (T8) | bg 11, prof |
| OBJ | `render_scanline_obj_c` | 128 per line with no entries; ~310 per non-empty list; 400-670 per 64-px tile row; 960-1,340 per affine row | 0.65 (T4), 0.68 M (T5) | obj 8 |
| windows | `generate_window_masks` + `apply_windows` | ~36 per line off; 300-550 per line on | 0.08 M (T6) | compose 9.1 |
| 3D visibility | `set_3d_visibility` + gather | 424 per quarter (DraStic), 28 (comp.c's table) | 0.326 -> 0.024 M | README |
| composite | `render_scanline_2d_composite` + helpers | per quarter: simple 620-1,100, + shade 1,200-1,700, complex 2,600-2,900, complex + brightness 4,850-6,100 | S7 0.48 (comp.c fused: 0.07), L4 0.82 (0.39), T7 0.39, T9 1.66 M | compose 9 |
| capture | `capture_direct` / `capture_blended(_3d)` | 292 / 8,465-8,719 per call; 4 calls per line at 2x | T9 1.33 M; up to 6.7 M | compose 9 |
| convert | `direct_32_2x` / `shade_32_2x` | 361 / 937 per 512-px row; 2 rows per line | 0.139 M per engine; +0.22 M during a fade | compose 8 |
| present copy | `update_screen` memcpy | 768 KiB per screen | not in the profile (libc); ~0.06 M, memory-bound [G] | [D] |
| **engine A** | | ~5,600 per line, 3D only | S7 1.07 (0.36 with comp.c's hooks); T0-T8 1.0-1.9; T9 4.1 M | prof2dcat |

The T-scene totals are DraStic-alone profiles (`re/2d/prof/*.rep`) minus engine B's display-off share (~0.16 M).
Neither ds2d nor the 3D test ROMs put content on engine B.

---

## 2. Architecture of the replacement

### 2.1 Principles

1. **Snapshot at 192, render later, same bytes.** The emulation thread hands over a copy of everything engine A's
   frame depends on (0.1) and continues. The worker reproduces DraStic's line loop on that copy: render line L, then
   replay the events of line L.
2. **DraStic's struct stays the master copy.** The emulation thread still advances DraStic's own engine A state with
   DraStic's replay-only path (1.1). Vblank writes, catch-up renders, savestates and synchronous frames therefore see
   exactly DraStic's state. The two carried bytes (0.2) are written back when the job is joined. **No private state
   survives a frame.** Savestate loads, resets, frame skip and synchronous frames need no resynchronisation.
3. **One job in flight.** Frame N's job is joined before frame N+1's snapshot, and before anything else touches engine
   A's render state. Presents stay in order. A worker slower than a frame applies back-pressure.
4. **Start with DraStic's stage routines, replace them one by one.** The worker's control flow is our port of
   `render_scanline` and `render_scanline_2d`. The stages are function pointers: DraStic's routine first, ours once it
   is proven exact (the `b0.c` approach of the 3D rasterizer).
5. **Fall back to synchronous rendering** for any frame whose semantics the job cannot reproduce (2.4).

### 2.2 Timeline of a deferred frame

```
            192 update_frame                                          215                         192 (next)
emu     ...|post job; copy small; replay-only; wait B + VRAM copy|present B (A pending)|JIT ...|3D bins (gated)|..|join
B thr      |render B (DraStic, unchanged)|
worker     |copy VRAM pages + ext pal|render A lines 0..191 -> dsflip buffer, replay events|done -> dsflip commit
3D bins                                                                    |resolve bin b after worker line >= 16b+15|
```

### 2.3 Hooks (16-byte `ldr x16,#8; br x16` patches through `rast_hook`)

| hook | address | first words | trampoline | role |
|---|---|---|---|---|
| `video_render_scanlines` | 0x30be0 | a9b97bfd 910003fd a9025bf5 b9622015 | yes | Joins the job in flight on every call, frame end and catch-up, before B is signalled. Calls the original. Before returning, waits until this frame's job has copied its VRAM pages. |
| `video_2d_render_scanlines` | 0x42a90 | a9b97bfd d2800187 910003fd a90573fb | yes | engA, VCOUNT `[st+0x14]`==192, (0,191): decides (2.4). Deferred: copies the small inputs, posts the job, runs the original with `[engA+0x38]=NULL` (replay only), restores the pointer |
| `update_screen` | 0x8a120 | a9b87bfd 910003fd a9025bf5 2a0003f5 | yes | A's screen in a deferred frame. dsflip mode: returns at once (the worker writes the dumb buffer). Otherwise: joins, then the original |
| `update_frame_3d_4x` / `_1x`, `reset_video_3d` | comp.c | (already wrapped) | | 3D-frame writers: the release gate (2.7) |
| our bin loops (`rast.c` `render_bins`, `hr.c` 3x) | | | | per-bin gate before an output block is written |
| `save_state` 0x74da0, `video_load_savestate` 0x315e0, `reset_video` 0x31370, `menu` 0x7fbd0, `set_screen_hires_mode` 0x8a2c0 | | d11543ff…, a9ba7bfd…, a9b97bfd…, d11883ff…, a9bb7bfd… | yes | Join. At menu and save, also refresh DraStic's A screen buffer from the last finished frame (thumbnails) |
| dsflip `SDL_DestroyTexture` | ours | | | joins before freeing a texture's buffers |

`load_state` (adrp in the 4th word) and `reset_system` (adrp in the 2nd) are covered by `video_load_savestate` and
`reset_video`.

### 2.4 The deferral decision (at the hook, per frame)

The frame is deferred only when all of these hold:
- It is a normal frame: `[video+0x458894]==0`, VCOUNT 192, engine A rendered (`[engA+0x38]` set).
- No capture: `C+0x56==0`.
- `cfg+0x4a0==1` (2x) and 32 bpp.
- A scan of A's log finds no POWCNT1 event (key 0x305 would repoint `[eng+0x38]` mid-loop) and no DISPCNT event
  selecting display mode 2 or 3.
- No DMA channel is enabled with start mode 4 (the FIFO DMA has emulation side effects at line 191).
- The log has fewer than 0x8000 entries (beyond that, DraStic's queue overruns).
- The deferred present path is available: dsflip active, or the simulator mode of 2.8.

Every other frame is synchronous. In v1 it renders with DraStic. In P6 it renders with our engine on the emulation
thread. `RAST2D_STATS` counts the frames of each kind and the reason for each synchronous one.

### 2.5 The snapshot

| item | size | taken by | notes |
|---|---|---|---|
| engine header `eng+0x00..0x380` (registers, derived fields, layer structs, window masks, eb4/eb5/eb6) | 896 B | emulation thread, before the replay-only pass | frame-start values (nothing was replayed yet) |
| palette `[eng+0x18]`, OAM `[eng+0x30]` | 1 KiB each | emulation thread | in deferred mode the real arrays hold the frame-start values; the worker's replay writes its copies |
| log entries `[start,count)` | 12 B each (tens to ~2,000) | emulation thread | the queue is reused from line 0 of the next frame |
| OBJ tables in use: records (11 KiB), counts (0x3c0 B), line flags (0xc0 B), image fields, the list bytes each count names | ~2-15 KiB | emulation thread | **copied, not recomputed.** `start_frame` made the image decision with line-0 bank records and validity bits, and CPU VRAM stores during the frame can clear those bits |
| bank records `video+0x10..0xa0`, `C+0x20..0x23`, `C+0..0x1f` | ~180 B | emulation thread | for mid-frame re-sorts, which DraStic runs at render time |
| VRAM view: engine A's reachable 16 KiB pages that are backed by a bank, copied through DraStic's alias after the remaps to the same offsets of a private flat 8 MiB view (`MAP_NORESERVE`) | <= 41 pages (656 KiB), typically 8-24 | **worker, while the emulation thread waits for engine B** | Reachable: BG `[0, 0x100000)` (bases + 64 KiB tiles + unmasked overflow, bg 2.2); OBJ `[0x400000, 0x460000)` (obj 2.4, +64 KiB affine wrap). Pages without a bank read as zeros in v1 (open question) |
| BG ext slots `[[eng+0x20]+8s]`, OBJ ext `[eng+0x28]` | <= 40 KiB | worker | only if DISPCNT.30/.31 is set at frame start or in an event |
| 3D frame | pointer | emulation thread | not copied: the release gate (2.7) protects it |
| hi-res capture buffers (direct layers, OBJ image) | pointers | | written only by capture frames, which are synchronous and come after a join |
| scanout | dsflip buffer of A's texture, and its pitch | emulation thread (dsflip API) | zero-copy (2.8) |
| cfg hires, bpp, threaded_3d | | emulation thread | |

**Copying VRAM behind B.** Between posting the job and leaving `video_render_scanlines`, nothing can write VRAM: the
emulation thread (ARM9, ARM7, DMA) is parked waiting for B, and B and the 3D threads only read VRAM. So the worker
copies VRAM during B's render. The emulation thread waits for `max(B, copy)`, and it already waits for B.

At 0.1-0.7 MB a frame (6-40 MB/s) the copy is about 0.05-0.3 ms at 600 MHz [G], usually hidden. It also pollutes the
worker's cache, not the emulation thread's.

**Rebasing pointers in the private engine copy** (DraStic's layout, 0x81420 B, allocated once):
- `[eng+8]` and `L+8` → the view.
- `[eng+0x18]` and `L+0x10` → the palette copy.
- `[eng+0x30]` → the OAM copy.
- `[eng+0x20]` → a private 4-entry slot table; `L+0x18` and `[eng+0x28]` → the ext copies.
- `L+0x20` (direct layers) and `[eng+0x21400]` (image) → the view at the same alias offset.
- Each OBJ record's VRAM and palette pointers (`+0x38`, `+0x30`) → the view or the copies.
- `[eng+0x38/0x40]` → the output.
- `[eng+0]` stays the real `video`. DraStic's `process_event` reads only constant bank-base pointers through it.
  POWCNT1 events are excluded (2.4).
- `L+0x28`, `[eng+0x21408]` and `[eng+0x10]` are left as they are.

### 2.6 The worker

```
job(J):  wait for the VRAM copy (it does it itself), rebase J.eng
  for line = 0..191:
     line_init(J, line)                       line 0: window Y init, counters = refs, L2/L3+0xae = 1; else += PB/PD
     mode 1: render_2d(J, line) -> 4 quarter plane sets (port of render_scanline_2d, compose 3.1, with no capture)
             stage table: render_scanline_bg, render_scanline_obj_c, generate_window_masks, apply_windows,
             disable_blank_layers, set_3d_visibility (comp.c's hook: table or NEON), horizontal_shift_3d,
             render_scanline_2d_composite (comp.c's fused hook) - DraStic's addresses until ours are proven
             OAM dirty: obj reorder port (obj_model.c) with J's bank records and validity
             3D quarter pointer = J.frame3d + line*0x1000 + q*0x400 (our own render_scanline_3d)
     mode 0: white
     convert with J.eng MASTER_BRIGHT: DraStic's direct_32_2x / shade_32_2x / memset -> rows 2L, 2L+1 of J.out
     release_3d(J, line + 1)                   (2.7)
     for e in J.log with e.line == line: video_2d_process_event(J.eng, &e)     DraStic's own switch, on the copy
  J.eb4 = J.eng[0xb4]; J.eb6 = J.eng[0xb6]; done (2.8)
join(J) (emulation thread): wait for done; engA[0xb4] = J.eb4; engA[0xb6] = J.eb6
```

- The worker keeps `S` as a per-thread buffer that is never cleared. Within a frame its stale bytes behave like
  DraStic's stack. At frame start DraStic's stack holds whatever was left there, which in practice is most likely the
  previous frame's render at the same depth [G].
- The affine counters and clip edges are not written back. DraStic reloads them at line 0. Its own frame-skip path
  leaves them unstepped too, so only savestate bytes differ.
- The emulation thread runs `video_2d_reorder_obj` and `map_bg_direct_layers` at `start_frame` for engine A,
  unchanged.

Emulation-thread cost of engine A after the change [G]: `start_frame`'s work (0.006-0.04 M), the log appends
(unchanged), the small copies and the replay-only pass (~0.005-0.01 M), and no A memcpy in `update_screen`. That is
about 0.02-0.06 M a frame instead of 1-4 M plus a 768 KiB copy.

### 2.7 The 3D layer: the release gate

The output frame that frame N's job reads is overwritten from line 215 of frame N in both threading modes.
- threaded_3d=0: `update_frame_3d` renders the next frame into the same buffer.
- threaded_3d=1: `update_frame_3d_4x` picks the unpublished buffer, which is the one frame N composited.

Instead of copying 768 KiB, the job publishes `line3d` (release-store) after it has composited each line.
- Our bin loops (`rast.c` `render_bins` and diff mode, `hr.c`'s 3x loop) wait before resolving bin b into a buffer a
  job reads, until `line3d >= 16(b+1)`. A bin is 32 output rows, 16 DS lines.
- DraStic's gap passes run after all bins, so the worker is past line 191 by then.
- comp.c's `update_frame_3d_4x` wrapper waits for full release before the original only with threaded_3d=1, where it
  runs on `video_3d_run_thread`. That covers the no-render `memcpy(SYS_OUTPUT, SYS_LAST)`.
- `update_frame_3d_1x` and `reset_video_3d` join.
- A job with no 3D line releases at once.

Ordering is deadlock-free: the job waits on nothing from the 3D, and `video_3d_finish_rendering` (at the next 192)
comes after our join. With threaded_3d=0 the emulation thread can wait at 215 only if the worker is behind the bin
being resolved [G]. With threaded_3d=1 a wait is on the 3D thread.

The deferred mode therefore needs our 3D renderer's hooks (`RAST=ours`). comp.c's three frame-writer wrappers move
into a shared module.

### 2.8 Presentation

- **dsflip mode (device):**
  1. At the snapshot, our hook asks dsflip for a buffer of A's texture (`take_free`, state WRITING). That texture is
     the screen table entry whose `+8` is `[engA+0x38]`. The job writes rows 2L and 2L+1 at `2L*pitch` and
     `(2L+1)*pitch`.
  2. `update_screen(A)` is skipped. `update_screen(B)` and `update_screens` run as usual.
  3. `SDL_RenderPresent` sees A's texture with a buffer marked 2D-pending and holds the frame (both panels).
  4. When the worker finishes, it calls `dsflip_2d_done(buf)`: WRITTEN, the held frame is enqueued, the presenter is
     woken, and the phase sample for adaptive pacing is taken at that time. Shader mode shades only after done.
  5. Latency stays about the same: the worker starts at 192 as DraStic's own engine A render did, on another core.
  6. DraStic's A screen buffer goes stale. Presumably only the menu background and the savestate thumbnails read it
     [G] (`save_state` copies two 0x18000-byte images [D]). So the menu and save hooks copy the last finished dumb
     buffer back. That read is slow (likely write-combined memory [G]), but rare.
- **Simulator and no-dsflip mode:** the job writes DraStic's own A screen buffer. `update_screen(A)` joins and then
  memcpys as usual. This mode is exact and tests everything except dsflip. It gains nothing over DraStic, because the
  emulation thread waits for `max(A, B)` as before.

### 2.9 What the emulation thread still waits for

| wait | where | normally |
|---|---|---|
| the previous job | `video_render_scanlines` (every call), `update_screen` (non-dsflip), the join hooks of 2.3 | done long before |
| this job's copy of the small inputs | the hook itself (it does that copy) | ~5-10 µs |
| this job's VRAM copy | end of `video_render_scanlines` | hidden behind B |
| engine B | unchanged until P7 | 0.3-1.5 M instructions of B |
| the 3D gate | only with threaded_3d=0, at 215, if the worker lags | rare [G] |
| presentation | never (dsflip's presenter commits asynchronously, as today) | |
| capture results | capture frames are synchronous (2.10) | |

### 2.10 Synchronous frames, and what deferring them would need

| case | why synchronous | later |
|---|---|---|
| Display capture (`C+0x56`) | The CPU may read or write the destination bank right after `update_frame`. `update_frame` marks hi-res validity and DraStic unmaps kind-6 banks after the render. A late capture would overwrite later CPU writes | P8: render deferred, trap CPU accesses to the destination bank until done (DraStic already traps writes to captured banks: `unmap_memory_page_region` at 0x31160); join before B's render, which may show the bank |
| Catch-up (display mode 2/3 at hblank, hblank DMA to VRAM, hack byte) | DraStic renders at hblank with the then-current VRAM | P6: our engine, synchronous, for the range |
| DISPCNT event to mode 2/3 mid-frame | mode 2 needs the LCDC bank; mode 3 needs the FIFO DMA | P6 |
| POWCNT1 swap event | A's rows would go to B's buffer mid-frame | stays synchronous (rare) |
| FIFO DMA (start mode 4) enabled | `dma_transfer_display` has emulation side effects | stays synchronous |
| hires 0/2, 16 bpp | not the 2x target | optional |

**Dual-screen-3D games capture every frame, or every other frame.** They stay synchronous until P8: a large class of
3D games.

### 2.11 Savestates, reset, menu, frame skip, forced blank, display modes

- **Save.** The join first writes back eb4 and eb6. DraStic's struct, palette, OAM and VRAM are then exactly as DraStic
  would hold them, except the affine counters (see 2.6). Thumbnails come from the refreshed screen buffer.
- **Load and reset.** Join, then DraStic's own code runs. The next snapshot comes from the loaded state. The 3D frames
  are covered by the existing `reset_video_3d` and update wrappers.
- **Menu entry and texture recreation** (hires toggle, `set_screen_hires_mode`, `SDL_DestroyTexture`): join.
- **Frame skip** (`[engA+0x38]==NULL`): DraStic's replay-only path; no job.
- **Forced blank** (DISPCNT.7): ignored, as in DraStic (T0 q3 tests it). Showing white would be a deliberate
  deviation.
- **Display mode 0:** white planes, deferred. With capture it is synchronous. Modes 2/3: see 2.10.

### 2.12 Engine B

Version 1 keeps DraStic's engine B: signalled before our hook, it renders concurrently with the job's VRAM copy, and
neither side writes the other's data. The emulation thread still waits for B at 192, which costs 0.3-1.5 M
instructions a frame (a 2D-heavy lower screen, or L4's direct-bitmap 4-quarter path at 0.72 M).

P7 runs B through the same job machinery: VRAM offsets +0x200000 (BG) and +0x600000 (OBJ), banks C, D, H and I, no
3D layer, C = NULL. dsflip holds both panels until both jobs are done. Our `video_render_scanlines` hook then does the
frame-end bookkeeping itself, including `[video+0x458894]=192`, and never signals `video_render_thread` on normal
frames.

### 2.13 Standalone exact replacements versus the deferred design

| piece | standalone hook (synchronous, emulation thread; also speeds up B and synchronous frames) | gain now |
|---|---|---|
| 3D visibility, 3D + backdrop quarters | done (`comp.c`, `compfuse.h`) | 0.3 + 0.4 M (S7) |
| composite, every flag path (closed form 5.6, NEON, one formula a pixel) | `render_scanline_2d_composite` 0x3c6d0 (comp.c's hook: replace the trampoline cases) | T9 1.66 -> ~0.4 M [G]; quarters with translucent 3D 2.6-6.1k -> ~0.6k |
| blended capture (scalar C, 33 instructions a pixel) | `capture_blended` 0x3bc60 / `_3d` 0x3bdb0 (no PC-relative instructions in the first words) | T9 1.33 -> ~0.15 M [G] |
| BG renderers (tbl palettes, per-pixel affine; bg 4.4 proves runs are unnecessary) | the `[L+0x30]` pointers (`update_bg_mode` 0x41b60 replaced) or entry patches | ~2-3x on 0.6-1.3 M [G] |
| OBJ (per-tile NEON, direct mask OR, incremental re-sort) | `render_scanline_obj_c` 0x36b70 (adrp rewrite as in objdiff.c), `reorder_obj` 0x3e530 | 0.65 -> ~0.25 M [G] |
| master-brightness shade | `shade_32_2x` 0xa0d80 | 0.22 M during fades |
| **needs the deferred design:** the line loop and replay, `render_scanline`/`render_scanline_2d` overhead (0.06-0.15 M), every stage's remaining cost, the 3D composite, the 768 KiB present copy, the time spent waiting on B (P7) | | all of engine A |

The stage replacements are the deferred worker's own building blocks. Each one is proven once (3.2) and then used both
in the hook and in the worker.

---

## 3. Exactness strategy

### 3.1 Reference C ports in `src/rast/spec/2d/` (DraStic's data layout, `spec_` names, not fast)

| file | from | status |
|---|---|---|
| `bg.c/.h` | `re/2d/bg/bgspec.c` (448 lines): renderers, edges, mosaic, blank layers | verified: t_bg 4 seeds x ~70k cases; bgdiff 4.96 M real lines, 0 differences |
| `obj.c/.h` | `re/2d/obj/h/obj_model.c` (423 lines): reorder, setup_edges, render | verified: 6000 random frames; in situ on T4/T5/T6/T8/T9 |
| `compose.c/.h` | `re/2d/compose/model/compose_spec.c` (483 lines) + `spec/composite.c`: windows, every composite path, capture, convert | verified: t_compose 3 seeds; compdiff 3.2 M composites |
| `pixel.h` | `re/2d/compose/pix/pixel.h`: the closed form | verified: 461 M pixels against the model; 1.4 M composites against DraStic |
| `line.c/.h` (new) | `render_scanline` (line init, modes 0/1, convert dispatch, capture dispatch) and `render_scanline_2d` (compose 3.1), with a stage table | P2 |

Each port gets a unit test in `tools/rast/ut`, run in DraStic's process (`run.sh`, as `t_composite.c` does):
- random states, from the generators of `t_bg.c`, objharness and `t_compose.c`;
- DraStic's routine and the port run on identical copies;
- every output is compared between guard bytes, and the whole engine struct afterwards;
- three or more seeds, plus a mutation spot check.

`t_line.c` compares DraStic's `render_scanline_2d(eng, planes, line, NULL, hires)` with ours on random engines. It
leaves out the inputs whose result depends on stale stack data (3.6).

### 3.2 Fast versions

Every NEON or fused routine is tested in ut against the C port (many seeds) and against DraStic's original, then
checked in the running emulator with a per-call check mode before it becomes the default.

### 3.3 In-situ check modes (`RAST2D_CHECK`, simulator and device)

- `=call`: per stage, ours and DraStic's run on the same input bytes, comparing everything either writes (the
  bgdiff/objdiff/compdiff pattern, comp.c's `RAST_COMPCHECK`).
- `=line` (P2): our synchronous line engine against DraStic's `render_scanline`, on saved copies of the engine header,
  the OBJ tables, `S` and the output. Compares the two output rows, the engine struct, and the capture's VRAM and
  hi-res writes.
- `=frame` (P3): at 192 the job is snapshotted, then DraStic renders engine A as usual into a shadow screen buffer.
  This advances DraStic's struct; there is no replay-only pass and no write-back. At the join the 384 rows are
  compared, and the job's final eb4/eb6 against DraStic's. `RAST2D_DELAY=<us>` delays the worker so the emulation
  thread runs ahead. The output must not change: this tests that the snapshot is complete and that the gates hold.
- Path counters for the DraStic-specific cases each ROM reaches, so coverage can be read:
  - the general affine path, lagging clip edges, degenerate axes, the null renderer, a NULL ext slot;
  - the OBJ image, the 2x brighten bug, the OBJ window with OBJ display off;
  - reads of VRAM pages without a bank, and synchronous frames by reason.

  Logged as `[2d] check: N frames, M lines differ` every 2 s, with the first differences. On a difference,
  `RAST2D_DUMP` writes that frame's snapshot.

### 3.4 Test content

ds2d (`/home/user/wt-scenes2d/stressrom`, commit ec38b9f, being merged into ROCKNIXDS `stressrom`), every frame a
function of the frame counter:

| scene | covers |
|---|---|
| T0 | text BGs, map sizes, flips, per-line HOFS and backdrop by HDMA (palette events every line), forced blank |
| T1 | ext palettes and slot switching |
| T2 | affine, the general path, mode 6, per-line affine registers and DISPCNT by HDMA |
| T3 | bitmaps, the direct layer, synchronous catch-up via VRAM display, FIFO and HDMA-to-VRAM |
| T4 | all OBJ sizes, 1D/2D, ext palettes, 90 OBJs on a line, OAM rewritten at line 96 (mid-frame re-sort) |
| T5 | affine, bitmap and semi-transparent OBJ, the OBJ image |
| T6 | windows: rectangles, edge cases, OBJ window, per-line WIN0H |
| T7 | blending, brightness, master-brightness fades (2x bug) |
| T8 | mosaic |
| T9 | 3D: HOFS, translucent 3D, capture feedback (synchronous), the image from a capture, VRAM display |

Also: dsscenes S6/S8/S9 (2D over 3D), dsstress L4 (engine B direct bitmap), and games on the device (`=frame` is slow
but works).

Missing content:
- engine B scenes (ds2d leaves B off), needed for P7;
- a vblank VRAM-write stress scene (CPU byte/half/word/`stm` and DMA writes, mapping swaps just after 192), needed for
  P8;
- capture read-back by the CPU.

### 3.5 Frame dumps and offline replay

`RAST2D_DUMP=<dir>` writes every Nth frame (and every differing frame):
- the job's output as `aNNNNN.ppm`;
- in check mode, DraStic's output as `dNNNNN.ppm`;
- `sNNNNN.bin`: the snapshot (header, palette, OAM, log, OBJ tables, VRAM pages with their alias offsets, ext
  palettes, plus a copy of the 3D frame, which is written for the dump only).

A native replay tool (`tools/rast/r2d.c`, x86 host, using the portable C references) renders a blob back into a PPM.
This gives a regression corpus from the ds2d scenes and from games, runnable without qemu.

### 3.6 Cases that cannot be reproduced, and the rules chosen

| DraStic reads stale or garbage data | rule |
|---|---|
| OBJ window mask with OBJ display off (obj 5) | keep the last mask computed in this frame (S persists); empty at the first frame |
| alpha plane under the full-screen OBJ image with a semi-transparent or bitmap OBJ on the line (compose 10.3) | same S persistence; the 3D alpha write of `binary32_alpha` is kept |
| BG line never written (NULL ext slot before any ext bank was mapped; mode-6 BG1/BG3 enabled) | keep the previous line in the slot |
| 1x composite path + capture of the 3D layer + BG0HOFS: q1..q3 read `S+0x400..` | reproduce by keeping DraStic's S layout (they are BG1..BG3 line bytes of the same line) |
| VRAM pages without a bank | zeros (DraStic: its alias's content, open question) |

The check mode flags each of these cases separately, so they cannot hide other differences.

---

## 4. Plan

### 4.1 Phases (estimates in agent-rounds; P1 can start now; P2 and P5 can run in parallel after P1)

| phase | scope | exactness check | estimate |
|---|---|---|---|
| P1 references | Import `bgspec.c`, `obj_model.c`, `compose_spec.c` and `pixel.h` into `src/rast/spec/2d/`. Unit tests `t_bg2d.c`, `t_obj2d.c` (objharness's random states, calling DraStic's functions in process) and `t_compose2d.c` in `tools/rast/ut`. A README section. No runtime change | ut on seeds 1, 7, 23 against DraStic's originals, byte for byte, with guard bytes and the whole struct; one mutation each | 1 |
| P2 line engine, synchronous | `spec/2d/line.c` + `src/rast/d2.c`: our `video_2d_render_scanlines` loop (DraStic's `process_event` for the replay), `render_scanline` (line init, modes 0/1, capture and convert dispatch), `render_scanline_2d` (compose 3.1) on a stage table; `RAST2D=sync` replaces engine A's normal frames on the emulation thread | `t_line.c`; `RAST2D_CHECK=line` on ds2d T0..T9 and cycle, S6/S8/S9, L4: 0 differing lines | 2 |
| P3 deferred engine, simulator | Job struct, small snapshot, VRAM copy behind B, rebased private copy, OBJ table import, replay-only via `[engA+0x38]=NULL`, eb4/eb6 write-back at join, worker thread, deferral rules and `RAST2D_STATS`, join hooks, 3D release gate (`rast.c`, `hr.c`, the shared frame-writer wrappers), simulator present mode, `RAST2D_CHECK=frame`, `RAST2D_DELAY`, `RAST2D_DUMP` | `=frame` with and without delays on every ROM: 0 differing rows; eb4/eb6 equal; savestate save/load, menu and reset round trips with the check on; threaded_3d 0 and 1; `RAST_SCALE` 2 and 3 | 2-3 |
| P4 zero-copy present, device | dsflip API (A's buffer locked at 192, 2D-pending frame, done → commit, pacing sample at completion, shader mode, texture lifetime), `update_screen(A)` skipped, screen buffer refresh at menu/save; performance A/B on RG DS / RG DS Plus (emulation thread CPU, `threads_avg`, fps at 600 MHz, latency) | `=frame` on the device on a game set (the job renders into a private buffer that is compared, then copied); frame dumps | 1-2 + device time |
| P5 fast stages | (a) closed-form composite for every path, plus the fused convert; (b) NEON blended capture; (c) BG text 4/8bpp/ext, per-pixel affine with the exact edges and the general-path bug, bitmaps; (d) OBJ render and incremental re-sort; (e) `shade_32_2x`. Each one both as a standalone hook (2.13) and as a worker stage | per stage: ut against the C port and DraStic's routine; `=call` in situ; then `=frame` | 3-5 (one per item; (a) and (b) first) |
| P6 synchronous frames on our engine | Capture frames, catch-up ranges, display modes 2/3 (the FIFO DMA stays DraStic's call), rendered by our engine on the emulation thread | `=line` on T3 q3, T9 q2/q3 and capture games | 1-2 |
| P7 engine B deferred | B's job (VRAM offsets, no 3D), both panels pending, our frame-end path without `video_render_thread`; an engine-B variant of ds2d | `=frame` for B | 1-2 |
| P8 snapshot cost and deferred capture | Reachable pages from the frame's registers (union over the log); VRAM dirty tracking (DraStic's page trap: `map_memory_page_from_memory_map` 0x13920, `unmap_memory_page_region_direct` 0x13b70, `memory_vram_arm9_get_page_pointer_store` 0x105f0, `dma_transfer`; or mprotect) with a persistent mirror; deferred capture with destination-bank trapping; optionally line bands on two workers | `=frame` + `RAST2D_DELAY` on the VRAM-write stress scene and capture games | 2-3 |

Total: about 13-20 rounds.
- **After P3 + P4**, engine A's whole 2D (1-4 M a frame) is off the emulation thread on every frame without capture.
- **After P7**, so is the wait for B.
- **After P8**, so are capture frames.

### 4.2 Risks

1. **Synchronous classes keep their cost until P6/P8.** That covers capture frames (dual-screen-3D games, motion
   blur), catch-up frames (VRAM display, hblank DMA into VRAM), mid-frame POWCNT1 swaps and FIFO DMA. `RAST2D_STATS`
   on the user's games in P3 ranks them.
2. **Snapshot completeness.** A stage routine that reads live data we did not copy shows up only when the emulation
   thread changes that data during the render. Mitigations: `=frame` with `RAST2D_DELAY`, and in P2 an audit of every
   pointer load of the routines the worker calls. `process_event`'s use of `video` is still unconfirmed (0.3).
3. **Mid-frame changes** are reproduced by construction: registers, palette and OAM through the log; VRAM, VRAMCNT,
   DISPCAPCNT and direct-layer/OBJ-image decisions with DraStic's own once-per-frame semantics. The risk is an input
   outside both, and none is known.
4. **Timing-dependent games.** Emulated timing does not change. Presentation latency stays about the same (the worker
   starts at 192). Capture read-back stays synchronous.
5. **Snapshot bandwidth at 60 fps:** 0.1-0.7 MB a frame (6-40 MB/s), hidden behind B's render. It becomes visible
   once B is deferred (P7), which is what P8's dirty tracking is for.
6. **The JIT or a DMA writing VRAM during the deferred render.** v1 is immune: the copy completes before the
   emulation thread resumes. Once dirty tracking replaces the copy (P8), every write path must be caught: the JIT's
   direct page map, DMA, ARM7-mapped banks, capture. That needs RE plus a stress scene.
7. **Savestates.** Saves are exact after the join's write-back, except the affine counters (as with DraStic's own
   frame skip). Thumbnails need the screen-buffer refresh (2.8).
8. **DraStic's threading of engine B.** B is signalled before our hook, only reads shared data and still blocks the
   emulation thread at 192 until P7. Catch-up frames render A and B on the emulation thread; the join comes first.
9. **3D gate.** With threaded_3d=0 the emulation thread waits at 215 if the worker is behind. The deferred mode
   requires our 3D hooks (`RAST=ours`), and `hr.c`'s 3x loop must gate too.
10. **600 MHz budget.** The worker (DraStic's stage routines, 1-4 M a frame), the 3D bins, the emulation thread and B
    share 4 A55 cores. Contention can lengthen frames. P4 measures it; P5 shrinks the worker.
11. **dsflip.** The pending-frame path is new presenter code: pacing samples, the shader worker, buffer lifetime, the
    menu. Without dsflip the deferred mode gains nothing.
12. **DraStic's garbage reads (3.6)** are accepted with rules and counted separately by the check mode.

### 4.3 Open questions

1. What DraStic's linear alias holds for pages without a bank (frame 15.7): zeros, the scratch page `sys+0xab070`, or
   mirrors?
2. How DraStic traps VRAM writes and re-arms the trap (`map_memory_page_from_memory_map`,
   `unmap_memory_page_region_direct`, the dirty bits `video+0x458878/0x45887c`), and which paths bypass it (DMA fast
   paths, ARM7). P8 depends on this.
3. Which threaded_3d setting the handhelds run: `drastic.cfg.base` has 0, ROCKNIXDS's `drastic-2x-plan.md` recommends
   1. Both are designed for.
4. How often real games hit synchronous frames.
5. Whether dsflip's adaptive latch should sample a deferred frame at completion or at `SDL_RenderPresent`.
6. Whether the stack at `render_scanline_2d`'s depth really holds the previous frame's `S` at frame start. This
   matters only for the garbage cases.
7. The source of the savestate thumbnails (`save_state` copies two 0x18000-byte images) and of the menu background.
8. Whether the worker should stay on its own thread or join a pool with the 3D bin threads; its priority and affinity.
9. Engine B test content: ds2d is engine A only.
10. Whether hires 0 (1x) or 16 bpp deferral is wanted.
11. Whether DraStic's bugs (2x brighten, OBJ order, forced blank ignored) stay forever or get an opt-in "fixed" mode.
12. Small corrections:
    - `update_frame` reads its present-skip option word at `st+0x8a374` (`add x21, x20, #0x88, lsl #12; ldr
      [x21,#0x2374]`). frame.md's `[st+0x882374]` is probably that address.
    - The halfword at `sys+0x1b374` that picks A's screen is presumably POWCNT1.
