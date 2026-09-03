# Chuck-hair flicker — part 92 investigation record (OPEN)

The operator reported a flicker "at the back of Chuck's hair" and confirmed it does
NOT occur on real hardware. This is the investigation record; the defect is still
open but has been localized to a shader and a class.

## What is established (each by its own arm/measurement)

- **It is OURS.** It survives at the title's native internal 1280x720 (operator A/B),
  so it is not a super-resolution artifact of a 2010 asset, and the operator confirms
  hardware does not show it.
- **It is SHADING, not coverage/geometry.** The decisive test (2026-09-02): a shader
  arm forcing Chuck's body pixel shaders to a CONSTANT colour. In the operator's F8
  burst of that arm (`play_0902_1439`), a per-pixel oscillator classification found
  **513 flickering pixels, ALL brown (surfaces still running real lighting), ZERO on
  the greyed constant-output body**. A constant cannot flicker, and greying a surface
  stopped its flicker entirely — so the meshes' coverage/depth is stable and the
  defect lives in the lighting math of the surfaces still shaded normally.
- **It is CONTINUOUS and MOTION-GATED**, not z-fighting. Burst pixel traces show a
  continuous per-frame swing (gap-ratio ~0.18, not a two-level toggle), with long
  perfectly-stable stretches when nothing moves and churn under animation.
- **4x supersampling (6880x2880) did NOT fix it** — so it is TEMPORAL (the shaded
  value at a fixed surface point changes frame to frame), not spatial specular
  aliasing.

## The six-session misidentification, corrected

Until 2026-09-02 the "hair" was taken to be the 7938-vert draw (vs=c3d6d301
ps=ea2cd381 / the overlay vs=e86e7024 ps=1f93b74b). The grey arm proved that mesh is
Chuck's **TORSO**: greying ps_ea2cd381 + ps_45109c37 turned the torso pale and left
the hair brown and flickering. Every arm aimed at those shaders therefore could not
have moved the hair. **Chuck's character VS is c3d6d301, which pairs with THREE pixel
shaders** — ea2cd381, 45109c37 (body, now known) and **ps_522e2b166969c4cd**, the
third, never tested and the prime hair suspect.

## Arms EXONERATED (do not re-buy)

shader/A2M path; all four skinned-constant optimizations (CZ_VK_NO_BOUNDED_DYNAMIC,
NO_PATCH_MEMO, CONST_GATHER=0, NO_PARALLEL_GUARD); the cross-frame stream store
(CZ_VK_STREAM_GUARD_EXACT); frames-in-flight=1; parallel record (NO_PAR_RECORD);
deferred clears (NO_DEFERRED_CLEAR); 4x supersampling; the shadow term
(CZ_VK_SHADOW_FILL=1.0, and the hair shader samples its own shadow); the textureless
sheen overlay pass (CZ_VK_SKIP_VS=e86e7024); depth-tie quantization (a full D24F
20-bit-mantissa arm cache).

## READY for the next operator look

`assets/shader_spv_clip_a2m_hairid` — grey body + **magenta ps_522e2b16**. Launch:
```
PLAIN=1 CZ_SHADER_SPV=$PWD/assets/shader_spv_clip_a2m_hairid \
  CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 tools/play_session.sh
```
DebugJump to Case 0-2, hold still. **Question: is Chuck's HAIR the magenta part?**
- Hair magenta AND no longer flickering -> ps_522e2b16 is the hair and the defect is
  in its shading; read tools' translated HLSL of it and diff against the microcode.
- Magenta elsewhere -> the hair uses another shader; note what turned magenta.

Diagnostic patch hooks committed: tools/patch_hair_grey_hlsl.py (constant grey),
tools/patch_hair_id_hlsl.py (grey body + magenta suspect), tools/patch_depth_quant_hlsl.py
(D24F depth quantization). Build an arm cache with e.g.
`CZ_HLSL_PATCH=$PWD/tools/patch_hair_id_hlsl.py CZ_DXC_DEFINES="-D XE_USER_CLIP_PLANES=1 -D XE_ALPHA_TO_MASK=1" tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps <outdir>`
and gate it: names identical to the play cache, only the intended PS differ.

## Leading hypothesis for the fix

The value at a fixed hair surface point changes per frame under animation (temporal,
supersampling-proof). Positions are smooth (v0 drifts monotonically). The suspect is
the hair shader's NORMAL/specular path: a per-frame-unstable tangent-space basis or
normal-map LOD as the skinned mesh deforms, which hardware's filtering/MSAA-resolve
smooths and our one-sample-per-pixel path does not. Confirm the shader first, then
read ps_522e2b16's translated HLSL for the normal/specular arithmetic.

## RESOLVED DIAGNOSIS (2026-09-03) — layered translucent hair cards, needs MSAA

