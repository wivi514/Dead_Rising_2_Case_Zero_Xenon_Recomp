# The RT + FOV plan — ray-traced AO / lighting / shadows / GI tiers, and a Field of View slider

Operator's instruction, verbatim scope (2026-08-21, closing part 60's session): *"we'll
add all of these for next plan: ray traced ambient occlusion, Ray traced lighting, ray
traced shadows, ray traced global illumination. they'll be shown like this we'll have
like the settings lighting with options available for it being OG(what we got now) and
rt low, rt medium, rt high. Follow all recommended way to implement those. Also add
Field of view slider to the plan."*

Ordered so every stage ships something and every stage can be REFUSED honestly by
measurement. The FOV slider is first because it is small and reuses machinery that
already exists; the RT stages are ordered shadows -> AO -> lighting/GI because that is
ascending order of how much engine knowledge each one needs. **Panel presentation, per
the instruction: each RT effect is a row with OG / RT LOW / RT MEDIUM / RT HIGH, where
OG is exactly today's renderer and stays the default and the control arm.**

## 0. The FOV slider (ships first, independent of everything below)

> **SHIPPED IN PART 61 (5b9fbba), CORRECTED THE SAME NIGHT IN PART 62
> (943227d; the record is `phase5-notes.md` §6cs)**: the part-61 form patched
> only the raw projection, which is the UI/frontend — the WORLD rides a
> view-projection COMPOSITE at c0-3 that this plan (and part 60's wide patch)
> never anticipated. The shipped design is **composite-only fov**: the world
> moves, the HUD stays pixel-static — better than this section's design, no
> HUD trade needed (`CZ_VK_FOV_RAW=1` restores UI-scaling for A/B). "The
> game's vertical half-fov is atan(1/B)" holds with B = the composite's OWN
> ||row1|| (the gameplay camera is 41.64°, not the raw form's 45°, with a
> per-camera zoom). Null byte-identical; same-run flip is the verification
> form (gotcha 378 rewritten). Operator verdicts owed on the re-scoped slider
> AND on wide-mode gameplay, which the composite fix unstretched.

**What exists already**: the wide-mode projection patch (part 60) — a structural
recognizer for the title's 16:9 scene perspectives (`Is169Perspective`) and a patch
point in the VS constant copy where every recognized projection can be modified before
the shaders read it, with the memo reusing patched bytes and the verify arm patched to
match. A FOV slider is the same patch with a second factor.

**Design**: `fov=N` in cz_settings.txt, N in degrees of ADJUSTMENT from the game's own
camera (-10..+30 in steps of 1; 0 = OG, the default). Panel row "FIELD OF VIEW" as a
LEFT/RIGHT slider showing `OG` at 0 and `+N`/`-N` otherwise. The patch: for a
recognized perspective with x scale A and y scale B, the game's vertical half-fov is
atan(1/B); the patched B' = 1/tan(atan(1/B) + N/2 in radians), and A' scales by the
same ratio so the aspect is untouched (this composes cleanly with the wide-mode X
patch, which multiplies A separately — order them: fov first, wide second). Applied
LIVE (the constant copy runs every frame; the settings value is re-read per frame like
the shadow tier).

**The UCP compensation must scale too**: the clip planes are images of view-space
planes under the ORIGINAL projection; with B changing, both the x AND y coefficients
of published planes scale by their inverse factors (the part-60 wide compensation
generalized from one axis to two). Without it, gore cuts drift when the slider is off
zero.

**Known trades, stated in advance** (same class as 21:9): the title's CPU culling is
computed for ITS fov, so widening pops objects at the frame edge; the DoF/blur
footprints are screen-fraction and unaffected; cutscene cameras are recognized
perspectives too and WILL widen — if that looks wrong in play, gate the patch on
gameplay (investigate: does the cinematic camera's projection differ structurally?).

**Verify**: headless A/B on the attract backdrop — the flank-content measurement from
part 60 §6cp reused: +15° must show new geometry at all four edges vs OG; 0 must be
byte-identical to OG (the null). Operator: comfort pass, and a slice check with the
slider off-zero.

## 1. RT stage 0 — the capability probe and the honest refusal

> **DONE IN PART 61** (4cc4f4a). The probe runs at CreateDevice and stores
> `Renderer::rtSupported`; the operator's RTX 3070 (Ampere, driver 610.43.03)
> carries all three extensions — ray-query hybrid RT is AVAILABLE. Tier-budget
> class: mid-high Ampere; price tiers in stage 2 before promising HIGH. The
> CZ_VK_RT / CZ_VK_RT_FORCE arms land with the first RT row (nothing to kill
> yet).

Vulkan ray tracing needs `VK_KHR_acceleration_structure` +
`VK_KHR_ray_query` (the recommended retrofit form: ray queries from an ordinary
compute pass — no RT pipeline, no shader binding table; every major vendor recommends
ray query for hybrid effects) + `VK_KHR_deferred_host_operations` and
`bufferDeviceAddress`. Stage 0 is a probe at renderer init: log every missing piece,
and when absent the RT rows show "UNSUPPORTED" and refuse to leave OG — a row that
pretends is the gamma slider again. `CZ_VK_RT=0` env kills all RT whatever the file
says (the whole-feature control arm); `CZ_VK_RT_FORCE=1` overrides the probe for
driver experiments and is a diagnostic arm, never a fix.

**Measure before designing further** (evidence rule): the operator's GPU model and its
ray-query throughput class decide the tier budgets. One probe run + `vulkaninfo` grep
is the whole step.

## 2. RT stage 1 — the geometry investigation (no pixels change)

> **DONE IN PART 63 — the record with every number is `phase5-notes.md` §6cu,
> the instrument is `CZ_VK_RT_CENSUS=1`, and the verdict is GO for stage 2,
> cheaper than this section budgeted.** Highlights: world position data is
> **100% float3** at 28-32 B stride (the short4 possibility does not exist
> here); **96.8% triangle strips, 100% 16-bit indices**; **98.1% of world bytes
> are BLAS-once stable** (the rewritten set is CLOSED at 79 tiny streams,
> 1.2 MB); TLAS scale ~2,100-2,600 instances / ~1.2-1.4M unique triangles at
> the heaviest measured crowd; and the world-transform investigation below was
> never needed — §6cs had already shown world streams are WORLD-SPACE (identity
> instance transforms) and the census's bounds scan survived its refutation
> check. The skinned exclusion is STRUCTURAL: actors carry an affine at c0-3,
> so the composite gate is itself the rigid-world filter. BLAS input reuses the
> persist-cache's own buffers (one usage bit away); the only data
> transformation stage 2 needs anywhere is strip→list index expansion over
> 2.7 MB of cumulative u16 indices.

Everything RT needs geometry in WORLD space; everything this renderer has is guest
STREAMS in object space plus per-draw VS constants. The recommended retrofit shape
(the one RTX-Remix-class injectors use) is: **BLAS per stable mesh in object space,
TLAS instance per draw with the draw's own world transform** — and both halves map
onto machinery this port already has:

* **BLAS identity = the persist-cache identity** (address, size, content hash): the
  content guards already say per frame whether a stream's bytes changed. A stream
  whose guard holds steady across frames is a rigid mesh — BLAS built once, reused;
  a stream that changes every frame is skinned (zombies) and is EXCLUDED from stage
  1 (their RT shadows/AO come later or never — state it in the panel note, not in
  small print).
* **The per-draw world transform lives somewhere in the VS constant window** — c0-3
  proj and c12-14 view are known (pose_read.py); the world matrix's registers are
  NOT yet mapped. The investigation: capture two frames of constants for one moving
  prop (the F9 pose machinery extended to a named draw), diff, and bind the matrix
  by dataflow (the bind-symbol-tables-by-dataflow memory: ask what refutes the
  binding). Alternative already half-proven: many draws' positions are pre-baked to
  world (the big street meshes) — the census says which by comparing stream bounds
  against the known world bounds.
* **Vertex position format census**: the vertex declarations are already decoded per
  draw (the fetch machinery); census the position formats over an outdoor frame
  (float3 expected dominant; short4-scaled possible). A BLAS builder consumes
  exactly what the census returns, nothing speculative (gotcha 5).

Deliverable: a census tool + a doc section, zero renderer changes. This stage decides
whether stages 2-4 are cheap or dear, and its numbers replace guesses.

## 3. RT stage 2 — ray-traced SHADOWS (the first visible tier)

> **ROUTE (a) BUILT AND THEN ABANDONED IN PART 64 (2026-08-22); ROUTE (b) IS THE
> LIVE DIRECTION AND IS PART 65's SUBJECT, by the operator's decision.** The
> record is `phase5-notes.md` §6cv (§7j is the verdict); the hand-off is
> `part65-kickoff.md`; the backlog entry is `open-items.md` 0v.
>
> **THE FORK BELOW IS RESOLVED, AND NOT THE WAY THIS SECTION GUESSED.** It
> called (b) "the recommended route" and (a) "the fallback that touches no
> shader", and told the next session to "decide by trying (a) in an afternoon —
> it is measurable in stills". That instruction was exactly right and it is what
> settled it: (a) was built, its injection point was PROVEN (the fill
> experiment's two polarities, and a poison control landing on the fill's own
> number), and then **five independent knobs failed to make it correct**.
> Occluder set, union vs replacement, the light-matrix binding, a bounds gate,
> and a 6.7x bias sweep: every configuration lands at 64-66 median outdoor luma
> against OG's 80.61, with an all-shadow floor of 61.2.
>
> **The reason is structural and this section did not anticipate it**: writing
> the shadow MAP means every receiver inside the map is compared against itself,
> so it self-shadows, and route (a) has no receiver-side offset to apply. The
> operator named it in one sentence — *"shadow squares following where the
> player is and normal shadow still on"* — after three headless statistics had
> failed to. Route (b) does not have the defect BY CONSTRUCTION: the factor is
> computed per receiving pixel from that pixel's own position, so a surface
> cannot occlude itself.
>
> Everything (a) built is reusable by (b) unchanged — BLAS/TLAS, the sun-matrix
> capture and its dataflow binding, the pooled AS allocator, the arms, the `rt`
> profiler phase. **And the tier ladder below belongs to (b)**: on (a) the rungs
> were not even expressible, because a depth map cannot carry softness however
> many rays are fired.
>
> The panel presentation also changed on the operator's instruction, and the
> plan's header should be read with it: **not a separate RT row per effect for
> shadows, but ONE `SHADOW` row** whose values are LOW/MEDIUM/HIGH then RT
> LOW/MEDIUM/HIGH, where selecting an RT value REPLACES the raster shadow.
>
> ---
>
> *(the original part-64 banner follows, kept because its measured facts stand)*
> The fork below was decided by measurement in an afternoon, exactly as this
> section asked: **route (a) works and needs no shader patch at all** —
> `CZ_VK_SHADOW_FILL`'s two polarities move the outdoor median luma in opposite
> directions (80.61 -> 61.43 at 0.0, 81.46 at 1.0), so the atlas snapshot IS
> where the title's shadow term reads, and the convention is standard (near =
> occluder). The whole ray path was then shown to reach that term: the poison
> control (all-shadow written by the TRACE PIPELINE) lands on the direct fill's
> number to 0.25 luma.
>
> **What this section did NOT anticipate, and it is the open item**: the title's
> own cascade is **52.8% empty** — it draws CASTERS, not the world — while the
> collector traces everything the camera sees, so real rays read 63.72 against
> OG's 80.61 and the world shadows itself. An atlas diff (nearer on 49.6% of
> texels, farther on 1.3%) says we are a superset adding near geometry.
> `CZ_VK_RT_CASTERS=cascade` and `CZ_VK_RT_COVERAGE=1` are the arms built to
> close it; their first numbers are in §6cv.
>
> **And one thing to weigh before the next tier is designed**: tracing INTO the
> 4096x1024 atlas cannot beat the atlas's resolution, so route (a)'s ceiling is
> exact depths and missing occluders — not soft shadows and not per-pixel ones.
> The tier ladder below (MEDIUM = full-res, HIGH = soft) therefore belongs to
> route (b), and every piece part 64 built (BLAS, TLAS, the sun-matrix capture,
> the arms, the `rt` profiler phase) is reusable by it unchanged. The panel row
> ships OG / RT LOW only and refuses MED/HIGH at the setter until the ladder is
> priced on the operator's machine, which is what §6 asks for.

