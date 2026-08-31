# Perf plan, part 89 — pricing the MAXIMAL parallel record

**THE OPERATOR CHOSE THIS SUBJECT closing part 88's conversation**: asked for any lead
worth ≥1 ms, the answer was that exactly one exists — this one — and the instruction was
*"we'll do that in a new conversation start the plan with that."* This file is that plan.
`part89-kickoff.md` §0a points here; `phase5-notes.md` §6eh is the state of the frame it
starts from.

## 0. What the item is, and the exact boundary of what is already refuted

**The item**: move the per-draw RECORD phase — ~513-524 ns/draw of state computation,
stream binds and the draw call itself (§6ec §1: record 524 = driver 251 + ours 273) —
off the pump's serial walk onto workers, keeping the PM4 walk serial (a command stream's
meaning is positional) and draw ORDER intact (this title depends on overdraw order).
**Ceiling ~3.2 ms at the operator's crowd** — the largest number attached to any open
CPU item, roughly 8x part 88's two items combined.

**What part 80 refuted is NARROWER than "parallel recording", and the distinction is the
whole reason this plan exists** (port-history part-80 block; gotcha 473):

* REFUTED: the MINIMAL design — distributing the DRIVER calls only. Driver share is
  251 ns/draw = 2.33 ms, so three workers save 1.56 ms *before* capture, scheduling or
  re-establishment, against a pre-registered 1.5 ms kill — and the state cache already
  elides descriptor-sets 100%, blend 100%, viewport 99.4%, scissor 99.3%, pipeline ~70%
  of the calls a distributor would have moved. Do not re-buy.
* **NOT refuted, never priced: the MAXIMAL design** — move the whole ~513 ns phase
  (state computation + binds + record, ours AND driver) into per-chunk secondary
  command buffers. Part 80's own record says it "is unpriced, needs a re-entrant
  `UploadStream`, and still needs those threads."

**What already exists for it** (do not rebuild):

* **The order gate is BUILT and poison-proven** (part 72; `OrderGateArmed`,
  `CZ_VK_ORDER_POISON=N` transposes the Nth pair and the gate must fail). It was
  shipped proven precisely so this item could be written against it.
* The thread budget (`cpu/thread_budget.h`): operator's 8-core box → **3 workers
  total**, and the guard prehash pool takes all three. Part 80 noted
  `ThreadBudget_Take` grants a record pool ZERO threads. BUT the fresh part-88 profile
  shows the guard pool near-idle between dispatches (552 dispatches/window, 0 blocked,
  prehash serving 98.2% with 84.7 MB/frame). **Sharing the existing pool is a live
  option and step 0 must measure its occupancy** rather than assume either way.
