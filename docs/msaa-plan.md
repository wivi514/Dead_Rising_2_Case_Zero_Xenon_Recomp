# MSAA plan — true multisampled EDRAM (the faithful hair-flicker fix)

**Commissioned 2026-09-03 by the operator** ("Go for full MSAA") as the console-accurate
fix for the part-92 Chuck-hair flicker. Start this in a FRESH conversation; read
`docs/hair-flicker-part92.md` first (the full diagnosis) and this file second.

## 0. Why, in one paragraph

Chuck's hair is 178 layered alpha-blended cards (vs `d78d670a` ps `34524bb6`) that write
depth and test LESS_EQUAL. Their depth ordering flips frame to frame at card crossings as
the skinned mesh animates → flicker. Confirmed: `CZ_VK_NO_BLEND_DEPTH_WRITE=1` kills the
flicker but makes the hair see-through (cards must occlude each other); `CZ_VK_DEPTH_FLOAT=1`
fixes precision but not the flicker (the cards genuinely cross). **The console is clean
because Xenos renders into a natively MULTISAMPLED EDRAM surface**: per-sample depth makes a
card crossing transition gradually across samples instead of flipping a whole pixel. Our
renderer is single-sample. The fix is true MSAA. It also sharpens every edge in the game,
not just the hair.

## 1. The key insight that makes this tractable

**Xenos's model maps 1:1 onto Vulkan MSAA.** Xenos EDRAM is a multisampled surface; the
guest's `RB_COPY` (our `DoResolve`) IS the MSAA resolve. So:

- our persistent EDRAM colour+depth images (`R->color`, `R->depth`) become **multisampled**;
- **`DoResolve` becomes the resolve** — `vkCmdResolveImage` for colour, a depth resolve for
  depth — instead of `vkCmdCopyImage`. The snapshots it writes stay single-sample (they are
  sampled as ordinary 2D textures by later passes, exactly as today);
- the **present path** resolves the final EDRAM colour to the single-sample present image.

This is why MSAA is the *faithful* model and not a bolt-on: the resolve points already exist
in the renderer and correspond exactly to hardware's resolves.

## 2. What exists today (recon done 2026-09-03 — do not re-derive)

- **Single-sample everywhere.** Three pipeline sites hardcode `rasterizationSamples =
  VK_SAMPLE_COUNT_1_BIT`: `vk_renderer.cpp:10324` (inline draw), `:18473` (RT factor pass),
  `:19486` (depth-view / misc). The cube image sets `ci.samples` at `:5741`.
- **EDRAM is persistent, not per-pass.** Rendering uses `LOAD_OP_LOAD`/`STORE_OP_STORE`
  (`ParRec_RecordInstance`, `:10968`) across many BeginRendering/EndRendering cycles. The
  EDRAM colour+depth are created once at bring-up (`:24820`/`:24833`) and on live rescale
  (`:28288`), via `CreateImage`.
- **The EDRAM is ALREADY SUPERSAMPLED for 2x/4x surfaces**, via window-coordinate scaling:
  `RS`/`RSX`/`RSi`/`RSXi` (`:1469-1472`) scale extents by `InternalH/720` and `InternalW/1280`,
  and for a 4x surface the window Y is doubled (`CZ_VK_NO_MSAA_WINDOW_SCALE_Y`, near `:22120`)
  so "one host pixel is one guest sample." **This is the emulation MSAA replaces** — see §4.
- **`RB_SURFACE_INFO` MSAA field** is bits 16-17 (`(regs[kRbSurfaceInfo] >> 16) & 3`:
  0=1x, 1=2x, 2=4x). Read today in the A2M path (`:22118`-ish) and `IsShadowSurface`
  (`:5244`). The A2M dither and the 2x-window-scale both key off it.
- **`DoResolve`** (`:23801`) copies EDRAM→snapshot with `vkCmdCopyImage` (`:24316`), colour or
  depth per `copy_src_select`. Snapshots are single-sample 2D images sampled later.
