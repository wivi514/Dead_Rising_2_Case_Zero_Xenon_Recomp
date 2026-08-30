# Part 89 kickoff — the constants block is done; the board decides what's next

**THIS IS THE LIVE HAND-OFF**, superseding `part88-kickoff.md` (kept as the record of
part 88's step list — every step in it ran and every pre-registered threshold held).

## 0. What part 88 established (`phase5-notes.md` §6eh is the record and wins on numbers)

**Both §6eg items are SHIPPED, ON BY DEFAULT, verified and measured** — the whole
kickoff executed in one part:

| item | mechanism (exact) | milliseconds (3v3, crowd route) | arms |
|---|---|---|---|
| **bounded gather** | 52-54M dynamic copies/run at **−84.3% bytes** (16.5 → 3.0 MB/frame); control arm 0 bounded | dominant band 9,000-9,500: **13.06 → 12.67 ms, −3.0% / −0.39 ms**; GPU unchanged | `CZ_VK_NO_BOUNDED_DYNAMIC=1` off; verify 65.4M/0 bad; poison fires (5.51M, split counter) |
| **patch memo** | **99.9% of 118-122M patches/run served** (4-way MRU; 130-136k misses); control arm prints nothing | all bands ≥4,500 negative: 8,500-9,000 **−0.53 ms**, 9,500-10,000 −0.45 ms; GPU unchanged | `CZ_VK_NO_PATCH_MEMO=1` off; verify 115.7M/0 bad; poison fires on 100% |

Per-band deltas sit at the route's ±2.9% floor, which is why each item's mechanism
number is quoted beside its milliseconds — the mechanism is exact and cannot be argued
with (`part88-kickoff.md` §1 step 3's reading rule, honoured).

Step 0's census (`CZ_VK_PALETTE_EXTENT_CENSUS=1`) remains the standing re-check:
clean-cover 98.3-98.6%, dirty-fallback 1.4-2.2% (the high-water fallback is c255 =
full copy, honest and cheap), reuse 0.0% corroborated by §6ef. The read-above-extent
question was answered by the fill poison at the crowd (intact articulated crowds both
arms, `~/DR2CZ-troubleshooting/part88-filldump/`), not by a value compare, because no
value compare can see it (gotcha 432).

**What this leaves in the constants block** (17.3% ≈ 3.2 ms at the crowd before part
88): copy ~0.1 ms, patch ~0.05 ms, vs residual 0.61, shared 0.65, block residual 0.73.
**No mechanism-level lead remains inside it.** Re-profile before believing these
numbers — they are §6eg's, taken before both fixes.

## 1. THE BOARD (carried from part 88 §4, in order)

1. **The CW 2a/2b serial record-restructure re-pricing** — the one remaining named
   CPU item (+0.35 ms at THEIR crowd, ~10k diverged lines; re-price against §6ec's
   524 ns/draw before porting anything). If the re-price says <0.3 ms here, it dies
   without a port.
2. **The Windows bundle save-squatter hunt** — three saves eaten;
   `part86-kickoff.md` §0b has the specified repro. Needs czwin.
3. A natural level-up check (`part86-kickoff.md` §0b(b)); the glibc floor / AppImage;
   macOS (milestone C, needs hardware).
4. The combo bench vs phantom card grants (`part86-kickoff.md` §0c residual).

A fresh `CZ_VK_PROFILE` decomposition at the crowd is the cheap first step for any new
performance work: §6ec's per-draw table and §6eg's block split both predate part 88's
two fixes and the CW pump-stack import.

## 2. Gates inherited (unchanged from part 88 §3)

`--smoke` after every build; both PM4 boundary oracles after any pm4.cpp change (part
88 ran them for the tracker: 24.5M + 28.7k, 0 disagreements); `truncated=0`; any new
default ships WITH its off-arm and measured milliseconds in the same commit; the
censuses stay diagnostic arms and are never armed in a timed A/B arm.

## 3. For Case West (standing send-back, now with two more items)

Same engine, so both part-88 items should transfer nearly verbatim once CW has a
renderer at this stage: their bone palettes are the same `vc({8,9,10}+a0)` shape
(check their bank's `aluDynamicExprs` first — one grep), and their projection patch,
if they build one, should be born memoised. The write-extent tracker costs two
comparisons per ALU-overlapping run and is the kind of thing to build on day one.
