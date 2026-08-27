# Part 81 kickoff — the board after part 80 killed its own item 1 and made the route autonomous

> **SUPERSEDED BY `docs/part82-kickoff.md`.** ~~THIS IS THE LIVE HAND-OFF, superseding
> `part80-kickoff.md`.~~ Part 81 executed this file's item 0 and closed its item 1, then the
> operator parked performance. **Item 1 below — the guard's 86.2 MB — is CLOSED: the pump
> reads 1.11 MB/frame, 0.077 ms/frame, and the 59 MB was a subtraction between two counters
> that were never a pair (`phase5-notes.md` §6ee §6, gotcha 481).** The rest of the board
> stands and is the starting point if performance resumes.
>
> **Read `phase5-notes.md` §6eb and §6ec, in that order.** They are part 80 end to end: the
> operator's crowd route and what it took to transcribe it, item 1's measured ceiling, and
> the three censuses that followed.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6ed** | **the 4.83 driver calls decomposed, and the implicit layer measured as a NULL — this is where item 0 comes from** |
> | **`phase5-notes.md` §6ec** | **the corrected per-draw CPU decomposition, three refutations, and why this renderer resists memoisation** |
> | **`phase5-notes.md` §6eb** | **the crowd route, its regime, its noise floor, and item 1's 251 ns/draw ceiling** |
> | `phase5-notes.md` §6ea §2 | part 79's owed verdict, COLLECTED — retracted in place |
> | `phase5-notes.md` §6dv §2, §6ea §4 | the operator's own regime tables, which §6eb §2 now reproduces |
>
> Lessons: gotchas **473-480**.
>
> **AND THERE IS A LIVE PLAN AGAIN: `docs/perf-plan-part81.md`.** There has not been one since
> part 73 exhausted `perf-plan-autonomous.md`, because no item was concrete enough to plan.
> Item 0 below is: buildable, threadless, and incapable of changing a pixel. **The plan is the
> execution order, the arms, the verifiers and the pre-registered kills; this file is the
> board.** Read the plan's §1.0 before writing any code — it is a census that decides whether
> half the item exists.

---

## 0. WHAT PART 80 DID, IN ONE PARAGRAPH

It collected the sentence part 79 owed — **"Nothing — felt smooth"**, closing the
stream-store hitch class on both channels. Then it took the board's item 1, **parallel
command recording**, and killed it with two runs: `CZ_VK_NO_DRIVER_RECORD=1` measured the
driver's own share of the record path at **251 ns a draw**, which is **2.33 ms a frame** at
the operator's load and a **1.56 ms ceiling with three workers** — against a pre-registered
1.5 ms kill, before any capture, re-establishment or scheduling cost, and with the thread
budget granting a `record` pool **zero threads**. Three censuses then chased the lead that
refutation left and all three refuted their own item. **And along the way the operator made
the route autonomous**: their own 9,300-draw route now replays unattended, which is what the
last four parts have each said they needed.

## 0b. THE FOUR THINGS PART 80 WOULD TELL YOU BEFORE YOU START

**One. THE ROUTE PROBLEM IS SOLVED. USE `tools/part80_crowdroute.sh`.** It replays the
operator's own route and holds **9,300-9,700 draws**, against `autoroute.sh`'s ~6,200. Its
regime matches theirs band for band (§6eb §2), and its noise floor is **±2.9% in the decisive
band** with three runs pooled — measured, not assumed. `autoroute.sh` is not retired: it is a
different, lighter journey and several recorded numbers are on it. But **no CPU item should
ever be measured on `autoroute.sh` again.**

**Two. GPU ITEMS ARE WORTH ZERO AT THIS LOAD, AND THAT KILLS TWO BOARD ITEMS WITHOUT A RUN.**
The fence is **0.00 at every band from 5,000 draws up** and the frame is CPU-bound with
2.31 ms (mine) to 3.06 ms (theirs) of headroom. A GPU saving converts to frame time **only
after the CPU has fallen by that headroom**. `part80-kickoff.md`'s items 4 and 5 — the
resolve clears (0.568 ms) and the resolve copies (0.699 ms) — are both GPU items, and
together they are less than the headroom. **They cannot move the frame today.** They become
live the moment a CPU saving of ~2.5 ms lands, and not before. Do not spend a session on
them first.

