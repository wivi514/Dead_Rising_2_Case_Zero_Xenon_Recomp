# Perf plan, part 111 — BUILD ITEM B: move the per-draw work to the idle cores

**The instruction (2026-09-10, after part 110 closed):** the operator was shown that item B
tops out at **~113 fps and cannot reach the 120 they asked for**, and said *"prepare the
plan to build that"*. So the ceiling is accepted, not overlooked, and this plan's job is to
capture as much of the **~2.3 ms** as survives contact with the code.

**Read `docs/perf-plan-part110.md` §6-§7 first.** Nothing in it is re-derived here.

---

## §0. What is already established, and must not be re-measured

| | |
|---|---|
| the pump's serial floor `F` | **2.49 ms** (`CZ_VK_NO_DODRAW=1`, six alternated runs, monotone in six bands) |
| the movable per-draw work `M` | **8.44 ms** |
| the ideal with 3 workers, `F + M/3` | **5.33 ms** — the part-110 kill (8.0) did not fire |
| the wall's OTHER floor | **~8.8 ms**, the guest's own code; not our pump tick (null at 25 µs and 10 µs) |
| what item B is therefore worth | **~2.3 ms, ~90 → ~113 fps** |

`wall ~ max(pump, 8.8 ms, GPU)`. Item B moves only the first term, and once the pump drops
below ~8.8 ms **every further millisecond of pump time is worth nothing**. That is a
budget, and it is the single most important sentence in this plan:

> **Item B has ~2.1 ms of USEFUL headroom (10.93 → 8.8), not 5.6 ms.** A stage that lands
> the pump at 8.8 ms has extracted the whole available win, and anything after it is
> unpaid work. Stop there.

---

## §1. THE STRUCTURAL FINDING THAT RESHAPES PART 110's SKETCH — and it inverts the staging

Part 110 §3.3 sketched "the pump walks and dispatches; workers do `DoDraw`'s body", and
§3.4 staged it **constants first** (cheapest, self-contained, proves the machinery) then
streams and textures ("the hard one"). **Reading the code says that order is backwards.**

**A deferred job's SOURCE has to survive until the job runs, and the constants' source is
the one thing the pump overwrites constantly.**

* `CopyConstWindow` copies from **`g_regs`**, the live register file, into the arena. The
  pump's own walk is what writes `g_regs` — `WriteRegisterRun` is 1.07 ms/frame — so a
  constants job dispatched for draw N and run while the pump is executing packets for
  draw N+1 reads a register file that has already moved.
* And it moves nearly every draw: the const memo says the **VS window changes on 98.3% of
  draws** (1.7% served) while the PS window is 62.7% served. So "bound the lag and wait
  before the next ALU write" serialises essentially completely.
* Snapshotting the window to make it safe **is the copy** (4,096 B VS + 4,096 B PS per
  draw). Part 110 §3.3's own hazard 2 said this — "if capturing per-draw register state
  costs more than the work it moves, the item is a loss" — and the memo rates say it does.

