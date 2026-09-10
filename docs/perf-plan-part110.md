# Perf plan, part 110 — the profiler that lies, and the four idle cores

**The instruction (2026-09-10, after part 109 closed):** *"You think you can make it so
that we use more the third core last time you said it's pretty much only used like at 3%
and if you can fix the profiler for the things he lie to us about?"* — and, on why the
CPU and not the GPU: *"we can run it at 1080p 60 fps on a gtx 1050 ti / Steam Deck."*

**Read `docs/perf-plan-part109.md` §5 first.** This plan is built entirely on part 109's
decomposition and does not re-derive any of it. The 120 fps target stands: **the CPU frame
under 8.33 ms at ~9,300 draws on the operator's Ryzen 7 5700**, which today is 10.8-11.2 ms.

---

## §1. What part 109 established, so a fresh session does not re-measure it

**The pump thread is the frame.** 97.7% of a core, 25% of all the CPU this process uses,
and the wall frame is 10.8-11.2 ms against a pump doing ~10.5 ms of CPU work.

**The machine is idle.** 3.91 of 8 physical cores, measured at the crowd:

| thread | % of one core | what it is |
|---|---|---|
| pump | **97.7** | the PM4 walk **and** every per-draw renderer call |
| guest | 72.9 | recompiled title code |
| guest | 58.5 | recompiled title code |
| worker ×3 | 34.0 / 33.8 / 33.7 | `GuardFold` is 84% of them, `ParRec_RecordInstance` ~1% |

**The thread budget is fully spent and the record pool gets nothing.** The boot line reads
`budget 3 workers` → `guard 3 of 4 wanted (clamped)` → `0 worker slots unclaimed`. There
is no 3%-busy thread; there are three 34%-busy ones and no budget left. **Any design in
§3 that wants workers must say where they come from** — `ThreadBudget` is the seam and the
operator's standing rule is *leave two cores for the machine* (`budget 3 (reserve 2, cap 6)`).

**Where the pump's ~10.5 ms goes** (self time, `perf -F 999`, symbols; one point of the
thread ≈ 0.105 ms of frame):

| symbol | % of pump | ms | movable in principle? |
|---|---|---|---|
| `DoDraw` (+ its `$_3` lambda) | 20.7 | 2.17 | **yes** — per draw, 388 lines, no hotspot |
| `WriteRegisterRun` | 10.2 | 1.07 | **no** — the walk |
| `UploadStream` | 9.4 | 0.99 | **yes**, if the cache can be sharded |
| `ExecutePacket` | 8.1 | 0.85 | **no** — the walk |
| `UploadTextureUncached` | 7.5 | 0.78 | **yes**, same condition |
| `__memset_avx2` (the shared block) | 4.2 | 0.44 | **yes** — per draw, into the arena |
| `PersistFind` | 4.2 | 0.44 | **yes**, same condition |
| `CopyConstWindow` | 3.6 | 0.38 | **yes** |
| `ExecuteLinear` | 3.6 | 0.38 | **no** — the walk |
| `__memmove_avx` | 3.1 | 0.33 | **yes** |
| `__memcmp_avx2` (the stream guard) | 2.6 | 0.28 | **yes** |
| `TexFind` | 2.6 | 0.27 | **yes** |
| `PipelineKey::_M_locate` | 1.6 | 0.16 | **yes** |

**The walk is ~2.30 ms and is inherently serial** (a register state machine; draw order is
semantic). Everything else is per-draw. **That is the whole thesis of §3 and §3.1 is the
measurement that tests it rather than assuming it.**

**Three more facts that constrain any item here:**

* **Memory latency, not compute.** Vectorising the hottest LINE on the pump (a scalar byte
  swap, 0.76 ms, 69.9% of its symbol) was built, gated, proven engaged at 90.6%, and
  measured **+0.14 ms** (gotcha 545). `UploadStream`'s four hot lines are four dependent
  loads; `PersistFind` is a DRAM round trip. **The lever is fewer memory touches, not
  faster ones — and moving work to another core is exactly "fewer touches per core".**
* **No GPU item converts.** GPU 5.93 ms against an 11.23 ms wall, fence **0.00**.
* **The p99 is not a hitch class.** Same draws, same GPU time, no upload, no compile — the
  identical work takes 35% longer. A median at 8.33 ms with this shape leaves the 99th
  percentile at **10.5-11.2 ms (89-96 fps)** depending on whether the tail scales with the
  work or is additive jitter; part 109 measured one run and cannot say which.

---

