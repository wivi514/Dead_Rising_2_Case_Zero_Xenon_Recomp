# Part 48 kickoff — PERFORMANCE, still, to the operator's order; the plan is `docs/perf-plan-part48.md`

Written at the close of part 47 (2026-08-16). **This is the LIVE hand-off**,
superseding `part47-kickoff.md`.

## START HERE

**The operator's order is unchanged and explicit: *"For now performance is the
most important."*** They have picture defects to report and said so — items 00m
(decals) and 00n (a sign and some items wrong at distance) are filed from their
own words, both confirmed by them as PRE-EXISTING and not caused by the
performance work — and they explicitly deferred them. Do not start on those.

**The plan is `docs/perf-plan-part48.md`.** It is built on their OWN part-47
frame, not on the headless route. Read it, then §6cd of `docs/phase5-notes.md`.

**Action zero, and it is a LAUNCH, not a measurement**:
`tools/part47_operator_session.sh`. One change has landed since their last
session and has never been measured on their machine — the guard fold's four
lanes, which headlessly takes `record` from 1,636 to 1,198 ns/draw and on their
81.65 MB/frame of guard hashing predicts **~6.8 ms**, i.e. 42.8 → ~36 ms. Their
frame is the one the 33 ms target is set against.

Ask them the same two questions as last time; the second still matters most:

1. **Is it faster where you play?**
2. **Does any texture or surface ever look STALE?** Part 47 changed both content
   guards — the texture guard's cadence and the stream guard's hash — and
   staleness is the only symptom either could produce. `CZ_VK_TEX_GUARD_EVERY_FETCH=1`
   and `CZ_VK_GUARD_FOLD_SERIAL=1` are the arms that undo them individually.

## What part 47 achieved

**The headless route: the crowd frame lands on the two-vblank pacing floor.**
Three runs an arm, one binary, both negative controls reading exactly zero:

| draw bin | part 47 | pre-47 |
|---|---|---|
| 3,000-5,000 | **32 ms, 98% pinned** | 32-40 ms, 7-66% pinned |
| 5,000-8,000 | **32 ms, 73-85% pinned** | 42-46 ms, 5-13% pinned |
| 8,000+ | **36-37 ms** | never reached |

**The operator's own machine, matched on draw count**: 64.1 → **42.8 ms**,
15.6 → **23.4 fps**, `textures` 25.19 → **4.45 ms**. Their words: *"performance
is way better, still far from perfect"*, *"pretty much 10 fps difference"*, and
*"games looks pretty much the same as last time"* — no staleness reported.

Built, each in its own commit with its own same-binary arm:

| item | change | arm |
|---|---|---|
| 1.1 | texture content guard once per frame per cache entry | `CZ_VK_TEX_GUARD_EVERY_FETCH=1` |
| — | the guard fold gets four lanes, 9.0 → 35.7 GB/s | `CZ_VK_GUARD_FOLD_SERIAL=1` |
| 1.2/1.3 | 21 hot `Count`→`COUNT`; per-fetch scan behind its readers' gate | mechanical |
| 2.1/2.2 | PM4 writes register RUNS in bulk | `CZ_PM4_NO_BULK_REGS=1` |
| 3.1 | state cache covers vertex/index binds | `CZ_VK_NO_BUFFER_BIND_CACHE=1` |
| — | per-fetch sampler lookup as a flat 512-entry table | provably equivalent |
| — | `record` split into state/vertex/index/residual | an instrument |

## What part 47 settled (do not re-derive)

* **The texture revalidation guard was nearly the whole texture phase**, and the
  fix is a CADENCE change: **93.4% of checks skipped, 15.1x less hashing**. The
  redundancy factor was the entire size of the item and had never been measured;
  an estimate off run totals said 2x and was wrong by the length of the route
  (gotcha 323).
* **`record`'s vertex section was 70% of it, and the work in it was the STREAM
  guard, not the vertex walk** — charged there because `g_prof.streams` wraps only
  the copy. Gotcha 238 contained this exact example nine parts before anyone acted
  on it (gotcha 326).
* **The guard fold was latency-bound, not bandwidth-bound.** Four accumulators,
  4.0x, same bytes, and a single-bit sweep at 676 positions confirming 0 misses on
  both folds — the sensitivity check is what makes a hash speedup safe (gotcha 324).
* **THEIR WORKLOAD DIFFERS FROM OURS IN KIND**: 144 ns/packet against 110-113, and
  **7.8 register dwords per packet against 9.4**. Item 2.1's bulk path buys them
  less, and per-PACKET cost dominates their walk. Quote walk changes as ns per
  packet, not as `outside` in milliseconds.
* **Both PM4 boundary oracles are blind to anything inside `ExecutePacket`** — they
  verify packet-length and indirect-walk arithmetic and would pass identically if a
  rewrite there were wrong. The incumbent implementation is the oracle
  (`CZ_PM4_VERIFY_BULK_REGS`, 0 mismatches over 152 M dwords, with a poison arm
  first). Gotcha 322.
* **Three ways a perf A/B on this title reads wrong**, all of which bit in one
  afternoon: phase SHARES move when other phases do (quote milliseconds, 320);
  pooling profile windows across a route measures the route (matched draw band,
  321); and `msec` is the LAST of the eighteen `.stats` columns.
  `tools/part47_perf_read.py` does all three.

## What is OWED, and what is unresolved

* **The operator's confirmation of the guard fold** — action zero.
* **The vertex/index bind cache has never been A/B'd on its own**, and `record`
  came out ~1 ms higher on the part-47 arm in BOTH the headless and the operator's
  data. Neither is established (a matched draw COUNT is not a matched draw
  COMPOSITION) but the sign is consistent. `CZ_VK_NO_BUFFER_BIND_CACHE=1`, three
  runs an arm. **If it is a loss, delete it.** Plan §2d.
* **Item 1.1's registered claim is half-answered.** `changed` per frame did not
  fall (0.0739 vs 0.0640) but the every-fetch arm saw more DISTINCT addresses ever
  change (157 vs 141) while the part-47 runs covered more ground. Confounded by
  route; the operator's session is where it can be settled.

## Standing state

* **Runtime defaults changed in part 47**: texture guard once per frame per entry;
  the guard fold's four lanes; PM4 bulk register runs; the state cache covering
  vertex and index binds. Every one has an arm, listed above.
* **The A2M work is still NOT a default** — `CZ_SHADER_SPV=assets/shader_spv_a2m
  CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1`, which is what the session driver runs.
* **Tooling**: `part47_perf_ab.sh` (arm as a parameter), `part47_perf_read.py`,
  `part47_gates.sh` (every standing gate in one command),
  `part47_operator_session.sh` (two chained arms, instruments wired in).
* **Artifacts**: `~/DR2CZ-troubleshooting/part47/` (its README says what `base`
  means in each subdirectory — it is a DIFFERENT binary in `perf/` and `perf2/`)
  and `part47-operator/` (their two-arm session, with two F9 captures at the
  gas station).
* **Gates at close**: `tools/part47_gates.sh` — all clean, E3 at **+0.877,
  LAYOUT AGREES**.
* **The picture items, parked by the operator's own instruction**: 00m decals
  (new, never characterised), 00n a sign and some items at distance (the tail of
  00i), the tree fix proper (`CZ_VK_A2M_MODE=1` ships and is good enough), the mip
  overshoot, the 0u residues, part 41's clamp modes, and the part-43 freeze which
  has not recurred in any operator session since.
