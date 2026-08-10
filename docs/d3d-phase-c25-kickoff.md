# D3D phase C, part 25 hand-off (for part 26). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c24-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session three times now.

## The one-paragraph state of the port

The game boots, renders and plays. A headless run reaches the title, the menus, the
prologue and live gameplay with no operator, and the outdoor recipe reaches a crowd at
6,000-8,700 draws a frame. Ordinary gameplay is ~30-31 fps and that is the title's own
two-vblank pacing floor, not our ceiling. The CPU and GPU overlap (part 23). The HUD /
ammo defect is fixed (part 24). **Part 25 built open item 00: cube maps are bound.** 92 of
the cache's 397 shaders sample `TextureCube[]` and every one of them had read the 1x1
dummy on every draw since phase 5; they now read a real six-face cube map out of set 2.
**What that item still owes is the OPERATOR'S VERDICT on the picture** — it is a
reflection defect and the surfaces it should change are ones only they have named.

## READ THIS BEFORE MEASURING ANYTHING

* **The frame time is PINNED at two vblanks (~32 ms) for everything this port can
  currently reach**, including a 6,778-draw crowd, so a CPU saving does not show up as
  frame rate and neither does a CPU cost (gotchas 237 and 243). Quote `outside` (the
  headroom) alongside the phase percentages.
* **Every phase in `CZ_VK_PROFILE` is EXCLUSIVE of the ones nested inside it** as of part
  20; earlier numbers overstate `record` by the whole of `streams` (gotcha 228).
* **Do not pin the GPU clock** — sample it with `tools/gpu_clock_sample.py` (gotcha 219,
  retracted in part).
* **Three runs an arm on any crowd frame-time claim**; the noise floor is 10-13% at one
  run a side (gotcha 229). Run the null comparison first.
* **THE VULKAN VALIDATION LAYER IS NOT INSTALLED ON THIS MACHINE.** Every log in this repo
  says `VK_LAYER_KHRONOS_validation is NOT INSTALLED`, so grepping any of them for `VUID`
  returns zero for the reason gotcha 25 exists — a grep that cannot match is not a clean
  result. `sudo dnf install vulkan-validation-layers` is the cheapest outstanding safety
  net this renderer has, and part 25 found a real layout bug by reading rather than by
  symptom that validation would have caught instantly (gotcha 245).

## Where part 26 starts, in order

1. **Item 00's remaining half: get the operator to look.** The cube-map path is built and
   headlessly exercised; what no instrument here can answer is whether the "unicorn
   colour" filing cabinet, the dumpster and the reflective surfaces are right now.
   `CZ_VK_NO_CUBE=1` is the same-binary control arm — ask for a shot of the same spot
   with and without it.
2. **`06805000` is a cube map at a resolve destination**, and it is the only one.
   A resolve's pixels never reach guest memory, so if the title renders that cube
   dynamically we are feeding it zeros. The measurement is one row of `CZ_VK_TEX_CENSUS`;
   the fix, if needed, is a cube snapshot path (six resolves into six layers), not
   anything in the dimension decode.
3. **The eleven sidecars with no `tfetchDims`** — cache entries whose microcode is gone,
   one of which samples a cube map and is therefore still unbound. `tools/shader_dim_census.py`
   names them. Any run that might load one should carry `CZ_SHADER_DUMP`, and **the dump
   directory must not be under /tmp**, which is a tmpfs; use
   `~/DR2CZ-troubleshooting/ucode-dumps`.
4. **The binned frame-time A/B still owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23). Use
   `tools/frame_perf_bins.py` and read the MEDIAN and the vblank-pinned share, not the
   mean (gotcha 237).
5. The rest of `docs/open-items.md` in order: shadow cascade (3), mipmaps (4), colour (6),
   and item 12 (keyboard-as-user-2 fallback, low priority).

**Still deliberately NOT planned: giving `CZ_FAKE_PRESS_SEQ` a trigger.** Its vocabulary
has no RT/LT and attack here is RT, but the button is the easy half — a recipe would still
have to ACQUIRE a gun and ammo along a long scripted path. Propose the acquisition first.

## What part 25 delivered

* **`tfetchDims` in the shader sidecar** — the per-slot texture dimension, from bits 14..15
  of the fetch instruction's third word. Per SLOT, not per module: one pixel shader here
  samples slot 3 as a cube and slots 0 and 2 as 2D in three consecutive instructions.
* **`tools/shader_dim_census.py`, a TWO-SIDED gate.** The dimension is derivable twice
  independently — our ucode parse, and DXC's `OpDecorate ... DescriptorSet` words — and
  over the rebuilt cache the two agree on every shader: 298 modules / 973 slots 2D,
  92 modules / 92 slots cube, zero 1D and zero 3D. Shown capable of failing by moving the
  parse one bit.
* **Cube maps upload as six faces** at a stride of one face's tiled footprint, into a
  `VK_IMAGE_VIEW_TYPE_CUBE` view registered in set 2 out of its own slot space, with
  `kSharedTexCube[constIdx]` published. `CZ_VK_NO_CUBE=1` is the control arm.
* **The fetch constant's dimension field, located by CENSUS rather than recollection** —
  which was wrong. `CZ_VK_DIM_CENSUS=1`; dword5 bits 9..10, cross-checked against dword2's
  stack depth reading 5 for every cube fetch and 0 for every 2D one. Both sources are now
  compared on every fetch; they disagree on 114 of 337,716 (0.03%), which are declined and
  counted.
* **`Barrier`'s hardcoded `layerCount = 1`, fixed** — already live in `R->dummyCube` since
  phase 5, so five of the dummy's faces were written and sampled in `UNDEFINED`.
* The shader cache is **397**, up from 394.

## Gates, on the part-25 binary

`--smoke` OK. `tools/shader_dim_census.py` exit 0, zero disagreements. **Not re-run this
part and owed before any claim that rests on them**: the A5 kernel-call diff,
`truncated=0`, the PM4 capture oracles, and the capture-E picture correlation.

## The method notes worth carrying

* **Gotcha 244 — a field another oracle can PREDICT should be located by census, not by
  recollection.** Two independent descriptions of the same fact make each a free oracle
  for decoding the other, and the decode becomes a measurement with a stated refutation
  instead of a remembered constant. One counter and one report; use it by default.
* **Gotcha 245 — a struct field that is constant everywhere is a defect waiting for the
  first exception**, and the exception arrives as undefined behaviour rather than as a
  wrong picture. When adding the first instance of a shape a helper has never seen, read
  the helper for what it assumed.
* **Serialise background runs through ONE job, not several polling the same condition.**
  Three queued jobs each waiting on `until ! pgrep cz_runtime` all wake together and run
  concurrently, which silently contaminates every timing and depth claim built on them.
