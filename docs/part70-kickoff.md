# Part 70 kickoff — the RT geometry work is DONE and the defect is downstream of it

> **THIS IS THE LIVE HAND-OFF**, superseding `part69-kickoff.md`.
>
> **THE DOCUMENT TO EXECUTE IS `docs/part69-night-plan.md`, and its §3 is the live path.**
> Its §1 has already been run and answered; §2 is explicitly NOT the work. Read, in order:
> `phase5-notes.md` **§6db** (part 69's operator session and the two findings), then
> **§6da** (what part 69 built), then the night plan. The backlog entry is
> `open-items.md` 0v; the lessons are gotchas **403-410**.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction), and the
> Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. Where the feature is, in five lines

* **The Remix plan's items 0-3 are built, and items 1 and 2 are PROVEN on the operator's
  machine**: at `CZ_VK_RT_DYN_SETTLE=0` — the configuration that used to climb to the 1 GB
  cap — a real play session held `tlasInst=5001 blas=8029 (148.6 MB) built=8058 flushes=0`
  with `outOfRange=0`.
* **THE PRIMARY RAY RESOLVES THE REAL WORLD.** `CZ_VK_RT_FACTOR_DEBUG=18` renders a
  recognisable depth image — Chuck, both lamp posts, the power lines, the gantry, the
  fence, the hills. Part 68 read the same instrument as *"a flat plain... no fence, no
  Chuck"*. The occluder work fixed it.
* **And the shadows are still wrong.** The operator: *"the shadows was under them not
  placed in the right way and passing through thing it shouldn't"*, and the settle-0 arm
  produced a flat slab with a hard straight boundary across a container, tyres, cars, a
  fence and the ground, bending at none of them.
* **So the defect is NOT the occluder set** and has not been for some time. Four rounds of
  that (67, 68, 69, and admitting the actors) have not moved the signature, while the
  thing all four were aimed at is now demonstrably correct (gotcha 409).
* **The live lead is the SUN**: its Z component flips sign between headless and windowed
  runs, censused across every run of the session.

## 1. Start here — and it is not a build

`docs/part69-night-plan.md` §3, item 1. The sun census:

```
windowed (operator: bake, nobake, dyn0)   sun=(-0.364  0.546  -0.755)
headless (v_final, seq, m18_settle0)      sun=(-0.371  0.557  +0.743)
```

Two components agreeing to 2% while the third flips sign is not a day/night cycle. **Every
headless RT measurement this feature has ever made was taken with the mirrored one** —
part 66's 0.987 hemisphere reading, part 67's 0.650, all of §6da.

Establish WHICH is right before touching anything. The cheap oracle is not an arm: the
title renders its own raster shadows from its own cascade, so their direction in an
operator F9 is a value no instrument of ours chose. `CZ_VK_RT_SUN_FLIP=1` is the arm once
the answer is known, and the mechanism to check first is the per-frame slice VOTE (§6cw),
because the census reads one distinct vector per run rather than a drift.

Then §3's items 2 and 3 (the origin bias, the ray length) — **and note that all three were
"exonerated" in part 67 against a pile at the world origin, which is no test** (gotcha 172).

## 2. Do not re-buy any of these

* the placement, the transform table, the blend descriptor — verified against hardware
  twice, and `rt_world_xform_census.py` is a coverage gate at 104 of 104;
* **more occluders.** Four rounds have not moved the signature and §1 says the fifth would
  not either;
* the **edge-density statistic** — RETRACTED in §6db as a gate for this class of defect;
  its sign was wrong (several separate actor shadows produce MORE boundary than one smear);
* the **frustum test** on a metres-scale error — it saturates (gotcha 403);
* "a filter is eating the buildings", route (a), palette entry 0 vs entry 1 — all measured.

## 3. Known-open, named so the session is read against a list

1. **A main-menu zombie flicker**, operator-reported, unattributed. The only part-69 change
   touching the raster path is the persist store's extra usage flag, and it is weakened —
   the store reports `memory type 3, heap 1` in both the part-68 and part-69 binaries.
   `tools/part69_menu_flicker.sh` is a forty-second two-arm test, now with an engagement
   gate; **it has never been run with a working control** (gotcha 408).
2. **The frame rate has no denominator.** `18.0 fps median at 8,578 draws` at settle 0 with
   RT on, one 194 ms stall; the two arms before it logged none. A headless RT-on/RT-off
   pair at a matched draw band bounds it, and `CZ_FPS_LOG` is one counter and one clock
   read.
3. **`budgeted-out` is large** (464,290 at settle 120, more at settle 0) — a big share of
   refits deferred per frame, which would show as geometry lagging a running actor.
   `CZ_VK_RT_REFIT_MB` is the knob.
4. **`CZ_VK_RT_DYN_SETTLE`'s default is still 'exclude any ever-dynamic stream'.** Settle 0
   is now survivable and that is a shipping decision the picture has to make.
5. RT HIGH's four rays buy no penumbra; self-shadowing untested against correct geometry;
   `alpha` stays raster-only; `palConflict` is 2,150-5,624 and bounded by design.

## 4. Carry-overs (non-blocking)

* Turn-stutter under the wide-culling over-widen — operator-deferred (`perf-state-parked.md`).
* Small verdicts from parts 61-62: a gore cut with the fov slider off zero; aiming under
  the slider; the cutscene 21:9 CROP look. **Owed since part 60**: the shadow Low-vs-High
  LOOK verdict.
* Suspected input leak at panel open; decal flicker; the point-list PointSize VUID class
  (6 lines a run, unchanged).
* **PERFORMANCE IS PARKED** — `docs/perf-state-parked.md` resumes it.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 5. Gates at part 69's close

* **`CZ_VK_VALIDATION=1 CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0`, headless outdoor route,
  300 s to `passes=6145`: no VUID but the known point-list one.** Four synchronisation
  VUIDs were found and fixed inside part 69 (§6da §9). **Re-run this after any change to
  the RT structure code — it is the cheapest gate this feature has.**
* `--smoke` OK. `rt_world_xform_census.py` 104 of 104, exit 0, and it self-checks its JSON.
* `shader_dim_census.py` clean on all sixteen caches; the play cache's NAME diff empty.
* **A5 is owed**, carried since part 67. No kernel path changed in 67, 68 or 69.
