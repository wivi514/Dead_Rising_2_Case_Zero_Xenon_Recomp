# Part 40 hand-off (for part 40). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part39-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

Part 39 found and implemented a whole input the renderer had been discarding since
phase 5: **the guest's mip chain**. A Xenos fetch constant names a second address for
mip levels 1..n; we uploaded one level and sampled it at every distance. Hardware
declares chains up to nine levels deep on the majority of its fetches. The chain is
built, its layout verified against hardware's own bytes, the packed tail declined and
counted, `CZ_VK_NO_MIPS=1` the control arm. The A/B was run twice and the first result
turned out to be a bug the guard then caught, so the chain stands on correctness alone
and does **not** show item 00i fixed — the packed tail, where the deepest minification
lives, is still declined. Item 00i's level-0 input was exonerated by an md5-identical content pairing
against R4 first, which is what pointed here. Item 0t's suspect (ALPHA-TO-MASK) is
**refuted** across 40,703 hardware draws and the item now needs one capture, not an
investigation.

## WHAT PART 39 DID — do not rebuild any of this

Records: `phase5-notes.md` §6bq; gotchas 295-300; `port-history.md`. Commits 40deac8,
b8b856f, 6a24b9e, adeabe9, d21cfcf and the wrap-ups.

* **The mip chain uploads by default.** `CZ_VK_NO_MIPS=1` restores the pre-part-39
  renderer. Counters: `mip: chain uploaded` (1,815 textures on the outdoor route),
  `mip: PACKED TAIL DECLINED`, `mip: CUBE chain not uploaded` (6), and
  `mip: level REJECTED — diverges from the level above`, the guard on the offset rule,
  **which fires on 254 of 1,818 chained textures on the outdoor route and drops
  those levels** (it PRINTS only eight — the rest are in the counter).
* **`tools/frame_arm_spread.py`** — compare two arms without picking which pair supplies
  the null (gotcha 300). Use it instead of `frame_era_medians.py` for any 3-runs-an-arm
  block.
* Both texture dumpers (`--dump-texture` and `live_texdump.py`) now read a TILED
  surface's whole footprint instead of `w*h*bpp`.
* `xenos::TextureFetch` now carries `mipAddress` and `packedMips`; both censuses (ours
  and `tools/xtr_draw_bindings.py`) print `mip=lo..hi mipAddr=…`, so the two are
  diffable in one vocabulary as they were designed to be.
* `xtr_draw_bindings.py` also carries **RB_COLORCONTROL, RB_ALPHA_REF,
  RB_BLENDCONTROL0 and RB_DEPTHCONTROL** per draw, decoded in words.
* **SIGTERM/SIGINT dump the renderer counters** — every headless recipe here ends with
  `timeout`, and that path printed nothing (gotcha 297).

## WHERE TO START

1. **THE PACKED MIP TAIL, and the guard has already told you where the rule breaks.**
   The divergence guard fires on **254 of 1,818 chained textures** (it prints eight),
   every printed one with a level 1 NARROWER than a macro tile and reading ~1/3 of its
   base's luma — a wrong PITCH, not
   corrupt data (the two chains verified by hand both have a 32-block-wide level 1, so
   neither could have shown it). Derive the packed-mip pitch/offset rule for exactly
   those shapes, and the guard becomes the regression test for it.
   **The A/B is done twice and says nothing about 00i.** First block (bad levels bound):
   mean luma −1.35%, resolved, agreeing with hardware's darker frames. Re-run with them
   rejected: **+0.36%, UNRESOLVED** — sign flipped, magnitude gone, so the first result
   was the bug and not the feature (gotcha 301). Distinct colours −5.45% against a 7.91%
   within-arm spread, also unresolved. Runs: `mipA5/A6/A7` against `mipB/B2/B3`. **The
   chain stands on correctness alone and no distant building panel regained its siding**,
   which is why the tail is item 1 rather than a nicety: we upload only down to the first
   sub-tile level and reject the chain outright on 254 textures, so the deepest
   minification — the range item 00i is about — still has no level at all.

2. **HOW TO DO IT.** Levels below one tile share a tile at sub-tile offsets this code
   declines and counts. The oracle method is worked out and cheap: pull the chain out of
   an R4 trace (a raw memory read + `tools/tex_decode.py`) and accept an offset only when
   the mean holds while distinct colours fall — §6bq's table has verified data points to
   fit against. The runtime already has the matching guard: `mip: level REJECTED —
   diverges from the level above` drops any level whose DXT endpoint luma is more than 32
   off the level above it, so a wrong offset announces itself instead of painting a
   distant surface the wrong colour. Its count is DETERMINISTIC (254 in every run), which
   makes it a usable regression test: derive a rule, and watch that number fall.
3. **Item 0t is now a CAPTURE REQUEST**: one single-frame F4 trace **standing at a
   shard tree** (the operator's `capture_f28446` location). `ps_c9ca4f73ba93d023` is
   absent from R4's 261-shader bank, so R4 cannot answer it. Add it to
   `docs/xenia-capture-requests.md` and ask.
4. **DO NOT RUN ANYTHING CPU-HEAVY BESIDE A MEASUREMENT RUN.** Part 39 lost one arm run
   to a stats census overlapping it and nearly lost two more to gate scripts. A 400 s
   headless run is a wall-clock experiment on a shared machine; the analysis waits.
5. **The guard-cost frame-time A/B** for the part-38 revalidate default — still owed,
   still three runs an arm, medians and pinned-share not means (gotcha 237).
6. `docs/perf-cpu-plan.md`'s CPU/GPU overlap — still the largest performance item.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-38 stands, plus gotchas 295-300.
* **For every field a decoder parses, grep for a READER** (295). The mip fields sat
  parsed and unread for thirty-four parts and presented as "hardware must not use this".
* **A count that saturates is a question about its emitter** (296). 324 of 324 of our
  pixel shaders "contain a discard"; hardware's own microcode says 1 of 208.
* **"Distinct colours" REWARDS ALIASING** (298). Part 39's registered prediction had the
  sign wrong: a fix that removes aliasing scores WORSE on that statistic while being more
  correct. Register predictions against the ORACLE's value (hardware's R4 frames read
  meanLuma 58.6, distinct 127,574), not against "more" or "less".
* **A stats file still being appended to parses cleanly and is not a run** (299). It
  inflated one arm's null 47x and flipped a verdict. Confirm the process has exited.
* **A null belongs to the pair that produced it** (300) — use `frame_arm_spread.py`.
* A matched-frame outdoor picture A/B is still unsatisfiable (gotcha 254); era medians
  with the within-arm spread as the floor are the instrument.

## Gates, on this binary (part-39 final)

* `--smoke` OK after every change (run, green).
* `CZ_VK_VALIDATION=1` on the outdoor route after the mip change: **no new messages** —
  the only ones are the pre-existing `topology-08773` point-size trio.
* Re-run in part 39 and green: **both PM4 capture oracles** (24,527,474 packet lengths
  agreeing, 0 disagreeing; every indirect buffer's address and size matching),
  **`shader_dim_census.py`** (319 modules 2D / 97 cube, 0 disagreements) and
  **`find_unlowered_switches.py`** (0 defects, 2 benign thunks).
* **OWED and NOT re-run in part 39**: the A5 kernel-call gate, `truncated=0` and the
  capture-E identity correlation. None of part 39's changes touch the CP, the kernel or
  the shader cache, but re-run them before any claim rests on them.
