// THE DRAW-ID PIXEL SHADER: paint the draw's own index instead of its colour.
//
// WHY THIS EXISTS (part 39). Every defect this port has chased in the picture has been
// identified by INFERENCE — "the shard tree is probably the shader that binds a DXT5 and
// a DXN, because leaf cards usually do". That inference was wrong twice in two sessions;
// the last time it selected a HAIR material and an entire investigation was built on it
// (gotchas 291, 302). Nothing in the runtime could answer the only question that matters
// when an operator points at something: WHICH DRAW PAINTED THAT PIXEL.
//
// This shader answers it. Bound in place of the translated pixel shader for one frame,
// with blending forced off and depth left alone, every draw writes its own index into
// the colour attachment. The scene resolve then carries an image whose every pixel IS a
// draw number, and `tools/drawid_read.py` turns a pixel coordinate into the census line
// for that draw.
//
// The push-constant block MUST match the one the translated shaders declare, because the
// pipeline layout is shared: three 64-bit device addresses at offsets 0, 8 and 16, then
// the draw index this shader adds at 24. They are declared here as uint4 + uint2 so the
// file needs no 64-bit integer support, which keeps it compilable by the same DXC
// invocation the shader cache uses.
//
// Regenerate with tools/gen_drawid_shader.sh; the result is checked in as
// runtime/gpu/drawid_ps_spv.h so an ordinary build needs no shader compiler.

struct PushConstants
{
    uint4 addrLo;     // offsets  0..15 — vs and ps constant buffer addresses
    uint2 addrHi;     // offsets 16..23 — shared constants address
    uint  drawIndex;  // offset  24     — ours
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

float4 main() : SV_Target0
{
    // +1 so that ZERO means "no draw touched this pixel". A cleared background and
    // draw 0 would otherwise be the same value, and draw 0 is a real draw.
    const uint id = g_push.drawIndex + 1;
    // 24 bits over three 8-bit channels, low byte first. The colour attachment is UNORM,
    // so each channel round-trips exactly through /255 and round(v * 255) — which is why
    // this encodes bytes rather than, say, a float ramp.
    return float4(float(id & 0xFF) / 255.0,
                  float((id >> 8) & 0xFF) / 255.0,
                  float((id >> 16) & 0xFF) / 255.0,
                  1.0);
}
