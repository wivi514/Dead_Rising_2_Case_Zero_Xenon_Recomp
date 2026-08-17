# Part 53 kickoff — part 52 made ONE THREAD smaller; part 53 is where the work gets SPREAD

Written at the close of part 52 (2026-08-17). **This is the LIVE hand-off**, superseding
`part52-kickoff.md`.

---

## START HERE — the one thing part 52 did not do

`docs/perf-plan-part52.md` §1 names two strategies and commits to one:

> **(a) Make that thread's work smaller.** Every plan so far. …
> **(b) Move work off that thread onto cores that are doing nothing.** Never attempted …
> The prize is larger than (a) by construction: it is not a percentage of a phase, it is
> the whole phase moving off the critical path.
> **This plan puts (b) first.**

**Part 52 shipped four items and all four are (a).** Not one line of work was moved onto
another core. That happened by following the plan's §10 ORDER, which front-loads three
serial items ahead of the first parallel one — the order and the prose disagree, and
nobody noticed until the operator asked at the end of the part whether the CPU work was
being spread across cores. **It is not.** Measured today on this build at ~6,200 draws:

```
process total   223.8% of one core = 2.24 cores of 16 (14.0% of the machine)
busiest thread  91.7%  -- it is 41% of all CPU this process is using
VERDICT: PARTIALLY parallel. The critical path is likely still one thread.
```

**~13 cores are idle**, and at the operator's heaviest place our pump reads **97.5-97.8%
on CPU** — saturated, with only 0.11-0.12 ms of that blocked. There is no slack left in
strategy (a) at that load.

**So part 53's job is item 1.1, and its job is (b).** Everything below is in service of
that.

### What "spread across all cores" can and cannot mean here

Do not promise 16 busy cores; the architecture forbids it. **The PM4 walk is inherently
serial** — the stream's meaning is positional, and `pm4.h` states why: a queue that defers
a draw past the next `SET_CONSTANT` renders it with the following draw's state, which
looks like a shader bug and is not one. What can move is the **pure, order-independent**
work the walk currently does inline:

| item | what moves | size | risk |
|---|---|---|---|
| **1.1 parallel content guards** | a pure function — reads guest memory, returns a `uint64_t` | **~5.3 ms** | medium |
| 1.2 parallel texture untile | a pure address swizzle into staging | −1..2 ms | med-high |
| 1.3 readback off the pump | a copy with no dependency on the next frame | ~0.55 ms | low |
| 1.4 parallel command recording | the ORDERED work — the only one that touches draw order | ~4 ms at 4 workers | **high** |

Realistically that is **3-5 busy threads, not 16**, and saying so up front is better than
discovering it in the write-up.

---

## THE PLACE TO MEASURE FROM — the operator found it, and it is NOT at the cap

This is the second-most important thing in the document, because without it none of the
above can be shown to help a player (`phase5-notes.md` §6ci §12).

* the operator's soak sustains **7,162-7,529 draws with peaks to 8,562**, held for three
  minutes — heavier than any place this project has measured;
* **0% of its frames sit on a pacing rung.** CPU-bound, not pacing-limited, so **items buy
  frames here rather than headroom**;
* uninstrumented it is roughly **16.7-18.7 ms, ~53-60 fps** (`CZ_VK_FRAME_STATS` printed
  its own bill at **3.21-3.23 ms/frame** right there, a fourth direct confirmation);
* the pump is **97.5-97.8% on CPU**, blocked 0.11-0.12 ms.

**Ask the operator for a three-minute SOAK there in every arm of every future A/B.** It is
the best measurement shape this project has found: standing still makes frames DENSE, so
one draw bin held **7,773 frames against 6,079** where a walked A/B's best bin held 1,348
— **significance +211 against +13**. Their route also survives spawn variance and dwell
time, because the comparison is binned by draw count.

**Contrast with the headless route, which is now unusable for frame time at the shipped
cap** (§6ci §5c): both arms land on the rung and the A/B reads zero whatever the change was
worth — an UNMEASURABLE result, not a null one. `tools/part52_item_campaign.sh` works
around it by raising `CZ_FPS_CAP=120` in every arm; what it then reports is a CPU saving,
not a frame rate anyone sees.

---

## THE BUDGET AT THAT PLACE — and item 1.4 is priced

