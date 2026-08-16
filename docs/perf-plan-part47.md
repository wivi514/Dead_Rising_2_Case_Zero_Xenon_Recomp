# The performance plan — written against the OPERATOR'S OWN FRAME, part 46 → 47

> ## STATUS after part 47 executed tiers 1 and 2 — read this before the plan below
>
> **THE TARGET IS MET ON THE HEADLESS ROUTE.** Three runs an arm, same binary,
> the three part-47 arms the only variable, both negative controls reading
> exactly zero:
>
> | draw bin | part 47 | pre-47 |
> |---|---|---|
> | 3,000-5,000 | **32 ms, 98% pinned** | 32-40 ms, 7-66% pinned |
> | 5,000-8,000 | **32 ms, 73-85% pinned** | 42-46 ms, 5-13% pinned |
> | 8,000+ | **36-37 ms** | never reached |
>
> The crowd frame stops being CPU-bound and becomes PACING-bound. §7's table
> predicted 61.7 -> 41.0 ms for tiers 1+2; on this route the saving is larger
> than that and tier 3 is no longer needed to reach 30 fps HERE. **The operator's
> frame is the one that decides, and it is ~2x this one.**
>
> **Item 1.1 was correctly ranked first and was worth about twice its estimate.**
> The plan's own first instruction was one run of `CZ_VK_NO_TEX_REVALIDATE=1` to
> bound it, and that run says the guard is **nearly the whole texture phase**: on
> the outdoor route `textures` is **15.9 ms with it and 2.3 ms without**, against
> an estimate of 8-11 ms. §7's ranking stands; §1.1's price does not.
>
> **What part 47 built, all of it in tiers 1-2 plus 3.1:**
> * 1.1 as a CADENCE change — the guard runs once per frame per cache entry rather
>   than once per fetch. **93.4% of checks skipped, 15.1x less hashing**;
>   `textures` **17.18 -> 2.47 ms**, i.e. essentially the whole upper bound
>   recovered while keeping the mechanism. Arm `CZ_VK_TEX_GUARD_EVERY_FETCH=1`.
> * 1.2 and 1.3 as written.
> * 2.1/2.2 as written, and **verified against the code they replace** — 0
>   mismatches over 152 M dwords with a positive control, **100.0% bulk**.
>   **151-158 -> 110-113 ns per packet**, zero overlap over nine windows an arm.
>   Arm `CZ_PM4_NO_BULK_REGS=1`.
> * 3.1 turned out to be **already bought for its five binds** (pipeline 70-81%,
>   viewport/scissor 99%, blend and descriptor sets 100% skipped since part 18).
>   What was NOT bought is the vertex and index binds, which part 18 deliberately
>   only counted; their repeat rate is 51.0% / 39.4% on the operator's session, so
>   they are now cached too. Arm `CZ_VK_NO_BUFFER_BIND_CACHE=1`.
> * Not in the plan, same class as 1.2: the per-fetch sampler lookup was a
>   `std::map` over a NINE-BIT key and is now a 512-entry table.
>
> **Two things recorded AGAINST the result rather than around it**, both in
> `docs/phase5-notes.md` §6cd. `outside` and `record` read slightly worse on the
> part-47 arm and that comparison is INADMISSIBLE — the arms do not submit the
> same command stream, their packets-per-frame differ by 40%, and a matched DRAW
> band does not match a PM4 workload; cost per packet is the admissible
> statistic. And item 1.1's registered claim ("`changed` must not fall") holds on
> the event rate but is UNRESOLVED on the distinct-address measure, where the
> arms' route difference confounds it.
>
> **What 1.4 turned out to be**: the "two cache lookups per fetch" is arm-gated —
> the first `R->textures.find` is inside the `CZ_VK_TEX_CACHE_FIRST` test. There
> is one lookup on the default path and nothing to collapse. The per-draw memo
> half is now largely redundant, since the once-per-frame cadence removes the
> work it would have skipped.
>
> **Still open**: 3.2, multithreaded recording — deliberately last, and now less
> urgent. The PM4 census counters, four atomic RMWs per packet at 94,000 packets
> a frame (`docs/part48-kickoff.md` item 1b). And the one lever inside 1.1 that
> trades detection for cost, hashing a bounded prefix: `CZ_VK_TEX_GUARD_BYTES=N`
> exists, **its default is unchanged**, and the histogram that prices it is
> printed with the stats.


