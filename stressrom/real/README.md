# dsreal: a realistic DS stress ROM

Own code (GPL-3.0). `python3 build.py` (clang, ld.lld, llvm-objcopy) writes `out/`:

| ROM | What |
|---|---|
| `dsreal.nds` | level 5 (realistic), every scene in turn, looping (soak = run it long) |
| `dsreal-L1..L10.nds` | load levels (L5 = realistic; L10 reaches ~1,500 stored polygons / ~5,400 vertices and lags in the game itself) |
| `dsreal-race/-action/-town.nds` | one scene forever (profiles of a scene) |

`DSREAL_OUTDIR=out-v2 python3 build.py` is **v2** (afcef05: the race gets MKDS-like 4x fill: a ground plane, mountains and
dust behind every kart; used for all 4x work), `DSREAL_PROBE=1` builds `out-probe/` with the universal companion's
probe block at 0x023F0000. v1 = the sources before afcef05 (`out/`, used for the 2x/3x A/Bs of 2026-11-01).

Each has a `.phases.tsv` beside it (stress-run.sh / summarize.py): race 25 s, action 20 s, town 25 s (70 s a loop).

## Scenes (level 5, emulated numbers from the ROM's own counters on the status line)

| Scene | Models | Load (L5) | Evidence |
|---|---|---|---|
| race | Mario Kart DS race: 3D course, 8 karts, item boxes, trees, houses, smoke; HUD sprites/BG over 3D; course map on the bottom (affine BG + icons) | ~1,070 polygons / ~3,700 vertices stored, display lists by the GX FIFO DMA (one per object, ~250 DMA starts), matrices by CPU STRs, fog, translucent sort; ARM9 ~58% | MKDS race start fingerprint (docs/PERF.md, 2026-10-18): ~1,330 polygons, display lists via GX FIFO DMA, ~1,500 ITCM calls. Unvalidated until the final game check |
| action | 2D action on both screens (Contra 4 / Castlevania): 4 BGs per engine with parallax, ~180 entities, 128/96 sprites incl. affine + semi-transparent, OBJ tiles streamed 6 KB/frame, palette cycling, alpha blend, mosaic, HBlank-DMA scroll wave (A), HBlank-IRQ window spotlight (B), explosion spikes every 4 s | ARM9 ~31% | synthetic (no 2D reference game measured yet; owner has Contra 4 on the Plus) |
| town | Pokemon HG / Platinum overworld: 3x3 map blocks, houses with line-polygon outlines, NPC billboards whose texture frames are uploaded to texture VRAM in the vblank, trees, narrow camera (most polygons clipped), CPU-printed text box, map chunks streamed from the card, a door every 8 s (fade, overlay + texture set + map from the card, code into RAM) | ~96 polygons stored of ~600 submitted, ARM9 ~45%, door hitches 2 lag frames | Origin HG first grass area fingerprint (docs/PERF.md, 2026-10-26): 568 submitted, 81% clipped, ~93 stored. Unvalidated |

All scenes: ARM7 sound driver (sequencer at ~192 Hz on 12 channels + 2 noise, IMA-ADPCM stream from the card decoded on the
ARM9 into a 2-half ring that timer 1 asks to refill over the IPC FIFO, touch through SPI each vblank), IRQ-driven
both CPUs (halt between frames), protection unit + caches on like a game's crt0, IRQ handler and collision maths in
ITCM, game logic in Thumb, script VM (branchy dispatch), DIV/SQRT units, card reads (B7), EEPROM saves.

## Checksums

At each scene's end a 32-byte record goes to the save (EEPROM, `<rom>.sav`): loop, scene, frames, a hash of 4 display
captures of engine A (frames 150/450/... of the scene), the game-state hash, the ARM7 driver's hash, the ARM9 load
(average %) and the lag frames. `savdump.py a.sav [b.sav]` prints them / compares two runs (SAME/DIFF). Pictures and
state are the same in every loop of one build (DraStic takes the EEPROM writes as FLASH: savdump.py finds the shifted records); the ARM7 hash depends on the loop (timer phase), so compare runs
loop by loop. Captures are 1x (quarter 0 of hi-res lines), so they catch emulation differences and part of the
3D layer; the hi-res output itself is checked with BananaDS's render.gengis_verify / render.2d_verify, and the
emulation with audio.tap.
