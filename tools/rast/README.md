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
  included; `dev/cycles.py`), DraStic's renderer (RAST=off), 0.5.0-beta.1, the scheduling and NEON walker of
  2026-10-08, and with everything below too (the group head, the fused 2x walker, the scheduler's A55 model, the
  fused resolve and the 3x walker's steps; profiles of 90 s), and the second round below (the translucent tails and
  st4 stores, the 2x fog and edge-marking resolves, the 3x edge marking's id screen and walker stores, the 2x span
  setup and batch splitting):

  | | DraStic's renderer | 0.5.0-beta.1 | scheduled, NEON walker | all of the first round | second round |
  |---|---|---|---|---|---|
  | stress ROM L4, 2x | 39.9 M | 24.7 M | 22.1 M (-10.4%) | 19.2 M (-22.2%) | 18.1 M (-3.3%) |
  | field scene S7, 2x | 7.69 M | 5.33 M | 5.20 M (-2.4%) | 4.57 M (-14.3%) | 4.08 M (0.0%) |
  | fog and edge marking S4, 2x | 19.0 M | 13.6 M | 12.9 M (-5.5%) | 11.5 M (-15.1%) | 9.75 M (-11.7%) |
  | stress ROM L4, 3x | | 48.5 M | 38.8 M (-20.0%) | 35.9 M (-26.0%) | 34.2 M (-3.3%) |
  | field scene S7, 3x | | 9.65 M | 8.36 M (-13.4%) | 8.08 M (-16.3%) | 7.41 M (-2.3%) |
  | fog and edge marking S4, 3x | | 27.0 M | 24.4 M (-9.4%) | 21.0 M (-22.3%) | 18.9 M (-7.5%) |

  The totals include 0.07-0.83 M a frame of blocks outside DraStic and librast (cycles.py's `[other]`) that vary
  from run to run: the second round's changes are against the first round's profiles with those taken out (L4 2x
  18.42 -> 17.82 M, S7 4.00 -> 4.01, S4 10.90 -> 9.62, L4 3x 35.05 -> 33.89, S7 3x 7.51 -> 7.34, S4 3x 20.32 ->
  18.80; S7 at 2x spends most of its 3D time in the kernel 14010, which did not change); the earlier columns include them.

  The same pixels: RAST=diff gives DraStic's renderer's differing bins (the quirk above) at all 38 checkpoints of
  the scene cycle to 91200 bins. Since then each group starts with one basic block (`kerngen.py`'s head()): the
  depth test, the perspective weights beside it (computed for every group, so that the two chains overlap: the
  in-order core overlaps only what interleaves in program order) and the next group's steps (no longer the latch's
  last result); and the texel gather takes its addresses with umov instead of through the stack. kpath, cycles a
  group of 8 pixels: 02000 156 -> 135, 02100 207 -> 186, 04010 165 -> 144, 14010 166 -> 158, 23110 219 -> 201;
  cycles.py, stress ROM L4 at 2x: 21.85 M -> 20.98 M cycles a frame (-4.0%; librast 16.95 M -> 16.07 M), the
  kernel 02000 5.29 M -> 4.84 M, 02010 2.59 M -> 2.34 M (instructions +4%: the weights of groups that fail).
  Then the 2x walker fused (`walk.c`): each edge's five routines in one pass over its lines, 8 a step in registers
  (the first 8 in the edge setup's basic block), no float pairs and weights through scratch memory, no per-routine
  reloads of the vertices; the span setup, with render_polygon_4x's edge-marking fix-up folded in, and the edge
  markers 4 lines a step. Stress ROM L4 at 2x: the walk (edges, x/z, span setup, render_polygon_4x) 2.73 M -> 1.75 M
  cycles a frame; the edge-marking scene S4 0.76 M -> 0.44 M (its frame 12.59 M -> 12.21 M). With `batch_asm`'s kernel and flags chosen once per polygon, `f_run`'s batch
  loop two lines a step and walk.c calling the setup without rast.c's hook, the per-polygon setup 1.19 M -> 1.05 M:
  the whole frame 21.83 M -> 20.81 M (-4.7%), the same differing bins at all 43 checkpoints of the scene cycle.
  `kernsched.py`'s machine model is now llvm-mca's in-order A55 as measured (128-bit and widening NEON operations
  hold a pipe two cycles and issue first in their group, `ldp q` holds the load pipe 6 cycles, results write back in
  program order), each block scheduled several ways (the list scheduler, the original order, seven model-driven
  rankings) and the one that reaches its branch soonest from the state the previous block leaves kept; an
  out-of-line block's return takes a copy of the code after its join, so the blocks around the join become one.
  kpath over 59 kernel paths: 10116 -> 9670 cycles (-4.4%). The combined build: the scene cycle's differing bins
  as DraStic's renderer's at all 28 checkpoints to 67200 bins, deferred shading to 81600, the compositor's tables
  checked against the C port (5657, 0 differ), the stress ROM's 6 bins in 4800 as before.
  Then across blocks (`kerngen.py`'s layout; kpath, cycles a group): the translucent kernels load the destination
  colours and the depth words (or the attribute words) into the blend's registers, free by then, before the fog and
  depth-update branches, and store colours and attribute words after them in one block, so that the two
  read-modify-writes interleave: 02100 184 -> 170 (with blending 212 -> 197), 02110 178 -> 168, 04100 (16 colours)
  193 -> 178, 23110 198 -> 188, 00100 114 -> 104. The destination's halves come split by ld2 (no ldp q and uzp
  pair: 02100 170 -> 165, with blending 197 -> 189); the textured kernels' alpha modulate and alpha test sit behind
  one flag test (bit 15, set per batch: no alpha test and A is 31), both out of line, so the modulates and the store
  are one block with no taken branch (the model does not price taken branches: 1 cycle less; two taken branches a
  group fewer); the lit direct-textured opaque kernels store a full group with st4 from the modulates' bytes instead
  of four zips and stp (with the flag test 02000 129 -> 125, white lines 117 -> 115), and so do the flat textured
  ones whose depth words are in registers, through v24-v27 (02010 128 -> 122, 22010 119 -> 114). cycles.py, 90 s
  profiles against e02a298's: stress ROM L4 at 2x, librast 14.75 M -> 14.38 M cycles a frame (02000 4.69 -> 4.58,
  02100 2.86 -> 2.72, 02010 2.31 -> 2.24, 02110 1.46 -> 1.41); fog and edge marking S4 at 2x, 04010 3.42 -> 3.30.
  The 2x walker's span setup (`walk.c`'s walk_spans) then takes the left/right swap a step of 4 lines at a time when
  all 4 agree (the winding decides which chain is left; only the lines where the chains meet differ): no selects,
  and without a swap no stores of the left arrays besides x. Stress ROM L4 at 2x: walk_polygon_4x 1.75 M -> 1.62 M
  cycles a frame (its costliest block, the span setup's step of 4 lines, 58 -> 36 cycles; 4% of the steps mix); an
  edge's first coefficient lanes as one vector product and sum (the same bits, walk.c says why): 1.62 M -> 1.59 M.
  `f_run`'s batch splitting (runs of lines with pixels, at most 512 together; a chain of dependent loads and compares,
  ~10 cycles a line) is 8 lines a step as NEON prefix sums (`batch_end`, `ut/batch_end` checks it against the loop it
  replaced): f_run 0.70 M -> 0.64 M a frame, at 3x 0.83 M -> 0.68 M (the same batches).
  The 3x resolve's edge marking (`hr.c` edge_lines) first screens each block of 16 pixels of its two lines on the
  polygon ids alone: a block whose neighbour pairs all have the same id has no edge and is left as it is (96% of the
  blocks in the edge-marking scene S4), the ids read from byte planes of the attribute lines' top bytes made once per
  bin (edge_tplanes, ldr q and uzp2). Modeled A55 cycles a frame, S4 at 3x: the edge marking 2.38 M -> 0.95 M
  (edge_lines 0.63 M + edge_tplanes 0.32 M), the frame 20.98 M -> 19.11 M; `ut/hr_ab` against e02a298: the same output.
  The 3x walker stops a chain at its window (`EDGES_LINE_CAP` in `spec/edges_impl.h`, hr.c only): a chain that goes
  down, up and down again (a self-intersecting polygon) covered lines twice and ran on in the span arrays, past 64
  entries the right chain's overwrote the left chain's first entries and past 128 lines its perspective coefficients
  ran out of the span block (into the heap: what made `ut/hr_ab`'s two copies of the same hr.c hand the kernels
  different spans for a few polygons in 5000). Within the window the spans the kernels read are the same (only span
  array 10's scratch leftovers differ, and only with edge marking off, when nothing reads it); convex polygons never have such chains (9-
  and 10-vertex polygons with repeated vertices may), so real scenes should render as before. `ut/hr_ab` counts the polygons with such chains apart against a base
  without the cap: against e02a298 0 of the others differ (and ~40% of the ~19% random polygons with one do).
  The NEON walker's interpolate_parameters stores its interleaved halfwords with zip1 and str q instead of st2 (38 ->
  28 modeled cycles a step of 4 lines): stress ROM L4 at 3x, hr_render_polygon_interpolate_edges 2.11 M -> 1.94 M
  cycles a frame (the frame 35.88 M -> 35.37 M); the same bytes (t_edges.c against DraStic's, `ut/hr_ab`).
  The 2x resolves with fog and edge marking are ours too (`res2.c`'s res2_resolve_fx(), dispatched by rast.c's
  resolve_bin): DraStic's drivers called a leaf routine per stage and line (fog weights into a byte array, the
  modulate, edge identify into an edge array, edge mark), each a pass over memory, and the table pass read the block
  back. Now hr.c's NEON fog (weights and modulate in one pass over byte planes) and edge marking (identify and mark,
  two lines a step sharing their compares) run on the context's colour lines in place, the edge marking skipping
  the depth-key compares of steps in which no pixel's polygon id differs from a neighbour's and the fog the modulate
  of steps whose factors are all 0, and the plain resolve's
  pass (res2_line_asm) writes the block and its table entries; DraStic's line structure is kept (bin 0's top line,
  bin 11's bottom line with line 30's attributes, the gap copies). Fog and edge-marking scene S4 at 2x: DraStic's
  identify 1.42 M, weights 0.50, mark 0.38, modulates 0.51 and the table pass 0.18 M cycles a frame become fog 0.78 M
  (0.89 M without its skip), edge marking 0.57 M (1.02 M without its skip) and the resolve pass 0.26 M: the frame
  11.05 M -> 9.82 M (-11.1%; profiles of both builds from the same session. An older profile of the base build gave
  11.54 M, of which 0.5 M was a block outside DraStic and librast that later profiles of either build do not show).
  `ut/t_resolve.c` compares the block, the gap buffers and the table entries with DraStic's drivers (all six modes,
  every bin, polygon ids in rectangles and zero fog tables for the skips; it fails mutants of either skip); S4's
  frame diff 0 bins in 12000 with the compositor's checks 0 differ, the stress ROM's 6 in 4800.
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
  scalar versions); fully transparent outputs carry the clear colour, and only the 5-bit alpha counts as coverage.
  The edge marking works in place on the colour lines and loads and stores only steps with a marked pixel (bits
  29-31, which the 2x resolve clears, are ignored by the downsample), both lines' tests of a step before the one
  test; the fog takes two groups a step, two independent chains for the in-order core (S4 at 3x, modeled A55 cycles
  a frame: the edge marking 3.03 -> 2.38 M, the fog 2.75 -> 2.12 M). Changes to these stages are checked against
  an earlier hr.c by `ut/hr_ab/run.sh` (both versions on the same random bins, the output blocks compared).
  The field scene costs 4.9 M instructions a frame at 3x against 1.7 M at 2x: the kernels 2.8 M (the pixels), the
  downsample 0.66 M (four triples a step; groups whose 36 alphas are all 31 or all 0 take fast paths, the same bits
  as the general case), the rest the clear and the resolve over 2.25x the pixels (`RAST_DUMP` also writes the 3x
  frames as `hNNNNN.ppm`).
  Two things at the bin boundaries (ROCKNIXDS issue 47: black or coloured lines every 32 rows at 3x, and a line
  flickering while the scene moves): with edge marking on, DraStic's `update_frame_3d_4x` re-marks rows 32k-1 and 32k
  after the bins from gap buffers that only its own bin resolve filled, so each 3x bin now fills them with its own
  rows and attributes that make that pass an identity (`RAST_HRCHECK=1` compares those rows after the update: 22
  rows a frame changed on the fog and edge-marking scene S4 before, 0 after); and DraStic's bin lists are made from
  the 2x lines, while a polygon of bin k - 1 alone can cover 3x line 48k (the field scene S7: one polygon in about
  every frame, a missing row of the clear colour in 10 of 69 sampled frames) and the edge marking's neighbour lines,
  so a 3x bin also renders its neighbours' polygons, in DraStic's order (`hr_lists`). DraStic's "disable edge
  marking" setting now turns the 3x edge marking off too.
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
  time. With edge marking (res2_resolve_fx(), see Status) the same pass writes the fogged and marked lines; with
  fog alone `comp_bin` reads the block back with the generator's second function (`ldr q` + `uzp2` for the alphas
  instead of compvis.h's `ld4`; at 3x too). Both are list-scheduled for
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
included: `t_edges.c` tests both forms); `src/rast/walk.c` is render_polygon_4x for the 2x bins on fused NEON forms
of them (an edge's five routines in one pass, DraStic's values on the lines the setup and the markers read; sprites,
shadow polygons and odd vertex counts stay with DraStic's; `RAST_WALK=0` all of them; `t_walk.c` compares the span
arrays with DraStic's walker's on random polygons). `src/rast/fused.c` runs every pixel through all
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
stack apart; the branches stay where they are, but an out-of-line block's jump back to the common path takes a copy
of the instructions after its join, which makes one block of the code on both sides of the join): of several
candidate orders it keeps the one that runs fastest on its model of llvm-mca's in-order Cortex-A55 (issue groups, the
FP and load pipes' occupancy, the in-order writeback; it matches llvm-mca's cycles on the group loops within a
cycle), with the state the block before leaves; `KERNSCHED=0 python3 kerngen.py` writes the kernels unscheduled.

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
- The 2x fog (`res2.c` fog_line2x, 0.78 M cycles a frame on S4 at IPC 0.57) is bound by the FP pipe: four channels'
  smull/smull2/shrn/shrn2 per 16 pixels and the weights' two tbl2. The attributes by ldr q + shrn instead of ld4 + zip
  models worse (0.89 -> 0.92 M). DraStic's gap passes (rows 32k-1 and 32k, 0.08 M) still run. hr.c's 3x fog and edge
  marking could take res2.c's skips (S4 2x: edge marking 1.02 -> 0.57 M, fog 0.89 -> 0.78 M).
