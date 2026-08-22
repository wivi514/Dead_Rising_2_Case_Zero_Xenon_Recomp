# Part 66 kickoff — RT shadows, ROUTE (B): the operator's verdict

> **THIS IS THE LIVE HAND-OFF**, superseding `part65-kickoff.md`. Part 65
> (2026-08-22) executed that kickoff's whole spec: the census, the shader
> substitution, the factor pass and the tier ladder. **Route (b) is built end to
> end and NOBODY HAS LOOKED AT IT.** Part 66's first action is the operator
> session; everything else in this document is contingent on what it shows.
>
> The record is `phase5-notes.md` **§6cw**; the backlog entry is
> `open-items.md` 0v; the arms are in `instruments.md`; the transferable lessons
> are gotchas **387-390**. Route (a)'s record (§6cv) is history now — read §7j
> only if someone proposes re-opening it.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part
> 59). Part 64's session is why part 65 exists at all: eleven words from the
> operator named a mechanism three headless statistics had missed.

---

## 0. THE FIRST ACTION: `tools/part65_operator_session.sh`

Three chained arms. Launch it, hand it over, read what comes back.

* **arm 1 `poison`** (~1 min) — `CZ_VK_RT_FACTOR_POISON=1`. The factor is forced
  to all-shadow, so stepping the SHADOW row from HIGH to RT LOW must DARKEN the
  world. **This arm GATES the other two**: if the picture does not change, the
  substitution never reached the frame and nothing measured afterwards means
  anything (gotcha 386). The script prints the three engagement lines
  (`RT shadow variant cache`, `[rtb] factor pass ready`, `[rtb] TOTAL`) after
  each arm so a silent failure has somewhere to show.
* **arm 2 `live`** (~5-10 min) — no env arm at all, so the panel's SHADOW row
  drives. HIGH → RT LOW → RT MEDIUM → RT HIGH → HIGH, standing still outdoors.
* **arm 3 `softmax`** (~1 min) — four rays over a 5x sun disc; the penumbra
  should be obviously, excessively soft and stepped in ~5 levels.

**The questions are about SHAPE, not brightness.** Route (a) died because three
statistics all said "14 luma dark" and none of them could say *the white vans
have gone grey*. So: do LIT surfaces stay lit? Do shadows sit UNDER their
casters or offset from them? Is there acne (a stippled/crawling look on flat lit
ground)? Do shadows DETACH from the base of a wall or lamppost? Are RT HIGH's
edges softer than RT MEDIUM's?

### What each answer means, decided in advance

| what the operator reports | what it says | next move |
|---|---|---|
| arm 1 does not darken | the substitution is inert | read `[rtb] TOTAL`'s skip counters — `noScene`/`noLight`/`noTlas` each name a different missing input — and the variant-cache line |
| lit surfaces go grey, as in route (a) | route (b) has the same disease and the diagnosis was wrong | the bias defaults are the first suspect: raise `CZ_VK_RT_FACTOR_BIAS` until acne appears, which brackets the world unit |
| acne / stippling on lit ground | the ray-origin offset is too small | `CZ_VK_RT_FACTOR_BIAS` up; if it needs to be large before the acne clears, the depth reconstruction is the problem, not the offset |
| shadows detached from their casters | the offset is too large | both biases down; the two are separable, so move one at a time |
| shadows in the WRONG PLACE (not just offset) | the sun direction or the scene composite is wrong | `[rtb] passes=` prints the sun vector; the composite has its own distinct-value counter in `[rtb] TOTAL` |
| broadly right, some things missing | expected — skinned actors and alpha-tested foliage are not in the TLAS | that is the MED/HIGH tier work below |
| broadly right and it looks good | ship it | price the tiers in ms on their machine, then close 0v |

**Do not tune more than one knob per arm**, and re-check the engagement line
after any change that combines two others (gotcha 386).

---

## 1. What part 65 built, so it is not rebuilt

