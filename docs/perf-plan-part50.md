# The CPU performance plan for part 50 — written against the operator's whole-map lap at 60 fps

Written at the close of part 49 (2026-08-16). **Supersedes `docs/perf-plan-part48.md`
as the live plan**; every tier-1 and tier-2 item in that document is built, measured
and closed, and its target was met.

---

## 0. WHY THIS PLAN CAN BE BELIEVED WHERE THE LAST THREE COULD NOT

Parts 46-48 all had to argue from profiler phase shares, because the frame was pinned
to a 32 ms floor and **a CPU saving below 33 ms measured as exactly zero** (gotchas
237/238). Part 49 removed that floor: the title's 30 fps is its own present interval,
a 60 fps configuration is one it already ships with, and the vblank ladder that made
it 60-or-30 is now 8 ms instead of 16.

**Frame time is a usable instrument again, for the first time since part 30.** Read it
directly. The pinned-share statistic that gotchas 237/238 prescribe is no longer the
primary reading — it was a workaround for a ceiling that is gone.

Two consequences that shape everything below:

* **The frame is now CPU-bound essentially everywhere.** `submit.gpu` is **0.0% median
  over 22 windows** of the operator's lap. The GPU is idle. Every millisecond in this
  document is a CPU millisecond and there is no GPU item anywhere in it.
* **At 60 fps the per-FRAME costs are paid twice as often per second.** The guest
  simulates twice, the PM4 stream is walked twice, the readback runs twice. Anything
  per-frame is now worth double what the part-48 budget said it was.

---

## 1. THE BUDGET — the operator's own machine, 16,788 frames, whole map

| draws/frame | frame | fps | share of their lap |
|---|---|---|---|
| 0-1,500 | 16.0 ms | **62.5** | 7% |
| 1,500-3,000 | 16.0 ms | **62.5** | 21% |
| 3,000-5,000 | 23.0 ms | 43.5 | 9% |
| **5,000-7,000** | **28.0 ms** | **35.7** | **60%** |
| 7,000+ | 35.0 ms | 28.6 | 3% |

**The 5,000-7,000 draw band is 60% of their play and it is the target.** At 28.3 ms:

| term | ms | what is in it |
|---|---|---|
| **`outside`** | **11.3** | the PM4 walk — **81,106 packets/frame at 100 ns = 8.1 ms** — plus the guest's own simulation, which is the remaining ~3 ms |
| **`record`** | **7.4** | 1,250 ns/draw = state 172 + **vertex 662** + index 220 + residual 182 |
| **`other`** | **4.4** | 717 ns/draw = shader 96 + key 36 + pipeline 124 + begin 101 + fetch 114 + tail 40 + **residual 206** |
| `textures` | 3.1 | closed in part 47; do not reopen without a reason |
| `readback` | ~1.2 | the present path, and it now runs twice as often per second |
| `constants` | ~1.2 | |
| `submit`/GPU | ~0.0 | the GPU is idle |

**TARGET: 16 ms at 7,000 draws**, i.e. hold the 60 fps cap through a crowd. That is a
43% cut and it is deliberately ambitious. A creditable intermediate is **20 ms (50
fps)**, which tiers 1 and 2 alone should reach.

---

## 2. TIER 1 — THE PM4 WALK, 8.1 ms and the largest single item

Part 48 took the per-packet cost from 144 ns to 100 ns (the `getenv`, and the
per-thread census counters). What remains is **81,106 packets a frame**, and the
opcode census — printed for the first time in part 48 — says what they are:

```
t0(reg-run) 34.5%   t1 0.0%   t2(FILLER) 28.7%   t3(command) 36.8%
SET_BIN_MASK_LO 11.9%   DRAW_INDX 7.9%   EVENT_WRITE 6.0%
EVENT_WRITE_EXT 5.9%    IM_LOAD 2.0%     LOAD_ALU_CONSTANT 2.3%
```

### 1a. 28.7% of every packet walked is TYPE-2 RING FILLER that does no work — EXPECTED 1.5-2 ms

A type-2 packet is a one-dword no-op. It currently costs a full `ExecutePacket` call:
the `Source` fetch with its wrap modulo, the header decode, the census bump, the
return, and the caller's loop bookkeeping. **At 81,106 packets a frame, 23,000 of them
are this.**

The change is to recognise a RUN of filler in the caller's loop and skip it with one
counter update instead of 23,000 calls. The census must still report the same count —
that is the correctness check, and it is free.

**Prediction**: 20-30 ns off the mean per-packet cost. **Arm**:
`CZ_PM4_NO_FILLER_RUNS=1`. **Verify** the type-2 census total is unchanged over a full
run.

