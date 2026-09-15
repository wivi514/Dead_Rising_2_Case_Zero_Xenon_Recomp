// THE DISPLAY GAMMA RAMP, applied at present (part 119).
//
// The title loads a 256-entry 10:10:10 table into the display controller through the
// ring at boot — 256 type-0 writes of DC_LUT_30_COLOR (0x1925), each followed by a
// COND_WRITE that bumps DC_LUT_RW_INDEX — and hardware routes the 8-bit front buffer
// through it on the way to the screen. The table is Direct3D's own doing, not the
// game's: `sub_8284D5C0` builds it from `VdGetCurrentDisplayGamma`'s answer, and for the
// (type 2, 2.2222) this runtime and Xenia both report it is rec709_encode(srgb_decode(x))
// — a re-encoding of sRGB content for a TV's transfer, which DARKENS: 32/255 -> 16.5,
// 128 -> 115, white untouched. For a type-1 (sRGB) display Direct3D loads IDENTITY
// (checked: `sub_8284D460(x,1)` then `(x,0)` is decode-then-encode). Xenia applies the
// table at swap (`apply_gamma_table.xesli`, BSD-3 — docs/lighting-plan-part119.md §2.2
// records the licence); its screenshots are therefore table[front buffer], and
// tools/xtr_frame_extract.py closes that identity to within a level on the R4 set.
//
// This pass is the same lookup: one texel in, `table[round(c * 255)] / 1023` out, per
// channel. DC_LUT_30_COLOR packs blue in bits 0:9, green in 10:19, red in 20:29. The
// table arrives as 64 uint4 (256 dwords) in a uniform buffer; the source is Loaded at
// the fragment's own pixel, so no sampler is involved and no filtering can blur the
// lookup.
//
// It is an ARM (`CZ_VK_GAMMA_RAMP=1`), OFF by default, because the measurement that
// motivated it points the other way — see docs/phase5-notes.md §6fb: the ramp makes
// the picture DARKER, and the operator's report is that ours is already too dark.
// Compiled by tools/build_rt_shaders.sh (XenosRecomp's own DXC) into gamma_ramp_spv.h,
// which is committed — the runtime build does not need DXC.

[[vk::binding(0, 0)]] Texture2D<float4> g_src;
[[vk::binding(1, 0)]] cbuffer Ramp
{
    uint4 g_table[64];   // 256 DC_LUT_30_COLOR dwords, entry i at g_table[i >> 2][i & 3]
};

float4 VsMain(uint vid : SV_VertexID) : SV_Position
{
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

uint Entry(uint i)
{
    uint4 q = g_table[i >> 2];
    uint k = i & 3;
    return k == 0 ? q.x : k == 1 ? q.y : k == 2 ? q.z : q.w;
}

float4 PsMain(float4 pos : SV_Position) : SV_Target0
{
    float4 c = g_src.Load(int3(int2(pos.xy), 0));
    // UNORM -> 8-bit index the way Direct3D 10+ rounds (Xenia's own expression).
    uint3 idx = uint3(saturate(c.rgb) * 255.0 + 0.5);
    uint er = Entry(idx.r), eg = Entry(idx.g), eb = Entry(idx.b);
    float3 o = float3(float((er >> 20) & 0x3FF), float((eg >> 10) & 0x3FF),
                      float(eb & 0x3FF)) / 1023.0;
    return float4(o, c.a);
}
