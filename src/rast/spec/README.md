# DraStic 3D routine specs

Exact C ports of DraStic r2.5.2.2's 3D raster routines (the hand-written NEON `*_asm` ones and the C ones they work
with), one group per file. Each `spec_<name>()` has the original's signature and memory behaviour and is unit-tested
bit-exact against the original inside DraStic's process (`tools/rast/ut/`). They are the reference for the fused
rasterizer; they are not meant to be fast. Each file's header comment documents the per-pixel math.
