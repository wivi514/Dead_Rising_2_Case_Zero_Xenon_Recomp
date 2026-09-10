# Perf plan, part 109 — 120 fps on the OPERATOR's CPU

**The instruction (2026-09-10, the operator going to sleep):** *"I want to try to get the
cpu ms to be able to do 120 fps on my cpu that's the goal if possible ... do an overnight
plan."*

Read `docs/perf-plan-part107.md` first: its §0.3 decomposition and §2 item list are this
plan's parent, and items 4-6 there were never run. This plan re-targets them from "60 on
a Ryzen 3 3100" to "**the CPU frame under 8.33 ms on a Ryzen 7 5700**", which is a
different question with a different order.

## §0. The arithmetic, and what "120 fps on my CPU" means

120 fps = **8.33 ms**. The goal is explicitly the CPU half: the frame must not be
CPU-bound at 120 fps. What the GPU then does at 3440x1440 is a separate question the
operator did not ask.

**The box:** Ryzen 7 5700, 8 cores / 16 threads, Zen 3, **cap restored to 4654 MHz**
(checked 2026-09-10, `scaling_max_freq` = 4654000 — this closes part 108 kickoff §1b item
0), governor `powersave`, RTX 3070 at 2100 MHz.

**Baseline, 2026-09-10, three runs, `tools/part80_crowdroute.sh`, mirror ON, MSAA 2x,
profiler OFF, at the operator's own 3440x1440:**

| run | crowd windows | median | p99 | draws |
|---|---|---|---|---|
| 1 | 22 | 11.39 ms | 14.15 | 9,657 |
| 2 | 22 | 10.72 ms | 13.92 | 9,036 |
| 3 | 22 | 11.42 ms | 14.50 | 9,329 |

**11.2 ms median at the crowd = 89 fps.** That is wall, at their native resolution, so it
is CPU and GPU together. The 1080p pair below is what separates them, because part 106
put the GPU at 4.0 ms there against a 10.3-10.6 ms wall — CPU-bound by ~6.5 ms.

**The gap to close: about 2.5-3 ms of CPU at ~9,300 draws**, if the 1080p wall confirms
the CPU frame is ~11 ms. Light street (2,000-3,000 draws) was 3.9 ms in part 106 and is
already past 250 fps; **this plan is entirely about the crowd.**

