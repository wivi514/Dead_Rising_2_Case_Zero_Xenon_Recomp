// RT stage 2, ROUTE (B) (part 65) — the SCREEN-SPACE SHADOW FACTOR pass.
//
// Route (a) — part 64 — wrote ray-traced depths into the cascade atlas the title
// samples, and could not be made correct: writing the MAP means every receiver inside
// the map is compared against itself, with no receiver-side offset available. Five
// independent knobs all landed at 64-66 median outdoor luma against the original's
// 80.61 (docs/phase5-notes.md §6cv §7j).
//
// This pass computes the shadow term per RECEIVING PIXEL instead. The ray starts at the
// receiving surface and is pushed off it along its own view/sun offset, so a surface
// cannot shadow itself — the defect is impossible by construction rather than tuned
// away. tools/patch_rt_shadow_hlsl.py then redirects the 140 atlas taps the census
// found (config/rt_shadow_slots.json) to read what this pass wrote, at their own
// SV_Position.
//
// It runs as a fullscreen-triangle FRAGMENT pass writing one R8 target, for the same
// reason part 64's trace pass is a fragment pass: `VK_KHR_ray_query` works in any
// stage, and reusing the graphics path means no compute pipeline layout, no separate
// queue reasoning, and one shape of code to read.
//
// WHERE THE RECEIVER COMES FROM — and this is the whole of part 66.
//
// ~~WHEN IT RUNS, and why that is not a heuristic: at the first draw of a pass that
// samples the atlas. This title issues a real Z PREPASS — 233,155 depth-only draws
// against 148,150 colour-mode ones over a boot (§6u) — so by the time its own first
// shadow-sampling draw is recorded, the depth buffer holds the finished scene
// depth.~~ **RETRACTED, against hardware, by tools/rt_depth_order_census.py.**
//
// That count was over a whole BOOT and says nothing about ORDER inside a frame. Walked
// in stream order across all twenty `.xtr` world traces, the picture is the opposite:
// the 233,155 depth-only draws are the SHADOW CASCADE (EDRAM depth base 0, pitch 1040,
// RB_MODECONTROL 5, colour mask 0 — ~969 a frame), and the scene pass (base 736, pitch
// 640) has **NO prepass at all**. In every trace the FIRST draw of the scene pass
// already samples the cascade atlas, with 0 depth-writing draws before it and ~5,200
// (about 2.0 million vertices) after it.
//
// So when this pass fires, the scene depth buffer is at its CLEAR VALUE. That is
// exactly, and completely, what part 65's ladder measured: mode 8's dither said the
// sampled depth does not vary and mode 9's mean said the value is 1.0. The descriptor
// was never the suspect it looked like.
//
// The fix cannot be a better trigger, because no moment exists at which the depth is
// both complete and still ahead of the draws that need it. It is to stop needing the
// depth buffer: fire a PRIMARY RAY from the camera through the pixel into the same
// TLAS the shadow ray uses, and take its closest hit as the receiver. The TLAS is
// built from the PREVIOUS frame's draws (`rtshadow::g_prevKeys`), so it is fully
// populated at the moment the pass runs, whatever the title's draw order is — the
// defect is impossible by construction rather than timed around, which is the same
// argument that chose route (b) over route (a).
//
// What it costs, stated rather than hidden: the TLAS holds only opaque depth-writing
// draws, so a pixel covered by a skinned actor or by alpha-tested foliage receives the
// factor of the opaque surface BEHIND it. Those meshes already cast no RT shadow
// (§2 of the part-66 hand-off), so the tier is consistent with itself; adding them is
// the MED/HIGH feature it always was, and it fixes both halves at once.
//
// `CZ_VK_RT_FACTOR_SOURCE=depth` restores the depth-buffer reconstruction as a
// same-binary control arm.
//
// Compiled by tools/build_rt_shaders.sh (XenosRecomp's own DXC) into rt_factor_spv.h,
// which is committed — the runtime build does not need DXC.