## §2. ITEM A — the profiler that lies

### A.0 The defect, stated exactly

`CZ_VK_PROFILE` prints a phase table that reads as a decomposition of the frame. It is
not one. **A `ProfScope` measures a region of code, and the regions do not cover the
functions they are named after.** `streams` reads **0.3%** of the frame while the SYMBOL
`UploadStream` is **9.4% of the pump thread** — a factor of thirty.

This is not new and that is the point: **part 22 closed the stream cache on `streams`
reading 0.0%; part 55 re-opened it from a `perf` symbol profile and found 13.1% of the
pump; gotcha 343 was written about it; and part 109 read the same 0.3% and wrote "item 2
is almost certainly dead on price" four hours before its own symbol profile said
otherwise.** Three times, same function, same instrument. The instrument is the defect.

**Why nobody simply scoped the whole function:** `UploadStream` is called ~46,000 times a
crowd frame and a `ProfScope` is two clock reads at ~21 ns, so a per-call scope is ~2 ms a
frame — larger than several phases it would separate (gotchas 7, 223). That constraint is
real and A.2 works within it rather than pretending it away.

### A.1 — Make the profiler state its own coverage (do this first; it is the cheapest and it alone would have prevented all three mistakes)

Print, every profile window, **what fraction of the pump thread the phases actually
account for**, and the residual by name.

* The pump's own CPU time per window is already available (part 52's thread-clock read;
  `pump thread: 98.1% on CPU` is printed today).
* Sum the phases. Print `phases account for N% of the pump; M% is UNSCOPED`.
* **The line must name the unscoped share as unscoped, not distribute it.** Today's
  `outside` reads like a category ("the walk"); an unscoped remainder is not a category.

**Acceptance:** on one crowd run, the printed coverage must agree with a `perf` symbol
profile of the same run within **5 percentage points**. If the phases cover 60% of the
pump, the line must say 60%.

### A.2 — Sampled whole-function timers, so a phase means what its name says

For each function whose scope is partial, add a **sampled** timer around the WHOLE
function: time 1 call in N (N = 16, the ratio part 89's resolve-split census already
uses), scale by N, and print it beside the existing scope so the two are visibly different
numbers rather than one replacing the other.

Functions to cover, in order of the gap they hide:

| function | scope reads | symbol is |
|---|---|---|
| `UploadStream` | `streams` 0.3% | 9.4% of the pump |
| `PersistFind` | (none) | 4.2% |
| `UploadTexture` / `UploadTextureUncached` | `textures` 5.0% | 7.5% + `TexFind` 2.6% |
| `CopyConstWindow` | inside `constants` | 3.6% |
| `DoDraw` as a whole | split across `record`/`other`/… | 20.7% |

**Two rules this must obey or it becomes the next thing that lies:**

1. **The sampling must be unbiased across calls, not "every 16th draw".** A draw's streams
   are not interchangeable with another draw's; sample on a per-CALL counter.
2. **It must cost nothing when the profiler is off.** Gate on the same armed flag
   `ProfScope` uses, and gate it as a plain global compare, not a `static const` initialiser
   on a hot path (gotcha 453; part 76 had to take a `getenv` back off a per-draw path).

**Acceptance:** each covered function's new number must land within **20%** of its `perf`
symbol share on the same run. **Identity gate:** a profiler-OFF crowd run must be
unchanged — three runs an arm against part 109's baseline through `tools/part109_band.py`,
and a delta outside ±0.10 ms means the sampling is not really off.

**Bill:** state it. The sampled timers add 2 clock reads per 16 calls ≈ 46,000/16 × 42 ns
≈ **0.12 ms a frame** on a profiled run. That is inside the profiler's existing 4.2-5.0 ms
and must be printed next to it, not hidden.

### A.3 — The standing cross-check, which is what stops this recurring

`tools/phase_vs_perf.py <perf.data> <profiled log>`: read the phase table out of the log
and the per-thread symbol shares out of the perf data, print them side by side against a
hand-maintained phase→symbol map, and **exit 1 on any phase disagreeing with its symbol by
more than 2x**.

**Its positive control already exists and must be run first.** Point it at part 109's
archived artifacts — `~/DR2CZ-troubleshooting/part109/crowd3440b.perf.data` and a profiled
1080p log from `~/DR2CZ-troubleshooting/part80-crowd/crowd_0910_03*_prof1080_*.log` — and
**it must flag `streams` vs `UploadStream`**. A checker that cannot detect the known case
proves nothing by passing (gotcha 30).