Written at the end of part 46 (2026-08-16), after the first session that ever
profiled the operator's real, windowed, played frame. Supersedes
`docs/perf-cpu-plan.md` as the live plan; that document's §1 ranking was built on
a headless route that **understates this workload by about a factor of two**, and
its numbers should be read as history.

Target, stated once: **Xbox 360 shipped this game at 30 fps, i.e. a 33 ms frame.**
The operator's frame is **61.7 ms**. We need to remove **~29 ms**. Every item
below is priced against that budget, so "how much of the 29" is always answerable.

---

## 1. THE BUDGET — where the operator's 61.7 ms actually goes

One 20 s `CZ_VK_PROFILE` window from
`~/DR2CZ-troubleshooting/part46-operator4/budget.log`, at **7,231 draws/frame**,
windowed, on their own route. The sub-phases are percentages OF THE FRAME and sum
to the draw path, so they can be read as milliseconds directly.

| phase | % of frame | **ms** | what it is |
|---|---|---|---|
| **textures** | 42.9% | **26.5** | `UploadTexture`, once per texture fetch per draw |
| **outside** | 28.5% | **17.6** | not in the draw path — mostly the PM4 walk (below) |
| **record** | 17.7% | **10.9** | Vulkan command recording, ~1.5 µs per draw |
| other | 7.3% | 4.5 | the rest of the draw path |
| constants | 2.3% | 1.4 | the ALU constant copies |
| readback | 1.2% | 0.7 | |
| streams | 0.0% | 0.0 | **closed in part 22 — do not revisit** |
| submit | 0.1% | 0.1 | |

And from the same run:

* **PM4: 94,098 packets/frame at 151 ns each = 14.2 ms/frame**, carrying 797,624
  register dwords/frame (8.5 per packet, ~17.8 ns per dword).
* **The GPU is ~34% utilised at P5/559 MHz.** `submit` is 0.1% and its `gpu`
  column is 0.0. **We are not waiting on the GPU at all** — this is a pure CPU
  problem, and every millisecond removed from the CPU is a millisecond of frame
  time, up to the point where the GPU becomes the wall.

**The three targets are textures (26.5), the PM4 walk (14.2) and record (10.9).**
Together they are 51.6 of the 61.7 ms. Nothing else is worth touching first.

---

## 2. TIER 1 — the texture phase, 26.5 ms

### 1.1 The texture revalidation guard reads 92.9 MB/frame to catch 0.0037% of anything — EXPECTED SAVING 8-11 ms

**The evidence, from the operator's own session** (3,946 frames):

```
texture guard: 26,795,428 cache hits checked,
               986 served an image whose guest bytes had CHANGED (0.00%),
               986 re-uploaded | guard read 366,677.2 MB
```

366 **gigabytes** of content hashing over the session — **92.9 MB per frame** —
to detect **986 real changes in total**. That is 6,790 texture cache hits per
frame, each re-hashing the guest bytes behind the texture to check the address
has not been recycled underneath it.

The revalidation is *correct* and it is *load-bearing*: part 38 built it because
the once-only upload cache served a streaming-recycled address's first occupant
forever (the tanker cylinder wearing a brick wall), and it is
operator-confirmed. **The mechanism stays. Its cost does not have to.**

**The change**, and it is the same insight that just fixed the UI text layer, one
subsystem over: exactness is being bought for every texture on every fetch, when
what is needed is exactness for the textures that actually get recycled. Options,
in the order to try them:

1. **Round-robin the revalidation under a per-frame byte budget.** A recycled
   address is not a one-frame emergency — the wrong picture persists until the
   next upload either way, so checking each texture every N frames instead of
   every fetch is nearly as good and costs 1/N. With 92.9 MB/frame and a 8 MB/frame
   budget the worst case is a ~12-frame detection latency, a fifth of a second.
