# Night Run 1 — the unattended performance campaign of 2026-08-19 → 20

**This is NOT part 59.** Part 59's plan (R6 trace analysis + operator-directed picture
work) is unchanged and runs tomorrow; the operator's first obligation tomorrow is the R6
capture, and the standing reminder about it survives this document. Night Run 1 is a
separate, operator-requested overnight campaign: *"prepare a plan to improve performance
that I can run while sleeping."* That instruction is this night's explicit authorization
for headless game runs — the standing "operator runs all verification" rule is not
otherwise relaxed, and every number this campaign produces is **advisory until confirmed
at the operator's soak** (gotcha 355; the operator's frame is ~2x the headless one).

Everything here executes `docs/perf-state-parked.md`'s own resume list. No runtime code
changes, no default changes, no item A — item A needs its order gate built first and
that is supervised work, not a script.

## What runs tonight

### Phase 0 (offline, before the game runs): the ALU constant usage census — owed item 4

The parked doc's item C (copy only the constants the shader reads) is gated on a census
that does not exist: the distribution of max-constant-used across the shader modules,
plus which shaders use `a0`-relative (dynamic) indexing and therefore force the full
copy. The sidecars carry no ALU constant usage, so it comes out of the translated
SPIR-V. Tool: `tools/alu_const_census.py` (written tonight); result lands in this file's
§Results and decides whether item C is worth building. Pre-registered kill: **if the
median shader's constant span is ≥ 200 of 256 float4s, item C is dead** (the parked
doc's own framing).

### Phase 1 (the game, unattended): four chained A/B experiments — `tools/nightrun1.sh`

The soak: the DebugJump route (event-anchored, lands by the military camp at ~7,400
draws), then standing still for ~10 minutes per run, headless, `CZ_FPS_LOG=10` as the
only frame instrument (the one with no bill). God mode / no death / zombies-ignore held
by the pump in EVERY arm via `CZ_DEBUG_FLAGS`, exactly as the part-55 operator harness
did, so nothing can move the camera. One snapshotted binary for all arms. 3 runs per
arm, alternated A,B,A,B,A,B within each experiment. GPU clock sampled through every run
(`gpu_clock_sample.py`) because the monitor will be ASLEEP — the exact condition that
manufactured the P8/210 MHz artifact — so every quoted number carries its clock and
`display_active` state.

| # | experiment | control | test | why |
|---|---|---|---|---|
| N0 | null floor | clip cache, defaults | identical config | the floor every other row is read against; a campaign without one is uninterpretable |
| N1 | **the clip-cache cost** | `CZ_SHADER_SPV=shader_spv_a2m` (stock) | `shader_spv_clip_a2m` (the play cache) | the parked doc's FIRST experiment on resume: attribute the ~1.5 ms the part-58 spot check found over part 55's close |
| N2 | guard workers | defaults (budget grants 3) | `CZ_VK_GUARD_WORKERS=4` | owed item 2: the thread budget silently changed part 53's measured configuration |
| N3 | constant memo, 2nd pair | defaults (memo on) | `CZ_VK_NO_CONST_MEMO=1` | owed item 1: −2 to −3% was near one pair's resolution limit |

**Cache admissibility, fixed tonight before launch**: `shader_spv_a2m` was missing
`vs_c17bbebf65383249` (part 57 added it to main + clip_a2m only); it was built with the
a2m define and installed, so N1's arms now differ ONLY in the clip epilogue + its
translation era. Per-run gate: `no translated shader` must be 0 in every log or the run
is inadmissible.

**Pre-registered claims and kills:**
* N1: the clip cache costs measurably more than stock at matched draw bins. If the
  delta clears the N0 floor, the ~1.5 ms attribution gains its first direct evidence and
  "publish/dot planes only when enabled" becomes a sized item; if it does NOT clear the
  floor, the attribution moves off the clip cache and onto part 56's per-draw
  dynamic-state calls (the other candidate named in the parked doc).
* N2: pre-registered as "either direction is a finding" — budget-3 was never measured
  against part 53's 4. Kill: a delta below the N0 floor closes owed item 2 as "the
  budget change was free."
* N3: the memo's headless delta, read only as corroboration; the operator-soak figure
  stays the soft one per the parked doc. No kill — this is replication, not a decision.

**How to read it tomorrow** (the script pre-computes all of this into `SUMMARY.txt`):
`tools/part54_fps_bins.py` on pooled per-arm logs, matched draw bands only, N0's floor
quoted FIRST, each run's `[threads]`/memo/cache lines and GPU clock line beside it.
Windows with wide draw spread are dropped by the tool. The two arms of a pair never
land at the same draw count — read the banded table, not the headlines.

**Results land in `~/DR2CZ-troubleshooting/nightrun1-2026-08-19/`** (disk, not tmpfs):
per-run logs, per-run GPU clock summaries, `checks.txt` (engagement evidence per run),
`SUMMARY.txt`. This document gets a §Results section written by the morning session —
whichever session reads the night's output writes it here, then routes anything
actionable back into `perf-state-parked.md`.

