# D3D phase C, part 20 hand-off (for part 21). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c19-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
part 19's kickoff had two items already done before it was read.

## The one-paragraph state of the port

Case Zero boots, renders and plays. Save and load are a closed round trip. The
view-dependent whole-frame black is solved. Ordinary gameplay is 31 fps and closed at
the title's own two-vblank pacing; **crowds are the open performance item and are
CPU-bound in our runtime**. Part 20 re-measured that CPU frame and found the profiler
itself was wrong, which re-ordered `docs/perf-cpu-plan.md`'s own ranking; it then took
the instrumentation off the per-draw path for a measured win in the crowd bins. What is
open on the picture: the shadow cascade, mipmaps, NPC part meshes, the magenta sky /
colour-grading LUT, and the prologue cinematic.

## READ THIS BEFORE MEASURING ANYTHING

Three things part 20 established that will otherwise cost a session each.

1. **The noise floor of a crowd frame-time A/B is 10-13% at one run a side** (gotcha
   229). The recipe is 57 fixed 8-second steps against a boot whose depth in wall time
   is a distribution, so two runs of ONE binary visit different places for different
   durations. `tools/frame_perf_bins.py` bins frames by DRAW COUNT, which is necessary
   and not sufficient: a genuine null arm still moved individual crowd bins 10-13%, with
   the tool's own standard-error column reading up to 22 sigma, because consecutive
   frames in a bin share a camera and a location and are nowhere near independent.
   **Three runs an arm, alternated a/b/a/b/a/b, and read the per-run spread the tool
   prints before reading the delta.** One 620 s run is ~10 minutes, so a real A/B here
   is an hour; budget for it or do not make the claim.
2. **DO NOT PIN THE GPU CLOCK, and gotcha 219 is retracted in part.** The P8/210 MHz
   this project quoted for five sessions came from an overnight run with the MONITOR
   ASLEEP. Measured with the display awake over a full 620 s crowd run: **P5 in 182 of
   200 samples, mean 524 MHz, 32% utilisation, 28.6 W** — the same place `vkcube`
   settles on this machine, which is the control nobody had run. Sample and quote with
   `tools/gpu_clock_sample.py`; every part-20 number was taken at that governed clock.
   **A low clock at LOW utilisation is the governor being correct** (gotcha 231) — the
   GPU is idle 68% of every frame because `SubmitAndWait` blocks immediately after
   submitting, so our CPU and GPU never overlap. That is the defect, not the clock.
3. **A profiler is instrumentation, so break it on purpose before trusting it**
   (gotcha 228). `ProfScope` counted nested phases twice for the whole of this project's
   life, and every column still summed to the total. That is the class: an error that
   MOVES time between columns leaves nothing looking wrong.

## THE OPERATOR SESSION CHANGED THE ORDER — read `docs/phase5-notes.md` §6as first

19 minutes of live play across seven crowds (26,241 frames) landed after the rest of
this file was written, and it moves things. The short version:

* **The performance item is one sentence: a ~7,500-draw crowd is ~20 fps.** The areas
  are interchangeable at matched draw counts; there is no per-area problem. Below ~4,000
  draws everything is already at the title's 31 fps cap.
* **There is a FIRST-VISIT STUTTER worth 16.7 ms a frame** and nothing here could see
  it: `other` spikes to 20-26% on arrival at new material and reads 6.1-6.3% across six
  consecutive windows when the same spot is revisited. Invisible to any repeat
  measurement, which is every measurement this project makes.
* **Its cause is NOT pipeline compilation** — that was inferred three times, failed a
  pre-registered prediction, and was refuted on magnitude by the counter it needed
  (a pipeline costs 0.08-0.15 ms here, not the ~3 ms assumed; gotcha 232). `other` is
  now narrowed to the two shader-map lookups, the `std::map<PipelineKey>` lookup, the
  fetch-constant decode loop and the vertex-attribute loop.
* **`streams` is the largest draw-path term in real crowds** — 12.3-14.3%, twice what
  the headless recipe shows. Plan §1b was ranked on evidence half the true size.
