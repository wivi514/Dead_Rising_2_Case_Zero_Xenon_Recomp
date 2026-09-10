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

### 10.1 — STEP 1, §3's census: RUN, AND IT CLEARS B2 WHILE REFUTING B3

`CZ_VK_PARDRAW_CENSUS=1`, one crowd run (`tools/part80_crowdroute.sh`, peak 9,398 draws,
21 windows at ≥8,000). The instrument counts the high-frequency operations and TIMES the
low-frequency ones — a `steady_clock` read is ~25 ns and the arena bump runs 2.4 times a
draw, so timing that would have added 1.4 ms to the frame and measured the instrument.

Read the per-DRAW column, not the per-frame one: the run averages over 21,791 frames
including the boot and the menus, so its denominator is 5,838 draws/frame where the crowd
windows are ~9,400. The per-draw rates are the load-independent quantity, and they
reproduce part 110's independent readings (4.94 stream lookups a draw × 9,400 = 46,400
against part 110's "~47,000 a frame").

| operation | per frame (run avg) | per draw | what it settles |
|---|---|---|---|
| arena bump | 14,010 (42.96 MB) | 2.40 | serial, ours, and cheap enough to keep on the pump |
| persist bump | 6 (0.01 MB) | — | trivial |
| **stream-cache finds** | **28,834** | 4.94 | the READ traffic |
| **stream-cache inserts** | **2,581 = 8.95%** | 0.44 | **91.05% of the traffic is reads** |
| stream-insert cost | **0.073 ms/frame** | 28 ns | the serial residue B2 cannot shard away |
| persist finds / inserts / mirror pushes | 2,581 / 6 / 32 | — | 0.002 ms/frame — trivial |
| **texture finds** | **10,376** | 1.78 | **every one WRITES `lastUsedFrame`** |
| texture inserts | 0.1 | — | trivial |
| **descriptor-set writes** | **0** | 0 | **B2's workers need no Vulkan calls at all** |
| pipeline-cache finds / inserts | 1,731 / 0.08 | — | trivial |
| **`g_regs` const-window copies** | **7,858** | **1.35** | **B3's source race, counted** |
| `g_regs` fetch walks | 5,838 | 1.00 | the decisions stay on the pump |

**`S` — the timed mutations that must stay serial or shard — is 0.075 ms/frame** at this
denominator, ~0.12 ms at the crowd. **The §3 kill does not fire and is not close:**
`F + S + M2/3` = 2.49 + 0.12 + 0.89 = **3.5 ms** against a 9.5 ms bar.

Four things the census decided that the plan had listed as open:

* **B2's tables can be read-mostly.** 91.05% of shared-table traffic is reads, and the
  whole mutating half costs 0.075 ms/frame. A small serial insert queue drained by the
  pump is enough; per-worker shards with a frame-end merge are not needed and should not
  be built.
* **The workers make no Vulkan calls.** Descriptor writes are **0 per frame** at the
  crowd — they happen only on a texture upload or a first-sight sampler, both of which
  are misses that do not occur in a warm crowd. So `vkUpdateDescriptorSets`'s external
  synchronisation requirement, which §3 called "the other real question", is not a
  constraint on B2's design at all.
* **The texture table's LRU stamp is real and now has a number: 10,376 read-modify-writes
  a frame.** §5's item 4 named this as the trap (part 109's texture memo nearly broke the
  reclaimer by skipping the lookup that stamps `lastUsedFrame`). It is not a barrier — the
  store is a single monotonic `uint64` of the same value from every writer — but a sharded
  or deferred texture path must keep it and needs its own verifier arm.
* **B3 IS REFUTED, as §6 expected, and by a count rather than an argument.** There are
  **1.35 `g_regs` const-window copies per draw**, i.e. essentially every draw re-reads the
  live register file that the pump's own walk is rewriting. `CZ_PM4_ALU_WRITE_CENSUS`'s
  recorded answer (checked rather than re-run, per §6) says the same thing from the other
  side: constant uploads are whole-span bursts interleaved with draws, not bulk runs
  between them. **B3 will not be built.**


