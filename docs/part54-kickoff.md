# Part 54 kickoff — the work is off the pump and the operator has confirmed it

Written at the close of part 53 (2026-08-17). **This is the LIVE hand-off**, superseding
`part53-kickoff.md`.

---

## START HERE — what part 53 did, in one paragraph

Part 53's whole job was strategy **(b)**: move work off the one thread that is the frame,
onto the thirteen cores that are idle. It did that. **Both content guards — the stream
store's and the texture cache's — now fold on a four-worker pool**, filed a frame ahead
from the working set the pump saw last frame, with an inline fallback on every miss.
`GuardFold` went from **25.87% of the pump thread to 0.86%**, the pump itself from
**63.4% to 50.3% of a core**, and frame time fell **12.5-13.1%** across four adjacent draw
bands against a null of **+0.1%**. **The operator then confirmed it at −17.5% on their own
soak** — see the next section, which also records the second item coming apart there.

`docs/phase5-notes.md` **§6cj** is the record. `perf-plan-part52.md` **§9c** is the plan's
own status. `perf-plan-part52.md` is still the live plan.

---

## THE OPERATOR HAS NOW JUDGED IT — and it settled one item and unsettled another

Done at the close of part 53, `ARM=ab tools/part53_operator_session.sh`, two soaks in
their heaviest place, both items switched together in arm B (`phase5-notes.md` §6cj §10).

**Item 1.1 is confirmed and is bigger on their machine than on the headless route.**

| | |
|---|---|
| 7,000-7,999 draws | **24.65 -> 20.33 ms, −17.5% mean / −16.7% median, significance −50.2** |
| the light bins, as the experiment's own null | **+0.1%** and **+0.5%** |
| `record`'s GUARD | **518 -> 13 ns/draw** |
| `textures` | **428 -> 227 ns/draw** |
| 4,000-5,999 draws, share pinned to the 60 fps cap | **55% -> 98%** and **29% -> 93%** |
| guards served by a finished pre-hash | **97.8%**, 0 pending, 0 blocked, **0 slot mix-ups** |

**And the reading worth carrying forward: the pump did NOT get less busy — it stayed
saturated at ~94% of a core in both arms.** What changed is what each FRAME costs it,
**23.6 -> 18.75 ms**, so the same pinned thread delivers 24% more frames. Headlessly the
pump had slack and the item read as a thread doing less (63.4% -> 50.3%); on the critical
path it reads as throughput instead. Their bill: **2.64 -> 3.11 cores**, four workers at
11.1-11.2% of a core each.

### Item 1.3 looked like it had failed there, and it had not — it was a confound

`readback` read **0.58 -> 0.66 ms** across their two arms, against a headless −0.78, and
was filed as "did not survive". **Its own arm, run the same evening, retracts that**
(§6cj §11): windowed, the guard pool held ON in both sides, `CZ_VK_PRESENT_STAGING` the
only difference, thirteen profile windows an arm — **0.668 vs 1.135 ms, −0.467 ms, with no
overlap at all** between the two sets.

| guard pool | present copies | `readback` |
|---|---|---|
| off | 2 (staging) | **0.56 ms** — the operator's arm B |
| on | 2 (staging) | **1.14 ms** |
| on | 1 | **0.67 ms** — the default, and 0.66 in their arm A |

Their A/B switched both items together, so item 1.3 was compared against an arm that had
the workers off as well — and **an item can read NEGATIVE when the other item in the same
arm taxes it** (gotcha 347). Both items ship.

**Then the operator corrected the correction, and it is the more useful half.** The
per-item arm sat at the military camp — **1,891-4,777 draws** — while their soak is
**7,000-7,500**. Read down its columns instead of taking its mean and `readback` goes
**0.525 -> 0.700 ms with load INSIDE one arm**, so item 1.3's saving is a slope like
everything else here: **−0.37 ms at 1,900 draws, −0.59 at 4,200**. Their load is above the
whole of that range, so the item is worth MORE there than the arm can show. The
three-configuration table above is an INFERENCE, not a matched measurement — it compares
40 fps against ~100 fps — and §6cj §12 says what survives it.

**Two reading errors in one write-up, both "quoted the arm mean without looking down the
column".** That is gotcha 237's failure one instrument over, and this part committed it
twice in an hour. Look down the column.

**Carry the bandwidth number forward.** The pool moves **2.2-3.0 GB/s** in every
configuration measured (23-30 MB/frame at ~100 fps here; **69.8 MB/frame at 49.8 fps on
their machine**), and that pressure is what sets the cost of an unrelated 3.5 MB copy.
Item 1.2 moves texture UNTILING, which is a pure bandwidth job, so **it must be priced
against the memory system and not only against the CPU it frees** — it may not be
−1..2 ms at all.

**Cheap and owed:** fold a `CZ_VK_PRESENT_STAGING`-only pair into the next operator soak.
Two three-minute holds at their load, everything else constant. It is 0.5 ms so it does not
justify an evening alone, but it costs nothing beside an item that does.

