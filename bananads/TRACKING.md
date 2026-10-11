# Work in progress, and what it means for BananaDS

A lot is moving in SuperDrastic and ROCKNIXDS at once. This is the list of what's open, as of **2026-10-11**, and
what each piece means for BananaDS. Update it when a PR merges or a branch moves: change the row, and port what
the "For BananaDS" column asks for.

**Names:** "codename Banana" (ROCKNIXDS `banana` branch, PR #60, `BANANA.md`) is the post-1.6.1 work for
DraStic/SuperDrastic on one test branch. **BananaDS** is this emulator. They are different things. BananaDS isn't
part of codename Banana's device checks.

## SuperDrastic (JorreFog/SuperDrastic)

| PR / branch | What | State | For BananaDS |
|---|---|---|---|
| #8 `claude/gengis-a55-scheduling` | Gengis Engine: A55 scheduling, pipelined kernels, a NEON walker, fused resolves, DraStic's geometry stage and 2D routines replaced | draft, active (updated 2026-10-11) | Milestone F, option 2. Its `walk.c` and `res2.c` are the hi-res walker and resolve a melonDS `Renderer3D` would need. `dsgeom.c` and `ds2d.c` are DraStic-only. Track its kernels' measured costs |
| #7 `claude/open-issues-rocknix-nightly-1l4s4w` | 0.5.0-beta.1-rocknixds.8: in-game menu fixes (#3, #4, #5), ROCKNIXDS #26 mic and #49 brightness, #47 3x lines | draft, in `banana` | The mic fixes (DC blocker, leak from the level above the floor, hysteresis, 120 ms hold) **are in `src/mic.cpp`**. Port the menu fixes and the per-screen brightness sliders with `menu.c` (E) |
| #6 `claude/cpugov-3x-memory` | cpugov: 3x gets its own remembered-drops file | draft, not in `banana` | When `cpugov.c` is ported (E), key its memory per renderer and scale from the start |
| #1 `cursor/critical-bug-management-657e` | The quit save keeps the previous resume state if the new one doesn't finish | draft | Covered: BananaDS writes every save and state to a `.tmp` file and renames it |
| `1.6-prep` (3de7875, 0861654, d8291e0, 01269ed) | The parked WFC work, the mic tunables | parked | WFC: melonDS replaces `wifi.c`. Its DNS presets and the per-device-MAC lesson are in `src/wifi.cpp`. `wfcnet.c` is the fallback if slirp is too heavy. Mic tunables: in, with the same names (docs/PORTING.md) |
| `opt/compfuse`, `release-0.4.0-beta.2` | Earlier rasterizer work and a release branch | inactive | Nothing |

## ROCKNIXDS (JorreFog/ROCKNIXDS)

| PR / branch | What | State | For BananaDS |
|---|---|---|---|
| #60 `banana` | Codename Banana: #58, #55 and #57 merged for device testing | draft | H: once BananaDS runs on a device, it can join `banana` as a core to try, with a row in `BANANA.md` |
| #58 `claude/open-issues-rocknix-nightly-1l4s4w` | Open issues after 1.6.1, the base ROCKNIX nightly from the menu, SuperDrastic `.8` | draft | Its save folders (#42/#56) are `save.per_game_folder`. *Apply recommended settings* (#45) needs BananaDS rows at H |
| #59 `claude/irq-affinity-test` | A/B test for interrupt placement, the kernel tick | draft | B: run BananaDS's benchmark with and without its placement. The JIT thread wants the quiet core too |
| #57 `claude/perf-logs-1.5.12` | Perf logs record model, renderer, 3D resolution | draft, in `banana` | H: write the same perf-log format, with the emulator recorded |
| #55 `claude/app-store` | ROCKNIXDS Store | draft, in `banana` | H: a way to ship BananaDS before it is in the image |
| #54 `claude/rocknixds-feature-parity-1wpizz` | Undervolt tool | draft, not in Banana | B: measure with and without it |
| #39, #38, #29, #18, #14 (`cursor/*`) | Test coverage and a perf-log fix | draft | Nothing directly |
| #20 / #19 `cursor/resume-off-quit-hotkey(-plus)` | The quit hotkey kills DraStic when resume on quit is off | draft | E: the quit hotkey ends the game in every case (the 1.6 stall-watch lesson) |
| #17 `cursor/drastic-wfc-dns-24ad` | DraStic online DNS servers | draft | Its presets (Kaeru, AltWFC, WiiLink) are `wifi.dns`'s. The ES option `nds.wfc_dns` maps onto it at H |
| #7, #6 `cursor/*816mhz*` | 816 MHz research | draft | B: the low-clock numbers to compare with |
| #5, #23 | 1.5 beta review, menu mockups | draft | Nothing |
| `rgds-1.6.1`, `plus-beta`, `main` | The released lines (1.6.1 for RG DS, 1.6.1-plus for RG DS Plus) | released | BananaDS must run on both panels' sizes (640x480 and 1024x768) at D |

## Open questions for the owner

1. **Where BananaDS lives.** It's in `bananads/` on SuperDrastic's `claude/bananads` branch for now, because it
   grows out of SuperDrastic. It's GPL (melonDS) where SuperDrastic is MIT, so a repository of its own
   (`JorreFog/BananaDS`) would be cleaner once it runs on a device.
2. **B's result decides F**, and whether BananaDS replaces DraStic for every game or sits next to it as the core
   for the games that need Wi-Fi or the mic.