**What is NOT in scope.** The GPU at 3440x1440 (4.0 ms was a 1080p number; 3440x1440 is
2.4x the pixels). If the CPU reaches 8.33 and the GPU does not, the operator gets 120 fps
at a lower resolution or with MSAA off and the CPU claim still stands. Say which is which
in the record; do not quote a wall number as a CPU result (gotcha 219's shape).

## §1. Protocol, and the rules this plan does not get to bend

* **Three runs an arm, alternated, `tools/part80_crowdroute.sh <tag> [ENV=VAL]`** — the
  tag is the FIRST argument and env follows it (an env var passed first is silently eaten
  as the tag and the run is unprofiled; that happened once tonight and the three runs it
  produced were re-read as an honest profiler-OFF baseline rather than thrown away).
* **The band is >= 8,000 draws**, read with `tools/read_crowd.py`. A run-wide median
  measures how long the menus lasted.
* **The profiler is for MECHANISM, never for wall** (gotcha 454: it inverts the regime and
  costs 2-4 ms). Every wall number in this plan comes from a profiler-OFF run.
* **A kill rule per item, pre-registered below.** Under it, the item is closed and NOT
  shipped, however good the idea was.
* **Gates before any A/B is believed:** `--smoke`; the A5 kernel-order diff exit 0;
  `truncated=0`; and for anything touching the renderer, `CZ_VK_VALIDATION=1` clean and
  `CZ_VK_SYNC_VALIDATION=1` at 0 hazards with the poison control at 30.
* **A memo needs a verifier arm.** Every cache added here ships with an env var that
  computes both answers and counts disagreements, and the A/B is only believed after a
  run with the verifier reads 0. This is the shape that made part 55's constant memo safe
  and part 24's stream guard unsafe.

## §2. The items, in order of expected milliseconds ON THIS BOX

Order differs from part 107's, which was ordered for a 4-core machine.

### Item 0 — the fresh decomposition (running first, no code)

`CZ_VK_PROFILE=10` at 1080p and at 3440x1440, three runs each, plus a `perf record` of the
pump for symbol-level truth (`tools/part55_srcline.py` reads it). Part 107's §2b table was
taken on the 4-core stand-in with the glyph scan still running; **every share in it has a
shelf life.** Nothing below is priced until this exists.

### Item 1 — `UploadTexture` as a pure lookup (~1.5 ms predicted)

13,900 calls a frame and **0.0014% of them do work**. Each call today computes an FNV-1a
over six fetch-constant dwords plus the shader dimension, runs `DecodeTextureFetch`, and
then does a hash-map lookup. The constants for a given slot almost never change between
draws.

**Design:** a per-`constIdx` memo holding the six dwords, the shader dim, and the answer.
A hit is a 24-byte `memcmp` plus one compare, and skips the hash, the decode and the map.
**The invalidation is the whole design:** the answer depends on the resolve-snapshot set,
which changes DURING a frame, so the memo carries a generation counter bumped by every
snapshot registration and by frame start. A memo that skipped that would freeze a
colour-grading LUT for the run — the exact defect the snapshot-before-cache ordering
comment in `UploadTexture` was written about.

**Verifier arm `CZ_VK_TEXMEMO_VERIFY=1`:** compute both answers, count disagreements,
print at exit. Ship only after a crowd run reads 0.
**Kill rule: below 0.4 ms it does not ship.**

### Item 2 — `UploadStream`'s resolve half (~1.5 ms predicted)

162 ns/draw, and the flat cache's lookup is the measured majority. The key set repeats 94%
frame to frame and a perfect hash over the stable working set was never tried. Same
verifier discipline.
**Kill rule: below 0.4 ms it does not ship.**

### Item 3 — the constants (~1.5 ms predicted)

The gather serves 93% from the part-55 memo; what remains is the copy of ~9 VS / 27 PS
registers per draw and the projection patch. The copy is the target, not the memo.
**Kill rule: below 0.4 ms it does not ship.**

### Item 4 — the PM4 walk's per-packet dispatch (~3.1 ms, the largest single term)

Never decomposed. Part 108 established that item 3(a) of the old plan (bulk register runs)
**already exists** and that the cost is the **per-packet dispatch**, not the register
writes. This is research and it is last on purpose: it is the biggest number and the
least-understood one, and an overnight session should not start there.
**Do not re-buy memoisation** (gotchas 474/4, refuted twice).

### Item 5 — the p99, separately from the median

`CZ_FPS_LOG` p99 is 13.9-14.5 ms at the crowd against an 11.2 ms median. **A locked 120 is
a p99 claim, not a median claim.** Classify the slow frames by where the time went
(`CZ_VK_FRAME_TRACE`'s columns; texture-upload frames counted separately). If the median
reaches 8.33 and the p99 sits at 14, the operator does not have 120 fps.

## §3. What this plan will and will not have proved

It can prove: the CPU frame at ~9,300 draws on THIS box, before and after, three runs an
arm, at a stated resolution with stated instruments.

It cannot prove: anything about the operator's felt experience (their route is not this
route), anything about the GPU at 3440x1440, or that 120 fps is reachable at all — if the
items land and the frame stops at 9.5 ms, **that is the answer**, and the honest close is
to say so and name what remains rather than to keep buying items.

## §4. Execution record

### 4.1 The baseline, and the finding that shapes the whole plan (2026-09-10)

Six more runs, `tools/part80_crowdroute.sh`, mirror ON, MSAA 2x:

| arm | runs | crowd median | p99 | draws |
|---|---|---|---|---|
| **3440x1440, profiler OFF** | 3 | 10.72 / 11.39 / 11.42 ms | 13.9-14.5 | 9,036-9,657 |
| **1920x1080, profiler OFF** | 3 | **10.74 / 10.96 / 10.97 ms** | 13.6-14.3 | 8,585-8,700 |
| 1920x1080, `CZ_VK_PROFILE=10` | 3 | 14.86 / 15.19 / 15.98 ms | 17.7-18.7 | 8,575-9,301 |

**THE FINDING: 1080p and 3440x1440 read the SAME frame time.** 10.9 ms against 11.2 ms
across a 2.4x difference in pixels. The frame is **CPU-bound at the operator's own native
resolution**, not just at 1080p — so the entire 2.5-3 ms gap to 8.33 ms is CPU, and
nothing in this plan needs to argue about the GPU at all. (The draw counts differ by ~8%
between the two, so this is a regime statement, not a matched A/B; it does not need to be
one.)

**The profiler's bill, re-measured on this box: +4.2 to +5.0 ms, about 40%.** Gotcha 454
stands and every mechanism number below is read from profiled runs while every wall number
comes from an unprofiled one.

**The p99 is 13.6-14.5 ms against an ~11 ms median**, so item 5 is not optional: a median
at 8.33 with a p99 at 14 is not 120 fps.

### 4.2 Item 1 — built, OPT-IN, not yet verified or priced

`CZ_VK_TEXMEMO=1` engages a 32-entry per-fetch-slot memo; `CZ_VK_TEXMEMO_VERIFY=1`
computes both answers and counts disagreements; an exit line reports hits, misses and
disagreements. **Default OFF on purpose** — HEAD must behave exactly like the released
v1.0.2 until a crowd run reads 0 disagreements and three runs an arm clear the kill rule.

The design's load-bearing part is what it must NOT break. `TexFind` stamps
`lastUsedFrame` on every lookup and the LRU reclaimer evicts by it, so a memo that skipped
the lookup would stop marking live textures as used and the reclaimer would evict them
mid-session. The memo therefore stores the entry pointer and stamps it itself, and
`g_texGen` invalidates every entry at all six sites that can change an answer: snapshot
emplace, erase and clear, `TexInsert`, and both arms of `ReclaimTextureSlot`.

**Next session starts here:** one crowd run with `CZ_VK_TEXMEMO=1 CZ_VK_TEXMEMO_VERIFY=1`
and read `[texmemo]`. Zero disagreements is the gate; then three runs an arm against the
4.1 baseline, kill rule 0.4 ms.

### 4.3 Item 1 — VERIFIED CORRECT, PRICED, and CLOSED under its own kill rule (2026-09-10)

**The verifier reads zero.** Three crowd runs with `CZ_VK_TEXMEMO=1`, one of them also
carrying `CZ_VK_TEXMEMO_VERIFY=1`:

| run | hits | misses | served | disagreements |
|---|---|---|---|---|
| verify | 235,158,100 | 81,699,641 | 74.2% | **0** |
| on_1 | 229,557,196 | 84,405,725 | 73.1% | 0 |
| on_3 | 253,479,524 | 82,021,676 | 75.6% | 0 |

The generation counter works: `final gen` is ~2,935 over a whole run, so invalidation is
rare and none of the misses are it — they are real fetch-constant changes between draws.

**THE INSTRUMENT COULD NOT FIRE, AND THAT WAS FOUND BY RUNNING IT.** The exit report was
written into the LIVE RESCALE path — a function a headless crowd run never calls — so the
first verify run drove the whole route and printed nothing at all, and "0 disagreements"
and "the instrument never spoke" were the same output. It now lives in
`VkRenderer_DumpStats()`, which is what the `timeout` SIGTERM handler calls, and the
control arm proves the report is a real variable: all three `memo OFF` runs print no
`[texmemo]` line and all three `memo ON` runs do. Gotchas 543-544.

**The price, three runs an arm, alternated, profiler OFF, 3440x1440, matched draw bands:**

| band (draws) | OFF | ON | delta |
|---|---|---|---|
| 8000-8249 | 9.80 | 9.72 | −0.08 (−0.8%) |
| 8500-8749 | 10.40 | 10.11 | −0.29 (−2.8%) |
| 8750-8999 | 10.76 | 10.39 | −0.37 (−3.4%) |
| 9000-9249 | 10.90 | 10.63 | −0.27 (−2.5%) |
| 9250-9499 | 11.08 | 10.82 | −0.26 (−2.4%) |
| **weighted** | **10.75** | **10.42** | **−0.33 ms (−3.1%)** |

**BANDING WAS NOT OPTIONAL HERE.** The ON arm happened to land in denser crowds (median
9,411 draws against the OFF arm's 8,989 — the random zombie spawn this route's header
warns about), so the two arms' raw run medians are 10.81 and 10.82 ms: a dead null. The
effect is entirely hidden by a 4.7% draw-count difference until the bands are matched.
Reading `tools/read_crowd.py`'s run medians alone would have killed a real 0.33 ms.
`tools/part109_band.py` is that reader, and it refuses to summarise arms with no
matched bands rather than averaging across a difference it cannot see.

**VERDICT: CLOSED. The kill rule was 0.4 ms and this is 0.33 ms.** The code stays in the
tree, opt-in and unchanged, because it is verified correct over 718 million served
lookups and because it costs nothing when off — but it does not ship, and this plan does
not get to move its own goalposts after seeing the number (§1).

**And the ceiling is not much higher, which is why it is not worth a second attempt.** The
saving is 0.33 ms over ~11,760 hits a frame = **28 ns a hit**, so even a perfect memo
serving 100% of the ~13,900 calls would be ~0.39 ms. A wider (set-associative) memo to
catch alternating materials cannot reach the kill rule. The item is ~0.4 ms in principle,
not in execution, and the gap to close is 2.5-3 ms.

**A consequence for items 2 and 3, taken from the profiled runs of §4.1 and to be
re-checked by item 0's decomposition:** at the crowd, `textures` reads 5.0% and `streams`
reads **0.3%**. Item 2's whole envelope is therefore ~0.04 ms and it is almost certainly
dead on price before a line is written — the flat cache of part 55 already took that cost
out. `constants` reads 18.2% ≈ 2.71 ms, of which `constShared` — a 2,192-byte `memset`
into write-combined arena memory on EVERY draw — is 3.8% ≈ 0.57 ms on its own. Item 3 is
bigger than the plan assumed and item 2 is smaller.

### 4.4 Item 0 — THE DECOMPOSITION, and it disagrees with the phase profiler (2026-09-10)

`tools/part109_probe.sh` (new): part 107's stand-in probe re-pointed at the operator's own
box — 8c/16t unmasked at 4654 MHz, **3440x1440 through a real swapchain**, no
`CZ_VK_PROFILE`. A flat `perf record -F 999` of the whole process for 30 s inside the
stationary crowd soak, read per thread by `tools/part53_symbols.py` and split by source
line by `tools/part55_srcline.py`.

**The gate had to be fixed before the profile was worth anything.** The route's camera
sweeps swing Chuck's view INTO the crowd and back out before the soak begins, so the
first run's windows read 2,512 / **9,141** / 6,495 / 5,828 / 5,799 / 5,944 / 8,942, the
one-window gate fired on the 9,141 spike, and `perf` sampled the 5,800-6,500-draw walk.
Nothing in the artifacts would have said so. The gate now requires **two consecutive**
crowd windows and the soak is 120 s.

**Who is busy** (15 s window, 53 threads):

| thread | % of one core | what it is |
|---|---|---|
| pump | **97.7%** | the renderer + the PM4 walk — **the critical path** |
| guest | 72.9% | recompiled title code, diffuse (`__imp__sub_*`) |
| guest | 58.5% | recompiled title code (`__imp__sub_827D5B18` 15.3% of it) |
| guard x3 | 33.7-34.0% | `GuardFold` 84% — the parallel content-guard pool |

Process total **391% of one core = 3.9 of 8 physical cores**. The busiest thread is 25% of
all the CPU this process uses, so the frame is still one thread's length.

**Where the pump's ~10.5 ms goes** (self time; the crowd frame is 10.8 ms and the pump is
97.7% busy, so a point of the thread is ~0.105 ms of frame):

