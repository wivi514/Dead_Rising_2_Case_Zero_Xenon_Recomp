# Part 64 kickoff — RT stage 2: ray-traced shadows, LOW tier first

> **THIS IS THE LIVE HAND-OFF**, superseding `part63-kickoff.md`. Part 63
> (2026-08-21 night) shipped RT stage 1 — the geometry census — as
> `CZ_VK_RT_CENSUS=1` (three commits) plus the record `phase5-notes.md` §6cu
> (+addendum) and the DONE banner on `rt-and-fov-plan.md` §2. **The verdict is
> GO for stage 2, cheaper than the plan budgeted.** Zero renderer changes; the
> instrument is inert to one static-bool test per draw when off.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing
> instruction), and the Fable 2 port is NOT a renderer reference (operator
> instruction, part 59). Stage 1's census runs were headless measurement on the
> DebugJump/EXPLORER routes (the part-61/62 practice for instruments); every
> LOOK verdict from stage 2 on is the operator's.

## 0. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed
  in `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the
  part's work instead.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off
  zero; aiming behaviour under the slider; the cutscene 21:9 CROP look.
* Suspected input leak at panel open (one Resolution-row write per open;
  harmless on the operator's config).
* Owed from part 60: the shadow Low-vs-High LOOK verdict — **fold it into stage
  2's operator session**, since the same session will judge RT vs OG shadows.

## 1. What stage 1 established (build against these, do not re-measure)

`phase5-notes.md` §6cu is the full record. The numbers stage 2's design leans on:

* **Position data: 100% float3, stride 28-32 B**, in the SAME persist-cache
  buffers the raster path uploads (dword-swapped, GPU-resident,
  VERTEX|INDEX|STORAGE usage + optional device address at ~vk_renderer.cpp:3839).
  BLAS input = one more usage bit
  (`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`) + device addresses.
  **No geometry copies.**
* **Topology 96.8% tristrip, indices 100% u16** (2.7-5.9 MB cumulative).
  Strip→list index expansion is the ONLY data transformation anywhere.
* **BLAS identity = persist-cache identity + content stamp.** 98.1% of world
  bytes are build-once-stable; the rewritten class is 134 map-wide smallware
  streams (1.4 MB) — exclude them. Stream-address REUSE is real (part 39's
  lesson): the stamp is part of the identity, never the key alone.
* **TLAS: identity transforms** (world-space streams, §6cs), ~2,100-2,600
  instances typical, worst observed frame 7,034 world draws (÷2 for the
  640-wide tiles) / 3.46M drawn tris. Skinned actors are 3.0% of scene draws
  and self-select out by their AFFINE c0-3 form — the exclusion is structural.
* **Churn while roaming: ~13 new stable streams (~150 KB)/s** — async BLAS
  builds inside a per-frame budget, eviction through the part-60
  deferred-retire path (do not re-buy the tier-freeze, gotcha 376).
* The cascade pass is named by EDRAM pitch 1040 (part 60's predicate — the
  census reused it to split shadow from skinned; stage 2 reuses it again for
  the injection).

## 2. The work: stage 2 LOW, alone first (`rt-and-fov-plan.md` §3)

The plan section is the spec. The order of decisions:

1. **The injection experiment first** (the plan's (a)-vs-(b) fork): route (a) —
   synthesize the 4096x1024 cascade atlas's DEPTHS so the title's own
   comparison yields our answer — is an afternoon and touches no shader.
   Measure it in stills before building (b) (patching the ~dozen
   shadow-sampling PS to read a factor texture). The atlas is DEPTH the shader
   compares, not a factor — re-read §3 before writing anything.
2. **BLAS/TLAS plumbing** behind `CZ_VK_RT=1` + `CZ_VK_RT_SHADOWS=1`: null
   (RT off) must be byte-identical/instruction-path identical; the poison
   positive control (all-black shadow factor must darken the frame — the
   CZ_VK_CUBE_POISON pattern) lands WITH the feature, not after.
3. **LOW tier only**: half-res, 1 ray toward the sun (the cascade pass's view
   matrix IS the sun — vc12-14 of the shadow pass, pose_read.py), temporal
   accumulation only. Tiers MED/HIGH wait for LOW's operator verdict and a
   priced ladder (CZ_VK_PROFILE `rt` phase from day one).
4. Known trades stated in advance: zombies cast no RT shadows in stage 2 (OG
   cascade remains the fallback); A2M foliage traced opaque will over-shadow
   (ship opaque-only and say so); DEP-position quads and points are not in the
   TLAS.

**Gates**: validation with RT extensions on (CZ_VK_VALIDATION), the PM4 oracles
untouched, `--smoke`, A5 exit 0, E identity, and a same-run flip arm for any
live tier row (the part-61 lesson: live settings need live verification).

## 3. The census instrument, for reuse

`CZ_VK_RT_CENSUS=1` (instruments.md) — rerun it whenever a stage-2 assumption
needs a number from a new place; it is a diagnostic arm (never quote frame
times from a census run). The form-0 subclassification (shadow/skinned/cube)
and extent×depVS split live in its dump. Its three part-63 logs were on /tmp
(tmpfs) — the numbers are transcribed in §6cu; re-run rather than hunt for the
files.

## 4. Standing list

* Performance PARKED (`perf-state-parked.md`) — includes part 62's turn-stutter.
* Decal flicker: waiting on a sighting; F8 burst + CZ_VK_NO_PARALLEL_GUARD=1.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; point-list PointSize VUID class named, cheap,
  unowned.
* Shader cache 449; any run reaching new ground carries CZ_SHADER_DUMP
  (~/DR2CZ-troubleshooting/ucode-dumps, never /tmp).
