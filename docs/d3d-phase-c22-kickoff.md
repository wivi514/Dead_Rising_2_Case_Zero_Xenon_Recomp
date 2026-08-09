# D3D phase C, part 22 hand-off (for part 23). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c21-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session twice now.

## The one-paragraph state of the port

Case Zero boots, renders and plays. Save and load are a closed round trip. The
view-dependent whole-frame black is solved. Ordinary gameplay is 31 fps and closed at the
title's own two-vblank pacing; **crowds are the open performance item and are CPU-bound in
our runtime**. Part 22 built the cross-frame stream store the last three parts were
pointing at: stream copying falls from 61-66 MB a frame to 0.23, and `streams` leaves the
top of the CPU plan. What is open on the picture is unchanged: the shadow cascade,
mipmaps, NPC part meshes, the magenta sky / colour-grading LUT, and the prologue
cinematic.

## READ THIS BEFORE MEASURING ANYTHING

The first three are carried forward unchanged, because each still costs a session. The
fourth is new and is the one part 22 would most like to have known at the start.

1. **The noise floor of a crowd frame-time A/B is 10-13% at one run a side** (gotcha 229).
   `tools/frame_perf_bins.py` bins by draw count, which is necessary and not sufficient.
   **Three runs an arm, alternated a/b/a/b/a/b**, and read the per-run spread before the
   delta. One 620 s run is ~10 minutes, so a real A/B here is an hour.
2. **DO NOT PIN THE GPU CLOCK** (gotcha 231; gotcha 219 retracted in part). Awake, this
   workload governs itself to P5, mean 524 MHz, 32% utilisation. Sample and quote with
   `tools/gpu_clock_sample.py`. **A low clock at LOW utilisation is the governor being
   correct.**
3. **A profiler is instrumentation, so break it on purpose before trusting it**
   (gotcha 228), and **a comparison that only ever reports 100% has not been shown capable
   of reporting anything else** — salt it and require 0% (gotcha 234).
4. **NEW AND THE MOST IMPORTANT ONE HERE, gotcha 237: a MEAN over frames measures this
   title's PACING FLOOR, not your change.** `tools/frame_perf_bins.py` reports means, and
   it scored part 22's store at **+1.7% against a +1.3% null** — which reads as "below the
   noise floor, not worth the session". Read as medians, the same data is **44 ms -> 32 ms
   at ~3,700 draws**, and the number that proves it is neither: the share of frames within
   1 ms of a 16 ms multiple goes **10% -> 97%**. A CPU saving converts to frame rate only
   where the frame is above one vblank floor and within reach of the next; at ~6,500 draws
   both arms were already parked on the 48 ms three-vblank floor and 5 ms could not reach
   32. **Quote the pinned-share — it is far more sensitive than the mean.**
5. **NEW, gotcha 235: match the LOOKBACK of a measurement to the LIFETIME of the thing
   you are designing.** Part 21 measured how often a stream's bytes changed since LAST
   FRAME and got 0.0016%. The store built on it compares against the last COPY, and its
   counter immediately read **~20 stale streams a frame** — two orders of magnitude more.
   Neither number is wrong; an address the guest recycles after a gap is invisible to a
   frame-to-frame comparison by construction. The tell was that the instrument's window
   (`g_prevStreamKeys`, cleared every `BeginFrame`) was a parameter nobody chose.
6. **NEW, gotcha 238: a zeroed profiler column is not a saving until you check the
   residual.** `streams` reads 0.0% with the store on and the copying really is gone — but
   the guard that makes the store safe runs outside `ProfScope(streams)` and inside
   `record`'s scope, so `record` nearly doubled and the true net was 3.3 ms, not 5.5.

## Where part 23 starts, in order

**The order has changed, because part 22 removed the item that was first.**

1. **CPU/GPU OVERLAP — now unambiguously the biggest item, and the only architectural
   one.** A crowd frame is CPU work then GPU work strictly in series, because
   `SubmitAndWait` blocks immediately after submitting; the GPU is idle 68% of every frame
   and the driver correctly governs it down. `CZ_VK_NO_SUBMIT=1` is the ceiling arm that
   measured the bound without building it (~1.45x). It needs the per-frame readback off
   the critical path — a real `VkSwapchainKHR` present instead of SDL's CPU blit — and a
   second frame-in-flight arena. It touches two things `CLAUDE.md` flags as deliberate:
   the synchronous submit, and phase 3's renderer/window separation. Budget a session.
   **Part 22 made this cheaper in one way and added one obligation.** Cheaper: the streams
   that dominated the arena's high-water are now in a store that is not per-frame, so the
   second arena is smaller. The obligation is written at the line that breaks —
   `UploadStream`'s stale path **overwrites a store slot in place**, which is safe only
   because the submit is synchronous. With frames in flight it must allocate a fresh slot
   instead, and if it does not the failure is a wrong mesh, silently.
2. **The PM4 walk — ~12 ms, and it is a register-write loop.** 90,316 packets a frame
   carrying **815,020 register-write dwords** at 15.3 ns each, which is essentially all of
   it. Biggest lead: `LOAD_ALU_CONSTANT` reads from GUEST memory, so a crowd frame streams
   ~3.3 MB through the cache one `GuestLoad32` at a time where a contiguous run wants one
   bulk byte-swapping pass. Cheapest: `ExecutePacket` does 2-4 `lock`ed atomics per packet
   while `gpu/pm4.h` states everything here runs on one thread — ~2 ms.
   `Source::operator()`'s `%` is ring-level only and is NOT on the hot path; do not spend
   time there. **`pm4.cpp` is under two exit-1 capture oracles — run them after every
   change.**