- The deferred shade pass without re-setup (keep the visibility pass's kernel arguments in the queue entry) and
  the per-line overhead of the visibility pass; 64-bit texel-pair loads in the bilinear gathers.
- The composite's row-level shortcut: when both quarters of an output row are fused, convert the 3D frame straight
  into the scanout and skip the planes (~0.08 M a frame; needs the convert hook keyed by plane pointer).
- DraStic's 2x line conversion (`render_scanline_color_convert_direct_32_2x_asm`, called from render_scanline for
  every output line: 768 calls a frame, 0.62 M cycles at IPC 0.45 in every game, 2x and 3x): its `ld1 {q}` loads
  with writeback hold the A55's load pipe ~5 cycles each in llvm-mca's model. The same loop with `ldr q` at a
  register offset (x3 as the offset, `cmp x3, #256` as the test; still 31 instructions and the same registers)
  models at 28 cycles a group of 32 pixels instead of 50: ~0.25 M a frame (S7 at 2x: ~5%). Needs a replacement
  hooked in (rast_hook to a function in librast) and a unit test against the original.
- The group loop is one dependence chain per group (depth test, reciprocal, interpolants, gather, modulate; IPC
  ~0.8-0.98 in the model): within its blocks the scheduler is near the model's optimum (a hill climb over its orders
  finds 0.1% more). Across blocks the translucent tail, the alpha stages and the st4 stores are done (Status); the
  reciprocal runs beside the depth test in the group head (head()), only the lazy loop's entry (28:) waits for the
  test. Left: the opaque store reloads the depth words from the stack (ldp q and the pid orr, ~6 cycles a group
  exposed on 02000: no two registers are free across the modulates; ORing pid in before the spill costs the head 1-3
  cycles); the z-depth paletted flat kernels (04010, S4's) could store with st4 as 02010 does if their depth words,
  spilled, came back into registers outside v24-v27; the texel gather's ld2 (7 cycles with nothing
  independent left; ldr q and uzp model 2 worse); the 16-colour gather's bytes through the stack (stp w and ld2 to
  assemble 8 bytes). Overlapping consecutive groups needs registers the kernels do not have (the bilinear kernels use
  them all). Hoisting the fall-through code over the w7 flag branches does not pay (llvm-mca:
  +1-8% on most kernels, the hoisted work lands above taken branches; -4-6% on the bilinear ones).
