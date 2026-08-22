# Part 63 kickoff — RT stage 1: the geometry census

> **THIS IS THE LIVE HAND-OFF**, superseding `part62-kickoff.md`. Parts 61-62
> (2026-08-21, one long day) shipped the FOV slider end to end through three
> mechanisms — it ended GAME-SIDE (the game renders AND culls at the widened
> fov; in 21:9 the camera is over-widened by k=9W/16H so the game's frustum
> covers the whole view) and the operator's verdict is **"It works."** Records:
> `phase5-notes.md` §6cs (the composite discovery), §6ct + addendum + closing
> (the culling saga); gotchas 378 (rewritten), 380; the part-62 kickoff carries
> the detail. Close gates on the final binary: `--smoke` green, A5 exit 0
> (4 permutation / 0 real), E gate +0.9599 identity — the standing numbers.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

## 0. Carry-overs from part 62 (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred** ("we'll
  fix that later"): filed in `perf-state-parked.md` with mechanism (first-sight
  upload bursts on camera turns against the ~1.8x culled volume) and four
  candidate directions, cheapest measurement first (profile a turn with
  CZ_VK_PROFILE). If the operator asks for it, THAT is the part's work instead.
* Small open verdicts, none blocking: a gore cut with the slider off zero;
  aiming behaviour under the slider (other camera nodes are addable to the
  substitution trivially — it is per-node); the cutscene 21:9 CROP look (a
  deliberate treatment change from part 62 — cutscenes show slightly less
  top/bottom instead of more sides).
* Suspected input leak at panel open (every open logs one Resolution-row write;
  harmless on the operator's config, would step the resolution on another).

## 1. The work: RT stage 1 — the geometry census (`docs/rt-and-fov-plan.md` §2)

The plan section is the spec; **zero renderer changes** — a census tool and a
doc section that decide whether RT stages 2-4 (shadows, AO, lighting/GI) are
cheap or dear. Nothing in stages 2-4 starts before these numbers exist.

What already exists that stage 1 reuses:

* **The hardest question is already answered** (part 62, §6cs/§6ct for free):
  static world meshes enter the VS in WORLD SPACE — the c0-3 composite is P*V,
  there is no per-draw world matrix in the transform — so their vertex streams
  are BLAS-ready as-is, and the VIEW matrix is extractable per frame from any
  composite (row3 = v2; rows 0/1 = A_eff*v0, B_eff*v1). `SceneXformForm()` in
  vk_renderer.cpp is the recognizer.
* **BLAS identity = the persist-cache identity**: the content guards already
  compute per-frame change stamps per stream; "guard held steady across frames"
  is the rigid-vs-skinned predicate, free. Skinned actors (zombies) are
  excluded from stage 1 by the plan.
* **The fetch machinery** already decodes vertex declarations per draw — the
  position-format census is a walk over existing decode (float3 expected
  dominant; a BLAS builder consumes exactly what the census returns, gotcha 5).
* **The census pattern**: per-DRAW on the raw register window, not per
  memo-miss (part 61's lesson — a copy-site census counts constant CHANGES and
  misses memo-hit draws). CZ_VK_FOV_CENSUS is the worked example.

Deliverables: a census over an outdoor frame (position formats, rigid/skinned
split, stream sizes/counts for TLAS budgeting), a doc section with the numbers,
and the stage-2 go/no-go pricing.

## 2. The camera/fov machinery, for reference (do not re-derive)

* The roaming camera's fov is a BEHAVIOR PARAM node read at ONE getter site
  (`sub_8246BF48`, lr 0x8246E31C); `cpu/camera_fov.cpp` substitutes there:
  authored+N degrees (slider), then over-widened by k in tan space in wide
  mode. **The node is STATE** — the game writes its smoothed fov back through
  it; capture authored at first sight, enforce absolute, never +=.
* The renderer's composite wide patch MULTIPLIES ROW1 by k (narrows vertical);
  the raw-form (UI) patch divides row0 (centering). UCP compensation mirrors
  each form. The renderer-side fov patch is a measurement arm only
  (CZ_VK_FOV / CZ_TEST_FOV_FLIP).
* Arms: CZ_NO_GAME_FOV=1 (substitution off), CZ_VK_WIDE=0 (16:9),
  CZ_FOV_PARAM_TRACE / CZ_FOV_PROP_TRACE (the discovery instruments).
* Verify any projection change with a SAME-RUN flip — a two-run picture pair is
  camera drift wearing a positive result (gotcha 378).

## 3. Standing list

* Performance PARKED (`perf-state-parked.md`) — includes part 62's turn-stutter.
* Decal flicker: waiting on a sighting; F8 burst + CZ_VK_NO_PARALLEL_GUARD=1.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; point-list PointSize VUID class named, cheap,
  unowned.
* Shader cache 449; any run reaching new ground carries CZ_SHADER_DUMP
  (~/DR2CZ-troubleshooting/ucode-dumps, never /tmp).
