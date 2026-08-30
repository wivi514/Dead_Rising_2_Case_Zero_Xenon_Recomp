# Part 88 kickoff — the bone-palette bounded gather

**THIS IS THE LIVE HAND-OFF**, superseding `part87-kickoff.md` (kept as the record of
part 87: all three CW leads refuted by census — its §0b — and the new lead found in the
same session's run data — its §0c).

**The subject is still PERFORMANCE.** Part 87 closed with the two largest addressable
CPU items known anywhere on the board, both inside the constants block, both priced on
the crowd route with the profiler's sub-scopes (`phase5-notes.md` §6eg is the record and
wins on any number):

| item | mechanism | price at the crowd | ceiling |
|---|---|---|---|
| 1. **write-extent-bounded dynamic gather** | 22 bone-palette VS (`vc({8,9,10}+a0)`) fall back to a full 4 KB window copy per draw — 87.3% of constant-copy bytes, ~11-12 MB/frame — while the mean palette upload is ~18 float4 regs | `constVsCopy` 0.48 ms | **~0.4 ms** |
| 2. **projection-patch memo** | `SceneXformForm` + fov/wide rewrite runs per VS copy on a 64-byte input | `constVsPatch` 0.51 ms | ~0.3 ms |

Item 1 first; item 2 only if 1 lands cleanly (one change per experiment).

---

## 1. ITEM 1, STEP BY STEP — and step 0 is a CENSUS, not code

**Step 0 — settle the correctness question before writing the fix.** The bound is sound
only if every dynamic-shader draw's `a0` reach is covered by the palette-region extent
written for THAT object. Two failure shapes to measure, both cheap on the census hooks
that already exist (`CZ_PM4_ALU_WRITE_CENSUS`'s two write-path sites):

* **Partial updates**: a palette-region write burst that does NOT start at c8. Part 87's
  histogram shows whole-span bursts (exact-equal count runs), which is evidence, not
  proof. Count them per frame at the crowd.
* **Short-after-long**: consumed-extent distribution per dynamic VS copy — add a
  `Pm4_TakeVsPaletteExtent()` (extent of c8-anchored writes since last consume) and have
  the dynamic copy path COUNT what it would have copied, changing nothing yet. This is
  the same ask-first discipline as §6ec/§6ef (gotchas 428, 434, 470).

**Pre-registered kill**: if the *sound* bound achievable (per-burst extent, or the
running high-water fallback if partial updates are real) saves **< 30% of full-copy
bytes**, the item dies — say so and stop (the pre-register-the-kill rule).

**Step 1 — the fix**, behind its own arm: `CopyConstWindow`'s dynamic path copies
`c0..c3 ∪ list ∪ [8, extent]` instead of 256 registers. `CZ_VK_NO_BOUNDED_DYNAMIC=1` is
the same-binary control (the part-87 renderer). Registers above the extent get the
`CZ_VK_GATHER_FILL` treatment on the poison arm, not silence.

**Step 2 — verification, two-sided**:
* Value identity where it is provable: a verify arm does BOTH copies and compares
  `[8, extent]` byte-for-byte (must be 0 disagreements over a crowd run), the
  `CZ_VK_VERIFY_CONST_GATHER` pattern extended to the dynamic population.
* Read-above-extent, which no value compare can see (gotcha 432's shape): the poison arm
  fills above-extent with the gather-fill constant — a zombie crowd that looks right
  under poison is the evidence; a frame-dump diff or the operator's eye is the reader.
  Zombies are the subject, so this needs the crowd, not the safehouse.

**Step 3 — measurement**: 3v3 on `tools/part80_crowdroute.sh` read with
`part80_trace_band.py` (frame-weighted, banded medians). Expected ~0.3-0.4 ms ≈ 2-3% —
**at the route's ±2.9% floor**, so quote the MECHANISM number alongside (gather-stats
bytes/frame: ~11-12 MB must fall to ~2-3 MB; that one is exact and cannot be argued
with). Rules from part 87 §1 stand: the rig arms nothing itself, run campaigns loud,
`pgrep cz_runtime` first, no builds while a chain measures.

## 2. ITEM 2 — the patch memo (after item 1 only)

Key = the 16 c0..c3 dwords (+ the fov/wide parameter stamps); on a hit, serve the
previously patched 64-byte block and skip `SceneXformForm` + both patches. Verify arm =
run the patch anyway and compare its output against the served block. Off-arm from day
one; same 3v3 + mechanism-counter reading.

## 3. GATES INHERITED

Part 87 §3's set unchanged (ring-latency engagement counters, `truncated=0`, any new
default ships WITH its off-arm and measured milliseconds in the same commit). Plus:
`--smoke` after every build; the reuse/vcull censuses stay diagnostic arms and must
never be armed in an A/B arm being timed.

## 4. ALSO ON THE BOARD (carried, in order)

1. The CW 2a/2b **serial** record-restructure re-pricing (wave-2; +0.35 ms at THEIR
   crowd, ~10k diverged lines — re-price against §6ec's 524 ns/draw before porting).
2. The Windows bundle save-squatter hunt (`part86-kickoff.md` §0b; repro specified).
3. A natural level-up check (§0b(b)); the glibc floor / AppImage; macOS (milestone C).
4. The combo bench vs phantom card grants (§0c residual).

**For Case West (send-back, standing offer):** their title is the same engine — before
they build either of their leads, run the vcull census at their crowd and the four-cell
reuse census (`part87-kickoff.md` §0b has the numbers to compare against).
