# Gengis Engine: our own 3D rasterizer for DraStic

Replaces DraStic r2.5.2.2's hi-res 3D raster stage (`video_3d_render_bins_4x` and `render_polygon_setup_4x`, hooked
by code patching in `src/rast/rast.c`) and matches DraStic's output bit for bit. Built into libsuperdrastic.so,
off unless `DSFLIP_RAST=1` (ROCKNIXDS: the "3D renderer" option).

## Status

- **Exact.** Checked polygon by polygon against DraStic's own rendering, in the same process, on the stress ROM
  and on the ten feature scenes of ROCKNIXDS `stressrom/scenes9.c` (texture formats, wrapping, translucency,
  shading modes, fog, edge marking, shadows, 2D-in-3D, a game-like field, the rear plane, clipping edge cases).
  One known deviation: a translucent polygon whose batch is a single pixel. There DraStic ANDs that pixel's
  translucent id with a byte of its scratch memory that no stage of that batch wrote, so the result depends on
  what earlier polygons left behind. About one pixel in a thousand 32-line bins of the stress ROM; invisible
  unless a later translucent polygon with the same id covers the pixel.
- **Faster than DraStic in the simulator.** Instruction counts under qemu, stress ROM L4 at 2x: the stage costs
  11.3 M instructions a frame against DraStic's 16.5 M (0.3.0-rast3: 12.2 M; the kernels' exact shortcuts of
  rast4, see below). The field scene S7 1.53 M (was 1.65), deferred shading on L4 9.8 M (was 13.1); at 3x S7 5.0 M
  (was 12.2) and the fog and edge-marking scene S4 13.1 M (was 28.0), after the NEON downsample, edge marking and
  fog of rast4. Device timings are the open question: the kernel's texel gather is scalar like DraStic's, the
  rest is NEON without DraStic's trips through scratch memory between stages.
- **Less on the emulation thread.** DraStic's 2x compositor asks, per half-row, which 3D pixels are visible
  (`render_scanline_set_3d_visibility`, 0.33 M instructions a frame on the emulation thread); the render threads
  now fill that table as they finish each bin and the hook answers from it (0.02 M on the emulation thread;
  `RAST_COMP` 0 off, 1 the NEON replacement only, 2 the table, the default; `RAST_COMPCHECK=1` checks every
  answer against a C port of DraStic's routine). A quarter that shows only the 3D layer and the backdrop is then
  written in one NEON pass instead of DraStic's five-stage chain (`render_scanline_2d_composite` hooked,
  `RAST_COMPFUSE=0` off): on the stress ROM the chain's 0.81 M a frame become 0.39 M, on the field
  scene 0.47 M become 0.07 M, checked byte for byte against the original in the running emulator.
