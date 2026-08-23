# Part 68 kickoff — RT shadows: the occluders are PLACED. One session is owed, and it is scripted.

> **THIS IS THE LIVE HAND-OFF**, superseding `part67-kickoff.md`. Part 67 ran the
> offline census part 66's hand-off demanded, and the census answered neither of
> its two questions as they were phrased — it answered a better one.
>
> The record is `phase5-notes.md` **§6cy**; the backlog entry is `open-items.md`
> 0v; the arm is in `instruments.md`; the lessons are gotchas **398-400**.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. THE ANSWER, so it is not re-derived

**No filter was eating the buildings. Every one of them was collected, and every
one of them was at the world origin.**

The position streams this title draws are **object-space**. `rtshadow::Collect`
admits a draw when `SceneXformForm(c0..c3) == 2`, and §6cs read that as "so the
stream is world-space". It is not: c0..c3 is the CAMERA's view-projection, and it
is the same matrix whether the shader feeds it a world position or an object
position it transformed one line earlier — which is exactly what these shaders do:

```
    r4.xyz = iPosition0.xyz;  r4.w = 1.0;
    r1.x = dot(vc(8),  r4);      <-- the object->world 4x3, row-major
    r1.y = dot(vc(9),  r4);
    r1.z = dot(vc(10), r4);
    oPos.x = dot(vc(0), r1);     <-- the camera composite Collect gates on
```

Measured against hardware's own frames — the twenty `.xtr` world traces, 46,820
structurally-accepted draws:

| | untransformed | placed |
|---|---|---|
| boxes intersecting the frustum the draw was issued into | **11.69%** | **98.64%** |
| VERTICES inside the clip volume (per shader) | **0.0%** | 61-98% |
| draws carrying a non-identity world translation | — | **100.0%** |

Every part-66 measurement was an honest reading of a pile: primary rays hit 85%
of the screen, the world checker was perspective-correct, and the hemisphere read
97.3% open because from a receiver out in the town a heap at the origin subtends
almost nothing. And **`tlasInst=216..722` was the DISTINCT MESH count** — the
placements had collapsed into the mesh identity.

## 1. STEP 1 — THE OPERATOR SESSION. It is written and it gates itself

```
tools/part67_placement_session.sh          # five arms, ~30 s each, outdoors
START=3 tools/part67_placement_session.sh  # resume at arm N
```

Four of the five need no eye: each prints a histogram of our own factor image and
the script reads them all back at the end (gotcha 397). **The arms come in PAIRS
and the pair is the result** — arms 2 and 4 are the same binary with
`CZ_VK_RT_OBJ_XFORM=0`, i.e. the part-66 renderer.

| arm | asks | PASS |
|---|---|---|
| 1 `place` | hemisphere occlusion (mode 20), placement ON | mean clearly below part 66's **0.987** |
| 2 `pile` | the same, placement OFF | reproduces **0.987 / 97.3% open** |
| 3 `real` | the shipped path, placement ON | clearly more than part 66's **0.9% shadowed** |
| 4 `realpile` | the shipped path, placement OFF | back to **0.9%** |
| 5 `look` | RT HIGH (tier 3), no debug | **the operator's eye**: are the shadows under the things that cast them, and do they stay put? |

**Read the `world box` line first, in every arm.** The script prints it. Still
Creek runs roughly x[-940 390] z[-720 370]; a box a couple of units across means
the placement is not engaged and nothing measured under it means anything.

**If arm 1 moves and arm 2 ALSO moves, this hand-off is wrong** — something other
than the placement changed and §6cy should be retracted in place.

**A5 is owed as arm 0.** Part 67 changed no kernel path, but the rule is the rule.

## 2. What to expect to be wrong next, in order

Named now so the session is read against a list rather than a hope:

1. **The sun direction becomes load-bearing again.** Part 66 exonerated it with
   mode 20 — but mode 20 was measured against the pile, so "no direction is
   occluded" said nothing about the sun. §6cw already records that in gameplay
   the naive capture reads **(-0.010, 1.000, 0.020) at a 3587-unit volume** (a
   top-down MAP render, not a sun) and that the binding is now by DATAFLOW
   through the atlas the census's shaders actually fetch. **Check
   `[rtb] ... sun=` in every arm's log before blaming the geometry.**
2. **Ray length and bias.** Both were exonerated at 0.9% against a pile, which is
   no test at all. Default TMax is the cascade's own volume; `CZ_VK_RT_RAY_LEN`
   and `CZ_VK_RT_FACTOR_BIAS` are the arms.