Then run it once per part, beside the other standing gates.

### A.4 — What item A is and is not

**It buys no milliseconds and it is not measured in milliseconds.** Its acceptance is the
three gates above. It goes first because every judgement in §3 is made from this table,
and part 109 spent a night proving what an unchecked one costs.

---

## §3. ITEM B — the four idle cores

**The thesis:** ~2.30 ms of the pump is the PM4 walk and must stay serial; the remaining
~8 ms is per-draw work, and there are four idle physical cores. **The thesis is not
established and §3.1 is designed to kill it cheaply if it is wrong.**

### §3.1 — STEP 0a: THE CEILING ARM. Build nothing until this number exists.

Build `CZ_VK_NO_DODRAW=1`: the PM4 walk executes normally — every register write, every
packet, every state change — but `VkRenderer_Draw` returns immediately. Nothing is
recorded and nothing is drawn.

* **DESTRUCTIVE, like `CZ_VK_NO_DRIVER_RECORD` (which is the model to copy — `vk_renderer.cpp:15496`).**
  It must announce itself loudly at boot and print the count of draws skipped, and no
  picture or wall-time claim may ever be made from a run carrying it.
* **Read the PUMP'S OWN CPU TIME, not the wall.** The arm renders nothing, so its wall
  time is meaningless and its GPU is empty; what is wanted is `perf`'s pump-thread seconds
  per presented frame, via `tools/part109_probe.sh`. Say this in the arm's own comment —
  an arm that renders less is inadmissible for wall (the A/B admissibility rule) and this
  one is *only* admissible for the pump's CPU.
* Three runs an arm, alternated with the default, `tools/part109_probe.sh`.

**The arithmetic, pre-registered:**

* `F` = the pump's CPU per frame with the arm on — the **serial floor**.
* `M` = 10.5 − `F` — the per-draw work that could in principle move.
* Ideal with the 3 budgeted workers: **`F + M/3`**, before any merge, snapshot or
  contention cost, which are all additive and none of which is zero.
* The wall is ~0.3-0.7 ms above the pump, so **the pump must reach ~8.0 ms** for the frame
  to reach 8.33.

**PRE-REGISTERED KILL: if `F + M/3` > 8.0 ms, item B cannot reach the target even
implemented perfectly. Report that number, close the item, and do not write threading
code.** Between 8.0 and ~9.5 the item cannot hit 120 but is still worth 1-2 ms; that is a
judgement about whether to spend a week for a number short of the goal, and it is
**the operator's**, not this plan's — surface it, do not decide it.

### §3.2 — STEP 0b: the shared-state MUTATION census (only if 0a survives)

`F + M/3` assumes the movable work is perfectly parallel. It is not: the parts that MUTATE
shared state must serialise. So count them before designing anything.

`CZ_VK_SHARED_MUTATION_CENSUS=1` — per frame, on the per-draw path, count and time:

| operation | expected shape | shardable? |
|---|---|---|
| arena bump allocations | ~4 per draw | **yes** — per-worker sub-arenas |
| stream-cache **finds** | ~4.9/draw, 46k/frame | yes — read-mostly |
| stream-cache **inserts** | **~2,000/frame** (the frame-first touches) | the real question |
| persist store inserts + `mirrorPending` pushes | ? | the real question |
| texture-cache finds / inserts | 13.9k finds/frame, ~0 inserts | yes / trivial |
| bindless descriptor-slot writes | ? | the real question |
| pipeline-cache inserts | ~0 at the crowd | trivial |

**The encouraging prior:** part 109 measured 46,000 stream lookups a frame against ~2,000
frame-first touches — **96% of that traffic is READS.** If that shape holds across the
other caches, the irreducible serial section is small and B is alive. If descriptor-slot
allocation or the persist store turns out to mutate per draw, it is not.

**Serial residue `S` = `F` + (the measured cost of the mutating operations).**
**PRE-REGISTERED KILL: if `S + (M − mutations)/3` > 8.0 ms, close item B** with the same
"between 8.0 and 9.5 is the operator's call" clause.

### §3.3 — The design, and it is only written if §3.1 and §3.2 both survive

Sketch, so a fresh session knows the shape being aimed at and its hazards — **not a
licence to start here:**

1. **The pump walks and DISPATCHES.** For each draw packet it captures the small amount of
   register state that draw depends on (`CopyConstWindow` already extracts exactly this)
   and posts a work item. It does not decode, upload or record.
