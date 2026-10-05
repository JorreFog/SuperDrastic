# DraStic r2.5.2.2: the 2D frame and line model, threading, state and events

> Copied into the repository on 2026-10-05 from the analysis scratch area, which is not kept: `re/drastic.dis` is
> `llvm-objdump -d` of DraStic r2.5.2.2, and `re/...` files not named here (per-function dumps, logs, profiles, frame
> dumps, scripts) were not kept. The other analyses are next to this file (`re/compositing-2x.md` is
> `compositing-2x.md`); the design that builds on them is `../2d-engine.md`.

Key: `frame`. Scope: how a frame of 2D is produced (update_frame, video_render_scanlines, video_2d_render_scanlines,
render_scanline, render_scanline_2d), with what state (the engine struct, VRAM/palette/OAM addressing, the
register-write log), what "catch-up" rendering is, and where the output goes. Sources: `re/drastic.dis` (every
offset below was read there; the function dumps are in `re/2d/dis/*.s`), the ELF for tables (`re/2d/jt.py`), and
`re/compositing-2x.md` for the compositing internals (not repeated here). Every claim not verified in the
disassembly is marked **guess**. No experiment was run for this document; the costs quoted are the ones measured
in `re/compositing-2x.md`.

Register convention: AAPCS64, x0..x7 arguments. `[p+o]` = memory at pointer p plus offset o.

---

## 0. Structures and base pointers

