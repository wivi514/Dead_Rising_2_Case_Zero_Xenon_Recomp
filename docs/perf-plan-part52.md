# Part 52 performance plan — stop making one core's work smaller

Written at the close of part 51 (2026-08-17). **This supersedes `docs/perf-plan-part50.md`,
which should now be read only for its executed history.** Of that document's seven items,
three are shipped, three are retired or refuted, and the biggest thing in the frame was
never in it at all.

---

## 0. WHAT CHANGED, AND WHY THIS PLAN IS SHAPED DIFFERENTLY

Every performance plan this project has written — `perf-cpu-plan.md`,
`perf-plan-part47.md`, `perf-plan-part48.md`, `perf-plan-part50.md` — has the same shape:
a table of the renderer's PHASES, sorted by milliseconds, worked top-down. That shape has
delivered real wins (parts 47 and 48 between them took the operator's frame from 64.1 ms
to 42.8), and it has now failed three times in a row in the same way.

**A phase is a scope somebody already suspected.** The per-phase profiler is code we
wrote, inside the thread we wrote, around the operations we thought were expensive. It
cannot report a cost that is not inside one of its scopes, and it cannot report a core
that is doing nothing. Three consecutive parts found their biggest result outside it:

| part | the biggest thing that part found | was it in the phase table? |
|---|---|---|
| 50 | `CZ_VK_PROFILE` costs 2-4 ms/frame | **no** — it was the table |
| 50 | we use 2.46 of 16 cores | **no** — a phase table has no denominator for this |
| 51 | the busiest thread is a spin-wait on our ring pointer | **no** — different thread |
| 51 | the pump slept 1 ms before every walk, ~3 ms/frame | **no** — a sleep is not a scope |
| 51 | `CZ_VK_FRAME_STATS` costs ~3 ms/frame | **no** — it was the other instrument |

So part 52 is planned from a **symbol** budget and a **core** budget, with the phase table
demoted to a cross-check. `perf record -F 999 -p <pid>` produces the first in one command,
reads the guest's code as well as ours, and needs no rebuild (gotcha 340).

### The three facts this plan is built on

1. **Our pump is the limiter and it is ONE thread.** The title's Draw Thread spends 84% of
   its time spinning on the ring read pointer that only our pump advances (part 51 §6ch
   §1, phase 1 finding 38). Our pump does the PM4 walk, the Vulkan recording, the uploads
   and the present, serially, on one core, at ~93% duty. **There are ~13 idle cores.**
2. **The biggest identified cost in the pump is a CONTENT GUARD, not the renderer.**
   With the instruments off, `GuardFold` is **20.04%** of the pump thread against
   `DoDraw`'s 13.00%. The phase table cannot say this: it charges the stream guard to a
   phase called `streams` that reads **0.03 ms**. The third-biggest symbol, `BindShader`
   at 12.47%, appears in no plan this project has written.
3. **The guard is memory-bound, not ALU-bound.** It reads ~35 MB/frame and takes ~3-4 ms,
   i.e. **~10 GB/s**, against the 35.7 GB/s the same fold measures on hot data (part 47).
   It is waiting on DRAM for cold guest memory. **A faster hash therefore buys nothing.**
   Only fewer bytes, or more memory-level parallelism, can move it — and the second of
   those is what 13 idle cores are for.

Those three together point at one strategy, and it is not the strategy of the last four
plans.

---

## 1. THE STRATEGY QUESTION, STATED PLAINLY

Two ways to make a frame shorter when one thread is the critical path:

**(a) Make that thread's work smaller.** Every plan so far. Still legitimate, and items 4-7
below are this. Its ceiling is now visible: the pump's work at ~6,000 draws is roughly
14 ms of draw path + 5 ms of walk-and-wait, and shaving 10% off any one phase is ~0.5-1.5 ms.

**(b) Move work off that thread onto cores that are doing nothing.** Never attempted,
because until part 50 nobody had asked how many cores were in use, and until part 51 nobody
had established that our thread — rather than the guest's — was the limiter. Both are now
answered. The prize is larger than (a) by construction: it is not a percentage of a phase,
it is the whole phase moving off the critical path.

**This plan puts (b) first**, with the explicit caveat that it is the harder and riskier
work, and with the items ordered so that the cheapest parallel win is proved before the
expensive one is attempted.

