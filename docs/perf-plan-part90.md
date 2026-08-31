# Perf plan, part 90 — the revived GPU surface: deferred scoped clears, and the copy census

Written after step 0 ran (the fresh decomposition `part90-kickoff.md` §1 item 0 demands);
the numbers below are from this part's own two runs, not any earlier table.

## §0 Step 0's answers (2026-08-31, part80_crowdroute, peaks 9,167/9,413)

**The regime: §0b's flip has effectively happened.** Census run (no profiler, GPU passes +
frame trace, both bills ~nil — GPU median at the crowd matches part 89's uninstrumented
3v3 to 0.06 ms):

| band (draws) | wall | GPU | fence | regime |
|---|---|---|---|---|
| 2,000-5,000 | 6.8-7.4 | 6.7-7.3 | 1.6-3.2 | GPU-bound |
| 5,000-7,000 | 8.1-8.4 | 7.9-8.2 | 0.9-1.1 | GPU-bound |
| 7,000-8,000 | 9.55 | 8.97 | 0.00 | CPU-bound by 0.58 |
| 8,000-9,000 | 10.88 | 10.56 | 0.00 | CPU-bound by 0.32 |
| 9,000-10,000 | 11.10 | 10.60 | 0.00 | **CPU-bound by 0.50** |

**The CPU decomposition is fresh and confirms part 89's**: `record` 431 ns/draw
(state 26 + vertex 127 + index 143 + guard 10 + residual 124) — every pre-part-89 table's
625 is stale, as the kickoff said. No CPU lead ≥0.5 ms is visible beyond the serial
residue (PM4 walk ~20% of the pump, the resolve half, constants 17.4% instrumented —
all change-detector class or closed at mechanism level in part 88). Guard pool 21% busy.

**The GPU census with parallel record ON attributes cleanly** (residual 0.5%, so the
instrument survives the chunked submission): whole-run 8.12 ms/frame — big passes 4.89
(the game), 1-draw 0.97 + small 0.67 (the post chain, refuted §6dx), **resolve copies
0.741 (50.4/frame, 14.7 us each, 50.6 Mpixel), resolve clears 0.615 (83.8/frame,
7.3 us each, 590 Mpixel written for 33.9 rendered — 94.3% scopeable)**, barriers 0.099,
present 0.061, snapshot views 0.029.

And the §4b line: the resolve/begin cycle costs the pump **0.598 ms/frame of CPU**, of
which part is exactly the clear recording this plan removes.

## §1 Item 1 — DEFERRED SCOPED CLEARS (build)

Today a resolve's clear bits cost: pump records barrier(s) + `vkCmdClear{Color,DepthStencil}Image`
over the WHOLE EDRAM stand-in (7.05 Mpix at 3440-scale), plus the layout round-trip
(TRANSFER_DST and back at next pass begin). 83.8 of these a frame.

The mechanism, using what part 89 built: **every pass records at least one
dynamic-rendering instance through `ParRec_RecordInstance`** (first chunk on a worker, or
the pump tail — and in the serial arm, `BeginRendering` opens the instance directly). A
clear latched at resolve time as a pending (aspect, value, rect) is emitted as
`vkCmdClearAttachments` at the head of the NEXT pass's first instance — an instance that
already exists, so there is **no dedicated mini-scope and no per-clear pump bill**, which
is what made `CZ_VK_SCOPED_CLEAR` a wash (78/frame × 6.6 us = 0.51 ms pump, part 80 §1
item 4 — do not re-buy).

Correctness never depends on the ordering assumption: **anything that reads EDRAM while
pendings are outstanding flushes them first** through a self-contained mini instance,
counted by reason. The three reader sites: the resolve copy (`DoResolve`), the swapchain
blit's EDRAM fallback, the readback's EDRAM fallback (snapshot views and cube faces read
snapshots, not EDRAM). Pendings may carry across a frame boundary — nothing reads EDRAM
between frames except those sites — and are dropped on EDRAM recreation.

Semantics: scoped-to-the-resolve-extent is CLOSER to Xenos (a copy block clears the
current surface's tiles, not all of EDRAM); the risk direction is a pass that depended on
our over-clear, and the arbiters are the picture gates below. Part 32 measured
scoped == full to four decimals on the cascade-zero statistic.

**Pre-registered predictions:**
* GPU clear class 0.615 → ≤0.15 ms at the same census; total GPU at the crowd −0.4 to
  −0.6 ms. Wall at the crowd moves little (CPU-bound by ~0.5) — the win is regime
  headroom plus every GPU-bound band; ship on the mechanism numbers (part 88's way).
* Some pump CPU back from the 0.598 ms cycle cost (clear barriers + clear records gone).
* **Kill**: if the flushed-for-a-reader share exceeds 30% of clears, the mechanism is
  defeated at its own game — park behind the arm.
* **Positive control**: `CZ_VK_CLEAR_POISON=1` must still paint the never-drawn pixels
  magenta with deferral ON (proves the deferred clears execute); the engagement counters
  must show ~84 deferred/frame and ~0 full.
* **Gates**: sync validation 0 hazards; era medians inside the null (4-run picture gate);
  `CZ_VK_NO_DEFERRED_CLEAR=1` is the same-binary control arm and ships in the same
  commit as the default flip.

## §2 Item 2 — THE RESOLVE-COPY CENSUS (diagnostic, then decide)

0.741 ms/frame of `vkCmdCopyImage` EDRAM → snapshot. Part 80 §1 item 5's unasked
question: does every resolve need its snapshot copied on the frame it is produced, or is
a copy DEAD (its destination overwritten by a later copy before any draw samples it)?
A diagnostic census (off by default): per snapshot key, mark the copy; mark any draw that
binds the snapshot's descriptor slot; at the next copy of the same key, count the prior
copy as dead if no sample intervened. **It either names an item or kills one** — a dead
share under ~25% (≤0.19 ms, half the route floor) kills it honestly; above that, the
mechanism question (prediction is the only lever, and a wrong prediction is a silent
stale texture) gets asked with a number in hand.

## §3 Order

1. Item 1 build + gates + census re-run (same-day A/B, one run a side for mechanism,
   3v3 only if wall claims are made).
2. Item 2 census + verdict.
3. Docs: phase5-notes §6ek, kickoff for part 91.
