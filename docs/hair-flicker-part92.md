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
