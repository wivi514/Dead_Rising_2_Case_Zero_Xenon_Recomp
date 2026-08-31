# Part 91 kickoff — after the deferred clears: the GPU frame has been cut twice, and one operator verdict is owed

**THIS IS THE LIVE HAND-OFF**, superseding `part90-kickoff.md` (kept as the record of
part 90's framing — its §0b pricing frame was confirmed by measurement and then acted
on in the same part).

## 0. What part 90 shipped (`phase5-notes.md` §6ek-§6el are the records and win on numbers; `perf-plan-part90.md` §4 is the executed plan)

**Deferred scoped clears are ON BY DEFAULT** (`CZ_VK_NO_DEFERRED_CLEAR=1` the
same-binary control; `CZ_VK_DEFER_FULL_RECT=1` the two-factor diagnostic):

| what | number |
|---|---|
| step 0 regime (census run, pre-fix) | crowd wall 11.10 / GPU 10.60 / fence 0.00 — CPU-bound by ~0.5 ms; every band <7,000 draws GPU-bound. §0b's flip confirmed measured |
| the mechanism | a resolve's clear bits latch as a scoped rect, emitted as `vkCmdClearAttachments` at the head of the next pass's first instance (worker chunk or pump tail — an instance that ALREADY exists; no per-clear scope, which is what made the part-32 arm a wash) |
| clear class | 0.66 → **0.009-0.010 ms/frame (−98.5%)**; 91.7% of the class's pixels removed (post-MSAA-correction); flush-before-read fallback 13-15% of emissions, kill (<30%) did not fire |
| device frame | **−0.8 to −0.95 ms in every one of 10 draw bands, monotone** (crowd band GPU 12.41 → 11.46) — more than the clear class alone; the 83.6 deleted TRANSFER_DST round-trips also de-serialized the timeline |
| pump CPU | §4b resolve/begin cycle 0.55-0.59 → 0.51-0.53 ms/frame |
| gates | sync validation 0 hazards (both builds); era medians INSIDE the null (2 fix runs); poison positive control two-sided (deferred == control to 0.1/255 on poisoned dumps, both hugely different from unpoisoned) |

**The copy census** (`CZ_VK_COPY_CENSUS=1`, item 2) NAMED and PARKED an item: 36.4% of
resolve copies are dead (13.1% of pixels — the bloom pyramid family at 22,139 dead
each, the shadow atlas, the scene depth), worth ~0.1-0.28 ms, mechanism is
prediction-only with a silent-stale failure mode. §6el.

## 0b. THE OPERATOR'S YELLOW-STREAK REPORT — the one thing OWED, and the first bisection

Mid-part the operator saw a transient "yellow streak / meteorite shower" during a
camera turn, absent in the previous run they watched. **It never reproduced in 20,000
dumped frames** (two routes, dense dumps, detector + contact sheets), but code reading
found a real divergence in exactly the right class: the scoped rect was missing the
draw path's **4x-MSAA sample-space factor** — 4.4 clears a frame covered ONE QUARTER
of their pass's EDRAM footprint (the whole-image clear had hidden the missing factor
since phase 5). Fixed in the same commit; counter
`resolve: clear rect scaled for a 4x MSAA surface`.

**Status: UNEXPLAINED-BUT-PLAUSIBLY-FIXED, not closed** (gotcha 506). The operator's
next session is the verdict. **The bisection order for ANY new picture complaint is
now: (1) `CZ_VK_NO_DEFERRED_CLEAR=1`, (2) `CZ_VK_DEFER_FULL_RECT=1` (scoping vs
ordering), (3) `CZ_VK_NO_PAR_RECORD=1`.** A recurrence under (1) exonerates part 90
entirely; a vanish under (2) says scoping and the MSAA fix was incomplete.

## 1. THE BOARD (in order)

0. **The operator verdict on §0b** — one play session, no instruments needed beyond
   their eyes; if they see it, the bisection above, ideally with
   `CZ_VK_FRAME_DUMP_EVERY=8` armed so the streak lands on disk.
1. **The regime after part 90**: the crowd is **CPU-bound by ~1.3-1.4 ms again**
   (wall ~11.2 vs GPU ~9.8, fence 0.00 — §6ek's clean-pair addendum corrected the
   first "balanced" reading), so the clears re-opened convertible room for any future
   CPU saving; the GPU-bound bands below 7,000 draws banked −0.6 ms of wall. The GPU
   side's remaining named items: the resolve copies' dead share (§6el, ~0.1-0.28 ms,
   risky mechanism), the post chain (REFUTED — title's own work), the big passes (the
   game). The CPU side: unchanged from §6ej §4 — PM4 walk and the resolve half,
   change-detector class. **No known lead ≥0.5 ms on either side.** At ~11 ms the crowd holds a
   locked 60 fps with margin; further work buys headroom and resolution, not felt
   frame rate. **Parking again is an honest outcome; surface it to the operator.**
2. The Windows bundle save-squatter hunt (`part86-kickoff.md` §0b; needs czwin) — and
   the Windows build has not been rebuilt since parallel record OR deferred clears
   shipped; build and run that leg before any operator session there.
3. A natural level-up check (`part86-kickoff.md` §0b(b)); glibc floor / AppImage;
   macOS (milestone C, hardware); the combo bench vs phantom card grants
   (`part86-kickoff.md` §0c residual).

## 2. Gates inherited (unchanged, plus one)

`--smoke` after every build; PM4 boundary oracles after any pm4.cpp change (part 90
touched none); `truncated=0`; sync validation 0 hazards for any barrier/clear change,
with `CZ_VK_BARRIER_POISON=1` its positive control; any new default ships WITH its
off-arm and measured milliseconds in the same commit. **The picture-complaint
bisection order is §0b's.**

## 3. For Case West (standing send-back)

The deferred-clear design transfers whole with the parallel recorder (it rides the
same every-pass-records-an-instance property), and gotcha 506 is the transferable
core: **audit every coordinate-space factor the draw path applies before scoping any
write the old code did over-broadly** — the MSAA sample-space factor reached CW's
notes 58 parts late here. Publish the clear as scoped from day one there, WITH the
factor, and keep the whole-image form as the control arm.