2. **Promote by observation, exactly like `PersistEntry::dynamic`.** A texture
   whose bytes have ever changed is checked every time; one that has never
   changed is checked on the budget. 986 of 26.8 M is the population that would
   promote.
3. **Hash a bounded prefix + the size**, as `StreamGuard` already does for
   streams, rather than the whole surface.
4. **Skip the guard entirely for addresses that are resolve destinations or that
   the VFS has never re-read** — a texture the guest has not touched cannot have
   been recycled.

**Prediction**: `textures` falls from 42.9% of the frame to under 25%, and the
frame falls by 8-11 ms. **Control arm**: `CZ_VK_NO_TEX_REVALIDATE=1` already
exists and is the pre-part-38 renderer — run it FIRST to put an upper bound on
what this item can possibly be worth, because that arm removes the whole cost and
its picture defects are known and tolerable for one measurement.
**Risk**: a recycled texture is served stale for up to the budget latency. The
part-38 defect returns in a milder form if the budget is too small — so the
counter `texture: cache hit but the GUEST BYTES CHANGED` must be watched, and its
986 must not grow.

### 1.2 The hottest counters in the renderer use the SLOW counter — EXPECTED SAVING 1-1.5 ms

`Count()` is `++g_stats[name]` — it constructs a `std::string` from the literal
(a heap allocation above 15 characters, and most of these names are long) and
walks a red-black tree, **per call**. This codebase already knows this and built
`COUNT(lit)`, which resolves the map node once per call site into a function-local
static; the comment at `vk_renderer.cpp:152` explains exactly why. **The texture
path never got converted.** Per frame, from the operator's counters:

| call site | per frame |
|---|---|
| `texture: cache hit` (two sites, 3213 and 3489) | ~6,790 |
| `texture: served from a DEPTH resolve snapshot` | ~1,932 |
| `texture: CUBE fetch` | ~274 |
| `texture: served from a resolve snapshot` | ~136 |
| `texture: snapshot served at the surface PITCH…` | ~110 |
| **total slow counter calls** | **~9,300/frame** |

At 100-200 ns each that is 0.9-1.9 ms of a 61.7 ms frame spent *counting*, inside
the phase being measured — the exact defect gotcha 230 names and that made part
18's state cache first measure as a dead heat.

**The change**: mechanical. Convert all 16 `Count(` sites inside `UploadTexture`
(vk_renderer.cpp 3049-3560) to `COUNT(`. **Prediction**: 1-1.5 ms, and every
counter reads identically afterwards — which is also the correctness check, since
the printing interface is untouched. **Risk**: none worth naming; the macro is
already in use elsewhere.

### 1.3 Linear scans on the per-fetch path — EXPECTED SAVING 0.5-2 ms

`R->snapshotsSampledThisPass` is a `std::vector<uint32_t>` searched with
`std::find` on **every fetch that hits a resolve snapshot** — 8.2 M times in the
operator's session, ~2,070 per frame, each a linear scan (vk_renderer.cpp:3414).
`seenCubeSnap` has the same shape at 3248.

Both are **diagnostics**: the vector exists to answer "which pass sampled the
scene", read by one instrument. **The change**: make them `unordered_set`, or
better, build them only when the instrument that reads them is enabled. A
diagnostic that costs a linear scan per texture fetch on every run is the same
class of mistake as 1.2.

### 1.4 Two cache lookups per fetch, and per-draw work that repeats — EXPECTED SAVING 1-3 ms

`UploadTexture` does `R->textures.find(key)` at 3209 and again at 3488, with the
snapshot search in between. And the whole function is called **per fetch per
draw**: the operator's 7,231 draws produce 6,790 cache hits, so most draws are
re-resolving fetch constants that have not changed since the previous draw.

