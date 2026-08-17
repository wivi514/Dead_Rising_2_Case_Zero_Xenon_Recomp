# Part 52 kickoff — CPU performance, with the limiter identified and the plan's biggest item closed

Written at the close of part 51 (2026-08-16). **This is the LIVE hand-off**, superseding
`part51-kickoff.md`.

## START HERE

**THE LIVE PLAN IS `docs/perf-plan-part52.md`**, written at the close of part 51 and built
from a SYMBOL budget rather than a phase table. `docs/perf-plan-part50.md` is now history:
of its seven items three shipped, three are retired or refuted, and the biggest thing in
the frame was never in it. Read the new plan, then `phase5-notes.md` §6ch for the evidence
behind it.

**Its item 0 (the recon) is already done** — `tools/part52_recon.sh`, three configurations,
artifacts in `~/DR2CZ-troubleshooting/part52/` — and it changed the item ranking before the
plan was finished. Two results to carry in:

* **`BindShader` is 12.47% of the pump (~2.3 ms/frame) and is in no plan this project has
  written.** It re-hashes every shader on every `IM_LOAD`, 1,919 a frame, byte-at-a-time.
  The hash IS the shader cache key so it cannot be made faster — it has to be memoized.
  **That is the plan's item 1.0 and the first thing to build.**
* **`DoSwapImpl` reads 19.4% of the pump with `CZ_VK_FRAME_STATS` on and vanishes from the
  top eleven with it off.** An item invented and destroyed by one environment variable.
  `perf` charges inlined code to its container; always re-take a profile with the
  instruments off before pricing anything from it.

**The operator's instruction is still current**: *"prepare a whole plan to fix CPU
performance issue and we'll start it in a fresh conversation."* Their two deferred picture
items (00m decals, 00n a sign and items at distance) remain deferred.

## WHAT PART 51 SETTLED — do not re-derive any of this

### The limiter is OURS, and that is the good answer

Part 50's open question was whether the 93.2% guest thread is the real limiter, and it
warned correctly that a CPU percentage cannot tell WORKING from SPINNING. **It is
spinning, on us.** 84.15% of that thread is `sub_8283C6C8` under `sub_82845160` — which
`docs/phase1-notes.md` **finding 38** identified in phase 1 as the Draw Thread waiting on
the ring read pointer, and the only thing in this process that advances that pointer is
our own pump. Part 50's table called it "a GUEST thread — the title simulating", which was
wrong in the one direction that would have retired the whole plan.