- A device A/B of the scheduling (llvm-mca's model is what it is: its integer latencies are 3, the A55's 1-2), and
  of the texel gather's trip through the stack (the texels stored as words, read back by ld2; the addresses now
  leave by umov), which the model cannot price (store-to-load forwarding).
- 3x: the top vertex for tied vertices; the edge markers and the edge-marking x adjust in NEON; later the hi-res 3D
  layer presented through the dsflip shader instead of downsampled.
  Polygons of 9 and 10 vertices: DraStic's render_polygon_4x takes the walk's ninth vertex as base + 0 and never
  writes a tenth's slot (it walks a stale stack slot; walk.c leaves 10 to DraStic). hr.c's vertex indices for them
  are nibbles 0 and 1 of the group's base sequence (`4 * (k & 7)`: what its former `seq >> (4 * k)`, undefined
  from k = 8, compiled to; ut/hr_ab: the same output on 1.5 M polygons): decide what 3x should walk there.
  The downsample is bound by its loads and the alpha test in the cycle model: two groups a step, the opaque outputs
  computed before the test and the loads a group ahead all modeled within 3% of the current loop. Its three ld3 cost
  about what nine ldr q and a 3-way deinterleave would (tbl of two and three registers plus ins: ~11 cycles a row in
  llvm-mca), and the alpha tests need no deinterleave; the u8 stage's wrap for 8-bit channels (ut/hr_ab's wide case)
  must stay. fog_line (S4 at 3x: 2.1 M, IPC 0.62) keeps 14 constant vectors: the compiler reloads one tbl table from
  the stack every step (ld1 of two registers); fewer constants (0x81 and 0x7f, the table pair) would keep it in
  registers.
