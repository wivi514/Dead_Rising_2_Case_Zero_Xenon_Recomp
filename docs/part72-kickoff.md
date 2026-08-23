# Part 72 kickoff — performance, with the frame re-baselined and the stutter solved

> **THIS IS THE LIVE HAND-OFF**, superseding `part71-kickoff.md` (which is the RT-shadow
> hand-off and stays accurate for a feature that is PARKED, not deleted).
>
> **The live subject is PERFORMANCE** (operator instruction closing part 70). **THE LIVE
> PLAN IS `docs/perf-plan-part72.md`** — it supersedes `perf-plan-part71.md`, which is kept
> because it was executed and records its own two retractions in place. The reference is
> `docs/perf-state-parked.md`; the record is `phase5-notes.md` **§6dd**; the lessons are
> gotchas **414-422**.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction), and the
> Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. What part 71 established, in six lines

* **THE RE-BASELINE: ~28.1 ms / 35.6 fps at ~9,800 draws, 3440x1440 internal.** The stored
  "~10.5 ms at ~7,000 draws" was not merely stale — **the OPERATING POINT moved**, to 5.4x
  the pixels and 1.4x the draws (gotcha 419). Re-price against this, not against §2's text.
* **THE FRAME IS CPU-BOUND at that load.** A quarter of the pixels at the same aspect ratio
  (`CZ_VK_RES=1720x720`, bit-identical draw set) costs **−6.8%** at 9,500-9,999 draws. So
  every item in `perf-state-parked.md` §2 is still worth what it says. At the LIGHT end it
  reverses (−24% at 2,000-2,499 draws).
* **THE POST-LOAD STUTTER WAS PIPELINE COMPILATION AND IT WAS 17.8 SECONDS A SESSION.**
  ~~A persisted `VkPipelineCache` takes it to 450.8 ms (−97.5%).~~ **THE FIX HALF IS
  RETRACTED (part 72):** the 17.8 s is real, but a **driver-side** cache is what removes it,
  and ours is a 7.5x pessimization once that is warm. `phase5-notes.md` §6df §1, gotcha 426,
  `docs/part72-fix-plan.md` §1.
  This renderer passed `VK_NULL_HANDLE` at all three `vkCreateGraphicsPipelines` sites from
  phase 5 until part 71. `frame 1257: 3,753.9 ms building 97 pipeline(s)`. A persisted
  `VkPipelineCache` takes the run's total to **450.8 ms (−97.5%)** and the worst presented
  frame from 4,050 to 291 ms (gotcha 420).
* **The per-draw hook fold is worth +0.74 ms (+2.6%)** and cleared its pre-registered
  0.3 ms kill threshold. `CZ_VK_NO_HOOK_FOLD=1` is its arm.
