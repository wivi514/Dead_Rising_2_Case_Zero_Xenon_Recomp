# D3D phase C, part 23 hand-off (for part 24). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c22-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session twice now.

## The one-paragraph state of the port

Case Zero boots, renders and plays. Part 23 landed the **CPU/GPU overlap** — the largest
item in the performance plan — and the fence wait went from 31.5% of a crowd frame to
0.2%. The operator's report is "way more stable, almost always around 30 fps". Then the
same operator reported **wrong textures throughout the game**, which turned into part 23's
larger finding: **91 of 395 shaders sample a cube map and every one of them gets the 1x1
white dummy, and has since phase 5.** That is now the top picture item and it is fully
specified. Two smaller things are unresolved and are written up honestly rather than
guessed at: an intermittent HUD/ammo defect that may be a part-22 regression, and the
frame-time A/B for the overlap, which was stopped mid-sweep and is still owed.

## READ THIS BEFORE MEASURING ANYTHING

The first four are carried forward because each still costs a session. The last two are
new and both are about being wrong in public.

1. **The noise floor of a crowd frame-time A/B is 10-13% at one run a side** (gotcha 229).
   Three runs an arm, alternated a/b/a/b/a/b. One 620 s run is ~10 minutes, so a real A/B
   here is an hour.
2. **A MEAN over frames measures this title's PACING FLOOR, not your change** (gotcha 237).
   `tools/frame_perf_bins.py` now prints the **median** and the **vblank-pinned share**
   next to the mean — part 23 put them in the tool so nobody computes them by hand again.
   Read the pinned column first.
3. **DO NOT PIN THE GPU CLOCK** (gotcha 231). Sample and quote with
   `tools/gpu_clock_sample.py`. A low clock at LOW utilisation is the governor being right.
4. **A profiler is instrumentation, so break it on purpose before trusting it**
   (gotcha 228), and a comparison that only ever reports 100% has not been shown capable
   of reporting anything else (gotcha 234).
5. **NEW — a counter that reads zero because its EMITTER is gated off is not evidence.**
   Part 23 reported "no `GUARD MISSED` lines" as though the stream store were clean. That
   line only prints under `CZ_VK_STREAM_CENSUS=2`, which had not been set. Gotcha 25
   exactly, and it was caught only by going and reading the emitter. **Read the emitter
   before quoting a zero** — every time, including when the zero is convenient.
6. **NEW — one sample per arm is not an A/B when the defect is intermittent.** The operator
   said the HUD breaks *sometimes*; part 23 nonetheless wrote a three-row table off one
   screenshot per arm and had to retract it in the next message. If the report contains
   the word "sometimes", the arm needs repetition before it needs a table.

## Where part 24 starts, in order

1. **CUBE MAPS ARE NEVER BOUND — open-items 00, the top picture item and fully specified.**
   91 of 395 shaders sample set 2 (`TextureCube[]`) and read descriptor index 0 every draw.
   `bindTextures` (`vk_renderer.cpp:4262`) writes only the 2D index array; `kSharedTex3D`,
   `kSharedTexCube`, `kSharedTex1D` are never written; `t.dimension` is hardcoded to 2D at
   `vk_renderer.cpp:2160` and `ShaderMeta::tfetchConsts` carries no dimension.
   Three parts, and part 1 is the work: carry the per-slot dimension into the sidecar
   (per SLOT, not per module — a shader may sample both); create cube images with six
   layers and a `VK_IMAGE_VIEW_TYPE_CUBE` view registered in **set 2**; write
   `kSharedTexCube[constIdx]`.
   **Predict before running it**: every reflective surface currently multiplies its
   specular by white, so the falsifiable claim is that the operator's "unicorn colour"
   cabinet and wrong dumpster colour change, and that item 6 ("colour is flat") moves
   partially. Say so in the commit, then have the operator look.
   Do NOT re-derive the two dead theories in open-items 00 — the LUT is not a Texture3D
   (zero modules use set 1) and the `constIdx >= 16` skip never fires (0 of 1,076 slots).
   That skip should still get a counter; it is a silent drop.

2. **FINISH THE OVERLAP'S FRAME-TIME A/B.** The mechanism is proven (fence wait 31.5% ->
   0.2%) and the picture is gated (E2 +0.9595 at N=1 vs +0.9594 at N=2), but the binned
   comparison is owed: three runs an arm, alternated, profiler OFF, on the FINAL binary.
   `/tmp/p23_ab.sh` is the driver that was written for it. Read medians and the pinned
   share, not the mean. Also still owed on part 23's change: the **A5 kernel-call gate**,
   `truncated=0`, `no translated shader = 0`, and both PM4 capture oracles — the last two
   are valid by construction (`gpu/pm4.cpp` is untouched) but should be run.

