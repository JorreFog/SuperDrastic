# From SuperDrastic to BananaDS, file by file

SuperDrastic is loaded into DraStic and works through DraStic's SDL calls and, for some parts, fixed addresses in
DraStic r2.5.2.2. BananaDS owns the emulator, so the SDL-hook layer goes away: frames, sound, input and memory
come straight from melonDS. Line numbers are SuperDrastic `main` (930c4c5).

**generic**: reusable as it is. **adapt**: the logic carries over, the DraStic glue is replaced. **DraStic-only**:
only makes sense inside DraStic.

| SuperDrastic | What it does | Verdict | In BananaDS |
|---|---|---|---|
| `dsflip.c` display (KMS/atomic, `mkbuf`, `try_commit` 372, `on_flip` 419, latch pacing `arm_latch` 549 / `note_phase` 591, the queue, the toast plane, the VOP2 underrun recovery 1174-1187) | Both DS screens on their own DSI panels, no compositor | **adapt** | Milestone D. DraStic drew straight into dumb buffers through the hooked `SDL_LockTexture`; BananaDS gives melonDS's 256x192 frames to the same buffers (one copy each, or the renderer writes there). Everything from `enqueue` on is unchanged |
| `dsflip.c` touch (886-905) | evdev touch to the stylus | **adapt** | DraStic only read relative mouse moves (the `xrel=-4096` pin). melonDS takes absolute points: `Emu::touch` |
| `dsflip.c` SDL hooks (1019, 1270-1515) | How SuperDrastic sits inside DraStic | **DraStic-only** | Not needed |
| `audio.c` pump and ALSA writer | Calls the emulator's sound on an even real-time clock | **adapt** | `src/alsa.cpp`: the pump pulls from melonDS's SPU instead of DraStic's SDL callback |
| `audio.c` mic (345-403) + 1.6-prep `d8291e0` + open-issues `4011933`/`b874a20` (#26) | Capture, floor, echo gate, a press of the fake-mic key | **adapt** | `src/mic.cpp`: the same DC blocker, floor, echo gate, hysteresis and hold, but the game gets the real PCM. The key, joystick-button and WAV-restart machinery is dropped |
| `audio.c` SFX / UI sounds | Menu and unlock sounds mixed in | **generic** | With the menu (E) |
| `shader.c` | GLES pass on dma-buf imports, a fence per frame; ds-crisp, ds-grid, ds-integer, ds-fsr | **generic** | E. Drop reading ROCKNIX's built-in shaders out of `libdrastouch.so` |
| `ui.c` | RetroAchievements pop-ups, the progress pill, the volume card on the overlay plane | **generic** | E |
| `menu.c` | The in-game menu on both panels | **adapt** | E. Save/Load call `Emu::save_state`/`load_state` and show the `.pic` beside each state, not DraStic's `.dss` thumbnails. The "DraStic menu" tile goes. The Microphone page shows `Mic::meter()` and uses `Mic::blow()` |
| `resume.c` | Quit with a savestate and resume it next time | **DraStic-only** in how (libc `rename`/`open` hooks, joystick presses of DraStic's save control) | Done natively: slot 0, `resume.on_quit` / `resume.on_start`. The stall watch's lessons (the exit hotkey always ends the game) apply to E's quit |
| `cpugov.c` | The lowest clock that holds full speed, with per-game memory | **adapt** | E. Name BananaDS's threads (`bds-*`), feed frame times straight in. SuperDrastic PR #6 (3x memory) carries over as "per renderer and scale" |
| `ra.c` | RetroAchievements (rcheevos, softcore) | **adapt** | E. Much simpler: rcheevos reads `NDS::MainRAM` and the DTCM directly, with no scan for the ROM header and no mmap of DraStic's shared-memory fd at `0x410000` |
| `volume.c` | The volume-key card | **generic** | E |
| `src/rast` (Gengis Engine) and PR #8 | DraStic's 3D renderer replaced: kernels, walker, resolve, geometry | **DraStic-only** as wired (every input is a DraStic structure at a DraStic offset) | F. The NEON kernels and `hr.c`/`walk.c` could become a hi-res melonDS `Renderer3D`. That needs an adapter from melonDS's `Polygon`/`Vertex` to Gengis's records, its own texture cache, and a hi-res 2D compositor. Weeks of work, decided after B's numbers |
| 1.6-prep `wifi.c` | DraStic's Wi-Fi registers patched (`0x10fa0`/`0x117f0`/`0x12120`) | **DraStic-only** | Not needed: melonDS emulates the hardware |
| 1.6-prep `wfcnet.c`, `wififrame.c` | A small NAT with DHCP that hands out the WFC DNS | **generic** | Not used for now: libslirp does the NAT, `WfcDns` forwards DNS to the WFC server. `wfcnet.c` stays the fallback if slirp is too heavy on the device |

## Settings that keep their meaning

The `DSFLIP_*` and `DSHOOK_*` variables become BananaDS settings (`docs/SETTINGS.md`) as each part is ported. The
mic's are already in: `DSHOOK_MIC_THRESH` is `mic.threshold`, `DSFLIP_MIC_GATE` is `mic.gate`,
`DSFLIP_MIC_COUPLING_MAX` is `mic.coupling_max` and `DSFLIP_MIC_HOLD_MS` is `mic.hold_ms`, with the same defaults.
ROCKNIXDS's ES choices (mic high/medium/low = 0.03/0.15/0.3, `nds.wfc_dns`) map onto them directly.