| symbol | % of pump | ms/frame | note |
|---|---|---|---|
| `DoDraw` | 21.37 | 2.25 | **no hotspot: 388 source lines, the top one 7.8% of it** |
| `WriteRegisterRun` | 10.37 | 1.09 | of which **`pm4.cpp:895` alone is 7.25% of the pump = 0.76 ms** |
| `UploadStream` | 9.43 | 0.99 | |
| `ExecutePacket` | 8.74 | 0.92 | |
| `UploadTextureUncached` | 6.89 | 0.73 | |
| `[unknown]` | 6.86 | 0.72 | unresolved — the driver, not yet attributed |
| `__memset_avx2` | 4.18 | 0.44 | the per-draw `kSharedSize` zero |
| `PersistFind` | 3.99 | 0.42 | called from `UploadStream` |
| `CopyConstWindow` | 3.57 | 0.38 | |
| `ExecuteLinear` | 3.31 | 0.35 | |
| `__memmove_avx` | 3.16 | 0.33 | |
| `TexFind` | 2.57 | 0.27 | |
| `__memcmp_avx2` | 2.57 | 0.27 | the stream guard |
| `PipelineKey` `_M_locate` | 1.41 | 0.15 | the one `std::unordered_map` left on this path |

Grouped: **the PM4 walk is 22.4% = 2.36 ms** (`WriteRegisterRun` + `ExecutePacket` +
`ExecuteLinear`), **the stream path 13.4% = 1.41 ms** (`UploadStream` + `PersistFind`),
**the texture path 9.5% = 1.00 ms**.

