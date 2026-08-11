# Part 31 hand-off (for part 32). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part30-kickoff.md` for "where the
port is".

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through. Part 31
finished the comparison part 30 left owed, **fixed a real renderer defect that had been
open since part 15**, and then **retired the model the white-surface item has been
prosecuted under since part 27**. It did not fix the white surfaces either. What it did
was rule out, by measurement, every remaining input to the pass everyone has been looking
at — which is worth more than another candidate, because it says the search has been in
the wrong place and names the kind of instrument that can find the right one.

## WHAT PART 31 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6bb, §6bc, §6bd, §6be.

* **THE 23 UNCHECKED CONSTANTS ARE CHECKED, AND THE CONSTANTS ARE EXONERATED AS A CLASS.**
  All seven R2 captures answer for the ground shader (part 27 recorded that `w1_spawn`
  could not; it cannot supply `c253..c255`, and supplies the other 29). **Every register
  the ground draw reads that is not a function of the camera is hardware's to the printed
  digit** — including `pc(21)`, a point light's WORLD POSITION, at
  `(-149.4081, 6.2343, -106.2974, 0.7400)` against hardware's
  `(-149.408096, 6.234319, -106.297379, 0.740000)`. That match was not planned and it is
  better evidence of a matched lighting state than the route could have been.
  The registered prediction (`c20`/`c24`/`c67` disagree) is refuted.
* **THE SHADOW ATLAS DEFECT IS FOUND, FIXED AND CONTROL-ARMED — `open-items.md` item 3.**
  The title packs four 1024x1024 cascades into one 4096x1024 atlas by pre-offsetting
  `RB_COPY_DEST_BASE` by 0x20000 each while leaving the window scissor at the origin;
  0x20000 is exactly +1024 texels in X in Xenos tiled address space. `DoResolve`
  un-offset the SCISSOR form of that idiom and not the ADDRESS form, so the four became
  four disjoint snapshots and a fetch of the base read zero past column 1023. **Ours was
  86.7% zero; hardware's copy of the same surface, dumped out of the capture, is 3.5%
  zero with all four bands populated.** Fixed: 53.125% non-zero across all 4,096 columns,
  one atlas, 17,355 folds, against 13.281% / four atlases / zero folds with
  `CZ_VK_NO_ADDR_TILE_FOLD=1`. `13.281% x 4 = 53.125%` exactly. Costs no frames.
* **THE WHITE PLATEAU IS NOT THE TONE CURVE AT `x = 1`.** Four whole-frame arms leave the
  pixels at exactly `rgb(180,180,180)` untouched: the sun `c24`, the additive `c67.w`
  term, the whole multiplicative path `c1.xyz` (which blacks out **61.5%** of the frame),
  and — decisively — `14.w=0.25`, which engaged on 11,835,619 draws, took the scene
  buffer from mean luma **35.07 to 18.30**, and put **zero** pixels on 119, where that
  curve sends `x = 1` at quartered exposure. A value produced by that curve cannot be
  invariant under scaling its exposure. **`180 = 255 * sqrt(0.5)` is now the coincidence
  to explain, not the explanation.**
* **The plateau itself is confirmed, and on DAYLIGHT this time.** Part 30 withdrew the
  frame-maximum argument because its seven frames were the operator's night captures.
  Frame 3000 of the outdoor route has mean luma 36.3 and 63,398 distinct colours, and
  still: 1,348 px at exactly 180, **zero** at grey 181, 182 or 183, and only 45 px of
  921,600 above 180 in all three channels (26 of them pure 255).
* **Three new instruments/tools**: `CZ_VK_EXPOSURE_TRACE` (per frame, the spread of
  `pc(14).w`; frame 3000 is 6,116 draws all within 0.214622..0.214647, so ONE exposure is
  in force per frame — assumed for four parts, now measured), `CZ_VK_NO_ADDR_TILE_FOLD`,
  and `tools/snap_plateau.py` (separates a pinned value from a distribution the curve
  compressed — the distinction the item turns on, which `snap_dump_stats.py` cannot make).
  `psbindLine` also went 2048 -> 8192 and now says `(PC LIST TRUNCATED)`.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-30's lists stands. Part 31 adds three, in `docs/gotchas.md`:

* **275 — a resolve destination can name a sub-region by its ADDRESS instead of by its
  scissor**, and a renderer that understands one of the two loses the other silently: the
  snapshot is the right size, populated and served, and is a quarter of the picture. Also
  the other half: **a surface you RENDER is still comparable**, because the capture
  carries the consumer's copy of it and `xtr_resolve_census.py` prints the title's own
  destinations. That check took ten minutes and had been available since part 26.
* **276 — when three or four successive arms all report "unmoved", the instrument CLASS
  is the finding.** Stop perturbing inputs. Each result reads as "not this one, try the
  next"; the four together say the pixels are not downstream of any input to that pass.
* **277 — an arithmetic coincidence that fits to three digits keeps its status as a
  hypothesis until a mechanism is measured, not until a better coincidence turns up.**
  Two independent derivations of 180 (part 27's gamma, part 30's trailing `sqrt`) still
  were not evidence that the pixels came from there. The cheap question that would have
  caught it four parts earlier: *what would move these pixels, and does it?*

## WHERE TO START

1. **THE WHITE SURFACES, with a per-draw instrument. Do not build another whole-frame
   arm — the four above are the argument that it cannot answer.** The question is now
   simply *which draw paints the pixels that end at exactly 180*, and the population is
   enumerable with what already exists:
   `CZ_VK_DRAW_CENSUS` lists every draw of one frame with its shader, bindings and
   constants; `CZ_VK_SKIP_TEX`/`CZ_VK_ONLY_TEX` remove a texture's draws and let the
   picture be diffed. Take a frame with a LARGE plateau (part 27's operator captures run
   to 15.36%; the headless outdoor route gave only 0.15-0.23% at frame 3000, so **find a
   fuller frame first** — the gas-station forecourt and the spawn area are the named
   ones). One caution from §6bd: a plateau count compared across two RUNS is not a
   measurement (gotcha 254); within one frame, "these pixels vanished when that draw did"
   is sound.
2. ~~**ASK THE OPERATOR WHETHER SHADOWS APPEAR NOW.**~~ **ASKED AND ANSWERED THE SAME
   DAY — the fix reaches the picture, and a SECOND defect is now the item.** Four operator
   screenshots at one Case 0-2 crowd spot, both arms of one binary, in
   `~/DR2CZ-troubleshooting/part31/operator-shadow-ab/`. Fold ON: *"much wider — the spot
   where shadows are is actually in front of the camera"*, and *"still way far from
   intended behaviour, but much better."* Fold OFF: a smaller lit patch that jumps around
   the frame.
   **The remaining defect is beyond the last cascade split, and it is well posed.** The
   split distances are `pc(46) = (8, 12, 32, 7)`; the fix takes the real shadow term from
   ~8 m to ~32 m, which is the improvement. Past the last split, line 81's
   `mul_sat r3.w, r3.w, c44.w` (with `pc(44) = (0.000244, 0.000977, 18, 0.071429)`) and
   the `tf5` term are supposed to fade the surface back to FULLY LIT. Work out what ours
   does there — that is a different question from the atlas, and `c40..c42` plus `tf5`
   are the inputs.
   It does NOT change the white surfaces (2,074 px at 180 before and after), and the sign
   says why — a zero depth sample reads as OCCLUDED.
   **Read gotcha 278 before judging the next shadow change**: this session called the fix
   a null on two control-arm screenshots, because both arms show a camera-dependent lit
   region and only its EXTENT separates them.
3. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s largest item — the
   CPU/GPU overlap work (gotcha 231), still the biggest performance term.

## Gates, on this binary

Run and clean:

* `--smoke` OK.
* `CZ_RING_TRACE=1` boot: **`truncated=0`**.
* A5 kernel-call diff with `--include-high-frequency`: **exit 0, 3 permutation windows,
  0 real** — the recorded baseline.
* `tools/shader_dim_census.py`: **exit 0**, 97 cube modules, `ps_926c15dd20571cf1` still
  the only sidecar without `tfetchDims`.
* No new Vulkan validation messages on a 260 s renderer run.
* Six renderer runs presented 5,883-6,206 frames at 7,315-7,403 peak draws, and the
  fold arm is within that band both ways, so the renderer is healthy on the outdoor route.

* Both PM4 capture oracles on B1: `pm4_packet_lengths.py` **exit 0** (0 disagreeing) and
  `pm4_indirect_walks.py` **exit 0**.

**Not re-run and owed before any claim resting on them**: the capture-E picture
correlation and the shader-cache name diff.