| name | how to get it | what it is |
|---|---|---|
| `st` | `[sys + 0xfba68]` | DraStic's system/emulation state. `[st+0x14]` u16 = **VCOUNT** (current scanline, 0..262). `st+0x855a8` = `cfg`. Event handlers (`event_scanline_start_function`, `event_hblank_start_function`) take `st` in x0. |
| `sys` | `[video + 0]`, `[eng + 0] -> video`, so `[[eng]]` | the memory/core struct: `sys+0x183070` = the ARM9 I/O register file (0x04000000); `sys+0x16070` palette RAM (A 0x400 B + B 0x400 B), `sys+0x16870` its deferred shadow; `sys+0x15070` OAM (A, B), `sys+0x15870` its shadow; `sys+0x15020..0x15060` the 9 VRAM bank allocations A..I; `sys+0xfd500` the linear VRAM alias (see 4.3); `sys+0xfd298` the DMA channel block; `[sys+0xfba70]` = `video`. |
| `video` | `st + 0x36d1ec0`, `[sys+0xfba70]` | the video struct. `[video+0]` = sys, `[video+8]` = cfg. Engine A = `video+0x2e78`, engine B = `video+0x84298` (sizeof engine = **0x81420**). 3D state at `video+0x1056c0`, geometry at `video+0x356cf0`. Frame-level fields at `video+0x4588xx` (section 8). |
| `cfg` | `[video+8]` = `st+0x855a8` | options: `cfg+0x468` threaded_3d, `cfg+0x498` screen swap (which physical screen shows engine A), `cfg+0x4a0` bit 0 = hires (2x) 3D, `cfg+0x4a4` (only read by render_scanline's 3D-only debug path; never in the menu, **guess**: always 0). |
| `eng` | A: `video+0x2e78`, B: `video+0x84298` | the 2D engine struct (section 7). `[eng+0xb7]` = engine index. |
| `C` | `video+0x458820` | the display-capture descriptor (section 8.2); passed as `capture` (x3) to the engine A render calls, NULL for B. |
| `IO` | `sys+0x183070` | raw register file. DISPCAPCNT = `[sys+0x1830d4]`, DISP_MMEM_FIFO = `[sys+0x1830d8]`. The 2D renderers do **not** read the register file; they read the engine struct, which is updated by the event replay (section 3). |

---

## 1. Frame timeline (emulation thread, per scanline)

`event_scanline_start_function(st)` 0x1d960 runs at the start of every scanline. `w20 = VCOUNT+1` is the line
about to start; `[st+0x14]` is updated to it at 0x1db38.

| new line | what happens | where |
|---|---|---|
| 0 (after 262) | `start_frame(video)` 0x30870 (section 1.1), then the "display start" DMAs (mode 3) | 0x1da58 |
| 192 | `update_frame(video)` 0x30dc0 (section 2), then `update_input`, auto-saves, **then** the vblank DMAs (mode 1) at 0x1de3c.. and `update_spu`/audio sync. So the frame is rendered **before** the vblank DMAs run. | 0x1ddd0 |
| 215 | 3D for the next frame: `video_3d_start_rendering` (threaded_3d) or `update_frame_3d` (synchronous). | 0x1d9d4 / 0x1df14 |

`event_hblank_start_function(st)` 0x1d5a0 runs at the hblank of every line. For lines 0..191 it decides the
**catch-up** (section 10):

```
if VCOUNT <= 191:
    if DISPCNT_A bit 17 (display mode 2 or 3)            [st+0x36d4dc8] = engA+0x90      (0x1d65c)
       or game hack byte [st+0x3b2a9a0] != 0  (apply_cycle_adjustment_hacks: set for one game code "B2F.."; 0xfb54)
        -> video_render_scanlines(video, VCOUNT)         render everything up to and including this line now
    for each DMA channel 0..3 (structs at st+0x36d1bf4 + ch*0x28: +0 dest, +4 control, +8 start-mode byte):
        if enabled (control bit 31) and start mode == 2 (hblank):
            if dest in 0x06000000..0x067fffff (VRAM): video_render_scanlines(video, VCOUNT)   (0x1d7bc..)
            dma_transfer(st, ch)
```

Nothing else triggers a mid-frame render: CPU stores to VRAM, palette, OAM and registers never do.

### 1.1 start_frame(video) 0x30870, line 0

1. `remap_palette_oam_deferred(sys)` (section 4.2): palette and OAM accesses by the CPU go through the deferred
   handlers until update_frame.
2. Screen selection: `swap = (cfg+0x498 != 0)`; `set_screen_hires_mode(screen, cfg+0x4a0 & 1)` for both screens;
   the two scanout pointers and pitches via `get_screen_ptr/pitch` 0x8a9c0/0x8aa40.
3. Frame skip: `w26 = [st+0x3b2a9a4]` (byte written by `system_frame_sync` 0xec80; **guess**: the frame-skip
   counter) and bit 2 of the option word `[st+0x882374]` (**guess**: "disable rendering"). Engine A renders iff
   `bit2 == 0 && (w26 == 0 || capture enabled)` (the capture-enabled test is gated by `[st+0x841a54] != 0`, not
   identified); engine B renders iff `w26 == 0 && bit2 == 0`. A rendered engine gets `video_2d_reorder_obj(eng)`,
   `video_2d_map_bg_direct_layers(eng)`, `[eng+0xb8] = 0`, `[eng+0x38] = scanout`, `[eng+0x40] = pitch`. A skipped
   engine gets `[eng+0xb8] = 1`, `[eng+0x38] = NULL` (render_scanline is then never called, but the event replay
   still runs, so the state stays exact).
4. Event queues reset: `[engA+0x81418] = [engA+0x8141c] = 0` (video+0x84290) and the same for B (video+0x1056b0).
5. `[video+0x458894] = 0` (lines rendered so far this frame), `[video+0x458871] u16 = 0`.
6. If DISPCAPCNT `[sys+0x1830d4]` bit 31: fill `C` (section 8.2) from DISPCAPCNT and `C+0x56 = 1`; at 2x,
   allocate (once, `memalign(16, 0x60000)`) the hi-res capture buffer of the destination bank at `C + bank*8`
   and put it in `C+0x30`. Otherwise `C+0x56 = 0`.

### 1.2 Everything DraStic does once per frame, at line 192 (update_frame 0x30dc0)

```
video_render_scanlines(video, 191)                        (section 2)
if C+0x56 (capture was active): C+0x56 = 0; DISPCAPCNT &= 0x7fffffff (one-shot);
     at 2x: mark the written 16 KiB blocks of the destination bank as holding hi-res data:
     C+0x20+bank |= rotate8(((1 << (w*h >> 13)) - 1) << (offset >> 13))   (0x30edc..0x30f48)
     and if the bank's mapping kind [video+0x10+bank*16] == 6: unmap_memory_page_region(..) so CPU writes to it trap
remap_palette_oam_direct(sys)                             CPU accesses palette/OAM directly during vblank
present: update_screen(which) / update_screens()          [engX+0xb8] decides which screens; cfg+0x498 the order
optional FPS text; if threaded_3d and option bit 3 clear: video_3d_finish_rendering(video+0x1056c0)
update_frame_geometry(video+0x356cf0)
```

So the presentation (`update_screens`, the platform flip) reads the scanout buffers **inside update_frame,
right after rendering, on the emulation thread**.

---

## 2. video_render_scanlines(video x0, last_line w1) 0x30be0, and the render thread

```
if [video+0x2220] != 0:                  (VRAM mapping changed since the last render, section 4.3)
    for each region bit i, each page bit j of [video+0x21e0+i*4]:
        remap_address_region_vram(sys, [sys+0xfd500] + (i<<19 | j<<14), [video+0xa0+(i*32+j)*8] - [sys+0xfd4f8], 0x4000)
    [video+0x21e0+i*4] = 0, [video+0x2220] = 0
first = [video+0x458894] (u16)
if last == 191 && first == 0:            THE NORMAL FRAME-END PATH
    lock video+0x4588a0; [video+0x458960] = 1; signal cond video+0x458900; unlock       -> render thread: engine B
    video_2d_render_scanlines(engA, 0, 191, C)                                          emulation thread: engine A
    lock video+0x4588d0; while [video+0x458961] == 0: wait cond video+0x458930; [..961] = 0; unlock
    [video+0x458894] = 192
else:                                    CATCH-UP (or the frame end after a catch-up): both on the emu thread
    video_2d_render_scanlines(engA, first, last, C)
    video_2d_render_scanlines(engB, first, last, NULL)
    [video+0x458894] = last + 1
```

`video_render_thread(video)` 0x2fa50 (created in `initialize_video`, 0x312ac): loop { lock 0x4588a0; wait until
`[+0x960]`; clear it; unlock; `video_2d_render_scanlines(engB, 0, 191, NULL)`; lock 0x4588d0; `[+0x961] = 1`;
signal 0x458930; unlock }.

**What is shared / copied for engine B: nothing is copied.** The render thread reads engine B's struct, its event
queue, VRAM (through the same linear alias), palette and OAM memory, while the emulation thread renders engine A
and then blocks until B is done. Both engines render inside the window where the emulation thread is parked in
update_frame, so there is no concurrent mutation: the "threading" is only the parallelism of A and B. The only
cross-thread writes are the two flags above (mutex/cond protected) and, when engine B is in display mode 3,
`dma_transfer_display` (DMA state) from the render thread. Mid-frame changes are handled entirely by the
register-write log replayed per line (section 3); on a catch-up frame engine B is rendered on the emulation thread
as well (`[+0x894] != 0`).

---

## 3. video_2d_render_scanlines(eng x0, first w1, last w2, capture x3) 0x42a90: the line loop and the event log

```
start = [eng+0x81418]; count = [eng+0x8141c]; ev = eng+0x21418 + start*12; ev[count].line = 0xff (sentinel)
for line = first..last:
    if [eng+0x38] != NULL: render_scanline(eng, [eng+0x38] + line*[eng+0x40], line, capture)
    while ev->line <= line: apply(ev); ev++          (the inlined switch, same as video_2d_process_event)
[eng+0x81418] = count; [eng+0x8141c] = index of ev           (0x42c10)
```

Event entry (12 bytes, array of 0x8000 at `eng+0x21418..0x81418`; `queue_event` prints a message at 0x8000 and
keeps writing): `+0 u32 key`, `+4 u32 value`, `+8 u8 line` (VCOUNT at the time of the write), `+9 u8 size`
(1/2/4 bytes). `video_2d_queue_event(eng x0, key w1, value w2, size w3, line w4)` 0x429f0 appends.

Every render call drains the queue completely: all logged events have `line <= VCOUNT <= last`, so after the call
`start == count == index` (the "stp count, index" at 0x42c10 is consistent only because of this). Events are
applied **after** the line they were logged on: a write during line L first affects line L+1. A write during
vblank (VCOUNT > 191) is not logged but applied immediately by `video_2d_process_event(eng, &event)` 0x420c0
(store_io_register_arm9_16 0x15b14..0x15b48; 8- and 32-bit stores the same).

Keys and their replay (jump table 0x11ddcc, decoded with `re/2d/jt.py 0x11ddcc 0x6d 0x42b7c`). `key & 0xfff` is the
I/O offset (engine B's 0x1000.. offsets share the table); byte writes (size 1) are merged into the stored
register at `(key & 1) * 8` (u32 registers: `(key & 3) * 8`), 4-byte writes set the register pair.

| key | register | engine field written | notes |
|---|---|---|---|
| 0x000 | DISPCNT | `eng+0x90` u32, then `video_2d_set_display_control(eng, v)` 0x41ef0 | section 7.2 |
| 0x004..0x007 | DISPSTAT/VCOUNT | ignored | |
| 0x008/0x00a/0x00c/0x00e | BG0..3CNT | `video_2d_set_bg_control(eng, n, v)` 0x41cb0 | |
| 0x010..0x01e | BGnHOFS/VOFS | `L(n)+0x9a` / `L(n)+0x9c` u16, `& 0x1ff` | `L(n) = eng+0xc0+n*0xb0` |
| 0x020/0x022/0x024/0x026 | BG2PA/PB/PC/PD | `L2+0x9e` / `+0xa2` / `+0xa0` / `+0xa4` s16; `L2+0xae = 1` | note the order: 0x9e PA, 0xa0 PC, 0xa2 PB, 0xa4 PD |
| 0x028 / 0x02c | BG2X / BG2Y | `L2+0x88` and `L2+0x90` = sext28(v); `L2+0x8c` and `L2+0x94`; `L2+0xae = 1` | ref and current counter both reloaded |
| 0x030..0x03c | BG3PA..BG3Y | same at `L3 = eng+0x2d0` | |
| 0x040 / 0x042 | WIN0H / WIN1H | `eng+0xaa` / `eng+0xac` u16; `eng+0xb5 |= 1 / 2` | window dirty bits |
| 0x044 / 0x046 | WIN0V / WIN1V | `eng+0xae` / `eng+0xb0` u16 | |
| 0x048 / 0x04a | WININ / WINOUT | `eng+0x9c` u32 `& 0x3f3f3f3f` | |
| 0x04c | MOSAIC | `eng+0xa8` u16 | |
| 0x050 / 0x052 | BLDCNT / BLDALPHA | `eng+0xa0` / `eng+0xa4` u16 | |
| 0x054 | BLDY | `eng+0xa2` u16 `& 0x1f` | |
| 0x06c | MASTER_BRIGHT | `eng+0xa6` u16 | |
| 0x305 | POWCNT1 high byte | bit 7 (display swap): `[eng+0x38] = [eng+0xb8] ? NULL : get_screen_ptr(screen)`, `[eng+0x40] = get_screen_pitch(screen)`, `screen = (bit7 || cfg+0x498) ? index : index^1` | same code as `video_2d_reload_screen_ptr(eng, v)` 0x42040 (which has no callers in the binary) |
| 0x100000 + a | palette byte/u16/u32 at a (`a & 0x3ff`) | `[eng+0x18] + a` | from store_palette_deferred{8,16,32} 0x10ca0.. |
| 0x200000 + a | OAM at a (`a & 0x3ff`) | `[eng+0x30] + a`; `eng+0xb6 = 1` (OAM dirty) | from store_oam_deferred{8,16,32} 0x10e20.. |

Not logged (so not line-exact in DraStic): VRAM contents and VRAMCNT, DISP3DCNT and the 3D state, DISPCAPCNT
(read at start_frame only), BG extended palettes (they live in VRAM).

---

## 4. How writes reach the renderer

### 4.1 I/O registers
`store_io_register_arm9_{8,16,32}` 0x149d0/0x155b0/0x16060: for offsets 0x000-0x003, 0x008-0x055, 0x06c-0x06f
(and 0x1000+ for engine B): `VCOUNT <= 191 ? video_2d_queue_event(eng, off, v, size, VCOUNT) :
video_2d_process_event(eng, {off, v, size})`. The raw register file at `sys+0x183070` is written as well (the
CPU reads it back from there).

### 4.2 Palette and OAM: two copies and a mode switch
Memory regions 0x05000000 (table `sys+0xfbe58`) and 0x07000000 (`sys+0xfbeb8`): `+0` mask 0x7ff, `+8` data
pointer, `+0x20..` load handlers, `+0x38..` store handlers.
- **direct mode** (`remap_palette_oam_direct(sys)` 0x1b6d0, called by update_frame at line 192): data pointer =
  the real arrays `sys+0x16070` / `sys+0x15070`, store handlers NULL (plain stores). The renderers' pointers
  `[eng+0x18]` (`[video+0x2e50 + 8*idx]` = sys+0x16070 / 0x16470) and `[eng+0x30]` (`[video+0x2e60 + 8*idx]` =
  sys+0x15070 / 0x15470) point into the same arrays, so vblank writes are visible to the renderer immediately.
- **deferred mode** (`remap_palette_oam_deferred(sys)` 0x1b520, called by start_frame at line 0, and by
  reset_memory): installs the `*_deferred_first*` handlers. The first CPU access of the frame copies the real
  array into the shadow (`sys+0x16870` / `sys+0x15870`, 0x800 B each: `memcpy` at 0x1272c), points the region at
  the shadow and installs the regular deferred handlers. A store compares the byte with the shadow, and if it
  differs queues an event `(0x100000|a or 0x200000|a, value, size, VCOUNT)` for engine `a & 0x400 ? B : A` and
  updates the shadow (0x10cb8..0x10d04). The real array, which the renderer reads, changes only when the event
  is replayed after the line it was written on.
- DMA to palette/OAM goes through the same region handlers (**guess**: dma_transfer uses the region tables).

### 4.3 VRAM: per-page pointers for the CPU, a linear alias for the renderer
- Bank memory: `[video+0x20a0 + bank*8]` = `[sys+0x15020 + bank*8]`, banks A..I (sizes per the DS: 128K x4, 64K,
  16K, 16K, 32K, 16K; **guess** on the order A..I). The file-backed allocation makes aliasing possible: the
  renderer's view `[sys+0xfd500]` is a 16 MiB linear image of 0x06000000.., remapped page by page with
  `munmap + mmap(MAP_SHARED|MAP_FIXED, fd=[sys+0xfd508], offset)` in `remap_address_region_vram` 0x13270 (when
  `[sys+0xfd513] == 0`; otherwise another method, not read).
- The CPU's view: `memory_vram_arm9_get_page_pointer_store(sys, addr)` 0x105f0 returns
  `[video+0xa0 + page*8] + (addr & 0xffc000)`, page = `(addr >> 14) & 0x3ff` (1024 pages of 16 KiB covering the
  16 MiB region; `video+0xa0..0x20a0`). An unmapped page points at a scratch page (`sys+0xab070`).
  `[video+0x2224 + page*2]` u16 = banks mapped at that page, `[video+0x2a24 + page]` u8 = `bank*8 + block`
  of the first mapping (0x1f = none, **guess**). A store also sets the 16 KiB-block dirty bits
  `[video+0x458878]` (banks A-D, bit `bank*8+block`) / `[video+0x45887c]` (E..) and clears the hi-res-capture
  validity bit `C+0x20+bank &= ~(1 << block)` (0x10678).
- VRAMCNT writes call `map_vram_page_region_arm9` 0x2f940 / `unmap_vram_page_region_arm9` 0x2f840: they rewrite
  `[video+0xa0+page*8]`, `[video+0x2224..]`, `[video+0x2a24..]`, change the CPU's direct page mapping
  immediately (`unmap_memory_page_region_direct`), and for pages < 0x200 (the first 8 MiB: BG/OBJ of both
  engines) set `[video+0x21e0 + (page>>5)*4] |= bits`, `[video+0x2220] |= 1 << (page>>5)`. The renderer's linear
  alias is updated from these masks **at the start of the next video_render_scanlines call** (0x30c60..0x30dbc),
  i.e. at the next catch-up or at line 192.
- The renderers address VRAM as `[eng+0x8] + offset`, `[eng+0x8] = [sys+0xfd500]` (initialize_video_2d 0x43678),
  with engine B's offsets carrying +0x200000 (BG) / +0x600000 (OBJ): `[eng+0x94] = [eng+0x98] = idx << 21`
  (0x43688), BG2/3 map bases get `+0x200000` in set_bg_control (0x41da4), OBJ base 0x400000 / 0x600000
  (reorder_obj 0x3e5ac/0x3e5c4), so engine A BG = alias+0, A OBJ = alias+0x400000, B BG = +0x200000, B OBJ =
  +0x600000: the DS address minus 0x06000000. **Consequence:** VRAM *contents* written by the CPU at any time are
  seen by the renderer the moment it reads them (shared pages); VRAM *mapping* changes are seen at the next
  render call. There is no VRAM snapshot anywhere in DraStic.
- Extended palettes: `[eng+0x20]` = `video+0x2130 + idx*0x20`, a table of 4 pointers (BG ext palette slots 0..3,
  8 KiB each) into bank memory, set by the VRAM mapping code (**guess**: by map_vram_page_region_arm9 for banks
  E/F/G with MST 4/5); `[L(n)+0x18]` = the layer's slot pointer (`slot = n + 2*BGnCNT.13` for BG0/1,
  set_bg_control 0x41e44; BG2/3 **guess**: fixed slots 2/3 set in the same mapping code). `[eng+0x28]` = OBJ
  extended palette pointer (read by reorder_obj when DISPCNT.31).

---

## 5. render_scanline(eng x0, out x1, line w2, capture x3) 0x404a0: one DS line of one engine

Stack frame 0x1aa0; `planes = sp+0x290` (4 quarter plane sets of 0x300 B: R6[256] G6[256] B6[256], u8).
`bpp = get_screen_bytes_per_pixel()`, `hires = cfg+0x4a0 & 1`, `cap = capture && C+0x51 != 0`.

1. Engine A with `cfg+0x4a4 != 0`: 3D only (`render_scanline_3d`, converted straight to XRGB), return.
2. **Main-memory display FIFO**: for each DMA channel (sys+0xfd2a8 + ch*0x28: `+0x20` control, `+0x24` start-mode
   byte) with enable (bit 31) and start mode 4 (**guess**: DraStic's code for "main memory display"):
   `fifo = dma_transfer_display(sys+0xfd298, ch, line)` 0x1f190 = pointer to main memory at
   `src + line*512` (the region table at `[dma+8]`, 0x60 B entries by addr>>23; a handler region returns a
   computed pointer, an unmapped one NULL). On line 191, channels without repeat (bit 25) are disabled and the
   register copy `[[ch+0x10]+8]` cleared; bit 30 raises the DMA IRQ.
3. **Line 0 initialisation** (0x40574): window vertical state `eng+0xb4` bit0 (WIN0) / bit1 (WIN1): set if
   `Y1 > 191`, then cleared if `Y2 > 191` (`WIN0V = [eng+0xae]`: Y1 = high byte, Y2 = low byte); affine counters
   reloaded: `L2+0x90..0x94 = L2+0x88..0x8c`, `L3` likewise; `L2+0xae = L3+0xae = 1`.
   **Lines 1..191** (0x40910): `L2.cur += (L2.PB, L2.PD)` (`+0x90 += s16[+0xa2]`, `+0x94 += s16[+0xa4]`), same
   for L3. (The per-line window Y1/Y2 toggling itself happens in render_scanline_generate_window_masks, not
   analysed here.)
4. `mode = DISPCNT[17:16]` (`[eng+0x90]`). If `cap`: source B: `C+0x52 == 3` (VRAM): `C+0x40 = [eng+0x10] +
   line*512` if the display block's bank kind `[video+0x10+blk*16] == 6` else NULL; `== 4` (FIFO): `C+0x40 =
   fifo` (if the FIFO is not running: a 0x200-B line of the DISP_MMEM_FIFO register value, 0x411d4); source B
   not blended and `line < C+0x50`: `memcpy([C+0x38], fifo, width*2)` (0x410b4, not analysed further).
5. Per mode:
   - **mode 0 (off)**: `render_scanline_2d(..)` is still called when `cap` (0x40648; the 2D render feeds the
     capture), then `memset(planes, 0xff, 0x300)` (white).
   - **mode 1 (graphics)**: `r = render_scanline_2d(eng, planes, line, capture, hires)`; r != 0 means four 2x
     quarters were produced (q0..q3 = planes + q*0x300); r == 0: all four convert inputs = planes.
   - **mode 2 (VRAM display)**: `expand_6bit_split(planes, [eng+0x10] + line*512)` where `[eng+0x10] =
     [video+0x20a0 + 8*DISPCNT[19:18]]` (the LCDC bank, set by set_display_control; the u16 BGR555 line is read
     straight from bank memory). With capture (0x40e28): if the bank's hi-res validity bit for the 32-line group
     (`C+0x20+blk` bit `line>>5`) is set, q1..q3 come from the bank's hi-res buffer (`[C+blk*8] + line*0x600`,
     three 256-u16 quarters), and `render_scanline_2d(eng, planes+0xc00, ..)` renders the 2D for source A.
   - **mode 3 (main-memory FIFO)**: `expand_6bit_split(planes, fifo)`; no FIFO: constant fill from the
     DISP_MMEM_FIFO register `[sys+0x1830d8]` (0x40ca8). With capture, render_scanline_2d first.
6. **Convert** (0x4067c): `MB = [eng+0xa6]`, `m = MB >> 14`, `f = (MB & 0x1f) * 2`. 32 bpp, 2x
   (`[eng+0x40]` pitch; row 2 at `out + pitch/2`): m 0 or f 0: `color_convert_direct_32_2x(q0, q1, out)` and
   `(q2, q3, out+pitch/2)`; m 1 (brighten): `shade_32_2x(.., 32-f, 63*f+16)`; m 2 (darken): `shade_32_2x(.., 32-f,
   16)`; f > 31: memset 0xff / 0x00 over 0x800 B per row. 1x and 16 bpp variants: 0x409b4, 0x40eb0, 0x40d04
   (inline NEON to RGB565). DraStic **ignores DISPCNT.7 (forced blank)**: no test of that bit exists in
   render_scanline, render_scanline_2d, set_display_control or the event handlers (python scan of all `tbz/tbnz
   #0x7` in 0x31800-0x44000: they test BGnCNT.7 or POWCNT).

---

## 6. render_scanline_2d(eng x0, planes x1, line w2, capture x3, hires w4) 0x3ef00 -> w0

The compositing internals (visibility bitmaps, priority encode, select, 2x quarters) are in
`re/compositing-2x.md` 1.3-1.5. What matters for the frame model:

1. `if [eng+0xb6]` (OAM changed by a replayed event): `video_2d_reorder_obj(eng)`, `[eng+0xb6] = 0` (0x40078).
   So OAM changes take effect on the next line, through a full OBJ re-sort.
2. Per-line inputs read from the engine: `DISPCNT`, `BLDCNT` (`eng+0xa0`), `BLDY`, `BG0HOFS` (sign-extended 9
   bits, for the 3D layer shift), the per-line OBJ flag byte `[eng+0x21340 + line]` (bit 0: a semi-transparent
   OBJ touches this line; written by reorder_obj), the OBJ full-screen image `[eng+0x21400]` (+ its 2x data
   `[eng+0x21408]`, priority `[eng+0x21410]`), the direct bitmap layers `[eng+0x240]`/`[eng+0x2f0]` (+ 2x data
   `[eng+0x248]`/`[eng+0x2f8]`), the window state `eng+0xaa..0xb5`.
3. 3D line: if DISPCNT.3 and BG0 enabled, or (capture && `C+0x51 == 2`): `p3d = render_scanline_3d(video, line)`
   0x59950 = `[video+0x34eb58]` (threaded_3d: `[video+0x34eb60]`, the buffer published by
   video_3d_finish_rendering) `+ line*0x1000` at 2x, `+ line*0x400` at 1x (hires == 2: a 1x downsample into the
   static buffer `[0x15ff28]`).
4. `render_scanline_bg(eng, S+0x1e0, S+0xda0, line)` 0x36880: for each BG in the 2D order list `eng+0x8c[0..
   eng+0xb2)` whose `L+0x20` (direct pointer) is NULL: mosaic adjust (MOSAIC `eng+0xa8`: V = bits 4-7, line %
   (V+1) subtracted from VOFS / from the affine counter), call `[L+0x30](L, line_buf = S+0x1e0+n*0x220+0x10,
   vis = S+0xda0+n*32, line)`, H mosaic on the result. The layer render functions (section 7.2) read VRAM through
   `[L+8] = [eng+8]`, the palette `[eng+0x18]`, the ext palette `[L+0x18]`.
5. OBJ (`render_scanline_obj_c` 0x36b70) when DISPCNT.12; windows; then the 1x or the 4-quarter 2x path; then
   the capture (step 7).
6. Composite call: `render_scanline_2d_composite(eng, planes[+q*0x300], S, layers[5], p3d, alpha, enabled_mask,
   BLDCNT & (0xf0f0 | en | en<<8), flags, line)`.
7. **Capture** (0x3f6a0..): if `C+0x51 && line < C+0x50`: `dst = C+0x28 + ((C+0x48 + C+0x4c*line) & 0xffff)*2`
   (u16 in the destination bank), `srcB = C+0x40`; at 2x the hi-res line is `C+0x30 + ((..)&0xffff)*6` and holds
   quarters q1..q3 (256 u16 each) while q0 goes to VRAM. Per quarter: source A = 2D: `C+0x53 ?
   capture_blended(C, dst, srcB, planes_q) : capture_direct_asm(C, dst, planes_q)`; source A = 3D: the `_3d`
   variants with `p3d + q*0x400`. `capture_direct`: `dst[i] = 0x8000 | (R6>>1) | (G6>>1)<<5 | (B6>>1)<<10` for i <
   width; `capture_direct_3d`: `(a != 0) << 15 | r6>>1 | ...`; `capture_blended`: `((A*EVA + B*EVB) >> 5)` per
   channel, saturated to 31, bit 15 set, with `EVA = C+0x54`, `EVB = C+0x55` (0..16), and `EVA == 16 && EVB == 0`
   short-cut to direct (0x3bc68). Width/height from `C+0x4c`/`C+0x50` (tables 0x11dc80: {128,256,256,256},
   0x11dc90: {128,64,128,192}).
   Return value: non-zero (the hi-res layer mask) iff the 4-quarter path ran.

---

## 7. The engine struct (`eng`, 0x81420 bytes)

"P" = primary state (a register or memory the game controls), "D" = derived by DraStic from P (recomputable),
"S" = saved by video_2d_store_savestate 0x43ba0 (the fields DraStic itself considers state).

### 7.1 Engine level

| offset | size | P/D | contents |
|---|---|---|---|
| 0x00 | 8 | D | `video` |
| 0x08 | 8 | D | VRAM linear alias base `[sys+0xfd500]` |
| 0x10 | 8 | D | VRAM-display bank pointer (`[video+0x20a0 + 8*DISPCNT[19:18]]`), set by set_display_control / load_savestate |
| 0x18 | 8 | D | palette pointer (A: sys+0x16070, B: sys+0x16470; 0x400 B: BG 0x200 + OBJ 0x200). Backdrop = `*(u16*)[eng+0x18]` |
| 0x20 | 8 | D | pointer to the 4 BG ext-palette slot pointers (`video+0x2130 + idx*0x20`) |
| 0x28 | 8 | D | OBJ ext-palette pointer (8 KiB in VRAM) |
| 0x30 | 8 | D | OAM pointer (A: sys+0x15070, B: sys+0x15470; 0x400 B) |
| 0x38 | 8 | D | scanout pointer for this frame (NULL = engine skipped), `+0x40` u32 pitch per DS line (0x1000 at 2x/32bpp) |
| 0x84 | 8 | D | composite order list: for priority p = 0..3: `4|p` (OBJ slot, if DISPCNT.12) then the enabled BGs of priority p in BG order; count at `0xb3`. From video_2d_reorder_layers 0x41300 |
| 0x8c | 4 | D | 2D BG render list (same order, BGs only, excluding BG0 when DISPCNT.3); count at `0xb2` |
| 0x90 | 4 | P,S | DISPCNT (engine B: `& 0xc0b1fff7`) |
| 0x94 / 0x98 | 4+4 | D | engine A: DISPCNT char base `[29:27]<<16`, screen base `[26:24]<<16`; engine B: 0x200000 both |
| 0x9c | 4 | P,S | WININ (low 16) / WINOUT (high 16), `& 0x3f3f3f3f` |
| 0xa0 / 0xa2 / 0xa4 / 0xa6 / 0xa8 | 2 each | P,S | BLDCNT, BLDY (&0x1f), BLDALPHA, MASTER_BRIGHT, MOSAIC |
| 0xaa / 0xac / 0xae / 0xb0 | 2 each | P,S | WIN0H, WIN1H, WIN0V, WIN1V |
| 0xb2 / 0xb3 | 1+1 | D | counts of the two lists above |
| 0xb4 | 1 | D,S | window vertical state: bit0 WIN0 active, bit1 WIN1 active (per line state machine; initialised at line 0) |
| 0xb5 | 1 | D | window masks dirty (bit0 WIN0, bit1 WIN1); set to 3 by load_savestate |
| 0xb6 | 1 | D | OAM dirty (set by an OAM event replay; consumed by render_scanline_2d) |
| 0xb7 | 1 | D | engine index (0 = A) |
| 0xb8 | 1 | D | 1 = this engine is not rendered this frame (frame skip) |
| 0xc0 / 0x170 / 0x220 / 0x2d0 | 0xb0 each | | BG0..BG3 layer structs `L(n)` (7.2) |
| 0x380.. | | D | OBJ tables written by video_2d_reorder_obj (sorted per-OBJ entries from `eng+0x3b0`, stride 0x30 (**guess** on the stride); `eng+0x20f80..0x21340` zeroed per frame (0x3c0 B, **guess**: per-line OBJ counts); `eng+0x21340..0x21400` per-line flag bytes (bit0: semi-transparent OBJ on the line) |
| 0x21400 / 0x21408 / 0x21410 | 8+8+1 | D | OBJ full-screen bitmap image: pointer into VRAM (12 64x64 bitmap sprites tiling the screen, same priority), its hi-res capture data, its priority |
| 0x21418 | 0x60000 | P | the event log, 0x8000 x 12 B (section 3) |
| 0x81418 / 0x8141c | 4+4 | P | event log start index / count |

### 7.2 Layer struct `L(n) = eng + 0xc0 + n*0xb0`

| offset | size | P/D | contents |
|---|---|---|---|
| 0x00 / 0x08 | 8+8 | D | `eng`, VRAM alias base |
| 0x18 | 8 | D | BG extended palette pointer for this layer |
| 0x20 | 8 | D | direct line source (u16 BGR555 rows of 512 B in VRAM) when the layer is a 256x256 16-bit bitmap with identity affine (BGnCNT & 0xc0fc == 0x4084, X = Y = 0, PA = PD = 0x100, PB = PC = 0; map_bg_direct_layers 0x41700, called at start_frame only); else NULL. `+0x28` its hi-res capture data (3 x 256 u16 per line) or NULL (only BG2 at `eng+0x240/0x248`, BG3 at `eng+0x2f0/0x2f8`) |
| 0x30 | 8 | D | render function: tiled `render_scanline_tiled_ext` 0xa1d20 (BG0/1 always; BG2 modes 0,1,3; BG3 mode 0), `render_scanline_affine_normal_ext` 0xa4730 (BG2 modes 2,4; BG3 modes 1,2), extended (`BGnCNT.7 ? (BGnCNT.2 ? render_scanline_bitmap_16bpp 0x32880 : bitmap_8bpp 0x33700) : render_scanline_affine_extended_ext 0xa4b90`; BG2 mode 5, BG3 modes 3,4,5), mode 6: BG1 and BG3 `render_scanline_null`, BG2 bitmap_8bpp (large). From video_2d_update_bg_mode 0x41b60 |
| 0x38 / 0x3c | 4+4 | D | absolute character base / screen (map) base in the VRAM address space: `(+0x44, +0x48) + (eng+0x94, eng+0x98)` |
| 0x40 | 4 | D,S | map/bitmap base for BG2/3: `BGnCNT[12:8] << 14` (+0x200000 on B); 0 in mode 6 |
| 0x44 / 0x48 | 4+4 | D,S | char base `BGnCNT[5:2] << 14`, screen base `BGnCNT[12:8] << 11` (raw, before the DISPCNT bases) |
| 0x88 / 0x8c | 4+4 | P,S | BGnX / BGnY reference (sext 28) |
| 0x90 / 0x94 | 4+4 | P,S | the running affine counters (reloaded at line 0 and on a ref write; += PB,PD per line in render_scanline; temporarily adjusted for mosaic) |
| 0x98 | 2 | P,S | BGnCNT |
| 0x9a / 0x9c | 2+2 | P,S | HOFS / VOFS (& 0x1ff) |
| 0x9e / 0xa0 / 0xa2 / 0xa4 | 2 each | P,S | PA, PC, PB, PD (s16) |
| 0xa6 / 0xa8 | 2+2 | D,S | width mask / height mask in pixels (text and bitmap sizes; mode 6: 0x3ff/0x1ff or 0x1ff/0x3ff) |
| 0xaa / 0xab / 0xac | 1 each | D,S | log2 width, tile-count mask, log2 width in tiles (affine) |
| 0xad | 1 | D,S | DISPCNT.30 (extended palettes enabled) |
| 0xae | 1 | D | affine parameters changed (consumed by the affine renderers; set by PA..Y events, line 0, load) |

Trigger map (what recomputes what): DISPCNT write -> `set_display_control`: ext-pal flags, bases (A), `[eng+0x10]`;
mode change -> `update_bg_mode` (+ `set_bg_control(2)` into/out of mode 6); bits 3, 8-12 change ->
`reorder_layers`. BGnCNT write -> `set_bg_control`: raw bases, `+0x38`; priority bits change -> `reorder_layers`;
bits 2/7 change -> `update_bg_mode`; sizes. **map_bg_direct_layers and reorder_obj are per frame** (start_frame),
plus reorder_obj on OAM dirty per line; a mid-frame BGnCNT/affine change that would turn a layer into or out of a
"direct" layer is therefore not applied until the next frame (DraStic inexactness to reproduce).

---

## 8. Frame-level fields of `video`

### 8.1 VRAM mapping and bank state
`video+0x10 + bank*16`: per-bank control (`+0` kind: 6 = the kind map_bg_direct_layers and update_frame test, **guess**
"mapped as engine A BG with offset"; `+8` offset in 16 KiB units: compared with `base & 0xfffe0000` / `<< 14`).
`video+0xa0 .. 0x20a0`: 1024 CPU page pointers (4.3). `video+0x20a0..0x20e8`: bank base pointers A..I;
`+0x20e8..0x2128`: dummies for unmapped slots (sys+0x1b2b0+i). `video+0x2130/0x2150`: ext-palette slot tables.
`video+0x21e0 + i*4`, `video+0x2220`: pending alias remaps. `video+0x2224 + page*2`, `+0x2a24 + page`: page
membership. `video+0x2e50..0x2e68`: palette A, palette B, OAM A, OAM B pointers. `video+0x2e70` u16: VRAM dirty
flags (**guess**: texture cache).

### 8.2 Capture descriptor `C = video+0x458820`

| offset | contents |
|---|---|
| +0x00..0x1f | hi-res capture buffer per destination bank A..D (0x60000 B each, lazily allocated); 0x600 B per line = quarters q1..q3 |
| +0x20..0x23 | per bank: bit b = 16 KiB block b holds valid hi-res data (set by update_frame after a capture, cleared by CPU stores, zeroed by video_load_savestate) |
| +0x28 | destination bank base pointer; `+0x30` the hi-res buffer of this capture |
| +0x38 | line buffer used by the FIFO-source path (not analysed) |
| +0x40 | source B line pointer, set per line by render_scanline |
| +0x48 | write offset in pixels (`DISPCAPCNT[19:18] << 14`); `+0x4c` u16 width; `+0x4e` dest bank; `+0x4f` DISPCNT VRAM display block; `+0x50` height |
| +0x51 | source A kind: 0 none, 1 graphics, 2 3D (`DISPCAPCNT.24 + 1` when source select != 1) |
| +0x52 | source B kind: 3 VRAM, 4 FIFO (`DISPCAPCNT.25 ? 4 : 3`, only when source select == 1) |
| +0x53 | blend (source select 2/3); `+0x54` EVA = min(16, bits 4:0); `+0x55` EVB = min(16, bits 12:8) |
| +0x56 | capture active this frame |
| video+0x458878 / +0x45887c | 16 KiB-block "written" dirty bits, banks A-D / E-I |
| video+0x458894 | u16 lines rendered this frame (catch-up position) |
| video+0x458898.. | render thread: pthread_t +0x898, mutexes +0x8a0/+0x8d0, conds +0x900/+0x930, flags +0x960 (B go) / +0x961 (B done) |

### 8.3 Scanout
`get_screen_ptr(screen)` 0x8a9c0: the platform screen table `[0x15fef8]`, entries of 0x28 B (`+8` buffer, `+0x20`
enabled, `+0x21` hires mode), `screen ^= [tab+0xac]` (swap). `get_screen_pitch` = `(3*hires + 1) * bpp * 256` =
0x1000 per DS line at 2x/32 bpp; DS line L of engine X is written at `[engX+0x38] + L*0x1000` (row 2L) and
`+0x800` (row 2L+1), 512 px x 4 B each, XRGB8888 (`c6 << 2` per channel, byte 3 = 0xff/0x00 pattern of
compositing-2x.md 2). At 1x: one row of 256 px (or 512 B at 16 bpp). The scanout is a DRM dumb buffer under
dsflip (from compositing-2x.md, not re-verified here): never read it back.

---

## 9. VRAM/palette/OAM addressing summary for a replacement

- BG/OBJ data, bitmap BGs, VRAM display, the direct layers, the ext palettes: flat reads from the linear alias
  (`[eng+8] + ds_address - 0x06000000`), with the mapping as of the last render call. Unmapped pages read as
  whatever the alias holds (**guess**: the dummy page, i.e. zeros or stale data).
- Palette: `[eng+0x18]` (0x400 B); backdrop = entry 0. OAM: `[eng+0x30]` (0x400 B). Both are only ever modified by
  the event replay during lines 0..191 and directly by the CPU during vblank.
- All I/O-register state: the engine struct fields of section 7, modified only by the replay (lines 0..191) or by
  process_event (vblank).

---

## 10. Catch-up and mid-frame change semantics (deliverable 2)

1. **Normal frame**: nothing is rendered before line 192. At line 192 all 192 lines of A and of B are rendered in
   one burst, line by line, each line followed by the replay of the register/palette/OAM writes logged on that
   line. The VRAM contents and mapping used are those at line 192 (minus nothing: the vblank DMAs run after
   update_frame). Lines already rendered are never re-rendered.
2. **Catch-up frame**: at the hblank of line L, if engine A is in display mode 2/3, or an hblank DMA writes VRAM
   (checked before each such DMA), or the per-game hack byte is set: lines `[video+0x458894] .. L` are rendered
   now (A then B, both on the emulation thread, with the events of those lines replayed), `[+0x894] = L+1`. At
   line 192 the remaining lines `[+0x894] .. 191` are rendered the same way (B also on the emulation thread). The
   lines rendered early used the VRAM contents/mapping at that hblank; later lines see later VRAM. Lines already
   rendered are untouched by later VRAM writes. A mode 2/3 engine A therefore renders every line at its hblank.
3. **Registers** (DISPCNT, BGxCNT, offsets, affine, windows, blend, mosaic, master brightness, POWCNT1 swap):
   line-exact by the log; a write during line L takes effect from line L+1. Vblank writes take effect at once
   (they are the line-0 state of the next frame). The derived state follows automatically through
   set_display_control/set_bg_control/update_bg_mode/reorder_layers at replay time.
4. **Palette / OAM**: line-exact the same way (byte-granular, only changed bytes logged). OAM changes trigger a
   full OBJ re-sort before the next line.
5. **Affine counters**: reloaded from the references at line 0 and whenever BGxX/Y is written; stepped by PB/PD
   at the start of lines 1..191 with the PB/PD values current at that time.
6. **Window Y state**: initialised at line 0 from WIN0V/WIN1V as in 5.3.
7. **Not line-exact in DraStic** (do not "fix" in an exact replacement): VRAM contents written by the CPU
   mid-frame (visible to all not-yet-rendered lines, including earlier-in-frame writes), VRAMCNT changes
   (applied at the next render call), direct-layer detection and the OBJ full-screen image (per frame),
   DISPCAPCNT (per frame), DISPCNT.7 forced blank (ignored), BG ext palettes (VRAM, so as VRAM).

---

## 11. Output (deliverable 3)

- Engine X, DS line L: rows 2L and 2L+1 at `[engX+0x38] + L*0x1000` and `+0x800`, 512 XRGB8888 pixels each.
  Both engines write their own buffer (`get_screen_ptr(screen)` with the swap applied; the lower screen gets the
  other one). The buffer pointer is fetched at start_frame (and re-fetched on a POWCNT1 swap event).
- The presentation reads the buffers in update_frame right after rendering (`update_screen` / `update_screens`,
  emulation thread, line 192 of every frame). A deferred renderer that is not finished by then must also defer
  the flip (one frame of latency) or block there.
- Capture: VRAM (u16, written during rendering of each captured line, destination bank + offset + width*line,
  wrapping at 64 K pixels) plus at 2x the per-bank hi-res buffer (q1..q3 of each line) and the validity bits
  `C+0x20+bank` (set after the frame). The CPU can read the captured VRAM as soon as update_frame returns, so a
  deferred engine must either complete the capture before the end of update_frame or render capture frames
  synchronously.
- Master brightness and the 3D-only path are applied in the convert step, per line, from `[eng+0xa6]` at that
  line.

---

## 12. The snapshot a deferred engine A needs (deliverable 1)

The plan that reproduces DraStic bit for bit: at line 192 (our hook on `video_render_scanlines` or on
`video_2d_render_scanlines` for engine A), copy the inputs, let the emulation thread continue, and run DraStic's
exact algorithm (line loop + event replay) on the copies on another core. The emulation thread must still apply
the events to DraStic's own engine struct/palette/OAM (the cheap part: the switch of section 3) so that vblank
writes and the next frame start from the right state.

| item | size | when it can change | notes |
|---|---|---|---|
| engine struct `eng+0x00..0x380` (registers, derived fields, lists, dirty bits) | 0x380 B | per line (replay) and vblank (process_event) | copy before the replay; the derived fields are recomputed by the replay, so the copy must include them |
| OBJ tables `eng+0x380..0x21418` | 0x21098 B | per frame (start_frame) and per line on OAM dirty | or recompute with reorder_obj on the copy: it needs only OAM, DISPCNT, VRAM (ext pal ptr) |
| event log `eng+0x21418 + start*12 .. count*12` | 12 B x events (typically tens to thousands) | appended during lines 0..191 | the queue is frozen after line 191: nothing is appended until the next frame (vblank writes go to process_event); start_frame resets it at line 0, so it must be copied before then |
| palette `[eng+0x18]` | 0x400 B | per line (replay), vblank (direct CPU writes) | copy at 192 + replay the palette events into the copy |
| OAM `[eng+0x30]` | 0x400 B | same | same |
| BG ext palette slots `[[eng+0x20]+8*s]`, OBJ ext palette `[eng+0x28]` | 4 x 8 KiB + 8 KiB | VRAM: any time the CPU writes | they are VRAM bank E/F/G contents |
| VRAM reachable by engine A | up to 512 KiB BG + 256 KiB OBJ (only the mapped 16 KiB pages: at most 656 KiB of bank memory in total) | any time the CPU/DMA writes (vblank uploads are the norm) | **the central cost**: either copy the mapped pages at 192 (<= 41 pages x 16 KiB), or protect them (mprotect + copy-on-write) while the deferred render runs. The mapping itself: the page table `video+0xa0..0x20a0` (8 KiB) or just the set of mapped pages |
| VRAM mapping (`video+0x21e0/0x2220` pending remaps already applied by our hook) | | at VRAMCNT writes, applied at render time | apply the pending remaps first, as DraStic does, then snapshot |
| the 3D frame `[video+0x34eb58]` / `[+0x34eb60]` | 0xc0000 B at 2x | overwritten at line 215 (threaded_3d = 0: the same buffer; threaded_3d = 1: the published buffer stays until the next finish at line 192) | with threaded_3d = 0 the deferred composite must finish before line 215, or copy the frame (0.75 MB), or hold update_frame_3d |
| capture descriptor `C` (0x58 B), the hi-res capture buffers and validity bits it points to | 0x58 B + 0x60000 B per bank | start_frame, update_frame, CPU VRAM stores (validity bits) | capture frames: render synchronously (see 11) |
| scanout pointer and pitch `[eng+0x38]`, `[eng+0x40]` | 12 B | start_frame, POWCNT1 event | |
| `cfg+0x4a0` hires, `cfg+0x468` threaded_3d, bpp | | menu | |
| the DMA display FIFO (mode 3) | 512 B per line | per line | mode 3 forces per-line catch-up anyway: render synchronously |
| window/affine per-line state (`eng+0xb4`, `L+0x90/0x94`, `L+0xae`) | in the struct | per line | reproduced by running the same line loop |

Also needed on the emulation thread side: the catch-up calls (section 10.2) must stay synchronous (render the
requested lines immediately, with the live VRAM), and `[video+0x458894]` must be kept so the frame end renders
only the remaining lines. On a catch-up frame engine B is also rendered on the emulation thread; moving B is a
separate decision (it is already off the emulation thread on normal frames).

---

## 13. Hook points (16-byte `ldr x16,#8; br x16; .quad` patches, `rast_hook(off, expect[4], to)`)

| function | offset | first 4 words | PC-relative inside? | args |
|---|---|---|---|---|
| update_frame | 0x30dc0 | a9b97bfd 528017e1 910003fd f9400002 | no | x0 video |
| video_render_scanlines | 0x30be0 | a9b97bfd 910003fd a9025bf5 b9622015 | no | x0 video, w1 last line |
| video_2d_render_scanlines | 0x42a90 | a9b97bfd d2800187 910003fd a90573fb | no | x0 eng, w1 first, w2 last, x3 capture/NULL |
| render_scanline | 0x404a0 | d283540c cb2c63ff a9007bfd 910003fd | no | x0 eng, x1 out, w2 line, x3 capture |
| render_scanline_2d | 0x3ef00 | d283a60c cb2c63ff b0000905 a9017bfd | adrp (3rd): replace fully | x0 eng, x1 planes, w2 line, x3 capture, w4 hires -> w0 |
| start_frame | 0x30870 | a9b77bfd d2940001 f2a07641 910003fd | no | x0 video |
| video_2d_queue_event | 0x429f0 | a9bb7bfd 910003fd a90153f3 91408413 | no | x0 eng, w1 key, w2 value, w3 size, w4 line |
| video_2d_process_event | 0x420c0 | a9bd7bfd 910003fd b9400023 a90153f3 | no | x0 eng, x1 event* |
| event_hblank_start_function | 0x1d5a0 | a9be7bfd 910003fd a90153f3 aa0003f3 | no | x0 st |
| event_scanline_start_function | 0x1d960 | a9bd7bfd 910003fd a90153f3 79402814 | no | x0 st |
| video_2d_reorder_obj | 0x3e530 | a9b27bfd aa0003e1 910003fd a90153f3 | no | x0 eng |
| video_2d_map_bg_direct_layers | 0x41700 | b9409001 f901201f f901781f 12000821 | no | x0 eng |
| video_2d_set_display_control | 0x41ef0 | a9be7bfd 2a0103e5 910003fd a90153f3 | no | x0 eng, w1 DISPCNT |
| video_2d_set_bg_control | 0x41cb0 | a9bc7bfd 910003fd a90153f3 2a0203f3 | no | x0 eng, w1 n, w2 BGnCNT |
| render_scanline_bg | 0x36880 | a9b67bfd 910003fd 79415008 a90153f3 | no | x0 eng, x1 lines, x2 vis, w3 line |
| render_scanline_3d | 0x59950 | f9400402 b944a043 b9446842 34000103 | cbz (4th) | x0 video, w1 line -> u32* |
| dma_transfer_display | 0x1f190 | a9be7bfd 12be0003 910003fd a90153f3 | no | x0 dma, x1 channel, w2 line -> u16* |
| remap_palette_oam_direct | 0x1b6d0 | a9bd7bfd 9143f001 d106a022 9143e809 | no | x0 sys |
| remap_palette_oam_deferred | 0x1b520 | 9143f003 a9bd7bfd d106a062 aa0203e4 | no | x0 sys |

The cleanest replacement seam is `video_2d_render_scanlines` for `eng == engA`: it receives the line range, the
capture pointer, and owns the replay. A hook on `video_render_scanlines` sees the frame-end vs catch-up decision.

---

## 14. Costs (from re/compositing-2x.md, light 3D scene, 2x)

render_scanline self: ~120 instructions per DS line; render_scanline_2d self ~490 per line; the frame driver
(video_2d_render_scanlines loop + replay) is negligible per line (about 15 instructions per line plus ~10 per
event). Engine A as a whole: ~1.04 M instructions per frame on the emulation thread (5450 per line), engine B in
display-off mode ~0.16 M on the render thread.

---

## 15. Open questions / guesses to verify

1. `[st+0x3b2a9a4]` = frame-skip counter and `[st+0x882374]` bit 2 = "rendering disabled" are inferred from use.
2. The VRAM bank table `video+0x10 + bank*16` field meanings (kind == 6, offset at +8) and the bank order of
   `video+0x20a0..`.
3. Who fills `video+0x2130..` (ext palette slot pointers) and the BG2/3 slot assignment.
4. The OBJ table layout at `eng+0x380..0x21340` (stride 0x30 per entry is a reading of reorder_obj's x9 use, not
   verified end to end).
5. `C+0x38` (FIFO-source capture path) and `video+0x458871`.
6. DMA start-mode code 4 = main memory display, and that palette/OAM DMA goes through the deferred handlers.
7. What an unmapped VRAM page reads as through the linear alias.
8. `[st+0x841a54]` gating the capture-forces-render rule.