### 1b. SET_BIN_MASK_LO is the most frequent packet in the stream — 12,000/frame, 11.9%

Half again as many as there are draws, and nobody had counted it before part 48. Its
handler is one OR into a global. So its cost is entirely the per-packet OVERHEAD, which
makes it the best possible probe for 1c: whatever the fixed cost of dispatching a
packet is, this pays it 12,000 times a frame for two dwords of work.

**Measure before changing anything**: instrument the per-packet fixed cost directly
(a scope around the decode-and-dispatch preamble, excluding the handlers). That number
times 81,106 is the ceiling on everything in this tier.

### 1c. The per-packet preamble — EXPECTED 2-3 ms, and 1b prices it

Everything `ExecutePacket` does before it knows which handler to call: the `Source`
functor's wrap modulo, the type decode, the `avail` bounds test, the zero-header check,
the predication evaluation (`(header & 1) && (g_binMask & g_binSelect) == 0`), the
`g_constWatchSource` store. Per packet, 81,106 times a frame.

Candidates, in order of confidence:
* **Hoist the wrap modulo.** `Source::operator()` does `% ring` per dword fetched. A
  walk that knows it is not near the ring end can index directly.
* **Skip the const-watch store** when the watch window is empty, which it is unless
  `CZ_PM4_CONST_WATCH` is set. Part 47 did this for the register run path and not here.
* **Dispatch on a table rather than a switch** only if the switch is measured to be
  the cost, which is unlikely and must not be assumed.

### 1d. The guest's own simulation, the rest of `outside` — ~3 ms, and it is NOT ours

`outside` minus the walk is guest code: the title simulating. It cannot be optimised
directly, but **it is now paid twice per second**, and one question is worth asking
once: does the title's simulation run per PRESENT or per its own timer? If per
present, a 60 fps cap doubles its cost and a 45 fps cap would be a better trade than it
looks. `CZ_FPS_CAP` makes that a one-run experiment — compare `outside` per SECOND at
30, 45 and 60.

---

## 3. TIER 2 — `record`, 7.4 ms, of which `vertex` is 662 ns/draw

### 2a. THE STREAM GUARD STILL HASHES 63-72 MB EVERY FRAME — EXPECTED 2-3 ms

This is the biggest item in tier 2 and the one with the clearest statement. The
cross-frame store's content guard is what `rec.vertex` is mostly doing. Part 47 made
that hash **four times faster** (9.0 -> 35.7 GB/s, four accumulators). **It did not
make it SMALLER.** At 35.7 GB/s, 72 MB is 2.0 ms every frame — and at 60 fps, 4 ms
every second more than at 30.

The question nobody has asked: **why do ~3,100 first-touch streams a frame require 72
MB of hashing at all?** Three lines of attack, cheapest first:

* **A bounded prefix, as the texture guard already has.** `CZ_VK_TEX_GUARD_BYTES`
  exists and is the exact precedent; `CZ_VK_STREAM_GUARD_BYTES` exists too and its
  bound is 16 KB (part 24). The size histogram prints with the stats. **Read it before
  choosing** — part 46 refuted "raise the bound" for the UI buffer by measurement, and
  the same discipline applies in reverse here.
* **Earn exactness per stream, as part 46's guard budget does.** A stream the store
  catches CHANGING is hashed exactly; one the cheap guard is proved able to see is
  demoted. That machinery already exists for the dynamic guard and is not applied here.
* **Do not hash at all where the guest tells us.** A stream whose fetch constant did
  not change and whose draw did not change is a candidate for a cheaper identity.

**The arm is `CZ_VK_GUARD_FOLD_SERIAL=1`** for the fold and the existing store arms for
the rest; the operator's part-48 session measured the fold at **6.9 ms**, so this
region is known to be worth several milliseconds on their machine.

### 2b. `recordVertex`'s per-attribute walk — after 2a, and only if it still dominates

`DecodeVertexFetch` + `PhysToVa` + `GuestRangeOk` + `BindVertexBufferCached` per
attribute at 3-5 attributes a draw. Cheap individually. **Re-read the split after 2a
lands** — part 47's experience is that removing the guard changes the ranking inside
`record` completely.

---

## 4. TIER 3 — `other`, 4.4 ms, and its residual is STILL unnamed after two splits

```
other 717 ns/draw = shader 96 + key 36 + pipeline 124 + begin 101 + fetch 114
                    + tail 40 + residual 206
```

* **`residual` 206 ns/draw (~1.4 ms) — SPLIT IT AGAIN.** Two splits have not named it,
  and splitting has found three items in two parts where reading code found none
  (gotcha 327). This is the highest-yield-per-hour item in the document.