So: **milliseconds taken off our pump are milliseconds off the frame.** The plan's
strategy is correctly aimed, and the parallelism question (part 51's item 2) is now about
where to move work TO, not about whether moving it can help at all.

### Item 2a — soft-dirty page tracking is REFUTED, and the item is closed

The kickoff asked for `clear_refs` to be priced before anything was built on it. Measured
(`tools/part51_clear_refs_cost.py`):

* **arming costs 24.4 ns per resident page = 7.5 ms/frame** on this runtime's 1.2 GB
  resident set, against the ~0.7 ms of hashing it would replace. Fatal on its own.
* **arming write-protects every page**, so re-touching the resident set costs
  **+773 ns/page** in minor faults — charged to whichever thread writes next, i.e. the
  GUEST's. Fatal on its own, and it was not on anyone's list.
* the **query really is cheaper**, 1.6-4x above 64 KB. The attractive part of the idea
  was the part that was fine.

The stream guard stays. Part 50 already established it is not waste (only 11-13% of
proven observations find a change, so it saves the copy on ~88%), and there is now no
cheaper way to ask the question. **Item 2a should be struck from the plan, not deferred.**

### The item nobody had: the pump SLEEPS, and somebody is waiting

Not in `perf-plan-part50.md` at all, because every item there makes the pump's WORK
smaller and a sleep is not work. The pump loop has begun with `sleep_for(1 ms)` since
phase 1; the `pump` line has reported it as 10-18% of the wall clock since part 18.

**The walk stops ~3.1 times a frame whatever the tick period is** — measured invariant
across a 40x range — so the tick sets only how long each of those three stops lasts, and
the saving is arithmetic rather than statistical:

| arm | tick | ticks/frame | `sleep` % | ticks whose walk found work | latency bound |
|---|---|---|---|---|---|
| slow (positive control) | 4 ms | 3.00-3.26 | 39-55% | 92-100% | <= 12.17 ms/frame |
| base | 1 ms | 3.00-3.44 | 10.6-15.9% | 87-100% | <= 3.17 ms/frame |
| fast | 100 us | 3.01-3.35 | 1.6-2.1% | 90-99.7% | <= 0.47 ms/frame |

**100 us is now the DEFAULT**, promoted on three unprofiled runs an arm read by draw bin:
at **3,000-5,000 draws the frame goes 19 -> 16 ms (-15.8%, outside its own 5.3% floor)**
with the 16 ms-pinned share **24-36% -> 72-95%**, and the 4 ms positive control is
**+34.8% to +56.2%, outside the floor in every bin** — i.e. the sleep converts to frame
time at ~1:1, which is what licenses the 2.7 ms in the bins where the direct comparison
sits inside its noise. `CZ_PM4_TICK_MS=1` is the control arm. Costs: process CPU
**2.57 -> 2.75 cores of 16**, pump duty cycle **79% -> 93%**.

## THE ORDER TO TAKE PART 52

**`docs/perf-plan-part52.md` §10 is the order.** It is reproduced here only in outline;
the plan carries the evidence, the control arm and the risk for each item.

| # | item | expected | risk |
|---|---|---|---|
| 1 | **`BindShader` memoization** — stop re-hashing every shader on every `IM_LOAD` | **−1.5..2.3 ms** | low |
| 2 | `Count("...")` -> `COUNT(...)` on the 28 draw-path sites | −0.3..0.4 ms | none |
| 3 | re-split `outside` | — | none |
| 4 | **parallel content guards** — `GuardFold` is 20% of the pump and is pure | **−2..3 ms** | medium |
| 5 | readback off the pump thread | −0.5..1 ms | low |
| 6 | pipeline `std::map` -> flat hash | −0.3..0.7 ms | low |
| 7+ | see the plan | | |

**The operator's verdict on part 51's tick is IN and part 52 does not need to re-ask it.**
They could not tell the two arms apart on feel — which rules out a gross pacing regression
and is not the same as "smoother" — while the profiler showed the win reproducing on their
machine (3,000-5,000 draws 21 -> 17 ms, pinned share 2% -> 51%; `outside` 7.89 -> 5.37 ms
against a predicted 2.7). §6ch §7.

## MEASUREMENT RULES THAT CHANGED IN PART 51

* **Run `perf record` before writing another phase split.** Every guest function is a real
  symbol here, so one command profiles the title's code and ours on the same time base,
  and it disagreed with the phase profiler about our own pump (`DoDraw` 9.84% of the pump
  thread; a content guard 16.79%). `tools/part51_thread_probe.sh`. Gotcha 340.
* **`perf` attributes INLINED code to its container.** Confirm with `--sort=sym,srcline`
  before naming a subsystem.
* **`CZ_VK_FRAME_STATS` has a bill of its own** — it zeroes a 2 MB bitmap and walks
  921,600 pixels per presented frame, and it was enabled in every performance run this
  project has ever recorded, the operator's lap included. It is now instrumented
  (`[vkprof] CZ_VK_FRAME_STATS itself:`). A/Bs carrying it in both arms stay valid; the
  ABSOLUTE frame times this project quotes are inflated by it on top of the profiler's
  2-4 ms.
* **A tick-count threshold is a duration in disguise.** Adding a sub-millisecond tick
  broke the ring trace's "sat on one wait for over a second" warning into "for over 6 ms"
  — on a line whose own comment warned about exactly this (gotcha 157). Both are fixed;
  check any other counter measured in ticks before changing the period again.

## STANDING STATE

* Runtime defaults: 60 fps since part 49 (`CZ_FPS_CAP=30` reproduces the shipped pacing),
  host vsync off, and **the ring tick is 100 us since part 51** (`CZ_PM4_TICK_MS=1` is its
  control arm; `CZ_PM4_TICK_MS=16` is still part 18's).
* New arms in part 51: `CZ_PM4_TICK_US=N`.
* New instrument lines: `[vkprof]   sleep on the critical path:` and
  `[vkprof]   CZ_VK_FRAME_STATS itself:`.
* New tooling: `tools/part51_thread_probe.sh` (per-thread CPU + `perf`),
  `tools/part51_clear_refs_cost.py`, `tools/part51_tick_campaign.sh`,
  `tools/part51_tick_read.py`, `tools/part51_operator_session.sh`.
* Artifacts: `~/DR2CZ-troubleshooting/part51/` — `base.perf.data` and `base.threadcpu`
  (the symbol profile), `profile/` and `frame/` (the tick campaign).
* **Gates at close: all clean.** `--smoke`; both PM4 capture oracles on B1 (24,527,474
  packets, 0 disagreeing); the switch gate (0 defects); the shader dimension census (328
  2D + 97 cube, 0 disagreements); `no translated shader` = 0; `truncated=0`; deepest file
  on a plain boot #83 `cinezombie.big`. Two numbers moved and both were predicted by the
  change: **A5 exit 0 with 4 permutation windows** (part 50 had 3 — a finer tick
  re-interleaves the boot, which is what a permutation window is), and **E3 best of
  fourteen +0.8043** with layout agreeing on every sample over threshold, against part
  50's +0.8820 best of five. The backdrop is animated, so that number is a fact about
  which moment was sampled (gotcha 133) and the cross-session comparison is weak; it is
  quoted low rather than tidied. If a future part wants E3 to be a real gate on this
  code path, dump the SAME frame index in both arms of one session.