### 10.2 — B1: BUILT, FULLY ENGAGED, AND ITS PRE-REGISTERED KILL FIRES AT +0.00 ms

**The item works exactly as designed and recovers nothing.** Three configurations, three
runs an arm, alternated, read as the pump's CPU per frame in matched draw bands
(`tools/part110_pumpcpu.py`, 65-66 crowd windows an arm, 6 shared bands):

| arm | how it is proved engaged | pump cpu vs stock |
|---|---|---|
| **stock** (`CZ_VK_NO_PREZERO=1`) | `[sharedzero] 0 bytes/draw not written`, no `[prezero]` line at all | — (10.88 ms) |
| **scoped** (`CZ_VK_SCOPED_SHARED_ZERO=1`) | `[sharedzero] 1,530 bytes/draw not written (69.8%)` | **−0.13 ms**, 5 of 6 bands negative |
| **pre-zeroed** (B1, default) | `[prezero] 100.0% of draws served pre-zeroed, 0 inline fallbacks, 19.1-19.7 MB/frame moved off the pump, drain 0.000 ms, 0 busy-chunk waits` | **+0.00 ms**, not monotone (−0.13 … +0.25) |

**§4.3's kill required ≥0.18 ms of pump CPU. It got 0.00. The kill fires.**

**But its stated REASON is wrong, and that matters more than the verdict.** §4.3 said a
failure would mean "the dispatch overhead is too high for a job this size". The
measurement says the opposite: the dispatch machinery is flawless. 100% of draws served,
**zero** inline fallbacks, **zero** milliseconds of drain, **zero** busy-chunk waits, over
three runs. Nothing was spent getting the work to the workers. The work simply arrived
somewhere that did not help.

**THE THREE ARMS ARE A CONTROLLED PAIR AND THEY NAME THE MECHANISM.** The scoped arm and
B1 both attack the same 2,192-byte `memset`, from opposite directions:

* **scoped** writes **70% fewer bytes**, on the **same thread** → **−0.13 ms**;
* **B1** writes the **same bytes** (25-38% more, counting the watermark margin and the
  2,304-byte stride), on **another thread** → **+0.00 ms**.

So the quantity that costs the pump is **bytes written, not which core writes them**. This
is a store-bandwidth bound, and bandwidth is a machine-wide resource that does not care
which core issues the stores — relocating them buys nothing, and the 27 MB/frame of
concurrent worker stores may even take some of it back (the +0.22 ms first pair).

**AND THE PERF PAIR SHOWS THE WORK LEAVING AND THE FRAME NOT MOVING.** Two
`tools/part109_probe.sh` captures at matched draws (9,334 / 9,340), binaries archived
(gotcha 550):

| | stock | pre-zeroed |
|---|---|---|
| `__memset_avx2` on the PUMP | **3.96% of the pump = 0.43 ms/frame** | **0.04% ≈ 0.004 ms** |
| the pump thread | 97.7% of a core | **97.5%** |
| each of the three guard workers | 30.9% of a core | **35.3%** |
| pump CPU per frame (66 windows an arm) | 10.88 ms | **10.88 ms** |

**0.43 ms of work provably left the pump, the workers provably picked it up, and the
pump's frame time did not move by a hundredth of a millisecond.** Where it went is in the
symbol table: with the pump's total unchanged and one symbol gone, every remaining symbol
grew by roughly the 4% the memset vacated — `UploadStream` 8.99 → 9.71%, `WriteRegisterRun`
10.59 → 10.93%, `ExecutePacket` 7.92 → 8.19%, `__memmove` 3.04 → 3.46%, `[unknown]`
7.54 → 8.24%. Nothing got faster; the rest of the frame expanded to fill the gap, because
the worker's stores now compete for the same memory pipe.

**This is gotcha 238 in its purest form, demonstrated rather than suspected**: a profiler
column fell to zero and the replacement cost was charged to *everything else*.