**The change**: (a) collapse the double lookup; (b) memoise the resolved bindless
slot on the fetch constant's six dwords **at draw granularity** — a one-entry
cache keyed on `(constIdx, the six dwords)` in front of the whole function would
hit on consecutive draws sharing a material, which this title's sorted draw order
makes common. **Measure the hit rate before building the second one** — the
counter costs nothing and decides whether it is worth anything.

---

## 3. TIER 2 — the PM4 walk, 14.2 ms

### 2.1 Bulk register writes — EXPECTED SAVING 5-9 ms

**94,098 packets/frame carrying 797,624 register dwords at ~17.8 ns per dword.**
A register write is a store; 17.8 ns is 50-70 cycles, so essentially all of it is
overhead around the store. `WriteRegister` (pm4.cpp:824) runs per dword:

```
if (index >= kRegCount) return;                                  // bounds
if (index >= g_constWatchLo && index <= g_constWatchHi) …        // A DIAGNOSTIC
g_regs[index] = value;                                            // the actual work
if (index >= kRegScratch0 && index <= kRegScratch7) …            // scratch mirror
```

plus, at the call site, a per-dword `body(i)` accessor that byte-swaps.

**The change**: for `SET_CONSTANT` / `SET_CONSTANT2` / `LOAD_ALU_CONSTANT` runs —
which are the overwhelming majority of these dwords — check the destination RANGE
once against the scratch window and the const-watch window, and when it
intersects neither (the common case by far), byte-swap and copy the whole run
into `g_regs` in one loop with no per-dword branching. A 32-bit byte-swapping
copy runs at memory bandwidth; 797,624 dwords is 3.2 MB/frame, which is under a
millisecond.

**Prediction**: the PM4 walk falls from 14.2 ms to 5-9 ms. **Control arm**: the
PM4 oracles (`tools/xtr_pm4_census.py` and the two boundary oracles) must stay
clean — run them before and after; exit 1 means we would desync on a real
stream. **Risk**: the scratch-mirror and const-watch side effects must be
preserved exactly for ranges that DO intersect. Write the range test so that the
fallback is the current per-dword path, and count how often the fallback is
taken — if it is common, this item is worth less than it looks.

### 2.2 The const-watch branch is a diagnostic on the hottest loop in the runtime

`g_constWatchLo/Hi` is read per dword. When the instrument is off, the range
should be made empty so the comparison is a single predictable branch — or,
better, the whole walk should be templated/duplicated on "watching or not" so the
off path has no test at all. Small, but it is 797,624 iterations a frame.

### 2.3 Ask whether every packet needs to be walked at all

94,098 packets a frame is the guest's own submission rate and we cannot reduce
it. But the walk currently does full dispatch per packet. Two cheap structural
wins worth measuring: hoist the opcode dispatch out of the inner loop for runs of
identical packet types, and confirm that no per-packet allocation or `std::string`
work happens on the common path (the same audit as 1.2, applied to pm4.cpp).

---

## 4. TIER 3 — command recording, 10.9 ms at 7,231 draws

That is **1.5 µs per draw**, which is high for `vkCmdBindDescriptorSets` +
`vkCmdPushConstants` + `vkCmdBindPipeline` + `vkCmdDrawIndexed`.

### 3.1 Redundant state binding — EXPECTED SAVING 2-4 ms

Cache the last-bound pipeline, descriptor sets and push-constant block per command
buffer and skip the call when unchanged. This title sorts by material, so
consecutive draws should share most state. **Measure first**: add counters for
"pipeline bind skipped / descriptor bind skipped / push constants skipped" and
read the ratio before writing the optimisation — if the guest interleaves
materials, the saving is not there.

### 3.2 Multithreaded recording — EXPECTED SAVING up to 6-8 ms, and the biggest single structural item

The draw path is 43.3 ms of a 61.7 ms frame and it is **entirely single-threaded**
on a 16-core machine, while the GPU sits 66% idle. Vulkan secondary command
buffers are the standard answer: partition the frame's draws into N ranges,
record them into secondaries on a worker pool, and execute them in order in the
primary.