The operator's soak, phase milliseconds, **after** part 52's items:

| phase | ms | note |
|---|---|---|
| `record` | 8.69 | **but see below — 39% of this is not recording** |
| `other` | 4.11 | pipeline lookup is now 38-43 ns/draw of it |
| `textures` | 3.13 | contains the TEXTURE guard, ~1.98 ms |
| `outside` | 2.77 | the PM4 walk; the memo took ~2.5 ms out of it |
| `constants` | 1.34 | |
| `readback` | 0.55 | item 1.3's real price |
| accounted total | 20.66 | measured frame 23.90 with both instruments |

**ITEM 1.4 WAS PRICED AND 39% OF IT BELONGS TO ITEM 1.1** (§6ci §13). The stream content
guard is charged to `record`, because `UploadStream` runs inside the `recordVertex` scope
while `ProfScope(streams)` wraps only the copy. Item 1.1 was priced off the `GuardFold`
SYMBOL and item 1.4 off the `record` PHASE, so **the two shared the same milliseconds**.
Split (`g_prof.streamGuard`, new in part 52):

```
record 1,007 ns/draw = state 141 + vertex 188 + index 161 + GUARD 391 + residual 126
```

| of the operator's heaviest frame | ms |
|---|---|
| stream guard inside `record` → **item 1.1** | **3.37** |
| texture guard inside `textures` → **item 1.1** | **1.98** |
| **item 1.1 total** | **~5.3** |
| real `vkCmd*` recording → **item 1.4** | 5.32 ceiling, **~4.0 at four workers** |

The split also **reconciles the phase table with the symbol profile for the first time**:
391 ns/draw x 6,508 draws = 2.54 ms of stream guard, `GuardFold` reads 4.52 ms by `perf`,
and the 1.98 ms difference is the texture guard `textures` should contain. Gotcha 343.

**So 1.1 and 1.4 are the same size and not the same risk**, and 1.1 wins on every other
axis: it is pure, it has a byte-exact oracle (the serial hash), and it has no ordering
constraint. Command recording owns renderer state, must preserve draw order, and has no
gate that would catch getting it wrong.

---

## THE ORDER TO TAKE PART 53

| # | item | expected | risk | note |
|---|---|---|---|---|
| **1** | **1.1 parallel content guards** | **~5.3 ms** | medium | **the whole point of the part.** Verify arm and control arm before any number is quoted |
| 2 | 1.3 readback off the pump | ~0.55 ms | low | the cheapest parallel win; repriced from `readback` with frame stats off, so it is 0.5-0.6 and not the 1.2 the old plan guessed |
| 3 | 1.2 parallel textures | −1..2 ms | med-high | only after 1.1 proves the worker pool |
| 4 | 2.3 audit the always-on censuses | −0.1..0.3 ms | none | |
| 5 | 3.3 `_int_malloc` on the frame path | −0.2..0.3 ms | low | part 52 removed ~1,500 mallocs/frame from the shader path as a by-product; it still reads 1.08-1.41% of the pump, so there is another caller |
| 6 | 2.2 frame-stats sampling | correction | none | |
| 7 | 3.4 `memcmp` at 3.4-4.2% | ? | — | **measure before touching**; part 52 added the memo's own `memcmp` to this symbol, so it is no longer only the state cache |
| 8 | **1.4 parallel command recording** | ~4 ms at 4 workers | **high** | now supported by numbers rather than ambition, but behind 1.1 on size and ahead of it on risk. Secondaries inherit no state but the render pass, and draw ORDER is semantic |
| 9 | 4.2 inline the PM4 walk | −2.2 ms ceiling | **high** | last, and only with a poison arm |

### Building item 1.1 — what the plan already specifies, and what part 52 adds

The plan's §3 item 1.1 has the design (`1.1a` speculative pre-hash from `persistCache`'s
previous-frame working set, falling back to inline hashing on a miss so correctness never
depends on the prediction). Three things part 52 adds to it:

* **Pre-register the hit rate**, as the plan says: below ~80% served by a completed
  pre-hash, the item is not working and the frame-time result will be noise. Part 52's
  memo hit 100.0% and its `[vkprof] shader memo:` line is the shape to copy.
