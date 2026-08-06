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

## Step 0 — DONE: `tools/frame_signature.py`

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

**Built and verified.** `tools/frame_signature.py --ref <E screenshot> <frame.ppm>`
crops both to the game area (the largest centred 16:9 rect), reduces to a 32x18 grid of
z-scored luminance, and correlates our frame against the reference under each candidate
transform — identity, flip-vertical, flip-horizontal, rotate-180 — reporting which fits
and exiting 1 when it is not identity.

Three cases, and the third is the one that made it trustworthy:

| case | result |
|---|---|
| the fixed frame vs E2 | `identity` at correlation **+0.947**, gap 0.124 — LAYOUT AGREES |
| `CZ_VK_NO_FLIP_Y=1` vs E2 | **`flip-vertical`** at +0.951, gap 0.166 — TRANSFORMED |
| title screen vs E1 (ESRB card) | **NO MATCH** — best correlates only +0.576 |

Two design mistakes are recorded in the file itself because both produced confident
wrong answers:

* **Cropping to the non-black bounding box** rather than to the game area. E's shots are
  mostly black, so the bbox finds the logo instead of the screen and the two images get
  squashed to the grid by different amounts. Fixing it took the correlation from 0.067
  to 0.947. The game area's geometry is checkable on E4, the one shot bright enough to
  reveal it: content 1384x785, aspect 1.763, full width and letterboxed about the centre.
* **Judging confidence as a fraction of the candidate spread.** On the negative control
  that reported "TRANSFORMED: flip-horizontal, 52% of the spread" about two unrelated
  images, because four nearly equal correlations make a tiny gap a large fraction of a
  tiny spread. Now gated on an absolute correlation floor AND an absolute gap.

Also imported from Fable 2: `tools/frame_matched_diff.py`, which measures the within-arm
noise floor from the same runs at the same time rather than quoting a constant band —
strictly better than `frame_compare.py` whenever there are two or more runs per arm.

## Step 1 — DONE: the compose exists; the POST CHAIN is what is black

One question, and the instruments already exist.

1. `CZ_VK_RESOLVE_TRACE=<frame>` at steady state gives the front-buffer pass its draw
   count. It already told us the whole story once (§6f).
2. In that pass, does any draw sample `06BE4000`? The counter
   `texture: served from a resolve snapshot` proves snapshots are being consumed
   (450,488 a run), but not by WHICH pass. Extend the resolve trace, or add a counter
   keyed on the fetched surface address, so the question is answered rather than inferred.

**ANSWER: outcome one. The compose exists and samples the scene.** The instrument is a
per-pass record of which resolve snapshots the pass's draws sampled, printed in the
resolve trace — the existing global counter proved snapshots are consumed but never by
WHICH pass, and that was the whole question.

That turns the frame into a dependency graph, which is the real deliverable of this step:

```
writes     extent        draws   reads
1439B000   4096x1024       111   (none)          shadow cascades
143BB000   4096x1024        90   (none)
143DB000   4096x1024       224   (none)
143FB000   4096x1024        38   (none)
06BE4000   1280x720        919   1439B000        THE SCENE  (tile 0)
06BF8000   1280x720        102   1439B000        THE SCENE  (tile 1)
149DC000    640x360         25   0684B000        works
14733000    320x180          1   0684B000        <- FIRST BROKEN LINK
1476F000    320x180          1   14733000
147AB000    160x90           1   1476F000
147BA000     96x45           1   147AB000
1439B000   1280x720          1   0684B000 1476F000 147AB000 147BA000   bloom composite
147C0000    640x360          1   1439B000 06BE4000
149A0000    640x360          1   06BE4000
00E48000   1280x720         59   1439B000 06BE4000 147C0000 149A0000   THE FRAME
```

And the state of each, measured in the same run:

| surface | coverage | reading |
|---|---|---|
| `06BE4000` / `0684B000` the scene | **73.74%** | fine |
| `149DC000` (25 draws) | **73.44%** | fine |
| `14733000`, `1476F000`, `147AB000`, `147BA000` | **0.00%** | the bloom chain, dead from its first link |
| `1439B000` bloom composite | **0.00%** | dead because its inputs are |
| `147C0000`, `149A0000` | **0.00%** | dead |
| `00E48000` the frame | 2.31% | the 2D layer only |

**The pattern is exact: every pass of ONE full-screen quad renders nothing; every
multi-draw pass works.**

### It is not the geometry — that was tested, not assumed

Every state on those draws is permissive and correct: unit-quad vertex data
`(0,0)(0,1)(1,0)(1,1)`, a matrix mapping `[0,1]` to clip, correct viewport and scissor,
depth compare **ALWAYS**, blend ONE/ZERO, colour mask `F`, and a texture served from a
populated snapshot.

The decisive test was to hand-patch one blur pixel shader's cache entry to output solid
red (`CZ_SHADER_SPV` makes that a five-minute experiment). Its targets went **0.00% ->
100.00% red**. So the quad rasterises and writes; the pass is black because **the pixel
shader outputs zero**.

### Where it starts

`1476F000` is correctly black: it is a 16-tap Gaussian blur of `14733000`, which is
black. Walking back, the first link that is black from a good input is:

    14733000 (0.00%)  <-  ps=279d45e49b6e68c3  <-  0684B000 (73.74%)

`shader_693CE7FC3E81A471` in the capture: a 4-tap bright-pass that sums four
`tfetch2D tf0` at offsets from `pc(118)`/`pc(119)`, then
`max(sum * c255.w + c255.z, c255.x) * c255.y`.

Its constants are populated and non-degenerate at steady state — `pc(255) =
(0.0721, 0.2125, 0.7154, 0.5000)`, `pc(118)/pc(119)` small texel offsets — so the
arithmetic **cannot** produce zero: the `max` floor alone forces at least
`0.0721 * 0.2125` ≈ 4/255. The surface is exactly 0.

So the remaining question is narrow: the shader's *sampled value* must be zero, which
means `tf0` is not delivering the snapshot at draw time. The candidates are the
bindless descriptor index we publish for that slot, the sampler slot (we write index 0
unconditionally), and the interpolated texcoord the taps are offset from.

### A measurement trap this step walked into

An earlier pass of this investigation reported "every pixel-shader constant is zero" and
was **wrong**. `CZ_VK_DRAW_PROBE` fires on a shader's first three draws *ever* — which
happen during the BOOT, before the guest has uploaded the constants that shader will
use. Watching the register itself (`CZ_PM4_CONST_WATCH`) showed it takes no zero writes
at all after frame 400. `CZ_VK_DRAW_PROBE_MINFRAME` now bounds the probe to a
steady-state frame; the same reading then shows a perfect symmetric Gaussian kernel.

Same shape as the earlier `MINVERTS` trap: **a probe that samples the first occurrence
samples the boot**, and the boot is not the state under investigation.

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
