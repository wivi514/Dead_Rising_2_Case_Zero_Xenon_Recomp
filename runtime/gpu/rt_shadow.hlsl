// RT stage 2 (part 64) — the shadow-atlas TRACE pass, route (a) of
// rt-and-fov-plan.md §3: replace the rasterized cascade's DEPTHS with ray-traced
// ones, so the title's own shadow comparison — which this renderer already serves
// through the atlas snapshot — produces ray-traced shadows without touching a
// single translated shader.
//
// One fullscreen triangle is drawn over the just-resolved cascade slice with this
// pixel shader writing SV_Depth and the pipeline's depth test set to LESS against
// the raster content: a traced hit only lands where it is NEARER (to the sun) than
// what the cascade rasterized, so the result is the UNION of the two occluder sets.
// That is what keeps the exclusions honest — skinned actors and alpha-tested
// foliage are not in the TLAS, and the raster cascade still carries their shadows.
//
// The ray construction leans on one measured fact (phase5-notes §6cs/§6cu): a
// cascade draw's c0-3 is the sun's ORTHO view-projection composite and world
// geometry feeds it in world space with the identity transform. An ortho composite
// is AFFINE in z, so a ray traced from the slice texel's NDC at z=0 to the same
// texel at z=1 has its hit's NDC depth equal to the ray parameter t — no forward
// matrix, no viewport algebra in the shader. The CPU side inverts the captured
// matrix and passes the inverse's rows.
//
// Compiled by tools/build_rt_shaders.sh (XenosRecomp's own DXC) into
// rt_shadow_spv.h, which is committed — the runtime build does not need DXC.

struct Push
{
    float4 invRow0;   // rows of the INVERSE light view-projection (clip -> world)
    float4 invRow1;
    float4 invRow2;
    float4 invRow3;   // unused by the math below (ortho w == 1) but kept whole so a
                      // future perspective light needs no layout change
    float4 region;    // x, y, w, h — the slice's rectangle in host pixels
    float4 misc;      // x = depth bias in NDC-z units, y = poison (1 = write the
                      //     all-shadow value everywhere, the positive control),
                      //     z = inverted-depth convention (CZ_VK_SHADOW_FILL's
                      //     experiment decides which polarity means "occluded"),
                      //     w unused
};
[[vk::push_constant]] Push pc;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;

float4 VsMain(uint vid : SV_VertexID) : SV_Position
{
    // The standard three-vertex fullscreen triangle; the viewport/scissor are the
    // slice region, so no geometry math is needed here.
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float PsMain(float4 fragPos : SV_Position) : SV_Depth
{
    const bool invert = pc.misc.z != 0.0;
    if (pc.misc.y != 0.0)
        return invert ? 1.0 : 0.0;   // POISON: everything shadowed. Positive control.

    // Fragment position -> this slice's NDC. y: the raster path maps NDC +1 to the
    // slice's top row (the negative-height viewport in DoDraw), so the trace pass
    // must agree or its shadows land vertically mirrored.
    float2 uv = (fragPos.xy - pc.region.xy) / pc.region.zw;
    float ndcx = uv.x * 2.0 - 1.0;
    float ndcy = 1.0 - uv.y * 2.0;

    // Clip -> world at the near and far ends of the light volume. Ortho: w == 1.
    float4 c0 = float4(ndcx, ndcy, 0.0, 1.0);
    float4 c1 = float4(ndcx, ndcy, 1.0, 1.0);
    float3 p0 = float3(dot(pc.invRow0, c0), dot(pc.invRow1, c0), dot(pc.invRow2, c0));
    float3 p1 = float3(dot(pc.invRow0, c1), dot(pc.invRow1, c1), dot(pc.invRow2, c1));

    RayDesc ray;
    ray.Origin = p0;
    ray.Direction = p1 - p0;   // unnormalized on purpose: t IS the NDC depth
    ray.TMin = 0.0;
    ray.TMax = 1.0;

    // FORCE_OPAQUE: the TLAS holds only draws the collector already screened to be
    // opaque depth-writers, and any-hit shading does not exist on this path.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float d = saturate(q.CommittedRayT() + pc.misc.x);
        return invert ? 1.0 - d : d;
    }
    // Miss = "no occluder" = the far value; the depth test then keeps whatever the
    // raster cascade put there, so the union of occluder sets costs nothing extra.
    return invert ? 0.0 : 1.0;
}