- The 2x walker (stress ROM L4, 1.59 M cycles a frame): an edge's step computes the next 8 lines' weights after its
  exit test (29 instructions, 43 cycles, one reciprocal chain). With the test first and that chain in the stores'
  block clang spills (13 stack accesses a step) and the step models at 99 cycles instead of 93: needs fewer live
  vectors (z as two vectors and a step, the colour bases in one). The edge setup (120 instructions, 99 cycles)
  starts the first weights' chain at its cycle ~49, behind the w loads and conversions. `batch_end` is latency-bound
  (38 cycles a step: ldp, uzp1, three ext + add, the compare, fmov): a polygon's prefix sums in one pass, then a
  compare a batch.
- The texture alpha cache (`tex_min_alpha`, fused.c) keys on DraStic's texture-cache entry and the frame: it
  assumes DraStic does not reload an entry with another texture within one frame. True on everything tested; a
  content signature in the key would make it certain.
- In the simulator, dsscenes-cycle.nds died at its scene 2 -> 3 transition (a SIGILL in DraStic's JIT cache, also
  with our hooks off, since 2026-10-03); regress.sh checks the later scenes one ROM at a time until that is
  understood. In a simulator set up afresh on 2026-10-08 it ran through every scene seven times without it (RAST=diff
  to 91200 bins, the same differing bins as DraStic's renderer at every checkpoint: 47 at 7200, 662 at 14400, 1919
  at 72000). A rare startup hang is DraStic's own (helpers waiting on locks not yet initialised, ROCKNIXDS
  docs/handoff-local.md); the simulator does not preload libdsflip, which works around it: run again.
