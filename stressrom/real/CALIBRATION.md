# dsreal calibration

Evidence per scene (stress-test-roms skill: 1 = emulator fingerprint, 3 = documented numbers, 4 = the owner's games).

| Scene | Reference | Stress ROM numbers | Game numbers | Device | Commit | Date | Status |
|---|---|---|---|---|---|---|---|
| race | MKDS race start (PERF.md 2026-10-18, qemu + device profile: ~1,330 polygons, GX FIFO DMA display lists, geometry 31% of the emu thread) | v1: 1,070 stored polygons; device 4x profile: geometry 42% of the emu thread, Gengis 0.9-1.0 M opaque + 0.4-0.5 M translucent span px | MKDS 4x (PERF.md 2026-10-26): 1.3-2.9 M opaque + up to 1.2 M translucent span px, Gengis 40-80 ms/frame | Plus | v1 3752e7b | 2026-11-01 | v1 under-filled at 4x -> v2 (afcef05) adds fill; unvalidated |
| town | Origin HG first grass area (PERF.md 2026-10-26: 568 submitted, 81% clipped, ~93 stored) | ~96 stored of ~600 submitted (PC counters) | - | PC | 3752e7b | 2026-11-01 | unvalidated |
| ohg | Origin HG's own notes (FAQ, work/notes/hardware_support.md: 60 fps overworld overruns 67 MHz in busy towns; one card read per glyph) | RG DS 2x: 908 of 2,408 frames lost in-game at 67 MHz, 6 at 133 MHz | (final check pending: owner's Origin HG, Plus) | RG DS | 3752e7b | 2026-11-01 | unvalidated |
| action | none (owner has Contra 4 on the Plus) | ARM9 31%; Plus 4x 2D thread 44-46% | - | Plus | 3752e7b | 2026-11-01 | synthetic |
| race v2 (4x) | MKDS boot -> demos, 4x, owner's ds-grid (Plus, cpugov on, 120 s): 54.36 / 59.45 fps (pin), 57.11 / 55.20 (integ) | v2 race, pinned 1416 MHz: 56.6 fps (pin), 58.0 (final); emu + 2D threads at 87-90% | see left | Plus | afcef05 | 2026-11-01 | same range and the same bottleneck class (CPU threads at 4x, heat); game runs vary 5 fps run to run: roughly validated |
| ohg | the owner's Origin HG on a route (Plus, 2x, owner's state): the game renders 300 of 600 frames at 67 MHz (30 fps in-game), 300 of 300 at 133 MHz | ohg scene: in-game 37 fps at 67 MHz, 60 at 133 (RG DS) | see left | Plus / RG DS | 3752e7b | 2026-11-01 | validated: same direction, same cause (ARM9 time); the game's route halves its rate where the scene loses ~38% |
