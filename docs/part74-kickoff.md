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

## 1b. THE PLAN FOR PART 74, in order, with what kills each step

**The shape of the part:** one short operator sitting that is owed anyway and re-aims
everything, then one instrument I can build and run myself, then the one concrete renderer
item that is left. Steps 2-4 are autonomous on `tools/autoroute.sh`.

### Step 0 — ONE OPERATOR SITTING, and it comes first because it can re-aim the whole part

Two things are already owed and a third costs one sentence. Bundle them:

1. **The sky-flicker verdict** on `tools/part72_flicker_session.sh`'s three arms
   (`nogather` / `fixed` / `prefix`). **`prefix` MUST flicker** or the fix is unconfirmed
   rather than confirmed. This is the only open question that could mean item C — the one
   thing this whole campaign shipped — carries a picture defect.
2. **"Is there still a felt stutter, and WHERE — menu, load, or gameplay?"** Part 72's
   binning of their session said gameplay was smooth (`>2x med` 0.0% over 51 windows) and
   every hitch sat below ~2,000 draws. **Part 73's route disagrees**: its worst frames are
   at 2,476 and 6,635 draws, which are gameplay counts. One of those two readings is about
   a route rather than about the game, and only their eye can say which. **If they report
   gameplay as smooth, steps 2-4 are chasing a route artifact and the part should stop and
   ask them what to fix instead.**
3. Anything they want fixed. Performance has had five parts; it is worth asking.

### Step 1 — THE A5 KERNEL GATE, owed since part 67, and I can run it myself

It needs no operator: a plain boot and the diff tool. No kernel path has changed in parts
67-73, so this is expected clean and it is cheap insurance that seven parts of renderer work
did not disturb one.

```
(cd runtime/build && timeout 300 ./cz_runtime > /tmp/a5.log 2>&1)
python3 tools/kernel_call_diff.py --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" \
    --ours /tmp/a5.log --include-high-frequency
```

### Step 2 — THE RESIDUAL INSTRUMENT: make the slow-frame table's columns SUM TO THE FRAME

**This is the heart of the part.** Part 73's table found slow frames with **none** of its
three candidates elevated and concluded "the cost is outside the renderer" — which is an
*absence*, and an absence is the weakest kind of finding this project accepts. Frame 8725
was 101.2 ms at a normal 5,739 draws with zero uploads and zero pipelines.

So: add coarse unconditional clocks to the same per-frame record until **the columns account
for the whole frame**, and print the leftover as its own column. Candidate columns, all of
which already have a natural scope in `vk_renderer.cpp` and none of which needs
`CZ_VK_PROFILE` (2-4 ms a frame — the same order as the thing being measured, gotcha 7):
the PM4 walk, draw recording, texture uploads (exists), pipeline creation (exists), the
resolve/begin cycles (exists, §4b), present + readback, and **the residual**.

**The residual column is the deliverable.** It converts "not in the renderer" into a number,
and it is the only design that cannot return a false absence — every millisecond lands
somewhere by construction.

* **Positive control, and it is not optional** (gotcha 30): add a debug arm that sleeps a
  known number of milliseconds on a chosen frame and confirm the residual reports it. A
  residual that cannot be shown to move is not a measurement.
* **Pre-registered kill:** if the residual is **below 30%** of the worst frames' time, the
  renderer owns the hitch after all, step 2 is finished, and step 3 is the work. If it is
  above 30%, step 4 is the work and the draw path should be left alone.
* Cost: a handful of clock reads per frame against thousands of draws — the same bill the
  three part-73 instruments already pay, and all of them are unconditional for the reason
  gotcha 418 gives.

### Step 3 — BATCH THE TEXTURE UPLOADS (only if step 2 says the renderer owns it)

The one concrete, priced, unbought renderer item left. Today every upload is its own
`vkQueueSubmit` + `vkQueueWaitIdle` — **2,350 round-trips a run at 258 us each, 96.7% of the
upload cost**, up to 77 ms inside a single frame. Part 73 proved **no cheaper wait primitive
exists** (three arms, gotcha 436), so the only fix is not doing 2,350 of them.

* **What it needs:** a per-frame staging arena. `R->staging` is one buffer written at offset
  zero by every upload, which is why the current code must drain before reusing it — that is
  the whole reason the wait is there.
* **What it must not break:** the refresh path (`Count("texture: uploaded")`'s neighbour at
  the re-upload site) overwrites an image the in-flight frame may still be sampling, and it
  is the one caller that genuinely needs prior work retired. It keeps its wait.
* **Pre-registered kill: below 40 ms off the worst frame of the route, do not ship it** — it
  is a hitch item, worth 0.020 ms/frame amortised, so a small win does not justify touching
  the upload path.
* Picture gate + the operator's look before shipping: it changes when pixels arrive.

### Step 4 — THE GUEST SIDE (only if step 2 says the residual dominates)

Frame 8725 is the frame **immediately after** a 692-upload burst, which points at the
guest's own streaming — file I/O, decompression, or the PM4 walk being starved — rather than
at anything the renderer does. The tools already exist and none is new code:

* `CZ_FILE_TRACE=1` — every open and read, including the not-founds;
* `CZ_GUEST_DIAG=1` (+ `CZ_GUEST_LOG=1`) — takes the outdoor route from **0 `[guest]` lines
  of 11,168 to 1,239** and is what makes the streaming and zone-LOD decisions readable. **A
  diagnostic arm only**: 2,013 formatting sites on the frame path, so never quote a frame
  time from a run carrying it;
* `perf record` against the DWARF line table, which reads guest AND host symbols and has
  disagreed with thirty parts of phase splitting before (`perf-symbol-profile-before-phase-profile`).

**Warning that belongs here rather than in a note:** a hitch is a rare event and `perf` over
a whole run gives an aggregate. Correlate against the slow-frame table's frame numbers, or
the profile will describe the 99% of frames that are fine.

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
