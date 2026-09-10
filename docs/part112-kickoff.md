# Part 112 kick-off — where part 111 left it

**READ `docs/perf-plan-part111.md` §10 FIRST.** It is the execution record and it is the
whole state. This file says only what to do next and what not to re-derive.

## §0. ITEM B IS CLOSED BY MEASUREMENT, AND THE REASON GENERALISES

The operator said build item B knowing it topped out at ~113 fps. Part 111 built its
cleanest stage, measured it, and stopped — which is what B1's pre-registered kill was
bought for.

**B1 — pre-zero the shared block on the idle cores — engages perfectly and recovers
nothing.** 100% of draws served, zero fallbacks, zero drain, zero busy-chunk waits;
`perf` says `__memset_avx2` on the pump went **3.96% -> 0.04%** (0.43 -> 0.004 ms/frame)
and the three guard workers went **30.9% -> 35.3%** of a core each; **the pump's CPU per
frame moved +0.00 ms** over three runs an arm and six matched draw bands.

**The mechanism is named by a controlled pair, not inferred.** The SCOPED arm attacks the
same `memset` by writing **70% fewer bytes on the same thread** and pays **−0.13 ms**;
relocating the **same bytes** pays nothing. **The pump is bound by BYTES, not by cycles,
and bytes are a machine-wide resource** (gotcha 551).

**So B2 is predicted dead and was not built.** §5's design moves exactly the
bandwidth-bound half — `UploadStream` + `__memcmp_avx2` + `UploadTextureUncached` =
**1.99 of B2's 2.68 ms is bytes** — and leaves the latency-bound 0.69 ms on the pump on
purpose. **B3 was refuted by the census** (1.35 `g_regs` const-window copies a draw).

**Do not re-buy any of this.** If a later part wants to reopen item B, the thing that
would change the verdict is a machine where the bound is different — part 107's Ryzen 3
stand-in, or a Steam Deck. `CZ_VK_PREZERO=1` asks, and it costs one run.

## §1. What is worth doing next, in order

### 1. The one verified saving still sitting off, and it is now the ONLY shape that works

`CZ_VK_SCOPED_SHARED_ZERO=1` is **−0.13 ms** here and **−0.21 ms** in part 109 — two
independent measurements — and part 111 has now established that the alternative to it is
a null. It ships OFF only because it missed a 0.4 ms bar that part 109 set when the plan
still expected 1.5 ms items, and part 110 then established there are none. Its poison arm
is inside the picture null and its gates are already run. **Flipping the default is one
line.** `CZ_VK_TEXMEMO=1` (−0.33 ms) is the other one in the same position.

**This is the operator's call, not a session's**, and it is the same call
`part111-kickoff.md` §0 put in front of them — except that one branch of it has now been
measured away.

### 2. THE SUBJECT NOBODY HAS TOUCHED: the guest's own 8.8 ms

Unchanged from `part111-kickoff.md` §1 and now more important, because item B is closed and
this is the largest single term in the frame. Two guest threads at 76.7% and 61.7% of a
core, diffuse `__imp__sub_*`, no hotspot on the busier one, and 2.05 ms of the wall is the
gap between the busiest thread and the frame — so it is a **dependency chain**, not a
throughput limit. Nothing in this project has ever decomposed it, and it is what decides
whether 120 fps is reachable on any hardware.

Three cheap first questions, in order:
1. **What are those two threads?** The guest names its threads through an exception channel
   we log (`JobThread0..5`, `cAsyncFileSystem`, ...) — bind the host TID to the guest name.
2. **Is the 2.05 ms gap a hand-off between the two guest threads, or between them and us?**
   `CZ_WAIT_TRACE` and part 107's fence-wait counters already exist.
3. **Does the floor scale with the crowd?** If it is the zombie simulation, it is a Case
   West finding too.

### 3. If performance is reopened on the renderer, ask about BYTES

Part 111's finding reframes the board. The addressable class is **fewer bytes** (the
scoped item's shape — 5.9 MB/frame instead of 19.6) and **overlapped misses** (the guard
pool's original shape, which part 53 measured working because `GuardFold` is
latency-bound). It is **not** "the same bytes on another core", and it is not compute
(part 109's vectorised byte swap: +0.14 ms at 90.6% engagement, gotcha 545).

The census in the tree will size any candidate in that class for the price of one run.

## §2. What is in the tree that was not before

* **`CZ_VK_PARDRAW_CENSUS=1`** — the per-draw mutation census. Counts reads, times
  mutations, prints `S` and the read share. Its answers are `perf-plan-part111.md` §10.1;
  read them before re-running it.
* **`CZ_VK_PREZERO=1`** — B1, **off by default because it was measured**. Prints a
  `[prezero]` line per FPS window carrying BOTH sides of the bill (gotcha 344).
  `CZ_VK_PREZERO_POISON=1` is its positive control and it **passes** — the picture breaks,
  +0.87 -> +0.27.
* **`tools/part47_gates.sh` now pins `CZ_VK_RES`** (`GATE_RES` overrides). It had been
  failing its E3 check since part 47 on any non-16:9 desktop, for reasons unrelated to the
  code under test. See gotcha 552 — and note the diagnosis cost a control build, which is
  the standing rule and was worth it.

## §3. Rules this part paid for

* **Ask whether an item is bound by cycles or by bytes before deciding another core can
  help.** A thread census showing one core at 97.7% and four idle is evidence about
  occupancy, not about what is scarce (gotcha 551).
* **A gate that takes any input from the environment will eventually convict an innocent
  change** (gotcha 552). Pin every input a gate does not intend to vary.
* **Write the race argument before the run, not after.** §10.4's argument caught two real
  defects with no debugging: a worker that could zero constants the pump had already
  written (a plain `ready` flag is not enough — ownership has to be a compare-exchange),
  and a `CZ_VK_NO_PARALLEL_GUARD=1` arm in which the pump would have memset the whole
  posted region.
* Parts 110's two rules still stand: **archive the executable beside every `perf`
  capture** (gotcha 550), and **do not rebuild while a campaign is running**.

## §4. Owed, unchanged

The Windows EYE tests (`windows-test-list` items 2-10) still need a `cz_play` session.