* **Build the verify arm FIRST and poison it.** Part 52's plan had a design that sounded
  complete and was refuted by its own verify arm on the first run (gotcha 342). A wrong
  guard hash is a stale mesh — the part-46 defect class — so
  `CZ_VK_VERIFY_PARALLEL_GUARD=1` computes both and aborts on disagreement, and a poison
  arm must make it fire before its silence means anything (gotcha 30). **Check that the
  verify arm does not wreck the statistics beside it**, which part 52's did until one
  comparison stopped it.
* **The race already exists.** The guard reads guest memory while the guest writes it
  today; a torn read produces a different hash, which reads as "changed", which is safe by
  construction. Moving it to a worker widens that window rather than creating a new class
  of bug. Say so in the code and keep the fallback.

---

## WHAT PART 52 SHIPPED — do not re-derive any of this

| plan item | result |
|---|---|
| **1.0 `BindShader` memoization** | **14.16% of the pump thread → 0.00%.** ~1.8-2.0 ms of the operator's frame at 5-7k draws, ~2.5 ms at their soak |
| **3.2 pipeline lookup** | `std::map` → hash + one-entry front cache. **110-112 → 38-43 ns/draw**, ~0.43 ms |
| **2.1 `Count` → `COUNT`** | ten sites, ranked by the counter DUMP; the plan's "28 sites in `DoDraw`" was counted by reading the source |
| **4.1 re-split `outside`** | one thread-CPU clock read per report; the pump is BLOCKED only **0.09-0.12 ms of a 16 ms frame**, which retires part 50's reading of that residual as guest simulation |

### The finding that matters most is a method, not a millisecond

**The plan's memo key was refuted by the verify arm the plan itself insisted on.** It
specified `(va, size)` plus the microcode's first and last dword, and argued the failure
mode was benign because a wrong hash would read as a cache MISS:

```
[pm4] SHADER MEMO MISMATCH #2: VS va=00000000 size=102 — memo said f2ef2d2f8de976d0,
      the microcode hashes to 8ed00911a7bc1eb1 (first=F1555004 last=A9A9C68D)
[pm4] SHADER MEMO MISMATCH #3: VS va=00000000 size=102 — memo said 8ed00911a7bc1eb1,
      the microcode hashes to f2ef2d2f8de976d0 (first=F1555004 last=A9A9C68D)
```

Two different shaders, same size, same both probe dwords, alternating — microcode is far
too regular for two dwords to identify it. And the wrong answer is **another real shader's
hash**, which IS in the cache, so it would have bound a real, wrong, translated shader
silently past every gate this project owns. **Gotcha 342.** The shipped memo compares the
whole microcode with `memcmp` — exact, and still ~30x cheaper, because the hash's cost is
a serial multiply chain and not a memory read.

### Two of the plan's own numbers are corrected

* **Item 2.1 is one site, not 28.** Of ~62.5 M plain-`Count` calls, 52,901,332 (84.6%) are
  the single site in `VkRenderer_Draw`. `VkRenderer_DumpStats` already prints every
  counter's call count — the statistic that ranks these sites — and reading the source
  ranks them by how alarming they look instead.
* **`BindShader` is ALU-latency bound, not memory bound** (~71% of its samples on four
  `imulq`s), which is the OPPOSITE of `GuardFold` (memory-bound at ~10 GB/s, plan §0 fact
  3). They need opposite fixes. **This matters directly for item 1.1**: a faster hash buys
  nothing there, only fewer bytes or more memory-level parallelism can.

---

## THE OPERATOR'S SESSIONS — judged, controlled, and then soaked

Three sessions in one evening (`tools/part52_operator_session.sh`), and each answered a
different question. **Their verdict on part 52: "performance is better."**

**1. A whole-map lap** (§6ci §10), profiler on, frame stats deliberately off. Against part
51's session on the same machine, `outside` −2.35 ms and `other` −0.44 — the two columns
the items live in — with everything else within ±0.30. *A cross-session comparison, so
read it for mechanism, not size.*

**2. The same-binary A/B** (`ARM=ab`, §6ci §11), which **supersedes it as a measurement of
size**: `outside` 3.06 → 4.42 ms, the only column that moved, everything else within
±0.21. Frame time +9.9%/+17.6% at 4-5k and +9.6%/+9.5% at 6-7k, with the light bins reading
**+0.0%** as the experiment's own null. `other` correctly did NOT move, because the
pipeline change has no run-time switch and rides in both arms.