**One correction while it is fresh:** the 0.44 ms `__memset_avx2` attribution part 109
made is CORRECT — this capture reads 0.43 ms for the same symbol on the same thread, and
an intermediate guess that extrapolated a smaller ceiling from the scoped arm's −0.13 ms
was wrong. §4.3's 0.18 ms kill was a fair bar against a real 0.43 ms item. B1 did not
miss it narrowly; it recovered nothing at all. (The scoped arm recovers 0.13 rather than
the 0.30 that 69.8% of the bytes would suggest because its path makes three to eight
`memset` calls where the block made one, and the call overhead eats the difference — no
contradiction, and it is why the two arms had to be measured rather than reasoned about.)

**This is the third independent measurement pointing the same way**, and the first one
built to test it:

* part 109 (c): three of the five largest blocks on the pump are memory-bound, not compute;
* part 109's vectorised byte swap — a pure COMPUTE reduction on the same store stream —
  measured **+0.14 ms** despite engaging at 90.6% (gotcha 545);
* part 111 B1: a pure RELOCATION of the same store stream measures **+0.00 ms** despite
  engaging at 100%.

**WHAT IT PREDICTS FOR B2, and this is what B1 was built to buy.** §5's design is "the
pump keeps the decisions, the workers do the bytes" — the cache lookup, the guard decision
and the arena allocation stay on the pump; `CopySwapped` and the texture untile go to a
worker. Those are **exactly** the bandwidth-bound half: `UploadStream` 0.99 +
`__memcmp_avx2` 0.27 + `UploadTextureUncached` 0.73 = **1.99 of B2's 2.68 ms is bytes**,
and B1 has just measured what moving bytes to another core is worth. The remaining
0.69 ms (`PersistFind` 0.42 + `TexFind` 0.27) is memory **latency**, which is the class
parallelism does help — but §5's own design leaves it on the pump, because those are the
change detectors and gotcha 474 says a change detector cannot be memoised away.

**So B2 as specified is predicted to be worth ~nothing, and the plan stops here** (§9
step 3: "B2 — only if B1 passed"). Part 111 does not build it. What a later part should
ask instead is in the hand-off: the addressable class on this pump is **fewer bytes**
(the scoped item's shape) and **overlapped misses** (the guard pool's original shape),
not **the same bytes on another core**.

### 10.3 — The gates, and one of them was broken before part 111 touched anything

`--smoke` OK; `find_unlowered_switches.py` clean; `shader_dim_census.py` clean; both PM4
boundary oracles clean; `no translated shader` = 0.

**`tools/part47_gates.sh`'s E3 picture gate FAILED, and it was the gate.** It captures with
`CZ_VKDRAW=1` and no `CZ_VK_RES`, so the renderer takes its internal resolution from the
DESKTOP — 3440x1440 here — and the gate then correlates that against E3, a 16:9 photograph
of an Xbox 360 screen. The right scene at the wrong aspect reads **+0.33 to +0.48** against
the gate's own +0.70 threshold.

It was diagnosed the way this project requires rather than by the plausible story
(the control is the old binary run NOW, gotchas 50/51/86): a `git worktree` at HEAD,
built and run the same afternoon.

| | unpinned (3440x1440) | pinned (1280x720) |
|---|---|---|
| **HEAD control, 1f9098a, unmodified** | **+0.4905** best of 5, 0 agreed | — |
| the part-111 tree | +0.4831 best of 5, 0 agreed | **+0.8621 best, 4 of 5 LAYOUT AGREES** |

So the failure reproduces exactly on code that predates this part, and the same part-111
binary passes at +0.86 — inside part 49's own recorded 0.688-0.875 spread. **The gate now
pins `CZ_VK_RES` (`GATE_RES` overrides).** This is the memory note "pin the resolution in
every perf run" in a harness nobody had checked: an unpinned resolution here does not
skew a number, it convicts an innocent change, and it would have done so for every session
run on a non-16:9 desktop since the gate was written in part 47.

### 10.4 — B1's race argument, per shared structure (§8: ThreadSanitizer OR this)

*"It did not crash in three runs"* is not a gate, so here is the argument. B1 shares five
things between the pump and the guard pool's workers.

