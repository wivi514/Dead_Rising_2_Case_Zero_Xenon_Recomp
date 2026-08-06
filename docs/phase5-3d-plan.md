# Plan: the title screen's 3D background

Written 2026-08-05, after the Y flip (§6q) and the texture swizzle (§6r) made the title
screen's 2D layer correct. Read `docs/phase5-notes.md` first — this plan assumes its
findings and does not repeat them.

## Where we actually are, measured

At the title screen's steady state, one run, snapshots and presented frame taken
together:

| surface | coverage | colours | reading |
|---|---|---|---|
| `06BE4000` — the scene | **26.0%** | **4,576** | the 3D scene IS being rendered: a lamp post with its fitting, road with yellow markings, kerb surfaces — with a class of geometry still distorted |
| `00E48000` — the presented frame | **2.9%** | **701** | the title screen's 2D layer only: logo, text. No scene. |

**So the scene renders and is not composed into the frame.** That is the whole defect the
operator sees as "the 3D render in main menu is just pure black", and it is one specific
question rather than a general "the renderer is wrong".

The post-processing chain downstream of the scene is alive too (`149DC000` 640x360 at
25.0%, and the whole 320x180 → 32x1 pyramid), so whatever consumes the scene is running;
it is the last link that does not reach the front buffer.

---

## Step 0 — FIRST: an instrument that can see a transform

Do this before any renderer change, because the last two defects say the current
instruments cannot adjudicate this work.

The Y flip and the texture swizzle were each found by a human looking at the screen for
a minute, and **neither was visible to any number this project computes** (gotcha 135). A
vertical flip preserves coverage, mean luminance, distinct-colour count and the whole
histogram exactly, so `tools/frame_compare.py` scores a flipped frame as identical to a
correct one.

The gap is that every metric here is an aggregate over pixel VALUES, and a transform
rearranges pixel POSITIONS. The cheap fix is a **spatial signature**: reduce the frame to
a small grid (8x8 or 16x16) of per-cell coverage and mean luminance, and compare that.

* A flip, mirror or rotation changes the grid and leaves the global aggregate untouched —
  exactly the blind spot.
* It can be compared against **capture E's screenshots**, which is the reference this
  phase has never used numerically. E2 is the title screen; its game area has to be
  located inside the windowed grab first (the E shots are different sizes — E1 1320x985,
  E2 1378x1125, E3 1401x1006 — so the crop is per-shot and must be found, not assumed).
* It stays honest about the animation: E3's background is animated, so compare E2 (the
  logo era, static) for layout and use the grid only for structure, not for exact values.

This is one tool, `tools/frame_signature.py`, and it is what makes every later step in
this plan adjudicable.

## Step 1 — why the scene never reaches the front buffer

One question, and the instruments already exist.

1. `CZ_VK_RESOLVE_TRACE=<frame>` at steady state gives the front-buffer pass its draw
   count. It already told us the whole story once (§6f).
2. In that pass, does any draw sample `06BE4000`? The counter
   `texture: served from a resolve snapshot` proves snapshots are being consumed
   (450,488 a run), but not by WHICH pass. Extend the resolve trace, or add a counter
   keyed on the fetched surface address, so the question is answered rather than inferred.

Two outcomes, and they are different repairs:

* **The compose draw exists and samples the scene.** Then the composite is being drawn
  and discarded — look at blend state, alpha, and the colour mask on that draw. The
  scene's surface is `8_8_8_8` while several intermediates are HDR formats, and the
  renderer has exactly one EDRAM format (§7), so a compose reading an HDR intermediate is
  a live candidate.
* **No draw in that pass samples it.** Then the compose reads an address our snapshot map
  does not hold — most likely because the surface it wants is one of the ones we resolve
  *from the wrong buffer* (§6d: a depth-only pass resolves our colour target), or because
  the base needs the same macro-tile normalisation §6f applies to the scene's own tiles.

## Step 2 — the remaining geometry distortion

**The earlier bisection is void and must be redone.** It was judged by eye on an animated
scene (§6o), and the Y flip has since changed every frame anyway.

Redo it with Step 0's signature as the judge and `CZ_VK_ONLY_VS` / `CZ_VK_SKIP_VS` as the
arms. What is already eliminated, each with a measurement in `phase5-notes.md`, and must
not be re-tested: the fetch-slot convention, both constant windows, the index endian
decode and width, the colour mask, culling, `VGT_INDX_OFFSET`, the `sges` idiom, and
primitive restart.

## Step 3 — depth resolves (§6d)

`RB_MODECONTROL` takes the value 5 (`kDepth`) as well as 4 and 6. Those passes resolve a
**depth** surface and our resolve unconditionally snapshots the **colour** target, so the
four shadow cascades — 111/90/224/38 draws each, 4096x1024 — resolve an empty colour
buffer. Shadows are part of what makes the scene read as a scene.

## Step 4 — size the EDRAM target from the surface

The cascades are 4096x1024 and our colour image is a fixed 1280x720, so their window is
clipped to the target (§7). The target should be sized from `RB_SURFACE_INFO` rather than
from the front buffer's dimensions.

---

## Order, and why

0 before everything: without it no later step can be judged, and two defects have
already been fixed by an operator's eyes because of that.

1 next, because it is the difference between "black menu" and "menu with a scene" and it
is a single question with two concrete repairs.

2, 3, 4 improve how the scene *looks* once it is visible, and 2 in particular is much
easier to judge against a background that is actually on screen.

## Standing rules for this work

* Every claim through `tools/frame_compare.py` **and** Step 0's signature. The median band
  is 1.36 pp; anything inside it is "no detectable effect", and the signature is what
  catches what the median cannot.
* Never conclude from one frame of the title screen — it is animated, and three runs of
  one binary give three unrelated pictures (§6o).
* The kernel gates keep passing: `--smoke`, A1's 84-deep prefix, A5 exit 0,
  `truncated=0`, both PM4 capture oracles. Run them, do not assume them.
