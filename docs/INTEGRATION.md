# Integrating SuperDrastic into a firmware

This is the contract between SuperDrastic and the firmware that starts DraStic: what the package contains, how it
gets the display, what it tells you, and every setting. The settings keep the `DSFLIP_` prefix from the library's
original name (libdsflip), so existing integrations keep working.

## The package

`superdrastic-<version>-aarch64.tar.gz` unpacks to one folder:

| File | |
|---|---|
| `libsuperdrastic.so` | The library, loaded into DraStic with `LD_PRELOAD` |
| `superdrastic-run` | The launcher: `superdrastic-run <drastic> [args...] <rom>` |
| `superdrastic-update` | Updates the folder from GitHub's latest release (keeps `superdrastic.conf`, `data/`, `logs/`) |
| `superdrastic.conf.example` | Every setting, commented |
| `shaders/` | The `ds-*` shaders |
| `VERSION`, `README.md`, `LICENSE`, `THIRD_PARTY.md` | |

The folder can live anywhere writable. `data/` (RetroAchievements login token, badge cache, the CPU governor's memory)
and `logs/` are created beside the launcher.

## Starting a game

Replace your DraStic command with `superdrastic-run` followed by the same command:

```sh
/opt/superdrastic/superdrastic-run /path/to/drastic --some-drastic-option /path/to/game.nds
```

The launcher gets the display from your frontend, runs DraStic with the library from DraStic's own folder (DraStic
reads its configuration from its working directory), waits for it and gives everything back. It exits with DraStic's
status: 0 when the player chose Exit in DraStic's menu, 137 when a hotkey killed it.

**Exit hotkeys** that `killall drastic` (or `kill -9` the DraStic process) keep working: DraStic still runs as its own
binary. If your hotkey matches the binary's name, keep the name `drastic` (ROCKNIX, for example, runs DraStic through
a symlink called `drastic`).

## Getting the display: `DSFLIP_HANDOVER`

Only one program can drive the panels, and SuperDrastic needs them for itself (DRM master). Pick how your frontend lets
go:

| Value | What happens | Use when |
|---|---|---|
| `auto` (default) | `cmd` if `DSFLIP_STOP_CMD` is set, else `vt` if a compositor (sway, weston, labwc, cage, gamescope, kwin, X) runs, else `none` | |
| `none` | Nothing | Your frontend is a KMS program that exits or drops the display while games run |
| `vt` | Switches the console to VT `DSFLIP_VT` (12) and back afterwards. A compositor started through seatd or logind releases the display on the switch and takes it back afterwards | Your frontend runs under a Wayland compositor with seatd/logind. The fastest way back to the menu: nothing restarts |
| `cmd` | Runs `DSFLIP_STOP_CMD` before the game and `DSFLIP_START_CMD` after it | Anything else: stop and start your frontend (and its compositor) |

Traps found on the RG DS:

- **`vt`:** the new VT must stay in text mode and unblanked, or the kernel powers the panels down under the game; the
  launcher handles both. A frontend that tears its GL context down for a game may fail to create it again after the
  switch: keep its window during games (EmulationStation: `HideWindow` off).
- **Stopping a compositor while its buffers are on screen** can leave the RK356x display controller in an underrun
  loop (tens of thousands of interrupts a second, until the next full modeset). SuperDrastic checks for it right
  after it takes the display (30 ms) and power-cycles the panels only if it's happening. If it starts when a game
  ends, switching your frontend's outputs off and on (a full modeset) clears it.

If SuperDrastic can't take the display, the launcher hands it back and starts DraStic the ordinary way
(`DSFLIP_FALLBACK=0` to give up instead).

## While a game runs

The launcher also sets, and puts back afterwards, the parts of the system that decide power use (each only where the
system has it):