**Three. THE PROFILER'S BILL IS ~807 ns A DRAW AT THIS LOAD, so every phase SHARE this
project has quoted at a crowd is distorted.** 18.6 scopes a draw at ~21.7 ns a clock read;
the frame reads 20.0 ms instrumented against ~12.9 ms not, and 807 x 9,466 = 7.6 ms accounts
for the difference. **Read the sub-scopes, which exclude their own reads. Do not read the
percentages.** `record`'s 113 ns/draw residual and `other`'s 212 ns residual are clock reads,
not work.

**Four. A CHANGE DETECTOR CANNOT BE MEMOISED ON THE THINGS IT IS WATCHING.** Three items died
on this in one session (§6ec §5). The per-draw CPU cost here is dominated by change
detection, not computation: 89,524 stream lookups a frame, a guard reading 86.2 MB, a texture
fetch walk that exists to drive `UploadTexture`'s guard, and a state cache already eliding
**descriptor-sets 100%, blend 100%, viewport 99.4%, scissor 99.3%, pipeline 70%**. Before
designing any "remember the answer" item, ask what guard the work you are skipping was
driving.

## 1. THE BOARD, IN ORDER

The corrected per-draw CPU decomposition at ~9,500 draws (§6ec §1), which is what ranks it:

| phase | real ns/draw | ms/frame | |
|---|---|---|---|
| **record** | **524** | **4.9** | driver 251, ours 273 |
| **other** | **323** | **3.1** | fetch 115, tail 53, pipeline 48, begin 45, shader 31, key 31 |
| PM4 walk | — | ~2.0 | ~131,000 packets/frame |
| textures | ~167 | 1.6 | |
| constants | ~161 | 1.5 | memo already serving |

**There is no single large CPU item left, and that is the headline.** The largest mechanism
in the frame is the Vulkan driver itself.

### ITEM 0 — THE DRIVER CALLS THEMSELVES. Decomposed in §6ed, and the top one is a SHAPE defect.

§6eb refuted *distributing* the 251 ns a draw across threads. §6ed asks the other question —
**how many of those calls need to exist** — and the answer is concrete, because the counters
were already on the stats line:

| call | per draw | ms/frame @9,300 |
|---|---|---|
| **`vkCmdBindVertexBuffers`** | **1.725** | **0.83** |
| `vkCmdDrawIndexed` / `Draw` | 1.000 | 0.48 |
| `vkCmdPushConstants` | 1.000 | 0.48 |
| `vkCmdBindIndexBuffer` | 0.640 | 0.31 |
| `vkCmdBindPipeline` | 0.280 | 0.14 |

Reconstruction 4.691 calls / 2.27 ms against the independently measured 4.83 / 2.33 — **within
3%**, which is what makes it a decomposition rather than a total shared out.

**(a) Batch the vertex binds — ~0.35 ms/frame.** `BindVertexBufferCached` issues **one call per
binding** where `vkCmdBindVertexBuffers` takes a contiguous range, and the bind loop assigns
`binding` with `++binding` as it walks the attributes, so **they are already contiguous**.
Census the run structure of CHANGED bindings first: binding the whole run unconditionally is
simplest but gives back some of the 47.9% per-binding elision; batching runs of changed
bindings keeps it.

**(b) Bypass the loader trampoline — 0.12-0.35 ms/frame, and it is the lowest-risk change on
this board.** This runtime calls the loader's *exported* `vkCmdDrawIndexed` and friends, which
fetch the dispatch table from the handle and jump through it. Resolving device-level commands
once via `vkGetDeviceProcAddr` and calling through stored pointers removes one indirection from
**every** call. Mechanical; cannot change behaviour; `CZ_VK_NO_DRIVER_RECORD` is already its
denominator.

**(c) `vkCmdPushConstants` is 0.48 ms and is probably STRUCTURAL — do not start here.** The
pushed value is the three constant-window addresses plus a draw index, and the constant memo
serves the VS window on only **2.9%** of draws, so that address genuinely differs nearly every
draw. Measure how often the three addresses repeat before believing otherwise; that counter
does not exist and 2.9% is a strong prior that this is dead.

Together (a) and (b) are **~0.5-0.7 ms** at their load, converting roughly 1:1 (fence 0.00,
2.3-3.1 ms headroom) — more than the parallel recorder could ever have delivered, with no
threads and no way to change a pixel.

### ITEM 1 — WHERE IS THE GUARD'S 86.2 MB A FRAME CHARGED? A census, and it is the one unexamined large number.

