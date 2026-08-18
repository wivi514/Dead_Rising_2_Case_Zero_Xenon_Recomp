# Part 54 kickoff — the work is off the pump, the operator confirmed it, and the GPU is
# a limiter for the first time

Written at the close of part 53 (2026-08-17/18). **This is the LIVE hand-off**, superseding
`part53-kickoff.md`. The record is `docs/phase5-notes.md` **§6cj**; the live plan is still
`docs/perf-plan-part52.md`, whose **§9c** is part 53's status inside it.

---

## START HERE — three things changed, and the third changes what part 54 should do

1. **Both content guards moved off the pump onto four workers** (plan item 1.1). This was
   the whole point of the part: the first work this port has ever moved onto idle cores.
   **Operator-confirmed at −17.5%** on their own soak.
2. **The present readback stopped making a redundant 3.5 MB copy** (item 1.3).
3. **The port gained an internal resolution knob, and it is nearly free where the frame is
   worst.** That is not a side quest — it is the finding that should shape part 54, because
   it is the first time the GPU has been the limiter anywhere in this port.

Two defaults also moved: the frame cap **60 → 500** and the readback's staging copy off.
Both are in STANDING STATE, and the cap change **inverts a measurement rule** — see
MEASUREMENT.

---

## 1. ITEM 1.1 — confirmed, and bigger on their machine than headless

`ARM=ab tools/part53_operator_session.sh`, two soaks in their heaviest place, both items
switched together in arm B (§6cj §10).

| | |
|---|---|
| 7,000-7,999 draws | **24.65 → 20.33 ms, −17.5% mean / −16.7% median, significance −50.2** |
| the light bins, as the experiment's own null | **+0.1%** and **+0.5%** |
| `record`'s GUARD | **518 → 13 ns/draw** |
| `textures` | **428 → 227 ns/draw** |
| 4,000-5,999 draws, share pinned to the 60 fps cap | **55% → 98%** and **29% → 93%** |
| guards served by a finished pre-hash | **97.8%**, 0 pending, 0 blocked, **0 slot mix-ups** |

**The reading worth carrying forward: their pump did NOT get less busy — it stayed
saturated at ~94% of a core in both arms.** What changed is what each FRAME costs it,
**23.6 → 18.75 ms**, so the same pinned thread delivers 24% more frames. Headlessly the
pump had slack and the same item read as a thread doing less (63.4% → 50.3%). **On the real
critical path a strategy-(b) item reads as THROUGHPUT, not as a smaller thread.** Their
bill: **2.64 → 3.11 cores**, four workers at 11.1-11.2% of a core each.

### Item 1.3 looked like it had failed there, and it had not — twice

`readback` read **0.58 → 0.66 ms** across their two arms and was filed as "did not
survive". Its own arm retracts that (§6cj §11): windowed, guard pool held ON in both sides,
`CZ_VK_PRESENT_STAGING` the only difference — **0.668 vs 1.135 ms, no overlap across
thirteen windows an arm**. Their A/B switched both items together, so item 1.3 was measured
against an arm with the workers off as well, and **an item can read NEGATIVE when the other
item in the same arm taxes it** (gotcha 347).

**Then the operator corrected the correction, and that half is the more useful one.** The
per-item arm sat at the military camp (1,891-4,777 draws) while their soak is 7,000-7,500.
Read down its columns instead of taking its mean and `readback` goes **0.525 → 0.700 ms
with load INSIDE one arm**, so item 1.3's saving is a slope too: **−0.37 ms at 1,900 draws,
−0.59 at 4,200**. Their load is above that whole range, so it is worth MORE there. The
three-configuration table in §11 is an inference across runs at different frame rates, not
a matched measurement; §6cj §12 says what survives it.

> **Two reading errors in one write-up, both "quoted the arm mean without looking down the
> column".** Gotcha 237's failure one instrument over, committed twice in an hour. **Look
> down the column.**

**Carry the bandwidth number forward:** the pool moves **2.2-3.0 GB/s** in every
configuration measured (69.8 MB/frame at 49.8 fps on their machine), and that pressure is
what sets the cost of an unrelated 3.5 MB copy. Item 1.2 moves texture UNTILING, a pure
bandwidth job, so **it must be priced against the memory system and not only against the
CPU it frees** — it may not be −1..2 ms at all.

---

## 2. THE RESOLUTION KNOB — and why it reorders the plan

`CZ_VK_RES=2560x1440` (§6cj §14). The operator asked for it, played it, and said
*"perfect looks all good"*.

| where | 1280x720 | 2560x1440 | cost |
|---|---|---|---|
| light zones | 119-147 fps | 96-97 | **−30 to −35%** |
| ordinary gameplay | 83-114 | 74-92 | −10 to −20% |
| **the heavy end** | **69-71** | **66** | **−4 to −7%** |

**Four times the pixels costs almost nothing where the frame is worst**, because there we
are CPU-bound and the GPU is idle 68% of every frame (gotcha 231). It costs what a
resolution setting should only in the light zones, where we were not.

Three consequences for part 54:

* **There is now somewhere to measure GPU work.** Every GPU-side idea this project has
  parked was unmeasurable because the GPU was never the limiter. At 1440p in a light zone
  it is.
* **The present readback is now the largest fixed per-frame cost in the renderer** — the
  scale SQUARED, **3.5 → 14.1 MB/frame** — which promotes `perf-plan-part52.md` §7's
  swapchain item out of "explicitly not in this plan".
* **Anything measured from here must say which resolution it was measured at.** A
  per-draw saving and a per-pixel saving now scale differently, and nothing in the existing
  instruments records the scale.

---

## 3. THE STATE OF THE PLAN

| item | expected | risk | note |
|---|---|---|---|
| **§7 a real swapchain** | removes 14.1 MB/frame at 2x | med-high | **promoted by the resolution knob.** At 720p it was 3.5 MB/frame and deferrable; at 1440p it is the biggest fixed cost in the frame. It is also the only item that gets BIGGER as the operator raises the resolution |
| **1.2 parallel texture untile** | −1..2 ms, **re-price it** | med-high | **unblocked** — the plan's precondition was "only after 1.1 proves the worker pool", and it does. But see the bandwidth note in §1: untiling is a bandwidth job and the pool already moves 2.2-3.0 GB/s |
| 2.3 audit the always-on censuses | −0.1..0.3 ms | none | |
| 3.3 `_int_malloc` on the frame path | −0.2..0.3 ms | low | still 1.2-1.6% of the pump; part 52 removed ~1,500 mallocs/frame from the shader path and there is another caller |
| 2.2 frame-stats sampling | correction | none | cheaper now that `CZ_FPS_LOG` exists for the cases that only wanted a frame rate |
| 3.4 `memcmp` | ? | — | **measure before touching** |
| **1.4 parallel command recording** | ~4 ms at 4 workers | **high** | the largest single CPU item left. Draw ORDER is semantic and there is no gate that would catch getting it wrong |
| 4.2 inline the PM4 walk | −2.2 ms ceiling | **high** | last, and only with a poison arm |

### Re-take the symbol profile before choosing — it is six minutes

Every ranking above predates part 53's items, and `GuardFold` was a quarter of that thread.
With it gone the shares of everything else have risen **without their costs changing**
(gotcha 320).

```
NO_DWARF=1 OUT=$HOME/DR2CZ-troubleshooting/part54 tools/part52_recon.sh p54_base nostats
tools/part53_symbols.py ~/DR2CZ-troubleshooting/part54/p54_base.flat.perf.data
```

`tools/part53_symbols.py` is the thing to use: `perf report --tid=N` does **not**
renormalise — it filters the ROWS and leaves each symbol's share of the WHOLE profile — so
every hand reading of a per-thread profile here has had to correct for it. `--diff` prints
two arms side by side, which is an item A/B in one command.

### Why item 1.2 is harder than the guard was, in one sentence each

* the guard needs only a **decision** by the time the draw is recorded; the untile needs
  the **result** — a filled staging buffer — before the upload can be recorded;
* staging allocation would happen from two threads;
* but part 47's finding still helps: a texture whose guard says "unchanged" needs no work
  at all, so the pool only handles the ones that changed — and that guard is now itself
  pre-computed a frame ahead, which is more slack than the plan assumed.

---

## 4. MEASUREMENT — one rule inverted, one added

* **INVERTED: "raise `CZ_FPS_CAP` in every arm" is no longer the instruction.** The default
  IS 500 now (a 1 ms vblank period), so the headless route is no longer on the cap and a
  `CAP=120` override **lowers** it. `tools/part5{2,3}_item_campaign.sh` keep their recorded
  `CAP=120` for reproducibility and each carries a loud note; **a fresh campaign should
  pass `CAP=500`.**
* **ADDED: say which resolution a number was measured at.** See §2.
* **A parallel item's price is not its symbol share (gotcha 344).** 13.1 points of a core
  left the pump and **33.2 appeared on the workers** — a memory-latency-bound loop is
  cheaper interleaved than run alone, so `perf` understates its isolated cost. Budget three
  things: the work that moves, the dispatch bookkeeping, and the **cache the moved work
  stops warming and starts polluting** (~0.4 ms/frame, and it surfaced in `recordState` and
  `otherBegin`, two phases with nothing to do with hashing).
* **A fast path that only runs when no instrument is armed is a fast path nothing tests
  (gotcha 345).** Ask what the GATES set before choosing a predicate.
* **Make a predicted slot carry the descriptor it was computed for (gotcha 346).**
* **Look down the column, not at the arm mean** (see §1).
* **Every saving here is a SLOPE.** Quote the draw count with it.

---

## 5. WHAT IS OWED

* **A `CZ_VK_PRESENT_STAGING`-only pair at the operator's load.** 0.5 ms, so it does not
  justify an evening alone, but it costs nothing folded into the next soak.
* Their two deferred picture items, **00m decals** and **00n a sign and items at
  distance**. If the CPU work stalls, these are the alternative.
* **The operator's standing instruction** from part 52: *"prepare a whole plan to fix CPU
  performance issue and we'll start it in a fresh conversation."* `perf-plan-part52.md` is
  that plan, still live; §9b/§9c record what parts 52 and 53 corrected in it.