**Why it is listed third despite being the largest**: it is invasive, it changes
the ordering guarantees the resolve/snapshot logic relies on, and it multiplies
the cost of every per-draw bug that already exists. **Do tiers 1 and 2 first** —
they are mechanical, they are individually verifiable, and if they land the frame
is already near 33 ms without touching the architecture.

**Prerequisite**: the per-draw path must first be made free of shared mutable
state — the texture cache, the stream store, the snapshot vectors and the counter
map are all written from the draw path today.

---

## 5. WHAT NOT TO DO — already measured, do not re-buy

* **The stream store** (`streams` 0.0% of the frame) — closed in part 22.
* **The three suspects for the "regression"** — part 45's interpolant liveness
  (six-run A/B, every bin inside its noise floor), part 41's per-fetch samplers
  and part 44/45's mip uploads (no movement in the `textures` share). §6cb.
  Note the caveat that matters here: those were exonerated on the HEADLESS route,
  which understates the draw path by ~2x. They are not the cause of the 26.5 ms,
  but they were never shown to be free on the operator's workload either.
* **Pinning the GPU clock** — retracted; the GPU governs itself to P5/559 MHz at
  34% utilisation and is not the bottleneck.
* **Reducing the draw count** — 7,231 draws is what the guest submits and
  hardware submitted the same. There is nothing to cut.
* **`CZ_VK_FRAMES_IN_FLIGHT`** — already 2, already bought (part 23).

---

## 6. HOW TO MEASURE ANY OF THIS — non-negotiable

* **`CZ_VK_PROFILE` phase shares, not frame time**, for anything below ~10%. The
  frame-time A/B needed three runs an arm and still landed inside its floor in
  every bin; the phase share separates arms the frame time cannot. Only quote
  frame time once a change is big enough to move it.
* **Medians and the 16 ms-pinned share, never means** (gotchas 237/238), via
  `tools/part46_perf_read.py`, which refuses a verdict below two runs an arm.
* **Three runs an arm, alternated** (`tools/part46_perf_ab.sh`). The noise floor
  is 10-13% at one run a side.
* **Every item gets a same-binary control arm** and a counter proving it engaged.
  An arm with no counter cannot be shown to have engaged (gotcha 151).
* **The headless route understates the operator's draw path by ~2x.** A win
  measured headlessly should be confirmed on their configuration before it is
  called a win — the reverse of the usual order, because this is the one metric
  where our route is not conservative.
* **Picture gates after every one of these**: the E3 correlation, `no translated
  shader` = 0, `tools/shader_dim_census.py`, and both PM4 oracles. A renderer
  optimisation that changes the picture is a bug, and several of these items
  touch code the picture depends on.

---

## 7. THE ORDER, and what it should add up to

| # | item | expected | cumulative frame |
|---|---|---|---|
| — | baseline | — | **61.7 ms** (16.2 fps) |
| 1.2 | `Count` → `COUNT` in the texture path | −1.2 | 60.5 |
| 1.3 | linear scans off the fetch path | −1.0 | 59.5 |
| 1.1 | texture revalidation budget | −9.5 | **50.0** |
| 1.4 | double lookup + per-draw memo | −2.0 | 48.0 |
| 2.1 | bulk register writes | −7.0 | **41.0** |
| 2.2 | const-watch off the inner loop | −0.5 | 40.5 |
| 3.1 | redundant state binding | −3.0 | **37.5** (26.7 fps) |
| 3.2 | multithreaded recording | −6.0 | **31.5 ms → 31.7 fps** |

**Tiers 1 and 2 alone are ~21 ms and need no architectural change.** They are
what to do first, in that order — cheapest and most certain at the top, so that
each one is verifiable before the next lands.

Every number in that table is an estimate derived from a measurement, not a
measurement. **Item 1.1's upper bound is knowable in one run**
(`CZ_VK_NO_TEX_REVALIDATE=1`), and that is the first thing to do — if it does not
move the frame by ~10 ms, this entire plan's top item is wrong and the ranking
should be rebuilt before anything is written.
