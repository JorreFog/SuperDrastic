# MK4x status (M1). MKDS at 4x, 60 fps stable on the RG DS Plus

Bench: ~/mk4x/tools/mkbench.sh (race.mst Figure-8 grid + kms.input A/steer v1, 40 s, SETTLE 4, user settings + explicit
scale; MKDS's per-game card setting is 3x). Base build ~/mk4x/obj/base = b4c6c67 + kms.input (mk4x/bench a8941d6).

## Numbers
| run | settings | fps | windows < 59 | lost | repeats | emu | gengis(3) | 2d | temp |
|---|---|---|---|---|---|---|---|---|---|
| b4n | 4x none | 41.60 | 28/28 | ~515 | 778 | 85.3% | 166.4% | 80.4% | 49-67 C |
| b4g | 4x grid | 41.01 | 29/29 | ~551 | 844 | 84.9% | 166.7% | 79.6% | 49-68 C |
| b3g | 3x grid (card) | 59.34 | 3/40 | ~32 | 41 | 61.4% | 147.8% | 64.7% | 50-62 C |
| b2g | 2x grid | 60.16 | 0/41 | 0 | 0 | 54.7% | 64.4% | 37.7% | 49-54 C |
| tl4n | 4x none, traced | 40.89 | 29/29 | ~554 | 854 | 87.4% | 166.4% | 79.2% | 49-68 C |
| i1-4n | integ 9e78792 (+M2 hrneon), 4x none | 47.55 | 33/33 | ~411 | 545 | - | - | - | 46-66 C |
| i2-4n | integ 1351bb1 (+M4 commit_thread), 4x none, traced | 46.88 | 33/33 | ~433 | 579 | 92.3% | 157.9% | 88.3% | 50-69 C |
| i3-4n | integ a122eb7 (+M3 2D), 4x none | 50.56 | 35/35 | ~330 | 420 | 90.5% | 176.7% | 67.4% | 49-68 C |
| i4-4n | integ b1c7cc9 (+M3 unit B key), 4x none | 51.79 | 36/36 | ~296 | 372 | 90.6% | 176.6% | 68.1% | 49-69 C |
| i5-4n | integ d43e161 (+M2 36a4073, +M4 93b0e71), 4x none | 54.74 | 30/39 | ~203 | 255 | 91.2% | 200.3% | 65.2% | 49-69 C |
| i5-4g | same, "grid" (REALLY none: card shader is None) | 54.65 | 29/38 | ~199 | 252 | 91.8% | 199.9% | 65.9% | 49-69 C |

NOTE: b4g / i5-4g were not grid runs (card shader = None now); grid needs BANANADS_KMS_SHADER=ds-grid-2x.
Gates: i5 taps IDENTICAL (41.1 s); 2x regress-all on i4: 9/9 alive, at rel1's level.
Gates: i3, i4 taps IDENTICAL to tapref-2x (37.8 / 38.7 s). Gates: tap of i1-4n and i2-4n == base 2x tap (tapref-2x) over 35.5 / 35.2 s: IDENTICAL.
i2 timeline: emu start->pub avg 21.7 ms (p90 25.1), Gengis bins 42.5 ms CPU/frame (was 52.6).

Timeline (tl4n): emu frame start->publish avg 25.0 ms (p90 30.3) = the late stage in every repeat; Gengis bins 52.6 ms
CPU/frame (all threads), 2D ~19.5 ms, emu ~10, presenter+audio ~3: ~85 ms CPU per frame vs 66.8 available (4 cores).

## Now (15:00): session ended (no device runs). HEAD ff7de99 = d43e161 + 45daa39 + M5, built as i6, unmeasured. Handoff: ~/mk4x/FINAL.md.
## Earlier (13:15)
- i3 = i2 + M3 09d2d78 (2D) -> a122eb7: 4x tap gate running. Laps steering: dropped for now (4 tries; the kart scrapes the first curve's wall); FINAL = long runs from the grid state.
## Next
- merge M3/M4/M5/M6 as they post ready; 2x regress-all on a candidate; laps steering (v1 input ends at a wall).