**RETRACTION, in place, of §4.3's last paragraph.** It read the phase profiler's `streams
0.3%` and concluded item 2 "is almost certainly dead on price before a line is written."
**That is wrong and it is wrong for a reason this project has already written down once:
a `ProfScope` is a region of code, not a subsystem (gotcha 343).** The `streams` scope
covers a slice of `UploadStream`; the SYMBOL is 9.43% of the pump and 13.4% with
`PersistFind`, which is 1.41 ms — the third-largest thing on the critical path. Part 55
learned exactly this about exactly this function and it was re-learned here in one
session. The phase table's shares are a map of the scopes someone thought to open;
`perf` needs nobody to have thought of anything.

**What the source-line split then says about each:**

* **`DoDraw` (2.25 ms) has no item in it.** 388 lines, the hottest 7.8% of the symbol
  (0.18 ms). It is a large per-draw function and its cost is its size. Nothing here is
  buyable in one change; it is the argument for doing less per draw, not for optimising a
  line.
* **`WriteRegisterRun` is a single line.** `pm4.cpp:895` — the scalar byte-swap loop of
  `Source::Read` — is **69.9% of the symbol, 7.25% of the pump, ~0.76 ms of the frame**,
  and the disassembly shows plain `bswap` 4x-unrolled with no vector instruction. That is
  item 4's concentrated core and it is where part 109 went next (§4.5).