---

## 6. GATES AT CLOSE — ALL CLEAN, at BOTH resolutions

Re-run whole after the resolution knob, the cap default change and `CZ_FPS_LOG`, because
the earlier pass in the part predates all three (§6cj §15).

`--smoke`; switch gate 0 defects; dimension census 0 disagreements; both PM4 oracles on B1
(24.5 M packets, 0 disagreeing); `no translated shader` 0 **at both scales**; `truncated=0`;
deepest file **#83 `cinezombie.big`**; **A5 exit 0, 4 permutation windows, 0 real**;
**0 `PARALLEL GUARD SLOT MIX-UP`**; **E3 at 1x best of five +0.8396** and **E3 at 2x best of
five +0.8562**, 4 of 5 agreeing on layout at each.

**The 2x row closes the last thing that was owed**: until it ran, the resolution change's
only oracle was the operator's eye. The picture at 2560x1440 now matches hardware's own
screenshot as well as the 1x picture does — and it needed no new tooling, because
`frame_signature.py` already compares across sizes.

> E3 read +0.8808 earlier in the part and +0.8396 here on the same code path, and the
> capture filenames say why: the earlier pass sampled frames 1,896-3,853 and this one
> 5,890-13,874. The gate presses F9 on a fixed schedule, the cap moved 60 → 500, so far
> more frames elapse in the same 120 s and the five presses land on different moments of an
> ANIMATED backdrop (gotcha 133). **A frame-index-addressed sample of a moving scene is
> re-aimed by anything that changes the frame rate.** Do not read a future E3 drop as a
> regression without checking which frames it sampled.

---

## 7. STANDING STATE

* **Runtime defaults**: **500 fps cap (a 1 ms vblank period — changed from 60 at the close
  of part 53, on the operator's choice)**, host vsync off, 100 us ring tick, **4 guard
  workers**, **no present staging copy**, internal resolution **1280x720** (the title's
  own). `CZ_FPS_CAP=60` is now the arm that restores the OLD default rather than being it;
  `CZ_FPS_CAP=30` still reproduces the shipped title pacing exactly.
* **Why the cap moved**: part 53 took the operator's soak frame to 14.44 ms, **under** the
  16 ms ceiling the 60 fps default imposed, and the title's presents are vblank-quantised —
  so the default had started rounding them to exactly 62.5 fps where the work supported 69,
  and to 62.5 in light zones where it supported 119-147. **The lever is the PERIOD, not the
  ceiling**: 120 and 250 both still present a 14.44 ms frame at 16.0 ms. Cost: the period is
  also the guest's vblank ISR cadence, **1000/s against 125**. §6cj §13.
* **New arms**: `CZ_VK_NO_PARALLEL_GUARD=1`, `CZ_VK_GUARD_WORKERS=N`,
  `CZ_VK_VERIFY_PARALLEL_GUARD=1`, `CZ_VK_VERIFY_PARALLEL_GUARD_POISON=1`,
  `CZ_VK_PRESENT_STAGING=1`, `CZ_VK_RES=WxH` / `CZ_VK_RES_SCALE=N`, `CZ_FPS_LOG=N`. All in
  `docs/instruments.md`.
* **New instrument lines**: `[vkprof] guard prehash:` (hit rate, MB/frame moved, miss
  reasons, dispatches, drain stalls), `[vkprof] guard prehash VERIFY:`, `[fps]`, and the
  `[vk] internal resolution` line at start-up.
* **New tooling**:
  * `tools/play_session.sh` — a JUDGING session: no profiler, no frame stats, no debug
    menu, `FPS=` and `RES=` knobs, F9 screenshots still armed. Use this when the question
    is "how does it feel"; paying 5.7 ms of instruments there is the probe that
    manufactures its own result.
  * `tools/part53_operator_session.sh` — a MEASURING session, `ARM=ab` for the two-arm
    soak, and it samples per-thread CPU on the operator's own F9 because a (b) item's bill
    has to travel with it.
  * `tools/part53_symbols.py` — per-thread, per-function `perf` shares with `--diff`.
  * `tools/part53_item_campaign.sh` — the frame-time campaign.
* **The shader cache is 438** (was 436). The operator's uncapped play session found
  `vs_44a271ebee6e6354` through the miss counter and **`ps_22a996258bacd2c8` through the
  NAME diff alone** — no run bound it, so the counter read 1 and not 2. **Fourth part
  running that the name diff catches an entry the count cannot.** Both caches
  (`assets/shader_spv`, `assets/shader_spv_a2m`) rebuilt and identical in membership.
* **Artifacts**: `~/DR2CZ-troubleshooting/part53/` — `p53_base.*` (the opening symbol
  budget), `p53_par.*`/`p53_ctrl.*` (the item's symbol A/B), `p53_baseprof.*` (guard
  volumes), `frame/` (the 9-run campaign). `~/DR2CZ-troubleshooting/part53-operator/` —
  `part53on/off.*` (the two soaks). `~/DR2CZ-troubleshooting/play/` — the uncapped 720p
  session and the 1440p one.
