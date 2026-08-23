# Part 71 kickoff — the sun is closed, and the shadow defect has three suspects left

> **THIS IS THE LIVE HAND-OFF**, superseding `part70-kickoff.md`.
>
> Read, in order: `phase5-notes.md` **§6dc** (part 70), then **§6db** and **§6da**
> (part 69). `docs/part69-night-plan.md` §3 is still the live plan **with its item 1
> struck out** — that was the sun and it is answered. The backlog entry is
> `open-items.md` 0v; the lessons are gotchas **411-413**, and **410's example is
> retracted where it stands**.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction), and the
> Fable 2 port is NOT a renderer reference (operator instruction, part 59).
>
> ---
>
> **BUT READ THIS FIRST: RT SHADOWS ARE PARKED AND THIS IS NOT THE LIVE SUBJECT.** The
> operator closed part 70 with *"We'll stop for now with trying to get ray tracing
> running. Disable that we can select it in game. We'll now switch to fixing performance
> issue."* The settings panel no longer offers the RT rungs and a persisted
> `rt_shadows=N` no longer engages the feature; `CZ_VK_RT_MENU=1` puts the rows back and
> `CZ_VK_RT_SHADOWS=N` engages it directly. **The live plan is
> `docs/perf-plan-part71.md`.** Everything below is the RT hand-off, kept intact and
> accurate so the feature can be resumed from exactly where it stopped rather than
> re-derived.

---

## 0. Where the feature is, in five lines

* **The sun is CLOSED and it was never wrong.** Part 69's "the Z flips between headless
  and windowed" was a confound: zero of the 36 archived RT logs contain a DebugJump
  request or synthetic input, so every one is an operator run from THEIR save while every
  run on the other side spawns at a fixed story point. **DebugJump destination 3 latches
  the operator's own `(-0.347 +0.520 -0.780)` HEADLESSLY**, which settles it — the sun
  depends on where you are in the story, not on whether there is a window.
* **Hardware states the sun twice in the same draw's constant file**, and both agree with
  our own decomposition to **0.00 degrees in twenty of twenty captures**
  (`tools/xtr_sun_oracle.py`). The runtime now reads the title's own constant
  (`CZ_VK_RT_SUN_SRC=cascade` is the control arm) and the two agree to 0.0 degrees over
  33,332 latches with zero rejections. **An architectural simplification, not a picture
  fix — do not report it as one.**
* **Structure, receiver and sun are all now demonstrably correct**, and the straight-line
  shadow signature has survived every one of them.
* **What is left is the RAY (bias, length) and the CONSUMPTION (how the factor image
  reaches the 126 patched shaders).** §1 is a bisection that separates those two in one
  operator session.
* Part 67's exonerations of the bias and the ray length were taken against a pile at the
  world origin and are **still void** (gotcha 172).

## 1. Start here — and RAY LENGTH IS ALREADY REFUTED (part 70 ran it)

> **The hypothesis below was tested and it is DEAD.** `tools/part70_ray_length.sh`, four
> arms: `len=88.2 -> 15.7%` shadowed, `300 -> 14.8%`, `1000 -> 15.1%`, `3000 -> 15.6%`.
> **A 34x change in ray length moves the share by one point, non-monotonically**, and
> since `TMax` only grows a longer ray cannot find FEWER occluders for a fixed camera —
> so the 300 arm's drop is route variance and it bounds the noise at ~1 point. Every arm
> is inside it. Full record in `phase5-notes.md` §6dc §8; the reasoning is left standing
> below because the shape argument is sound and only the premise was wrong.
>
> **What the same dumps did show, and it is the live thread:** the factor's profiles are
> identical in all four arms and neither is flat — the column profile ramps dark on the
> LEFT to bright on the RIGHT, the row profile is fully lit across the top ~30% and
> darker below. A large-scale screen-position gradient is what a "flat slab with a hard
> straight boundary" looks like as a profile, and a 34x ray does not touch it. Go after
> the POPULATION and the PASS, not the ray: §1b.

### The refuted hypothesis, kept for its shape argument

The signature to explain (operator, part 69): *a flat slab with a hard straight boundary
crossing a shipping container, tyres, cars, a chain-link fence and the ground without
bending at any of them, plus a horizontal cut through Chuck's chest.*