* **`UploadStream` is CACHE MISSES, not computation.** Its four hot lines are the flat
  cache's probe compare (`keys[i] == k`, 29% of the symbol), the probe step (9%), the
  `PersistEntry::dynamic` test (17%) and `return *hit` (15%) — four dependent loads, all
  of them the first touch of a line. A perfect hash (this plan's §2 item 2 design) would
  not move any of them. What would is **fewer lookups**: the profiler counts ~9.46 stream
  lookups a draw against the two-to-five streams a draw actually has, and
  `CZ_VK_STREAM_DEDUP_CENSUS=1` — written in part 87 to ask exactly this and **never
  run** — is the measurement that decides. Item 2 is redirected, not dead.
* **`__memset_avx2` at 0.44 ms** is the per-draw `memset(shared, 0, kSharedSize)`: 2,192
  bytes into write-combined arena memory on every one of ~9,300 draws = 20 MB a frame.
  The vertex-fetch table is 1,536 of those 2,192 bytes and is written only for the
  handful of slots a dependent fetch uses.

### 4.5 Item 4's concentrated core — BUILT, GATED, ENGAGED, and REFUTED (2026-09-10)

§4.4 named `pm4.cpp:895` — the scalar byte-swap loop in `Source::Read` — as **69.9% of
`WriteRegisterRun`, 7.25% of the pump, ~0.76 ms of the frame, in one line**. Everything
that could be checked before building it said it was buyable:

* the **disassembly**: plain `bswap`, 4x-unrolled, not one vector instruction, because a
  4-byte `memcpy` in a loop clang cannot prove non-aliasing for does not vectorise;
* the **census** (`CZ_PM4_REGRUN_CENSUS=1`, written for this): 710 M bulk runs, 13.7 G
  dwords, **mean 19.3 dwords a run and 90.3% of all dwords in runs of 16 or more**, so a
  32-byte-wide swap covers essentially the whole population rather than a corner of it.

Built as a runtime-dispatched AVX2 path (`target("avx2")` + `__builtin_cpu_supports`,
unaligned loads and stores, entered only with 8 dwords left, scalar tail otherwise), with
`CZ_PM4_NO_SIMD_SWAP=1` the same-binary control.

**Every gate passed.** `CZ_PM4_VERIFY_BULK_REGS=1` — which compares every dword against
`Source::operator()`, the incumbent definition of "read a dword of the packet stream" for
47 parts, and is therefore an oracle that is not this code — read **0 mismatches** over a
whole crowd route, and `CZ_PM4_VERIFY_POISON=1` made it fire (16, its report cap). And it
**proved it engaged**: the arm reported **90.3-90.9% of run dwords vectorised** in all
three runs against **0.0%** in all three controls, matching the census's 90.3% prediction.

**And it is worth nothing.** Three runs an arm, alternated, profiler OFF, 3440x1440,
matched draw bands:

| band (draws) | scalar | AVX2 | delta |
|---|---|---|---|
| 8500-8749 | 10.64 | 10.48 | −0.15 |
| 8750-8999 | 10.80 | 10.84 | +0.04 |
| 9000-9249 | 10.93 | 11.13 | +0.20 |
| 9250-9499 | 11.19 | 11.29 | +0.10 |
| 9500-9749 | 11.55 | 11.63 | +0.08 |
| **weighted** | **10.92** | **11.06** | **+0.14 ms (+1.3%)** |

**The re-profile says why, and it is the part worth keeping.** With the vector path in,
`SwapRunAvx2Impl` (6.06% of the pump) plus what was left of `WriteRegisterRun` (3.75%)
came to **9.81% against the scalar loop's 10.37%**. The work did not move. **It was never
the byte swap** — the samples were piling on the instruction that CONSUMES the load,
which is what an out-of-order core does, so a line can read as 70% of its symbol while
being 70% *waiting*: for the packet stream to arrive from memory, and for the `g_regs`
stores to retire. Gotcha 545.

**VERDICT: REVERTED.** The kill rule was 0.4 ms and this is a null with a slightly
adverse sign, and it would have added AVX2 dispatch to a binary that ships to strangers
for nothing. The census stays — it is the instrument that would have made the item look
even better, and the next reader deserves to see that it did.

**What this changes about the remaining items.** Three of the five largest things on the
pump are now known or strongly suspected to be **memory-latency bound, not compute**:
this loop, `UploadStream`'s four dependent-load lines (§4.4), and the 20 MB/frame
`memset` into write-combined memory. On this critical path the lever is **doing fewer
memory touches**, not doing the same touches faster. That is an argument for the stream
DEDUP (fewer lookups) over any faster hash, and against any further "make this loop
wider" item.

### 4.6 Item 2 — REDIRECTED, then measured small (2026-09-10)

`CZ_VK_STREAM_DEDUP_CENSUS=1`, written in part 87 to ask exactly this question and never
run until tonight, on the crowd route: **4.92 stream lookups a draw, 47.1% of them
REPEATING a key the same draw already looked up** (319,547,476 of 678,019,642), and **0
draws exceeded the 16-key window**, so nothing is under-reported.

47% sounds like an item and it is not, for the same reason §4.5's byte swap was not: **a
repeat within one draw is the CHEAP kind of lookup.** The first touch misses and pays the
cache; the repeat hits the same flat-cache slot while it is still in L1. §4.4's source-line
split says where `UploadStream`'s time really is — the probe compare `keys[i] == k` (29%
of the symbol), `PersistEntry::dynamic` (17%), `return *hit` (15%), the probe step (9%):
**four dependent loads, each the first touch of a line.** A per-draw dedup removes the
lookups that were already free.

