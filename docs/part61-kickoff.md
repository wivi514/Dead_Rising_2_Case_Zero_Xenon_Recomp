# Part 61 kickoff — the operator's verdict on the part-60 night run, then the list

> **THIS IS THE LIVE HAND-OFF**, superseding `part60-kickoff.md`. Part 60 shipped the
> host settings panel over the game's own options hub (the shipped OptionsPC screen is
> a SHELL — layout and strings present, verbs compiled out), and its overnight run
> landed all five night-plan items (`docs/part60-night-plan.md`, record in
> `phase5-notes.md` §6cp): aspect-fit presentation, shadow tiers, 21:9, the
> display-clamped resolution list and the live frame-cap row. **Every picture verdict
> from the night is headless; the operator's morning session is the authority on all
> of them.**
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction), and
> the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

## 0. The morning launch, and what to look at

One command (the panel is on the game's own `Options` hub; open it there):

```
cd runtime/build && ./cz_runtime
```

The operator's checklist, one line each — the six panel rows are: RESOLUTION,
DISPLAY MODE, VSYNC, SHADOW QUALITY, FRAME CAP, ASPECT:

1. **Black bars** (item 1): fullscreen on the 21:9 display with a 16:9 setting —
   equal side bars, no stretch. `CZ_VK_STRETCH=1` restores the old stretch if a
   comparison is wanted.
2. **21:9** (item 3): set ASPECT to 21:9, restart, play — full-bleed, wider view (not
   stretch), HUD centered, reflections/shadows sane, no seam at screen center, no
   flank artifacts. Watch for pop-in at the extreme flanks (the title's own CPU
   culling is 16:9 — expected, note severity). `CZ_VK_WIDE=0` kills it without
   touching the file.
3. **Shadow tiers** (item 2): at 1440p+ flip SHADOW QUALITY Low/High while standing
   under a hard shadow edge (the power-line shadow on the forecourt is the classic) —
   Low should be visibly blockier, High unchanged from before the part. It applies
   LIVE, no restart.
4. **Resolution list** (item 4): the row should only offer sizes the display can
   show.
5. **Frame cap** (item 5): flip OFF -> 60 -> 90 in play; the title-bar fps (readback
   arm) or a feel check should follow IMMEDIATELY. The one stated caveat: a live cap
   change leaves the guest's cached refresh rate stale until restart, which can very
   occasionally cost a frame — if the operator reports stutter after live changes,
   restart-and-check separates the two.
6. **Panel honesty**: no row that pretends. At 720p render scale the shadow row is
   inert and the footer says so.

## 1. What the night actually established (so nobody re-derives it)

* **Shadow investigation answer**: cascades rode CZ_VK_RES uniformly. The pass is
  identified by EDRAM `surfacePitch=1040` (= 13*80, i.e. "a 1024-wide surface" —
  measured census, only the cascade pass uses it). Same predicate at draw and resolve
  time = the two sides cannot disagree. Tier floors at 1x base.
* **21:9 needed NO HUD mechanism**: the title draws frontend UI and gameplay HUD
  under a 16:9 PERSPECTIVE the projection patch recognizes, so the patch centers them
  as a side effect — measured twice (attract copyright, outdoor "0 KILLED"), both
  landing on the centering prediction to within a point. Do not build the planned
  window-coordinate offset; it would catch post-chain quads.
* **The UCP planes are compensated** (plane.x * 21/16 for recognized draws) so gore
  cuts stay put in wide mode. If the operator reports slicing defects IN WIDE MODE
  ONLY, that compensation is the first suspect — `CZ_VK_WIDE=0` is the discriminator.
* **Truncating conversion is load-bearing** (gotcha 373): any new host-extent site
  must go through HostX/RSX, never a rounded multiply.
* **The vblank period is a live atomic in MICROSECONDS** (gotcha 375): env pins it,
  the menu is refused loudly under an env pin, and the env path reproduces the old
  ms arithmetic bit for bit.
* **Commit hygiene note**: items 4+5 share commit 893749b (two features, one commit —
  documented in its message; don't read it as one feature).

## 2. Gates state at hand-off

* `--smoke` OK after every commit of the night.
* Validation: wide boot and 16:9 control both show ONLY the pre-existing
  `VUID-VkGraphicsPipelineCreateInfo-topology-08773` class (point-list PointSize,
  6 each). **That VUID is now a named open item — cheap to fix (write PointSize in
  the vertex shader when topology is point list, or enable maintenance5); nobody has
  looked at whether point sprites render correctly.**
* A5 diff at close: **exit 0, 4 permutation windows, 0 real** — same shape as part
  59's sweep. E gate at close, default arm (16:9, scale 1): logo card
  **+0.9599 identity** against `E2_title_screen_logo.png` (the standing number is
  +0.9597), attract background +0.8974 identity against E3 (attract camera moment
  varies run to run; layout agrees). PM4 oracles and the dim census were NOT re-run:
  nothing in the night touched pm4.cpp or the shader cache (part 59's green stands).
* The one code path no headless gate can exercise: the swapchain overlay's
  horizontal centering in wider-than-16:9 windows (needs a window) — the operator
  sees it the first time they press F4 or open the panel on the 21:9 display.
* Tier A/B, wide A/B: headless halves done (engagement + no breakage); operator
  halves owed above.

## 3. After the verdicts, the standing list (unchanged from part 60)

* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1` ready.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Performance stays PARKED (`perf-state-parked.md`; Night Run 1's §6 updated it —
  remaining supervised items are item A's order gate and item C's gather design).
* The live-resolution switch (VkRenderer_RequestRenderScale) stays parked with its
  freeze logs — resolution is apply-at-next-launch on the operator's call.
