# The CPU plan: a crowd frame is 75% our own code, in two equal halves

Successor to `docs/perf-plan-overnight.md`, which is finished. That plan's subject was a
frame whose largest term turned out to be a `sleep_for`; this one's subject is what is
left after the sleep, the vblank and the GPU clock were fixed (`docs/phase5-notes.md`
§§6aj-6an).

**Read §6an first.** The one-line summary: ordinary gameplay is pinned at 31 fps by the
title's own two-vblank pacing and needs nothing; **a Still Creek zombie crowd runs at
22-25 fps and is CPU-bound in our runtime**, at 6,592 draws and 43.4 ms:

| term | ms | µs/draw | share |
|---|---|---|---|
| **renderer draw path** | **21.40** | **3.25** | **49.3%** |
| **PM4 command-processor walk** | **10.98** | **1.67** | **25.3%** |
| GPU fence wait (at 1950 MHz) | 6.64 | 1.01 | 15.3% |
| pump sleep | 3.86 | — | 8.9% |
| readback | 0.56 | — | 1.3% |

Both big terms scale linearly with the draw count and neither has ever been optimised,
because until the operator session of 2026-08-08 nothing in this port had profiled a
scene with more than ~1,930 draws in it.

### The SPLIT of the draw path in §1 was wrong, and part 20 measured it again

The table above stands: the draw path is ~half a crowd frame and the PM4 walk is ~a
quarter. **What was wrong is how the 21.40 ms divides between §1's four items**, because
`ProfScope` accumulated INCLUSIVE time and its scopes nest — `record` opens partway down
`DoDraw` and lives to the end of it, so `streams` ran inside it and was counted in both.
The residual was then computed by a subtraction that removed `streams` twice. Fixed in
part 20; the mechanism, and why a defect like it is invisible, is in the commit and in
the `ProfScope` comment.

