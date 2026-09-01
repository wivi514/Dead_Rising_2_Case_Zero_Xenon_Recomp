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

## 0c. PERFORMANCE IS PARKED (2026-09-01) — the operator's instruction and the state that resumes it

The instruction, verbatim, after being shown the arc's total (~13.5 → 11.2 ms at the
crowd, −17%, GPU 10.6 → 9.8 under it): *"Save everything on performance we'll switch
to something else so we can get a release really soon."*

**The state, all of it already recorded — do not re-derive**: `phase5-notes.md`
§6ek (the clears, four addenda, the final numbers) and §6el (the copy census);
`perf-plan-part90.md` §4 (the executed plan); the fresh decompositions in
perf-plan-part90 §0. The board on parking: **no known lead ≥0.5 ms on either side**
— the crowd is CPU-bound by ~1.3-1.4 ms (wall ~11.2 / GPU ~9.8, fence 0.00), the
serial residue is the PM4 walk + the resolve half (change-detector class, gotchas
474/4), and the one NAMED-but-parked GPU item is the resolve copies' 36.4% dead
share (§6el, ~0.1-0.28 ms, prediction-only mechanism, `CZ_VK_COPY_CENSUS=1`
re-asks it). Every arm is in `docs/instruments.md`; the picture-complaint bisection
order is §0b's. Whoever resumes: fresh profiled decomposition first (a recorded
number has a shelf life), and `tools/part80_crowdroute.sh` is still the route.

## 0d. WHAT TO TRY LATER TO IMPROVE PERFORMANCE — the resume list, in order of promise

Written at parking time so resuming costs a read, not a re-derivation. Every number
here has a shelf life (gotcha 13): re-profile at the crowd before pricing anything.

1. **The arithmetic that governs everything**: the crowd is CPU-bound by ~1.3-1.4 ms
   (wall ~11.2 / GPU ~9.8, fence 0.00) — a CPU saving converts 1:1 up to ~1.4 ms
   there; a GPU saving converts NOTHING at the crowd until the CPU falls, but is
   wall time in every band below ~7,000 draws and headroom/resolution everywhere.
2. **The resolve copies' dead share** (§6el) — the only NAMED item: 36.4% of copies
   dead, ~0.1-0.28 ms of GPU. Mechanism is prediction (skip a copy expected to be
   overwritten unsampled) with a SILENT-STALE failure mode and no inline fallback —
   build it only with an exact verifier (part 88's way: predict + verify + poison),
   and `CZ_VK_COPY_CENSUS=1` re-asks the number first.
3. **The PM4 walk** — ~3.1 ms instrumented share, the largest single serial term.
   It is a register-write change-detector loop (gotchas 474/4): memoisation is
   refuted, but nobody has tried (a) batching register-window writes into wider
   memcpys, or (b) a second walk thread speculating ahead on packet BOUNDARIES only
   (the order stays semantic). Both are research, not items — price the walk fresh
   first; it has never had its own decomposition.
4. **The resolve half of UploadStream** (162 ns/draw true, §6ei) — change-detector
   class, same caveat. The flat-cache lookup is the measured majority; a
   perfect-hash over the stable working set was never tried.
5. **Resolution scaling as the headroom spender** (memory: it was measured FREE in
   crowds at 1440p in part 51's era) — with ~1.4+ ms of GPU headroom at the crowd
   and more below it, CZ_VK_RES up a notch is the cheapest visible win the parked
   state supports. An operator preference question, not an engineering item.
6. **The instruments to trust**: `tools/part80_crowdroute.sh` (±2.9% floor, 3v3,
   band by draws), `CZ_VK_GPU_PASSES=1` (bill nil, residual first),
   `CZ_VK_PROFILE` (~806 ns/draw bill — mechanism only, never wall),
   `CZ_VK_FRAME_TRACE` + `part80_trace_band.py` for regime tables. FRAME_STATS
   costs ~15 ms/frame at 3440x1440 — picture gates only.

## 1. THE BOARD — a few small fixes first (operator's list), THEN the release

**2026-09-01, operator:** *"Do not want you to proceed with stuff for release yet I
want you to fix a few small things before we work on release."* The release board
below is NEXT AFTER those fixes, not the live work. The operator names the fixes.

**Fix 1 — DONE, operator-verified** (*"It is perfect tried multiple resolution and
it worked"*): live internal-resolution apply from the Visuals panel, pending-until-X
(`phase5-notes.md` §6em — the part-60 freeze was the apply's mid-frame placement,
fixed by relocating it to BeginFrame's non-recording entry). Further fixes as the
operator names them.

### The release board, for when the fixes are done ("really soon"), in order

0. **Rebuild the Windows leg** — the shipped bundle's binaries predate the ENTIRE
   performance arc (parts 87-90: constants, parallel record, deferred clears).
   czwin pull + build + `--smoke`, then the ps1 package gate (it RUNS the staged
   exe). Self-servable over ssh.
1. **The Windows bundle SAVE verification** — part 86 shipped the save relocation
   (`f396c2f`, saves per-profile in the OS saved-games location, with migration),
   which sidesteps the §0b(a) squatter, but no bundle-rooted Windows run has
   verified a save round-trip since. The part-86 repro (bundle root, reach gameplay,
   save once, `CZ_FILE_TRACE=1`) is the run; it likely needs the operator's hands
   for the save-point menu.
2. **Repackage and re-gate BOTH artifacts** on the current code: Linux
   (`release_text_identity.sh`, `release_package_linux.sh`,
   `release_gate_clean_container.sh` — must print GATE PASSED) and Windows (the ps1).
   The packaging scripts preserve player assets themselves (fd313b4) — verified
   again on the repackage.
3. **The glibc floor / AppImage** — the last named Linux packaging limitation
   (`release-plan.md` §9.8 owed list). An old-base container build or AppImage.
4. **First-boot polish**: the one-time ~20 s pipeline pre-warm runs silently after
   the progress window closes (§9.8's stated debt) — put it under a visible screen.
5. Operator checks when convenient: a natural level-up (§0b(b) — F4 awards verified,
   the natural path is the residual), and the combo bench vs phantom cards
   (`part86-kickoff.md` §0c residual).
6. macOS is milestone C and needs hardware — explicitly OUT of "really soon".

RT shadows stay parked (part 70). Performance stays parked (§0c).

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