`PersistFind` at 4.15% of the pump = 0.44 ms is the one worth remembering: it is a thin
wrapper on a flat-cache `Find` into the cross-frame store, which is tens of thousands of
entries across a 1 GB buffer, called about 2,000 times a frame on the frame-first touches
— roughly 200 ns each, which is a DRAM round trip. That is a prefetch question, not a
hash question, and it is not something to start at 6am.

### 4.7 Item 3's memset half — MEASURED, gated, OPT-IN at −0.21 ms

`memset(shared, 0, 2192)` on every draw, ~20 MB a frame into write-combined arena memory,
`__memset_avx2` at 4.18% of the pump = 0.44 ms. 1,536 of the 2,192 bytes are the
dependent-vertex-fetch table and **only a slot the vertex shader declares can be read out
of it**. Scoped to zero the head, the declared 16-byte entries, and the tail: **1,530 of
1,536 table bytes go unwritten on the average draw, 69.8% of the block.**

`CZ_VK_SHARED_ZERO_POISON=1` writes 0xFF over exactly the skipped bytes, so the premise
is tested rather than argued — a shader reading one would get a colossal device address
and size 0xFFFFFFFF, not a quiet zero. **Inside the picture null on all three era
statistics** at both the prefix-shaped skip and the wider per-entry one.

