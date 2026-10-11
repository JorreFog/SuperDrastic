# BananaDS settings

In `bananads.conf` (`key = value`, `#` for comments), found in `$BANANADS_DATA`. That is
`/storage/.config/bananads` when it exists, the current directory otherwise; `--config` names another file. Every
key can also come from the environment as `BANANADS_<KEY>` (dots become underscores, upper case:
`BANANADS_WIFI_DNS=altwfc`), which wins over the file, and from `--set key=value`, which wins over both.

| Key | Default | |
|---|---|---|
| `frontend` | `sdl` (or `headless` in a build without SDL) | `sdl`, `headless`; `kms` comes with milestone D |
| `jit` | `on` | melonDS's JIT; `off` for the interpreter (slow, for debugging) |
| `render.threaded` | `on` | the software 3D renderer on its own thread |
| `save.dir` | the ROM's folder | where `<game>.sav` lives |
| `save.per_game_folder` | `off` | `<save.dir>/<game>/<game>.sav` (ROCKNIXDS #42/#56) |
| `state.dir` | the save folder | savestates: `<game>.resume.mst` (the quit/resume state) and `<game>.<1-3>.mst`, each with a `.pic` |
| `resume.on_quit` | `on` | save the resume state when the game is quit |
| `resume.on_start` | `on` | load it when the game starts |
| `firmware.nickname` | `BananaDS` | the DS's user name (10 characters at most; games and WFC show it) |
| `firmware.language` | the firmware's | 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish |
| `wifi.dns` | `kaeru` | `kaeru`, `altwfc`, `wiilink`, an IPv4 address, `system` (the handheld's own DNS) or `off` (no Wi-Fi at all) |
| `wifi.local` | `off` | `host`, or `join:<the host's address>`: local wireless with another handheld (builds with `BANANADS_LAN`) |
| `wifi.local_players` | `4` | how many can join a host |
| `mic.source` | `device` | `device` (ALSA or SDL capture) or `none` (only the menu's Blow) |
| `mic.device` | `default` | the ALSA capture device |
| `mic.threshold` | `0.15` | how far above the room's noise a sound must be to reach the game (full scale 1). ROCKNIXDS's high / medium / low are 0.03 / 0.15 / 0.3. `0` turns the gate off |
| `mic.gate` | `3` | the echo gate's factor over the learned speaker leak; `0` turns it off |
| `mic.coupling_max` | `4` | the most leak it will learn |
| `mic.hold_ms` | `120` | the shortest time the gate stays open |
| `audio.out_device` | `default` | the ALSA output device |
| `audio.latency_us` | `30000` | ALSA's buffer |
| `audio.realtime` | `on` | the sound thread at SCHED_FIFO 10, when allowed |
| `sdl.scale` | `2` | the window's size |
| `headless.frames` | `600` | how many frames `headless` runs |
| `headless.dump_dir`, `headless.dump_every` | none, `60` | write both screens as `frame<N>.ppm` every N frames |
| `log.file` | none | a copy of the log (stderr always gets it) |
| `log.level` | `1` | 0 debug, 1 info, 2 warnings, 3 errors |

## The window's keys (`frontend=sdl`)

Arrows: the D-pad. X / Z: A / B. S / A: X / Y. Q / W: L / R. Enter: Start. Right Shift or Backspace: Select. The
mouse on the bottom screen: the stylus. F1: blow into the mic. F5-F7: save to slot 1-3. F9-F11: load slot 1-3.
Esc: quit (with the resume state).
