# Performance plan, part 81 — the driver's own call count

> **THIS IS THE LIVE PLAN.** There has not been one since part 73 exhausted
> `perf-plan-autonomous.md`; the three parts since worked straight off a kickoff board,
> because no item was concrete enough to plan. This one is, and it is the first plan in this
> project whose top item is **buildable, threadless, and incapable of changing a pixel**.
>
> Read `part81-kickoff.md` first for the board and the ruled-out list. Read
> `phase5-notes.md` §6eb, §6ec, §6ed for how part 80 got here.

---

## §0 THE STATE THIS PLAN IS BUILT ON — four numbers, all measured in part 80

1. **The route is autonomous.** `tools/part80_crowdroute.sh` replays the operator's own route
   at **9,300-9,700 draws** and reproduces their regime band for band. Every item below is
   measurable without them.
2. **The frame is CPU-bound with 2.31-3.06 ms of headroom and the fence is 0.00** at every band
   from 5,000 draws up. A CPU saving converts to frame time **roughly 1:1** up to that
   headroom. A GPU saving converts to **nothing**.
3. **The noise floor is ±2.9%** in the decisive 9,000-9,500 band, mixed sign, three runs
   pooled. At a 13 ms frame that is **±0.38 ms** — so an item worth less than ~0.4 ms cannot
   be measured on this route at all, and saying so up front is what stops a session
   manufacturing a result out of one run.
4. **The driver's call chain is 251 ns a draw = 2.33 ms a frame**, decomposed in §6ed to
   within 3% by two independent methods.

**The consequence, stated so it is not re-litigated:** the only CPU items worth a session are
ones whose ceiling is above ~0.4 ms, and after part 80 there are exactly three places left
that clear it.

## §1 ITEM 0 — THE DRIVER'S CALL COUNT. Three steps, in this order.

### §1.0 STEP 0 — THE CENSUS, AND IT DECIDES WHETHER STEP 2 EXISTS AT ALL

`BindVertexBufferCached` issues **one `vkCmdBindVertexBuffers` per binding**, and the API takes
a **contiguous range**. The bind loop assigns `binding` with `++binding` as it walks the
shader's attributes, so the bindings themselves are contiguous by construction. What is NOT
known is whether the **changed** ones are, and the two possibilities differ by the whole item:

```
bindings offered per draw : 3.310
bindings CHANGED per draw : 1.725   (52.1% of offered)

Hypothesis A — ALL-OR-NOTHING. A draw either reuses the previous draw's mesh or replaces it
whole, so ~52% of draws change all 3.31 bindings and ~48% change none.
    batched calls/draw = 0.521    saving 1.204/draw = 63 ns = 0.58 ms/frame   ITEM LIVES

Hypothesis B — SCATTERED. Changed bindings interleave with unchanged ones, so a run of changed
bindings is usually one binding long.
    batched calls/draw -> 1.725   saving -> 0.00 ms                            ITEM DIES
```

**Do not write the batcher before this number exists.** Part 80 refuted three items with a
census apiece and each cost one run; part 79 shipped a half-fix because the arithmetic was
skipped (gotcha 470).

`CZ_VK_BIND_RUN_CENSUS=1`, one plain global compare on the bind path in the shape
`g_fetchMemoCensus` already uses:

* per draw, the number of bindings offered, the number changed, and **the number of contiguous
  RUNS of changed bindings** — that last is the whole answer, and `batched calls/draw` is its
  mean;
* a histogram of run length, because a mean of 1.2 is consistent with both hypotheses and this
  project has been caught by a mean standing in for a distribution three times
  (gotchas 428, 434, and the pass histogram of `perf-plan-autonomous.md` §4);
* the count of draws using an **untracked** binding (`>= kMaxTrackedBindings`), which is always
  issued and can never be batched — small by expectation, but an unbudgeted exception is how a
  ceiling becomes wrong.

**Kill: if `batched calls/draw` is above 1.30, step 2 is dead** — the saving is under 0.20 ms
and below half the noise floor. Say so and move to §2.

### §1.1 STEP 1 — BYPASS THE LOADER TRAMPOLINE. Mechanical, and it is not conditional on the census.

Every `vkCmd*` here is the Vulkan **loader's exported symbol**: a trampoline that reads the
dispatch table out of the dispatchable handle and jumps through it. Resolving the device-level
commands once through `vkGetDeviceProcAddr` and calling stored pointers removes one indirection
from **every one of the 4.83 calls a draw**.

* **Ceiling 0.12-0.35 ms/frame** (5-15% of call overhead, the usual range for this change).
  It may land under the floor; §1.3 says what to do then, in advance.