**Streams and textures do not have this problem.** `UploadStream` copies from **guest
memory** (`base + va`), which the pump never writes; the guest may, and that risk is
already taken and already instrumented (the content guard's widened-race arm). Their
shared mutable state — the flat stream cache, the persist store, the texture table — is
**ours**, so it can be sharded, queued or made read-mostly. That is a tractable engineering
problem; the constants' source race is not.

**So the staging inverts, and there is a third, hazard-free stage that goes first.**

| stage | what moves | ms on the pump (part 109 symbols) | hazard |
|---|---|---|---|
| **B1** | the shared-block zero, **pre-zeroed ahead of the bump pointer** | `__memset_avx2` **0.44** | **none** — no source at all |
| **B2** | streams + textures | `UploadStream` 0.99 + `PersistFind` 0.42 + `UploadTextureUncached` 0.73 + `TexFind` 0.27 + `__memcmp_avx2` 0.27 = **2.68** | our own caches; shardable |
| **B3** | the constants | `CopyConstWindow` 0.38 + the inlined patch ~0.3 = **~0.7** | **the source race above; may be impossible** |

**B1 + B2 = 3.1 ms of pump symbols against 2.1 ms of useful headroom.** If both land at
even two-thirds efficiency the budget is spent and **B3 never has to be attempted** — which
is the cheapest possible answer to the hardest stage.

---

## §2. Protocol — unchanged from parts 109 and 110, and each line was paid for

* **Three runs an arm, alternated**, `tools/part80_crowdroute.sh <tag> [ENV=VAL]` (tag
  first, env after).
* **Read with `tools/part110_pumpcpu.py`, in matched draw bands.** The quantity is the
  **pump's CPU per frame**, not the wall: once the pump nears 8.8 ms the wall stops
  responding, and a stage that works will look like a null in wall time. Wall is the
  *outcome* measure and is quoted at the end, once.
* **The control is the arm run NOW.** Part 110 threw out an identity A/B whose control was
  an hour old and got a different answer alternated (§6.8).
* **Do not rebuild while a campaign is running** — the route script copies `cz_runtime` at
  each run's start.
* **Every arm proves it engaged with a counter, and its control prints the opposite**
  (gotcha 151), reported **per FPS window** as well as at exit (gotcha 543).
* **Archive the executable beside every `perf` capture** (gotcha 550).
* **`tools/phase_vs_perf.py` once per stage**, and prefer a `perf` capture from a CLEAN run
  at matched draws over one from the profiled run itself.

---

## §3. STEP 0 — the census that decides B2's design. Build nothing until it has run.

`CZ_VK_PARDRAW_CENSUS=1` — per frame, on the per-draw path, count and time. **It must ask
two questions per subsystem, not one**, because part 110 §1 shows the second is the one
that bites:

1. **what MUTATES shared state** (the part that must serialise or shard), and
2. **what READS state the pump will overwrite before a deferred job could run** (the part
   that cannot be deferred at all).

| operation | expected | question it settles |
|---|---|---|
| arena bump allocations | ~3/draw (VS 4,096 B + PS 4,096 B + shared 2,192 B) | per-worker sub-arenas, or is the bump itself cheap enough to keep serial? |
| stream-cache **finds** | ~47,000/frame (measured, part 110) | read-mostly? |
| stream-cache **inserts** | ~2,000/frame (part 109) | **the real question for B2** |
| persist-store inserts + `mirrorPending` pushes | ? | ditto |
| texture finds / inserts | ~21,400 finds/frame (part 110), ~0 inserts | trivial? |
| bindless descriptor-slot writes | ? | **the other real question** |
| pipeline-cache inserts | ~0 at the crowd | trivial |
| **reads of `g_regs` after dispatch** | the constants, confirmed; anything else? | **whether B3 exists at all** |

**The encouraging prior stands and is now measured twice**: 47,000 stream lookups a frame
against ~2,000 frame-first touches — **96% of that traffic is READS**.

**PRE-REGISTERED KILL for B2:** let `S` be the measured cost of the mutating operations. If
`F + S + (B2's movable work)/3` does not get the pump below **9.5 ms** (i.e. does not
recover at least half of B2's symbols), B2's sharding is not worth its risk and the plan
stops after B1.

---

## §4. B1 — pre-zero the arena ahead of the bump pointer

**The hazard-free stage, and it is worth building first even though it is the smallest,
because it builds and proves the dispatch machinery on a job that cannot be wrong.**

### 4.1 The design

Today every draw calls `memset(shared, 0, kSharedSize)` — 2,192 bytes into a freshly
allocated arena region, `__memset_avx2` = **0.44 ms/frame** and the 6th-largest symbol on
the pump. It has **no source**: it writes a constant into memory only this draw owns.

The arena is a **bump allocator**, so the bytes that the next allocation will return are
known before anyone asks for them. So:

* the shared blocks move to their **own contiguous sub-arena**, so pre-zeroing touches only
  bytes that need zeroing (a blanket pre-zero of the whole arena would zero the 8 KB of
  VS/PS constants that are about to be fully overwritten — 93 MB/frame instead of 20);
* a worker zeroes **ahead of** the sub-arena's bump pointer, in chunks, keeping a
  watermark;
* `ArenaAllocShared()` returns already-zeroed memory when the watermark is ahead of it, and
  **falls back to an inline `memset` when it is not** — so correctness never depends on the
  prediction, which is the pattern this renderer already uses for the parallel content
  guard.

**It composes with `CZ_VK_SCOPED_SHARED_ZERO=1` and the plan must say how.** That item
(verified, −0.21 ms, currently OFF) narrows the zero to the vfetch slots a shader declares
— 657 B/draw instead of 2,192. Pre-zeroing is blanket, so with B1 on, the scoped item
becomes redundant and its bytes go back up. **That is fine and it is the point**: B1 moves
19.7 MB/frame of memset OFF the pump entirely, where the scoped item shaved 5.9 MB while
leaving it on. **The two are alternatives, not additions**, and the A/B must be a
three-configuration one — stock / scoped / pre-zeroed — or the comparison is meaningless.

### 4.2 What it proves for the stages after it

The end-to-end machinery, on a job with no correctness risk: a work queue the pump posts
to, workers taking from the budget, a **drain point**, and the accounting that says where
the bill landed. Part 53 moved 13.1 points off the pump and **33.2 points appeared on the
workers** — B1's measurement must report both sides or it is not a measurement (gotcha 344).

### 4.3 Engagement, control and kill

* **Counters:** bytes pre-zeroed, bytes zeroed inline (the fallback), watermark misses per
  frame, worker busy time. A fallback rate above a few percent means the worker is not
  keeping up and the item is half-engaged — which would otherwise read as a weak win.
* **Control:** `CZ_VK_NO_PREZERO=1`, printing the opposite counters.
* **PRE-REGISTERED KILL: if B1 recovers less than 40% of the 0.44 ms it moves (i.e. under
  0.18 ms of pump CPU, three runs an arm, matched bands), the dispatch overhead is too high
  for a job this size and the whole design is refuted cheaply.** That is the outcome worth
  paying for: B1 exists to kill B2 and B3 for one day's work if the machinery does not pay.

---

## §5. B2 — streams and textures, where the money is

**2.68 ms of pump symbols, and the stage that decides whether item B reaches its ceiling.**
Only designed in detail after §3's census; the shape and its hazards, so the census knows
what to ask:

1. **The pump keeps the decisions, the workers do the bytes.** The cache lookup, the guard
   decision and the arena allocation stay on the pump (they are the change detectors, and
   gotcha 474 says a change detector cannot be memoised away); the **copy** — `CopySwapped`
   and the texture untile — goes to a worker.
2. **The source is guest memory, not `g_regs`**, so §1's race does not apply. The existing
   widened-race exposure is unchanged in kind, and `CZ_VK_GUARD_VERIFY` already sizes it.
3. **Sharding, in increasing order of risk:** a read-mostly shared table with a small serial
   insert queue drained by the pump (96% of traffic is reads, so this is probably enough);
   per-worker shards with a frame-end merge only if the census says inserts are hot.
4. **The LRU/recency stamps are the trap.** Part 109's texture memo nearly broke the
   reclaimer by skipping the lookup that stamps `lastUsedFrame`. Any sharded or deferred
   path inherits that hazard and needs its own verifier arm.
5. **The drain point is `vkQueueSubmit`**, not each draw: the bytes must be in the arena
   before the GPU reads them, and nothing on the CPU reads them back.

**Kill:** §3's, restated — if the pump does not reach 9.5 ms, stop.

---

## §6. B3 — the constants, and the honest expectation that it does not happen

§1 says the VS window changes on 98.3% of draws and that snapshotting it is the copy. **The
plan's expectation is that B3 is refuted by §3's census and never built.** It is written
down so that a future session does not re-derive the reasoning, and so that the one thing
that could change the verdict is named:

* if a **cheap invalidation** existed — the guest writing ALU constants in bulk runs
  between draws rather than interleaved — the pump could dispatch a whole RUN of draws
  sharing one window. `CZ_PM4_ALU_WRITE_CENSUS=1` (part 87) already exists and answers it;
  **check `docs/instruments.md` for its recorded answer before running it** (part 109
  re-bought a census parts 80 and 87 had already killed).

---

## §7. Where the threads come from, and it is the operator's rule

`ThreadBudget_Take` grants **3 workers** on their 8-physical-core box (`budget 3 (reserve 2,
cap 6)`), the guard pool asks for 4 and is clamped to 3, and **0 slots are unclaimed**. So
every stage here must say where its threads come from. In order of preference:

1. **Share the guard pool.** It runs at 34% busy per worker and its work is bursty and
   front-loaded (dispatched at swap, done early in the frame). B1's and B2's jobs are
   exactly what an idle guard worker should pick up. **This is the default assumption and
   it costs no budget at all.**
2. Raise `cap`/lower `reserve` — **the operator's rule, not this plan's** (*"leave core
   empty for user background item"*). Do not touch it without asking.

**A stage that needs its own pool must justify it against option 1 with a measurement.**

---

## §8. Gates — every stage, not optional

* `--smoke`; A5 kernel diff **exit 0**; `truncated=0`; `no translated shader` = 0.
* `CZ_VK_VALIDATION=1` identical to the control arm.
* `CZ_VK_SYNC_VALIDATION=1` **0 hazards**, with `CZ_VK_BARRIER_POISON=1` producing **30**.
* **Picture:** `tools/frame_era_medians.py`, with a null pair measured in the same block and
  the arm inside the null.
* **Threading:** ThreadSanitizer, **or** a written race argument per shared structure. *"It
  did not crash in three runs"* is not a gate.
* `tools/phase_vs_perf.py` once per stage.
* **Both sides of the bill**: pump CPU down AND worker CPU up, reported together.

---

## §9. Order of work

1. **§3's census.** It decides B2's design and is expected to refute B3.
2. **B1** — pre-zero the arena. Small, hazard-free, and its kill rule refutes the whole
   design for one day's work if the dispatch machinery does not pay.
3. **B2** — streams and textures, only if B1 passed and the census cleared it.
4. **Stop at 8.8 ms.** Past that the wall does not move and further pump savings are unpaid.
5. **B3** only if §6's census says the constants can be dispatched in runs.

**What closes this plan:** the pump's CPU per frame at the operator's crowd, banded, with
the wall beside it — and an honest statement of which of the ~2.1 ms of useful headroom was
taken. Part 110's precedent is the standard: name the number, and stop buying items when
the budget is spent.

## §10. Execution record

*(empty — part 111 writes here)*
