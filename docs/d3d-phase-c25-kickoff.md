# D3D phase C, part 25 hand-off (for part 26). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c24-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session three times now.

## The one-paragraph state of the port

The game boots, renders and plays. A headless run reaches the title, the menus, the
prologue and live gameplay with no operator, and the outdoor recipe reaches a crowd at
6,000-9,200 draws a frame. Ordinary gameplay is ~30-31 fps and that is the title's own
two-vblank pacing floor, not our ceiling. The CPU and GPU overlap (part 23). The HUD /
ammo defect is fixed (part 24). **Part 25 built open item 00 — cube maps are bound — and
then spent most of its length discovering that the headless harness cannot see the
result.** That is the real hand-off: the code is done and verified two-sided; the picture
question is open and belongs to the operator.

## READ THIS BEFORE MEASURING ANYTHING

* **RUN THE NULL ARM FIRST, IN THE SAME SERIAL BLOCK, AND QUOTE EVERY EFFECT AS A MULTIPLE
  OF IT.** Part 25 made this error three times in three disguises — a count with no
  denominator (gotcha 246), a positive control read with a statistic that could not see it
  (248), and an effect quoted with no null at all (249). "82 of 109 frames differ" is not a
  result: **two runs of the SAME configuration differ on 82 of 109 frames.**
* **Picture A/Bs need the fingerprint admissibility filter, and you must quote how many
  frames survive it.** Two arms are comparable only where `drawFingerprint` AND
  `cameraFingerprint` agree. On this title's recipes that is 13-44 frames of ~300, and
  **every one of them is under 1,800 draws** — so the outdoor era is currently unmeasurable
  and any headless claim about it is invalid (gotcha 247, `docs/measurement.md`).
* **`frame_matched_diff.py`'s pooled headline can invert its own per-pair lines.** Read the
  pairs.
* **The frame time is PINNED at two vblanks (~32 ms)** for everything reachable, so a CPU
  saving does not show as frame rate and neither does a CPU cost (237, 243). Quote
  `outside`.
* **Every phase in `CZ_VK_PROFILE` is EXCLUSIVE of nested ones** as of part 20 (228).
* **Do not pin the GPU clock**; sample it with `tools/gpu_clock_sample.py` (219, retracted
  in part).
* **Three runs an arm on any crowd frame-time claim**; the floor is 10-13% (229).
* **THE VULKAN VALIDATION LAYER IS NOT INSTALLED ON THIS MACHINE.** Every log here says
  `VK_LAYER_KHRONOS_validation is NOT INSTALLED`, so grepping any of them for `VUID`
  returns zero for the reason gotcha 25 exists. `sudo dnf install vulkan-validation-layers`
  is the cheapest outstanding safety net this renderer has — part 25 found two real layout
  bugs by reading, and validation would have caught both instantly.
* **SERIALISE BACKGROUND RUNS THROUGH ONE JOB.** Several jobs each waiting on
  `until ! pgrep cz_runtime` all wake together and run concurrently, silently contaminating
  every depth and timing claim built on them. And an operator can open the game at any
  moment: part 25 discarded a drift baseline for exactly that.

## Where part 26 starts, in order

1. **Item 00's remaining half, and it is the LARGER half by volume: the cube snapshot
   path.** `06805000` (64x64 `k_8_8_8_8`) is a cube map the title RENDERS ITSELF — a
   resolve destination whose pixels never reach guest memory (`uploaded BLACK, guest memory
   STILL zero`). It is **55% of all cube sampling in the opening hour** — 409,911 of
   746,355 cube-declared draws — and it is declined to the white dummy today, so it is
   white in both arms of every A/B. The fix is six resolves into six layers of one cube
   image registered in set 2. Everything needed to recognise the case already exists and is
   counted; `CZ_VK_CUBE_FROM_GUEST=1` is the arm that keeps the old zeros.
