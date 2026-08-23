# The performance plan, part 71 — resuming from the parked state

> **SUPERSEDED BY `docs/perf-plan-part72.md`.** This document was EXECUTED — both of its
> operator sessions ran and are recorded in `phase5-notes.md` §6dd §7-11 — and it is kept
> for two reasons: it records its own two retractions in place (§1.4's `CZ_VK_RT=0` and
> §6's `CZ_VK_WIDE=0`, gotchas 414-415), and its §5 measurement rules are still correct.
> **Its ITEM ORDER is superseded**, because §0's re-baseline found the operating point had
> moved as well as the code.

**Written on the operator's instruction closing part 70:** *"We'll stop for now with
trying to get ray tracing running. Disable that we can select it in game. We'll now
switch to fixing performance issue."*

RT shadows are **parked, not deleted** — the menu no longer offers them
(`CZ_VK_RT_MENU=1` puts the rows back, `CZ_VK_RT_SHADOWS=N` still engages the feature),
and everything the feature learned stays in `open-items.md` 0v and
`docs/part71-kickoff.md`. This document is the live performance plan.

> **`docs/perf-state-parked.md` IS STILL THE REFERENCE and is not superseded.** It holds
> the pump thread's symbol table, the four remaining items with their risks and expected
> sizes, every arm that exists, and the three ways part 55 got a measurement wrong. This
> plan says what to do NEXT and why that order; it does not restate any of that. Read it
> first. `docs/measurement.md` and `docs/perf-nightrun1.md` are the other two.

---

## 0. THE ONE THING THAT MUST HAPPEN BEFORE ANY ITEM IS PRICED

**The frame at the operator's soak has not been measured since part 58, and thirteen
parts have shipped since.** The trail:

| when | frame at ~7,000 draws | note |
|---|---|---|
| part 55 open | ~12.8 ms (~80 fps) | |
| part 55 close | **~10.5 ms (~95 fps)** | −18%, all from deleting work |
| part 58 spot check | 11.8-12.3 ms (82-86 fps) | **+1.3-1.6 ms, unattributed** |
| parts 59-70 | **never measured** | fov slider, settings panel, wide culling, RT stage 2, four RT sub-features, part 70's per-draw sun probe |

Every number in `perf-state-parked.md` §2 is priced against the ~10.5 ms baseline. **A
recorded estimate has the same shelf life as a recorded measurement** (gotchas 13,
50/51/86: the control is the old binary run NOW, not its remembered number). So the first
measurement is not an experiment, it is a re-baseline:

**Experiment 0 — one soak, current build, `CZ_FPS_LOG` only.** Three minutes standing in
the heaviest place they know (their own framing: *"soak at the spot that hit the cpu the
most"*). Quote the median frame time, the median draw count, and the `[threads]` line.
Nothing else runs in that session.

Then, and only then, the items below have a denominator.

---

## 1. FIND THE REGRESSION BEFORE OPTIMISING ANYTHING NEW

+1.3-1.6 ms appeared between part 55's close and part 58, and nothing since has been
measured at all. **Recovering a known regression is worth more than any item in
`perf-state-parked.md` §2 per unit of risk**, because the code that caused it is already
narrowed to a candidate list and most candidates already have an arm.

Cheapest arm first. All are one environment variable, all are the same binary, and all
chain in one session with `tools/part55_chained_ab.sh`:

| # | suspect | arm | prior |
|---|---|---|---|
| 1.1 | the **clip-plane cache** — six plane dots per vertex in all 104 VS, plus a per-draw plane-block zero+publish | `CZ_SHADER_SPV=assets/shader_spv_a2m` (stock, no clip) against the default `_clip_a2m` | Night Run 1: **+0.20 ms / +3.0%** at a ~5,000-draw headless band, ≈0 at 2,500. Consistent with a per-vertex cost and **probably not the whole 1.5 ms** |
| 1.2 | part 56's **per-draw dynamic-state calls** | **no arm exists — one is owed**, and it is the co-suspect Night Run 1 left standing | none |
| 1.3 | the **wide-culling over-widen** (k=1.34 in tan space, so ~1.8x the culled volume) | `CZ_VK_WIDE=0`, `CZ_NO_GAME_FOV=1` | submitted draws 5,538 -> 5,798 at stand-still; the operator's complaint is a TURN phenomenon (§4) |
| 1.4 | **parts 59-70's per-draw hooks**, part 70's included | ~~`CZ_VK_RT=0`~~ **RETRACTED — see below.** The arm is `CZ_VK_NO_HOOK_FOLD=1`, built in part 71 | none — and nobody has looked at this |

> **RETRACTION, part 71, in place: `CZ_VK_RT=0` DOES NOT BOUND THESE HOOKS.** It was
> written here as "the whole RT device off", which is true of the device and false of the
> cost. `ps.moduleRt` is populated by the RT variant cache loader with **no reference to
> `rtEnabled`**, and `NoteAtlasFetch`'s guard was `ps.moduleRt` **alone** — so with
> `CZ_VK_RT=0` every one of the 9.5 M-per-100 s fetch decodes below still runs, and the
> arm's only effect is to make two already-false predicates false one compare sooner. It
> would have measured ~nothing and the null would have read as "the hooks are free".
> The general form is worth keeping: **an arm named after a FEATURE bounds the feature's
> cost only if every piece of that cost is gated on the feature** — here one piece was
> gated on the ASSET the feature loads, which outlives it.


**1.4 is new and it is this project's own doing.** `DoDraw` now calls `FovCensus`,
`RtGeometryCensus`, `rtshadow::Collect`, `rtshadow::NoteGuestSun` and two
`rtfactor::Active()` tests on **every draw**, each returning almost immediately when its
feature is off. At ~33,000 draws a frame that is ~200,000 calls a frame whose only job is
to decide not to run. Part 55's entire result came from deleting work of exactly this
shape, and §2 is the item.

**Pre-register the kill threshold before running any of these** (gotcha 366): an arm that
recovers less than 0.3 ms at the soak does not justify a code change, and saying so in
advance is what stopped part 55 shipping a bad design.

---

## 2. THE NEW ITEM PARKING RT MAKES AVAILABLE — collapse the per-draw hook chain

With RT off by default and off in the menu, the five per-draw calls above are dead weight
in every shipped run. They are not free:

* each crosses a namespace boundary in a 15,000-line TU, so not all of them inline;
* `rtshadow::Collect` opens with `Active()`, which is `R->rtEnabled && TierThisFrame()`,
  and `TierThisFrame()` compares a frame counter and returns a cached int — cheap, but
  per draw and behind a call;
* `NoteGuestSun` (part 70) is the newest and does the same;
* `FovCensus` and `RtGeometryCensus` each test a static bool.

**And the per-draw hooks are not the worst of it — there is a per-FETCH one.**
`rtshadow::NoteAtlasFetch` is called from the texture walk for every declared fetch of
any shader that HAS an RT variant, guarded on `ps.moduleRt` and **not on whether RT is
running**. It does a real `DecodeTextureFetch` register decode, a linear scan of up to
eight candidates, and a recompute of the largest-by-area — and a 100-second run of the
PARKED build measured it at:

```
[rtb] depth surfaces fetched by the shadow shaders (the largest is taken as the cascade atlas):
[rtb]   1439B000  4096x1024   9482873 fetches   <-- ATLAS
```

**9,482,873 fetches in 100 seconds with the feature off**, in a shipped build. Gating it
on `rtshadow::Active()` is behaviour-preserving when RT is on (the binding is only ever
consumed by `LatchSun`, which needs `Active()` anyway) and removes all of it when RT is
off. **This is the single most concrete instance of the item and the one to price first.**
Do not assume its size: 9.5 M cheap operations over 100 s is a fraction of a percent of
wall time if each costs 20 ns, and worth real milliseconds if the decode is dearer than
that. `tools/part55_srcline.py` answers it at zero cost to the subject.

**The fix's shape is one per-FRAME decision instead of five per-DRAW ones**: a single
`R->drawHooks` word computed at frame roll and one `if` around the whole block. That is
the pattern this renderer already uses for the shadow tier, and the reason it belongs at
frame level is written on `TierThisFrame` itself — *"reading the settings store directly
would take its mutex ~7,000 times a frame"*.

* **arm:** `CZ_VK_NO_HOOK_FOLD=1` restores the five per-draw calls and the per-fetch one.
* **counter:** the folded path counts the frames it skipped, or it cannot be shown to
  have engaged (gotcha 151).
* **gate:** with every hook's feature ON, folded and unfolded must produce identical
  censuses — and that verify arm needs a poison arm to prove it can fire (gotcha 30).
* **expected: unknown, and that is the honest answer.** **Measure it with
  `tools/part55_srcline.py` BEFORE building it** — a `ProfScope` on a path called 33,000
  times a frame costs 1.3 ms and would manufacture its own result (gotcha 360). If the
  fold is worth under 0.3 ms, say so and drop it.

Cheap, low risk, and the only item here whose cost this project created in the last
thirteen parts rather than inherited.

---

## 3. THEN THE PARKED ITEMS, in `perf-state-parked.md` §2's order — with one change

That order is A (parallel recording), B (parallel untile), C (the constant gather),
D (the PM4 walk), E (refuted). **Take C before A**, for two reasons that were not known
when the order was written:

1. **C is now sized and A is not.** Night Run 1's `tools/alu_const_census.py` killed the
   range-copy design (median PS span 255 of 256 — the c255 tonemap cluster) and left the
   GATHER design alive at ~10x: median **9 VS / 27 PS** registers actually read of 256,
   with `a0`-relative indexing forcing a full copy on only 22 VS and 0 PS. The item is a
   per-shader register list in the sidecar, a gather copy, and a verify arm shaped like
   the constant memo's. The target is ~28 MB/frame of copy at soak load.
2. **C is the only thing that can un-refute E.** `CZ_VK_VRAM_STREAMS=1` measured ~14%
   SLOWER because we re-upload constants every draw (gotcha 363). Re-ask E after C lands
   and not before — and if C works, E is a second win for one item's risk.

A stays the largest single item (~40% of the pump: `DoDraw` plus the driver) and the
riskiest, and **its ORDER GATE must exist before any of it is written** — a per-frame
ordered hash of (draw index, pipeline, vertex range) compared between the serial and the
parallel path. `perf-state-parked.md` §5 item 3 has called this owed for sixteen parts and
it is still the right precondition: **there is no other gate that catches getting draw
order wrong**, and draw order on this title is semantic.

B and D are unchanged. D's ceiling is −2.2 ms, it is high risk, and it goes last.

---

## 4. THE OPERATOR-FELT ITEM, which no frame-time median will show

**Turn stutter under the wide-culling over-widen.** Their words: *"much worse when turning
the camera with big stutter"*, deferred at the time with *"we'll fix that later"*. The
steady-state draw cost barely moves (5,538 -> 5,798 at the camp), so this is an UPLOAD
BURST on first sight, not a per-frame cost — and a median frame time is the statistic
least able to see it (gotcha 237: read the distribution, not the mean).

Measure it as a distribution: `CZ_VK_PROFILE` across a deliberate 360-degree turn, plus
the share of frames above 2x the median. Candidate fixes in cost order: budgeted uploads
per frame; horizontal-only widening if the engine exposes a second scalar; a smaller k at
high slider values.

**Ask the operator whether this or the frame-time work comes first.** It is the only
performance problem they have reported *feeling*, and a felt stutter usually outranks a
percentage.

---

## 5. HOW TO MEASURE — the short form, because the long form is written down

All of this is `perf-state-parked.md` §3 and `docs/measurement.md`. The five broken most
often, as one-liners:

1. **The operator's soak is the only load worth quoting** (gotcha 355). Their heaviest
   spot, three minutes standing still, both arms in one sitting.
2. **The two soaks are never at the same draw count.** Project one onto the other using
   each arm's OWN within-arm slope, quote both projections, say which windows.
3. **Compare an arm against its own control, never against last session's number**
   (gotcha 364).
4. **Quote medians and the 16 ms-pinned share, not means** (gotchas 237/238), and say
   which instruments were armed — `CZ_VK_FRAME_STATS` alone costs 12-20%.
5. **Every parallel item needs four things**: a verify arm, a poison arm proving the
   verifier can fire, a counter proving it engaged, and its BILL measured on the workers
   (gotchas 30, 151, 342, 344, 346).

And the one this plan adds: **re-baseline first.** Thirteen parts of unmeasured features
sit between the last number and this one.

---

## 6. THE FIRST SESSION, concretely

One operator sitting, chained arms, `CZ_FPS_LOG` only, their heaviest spot, three minutes
each. **BUILT: `tools/part71_perf_session.sh`.** Two of the four arms named below were
replaced, and both replacements are corrections rather than preferences:

1. **`base`** — current build, no arms, hook fold ON — the re-baseline (§0).
2. **`nofold`** — `CZ_VK_NO_HOOK_FOLD=1` (§2, and §1.4's real arm). **This is also the
   pre-part-71 build**, so its median is what compares with part 58's 11.8-12.3 ms, and
   `base` minus it is the fold. Replaces `CZ_VK_RT=0`, retracted in §1.4 above.
3. **`noclip`** — `CZ_SHADER_SPV=assets/shader_spv_a2m` — the clip-cache arm (§1.1), the
   resume experiment `perf-state-parked.md` §6 already names.
4. **`nogamefov`** — `CZ_NO_GAME_FOV=1`, the wide-mode over-widen off at a CONSTANT pixel
   count. Replaces `CZ_VK_WIDE=0`: this operator's `cz_settings.txt` is 3440x1440, so
   `CZ_VK_WIDE=0` would force 2560x1440 — a 26% resolution change wearing a culling
   change's label, whose frame-time delta would be mostly GPU. §1.3 and §4 in one arm.

Four arms, twelve minutes of soak plus menu time. **Each arm ends with ~30 s of continuous
camera turning**, which is new: `CZ_FPS_LOG` now carries `p99`, the worst frame and the
share above 2x the window median, so the same run answers §4's felt question and §0's
measured one. Every arm prints a line that proves it engaged, the harness refuses to
report one that does not and exits non-zero (gotcha 408), and all four gates were tested
against deliberate breakages (gotcha 30). **Nothing further is built until that session is
read.**

### What part 71 built BEFORE that session, and why that is not a violation of the rule

§2's fold is shipped **on** with `CZ_VK_NO_HOOK_FOLD=1` as its control arm, rather than
measured first with `tools/part55_srcline.py` as this plan asked. The reason is that
`srcline` needs a run of the game and every run belongs to the operator (standing
instruction, part 57) — so "measure first" and "build first" cost the same one session,
and shipping the arm means that session PRICES the item instead of only sizing it. The
fold is behaviour-preserving by construction (its word is the OR of every hook's own arm),
so the risk that argument was protecting against is not present. **The kill threshold
stands unchanged: under 0.3 ms at the soak and it comes back out.**