---

## AND THE PORT GAINED A RESOLUTION KNOB AT THE END OF PART 53 — read this before
## picking a GPU item, because it changes where the limiter is

`CZ_VK_RES=2560x1440` (`phase5-notes.md` §6cj §14). The operator asked for it, played it,
and said *"perfect looks all good"*. The measurement is the part worth carrying:

| where | 1280x720 | 2560x1440 | cost |
|---|---|---|---|
| light zones | 119-147 fps | 96-97 | **−30 to −35%** |
| ordinary gameplay | 83-114 | 74-92 | −10 to −20% |
| **the heavy end** | **69-71** | **66** | **−4 to −7%** |

**Four times the pixels costs almost nothing where the frame is worst**, because there we
are CPU-bound (gotcha 231: the GPU is idle 68% of every frame). It costs what a resolution
setting should only in the light zones, where we were not. So **this is the first change in
this port that makes the GPU the limiter anywhere**, and it gives GPU-side items a place to
be measured that did not exist before.

**Its bill is the present readback, which is the scale SQUARED: 3.5 -> 14.1 MB/frame.**
That is four times the copy item 1.3 just removed, and it makes the plan's §7 swapchain
item — deferred as "a different and much larger job" — the strongest remaining candidate
outside the parallel tier. Read `readback` in `CZ_VK_PROFILE` before blaming anything else
for a frame time at 2x.

---

## THE STATE OF THE PLAN

| item | expected | risk | note |
|---|---|---|---|
| **§7 a real swapchain** | removes 14.1 MB/frame at 2x | med-high | **promoted by the resolution knob.** It was 3.5 MB/frame and deferrable; at 1440p it is the largest fixed per-frame cost in the renderer |
| **1.2 parallel texture untile** | −1..2 ms | med-high | **unblocked** — the plan's precondition was "only after 1.1 proves the worker pool", and it does. See below for why it is harder than the guard |
| 2.3 audit the always-on censuses | −0.1..0.3 ms | none | |
| 3.3 `_int_malloc` on the frame path | −0.2..0.3 ms | low | still 1.2-1.6% of the pump; part 52 removed ~1,500 mallocs/frame from the shader path and there is another caller |
| 2.2 frame-stats sampling | correction | none | |
| 3.4 `memcmp` | ? | — | **measure before touching** |
| **1.4 parallel command recording** | ~4 ms at 4 workers | **high** | now the largest single item left. Draw ORDER is semantic and there is no gate that would catch getting it wrong |
| 4.2 inline the PM4 walk | −2.2 ms ceiling | **high** | last, and only with a poison arm |

### Re-take the symbol profile before choosing

Every ranking above is from a profile taken **before** part 53's items landed, and
`GuardFold` was a quarter of that thread. With it gone the shares of everything else have
risen without their costs changing (gotcha 320). One command, six minutes:

```
NO_DWARF=1 OUT=$HOME/DR2CZ-troubleshooting/part54 tools/part52_recon.sh p54_base nostats
tools/part53_symbols.py ~/DR2CZ-troubleshooting/part54/p54_base.flat.perf.data
```

`tools/part53_symbols.py` is new and is the thing to use: `perf report --tid=N` does not
renormalise — it filters the ROWS and leaves each symbol's share of the WHOLE profile — so
every hand reading of a per-thread profile here has had to correct for that. `--diff`
prints two arms side by side, which is an item A/B in one command.

### Why item 1.2 is harder than the guard was, in one sentence each

* the guard needs only a **decision** by the time the draw is recorded; the untile needs
  the **result**, a filled staging buffer, before the upload command can be recorded;
* staging allocation would happen from two threads;
* but part 47's finding still helps: a texture whose guard says "unchanged" needs no work
  at all, so the pool only ever handles the ones that changed — and the guard that decides
  that is now itself pre-computed a frame ahead, which is more slack than the plan
  assumed.

---

## WHAT PART 53 LEARNED THAT CHANGES HOW THE NEXT ONE MEASURES

* **A parallel item's price is not its symbol share (gotcha 344).** 13.1 points of a core
  left the pump and **33.2 appeared on the workers**. A memory-latency-bound loop is
  cheaper interleaved with other work than run alone, so `perf` understates its isolated
  cost — a symbol share tells you what to MOVE, not what moving it will cost. Budget three
  things: the work that moves, the dispatch bookkeeping, and the **cache the moved work
  stops warming and starts polluting** (~0.4 ms/frame here, and it showed up in
  `recordState` and `otherBegin`, two phases with nothing to do with hashing).
* **A fast path that only runs when no instrument is armed is a fast path nothing tests
  (gotcha 345).** Item 1.3's obvious predicate — "copy only if an instrument will read it"
  — would have shipped a default path that no gate in this project exercises, because
  every picture gate sets one of those variables. Ask what the gates set.
* **Make a predicted slot carry the descriptor it was computed for (gotcha 346).** The
  only silent failure in a pre-hash design is one entry receiving another's valid hash.
  Two words per slot and a loud line make it impossible; the poison arm beside it is what
  makes the silence mean something.