**3. The soak** (§6ci §12), their own idea and the best measurement here — see "the place
to measure from" above.

**The memo is ~1.8-2.0 ms of their frame at 5-7k draws and ~2.5 ms at their soak.** It is a
SLOPE, not a number: it runs per shader-load packet, and the soak measured 3,010-3,047
loads/frame against 2,224 on the walk. **Quote the draw count with any per-draw or
per-packet saving.** With the pipeline (~0.4) and counters (~0.3) added back, part 52 is
**~2.5-3.0 ms of the operator's frame**.

---

## WHAT IS OWED

* **The parallelism work itself.** See START HERE. This is the whole of what part 52 left
  undone.
* Their two deferred picture items — **00m decals, 00n a sign and items at distance** —
  remain deferred. If part 53's CPU work stalls, these are the alternative.
* **The operator's standing instruction**: *"prepare a whole plan to fix CPU performance
  issue and we'll start it in a fresh conversation."* `perf-plan-part52.md` is that plan
  and it is still live; its §9b records what part 52 corrected in it.

---

## MEASUREMENT RULES THAT CHANGED IN PART 52

* **A capped frame cannot report a CPU saving.** Raise `CZ_FPS_CAP` in EVERY arm and say
  the number is a saving, not a frame rate. Better: measure at the operator's soak, which
  is not capped.
* **Two items priced off two instruments can share the same milliseconds** (gotcha 343).
  Reconcile the phase table against the symbol profile before pricing anything.
* **A per-packet saving is a slope, not a number.** Quote the draw count with it.
* **`frame_perf_bins.py`'s `pinned%` is defined for a 16 ms ladder.** At another cap it
  reads "on the second rung", not "on the ladder". Name the ladder.
* **A verify arm can wreck the statistic beside it.** Part 52's re-inserted into the memo
  on every load and reported 46x more evictions than misses until one comparison stopped
  it. Gotcha 7, one level in.
* **The shader-cache NAME diff must run in any part where `CZ_SHADER_DUMP` was set** —
  for a performance part that is every part, because the recon scripts set it. It found
  `ps_bd5d8eb053e36a84`: in the dumps, not in the cache, never bound by any run, so the
  miss counter read 0 and the COUNT matched at 435 = 435 with different members.

---

## STANDING STATE

* Runtime defaults unchanged from part 51: 60 fps cap, host vsync off, 100 us ring tick.
* **New arms**: `CZ_PM4_NO_SHADER_MEMO=1`, `CZ_PM4_VERIFY_SHADER_HASH=1`,
  `CZ_PM4_VERIFY_SHADER_POISON=1`, `CZ_VK_NO_PIPELINE_CACHE1=1`. All in
  `docs/instruments.md`.
* **New instrument lines**: `[vkprof] shader memo:`, `[vkprof] pipeline lookup:`,
  `[vkprof]   pump thread:`, and **`GUARD` inside the `record` split**.
* **New tooling**: `tools/part52_item_campaign.sh` (frame-time campaign above the cap),
  `tools/part52_operator_session.sh` (single arm or `ARM=ab`, `PROF=N`);
  `tools/part52_recon.sh` gains `ENVX=` and `NO_DWARF=1`, which makes it the A/B harness
  for an item rather than only a survey.
* **The shader cache is 436** (was 435), and `assets/shader_spv_a2m` was topped up to match
  it — the two now have identical membership.
* Artifacts: `~/DR2CZ-troubleshooting/part52/` — `p52i10_{memo,ctrl}.*` (the symbol A/B),
  `frame/` (the headless campaign, 9 runs); `~/DR2CZ-troubleshooting/part52-operator/` —
  `part52.*` (the lap), `ab1_walk/` (the walked A/B), `part52{on,off}.*` (**the soak**).
* **Gates at close: ALL CLEAN.** `--smoke`; both PM4 oracles on B1; the switch gate (0
  defects); the dimension census (0 disagreements); `no translated shader` = 0;
  `truncated=0`; deepest file #83 `cinezombie.big`; **A5 exit 0 with 4 permutation windows
  and 0 real** (part 51 also had 4); **E3 best of five +0.8771, 4 of 5 agreeing on layout**
  (part 51 +0.8043 of fourteen, part 50 +0.8820 of five — an animated backdrop, so read the
  spread and not the point, gotcha 133).