* **The clip-plane cache is NOT the part-58 regression** (+0.09 ms at the operator's load,
  against Night Run 1's headless +3.0%). §1.1 closed; **§1.2, part 56's per-draw
  dynamic-state calls, is the only named suspect left** and still has no arm.
* **`CZ_FPS_LOG` now carries `p99`, `worst` and `>2x med`**, and reproduced the operator's
  felt stutter ranking on its first outing.

## 1. Start here — the items, in priced order

**This order is new and it is built from part 71's measurements, not from
`perf-state-parked.md` §2's text**, which was written when the frame was believed to be
~10.5 ms at ~7,000 draws.

**`docs/perf-plan-part72.md` §1-5 is this table with a design, arms and a pre-registered
kill threshold for each. The summary:**

> **BOTH OPERATOR SITTINGS ARE RUN. The fix list is `docs/part72-fix-plan.md`, the record
> is `phase5-notes.md` §6df, and the lessons are gotchas 426-429.** Row 1 is UNPRICED (the
> census that was to price it refuted itself, and is fixed); row 2's question was answered
> and INVERTED part 71's headline. Below is what session 1 established, which still stands.
>
> **SESSION 1 (desk work) MOVED ROW 1 TWICE.** Its price was an upper
> bound, not a value (the arm removes the horizontal widening too, so the recoverable
> draws are strictly fewer than 1,930 — ≈2.5-2.8 ms), and **route (a) is dead**: the engine
> has no aspect scalar, settled by a census over the whole image. Both are retracted in
> place in `perf-plan-part72.md` §1a/§1b; the record is `phase5-notes.md` **§6de**; the
> lessons are gotchas **423-425**. Row 1 now has a real arm and the sitting that uses it is
> `tools/part72_vcull_session.sh`.

| # | item | measured price | risk | arm |
|---|---|---|---|---|
| 1 | **the wide-culling over-widen** — ~~+1,930 draws (+24%)~~ **strictly fewer** | ~~≈4.8 ms~~ ~~≈2.5-2.8 ms~~ **UNPRICED — the census refuted itself, see §1a** | low-med | `CZ_VK_VCULL_CENSUS=1` (fixed in `4b701e3`) |
| 2 | item A, parallel command recording (`DoDraw` + driver, ~40% of the pump) | −3 to −5 ms *(re-price)* | **high** | none — **the ORDER GATE is owed first** |
| 3 | item C, the constant GATHER (median 9 VS / 27 PS registers of 256) | ~28 MB/frame of copy | med | none yet |
| 4 | item B, parallel texture untile | unpriced | med | none |
| 5 | §1.2, part 56's per-draw dynamic-state calls | unpriced | low | **one is owed** |
| 6 | item D, the PM4 walk | −2.2 ms ceiling | high | none |

~~**Item 1 is the change of order and it was measured as a side effect of a turn test.** It
is bigger than everything except item A, its fix is already written down
(`perf-state-parked.md`'s part-62 addendum: horizontal-only widening if the engine exposes
a second scalar, or a smaller k at high slider values), and it is the operator's own
felt complaint.~~ **CORRECTED BY SESSION 1.** It is still the operator's own felt
complaint and it still clears its 700-draw kill threshold, but it is roughly half the size
it was priced at, and **"if the engine exposes a second scalar" is answered: it does
not.** So the two remaining routes are (b) hooking the engine's cull — which has no debug
strings anywhere in the image — and (c) a smaller k, which is a picture decision and the
operator's call. **Decide with the census, not with either model.** **The catch to state
before building it:** the over-widen exists because
the 21:9 view shows flanks the game's own 16:9 frustum culls away, so narrowing it brings
back pop-in — this is a picture/performance trade, and the operator decides it, not a
number. Build the horizontal-only form first; that is the one that might be free.

### 1a. What session 1 left for the next sitting

Two things, one sitting, and they do not interact:

```
ORDER=cold,warm,nocache tools/part71_pipeline_session.sh     # 90 s, closes §2 below
tools/part72_vcull_session.sh                                # prices item 1
```

The second is four arms on one snapshotted binary — the census, its semantic control
(`CZ_NO_GAME_FOV=1`), and both directions of its mechanical control. Its preflight runs the
predicate's offline gate and **exits if the internal resolution is 16:9**, because then
there is no over-widen to measure at all. `SELFTEST=1` runs the harness's own twelve gate
cases (four clean, eight deliberate breakages). **Read the controls before the headline**;
if either fails the headline means nothing.

## 2. ~~The one measurement owed~~ — RUN, AND IT INVERTED THE CONCLUSION (see §1a)

**Part 71's three-step improvement has an unattributed middle step.** Pipeline compilation
went 17,827 -> 1,160 -> 451 ms across arms run in that order, and the big step happened
while our cache file was still EMPTY (`0 bytes seeded`). So it is either (a) merely having
a `VkPipelineCache` OBJECT, which lets the driver reuse stages within a run, or (b) the
driver's own implicit on-disk cache, warmed by arm 1. Arm order and cache state are
confounded by construction (gotcha 422). One run separates them:

```
ORDER=cold,warm,nocache tools/part71_pipeline_session.sh
```

If `nocache` still reads ~17 s with the caches warm, the object is doing it. If it
collapses, a driver-side cache is — and then a fresh machine, a driver update or a new
shader can resurrect the 17 s, which changes whether PREWARMING at load is worth building.

**Also watch the cache file's size.** It went 12.4 -> 12.9 -> 13.3 MB over three runs. It
should asymptote as the pipeline set closes; if it does not, it needs a cap.

## 3. Do not re-buy any of these

* **the clip-plane cache** as the part-58 regression — measured at +0.09 ms at the
  operator's load, §6dd §7;
* **`CZ_VK_RT=0` as a bound on parts 59-70's per-draw hooks** — it bounds nothing, the
  cost is gated on the RT variant SHADER CACHE (gotcha 414);
* **`CZ_VK_WIDE=0` as the wide-culling arm** on a 21:9 setting — it also drops 26% of the
  pixels (gotcha 415). `CZ_NO_GAME_FOV=1` is the arm — **but only as the SEMANTIC CONTROL,
  never as the item's price**: it removes the horizontal widening too, which is the
  part-62 fix and is being kept, so its draw difference is an upper bound (gotcha 423).
  `CZ_VK_VCULL_CENSUS=1` is what prices it;
* **searching for the game's aspect scalar** — there isn't one. 2,056 property
  registrations, 1,966 names, exactly one `Aspect` and it belongs to `cZombieSpawnRegion`;
  the image's single 16/9 constant is the UI's. `tools/find_named_properties.py` re-asks it
  in seconds if you doubt it, and a live property trace cannot answer it at all
  (gotcha 424);
* **a felt A/B below the perception floor** — the operator separated 17.8 s from 1.2 s
  instantly and could not rank 1.16 / 0.45 / 0.48 s (gotcha 421);
* **500-draw bands on this workload** — at ~2.5 us/draw a 300-draw mismatch inside a bin
  is 0.75 ms, which flipped both of session 1's conclusions (gotcha 417);
* geometry in VRAM (item E) — refuted, and re-ask only after item C (gotcha 363).

## 4. How to run a session

`tools/part71_perf_session.sh` (four soak arms + turn blocks) and
`tools/part71_pipeline_session.sh` (the stutter and the CPU/GPU probe) are the two
harnesses, and both are worth copying rather than rewriting: **every arm proves it engaged
from a line the FEATURE prints, the harness refuses to report one that does not and exits
non-zero, and every gate was tested against a deliberate breakage before an operator saw
it** (gotchas 30, 408). Preflight NAME-diffs any shader caches the session switches
between (gotcha 390) and echoes `cz_settings.txt`, because part 71 proved the
CONFIGURATION goes stale as fast as the code does.

Read results with `tools/part54_fps_bins.py`, at a band narrow enough for the slope, and
**print each arm's within-band draw median beside its frame time** (gotcha 417).

## 5. Carry-overs (non-blocking)

* RT shadows are PARKED — `part71-kickoff.md` is that hand-off, `open-items.md` 0v the
  backlog. The remaining suspects are the ray's origin bias and the factor's CONSUMPTION,
  and the next move is `CZ_VK_RT_FACTOR_PGM` dumped where the defect appears.
* A main-menu zombie flicker, unattributed; `tools/part69_menu_flicker.sh` has never run
  with a working control.
* Small verdicts owed from parts 61-62, and the shadow Low-vs-High LOOK verdict (part 60).
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 6. Gates at part 71's close

* `--smoke` OK.
* `shader_dim_census.py` clean on all sixteen caches; the play cache's NAME diff empty.
* `rt_world_xform_census.py` 104 of 104, exit 0.
* Zero `no translated shader` across all eight operator arms of part 71.
* **A5 is owed**, carried since part 67. No kernel path changed in 67-71.