- **Depth format** is now `EdramDepthFormat()` (`:5870`): `D24_UNORM_S8` default,
  `D32_SFLOAT_S8` under `CZ_VK_DEPTH_FLOAT=1`. Keep the float arm — MSAA + float depth is the
  most faithful combination.
- **Present** (`:4283`+): default resolves EDRAM→host buffer→SDL; `CZ_VK_SWAPCHAIN=1` blits.
- **Two mitigation arms already shipped** (part 92, off by default): `CZ_VK_NO_BLEND_DEPTH_WRITE`,
  `CZ_VK_DEPTH_FLOAT`. Keep them; they are the fallback if MSAA is deferred again.

## 3. Design decisions to make first (before code)

1. **Sample count.** Xenos supports up to 4x. Recommend a single fixed host sample count
   behind `CZ_VK_MSAA=N` (N ∈ {2,4}), NOT per-surface variable MSAA — the persistent EDRAM
   is one image and cannot change sample count per pass. 4x is the faithful max; 2x is the
   cheaper first target. **Query `VkPhysicalDeviceLimits::framebufferColorSampleCounts &
   framebufferDepthSampleCounts`** and clamp; fall back to 1x (current renderer) if the
   device lacks it.
2. ~~**Replace or keep the supersample emulation?** They are two ways to get a sample grid and
   MUST NOT stack. Decision: when `CZ_VK_MSAA` is on, **turn OFF the 2x window-Y scale and the
   4x-surface supersampling of the EDRAM** (the sample grid now comes from real MSAA), and
   render the EDRAM at the plain internal resolution with `rasterizationSamples=N`.~~
   **RETRACTED IN PART 93, when the build session read the code instead of this plan's
   recon summary.** The msaa==2 window-coordinate doubling is NOT a supersample of the
   EDRAM image — the image's extent never changes with the surface's declared MSAA; the
   doubling is COORDINATE ALIASING that keeps differently-declared surfaces covering the
   same stand-in pixels (the title re-declares a surface 4x purely to CLEAR it: the scene
   tile's clear is `(0,0)-(320,720)` declared 4x over a 640-wide 2x tile, and only the
   doubling makes it cover the tile — `vk_renderer.cpp` ~22111's own comment). Turning it
   off under MSAA would half-cover those clears and reintroduce that defect. So real MSAA
   **keeps the whole window-coordinate mapping untouched** and adds N samples per stand-in
   pixel orthogonally: nothing stacks, because the two mechanisms govern different axes
   (where a draw lands vs. how many samples each landed pixel carries). The A2M
   alpha-to-coverage switch remains a possible step-5 follow-up, unchanged.
3. **Depth resolve mode.** `vkCmdResolveImage` does NOT resolve depth. Use a resolve
   ATTACHMENT with `VkResolveModeFlagBits` (`VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` minimum, or
   `MIN`/`MAX` — check `VkPhysicalDeviceDepthStencilResolveProperties`). Colour resolves via a
   resolve attachment on `vkCmdBeginRendering` OR via `vkCmdResolveImage` at DoResolve time —
   pick per §5 step ordering.

## 4. The interactions that will break if ignored (the risk list)

- **`DoResolve`'s `vkCmdCopyImage`** cannot copy a multisampled image to a single-sample
  snapshot — it must become a resolve. This is the single biggest code change.
- **The 2x window-Y scale / RSX-RS supersampling** must be disabled under MSAA or the image
  is double-sampled (wrong extents, 2x memory on top of Nx). Every `RS/RSX` call site is
  implicated; the cleanest is to make `InternalW/H` NOT include the 2x factor when MSAA is on.
- **A2M** (`XeAlphaTestThreshold`, `g_AlphaToMask`, `CZ_VK_A2M_MODE`): the per-sample dither
  assumes one-host-pixel-is-one-guest-sample. Under real MSAA switch to `alphaToCoverageEnable`.
- **Snapshots sampled as textures** stay single-sample (they are the resolve *output*) — good,
  no change to the sampling side.
- **RT depth read** (`CZ_VK_RT`) samples `R->depth`; a multisampled depth image needs a
  resolved copy or a `sampler2DMS`. RT is parked, so gate MSAA to refuse+warn under RT for now.
- **Present readback / F8 / F9 / SNAP_DUMP** read EDRAM; they must read the *resolved* image.
- **Barriers**: multisampled images have the same layout rules; the resolve adds a
  TRANSFER/RESOLVE stage. Run `CZ_VK_SYNC_VALIDATION=1` — it is the gate for this.
- **Memory/perf**: 4x EDRAM colour+depth at internal res is ~4x the attachment memory and
  bandwidth. Measure with `CZ_VK_GPU_PASSES=1`; expect a real GPU cost (this is why it is an
  arm, and why 2x is the cheaper first target).

## 5. Build order (each step gated, committed separately, arm-first)

**EXECUTED 2026-09-03 (part 93), steps 0-4 as one arm — §9 below is the record;
steps 5-7 remain.**

0. **Plan/measure baseline.** Capture the hair flicker headlessly if possible (the part-92
   harness lands on the DebugJump menu — may need the operator for the close view), and
   record `CZ_VK_GPU_PASSES=1` frame cost at the crowd for the perf delta.
1. **Add `CZ_VK_MSAA=N`** and a `SampleCount()` helper (query limits, clamp, default 1).
   Wire `rasterizationSamples` at the three pipeline sites from it. No attachment change yet —
   this alone is invalid (samples mismatch attachments) so it is a compile/plumbing step only,
   folded into step 2.
2. **Multisampled EDRAM.** Create `R->color`/`R->depth` with `SampleCount()` at both creation
   sites; add single-sample **resolve targets** (`R->colorResolve`, and depth resolve target if
   needed). Disable the 2x/4x supersample scaling under MSAA (§4). Gate: `CZ_VK_VALIDATION=1`
   clean, `--smoke`, renders at all.
3. **Resolve at `DoResolve`.** Replace `vkCmdCopyImage` with a resolve (colour: resolve then
   copy region, or resolve-attachment; depth: resolve-mode attachment). Snapshots stay 1x.
   Gate: picture matches the non-MSAA arm on a still frame (era medians / F9 diff), sync
   validation 0 hazards.
4. **Present from the resolved image.** Point the present readback / swapchain blit and the
   F8/F9/SNAP paths at the resolved colour. Gate: F9 looks correct, no black.
5. **A2M → alphaToCoverage** under MSAA; retire the dither for the MSAA path. Gate: foliage
   looks correct (operator), isolated-pixel share back down.
6. **The hair verdict.** Operator DebugJump 0-2, camera still: flicker gone AND hair solid.
   This is the acceptance test the whole feature exists for.
7. **Perf + default decision.** `CZ_VK_GPU_PASSES=1` cost at crowd; era medians; decide 2x vs
   4x and whether it ships default-on or arm-only. Keep `CZ_VK_MSAA=0` as the control forever.

## 6. Gates (standing, run every step)

`--smoke`; `CZ_VK_VALIDATION=1` clean; `CZ_VK_SYNC_VALIDATION=1` 0 hazards (the barrier gate —
the resolve is new synchronization); `tools/frame_era_medians.py` inside the null on a still
frame vs the `CZ_VK_MSAA=0` arm; the shader-cache name-diff and dim-census gates unaffected
(no shader-cache change except the A2M pipeline flag); PM4 oracles unaffected (no pm4.cpp
change). Every step is a same-binary A/B: `CZ_VK_MSAA=0` is the pre-MSAA renderer bit-for-bit.

## 7. Fallback

If MSAA proves too costly or too invasive, the shipped part-92 arms
(`CZ_VK_DEPTH_FLOAT=1 CZ_VK_NO_BLEND_DEPTH_WRITE=1`) give "flicker gone, slightly see-through
hair" as a one-line default. Document whichever is chosen with its control arm.

## 8. For Case West

Xenos EDRAM is multisampled in every 360 title; whatever MSAA lands here transfers whole to
Case West (same engine). The 1:1 "RB_COPY is the resolve" mapping is general, not Case-Zero
specific.

## 9. Execution record (part 93, 2026-09-03) — the arm is BUILT and headlessly gated

Commit c02d2cc; everything below measured on it, same binary both arms, at
1280x720 pinned unless stated.

**What shipped** (`CZ_VK_MSAA=N`, N in {2,4}, OFF by default — unset is the
single-sample renderer bit for bit):

- EDRAM colour+depth created at N samples; every draw pipeline rasterizes at N
  (one site — the RT trace/factor pipelines render into 1x images and RT is
  refused under MSAA). Live rescale rebuilds everything.
- `DoResolve`: colour snapshot copy → `vkCmdResolveImage`, region-for-region.
  Depth (no colour-only API covers it) → a zero-draw dynamic-rendering pass
  whose SAMPLE_ZERO resolve attachment writes the new single-sample
  `R->depthResolve` over exactly the copy's renderArea, then the existing
  `vkCmdCopyImage` reads that image with unchanged offsets. Snapshots stay 1x,
  so every downstream consumer (present, F8/F9, SNAP_DUMP, cube faces, sized
  views) is untouched.
- The present raw-EDRAM fallback resolves into `R->colorResolve` first
  (blit/readback cannot read multisampled); 10 engagements on a boot, counted.
- Device gate: N clamped down to `framebufferColorSampleCounts &
  framebufferDepthSampleCounts`, SAMPLE_ZERO depth AND stencil resolve modes
  verified, every refusal printed by name.

**What the gates caught** (both real, both fixed in the commit):

1. `VUID-VkImageViewCreateInfo-image-04441` — the colour companion had only
   TRANSFER usage while `CreateImage` unconditionally builds a view; a SAMPLED
   bit nobody reads satisfies the view. Validation delta over the control arm
   is now ZERO (the 8 `topology-08773` PointSize messages are pre-existing —
   identical count in the MSAA-unset control).
2. Sync validation, 10 WRITE-AFTER-WRITE: a depth RESOLVE-ATTACHMENT write is
   modelled at COLOR_ATTACHMENT_OUTPUT with COLOR_ATTACHMENT_WRITE access —
   both barriers around the mini-pass (into the pass, and out to TRANSFER_SRC)
   must name that scope, which `LayoutMasks`' depth-attachment entry does not.
   Fixed with two explicit barriers at that one site (LayoutMasks untouched, so
   the 1x arm's barriers are byte-identical). **0 hazards over 16,164 depth
   resolves; the CZ_VK_BARRIER_POISON positive control produces exactly the
   documented 30.**

**Measurements** (DebugJump crowd route, three-run serial block base/4x/base,
all arms reaching 7.6k draws, era = frames >= 1800 draws):

- **Picture: inside the null.** meanLuma 0.17% from base (null 1.14%),
  distinctColours 0.53% (null 1.43%); coverage saturated as always.
- **GPU: +0.33 ms/frame at 720p** (5.22/5.25 base → 5.57, +6.3% against a 0.5%
  null). The resolve-copy class 0.22 → 0.41 ms (it is a real downsample now);
  the >=256-draw passes +0.12 ms. NOT measured at the operator's 3440x1440,
  where it will be several times larger — that read is owed before any
  default-on decision (step 7).
- **Frame time: unmoved** (median 10-11 ms all three arms — the GPU cost is
  absorbed by this route's headroom, consistent with the part-90 regime).

**Owed:** step 6, the operator's hair verdict (DebugJump 0-2, camera still,
`CZ_VK_MSAA=4` vs unset — flicker gone AND hair solid is the acceptance test);
the 3440x1440 GPU cost read; step 5 (A2M alphaToCoverage) which is optional
polish, not blocking; step 7's 2x-vs-4x and default decision, theirs.
