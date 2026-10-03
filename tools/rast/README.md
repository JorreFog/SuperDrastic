# Our own 3D rasterizer for DraStic (work in progress)

Replaces DraStic r2.5.2.2's hi-res 3D raster stage (`video_3d_render_bins_4x` and `render_polygon_setup_4x`, hooked by
code patching in `src/rast/rast.c`) and must match DraStic's output bit for bit.

Status:
- `src/rast/spec/`: about 85 DraStic 3D routines ported to exact C. Each one is unit-tested bit-exact against the
  original inside DraStic's process (`tools/rast/ut/`, `run.sh t_<group>.c ../../../src/rast/spec/<group>.c`).
- `src/rast/b0.c`: DraStic's per-polygon pipeline (batching + flush) rebuilt from the specs. Bit-exact in-frame.
- `src/rast/fused.c` / `fused_neon.c`: one pass per pixel, scalar reference and NEON. Bit-exact per polygon on the
  stress ROM and on all 10 feature scenes (ROCKNIXDS `stressrom/scenes9.c`). The comparison skips 2 cases where
  DraStic reads stale stack memory, which even DraStic cannot reproduce: a translucent id at the end of a
  batch-ending line with 5 mod 8 pixels, and 1-pixel translucent batches.
- Performance (qemu instruction counts, stress ROM L4): the NEON version is not yet faster. The line kernels
  cost about 197 instructions per 8 pixels; DraStic's staged code costs about 188.
  Next: tighten the kernel (target about 140: hoist constants, branch-free texture wrap, fewer mask conversions,
  white-vertex-colour shortcut), drop the b0 fallback for the quirk batches, and add NEON decal/toon/highlight.
  Then add algorithmic savings: opaque deferred shading (a depth pre-pass, then shade only the winning pixels) and
  per-bin caching of unchanged bins.

Simulator usage (see `tools/sim/` for setup; `dev/` holds the scripts used during development, with the
session's paths):
- `RAST=diff` renders every bin with both renderers and compares the output frames.
- `RAST=ours RAST_PDIFF=1` compares per polygon and reports the differing pixels.
- `RAST_PIPE` selects the pipeline: 0 = b0, 1 = fused scalar, 2 = fused NEON (the default).
- `RAST_DUMP=<dir>` writes frames as PPM.

Layout notes are in `src/rast/ds3d.h` and in each spec file's header comment.
