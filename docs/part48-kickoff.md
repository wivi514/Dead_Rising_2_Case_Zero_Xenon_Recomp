# Part 48 kickoff — the performance work needs the OPERATOR'S verdict next, and the tree fix is the picture item behind it

Written at the close of part 47 (2026-08-16). **This is the LIVE hand-off**,
superseding `part47-kickoff.md`.

## START HERE

**Part 47 executed tiers 1 and 2 of `docs/perf-plan-part47.md` and the plan's own
first run repriced its top item upward.** Read the plan's STATUS header, then
`docs/phase5-notes.md` §6cd.

**The first action of part 48 is not a measurement, it is a LAUNCH**:
`tools/part47_operator_session.sh`. It runs two chained arms, one binary, three
environment variables apart, with `CZ_VK_PROFILE` and `CZ_VK_FRAME_STATS` wired
in. Everything part 47 measured is on the headless route, **which understates the
operator's draw path by about a factor of two**, so a headless win here is not
conservative — the reverse of the usual order (gotcha, and
`docs/part47-kickoff.md`'s standing rule).

Two things to ask them, and only two:

1. **Is it faster where you play?** The headless answer is that the crowd frame
   moves onto the two-vblank floor.
2. **Does any texture ever look STALE — a surface wearing something that belongs
   somewhere else, or one that does not update when it should?** This is the one
   thing part 47's item 1.1 could plausibly have broken, and it is the symptom
   part 38 fixed: the guard now runs once per frame per cache entry instead of
   once per fetch, so a texture the guest rewrites MID-frame is served stale for
   the rest of that frame. `CZ_VK_TEX_GUARD_EVERY_FETCH=1` is the arm that undoes
   exactly that, and it is arm B of the session driver.

## What part 47 settled (do not re-derive)

* **The texture revalidation guard was nearly the whole texture phase.**
  `CZ_VK_NO_TEX_REVALIDATE=1`: `textures` **15.9 ms → 2.3 ms** on the outdoor
  route, against the plan's 8-11 ms estimate. In the 5,000-8,000 draw bin the
  frame goes 47-48 ms at 23-24% pinned → **32-33 ms at 67-94% pinned**. That arm
  is the upper bound, not a configuration — it is the defect part 38 fixed.
* **The fix is a CADENCE change and it recovers most of that**: once per frame per
  cache entry, **93.4% of checks skipped, 15.1x less hashing**. The redundancy
  factor was the whole size of the item and had never been measured; a first
  estimate off run totals said 2x and was wrong by the length of the route
  (gotcha 323).
* **The PM4 walk writes register RUNS in bulk**, and it is verified against the
  per-dword code it replaced rather than against a gate that could not see it:
  **0 mismatches over 152,020,384 dwords**, with `CZ_PM4_VERIFY_POISON=1` first to
  show the check can fail (gotcha 322). **100.0% of dwords take the bulk path.**
* **The state cache now covers the vertex and index binds.** Part 18 added the
  counters and deliberately did not act on them; the rate came back 51.0% / 39.4%
  on the operator's own session over 16.17 M draws.
* **Item 1.4 is void as written**: the "two cache lookups per fetch" is arm-gated
  (`CZ_VK_TEX_CACHE_FIRST`), so the default path has one.
* **Three ways a perf A/B on this title reads wrong**, all of which bit in one
  afternoon: phase SHARES move when other phases do (quote milliseconds, gotcha
  320); pooling profile windows across a route measures the route and calls it
  noise (matched draw band, gotcha 321); and `msec` is the LAST of the eighteen
  `.stats` columns, not the first. `tools/part47_perf_read.py` does all three
  correctly and refuses a verdict below two runs an arm.

## The plan

0. **THE OPERATOR SESSION, first.** `tools/part47_operator_session.sh`. Nothing
   below is worth doing before their answer, because their frame is the one the
   target is set against.
1. **The remaining lever inside item 1.1, if their frame is still over budget.**
   `CZ_VK_TEX_GUARD_BYTES=N` exists and **its default is deliberately unchanged at
   16384**. Four fifths of the guard's bytes are spent on textures whose source is
   32 KB or larger, every one read at the 16 KB cap, so a lower bound is a large
   further cut — but it is the one option that trades DETECTION for cost. The
   histogram and the per-address `changed` table (which now carries each texture's
   source size) are what price both halves; choose from those, and confirm with an
   operator, never from the argument that a recycled address looks different in
   its first bytes.
2. **Tier 3.2, multithreaded recording**, which the plan puts last on purpose: it
   is invasive, it changes the ordering the resolve/snapshot logic relies on, and
   it multiplies the cost of every per-draw bug. Its prerequisite is that the
   per-draw path be free of shared mutable state, and it is *less* urgent now.
3. **THE TREE FIX PROPER** — unchanged from `part47-kickoff.md` item 1, and still
   the top picture item. `CZ_VK_A2M_MODE=1` ships today and is good enough that
   this is not urgent. The renderer change is to sample-expand 2x surfaces the way
   `msaa == 2` ones already are, then flip `XeAlphaTestThreshold`'s dither to the
   1x2 pattern. The property is already named: canopy p05/p95 against E3's 0.326,
   and the hard-edge share, which must come DOWN toward hardware's 0.21%.
4. **Parked, unchanged**: the mip overshoot (re-run `CZ_VK_NO_MIPS=1` on the FIXED
   cache before quoting part 44's result again); the 0u residues; part 41's clamp
   modes / cyan fringes; the AO-only-up-close observation; and the part-43
   sledgehammer FREEZE, which has not recurred in any operator session since.

## Standing state

* **Runtime defaults changed in part 47**: the texture content guard runs once per
  frame per cache entry (`CZ_VK_TEX_GUARD_EVERY_FETCH=1` is the arm); the PM4 walk
  writes register runs in bulk (`CZ_PM4_NO_BULK_REGS=1`); the state cache covers
  vertex and index binds (`CZ_VK_NO_BUFFER_BIND_CACHE=1`). Every other part-47
  change is mechanical and has no arm because it has no behaviour.
* **The A2M work is still NOT a default** — it needs
  `CZ_SHADER_SPV=assets/shader_spv_a2m CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1`,
  which is what `tools/part47_operator_session.sh` runs.
* **Tooling added in part 47**: `part47_perf_ab.sh` (the arm is a parameter),
  `part47_perf_read.py`, `part47_gates.sh` (every standing gate in one command),
  `part47_operator_session.sh`.
* **Gates at close**: run `tools/part47_gates.sh` — link gate, unlowered switches,
  shader dimension census, both PM4 boundary oracles, and the E3 picture gate,
  which reads **+0.878, LAYOUT AGREES**.