The hair is **vs=d78d670a ps=34524bb6**: 178 alpha-blended (SRC_ALPHA/INV_SRC_ALPHA,
blend 07060706) cards layered on the top of the head (v0 y 1.66-1.82), all sampling one
512x512 hair texture, all writing depth (RB_DEPTHCONTROL bit 2) and testing LESS_EQUAL.
Positively identified by the four-colour character-ID arm (the body is ea2cd381/45109c37,
the watch is 522e2b16, ab3a6ccc is a small specular+cube mesh — none of them the hair).

The flicker is the cards' **depth-test result flipping frame to frame at card
crossings** as the skinned mesh animates. Confirmed by two same-binary arms:

| arm | flicker | look |
|---|---|---|
| default (D24_UNORM, blend depth-write ON) | YES | correct (solid) |
| `CZ_VK_NO_BLEND_DEPTH_WRITE=1` | **gone** | see-through layers |
| `CZ_VK_DEPTH_FLOAT=1` (D32_SFLOAT depth) | YES | correct (solid) |
| `CZ_VK_DEPTH_FLOAT=1 CZ_VK_NO_BLEND_DEPTH_WRITE=1` | **gone** | see-through layers |

So: card-vs-card depth-write is REQUIRED for the solid look and is the CAUSE of the
flicker; float precision alone does not fix it (the cards genuinely cross in depth, it
is not just a UNORM tie). The console is clean because Xenos renders into a natively
**multisampled** EDRAM surface: per-sample depth makes a card crossing transition
gradually across samples instead of flipping a whole pixel. Our renderer is
single-sample and has no downsampling resolve, so the crossing flips. 4x supersampling
did not help (stale, pre-ID test) consistent with this being a per-sample-DEPTH effect,
not a shading-resolution one.

**The clean fix is real MSAA** (multisampled colour+depth with a resolve), or
depth-sorted / order-independent transparency for the hair. Both are substantial
renderer features, out of proportion to a cosmetic bug and competing with the release
board — decision deferred to the operator. Two partial mitigations exist as arms if a
"flicker gone, slightly see-through" trade is preferred:
`CZ_VK_DEPTH_FLOAT=1 CZ_VK_NO_BLEND_DEPTH_WRITE=1`.

Arms added this session (same-binary, off by default): `CZ_VK_NO_BLEND_DEPTH_WRITE=1`
(depth-write off for blended draws), `CZ_VK_DEPTH_FLOAT=1` (D32_SFLOAT_S8 EDRAM depth).

## MSAA BUILT AND TESTED (2026-09-03, part 93) — IT WORKS BUT DOES NOT FIX THE FLICKER

`CZ_VK_MSAA=4` (true multisampled EDRAM + resolves, `docs/msaa-plan.md` §9,
commit c02d2cc) was built and put in front of the operator at DebugJump 0-2. The
verdict, both arms same session, camera still:

| arm | flicker | hair |
|---|---|---|
| `CZ_VK_MSAA=4` | **still present** | solid |
| control (no MSAA) | still present | solid |

**The operator saw NO difference between the arms.** That could mean MSAA was not
reaching the presented image, so it was checked headlessly rather than assumed:

- **MSAA genuinely reaches the screen.** A frame-sharpness A/B (mean |luma gradient|,
  `tools/frame_sharpness.py`, ~800 frames/arm on the crowd route) read control 6.244
  vs 4x **6.128 — 1.9% softer**, in the anti-aliasing direction. Small because a
  whole-frame gradient is dominated by textured interiors and MSAA only touches
  silhouette EDGES — and a 7x zoom on a static car's silhouette against the road
  confirms it directly: hard stair-steps in the control, graduated blend pixels along
  the same diagonal at 4x. The resolve works; AA is on the picture.
- **So the flicker is NOT a coverage/edge effect**, which is why per-sample coverage
  cannot touch it. This RECONCILES the two halves of this document that looked in
  tension: the early grey-arm finding (the flicker is in the SHADING of whole
  surfaces, not geometry/coverage) was right, and the later "depth-test flip at card
  crossings" framing was incomplete. The cards do not cross along clean LINES that
  MSAA could dither — they are near-coplanar/interpenetrating over BROAD regions, so
  when the depth ordering flips, every one of a pixel's 4 samples flips together and
  the whole area's blended colour swings. 4x MSAA (and, consistently, the earlier 4x
  supersample) leaves it untouched for the same reason.

**Consequence for the fix.** The flicker is a translucency-ORDERING instability, not
an aliasing one. The remaining faithful fixes are the heavy ones this doc already
named — per-frame back-to-front depth sorting of the 178 hair cards, or
order-independent transparency — and MSAA is not a substitute for them. What MSAA IS:
a genuine, working, game-wide anti-aliasing feature that happens not to be the hair
fix. Keep it as an arm on its own merits (`docs/msaa-plan.md` §9 has its cost:
+0.85 ms at 2x / +1.32 ms at 4x at 1440p, frame time unmoved on the dev machine).

**The standing hair mitigation remains `CZ_VK_DEPTH_FLOAT=1 CZ_VK_NO_BLEND_DEPTH_WRITE=1`**
(flicker gone, hair slightly see-through) until OIT/sorting is judged worth its cost
against the release board. Decision is the operator's.