`rt_factor.hlsl` sets the shadow ray's `TMax` to `pc.sun.w`, which **defaults to the
cascade's depth extent** — `len=88.5` in the operator's last run, and hardware's own
cascade volumes are 63.4/67.1/89.7. The town is ~1,100 units across and one frame's world
box measured `x[-610 324] z[-681 107]`. A ray that stops at 88 units cannot find an
occluder 200 units away, and a cascade's near-slice depth extent was never a shadow ray's
reach — it is the length scale the frame happened to hand us, which is what the code's own
comment says it is.

**The SHAPE that produces is the observed one.** The set of receiver points whose
fixed-length ray just clears a given occluder is a PLANE in world space, and a world plane
projects to a straight line in screen space that bends at nothing it crosses — because the
cut-off belongs to the RAY, not to the surface receiving it. A constant-height cut through
a standing character is the same statement.

`part69-night-plan.md` §3 ranked this THIRD, reasoning that a short ray would leave
"everything past it lit — the opposite signature". **That geometry is wrong**: a short ray
does not fail far from the CAMERA, it fails far from the OCCLUDER, and it fails along a
plane. Part 67 also exonerated the length — against a pile at the world origin, which is
no test (gotcha 172).

`tools/part70_ray_length.sh` sweeps `CZ_VK_RT_RAY_LEN` over default/300/1000/3000 with a
**pre-registered prediction**: the shadowed share rises monotonically and the straight
edges in the dumped factor move outward or vanish. A flat share across a 34x range kills
the hypothesis. Its result at part 70's close is in `phase5-notes.md` §6dc §8.

### 1b. THE LIVE THREAD — the occluder mode 18 cannot see, then the bisection

**`CZ_VK_FACTOR_DEBUG=18` cannot exonerate the occluder population, and part 69 read it
as if it could.** Mode 18 traces the PRIMARY ray, so it images only what the camera can
see. A large occluder above the scene or behind the camera contributes nothing to that
depth image and still blocks every shadow ray fired at the sun — and the signature that
produces is exactly the reported one, a flat slab whose boundary is the mesh's own edge,
straight in screen space because it belongs to the occluder rather than to the receiver.
The bounds gate that should catch it screens the stream's OBJECT extent against 50,000
units in a ~1,100-unit town, and is blind to a small mesh with a large SCALE in its
instance transform.

This binary now prints `largest admitted meshes by WORLD extent`, and
`tools/part70_bounds_cap.sh` sweeps `CZ_VK_RT_BOUNDS_CAP` 50000/5000/1000/200 beside it.
`tools/rt_tlas_census.py` answers the same question OFFLINE over hardware's own frames
for the static world.

> **PART 70 RAN THIS TOO, and the result is: a real anomaly, NAMED, that is not the
> defect.** The census finds a **4,950-unit mesh at (2234 7 -107), stream
> `va=B576A378`** — 4.5x the town, 6.4x the next largest, centred outside a world box of
> `x[-610 324]`, sailing through a 50,000-unit cap. Removing it (cap 5000 -> 1000) moves
> the shadowed share **0.6 points**, inside the ~1-point route-variance floor. The only
> real move is at cap 200, which deletes every building and costs 2.6 points. Record in
> `phase5-notes.md` §6dc §9. **The mesh is still worth understanding** — nothing should
> be 4.5x the town — but it is not making a slab.

## 1c. THE CAVEAT ON BOTH OF PART 70'S REFUTATIONS, and it decides what to do next

**The headless route reads 12-16% shadowed. The operator's settle-0 arm, the run that
produced the slab, read 39.7%.** This camera is not showing the defect at anything like
its reported strength, so both nulls are evidence about THIS location and no further —
gotcha 172 in a new place, and the "an A/B measures the load it sampled" family.

So the next move is not another mechanism to sweep. It is **the factor image dumped
where the defect actually appears**, which costs the operator nothing beyond standing
there:

```
CZ_VK_RT_FACTOR_READBACK=64 CZ_VK_RT_FACTOR_PGM=~/DR2CZ-troubleshooting/part71-factor
tools/rt_factor_pgm_read.py ~/DR2CZ-troubleshooting/part71-factor
```