* **`tools/shadow_shader_census.py`** — which pixel shaders sample the cascade
  atlas and at which fetch slot, out of the `.xtr` world traces. **126 shaders,
  140 (shader, slot) pairs**, 42,620 draws across twenty traces. It also
  classifies what each use FEEDS: `pcf4` (116 uses, four ±0.5 taps compared
  `> receiverDepth`) and `tap1` (24 uses, one centre tap feeding
  `saturate((receiver − sampled) * k − bias)`). Both monotonic and saturating,
  which is why ONE substitution serves all 140. Output:
  `config/rt_shadow_slots.json`.
* **`tools/patch_rt_shadow_hlsl.py`** + `build_shader_spv.sh`'s `CZ_HLSL_PATCH`
  hook — the substitution, on XenosRecomp's HLSL before DXC. Taps become a
  lookup of our factor at the shader's own `SV_Position`; `getWeights2D` on an
  atlas slot returns 0.5, which makes every 2x2 weight product 0.25 whatever the
  swizzle and turns the title's own filter into a five-level quantiser.
* **`runtime/gpu/rt_factor.hlsl`** + `rt_factor_spv.h` — the factor pass. World
  position from the scene depth and composite, origin pushed off the surface
  along the sun and toward the camera, ray query against route (a)'s TLAS.
* **The renderer wiring** — a variant module per shader in `ShaderMeta`,
  `PipelineKey::passFlags` bit 1, the pass triggered by the title's own first
  atlas-sampling draw and invalidated at every resolve, the scene composite
  captured with a distinct-value check, and `VkRenderer_RtAvailable()` now
  requiring the variant cache so a dead rung says why.

**The caches that exist**, all 449 modules: `shader_spv`, `_a2m`, `_clip`,
`_clip_a2m` (what a play session selects), `_rt`, `_clip_a2m_rt`. The runtime
finds the RT sibling by appending `_rt` to whatever `CZ_SHADER_SPV` names, so
turning RT on does not also change the foliage or the slicing.

## 2. What route (b) can still buy, if the verdict is good — priced, not promised

* **MED/HIGH could add the missing casters.** Nothing outside the TLAS casts an
  RT shadow: skinned actors (zombies, Chuck — structurally 3.0% of scene draws,
  §6cu) and alpha-tested foliage. On route (a) that hole was permanent because
  the raster cascade was unioned underneath; on route (b) the raster shadow is
  REPLACED, so the hole is visible and buying it back is a real tier feature.
  Per-frame BLAS rebuilds for skinned meshes is the price and it has never been
  measured on this title.
* **The five-level ceiling.** The `pcf4` family quantises our factor to five
  levels and the `tap1` family (24 uses) stays binary. Going past that means
  patching the compare itself, which is per-shader work at a population of 126 —
  do not start it without a reason from the picture.
* **Cost.** `CZ_VK_PROFILE`'s `rt` phase carries the factor pass. Nothing has
  been measured on the operator's machine; performance is PARKED
  (`perf-state-parked.md`) and an RT tier is opt-in, so this is a number to
  quote in the panel, not a regression to chase.

## 3. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed
  in `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the
  part's work instead.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict — arm 2 of the
  part-65 session asks for it, so it should come back with everything else.
* Suspected input leak at panel open (one Resolution-row write per open).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; the point-list PointSize VUID class is the only
  validation class the RT work leaves behind.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 4. Practical notes

* **Every variant cache needs the membership gate, not just the stock one**
  (gotcha 390). Part 65 found the operator's play cache ten shaders short since
  2026-08-19 — absent, not stale, so their draws were SKIPPED in every session
  for three parts. All six caches are now 449 and rebuilt.
* `/tmp` is a tmpfs; a 3440x1440 frame dump is ~15 MB. Dump to
  `~/DR2CZ-troubleshooting/`.
* Gates at part 65's close, RT off (the shipped default): `--smoke` OK; **A5 exit
  0** (4 permutation windows, 0 real); `shader_dim_census.py` clean on every
  cache; each RT cache differs from its plain rebuild in exactly 126 modules.