* Fresh sub-scope decomposition (part 88's profiler run, 9,086 draws): record 619
  ns/draw instrumented = state 164 + vertex 171 + index 161 + guard 10 + residual 113.
  The vertex/index scopes CONTAIN `UploadStream` — the flat stream cache runs 85,635
  lookups/frame (9.4/draw) on this path.

## 1. STEP 0 — PRICE IT BEFORE BUILDING ANY OF IT (one to two sessions, no fix code)

The refuted minimal design died on arithmetic that was available before anyone coded.
Apply the same discipline here. Three measurements, each cheap, and the kill is
pre-registered before the first one runs:

**0a. The movable fraction.** Classify record's ~513 ns/draw into:
  (i) work that touches SHARED MUTABLE state — `UploadStream`'s flat cache, the
      cross-frame stream store, the content guard, the bump arena allocator, the
      one-entry bind caches, counters;
  (ii) pure per-draw work — state derivation from the register snapshot, vkCmd*
      recording into a buffer that could be a secondary.
Instrument: a census arm (diagnostic, off by default) that buckets the existing
sub-scopes' time by which side of that line each block sits on, at the crowd. The
sub-scopes already nearly split it — what is missing is the (i)/(ii) split INSIDE
`vertex` and `index`, i.e. how much of their 171+161 ns is UploadStream/guard versus
the bind recording around it.

**0b. The serialization schedule.** The maximal design's throughput is bounded by the
serial residue (Amdahl, not vibes): the PM4 walk itself (25 ns/packet × 127k/frame),
whatever part of (i) must stay serial, and chunk capture/handoff. Compute the bound as
`serial + movable/W + overhead` with W = measured available workers from 0c — using
MEASURED numbers for every term, none quoted from an earlier part (gotcha 13; every
§6ec number predates part 86's pump stack and part 88's fixes).

**0c. The workers that actually exist.** Measure the guard prehash pool's busy share at
the crowd (its workers' on-CPU time, not its dispatch count — an idle-looking dispatch
count is not an occupancy figure, gotcha 151's shape). If the pool is <~50% busy,
record chunks can share it; if not, the budget says 0 workers and the item dies on the
operator's own one-budget rule unless they choose to change the policy — that is THEIR
call, surface it, do not decide it.

**PRE-REGISTERED KILL (stated now, before any measurement exists):** compute
`saving = movable × (W−1)/W − overhead_estimate` at the operator's crowd from 0a-0c.
**If saving < 1.0 ms, the item dies** — that is the operator's own bar ("1 or more
ms"), and it dies in step 0 for the cost of two diagnostic runs, exactly as the minimal
design should have.

## 2. THE DESIGN FORK (only reached if step 0 passes) — split UploadStream, don't lock it

Three shapes for the re-entrancy problem, with a recommendation:

* **(a) Lock/shard the shared structures** — per-shard mutexes on the flat cache and
  store. Rejected on prior evidence unless 0a says otherwise: 9.4 lookups/draw on a
  memory-latency-bound path is exactly where lock traffic erases the win (part 53
  measured 0.4 ms/frame of cache pollution from less, gotcha 344).
* **(b) RESOLVE serial, RECORD parallel** — the pump's serial walk keeps doing
  `UploadStream`'s resolve half (lookup/guard/store — the change-detector half that
  CANNOT be memoised or raced, gotcha 474), depositing resolved handles+offsets into
  the per-draw capture; workers turn captures into secondary command buffers. The
  guard prehash pool already runs a frame AHEAD on predictions, so the serial resolve
  half is mostly a pool lookup today (98.2%). **This is the recommended shape**: it
  moves (ii) without touching the correctness machinery, and its capture struct is
  what step 0a's census will have effectively enumerated.
* **(c) Lock-free everything** — not worth designing unless (b)'s measured overhead
  eats the win.

## 3. Verification (all arms exist or are one commit)

The order gate with its poison (exists, proven); `CZ_VK_SYNC_VALIDATION=1` standing
gate 0 hazards (a secondary-buffer change is a barrier-adjacent change); the picture
era-medians on the crowd route with a null pair; `CZ_WORKERS=0` as the whole-feature
off-arm (exists); an engagement counter per worker (chunks recorded, gotcha 151) so the
control and the fix are both provably what they claim. Any new default ships with its
off-arm and its measured milliseconds in the same commit (part 87 §3's rule, still
standing).

## 4. Measurement

3v3 on `tools/part80_crowdroute.sh`, `part80_trace_band.py`, frame-weighted banded
medians, mechanism counters quoted beside the milliseconds (chunks/frame per worker,
serial-residue time) — part 88 §6eh's protocol verbatim. The route floor is ±2.9%; a
≥1 ms effect at the 13 ms crowd frame is ~8%, comfortably above it — this item, unlike
part 88's, can be read from wall time directly.

## 5. The honest failure modes, named in advance

* Step 0 says the movable fraction is small because `ours 273 ns` is mostly resolve
  (i): the item dies cheap. This is a REAL possibility — the bind caches and change
  detectors are the record path's cost centre by every prior census.
* The pool is busy and the budget says 0 workers: the item is BLOCKED ON A POLICY
  DECISION, not dead — write it up and ask the operator.
* It works and the win is real but the picture gate or order gate fails
  intermittently: park it behind its arm and keep the capture/secondary machinery —
  that is how RT shadows were parked, and it is a fine outcome compared to shipping a
  transposition.

---

## 6. EXECUTED — part 89 ran this plan end to end in one part (2026-08-31)

* **Step 0 PASSED, the kill did not fire** (`phase5-notes.md` §6ei): movable 270-290
  ns/draw ≈ 2.4-2.6 ms at 9,000 draws; formula saving 1.0-1.24 ms conservative,
  ~2.2 ms schedule-bound; W = 3 (pool 13-16% busy, the <50% criterion met 3x over).
  Both step-0 instruments were positive-controlled before belief (resolve census
  +112 ns under NO_FLAT_CACHE; occupancy 36% on 1 worker vs 13-16% on 3). The driver
  share was re-measured bill-free at 235 ns/draw (part 80's 251 corroborated; the
  first no-driver run desynced the analog route until CZ_FPS_CAP=60 matched pacing).
* **§2's fork (b) BUILT** — with one simplification the plan did not anticipate: no
  secondaries and no suspend/resume. LOAD/LOAD attachments make an instance split
  the identity, so chunks are self-contained dynamic-rendering instances in plain
  per-worker PRIMARY buffers, submitted as one ordered vkQueueSubmit. §6ej §1.
* **§3's verification all ran and all passed**: order gate 0 fails / 22.1M draws,
  poison fails 100% naming the transposed draw; sync validation 0 hazards; picture
  era medians inside the null (4 runs); engagement counters live in the timed runs.
* **§4's measurement**: 3v3, dominant crowd band **13.00 → 11.20 ms, −13.9%,
  −1.80 ms** — above the operator's ≥1.0 ms bar and above step 0's conservative
  bracket. GPU-bound sub-5,000 bands +0.09-0.22 ms (instance splits), fence-covered.
* **§5's failure modes**: none fired. The pool shared without starvation (prehash
  held 98.3%), the pump never had to help, and no policy decision needed surfacing.
* **SHIPPED ON BY DEFAULT**, `CZ_VK_NO_PAR_RECORD=1` the control arm, milliseconds
  in the same commit (part 87 §3's rule).
