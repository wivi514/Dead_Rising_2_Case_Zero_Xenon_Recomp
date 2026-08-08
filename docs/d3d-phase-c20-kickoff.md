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

1. **`docs/perf-cpu-plan.md` §2 — the PM4 walk — is the biggest untouched item, and
   part 20 MEASURED what it is made of, so this starts from a number rather than a
   suspicion.** At 6,876 draws a frame and 48.0 ms (P8), the walk's own cost is 12.5 ms
   (26.0%) and it executes **90,316 packets carrying 815,020 register-write dwords** —
   9.0 dwords per packet, 138 ns per packet, **15.3 ns per register dword**. The dwords
   account for essentially the whole 12.5 ms: `WriteRegister` is the walk.
   `[vkprof]` prints all of it; §2 of the plan is rewritten around it. In order of the
   size the number supports:
   * **`LOAD_ALU_CONSTANT` reads from GUEST memory**, so a crowd frame streams ~3.3 MB
     through the cache one `GuestLoad32` at a time — a `memcpy`, a `bswap` and a
     non-inlined `WriteRegister` call per dword, where a contiguous run wants one bulk
     byte-swapping pass. This is the biggest single lead and it is where to start.
   * **`ExecutePacket` does 2-4 `lock`ed atomic increments per packet** (`g_packets`,
     `g_types[]`, `g_opcodes[]`, `g_draws`). `gpu/pm4.h` states in its own header that
     "everything here runs on one thread (the vblank pump)" and that the counters are
     atomic only so a future tracer can read them safely — which makes a relaxed
     load-add-store equivalent and a plain `add` instead of a `lock xadd`. ~7 ns each
     against 90,316 packets a frame is **~2 ms**, and it is cheap to arm and revert.
   * `g_regs` is larger than L1; whether its footprint costs anything is a hardware
     counter question, not an argument.
   **Do not split the walk with `ProfScope`** — two `clock_gettime` calls at ~25 ns
   against a 138 ns packet would report mostly itself (gotcha 7). If finer attribution
   is needed, use `perf record -F 999 -g` on the crowd recipe; `perf_event_paranoid` is
   2 on this machine, which is enough for user-space symbols.
   **`pm4.cpp` is under two capture oracles** — `tools/pm4_packet_lengths.py` and
   `tools/pm4_indirect_walks.py`, both exit-1-on-defect. Run them after every change
   there; part 20 did and both are clean.
2. **§1d — `other`, `DoDraw`'s untimed work — is 5.6 ms and is now the second biggest
   term in the draw path.** Part 20 halved it by removing the instrumentation, and what
   is left is real work: two `std::map<uint64_t>` shader lookups, a
   `std::map<PipelineKey>` lookup with a 40-byte `memcmp` comparator, the fetch-constant
   walk and the register decode. Memoising the last (vsHash, psHash) and the last
   `PipelineKey` is the obvious first move — the state cache already reports the
   pipeline unchanged on 74% of draws, so the lookup that produced it was wasted 74% of
   the time. Measure it as a counter first; the frame-time floor is 10-13%.
3. **§1a hypothesis D — the per-draw constant block — is the one item that pays twice.**
   8 KB of the ~27 KB a draw the arena has to hold is the ALU constant copy, and the
   arena is what blacked frames out until part 19 (`docs/phase5-notes.md` §6ap). The
   cheap exact instrument is NOT a hash of the block: it is a generation counter bumped
   in `WriteRegister` when an ALU constant register is written, compared against the
   previous draw's. That costs nothing, is exact, and is also the mechanism for the fix.
4. **§1a hypothesis A is measured and mostly refuted — do not re-derive it.** In the
   crowd era 34% of vertex binds and 22% of index binds repeat the previous offset,
   worth ~1.4 ms. Real, about a third of what the hypothesis expected, and below the
   noise floor. The counters are in the `CZ_VK_STATS` block as "binds NOT cached".
5. **The remaining picture defects**, unchanged from part 19's list and all worth
   re-testing now that neither the whole-frame black nor a collapsed luminance ladder is
   contaminating the evidence: the shadow cascade (open-items 3), mipmaps (4), NPC part
   meshes (3d), the magenta sky / colour-grading LUT (6), the prologue cinematic.
6. **The safehouse door renders as a fully saturated white slab** — carried over from
   part 19 unchanged. It has no counter behind it and no measurement, so it is an
   observation, not a defect. `CZ_VK_SKIP_TEX` to name the address is the cheap first
   move.

## What part 20 delivered

* **The profiler exclusivity fix** (`docs/phase5-notes.md` §6aq, gotcha 228). Each scope
  now subtracts what its children consumed, and the whole-draw total is a SUM of the
  columns rather than a separately measured quantity — two statements that can disagree
  beat one that cannot.
* **The corrected crowd table**, re-measured at 6,737-6,806 draws (P8): `record` 6.7 ms,
  `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3, draw path 19.9 ms
  (36.5%); PM4 walk 11.8 ms (21.6%); GPU fence 19.2 ms at P8.
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