**−0.21 ms (−1.9%)**, three runs an arm, four of five bands negative, both arms proved
engaged in all six runs. **Kill rule 0.4 ms → ships OFF** (`CZ_VK_SCOPED_SHARED_ZERO=1`).

### 4.8 Item 5 — THE p99 IS NOT A HITCH CLASS (2026-09-10)

One crowd run with `CZ_VK_FRAME_TRACE`, no profiler; 11,268 frames at >= 8,000 draws.
Median 11.23 ms, p95 12.37, p99 14.10, worst 21.34.

**What distinguishes a p99 frame from the median band — medians of each column:**

| column | median band | p99 frames | delta |
|---|---|---|---|
| draws | 9,410 | 9,412 | +2 |
| **walk (the CPU frame)** | **11.03 ms** | **14.93 ms** | **+3.90** |
| fence | 0.00 | 0.00 | 0 |
| sleep | 0.20 | 0.20 | 0 |
| **GPU** | **5.93 ms** | **5.92 ms** | **−0.01** |
| texture uploads | 0 | 0 | 0 |
| pipelines built | 0 | 0 | 0 |

**3 of 113 p99 frames uploaded a texture; 0 built a pipeline.** The slow frames render the
SAME draw count in the SAME GPU time with no upload, no compile, no fence and no sleep:
**the identical work simply takes 35% longer on the CPU.** There is no separate stutter
class to fix at this load — the p99 is the upper tail of the pump's own work
distribution, so **the only thing that moves it is making that work shorter.** A median at
8.33 ms with this shape would put the p99 near 11 ms, i.e. ~90 fps worst-case, and the
operator would feel 120 as a soft 120.

