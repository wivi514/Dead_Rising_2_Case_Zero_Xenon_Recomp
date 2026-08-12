# Part 32 hand-off (for part 33). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part31-kickoff.md` for "where the
port is".

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through. Part 32 took
the item part 31 handed it — *"the shadow tail past the last cascade split"* — found that
was the wrong place, and located the real remaining shadow defect one layer up: **half of
every cascade band is zero, the geometry for it is submitted and rejected by a depth test
against a region nothing ever clears, and the cause is that a 4x MSAA surface is twice as
TALL in samples as well as twice as wide.** A one-line arm takes the atlas from 46.875%
zero to 0.0038%. It also **retracted the hardware oracle part 31's shadow work was
measured against**: the 16 MB dumped from the capture as "hardware's shadow atlas" is the
previous frame's composited scene, with the game's own HUD legible in it.

## WHAT PART 32 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6bf. Open item 3 is rewritten.

* **THE REMAINING SHADOW DEFECT IS MEASURED AND ITS MECHANISM IS DERIVED.** The atlas is
  **46.8750% zero in every band** — rows 0..511 populated across every column, rows
  512..1023 only in the last 64 columns — on two routes and two frames. 15/32 exactly, in
  four bands rendered from four different light frusta by 108/87/221/35 draws, so it is
  structural and not scene content.
  * `CZ_VK_DEPTH_ALWAYS=1` -> **1.86% zero**: the geometry IS submitted for the whole
    1024x1024 and the bottom half is REJECTED, against the zero the image was created with.
  * `CZ_VK_DEPTH_CLEAR_FAR=1` -> **0.0113% zero**: the input is the clear value. This
    title leaves `RB_DEPTH_CLEAR` at `00000000` for nearly every pass.
  * `CZ_VK_MSAA_WINDOW_SCALE_Y=1` -> **0.0038% zero**, title-screen picture unmoved
    (luma 29.08 -> 29.04, black 61.58% both, 19,172 -> 19,175 distinct colours). This is
    the same result reached by honouring the guest's own clear rects instead of ignoring
    a register, and it is the candidate FIX.
  * A registered prediction, **refuted**: scoping each clear to the pass's own region
    (`CZ_VK_SCOPED_CLEAR=1`) leaves the atlas at 46.8750% to four decimals.
* **THE DERIVATION, because it is what makes this a measurement.** `CZ_VK_RECT_TRACE=0`
  prints every rect-list clear with its surface pitch and MSAA mode. The cascade's two
  are `(0,0)-(480,512)` on a **520-pitch 4x** surface and `(960,0)-(1024,1024)` on the
  1040-pitch one. 520 x 2 = 1040 is the cascade's own sample pitch; Xenos 4x is a 2x2
  sample grid. Scale both axes and the two rects tile 1024x1024 **exactly**; scale X only
  and the union is 557,056 of 1,048,576 = **53.125%**, the observed coverage to four
  decimals. Part 15's *"those do not cover a 1024x1024 map and nothing this renderer does
  causes it"* is wrong on both halves.
