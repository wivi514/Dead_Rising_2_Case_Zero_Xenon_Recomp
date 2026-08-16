# Part 47 kickoff — PERFORMANCE FIRST, to the operator's own order; the picture items are diagnosed and parked behind it

Written at the close of part 46 (2026-08-16). **This is the LIVE hand-off**,
superseding `part46-kickoff.md`.

## START HERE

**The operator set part 47's subject before going to sleep: performance, "as
close as you could run it on an Xbox 360".** The plan for it is
**`docs/perf-plan-part47.md`** and it is written against their OWN profiled
frame, not a headless route. Read it before this document's picture items.

**The very first action, and it is one 600 s run**: `CZ_VK_NO_TEX_REVALIDATE=1`
on the outdoor route with `CZ_VK_PROFILE`. That arm already exists and it puts an
upper bound on the plan's largest item — the texture revalidation guard, which
read **366 GB over one operator session (92.9 MB/frame) to catch 986 real changes
out of 26.8M checks, 0.0037%**. If it does not move the frame by ~10 ms the
plan's top item is wrong and the ranking should be rebuilt before any code is
written.

The budget it is working against, from
`~/DR2CZ-troubleshooting/part46-operator4/budget.log` at 7,231 draws:

| phase | ms of 61.7 |
|---|---|
| textures | 26.5 |
| outside (PM4 walk 14.2) | 17.6 |
| record | 10.9 |
| other / constants / readback | 6.6 |

GPU 34% utilised, `submit.gpu` 0.0 — **a pure CPU problem**. Target 33 ms.

**Two standing rules that part 46 paid for:**
* **Wire `CZ_VK_PROFILE` and `CZ_VK_FRAME_STATS` into every operator launch.**
  Part 46's first session shipped without them and their "around 20 fps" had no
  measurement behind it at all.
* **The headless route understates the operator's draw path by ~2x** (28.7 ms at
  5,241 draws vs their 53.9 at 5,080). A headless win is not conservative here;
  confirm it on their configuration.

## What part 46 settled (do not re-derive)

* **THE UI TEXT / HUD DEFECT IS FIXED AND OPERATOR-CONFIRMED** (open items
  00c/00k). Exactness is EARNED per stream — a stream the cross-frame store
  catches CHANGING is hashed exactly, one the cheap sampled guard is proved able
  to see is demoted back. `CZ_VK_GUARD_BUDGET` is the default,
  `CZ_VK_NO_GUARD_BUDGET=1` the control arm, and the operator ran both: fix good
  throughout, control broke. **"Raise the bound" is REFUTED** — 256 KB still
  dropped the HUD, so the buffer is above 256 KB where exactness costs 121+
  MB/frame. It still costs ~30 MB/frame and that is folded into the performance
  plan, not treated as done. §6cc addendum.
* **THE TREE SETTING TO SHIP IS `CZ_VK_A2M_MODE=1`** (with
  `CZ_VK_A2M_ANY_SURFACE=1` and the `assets/shader_spv_a2m` cache). The
  operator's near-matched A/B: isolated-pixel share **0.71% (mode 1) vs 4.17%
  (mode 2, the faithful dither)**, hardware 0.00%. Mode 2 screen-doors and mode 1
  does not; neither is hardware-exact, because that needs the renderer change in
  item 1 below.

* **THE TREE SHARDS ARE MISSING ALPHA-TO-MASK COVERAGE. Demonstrated, not
  argued.** The canopy draws are alpha test GREATER at `RB_ALPHA_REF = 0.0`
  plus ALPHA_TO_MASK over a **DXT4/5** albedo — fractional alpha. At ref 0 the
  alpha test keeps everything, so A2M is doing the whole cutout alone, and we
  declined to emulate it on an excuse that is false exactly at ref 0
  (gotcha 317). An arm that supplies the coverage removes the hard plates and
  moves the named property onto the oracle: canopy p05/p95 **0.291 → 0.324**
  against hardware's **0.326**, with **three byte-identical null controls**.
* **The repro is the TITLE SCREEN, and its oracle is already in the repo.**
  `CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,F9,NONE`, 120 s, no input, near-static
  camera; hardware's picture of that exact screen is
  `Xenia logs/E_screenshots/E3_title_background_stillcreek.png` (content box
  x 0..1399, y 96..880 → resize to 1280x720 and it registers). Gotcha 319.
* **The surface is 2x MSAA, on BOTH machines.** Our counter says msaa=1 on
  69,390 A2M draws (msaa=0 on 518, 4x on none); `tools/xtr_draw_bindings.py`
  now carries `RB_SURFACE_INFO` and hardware's `w1_spawn` says msaa=1 on every
  leaf draw and every one of its 240 A2M draws. This is why the exact
  per-sample dither §6ca first proposed does not apply as the renderer stands.
* **Refuted against hardware, do not re-buy**: the render-state read (colour
  control, alpha ref, blend — byte-equal across all 19 round-2/3/4 traces) and
  the `k_10_11_11` normal decode (hardware's own streams decode to unit length
  on 512 of 512 sampled vertices). **Demoted**: the denormal/NaN packed-normal
  suspect — its arm fires (5.41% of canopy pixels) so the bits do reach the
  shader, and A2M explains the symptom without it.
* **Method, worth keeping**: selecting the DEFECTIVE and the CORRECT pixels
  separately and reading both through the draw-ID map showed they were the SAME
  DRAW, which retired every per-draw input in one measurement (gotcha 318).
  Do that before comparing inputs, not after.

