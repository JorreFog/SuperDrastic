# Gengis Engine in BananaDS (milestone F, option 2)

Gengis Engine is SuperDrastic's own 3D rasterizer: `src/rast`, worked on in PR #8. Its code is ours and MIT, so
BananaDS can use it. But as it stands, every input is a DraStic structure at a DraStic r2.5.2.2 offset. This
is the plan to give it a small API of its own, and to drive that API from melonDS's 3D state.

## What carries over, and what doesn't

| Gengis module | What it does | For BananaDS |
|---|---|---|
| `rast_kern.S`, `fused.c`, `fused_neon.c`, `fused_asm.c` (`kerngen.py`, `kernsched.py`) | The per-span pixel pipeline: texture, blend, depth, the NEON kernels | **Core.** Pure over span arrays and a polygon record |
| `walk_impl.h`, `walk.c` | The 2x edge walker | **Core,** once its two tables (walk orders, reciprocals) are its own instead of read from DraStic |
| `res2.c` | 2x resolve, with fog and edge marking | **Core.** `res2_resolve` is pure; `res2_resolve_fx` takes its registers from a frame-state struct instead of DraStic's |
| `binsched.c` | Hands bins to threads, longest first | **Core,** self-contained |
| `hr.c` (3x) | Its own walker and the 3:2 downsample | **Core** once it takes clip-space vertices from the adapter |
| `rast.c`, `comp.c`, `ds2d.c`, `dsgeom.c`, `geom_tvb.S`, `ds3d.h` | Hooks into DraStic, DraStic's 2D compositor and geometry stage | **Not used.** DraStic-only |

## The core API (to be cut out of `src/rast`, shared by SuperDrastic and BananaDS)

```c
typedef struct { int32_t w; uint16_t x, y, z, color555; int16_t s, t; } gx_vertex;  // 16 bytes, 2x/3x screen
typedef struct gx_texture { uint32_t params; uint16_t w, h; uint8_t format, paletted;
    const uint32_t *texels; const uint8_t *idx8; const uint32_t *palette; } gx_texture;  // decoded
typedef struct { uint16_t _0, texwrap; uint32_t attr; uint32_t walk; uint32_t _c;
    const gx_texture *tex; uint16_t _18, vbase, _1c, _1e; } gx_polygon;   // 32 bytes: the kernels' layout
typedef struct { uint32_t disp3dcnt, alpha_ref, clear_color, clear_attr;
    const uint16_t *rear_color, *rear_depth; uint16_t rear_ofs;
    uint8_t fog_table[32]; int8_t fog_delta[32]; uint32_t fog_color; uint16_t fog_offset;
    uint8_t edge_r[8], edge_g[8], edge_b[8]; uint8_t toon[3][32]; } gx_frame_state;
typedef struct { const gx_vertex *v; const gx_polygon *opa, *trl; uint32_t nopa, ntrl; } gx_scene;

gx_ctx *gx_create(int scale /* 2 or 3 */, int threads);
void gx_render_frame(gx_ctx *, const gx_scene *, const gx_frame_state *, uint32_t *out);  // 12 bins x 0x10000
void gx_get_line(const uint32_t *out, int y, uint32_t *dst);  // de-interleaved, 6/6/6/5 like melonDS's
```

SuperDrastic would fill these from DraStic, which mostly means pointing at what DraStic already has. BananaDS
fills them from melonDS's `GPU3D`.

## The melonDS adapter: `Renderer3D` "gengis"

1. **Vertices:** melonDS's `Vertex::HiresPosition` has a 4-bit fraction, so 2x screen x/y is `HiresPosition >> 3`.
   z goes to 16 bits, colour to BGR555 (as Gengis expects), s/t as they are.
2. **Polygons:** `RenderPolygonRAM[0..RenderNumPolygons)` from `GPU3D`: attributes, texture parameters and
   palette into `gx_polygon`. The walk order comes from the vertex order, and opaque and translucent go into
   their own lists.
3. **Textures:** decode each `TexParam`/`TexPalette` pair once per frame into a cache. That covers all seven
   formats, including the 4x4 compressed one (melonDS's `GPU3D_Texcache` is the reference).
4. **Frame state:** `RenderDispCnt`, `RenderAlphaRef`, `RenderClearAttr1/2`, `RenderFog*`, `RenderEdgeTable`,
   `RenderToonTable`, and the rear plane from texture slots 2-3 when `CLRIMAGE` is on.
5. **Output:** melonDS's 2D compositor is 1x, so a 2x 3D layer needs a hi-res compositor as well. That compositor
   is the real cost of this milestone: BG layers, sprites, windows and blending at 512x384, or at least the 3D
   layer kept at 2x with the 2D layers scaled.

## Accuracy

Gengis matches DraStic bit for bit, approximations included. Among them:
- float perspective weights;
- colour reduced to BGR555;
- DraStic's fog interpolation;
- the toon index `r >> 1`;
- edge-marking rules;
- no anti-aliasing path at 2x;
- shadow polygons and vertex counts 10-15 left to DraStic.

melonDS follows the hardware more closely. So in BananaDS, Gengis would be the fast hi-res option, and
melonDS's renderer would stay the accurate 1x one. Each of the cases above gets a melonDS-side decision: follow
Gengis or match melonDS. That needs to be settled with the owner before the adapter is written.
