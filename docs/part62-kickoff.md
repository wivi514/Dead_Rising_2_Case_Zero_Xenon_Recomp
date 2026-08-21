# Part 62 kickoff — the FOV verdict, then RT stage 1 (the geometry census)

> **THIS IS THE LIVE HAND-OFF**, superseding `part61-kickoff.md`. Part 61 shipped
> the first two stages of `docs/rt-and-fov-plan.md`: the **FIELD OF VIEW slider**
> (sixth panel row, live, verified headlessly on all three legs) and the **RT
> stage-0 capability probe** (the operator's RTX 3070 carries every required
> extension — ray-query hybrid RT is AVAILABLE). The record is
> `phase5-notes.md` §6cr; gotchas 378-379 are the part's two instrument lessons.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

## 0. The operator session this kickoff wants

Launch plainly (`cd runtime/build && ./cz_runtime`). The panel is on the game's
own Options hub; the new row is FIELD OF VIEW (sixth, below FRAME CAP).

1. **Comfort pass** (plan §0's owed verify): play with the slider at a few values
   (+10 and +20 are good probes; LEFT/RIGHT one degree per press, clamped at
   -10/+30). It applies LIVE — no restart. `CZ_VK_FOV=0` pins it off if anything
   looks wrong.
2. **THE ONE TRADE TO JUDGE, stated up front: the HUD scales with the slider.**
   The title draws its HUD/UI through the same (only) scene projection — vfov
   45.00°, one bit-identical matrix game-wide, census-proven — so at +15 the HUD
   sits ~72% of its normal size, drawn toward center; at -10 it grows. This was
   MEASURED, not guessed (card and HUD both land on the predicted ratio to a
   point), and the obvious exemption was refuted: 81% of the projection's
   ztest-off draws are sky/effects/decals, so a depth-state carve-out tears
   effects off the world (gotcha 379). If the HUD scaling is unacceptable, say so
   — the next discriminator candidate is the HUD's own pixel-shader set, a real
   (part-sized) investigation, and the slider is still useful meanwhile at small
   values.
3. **Slice check off zero** (plan §0): with the slider at +15, cut a zombie —
   gore planes are compensated on both axes, so cuts should stay put. A slicing
   defect ONLY with the slider off zero points at that compensation
   (`CZ_VK_FOV=0` is the discriminator).
4. **Cutscenes**: cutscene cameras are recognized perspectives and WILL widen
   with the slider (plan's stated trade). If that looks wrong, note it — gating
   the patch on gameplay needs a cutscene-vs-gameplay discriminator nobody has
   measured yet.

## 1. What part 61 established (do not re-derive)

* **One projection serves the whole game so far**: vfov exactly 45.00°
  (B = -(1+√2)), zn 0.1 / zf 1000, bit-identical across title, menus, outdoor
  crowd. Window base is effectively always 0 (24 moved of 89.9M draws).
  `CZ_VK_FOV_CENSUS=1` re-measures on new ground (cutscenes and aim-cameras have
  NOT been censused — a second distinct projection would show up there first).
* **~2% of draws carry that projection and they ARE the visible scene** — the
  98% are shadow/cube/depth/post passes, correctly untouched (gotcha 378: a
  draw-count share says nothing about picture coverage).
* **The fov patch composes with wide mode** (fov first, wide second — the fov
  ratio preserves recognition) and with the constant memo (per-frame latch; the
  memo never crosses a frame). Null is byte-identical; the verify arm recomputes
  with both patches in order.
* **Evidence**: `~/DR2CZ-troubleshooting/part61-fov/` (indexed).

## 2. Gates at close of part 61

* `--smoke` green after every commit.
* A5 diff (gate run, final binary): **exit 0, 4 permutation windows, 0 real** —
  same shape as parts 59/60.
* E gate (default arm pinned by env: CZ_VK_RES=1280x720 CZ_VK_WIDE=0): logo card
  **+0.9599 identity** vs E2 (standing +0.9597-0.9599), found by scanning the
  dump for the card per part 59's attract-drift trap.
* PM4 oracles and dim census NOT re-run — nothing touched pm4.cpp or the shader
  cache in part 61 (part 59's green stands).

## 3. After the verdicts: RT stage 1 — the geometry census (plan §2)

The plan section is the spec; read it first. Zero renderer changes — a census
tool and a doc section that decide whether RT stages 2-4 are cheap or dear.
What already exists that stage 1 reuses (the section a kickoff owes):

* **BLAS identity = the persist-cache identity** — the content guards already
  compute per-frame change stamps per stream; "guard held steady across frames"
  is the rigid-mesh predicate, free.
* **tools/pose_read.py** knows c0-3 proj / c12-14 view; the world matrix's
  registers are UNMAPPED — part 61 adds one hard fact: the VS window is
  per-draw (VS memo hit 3.2%), so per-draw world state definitely lives in the
  window; bind it by dataflow (diff two frames of one moving prop), not by
  adjacency.
* **The fetch machinery** already decodes vertex declarations per draw — the
  position-format census is a walk over existing decode, not new parsing.
* The part-61 census pattern (per-DRAW on the raw register window, not per
  memo-miss) is the right shape for any stage-1 counting.

## 4. Standing list (unchanged)

* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Performance PARKED (`perf-state-parked.md`; supervised items: A's order gate,
  C's gather design).
* Live-resolution switch parked (apply-at-next-launch is the operator's call).
* The point-list PointSize VUID class (6 per boot) — named, cheap, unowned.
