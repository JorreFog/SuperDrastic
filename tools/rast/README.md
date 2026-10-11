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
- **DraStic's geometry stage, cheaper.** Four of its functions on the emulation thread are replaced by exact
  ones (the command queue's fast paths, the vertex batch, the A55-scheduled vertex transform, the polygon binning;
  `RAST_GEOM=0` off, `RAST_GEOMCHECK=1` checks every call against DraStic's): 0.063 M modeled cycles a frame less
  on the field scene, 0.166 M on the stress ROM (below).
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
  setup and batch splitting) and the third (the opaque kernels' attribute words kept in registers and st4 for the
  paletted ones, the 2x and 3x fog's depth screens, the edge screens' loads, the 3x downsample's uniform steps, the 2x
  walker's edge steps), and the fourth and fifth (DraStic's 2D scanline routines replaced, the render threads'
  bin queue, the fog's zero step at both scales, the 3x walker on the 2x walker's fused routines, the deferred
  kernels' visibility group in one block, the five hottest kernels' software-pipelined group loops):

  | | DraStic's renderer | 0.5.0-beta.1 | scheduled, NEON walker | all of the first round | second round | third round, net | fourth and fifth, net |
  |---|---|---|---|---|---|---|---|
  | stress ROM L4, 2x | 39.9 M | 24.7 M | 22.1 M (-10.4%) | 19.2 M (-22.2%) | 18.1 M (-3.3%) | 17.7 M (-0.9%) | 16.7 M (-5.3%) |
  | field scene S7, 2x | 7.69 M | 5.33 M | 5.20 M (-2.4%) | 4.57 M (-14.3%) | 4.08 M (0.0%) | 3.92 M (-2.2%) | 3.59 M (-8.4%) |
  | fog and edge marking S4, 2x | 19.0 M | 13.6 M | 12.9 M (-5.5%) | 11.5 M (-15.1%) | 9.75 M (-11.7%) | 9.12 M (-5.2%) | 7.85 M (-13.9%) |
  | stress ROM L4, 3x | | 48.5 M | 38.8 M (-20.0%) | 35.9 M (-26.0%) | 34.2 M (-3.3%) | 33.8 M (-0.3%) | 30.2 M (-10.6%) |
  | field scene S7, 3x | | 9.65 M | 8.36 M (-13.4%) | 8.08 M (-16.3%) | 7.41 M (-2.3%) | 7.13 M (-2.9%) | 6.68 M (-6.3%) |
  | fog and edge marking S4, 3x | | 27.0 M | 24.4 M (-9.4%) | 21.0 M (-22.3%) | 18.9 M (-7.5%) | 17.7 M (-6.0%) | 14.8 M (-16.6%) |

  The totals include 0.07-0.83 M a frame of blocks outside DraStic and librast (cycles.py's `[other]`) that vary
  from run to run: the second round's changes are against the first round's profiles with those taken out (L4 2x
  18.42 -> 17.82 M, S7 4.00 -> 4.01, S4 10.90 -> 9.62, L4 3x 35.05 -> 33.89, S7 3x 7.51 -> 7.34, S4 3x 20.32 ->
  18.80; S7 at 2x spends most of its 3D time in the kernel 14010, which did not change); the earlier columns include
  them. The third round's column is net of them, against the second round's net figures (its profiles, run beside
  a scene-cycle diff, reached fewer frames and so a different part of each ROM's scene mix: the per-function
  figures in its paragraphs below are the firmer measure; the stress ROM's kernels and walker -0.22 M a frame).
  The fourth and fifth rounds' column is net of them too, against the third's (90 s profiles of each ROM beside
  other simulator jobs: L4 2x 699 frames, S7 4382, S4 2560, L4 3x 415, S7 3x 1525, S4 3x 752). S7 spends ~45% of
  its 3D time in the kernel 14010, whose lines do not take the pipelined loop yet (1.31 M a frame at 2x, as before).

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
  cycles a frame; the edge-marking scene S4 0.76 M -> 0.44 M (its frame 12.59 M -> 12.21 M). With `batch_asm`'s kernel
  and flags chosen once per polygon, `f_run`'s batch loop two lines a step and walk.c calling the setup without
  rast.c's hook, the per-polygon setup 1.19 M -> 1.05 M: the whole frame 21.83 M -> 20.81 M (-4.7%), the same
  differing bins at all 43 checkpoints of the scene cycle. `kernsched.py`'s machine model is now llvm-mca's in-order
  A55 as measured (128-bit and widening NEON operations hold a pipe two cycles and issue first in their group, `ldp q`
  holds the load pipe 6 cycles, results write back in program order), each block scheduled several ways (the list
  scheduler, the original order, seven model-driven rankings) and the one that reaches its branch soonest from the
  state the previous block leaves kept; an out-of-line block's return takes a copy of the code after its join, so the
  blocks around the join become one. kpath over 59 kernel paths: 10116 -> 9670 cycles (-4.4%). The combined build: the
  scene cycle's differing bins as DraStic's renderer's at all 28 checkpoints to 67200 bins, deferred shading to 81600,
  the compositor's tables checked against the C port (5657, 0 differ), the stress ROM's 6 bins in 4800 as before. Then
  across blocks (`kerngen.py`'s layout; kpath, cycles a group): the translucent kernels load the destination colours
  and the depth words (or the attribute words) into the blend's registers, free by then, before the fog and
  depth-update branches, and store colours and attribute words after them in one block, so that the two
  read-modify-writes interleave: 02100 184 -> 170 (with blending 212 -> 197), 02110 178 -> 168, 04100 (16 colours) 193
  -> 178, 23110 198 -> 188, 00100 114 -> 104. The destination's halves come split by ld2 (no ldp q and uzp pair: 02100
  170 -> 165, with blending 197 -> 189); the textured kernels' alpha modulate and alpha test sit behind one flag test
  (bit 15, set per batch: no alpha test and A is 31), both out of line, so the modulates and the store are one block
  with no taken branch (the model does not price taken branches: 1 cycle less; two taken branches a group fewer); the
  lit direct-textured opaque kernels store a full group with st4 from the modulates' bytes instead of four zips and
  stp (with the flag test 02000 129 -> 125, white lines 117 -> 115), and so do the flat textured ones whose depth
  words are in registers, through v24-v27 (02010 128 -> 122, 22010 119 -> 114). cycles.py, 90 s profiles against
  e02a298's: stress ROM L4 at 2x, librast 14.75 M -> 14.38 M cycles a frame (02000 4.69 -> 4.58, 02100 2.86 -> 2.72,
  02010 2.31 -> 2.24, 02110 1.46 -> 1.41); fog and edge marking S4 at 2x, 04010 3.42 -> 3.30. The 2x walker's span
  setup (`walk.c`'s walk_spans) then takes the left/right swap a step of 4 lines at a time when all 4 agree (the
  winding decides which chain is left; only the lines where the chains meet differ): no selects, and without a swap no
  stores of the left arrays besides x. Stress ROM L4 at 2x: walk_polygon_4x 1.75 M -> 1.62 M cycles a frame (its
  costliest block, the span setup's step of 4 lines, 58 -> 36 cycles; 4% of the steps mix); an edge's first
  coefficient lanes as one vector product and sum (the same bits, walk.c says why): 1.62 M -> 1.59 M. `f_run`'s batch
  splitting (runs of lines with pixels, at most 512 together; a chain of dependent loads and compares, ~10 cycles a
  line) is 8 lines a step as NEON prefix sums (`batch_end`, `ut/batch_end` checks it against the loop it replaced):
  f_run 0.70 M -> 0.64 M a frame, at 3x 0.83 M -> 0.68 M (the same batches). The 3x resolve's edge marking (`hr.c`
  edge_lines) first screens each block of 16 pixels of its two lines on the polygon ids alone: a block whose neighbour
  pairs all have the same id has no edge and is left as it is (96% of the blocks in the edge-marking scene S4), the
  ids read from byte planes of the attribute lines' top bytes made once per bin (edge_tplanes, ldr q and uzp2).
  Modeled A55 cycles a frame, S4 at 3x: the edge marking 2.38 M -> 0.95 M (edge_lines 0.63 M + edge_tplanes 0.32 M),
  the frame 20.98 M -> 19.11 M; `ut/hr_ab` against e02a298: the same output. The 3x walker stops a chain at its window
  (`EDGES_LINE_CAP` in `spec/edges_impl.h`, hr.c only): a chain that goes down, up and down again (a self-intersecting
  polygon) covered lines twice and ran on in the span arrays, past 64 entries the right chain's overwrote the left
  chain's first entries and past 128 lines its perspective coefficients ran out of the span block (into the heap: what
  made `ut/hr_ab`'s two copies of the same hr.c hand the kernels different spans for a few polygons in 5000). Within
  the window the spans the kernels read are the same (only span array 10's scratch leftovers differ, and only with
  edge marking off, when nothing reads it); convex polygons never have such chains (9- and 10-vertex polygons with
  repeated vertices may), so real scenes should render as before. `ut/hr_ab` counts the polygons with such chains
  apart against a base without the cap: against e02a298 0 of the others differ (and ~40% of the ~19% random polygons
  with one do). The NEON walker's interpolate_parameters stores its interleaved halfwords with zip1 and str q instead
  of st2 (38 -> 28 modeled cycles a step of 4 lines): stress ROM L4 at 3x, hr_render_polygon_interpolate_edges 2.11 M
  -> 1.94 M cycles a frame (the frame 35.88 M -> 35.37 M); the same bytes (t_edges.c against DraStic's, `ut/hr_ab`).
  The 2x resolves with fog and edge marking are ours too (`res2.c`'s res2_resolve_fx(), dispatched by rast.c's
  resolve_bin): DraStic's drivers called a leaf routine per stage and line (fog weights into a byte array, the
  modulate, edge identify into an edge array, edge mark), each a pass over memory, and the table pass read the block
  back. Now hr.c's NEON fog (weights and modulate in one pass over byte planes) and edge marking (identify and mark,
  two lines a step sharing their compares) run on the context's colour lines in place, the edge marking skipping the
  depth-key compares of steps in which no pixel's polygon id differs from a neighbour's and the fog the modulate of
  steps whose factors are all 0, and the plain resolve's pass (res2_line_asm) writes the block and its table entries;
  DraStic's line structure is kept (bin 0's top line, bin 11's bottom line with line 30's attributes, the gap copies).
  Fog and edge-marking scene S4 at 2x: DraStic's identify 1.42 M, weights 0.50, mark 0.38, modulates 0.51 and the
  table pass 0.18 M cycles a frame become fog 0.78 M (0.89 M without its skip), edge marking 0.57 M (1.02 M without
  its skip) and the resolve pass 0.26 M: the frame 11.05 M -> 9.82 M (-11.1%; profiles of both builds from the same
  session. An older profile of the base build gave 11.54 M, of which 0.5 M was a block outside DraStic and librast
  that later profiles of either build do not show). `ut/t_resolve.c` compares the block, the gap buffers and the table
  entries with DraStic's drivers (all six modes, every bin, polygon ids in rectangles and zero fog tables for the
  skips; it fails mutants of either skip); S4's frame diff 0 bins in 12000 with the compositor's checks 0 differ, the
  stress ROM's 6 in 4800. Third round, the kernels (`kerngen.py`'s keep_dep()): the opaque textured kernels with z or
  w depth whose spare registers all hold colour (the flat paletted 03010 04010 13010 14010, S4's and S7's main
  kernels, and the lit ones, 02000's family) spilled the group's attribute words at the depth test and reloaded them
  for the store (ldp q from the stack, which holds the A55's load pipe 6 cycles, and the pid orr). The pass mask now
  lives in x8 only (the alpha test narrows x8, the store and the partial group take it from there), which frees v28:
  the words stay in v25 v26, and the paletted ones store with st4 from v28-v31 (b's result last, into the alpha
  texel's register). kpath, cycles a group: 04010 140 -> 133 (16 colours 137 -> 130), 14010 148 -> 142, 03010 157 ->
  150, 13010 165 -> 159, 04000 141 -> 136, 14000 149 -> 145, 02000 125 -> 122, 12000 136 -> 133; the hi-res set's the
  same. `ut/kern_ab` (new) runs every nearest-filtering kernel of both sets, base and worktree, on the same random
  batches (spans, depths, textures, palettes, colours, flags) and compares everything they write: against 94f9689 0 of
  34000 batches differ (it caught a first version's scratch-register clash in the lit paletted kernels' alpha stage);
  the scene cycle's differing bins as DraStic's renderer's at every checkpoint to 84000 bins, S4 and S7 0 differ, the
  stress ROM's 9 bins in 7200. cycles.py, 90 s profiles against the second round's: the field scene S7 at 2x, 14010
  1.36 -> 1.31 M cycles a frame (librast 2.62 -> 2.57 M); the stress ROM L4 at 2x, 02000 4.58 -> 4.52 M (librast 14.16
  -> 14.10 M; the unchanged kernels and the walker the same to 0.01 M); the fog and edge-marking scene S4 against a
  base profile from the same session (under load 90 s reach fewer frames, and S4's 3D load varies with the span:
  compare the instructions), at 3x h04010 7.68 -> 7.15 M (instructions 6.47 -> 6.07 M, kpath's 5% fewer a group and
  slightly less work), librast 16.44 -> 16.06 M; at 2x 04010 and 04000 together 4.34 -> 4.16 M, librast 7.25 -> 7.06 M
  (1747 and 1027 frames). Third round, 3x: hr.c's fog (fog_line) tests a step's 32 depths against the fog offset
  before the rest of the factors: when the density table starts at 0 (as fog tables do) and every depth is at most the
  offset, every factor is 0 (d - off saturates to 0, so i = 0, f = 0 and w = table[0]) and the step is its colours
  with the fog flags cleared, without the weights' tbl and multiplies and the modulates; 73% of the fogged steps of S4
  at 3x (and nearly all of those whose factors are all 0). Modeled A55 cycles a frame, S4 at 3x: fog_line 2.12 M ->
  1.49 M (a skipped step 165 -> 89 modeled cycles with full fog, 136 -> 86 alpha only; the others 3-4 more);
  `ut/hr_ab` (its fog and resolve cases now with zero-first tables and steps below the offset, failing mutants of the
  test) against 94f9689: the same. The 3x downsample (hr_downsample) passes uniform steps through: when each of a
  step's four triples has the same r, g, b in all 9 of its pixels (channels at most 63) and every alpha is 31, the
  opaque case's outputs are those colours with alpha 31 (counted in the simulator: 65% of the steps of the field scene
  S7 at 3x, 15% of S4's; the stress ROM's 42% come in short runs). The test (the 9 vectors' OR beside their AND, the
  opaque test in the same max) makes a step that is not uniform 90 -> 111 modeled cycles and a uniform one 90 -> 63,
  so after a step that is not uniform the next 7 go without it (DS_SKIP; 3 modeled worse on S4, the same on S7).
  Modeled A55 cycles a frame at 3x, hr_downsample: S7 1.11 M -> 0.93 M, L4 1.11 -> 1.10, S4 1.38 -> 1.39 (a verifier's
  profile with fewer steps of the general case than the base's: about 2% more on the same mix, against S7's 17% less);
  `ut/hr_ab` (its downsample test now with uniform blocks at random and in runs, with flag bits, channels of 64 and
  more and one bit off, failing mutants of the test) against 94f9689: the same. The 3x edge marking's id screen
  (edge_lines) takes two blocks of 16 pixels a step while no block is forced (one max and branch for both; a pair that
  does not pass is taken a block at a time): S4 at 3x, edge_lines 0.63 M -> 0.55 M (a screened pair ~55 modeled cycles
  instead of ~68); `ut/hr_ab`'s polygon-like contexts fail mutants of it. The NEON walker's interpolate_parameters
  (spec/edges_impl.h) takes its products as smull and add (clang made each a mov of the base and an smlal): 29 -> 27
  modeled cycles a step of 4 lines; stress ROM L4 at 3x, hr_render_polygon_interpolate_edges 1.94 M -> 1.90 M
  (t_edges.c against DraStic's; `ut/hr_ab`, whose base side now takes the base's headers too: the same). The four
  changes against the second round's profiles, net of `[other]` (whose share varies run to run, as does the scenes'
  mix in shorter runs): S4 at 3x 18.80 -> 18.08 M a frame (fog -0.63, edge marking -0.08), S7 at 3x the downsample
  -0.18 M a frame; 2x does not use hr.c (S4's frame diff 0 bins in 7200, the compositor's checks 0 differ). Third
  round, the 2x walker's edges (`walk.c` walk_edge): a step that continues tests first and computes the next 8 lines'
  weights among its stores (an edge's last step computes none), which fits in registers with z as one vector and a
  step (was four and a step) and x as one and a step, and with the products in fewer instructions: w's (s64)dw q >> 15
  as sqdmulh of q << 16, s and t's bits 15..30 as addhn of sqdmull (2 q d) and (s_a << 16) + 0x1000. Both saturate
  only for q = -32768 with dw = INT32_MIN or d = -32768, so edges with such deltas take the former products
  (`t_walk.c` now makes such edges; it fails either fast form forced on them). The setup takes the vertices' z, c, s,
  t halfwords as vectors and the colour channels of both vertices in one (no scalar unpacking and lane inserts), the
  reciprocal table's bound is a select and walk_chain is inlined once per depth source: the setup is one basic block
  with the x and z setup. Modeled A55 cycles of a z edge: setup and first step 188 -> 160, a continuing step 90 -> 68
  (65 instructions: now issue-bound). walk_polygon_4x, cycles a frame (90 s profiles): stress ROM L4 at 2x 1.59 M ->
  1.43 M, S4 0.41 -> 0.37, S7 0.14 -> 0.12. The same span arrays as DraStic's walker (`t_walk.c`), the scene cycle's
  differing bins as DraStic's renderer's at every checkpoint reached (to 93600 with deferred shading), the
  compositor's checks 0 differ, S4 0 bins, the stress ROM's 9 in 7200, the single scenes as 94f9689's build.
  Third round. The 2x fog computed the factors of ~4100 steps of 32 pixels a frame on S4 only to find them all 0 (82
  modeled cycles a step): the pixels before the fog offset. A depth screen now finds most of them first: res2.c's
  fog_zero_b2() derives once per bin, from the fog table, its deltas, the offset and the shift, a bound on the
  attributes' depth byte (bits 16-23) at or below which every factor is 0 (its comment has the derivation; S4: the
  steps before the offset), and a step whose fog-flagged pixels are all at or below it clears the flags without the
  factors (its attribute ld4 issued with the colours', before the flag test). Fog and edge marking S4 at 2x, modeled
  A55 cycles a frame: fog_line2x 0.78 M -> 0.60 M (the frame net of `[other]` 9.62 -> 9.42 M). `t_resolve.c`'s fx test
  adds tables zero up to a small delta and depths in a band about the offset (a bound one too large fails it); S4's
  frame diff 0 bins with the compositor's checks 0 differ, the scene cycle's bins as before. The 2x edge marking's
  screen (edge_lines2x: ~5600 of ~6100 steps of 16 pixels of two lines a frame on S4 have no id edge) read the six
  attribute blocks it compares by ld4 (~10 cycles of the load pipe each) for their top bytes alone: it now reads the
  four lines' words by single ldr q (one asm block: clang pairs them into ldp q, 6 cycles each) and takes the top
  bytes with uzp2, the right neighbours' plane the same shifted by one with the next pixel's byte; only a step with an
  id edge loads the full planes by ld4. A skipped step 82 -> 61 modeled cycles; S4 at 2x: edge_lines2x 0.57 M -> 0.50
  M cycles a frame, the frame net of `[other]` with both changes 9.62 -> 9.34 M. `t_resolve.c` (its rectangles of
  polygon ids; a screen that drops the next pixel's id fails it), S4's frame diff 0 bins, the scene cycle as before to
  55200 bins with the compositor's checks 0 differ.
  Fourth round, the render threads' bins (`binsched.c`, new): DraStic renders a hi-res frame on N threads (3 in the
  simulator: the thread that calls update_frame_3d_4x, the emulation thread with threaded_3d=0, and two helpers it
  wakes), each a fixed set of bins (first, first + N, ...), so the frame waits for the thread whose bins hold the
  heavy part of the scene. Each bin's thread CPU time (`RAST_SCHEDSTATS`; the simulator's threads share loaded host
  cores, so the times are replayed on a core per thread): the slowest thread's work is 1.07 of the mean thread's on
  the stress ROM L4 and the fog scene S4, at 2x and 3x, and 1.19-1.20 on the field scene S7 (its largest bin ~65% of a
  thread's share). The threads now claim the bins from a queue per frame, longest first by each bin's time in the
  previous frame (list scheduling, LPT; a claim is an atomic or of the bin's bit, so each bin goes to exactly one
  thread and one that starts late takes fewer); comp.c's wrapper on update_frame_3d_4x opens the queue before DraStic
  wakes the threads and closes it after they all returned. Slowest thread / mean thread, fixed sets -> queue (the
  order it took; by the previous frame's CPU times in brackets): L4 2x 1.070 -> 1.058 (1.048), S7 2x 1.190 -> 1.099
  (1.093), S4 2x 1.066 -> 1.057 (1.051), L4 3x 1.070 -> 1.058 (1.046), S7 3x 1.200 -> 1.119 (1.110), S4 3x 1.072 ->
  1.061 (1.050): the 3D's critical path -7% on S7, -1% on L4 and S4 (the summed cycles do not change). Ordered by each
  frame's own times the queue would reach 1.02 on L4 and S4 and 1.08-1.09 on S7 (the bins' times vary from frame to
  frame, in the simulator at least; an order by the bin lists' polygon counts, or by the previous times scaled by
  their change, does no better: S4 2x 1.059 and 1.064 against 1.063), and the best fixed split of whole bins 1.01
  and 1.06-1.07 (S7's largest bin); a split that is the best for the previous frame's times does worse than the queue
  (S7 3x 1.16, with stealing a set's last bins 1.13). RAST=diff keeps DraStic's sets (its quirk pixels read scratch
  memory the thread's earlier bins left; `RAST_SCHEDDIFF=1` claims there too, each claimed bin through both
  renderers alone), `RAST_SCHED=0` keeps them everywhere. The same output: the scene cycle with `RAST=diff
  RAST_COMPCHECK=1` gives DraStic's renderer's differing bins at every checkpoint reached (to 31200 bins; with
  `RAST_SCHEDDIFF=1` to 24000), the compositor's checks 0 differ; frame by frame (`RAST_SCHEDHASH`) the output frames
  with the queue equal those with the fixed sets (L4 2x, 507 frames, the compositor's checks 0 differ); 3x runs of
  S2, S4, S7 and L4 without signals. (`ut/hr_ab/run.sh` copied no base headers: git ls-tree's paths are relative to
  the directory of -C; now --full-tree, so the base side builds with the base's headers as its comment says.)
  Then render threads of librast's own (`RAST_WORKERS=k` / `DSFLIP_RAST_WORKERS`, default 0, only with threaded_3d
  off): with threaded_3d=0 the emulation thread is one of DraStic's 3 render threads and a four-core handheld's
  fourth core is left to the rest, so a worker with a context of its own claims bins beside them. The first DraStic
  thread of a frame to start (DraStic wakes its threads after binning the frame's polygons) copies its context header
  into the workers' and wakes them with the frame's generation; a worker claims by compare-and-swap only while the
  claim word holds that generation (a late one takes nothing of the next frame), and DraStic's threads, once no bin
  is left, wait for the bins still being rendered (a short spin, then a futex), so the frame is complete when they
  return. Modeled as above (the queue on a core per thread, against DraStic's fixed sets and mean thread): one worker
  takes the slowest thread from 1.19 to 0.88 of the mean on S7 at 2x and 3x, and from 1.07 to 0.82 on L4 at 2x: the
  3D's critical path -23 to -26% if the fourth core is free (two workers, on more cores: L4 at 3x 1.07 -> 0.72);
  off until a device A/B shows that (an audio or display thread there would share it). The same output: with workers
  the output frames equal those with the fixed sets frame by frame (`RAST_SCHEDHASH`; one worker: L4 2x 270 frames
  with the compositor's checks 0 differ, with deferred shading 527, S7 3x 922; two workers: L4 3x 209), the scene
  cycle's `RAST=diff RAST_DEFER=1` (which keeps the workers off) DraStic's renderer's differing bins to 36000 bins
  with the fixed sets and to 55200 with `RAST_SCHEDDIFF=1`, `ut/t_comp_hook.c` passes; binsched.c's own cost 0.003 M
  modeled cycles a frame (L4 2x).
  Fourth round, the walk: the 3x walker (hr.c's hr_polygon) ran edges_impl.h's routines one after another, each a
  pass over the chain's lines through the scratch (the float coefficients, the Q15 steps, then w, the parameters and
  x/z), and the span setup and markers 8 lines a step; it now runs the 2x walker's fused routines (walk.c's, moved to
  `walk_impl.h`, a template for both layouts: the array stride, the x clamp, the reciprocal table, the 3x chain cap
  and hr.c's edge-marking x rule, + 1 unless bit 15 or x >= 768; `RAST_WALK=0` keeps edges_impl.h's). Modeled A55
  cycles a frame, stress ROM L4 at 3x: the walk (interpolate_edges 1.90 M, interp_x 0.65, setup_spans 0.58,
  hr_render_list 0.53) 3.66 M -> 2.05 M (hr_render_list, all of it inlined; f_run's 0.68 M the same workload), the
  frame net of `[other]` 33.78 -> 32.35 M (-4.2%); the fog and edge-marking scene S4 at 3x 1.01 M -> 0.53 M. The 2x
  code is the same instructions. `ut/hr_ab` against 4f63ae7: 0 of 50000 polygons differ in what f_run receives (it now
  hashes span array 10, the edge markers, only with edge marking: without it nothing reads the array, and the former
  walker left its scratch there; mutants of the 3x x rule, the cap and the s, t rounding fail it); the 3x scenes S7,
  S4, S2 and L4 run (no signals). Why the walk is not shared across bins: each 2x bin restarts an edge's float
  accumulators at the bin's first line (n0 = 0 B + k A, DraStic's skip), so a line's weights depend on the bin that
  draws it and a walk continued from the bin above would round differently (exact only while every sum stays below
  2^24); what is the same for all bins of a polygon is its setup (the vertices, an edge's constants, f_run's and
  asm_batch_for's per-polygon work: about 700 of the ~1900 modeled cycles a polygon and bin costs on L4), but L4 has
  ~505 polygons a frame in 1166 polygon-bins (2.3 bins each), and the three render threads take every third bin, so a
  per-thread cache would almost never hit: sharing needs a per-frame table across the threads (Next). At 3x f_run's
  batch splitting (batch_end, a chain of prefix sums, ~40 modeled cycles a step of 8 lines) ran over every line of
  nearly every polygon-bin to find one batch (up to 4096 pixels): `one_batch` first checks in one pass of independent
  steps (sums and mins) whether the lines with pixels are one run that fits, the tips of width 0 skipped
  (`ut/batch_end` checks it against the loop too): L4 at 3x f_run 0.68 M -> 0.61 M. At 2x half of L4's polygon-bins
  are more than one batch of 512 and the test cost more than it saved (0.64 -> 0.73 M): 3x only.
  Fifth round, the frame pipeline: the fog's zero step at both scales (`hr.c` fog_line, `res2.c` fog_line2x). Most
  steps of a fogged frame have every factor 0 (the pixels before the fog offset: 84% of S4's steps at 3x), and each
  cost ~80 modeled cycles: two ld4 of the colour planes and the flag test, two ld4 of the attribute planes and the
  depths' u16 compare, and + st4. Now the step's 16 words come by ldr q at immediate offsets (asm: single loads, no
  ldp), the attributes' depth bytes are gathered by tbl (a cycle, whatever the table's size, in llvm-mca's A55) and
  screened first against `fogzero.h`'s bound (res2.c's fog_zero_b2(), now shared: any table, offset and shift; every
  pixel, since an unflagged one has factor 0 whatever its depth), and a zero step clears its flags by word (and + 8
  str): 53 modeled cycles. A fogged step then gathers its alpha planes (the flag test), the depths by shrn, the
  colour planes only when it fogs, and stores by st4 (~171 modeled cycles against 160); the fog colour is broadcast
  where the modulate needs it (four constants held across the step made clang spill a colour group). Modeled A55
  cycles a frame: S4 at 3x fog_line 1.51 -> 1.19 M (13.4 K steps a frame in both profiles); S4 at 2x fog_line2x
  0.59 -> 0.50 M. `ut/hr_ab` against 4f63ae7 (fog 2000, resolve + downsample 400, walker 10000 cases: the same),
  `t_resolve.c`, S4's frame diff 0 bins in 4800 with the compositor's checks 0 differ, the scene cycle's differing
  bins as DraStic's renderer's at every checkpoint reached (to 19200 bins in 330 s on a loaded host), the compositor's
  checks 0 differ.
  Then the edge marking's id screen at 2x (`res2.c` edge_lines2x) on per-bin planes, as hr.c's at 3x: the bin's 32
  attribute lines' top bytes once as byte planes (edge_tplanes2x, a buffer per render thread, with a line of the
  clear attribute's byte for bins 0 and 11's neighbour), and the screen on them, two blocks of 16 pixels a step while
  no block is forced (56 modeled cycles a step of 32 pixels against 61 a block from the words: 16 ldr q and 12 uzp2).
  The plane pass gathers each four words' byte 3 by one tbl (a cycle in llvm-mca's A55) from 16 ldr q a step of 64
  pixels, 13 modeled cycles a step of 32 against the two levels of uzp2's 22, which the 3x edge_tplanes takes too; and
  the last block of a pair, whose right neighbours are the clear attribute, is screened like the others at both scales
  (its top byte in the plane's place; it was always taken in full). Modeled A55 cycles a frame: S4 at 2x edge_lines2x
  0.50 -> 0.27 M plus the planes' 0.10 M (-0.13 M); S4 at 3x edge_tplanes 0.30 -> 0.19 M (edge_lines 0.53 -> 0.55
  M: the base profile's 76 frames are another part of the scene; the screened last block is ~1% of its steps).
  `ut/hr_ab` against 4f63ae7 (two seeds; the polygon-like contexts' rectangles end at and beside block boundaries and
  at the right edge), `t_resolve.c` (its edge drivers on rectangles of ids against DraStic's), S4's frame diff 0 bins
  with the compositor's checks 0 differ, the scene cycle's differing bins as DraStic's renderer's at every checkpoint
  to 81600 bins (6845 tables checked, 0 differ).
  Fifth round, overdraw (`defer.c`, the deferred kernels). Measured first, per bin (a C NEON build's RAST_STATS): the
  stress ROM writes 1.5 opaque pixels per covered one (1.25-1.75 in every bin), the field scene S7 1.0 everywhere, the
  fog and edge-marking scene S4 1.15 (1.0 in 75% of its bins, up to 2.25 in its heaviest); polygons whose depth test
  fails a whole group are already cheap in the one-pass kernels (the lazy loop: ~4% of 02000's groups on the stress
  ROM), so an exact coarse depth rejection has little left to skip. Deferred shading, modeled (all bins deferred, the
  stress ROM at 2x), cost 14.61 M cycles a frame of librast against 13.95 M in one pass: its visibility kernel v00
  2.37 M (five blocks a group, ~36 cycles a full group and ~20 more for the 20% partial ones) and the shade kernels
  slower a group than the one-pass ones (s200 143 kpath cycles against 02000's 122: none of the third round's
  registers). Now the untextured visibility kernels with z or constant depth (v00 v20, `kerngen.py`'s vis_group())
  take a group in one block with no branch but the loop's: the test's mask merges the attribute words and the
  polygon's index into the loaded ones with bit and both are stored for every group (29 modeled cycles a group, every
  group; v00 2.37 -> 2.02 M a frame); the lit direct-textured and lit paletted shade kernels keep the vertex colour in
  registers as the one-pass ones do (`roles()`'s vc and vs, the index, fog bit and A in the depth's v6-v9): kpath s200
  143 -> 113, s100 160 -> 131, s400 153 -> 123, s300 170 -> 144 (s200 3.27 -> 2.72 M a frame); the owner buffer is no
  longer cleared after every flush (32 KiB a bin at 2x: the indices run on between flushes, and it is cleared when
  they would reach 0xffff), and the queue copies only the polygon's lines of the span arrays the shade pass reads (in
  defer_poly itself, 16 bytes a step, instead of libc's memcpy of the whole block: defer_poly 0.05 -> 0.21 M a frame
  on the stress ROM with bilinear filtering, the memcpy's cost gone from outside librast; a wash). The stress ROM all
  deferred, same-session profiles (the opaque kernels, f_run and the queue, M cycles a frame): 2x 7.70 in one pass,
  7.58 deferred (break-even at overdraw 1.5: the shade pass saves what the visibility pass and the second setup
  cost); 3x 15.35 -> 14.71 (a line holds 1.5 times the groups to share its setup). A per-bin choice
  (`defer_bin()`, `RAST_DEFER=2`, both scales: a bin is deferred when its opaque span pixels of the frame before,
  which f_run() counts, are above twice its pixels) takes S4's heaviest bins (overdraw 1.5-2.25 at 2x) and leaves the
  stress ROM's (depth complexity 1.5-2.0, 2x 7.77, 3x 15.20) and S7's (none of its bins deferred, 1.42 M)
  in one pass; but on S4 it loses: 2x 4.76 -> 4.88 M, 3x 9.98 -> 10.21 M (with a threshold of 1.5
  more): the depth complexity does not tell polygons drawn back to front from front to back, and S4's flat paletted
  kernels save little a group (kpath 133 in one pass, 122 shaded). So with nearest filtering the default stays one
  pass, the per-bin choice an option (Next: the overdraw itself as its measure); bilinear filtering (deferred by
  default) takes the cheaper visibility pass: the stress ROM 2x, the opaque kernels, f_run and the queue 14.63 ->
  14.42 M a frame (v00 2.37 -> 2.03). Exact: `ut/kern_ab` against 4f63ae7 (the deferred kernels of both sets: 0 of
  85000 batches differ, seeds 7 and 11; 0 of 51000, seed 13), the scene cycle with `RAST_DEFER=2` and the
  compositor's check: DraStic's renderer's differing bins at all 41 checkpoints to 98400 bins, 0 differ in the
  check; with `RAST_DEFER=1` (every opaque polygon deferred) at every checkpoint to 43200 bins (a loaded host); 3x
  has no DraStic reference: `RAST_DCHECK=1` (new, hr.c) renders every bin's opaque list both ways from the same
  context and compares colours, attribute words, ids and the fog flag (with `RAST_DEFER=2`: stress ROM 1200 bins,
  S4 1200, S7 6000: 0 differ), `ut/hr_ab` against 4f63ae7 PASS, the 3x runs of S7, S4, S2 and the stress ROM with
  `RAST_DEFER=2` (0 signals).
  Fifth round, the kernels: software-pipelined group loops (`kerngen.py`'s pipe()). Each group of 8 pixels was one
  chain of dependent stages (depth test, reciprocal weights, coordinates, gather, modulates, store) and the in-order
  core overlaps only what interleaves in program order. The hottest kernels (02000 02010 04010 12000 14010 and their
  hi-res twins) now have a second group loop for the lines whose flags allow it (perspective, no depth-equal test,
  no white line, no alpha stage: pipe_check(), once per line at the lazy entry 27:, or at the line's start with w
  depth): a step stores the current group while the next group's head (its depth test, weights and steps) runs in
  the same block. The registers come from constants the colour stage now loads where it uses them (the line's ub vb
  b0 splats from the stack, the texture masks and pid from the kernel arguments; z depth's 8-step register freed by
  taking the steps as two 4-step additions, the same u64 sums), two copies of the step alternate the current and the
  next group's registers, a full group whose next group passes falls through (one ccmp tests both masks), and
  `kernsched.py`'s climb_loop() orders the loop's steady path as a whole (a hill climb on its machine model over the
  whole loop, 24000 evaluated moves a loop, deterministic: the per-block list schedules alone gave 02000 106 cycles
  a group, the climb 93). kpath (`loop=51`, two groups a pass), cycles a group: 02000 122 -> 93, 02010 122 -> 90.5,
  04010 133 -> 98.5 (16 colours, `loop=81`: 130 -> 92.5), 14010 142 -> 100 (98), 12000 133 -> 96; the hi-res set's
  the same; every other flag path of these kernels, and every other kernel, as before (the depth-equal path too: a
  first version tested the line's flags at 28:, which the depth-equal head joins every group). `ut/kern_ab` against
  4f63ae7 (its batches now with the pipelined loops' flags forced on a quarter, lines of 1-25 pixels at the buffer's
  edge and groups that pass or fail whole): 0 of 255000 batches differ (three seeds, 500 batches a kernel, 90157
  with the pipelined flags); the scene cycle's differing bins as DraStic's renderer's at every checkpoint to 60000
  bins (a 600 s run) with the compositor's checks 0 differ, with deferred shading to 40800, S4 0 bins in 7200, the
  stress ROM's 3 in 2400 (one checkpoint under the load of the time), the four 3x runs without signals. Generating
  the kernels takes about 2.5 minutes (the climb; `KERNSCHED=0` skips scheduling and climbing).
  Then the lit paletted kernels (04000 14000 and their hi-res twins, keep_dep()'s "litp"; S4's second kernel) got
  the pipelined loop too: their spare registers hold the vertex colour and its modulate scratch, so the next group's
  head takes v13 v14 v12 (the line's ub vb b0 splats, loaded from the stack where the colour stage uses them) for
  its results and v23 and v10 (the fog bytes and the r0 splat, from the stack too) for its scratch, the weights'
  second half and reciprocal scratch doubling as the depth test's two word registers (as 14010's head did); one
  register set serves both palette forms. kpath, cycles a group: 04000 136 -> 108 (16 colours 133 -> 98), 14000 145
  -> 106 (142 -> 95); `ut/kern_ab` caught the first version's register clash (with w depth the modulate scratch is
  v9 and v22 keeps pid << 24, which the loop's vb load overwrote: 14000 alone, 6 of 68000 batches). `kernsched.py`'s
  model got two figures of llvm-mca's A55 it lacked (the pipelined loop's path through llvm-mca's timeline and the
  model side by side, instruction by instruction): umov's result takes 4 cycles, not 3 (the texel gather's eight
  umov -> ldr pairs), and st2/st4 hold the store pipe a cycle a register (the full group's st4 delayed the attribute
  words' stp q 4 cycles the model did not see); it now matches llvm-mca on the pipelined loops within a cycle a pass
  (02000 182.7 vs 184.0 cycles for two groups, before 171.7). Every block rescheduled on it: the 56 bilinear kernels
  with clamped textures (T 1 and 3; 28 a set) 3-5 cycles a group faster in kpath (01001 436 -> 431, 03101 525 ->
  522), the others the same; the pipelined loops re-climbed within the climb's run-to-run noise (1-2 cycles a group:
  02000 94, 02010 90.5, 04010 100/90, 12000 93.5, 14010 104/96.5). The climb's inner loop (Machine.when/issue)
  without generators: the same schedules, 1.5x faster; generating the kernels takes about 2.3 minutes with 11
  distinct loops. Modeled A55 cycles a frame (90 s profiles of this build and of 4f63ae7's, same session): stress
  ROM L4 at 2x, 02000 4.54 -> 4.03 M (-11%; IPC 0.92 -> 1.15), the frame net of [other] 17.66 -> 17.12 M (-3.1%); at
  3x h02000 9.69 -> 8.41 M (-13%), the frame net 33.78 -> 32.34 M (-4.3%); fog and edge marking S4 at 2x, 04010 3.21
  -> 2.50 M (-22%), 04000 0.95 -> 0.80 M (-16%; instructions +4-7%: a different span of the scene), the frame net
  9.16 -> 8.27 M (-9.7%); the field scene S7's 14010 (white lines: the next step) the same. `ut/kern_ab` against
  4f63ae7: 0 of 323000 batches differ (four seeds); the scene cycle's differing bins as DraStic's renderer's at
  every checkpoint to 74400 bins with the compositor's checks 0 differ, with deferred shading to 67200, S4 0 bins in
  12000, the stress ROM's 6 bins in 4800, the four 3x runs without signals.
  Fifth round, 2D: `ds2d.c` (the section "DraStic's 2D scanline routines" below) replaces seven of DraStic's 2D
  routines in a first commit and ten more in a second, each exact (`t_ds2d.c`, RAST_2DCHECK=1 over the 2D and 3D
  scenes); modeled A55 cycles a frame: every 2x / 3x game's conversion 0.62 -> 0.29 M, the field scene S7 frame 4.49
  -> 4.21 M, the 2D scene cycle 3.44 -> 2.82 M (DraStic's 2D routines and ours together).
  Fifth round, the geometry stage: four of DraStic's geometry functions on the emulation thread are replaced through
  the same hook mechanism (`dsgeom.c`, the section below): the command queue's fast paths, the vertex batch's append,
  the vertex block's transform rescheduled for the A55 and the polygon binning visiting only occupied rows; checked
  call by call against the originals in the running emulator (`RAST_GEOMCHECK=1`) and in `ut/t_geom.c`. Modeled A55
  cycles a frame, base -> ours, on the stress ROM L4: the queue 0.314 -> 0.204 M (46.6 -> 30.3 cycles a call, 6740
  calls a frame), add_vertex 0.077 -> 0.050 M, the vertex block 0.058 -> 0.031 M, the y-sort 0.151 -> 0.148 M;
  together 0.167 M a frame less on L4, 0.088 M on S4, 0.063 M on S7.
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
  runs twice: with nearest filtering it was slower (stress ROM, overdraw 1.5: +2.5%; field scene, overdraw 1.0:
  +28%), with bilinear filtering faster on the stress ROM (20.9 against 22.6 M) and still +10% on the field scene.
  After the fifth round (Status) the stress ROM gains 1.5% of the opaque work at 2x and 4% at 3x. It is on
  with bilinear filtering and off otherwise; `RAST_DEFER=0/1` overrides, `RAST_DEFER=2` chooses per bin
  (`defer_bin()`: the bins whose opaque polygons of the frame before rasterized more than twice their pixels).

- **3x internal resolution** (`DSFLIP_RAST_SCALE` / `RAST_SCALE=3`, `src/rast/hr.c`). The scene is rendered at
  768x576 and supersampled into DraStic's 2x frame: every bin at 48 lines x 768 (plus a neighbour line above and
  below for the edge marking), with the vertices' 3x coordinates from a hook on DraStic's perspective transform
  (the same math with the viewport scaled by 3), our own polygon walker (spec/edges.c's routines with the wider
  layout, on walk.c's fused forms: `walk_impl.h`), the same kernels in a hi-res instantiation (rast_kern_h*, strides
  from the kernel arguments; deferred shading and bilinear filtering included), the bin resolve over 768-pixel lines
  and a 3:2 alpha-weighted box filter into the output block. The 2D compositing then sees a normal 2x frame, with
  anti-aliased polygon edges and 2.25 samples per pixel of texture detail. Shadow polygons (mode 3) render on the
  scalar line path with DraStic's stencil rules; DraStic's sprite path (axis-aligned textured quads) goes through the
  general walker; points and lines (1- and 2-vertex polygons) walk like DraStic's. The vertex table holds the DS's
  6144 vertices (an earlier 1568 limit skipped polygons with higher vertex indices: a black sky and missing sparkles
  behind Ho-Oh in HG/SS). The downsample, the edge marking and the fog of the resolve are NEON (unit-tested against
  the scalar versions); fully transparent outputs carry the clear colour, and only the 5-bit alpha counts as coverage.
  The edge marking works in place on the colour lines and loads and stores only steps with a marked pixel (bits
  29-31, which the 2x resolve clears, are ignored by the downsample), both lines' tests of a step before the one
  test; the fog takes two groups a step, two independent chains for the in-order core (S4 at 3x, modeled A55 cycles
  a frame: the edge marking 3.03 -> 2.38 M, the fog 2.75 -> 2.12 M). Changes to these stages are checked against
  an earlier hr.c by `ut/hr_ab/run.sh` (both versions on the same random bins, the output blocks compared).
  The field scene costs 4.9 M instructions a frame at 3x against 1.7 M at 2x: the kernels 2.8 M (the pixels), the
  downsample 0.66 M (four triples a step; groups whose 36 alphas are all 31 or all 0 take fast paths, the same bits
  as the general case), the rest the clear and the resolve over 2.25x the pixels (`RAST_DUMP` also writes the 3x
  frames as `hNNNNN.ppm`).
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
  instructions a call (the kernel 124, `src/rast/compvis.h`: `ld4` for the alpha bytes, `cmtst` + bit weights + an
  `addp` tree for the bitmap, `umin(a, a ^ 31)` for the flag), for any pointer DraStic passes (the 2x frame, the 1x
  line, a BG0HOFS-shifted copy on its stack).
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
  missing table copy (a frame-skipping S4 with `threaded_3d=1` flags only its first copy).
  `tools/rast/ut/t_comp_hook.c` covers that bookkeeping on random frames: the hook's lookup and the wrappers' validity
  rule (rendered with re-marked gap rows, copied, unchanged, 1x, reset; `threaded_3d` on and off) against the
  original, with stubs that write the frames as the disassembly of `update_frame_3d_4x` shows.
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
instructions for a quarter that is all 3D, 27 all backdrop, ~140 mixed. Otherwise it calls DraStic's `select_pixels`
as the original does; blending or brightness flags, no 3D layer (engine B) or another layer mask go to the original
through a trampoline. Exactness: the C ports of the chain are unit-tested against the originals, and the fused pass
against DraStic's `select_pixels` and the ports (`t_composite.c`); `t_comp_hook.c` runs the hook itself against the
original on random engines, scratch areas and quarters through every path, with the check mode on and off;
`RAST_COMPCHECK=1` runs every fused call twice in the emulator (ours, then the original through the trampoline on the
same input bytes) and compares the scratch frame, the planes, the 3D pixels and the engine (`[comp] composite:` counts
every 2 s). Cost per frame (simulator instruction counts, 90 s each): the chain's functions on the stress ROM L4 0.81
M -> 0.39 M (the quarters of engine B, which has no 3D layer, stay with DraStic's chain), on the field scene S7 0.47 M
-> 0.07 M; frame totals 16.6 M (was 17.8) and 3.5 M (was 3.4; the totals move by about 0.5 M between runs, the
per-call costs are exact: about 620 instructions a quarter before, 45 / 30 / 150 after).

## DraStic's 2D scanline routines (`src/rast/ds2d.c`, `ds2d_asm.S`, exact)

DraStic composites every DS line on the emulation thread, so its 2D routines sit on every frame's critical path; at
2x half of the field scene's frame is DraStic's own non-3D code. `ds2d.c` replaces seven of them through rast_hook()
(the entry's first four instructions checked, a jump into librast, the trampoline kept for the check mode), each with
the same bytes and no other effect a caller can see (all their callers are C, so the standard calling convention is
all a replacement keeps; `ds2d.c`'s header lists the callers):

- `render_scanline_color_convert_direct_32_2x_asm` (two quarters' 6-bit planes into a 512-pixel xRGB row, 768 calls a
  frame in every 2x / 3x game): the same zip / st4 loop with `ldr q` at a register offset and the loads a step ahead,
  23 modeled cycles per 32 pixels against 50 (DraStic's odd alpha bytes, `movi .4h #0xff`, kept). Its 1x sibling
  (`_32_1x`, hires_3d off) the same way, 21 against 43; the 16-bit conversions do not run with 32-bit output.
- `render_scanline_select_pixels` (the quarter's layer merge: DraStic's u16 passes binary / binary_scalar through a
  stack line, expand_6bit_split, binary32 / binary32_alpha for the 3D bytes; ~630 cycles a quarter with one layer):
  one pass per 32 pixels in registers, specialised for 1..5 layers and for the 3D bytes; overlapping buffers go to
  DraStic's.
- `render_scanline_color_effects_apply_asm` (the blend of two planar quarters, S4's blending quarters): 32 pixels a
  step from `ldr q`, 63 cycles against 89 per 16.
- `render_scanline_select_blend_enable_asm` and `render_scanline_priority_encode_single_asm`: the same walks with
  `ldr q` / `str q` instead of `ld1` / `st1` of two registers with writeback, skipping layers that change nothing.
- `render_scanline_tiled_span_4bpp_asm` (a 4bpp text BG line, the costliest DraStic routine of the 2D scenes): each
  step's scalar loads (flags, map entries, the four tile rows by `ldr s` + `ld1` lanes) issued during the step before,
  the palettes by `ldr q` + `uzp` instead of `ld2`, the pixels by `zip` + `str q` instead of `st2`; 63 modeled cycles
  per 4 tiles against 94. Its first instruction is an `adr`, so the check mode reaches the original through
  `ds2d_span4_orig` (its first four instructions' effects, then a jump to its fifth) instead of the trampoline.

A second batch (the fifth round's second commit) takes ten more leaves the same way, all called from C:
`render_scanline_palette_lookup_8bpp_asm` / `_12bpp_asm` (8bpp and extended-palette indexes to colours: the
original's iteration is one dependence chain, 26 / 28 modeled cycles per 8 pixels; ours is software pipelined over
two register sets, an iteration's word loaded and its indexes computed while the previous gathers land, 16 / 19;
arguments where that reordering could show, dst just below or over the source or over the palette, take a verbatim
copy of the original's loop), the 8bpp text BG spans (`_span_8bpp_normal_palette_asm`, `_ext_palette_asm`: rows
loaded straight into vector lanes instead of through general registers, zip + str q instead of st2), the double
priority encoder, the blend weights' four setups and the brightness shade (per block as the originals, the mask
bytes by ld1r + tbl, empty mask blocks shortcut). In the check mode the entries that start with an adr reach the
original through an ORIG4 stub of `ds2d_asm.S` (as `ds2d_span4_orig`), and the two palette lookups, whose loops
re-enter the patched entry, through a private executable copy of the whole routine (both position independent in
every instruction; check mode only, nothing is written into DraStic). Per call, modeled cycles (2D scene cycle,
calls a frame): palette_lookup_8bpp 770 -> 540 (132), _12bpp 838 -> 636 (52), span_8bpp 329 -> 276 (105) and
418 -> 356 (48), shade 699 -> 401 (32), double encoder ~330 -> 230 (48); S4: setup_alpha_base 194 -> 85, the double
encoder 70 -> 54 (129 each).

`RAST_2D=0` (or `DSFLIP_RAST_2D=0`) leaves them alone (otherwise a bit mask of the hooks, default all);
`RAST_2DCHECK=1` runs every call twice, ours and then DraStic's (through the trampoline) on the same input bytes, and
compares the outputs with 64 bytes either side (`[ds2d] check:` counts every 2 s, `[ds2dcheck]` lines for a
difference). Exactness: `tools/rast/ut/t_ds2d.c` (each routine against the original on random inputs of any byte
values with guard bytes, the overlap cases, the check wrappers, and the patched entries called through DraStic's own
addresses); `RAST_2DCHECK=1` in the emulator, 0 differences: the 2D scene cycle (4.1 M conversions, 1.9 M spans,
1.6 M merges, 0.28 M blends, 0.8 M blend masks), the stress ROM L4, the 3D scene cycle at 3x with `RAST_COMPCHECK=1`
(0.98 M merges through comp.c's checked composites) and at 2x with `RAST=diff` (DraStic's renderer's differing bins
at every checkpoint), and the 3D scene cycle at 1x (hires_3d off, from a private copy of the run directory: 2.2 M
1x conversions).
Cost per frame, modeled A55 cycles (`cycles.py`, the replaced routines against DraStic's, 90-100 s runs): every 2x /
3x game's conversion 0.62 -> 0.29 M; the field scene S7 0.66 -> 0.32 M (frame 4.49 -> 4.21 M); the fog and blending
scene S4 1.09 -> 0.57 M (conversion, merge chain 0.23 -> 0.18, colour effects 0.18 -> 0.07, blend masks 0.035 ->
0.008, encoder 0.030 -> 0.023); the stress ROM L4 1.17 -> 0.67 M (merge chain 0.48 -> 0.33); the 2D scene cycle 1.32
-> 0.79 M (4bpp spans 0.27 -> 0.19, merge chain 0.32 -> 0.25; frame 3.42 -> 2.95 M).

## DraStic's geometry stage (`src/rast/dsgeom.c`, `geom_tvb.S`, exact)

The emulation thread also runs DraStic's DS geometry engine: the command queue, the vertex batches, their transform
and lighting, clipping, and the binning of the finished polygons for the render threads. With the renderer hooked,
`geom_init()` replaces four of its functions (`DSFLIP_RAST_GEOM` / `RAST_GEOM=0` leaves them alone):

- `queue_geometry_command` (one call per word written to the command ports): the paths that neither run
  process_geometry nor touch the SWAP_BUFFERS flag do the original's stores in its order, without its stack frame;
  every other call goes to the original through the trampoline before anything is written.
- `geometry_add_vertex`: the same for a vertex appended to a batch that is not full, with no matrix update pending
  (texture coordinate mode 3's two dot products included).
- `geometry_transform_vertex_block_asm` (`geom_tvb.S`): the batch through the clip matrix, four vertices a step as
  the original (the group past a multiple of 4 included), its 24 multiply-accumulates in eight independent chains
  instead of eight chains of three, the translation added after the narrowing (the same bits): 130 -> 66 modeled
  cycles a step (llvm-mca, Cortex-A55).
- `video_3d_bin_polygons_y_sort_4x` (the opaque and the y-sorted translucent lists, a frame each): DraStic's three
  passes (per polygon: y range, shading flags, texture lookup, top row; rows down: by bottom y; rows up: into the
  bins) on its own sort arrays, step by step, but visiting only the rows that hold polygons (a byte a row and a bit
  per 8 rows) where the original walks all 385 rows twice.

The callers assume the AAPCS and nothing more (from the disassembly: the JIT's 32-bit store helper saves w12-w18, x30
and NZCV around its call; the C callers keep only callee-saved registers across the calls; each original calls code
compiled outside its unit, so GCC's IPA-RA cannot have narrowed its clobber set), so plain C is a valid replacement.
Modeled A55 cycles (`dev/cycles.py`, 90 s profiles of
4f63ae7's build and this one from the same session; the patched entries' jumps included), the stress ROM L4: the
queue 46.6 -> 30.3 cycles a call (6740 calls a frame: 0.314 -> 0.204 M a frame), add_vertex 47.1 -> 30.6 (1633:
0.077 -> 0.050 M), the vertex block 2071 -> 1107 (28: 0.058 -> 0.031 M), the y-sort 0.151 -> 0.148 M (L4's rows
are nearly all occupied: there the per-polygon work, which is DraStic's, dominates); the fog and edge scene S4:
0.132 -> 0.086, 0.032 -> 0.021, 0.025 -> 0.013, 0.043 -> 0.024 M; the field scene S7: 0.073 -> 0.047, 0.021 ->
0.014, 0.017 -> 0.009, 0.046 -> 0.024 M. Together 0.167, 0.088 and 0.063 M a frame less on the emulation thread
(L4, S4, S7). Exactness: `tools/rast/ut/t_geom.c` runs each replacement against DraStic's original in its process on
random states (queue states at the 0x400-byte limit, every parameter count and the swap flag; full batches and
pending matrices; any matrix and coordinates, the 64-bit sums wrapping; polygons of 0..15 vertices with y beyond
0x180, repeated texture keys, quads' sprite path and stale sort rows, with DraStic's texture_cache_lookup and
render_sprite_check patched to logging stubs so the calls are compared too), then DraStic's entry patched as
`geom_init()` patches it (seeds 1, 3, 5; seven mutants, one per path, fail it). `RAST_GEOMCHECK=1` runs every call
ours handles twice in the emulator, ours and then the original through the trampoline on the same input bytes, and
compares everything either may write (`[geom]` counts every 2 s, `[geomcheck]` for a difference): at 2x over the ten
scenes (60 s each), the scene cycle (330 s) and the stress ROMs L4 and L10 (90 s), 18.6 M calls of the queue, 4.09
M of add_vertex, 24647 of the y-sort and 173276 of the vertex block checked, 0 differ; at 3x (hr.c's vertex hook
active too) the same ROMs, 15.8 M, 3.52 M, 24093 and 156416, 0 differ; at 1x (hi-res off: DraStic's 1x binning, the
same queue and batch) the stress ROM, 8.48 M, 2.02 M and 35296, 0 differ (the 2D cycle's test ROM never writes the
geometry ports). The scene cycle's frame diff gives exactly DraStic's renderer's counts at every checkpoint reached
(to 132000 bins).

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
cycle), with the state the block before leaves; the pipelined loops' steady paths are then climbed as a whole
(climb_loop(), 24000 evaluated moves a loop: nearly all of the 2.5 minutes a generation takes); `KERNSCHED=0 python3
kerngen.py` writes the kernels unscheduled and unclimbed.

## Simulator usage (see `tools/sim/` for setup; `dev/` holds the scripts used during development)

- `RAST=diff` renders every bin with both renderers and compares the output frames.
- `RAST=ours RAST_PDIFF=1` compares per polygon and reports the differing pixels; `RAST_PDIFF_STRICT=1` includes
  the 5-mod-8 translucent quirk pixel (exact), `2` the 1-pixel batches (the known deviation).
- `RAST_PIPE` selects the pipeline: 0 = b0 (stage by stage), 1 = fused scalar, 2 = fused C NEON, 3 = assembly.
- `RAST_DUMP=<dir>` writes frames as PPM; `RAST_STATS=1` prints the opaque overdraw; `RAST_FRAMES=1` prints the
  frame count every 10 frames (for per-frame figures from a block profile).
- `RAST_SCHED=0` gives each render thread DraStic's fixed bins (`binsched.c`); `RAST_SCHEDSTATS=1` (or N: every N
  frames) prints the threads' balance, `RAST_SCHEDHASH=1` a hash of every output frame (runs with the fixed and the
  claimed bins compare frame by frame), `RAST_SCHEDDIFF=1` claims the bins in `RAST=diff` too; `RAST_WORKERS=k`
  adds k render threads of librast's own (with threaded_3d off; not in `RAST=diff`; -1: one, while it pays).
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

- DraStic's geometry stage, next (`dsgeom.c`): geometry_transform_vertexes' lighting (L4 0.24 M, S4 0.09 M modeled
  cycles a frame: per vertex and light a chain of dependent multiplies; the whole function would have to be
  replaced, its batch bookkeeping too), process_geometry_commands' dispatch (L4 0.13 M) and geometry_flush_polygons
  (L4 0.23 M; 12 KiB of code), geometry_clipped_vertex_interpolate_edge (L4 0.07 M; a static function: check its
  callers' register assumptions first). The y-sort's per-polygon work is still DraStic's in C (L4 0.148 M): a
  row's loads first into a local array, to overlap the entries' load chains, modeled worse (0.162 M; clang keeps
  that loop one entry a block). Games feed the geometry engine by DMA into GXFIFO (`queue_geometry_command_packed`
  and `_packed_multi`, not replaced; the test ROMs write the command ports), so the queue's share there is
  unmeasured.
- Device A/B: the same game with the option off and on, from the performance logs (`threads_avg` in the summary).
- The render threads' bins (`binsched.c`): the queue's order is the previous frame's bin times; S7's largest bin is
  ~65% of a thread's share, so no order of whole bins gets S7 below ~1.06 of the mean thread. Splitting the heaviest
  bin (two halves of 16 lines on two threads) needs the resolve, whose edge marking and fog read the neighbour lines,
  across two contexts. On the device: how late the helpers start after DraStic wakes them (the queue absorbs it), and
  an A/B of `RAST_WORKERS=1` with threaded_3d=0 (frame times, and the audio): whether the fourth core is free.
  `RAST_SCHEDSTATS=1`'s "update wall" is update_frame_3d_4x's wall time a frame (DraStic's texture cache and
  binning, the render, the gap passes) on the device as in the simulator: with `RAST_SCHED=0`, 1, and 1 with
  `RAST_WORKERS=1` it is the A/B's measure; its slowest-thread figures there are the device's own bins' times.
  `cpugov.c` sizes the clock by DraStic's busiest thread and the main thread's heaviest frame in CPU time: with the
  queue the emulation thread takes over the bins of helpers that start late (more CPU time, less waiting), with
  workers it waits more and renders less; its figures shift with them. `RAST_WORKERS=-1` lets one worker decide for
  itself: windows of 30 frames with and without it by turns, then 8 windows of the one whose updates took less wall
  time (`RAST_SCHEDSTATS` logs each choice); in the loaded simulator its choices are noise (the same output frames as
  the fixed sets': L4 2x 451, S7 3x 922), on the device they would show whether the fourth core is free.
- The 2x fog (`res2.c` fog_line2x; its zero step is now 53 modeled cycles, Status) is bound by the FP pipe where it
  fogs: four channels' smull/smull2/shrn/shrn2 per 16 pixels and the weights' two tbl2; a fogged step's two reductions
  in series (the depth screen, then the flag test) cost it ~10 cycles against the planes' form. DraStic's gap passes
  (rows 32k-1 and 32k, 0.08 M) still run. hr.c's 3x fog skips the steps below the fog offset when the table starts at
  0 (Status); res2.c's fog_zero_b2() bound (any table, the offset and the shift) now screens hr.c's fog_line too
  (Status, fifth round), and hr.c's edge marking could take res2.c's skip of the key compares (S4 2x: edge marking
  1.02 -> 0.57 -> 0.50 M with the top-byte screen, fog 0.89 -> 0.78 -> 0.60 M with the depth screen). The 2x edge
  screen now reads a bin's top-byte planes made once, as hr.c's at 3x (fifth round); its skipped pair of blocks (56
  modeled cycles: 12 loads, the xors and ors, one max) is near the issue limit.
- Deferred shading (fifth round, Status): a better per-bin predictor than the depth complexity, which cannot tell
  polygons drawn back to front (overdraw) from front to back (cheap one-pass failures): the overdraw itself (written
  and owned pixels: the visibility and shade kernels would return counts instead of their any-pass flags). The shade
  pass without f_run's re-setup (keep the kernel arguments and the batches in the queue entry; f_run 0.64 -> 0.94 M a
  frame with every polygon deferred) and the visibility pass's per-line z setup (26 cycles a line, 0.27 M); w-depth
  and textured visibility kernels in vis_group()'s form; 64-bit texel-pair loads in the bilinear gathers.
- The composite's row-level shortcut: when both quarters of an output row are fused (comp.c's all-3D pass, 0.15 M on
  S7; its ld4 form models better than ldr q + uzp), convert the 3D frame straight into the output row (ld4, shl,
  zip, st4: ~30 modeled cycles per 32 pixels against the conversion's 23 plus the planes' ~24, ~0.2 M a frame on
  S7). ds2d.c now owns the conversion hook, so it could take the rows by plane pointer; first prove that nothing
  reads the planes between the composite and the conversion (render_scanline memsets and reuses them in some
  display modes; display capture reads them).
- DraStic's 2D path after `ds2d.c` (the section above): what is left is C. render_scanline_obj_c (6.8 KB, 0.26 M
  on the 2D scene cycle and the largest 2D item of any sprite-heavy game: its per-pixel loops store the colour, the
  index and the attribute byte per sprite pixel) can only be taken whole: DraStic's OBJ renderer re-implemented
  exactly (1D / 2D mapping, bitmap and affine sprites, mosaic, the OBJ window) against the decompiler's C, with
  RAST_2DCHECK over sprite scenes; next render_scanline_bitmap_8bpp / _16bpp (3.5 KB each, 0.06 / 0.05 M, mode 3-5
  games), render_scanline_capture_blended_3d (324 B, 0.04 M when a game captures every frame), the affine tile
  renderers (render_scanline_affine_render_tiles_*_asm, 0.036 M: variable-length runs with overlapping 16-byte
  stores) and render_scanline_bg (0.10 M on the 2D cycle, nearly all of it its horizontal mosaic loops).
  render_scanline_2d (0.10-0.34 M) and render_scanline (0.06 M) are per-line drivers whose cost is prologue and
  inlined NEON (L4: the OBJ lines' bit-15 visibility bitmaps): not hookable without replacing them whole.
- The group loop is one dependence chain per group (depth test, reciprocal, interpolants, gather, modulate; IPC
  ~0.8-0.98 in the model): within its blocks the scheduler is near the model's optimum (a hill climb over its orders
  finds 0.1% more). Across blocks the translucent tail, the alpha stages and the st4 stores are done (Status); the
  reciprocal runs beside the depth test in the group head (head()), only the lazy loop's entry (28:) waits for the
  test. The opaque textured kernels without a dep register now keep the depth words in v25 v26 (the pass mask in x8
  only frees v28; kerngen.py's keep_dep()), and the paletted ones store with st4 (Status); ORing pid in at the depth
  test instead of the store models 1-2 cycles worse. Left: the texel gather's ld2 (7 cycles with nothing
  independent left; ldr q and uzp model 2 worse); the 16-colour gather's bytes through the stack (stp w and ld2 to
  assemble 8 bytes; ORed together in x9 and moved by fmov they model the same). Consecutive groups now overlap in
  the hottest kernels' pipelined loops (Status, fifth round); left for them: the other kernels (03010 13010 with
  clamped textures, whose clamp branches would need a loop per mode; 01000 11000; the lit paletted 04000 family,
  whose spare registers hold the vertex colour; the visibility and shade passes; bilinear, which uses every
  register), the w-depth kernels' lines with 64-bit products, and the loops' steady path itself (FP-pipe bound
  ~73 cycles a group for 02000 in llvm-mca's model, 92 now; the climb's model and llvm-mca differ by a few cycles).
  Hoisting the fall-through code over the
  w7 flag branches does not pay (llvm-mca: +1-8% on most kernels, the hoisted work lands above taken branches; -4-6%
  on the bilinear ones).
- A device A/B of the scheduling (llvm-mca's model is what it is: its integer latencies are 3, the A55's 1-2), and
  of the texel gather's trip through the stack (the texels stored as words, read back by ld2; the addresses now
  leave by umov), which the model cannot price (store-to-load forwarding).
- `ut/hr_ab`'s walker test sees the parameters (fourth round: a mutant of the fused walker's s, t rounding fails it in
  ~half the polygons): the earlier mutant of edges_impl.h passed because both sides then took the worktree's headers.
  The 3x walker is now walk_impl.h's (edges_impl.h's NEON form only with `RAST_WALK=0`, checked by t_edges.c).
- Sharing a polygon's per-bin setup across bins (fourth round, Status): a per-frame table of the per-polygon work
  (the vertex pointers, the edges' constants, f_run's poly_t and the kernel arguments but the per-thread fields:
  ctx, the owner buffer), filled by whichever render thread meets the polygon first and published lock-free (a
  generation word per polygon, release/acquire), would save up to ~700 modeled cycles in ~660 of L4's 1166
  polygon-bins a frame (~0.4 M at 2x), less the copies; the walk's weights stay per bin. Contiguous bins per thread
  (the scheduling's choice) would let a per-thread cache do the same without sharing.
- Tried in the fourth round, not kept: (1) walk_spans with the chains' arrays exchanged per polygon (the forward chain
  into the right arrays when its second vertex lies right of the backward chain's; DraStic's tie rules then take
  r + 1 in the compares, exact: t_walk.c and `ut/hr_ab` pass and fail mutants), so that L4's steps swap no line: the
  step 19 -> 13 modeled cycles, but walk_polygon_4x as a whole did not drop (1.433 -> 1.441 M on L4 at 2x; S7's
  polygons have the other winding anyway). (2) one_batch at 2x (above). (3) At 3x without edge marking, not drawing
  context lines 0 and 49 (nothing reads them: 2 of 50 lines; h02000 9.70 -> 9.38 M on L4 at 3x), but the kernels are
  not strictly line-local: a kern_ab-style test (a batch against the same batch without its first or last line,
  every other line compared) fails h24000 (16-colour paletted, constant depth) and the shade kernel hs400, so a
  trimmed batch would not give 4f63ae7's bytes; find out what crosses lines there first (the 16-colour gather's
  bytes through the stack?).
- 3x: the top vertex for tied vertices; later the hi-res 3D
  layer presented through the dsflip shader instead of downsampled.
  Polygons of 9 and 10 vertices: DraStic's render_polygon_4x takes the walk's ninth vertex as base + 0 and never
  writes a tenth's slot (it walks a stale stack slot; walk.c leaves 10 to DraStic). hr.c's vertex indices for them
  are nibbles 0 and 1 of the group's base sequence (`4 * (k & 7)`: what its former `seq >> (4 * k)`, undefined
  from k = 8, compiled to; ut/hr_ab: the same output on 1.5 M polygons): decide what 3x should walk there.
  The downsample is bound by its loads and the alpha test in the cycle model (uniform steps aside, Status): two
  groups a step, the opaque outputs computed before the test and the loads a group ahead all modeled within 3% of the
  loop. A cheaper uniform test would pay on the stress ROM, whose uniform steps come in short runs. Its three ld3 cost
  about what nine ldr q and nine three-register tbl do (fifth round: the test block models 49 -> 50 cycles, the one
  without the uniform test 37 -> 40: the tbl are a cycle each on the FP pipe, which the tests' 20 one-cycle logic
  operations already fill, and ld3's latency mostly overlaps them); the alpha tests need no deinterleave; the u8
  stage's wrap for 8-bit channels (ut/hr_ab's wide case) must stay. The opaque block is 40 NEON operations at
  throughput 1 (add, mla, two ushll, two uaddw, two sqrdmulh, uzp1, sli per output vector) for its 50 cycles: the
  formula's floor in this form. cf_run's all-3D quarter by ldr q + three tbl a block instead of ld4 models 189 -> 165
  cycles a quarter (0.02 M a frame): not taken.
- The 2x walker (stress ROM L4, 1.43 M cycles a frame): a z edge's setup (127 instructions, 109 modeled cycles) is
  the costliest block. Its first weights' chain is ~60 cycles of latency, but clang's schedule starts the first
  frecpe at cycle ~40 and its uzp1 lands at ~103, behind the colour, x and z work it interleaves (and blocks on);
  without the chain the rest is ~85 cycles. Clang's other schedulers model worse (-misched-topdown/bottomup,
  ilpmax/min), and small source changes move the block by +-10 cycles either way (z kept as z << 2 with uzp2 stores:
  2 instructions fewer a step, 3 cycles more; the vertices' y copied for the chain scan: the scan 18 -> 14 cycles,
  the setup 109 -> 121): a scheduled block (res2gen.py's way) is what is left. The continuing step is issue-bound
  (65 instructions, 68 cycles). `batch_end` is latency-bound (38 cycles a step: ldp, uzp1, three ext + add, the
  compare, fmov): a polygon's prefix sums in one pass, then a compare a batch.
- The texture alpha cache (`tex_min_alpha`, fused.c) keys on DraStic's texture-cache entry and the frame: it
  assumes DraStic does not reload an entry with another texture within one frame. True on everything tested; a
  content signature in the key would make it certain.
- In the simulator, dsscenes-cycle.nds died at its scene 2 -> 3 transition (a SIGILL in DraStic's JIT cache, also
  with our hooks off, since 2026-10-03); regress.sh checks the later scenes one ROM at a time until that is
  understood. In a simulator set up afresh on 2026-10-08 it ran through every scene seven times without it (RAST=diff
  to 91200 bins, the same differing bins as DraStic's renderer at every checkpoint: 47 at 7200, 662 at 14400, 1919
  at 72000). A startup hang is DraStic's own (helpers waiting on locks not yet initialised, ROCKNIXDS
  docs/handoff-local.md: initialize_video_3d creates each render helper before it initialises the helper's mutexes and
  condition variables and clears its flags, so a helper that already waits is lost and the first frame's
  update_frame_3d_4x waits for it at 0x592fc forever; in the fourth round's loaded simulator 5 of 17 runs at 3x hung
  so, with either bin assignment, and none of 22 at 2x); the simulator does not preload libdsflip, which works around
  it: run again. On the evening of 2026-10-08 the scene 2 -> 3 SIGILL came back in every run of every build, at low
  load too, and after the container restarted it was gone again (2026-10-09: to 148800 bins, the same bins as
  DraStic's renderer at every checkpoint): a state of the long-running simulator host, not of the code; restart
  before suspecting a build.
