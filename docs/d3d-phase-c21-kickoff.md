# D3D phase C, part 21 hand-off (for part 22). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c20-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session twice now.

## The one-paragraph state of the port

Case Zero boots, renders and plays. Save and load are a closed round trip. The
view-dependent whole-frame black is solved. Ordinary gameplay is 31 fps and closed at the
title's own two-vblank pacing; **crowds are the open performance item and are CPU-bound in
our runtime**. Part 21 answered the largest open question in the CPU plan — the stream
cache copies 74-77 MB a frame that it copied last frame — and moved the arena growth off
the draw path. What is open on the picture: the shadow cascade, mipmaps, NPC part meshes,
the magenta sky / colour-grading LUT, and the prologue cinematic.

## READ THIS BEFORE MEASURING ANYTHING

Carried forward from part 20 unchanged, because all three still cost a session each.

1. **The noise floor of a crowd frame-time A/B is 10-13% at one run a side** (gotcha 229).
   `tools/frame_perf_bins.py` bins by draw count, which is necessary and not sufficient.
   **Three runs an arm, alternated a/b/a/b/a/b**, and read the per-run spread before the
   delta. One 620 s run is ~10 minutes, so a real A/B here is an hour.
2. **DO NOT PIN THE GPU CLOCK** (gotcha 231; gotcha 219 retracted in part). Awake, this
   workload governs itself to P5, mean 524 MHz, 32% utilisation, 28.6 W. Sample and quote
   with `tools/gpu_clock_sample.py`. **A low clock at LOW utilisation is the governor
   being correct** — the GPU is idle 68% of every frame because `SubmitAndWait` blocks
   straight after submitting.
3. **A profiler is instrumentation, so break it on purpose before trusting it**
   (gotcha 228). And part 21's version of the same rule, which is now gotcha 234:
   **a comparison that only ever reports 100% has not been shown capable of reporting
   anything else.** Salt it and require 0%.

## Where part 22 starts, in order

**Item 1 is now fully specified and sized, which it was not when part 20 wrote it.** The
rest of the order is unchanged from the part-20 list minus what part 21 did.

1. **THE CROSS-FRAME STREAM CACHE — 5.6-5.9 ms of a ~50 ms crowd frame (≈11%), and it is
   the largest named CPU item that is not architectural.** Part 20's item 1 said "count
   hits, misses and bytes before writing anything, the two readings need opposite fixes".
   Counted (`docs/phase5-notes.md` §6at, `CZ_VK_STREAM_CENSUS=1|2`):

   | at ~6,400 draws in a ~50 ms frame | |
   |---|---|
   | hit rate WITHIN a frame | 93.6-94.0% |
   | misses per frame | ~2,000, average 37 KB |
   | **bytes copied per frame** | **74-77 MB** |
   | copied bytes repeating LAST frame's key | **95-97%** |
   | of repeated keys, content unchanged | 99.9984% |

   vertex bindings 61-63 MB, dependent fetches 11 MB, index buffers 1.8 MB.

   **It is real copying and the fix is the cache's LIFETIME, not a cheaper key.** Three
   requirements, and the third is not optional:
   * **Storage that outlives the frame.** `R->arena` is a bump allocator reset at every
     swap. Persistent streams need their own allocation and an eviction policy — and note
     the arena high-water is already 139-161 MB, so this is a real memory decision, not a
     free one.
   * **INVALIDATION.** 0.0016% of repeated keys change in place — 164 of 10,154,820 over
     two runs, a recurring set of ~26 — and a stale vertex buffer draws the wrong mesh.
     Hashing to detect it costs what the copy costs, so the candidate is **guest-page
     write tracking**: `mprotect` on the guest map plus a `SIGSEGV` handler that must
     coexist with `cpu/crash_report.cpp`'s (which uses `SA_NODEFER` — read it first).
     Static geometry then faults never; dynamic buffers fault once a page a frame.
   * **A counter for both**, because a cache that silently serves stale data looks exactly
     like a rendering bug twenty frames later, and this project has spent parts chasing
     those.

   `CZ_VK_STREAM_CENSUS=2` is both the before-measurement and the arm that says whether it
   worked. Budget a session; do not start it inside another item.

2. **Narrow the FIRST-VISIT STUTTER** (open-items 0b, §6as). 16.7 ms a frame on arrival at
   new material, 6.1-6.3% flat on revisit, not a draw-count effect. **Pipeline compilation
   is excluded by measurement — do not re-derive it** (0.08-0.15 ms per pipeline,
   gotcha 232). What is left in `other` is per-draw: two `std::map<uint64_t>` shader
   lookups, a `std::map<PipelineKey>` lookup with a 40-byte `memcmp` comparator, the
   fetch-constant decode loop and the vertex-attribute loop. **Next cheap step: a per-draw
   census of sampler slots and vertex attributes**, to test whether first-visit draws
   simply carry more of both.
   Reproducing this needs virgin material — an operator, or a recipe that enters an area
   no run has entered. Every instrument here is blind to it by construction.