* **Scope: the record path first** — `vkCmdBindPipeline`, `SetViewport`, `SetScissor`,
  `SetBlendConstants`, `SetStencil{Reference,CompareMask,WriteMask}`, `BindDescriptorSets`,
  `PushConstants`, `BindVertexBuffers`, `BindIndexBuffer`, `DrawIndexed`, `Draw`. Those are
  99.9% of the calls. The resolve/barrier/copy paths are a handful a frame and can follow or
  not.
* **The one failure mode, and it must be loud.** `vkGetDeviceProcAddr` returns `nullptr` for a
  name the device does not support or that is misspelled, and a null function pointer is a
  crash with no message. **Resolve them all in one place, count them, print the count, and
  abort on any null with the name in the message.** A silent fallback to the loader symbol is
  worse than a crash: it would make the arm partially engaged and the measurement a blend of
  two configurations (gotcha 151).
* **Control arm `CZ_VK_NO_DEVICE_PFN=1`** — use the loader's exported symbols, i.e. the code as
  it is today. Both arms print which they are using, because *an arm with no counter cannot be
  shown to have engaged* (gotcha 151) and *assert that an arm engaged* is already a memory of
  this project's.
* **It cannot change behaviour**: same functions, same arguments, one less indirection. So the
  picture gate is a formality here — run it anyway, because "cannot change behaviour" is an
  argument and this project's standard is a measurement.

### §1.2 STEP 2 — BATCH THE VERTEX BINDS. Only if step 0 says hypothesis A.

Accumulate the draw's binds into a small per-draw array and flush **contiguous runs of changed
bindings**, one `vkCmdBindVertexBuffers` per run.

* **Keep the per-binding cache comparison.** The 47.9% elision is real and the batch must not
  buy its call reduction by giving that back. Binding the whole range unconditionally is the
  simpler code and the wrong trade — it would issue 3.31 bindings' worth of driver work on
  every draw that changed one.
* **The untracked path (`binding >= kMaxTrackedBindings`) stays exactly as it is** — always
  issued, never assumed unchanged, never batched. It is the safety valve and the census counts
  it.
* **Flush before the draw, and only there.** The array is per draw and dies with it.
* **Control arm `CZ_VK_NO_BIND_BATCH=1`** — one call per binding, the code as it is today.
* **The verifier, because no existing gate covers bind ORDER or bind CONTENT.** The order gate
  (`CZ_VK_ORDER_GATE=1`) hashes pipeline and vertex RANGE, not which buffer landed in which
  binding — so a batcher that got the `firstBinding` arithmetic wrong would hand a draw another
  stream in the right-looking slot, and the order gate would pass. `CZ_VK_VERIFY_BIND_BATCH=1`
  records the (binding, buffer, offset) triples the batched path issues, replays the same draw
  through the unbatched path into a scratch list, and counts disagreements. **And
  `_POISON=1` must make it read 100%**, because a checker that has never failed has not been
  shown capable of failing (gotcha 30).

### §1.3 HOW IT IS MEASURED, AND THE KILL, BOTH PRE-REGISTERED

**Three configurations, three runs each, alternated, one binary**, on
`tools/part80_crowdroute.sh` with the resolution pinned:

| arm | what it is |
|---|---|
| `both` | steps 1 and 2 on — the shipping candidate |
| `no-pfn` | `CZ_VK_NO_DEVICE_PFN=1` — attributes step 1 |
| `no-batch` | `CZ_VK_NO_BIND_BATCH=1` — attributes step 2 |