3. **Narrow the FIRST-VISIT STUTTER** (open-items 0b, §6as). 16.7 ms a frame on arrival at
   new material, 6.1-6.3% flat on revisit, not a draw-count effect. **Pipeline compilation
   is excluded by measurement — do not re-derive it** (gotcha 232). What is left in
   `other` is per-draw: two `std::map<uint64_t>` shader lookups, a `std::map<PipelineKey>`
   lookup with a 40-byte `memcmp` comparator, the fetch-constant decode loop and the
   vertex-attribute loop. **Next cheap step: a per-draw census of sampler slots and vertex
   attributes.** Note `other` now also carries the store's guard hash (see below), so
   re-baseline it before attributing anything.
   Reproducing this needs virgin material — an operator, or a recipe entering an area no
   run has entered. Every instrument here is blind to it by construction.
4. **The remaining picture defects**, unchanged and all worth re-testing now that neither
   the whole-frame black nor a collapsed luminance ladder contaminates the evidence: the
   shadow cascade (open-items 3), mipmaps (4), NPC part meshes (3d), the magenta sky /
   colour-grading LUT (6), the prologue cinematic.
5. **The safehouse door renders as a fully saturated white slab** — an observation with no
   counter and no measurement, carried from part 19. `CZ_VK_SKIP_TEX` to name the address
   is the cheap first move.

**Small and specified, if a session wants a warm-up:** open-items 0a-i, `CopySwapped` as
one `pshufb` rather than the 10-instruction SSE2 sequence it currently compiles to
(`-msse4.1 -mavx` is applied to the `ppc_image` target, not the runtime). **Its value fell
by an order of magnitude when the store landed** — stream copying is now 0.23 MB a frame,
so the beneficiary is texture upload only. Recorded mostly because the reason it shrank is
the lesson: sizing a micro-optimisation before the structural change lands prices it
against the wrong baseline.

**Retired — do not re-derive:** §1a hypothesis A (vertex/index bind caching; ~1.4 ms,
permanently below the noise floor); pipeline compilation as the first-visit cost; §1b's
"the fix is a cheaper key" (it was a longer lifetime); **and the whole of open-items 0a,
which is built.** In particular do not re-derive the `mprotect` design — part 22 measured
that every stream this title rewrites in place is exactly 80 bytes, so a 512-byte exact
guard covers the observed population and page tracking was never needed. §6av records the
design that was not built, including the hazard that would have bitten it (`kernel/vfs.cpp`
reads file data into guest memory with `fread`, and `read(2)` into a `PROT_READ` page
returns EFAULT rather than faulting, so a level reload into a protected page would have
failed silently).

## What part 22 delivered

* **The cross-frame stream store** (open-items 0a, §6av). 97-99% of first-touch streams
  served across the frame boundary; **copied bytes 61-66 MB/frame -> 0.23**; `streams`
  11.1% of a crowd frame -> **0.0%**; net CPU saving 3.3 ms after the guard. A second
  buffer rather than a region of the arena, deliberately, so none of the arena's
  exhaustion machinery moved.
  **In frame rate that is 44 ms -> 32 ms at ~3,700 draws and roughly nothing at ~6,500**,
  for the pacing-floor reason above. Read §6av's two tables before quoting either number.
* **The census now names WHICH streams are rewritten in place**, which is what collapsed
  the invalidation question. All 30 are exactly 80 bytes and all are declared vertex
  bindings.
* **`GUARD MISSED`** — the correctness counter for the store, with the poison arm as its
  control (0 unpoisoned, 240,652 of 240,652 poisoned).
* Gotchas 235 (lookback vs lifetime) and 236 (an instrument that writes files must
  complain when it cannot — `CZ_VK_FRAME_DUMP` silently wrote nothing into a
  non-existent directory, which looks exactly like a renderer that drew nothing).

## Two things about the store the next session should hold in mind

* **The guard's cost lands in `other`, not in `streams`.** `ProfScope(streams)` still
  wraps only the copy, so the store moves work out of a timed column into an untimed one.
  `guard read MB/frame` on the `[vkprof] store` line is what prices it; do not read a
  fallen `streams` as the whole saving without looking at `other`.
* **Eviction is a whole drop, on purpose.** An LRU with compaction would have to MOVE live
  streams, which is the copying the store exists to remove. `flushes` is on the profile
  line and is 0 in a crowd after one growth to 256 MB. If it ever is not, that is the
  evidence for building the harder thing — and not before.

## Gates, all run on the part-22 binary

`--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (24,527,474 packet lengths
agreeing, every indirect buffer tiling exactly). **The picture WAS re-checked this part,
because the store can only fail by drawing the wrong mesh**: capture E2 at frame 576 reads
+0.9590 identity with the store on against +0.9596 with it off, and the two arms' own
frames correlate +0.9998/+0.9934/+0.9929/+0.9921 at matched indices.

**A1's strict-prefix gate is BIMODAL** and the standing advice is unchanged: quote A5.