struct Push
{
    // The inverse of the SCENE view-projection, row-major, so
    // world_i = dot(invRow_i, clip) / dot(invRow3, clip). Unlike the light matrix this
    // one is a PERSPECTIVE composite, so the w divide is not optional.
    float4 invRow0;
    float4 invRow1;
    float4 invRow2;
    float4 invRow3;
    float4 sun;      // xyz = unit direction TOWARD the sun; w = ray length in world units
    float4 params;   // x = origin bias along the sun, y = bias toward the camera,
                     // z = poison (1 = write all-shadow everywhere: the positive
                     //     control, which must darken the world),
                     // w = rays (1, 2 or 4) x 1000 + the sun's angular radius in
                     //     radians — packed because 112 bytes is the guaranteed push
                     //     constant size and this was the last field
    float4 view;     // xy = 1 / factor-image size (SV_Position -> uv);
                     // z  = CZ_VK_RT_FACTOR_DEBUG mode (0 = off);
                     // w  = WHERE THE RECEIVER COMES FROM. 0 = the scene depth buffer
                     //      (part 65's route, kept as the control arm), 1 = a PRIMARY
                     //      RAY into the TLAS. See the header note above PsMain.
    float4 camera;   // xyz = camera world position (for the toward-camera bias);
                     // w   = the depth image's uv.y scale. `uv` below is normalised over
                     //       the VIEWPORT, because that is the space the patched shaders
                     //       look up in; the EDRAM depth attachment is TALLER than the
                     //       viewport (padded for the cascade), so the control arm's
                     //       depth sample has to be scaled back into it. The last free
                     //       float in the guaranteed 128-byte block.
};
[[vk::push_constant]] Push pc;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::binding(1, 0)]] Texture2D<float4> g_depth;
[[vk::binding(2, 0)]] SamplerState g_point;
// THE DEPTH PROBE'S OWN CONTROL (part 65). Three probes agreed the sampled depth is
// uniformly 1.0, and every one of them shares a single untested assumption: that this
// pass can sample an EDRAM attachment at all. The colour buffer is the same kind of
// image, sampled through the same descriptor set, the same sampler and the same uv — but
// its contents are known to vary, because it is the picture. If mode 12 is uniform too,
// the fault is the sampling path and not the depth; if it varies, the depth image really
// does read far everywhere and the question moves back to WHEN we read it.
// An instrument must not depend on its subject, and until now this one did.
[[vk::binding(3, 0)]] Texture2D<float4> g_colour;