* **`CZ_FPS_CAP` up in every arm, still.** The headless route is on the cap; what a
  campaign there reports is a CPU saving, not a frame rate.
* **The saving is a slope.** 1.92 ms at 6,000-6,999 draws, 2.41 at 8,000-8,999. Quote the
  draw count.

---

## WHAT IS OWED

* **A `CZ_VK_PRESENT_STAGING`-only pair at the operator's load** — see above. Cheap, and
  it is the only number item 1.3 is missing.
* Otherwise nothing from part 53 itself. Both items are measured, confirmed on the operator's own
  machine and route, and their verdict on the picture is *"look and feel is as usual"* —
  which rules out a GROSS stale-buffer regression, the way this change could have failed
  badly, and is not evidence that the widened race never fires (the verify arm puts that at
  0.0002-0.0021%, which no human catches by eye).
* Their two deferred picture items, **00m decals** and **00n a sign and items at
  distance**, still deferred. If the CPU work stalls, these are the alternative.
* **The operator's standing instruction** from part 52: *"prepare a whole plan to fix CPU
  performance issue and we'll start it in a fresh conversation."* `perf-plan-part52.md` is
  that plan, it is still live, and §9b/§9c record what parts 52 and 53 corrected in it.

---

## STANDING STATE

* Runtime defaults: **500 fps cap (a 1 ms vblank period — changed from 60 at the close of
  part 53, on the operator's choice, because part 53 took their frame UNDER the 16 ms
  ceiling the old default imposed)**, host vsync off, 100 us ring tick, **4 guard
  workers**, **no present staging copy**, internal resolution **1280x720** (the title's
  own; `CZ_VK_RES=2560x1440` is the knob).
* **New arms**: `CZ_VK_NO_PARALLEL_GUARD=1`, `CZ_VK_GUARD_WORKERS=N`,
  `CZ_VK_VERIFY_PARALLEL_GUARD=1`, `CZ_VK_VERIFY_PARALLEL_GUARD_POISON=1`,
  `CZ_VK_PRESENT_STAGING=1`, **`CZ_VK_RES=WxH` / `CZ_VK_RES_SCALE=N`**, **`CZ_FPS_LOG=N`**.
  All in `docs/instruments.md`. `CZ_FPS_CAP=60` is now the arm that restores the OLD
  default rather than being it.
* **New instrument lines**: `[vkprof] guard prehash:` (hit rate, MB/frame moved, miss
  reasons, dispatches, drain stalls) and `[vkprof] guard prehash VERIFY:`.
* **New tooling**: `tools/play_session.sh` (a judging session: no profiler, no frame
  stats, no debug menu, `FPS=` and `RES=` knobs, F9 screenshots still armed),
  `tools/part53_operator_session.sh` (the two-arm soak), `tools/part53_symbols.py` (per-thread, per-function `perf` shares with
  a `--diff`), `tools/part53_item_campaign.sh` (the frame-time campaign, which also
  samples per-thread CPU in every arm because a (b) item's bill has to travel with it).
* The shader cache is **438** (was 436). The operator's uncapped play session found
  `vs_44a271ebee6e6354` through the miss counter and **`ps_22a996258bacd2c8` through the
  NAME diff alone** — no run bound it, so the counter read 1 and not 2. Both caches
  rebuilt and identical in membership.
* **A DEFAULT IS NOW WRONG AND IT IS PART 54'S CHEAPEST WIN: the shipped `CZ_FPS_CAP=60`
  costs the operator frames.** Part 53 took their soak frame to 14.44 ms, under the 16 ms
  ceiling, and the title's presents are vblank-quantised — so they get exactly 62.5 fps
  where the work supports 69, and 62.5 in light zones where it supports 119-147. **The
  lever is the PERIOD, not the ceiling**: 120 and 250 both still present a 14.44 ms frame
  at 16.0 ms; only 500 (a 1 ms period) gives 15.0. Its cost is the guest vblank ISR at
  1000/s against 125, which has ~4 minutes of evidence and no complaint. `phase5-notes.md`
  §6cj §13.
* Artifacts: `~/DR2CZ-troubleshooting/part53/` — `p53_base.*` (the opening symbol budget),
  `p53_par.*` / `p53_ctrl.*` (the item's symbol A/B), `p53_baseprof.*` (guard volumes),
  `frame/` (the 9-run campaign).
* **Gates at close: ALL CLEAN.** `--smoke`; both PM4 oracles on B1 (24,527,474 packets, 0
  disagreeing); the switch gate (0 defects); the dimension census (0 disagreements); `no
  translated shader` = 0; `truncated=0`; deepest file **#83 `cinezombie.big`**; **A5 exit 0
  with 4 permutation windows and 0 real**; **E3 best of five +0.8808**, 4 of 5 agreeing on
  layout; **0 `PARALLEL GUARD SLOT MIX-UP` over every run of the part**.