* **`pipeline` 124 ns/draw (~0.8 ms).** A `std::map<PipelineKey, VkPipeline>` — a
  red-black tree walked once per draw. Part 47 turned the sampler `std::map` into a
  flat table; this is the same shape and the last one left. Note the plan's §5
  prediction in part 48 was that this would dominate `other` and it did not — it is
  17% — so it is worth doing and is not the headline.
* **`begin` 101 ns/draw (~0.7 ms).** `BeginFrame` + `BeginRendering`, which are
  per-FRAME work amortised over every draw. At 60 fps it is paid twice as often per
  second. An amortised cost divided by 6,000 draws looks like nothing and is not.
* **`shader` 96 ns/draw (~0.6 ms).** Two `unordered_map` lookups per draw to turn two
  hashes into shader records. The consecutive-draw repeat rate is almost certainly
  high; a one-entry cache of the last (vs, ps) pair is a few lines. **Count the repeat
  rate first** — part 18 added counters and deliberately did not act until they
  justified it, which is the right order.

---

## 5. TIER 4 — the present path, ~1.2 ms and now paid twice a second

`readback` went **2.7% -> 4.7%** of the frame between 30 and 60 fps, for the structural
reason that it is per-frame: our renderer draws into an offscreen image, **reads it
back to host memory**, and blits it through SDL. That is a full-frame copy across the
PCIe bus every present.

The architectural answer is to present from a real Vulkan swapchain and never touch
host memory. That is invasive — it changes the resolve/snapshot path and the window
module — and it is last for the same reasons multithreaded recording is. But it is now
worth naming, because at 60 fps it costs double what part 48 priced it at, and because
**the GPU is idle**: it has capacity to do the blit itself.

---

## 6. THE ORDER, and what it should add up to

| # | item | expected | cumulative at 7,000 draws |
|---|---|---|---|
| — | measured baseline | — | **28.3 ms (35.7 fps)** |
| 1a | skip runs of type-2 filler | −1.5 | 26.8 |
| 1c | the per-packet preamble | −2.5 | 24.3 |
| 2a | the stream guard's BYTES | −2.5 | **21.8 (46 fps)** |
| 3 | split the `other` residual, then act | −1.5 | 20.3 |
| 3 | pipeline map → flat, shader pair cache | −1.0 | **19.3 ms (52 fps)** |
| 1d | ask what the guest sim costs per present | ? | ? |
| 5 | present without the readback | −1.2 | 18.1 |

**Tiers 1-3 are mechanical and individually verifiable.** 16 ms at 7,000 draws needs
tier 5 or better than expected elsewhere; 20 ms is reachable without any architectural
change.

---

## 7. HOW TO MEASURE ANY OF THIS

The rules parts 47 and 48 paid for, with one important change at the top:

* **FRAME TIME IS USABLE AGAIN.** The 32 ms floor is gone below 5,000 draws entirely
  and is 28 ms above it. Quote fps and ms directly. Gotchas 237/238's pinned share was
  a workaround for a ceiling that no longer exists — do not reach for it first.
* **EVERY CAMPAIGN NEEDS A NULL-CONTROL ARM** — one whose change cannot move the
  statistic you are reading. Whatever it reads IS the floor (gotcha 331). Part 48
  believed a fake 8% for an hour without one.
* **A "matched draw band" of 3,000-8,000 is TOO WIDE for a per-draw statistic**:
  `record` varies 1,204 -> 1,033 ns/draw across it. Use 4,000-6,000 or narrower and
  check the drift; `tools/part48_draw_read.py` prints it and refuses a verdict without
  `--null`.
* **For the WALK, quote ns per PACKET** (`tools/part48_walk_read.py`), which also
  checks that the arms walked the same packet MIX before believing the comparison.
* **Quote MILLISECONDS, not phase shares** (gotcha 320).
* **Every item gets a same-binary arm and a counter proving it engaged** (gotcha 151),
  and each lands in **its own commit**.
* **A gate that would pass whether or not your change is correct has not tested it**
  (gotcha 322). Both PM4 boundary oracles are blind inside `ExecutePacket`, which is
  where all of tier 1 lives — the incumbent implementation is the oracle, as
  `CZ_PM4_VERIFY_BULK_REGS` and `CZ_PM4_VERIFY_COUNTERS` both do, with a poison arm
  first.
* **An item can be perfect on its own counter and a net loss** (gotcha 330). Count both
  populations before building a cache change: part 48's stream-cache stamp cut
  allocations 45x and made the phase 8.5% slower.
* **Run `tools/part47_gates.sh` before handing the operator a binary**, and launch
  through a guarded script — two instances at once measures contention and it has
  happened twice.