Converted, and then re-measured on the part-20 binary at 6,737-6,806 draws a frame
(the headless outdoor recipe, GPU at its own governed clock — **P5, mean 524 MHz, 32%
utilisation**, which is where this workload leaves it and is NOT the 210 MHz this
project had been quoting; see §3 and gotcha 219's retraction):

| draw-path term | as §1 had it | corrected | re-measured, 6.8k draws |
|---|---|---|---|
| `record` — the `vkCmd*` calls | **11.07 ms** | 6.30 | **6.7 ms** (12.3%) |
| `other` — `DoDraw`'s untimed work | **0.91 ms** | 5.68 | **5.6 ms** (10.2%) |
| `streams` — the dword-swap copy | 4.77 | 4.77 | 3.7 ms (6.8%) |
| `textures` — untile + upload | 3.43 | 3.43 | 2.7 ms (4.9%) |
| `constants` — the ALU block copy | 1.22 | 1.22 | 1.3 ms (2.3%) |
| whole draw path | 21.40 | 21.40 | 19.9 ms (36.5%) |

**§1d — `other` — is therefore the SECOND biggest item in the draw path, not the
smallest**, and this document filed it as "the cheapest item in this document" on the
strength of the broken number. §1a's `record` is under a third of the draw path rather
than a half. Both sections are ranked by expected size, so read the ranking below
against this table and not against its own prose.

The PM4 walk is unaffected and re-measures at **11.8 ms (21.6%)**, because it was always
derived by subtracting the renderer's inclusive total from the pump's walk — a quantity
the defect did not touch. `[vkprof]`'s pump line now prints it as a `pm4` column rather
than leaving it to be worked out by hand.

**The noise floor of a single crowd run is 10-13%, measured rather than assumed.** Two
runs of one binary do not visit the same places for the same durations (the recipe is 57
fixed 8 s steps against a boot whose depth in wall time is a distribution), so
`tools/frame_perf_bins.py` compares frames BINNED BY DRAW COUNT instead of averaging a
run. That is necessary and **it is not sufficient**: a null arm — the part-20 instrument
fix, which changes nothing that executes — moved individual crowd bins by 10-13%, with
the tool's own standard-error column reading up to 22 sigma. Consecutive frames in a bin
share a camera, a location and a thermal state, so they are nowhere near independent
samples and any significance computed from the raw frame count is confidently wrong
(gotcha 229).

**Three runs an arm, alternated a/b/a/b/a/b, and read the per-run spread before the
delta.** That is ~1 hour of wall time per A/B on this machine. Part 20's measured
example, the mean of every frame with >= 6,000 draws: arm A 55.30 / 53.00 / 53.17
against arm B 48.35 / 48.51 / 46.89 — **−11.0% with no overlap between the arms**,
which is a result, where either arm alone would have been a coin flip.

---

## 0. ~~FIRST, AND IT BLOCKS EVERYTHING ELSE~~ **DONE (part 19): the recipe is in
## `CLAUDE.md` and it reaches 6,400-8,100 draws a frame**

The change was smaller than this section feared: one extra `B` press to open the
safehouse door, then alternating `LSUP` with `RSRIGHT`/`RSLEFT` to walk Chuck into the
junkyard behind it and sweep the camera. 57 entries at the existing 8 s interval; the
world arrives at ~300 s. The acceptance test is the number, as prescribed:
`awk 'NR>1 && $2>m {m=$2} END {print m}' frame_stats.txt` reads 7,175-8,130 across four
runs.

The warning below still stands and is worth more now, not less: it is 57 fixed 8 s steps
against a boot whose depth in fixed wall time is a distribution, so **read the draw count
off every run before quoting anything measured on it**. §1 and §2 are unblocked.

Kept below as written, because its argument for why this had to come first is the
reusable part — and it was right twice over: the same recipe is what finally reproduced
the view-dependent whole-frame black, which had been an operator report for six parts
(`docs/phase5-notes.md` §6ap).

---

## 0-original. FIRST, AND IT BLOCKS EVERYTHING ELSE: a headless recipe that reaches a crowd

Every number above came from an operator playing. The existing recipe in `CLAUDE.md`
reaches live gameplay headlessly but renders ~1,930 draws — **it never enters the
workload this plan is about**, so with it every A/B here would be run on a frame sitting
against the vblank cap, where CPU savings are invisible by construction. That is exactly
how the state cache first measured as a dead heat.

`CZ_FAKE_PRESS_SEQ` already has stick entries (`LSUP/LSDOWN/LSLEFT/LSRIGHT` walk Chuck,
`RSUP/...` aim the camera, and a stick entry HOLDS for its interval). The job is to
extend the sequence until a headless run reports **>4,000 draws/frame** in
`CZ_VK_FRAME_STATS`, then pin that recipe in `CLAUDE.md` beside the existing one.

Gotcha 190's rule, one more time: a measurement that needs a human is a measurement
nobody will repeat. **Do not start §1 or §2 before this exists** — and note the acceptance
test is a number in the frame stats, not a screenshot, so it is self-checking.

Watch for: the sequence is fixed 8-second intervals against a boot whose depth in fixed
wall time is a distribution (gotcha 75), and the game now runs **2.5x faster** than when
the current recipe was written, so its press timings are already suspect. Re-derive, do
not extend blindly.

---

## 1. The renderer draw path — 21.4 ms, 3.25 µs per draw

Ranked by (expected size) x (confidence), each with the measurement that settles it
BEFORE any code changes. The whole overnight session is the argument for that ordering:
its prime suspect was 0.5% of the frame and the real cause was a sleep nobody had timed.

### 1a. ~~`record` is 11.07 ms~~ **`record` is 6.7 ms** — 1.0 µs for ~5 remaining vkCmd calls

**Read the corrected table at the top of this file before this section.** `record` was
never 11.07 ms; that number contained the whole of `streams`. At ~200 ns per remaining
`vkCmd*` this is close to what a driver call should cost, which weakens the premise the
four hypotheses below were built on — and hypotheses B and C turned out to be in `other`
(§1d) rather than in `record` at all, because the code they name sits above the `record`
scope. Part 20 acted on B and C for the reason B was always filed under: instrumentation
overhead inside the thing being instrumented is worth removing whatever column it lands
in.

~~That is ~340 ns per call, where a `vkCmd*` on this driver should be 50-150 ns.~~ At the
corrected 6.7 ms over ~6.4 calls a draw it is **~155 ns per call**, which is inside the
range this section expected and removes the premise that "something around them" must be
costing. What remains per draw after the state cache: `vkCmdBindVertexBuffers` (once per
binding, ~3.0 of them), `vkCmdBindIndexBuffer` (~0.9), `vkCmdPushConstants` (24 bytes),
`vkCmdDrawIndexed`.

* **Hypothesis A — the vertex/index binds repeat too.** A crowd is many copies of a few
  zombie meshes, and `UploadStream` already caches per frame by (address, size, endian),
  so consecutive draws may be binding the *same buffer at the same offset*. The state
  cache deliberately stopped short of these; extending `Renderer::BoundState` to them is
  a dozen lines. **Measure first:** add skip counters exactly like the existing ones and
  run WITHOUT acting on them — if the repeat rate is low this is dead, and the counters
  cost nothing to leave in.
  **DONE (part 20), and it is about a third true.** In the crowd era **34% of vertex
  binds and 22% of index binds** repeat the previous offset (23.7% / 4.8% in the
  safehouse era, so quote the crowd figures). That is ~1.3 of the ~6.4 `vkCmd*` calls a
  draw, **~1.4 ms of a 54 ms frame at ~155 ns a call — 2.5%**. Real, permanently below
  this workload's noise floor, so it could only ever be claimed from the counter. The
  counters are in the `CZ_VK_STATS` block as "binds NOT cached" and are reset exactly
  where `BoundState` is. Not acted on, as this section asked.
* **Hypothesis B — `Count()` is on the hot path.** `perf` at 1,930 draws already showed
  `std::map<std::string>::operator[]` at 0.44% and `__strncmp_avx2` at 0.75%; at 6,600
  draws that is ~3x, and `DoDraw` calls it several times per draw. Every call constructs
  a `std::string` and does a red-black tree walk. **This is the one item here that needs
  no measurement to justify** — it is instrumentation overhead in the thing being
  instrumented, the same defect that made the state cache measure as a dead heat. Convert
  to an enum-indexed `uint64_t[]` with the names in a static table; `g_stats` keeps its
  printing interface.
* **Hypothesis C — `getenv` on the hot path.** `perf` showed `getenv` at 0.42%. Every
  `Env("...")` that is not behind a function-local `static const` is a `strncmp` walk of
  the environment per draw. **Measure:** `grep -n 'Env(\|getenv' runtime/gpu/vk_renderer.cpp`
  and check each is cached; `pm4.cpp` has at least one per-tick `getenv` too.
* **Hypothesis D — push constants per draw are unnecessary.** The 24 bytes are three
  arena addresses, and `constants` allocates a fresh arena block per draw whether or not
  the values changed. If consecutive draws share identical constant blocks, both the
  allocation and the push could be skipped. **Measure:** hash the constant block per draw
  and count repeats before writing anything.

### 1b. ~~`streams` is 4.77 ms — 0.72 µs per draw~~ **MEASURED (part 21): 5.6-5.9 ms, it
### is REAL COPYING, and the fix is the cache's LIFETIME**

The per-frame dword-swap copy, cached by (address, size, endian) in an
`unordered_map<uint64_t, VkDeviceSize>`. This section used to say the ambiguity could not
be resolved by reading the code and had to be counted. It was counted —
`CZ_VK_STREAM_CENSUS=1|2`, `docs/phase5-notes.md` §6at — and it resolves to the *second*
reading, not the "nearly all cache hits" one this text expected:

| at ~6,400 draws in a ~50 ms crowd frame | |
|---|---|
| hit rate WITHIN a frame | 93.6-94.0% |
| misses per frame | ~2,000, average 37 KB |
| **bytes copied per frame** | **74-77 MB** |
| `streams` | 11.3-11.7% = **5.6-5.9 ms** |
| **copied bytes repeating LAST frame's key** | **95-97%** |
| of repeated keys, content unchanged | 99.9984% (164 changed of 10,154,820) |

vertex bindings 61-63 MB, dependent fetches 11 MB, index buffers 1.8 MB.

Both halves of the old guess were wrong in an instructive way. The hit rate IS high — but
a high hit rate and a low byte cost are different claims, and 94% hits still leaves
2,000 misses copying 74 MB. And `ProfScope(streams)` wraps only the `CopySwapped`, so a
hit never touched this column at all: the lookup cost was in `other` the whole time.

**The fix is a cache that outlives the frame**, worth ~5.5 ms of a crowd frame (≈11%).
It is NOT a cheaper key. It needs its own storage (the arena is reset every swap) and it
needs **invalidation** — 0.0016% of repeated keys do change in place, hashing to detect
that costs what the copy costs, so the candidate is guest-page write tracking. Budget a
session; §6at states the three requirements.

**BUILT IN PART 22 AND THIS ITEM IS CLOSED — `streams` is now 0.0%** (§6av, open-items
0a). Three corrections this section earned, all worth reading before working §1c or §1d,
because each one changes how the numbers above should be read:

* **Invalidation did NOT need guest-page write tracking.** Extending the census to name
  the rewritten streams showed all 30 are exactly 80 bytes, so a guard hashing up to
  512 bytes exactly covers the observed population. `mprotect` was never built.
* **The 0.0016% understated the risk by two orders of magnitude** (gotcha 235). It is a
  frame-to-frame number and a persistent cache compares against the last COPY; the store
  catches **~20 stale streams a frame**. Do not reuse that percentage for anything with a
  lifetime longer than one frame.
* **The saving does not all land in the frame, and part of it does not land in `streams`.**
  The guard is charged to `record` (gotcha 238), and what remains is absorbed by the
  title's vblank floor except in the band where it crosses one (gotcha 237): 44 ms -> 32 ms
  at ~3,700 draws, ~nothing at ~6,500. **Net draw path at ~6,000 draws: 13.9 ms -> 9.2 ms.**

**What this does to the ranking below, stated carefully, because the obvious phrasing is
wrong.** `textures` (§1c) did **not** get promoted by the store — it was already the
second-largest draw-path term before it, behind `streams`, and it is still second, behind
`record`. It did not move at all: 6.4% -> 6.5% of the frame, ~3.1 ms either way. Matched at
~6,100 draws in a 48 ms frame:

| | store OFF | store ON |
|---|---|---|
| `streams` | **11.0% — 5.28 ms** | 0.0% |
| `textures` | 6.4% — 3.07 ms | 6.5% — 3.12 ms |
| `record` | 5.3% — 2.54 ms | **7.1% — 3.41 ms** |
| `other` | 4.7% — 2.26 ms | 4.8% — 2.30 ms |
| `constants` | 2.4% — 1.15 ms | 2.5% — 1.20 ms |
| **draw total** | **14.3 ms** | **10.0 ms** |

Two things did change and neither is a re-ranking: the gap to first closed (textures was
58% of the largest term, now 92% of it, near enough tied with `record`), and it is a
bigger slice of a smaller draw path (21% -> 31%). So it is worth attacking now for the
same reason it always was, only with less above it.

**And §1c's own open question is now ANSWERED — it is lookup, not upload.** §1c below says
to measure uploads per frame against fetches per frame. Done, one outdoor run:
**166,715,853 `UploadTexture` calls and 2,387 actual uploads — 0.0014%** (74.2% cache hit,
22.9% depth snapshot, 1.5% resolve snapshot, 1.4% not a texture). `ProfScope(textures)`
wraps the whole function, so the 3.1 ms is ~13,900 calls a frame at ~223 ns of six-dword
FNV hash plus `unordered_map` find. open-items **0a-ii** is that item; **0a-i** —
vectorising `CopySwapped` — is retracted, because there is nothing left for it to swap.

### 1c. `textures` is 3.43 ms — and it is 7.9-10.9% in crowds against 2.5% in ordinary
gameplay

That rise is the tell: crowds stream textures. The cost is untile + upload, plus a cache
lookup per fetch. **Measure:** uploads per frame versus fetches per frame
(`CZ_VK_TEX_CENSUS=1` already has the columns). If uploads are near zero and this is
still 3.4 ms, it is the lookup path and belongs with 1b; if uploads are frequent, it is
real streaming work and the question becomes whether the untiler is efficient.

### 1d. ~~`other` is 0.91 ms~~ **`other` is 5.6 ms and it is the second biggest item here**

The 0.91 ms was the profiler's nested-scope defect (see the top of this file): the
residual was computed by a subtraction that removed `streams` twice. Corrected and
re-measured, `DoDraw`'s own untimed work is **5.6 ms of a 54.5 ms crowd frame, 10.2%**,
second only to `record` in the draw path and larger than `streams` or `textures`.

What is in it: the two shader-map lookups, the pipeline-key build and its
`std::map` lookup, the fetch-constant walk, the register decode, and — until part 20 —
several counters and diagnostics that ran on every draw whether or not anyone had asked
for them. That last group is hypotheses B and C below, which were filed under §1a on the
assumption that `record` was where the time was; they are not in `record` at all, they
are here.

A term this size still deserves splitting further before anyone optimises inside it:
one `ProfScope` each around the shader lookups, `GetPipeline`, and the fetch-constant
walk would say which third it is, and it costs three lines. But note the overhead
budget honestly — a `ProfScope` is two `clock_gettime` calls at ~25 ns, so three more
scopes on a 6,600-draw frame is ~1 ms of instrument on a 5.6 ms term (gotcha 7). Either
accept that and state it, or sample.

---

## 2. The PM4 walk — 11.8 ms, and it is a REGISTER-WRITE LOOP

**Answered in part 20, at the census level.** This section used to say "completely
uninstrumented inside", and its own advice was to split it with `ProfScope`s. Do not:
a scope is two `clock_gettime` calls at ~25 ns against a packet that costs ~138, so the
instrument would report mostly itself (gotcha 7). A COUNT is free, and it turned out to
be enough.

`[vkprof]` now prints, per reporting window:

```
[vkprof] pump 705 ticks (3.37/frame) | sleep 7.4% walk 92.6% [pm4 26.0] ...
[vkprof] pm4 18876153 packets (90316/frame, 138 ns each) | 170339227 register dwords
         (815020/frame, 9.0/packet)
```

At **6,876 draws a frame, 48.0 ms** (part-20 binary, GPU at its governed P5/~524 MHz):

| | per frame | per draw |
|---|---|---|
| PM4 walk's own cost | **12.5 ms** (26.0%) | 1.8 µs |
| packets executed | **90,316** | 13.1 |
| register-write DWORDS | **815,020** | 118.5 |
| cost per packet | 138 ns | |
| **cost per register dword** | **15.3 ns** | |

**815,000 register writes a frame is the whole of it.** `WriteRegister` was this
section's leading suspect on the grounds that a crowd "carries a great many"; it carries
nine dwords for every packet in the stream, and at 15.3 ns each they account for
essentially all 12.5 ms. Everything else in the walk — the header decode, the opcode
switch, the draw-sink dispatch — is sharing 13 packets a draw between them.

So the target is named, and the work is per-dword rather than per-packet. What is in
`WriteRegister` per call: a bounds test, a two-sided const-watch range test on two
globals, the store into `g_regs`, and a two-sided scratch-register range test. Plus the
caller's own `GuestLoad32` (a `memcpy` and a `bswap`) per dword — and for
`LOAD_ALU_CONSTANT` that read is from GUEST MEMORY, so a crowd frame streams ~3.3 MB
through the cache to fill the register file. Candidates, in the order their size can be
argued:

* **The guest-memory stream is 3.3 MB a frame and cannot be removed**, only made
  cheaper. `LOAD_ALU_CONSTANT` copies a contiguous run of dwords; a bulk byte-swapping
  copy is one pass over the source instead of a call per dword.
* **`ExecutePacket` does 2-4 `lock`ed atomic increments per packet** — `g_packets`,
  `g_types[]`, `g_opcodes[]`, `g_draws`. `gpu/pm4.h` states in its own header that
  "everything here runs on one thread (the vblank pump)" and that the counters are
  atomic only so a future tracer can read them safely, which makes a relaxed
  load-add-store equivalent and a plain `add` instead of a `lock xadd`. At ~7 ns each
  and 90,316 packets a frame that is ~2 ms — worth having, and cheap to arm and revert.
* **`g_regs` is larger than L1.** Whether the register file's own footprint is costing
  anything is measurable with a hardware counter rather than argued.

NOT a candidate, checked and dismissed: `Source::operator()` does a `%` per dword, but
only at RING level — `ExecuteLinear` constructs its `Source` with `wrapDwords = 0`, and
essentially every packet is inside an indirect buffer. The division is not on the hot
path.

---

## 3. What NOT to do

* **Do not optimise anything measured at ~1,930 draws.** That frame is against the
  two-vblank cap; savings there are structurally invisible, and a change that measures
  as zero will be discarded when it was actually worth 5% in the workload that matters.
* **Do not touch the pump tick, the vblank deadline, the ring brake, the per-CPU ISR
  acknowledge or the vblank gate.** They are the title's frame pacing and cost 40 runs
  to establish (parts 5-6, 18).
* **Do not put a `Count()`, an `Env()` or a `std::string` on a path you are timing.** It
  has now caused two false results in this project in one day.
* ~~**Do not quote a frame rate measured with the GPU at stock clocks.**~~ **DO NOT PIN
  THE CLOCK. Sample it and quote it** (`tools/gpu_clock_sample.py`). The P8/210 MHz
  reading gotcha 219 was built on came from an overnight session with the MONITOR
  ASLEEP. With the display awake this runtime runs at **P5, mean 524 MHz, 32%
  utilisation, 28.6 W** across a full crowd run — statistically where `vkcube` settles on
  the same machine. Pinning to 2100 MHz costs 52.8 W against 28.6 to finish work the
  frame is not waiting on, and it makes every number describe a machine no player runs.
  It is only legitimate when the governor is demonstrably wrong, which means a low clock
  at HIGH utilisation; ours is a low clock at LOW utilisation, and that is the governor
  being right (gotcha 231).
* **Do not expect ordinary gameplay to move.** 31.2 fps is the title pacing itself at its
  console target. Only crowds can improve, and only the two CPU halves above.

---

## 4. What success looks like

A crowd frame is 43.4 ms. The GPU is 6.6 ms of it and the pacing is 3.9 ms, so **the
floor without touching either is about 11 ms — 90 fps** and utterly unreachable. A
realistic target is halving the two CPU halves: 21.4 + 11.0 = 32.4 ms becomes ~16 ms,
frame ~27 ms, **crowds at ~35 fps** — i.e. above the title's own cap, which would put
every scene in the game on the 31 fps pacing rather than below it.

That is the honest goal: **not "faster", but "never below the title's own frame rate".**
If §1 and §2 together cannot get the CPU halves under ~20 ms combined, say so and stop —
the remaining structural option is a second thread for command recording, which is a much
larger piece of work and should not be started on an assumption.