## The plan — performance first, then the picture items

0. **PERFORMANCE: `docs/perf-plan-part47.md`, tiers 1 and 2 first.** They are
   ~21 ms with no architectural change and each item is individually verifiable:
   the texture revalidation budget, the `Count`→`COUNT` conversion on the texture
   path (~9,300 slow `std::map<std::string>` calls a frame), the linear
   `std::find` scans per fetch, and bulk register writes in the PM4 walk (797,624
   dwords a frame at 17.8 ns each where the store is one cycle). Tier 3
   (multithreaded recording) is last on purpose. **Run the PM4 oracles and the
   picture gates after every one of these** — several touch code the picture
   depends on.

1. **THE TREE FIX PROPER, when performance is done.** Mode 1 ships today and is
   good enough that this is no longer urgent. **BUILD THE FIX. It is a renderer change; the shader half is already done
   and controlled.** Give the coverage somewhere to be resolved, one of:
   * **(recommended) sample-expand 2x surfaces the way `msaa == 2` ones already
     are.** Xenos 2x is a vertical sample pair, so a Y-expanded image plus a
     1x2 dither is exact, and the existing resolve path averages it. The 4x
     path in `vk_renderer.cpp` (`msaa == 2`, window scale in X and Y) is the
     worked example to follow.
   * or rasterise A2M draws with real Vulkan MSAA + `alphaToCoverage`.
   Then flip `XeAlphaTestThreshold`'s dither to the 1x2 pattern and re-read the
   table in §6ca's addendum. **The property is already named**: canopy p05/p95
   against E3's 0.326, and the hard-edge share, which must come DOWN toward
   hardware's 0.21% — the pixel-granularity arm pushed it UP to 4.92%, and that
   is the number that says whether the expansion worked.
   `CZ_VK_A2M_ANY_SURFACE=1` stays a diagnostic; it is not the fix.
2. **Then take the tree fix to the operator's own trees.** The menu tree is
   `ps_03533a74cbd5228c`; the gameplay canopies the operator photographed are
   `ps_69a5c3be9359b87c` / `ps_8602b5fd69289893`. All are A2M at ref 0, so the
   fix should carry — but **only 26 of the 78 leaf draws in the operator's frame
   will change, and the texture format says which**. In frame 6615 the split is
   exact: all 58 `cc=AA000007` draws (alpha test OFF, opaque) bind a **DXT1** at
   slot 0, and all 26 draws that enable the test (10 `AA00000C` alpha-blended,
   16 `AA00001C` A2M) bind a **DXT4/5**. Cutout foliage is fractional-alpha DXT5
   with the test on; the solid trunk/branch/LOD geometry is DXT1 with it off, on
   hardware too. Expect about a third of the leaf draws to move, and do not read
   the other two thirds staying put as a partial fix.
3. **Parked, unchanged from part 46's kickoff**: the mip overshoot (re-run
   `CZ_VK_NO_MIPS=1` on the FIXED cache before quoting part 44's result again);
   the 0u residues; part 41's clamp modes / cyan fringes; the AO-only-up-close
   observation; and the part-43 sledgehammer FREEZE, which has not recurred in
   any of part 46's five operator sessions with `CZ_WAIT_TRACE=1` armed.

## Standing state

* **Runtime defaults changed in part 46**: `CZ_VK_GUARD_BUDGET` on (the UI fix;
  `CZ_VK_NO_GUARD_BUDGET=1` is the control arm). The A2M work is NOT a default —
  it needs `CZ_SHADER_SPV=assets/shader_spv_a2m CZ_VK_A2M_ANY_SURFACE=1
  CZ_VK_A2M_MODE=1`, which is what the operator sessions ran.
* **Caches**: `assets/shader_spv` (435, the default bank),
  `assets/shader_spv_pre45` (the part-45 control arm), `assets/shader_spv_a2m`
  (built with `CZ_DXC_DEFINES="-D XE_ALPHA_TO_MASK=1"` — rebuild it after any
  XenosRecomp change).
* **XenosRecomp carries a local change**, committed there as `e0c086f`:
  `XeAlphaTestThreshold()` in `shader_common.h` and the emitter calling it. It is
  a proven no-op without the define — the whole 434-shader bank was rebuilt from
  the new emitter and the canopy crop came out byte-identical. Recorded in
  `docs/xenonrecomp-upstream-bugs.md`.
* **Session drivers** (chained arms, quit one to start the next):
  `tools/part46_operator_session{,2,3,4}.sh`. Copy the newest for part 47 and
  keep `CZ_VK_PROFILE` + `CZ_VK_FRAME_STATS` in it.
* **A/B tooling**: `tools/part46_perf_ab.sh` (three runs an arm, alternated) and
  `tools/part46_perf_read.py` (per-bin medians, pinned share, within-arm noise
  floor; it refuses a verdict below two runs an arm).
* **Artifacts**: `~/DR2CZ-troubleshooting/part46/` (headless menu arms, draw-ID
  map, the perf A/B) and `part46-operator{,2,3,4}/` (four operator sessions —
  `operator4/budget.log` is the profiled frame the performance plan is built on).
* **Gates all clean at close**: `--smoke` 0, `find_unlowered_switches.py` 0
  defects, `shader_dim_census.py` agrees on every shader, working tree committed.