First because it needs the least: one ray per pixel toward one light, and the light
is KNOWN — the cascade pass's view matrix is the sun's (vc12-14 of the shadow pass,
already understood by pose_read). The recommended hybrid form:

* A compute pass after the scene's depth is resolved: reconstruct world position from
  depth (the projection's zn/zf are derived — clip_plane_space.py: zn 0.1, zf 1000)
  + normal from depth derivatives, ray-query toward the sun direction against the
  TLAS, write a shadow factor texture.
* **Composite by replacing the sampled atlas**: the title fetches the 4096x1024
  cascade atlas (the most-fetched texture in the frame); serving the shadow factor
  through the EXISTING shadow term means the title's own lighting math applies it —
  no shader patching. The investigation half: the atlas holds DEPTH the shader
  compares, not a factor — so the injection point is either (a) a synthesized atlas
  whose depths force the title's comparison to our answer (write 0/far per texel —
  crude but shader-untouched), or (b) patching the ~dozen shadow-sampling PS to read
  our factor texture instead (XenosRecomp emits them; a targeted patch keyed on the
  atlas fetch slot). (b) is the recommended route; (a) is the fallback that touches
  no shader. Decide by trying (a) in an afternoon — it is measurable in stills.
* Tiers: LOW = half-res, 1 ray, temporal accumulation only; MEDIUM = full-res 1 ray +
  spatial filter; HIGH = full-res, soft shadows (cone-sampled sun radius, 2-4 rays).