* **The GPU clock retraction is confirmed seven times windowed**: P3-P0, 765-1290 MHz,
  19-48%, 31-45 W. Never pin it.
* One spike IS explained: the arena growth runs inside `BeginFrame`, which is called
  from inside `DoDraw`, so it charges `other` directly. Moving it out is small and
  named.

## Where part 21 starts, in order

**This list was re-ordered by the operator session; the ranking above it is the reason.**
Sizes are from real crowds (7,000-8,000 draws, ~50 ms), not from the headless recipe.

1. **`streams` — 7.2 ms a frame, and the largest draw-path term in EVERY area the
   operator visited** (12.3-14.3%, against 6.8% headless). This is plan §1b, which the
   plan ranks third and describes as "genuinely ambiguous": the per-frame dword-swap
   copy is cached by (address, size, endian) and cleared every frame, so a crowd could be
   nearly all hits (and 0.7 µs/draw is then the LOOKUP, wanting a cheaper key) or mostly
   misses (and it is real copying, wanting a different cache lifetime). **Count hits,
   misses and bytes copied per frame before writing anything** — the two readings need
   opposite fixes and no amount of reading the code decides it.
2. **Move the arena growth out of `DoDraw`.** `BeginFrame()` allocates and maps the new
   arena and is called from inside `DoDraw`, so a growth charges `other` — measured at
   29.8% of a frame, the largest single spike of the operator session. Small, the
   mechanism is the call graph rather than a theory, and it closes the last live piece of
   open-items 1c.
3. **Narrow the FIRST-VISIT STUTTER** (open-items 0b, §6as). 16.7 ms a frame on arrival
   at new material, 6.1-6.3% flat on revisit, not a draw-count effect. **Pipeline
   compilation is excluded by measurement — do not re-derive it** (0.08-0.15 ms per
   pipeline, gotcha 232). What is left in `other` is per-draw: two `std::map<uint64_t>`
   shader lookups, a `std::map<PipelineKey>` lookup with a 40-byte `memcmp` comparator,
   the fetch-constant decode loop and the vertex-attribute loop. **Next cheap step: a
   per-draw census of sampler slots and vertex attributes**, to test whether first-visit
   draws simply carry more of both.
   Reproducing this needs virgin material — an operator, or a recipe that enters an area
   no run has entered. Every instrument here is blind to it by construction.
4. **CPU/GPU overlap — the biggest single win, and the only architectural one.** A crowd
   frame is ~27.7 ms of CPU then ~13.8 ms of GPU, strictly in series, because
   `SubmitAndWait` blocks immediately after submitting; the GPU is idle 68% of every
   frame and the driver correctly governs it down (gotcha 231). Overlapping is **~1.45x,
   to 27-30 fps**. `CZ_VK_NO_SUBMIT=1` is the ceiling arm that measured the bound without
   building it.
   It needs the per-frame readback off the critical path — a real `VkSwapchainKHR`
   present instead of SDL's CPU blit — and a second frame-in-flight arena (139 MB
   high-water, so a real memory decision). It touches two things `CLAUDE.md` flags as
   deliberate: the synchronous submit, and phase 3's renderer/window separation. Budget a
   session; do not start it inside another item.
5. **The PM4 walk — 12.5 ms, and it is a register-write loop.** 90,316 packets a frame
   carrying **815,020 register-write dwords** at 15.3 ns each, which is essentially all
   of it. Biggest lead: `LOAD_ALU_CONSTANT` reads from GUEST memory, so a crowd frame
   streams ~3.3 MB through the cache one `GuestLoad32` at a time where a contiguous run
   wants one bulk byte-swapping pass. Cheapest: `ExecutePacket` does 2-4 `lock`ed atomics
   per packet while `gpu/pm4.h` states everything here runs on one thread — ~2 ms.
   `Source::operator()`'s `%` is ring-level only and is NOT on the hot path; do not spend
   time there. **`pm4.cpp` is under two exit-1 capture oracles — run them after every
   change.**
6. **The remaining picture defects**, unchanged and all worth re-testing now that neither
   the whole-frame black nor a collapsed luminance ladder contaminates the evidence: the
   shadow cascade (open-items 3), mipmaps (4), NPC part meshes (3d), the magenta sky /
   colour-grading LUT (6), the prologue cinematic.