1. **The queue cursors** (`g_pzQueued` / `g_pzClaim` / `g_pzDone`). The `ParRec` idiom,
   unchanged: the pump publishes with a release store to `g_pzQueued` and every worker
   acquire-loads it before claiming, so everything the pump wrote before the post is
   visible to every claimer. Claims are a `compare_exchange_weak` on `g_pzClaim`.
2. **`g_pzBase`** (the region's mapped address, stamped so a worker never reads `R`).
   Written by the pump at dispatch, published by the same release store as (1). The
   pump's own help-drain runs BEFORE `g_pzBase` is updated and therefore uses the old
   value, which is correct — those chunks belong to the previous dispatch.
3. **The chunk states** (`g_pzChunkState[]`). This is the one that needed designing, and
   the first version was wrong. A plain `ready` flag let the pump reach a chunk first,
   memset it inline, fill its constants — and then a worker claim that same chunk and
   zero the constants underneath it. Intermittent garbage descriptor indices and cleared
   clip planes, at exactly the load where the workers fall behind and nobody is looking
   at correctness. **The fix is exclusive ownership by compare-exchange**: FREE is claimed
   by whichever of the two gets there first, and only the owner writes the bytes. A pump
   that finds BUSY *waits* rather than going inline (bounded by one 295 KB `memset`, and
   counted — `g_pzWaits` read 0 in every run).
4. **The sub-arena memory itself.** Follows from (3): a worker writes chunk `c` only after
   winning the CAS, and the pump touches a slot in `c` only after an acquire-load reads
   READY (so the memset happens-before) or after winning the CAS to PUMP. Exclusive
   ownership, both directions.
5. **The counters.** All of them are pump-only except `g_pzWorkerSkips`, which
   `Prezero_RunChunk` reaches from a worker AND from the pump helping its own drain —
   so that one is `std::atomic<uint64_t>` and the others are deliberately not. A race
   argument that says "these threads never overlap" has to be true for every counter it
   covers, not just the interesting ones.

**And the lifetime hazard, which no fence covers.** `GrowSharedArenaIfNeeded` destroys the
buffer the workers write into, so it drains first; `Prezero_Dispatch` drains before it
re-posts, because with `framesInFlight = 1` the previous dispatch's region and the new
one are the SAME memory and a generation counter on the flags would not have covered a
straggler still inside its `memset`. The drain has the pump HELP (claim and zero what is
unclaimed) rather than block, so its cost is bounded by one chunk per worker — it measured
**0.000 ms/frame**.

**One interaction found by asking rather than by a crash:** with `CZ_VK_NO_PARALLEL_GUARD=1`
— the first picture-bisection arm this project reaches for — the pool grants no workers,
and the pool also does not spawn its threads until the first frame files a guard job. In
both cases posted chunks would have been left for the PUMP to clear at the next drain,
i.e. the pump would `memset` the whole posted region instead of 2,192 bytes a draw: a
regression, in the arm least likely to be measured. `Prezero_Dispatch` now posts nothing
and zeroes the capacity when there are no started workers, which sends every draw down the
general-arena path — byte-for-byte the pre-part-111 one.

### 10.5 — Gates

| gate | result |
|---|---|
| `--smoke` | OK |
| `find_unlowered_switches.py` | clean |
| `shader_dim_census.py` | clean |
| PM4 boundary oracles (both) | clean |
| E3 picture, `tools/part47_gates.sh` | **ALL GATES CLEAN**, best +0.8652, 4 of 5 LAYOUT AGREES (after §10.3's fix) |
| A5 kernel diff, `--include-high-frequency` | **exit 0** — 5 permutation windows, 0 real |
| `truncated=` | **0** |
| `no translated shader` | 0 |
| **B1 poison positive control** | **PASSES: the picture breaks**, +0.87 -> **+0.27**. The pre-zeroed block is what the draw reads, so the null is a real null and not an inert arm (gotcha 30) |

B1 issues no Vulkan commands and adds no barriers — it is a host write into
`HOST_VISIBLE | HOST_COHERENT` memory, made visible by the implicit host-write dependency
of `vkQueueSubmit`, which is the same guarantee the arena has always relied on. The
shipped default is `CZ_VK_PREZERO` OFF, so the binary's Vulkan behaviour is part 110's.