`[vkprof]` reports **`guard read 86.21 MB/frame`** while the prehash pool reports **96.2%
served, 27.2 MB/frame moved off the pump**. Those two numbers do not obviously reconcile: if
59 MB a frame were still being read on the pump it would be milliseconds, and no profiler
phase shows it — `streams` reads 0.1-0.2% and `record`'s GUARD column is 10 ns a draw.

So one of three things is true, and they want different work: the pump reads far less than
the subtraction suggests; or the reading is charged to a scope nobody has attributed it to;
or the counter means something other than what its name says. **Ask before doing anything
else on the CPU** — it is the largest number in the frame that has never been placed, and
§6ec's whole argument is that the guards ARE the remaining cost.

Cheap: one clock, one run, in the shape §6eb's ceiling probe used.

### ITEM 2 — THE LAST UNEXPLAINED HITCH CLASS. Unchanged from part 80's board, still cheap, still needs one instrument.

Carried verbatim: session 1 had two spikes that were **not** growths and were **not** felt —
50.0 ms at t=77.4 s and 60.3 ms at t=93.2 s, both far from any load, both with the
isolated-single-frame signature. Session 3 showed none but ran 88.5 s against the 96.8 s in
which they appeared, so **absence there is weak evidence** (§6ea §3).

The trace cannot answer it: the phase columns are `ProfScope`s and read zero without
`CZ_VK_PROFILE`, which now costs ~807 ns a draw and inverts the regime. This needs an
**always-on, coarse split of the pump's walk** — cheap enough to leave on in every session,
which is the property that matters, because the events are two a session and nobody can
predict when.

Their threshold is a useful bound: they felt 87, 158 and 352 ms and did not notice 50 or 60,
in a crowd.

### ITEM 3 — THE MAXIMAL PARALLEL DESIGN. Unpriced, high risk, and the only CPU item that could be large.

§6eb §3c: real `record` is ~513 ns a draw, so moving the WHOLE phase has a 3-worker ceiling
of **3.2 ms**, which clears comfortably where the low-risk design does not. It needs the
per-draw register file captured and `UploadStream` made re-entrant against a shared stream
store — and a per-worker arena would change which buffer a stream lands in, defeating the
vertex/index bind cache the cross-frame store depends on.

**And it still needs threads that do not exist.** `ThreadBudget_Take` is first-come-first
-served, the whole budget is 3 on an 8-physical-core box, and the guard pool takes all 3.
`cpu/thread_budget.h` says in as many words *"Revisit when the second pool lands"* — this is
that moment, and the decision is the operator's, because part 53 measured the guard pool's
value and taking its threads is a trade, not a free choice.

**Do item 1 first**: if the guard's real cost is smaller than its counter suggests, the guard
pool may be affordable to shrink, and that unblocks this. If it is larger, this design gets
worse rather than better.

### ITEM 4 — THE UNTILE LOOP. Load frame only, and the arithmetic still has not been done.

Carried from part 80's board unchanged. At the operator's load the texture path's untile
loops are **113.3 + 64.0 = 177.3 ms, 57.7% of the decode** (§6dy §5). But it reaches only the
LOAD frame: their biggest is 111.2 ms carrying 62.0 ms of texture work, of which the untile
is ~58%, so a 3x buys ~24 ms of a 111 ms frame against a standing 40 ms kill. The
decomposition is proven — `Tiled2DOffset` separates into a 32x32 table plus a macro-tile
base, **967,680 combinations checked, 0 mismatches** (`tools/tile_offset_separable.py`).
**Do the arithmetic before the work.**

## 2. WHAT IS RULED OUT — do not start these

* **PARALLEL COMMAND RECORDING, the low-risk design** (capture the driver calls, replay them
  in secondaries). Measured ceiling **251 ns a draw = 2.33 ms**, so 1.56 ms with three
  workers and **0.00 ms with the thread budget as it stands** — against its own 1.5 ms kill,
  before capture, re-establishment or scheduling. §6eb §3. The maximal design is item 3 and
  is a different thing.
* **a vertex-fetch decode memo**, on either key. 41.2% on a whole-file stamp, 53.8% on an
  exact per-attribute hash worth 0.621 ms as a CEILING — and it fails on CORRECTNESS anyway,
  because the loop it skips is where the stream content guard runs (§6ec §2, §6ec §3). That
  is part 24's HUD defect approached from the other side.
* **a per-draw stream-lookup dedup.** 47.9% of lookups repeat within one draw, and a repeat
  returns before the guard, so it is worth **~0.27 ms** — below this route's floor (§6ec §4).
