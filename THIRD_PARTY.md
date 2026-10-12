# Third-party parts

SuperDrastic's own code is MIT licensed (see `LICENSE`). These parts keep their own terms:

| Part | Where | License |
|---|---|---|
| [rcheevos](https://github.com/RetroAchievements/rcheevos) (RetroAchievements), v12.5.0 with rc_client_do_frame split in two (see its VERSION) | `src/third_party/rcheevos` | MIT (`src/third_party/rcheevos/LICENSE`) |
| [stb_image, stb_truetype, stb_vorbis](https://github.com/nothings/stb) | `src/third_party/stb` | public domain or MIT (the end of each file) |
| AMD FidelityFX Super Resolution 1.0 (EASU), in `ds-fsr` | `shaders/ds-fsr.frag` | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (notice in the file) |
| libretro glsl-shaders: lcd1x_nds (Gigaherz's lcd3x, public domain, with hunterk/Pokefan531's NDS colour, public domain; edited by jdgleaver), in `lcd1x-nds` | `shaders/lcd1x-nds.frag` | GPL-2.0-or-later (notice in the file) |
| libretro glsl-shaders: lcd1x and lcd3x (Gigaherz, public domain) | `shaders/lcd1x.frag`, `shaders/lcd3x.frag` | as in each file |
| libretro glsl-shaders: nds-color (hunterk, Pokefan531) | `shaders/nds-color.frag` | public domain (notice in the file) |
| libretro glsl-shaders: zfast_lcd and zfast_crt (Greg Hogan, SoltanGris42) | `shaders/zfast-lcd.frag`, `shaders/zfast-crt.frag` | GPL-2.0-or-later (notice in the files) |
| libretro glsl-shaders: retro-v2, bevel | `shaders/retro-v2.frag`, `shaders/bevel.frag` | GPL-2.0-or-later (notices in the files) |
| libretro glsl-shaders: sharp-bilinear-simple, sharp-bilinear-scanlines (rsn8887, Themaister) | `shaders/sharp-bilinear.frag`, `shaders/sharp-bilinear-scanlines.frag` | as in each file |
| libretro glsl-shaders: fxaa (Timothy Lottes' FXAA) | `shaders/fxaa.frag` | as in the file |

**DraStic** is not part of SuperDrastic and is not included: it is Exophase's closed-source emulator, which your
firmware provides. SuperDrastic doesn't modify it; it is a library loaded next to it. ROCKNIX's built-in DraStic
shaders are read from ROCKNIX's `libdrastouch.so` at run time when present, and are not copied here either.

The libretro shaders above (github.com/libretro/glsl-shaders) are converted to this project's shader interface by
`tools/shaders/conv.py` (their fragment stage, libretro's names mapped onto u_texture, u_texture_size, u_output_size
and v_texcoord, parameters at their defaults); each file keeps the original's notice at its top.
