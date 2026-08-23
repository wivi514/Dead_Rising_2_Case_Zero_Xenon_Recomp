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

## 1. Start here — THE SHADOW RAY IS 88 UNITS LONG IN A 1,100-UNIT TOWN

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

If the length is not it, the fallback bisection is one dump:

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
