# Part 91 kickoff — after the deferred clears: the GPU frame has been cut twice, and the visual report against them is closed

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
| clear class | 0.66 → **0.009-0.010 ms/frame (−98.5%)**; **90.2% of the class's pixels removed** (final: SURFACE-footprint rects × the MSAA factor — see §0b's resolution); flush-before-read fallback 13-15% of emissions, kill (<30%) did not fire |
| device frame | **−0.8 to −0.95 ms in every one of 10 draw bands, monotone** (crowd band GPU 12.41 → 11.46) — more than the clear class alone; the 83.6 deleted TRANSFER_DST round-trips also de-serialized the timeline |
| pump CPU | §4b resolve/begin cycle 0.55-0.59 → 0.51-0.53 ms/frame |
| gates | sync validation 0 hazards (both builds); era medians INSIDE the null (2 fix runs); poison positive control two-sided (deferred == control to 0.1/255 on poisoned dumps, both hugely different from unpoisoned) |

**The copy census** (`CZ_VK_COPY_CENSUS=1`, item 2) NAMED and PARKED an item: 36.4% of
resolve copies are dead (13.1% of pixels — the bloom pyramid family at 22,139 dead
each, the shadow atlas, the scene depth), worth ~0.1-0.28 ms, mechanism is
prediction-only with a silent-stale failure mode. §6el.

## 0b. THE OPERATOR'S VISUAL-ISSUE REPORT — **CLOSED 2026-09-01, operator-verified** (the history below is kept as the record of the hunt)

**RESOLUTION (§6ek addendum 9)**: the operator's A/B/A tracked the arm exactly
(absent on `CZ_VK_NO_DEFERRED_CLEAR=1` twice, present on the default, absent under
`CZ_VK_DEFER_FULL_RECT=1`) — coverage, not ordering. The shipped rect was the
RESOLVE WINDOW; hardware's clear bits clear the SURFACE's tiles, and the sun-glow
machinery reads inside the surface but outside the window. **The scoped rect is now
the destination surface's footprint** (× the MSAA sample factor), and the operator
could not reproduce the blow-out at the same view on the fixed default. The
bisection order for future picture complaints stands as written below.

### The hunt as it unfolded (superseded by the resolution above)

Mid-part the operator reported a "yellow streak / meteorite shower" during a camera
turn. **It never reproduced in 20,000 dumped frames**, but code reading found a real
divergence in the right class: the scoped rect was missing the draw path's **4x-MSAA
sample-space factor** — 4.4 clears a frame covered ONE QUARTER of their pass's EDRAM
footprint. Fixed in the same commit (`resolve: clear rect scaled for a 4x MSAA
surface` is the counter).

**Then the operator played the CORRECTED build, captured the artifact with F9, and
corrected the report: it is NOT a transient streak — it is VIEW-DEPENDENT** ("depends
on where the camera is placed"). The evidence is
`~/DR2CZ-troubleshooting/play/play_0831_1458/` — **capture_012456 is the defect
live**: a huge geometry-shaped white-yellow blow-out over the foreground zombies with
the sun behind them; capture_013440 is nearly the same view CLEAN seconds later.
Established from the frame's own resolve snapshots: **the blob boundary follows
character silhouettes** (material/lighting, not a scissor-shaped clear rectangle);
**cube faces normal**; **luminance chain higher in the defect frame** (reacting, not
causing); the bloom bright-pass carries the blown mass downstream. So the artifact
SURVIVES the MSAA correction, and whether it is part 90's at all is UNKNOWN — it may
equally be part 88's constants work (a specular/rim term is exactly a constants-fed,
view-dependent thing), part 89's, or the title's own sun-behind rim look the operator
never met at this hour of the in-game clock.

**Status: OPEN, UNATTRIBUTED. The A/B protocol is defined and OWED** — the operator
left for work before running it: play at that spot (by the van, sun behind the
crowd) under (1) `CZ_VK_NO_DEFERRED_CLEAR=1`, then the default, F9 in each.
Seen-in-both exonerates part 90 → bisect next on `CZ_VK_NO_PATCH_MEMO=1` /
`CZ_VK_NO_BOUNDED_DYNAMIC=1` (part 88) and `CZ_VK_NO_PAR_RECORD=1` (part 89);
seen-only-in-default → `CZ_VK_DEFER_FULL_RECT=1` splits scoping from ordering. The
in-game clock moves the sun between sessions — match the VIEW, not the wall time.
The `.pose` for the defect camera is beside the capture.

## 1. THE BOARD (in order)

0. ~~The operator verdict on §0b~~ — **DONE 2026-09-01**: A/B/A run, defect
   convicted (window-vs-surface scope), fixed, and the fix operator-verified at the
   same view. Nothing owed on §0b.
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
notes 58 parts late here, and the second conviction is the rule to publish: **the
copy block's clear bits clear the destination SURFACE's tile footprint, not the
resolve window** — the sun-visibility machinery reads between the two. Scope to the
surface footprint (× the sample factor) from day one, and keep the whole-image form
as the control arm.
