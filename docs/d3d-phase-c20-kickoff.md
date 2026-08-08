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
2. **The GPU was at P8/210 MHz of 2100 for the whole of part 20** — this machine has no
   passwordless sudo, and `sudo nvidia-smi -pm 1 && sudo nvidia-smi -lgc 2100,2100` needs
   the operator (suggest they type `! sudo nvidia-smi ...`). It inflates the `gpu` column
   ~2.9x and nothing else, so every CPU number in part 20 stands; every FRAME TIME in it
   is a P8 frame time. Say which you are quoting (gotcha 219).
3. **A profiler is instrumentation, so break it on purpose before trusting it**
   (gotcha 228). `ProfScope` counted nested phases twice for the whole of this project's
   life, and every column still summed to the total. That is the class: an error that
   MOVES time between columns leaves nothing looking wrong.

## Where part 21 starts, in order

1. **`docs/perf-cpu-plan.md` §2 — the PM4 walk, 11.8 ms (21.6%) of a crowd frame — is
   now the biggest single untouched item, and part 20 left the instrument for it half
   built.** `[vkprof]` prints the walk's own cost as a `pm4` column, and a second line
   giving packets/frame, ns/packet, register-write dwords/frame and dwords/packet. That
   turns §2's leading suspect — `WriteRegister`, called once per dword of every
   SET_CONSTANT and LOAD_ALU_CONSTANT — into a testable claim for the first time.
   Two specific leads, both cheap:
   * **`ExecutePacket` does 2-4 `lock`ed atomic increments per packet** (`g_packets`,
     `g_types[]`, `g_opcodes[]`, `g_draws`). `gpu/pm4.h` states in its own header that
     "everything here runs on one thread (the vblank pump)" and that the counters are
     atomic only so a future tracer can read them — which makes a relaxed
     load-add-store equivalent and a plain `add` instead of a `lock xadd`. At ~7 ns each
     that is worth ~25 ns a packet; the packets/frame figure says whether that matters.
   * `Source::operator()` does a `%` per dword AT RING LEVEL only (indirect buffers pass
     `wrapDwords = 0`), so the division is NOT on the hot path. Do not spend time there.
   The plan says split before theorising. **Do not do that with `ProfScope`** — two
   `clock_gettime` calls at ~25 ns against a packet that costs maybe 150 ns would report
   mostly itself (gotcha 7). Use `perf record -F 999 -g` on the crowd recipe instead;
   `perf_event_paranoid` is 2 on this machine, which is enough for user-space symbols.
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
* **`[vkprof]`'s `pm4` column and the packet/register-dword census**, which is the
  instrument §2 needs.
* **The bind-repeat measurement**, recorded and deliberately not acted on.
* Gotchas 228 (a nested profiler scope counts twice and every column still adds up),
  229 (measure the noise floor with a null arm; binning is not enough), 230 (an
  instrument that is off must be free in its WORK, not just its OUTPUT).

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