Two facts worth carrying: **the GPU is 5.93 ms against an 11.23 ms wall** — 53%, so it is
not the constraint and §4.1's CPU-bound reading is confirmed a third way — and the fence
is 0.00 on every band, so no GPU-side saving converts to frame rate here (gotcha 476
still stands). The caveat: this route's soak streams no new textures, so the
texture-hitch class part 77 addressed is invisible to it by construction and this says
nothing about it.

## §5. WHERE PART 109 ENDS, AND WHAT IT DID AND DID NOT PROVE

**The target was the CPU frame under 8.33 ms at ~9,300 draws. It is 10.8-11.2 ms and it
did not move.** §3 pre-registered this outcome and its instruction: say so, and name what
remains, rather than keep buying items.

**What the night measured, in order:**

| item | verdict | ms |
|---|---|---|
| 1 — `UploadTexture` memo | correct (0 disagreements / 718 M served), **under kill** | **−0.33** |
| 0 — the decomposition | **done**, and it retracted a phase-profiler reading | — |
| 4's core — the PM4 byte swap | built, gated, engaged, **REFUTED and reverted** | +0.14 |
| 2 — the stream path | **redirected then measured small**; the repeats are the cheap lookups | — |
| 3's memset half | correct (poison inside the null), **under kill** | **−0.21** |
| 5 — the p99 | **not a hitch class**: same work, 35% longer | — |

**THE STRUCTURAL FINDING, and it is the one that decides what a part 110 should do.** The
pump thread is 97.7% of a core and 25% of all the CPU this process uses, while the
machine runs at 3.9 of 8 physical cores. Parallel command recording already exists and
already runs — three workers capture ~7,200 of ~8,650 draws a frame — but the profile
says those workers spend **84% of their time in `GuardFold`** and about 1% in
`ParRec_RecordInstance`. **The half that was moved off the pump was the cheap half.** The
expensive half — `DoDraw`'s decode, constants, textures and streams, 2.25 ms + 1.41 + 1.00
+ 0.38 — is still serial because it touches the arena and the caches. Nothing smaller than
that is going to find 2.5 ms.

**And there is no large item left inside it.** `DoDraw` is 2.25 ms spread over 388 source
lines with the hottest at 0.18 ms. Three of the five largest blocks are memory-latency
bound rather than compute (§4.5's refutation, `UploadStream`'s four dependent loads,
`PersistFind`'s DRAM round trip), so the lever on this path is **fewer memory touches**,
not faster ones.

**THE ONE DECISION FOR THE OPERATOR.** Two items are verified correct and are worth
**−0.33 and −0.21 ms**, and both are OFF because each missed a 0.4 ms bar that was
pre-registered when the plan still expected single items worth 1.5 ms. Item 0 then
established there are no such items. Whether a bundle of sub-threshold items is worth
taking is a judgement about risk appetite, not a measurement, so it is theirs:
`CZ_VK_TEXMEMO=1` and `CZ_VK_SCOPED_SHARED_ZERO=1` turn them on today, flipping either
default is one line, and both already have their gates run. Together they are ~0.5 ms of
a 2.5 ms gap — worth having and not worth calling 120 fps.
