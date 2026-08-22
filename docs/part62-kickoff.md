# Part 62 kickoff — the composite fix is in; two verdicts wanted, then RT stage 1

> **THIS IS THE LIVE HAND-OFF**, superseding `part61-kickoff.md`. **UPDATED the
> same night it was written**: the operator's first session refuted part 61's
> slider ("field of view only impact the UI... In game it doesn't do anything"),
> and the investigation that followed found and fixed the real mechanism in one
> evening — **the world's draws carry a view-projection COMPOSITE at c0-3, not
> the raw projection; both the fov slider and part 60's wide patch only ever
> matched the raw form (= the UI and frontend)**. Commit 943227d recognizes and
> patches both forms; the fov slider is COMPOSITE-ONLY (world moves, HUD
> pixel-static — the operator's requested scope), and **21:9 gameplay geometry
> is unstretched for the first time** (it had been stretched ~34% since part
> 60; only the frontend was ever truly widened). Records: `phase5-notes.md`
> §6cs (the discovery), §6cr + §6cp carry retractions in place, gotchas 378
> (rewritten), 380. Evidence: `~/DR2CZ-troubleshooting/part62-fov-composite/`.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

## 0. The operator session this kickoff wants — TWO verdicts

Launch plainly (`cd runtime/build && ./cz_runtime`). FIELD OF VIEW is the sixth
panel row (Options hub), LIVE, one degree per press.

1. **The fov slider, now GAME-SIDE (§6ct, same night)**: the operator's culling
   report ("everything on the side gets culled") retired the renderer-side
   mechanism — the slider now feeds the game's own camera, which renders AND
   CULLS at the widened fov, live, with the HUD and cutscenes untouched by
   construction. Verified headless: fov=10 -> scene at exactly 53.00°, UI at
   45.00°, draws +11%. In the world, slide to +10/+20 — wider view, no side
   pop-in, HUD still. `CZ_NO_GAME_FOV=1` is the control arm.
2. **Wide-mode gameplay just changed appearance**: the world at 3440x1440 is now
   a true wider view instead of a ~34% horizontal stretch. This is a fix of a
   two-part-old defect, but it LOOKS different from what the operator has been
   playing — the verdict on the new look is owed. (If anything seems wrong
   outdoors at 21:9, `CZ_VK_WIDE=0` for a 16:9 control; the frontend is
   unchanged either way.)
3. **Slice check with the slider off zero** (unchanged ask): gore cuts should
   stay put.
4. **Cutscenes**: with the game-side mechanism cutscene cameras keep their
   authored fov (separate camera nodes — the old renderer-patch trade is
   RETIRED). A cutscene look is still worth one glance.
5. **Aiming**: if the aim camera's fov looks unadjusted or snaps while the
   slider is off zero, say so — other camera nodes can be added to the
   substitution trivially (it is per-node by construction).

## 1. What parts 61-62 established (do not re-derive)

* **Two transform forms, cleanly split**: the raw 16:9 projection (ONE
  bit-identical matrix game-wide — vfov 45.00°, zn 0.1/zf 1000, ~2% of draws) is
  the UI/frontend; the world rides P*V composites (gameplay camera vfov 41.64°,
  per-camera zoom on top, 95% depth-writing draws). `SceneXformForm()` in
  vk_renderer.cpp is the recognizer; its four conditions are measured, not
  assumed (miss-dump, §6cs). Shadow orthos, skinning affines and cube-face
  cameras fail it by construction — also measured.
* **The fov slider patches composites only** (default); wide patches both forms
  (the raw-form UI centering in wide is a wanted part-60 feature). UCP
  compensation mirrors each patch's scope.
* **Verification method for any projection change: the SAME-RUN flip**
  (`CZ_TEST_FOV_FLIP=N`). A two-run picture pair is camera drift wearing a
  positive result (gotcha 378 rewritten) — part 61 shipped a false scene claim
  on exactly that.
* **The depth-state UI discriminator stays refuted** (gotcha 379); the FORM is
  the discriminator.
* Instruments: `CZ_VK_FOV_CENSUS` (per-form classification), `CZ_VK_FOV_MISS=N`
  (the discovery instrument), per-form patch counters.

## 2. Gates

* `--smoke` green after every commit (943227d included).
* Part 61's close ran A5 (exit 0, 4 permutation / 0 real) and E gate (+0.9599
  identity) on the pre-composite binary; 943227d touches only the draw path.
  **The pair is owed a re-run at this part's close** (standing rule). PM4
  oracles / dim census remain untouched.

## 3. After the verdicts: RT stage 1 — the geometry census (plan §2)

The plan section is the spec. **§6cs already answered its hardest question for
free**: static world meshes enter the VS in WORLD SPACE (the c0-3 composite is
P*V — there is no per-draw world matrix in the transform), so their vertex
streams are BLAS-ready as-is, and the VIEW matrix is extractable per frame from
any composite (row3 = v2; rows 0/1 = A_eff*v0, B_eff*v1). What stage 1 still
owes: the rigid-vs-skinned split via the content guards' change stamps, and the
position-format census over the fetch machinery. The part-61 census pattern
(per-DRAW on the raw register window) is the right counting shape.

## 4. Standing list (unchanged)

* Suspected input leak at panel open: every open immediately logs one
  Resolution-row write (harmless at the operator's config — last list entry,
  clamped — but on another config it would silently step the resolution).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Performance PARKED (`perf-state-parked.md`).
* Live-resolution switch parked; point-list PointSize VUID class named, cheap,
  unowned.