* **THE RETRACTION: §6bc's hardware-side atlas measurement is an artifact.**
  `xtr_draw_bindings.py --dump-texture 1812F000` on `w1_spawn` returns 16 MB that is the
  previous frame's COMPOSITED SCENE — detile it and *"8 KILLED"* is legible across it.
  There is exactly ONE memory chunk covering that address, at walk position 39; the first
  resolve INTO it is at position 3522. **A capture cannot supply any surface the GPU
  produces inside the traced frame.** Gotcha 275's second half is corrected by gotcha 280.
  The address-fold FIX stands (it rests on register values and the operator's verdict);
  the yardstick does not. The target is 100%, not somebody's 96.5%.
* **`xtr_draw_bindings.py --dump-texture` now gates itself** — reports how many memory
  snapshots cover the range and when they arrived, says whether the address is a resolve
  destination, and **exits 2** when every snapshot predates the first resolve to it. The
  ordinary case prints *"a sound oracle"*. Checked both ways; **part 27's ground-texture
  comparison is unaffected** (`12389000` exits 0). Self-test 416/416.
* **Six instruments**, all off by default: the resolve trace now prints its DERIVED copy
  geometry and both clear values beside the registers; `RB_DEPTH_CLEAR` prints once per
  distinct value as `RB_COLOR_CLEAR` already did; `CZ_VK_DEPTH_ALWAYS`,
  `CZ_VK_DEPTH_CLEAR_FAR`, `CZ_VK_SCOPED_CLEAR`, `CZ_VK_RECT_TRACE`.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-31's lists stands. Part 32 adds two, in `docs/gotchas.md`:

* **279 — an arm whose FAILURE MODE is the symptom it was built to rule out will confirm
  the wrong answer, confidently.** `CZ_VK_NO_DEPTH_TEST=1` exists to separate "never
  submitted" from "submitted and rejected". Vulkan ties depth WRITES to the depth TEST, so
  on a DEPTH-ONLY pass it empties the buffer and reports 100% zero — the very symptom.
  Before believing an arm, ask what it would print if the defect were absent and what it
  would print if the arm were broken; if those are the same string, it cannot answer.
* **280 — a capture's memory records are SNAPSHOTS WITH A TIME.** Xenia dumps the bytes
  behind a resource the first time the GPU reads it and never again, so for any address
  the title resolves into during the traced frame the only snapshot predates the surface.
  A dense, plausible, wrong oracle is worse than none. And the corollary: when the oracle
  for an intermediate surface is gone, measure against the surface's own DEFINITION — a
  shadow map's unwritten region must read FAR, so the target is 100%.

## WHERE TO START

**0. RECONCILE THE SCENE TILE'S 4x CLEAR AND SHIP `CZ_VK_MSAA_WINDOW_SCALE_Y` AS THE
DEFAULT. Recommended first — the work is done and this is what is between it and the
picture.** The blocker is one table row: the cascade's 4x clear is `(0,0)-(480,512)` and
wants Y scaled; the scene tile's 4x clear is `(0,0)-(320,720)` on a tile 640 samples wide
and 720 rows tall, and appears not to. The likely reconciliation is that the scene's 720
is already a SAMPLE count where the cascade's 512 is a pixel count — i.e. the
discriminator is not the MSAA mode alone. `CZ_VK_RECT_TRACE=0` prints every rect with its
pitch and mode and is the whole input; the scene tile's surface height is the missing
term. **Do not ship it on the cascade result alone** — the scene tile clear is the pass
whose half-covered version cost part 9 a session.

1. **ASK THE OPERATOR FOR A THREE-WAY SHADOW VERDICT once 0 lands** — null,
   `CZ_VK_MSAA_WINDOW_SCALE_Y=1`, and `CZ_VK_NO_ADDR_TILE_FOLD=1` (the pre-part-31
   renderer), at ONE Case 0-2 crowd spot with the camera not moved between shots.
   **Read gotcha 278 first and name the property before looking**: with the fold alone the
   real shadow term reaches the third split at ~32 m and half of every cascade reads as
   occluded; with both, the whole map is real. The measurable is the EXTENT and the
   CONTINUITY of the shadowed region, not whether shadows exist.
2. **THE WHITE SURFACES, with a per-draw instrument** — unchanged from part 31's list and
   still the largest open item. Do not build another whole-frame arm; four of them in a
   row report "unmoved" (gotcha 276). `CZ_VK_DRAW_CENSUS` plus `CZ_VK_SKIP_TEX`, on a
   frame with a LARGE plateau. **The one thing to ask the operator for is a
   `CZ_CAPTURE_KEY` frame where the white ground is large** — headless outdoor frames
   carry 0.15-0.23%, their part-27 captures run to 15.36%.
3. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s CPU/GPU overlap work
   (gotcha 231), still the biggest performance term.

## An instrument this runtime still lacks, and part 32 wanted it twice

**What the EDRAM stand-in holds AT a given resolve, over its full 1280x1024 extent.** A
resolve snapshot shows only the window the copy takes, so "the cascade's bottom half is
zero" could be seen and "which pass last wrote those rows" could not. The resolve trace
now prints the derived copy geometry and both clear values, which is where it should grow.
It is the difference between knowing a pass started from the wrong state and knowing who
put it there.

## Gates, on this binary

* `--smoke` OK.
* `tools/xtr_draw_bindings.py --self-test`: **416 reproduce their filename, 0 do not**.
* Renderer runs on the outdoor DebugJump route complete and present normally in both arms.

**Not re-run and owed before any claim resting on them**: the A5 kernel-call diff, the
`CZ_RING_TRACE` `truncated=0` gate, the PM4 capture oracles, `shader_dim_census.py`, the
capture-E picture correlation and the shader-cache name diff. Part 32 touched the
renderer's clear path and its window-coordinate mapping behind arms that are off by
default, so the recorded baselines should still hold — but they were not re-measured.
