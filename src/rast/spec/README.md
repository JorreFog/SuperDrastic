# DraStic 3D routine specs

Exact C ports of DraStic r2.5.2.2's 3D raster routines (the hand-written NEON `*_asm` ones and the C ones they work
with), one group per file. Each `spec_<name>()` has the original's signature and memory behaviour and is unit-tested
bit-exact against the original inside DraStic's process (`tools/rast/ut/`). They are the reference for the fused
rasterizer (and, for `composite.c`, of the 2D compositor's 3D visibility step that `comp.c` replaces); they are not
meant to be fast. Each file's header comment documents the per-pixel math.

`2d/`: the same for DraStic's 2D engine (BG renderers, sprites, windows, the composite on every path, capture, the
scanout conversion, and the composite's per-pixel closed form), the reference of the 2D engine project
(`tools/rast/2d-engine.md`; tests `tools/rast/ut/t_bg2d.c`, `t_obj2d.c`, `t_compose2d.c`). Built into the unit tests
only (the library's build compiles `spec/*.c`, not `spec/2d/`).