### What makes a piece of pump work movable

Ask three questions of any candidate. All three must be yes:

1. **Is it pure?** Does it read guest memory and produce a value, without calling Vulkan
   and without mutating renderer state? (The guard: yes. `vkCmdDraw`: no.)
2. **Is its input known before the draw needs it?** The pump discovers work by walking
   packets, so most of it is discovered exactly when it is needed. Anything moved off the
   thread must either be *predictable* (the same buffer as last frame) or *deferrable*.
3. **Is there an oracle for it?** The incumbent serial implementation must be retainable as
   a same-binary arm, so a parallel result can be checked against it rather than trusted.
   **A wrong guard hash is a stale mesh** — the exact defect class part 46 spent a session
   on — so this is not optional.

---

## 2. THE BUDGET, AS MEASURED

### 2a. Phases — the operator's own machine and route (part 51's session, tick100 arm)

Median over the 3,000-8,000 draws/frame band, `CZ_VK_PROFILE` on:

| phase | ms | note |
|---|---|---|
| **draw total** | **14.45** | |
| — `record` | 6.21 | the `vkCmd*` calls |
| — `other` | 3.92 | pipeline lookup, fetch walk, shader bind, begin/tail |
| — `textures` | 3.20 | untile + upload |
| — `constants` | 1.10 | per-draw ALU constant copy |
| — `streams` | 0.03 | **see §2b — this number is a lie of omission** |
| **outside** | **5.37** | the PM4 walk, the guest ISR, and blocking |
| readback | 0.60 | image -> host buffer -> window |
| submit | 0.07 | the GPU is idle; we are CPU-bound |

**Both instruments are excluded from those columns and both were running.**
`CZ_VK_FRAME_STATS` measured **1.86-3.32 ms/frame on the operator's machine** and
`CZ_VK_PROFILE` costs a further 2-4. The frame they actually play at ~6,000 draws is
therefore **~18-19 ms (~53-55 fps)**, not the 24 ms the profiler prints, and not the
28.3 ms this project quoted for twenty parts.

### 2b. Symbols — the pump thread, INSTRUMENTS OFF, and why the phase table is not enough

`tools/part52_recon.sh nostats` — no `CZ_VK_PROFILE`, no `CZ_VK_FRAME_STATS`, sampled in an
outdoor crowd 100 s after the DebugJump lands. **This is the player's frame**, and it is the
first symbol budget this project has taken without an instrument in it:

| symbol | % of the pump thread | with frame stats ON | which phase claims it |
|---|---|---|---|
| **`GuardFold`** | **20.04%** | 16.79% | split between `textures` and `streams` — and `streams` reads **0.03 ms** |
| `DoDraw` | 13.00% | 9.84% | `other` |
| **`BindShader`** | **12.47%** | 6.48% | `other` |
| `UploadStream` | 7.66% | 7.21% | `streams` |
| `memcmp` | 5.96% | 4.57% | state cache / guard comparisons |
| `UploadTexture` | 5.28% | 4.92% | `textures` |
| `WriteRegisterRun` | 4.67% | 2.92% | `outside` |
| `memmove` | 3.78% | 3.78% | the copies |
| `ExecutePacket` | 3.24% | 2.42% | `outside` |
| **`std::map<std::string, uint64_t>::operator[]`** | **2.30%** | 1.56% | **nothing — it is the counter census** |
| `ExecuteLinear` | 1.37% | 1.14% | `outside` |
| `DoSwapImpl` | **absent from the top 11** | **19.40%** | — |

**Read the last row first.** With frame stats on, `DoSwapImpl` was the biggest symbol in the
pump and looked like a headline item ("the present path is a fifth of our thread"). With
frame stats off it drops out of the table entirely. That is the inlining trap measured
rather than argued: the instrument is inlined into the present function, and `perf` charges
inlined code to its container (gotcha 340). **An item was invented and destroyed by one
environment variable**, and the destroying run took six minutes.

The rest of the table, converted to milliseconds — the pump thread runs at 91.2% of a core,
so on a ~20 ms frame it does ~18 ms of work:

| symbol | ~ms/frame at 6,000 draws |
|---|---|
| `GuardFold` | **3.6** |
| `DoDraw` | 2.4 |
| `BindShader` | **2.3** |
| `UploadStream` | 1.4 |
| `memcmp` | 1.1 |
| `UploadTexture` | 1.0 |
| `WriteRegisterRun` | 0.9 |
| `memmove` | 0.7 |
| `ExecutePacket` + `ExecuteLinear` | 0.8 |
| `std::map<std::string,…>` | **0.4** |

Three things follow that the phase table could not have said:

* the renderer's actual draw recording is **an eighth** of the thread;
* the biggest real cost is a **content guard**, and the phase named after the subsystem it
  guards reads 0.03 ms — the cost is charged to `textures` and to `other`;
* **`BindShader` is the third-biggest symbol and appears in no plan this project has ever
  written.** With the instruments off it is 12.47%, nearly as large as `DoDraw` itself.

---

## 3. TIER 1 — MOVE WORK OFF THE CRITICAL THREAD

### Item 1.0 — `BindShader` re-hashes every shader, every bind, every frame (DO THIS FIRST)

**This was found by the recon, it is the third-biggest symbol in the pump at 12.47%
(~2.3 ms/frame), and it is not a parallelism item at all — it is work that does not need
doing.** It leads this plan because it is the largest item with the smallest risk.

**What it does.** `BindShader` in `gpu/pm4.cpp` runs on every `IM_LOAD` packet — **1,919 a
frame** by the opcode census — and for each one it calls `Fnv1a(code, sizeDwords * 4)` over
the *entire* shader microcode. `Fnv1a` is byte-at-a-time with a multiply on the dependency
chain (one `imul` per byte, ~5-cycle latency), so it runs at roughly a byte per cycle. At
12.47% of the thread that is on the order of **9 MB of microcode hashed per frame** — to
recompute, 1,919 times a frame, the same few hundred hashes it computed last frame and the
frame before.

**The constraint that decides the fix, and it is easy to miss.** *The hash cannot be
replaced with a faster one.* It is the SHADER CACHE KEY: `assets/shader_spv/vs_<hash>.spv`,
`tools/build_shader_spv.sh`, the `[imload]` line and the renderer's "no translated shader"
miss report all name shaders by this exact value. Swapping in a wider fold would orphan all
435 cache entries and silently re-translate the world. **So the hash must be avoided, not
accelerated.**

**The fix.** Memoize on `(ucodeVa, sizeDwords)`. The guest re-binds a small working set of
shaders from stable addresses; the `[imload]` line already prints the va and the size, and
a run announces only ~249 distinct shaders. A direct-mapped table of a few hundred entries
turns 1,919 full hashes a frame into 1,919 lookups plus a handful of real hashes.

**The correctness question, asked properly.** A cache on `(va, size)` is wrong if the guest
writes *different* microcode to the *same* address at the same size. That is exactly the
staleness class part 46 spent a session on, so it gets the same treatment rather than an
assumption:

* verify arm `CZ_PM4_VERIFY_SHADER_HASH=1` computes the memoized value AND the real hash and
  reports every disagreement with va, size and both hashes. **Run it to completion on the
  outdoor route and on a full operator session before the memo is trusted**;
* cheap probe in the key: include the first and last dword of the microcode alongside
  `(va, size)`. Two loads, and it catches the overwhelming majority of a re-upload;
* the failure mode if it is ever wrong is loud rather than silent in one useful way — a
  wrong hash means a cache MISS ("no translated shader"), which the standing gate already
  counts, not a wrong shader — *unless* it collides with another real shader's hash, which
  the probe makes vanishingly unlikely.

**Also in the same function, for free while you are there:** the `announced` vector is
scanned linearly on every bind (`std::find` over ~249 entries × 1,919 binds = ~478,000
comparisons a frame) purely to decide whether to print a line that is printed once per
distinct shader. Make it a `std::unordered_set`, or skip the check entirely once the
renderer reports the cache is complete.

**Expected: −1.5 to −2.3 ms. Risk: low**, given the verify arm. **Measure with:** the
`GuardFold`/`BindShader` shares in a `perf` profile (primary), and frame time by draw bin
(three runs an arm) once it lands.

### Item 1.1 — Parallel content guards (THE headline item)