- **Scheduled for the handhelds' in-order core.** The RG DS and RG DS Plus run Cortex-A55 cores, which issue in
  program order, at most two instructions a cycle, and stall on every operand that is not ready: instruction counts
  miss that. `tools/rast/dev/cycles.py` models it (every executed block of a simulator profile through llvm-mca's
  Cortex-A55 model): the kernels issued at an IPC of about 0.6, each group of 8 pixels a chain of dependent stages.
  `kernsched.py` list-schedules every basic block of the generated kernels for that core, the kernels hold more in
  registers so that independent chains can overlap (the vertex colour among the texel gather's loads, the three
  colour channels' modulates side by side), and the polygon walker is NEON (`spec/edges_impl.h`), at 2x too
  (`walk.c`, our own render_polygon_4x). Modeled A55 cycles a frame, whole emulated frames (CPU emulation and 2D
  included), 0.5.0-beta.1 -> the scheduling, the NEON walker and the lit direct-textured kernels (profiled before
  the flat and translucent kernels' changes, which take 2-11% more off those kernels' groups):

  | | 2x | 3x |
  |---|---|---|
  | stress ROM L4 | 24.7 M -> 22.5 M (-9.0%) | 48.5 M -> 39.5 M (-18.5%) |
  | field scene S7 | 5.33 M -> 5.18 M (-2.8%) | 9.65 M -> 8.41 M (-12.9%) |
  | fog and edge marking S4 | 13.6 M -> 13.0 M (-4.3%) | 27.0 M -> 24.8 M (-8.2%) |

  The same pixels: RAST=diff gives DraStic's renderer's differing bins (the quirk above) at all 38 checkpoints of
  the scene cycle to 91200 bins.
- Not yet measured on a handheld.

## Options beyond DraStic's rendering

- **Texture filtering** (`DSFLIP_RAST_TEXFILTER` / `RAST_TEXFILTER`: 0 nearest = exact, 1 bilinear, 2 sharp
  bilinear). Kernel variant B blends the four texels around the sample point with 3-bit weights; "sharp" remaps the
  texel fraction through a curve that keeps texels crisp away from their edges. Where one of the four texels is
  transparent the pixel takes the nearest texel in all four channels, so cut-out textures (colour-0 transparency,
  alpha-tested sprites) keep their shape and show no dark fringes. Costs about 1.8x the raster stage on the stress
  ROM and 2.4x on the field scene (four texel gathers a pixel instead of one). Toon, highlight and decal polygons
  stay nearest.
- **Deferred opaque shading** (`DSFLIP_RAST_DEFER` / `RAST_DEFER=1`, `src/rast/defer.c`). Opaque polygons go
  through a visibility pass first (depth test, the alpha test only for textures whose lowest texel alpha can fail
  it, attribute words, edge marks, and the polygon's index per pixel in an owner buffer), and are shaded afterwards
  only where they still own the pixel: a pixel is shaded once however many polygons covered it. Same bits as the
  one-pass render (checked with `RAST=diff` on the stress ROM and the scenes). Toon, highlight and shadow polygons
  flush the queue and render at once. It pays only when shading is expensive relative to the per-line setup, which
  runs twice: with nearest filtering it is slower (stress ROM, overdraw 1.5: +2.5%; field scene, overdraw 1.0:
  +28%), with bilinear filtering faster on the stress ROM (20.9 against 22.6 M) and still +10% on the field scene.
  So it is on by default with bilinear filtering and off otherwise (`RAST_DEFER=0/1` overrides).

- **3x internal resolution** (`DSFLIP_RAST_SCALE` / `RAST_SCALE=3`, `src/rast/hr.c`). The scene is rendered at
  768x576 and supersampled into DraStic's 2x frame: every bin at 48 lines x 768 (plus a neighbour line above and
  below for the edge marking), with the vertices' 3x coordinates from a hook on DraStic's perspective transform
  (the same math with the viewport scaled by 3), our own polygon walker (spec/edges.c's routines with the wider
  layout), the same kernels in a hi-res instantiation (rast_kern_h*, strides from the kernel arguments; deferred
  shading and bilinear filtering included), the bin resolve over 768-pixel lines and a 3:2 alpha-weighted box
  filter into the output block. The 2D compositing then sees a normal 2x frame, with anti-aliased polygon edges
  and 2.25 samples per pixel of texture detail. Shadow polygons (mode 3) render on the scalar line path with
  DraStic's stencil rules; DraStic's sprite path (axis-aligned textured quads) goes through the general walker;
  points and lines (1- and 2-vertex polygons) walk like DraStic's. The vertex table holds the DS's 6144 vertices
  (an earlier 1568 limit skipped polygons with higher vertex indices: a black sky and missing sparkles behind
  Ho-Oh in HG/SS). The downsample, the edge marking and the fog of the resolve are NEON (unit-tested against the
  scalar versions); fully transparent outputs carry the clear colour, and only the 5-bit alpha counts as coverage. The field scene costs 4.9 M
  instructions a frame at 3x against 1.7 M at 2x: the kernels 2.8 M (the pixels), the downsample 0.66 M (four
  triples a step; groups whose 36 alphas are all 31 or all 0 take fast paths, the same bits as the general case),
  the rest the clear and the resolve over 2.25x the pixels (`RAST_DUMP` also writes the 3x frames as `hNNNNN.ppm`).
- **Palette lookups with tbl.** 4- and 16-colour textures (the common DS formats I2 and I4) keep their palette
  in four NEON registers and look texels up with `tbl` instead of a dependent load per texel; exact. The
  instruction count hardly changes; the gain is the removed load latency on the handheld's in-order cores.

## The 2D compositor's 3D visibility step (`src/rast/comp.c`, exact)

DraStic composites the 3D screen on the emulation thread, per DS line in four quarters of 256 pixels (the two
output rows' even and odd pixels). For each quarter `render_scanline_set_3d_visibility` turns the 3D pixels' alpha
bytes into BG0's visibility bitmap (bit = alpha != 0) and a flag (2: some alpha not 0 or 31, which selects the
blending path; 0x10: visible and all opaque; 0: nothing visible): ~425 instructions, 768 calls a frame. With the
renderer hooked (`RAST=ours`/`diff`, `DSFLIP_RAST=1`):

- **NEON replacement** (`DSFLIP_RAST_COMP` / `RAST_COMP=1`): the same 32 bytes and return value in ~140
  instructions a call (the kernel 124, `src/rast/compvis.h`: `ld4` for the alpha bytes, `cmtst` + bit weights + an `addp` tree for the
  bitmap, `umin(a, a ^ 31)` for the flag), for any pointer DraStic passes (the 2x frame, the 1x line, a
  BG0HOFS-shifted copy on its stack).
- **Per-bin table** (`RAST_COMP=2`, the default): the result depends only on the 1 KiB half-row the pointer
  addresses, so the render threads compute all 64 half-rows of a bin right after its output block is written, into
  a table per output frame; the hook copies the entry (~28 instructions) when the pointer is a half-row of a frame
  whose table is current. Wrappers on `update_frame_3d_4x` (re-marked gap rows, the frame copy when nothing is
  rendered with `threaded_3d`, the unpublished frame), `update_frame_3d_1x` and `reset_video_3d` keep the tables in
  step with every write DraStic makes to the two output frames (the rule is in comp.c's header).
- **The resolve fused with the table** (`src/rast/res2.c`; `res2_line.S`, generated by `res2gen.py`): without fog
  and edge marking, the bin's resolve (DraStic's `video_3d_resolve_bin_asm_4x`: the colour lines masked to 29 bits
  and split into the even and odd half-rows) and the table's 64 entries come from the same registers, a line at a
  time. With fog or edge marking DraStic's resolves run and `comp_bin` reads the block back with the generator's
  second function (`ldr q` + `uzp2` for the alphas instead of compvis.h's `ld4`). Both are list-scheduled for
  llvm-mca's A55 model, which retires in order: an instruction that would write back before an earlier one waits,
  so every 7-cycle `ld2` (11-cycle `ld4`) stalls what follows it (DraStic's resolve loop: 68 cycles per 32
  pixels). Modeled cycles a line of 512 pixels: resolve and both half-rows' entries 707, against DraStic's
  resolve 1088 plus `comp_bin`'s ~610; `comp_bin` alone 468. A frame (`cycles.py`, 90 s): stress ROM L4 0.413 +
  0.234 M -> 0.292 M (frame 21.83 -> 21.49 M); fog and edge marking S4 `comp_bin` 0.234 -> 0.196 M. Exact:
  `t_resolve.c` (the block against DraStic's resolve, the entries against the C port, alpha bytes of any value),
  `t_comp_hook.c`, and `RAST=diff RAST_COMPCHECK=1` on the scene cycle (DraStic's renderer's differing bins at
  every checkpoint, every table equal to the C port).
- Exactness: `spec/composite.c` is the C port, unit-tested against the originals (`tools/rast/ut/t_composite.c`,
  which also tests the NEON version); `RAST_COMPCHECK=1` checks every call in the running emulator against the C
  port, and every table when it becomes valid (`[comp] check:` every 2 s, `[compcheck]` lines for differences).
  The test ROMs' 3D layer is opaque nearly everywhere, so a stale table entry mostly gives the right answer anyway:
  the check mode catches a missing gap-row update (S4: 172 differing calls in 30 s with it removed) but hardly a
  missing table copy (a frame-skipping S4 with `threaded_3d=1` flags only its first copy). `tools/rast/ut/t_comp_hook.c`
  covers that bookkeeping on random frames: the hook's lookup and the wrappers' validity rule (rendered with
  re-marked gap rows, copied, unchanged, 1x, reset; `threaded_3d` on and off) against the original, with stubs that
  write the frames as the disassembly of `update_frame_3d_4x` shows.
- Cost per frame, simulator instruction counts (stress ROM L4 and the field scene S7, 90 s each): DraStic's
  function and its gather 0.326 M (425 a call); NEON 0.108 M (139 a call); table 0.024 M in the compositor (28 a
  call) plus 0.100 M on the render threads (`comp_bin`, 130 a half-row) and 0.006 M for the gap rows. On the
  emulation thread's critical path: with `threaded_3d=1` 0.024 M against 0.108 M for NEON alone; with
  `threaded_3d=0` the emulation thread renders a third of the bins and waits for the rest, so about 0.063 M against
  0.108 M. (The block profiler keys blocks by start ^ length; two of hook_vis's blocks collide there, so its
  report undercounts the NEON path as 99 a call: the figures here are the static path lengths.)

### The fused 3D + backdrop quarter (`render_scanline_2d_composite`, `src/rast/compfuse.h`)

After the visibility step, DraStic composites the quarter: `render_scanline_2d_composite` runs its priority encoder
(which layer is on top per pixel: 256-bit masks per layer and for the backdrop) and `select_pixels`, which merges the
layers' u16 lines, puts the backdrop in, expands the u16 line into 6-bit R/G/B planes, and then writes the 3D pixels'
bytes over the planes where BG0 is on top (`spec/composite.c` documents every routine; ~620 instructions a quarter,
768 quarters a frame on the emulation thread). The hook on `render_scanline_2d_composite` (`comp.c`) runs DraStic's
own encoder, so the masks in the scratch area are its bytes, and reads them: when no other layer of the layer mask
claims a pixel and every pixel is BG0's or the backdrop's, the planes are `mask ? 3D bytes : backdrop` for every
pixel, whatever BG0's own u16 line holds (the derivation is in `compfuse.h`), and one NEON pass writes them: 40
instructions for a quarter that is all 3D, 27 all backdrop, ~140 mixed. Otherwise it calls DraStic's
`select_pixels` as the original does; blending or brightness flags, no 3D layer (engine B) or another layer mask go
to the original through a trampoline. Exactness: the C ports of the chain are unit-tested against the originals, and
the fused pass against DraStic's `select_pixels` and the ports (`t_composite.c`); `t_comp_hook.c` runs the hook
itself against the original on random engines, scratch areas and quarters through every path, with the check mode
on and off; `RAST_COMPCHECK=1` runs every fused call twice in the emulator (ours, then the original through the
trampoline on the same input bytes) and compares the scratch frame, the planes, the 3D pixels and the engine
(`[comp] composite:` counts every 2 s). Cost per frame (simulator instruction counts, 90 s each): the chain's
functions on the stress ROM L4 0.81 M -> 0.39 M (the quarters of engine B, which has no 3D layer,
stay with DraStic's chain), on the field scene S7 0.47 M -> 0.07 M; frame totals 16.6 M (was 17.8) and 3.5 M (was 3.4; the totals move by about 0.5 M between runs, the per-call costs are exact: about 620 instructions a quarter before, 45 / 30 / 150 after).

## How it works

`src/rast/spec/`: DraStic's ~85 raster routines ported to exact C, each unit-tested bit for bit against the
original inside DraStic's process (`tools/rast/ut/`). `src/rast/b0.c` rebuilds DraStic's per-polygon pipeline from
them (the reference, and still the path for shadow polygons). The polygon walker's routines (`spec/edges_impl.h`, a
template: DraStic's layout and the 3x one) also exist in NEON (`EDGES_NEON`, the same bytes, DraStic's overruns
included: `t_edges.c` tests both forms); `src/rast/walk.c` is render_polygon_4x on them for the 2x bins (sprites,
shadow polygons and odd vertex counts stay with DraStic's; `RAST_WALK=0` all of them; `t_walk.c` compares it with
DraStic's on random polygons). `src/rast/fused.c` runs every pixel through all
stages at once; `fused_neon.c` is that in C NEON (8 pixels a step), and `kerngen.py` generates `rast_kern.S`, the
same in assembly with a fixed register allocation, one kernel per variant (depth source x texture x translucency
x flat colour), which is what runs. Shortcuts that give the same bits: a white vertex colour with alpha 31 makes
modulate the identity, so the texel is the colour; a group of 8 pixels that all fail the depth test stops at the
test; one that all pass is stored straight; a texture whose lowest alpha passes the alpha test skips it, and A = 31
skips the alpha modulate; with z or constant depth the perspective weights wait for the depth test, and a line's
weights and interpolants for its first passing pixel; the vertex colour and the w depth use 16-bit products where
they cannot overflow (checked per line). `kerngen.py`'s docstring lists the flags and the register use.
`kernsched.py` then reorders each basic block of the generated code for the A55 (its docstring has the rules: every
register, NZCV and memory access tracked, the base registers' roles telling read-only memory, the lines and the
stack apart; the branches stay where they are); `KERNSCHED=0 python3 kerngen.py` writes the kernels unscheduled.

## Simulator usage (see `tools/sim/` for setup; `dev/` holds the scripts used during development)

- `RAST=diff` renders every bin with both renderers and compares the output frames.
- `RAST=ours RAST_PDIFF=1` compares per polygon and reports the differing pixels; `RAST_PDIFF_STRICT=1` includes
  the 5-mod-8 translucent quirk pixel (exact), `2` the 1-pixel batches (the known deviation).
- `RAST_PIPE` selects the pipeline: 0 = b0 (stage by stage), 1 = fused scalar, 2 = fused C NEON, 3 = assembly.
- `RAST_DUMP=<dir>` writes frames as PPM; `RAST_STATS=1` prints the opaque overdraw; `RAST_FRAMES=1` prints the
  frame count every 10 frames (for per-frame figures from a block profile).
- `dev/prof.sh` profiles a run block by block; `dev/cycles.py <prof.txt> <log> <librast.so>` reports instructions and
  modeled Cortex-A55 cycles a frame per function, DraStic's and ours (`BLOCKS=<function>` lists its costliest
  blocks); `dev/kpath.py rast_kern.S <kernel>` a kernel's cycles per group of 8 pixels. Both need llvm-mca. The
  profiler plugin keyed blocks by pc ^ length until 2026-10-08 (colliding blocks were counted as one): figures from
  before then can be off by a few percent per function.
- The simulator's qemu 9.2.0 needs `tools/sim/qemu-9.2.0-fold_bitsel_vec.patch` (setup.sh applies it): unpatched, its
  TCG optimizer folds a vector bit-select with a constant all-ones false operand to all-ones, so NEON C code using
  `vbslq` with such a constant computes the wrong thing under the simulator only. The generated kernels use runtime
  masks and are not affected; the 3x stage functions' unit tests (edge, fog, downsample) pass with the patched qemu.

## Next

- Device A/B: the same game with the option off and on, from the performance logs (`threads_avg` in the summary).
- The deferred shade pass without re-setup (keep the visibility pass's kernel arguments in the queue entry) and
  the per-line overhead of the visibility pass; 64-bit texel-pair loads in the bilinear gathers.
- The composite's row-level shortcut: when both quarters of an output row are fused, convert the 3D frame straight
  into the scanout and skip the planes (~0.08 M a frame; needs the convert hook keyed by plane pointer).
- The paletted kernels with z depth have no spare register (the palette holds v18-v21, the z steps v6-v9), so their
  colour channels still queue on one scratch register, and the lit paletted ones compute the vertex colour after the
  gather: free some (the z step v9 from v8 in the latch, the texture masks or pid << 24 from the arguments).
- A device A/B of the scheduling (llvm-mca's model is what it is: its integer latencies are 3, the A55's 1-2), and
  of the texel gather's two trips through the stack, which the model cannot price (store-to-load forwarding).
- 3x: the top vertex for tied vertices; the edge markers and the edge-marking x adjust in NEON; later the hi-res 3D
  layer presented through the dsflip shader instead of downsampled.
- The texture alpha cache (`tex_min_alpha`, fused.c) keys on DraStic's texture-cache entry and the frame: it
  assumes DraStic does not reload an entry with another texture within one frame. True on everything tested; a
  content signature in the key would make it certain.
- In the simulator, dsscenes-cycle.nds died at its scene 2 -> 3 transition (a SIGILL in DraStic's JIT cache, also
  with our hooks off, since 2026-10-03); regress.sh checks the later scenes one ROM at a time until that is
  understood. In a simulator set up afresh on 2026-10-08 it ran through every scene seven times without it (RAST=diff
  to 91200 bins, the same differing bins as DraStic's renderer at every checkpoint: 47 at 7200, 662 at 14400, 1919
  at 72000). A rare startup hang is DraStic's own (helpers waiting on locks not yet initialised, ROCKNIXDS
  docs/handoff-local.md); the simulator does not preload libdsflip, which works around it: run again.