3. **CPU/GPU overlap — the biggest single win, and the only architectural one.** A crowd
   frame is ~27.7 ms of CPU then ~13.8 ms of GPU, strictly in series, because
   `SubmitAndWait` blocks immediately after submitting; the GPU is idle 68% of every frame
   and the driver correctly governs it down. Overlapping is **~1.45x, to 27-30 fps**.
   `CZ_VK_NO_SUBMIT=1` is the ceiling arm that measured the bound without building it.
   It needs the per-frame readback off the critical path — a real `VkSwapchainKHR` present
   instead of SDL's CPU blit — and a second frame-in-flight arena. It touches two things
   `CLAUDE.md` flags as deliberate: the synchronous submit, and phase 3's renderer/window
   separation. Budget a session.
   **Note the interaction with item 1:** a persistent stream cache makes the second arena
   cheaper, because the streams that dominate its high-water stop being per-frame. Doing
   item 1 first is the cheaper order.

4. **The PM4 walk — 12.5 ms, and it is a register-write loop.** 90,316 packets a frame
   carrying **815,020 register-write dwords** at 15.3 ns each, which is essentially all of
   it. Biggest lead: `LOAD_ALU_CONSTANT` reads from GUEST memory, so a crowd frame streams
   ~3.3 MB through the cache one `GuestLoad32` at a time where a contiguous run wants one
   bulk byte-swapping pass. Cheapest: `ExecutePacket` does 2-4 `lock`ed atomics per packet
   while `gpu/pm4.h` states everything here runs on one thread — ~2 ms.
   `Source::operator()`'s `%` is ring-level only and is NOT on the hot path; do not spend
   time there. **`pm4.cpp` is under two exit-1 capture oracles — run them after every
   change.**

5. **The remaining picture defects**, unchanged and all worth re-testing now that neither
   the whole-frame black nor a collapsed luminance ladder contaminates the evidence: the
   shadow cascade (open-items 3), mipmaps (4), NPC part meshes (3d), the magenta sky /
   colour-grading LUT (6), the prologue cinematic.

6. **The safehouse door renders as a fully saturated white slab** — an observation with no
   counter and no measurement, carried from part 19. `CZ_VK_SKIP_TEX` to name the address
   is the cheap first move.

**Retired — do not re-derive:** §1a hypothesis A (vertex/index bind caching; 34% and 22%
repeat, ~1.4 ms, permanently below the noise floor); pipeline compilation as the
first-visit cost; **and §1b's "nearly all cache hits, so the fix is a cheaper key" — the
hit rate is 94% and the fix is a longer lifetime, which are not the same claim.**

## What part 21 delivered

* **`CZ_VK_STREAM_CENSUS=1|2`**, and the answer to `perf-cpu-plan.md` §1b: 94% hits,
  74-77 MB still copied a frame, 95-97% of it repeating last frame's key. §6at.
* **`CZ_VK_STREAM_CENSUS_POISON=1`**, the control that made the content number believable
  and then changed it — 0 of 96,048 poisoned against 100.0% unpoisoned, and 164 real
  mismatches of 10,154,820 that the rounding had hidden.
* **The arena growth moved out of `DoDraw`** to the end of `DoSwapImpl`, with the
  black-frame count verified unchanged at one per growth against the stated prediction.
  §6au; closes the last live piece of open-items 1c.
* Gotchas 233 (a cache's hit rate and its cost are different questions; count bytes) and
  234 (salt an equality check and require 0%, because the passing state is silent).

## A note on what this part deliberately did NOT do

It measured item 1 and stopped, which is what the plan asked for. That was the right call
for a reason it did not predict: **the measurement found the 0.0016% of streams that are
rewritten in place**, and a cache written first would have discovered them as an
intermittent wrong-mesh bug weeks later, in the class of defect this project has spent
whole parts chasing. The cheap instrument found the expensive requirement.

The corollary is the standing warning: item 1's numbers are one afternoon's (gotchas
50/51/86). Re-run `CZ_VK_STREAM_CENSUS=2` on the current binary before building against
them.

## Gates, all run on the part-21 binary

`--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (24,527,474 packet lengths
agreeing, every indirect buffer tiling exactly). The picture was **not** re-checked
against capture E this part — nothing in it touches what is drawn, only how long drawing
takes — but that is an argument, not a measurement, and `tools/frame_signature.py` against
E2 is two minutes if part 22 wants it before touching the renderer again.

**A1's strict-prefix gate is BIMODAL** and the standing advice is unchanged: quote A5.

## The method notes worth carrying

* **The plan's own decision rule was written before the measurement, and it worked — but
  the plan then guessed which branch anyway, inside the same paragraph.** §1b said "a high
  miss rate and a high hit rate need opposite fixes" and then wrote "this should be nearly
  all cache hits, in which case the fix is a cheaper key". The rule survived; the guess did
  not. Write the rule, then do not append the guess to it — the next reader takes the
  guess as the finding.
* **Half of what was declared unmeasurable-by-reading was answerable by reading nine
  lines.** `ProfScope(streams)` wraps only the copy. Before believing a column contains
  what its name suggests, look at where its timer starts.
* **The control arm changed the answer, not just the confidence.** This is the argument
  for running controls even when the result "looks fine": the poison arm cost four lines
  and turned a wrong design conclusion into a right one.
