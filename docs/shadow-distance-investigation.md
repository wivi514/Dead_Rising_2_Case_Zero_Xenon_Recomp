# Shadow-distance ("shadows only render up close") — investigation, PARKED part 93

**Operator request (2026-09-03):** add a Visuals option to increase the distance at
which objects receive proper (cascaded) shadows. In this game you must be close to an
object for its shadow to render; beyond the near cascades the world falls to a coarse
static town-wide shadow. Reference shots: `~/DR2CZ-troubleshooting/shadow-distance-ref/`
(the junkyard — the fence's shadow in front of the junk cars, and a car's own shadow,
both only visible when Chuck is close).

**Status: PARKED. Two approaches built, BOTH confirmed NON-WORKING by the operator.**
The menu row and persisted setting were REVERTED (a setting that does nothing is the
gamma-slider anti-pattern). What remains committed: the env-only parked experiment
`runtime/cpu/shadow_distance.{h,cpp}` (gated on `CZ_SHADOW_DIST`, off = bit-identical),
and the recon instrument `CZ_PROP_TRACE_ALL` (commit 96c3b95).

## The data flow (established, with disassembly evidence)

The cascade split distances are data-driven named properties, bound through the
universal property binder `sub_82375518` (the same mechanism the FOV slider uses):

- `Start_CascadeDist` (count 7) → object field `this+0xB4`
- `End_CascadeDist` → object field `this+0xF8` (Start/End names do NOT pair 1:1 with the
  interpolation's start/end blocks — the interpolator pairs `+0xB4` with `+0x11C`)
- At Still Creek the authored 7-float array is `[5, 15, 35, 40, 0, 0, 50]`.

A time-of-day interpolation `sub_823C1CC8` reads those fields, blends Start→End, and
writes the ACTIVE cascade distances into SEVEN scattered globals:

```
0x829DFBFC 0x829DFC00 0x829DFC04 0x829DFC08   (from field +0xB4..+0xC0)
0x829DEB7C 0x829DEB80 0x829DEB88               (from field +0xC4..+0xCC)
```

The cascade builder `sub_825A89A8` re-reads those globals every render and builds the
per-cascade near/far splits + projection matrices (calling matrix builder
`sub_825A7CE8`), which become shader constants pc(45)/pc(46) (the splits) and pc(30-42)
(the cascade matrices). The far/static town shadow is pc(40-42) + pc(47).w = 3583 units.

## What was tried, and why each failed

**Approach 1 — write scaled values into the object field (`+0xB4`).** Wired through the
binder-latch + a per-frame enforce. **Operator-confirmed no effect (4x == 1x).** The game
reads the field ONCE per time-of-day update, caches into the seven globals, and never
re-reads the field. Writing after the cache is built is ignored.

**Approach 2 — hook the consumers.**
- `sub_823C1CC8` (the interpolator): **NEVER CALLED** on this title. 0 ENTER calls even
  with shadows rendering — Case Zero's Still Creek almost certainly has STATIC
  time-of-day, so the interpolation never ticks. Hooking it did nothing.
- `sub_825A89A8` (the builder): **fires** (confirmed, 1 call when shadows render). Dumped
  the seven globals at its entry: `5.0 10.0 15.0 0.0 1.0 1.0 50.0` (note: these differ
  from the raw field `[5,15,35,40,0,0,50]` — the write path transforms them). Scaled the
  `>1.0` ones (5,10,15,50) at entry and restored at exit. **Operator-confirmed no effect
  (4x == 1x).**

## Why approach 2 probably failed — the open leads for whoever resumes

The builder fires and reads the globals, yet scaling them moved nothing. Candidate
reasons, in rough priority:

1. **We may be scaling the wrong globals.** The disassembly showed `sub_825A89A8`
   reading `0x829DEB7C` (=1.0) and `0x829DEB80` (=1.0) and computing a "split delta" from
   them — i.e. it reads the globals that held **1.0**, which our `>1.0` rule SKIPPED. The
   distance-looking values (5,10,15,50) may not be what the builder's render extent comes
   from. **Try scaling ALL seven globals (including the 1.0 ones), or specifically
   0x829DEB7C/80.**
2. **The RENDER extent and the SAMPLING extent are separate.** Extending shadows needs
   BOTH what is DRAWN into the shadow atlas (the cascade render projection/viewport) AND
   how the shader SAMPLES it (the split constants + matrices) to grow consistently. The
   builder may only produce one of them; the render-pass projection may be set elsewhere.
   Find where the cascade shadow PASS's projection/viewport is set.
3. **The builder may cache its matrices** and only rebuild on a detected change, so the
   scale-then-restore is undone before the render. If so, PERSIST the scaled globals
   (keep them set while `CZ_SHADOW_DIST != 1`) instead of restoring — but that risks
   compounding if anything re-derives them; check first.

## How to VERIFY without burning an operator session (do this next time)

The mistake this session was handing the operator two unverified guesses. Verify the
render extent moved HEADLESSLY first: dump the shadow atlas (the cascade depth snapshot,
e.g. `*_8192x2048_depth.ppm` / `*_2560x1440_depth.ppm` in a `CZ_VK_SNAP_DUMP` frame) at
`CZ_SHADOW_DIST=1` vs `=4` and diff. If the atlas depth content changes, the render
extent moved and it's worth an operator look; if identical, the lever is still wrong.
(Headless DebugJump runs reach shadow rendering only intermittently — often 0-1 shadow
resolves per run because the fake input lands in menus — so use `CZ_AUTOCHUCK` and a
long timeout, and check `resolve: source is the DEPTH buffer` count > 0 before trusting a
null.)

## Renderer-side fallback (if the guest path stays intractable)

Intercept the cascade projection MATRICES (pc30-42) and split constants (pc45-47) in the
renderer's constant upload AND widen the cascade render pass. This is guaranteed to
intercept but is fiddly (per-cascade ortho rescale) and must keep render + sample
consistent — the whole reason the guest-source approach was preferred.

## Files / handles

- `runtime/cpu/shadow_distance.{h,cpp}` — the parked hook (currently on `sub_825A89A8`),
  env `CZ_SHADOW_DIST=<0.25..8>`, `CZ_SHADOW_DIST_TRACE=1` dumps the globals.
- `CZ_PROP_TRACE_ALL=1` (camera_fov.cpp) — the full named-property census that found
  `Start_/End_CascadeDist`.
- Reference captures: `~/DR2CZ-troubleshooting/shadow-distance-ref/` (junkyard, stock).