7. **The safehouse door renders as a fully saturated white slab** — an observation with
   no counter and no measurement, carried from part 19. `CZ_VK_SKIP_TEX` to name the
   address is the cheap first move.

**Retired — do not re-derive:** §1a hypothesis A (vertex/index bind caching; 34% and 22%
repeat, ~1.4 ms, permanently below the noise floor — claim it from the counter or not at
all), and pipeline compilation as the first-visit cost (item 3).

## What part 20 delivered

* **The profiler exclusivity fix** (`docs/phase5-notes.md` §6aq, gotcha 228). Each scope
  now subtracts what its children consumed, and the whole-draw total is a SUM of the
  columns rather than a separately measured quantity — two statements that can disagree
  beat one that cannot.
* **The corrected crowd table**, re-measured headlessly at 6,737-6,806 draws: `record`
  6.7 ms, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3, draw path 19.9 ms
  (36.5%); PM4 walk 11.8 ms (21.6%). **Superseded for ranking purposes by the operator
  session** — real crowds put `streams` at 12.3-14.3% and the headless recipe
  under-samples it by half.
* **The per-draw instrumentation removed** — five `Count()` calls, four `getenv`s and an
  ungated `snprintf`. `record` −47%, `other` −49% at matched draw counts, and the crowd
  frame time follows.
* **`tools/frame_perf_bins.py`**, and the noise floor it exposed.
* **`[vkprof]`'s `pm4` column and the packet/register-dword census**, which answered
  §2 rather than merely instrumenting it: the walk is a register-write loop.
* **The A/B result: −11.0% of a crowd frame** (arm A 55.30/53.00/53.17 ms against arm B
  48.35/48.51/46.89, three runs an arm alternated, no overlap), 18.7 -> 20.9 fps in the
  top populated bin at P8.
* **The bind-repeat measurement**, recorded and deliberately not acted on.
* Gotchas 228 (a nested profiler scope counts twice and every column still adds up),
  229 (measure the noise floor with a null arm; binning is not enough), 230 (an
  instrument that is off must be free in its WORK, not just its OUTPUT).

## A NOTE ON THIS FILE, written after it was first drafted

The ordered list above was rewritten twice in one session — once when the profiler fix
re-ranked the plan, and once when the operator session re-ranked it again. The first
draft of this file kept its old numbered list under a new preamble saying "the order
changed", which is precisely the stale hand-off gotcha 13 describes and which part 19's
kickoff had already cost a session. **If you find a preamble here disagreeing with a
numbered item, the numbered item is the stale one — check the git log and this file's
own history before working it.**

## Gates, all run on the part-20 binary

`--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (24,527,474 packet lengths
agreeing, every indirect buffer tiling exactly). The picture was not re-checked against
capture E this part — nothing in it touches what is drawn, only how long drawing takes,
and the frame-stats fingerprints are unchanged — but that is an argument, not a
measurement, and `tools/frame_signature.py` against E2 is two minutes if part 21 wants
it before touching the renderer again.

## The method notes worth carrying

* **The kickoff said "re-measure before optimising" and it was right for a reason it did
  not guess.** It warned the table was stale. The table was not stale, it was wrong —
  and the only thing that distinguishes those two is re-running the measurement rather
  than reasoning about how old it is.
* **The null arm is the whole measurement.** Running the A/B with nothing changed is
  what turned "arm B is 10.7% faster" from a result into a coin flip, and then a
  three-runs-a-side alternation is what turned it back into a result. It costs one extra
  run and there is no substitute for it.
* **The plan's own discipline paid off twice.** §1a hypothesis A said "add the counters
  and run WITHOUT acting on them, because a low repeat rate kills the idea for free" —
  and the rate was a third of what the prose expected, so the dozen lines were never
  written. Writing the decision rule down before the measurement is what makes a
  disappointing number cheap instead of an argument.
* **Gotcha 223 was a day old and the code still had the defect in three forms.** A rule
  in the ledger is not a rule in the code; the check is a grep, and it belongs in the
  same commit as the rule.
