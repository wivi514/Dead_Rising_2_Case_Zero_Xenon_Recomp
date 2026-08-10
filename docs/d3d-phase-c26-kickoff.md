# D3D phase C, part 26 hand-off (for part 27). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c25-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session three times now.

## The one-paragraph state of the port

The game boots, renders and plays. Ordinary gameplay is ~30-31 fps and that is the title's
own two-vblank pacing floor, not our ceiling. The CPU and GPU overlap (part 23). The HUD /
ammo defect is fixed (part 24). Cube maps are bound (part 25) **and the one the title
RENDERS ITSELF is now assembled from its six resolve snapshots (part 26)** — 35.9% of all
cube fetches on the outdoor route, every one of which read a 1x1 white dummy before.

**Part 26's other result is a measurement discipline one, and it is the more important
half.** The matched-frame admissibility filter — two arms are comparable only where
`drawFingerprint` and `cameraFingerprint` agree — **cannot be satisfied outdoors, and the
route is not why.** Two runs of ONE configuration share 0 of 12,174 outdoor frames. The
route is fine (93% of its frames are outdoors, and two runs' draw counts agree to 1.4%);
a crowd of animated actors simply never renders the same draw list twice. **The replacement
is an era aggregate with a measured null** (`tools/frame_era_medians.py`, 0.94% on median
mean-luma, 0.76% on median distinct colours), and that is what unblocks items 00, 3, 4 and 6.

Three of the validation layer's five defects are closed, and the layer now names our
objects.

## READ THIS BEFORE MEASURING ANYTHING

* **RUN THE NULL ARM FIRST, IN THE SAME SERIAL BLOCK, AND QUOTE EVERY EFFECT AS A MULTIPLE
  OF IT.** Part 25 made this error three times in three disguises (gotchas 246, 248, 249);
  part 26 mechanised the fix into `tools/frame_era_medians.py`, which takes the null as a
  required argument.
* **DO NOT use matched-frame picture comparison outdoors.** Its n is zero and it always
  was (gotcha 254). `tools/frame_determinism.py` is the check, and it takes seconds on
  stats files you already have. Indoors the filter still works and still yields 13-44
  frames under 1,800 draws.
* **The frame time is PINNED at two vblanks (~32 ms)** for everything reachable, so a CPU
  saving does not show as frame rate and neither does a CPU cost (237, 243). Quote
  `outside`.
* **Every phase in `CZ_VK_PROFILE` is EXCLUSIVE of nested ones** as of part 20 (228).
* **Do not pin the GPU clock**; sample it with `tools/gpu_clock_sample.py` (219, retracted
  in part).
* **Three runs an arm on any crowd frame-time claim**; the floor is 10-13% (229).
* **`CZ_VK_VALIDATION=1` on at least one run per session, and quote the tally.** As of
  part 26 it is **20 `VkGraphicsPipelineCreateInfo-Input-08733` + 6
  `VkGraphicsPipelineCreateInfo-topology-08773`, and nothing else** — down from five VUIDs
  and 64 messages. The layer now brings `VK_EXT_debug_utils` with it and every image we
  create is NAMED, so a message reads `[resolve snapshot 14A7A000 96x45 slot 32]` rather
  than a handle. That is what made 09600 diagnosable (gotcha 255) — name a new object type
  the moment you add one.
* **SERIALISE BACKGROUND RUNS THROUGH ONE JOB.** Several jobs each waiting on
  `until ! pgrep cz_runtime` all wake together and silently contaminate every depth and
  timing claim built on them. An operator can also open the game at any moment.

## Where part 27 starts, in order

1. **THE OPERATOR'S VERDICT ON THE CUBE MAPS, and it is now a THREE-way question.** Same
   spot, outdoors, reflective surfaces (a car bonnet, a shop window, the gas-station
   forecourt), three configs on the same binary:
   ```
   CZ_VKDRAW=1 ./cz_runtime                            # the rendered cube map
   CZ_VK_NO_CUBE_SNAPSHOT=1 CZ_VKDRAW=1 ./cz_runtime   # part 25: that map is white
   CZ_VK_NO_CUBE=1 CZ_VKDRAW=1 ./cz_runtime            # pre-part-25: no cube map at all
   ```
   **Know what the headless answer is before asking.** Six runs, era medians over ~12,170
   outdoor frames each: default 56.593 / 56.907 / 56.738 median mean-luma, no-snapshot
   56.291 / 57.086, **no-cube-at-all 59.469**. So removing EVERY cube map is 8x the
   baseline band with no overlap — the instrument is sensitive outdoors — while removing
   only the rendered map does not separate at all. **Ask the operator about SURFACES, not
   the frame**: 35.9% of cube fetches is not 35.9% of the picture (gotcha 257), and a
   whole-frame median cannot see one 64x64 environment map on scattered reflective patches.
   Put `CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps` on the run — never under `/tmp`.
2. **The other five cube maps are LOADED, and one of them uploads BLACK.** `01330000`
   (4x4): `uploaded BLACK, guest memory is NON-ZERO NOW`, i.e. the texture arrived after
   our one and only upload and the fetch-constant cache froze it black. That is a
   different defect from the rendered one and it is small and well specified —
   `CZ_VK_TEX_REFRESH=01330000` is the arm that says whether re-reading fixes it.
3. **SOUND — the game is silent, the operator has asked for it, and part 26 settled the
   first question.** The guest hands us REAL BUFFERS FULL OF ZEROS: `null=0`,
   `non-silent=0`, `maxpeak=0.000000` over every frame of a run to gameplay, with the
   scanner self-testing at 0.5000 on a synthetic frame. So the silence is upstream of the
   mixer and **the next step is XMA DECODE, not an output device**. Fable 2's
   `audio/xma_hw.cpp` (430 lines, the hardware register contract) and `audio/xma_decoder.cpp`
   (192, ffmpeg) are directly liftable, and its `docs/audio-xma.md` is titled "why nothing
   the game mixed was audible". Output last (`audio/audio_out.cpp`, 175 lines, same frame
   format: 6 planes of 256 planar big-endian float32). **Its timer trap applies to us
   verbatim**: our pump is `sleep_for(5333us)`, which Fable 2 measured as ~184 frames/s
   against the 187.5 that 48 kHz needs — a ~2% deficit that starves the device into a
   stutter easily mistaken for a decode bug. Full item: `docs/open-items.md` 00e.