* Skinned actors are not in the TLAS in stage 2: their shadows stay OG (the cascade
  path still renders — RT REPLACES THE ATLAS CONTENT, not the pass, so the OG
  machinery remains the fallback per tier and per failure).

**Arms and gates**: `CZ_VK_RT_SHADOWS=0|1|2|3` env over the file row; the null is OG
vs RT-off byte-identical; the positive control is a poisoned shadow factor (all-black)
that must darken the whole frame (the CZ_VK_CUBE_POISON pattern). Operator judges the
look; headless judges engagement counters + validation clean.

## 4. RT stage 3 — ray-traced AMBIENT OCCLUSION

One tier row "AMBIENT OCCLUSION: OG / RT LOW / MED / HIGH". Hemisphere ray queries
(cosine-weighted, 1/2/4 rays by tier) from depth-reconstructed position+normal,
temporal accumulation (the frames-in-flight machinery already double-buffers),
bilateral spatial filter at MED+. **Composite: multiply into scene color as a post
pass before the title's tone map** — the injection point is the resolve chain, where
the scene surface identity is already tracked (the front-snapshot machinery). The
title has no SSAO of its own to disable, so OG really is "AO off" — the row is
honest by construction.

## 5. RT stage 4 — ray-traced LIGHTING and GI (one row, the research stage)