3. **THE HUD / AMMO DEFECT — open-items 00c, and it may be a part-22 regression.** The ammo
   count flickers between 26 and 27 every frame regardless of the real value, triggered by
   firing; the HUD intermittently collapses. It happens at `CZ_VK_FRAMES_IN_FLIGHT=1`, so
   part 23's ping-pong is not the cause. One run with `CZ_VK_NO_PERSIST_STREAMS=1` looked
   clean, which points at the cross-frame stream store — **one sample, against an
   intermittent defect; treat it as a lead.**
   Every headless instrument came back **blind, not negative**: the recipe never fires a
   weapon or changes a HUD number, so `GUARD MISSED` reads `0 of 0` and the only rewritten
   streams are 30 keys of exactly 80 bytes, all under the guard's exact bound.
   **The cheapest real progress is a headless recipe that FIRES A WEAPON and watches a HUD
   number.** Until that exists this needs an operator (gotcha 190).

4. **The remaining picture defects**, all older than part 23 and all worth re-testing after
   item 1: the shadow cascade (open-items 3), mipmaps (4), the pitch-vs-declared-size path
   firing 1.3M times a run (4b), NPC part meshes (3d), the magenta sky / colour-grading LUT
   (6), the prologue cinematic. Plus two new observations with no mechanism yet: a large
   translucent overlay with a **hard vertical seam at the exact middle of the frame** (the
   tile boundary — a tiling or predication question, not a texture one), and
   `texture: fetch constant is not a texture` returning the dummy **21,567 times** in one
   operator session, which is what a blank untextured wall looks like.

5. **The PM4 walk — ~12 ms, a register-write loop**, unchanged from part 22's hand-off.
   90,316 packets a frame carrying 815,020 register dwords. Biggest lead:
   `LOAD_ALU_CONSTANT` reads guest memory one `GuestLoad32` at a time where a contiguous
   run wants one bulk pass. Cheapest: `ExecutePacket`'s 2-4 locked atomics per packet on a
   single-threaded path, ~2 ms. **`pm4.cpp` is under two exit-1 capture oracles.**

## What part 23 delivered

* **The CPU/GPU overlap** (§6aw). `CZ_VK_FRAMES_IN_FLIGHT=N`, default 2, `=1` is the old
  renderer exactly. Fence wait **31.5% -> 0.2%** of a crowd frame. Picture unchanged.
  The plan's "this needs a real swapchain" is **retracted** — a per-slot readback presented
  one frame later keeps phase 3's renderer/window separation and costs one frame of
  latency. The arena is cut into N regions of one buffer, so none of the growth or
  exhaustion machinery moved. The store's stale path ping-pongs to a twin slot; `evicted,
  no twin` read 0 everywhere.
* **The cube-map finding** (§6ax, open-items 00) — the top picture item, found by censusing
  395 SPIR-V modules rather than by reasoning about screenshots.
* **`CZ_VK_TEX_GUARD` / `CZ_VK_TEX_REVALIDATE` / `CZ_VK_TEX_GUARD_POISON`** — a content
  guard over the texture cache. It **refuted its own hypothesis** at 0.00% on the
  operator's session and is kept because it is two-sided and it is the only thing that can
  see a stale texture at all.
* **`tools/frame_perf_bins.py` reports the median and the vblank-pinned share**, turning
  gotcha 237 from a hand-off warning into a column.
* One real performance fix found in passing: `UploadTexture` formatted an address string
  on **every** call whether or not `CZ_VK_TEX_DUMP` was set — ~13,900 `snprintf`s a frame
  charged to the `textures` column that open-items 0a-ii is about. Same defect part 20
  fixed for `psbind`, in the one place that was missed.

## Gates, on the part-23 binary

`--smoke` OK; the build is silent; the picture is unchanged (capture E2 **+0.9595** at N=1
and **+0.9594** at N=2, identity orientation, against part 22's +0.9596/+0.9590).
**Still owed and listed in item 2 above:** A5, `truncated=0`, `no translated shader`, both
PM4 oracles, and the binned frame-time A/B.

## The method notes worth carrying

* **A census over the shader bank beat three rounds of reasoning about pictures.** The
  three theories that preceded the finding were each plausible, each testable, and each
  wrong; the survivor came from parsing 395 SPIR-V modules for one decoration word, which
  took about a minute and needed neither a run nor an operator. The shader bank is a
  population. Count it.
* **Two of part 23's own claims were retracted inside the session** — a three-row A/B table
  built on one sample per arm, and "no `GUARD MISSED` lines" quoted from a counter whose
  emitter was gated off. Both were caught by going back to the instrument rather than by
  thinking harder about the result.
* **The operator's pictures named subsystems no counter here could.** A hard vertical seam
  at x=640 is the tile boundary; a flat cream wall is the white dummy's signature. Neither
  is visible in any aggregate this runtime prints.