4. **The two remaining validation defects, and MEASURE before changing either.**
   `Input-08733` (20) is a vertex attribute at Location 15 declared `R32_UINT` under a
   shader input that is a float `vec4` — this is very likely the deliberate
   `USCALED`/`SSCALED` decision seen from the layer's side, so read it against gotcha 122
   and `CLAUDE.md`'s shared-decode table first. `topology-08773` (6) is a `POINT_LIST`
   pipeline whose vertex shader never writes `PointSize`, i.e. point size is undefined for
   those draws; the question is what those six pipelines draw.
5. **Re-test the remaining picture defects with the outdoor instrument that now exists** —
   shadow cascade (item 3), mipmaps (4), colour grading (6). All three were parked partly
   because no outdoor comparison was possible; `tools/frame_era_medians.py` plus the
   DebugJump route is the thing that was missing. Quote the null and the frame count.
6. **The binned frame-time A/B still owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23). Read
   the MEDIAN and the vblank-pinned share, not the mean (237).
7. The rest of `docs/open-items.md`, including item 12.

**Still deliberately NOT planned: giving `CZ_FAKE_PRESS_SEQ` a trigger.** The button is the
easy half; a recipe would still have to ACQUIRE a gun and ammo along a long scripted path.
Propose the acquisition first.

## What part 26 delivered

* **The cube snapshot path.** `06805000` is assembled from the six resolve snapshots at
  `base + i * 0x4000` into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image in set 2, refreshed
  by each face's own resolve in that resolve's own command buffer (8,850 face refreshes in
  240 s — the title re-renders it continuously, so a one-shot fill would freeze it). The
  face layout was PRINTED face by face and could have refuted the stride model; six of six
  filled. **358,767 of 999,508 cube fetches (35.9%)** now read it.
  `CZ_VK_NO_CUBE_SNAPSHOT=1` is the same-binary arm.
* **`tools/frame_determinism.py`** — the null for any matched-frame comparison, and the
  measurement that retired the outdoor filter: 0 of 12,174.
* **`tools/frame_era_medians.py`** — the replacement protocol, with the null as a required
  argument.
* **Three validation defects closed** (03320, 01021, 09600) and **object naming**, which is
  what made the third diagnosable at all.
* **The audio trace rewritten** so silence and blindness are different numbers, with a
  self-test on the scanner. It says SILENCE.
* **The outdoor cube A/B itself**, six runs in one block: the cube-map CLASS is 8x the
  noise floor, the single rendered map is below it, and the third baseline is what turned a
  "12.0x the null" result into no result at all.
* Gotchas 254 (an exact-equality filter is a test for stasis), 255 (name your objects
  before chasing a message that names one), 256 (with a bindless heap, publish the
  descriptor AFTER the transition), 257 (a fetch count is not a screen area), 258 (run the
  third baseline before publishing a multiple of a two-run null).

## Gates, on the part-26 binary

`--smoke` OK. `tools/shader_dim_census.py` exit 0 across all **409** shaders, the ucode
parse and the translated SPIR-V agreeing on every one. Vulkan validation: **26 messages,
2 distinct VUIDs** (was 64 / 5), both pipeline-creation rather than per-draw.

**Not re-run this part and owed before any claim that rests on them**: the A5 kernel-call
diff, `truncated=0`, the PM4 capture oracles, the capture-E picture correlation, and
`grep -c "no translated shader"`.

## The method notes worth carrying

* **The route was never the problem — the FILTER was.** Part 25 read "13-44 admissible
  frames, all under 1,800 draws" as a statement about the recipe and built a better recipe.
  It was a statement about exact equality on an animated scene, and the way to tell the two
  apart costs one command: run the filter on two runs of ONE configuration. If it reports
  nothing there it can never report anything (gotcha 254).
* **A validation layer tells you which rule broke; only you can tell it which of your
  objects broke it.** ~40 lines of `vkSetDebugUtilsObjectNameEXT` turned an unidentifiable
  handle into a diagnosis in one run (255).
* **When one path in a file is quiet and its twin is not, read the quiet one.** The
  snapshot VIEW path had the publish order right; the snapshot path did not, and the
  difference was the whole of 09600 (256).
* **A statistic earns "usable" by reproducing across THREE runs.** Median mean-luma did
  (0.55% over three baselines); median distinct-colour count did not (5.4%, after two-run
  nulls of 0.76% and 0.12% — a 6x disagreement that was the warning). The 12x result this
  block nearly published was on the second statistic (258).
* **A model that prints itself can refute itself.** The cube face stride is a model of the
  guest's layout; printing each derived address with whether a snapshot was found there
  costs six lines and converts an assumption into a measurement (gotcha 244's shape).
