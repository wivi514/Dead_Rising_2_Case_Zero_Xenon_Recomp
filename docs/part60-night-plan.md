# Part 60 night plan — 21:9, aspect-correct presentation, shadow-resolution tiers

Operator's instruction, verbatim scope: *"21:9 support. also apply black bar if we
select like 16:9 resolution on 21:9 screen like all other games do instead of
stretch to fit. and also make so we change shadow resolution with the shadow
settings."*

Ordered easiest-first so every hour of the night ends with something shippable.
Each item lands as its own commit(s) behind its own arm, gates run after each.
Resolution stays apply-at-next-launch (the live path is parked with its freeze
logs; not in tonight's scope).

## 1. Aspect-correct presentation (pillarbox / letterbox) — the black bars

**What**: the presented frame keeps its aspect ratio inside the window/display.
A 16:9 internal frame on the operator's 21:9 display gets side bars instead of
the current stretch; a future 21:9 internal frame on a 16:9 display would get
top/bottom bars. This is presentation-only — no internal rendering changes.

**Where** (both present arms, one fitted-rect computation shared):
- Swapchain arm: `RecordSwapchainBlit` — clear the swapchain image to black,
  then `vkCmdBlitImage` into the centered aspect-fit rectangle instead of the
  full extent.
- Readback arm: the `SDL_RenderCopy` destination rect in window.cpp, with a
  black clear first.
- The F4/settings overlay blit is window-relative and stays as it is.

**Arm**: aspect-fit is the new default (it is what every shipped game does);
`CZ_VK_STRETCH=1` restores today's behavior as the control.

**Verify**: code + validation run headlessly; the picture needs the operator
(present paths are invisible to headless gates — the part-54 lesson). Morning
check: fullscreen on the 21:9 display, bars present, image undistorted.

## 2. Shadow-resolution tiers — wire the Shadow Quality row

**Investigation first** (the kickoff §3 unknown): how the shadow surfaces ride
`CZ_VK_RES` today. Known going in: the cascades render into the scaled EDRAM;
the 4096x1024 fmt-22 atlas resolves into snapshots at the scene scale; the R6
work identified the atlas draws (bound at s2/s7) and part 58 identified the
shadow passes (the ten ortho `bvc` blocks).

**Design (to be confirmed by the investigation)**: a per-pass scale factor for
shadow rendering — tier High = scene scale, Medium = half, Low = quarter
(floor 1x against the 1280-base) — applied to the shadow pass's viewport/
scissor and to the atlas resolve extents so the snapshot and its normalized-UV
fetches stay consistent. If resolve-side scaling forces copy→blit changes in
the resolve path, the fallback design is High/Low only (Low = 1x host, i.e.
shadows stop scaling with resolution) — still an honest, visible tier.

**Arm**: `CZ_VK_SHADOW_TIER=0|1|2` overrides the settings file (measurement
wins); the panel row drives the persisted value and drops its "not wired" label
only when this lands.

**Verify overnight, headlessly**: outdoor DebugJump route, frame dumps at tier
0 vs 2 — shadow edge blockiness is visible in stills; snapshot-creation log
lines carry the host extents, so the tier engaging is a grep, not an argument.
Apply at next launch first; live later only if it is trivially safe.

## 3. 21:9 internal rendering

**The arithmetic that makes it plausible**: 21:9 of 720 rows is exactly
1680x720 — and 1680 = 1280 * 21/16 with 21/16 also taking 640 → 840 exactly.
So the wide mode multiplies every RENDER-PIPELINE X extent by 21/16 on top of
the integer scene scale: EDRAM 1680s wide, the title's two 640-wide tile
scissors become 840s each, both exact. The part-41/45 invariant holds: surfaces
from the render pipeline scale, surfaces from guest memory never do.

**The pieces**:
1. `RSX()/RSY()` split of today's uniform `RS()`; X picks up the 21/16 factor
   in wide mode. Sweep all 23 `RS()` sites + scissor/viewport paths and classify
   each as X, Y, or area.
2. **Projection patch**: the scene projection must widen its horizontal FOV by
   16/21 on `m[0][0]` at constant-upload time, or the wider frame just shows
   the same view stretched. Discriminator measured in part 58's clip-plane
   work: 16:9 perspective projections carry an exact 9/16 xscale/yscale ratio —
   the shadow orthos and the CUBE FACE projections (1:1) do not match and stay
   untouched, which is what keeps reflections and shadows correct.
3. **Window-coordinate draws (HUD, menus)**: centered in the wide frame with a
   +200s guest-pixel X offset — the 16:9 HUD floats centered, the standard
   ultrawide presentation.
4. Resolve/snapshot extents for scene surfaces follow RSX; texture uploads are
   untouched by construction.
5. Presentation: 1680s x 720s is exactly 21:9, so item 1's aspect-fit shows it
   full-bleed on the operator's display.

**Settings**: a fifth panel row, `ASPECT: 16:9 / 21:9` (`aspect=0|1` in
cz_settings.txt), apply at next launch. `CZ_VK_WIDE=1` is the env arm and wins.

**Verify overnight, headlessly**: outdoor frame dumps 16:9 vs 21:9 — the wide
dump must show GEOMETRY at the flanks that the 16:9 dump does not contain (a
wider field of view, not a stretch); HUD elements centered; cube-reflection and
shadow spot-checks in the same dumps. The known risks to watch: draws whose
projection the discriminator misses (present as a layer at the wrong width),
non-multiple-of-16 guest scissor X values (present as 1px seams), and the DoF/
blur taps whose screen-fraction footprint is already accepted behavior at 2x.

## Standing tail for the night

- Gates after each item: `--smoke`; the A5 diff once more before morning (the
  input-import hook changed this part); `CZ_VK_VALIDATION=1` pass on the
  final build.
- Docs: the part-60 story so far (the shipped-screen shell verdict, the LZX
  trailer finding, the panel pivot) into `phase5-notes.md` + `gotchas.md`
  candidates; refresh `part61-kickoff.md` at close.
- Morning hand-off: one launch command; the operator checks (a) bars on 16:9
  fullscreen, (b) 21:9 mode look, (c) shadow Low vs High, (d) panel rows honest.
