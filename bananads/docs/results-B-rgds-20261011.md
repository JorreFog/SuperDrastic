# BananaDS milestone B on the RG DS (non-Plus): 2026-10-11

Kit `bananads-device-e67e3ef` (the PR #9 merge commit of 4536791), from CI run 38136958396, run with
`sh device-test.sh` (no arguments) on an Anbernic RG DS, ROCKNIXDS `next` build of 2026-10-01 (glibc 2.41).

## Summary

- **Correctness: 16 of 16 PASS on the A55s.** No FAIL, no crash, no hang.
- **BananaJIT is faster than the interpreter but slower than melonDS's JIT.** At 1992 MHz it is behind by
  5.3 fps on L5, 7.2 on L10 and 11.4 with the ARM7 busy (79% vs 98% of a DS). On L1 all three are equal
  (rendering is the limit).
- **Below 100% at the full clock:** every configuration on L10 and on L5 with the ARM7 busy. On L5, only
  melonDS's JIT reaches 100% (59.9 fps). BananaJIT gets 91%. L1 is above 100% for all three (133-137%).
- **`render.threaded=on` is slower:** 48.6 fps on L10 against 52.2 off.
- **Temperatures:** 38 °C at the start, 43-50 °C after each run, 45 °C at the end. It never got hot or slow.

## Conditions

- **No games:** `/storage/roms/nds` was empty, so only the kit's stress ROMs were run.
- **Battery:** the run started on battery at 35%. The charger was connected during the speed section,
  between L5 at 1104 MHz and the busy-ARM7 rows. Every row pins the clock, so the effect should be small.
- **What was running:** ES idle in the menu under sway, with no game running.
- **The governor:** restored afterwards to schedutil, 408000-1992000 kHz, the same as before.

## The report from device-test.sh

BananaDS e67e3ef (SDL window: ON), from `device-test.sh`.

| | |
|---|---|
| Device | Anbernic RG DS |
| Kernel | 7.0.2 |
| OS |  |
| CPU frequencies (kHz) | 408000 600000 816000 1104000 1416000 1608000 1800000 1992000  |
| Governor before | schedutil (408000-1992000 kHz) |
| Memory | 3051676 kB total, 2654740 kB available |
| Temperature at start | 38 °C |
| Running | emulationstation sway  |

## 1. Correctness (BananaJIT against the interpreter, on the A55s)

| Check | Result |
|---|---|
| pictures, stress L5 (arm) | PASS |
| pictures, stress L5 (thumb) | PASS |
| pictures, stress L5 (arm7) | PASS |
| pictures, stress L5 (arm7-thumb) | PASS |
| exact: jit.fastloads off = on (L5, Thumb) | PASS |
| exact: jit.faststores off = on (L5, Thumb) | PASS |
| exact: jit.link off = on (L5, Thumb) | PASS |
| exact: jit.mul off = on (L5, Thumb) | PASS |
| exact: jit.fastmem7 off = on (ARM7 workload) | PASS |
| exact: jit.mul off = on (ARM7 workload) | PASS |
| diff.nds: interpreter = ARMv5TE | PASS |
| diff.nds: BananaJIT = ARMv5TE | PASS |
| diff.nds: melonDS JIT = ARMv5TE | PASS |
| diff_extra.nds: interpreter = ARMv5TE | PASS |
| diff_extra.nds: BananaJIT = ARMv5TE | PASS |
| diff_extra.nds: melonDS JIT = ARMv5TE | PASS |

## 2. Speed (headless: frames as fast as the CPUs go; 100% = a DS's 59.83 fps)

`headless` runs flat out with no display or sound, so the percentage is headroom: above 100%, that
configuration can hold full speed with the rest to spare. 3D is melonDS's software renderer at 1x
(DraStic's bar on these handhelds is 60 fps at 2x, through SuperDrastic).

| ROM | Clock | Interpreter | melonDS JIT | BananaJIT | °C after |
|---|---|---|---|---|---|
| stress L1 (light 3D) | 1992 MHz | 81.8 fps (137%) | 81.3 fps (136%) | 79.7 fps (133%) | 50 |
| stress L1 (light 3D) | 1104 MHz | 54.2 fps (91%) | 54.5 fps (91%) | 54.2 fps (91%) | 45 |
| stress L5 | 1992 MHz | 49.5 fps (83%) | 59.9 fps (100%) | 54.6 fps (91%) | 48 |
| stress L5 | 1104 MHz | 30.7 fps (51%) | 37.3 fps (62%) | 34.0 fps (57%) | 45 |
| stress L10 (heaviest 3D) | 1992 MHz | 45.2 fps (76%) | 55.6 fps (93%) | 48.4 fps (81%) | 48 |
| stress L10 (heaviest 3D) | 1104 MHz | 28.5 fps (48%) | 35.2 fps (59%) | 31.2 fps (52%) | 43 |
| stress L5, ARM7 busy (Thumb) | 1992 MHz | 36.6 fps (61%) | 58.9 fps (98%) | 47.5 fps (79%) | 48 |
| stress L5, ARM7 busy (Thumb) | 1104 MHz | 23.3 fps (39%) | 35.8 fps (60%) | 29.0 fps (48%) | 45 |

### The 3D thread (one ROM, BananaJIT, full clock)

| render.threaded | fps |
|---|---|
| on | 48.6 |
| off | 52.2 |

Temperature at the end: 45 °C. Raw log: `bananads-results-20261011-080419.log`.

**Correctness: 0 check(s) failed.**
