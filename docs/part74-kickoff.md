# Part 74 kickoff — the performance plan is exhausted; the remaining cost is outside the renderer

> **THIS IS THE LIVE HAND-OFF**, superseding `part73-kickoff.md`.
>
> **The live subject is still PERFORMANCE, but there is no live PLAN any more** — part 73
> ran the last unrun item in `docs/perf-plan-autonomous.md` and every item in it is now
> closed, refuted or shipped. Read in this order:
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6di** | **part 73's record — read this first.** Six runs, two items closed on price, two refuted at their mechanism |
> | `docs/perf-plan-autonomous.md` | the plan, **EXHAUSTED**, carrying part 73's four retractions in place |
> | `docs/perf-plan-part72.md` | the item table, with item 1's retraction in place |
> | `docs/perf-state-parked.md` | the reference the item designs came from — **still not superseded** |
> | `docs/open-items.md` **0w** | the one item with life left in it, and it now points OUT of the renderer |
>
> Lessons: gotchas **435-438**. The route is `tools/autoroute.sh` and the operator's
> standing authorisation for it is unchanged.

---

## 0. WHAT PART 73 SETTLED — four items, none of them by shipping code

| item | verdict |
|---|---|
| **§4b, 41 near-empty passes/frame** | **CLOSED at 0.271 ms/frame**, reproducing to 0.7% over two runs — and that is a ceiling, not a saving. The section's own pre-framing was "either ~0.1 ms and irrelevant or ~1 ms and the best-priced item on the board" |
| **item 1, the wide-culling over-widen** | **REFUTED AT ITS MECHANISM.** 0.0-35.9 draws/frame off-screen vertically against a 700 kill. The control moved the *opposite* way to the plan's written prediction, and that inversion is the finding: our widening is what makes the geometry the game already submits *visible* |
| **item E, geometry in VRAM** | **DEAD — 4.3x slower at a matched band**, not the ~14% on record, against a null pair agreeing to 1.5%. And the reason it was re-opened was also wrong: removing the constant gather entirely changes nothing |
| **item 0w, the stutter** | **SPLIT INTO TWO POPULATIONS**, and the larger one is not in the renderer |

**Nothing was shipped.** One fix was predicted, run, refuted (3.2x worse), discriminated
and reverted in the same sitting. Three instruments were added; all are unconditional and
free, and all three are in `instruments.md`.

## 1. THE ONE PLACE WITH WORK LEFT: open item 0w's second population

The slow-frame table records the twelve worst frames of a run and what was inside each.
It deliberately instruments no single candidate, so that a slow frame with **none** of them
elevated is itself a finding — and that is what it returned:

```
frame  2776:  315.4 ms   2,476 draws   776 uploads ( 54,862 KB,  75.2 ms)   97 pipelines
frame  8724:  243.7 ms   6,635 draws   692 uploads ( 14,410 KB,  77.1 ms)    5 pipelines
frame  8725:  101.2 ms   5,739 draws     0 uploads (      0 KB,   0.0 ms)    0 pipelines
frame     7:  234.9 ms      48 draws     0 uploads (      0 KB,   0.0 ms)    0 pipelines
```

**(a)** Texture streaming bursts are real — up to 77 ms of one frame — but only **24-32%**
of the worst frames, and **96.7% of that is one submit round-trip per texture**
(`vkQueueSubmit`+`vkQueueWaitIdle`, 258 us each). The only change that removes it is
**batching**, which needs a per-frame staging arena because `R->staging` is a single buffer
written at offset zero by every upload. That is the one concrete, unbought renderer item
left, and its price is a fraction of a hitch, not of the frame.

**(b)** The rest is not draws, not pipelines, not texture uploads. **Do not instrument the
draw path for it.** Frame 8725 is the frame immediately after a 692-upload burst, which
points at the guest's own streaming — file I/O, decompression, or the PM4 walk being
starved — rather than at anything the renderer does. **The next instrument belongs on the
guest side**, and this port already has the tools: `CZ_FILE_TRACE=1`, `CZ_GUEST_DIAG=1`
(which takes the outdoor route from 0 `[guest]` lines to 1,239 and is what makes streaming
decisions readable), and `perf` against the DWARF line table, which has disagreed with
thirty parts of phase splitting before.

## 2. WHAT IS OWED BY THE OPERATOR — unchanged from part 73, and none of it is long

1. **The sky-flicker verdict** on `tools/part72_flicker_session.sh`'s three arms. `prefix`
   MUST flicker or the fix is unconfirmed rather than confirmed. Still the only open
   question that could mean item C shipped a picture defect.
2. **The thread-budget decision** for item A — and item A is now demoted twice over, so
   this is no longer urgent.
3. **The A5 kernel gate**, carried since part 67. No kernel path has changed in 67-73.

## 3. DO NOT RE-BUY — part 73 adds four entries to this list

* **the wide-culling item** — refuted at its mechanism, not merely under threshold (§0);
* **geometry in VRAM** — 4.3x slower, and the constant-gather rationale for re-asking it is
  dead too;
* **the 41 near-empty passes** — 0.27 ms/frame ceiling;
* **a cheaper wait primitive for texture uploads** — three arms say ~250 us is what a submit
  round-trip costs here whatever waits for it (gotcha 436). Batching, or nothing;
* everything on part 73's list: the pipeline-cache A/B, route (a) for the wide culling,
  whole-pass parallel recording, a range copy for the constants.

## 4. THE ROUTE HAS A TAIL — read this before quoting any number off an autoroute log

`tools/autoroute.sh` builds its presses from 8-second intervals and then runs to a much
longer timeout, so **~60% of every run is a PARKED camera**. It shows up as windowed
statistics identical to the decimal across consecutive windows — part 73's census read
`scene draws 4,313 / classified 804 / on screen 798 / horizontal 6.0` four windows running.
**Discard the frozen tail**, prefer per-window rates over run means, and see gotcha 438.

## 5. GATES AT PART 73's CLOSE

```
--smoke OK on every build                the renderer is behaviour-identical to part 72's close
```

The three additions are counters and clocks; the one behavioural change was reverted in the
same session it was made. The full gate sweep from part 73's close is unchanged from
`part73-kickoff.md` §5 — no shader cache, config or kernel path was touched.

## 6. THE ONE THING TO CARRY FORWARD

Part 72's lesson was *"the instrument worked exactly as built and was not measuring what the
decision turned on."* Part 73's is its sibling: **three of the four items it closed had
already been priced, and not one of those prices was a measurement.** Item 1 carried 4.8 ms,
then 2.5-2.8 ms, then "unpriced"; item E carried "~14% slower"; the near-empty passes
carried "~0.1 or ~1 ms". Measured: essentially zero, 4.3x, and 0.27 ms. **A number that
entered a plan as an estimate keeps its authority forever unless somebody runs it** — and
the cheapest thing in this whole part was the clock that killed the near-empty-pass item in
one run.