**What.** Hash the vertex/index stream guards on a small worker pool instead of inline in
`UploadStream`.

**Why it is the best item on the board.** It is the largest single symbol in the pump
(16.79%, ~3-4 ms/frame); it is **pure** (reads guest memory, returns a `uint64_t`, touches
no Vulkan and no renderer state); it is **embarrassingly parallel** (each stream is an
independent buffer); and it is **memory-latency bound**, which is the one kind of work that
scales almost perfectly with threads even when the machine's total bandwidth is nowhere
near saturated — four threads waiting on DRAM overlap four misses instead of one.

**The obstacle, stated honestly.** The pump discovers streams as it walks packets, so the
input is not known in advance. Two designs, and the first is much cheaper to try:

* **1.1a — speculative pre-hash from last frame's working set.** `persistCache` already
  holds every (address, size, endian) the renderer has recently seen. At frame start, hand
  the worker pool the entries seen in the previous frame; they hash while the pump walks.
  `UploadStream` then looks up a *completed* hash instead of computing one. On a miss
  (address not pre-hashed, or the pre-hash has not finished) it falls back to hashing
  inline — so correctness never depends on the prediction, only performance does.
  **The prediction is very likely good**: part 22 established that 94-97% of stream bytes
  are byte-identical frame to frame, which means the *set* is stable even when contents
  are not.
* **1.1b — deferred hashing with a copy-on-first-use fallback.** Harder; only if 1.1a's
  hit rate disappoints.

**How to measure it.** `GuardFold`'s share of the pump thread in a `perf` profile (primary,
because it is the quantity being moved), the guard's MB/frame counter (must be unchanged —
the same bytes are still hashed, just elsewhere), and frame time by draw bin (three runs an
arm). **Pre-register the hit rate**: if fewer than ~80% of guard requests are served by a
completed pre-hash, the item is not working and the frame-time result will be noise.

**The control arm and the oracle.** `CZ_VK_NO_PARALLEL_GUARD=1` restores the inline path.
And because a wrong hash is a stale mesh, add `CZ_VK_VERIFY_PARALLEL_GUARD=1`, which
computes BOTH and aborts loudly on a disagreement — the same shape as part 50's
`CZ_PM4_VERIFY_FILLER_POISON`. Run the verify arm to completion on the outdoor route
before quoting any speedup.

**Risk.** Medium. The hazard is not the hash, it is lifetime: a worker reading guest memory
while the guest writes it. That race **already exists** in the inline version (the guard is
racing the guest today and always has been — a torn read produces a different hash, which
reads as "changed", which is safe by construction). Moving it to another thread does not
create a new class of bug; it widens the window. Say so in the code, and keep the fallback.

**Expected: −2 to −3 ms.** It is 3-4 ms of the pump; a 4-worker pool should hide most of it.

### Item 1.2 — Parallel texture untile + upload

**What.** `UploadTexture` is 4.92% of the pump and `textures` is 3.20 ms. Untiling is a pure
address-swizzle over a source buffer into a staging buffer: no Vulkan calls, no shared
state.

**Why second and not first.** The output is a mapped staging buffer that must exist before
the upload command is recorded, so the dependency is tighter than the guard's — the pump
needs the *result*, not just a decision. Prefer the same speculative shape: textures whose
guard says "unchanged" need no work at all (part 47's finding), so the pool only handles
the ones that changed, and the pump waits on a future only for the texture it is about to
bind.

**Expected: −1 to −2 ms.** **Risk:** medium-high (staging allocation from two threads).

### Item 1.3 — Move the present readback off the pump

**What.** `DoSwapImpl` copies the resolved image into host memory and hands it to the
window. `readback` is 0.60 ms of profiler time, but the *symbol* table shows `DoSwapImpl`
at 19.4% — almost all of it the frame-stats instrument, so **do not price this from the
symbol table** (that is the inlining trap, gotcha 340). Price it from `readback` plus the
`memcpy` it does, with frame stats OFF.

**Why it is still worth doing.** It is the simplest possible parallel win: the copy has no
dependency on anything the next frame does. But it is ~0.6-1.0 ms, not the 1.2 the old
plan guessed, and the old plan's framing ("present without the readback", i.e. use a real
swapchain) is a different and much larger job — see §7.