* **the resolve clears and the resolve copies**, and every other GPU item, **until a CPU
  saving of ~2.5 ms has landed.** fence 0.00. See §0b point two.
* **`CZ_VK_SCOPED_CLEAR` as the clear fix** — it spends pump time to buy GPU time, and at
  this load pump time is worth strictly more. Do not re-derive it.
* **the post chain** (§6dx, §6dy §4), **the texture upload submit path** (§6ds, §6dw),
  **growing the stream store more gracefully** (it cannot grow), **pipeline compilation on
  the load frame** (§6du §5), **the readback** (§6dq), **the constant path** (§6dp), **the
  guest side** (§6dm), and everything on part 73's exhausted list.
* **the implicit Vulkan layers.** `VK_LAYER_LS_frame_generation` has been inserted as a DEVICE
  layer in every run this project has ever made (installed 2026-05-09, three months before the
  port began, and enabled by default) — and it is a **NULL**: `record` reads 637/645 ns/draw
  with it and 639/639 with `DISABLE_LSFG=1`, where the two default runs differ from each other
  by more than the arms do. It defines **no `vkCmd*` entry points at all**, so the loader's
  dispatch table holds the driver's own pointers. No past measurement is contaminated and
  frame generation was never active — and could not have inflated anything anyway, because
  this runtime counts its own presents at its own seam. §6ed §2.
* **any CPU A/B on `autoroute.sh`.** Use `tools/part80_crowdroute.sh`.

## 3. THE MEASUREMENT METHOD

* **`tools/part80_crowdroute.sh <tag> [ENV=VAL …]`** — the operator's route, unattended,
  9,300-9,700 draws, resolution pinned, gate at 8,000 draws with a failed run renamed
  `.rejected` so no glob can pick it up. `SOAK=N` for a longer stationary soak.
* **`tools/part80_trace_band.py A=<glob> B=<glob>`** — the reader. Pools frame traces, bands
  by draw count, prints GPU and fence alongside so the same table says whether a saving could
  convert. **Band, never match frame indices**: the route does not land on the same spot
  twice and the operator diagnosed why — *"the random zombie spawn placing zombies on the
  way"*. Floor: **±2.9%** in the decisive band, mixed sign.
* **`tools/part76_regime.py <trace>`** — CPU-bound or GPU-bound, per draw band. Read it
  before pricing anything (§0b point two).
* **`CZ_VK_NO_DRIVER_RECORD=1`** — the ceiling probe. Destructive; read `record` ns/draw and
  nothing else.
* **`tools/part80_transcribe_route.py <log>`** — turn a `CZ_INPUT_TRACE` recording into a
  recipe. Read §6eb §1b before trusting one: three separate wrong answers came out of it,
  all the same shape — **a trace records CHANGES, so an input's extent is a property of its
  SUCCESSOR**.
* **`tools/part79_picture_gate.sh`** with a `STILL=1` pair (gotcha 465), and
  `CZ_VK_SYNC_VALIDATION=1` for anything touching a barrier.

## 3b. THE GATES PART 80 RAN, so part 81 knows what it inherits

Part 80 touched the **command processor** (`pm4.cpp` gained a fetch-constant version stamp),
so the two PM4 boundary oracles were run rather than assumed:

* `tools/pm4_packet_lengths.py` on B1 — **24,527,474 lengths agreeing, 0 disagreeing**, exit 0
* `tools/pm4_indirect_walks.py` on B1 — exit 0
* `CZ_PM4_VERIFY_BULK_REGS=1` over a boot — **0 bulk register mismatches** (the bulk path is
  where the new stamp's second bump lives)
* `--smoke` OK; `shader_dim_census.py` clean on the stock AND the play cache; all six live
  caches at 449; `rt_world_xform_census.py` exit 0 with no change to its config
* every reportable route run passed its own 8,000-draw gate, failures renamed `.rejected`
* the new censuses **shown free when off**: +0.4% frame-weighted, mixed sign, against the
  three pre-census nulls

Part 74's A5 and `alu_const_gate --hlsl-dir` sweeps, part 75's cache gates and part 78's
barrier gates were **not** re-run and do not need to be: nothing shipped and no shader,
texture, barrier or constant path changed.

## 4. WHAT IS OWED

**Nothing.** Part 79's one sentence was collected in part 80's first message and §6ea §2
carries the retraction in place.

The operator has not seen a build from part 80, and does not need to: nothing shipped. Every
change in it is an instrument, a census or a route, all off by default and shown free when
off (+0.4% frame-weighted with mixed sign against the pre-census nulls).