2. **The operator's verdict on the other 45%.** Reflective surfaces, same spot, twice:
   `CZ_VKDRAW=1 ./cz_runtime` and `CZ_VK_NO_CUBE=1 CZ_VKDRAW=1 ./cz_runtime`. This is not a
   preference for human evidence — the harness has been MEASURED blind to a change of this
   size (0.038-0.085 median against a 0.069 drift floor, where a positive control reads
   0.401). **Put `CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps` on that run** — never
   under `/tmp`, which is a tmpfs and is why eleven cache entries have no microcode left.
3. **A harness that can reach an admissible outdoor frame.** Every drift-honest filter
   throws away everything above ~1,800 draws. Until that changes, no headless picture claim
   about reflections, shadows, or anything outdoors is possible. This blocks items 3, 4 and
   6 as much as it blocks item 00.
4. **The eleven sidecars with no `tfetchDims`** — `tools/shader_dim_census.py` names them;
   one samples a cube map and is therefore still unbound. A run that loads one recovers it.
5. **The binned frame-time A/B still owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23). Read
   the MEDIAN and the vblank-pinned share, not the mean (237).
6. The rest of `docs/open-items.md`: shadow cascade (3), mipmaps (4), colour (6), item 12.

**Still deliberately NOT planned: giving `CZ_FAKE_PRESS_SEQ` a trigger.** The button is the
easy half; a recipe would still have to ACQUIRE a gun and ammo along a long scripted path.
Propose the acquisition first.

## What part 25 delivered

* **`tfetchDims` in the shader sidecar** — the per-slot texture dimension, from bits 14..15
  of the fetch instruction's third word. Per SLOT, not per module.
* **`tools/shader_dim_census.py`, a TWO-SIDED gate.** The dimension is derivable twice
  independently — our ucode parse, and DXC's `OpDecorate ... DescriptorSet` words — and over
  the rebuilt cache the two agree on every shader: 298 modules / 973 slots 2D, 92 modules /
  92 slots cube, zero 1D and zero 3D. Shown capable of failing by moving the parse one bit.
* **Cube maps upload as six faces** at a stride of one face's tiled footprint, into a
  `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2 out of its own slot space, with
  `kSharedTexCube[constIdx]` published. `CZ_VK_NO_CUBE=1` is the control arm.
* **The fetch constant's dimension field, located by CENSUS rather than recollection** —
  which was wrong (`CZ_VK_DIM_CENSUS=1`; dword5 bits 9..10, cross-checked against dword2's
  stack depth reading 5 for every cube fetch). Both sources are compared on every fetch now.
* **`CZ_VK_CUBE_POISON=1`, a positive control that is POSITIVE**: 80 of 110 frames change,
  worst frame 72% of pixels. The cube sample reaches the presented image.
* **Three latent instrument defects fixed**, each of which would have produced a confident
  wrong answer: `Barrier`'s hardcoded `layerCount = 1` (already live in `R->dummyCube` since
  phase 5), the dummy upload writing 4 bytes for a 6-layer copy, and `CZ_SHADER_DUMP`
  failing silently into a directory that did not exist.
* The shader cache is **397**, up from 394.

## Gates, on the part-25 binary

`--smoke` OK. `tools/shader_dim_census.py` exit 0, zero disagreements. **Not re-run this
part and owed before any claim that rests on them**: the A5 kernel-call diff, `truncated=0`,
the PM4 capture oracles, and the capture-E picture correlation.

## The method notes worth carrying

* **Gotcha 244 — a field another oracle can PREDICT should be located by census, not by
  recollection.** Two independent descriptions of one fact make each a free oracle for
  decoding the other. One counter and one report; use it by default.
* **Gotcha 245 — a struct field constant everywhere is a defect waiting for its first
  exception**, and it arrives as undefined behaviour rather than a wrong picture.
* **Gotchas 246 / 248 / 249 are one error in three disguises**: a count with no denominator,
  a control read with a statistic that cannot see it, and an effect quoted with no null.
  The mechanical fix covers all three — **measure the arm against ITSELF first, in the same
  block, and quote ratios.**
* **Gotcha 247 — an A/B whose admissible n is not stated is not an A/B.**
* **When a harness is shown blind, say so and hand the question over.** Part 25's most
  useful output is not the cube path; it is the measurement that says this project cannot
  currently adjudicate an outdoor picture change without an operator.