The instruction names both "ray traced lighting" and "ray traced global illumination";
implemented as one row "LIGHTING: OG / RT LOW / MED / HIGH" because they share
everything: light extraction + a per-pixel radiance pass + a composite that must
coexist with the title's own lighting.

* **Light extraction is the hard investigation**: the title's point/spot lights live
  in PS constants in an unmapped layout. The route: census PS constant windows over
  scenes with obviously distinct lighting (day street vs the dark pawnshop vs neon
  signs at night), diff, bind by dataflow. If the layout resists, HIGH-tier GI can
  still ship SUN-ONLY (one bounce off the sun via ray query — visually most of Still
  Creek's daylight) — state the reduced scope in the plan record, not silently.
* LOW = RT sun bounce at half res (1 bounce, 1 ray, heavy temporal); MED = full res +
  extracted primary lights; HIGH = 2 bounces or per-light shadow rays, budget
  permitting. Composite additively pre-tone-map with an exposure-matched scale — the
  first version WILL double-light the scene (the title's baked lightmaps stay in its
  textures); the honest mitigation is a dampening factor derived by matching a
  reference frame's mean luma, recorded as the approximation it is.
* This stage is allowed to conclude "not worth shipping" — pre-register the kill
  threshold (the part-55 rule): if HIGH cannot beat OG in a side-by-side the operator
  actually prefers, the row ships as shadows+AO only and the record says why.

## 6. Cross-cutting rules (all stages)

* **One settings row per effect**, values OG/RT LOW/RT MEDIUM/RT HIGH, OG default;
  rows show UNSUPPORTED (and refuse to move) when the probe failed. Live where
  cheap (tier changes re-read per frame like the shadow tier); a stage that needs
  restart says "next launch" like the resolution row.
* **Env arms win over the file** for every row (the standing rule), and `CZ_VK_RT=0`
  is the master control arm: with it set the binary must be instruction-path
  identical to OG.
* **Performance discipline**: every stage lands with its CZ_VK_PROFILE phase (an
  `rt` column), a frame-time A/B at the operator's load (three runs an arm, medians,
  the pinned-share statistic — the part-59 rules), and the tier ladder priced in ms
  on THEIR machine before the operator is asked for a verdict.
* **The retire machinery is the allocator**: BLAS/TLAS buffers churn with streaming;
  they go through the part-60 deferred-retire path from day one — the shadow-tier
  freeze is not to be re-bought with acceleration structures.
* **Validation runs at every stage close** (CZ_VK_VALIDATION with the RT extensions
  enabled), and the PM4 oracles are untouched by all of this (no command-processor
  changes anywhere in the plan).

## 7. Order of landing

FOV slider -> stage 0 probe -> stage 1 census -> stage 2 shadows (LOW first, alone)
-> stage 3 AO -> stage 4 lighting/GI. Each is its own part-sized chunk; each ends
with an operator session and a kickoff update. Nothing in stages 2-4 starts until
stage 1's census numbers exist — the plan's budgets are guesses until then, and the
project has paid for building on a guessed budget before (gotcha 362's lesson: run
the cheapest measurement before building the mechanism).
