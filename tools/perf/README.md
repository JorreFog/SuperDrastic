# tools/perf: measuring SuperDrastic on the handheld

What the 2026-10-05 cache and pacing work was measured with. Rough tools: they assume ROCKNIXDS on an RG DS Plus,
ROMs and savestates at the paths in `device/bench.sh`, and an ssh wrapper for the handheld (`RGDS_RG`, `RGDS_HOST`).

`device/` (copied to `/storage/dsflip/opt/` on the handheld):

- `bench.sh <tag> <game> [NAME=value...]`: one run of a game with a test library on the bare panels (ES and sway
  stopped; `bench.sh restore` brings them back): per-thread CPU ms a frame, `perf stat`/`perf record`, the log.
  Refuses to start while a player's game session runs.
- `pace.sh <tag> <library|installed> <game> <secs> [NAME=value...]`: the same game through the real path (ES's API,
  session.sh with the player's profile, shader, CPU placement), walking, and the repeated/dropped frames a second.
  No perf and no read-backs: this is the one to judge smoothness with. Puts the installed library back afterwards.
- `walk.py`: walks in a square from one process (an interpreter per key press shows up as hitches).
- `hcmp.sh`, with the library's `DSFLIP_HASH=N[:from[:to]]` and `DSFLIP_TEST_TIME=<epoch>`: frame hashes of two
  runs compared (boot and attract sequences without input are deterministic once the DS's clock is fixed).
  `DSFLIP_DUMP=dir:frame[,frame...]` writes those frames' screens for a pixel diff. Both read scanout memory
  back (tens of ms a frame): they stutter, by design.
- `maps.py <pid>` (large mappings, resident and huge-page sizes), `lutprobe.py` (the CRTCs' GAMMA_LUT),
  `drmprops.py`, `colortest.sh` (ROCKNIXDS's screen colour tool under sway).

`host/`: wrappers that run the above over ssh in a systemd unit and collect the results (`bench`, `hashcmp2`,
`paceseries`, `lockseries`, `series1`), `sdbuild` (a build with symbols next to the stripped one), `annot`
(per-instruction sample shares from a `perf record`), `shot` (a scanout dump as PNG).
