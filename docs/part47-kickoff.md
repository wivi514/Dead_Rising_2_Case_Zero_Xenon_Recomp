# Part 47 kickoff — finish the tree fix (the mechanism is proven, the renderer change is named), then whatever part 46's performance A/B left open

Written at the end of part 46 (2026-08-15). **This is the LIVE hand-off**,
superseding `part46-kickoff.md`. Read `docs/phase5-notes.md` §6ca **and its
addendum** before anything else; open item 0t carries the same material in
backlog form.

## What part 46 settled (do not re-derive)

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

## The plan

1. **BUILD THE FIX. It is a renderer change; the shader half is already done
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
2. **Then take it to the operator's own trees.** The menu tree is
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
3. **PERFORMANCE — READ `docs/perf-plan-part47.md`. It is the live plan and it is
   written against the operator's OWN profiled frame**, which part 46's last
   session finally captured: 61.7 ms at 7,231 draws, textures 26.5 ms, PM4 walk
   14.2 ms, record 10.9 ms, GPU 34% utilised and `submit.gpu` 0.0 — a pure CPU
   problem. **Start with item 1.1's upper bound, which is one run**:
   `CZ_VK_NO_TEX_REVALIDATE=1`. The texture revalidation guard read 366 GB over
   their session (92.9 MB/frame) to catch 986 real changes. If that arm does not
   move the frame by ~10 ms the plan's top item is wrong and the ranking should
   be rebuilt before any code is written.
   The three suspects below are exonerated but only on the HEADLESS route
   (§6cb + addendum), which understates the operator's draw path by ~2x.
   Part 45's liveness fix: six alternated 600 s runs, three an arm, one variable
   — **every draw bin inside its own noise floor**, and the two bins off the
   pacing floor disagree in sign. Part 41's per-fetch samplers and part 44/45's
   mip chain/tail: one run each on the `textures` phase share — **44.6 / 46.0 /
   44.2 against a baseline of 45.4**, all unmoved, every arm shown to have
   engaged by the counter the others carry.
   **Do not run a fourth arm here.** Four unmoved results in a row are a fact
   about the framing: this headless route does not reproduce what the operator
   reports. What is needed is a run on THEIR configuration — windowed, their
   route, `CZ_VK_PROFILE` + `CZ_VK_FRAME_STATS` — and, if you can get it, the
   same on a binary from before part 41, which is the only way to test "the last
   few days" as a whole rather than one change at a time.
   **What the profiler did say, and it is worth someone's time regardless:**
   `textures` is **43-45% of the draw path**, ~29-30% of the frame, on every arm.
   It is structural rather than recent. Whether that is a real cost worth
   attacking or just a share that was always this high is open — §6cb's
   comparison against part 20's 13.6% is explicitly the weak half of that
   argument (different route era, binary and session; gotcha 13).
4. **The UI text layer (open items 00c/00k) is FIXED and operator-confirmed** —
   "Ui stay good the whole time", then "Hud stay good and all" on the cheaper
   variant, against a control arm (`CZ_VK_NO_DYNAMIC_GUARD=1`) that broke.
   Exactness is now EARNED per stream rather than bought by size: a stream the
   store catches changing is hashed exactly, and one the sampled guard is proved
   able to see is demoted back. `CZ_VK_GUARD_BUDGET` is the default;
   `CZ_VK_NO_GUARD_BUDGET=1` is the control. §6cc addendum.
   **What is still owed**: it costs 30.5 MB/frame on their session and their own
   A/B priced the earlier version at +22.7% in the 4500-6000 draw bin. It is
   item 1.1's sibling and the same budgeting idea applies — fold it into the
   performance plan rather than treating it as done.

## Standing state

* Caches: `assets/shader_spv` (435, default, unchanged this part),
  `assets/shader_spv_pre45` (the part-45 control arm). Part 46 built three more
  under `/tmp` — rebuild them, they are on a tmpfs:
  `CZ_DXC_DEFINES="-D XE_ALPHA_TO_MASK=1"` for the arm, no defines for the null.
* Artifacts: `~/DR2CZ-troubleshooting/part46/` — `menu_a` (default), `menu_id`
  (draw-ID map + census), `menu_null`, `menu_a2m` (gated off), `menu_a2m_any`
  (the arm), `menu_nankill`, `tree_a`/`tree_id` (the gameplay spawn), and
  `perf/` (the A/B's stats and logs).
* Tools added: `tools/part46_perf_ab.sh` (the alternating A/B driver),
  `tools/part46_perf_read.py` (medians + pinned share + within-arm noise floor).
* XenosRecomp carries two new local changes, both no-ops without the define:
  `XeAlphaTestThreshold()` in `shader_common.h` and the emitter calling it.
  Record them in `docs/xenonrecomp-upstream-bugs.md` if they are kept.
