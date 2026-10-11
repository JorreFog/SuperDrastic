# Testing BananaDS on a handheld

This kit tests BananaDS on an RG DS or RG DS Plus running ROCKNIX(DS). It checks two things:
- **Correctness on real hardware:** BananaJIT against melonDS's interpreter, on the A55 cores rather than under qemu.
- **Speed:** milestone B in `docs/PLAN.md`. Here, "the JIT" means BananaJIT unless it says melonDS's.

BananaDS has no handheld display path yet (milestone D). So the main test is **headless**: no picture, no sound,
frames as fast as the CPUs go. A desktop-window test under ROCKNIX's sway is optional, at the end.

## What's in the kit

| | |
|---|---|
| `bin/bananads-bananajit` | BananaDS with BananaJIT (`jit=off` gives melonDS's interpreter) |
| `bin/bananads-melonjit` | BananaDS with melonDS's own JIT (the comparison) |
| `bin/bananads-difftest-*` | The difference harness (two runs of a ROM compared frame by frame) |
| `roms/stress/{arm,thumb,arm7,arm7-thumb}/` | SuperDrastic's 3D stress ROMs: the ARM9 code as ARM or as Thumb, and with an ARM7 that never halts |
| `roms/diff/` | The DraStic JIT study's differential ROMs (one instruction per test, expected values in `tests*.txt`) |
| `device-test.sh` | Runs everything and writes the report |
| `tools/check_results.py` | Checks a differential ROM's results (needs python3, which ROCKNIX has) |

The binaries are aarch64, linked against the handheld's own glibc (2.39 or newer), SDL2 and ALSA. libstdc++
is linked in.

## Getting the kit

- **From CI:** the newest green `bananads` workflow run on the `claude/bananads` branch has an artifact named
  `bananads-device`:
  ```sh
  gh run download -R JorreFog/SuperDrastic -n bananads-device \
    $(gh run list -R JorreFog/SuperDrastic -b claude/bananads -w bananads -s success -L 1 --json databaseId -q '.[0].databaseId')
  ```
  The zip holds `bananads-device-<commit>.tar.gz`.
- **Or build it:** on an x86-64 Debian/Ubuntu PC, run `sh bananads/tools/package-device.sh` from a SuperDrastic
  checkout of `claude/bananads`. Its header lists the packages it needs.

## Running it

```sh
scp bananads-device-*.tar.gz root@<handheld>:/storage/      # password: rocknix
ssh root@<handheld>
cd /storage && tar xzf bananads-device-*.tar.gz && cd bananads-device
sh device-test.sh                       # ~30-45 min; or: sh device-test.sh --quick   (~10 min)
sh device-test.sh /storage/roms/nds/Game1.nds /storage/roms/nds/Game2.nds   # choose the games
```

- **Before:** no game running. Leave the handheld in the menu (EmulationStation idle), plugged in, and keep it
  the same for the whole run.
- **What it touches:**
  - Saves and states only in `/tmp/bananads-test`, so your saves are never read or written.
  - The CPU governor and limits (policy0): set for the benchmark, put back on exit, Ctrl-C included.
- **Games:** with no paths given, it takes up to three `.nds` files from `/storage/roms/nds`. BananaDS reads
  plain `.nds` only, not `.zip`/`.7z`.
- **Output:** `/storage/bananads-results-<date>.md`, the report, and `.log`, the raw output.
- **Exit code:** 0 when every correctness check passed.

### What it does

1. **Correctness:**
   - The stress ROM's pictures at frames 150/300/450/600, BananaJIT against the interpreter, for all four ROM
     variants.
   - Each BananaJIT fast path off against on, with main RAM compared after every frame: `fastloads`,
     `faststores`, `link`, `mul`, `fastmem7`.
   - The differential ROMs on the interpreter, BananaJIT and melonDS's JIT, against the ARMv5TE values.
   - On qemu all of these pass. On the handheld, a failure would point at something only real hardware does, such
     as instruction-cache flushing after code is written.
2. **Speed:**
   - Headless fps for the interpreter, melonDS's JIT and BananaJIT.
   - On stress L1/L5/L10, L5 with the ARM7 busy, and the games.
   - At the full clock and at 1104 MHz, with 15 s cool-downs.
   - Also, once: the 3D renderer's thread on and off.

## Optional: a desktop window under sway (sound, the microphone, the stylus)

The binaries include SDL2 (`VERSION` says `SDL window: ON`). Under ROCKNIX's sway, a window may come up with
picture and sound.
- This is not the handheld path: sway composites, and the window may span or land on one panel.
- The pad isn't mapped in the window: it reads a keyboard.

So this only checks that it runs, that sound plays, and that the mic reaches the game.

```sh
export $(tr '\0' '\n' < /proc/$(pidof sway)/environ | grep -E '^(XDG_RUNTIME_DIR|WAYLAND_DISPLAY)=')   # sway's own
timeout 60 ./bin/bananads-bananajit --set save.dir=/tmp/bananads-test --set resume.on_start=off \
  --set resume.on_quit=off --set log.file=/storage/bananads-sdl.log /storage/roms/nds/<game>.nds
```

`timeout 60` ends it. Look in `/storage/bananads-sdl.log` for `[sdl]` and `[mic]` lines, and note what showed and
whether there was sound.

## What to send back

- `bananads-results-<date>.md`, the report, and `bananads-results-<date>.log`.
- For any FAIL: the log's lines around it.
- Anything odd: a crash, the handheld getting hot, a game that won't boot. melonDS needs no BIOS, but a game
  that fails with its FreeBIOS is worth naming.