## What tonight deliberately does NOT touch

* **Item A** (parallel command recording): its order gate does not exist yet (owed
  item 3). Building instrument-grade code unattended is how gotcha-30 violations
  happen; it stays a supervised item.
* **Item C's runtime half**: census first, by its own doc.
* Any default, any commit to runtime code, any operator-facing behavior.

## Results

### Phase 0 — the ALU constant census (run 2026-08-19 23:00, before the game arms)

`tools/alu_const_census.py` over XenosRecomp HLSL regenerated from all 439 ucode dumps
(439/439 translated, zero failures). Detail JSON beside the night's logs.

| | modules | dynamic (`a0`) → full copy | static median **n_used** (gather) | static median **span** (range) | p90 n_used |
|---|---|---|---|---|---|
| VS | 104 | **22** (bone palette: `vc(8/9/10+a0)` ×~20, one `vc(209+a0)`) | **9** of 256 | 202 | 37 |
| PS | 335 | **0** | **27** of 256 | 255 | 41 |

**The item's two designs get opposite verdicts:**
* **RANGE copy (lo..hi memcpy) is DEAD** — the parked doc's own kill ("median ≥ 200 of
  256") fires: 318 of 335 static PS read a register ≥ 250 (the tonemap's c255 cluster)
  next to their low registers, so the span is ~the whole window.
* **GATHER copy (per-shader register list, built into the sidecar at cache-build time)
  is ALIVE and large**: the median copy would move ~430 bytes instead of 4,096 per
  stage, a ~10x byte reduction on every draw the memo does not already skip, and the
  full-copy fallback is needed on only 22 VS modules and no PS.

Caveats that go with the number: modules are weighted equally here while the frame
weights them by draw count (the run-time counter is item C's to build), and uncopied
registers hold stale bytes — safe exactly because the census says the shader never
reads them, which makes the census itself load-bearing and means item C's runtime must
carry a verify arm comparing gather vs full copy (same shape as the memo's).

### Phase 1 — the night's four A/Bs (completed 03:42, all 24 runs engaged)

Engagement was clean across the board: WAITJUMP fired in every run, 71 fps windows per
run, `missing shaders: 0` everywhere, and the arm counters prove each arm engaged (the
`nomemo` runs read `0.0% served`, the `guard4` runs print a different `[threads]` line).

**Two facts about the instrument before the numbers:**
* **The DebugJump landing is BIMODAL** — 11 of 24 runs landed in a ~2,500-draw state
  and 13 in a ~5,000-5,800 one (spawn variance; gotcha 159 in route form). The draw-band
  matching absorbs it, but it means most HEAVY-band comparisons below rest on ONE run
  per side; only the light bands carry 2-3 runs a side. A future night should double
  reps or gate on the landing class.
* **This load is near GPU-limited**: P0, 1965 MHz flat, 75-85% utilisation, ~98 W in
  every run (not the P8 sleep artifact — quoted per run in `checks.txt`). A CPU-side
  saving is partly absorbed by the GPU here, so these deltas READ LOW relative to the
  operator's CPU-bound frame. Direction survives; magnitude does not transfer.

| experiment | light band (~2,500 draws) | heavy band (~5,000) | verdict |
|---|---|---|---|
| **N0 null** | −0.1 to −0.4% | −0.4% | **the floor is ≤0.4%** on banded window medians — far tighter than the historical 10-13% frame-time floor, which is what soak + median windows + draw-banding buys |
| **N1 clip cache** | +0.4% (≈ floor) | **+3.0% (+0.20 ms)** | the clip cache costs REAL time and it scales with load (per-vertex plane dots) — but the heavy band is one run per side, and +0.20 ms at 5,000 draws does not obviously fill the operator's ~1.5 ms gap at 7,000+. Reads as "part of the attribution, probably not all of it"; part 56's per-draw dynamic-state calls stay live as the co-suspect. The decisive test remains the operator chained A/B |
| **N2 guard workers** | ≈ floor (weak bands) | **guard4 +4.5% SLOWER** (one pair) | no evidence for restoring 4; weak evidence the budget's 3 is actually better on this 8-core box. Owed item 2 closes as "keep budget-3; do not spend an operator session on this" |
| **N3 memo replication** | **nomemo +1.8 to +2.2% slower** (multi-run, 4-5x the floor) | no overlapping bands (bimodality) | the memo's value REPLICATES in the direction and rough size of part 55's −2 to −3%. Owed item 1 satisfied at this load; the operator-soak figure stays the authoritative magnitude |

One more number worth keeping: the memo's own counter says the full constant copy at
this uncapped load is multi-GB/s (heavy runs: ~1.4-1.5 TB NOT copied per 12-min run at
~38% served) — the traffic item C's gather design would cut ~10x further per Phase 0.

**Routing back into `perf-state-parked.md`:** item C is re-priced by the census
(gather-only), owed items 1 and 2 are answered as above, and N1's partial attribution
plus the dynamic-state co-suspect belong to the stock-vs-clip operator A/B that was
already the first resume experiment.