**Expected: −0.5 to −1.0 ms. Risk:** low.

### Item 1.4 — The strategic question this tier raises but does not answer

If 1.1 and 1.2 work, the next question is whether the **Vulkan recording itself** can be
parallelised with secondary command buffers. That is a genuine architectural project (draw
order must be preserved; state inheritance across secondaries is a minefield; the PM4 walk
that discovers the draws is inherently serial). **Do not start it in part 52.** Finish tier
1's cheap items first and re-measure: if the pump's remaining work is dominated by
`record`, the case is made by the numbers rather than by ambition.

---

## 4. TIER 2 — THE INSTRUMENTS ARE STILL ON THE HOT PATH

This tier is small in milliseconds and enormous in credibility, and part 51's experience
says it should be done EARLY rather than last: two of the last three parts found an
instrument in their own measurement.

### Item 2.1 — 28 `Count("...")` calls inside `DoDraw`

**Evidence.** `std::map<std::string, uint64_t>::operator[]` is **1.56% of the pump thread**.
The code already knows about this — the `COUNT(lit)` macro exists precisely to resolve a
counter's address once per call site — and it is used at 38 sites while 94 plain `Count(`
sites remain, **28 of them inside `DoDraw`'s body.**

**Fix.** Convert the hot ones. Mechanical, no behaviour change, and `VkRenderer_DumpStats`
cannot tell the difference by construction.

**Expected: −0.2 to −0.4 ms. Risk: none.** Verify by diffing the counter dump between arms:
every name and every value must match.

### Item 2.2 — Make `CZ_VK_FRAME_STATS` affordable, or make it sampled

**Evidence.** ~3 ms/frame, 12-20% of the window, measured on two machines (part 51 §6ch §6
and §7). It zeroes a 2 MB bitmap and walks 921,600 pixels **per presented frame**.

**Why it matters even though players never set it.** Every A/B this project runs carries
it, so it inflates every absolute number and — worse — it changes the *shape* of the frame:
3 ms is enough to push a frame off a vblank rung, which is the statistic parts 47-51 read
their results from (the pinned share). An instrument that can move the statistic it
reports is a gotcha-7 instrument.

**Fix, in order of preference:** (a) sample every Nth presented frame (`CZ_VK_FRAME_STATS_EVERY=N`,
default 1 for compatibility) — the era medians this project actually reads do not need
every frame; (b) drop the exact distinct-colour count, which is what the 2 MB bitmap is
for, or make it opt-in; (c) SIMD the luma/lit loop.

**Expected:** removes ~3 ms from every measured frame — **a measurement correction, not a
speedup** (gotcha 337). Do not bank it as a win. Do quote the corrected baseline once.

### Item 2.3 — Audit the always-on censuses

`g_streamCensus`, the packet census, the dimension census, `snapshotsSampledThisPass`.
`g_passInputsWanted` already exists as the pattern: ask once whether anything will read a
census, not per event. Sweep for the rest.

**Expected: −0.1 to −0.3 ms. Risk: none**, provided each census keeps working when armed.

---

## 5. TIER 3 — THE DRAW PATH, WHERE THE MILLISECONDS ACTUALLY ARE

### Item 3.1 — MOVED. `BindShader` is now item 1.0; see §3.

### Item 3.2 — The pipeline lookup: `std::map` -> flat hash

Inherited from part 50's item 3 leftovers and still real. `R->pipelines` is a `std::map`
keyed by a struct; a red-black tree walk per draw is cache-hostile where an open-addressed
table is one probe.

**Expected: −0.3 to −0.7 ms. Risk: low** (a container swap with identical semantics).

### Item 3.3 — `_int_malloc` on the frame path (1.20%)

Something allocates per draw or per frame. Find it with the DWARF call graph (§8) and give
it a reusable buffer. **Expected: −0.2 to −0.3 ms. Risk: low.**

### Item 3.4 — `memcmp` at 4.57%

Where? Almost certainly the state cache comparing bind arrays. If the arrays are small and
fixed-size, a hand-rolled compare beats a call; if they are large, the cache key is doing
too much work. **Measure before touching** — the state cache is a part-47 win and this
could be its cost of doing business, in which case leave it alone and say so.

---

## 6. TIER 4 — THE PM4 WALK AND THE GUEST HAND-OFF

