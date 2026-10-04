# Our own 3D rasterizer for DraStic

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
  13.5 M instructions a frame against DraStic's 16.5 M (a full frame 20.3 M against 22.9 M). Device timings are
  the open question: the kernel's texel gather is scalar like DraStic's, the rest is NEON without DraStic's
  trips through scratch memory between stages.
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
  scalar versions); fully transparent outputs carry the clear colour, and only the 5-bit alpha counts as coverage. The field scene costs 10.3 M
  instructions a frame at 3x against 1.7 M at 2x: the kernels 2.8 M (the pixels), the rest the clear, the resolve
  and the downsample over 2.25x the pixels (`RAST_DUMP` also writes the 3x frames as `hNNNNN.ppm`).
- **Palette lookups with tbl.** 4- and 16-colour textures (the common DS formats I2 and I4) keep their palette
  in four NEON registers and look texels up with `tbl` instead of a dependent load per texel; exact. The
  instruction count hardly changes; the gain is the removed load latency on the handheld's in-order cores.

## How it works

`src/rast/spec/`: DraStic's ~85 raster routines ported to exact C, each unit-tested bit for bit against the
original inside DraStic's process (`tools/rast/ut/`). `src/rast/b0.c` rebuilds DraStic's per-polygon pipeline from
them (the reference, and still the path for shadow polygons). `src/rast/fused.c` runs every pixel through all
stages at once; `fused_neon.c` is that in C NEON (8 pixels a step), and `kerngen.py` generates `rast_kern.S`, the
same in assembly with a fixed register allocation, one kernel per variant (depth source x texture x translucency
x flat colour), which is what runs. Shortcuts that give the same bits: a white vertex colour with alpha 31 makes
modulate the identity, so the texel is the colour; a group of 8 pixels that all fail the depth test stops at the
test; one that all pass is stored straight.

## Simulator usage (see `tools/sim/` for setup; `dev/` holds the scripts used during development)

- `RAST=diff` renders every bin with both renderers and compares the output frames.
- `RAST=ours RAST_PDIFF=1` compares per polygon and reports the differing pixels; `RAST_PDIFF_STRICT=1` includes
  the 5-mod-8 translucent quirk pixel (exact), `2` the 1-pixel batches (the known deviation).
- `RAST_PIPE` selects the pipeline: 0 = b0 (stage by stage), 1 = fused scalar, 2 = fused C NEON, 3 = assembly.
- `RAST_DUMP=<dir>` writes frames as PPM; `RAST_STATS=1` prints the opaque overdraw; `RAST_FRAMES=1` prints the
  frame count every 10 frames (for per-frame figures from a block profile).

## Next

- Device A/B: the same game with the option off and on, from the performance logs.
- TBL palette lookups for 4- and 16-colour textures (saves the second gather); 64-bit texel-pair loads in the
  bilinear gathers.
- 3x: the top vertex at 3x for tied vertices; a cheaper downsample; later the hi-res 3D layer presented through
  the dsflip shader instead of downsampled.