float4 VsMain(uint vid : SV_VertexID) : SV_Position
{
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float PsMain(float4 fragPos : SV_Position) : SV_Target0
{
    if (pc.params.z != 0.0)
        return 0.0;                       // POISON: everything shadowed. Positive control.

    // uv is over the VIEWPORT — the same space `Publish` puts the patched shaders in.
    // Getting these two ends into different spaces is the defect the operator found on
    // the first arm of part 66's first session; see EnsureResources in vk_renderer.cpp.
    float2 uv = fragPos.xy * pc.view.xy;
    // ...and the depth attachment is taller than the viewport, so its own sample needs
    // the ratio back. Only the control arm reads this.
    float z = g_depth.SampleLevel(g_point, float2(uv.x, uv.y * pc.camera.w), 0.0).x;

    // CZ_VK_RT_FACTOR_DEBUG — THE LADDER THAT SPLITS THIS PASS INTO ITS LINKS.
    //
    // It exists because "the world darkens under poison but nothing shadows with a real
    // factor" says the factor is ~1.0 everywhere, and that has three possible causes in
    // series: the DEPTH READ, the WORLD RECONSTRUCTION, or the RAY. A frame is the same
    // picture under all three, so no amount of looking at it can separate them. Each
    // mode is a positive control for exactly one link and states what a PASS looks like:
    //
    //   1  depth mask      — everything with geometry goes black, the sky stays lit.
    //                        PASS = a black world under a lit sky. If nothing darkens,
    //                        the depth sample is not returning the scene's depth and
    //                        nothing downstream can work.
    //   2  world checker   — a 4-unit checkerboard in WORLD space. PASS = a regular grid
    //                        painted on the ground that stays PINNED to the world as the
    //                        camera moves. If it swims with the camera, or the squares
    //                        are wildly uneven, the inverse view-projection is wrong.
    //                        The scale is deliberately the same order as the light
    //                        volume (~106 units), so a wrong world SCALE shows as
    //                        squares that are absurdly large or invisible.
    //   3  unbiased rays   — the ray path with zero origin offset and a 10x length.
    //                        PASS = heavy over-shadowing (self-hits everywhere), which
    //                        is what an unbiased shadow ray is supposed to produce. If
    //                        this is still fully lit, the rays miss regardless of bias
    //                        and the fault is in the TLAS or the ray construction.
    //   4  selector control — return all-shadow unconditionally, WITHOUT reading the
    //                        depth. It is poison reached through the debug parameter
    //                        instead of the poison one, which is the only way to tell
    //                        "the depth read is broken" from "the debug selector never
    //                        arrived": those two produce the same unchanged frame under
    //                        modes 1-3, and an instrument that cannot fail visibly is
    //                        not an instrument (gotcha 30).
    //   5,6  depth BYPASS — reconstruct from a FIXED clip z (0.99 / 0.999) instead of
    //                        the sampled one, so the world position, the TLAS and the
    //                        ray are exercised with the depth read taken out of the
    //                        chain. PASS = the frame darkens somewhere. That separates
    //                        "the depth read is the only broken link" from "everything
    //                        downstream is broken too", which modes 1 and 3 cannot,
    //                        because a bad depth and a bad reconstruction both end in
    //                        rays that hit nothing.
    //   7  straight down    — the ray fires at (0,-1,0) from the reconstructed point
    //                        with a 10x length, independent of the sun. Nearly every
    //                        pixel sits above ground, so PASS = heavy darkening. It is
    //                        the TLAS's own control: if 5/6 darken and 7 does not, the
    //                        structure is empty rather than the direction wrong.
    const int dbg = int(pc.view.z);
    if (dbg == 4)
        return 0.0;
    if (dbg == 1)
        return z >= 0.999999 ? 1.0 : 0.0;
    if (dbg == 5) z = 0.99;
    if (dbg == 6) z = 0.999;
    //   8  does z VARY?   — a 1/1024 dither on the sampled depth. If z changes at all
    //                       across the screen this is a strong half-and-half pattern and
    //                       the luma lands BETWEEN the all-lit and all-shadow arms; if z
    //                       is constant it is uniformly one or the other. It needs to
    //                       know nothing about the depth's scale or convention, which is
    //                       what makes it usable when both are in doubt.
    //   9  what IS z?     — return the sampled depth as the factor, so the frame's own
    //                       shadow term becomes proportional to it and the MEAN DEPTH
    //                       falls out of meanLuma. Reading a buffer's contents through a
    //                       statistic the harness already collects, instead of building
    //                       a readback for it.
    //  14  THE MISSING CONTROL: a pure SCREEN-SPACE STRIPE, touching no texture at
    //      all. Every arm that has ever worked here — poison, and the selector control
    //      — writes a UNIFORM factor, and every arm that failed writes a factor that
    //      VARIES across the screen. That pattern was staring at this ladder for hours
    //      and none of its modes tested it: if the patched shaders read our factor at a
    //      constant or wrong coordinate, a uniform factor still lands perfectly and a
    //      varying one averages away to nothing. PASS = obvious banding. FAIL = the
    //      round trip cannot carry spatial detail and no ray, depth or matrix is at
    //      fault.
    //  13  the colour control, as a BINARY question. Mode 12 returned the colour's
    //      luminance and could not discriminate: a working sample returning a realistic
    //      ~0.3 and a broken one returning 0.0 both land near the all-shadow arm. Asking
    //      "is there ANY colour here" separates them — working reads as LIT, broken as
    //      shadowed.
    if (dbg == 14)
        return frac(uv.x * 8.0) < 0.5 ? 0.0 : 1.0;
    //  19  THE VERTICAL TWIN OF 14, and the control that would have saved a session.
    //      Mode 14 asked "does the round trip carry SPATIAL detail" with a horizontal
    //      stripe and answered yes — while every row of the factor was being read
    //      1440/2048 of the way up the frame, because a horizontal pattern is invariant
    //      under a vertical error. PASS = eight clean HORIZONTAL bands, and — the part
    //      that matters — the SAME eight bands as mode 14 rotated, in the same places.
    //      Run 14 and 19 together; agreeing is the result, either one alone is not
    //      (gotcha 394).
    if (dbg == 19)
        return frac(uv.y * 8.0) < 0.5 ? 0.0 : 1.0;
    //  15, 16  IS THE DESCRIPTOR REAL? `GetDimensions` reads the EXTENT, which comes
    //      from the descriptor and not from the image's contents, so it separates "the
    //      descriptor references a real image whose contents are genuinely far or
    //      black" from "the descriptor references nothing" — the one thing modes 1, 8,
    //      9, 12 and 13 all share and none of them can test. PASS = the frame goes
    //      DARK (a bound image reports a width). Part 66 added these after finding
    //      `g_colour` had never been written at all: the pass allocated a
    //      three-element VkWriteDescriptorSet array and passed a count of TWO, which
    //      the validation layer had been reporting since the feature was built
    //      (`VUID-vkCmdDraw-None-08114`, "has never been updated"). Modes 12 and 13
    //      were reading an unwritten descriptor, so their readings say nothing.
    if (dbg == 15 || dbg == 16)
    {
        uint dw, dh;
        if (dbg == 15) g_depth.GetDimensions(dw, dh);
        else           g_colour.GetDimensions(dw, dh);
        return dw > 16 ? 0.0 : 1.0;
    }
    if (dbg == 13)
    {
        float3 c = g_colour.SampleLevel(g_point, uv, 0.0).rgb;
        return (c.r + c.g + c.b) > 0.003 ? 1.0 : 0.0;
    }
    if (dbg == 12)
    {
        float3 c = g_colour.SampleLevel(g_point, uv, 0.0).rgb;
        return saturate(dot(c, float3(0.299, 0.587, 0.114)));
    }
    if (dbg == 8)
        return frac(z * 1024.0) < 0.5 ? 0.0 : 1.0;
    if (dbg == 9)
        return saturate(z);

    // The renderer's viewport has NEGATIVE height, so NDC +1 is the TOP row and a
    // fragment's uv.y runs down from there. Getting this backwards mirrors every
    // shadow vertically, which is a thing that looks plausible in a still.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float3 world;
    // Mode 11 reports this and it exists only on the depth route; hoisted rather than
    // deleted so the control arm keeps every probe it had.
    float pdiv = 0.0;
    // Modes 17 and 18 force the primary path whatever the source is set to: an
    // instrument that silently does nothing under half its configurations is not an
    // instrument, and the depth source is exactly the configuration someone would pair
    // with them by accident.
    if (pc.view.w != 0.0 || dbg == 17 || dbg == 18)
    {
        // ---- THE PRIMARY RAY. The receiver is the closest TLAS hit along the camera
        // ray through this pixel, which needs no depth buffer and therefore has no
        // moment at which it is too early to run. Unproject the pixel at both ends of
        // the clip volume: the near point is the origin, and the segment between them
        // is exactly the view frustum's extent at this pixel, so TMax needs no guess.
        float4 cN = float4(ndc, 0.0, 1.0);
        float4 cF = float4(ndc, 1.0, 1.0);
        float wN = dot(pc.invRow3, cN);
        float wF = dot(pc.invRow3, cF);
        if (abs(wN) < 1e-12 || abs(wF) < 1e-12)
            return 1.0;
        float3 pN = float3(dot(pc.invRow0, cN), dot(pc.invRow1, cN),
                           dot(pc.invRow2, cN)) / wN;
        float3 pF = float3(dot(pc.invRow0, cF), dot(pc.invRow1, cF),
                           dot(pc.invRow2, cF)) / wF;
        float3 seg = pF - pN;
        float segLen = length(seg);
        if (!(segLen > 1e-6))
            return 1.0;

        RayDesc pr;
        pr.Origin = pN;
        pr.Direction = seg / segLen;
        pr.TMin = 0.0;
        pr.TMax = segLen;
        // NOT `ACCEPT_FIRST_HIT_AND_END_SEARCH`: the shadow ray wants any hit, the
        // primary ray wants the CLOSEST one, and using the shadow ray's flags here
        // would place the receiver on whichever triangle the traversal reached first.
        RayQuery<RAY_FLAG_FORCE_OPAQUE> pq;
        pq.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, pr);
        // A LOOP, not a single call. `ACCEPT_FIRST_HIT_AND_END_SEARCH` lets the shadow
        // ray below get away with one Proceed(); a closest-hit query must be driven to
        // completion or it reports whatever traversal had reached when it stopped.
        while (pq.Proceed())
            ;
        bool hit = pq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

        //  17  DOES THE PRIMARY RAY FIND THE WORLD? `hit ? shadow : lit`. PASS = the
        //      world turns black and the sky stays lit — the same picture mode 1 was
        //      supposed to produce and never did. THIS IS THE GATE: no build goes to
        //      the operator until this arm lands near the all-shadow calibration
        //      (90.2) rather than near the all-lit one (99.9). Part 65 handed over
        //      three builds without meeting the equivalent gate and spent three of the
        //      operator's sessions learning it was not met.
        //  18  HOW FAR is the hit — saturate(t / 500). A gradient that gets darker
        //      toward the camera, i.e. a depth image made entirely of rays. It
        //      separates "the rays hit something" from "the rays hit the right thing":
        //      a TLAS full of junk geometry at the origin reads uniformly black here
        //      while mode 17 still passes.
        if (dbg == 17)
            return hit ? 0.0 : 1.0;
        if (dbg == 18)
            return hit ? saturate(pq.CommittedRayT() / 500.0) : 1.0;

        if (!hit)
            return 1.0;                   // sky, or geometry the TLAS does not carry
        world = pN + pr.Direction * pq.CommittedRayT();
    }
    else
    {
        // ---- THE DEPTH BUFFER, part 65's route. Kept as the same-binary control arm
        // (`CZ_VK_RT_FACTOR_SOURCE=depth`) and NOT as a fallback: on this title it is
        // known to read its clear value at the moment this pass runs, so a silent
        // fallback to it would be a silent fallback to "no shadows".
        //
        // Nothing was drawn here — sky, or a region this pass ran ahead of.
        // 1.0 is LIT, which is the honest failure: the frame loses a shadow rather
        // than gaining a black one.
        if (z >= 0.999999)
            return 1.0;
        float4 clip = float4(ndc, z, 1.0);
        pdiv = dot(pc.invRow3, clip);
        if (abs(pdiv) < 1e-12)
            return 1.0;
        world = float3(dot(pc.invRow0, clip), dot(pc.invRow1, clip),
                       dot(pc.invRow2, clip)) / pdiv;
    }

    //  10  HOW BIG is the reconstructed position — saturate(length(world)/1000). The
    //      town is ~1,100 units across (§6cu), so a correct reconstruction lands
    //      mid-range. Reading all-dark means the positions collapse to the origin;
    //      all-lit means they blow up. Both are what a wrong matrix convention or a
    //      near-zero perspective divide look like, and neither is distinguishable from
    //      the other — or from a broken depth read — in the frame itself.
    //  11  the perspective divide's own denominator, saturate(|w|). If this is ~0 the
    //      division that follows produces infinities whatever the rest of the matrix is.
    //      DEPTH SOURCE ONLY — the primary ray has no perspective divide, and reads 0
    //      (all-shadow) there, which is the honest answer rather than a plausible one.
    if (dbg == 10)
        return saturate(length(world) / 1000.0);
    if (dbg == 11)
        return saturate(abs(pdiv));
    if (dbg == 2)
    {
        float3 c = floor(world / 4.0);
        return frac((c.x + c.y + c.z) * 0.5) > 0.25 ? 1.0 : 0.0;
    }

    // TWO OFFSETS, and they answer different failure modes. Along the SUN: the standard
    // shadow-acne offset, the one route (a) had no place to put. Toward the CAMERA: a
    // depth-buffer reconstruction is only as precise as D24 at this range, and the
    // reconstructed point can sit fractionally BEHIND the surface it came from, which
    // no sun-side offset can rescue at a grazing sun.
    float3 toCam = pc.camera.xyz - world;
    float camLen = max(length(toCam), 1e-6);
    float bias0 = (dbg == 3 || dbg == 7) ? 0.0 : pc.params.x;
    float bias1 = (dbg == 3 || dbg == 7) ? 0.0 : pc.params.y;
    float3 origin = world + pc.sun.xyz * bias0 + (toCam / camLen) * bias1;

    // THE TIER, and what it buys. One ray is a hard shadow. Two or four spread across
    // the sun's angular radius give a real penumbra — which the patched shaders can
    // actually carry, because the weight substitution turns the title's own 2x2 PCF
    // into a plain mean and therefore into a five-level quantisation of whatever
    // continuous value arrives (tools/patch_rt_shadow_hlsl.py's header).
    //
    // The cone offsets are FIXED, not jittered per pixel. This renderer has no temporal
    // accumulation to resolve noise into shading, and per-pixel jitter would also break
    // the frame-determinism the A/B tooling depends on (tools/frame_determinism.py).
    // Fixed offsets band the penumbra instead, which is visible and honest.
    int rays = int(pc.params.w / 1000.0);
    float cone = pc.params.w - float(rays) * 1000.0;
    rays = clamp(rays, 1, 4);

    // A tangent basis around the sun direction, built from whichever axis is least
    // aligned with it so the cross product cannot be degenerate.
    float3 up = abs(pc.sun.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tx = normalize(cross(up, pc.sun.xyz));
    float3 ty = cross(pc.sun.xyz, tx);
    const float2 kOffsets[4] = { float2(0.0, 0.0), float2(0.75, 0.0),
                                 float2(-0.375, 0.65), float2(-0.375, -0.65) };

    float lit = 0.0;
    for (int i = 0; i < rays; ++i)
    {
        float3 dir = normalize(pc.sun.xyz + (tx * kOffsets[i].x + ty * kOffsets[i].y) * cone);
        if (dbg == 7)
            dir = float3(0.0, -1.0, 0.0);
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = 0.0;
        ray.TMax = (dbg == 3 || dbg == 7) ? pc.sun.w * 10.0 : pc.sun.w;
        // FORCE_OPAQUE: the TLAS holds only draws the collector screened to be opaque
        // depth-writers, and there is no any-hit shading on this path.
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, ray);
        // Documented to return false on the first call under FORCE_OPAQUE +
        // ACCEPT_FIRST_HIT, so one call is the usual shadow-ray idiom — but this is the
        // one link part 66's session has NOT been able to exonerate, and a loop costs
        // nothing on a query that terminates immediately. Not claimed as a fix.
        while (q.Proceed())
            ;
        lit += q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }
    return lit / float(rays);
}