### Item 4.1 — `outside` is 5.37 ms and is no longer mostly sleep

Part 51 removed ~2.7 ms of sleep from it. What is left is the walk itself
(`ExecutePacket` + `ExecuteLinear` + `WriteRegisterRun` = 6.48% of the pump), the guest's
vblank ISR, and genuine blocking. **Re-split it before working on it** — the same move that
found the sleep. In particular, `outside` still contains time the pump is BLOCKED, and
blocked time is not work (part 50 §6cg §7 point 3).

### Item 4.2 — Item 1c from the old plan: inline the walk

Priced by measurement at **~2.2 ms ceiling** (part 50: one `ExecutePacket` call is 24-40 ns
at 74,767 packets/frame). It has **no single lever** — call, fetch, census read, two counter
updates, switch, return — so it is a refactor of the whole walk with real desync risk. Both
PM4 oracles are blind inside `ExecutePacket`, so the incumbent walk is the oracle and it
needs a poison arm.

**Take this only after tiers 1-3.** It is the highest-risk item with a mid-sized payoff.

### Item 4.3 — Does the guest simulate per PRESENT or on its own timer?

Old item 1d, still unanswered and now cheap: `CZ_FPS_CAP` makes it a one-run experiment
(compare guest-thread CPU per SECOND at 30, 45, 60). If the guest's work scales with our
frame rate, then every millisecond we save costs some of itself back — which would change
how every item above is priced.

---

## 7. EXPLICITLY NOT IN THIS PLAN

* **Soft-dirty page tracking for the stream guard.** Refuted twice in part 51: arming costs
  7.5 ms/frame and write-protects every page (+773 ns/page of faults charged to the guest's
  threads). **Struck, not deferred.**
* **`other`'s residual.** It was the profiler measuring itself (part 50). There is no frame
  time there.
* **Hoisting the ring-wrap modulo.** `INDIRECT_BUFFER` is 43-46 packets/frame; every other
  packet is fetched with `wrapDwords == 0` already.
* **A faster guard hash.** §0 fact 3: it is memory-bound at ~10 GB/s. SIMD buys nothing.
  (If you disbelieve this, the cheap test is to run the guard over a *hot* 1 MB buffer and
  compare — 35.7 GB/s hot against ~10 GB/s in situ is the whole argument.)
* **A real Vulkan swapchain.** Worth doing eventually, but it is a presentation
  architecture change, not a CPU item, and item 1.3 gets most of the CPU benefit.
* **Pinning the GPU clock.** Retracted in part 30; sample it, do not pin it.

---

## 8. THE RECON THAT MUST COME FIRST (and is already scripted)

`tools/part52_recon.sh` takes, per run: a flat `perf` profile, a DWARF call-graph profile,
and per-thread CPU — in three configurations:

| tag | instruments | what it answers |
|---|---|---|
| `nostats` | none | **the honest symbol budget** — what the player's frame contains |
| `stats` | `CZ_VK_FRAME_STATS` | the instrument's own footprint, by difference |
| `phases` | `CZ_VK_PROFILE` only | the phase budget without the frame-stats bill in it |

The DWARF profile is the one part 51 could not do, and it is what turns three of the items
above from "a symbol is hot" into "this call site is hot": it attributes `GuardFold` to the
texture guard versus the stream guard (different items, different fixes), names which
`Count(` sites are hot, and finds who calls `_int_malloc`.

**Read the recon before touching any item in this plan.** Two of part 50's items and one of
part 51's were repriced or killed by exactly this step.

---

## 9. MEASUREMENT PROTOCOL — the rules that have cost this project the most

1. **Three runs an arm, alternated, one pinned binary.** The frame-time noise floor on this
   route is 8-18% by draw band at one run a side. An item worth under ~1 ms is invisible in
   frame time and must be settled on a per-unit statistic (ns/packet, ns/draw, MB/frame,
   or a symbol share).
2. **Run the null control in the same block** — the same configuration against itself — and
   quote every effect as a multiple of it (gotcha 331).
3. **Build a positive control for anything whose mechanism is in doubt.** Part 51's tick
   result is quotable only because the 4 ms arm made the frame *worse* by the predicted
   amount. An arm that can only help is a hope.
