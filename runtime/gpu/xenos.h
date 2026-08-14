// Xenos register indices and format codes — the constants the renderer reads.
//
// WHY THIS IS A HEADER OF ITS OWN
// -------------------------------
// Every one of these is a magic number, and a wrong one is silent: it reads a
// neighbouring field, produces a plausible value, and the symptom lands in the picture
// rather than in a log. Keeping them in one file with the field layout written next to
// each means the question "is 0x2201 really the blend control?" has one place to be
// answered, and the answer does not have to be re-derived at each use site.
//
// The indices are the GPU's own register numbers, which is what our register file is
// indexed by: SET_CONSTANT's bank 4 adds 0x2000 because the Xenos context registers
// begin there, and SET_CONSTANT2 writes an absolute index. So `regs[kRbSurfaceInfo]`
// is literally what the guest wrote.
//
// Provenance: Xenia's register table, cross-checked against the previous port's
// renderer, which drove a real world with them. Where this port has verified one
// against Case Zero's own stream the comment says so.
#pragma once

#include <cstdint>

namespace xenos {

// --- render backend ---------------------------------------------------------------
constexpr uint32_t kRbSurfaceInfo = 0x2000; // pitch:14, msaa:2 @16
constexpr uint32_t kRbColorInfo = 0x2001;   // base:12 (EDRAM tiles), format:4 @16
constexpr uint32_t kRbDepthInfo = 0x2002;   // base:12, format:1 @16 (0 = 16-bit, 1 = 24_8)
constexpr uint32_t kRbColor1Info = 0x2003;
constexpr uint32_t kRbColor2Info = 0x2004;
constexpr uint32_t kRbColor3Info = 0x2005;

// The screen scissor, in pixels. This is the closest thing the stream has to "how big
// is the render target": the EDRAM has no extent of its own, only a pitch, so the
// height has to come from a scissor or a viewport.
constexpr uint32_t kPaScScreenScissorTl = 0x200E; // x:15, y:15 @16
constexpr uint32_t kPaScScreenScissorBr = 0x200F;

constexpr uint32_t kVgtMaxVtxIndx = 0x2100;
constexpr uint32_t kVgtMinVtxIndx = 0x2101;
constexpr uint32_t kVgtIndxOffset = 0x2102;

constexpr uint32_t kRbDepthControl = 0x2200; // see the bit layout in DepthState()
constexpr uint32_t kRbBlendControl0 = 0x2201;
// RB_COLORCONTROL IS 0x2202, NOT 0x2205 — and the wrong index cost this port the
// alpha test twice over (part 40). The original map here guessed BLENDCONTROL0..3
// contiguous at 0x2201..0x2204 with COLORCONTROL after them; the real layout
// interleaves the per-RT blend controls (0x2201, 0x2205, 0x2209, 0x220D — matching
// the RB_COLOR{,1,2,3}_INFO spacing), with COLORCONTROL at 0x2202. Settled
// empirically, not from a header: histogram register 0x2202 per draw over R4 trace 01
// and every value carries the 0xAA alpha-to-mask sample-offset signature in its top
// byte with alpha-test enable (bit 3) SET on 520 draws — GREATER (0x0C), GEQUAL
// (0x0E), EQUAL (0x0A) and GREATER+ALPHA_TO_MASK (0x1C) — while 0x2205's values
// (00018004..6) never set bit 3 anywhere. The Fable 2 port reads 0x2202 and its
// alpha test was picture-validated. Consequences of the wrong index, recorded so the
// history reads correctly: part 38 wired the alpha test to 0x2205 so it never fired
// on anything, and part 39 "refuted" the foliage alpha test by reading 0x2205 across
// 40,703 hardware draws — a measurement of the wrong register (open-items 0t).
constexpr uint32_t kRbColorControl = 0x2202;
constexpr uint32_t kRbBlendControl1 = 0x2205;
constexpr uint32_t kRbBlendControl2 = 0x2209;
constexpr uint32_t kRbBlendControl3 = 0x220D;
constexpr uint32_t kRbColorMask = 0x2104;    // 4 bits per render target
constexpr uint32_t kRbStencilRefMask = 0x210D;
constexpr uint32_t kRbAlphaRef = 0x210E;
constexpr uint32_t kRbBlendRed = 0x2105;     // .. 0x2108 = green, blue, alpha

// The copy (resolve) block. RB_COPY_CONTROL's low 3 bits select the copy command;
// RB_COPY_DEST_BASE is where the resolved surface lands in guest memory, and matching
// it against VdSwap's front buffer is how the present seam knows which resolve is the
// frame rather than an intermediate post-processing target.
constexpr uint32_t kRbCopyControl = 0x2318;
constexpr uint32_t kRbCopyDestBase = 0x2319;
constexpr uint32_t kRbCopyDestPitch = 0x231A;  // pitch:14, height:14 @16
constexpr uint32_t kRbCopyDestInfo = 0x231B;
constexpr uint32_t kRbDepthClear = 0x231D;
constexpr uint32_t kRbColorClear = 0x231E;

constexpr uint32_t kPaSuScModeCntl = 0x2205 + 0x7B; // 0x2280: cull/front-face/fill
constexpr uint32_t kPaScWindowOffset = 0x2080;
constexpr uint32_t kPaScWindowScissorTl = 0x2081;
constexpr uint32_t kPaScWindowScissorBr = 0x2082;

// The viewport transform. PA_CL_VTE_CNTL says which of the six scale/offset terms the
// hardware applies; a term whose enable bit is clear is the identity, NOT the register
// value, and reading the register anyway is a silent geometry bug rather than a crash.
constexpr uint32_t kPaClVteCntl = 0x2206;
constexpr uint32_t kPaClVportXScale = 0x210F;
constexpr uint32_t kPaClVportXOffset = 0x2110;
constexpr uint32_t kPaClVportYScale = 0x2111;
constexpr uint32_t kPaClVportYOffset = 0x2112;
constexpr uint32_t kPaClVportZScale = 0x2113;
constexpr uint32_t kPaClVportZOffset = 0x2114;

// --- the constant windows our SET_CONSTANT decode maps into the register file -----
constexpr uint32_t kAluConstantBase = 0x4000;   // 512 float4: VS 0..255, PS 256..511
constexpr uint32_t kFetchConstantBase = 0x4800; // 32 groups of 6 dwords
constexpr uint32_t kBoolConstantBase = 0x4900;  // 8 dwords = 256 bits
constexpr uint32_t kLoopConstantBase = 0x4908;  // 32 dwords

// --- primitive types ---------------------------------------------------------------
enum PrimType : uint32_t
{
    kPointList = 1,
    kLineList = 2,
    kLineStrip = 3,
    kTriangleList = 4,
    kTriangleFan = 5,
    kTriangleStrip = 6,
    kRectangleList = 8,  // three corners of a screen-space rect; hardware makes a quad
    kLineLoop = 12,
    kQuadList = 13,
    kQuadStrip = 14,
};

// --- vertex fetch constant ---------------------------------------------------------
// Two dwords per slot, and a slot number is `constIndex * 3 + constIndexSelect` — which
// is why the six-dword groups of the fetch-constant file hold three vertex fetches
// each. Getting that packing wrong reads someone else's stream and draws a mesh out of
// unrelated data, which looks like a corrupt vertex buffer.
struct VertexFetch
{
    uint32_t address;    // guest physical, dword0 & ~3
    uint32_t sizeDwords; // dword1 >> 2
    uint32_t endian;     // dword1 & 3
    uint32_t type;       // dword0 & 3
};

inline VertexFetch DecodeVertexFetch(const uint32_t* regs, uint32_t slot)
{
    const uint32_t d0 = regs[kFetchConstantBase + slot * 2];
    const uint32_t d1 = regs[kFetchConstantBase + slot * 2 + 1];
    // dword1 is `endian:2, size:24, unused:6`. The 24-bit MASK is load-bearing and it
    // was missing on the first attempt: without it the six unused high bits ride along
    // and a perfectly good 85-dword stream reads as 67,108,885 dwords — which fails
    // every bounds check, so the symptom was not a wrong picture but 2.2 million draws
    // reported as "vertex stream outside the physical arena". A field's width is part
    // of the field.
    return { d0 & ~3u, (d1 >> 2) & 0xFFFFFF, d1 & 3, d0 & 3 };
}

// --- texture fetch constant ---------------------------------------------------------
// Six dwords per slot. Only the fields the renderer acts on are named; the rest are
// left as raw dwords rather than invented, because a field nobody reads cannot be
// wrong and a field guessed at can.
struct TextureFetch
{
    uint32_t type;       // dword0 & 3; 2 = texture
    uint32_t signX, signY, signZ, signW;
    uint32_t endian;     // dword0 bits 16:17
    uint32_t address;    // dword2-ish: base << 12, guest physical
    uint32_t width, height, depth;
    uint32_t format;     // dword3 bits 0:5
    // dword3 bits 1..12: four 3-bit fields saying which fetched component each of
    // x,y,z,w takes (0..3 = XYZW, 4 = constant 0, 5 = constant 1). This is RUNTIME
    // data, so a shader compiled without the fetch constant cannot bake it in — the
    // runtime has to apply it, and a font atlas is where forgetting shows first.
    uint32_t swizzle;
    bool tiled;
    uint32_t dimension;  // 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube
    uint32_t mipMin, mipMax;
    // THE SECOND ADDRESS. A Xenos fetch constant names the base level and the rest of
    // the mip chain SEPARATELY: `address` above holds level 0 only, and this holds
    // levels 1..mipMax. It is zero on a texture with no chain, so a renderer that never
    // read it — this one, until part 39 — cannot tell "no mips exist" from "mips exist
    // and I am ignoring them". `packedMips` says the levels below the tile size share
    // one tile rather than each starting on their own 4 KB boundary.
    uint32_t mipAddress;
    bool packedMips;
    uint32_t filterMin, filterMag, filterMip; // 0 = point, 1 = linear
    // dword3 bits 25..27. 0 = disabled, 1..5 = max 1/2/4/8/16 : 1. Position confirmed
    // two-sidedly in part 41: over 621 distinct hardware fetches the field reads only
    // 0/3/4 (valid enum values), the shadow atlas reads 0 with point filters, and the
    // world's albedo textures read 3..4 — which is also why a GLOBAL aniso sampler is
    // wrong (it speckled the shadow term the moment it was tried).
    uint32_t filterAniso;
    uint32_t clampX, clampY, clampZ;
    uint32_t pitchBlocks; // dword4 bits 22:31, in blocks of 32 texels
};

TextureFetch DecodeTextureFetch(const uint32_t* regs, uint32_t slot);

// --- texture formats ------------------------------------------------------------------
// The Xenos `k_*` enumeration, in full and in order rather than as the subset this
// title happens to use. Writing out the whole table is what makes a code checkable
// against its neighbours — the first version of this header listed DXT3A/DXT5A/CTX1 at
// 43/44/45, which are actually the INTERLACED formats, and nothing about the shorter
// list made that visible. The wrong code does not fail: it decodes a normal map as an
// 8-bit interlaced surface and produces a plausible wrong image.
//
// "EXPAND" means the channel is a signed value biased into the unsigned range; "AS_"
// forms are the same bits reinterpreted at a wider size by the sampler.
enum TexFormat : uint32_t
{
    kFmt_1_REVERSE = 0,
    kFmt_1 = 1,
    kFmt_8 = 2,
    kFmt_1_5_5_5 = 3,
    kFmt_5_6_5 = 4,
    kFmt_6_5_5 = 5,
    kFmt_8_8_8_8 = 6,
    kFmt_2_10_10_10 = 7,
    kFmt_8_A = 8,
    kFmt_8_B = 9,
    kFmt_8_8 = 10,
    kFmt_Cr_Y1_Cb_Y0_REP = 11,
    kFmt_Y1_Cr_Y0_Cb_REP = 12,
    kFmt_16_16_EDRAM = 13,
    kFmt_8_8_8_8_A = 14,
    kFmt_4_4_4_4 = 15,
    kFmt_10_11_11 = 16,
    kFmt_11_11_10 = 17,
    kFmt_DXT1 = 18,
    kFmt_DXT2_3 = 19,
    kFmt_DXT4_5 = 20,
    kFmt_16_16_16_16_EDRAM = 21,
    kFmt_24_8 = 22,
    kFmt_24_8_FLOAT = 23,
    kFmt_16 = 24,
    kFmt_16_16 = 25,
    kFmt_16_16_16_16 = 26,
    kFmt_16_EXPAND = 27,
    kFmt_16_16_EXPAND = 28,
    kFmt_16_16_16_16_EXPAND = 29,
    kFmt_16_FLOAT = 30,
    kFmt_16_16_FLOAT = 31,
    kFmt_16_16_16_16_FLOAT = 32,
    kFmt_32 = 33,
    kFmt_32_32 = 34,
    kFmt_32_32_32_32 = 35,
    kFmt_32_FLOAT = 36,
    kFmt_32_32_FLOAT = 37,
    kFmt_32_32_32_32_FLOAT = 38,
    kFmt_32_AS_8 = 39,
    kFmt_32_AS_8_8 = 40,
    kFmt_16_MPEG = 41,
    kFmt_16_16_MPEG = 42,
    kFmt_8_INTERLACED = 43,
    kFmt_32_AS_8_INTERLACED = 44,
    kFmt_32_AS_8_8_INTERLACED = 45,
    kFmt_16_INTERLACED = 46,
    kFmt_16_MPEG_INTERLACED = 47,
    kFmt_16_16_MPEG_INTERLACED = 48,
    kFmt_DXN = 49, // two-channel compressed, i.e. BC5 — normal maps
    kFmt_8_8_8_8_AS_16_16_16_16 = 50,
    kFmt_DXT1_AS_16_16_16_16 = 51,
    kFmt_DXT2_3_AS_16_16_16_16 = 52,
    kFmt_DXT4_5_AS_16_16_16_16 = 53,
    kFmt_2_10_10_10_AS_16_16_16_16 = 54,
    kFmt_10_11_11_AS_16_16_16_16 = 55,
    kFmt_11_11_10_AS_16_16_16_16 = 56,
    kFmt_32_32_32_FLOAT = 57,
    kFmt_DXT3A = 58,
    kFmt_DXT5A = 59, // single-channel compressed, i.e. BC4
    kFmt_CTX1 = 60,
    kFmt_DXT3A_AS_1_1_1_1 = 61,
};

} // namespace xenos
