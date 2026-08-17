# Part 54 kickoff — the work is off the pump; the operator has not seen it yet

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
bands against a null of **+0.1%**. A second, smaller item removed a redundant 3.5 MB copy
from the present path (`readback` 5.5-7.3% -> **0.0%**).

`docs/phase5-notes.md` **§6cj** is the record. `perf-plan-part52.md` **§9c** is the plan's
own status. `perf-plan-part52.md` is still the live plan.

---

## THE FIRST THING TO DO, AND IT IS NOT AN ITEM

**Everything in part 53 is headless.** The operator has judged every performance part
since 47 and has twice corrected a headless conclusion; part 52's own hand-off records
that their frame is heavier than this route and that the place to measure is **the
three-minute soak they found**, which is *not* at the frame cap.

Ask for one session, `ARM=ab tools/part52_operator_session.sh`, at the soak:

* it chains the arms so quitting one starts the next;
* the control arm is `CZ_VK_NO_PARALLEL_GUARD=1`;
* bin by draw count, and read the light bins as the experiment's own null;
* **and ask for the process's core count in both arms**, because this item costs CPU —
  it is the first one that does.

If they cannot, the headless campaign stands on its own (three runs an arm, alternated,
with a null), but "the operator has not judged it" belongs in any summary until they have.

---

## THE STATE OF THE PLAN

| item | expected | risk | note |
|---|---|---|---|
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

* **The operator's verdict on part 53** — see START HERE.
* Their two deferred picture items, **00m decals** and **00n a sign and items at
  distance**, still deferred. If the CPU work stalls, these are the alternative.
* **The operator's standing instruction** from part 52: *"prepare a whole plan to fix CPU
  performance issue and we'll start it in a fresh conversation."* `perf-plan-part52.md` is
  that plan, it is still live, and §9b/§9c record what parts 52 and 53 corrected in it.

---

## STANDING STATE

* Runtime defaults: 60 fps cap, host vsync off, 100 us ring tick, **4 guard workers**,
  **no present staging copy**.
* **New arms**: `CZ_VK_NO_PARALLEL_GUARD=1`, `CZ_VK_GUARD_WORKERS=N`,
  `CZ_VK_VERIFY_PARALLEL_GUARD=1`, `CZ_VK_VERIFY_PARALLEL_GUARD_POISON=1`,
  `CZ_VK_PRESENT_STAGING=1`. All in `docs/instruments.md`.
* **New instrument lines**: `[vkprof] guard prehash:` (hit rate, MB/frame moved, miss
  reasons, dispatches, drain stalls) and `[vkprof] guard prehash VERIFY:`.
* **New tooling**: `tools/part53_symbols.py` (per-thread, per-function `perf` shares with
  a `--diff`), `tools/part53_item_campaign.sh` (the frame-time campaign, which also
  samples per-thread CPU in every arm because a (b) item's bill has to travel with it).
* The shader cache is **436**, unchanged this part; the name diff found nothing new.
* Artifacts: `~/DR2CZ-troubleshooting/part53/` — `p53_base.*` (the opening symbol budget),
  `p53_par.*` / `p53_ctrl.*` (the item's symbol A/B), `p53_baseprof.*` (guard volumes),
  `frame/` (the 9-run campaign).
* **Gates at close: ALL CLEAN.** `--smoke`; both PM4 oracles on B1 (24,527,474 packets, 0
  disagreeing); the switch gate (0 defects); the dimension census (0 disagreements); `no
  translated shader` = 0; `truncated=0`; deepest file **#83 `cinezombie.big`**; **A5 exit 0
  with 4 permutation windows and 0 real**; **E3 best of five +0.8808**, 4 of 5 agreeing on
  layout; **0 `PARALLEL GUARD SLOT MIX-UP` over every run of the part**.