3. **Self-shadowing.** With the world placed, a receiver's own mesh is now in the
   structure at the receiver's own position. The origin bias exists for this and
   has never been tested against real geometry.
4. **The PALETTE approximation, which is the one place part 67 knowingly guesses.**
   Nine of the bank's shaders build the world matrix by blending `vc(base + a0)`
   entries with three PER-VERTEX weights, and the runtime uses entry 0 with unit
   weight. That is right for the static world — composing the second stage takes
   `vs_b677dc3457f5b41a` (2,658 of the gas-station frame's 4,512 accepted draws)
   to 99.5% of vertices on screen — but it is exactly what a SKINNED actor drawn
   through the same shader would fail, landing the whole mesh at its root bone.
   Part 63's "skinned exclusion is structural" rested on the c0-3 affine form,
   which we now know does not apply here, so the exclusion may be weaker than
   recorded. **`palette=` in the collector census is the population to watch**,
   and a wrong-looking shadow under a zombie is the symptom. It is measurable
   offline: extend `rt_tlas_census.py` to compare entry 0 against the true blend
   per vertex.
5. **The alpha and skinned populations.** `alpha` is 6,842 draws of 46,820 in the
   census and stays raster-only by a stated trade; a pixel covered by a skinned
   actor takes the factor of the opaque surface behind it. Both are MED/HIGH
   features and both are now worth pricing, because for the first time the
   opaque half is expected to work.
6. **Cost.** Instances go from ~500 to ~4,500 a frame and that is ~4,500 hash
   inserts on the PUMP thread, which part 55 measured as the thread that matters.
   Only paid with RT armed. `CZ_VK_PROFILE` and a soak are the way to price it —
   do not quote a frame time from an arm carrying `CZ_VK_FRAME_STATS`.

## 3. Do not re-buy any of these

Killed by measurement in parts 66-67:

* the pass's texture bindings, and the "Z prepass" the trigger rested on (this
  title has none — 20 traces, `tools/rt_depth_order_census.py`);
* ladder modes 12 and 13 (`g_colour` was never bound);
* the 427 px vertical misalignment (real, found by the operator, fixed);
* the sun's SIGN (`CZ_VK_RT_SUN_FLIP=1` gives a uniform blanket);
* the injection into the 126 shaders (poison reads 100.0% shadowed);
* the screen-space alignment on BOTH axes (modes 14 and 19, a clean transpose);
* route (a) entirely (§6cv §7j);
* **and "a filter is eating the buildings"** — the census says the buckets are
  ordinary and the population is complete (§6cy §2).

## 4. The instruments part 67 leaves behind

* **`tools/rt_tlas_census.py`** — re-runs `Collect`'s whole filter chain against
  hardware's frames, reading the real vertex bytes out of the `.xtr` traces, and
  prints the placement verdict. It re-runs the subject rather than summarising it,
  which is what made the instance-count coincidence visible (gotcha 400).
* **`tools/rt_world_xform_census.py`** + `config/rt_world_xform.json` — the
  per-shader object->world constant rows, read out of the microcode by the same
  XenosRecomp the cache is built with. **It is a GATE**: exit 1 when the cache
  holds a vertex shader the table misses. Re-run it after ANY shader-cache change.
* **`CZ_VK_RT_OBJ_XFORM=0`** — the part-66 renderer, same binary.
* The collector census now prints `placed=/declined:/world box`.
* Everything part 66 left: `CZ_VK_RT_FACTOR_READBACK` (start here, always), the
  20-mode ladder, `tools/rt_depth_order_census.py`.

## 5. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**
  (`perf-state-parked.md`, part 62).
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict (arm 5 can carry it).
* Suspected input leak at panel open; decal flicker (needs a sighting);
  doubled-slab watch (00q); the point-list PointSize VUID class.
* **PERFORMANCE IS PARKED** — `docs/perf-state-parked.md` is the one document
  that resumes it.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 6. Gates at part 67's close

RT is OFF by default and nothing outside the RT collector and the TLAS build
changed.

* `--smoke` OK.
* `shader_dim_census.py` clean on all sixteen caches; the play cache's NAME diff
  against stock is empty; the six RT-relevant caches are all 449.
* `rt_world_xform_census.py`: **104 of 104** cache vertex shaders covered, and it
  exits 1 on a planted gap.
* `rt_tlas_census.py` runs over all twenty traces and prints its own verdict.
* **A5 is owed** and is arm 0 of the session.
