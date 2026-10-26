# Mario Kart DS at 4x, 60 fps stable (2026-10-26). Read BRIEF.md (device lock, scripts, user rules) first.

## Goal (the user's)
Mario Kart DS (USA) at `render.scale=4` on the RG DS Plus: **60 fps stable** = 0 frames lost and 0 repeats in a full
race (3 laps) at the user's other settings (cpugov on), audio clean. Target setup: 4x with shader **None** (4x =
1024x768 = the panels' own size, shown 1:1); also report with the LCD grid (ds-grid-2x) on. No frame skipping, no
lowered 3D quality, no emulation-timing changes: real 60.

## Where it stands (X3, reports/x3.md)
- 0.5.0 beta build (installed on the Plus): BananaDS `release/0.5.0-bd` @ b4c6c67 (= JIT round + 3x/4x + DSi).
- MKDS title/demo at 4x: 53.4 fps (~193 lost / 35 s); at 3x 56.8; 2x 59.6-59.9. HG grass at 4x: 60.
- At 4x the CPU sits at 1992 MHz, busiest thread 85-93%: 3 Gengis render threads can't keep up, so the emulator thread
  draws 7-12% of the 3D bins itself; the 2D engine composites each 3D line 16x (at 4x); the shader pass is 9.5 ms
  a frame at 4x (vs ~5 ms at 2x); 4 cores for emu + 3 Gengis + 2D + presenter + audio = oversubscribed.
- Memory notes worth reading: bananads-gs60-perf.md (shader cost, 2D/3D sharing, thermal throttle at 70 C),
  bananads-session3-device-results.md (MKDS race-start dips, geom_thread hurt), bananads-device-testing.md.

## Base, build, bench
- Base: `release/0.5.0-bd` @ b4c6c67. Worktree: `git -C ~/superdrastic worktree add -b mk4x/<id> ~/mk4x/wt/<id> release/0.5.0-bd`;
  build `~/bdnight/tools/bdbuild.sh ~/mk4x/wt/<id> ~/mk4x/obj/<id>` (13 s). New melonDS patches: M1 0500-0509,
  M2 0510-0519, M3 0520-0529, M4 0530-0539, M5 0540-0549, M6 0550-0559.
- THE BENCH (M1 builds it first, ~45 min; until then use the MKDS title/demo via ~/bdnight/dev/gbench.sh with
  GAME="Mario Kart DS (USA) (En,Fr,De,Es,It)" and BANANADS_RENDER_SCALE=4 BANANADS_KMS_SHADER=none):
  `~/mk4x/tools/mkbench.sh <tag> <bin> [secs] [KEY=value...]` on a saved MKDS race state (made from a COPY of the
  user's save), scripted accelerate input (A held + steering pattern, deterministic by emulated frame if possible),
  reports fps windows, frames lost, repeats, per-thread CPU, clocks, temps, GPU time, end screenshot.
- Device: ONLY via `~/bdnight/tools/plus.sh` / the lock; M1 has priority. The Plus now runs 0.5.0 beta from slot a:
  never touch the user's saves/states (copies only), never leave mounts/env behind.

## Exactness (non-negotiable)
Rendering changes must not change emulation: the emulated sound (audio tap, `~/bdnight/tools/audiotest.sh` and
`audiocheck.py`) of a run at 4x must be bit-identical to the same run at 2x on the base binary (render-only
changes can't alter the sound; if it differs, the change leaked into emulation). 2x pictures must stay identical
(`regress-all.sh` + screenshots). Speed changes that touch emulation: J6's gate (`~/bdnight/tools/j6-regress.sh`).
No headless emulation runs (user rule).

## Who
- **M1 lead + bench + integration:** make the MKDS race state + mkbench.sh (above), baseline numbers (4x none / 4x
  grid / 3x / 2x), a per-frame timeline of where 4x misses (which thread is late: emu, Gengis bins, 2D, presenter),
  then integrate others' changes into `mk4x/integ`, measure, run the gates, keep ~/mk4x/STATUS.md (numbers / now /
  next) every ~45 min and a table on BOARD.md "MK4x".
- **M2 Gengis raster at 4x:** per-pixel kernels and bins at 1024x768 (4x the 2x pixels): tile/bin sizes, kernel cost
  per pixel (MKDS polygons: textured, translucent, fog, edge marking, toon?), work split so the 3 workers finish
  before the 2D thread needs the rows; no 3D on the emu thread.
- **M3 2D compositing at 4x:** the 16 passes per 3D line: compose the 2D layers once at 1x and expand only where
  needed, NEON, reuse unchanged lines (P7's skip), the 4x output path (bytes touched per frame), maybe 2 2D threads.
- **M4 presenter + shader + GPU at 4x:** shader None at 1:1 should need no GPU pass at all (scan out the 4x buffer
  directly: zero-copy, like 2x without shader); with the grid: a cheaper 4x grid twin; GPU clock floor/thermal;
  the presenter's own CPU.
- **M5 threads/cores/cpugov/thermal:** 4 A55s for emu (exclusive core, sched.emu_excl) + 3 Gengis + 2D + presenter +
  audio: placement, worker counts per scale, wake/sleep cost (P4's gengis_wake), priorities, cpugov behaviour at 4x,
  temperature over a 3-lap race (throttle at ~70 C).
- **M6 MKDS emulator thread:** what the emu thread does in an MKDS race (geometry-heavy: P5's GX notes, the JIT
  round's tools jit.stats/prof.hz/PERFMAP) and exact speedups for it, since at 4x it must still finish its frame
  while sharing the cores; anything that reduces its share helps all scales.

Every agent: worktree + branch as above, small commits with footers (`Co-Authored-By: Claude Opus 5.5
<noreply@anthropic.com>`, `Claude-Session: https://claude.ai/code/session_016CSJDDhSRQcmt16xoCea8M`), report
~/mk4x/reports/<id>.md with hourly checkpoints, "ready" lines on BOARD.md "MK4x" for M1. ~4 hours. Token-lean, no
sub-agents, nothing pushed. Final reply: short summary with numbers.