- **The GPU's clock** (the first `/sys/class/devfreq/*gpu*`): its lowest with no shader (nothing is drawn on the GPU),
  `simple_ondemand` with a floor near 400 MHz with a shader, full with `ds-fsr`. Another service changing it mid-game
  (ROCKNIX's charger watcher does) gets it put back within 2 s. `DSFLIP_GPU_CLOCKS=0` leaves it alone.
- **The CPU's clock limit**: SuperDrastic's own governor lowers `scaling_max_freq` while the game needs less. It works
  only when the `performance` governor is active and the CPU is one cluster (`cpufreq/policy0` only); otherwise it
  stays off and your governor decides. The launcher lifts the limit the moment DraStic exits. `DSFLIP_CPUGOV=0`
  turns the governor off. A clock that dropped frames is remembered per game, shader and resolution
  (`data/cpugov/`), so the next session doesn't find it again by dropping frames; two minutes at it without a drop
  forgive it. `DSFLIP_CPUGOV_MEMORY=0` starts every session knowing nothing.
- **PipeWire's rate**, set to DraStic's 44.1 kHz (`clock.force-rate`) so it doesn't resample every cycle, and put back
  afterwards. Set before DraStic opens its audio: switching mid-stream makes the audio timing uneven.
  `DSFLIP_AUDIO_RATE=0` leaves it alone.

## Quitting with a save (resume)

Exit hotkeys usually kill DraStic (`kill -9`), which loses the game's place. Send it **SIGUSR1** instead and set
`DSFLIP_RESUME_FILE`: SuperDrastic presses DraStic's own "save state" control (the joystick button `drastic.cfg`
maps to it), writes the savestate to that file instead of one of the player's slots, and exits with SIGKILL (status
137, as a hotkey kill). Measured on the RG DS: ~0.8 s for the save. A second SIGUSR1, or no savestate within 5 s,
exits at once; a resume file already there is left as it was if that save doesn't finish. Start the game next time with `DSFLIP_RESUME_LOAD=1` as well: once it runs, SuperDrastic presses
"load state" with DraStic's lookup pointed at the resume file, shows a "Resumed" pop-up and deletes the file.

Keep the file in DraStic's `savestates/` folder (a rename, no copy) and name it anything but `<game>_<digit>.dss`.
With DraStic's `backup_in_savestates` on (its default), loading a state also puts back the in-game save it was taken
with: don't load a resume state older than the game's `.dsv` (ROCKNIXDS drops it then). On ROCKNIX the hotkey runs
`killall $(cat /tmp/.process-kill-data)`, so writing `-USR1 drastic` there after `start_drastic.sh` has set it is enough.

## What SuperDrastic tells you

- **The verdict file**, `DSFLIP_STATE` (default `/tmp/superdrastic-state`): `ready` once SuperDrastic has both panels,
  or `passthrough: <reason>` when it gives up (DraStic then keeps running but draws nothing). It decides within ~6 s:
  up to 3 s waiting for the display, up to 3 s for a shader. Delete the file before starting DraStic.
- **The log**, `DSFLIP_LOG` (default `logs/superdrastic.log`): one per session, the previous three kept as `.1`..`.3`.
  Frame pacing, drops, the shader, audio, the CPU governor and RetroAchievements. `logs/run.log` is the launcher's
  own.

## Settings

Environment variables, or `superdrastic.conf` beside the launcher (a value there wins over the environment).

