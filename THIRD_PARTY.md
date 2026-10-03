# Third-party parts

SuperDrastic's own code is MIT licensed (see `LICENSE`). These parts keep their own terms:

| Part | Where | License |
|---|---|---|
| [rcheevos](https://github.com/RetroAchievements/rcheevos) (RetroAchievements), v12.5.0 with rc_client_do_frame split in two (see its VERSION) | `src/third_party/rcheevos` | MIT (`src/third_party/rcheevos/LICENSE`) |
| [stb_image, stb_truetype, stb_vorbis](https://github.com/nothings/stb) | `src/third_party/stb` | public domain or MIT (the end of each file) |
| AMD FidelityFX Super Resolution 1.0 (EASU), in `ds-fsr` | `shaders/ds-fsr.frag` | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (notice in the file) |

**DraStic** is not part of SuperDrastic and is not included: it is Exophase's closed-source emulator, which your
firmware provides. SuperDrastic doesn't modify it; it is a library loaded next to it. ROCKNIX's built-in DraStic
shaders are read from ROCKNIX's `libdrastouch.so` at run time when present, and are not copied here either.
