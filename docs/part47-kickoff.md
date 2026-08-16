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
   fix should carry — but **58 of the 78 leaf draws in the operator's frame are
   `cc=AA000007`, alpha test OFF and opaque, and hardware draws those opaquely
   too**, so they are trunk/branch geometry and will not change. Check that
   before reading a partial improvement as a partial fix.
3. **Performance.** See the part-46 record for what its A/B measured; the two
   suspects it could not separate with a preserved control arm are part 41's
   per-fetch samplers and part 44/45's mip uploads, neither of which has a
   whole-cache arm the way part 45's liveness fix has `assets/shader_spv_pre45`.
   If the A/B came back inside the noise floor, the next move is NOT another
   frame-time run: it is `CZ_VK_PROFILE` phase splits on the same route, which
   attribute time rather than measuring the pacing floor.
4. **The UI text layer (open item 00k) is still owed a FIX**, unchanged from
   part 46's kickoff: the mechanism is confirmed (the cross-frame stream store's
   guard, item 00c above the 16 KB bound), and "always exact" is not it —
   63.76 MB/frame against 9.28. Raise the bound, or make exactness a property of
   the stream KIND. The cheap headless check is the EMPTY card after pressing
   START at the title screen.

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