| Variable | Default | |
|---|---|---|
| `SDL_VIDEODRIVER` | `dummy` (set by the launcher) | Required: SDL must not open a window or the display |
| `DSFLIP_HANDOVER`, `DSFLIP_VT`, `DSFLIP_STOP_CMD`, `DSFLIP_START_CMD` | `auto` | See *Getting the display* |
| `DSFLIP_DRASTIC_DIR` | DraStic's own folder | Where DraStic runs (and reads its configuration) |
| `DSFLIP_SHADER` | none | A shader by name, e.g. `ds-crisp`; none = DraStic's frames go straight to the panels |
| `DSFLIP_SHADER_DIR` | | Another folder of `.frag` shaders (checked first) |
| `DSFLIP_CARD` | the first `/dev/dri/card*` with two connected DSI panels | The display device |
| `DSFLIP_TOP` | `DSI-2` | The connector that shows the DS top screen |
| `DSFLIP_TOUCH` | `fe5e0000.i2c` | A fragment of the bottom touchscreen's device path |
| `DSFLIP_TOUCH_INVERT` | none | `x`, `y` or `xy` |
| `DSFLIP_DATA` | `data/` beside the launcher | RetroAchievements login token and badge cache |
| `DSFLIP_RA_USER`, `DSFLIP_RA_PASSWORD` | | RetroAchievements account; the password is used once, then the token |
| `DSFLIP_RA_SOUND` | none | An `.ogg` played on unlocks |
| `DSFLIP_FONT` | DejaVu Sans / Liberation Sans | Font for the pop-ups (TTF/OTF) |
| `DSHOOK_MIC_THRESH` | 0 (off) | Microphone sensitivity: 0.03 high, 0.15 medium, 0.3 low |
| `DSFLIP_CPUGOV`, `DSFLIP_CPU_MIN`, `DSFLIP_CPU_MAX` | on, 1104 MHz, the CPU's top | The CPU governor and its bounds (kHz) |
| `DSFLIP_CPUGOV_MEMORY` | 1 | 0 = the governor doesn't remember clocks that dropped frames |
| `DSFLIP_QUEUE` | 1 | Frames that may wait behind the next one, 0-3 (0 = newest only). Each is a refresh (16.7 ms) of input latency, and covers one late frame |
| `DSFLIP_QUEUE_WAIT` | 0 (off) | Milliseconds DraStic is held when the queue is full, instead of dropping a frame (20 works; for capped clocks) |
| `DSFLIP_LATCH_MARGIN` | 1300 | The shortest time (µs) before a vblank that a frame is committed at (diagnostics) |
| `DSFLIP_RESUME_FILE`, `DSFLIP_RESUME_LOAD` | | See *Quitting with a save* |
| `DSFLIP_GPU_CLOCKS`, `DSFLIP_AUDIO_RATE` | 1, 1 | The launcher's GPU clock and PipeWire rate handling |
| `DSFLIP_AUDIO_PUMP` | 1 | 0 = DraStic's own SDL audio |
| `DSFLIP_SHADER_COPY` | 0 | 1 = upload DraStic's frames to the GPU instead of importing them |
| `DSFLIP_FALLBACK` | 1 | 0 = don't run DraStic without SuperDrastic when it can't take the display |

**Shaders** are GLSL ES 1.0 fragment shaders with the inputs DraStic's stock shaders use (`u_texture`,
`u_texture_size`, `u_output_size`, `v_texcoord`; see the top of `src/shader.c`). A shader that draws the DS screen
into part of the panel declares where (`// dsflip-viewport: x y w h`), and touch follows it. On ROCKNIX, its
built-in DraStic shaders (lcd3x, sharp-bilinear and the rest) are found in its `libdrastouch.so` by name.

## Examples

- **ROCKNIX:** [ROCKNIXDS](https://github.com/JorreFog/ROCKNIXDS) is the full integration (EmulationStation, sway,
  play stats, a DSi-style theme). Doing it by hand: ROCKNIX runs `/storage/.config/drastic/drastic` for DS games,
  from inside EmulationStation's own service, so a launcher that stops EmulationStation would stop itself. Start
  it in its own unit instead, and let it stop and start the frontend:

  ```sh
  #!/bin/sh
  # /storage/.config/drastic/drastic, with the real binary moved to drastic.real and a link to it named "drastic"
  # (mkdir -p /storage/.config/drastic/sd && ln -s ../drastic.real /storage/.config/drastic/sd/drastic)
  systemd-run --unit=superdrastic --collect -E XDG_RUNTIME_DIR=/var/run/0-runtime-dir \
      -E DSFLIP_HANDOVER=cmd -E DSFLIP_STOP_CMD="systemctl stop essway sway" \
      -E DSFLIP_START_CMD="systemctl start sway essway" -E DSHOOK_SHADER="$DSHOOK_SHADER" \
      -E DSFLIP_DRASTIC_DIR=/storage/.config/drastic \
      /storage/superdrastic/superdrastic-run /storage/.config/drastic/sd/drastic "$@" &&
  exec sleep 86400      # EmulationStation waits here until the unit stops it
  ```

  `XDG_RUNTIME_DIR` is what DraStic's audio (ALSA through PipeWire) and the launcher's PipeWire rate need; a systemd
  unit doesn't have it. The link makes the process's name `drastic`, which ROCKNIX's exit hotkey kills, while
  `DSFLIP_DRASTIC_DIR` keeps DraStic reading its configuration from its own folder.
- **A frontend under a seatd/logind Wayland compositor:** the default `vt` handover, with the frontend's window kept
  during games.
- **A frontend that is itself a KMS program** and quits (or releases the display) while a game runs: `none`.

## Problems

Look at `logs/superdrastic.log` first, then open an issue at https://github.com/JorreFog/SuperDrastic/issues with it
attached and your device's and firmware's name and version.