4. **Say which instruments were on.** Every absolute frame time in this repo before part 51
   is inflated by ~3 ms of frame stats, and by a further 2-4 if profiled.
5. **Medians and the vblank-pinned share, never means.** A mean measures this title's
   pacing floor. The share of frames within 1 ms of a 16 ms multiple is the sensitive
   statistic (10% -> 97% where a mean moved 1.7%).
6. **A saving converts to frame rate only between floors.** Below one vblank rung it is
   absorbed. Quote the draw band.
7. **The operator's frame is not the headless frame** — historically ~2x, though part 51's
   session measured them much closer. Confirm anything shippable on their machine, and ask
   about SMOOTHNESS as well as speed for anything that changes *when* work happens.

---

## 10. SUGGESTED ORDER

| # | item | expected | risk | why here |
|---|---|---|---|---|
| 0 | **§8 recon** — DONE for `nostats`/`stats`, finish `phases` | — | none | it has repriced or killed an item in each of the last three parts, and it produced items 1.0 and 2.1 of this one |
| 1 | **1.0 `BindShader` memoization** | **−1.5..2.3 ms** | low | the biggest item with the smallest risk, and it is work that does not need doing at all. Verify arm first |
| 2 | 2.1 `Count` on the draw path | −0.3..0.4 ms | none | mechanical; and it takes an instrument out of the measurement before anything else is measured |
| 3 | 4.1 re-split `outside` | — | none | one instrument, and it is how the last two parts found their biggest items |
| 4 | **1.1 parallel guards** | **−2..3 ms** | medium | the largest single symbol. Verify arm before any claim |
| 5 | 1.3 readback off-thread | −0.5..1 ms | low | simplest parallel win; re-price it from `readback` with frame stats OFF |
| 6 | 3.2 pipeline map -> flat | −0.3..0.7 ms | low | |
| 7 | 3.3 `_int_malloc` on the frame path | −0.2..0.3 ms | low | the call graph names the caller |
| 8 | 2.2 frame-stats sampling | correction | none | before the next campaign, not after |
| 9 | 1.2 parallel textures | −1..2 ms | med-high | only after 1.1 proves the pool |
| 10 | 3.4 `memcmp` at 5.96% | ? | — | **measure before touching**; it may be the state cache's legitimate cost |
| 11 | 4.2 inline the PM4 walk | −2.2 ms ceiling | **high** | last, and only with a poison arm |

### What this adds up to, and the honest ceiling

At ~6,000 draws the pump does ~18 ms of work per frame. Items 1.0, 2.1, 1.1 and 1.3 alone
are **~5-7 ms of it**, and none of them is an architecture change:

| | ~ms |
|---|---|
| 1.0 `BindShader` memo | 1.5-2.3 |
| 1.1 parallel guards | 2.0-3.0 |
| 1.3 readback off-thread | 0.5-1.0 |
| 2.1 + 2.3 instrument cleanup | 0.4-0.7 |
| **subtotal** | **4.4-7.0** |

That would take the operator's ~18-19 ms frame at 6,000 draws to **~12-14 ms**, i.e. from
~53 fps to the 60 fps cap in everything but the heaviest crowds. Tier 3's smaller items add
another 1-2 ms on top.

**Beyond that lies real architecture** — parallel command recording (item 1.4) and a true
swapchain — and neither should be started until the cheap items above have been taken and
re-measured. Part 51's lesson is that the next item is usually found by an instrument, not
by ambition: **three consecutive parts found their biggest result outside the table they
were working from.**

---

## 11. A STANDING WARNING FOR WHOEVER PICKS THIS UP

Every number in this document has a shelf life (gotcha 13), and the ones most likely to be
stale are the ones that look most solid:

* the symbol table is **one 40-second window of one route on one machine**. Re-take it
  (`tools/part52_recon.sh`) before trusting the ranking, and especially before concluding
  that something is *not* worth doing.
* the phase table is the operator's machine and the headless route differs from it —
  historically by about 2x, though part 51's session measured them much closer than that.
* **every millisecond figure here is a share of a thread whose total changes as items
  land.** After item 1.0 ships, `GuardFold`'s *share* will rise without its cost changing.
  Convert to milliseconds against a measured frame time before comparing two profiles taken
  at different times (gotcha 320).