* **The straight edge IS in the factor PGM** -> the defect is in the pass: the ray-origin
  bias, the ray length, or the pass's own viewport/scissor. `CZ_VK_RT_FACTOR_BIAS`,
  `CZ_VK_RT_FACTOR_CAMBIAS` and `CZ_VK_RT_RAY_LEN` are the arms, and all three currently
  DEFAULT off the cascade volume (`len=88.5 bias=0.133/0.044` in the operator's last run).
* **The factor PGM is smooth and the edge appears only in the frame** -> the defect is in
  the CONSUMPTION, i.e. the transform the 126 patched shaders use to address the factor.
  Part 66 already found that mapping 427 pixels out vertically once (gotcha 394), and its
  stripe arms (`CZ_VK_RT_FACTOR_DEBUG` stripe modes, `stripeX`/`stripeY`) are the
  alignment instrument — **spatial controls come in pairs; run both.**

The dump needs the operator only to stand in the right place; it fires by itself.

**Checked and NEGATIVE, so do not spend the session on it:** the "the operator runs at a
different RT tier so every headless instrument is blind" theory. Their runs and ours both
read `tier=1 1720x720 rays=1 src=primary-ray`.

## 2. Do not re-buy any of these

* **the sun** — closed above, with a hardware oracle and an in-runtime cross-check that
  runs on every future session for free;
* the placement, the transform table, the blend descriptor — verified against hardware
  twice, and `rt_world_xform_census.py` is a coverage gate at 104 of 104;
* **more occluders.** Four rounds have not moved the signature and the primary ray now
  resolves the real world (§6db §4);
* the **edge-density statistic** — retracted in `§6da §7` in place, its SIGN was wrong;
* the **frustum test** on a metres-scale error — it saturates (gotcha 403);
* "a filter is eating the buildings", route (a), palette entry 0 vs entry 1 — all measured.

## 3. Known-open, named so the session is read against a list

1. **The one free measurement part 70 left owed.** No archived run carries the title's own
   `c23` beside a `(-0.364 0.546 -0.755)` latch, because the instrument did not exist.
   **The next operator session on this binary prints both and `0 blocks REJECTED`.** If
   their save's `c23` also reads -0.755 the sun moved and the confound explanation is
   complete; if it reads something else while the latch says -0.755, the lead comes back
   with an oracle attached. Costs nothing — just read the line.
2. **A main-menu zombie flicker**, operator-reported, unattributed.
   `tools/part69_menu_flicker.sh` is a forty-second two-arm test with an engagement gate;
   **it has never been run with a working control** (gotcha 408).
3. **The frame rate has no denominator.** `18.0 fps median at 8,578 draws` at settle 0
   with RT on, one 194 ms stall. A headless RT-on/RT-off pair at a matched draw band
   bounds it; `CZ_FPS_LOG` is one counter and one clock read.
4. **`budgeted-out` is large** (464,290 at settle 120, 2,662,399 at settle 0) — a big
   share of refits deferred per frame, which would show as geometry lagging a running
   actor. `CZ_VK_RT_REFIT_MB` is the knob.
5. **`CZ_VK_RT_DYN_SETTLE`'s default is still 'exclude any ever-dynamic stream'.** Settle
   0 is survivable now and that is a shipping decision the picture has to make.
6. RT HIGH's four rays buy no penumbra; self-shadowing untested against correct geometry;
   `alpha` stays raster-only; `palConflict` is 2,150-5,624 and bounded by design.

## 4. Carry-overs (non-blocking)

* Turn-stutter under the wide-culling over-widen — operator-deferred (`perf-state-parked.md`).
* Small verdicts from parts 61-62: a gore cut with the fov slider off zero; aiming under
  the slider; the cutscene 21:9 CROP look. **Owed since part 60**: the shadow Low-vs-High
  LOOK verdict.
* Suspected input leak at panel open; decal flicker; the point-list PointSize VUID class.
* **PERFORMANCE IS PARKED** — `docs/perf-state-parked.md` resumes it.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 5. Gates at part 70's close

* `--smoke` OK.
* `shader_dim_census.py` clean on all sixteen caches; the play cache's NAME diff empty.
* `rt_world_xform_census.py` 104 of 104, exit 0.
* `tools/xtr_sun_oracle.py` over all twenty traces: 0.000 degrees worst disagreement.
* **A5 is owed**, carried since part 67. No kernel path changed in 67, 68, 69 or 70.
