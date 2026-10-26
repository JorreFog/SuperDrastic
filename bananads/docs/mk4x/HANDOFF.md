# MK4x FINAL + HANDOFF for tomorrow (M1, 2026-10-26 15:00). Mario Kart DS at render.scale=4 on the RG DS Plus

**Verdict: not yet 60 fps stable at 4x.** The final candidate runs MKDS's race at **~54.5 fps sustained** (base 41.6),
4x shader None and the LCD grid alike, but 160 of ~165 one-second windows are still under 59 in a 3-minute run, and
the SoC reaches the 70 C trip at ~40-45 s, after which the kernel holds 1416-1608 MHz for the rest of the race.

## Branch state (mk4x/integ, worktree ~/mk4x/wt/integ, nothing pushed)
- **HEAD ff7de99** = the measured candidate d43e161 + 45daa39 (grid toggle forgets out_skip rows: M4's merge note) +
  merge of mk4x/M5 (728a3b8 cpugov: late frames under the kernel's thermal cap don't strike a clock; 4667be6
  cpugov.sum_limit auto 3.3 cores at 3x/4x; b6918a9 sched.emu_share A/B setting, off). **45daa39 and M5 are built
  (~/mk4x/obj/integ/bananads.i6) but NOT run on the device** (session ended: no device runs): first thing tomorrow =
  mktap 4x + one 180 s run on i6 (commands below). Cpugov/sched changes can't change emulation; the tap gate still applies.

## Measured candidate
`mk4x/integ` @ **d43e161** (worktree ~/mk4x/wt/integ, build ~/mk4x/obj/integ/bananads.i5), on release/0.5.0-bd b4c6c67:
- a8941d6 kms.input (M1): frame-exact scripted input for the bench (off unless set)
- M2 b84bc86 + 36a4073: Gengis hi-res NEON for shadow/decal/toon/highlight polygons, 4x bin halves as work units,
  keyskip, texture min-alpha at decode (gengis_lazy off)
- M4 e662f8f + 93b0e71: kms.commit_thread (auto at 3x/4x), kms.grid_cpu (the ds-grid-2x grid written by the 2D engine at
  4x, no GPU pass; tables read back from the GPU's own shader output, 0 of 2359296 bytes disagree)
- M3 09d2d78 + 9bd207a: raw 3D step under windows, 3x/4x split compositing, render.2d_out_skip, unit B cache key by drawn content
Nothing pushed. In HEAD but not measured: 45daa39, M5. Not integrated: M6 (no READY), G1/G2 GPU 3D (in progress).

## Final runs (user settings: cpugov on; card shader is None now, so the grid was passed explicitly; 180 s measured)
Bench: race.mst (Figure-8 Circuit GP, 8 karts on the grid before the countdown) + kms.input v1 (A from GO + a fixed
left pulse pattern). **Not a 3-lap race**: the scripted steering takes the kart through the start with the pack, then it
ends against the first curve's outer wall (4 steering tries failed; see "Open"), so 180 s = start + ~165 s of the
race view at the wall (the same 3D scene load class: fps there 53-56 as in the start).

| run | 4x shader | fps | windows < 59 | lost | repeats | audio short | SoC | clock |
|---|---|---|---|---|---|---|---|---|
| f4n1 | None | 54.60 | 158/166 | ~890 | 1046 | 0 | 48-70 C | 1992 to ~45 s, then 1416/1608 |
| f4n2 | None | 54.39 | 161/165 | ~922 | 1075 | 0 | 50-70 C | same |
| f4g1 | ds-grid-2x (CPU grid) | 54.59 | 162/166 | ~894 | 1049 | 0 | 48-69 C | same |
| f4g2 | ds-grid-2x (CPU grid) | 54.15 | 161/163 | ~952 | 1156 | 0 | 50-71 C | same |

40 s runs (cold start, cpugov on, 4x None): base 41.60 -> i1 (+M2 hrneon) 47.55 -> i2 (+M4 commit) 46.88 (traced) ->
i3 (+M3) 50.56 -> i4 (+M3 key) 51.79 -> **i5 54.74** (lost ~515 -> ~203). 3x (card) base 59.34, 2x base 60.16 (0 lost).
Base with the real GPU grid (b4g2, explicit ds-grid-2x, 40 s): **37.09 fps** (lost ~596) -> the final grid runs are +47%.

## Gates
- Exactness: the emulated sound (audio.tap from the state load) of every integ step at 4x (i1..i5, None and grid) is
  **bit-identical** to the base binary's 2x run (tapref-2x) over 35-41 s (~mktap.sh).
- 2x regression (regress-all, i4 = i5 minus M2 36a4073/M4 93b0e71 whose new paths are off at 2x): 9/9 games alive, fps
  59.83-60.02 at rel1's level, no new audio pattern. Not re-run on i5 itself.
- Grid picture: shots LOOKED (grid cells visible, same as the GPU path by M4's table/writer self-checks).

## Where the time goes (timeline, ~/mk4x/runs/tl4n + i2-4n, ~/mk4x/tools/mktrace.py)
The emulator thread's frame (start -> publish) is late in every repeat (base avg 25.0 ms, i2 21.7). It is total-CPU and
memory bound: base ~85 ms CPU per frame (Gengis bins 52.6, 2D ~19.5, emu ~10, rest ~3) vs 66.8 available; Gengis is now
~39-42 ms. M6: the emulation itself takes ~+55% cycles at 4x from L3/DRAM contention; M5: clock 1992 -> 1608 costs only
~2% fps (memory-bound), and the thermal trip at ~40 s is unavoidable at today's load.

## mkbench numbers summary (user settings, cpugov on, 40 s cold unless noted)
| scale / shader | base b4c6c67 (+kms.input) | integ d43e161 |
|---|---|---|
| 4x None | 41.60 (lost ~515) | 54.74 (lost ~203); 180 s: 54.60 / 54.39 |
| 4x ds-grid-2x | 37.09 GPU grid (lost ~596) | 180 s: 54.59 / 54.15 (CPU grid) |
| 3x (card's MKDS setting; card shader None) | 59.34 (3 windows < 59, lost ~32) | not run |
| 2x | 60.16 (0 lost) | 2x regress-all on i4: MKDS 59.83 (lost ~11, same as rel1) |

## Resume commands (all device use through the lock; ~/mk4x/M1-NEXT makes other mkbench callers wait)
- build: `~/bdnight/tools/bdbuild.sh ~/mk4x/wt/integ ~/mk4x/obj/integ` (snapshot: `cp ~/mk4x/obj/integ/bananads ~/mk4x/obj/integ/bananads.iN`)
- gate (4x vs base 2x tap): `M1=1 SETTLE=4 ~/mk4x/tools/mktap.sh i6-4n ~/mk4x/obj/integ/bananads.i6 40 BANANADS_RENDER_SCALE=4 BANANADS_KMS_SHADER=none`
- grid: same + `BANANADS_KMS_SHADER=ds-grid-2x` (ALWAYS explicit: the card's shader is None now)
- long run: `M1=1 SETTLE=4 ~/mk4x/tools/mkbench.sh f6n1 ~/mk4x/obj/integ/bananads.i6 180 BANANADS_RENDER_SCALE=4 BANANADS_KMS_SHADER=none`
- timeline: add `BANANADS_KMS_LOWLAT_TRACE=/data/mk4x/runs/<tag>/trace.txt BANANADS_RENDER_GENGIS_STATS=1`, then
  `~/mk4x/tools/mktrace.py ~/mk4x/runs/<tag>/trace.txt ~/mk4x/runs/<tag>/bananads.log`
- 2x regression: `EXTRA="BANANADS_RENDER_SCALE=2" ~/bdnight/dev/regress-all.sh mk4x-i6 ~/mk4x/obj/integ/bananads.i6` (ref ~/bdnight/regress/rel1)
- merge a branch: `cd ~/mk4x/wt/integ && git merge mk4x/<id>` (footers from the brief); kms.input lives on mk4x/bench a8941d6
- tool edits: write a new file and `mv` it over (editing mkbench.sh in place broke runs in flight once)

## Open / next
1. ~10% more throughput is still needed for 60 sustained (54.5 -> 60) under the thermal cap (M5: 1992 -> 1608 costs
   only ~2%: memory-bound, so BYTES per frame are the lever, not cycles). Leads:
   - G1/G2 GPU 3D renderer (mk4x/G1 compute, mk4x/G2 classic GL -> GLES 3.1, render.gpu3d, off): takes the 3D raster
     (~40 ms CPU/frame) off the A55s; must prove pixel equality (or be an explicit setting) and the tap gate.
   - AFBC (compressed scanout / buffers) to cut the 4x output and scanout DRAM traffic.
   - M2 compact buffers: smaller Gengis per-pixel buffers (bytes streamed through the shared 512 KB L3 per frame).
   - M6 emu-thread memory: the emulation itself runs ~+55% cycles at 4x from L3/DRAM contention (IPC 0.45 -> 0.32);
     text ordering / hugepages / fewer render bytes. No M6 READY yet.
   - A thermal policy for 4x (cpugov holding <= 1608 at 4x: trip at ~77 s instead of ~36 s, ~2% fps), on top of M5.
2. Laps: a frame-scripted steering for 3 laps of Figure-8 (or a RAM-driven deterministic bot) for a true 3-lap FINAL.
3. 2x regress-all on i6; audio tap on a longer run; M5's audiocheck note: "pitch off ~9 cents" + startup short chunks at 4x in HG audiotests (identical with/without its change: a 4x startup-priming artifact to look at).
Files: ~/mk4x/STATUS.md, ~/mk4x/runs/<tag>/ (logs, shots, temps), tools ~/mk4x/tools (mkbench.sh, mkbench-dev.sh,
mktap.sh, mkinput.py, mktrace.py), device /data/mk4x (race.mst, race-f1350.mst, runs).