2. **Workers do `DoDraw`'s body** — constants, textures, streams, and the recording into
   their own secondary command buffer. `ParRec` already provides chunked secondaries and
   already preserves submission order; that half exists and works.
3. **Sharding:** per-worker arena, per-worker stream/texture cache shard with a merge at
   frame end, or a read-mostly shared table with a small serial insert queue drained by
   the pump.
4. **Threads:** there is no spare budget. Either the guard pool gives slots back (it runs
   at 34% each — is 3 the right number?) or `ThreadBudget`'s reserve changes, and that is
   the operator's rule to change, not this plan's.

**Four hazards, each of which has already bitten this port once:**

* **The bill lands somewhere.** Part 53 moved 13.1 points off the pump and 33.2 points
  appeared on the workers. Measure both sides (`a-parallel-items-price-is-not-its-symbol-share`).
* **A snapshot is not free.** If capturing per-draw register state costs more than the work
  it moves, the item is a loss and only a measurement says so.
* **The LRU/recency stamps.** Part 109's texture memo nearly broke the reclaimer by
  skipping the lookup that stamps `lastUsedFrame`. Any sharded cache inherits that hazard.
* **Draw order is semantic.** Recording order must be preserved even though the *work* is
  not ordered.

### §3.4 — Staging and gates

Build in this order, each with its own A/B and each shippable alone:

* **B1** — move the cheapest self-contained per-draw block (the constants: `CopyConstWindow`
  + the shared-block zero, ~0.8 ms) and prove the dispatch machinery, the snapshot cost and
  the bill on the workers. **Kill: if B1's measured saving is under 40% of the 0.8 ms it
  moved, the dispatch overhead is too high and the whole design is refuted cheaply.**
* **B2** — streams and textures, which need the sharded caches. The hard one.
* **B3** — the rest of `DoDraw`.

**Gates for every stage** (not optional; part 109 ran all of them): `--smoke`; A5 kernel
diff exit 0; `truncated=0`; `CZ_VK_VALIDATION=1` identical to the control arm;
`CZ_VK_SYNC_VALIDATION=1` at 0 hazards with `CZ_VK_BARRIER_POISON=1` producing 30; and a
**picture** gate — `tools/frame_era_medians.py` with a null pair measured in the same
block, arm inside the null. A threading change also needs **ThreadSanitizer or an explicit
race argument per shared structure**; "it did not crash in three runs" is not a gate.

---

## §4. Protocol — unchanged, and part 109 proved each of these the hard way

* **Three runs an arm, alternated**, `tools/part80_crowdroute.sh <tag> [ENV=VAL]` (the tag
  is the FIRST argument; env after it).
* **Read with `tools/part109_band.py`, never with run medians.** Part 109's texture memo
  read 10.81 vs 10.82 ms — a dead null — because the arms landed at 8,989 and 9,411 draws;
  matched bands recovered −0.33 ms monotone in all five (gotcha 544).
* **The profiler is for MECHANISM, never for wall** (+4.2 to +5.0 ms on this box). Wall
  from unprofiled runs only.
* **Every arm must PROVE IT ENGAGED with a counter, and its control must print the
  opposite** (gotcha 408). Part 109's arms all did; that is why its nulls are believable.
* **Report a counter on the run's own exit path AND per FPS window.** Two runs in part 109
  ended without the SIGTERM handler printing anything at all (gotcha 543).
* **Check `docs/instruments.md` for an arm's entry BEFORE running it** — census entries
  carry the ANSWER. Part 109 re-bought a census parts 80 and 87 had already killed.

## §5. Order of work

1. **A.1** (coverage line) — cheapest, and alone it prevents the class of error.
2. **A.3** (cross-check tool) with its positive control on part 109's archived artifacts.
3. **§3.1** (the ceiling arm). **This is the decision point for the whole plan.**
4. **A.2** (sampled timers) — while §3.1's runs are occupying the box, since it is code
   and not measurement.
5. **§3.2**, then §3.3/§3.4 only if both kills are survived.

**If §3.1 kills item B, that is a result and the plan ends there.** Part 109's §3 is the
precedent: the honest close is to name the number and what remains, not to keep buying
items. What would remain in that case is the two verified sub-threshold items part 109
left off (`CZ_VK_TEXMEMO=1` −0.33 ms, `CZ_VK_SCOPED_SHARED_ZERO=1` −0.21 ms) and the
operator's decision about whether to take them.

## §6. Execution record

### 6.1 — A.3 first, not A.1, and the reason is a retraction of A.1's claim

