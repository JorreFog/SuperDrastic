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
- `RAST_DUMP=<dir>` writes frames as PPM; `RAST_STATS=1` prints the opaque overdraw.

## Next

- Device A/B: the same game with the option off and on, from the performance logs.
- TBL palette lookups for 4- and 16-colour textures (saves the second gather); deferred shading of opaque
  polygons (skips the shading of pixels a nearer opaque polygon overwrites: 1.0-1.5x overdraw in the scenes).