Each item is compared **against its own control arm in the same block**, never against a
number from a previous session (that is a memory of this project's and it has cost it once).
Read with `tools/part80_trace_band.py`, banded, medians, and quote:

* the **frame-weighted delta and whether it is monotone across bands** — a real change moves
  every band the same way, and a mixed sign is noise or composition;
* the **`record` ns/draw** from a `CZ_VK_PROFILE` pair, which is a far tighter statistic than
  frame time (part 78's barrier classes reproduced to 0.001 ms/frame where the frame time
  needed three runs a side) and is the DIRECT evidence — the frame time is the consequence;
* the **calls-per-draw counter**, which is the mechanism and cannot be argued with.

**PRE-REGISTERED KILL: combined, below 0.30 ms at the crowd load across three runs an arm, do
not ship either step.** That is below the noise floor and the honest report is a null.

**And the exception, stated now rather than argued for afterwards:** if step 1 measures below
the floor but the calls-per-draw and `record` ns/draw both move in the right direction, **keep
it and report it as a NULL, not as a saving.** It is strictly less work for identical
behaviour, and the reason to keep it is code, not performance. Writing that down in advance is
what stops a sub-floor number being quoted as a win later (gotcha 465's neighbour).

### §1.4 WHAT NOT TO START — `vkCmdPushConstants`, 1.00/draw, 0.48 ms

It looks like the second-biggest item and it is probably structural. The pushed value is the
three constant-window addresses plus a draw index, and **the constant memo serves the VS window
on only 2.9% of draws** — so the vertex window is genuinely re-allocated, at a genuinely
different address, on ~97% of draws. There is nothing to elide.

If someone wants to try anyway, the cheap first move is a counter: **how often do the three
addresses repeat between consecutive draws?** That counter does not exist, and 2.9% is a strong
prior that the answer is "almost never". One run decides it.

## §2 ITEM 1 — WHERE IS THE GUARD'S 86.2 MB A FRAME CHARGED?

`[vkprof]` reports **`guard read 86.21 MB/frame`** while the prehash pool reports **96.2%
served, 27.2 MB/frame moved off the pump**. If 59 MB a frame were still read on the pump it
would be milliseconds, and **no profiler phase shows it**: `streams` reads 0.1-0.2% and
`record`'s GUARD column is 10 ns a draw.

Exactly one of three things is true and they want different work:

1. the pump reads far less than the subtraction suggests (the two counters measure different
   populations — requests vs bytes);
2. the reading is real and charged to a scope nobody has attributed it to;
3. the counter means something other than its name.

**This is the largest number in the frame that has never been placed**, and §6ec's whole
argument is that the guards ARE the remaining cost — so it is the number that decides whether
that argument has an item behind it. One unconditional clock over the guard's read, split by
which thread ran it, in the shape §6eb's ceiling probe used. One run.

## §3 ITEM 2 — THE LAST UNEXPLAINED HITCH CLASS

Carried unchanged from `part80-kickoff.md`. Two spikes in the operator's session 1 (50.0 ms at
t=77.4 s, 60.3 ms at t=93.2 s) that were **not** growths and were **not** felt. Session 3 saw
none but ran 88.5 s against the 96.8 s in which they appeared, so absence there is weak
evidence (§6ea §3).

Needs an **always-on, coarse split of the pump's walk** — the property that matters is that it
can be left on in every session, because the events are two a session and nobody can predict
when. `CZ_VK_PROFILE` cannot do it: ~807 ns a draw and it inverts the regime.

Their threshold is a useful bound while designing: they felt 87, 158 and 352 ms and did not
notice 50 or 60, in a crowd.

## §4 THE ORDER, AND WHY

1. **§1.0 the bind census** — free, one run, and it decides whether §1.2 exists.
2. **§1.1 the trampoline bypass** — mechanical, independent of the census, and the lowest-risk
   change on the board.
3. **§1.2 the bind batch**, if the census says hypothesis A.
4. **§1.3 the three-arm campaign**, once, measuring both steps together and attributing each.
5. **§2 the guard census** — one run, and it either names the next item or closes the argument
   that the guards are what is left.
6. **§3** only if there is time; it is an instrument, not a saving, and its value is to a
   future operator session rather than to this one.

**Do not reorder 1 before 0.** Every item part 80 killed was killed by a census that cost one
run, and the one time this project skipped that step it shipped a fix it had to replace.

---

## §5 EXECUTION RECORD — what part 81 did with this plan

Filed in the plan itself rather than only in the notes, so a reader who opens the plan sees
what happened to each item without having to find the section that corrected it (gotcha:
*retract where the claim is*).

* **§1.0 the census — RUN, and it says HYPOTHESIS A.** `CZ_VK_BIND_RUN_CENSUS=1` over
  118,515,047 draws on `part80_crowdroute.sh` (peak 9,622): offered 3.292/draw, changed
  1.742/draw, **runs of changed 0.468/draw**, mean run 3.72 bindings, **untracked
  0.000/draw**. Against a pre-registered kill of 1.30, so **step 2 lives**, and the census's
  own reconstruction of part 80's independently-derived 3.310/1.725 is within 1%.
  `phase5-notes.md` §6ee §1.
* **§1.1 the trampoline bypass — BUILT.** Thirteen record-path commands through
  `vkGetDeviceProcAddr`; `nm -u` shows all thirteen gone from the undefined-symbol list,
  which is a stronger engagement check than any counter. `CZ_VK_NO_DEVICE_PFN=1` is the
  control arm and both arms print which they are. §6ee §2.
* **§1.2 the batch — BUILT AND VERIFIED.** 0.464 and 0.474 calls a draw in two runs against
  the census's predicted 0.468; `CZ_VK_VERIFY_BIND_BATCH=1` read **0 disagreements of 173.8 M
  and 161.7 M triples**, and its poison arm was designed to fire on every check rather than
  only on multi-binding runs, because 22% of runs are one binding long. §6ee §3, §6ee §4.
