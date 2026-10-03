# tools

Test and measurement tools, run on the device unless noted.

| Tool | |
|---|---|
| `shtest.c` | Runs one shader on a frame without DraStic and times it (`sh build.sh <sysroot> shtest`) |
| `touchtap.py` | Injects a real tap or swipe into the touchscreen (evdev write) |
| `padkey.py` | Presses a gamepad button the same way |
| `touchcal.c` | Draws crosshairs on the panels to calibrate the touchscreen (KMS) |
| `dsprobe.c` | Logs DraStic's display path (SDL calls and timings), for studying a DraStic build |
| `kmstest.c` | Minimal KMS bring-up test for two panels |
| `ramp.py` | Maps a stress-ROM run's presents to its levels |
| `shaders.sh`, `shbench.sh` | GPU time per shader on real frames (run `shaders.sh` on a PC with `RGDS_SSH`/`RGDS_HOST`; it pushes `build/shtest`, `shaders/` and `shbench.sh` to the device) |
| `gen-color-shaders.py` | Writes `ds-crisp-color` and `ds-grid-color` from `ds-crisp` and `ds-grid` (on a PC) |
| `resume-keep-test.c` | Host test: a quit-save leaves the previous resume file until the new one replaces it (`clang -O2 -o /tmp/resume-keep-test tools/resume-keep-test.c src/resume.c -ldl -lpthread`) |
| `dev/` | The scripts used while bringing SuperDrastic up on the RG DS under ROCKNIX (they assume its paths) |