The plan's order put A.1 first on the grounds that it "alone would have prevented all
three mistakes". **That is wrong and A.1's own measurement is what refutes it.**

The coverage was measured before the line was written, off part 80's archived profiled
logs: at 2,472 draws the phases account for **69.1% of wall**, the PM4 walk for **27.3%**,
sleep for 2.3% — and the pump is 96.6% on CPU. As shares of the pump's CPU that is
**~73% phases, ~28% walk, ~0% UNSCOPED**. There is no missing time. The table already
accounts for essentially the whole thread.

**So the defect is MISATTRIBUTION, not omission, and a coverage line cannot see it.**
`UploadStream`'s cost is not unscoped; it is charged to `record`, a real scope that
really did contain it, because `ProfScope(streams)` deliberately wraps only the
`CopySwapped` and the flat-cache lookup, the content guard and the cross-frame store sit
outside it. A phase table can account for 100% of a thread and be wrong about every row.

A.1 was still built, because a table that does not state its own denominator is worse
than one that does — but **the line carries the warning as well as the number**, in those
words, and points at the check that can actually catch the defect.

### 6.2 — A.3: `tools/phase_vs_perf.py`, and its positive control fires

Reads the phase table out of a profiled log and the per-thread symbol shares out of a
`perf.data`, puts them on one denominator (the profiler's own `pump thread: N% on CPU`
line is exactly the conversion between "share of wall" and "share of the pump's cpu"),
and compares each phase with the symbols implementing **the subsystem it is named
after** — not with the symbols that happen to run inside the scope, which is a comparison
that could never catch this. Exit 1 past 2x.

**Positive control (gotcha 30), on part 109's archived artifacts:**

| phase | table | symbols | ratio | verdict |
|---|---|---|---|---|
| `streams` | 0.24% | 13.14% | **53.7x** | LIES |
| `textures` | 5.09% | 10.85% | **2.13x** | LIES |
| `constants` | 17.22% | 7.54% | 0.44x | note (advisory) |
| `pm4` (walk) | 22.75% | 21.13% | 0.93x | ok |

`streams` is flagged, so the checker fails on the case that already happened — and it
found a second one nobody had looked at: **`textures` under-reports its own subsystem by
2.1x**, because `TexFind` (the cache lookup) and `DecodeTextureFetch` sit outside the
scope and are charged to `other`/`otherFetch`.

The `constants` row is ADVISORY and the reason is a limit of the TOOL, stated rather than
hidden: `constVsPatch` is not a call — `SceneXformForm` and the two patch helpers are
inline code in `DoDraw` — so at -O2 their cycles land in the `DoDraw` symbol and no
symbol regex can claim them back. That row's symbol column is a lower bound.

**A mechanical finding worth its own line, because it would otherwise make every archived
capture in this project unreadable:** `perf` resolves a DSO by build-id and REFUSES a
file at the recorded path whose build-id has moved on. `tools/part80_crowdroute.sh`
re-links `cz_runtime_crowd` on every build, so part 109's captures read **89%
`[unknown]`** on the pump thread today — which looks exactly like a capture with no
symbols. `--binary` builds a symfs symlink farm (the system roots symlinked in, so libc
and the driver still resolve) and points `perf script` at it. Only one of part 109's four
captures still has its matching binary (`unkres.bin`); the other three are unreadable
forever. **Archive the binary with the capture.**

### 6.3 — The instrument §3.1 actually needs, and why it is new

§3.1's quantity is the pump thread's CPU milliseconds per presented frame. Until now that
was obtainable only by crossing two instruments taken over **different windows**: a "% of
one core" from `perf` or `part50_thread_cpu.py` over its own 15 s sample, divided into a
frame rate from somewhere else. This project has a name for that arithmetic and it once
invented 59 MB/frame that never existed (`two-counters-are-not-a-pair`).

So the `[fps]` line now carries `pump cpu N.NN ms/frame (M% of a core)` — one
`clock_gettime` per FPS WINDOW, not per frame, printed whenever `CZ_FPS_LOG` is on and
therefore available in a completely uninstrumented run. `tools/part110_pumpcpu.py` bands
it by draw count the way `part109_band.py` bands wall time.

**Its first reading reproduces part 109's cross-instrument arithmetic exactly**, which is
the check that it is measuring the right thing: at 9,100-9,140 draws, wall 11.2 ms median,
**pump cpu 11.03 ms/frame at 98% of a core**. Part 109 got 97.7% and ~10.5 ms by dividing
one instrument into another; one window of one run now says it directly.


