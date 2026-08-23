# Part 72 kickoff — performance, with the frame re-baselined and the stutter solved

> **THIS IS THE LIVE HAND-OFF**, superseding `part71-kickoff.md` (which is the RT-shadow
> hand-off and stays accurate for a feature that is PARKED, not deleted).
>
> **The live subject is PERFORMANCE** (operator instruction closing part 70). The plan is
> `docs/perf-plan-part71.md` with part 71's corrections in place; the reference is
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

| # | item | measured price | risk | arm |
|---|---|---|---|---|
| 1 | **the wide-culling over-widen** — +1,930 draws of 9,817 (+24%) | **≈4.8 ms of 28** | low-med | `CZ_NO_GAME_FOV=1` |
| 2 | item A, parallel command recording (`DoDraw` + driver, ~40% of the pump) | −3 to −5 ms *(re-price)* | **high** | none — **the ORDER GATE is owed first** |
| 3 | item C, the constant GATHER (median 9 VS / 27 PS registers of 256) | ~28 MB/frame of copy | med | none yet |
| 4 | item B, parallel texture untile | unpriced | med | none |
| 5 | §1.2, part 56's per-draw dynamic-state calls | unpriced | low | **one is owed** |
| 6 | item D, the PM4 walk | −2.2 ms ceiling | high | none |

**Item 1 is the change of order and it was measured as a side effect of a turn test.** It
is bigger than everything except item A, its fix is already written down
(`perf-state-parked.md`'s part-62 addendum: horizontal-only widening if the engine exposes
a second scalar, or a smaller k at high slider values), and it is the operator's own
felt complaint. **The catch to state before building it:** the over-widen exists because
the 21:9 view shows flanks the game's own 16:9 frustum culls away, so narrowing it brings
back pop-in — this is a picture/performance trade, and the operator decides it, not a
number. Build the horizontal-only form first; that is the one that might be free.

## 2. The one measurement owed, and it costs 90 seconds

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
  pixels (gotcha 415). `CZ_NO_GAME_FOV=1` is the arm;
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
