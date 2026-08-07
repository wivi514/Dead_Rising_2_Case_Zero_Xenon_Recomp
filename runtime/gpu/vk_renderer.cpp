#include "vk_renderer.h"

#include "pm4.h"
#include "xenos.h"
#include "../host/window.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

// ===================================================================================
// THE INTERFACE THE TRANSLATED SHADERS PRESENT
// ===================================================================================
// This is not a design choice on our side — it is what XenosRecomp emits, and getting
// it wrong produces a device-lost or a black frame rather than a compile error. Read it
// out of the generated HLSL (`XenosRecomp/shader_common.h`) rather than from here if
// anything ever disagrees; this is a transcription.
//
// PUSH CONSTANTS (24 bytes, both stages): three uint64 GPU device addresses —
//   +0  VertexShaderConstants   256 float4  (the guest's ALU constants 0..255)
//   +8  PixelShaderConstants    224 float4  (the guest's ALU constants 256..479)
//   +16 SharedConstants         the block laid out below
//
// SHARED CONSTANTS. Every offset here appears verbatim in the generated shaders, which
// is the only reason to trust it:
//   +0   .. +63   Texture2D   descriptor index, one uint per sampler slot (16 slots)
//   +64  .. +127  Texture3D   descriptor index
//   +128 .. +191  TextureCube descriptor index
//   +192 .. +255  Sampler     descriptor index
//   +256          g_Booleans          (the 16+16 named bool scheme)
//   +260          g_SwappedTexcoords  (bit N: TEXCOORD N needs a YXWZ unswizzle)
//   +264          g_HalfPixelOffset   (float2)
//   +272          g_AlphaThreshold    (float)
//   +276          g_ParamGenMask      (uint)
//   +280          g_TessGrid          (uint)
//   +288 .. +351  Texture1D   descriptor index
//   +352          g_PosScale          (float2)
//   +360          g_PosOffset         (float2)
//   +384 .. +511  the 32 Xenos loop constants
//   +512 .. +543  the 256-bit Xenos bool constant file
//   +544 ..       per-fetch-slot dependent-vfetch table, 16 bytes each
//
// DESCRIPTOR SETS. One unbounded array each, matching the HLSL register spaces:
//   set 0 = Texture2D[]   set 1 = Texture3D[]   set 2 = TextureCube[]
//   set 3 = Sampler[]     set 4 = Texture1D[]
// ===================================================================================

namespace {

constexpr uint32_t kSharedTex2D = 0;
constexpr uint32_t kSharedTex3D = 64;
constexpr uint32_t kSharedTexCube = 128;
constexpr uint32_t kSharedSampler = 192;
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedSwappedTexcoords = 260;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedParamGenMask = 276;
constexpr uint32_t kSharedTessGrid = 280;
constexpr uint32_t kSharedTex1D = 288;
constexpr uint32_t kSharedPosScale = 352;
constexpr uint32_t kSharedPosOffset = 360;
constexpr uint32_t kSharedLoopConstants = 384;
constexpr uint32_t kSharedBoolFile = 512;
constexpr uint32_t kSharedVfetchTable = 544;
constexpr uint32_t kSharedSize = 544 + 96 * 16; // one entry per vertex fetch slot

// BOTH stages get 256 float4 registers, and the pixel shader's 256 is load-bearing.
//
// XenosRecomp's README documents the pixel shader window as 224 float4 (3584 bytes) and
// this file believed it. The generated shaders do not: the macro they emit is
// `pc(INDEX) = select(INDEX < 256, RawBufferLoad(PixelShaderConstants + min(INDEX,255)*16), 0)`,
// so a shader reading c255 loads from offset 4080 — 512 bytes past a 224-register
// buffer, i.e. into whatever this arena allocated next.
//
// Case Zero's scene pixel shaders read c255 in their FINAL instructions, as the
// tone-map's scale and bias:
//     mul  r0.xyz, r0.xyz, c255.wwww
//     mad  r0.xyz, r0.xyz, c14.wwww, c255.xxxx
//     max  r0.xyz, r0.xyz, c255.zzzz
//     mul  r0.xyz, r0.xyz, c255.yyyy
// so a wrong c255 does not tint the scene — it collapses every pixel to a constant.
// That is what "930 draws producing three distinct colours" was.
//
// The guest states the true size itself and it is not 224: SQ_PS_CONST reads
// base=256 size=255, i.e. ALU float4 registers 256..511, which is 256 registers.
// Sizing a constant buffer from a tool's documentation rather than from the guest's
// own register is the whole mistake.
constexpr uint32_t kVsConstBytes = 256 * 16;
constexpr uint32_t kPsConstBytes = 256 * 16;

constexpr uint32_t kMaxDescriptors = 4096; // per heap; the frontend uses a few dozen

// --- diagnostics --------------------------------------------------------------------
// Every path that declines to do something increments one of these. The alternative —
// returning quietly — is what makes a renderer that draws 80% of a frame look exactly
// like one that draws all of it, and this project has already paid for that lesson in
// the command processor (gotcha 84: a parser that stops early must say so).
std::map<std::string, uint64_t> g_stats;
void Count(const char* name) { ++g_stats[name]; }

// CZ_VK_TEX_CENSUS=1 — per texture ADDRESS, where its pixels came from.
//
// The aggregate counters above say how many fetches took each path; they cannot say
// WHICH surface took which, and that is the whole question behind a black rectangle on
// screen. A surface our renderer resolved to and then served from guest memory is
// serving pixels nobody ever wrote there (gotcha 113: a resolve becomes a host image,
// not guest bytes), and the symptom is a filled black quad four layers away. The
// `zero` column is the one that matters: an upload whose every byte is zero is this
// runtime saying out loud that it had nothing to give.
//
// Gated on the env var because the `snapshot` column is hit ~500,000 times a run and a
// probe expensive enough to change the frame rate manufactures what it reports
// (gotcha 7).
struct TexSource
{
    uint32_t width = 0, height = 0, format = 0;
    uint64_t uploads = 0, zeroUploads = 0, fromSnapshot = 0, snapshotTooOld = 0;
    uint64_t maxAge = 0;
    bool everResolved = false;
    // Enough to re-read the same bytes at report time. "This upload was black" and
    // "this upload was black AND the guest has filled it in since" are completely
    // different defects — the first says the data was never there, the second says we
    // cached a texture that arrived late — and only a re-read separates them.
    const uint8_t* src = nullptr;
    uint64_t srcBytes = 0;
};
std::map<uint32_t, TexSource> g_texSources;
bool g_texCensus = false;

bool g_active = false;
bool g_initTried = false;

// Which feed owns the renderer this run. False = the PM4 executor (CZ_VKDRAW,
// phase 5); true = the D3D draw service (CZ_D3D_DRAW, phase C). Set once at init and
// never changed: the entries belonging to the other feed check it and return, so a
// run can never have both feeds drawing into one EDRAM image.
bool g_d3dMode = false;

const char* Env(const char* n) { return getenv(n); }
bool EnvOn(const char* n) { return getenv(n) != nullptr; }

// ===================================================================================
// Guest memory, again
// ===================================================================================
// Same convention as pm4.cpp and for the same reason: the PPC_LOAD macros need a
// `base` named exactly that in scope, and keeping the accessors local is what lets
// this file be read without the recompiled image in view.
constexpr uint32_t kPhysArenaBase = 0xA0000000u;
constexpr uint32_t kPhysArenaEnd = 0xBFFF0000u;

inline uint32_t PhysToVa(uint32_t addr) { return kPhysArenaBase | (addr & 0x1FFFFFFFu); }

// True when [va, va+bytes) is inside the physical arena. Every guest pointer the
// register file hands us goes through this: a fetch constant left over from a previous
// frame can name anything at all, and a memcpy from it is a host segfault attributed
// to our renderer rather than to the stale register it came from.
bool GuestRangeOk(uint32_t va, uint64_t bytes)
{
    return bytes && va >= kPhysArenaBase && uint64_t(va) + bytes <= kPhysArenaEnd;
}

// The GPU's per-address endian swizzle: 0 none, 1 = 8-in-16, 2 = 8-in-32, 3 = 16-in-32.
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian)
{
    switch (endian & 3)
    {
        case 1:
            for (size_t i = 0; i + 1 < bytes; i += 2)
            {
                dst[i] = src[i + 1];
                dst[i + 1] = src[i];
            }
            break;
        case 2:
            for (size_t i = 0; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, src + i, 4);
                v = __builtin_bswap32(v);
                memcpy(dst + i, &v, 4);
            }
            break;
        case 3:
            for (size_t i = 0; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, src + i, 4);
                v = (v >> 16) | (v << 16);
                memcpy(dst + i, &v, 4);
            }
            break;
        default:
            memcpy(dst, src, bytes);
            break;
    }
}

inline float F32(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// ===================================================================================
// Vertex formats
// ===================================================================================
// The Xenos format code, as the vertex fetch instruction carries it, to the Vulkan
// vertex format that reads the same bytes after the whole stream has been dword
// swapped. `numFormat` 0 means a normalized fraction and 1 means an integer kept as a
// float value, which is a different Vulkan format, not a shader-side difference.
//
// UNDEFINED IS NOT A FALLBACK. An unmapped format used to fall through as
// VK_FORMAT_UNDEFINED on the previous port and the pipeline drew anyway — undefined
// behaviour that eventually took the device down. Here it refuses the pipeline and
// names the format once, which turns "some geometry is missing" into a line of log.
VkFormat XenosVertexFormat(uint32_t fmt, bool isSigned, bool isInteger)
{
    // `numFormat` 1 — "integer kept as a float value" — is NOT a shader-side detail,
    // and treating it as one is a silent, total corruption of whatever the attribute
    // carries. A normalized format divides by the type's range, so an integer 32
    // arrives as 32/255 = 0.125, and a shader that does `floor()` on it to index
    // something reads element 0 every time. Case Zero has 15 such attributes; the
    // meshes that use them collapse to a vanishing point, which reads as scrambled
    // geometry rather than as a vertex-format bug.
    //
    // USCALED/SSCALED are exactly this concept in Vulkan — an integer in memory
    // delivered as its own value in a float input — so the shader needs no change and
    // the input stays float-typed, which a *_UINT format would not.
    switch (fmt)
    {
        case 6:
            if (isInteger)
                return isSigned ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED;
            return isSigned ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
        case 25:
            if (isInteger)
                return isSigned ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED;
            return isSigned ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
        case 26:
            if (isInteger)
                return isSigned ? VK_FORMAT_R16G16B16A16_SSCALED
                                : VK_FORMAT_R16G16B16A16_USCALED;
            return isSigned ? VK_FORMAT_R16G16B16A16_SNORM
                            : VK_FORMAT_R16G16B16A16_UNORM;
        case 31: return VK_FORMAT_R16G16_SFLOAT;
        case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;
        // There is no 32-bit-normalized Vulkan vertex format, so both flavours of
        // k_32 get the integer type. Better than rejecting the draw.
        case 33: return isSigned ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
        // k_10_11_11 packed normals are decoded IN the shader, which takes the raw
        // dword — so the input must deliver the untouched 32 bits, not a normalized
        // format that would pre-decode them wrongly.
        case 16: return VK_FORMAT_R32_UINT;
        case 7:
            if (isInteger)
                return VK_FORMAT_R32_UINT;
            return isSigned ? VK_FORMAT_A2B10G10R10_SNORM_PACK32
                            : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 36: return VK_FORMAT_R32_SFLOAT;
        case 37: return VK_FORMAT_R32G32_SFLOAT;
        case 57: return VK_FORMAT_R32G32B32_SFLOAT;
        case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// Dwords occupied by one element of a vertex format. Used only for bounds checking,
// and it is deliberately NOT the stride: a stream's last vertex only has to reach
// `offset + element`, and buffers are commonly sized exactly that tightly, so a
// `vertices * stride` bound reports an overrun on perfectly good geometry.
uint32_t VertexFormatDwords(uint32_t fmt)
{
    switch (fmt)
    {
        case 6: case 7: case 16: case 25: case 31: case 33: case 36: return 1;
        case 26: case 32: case 37: return 2;
        case 57: return 3;
        case 38: return 4;
        default: return 0;
    }
}

// CZ_VK_FETCH_SLOT_INVERT=1 — read vertex fetch constants at `95 - slot`.
//
// The arm for the one convention this renderer cannot derive. A vfetch's constant index
// is `const_index * 3 + const_index_sel`, and Xenia's disassembly prints the same
// shaders' fetches as vf0/vf1/vf2 where that formula gives 95/94/93 — so one of the two
// is a display convention. The first attempt to settle it (dumping the populated slots)
// was WEAKER than it looked: the shader it happened to catch asked for slot 0 twice,
// and both slot 0 and slot 95 were populated, so the observation was consistent with
// either reading. This is the version that cannot be ambiguous — invert it and look at
// the geometry.
uint32_t FetchSlot(uint32_t slot)
{
    static const bool invert = getenv("CZ_VK_FETCH_SLOT_INVERT") != nullptr;
    return invert ? (slot <= 95 ? 95 - slot : slot) : slot;
}

// ===================================================================================
// The shader cache
// ===================================================================================
struct VertexAttribute
{
    int32_t location = -1;   // -1 = a dependent fetch, read in-shader, not an input
    uint32_t fetchSlot = 0;
    uint32_t format = 0;
    uint32_t isSigned = 0;
    uint32_t isInteger = 0;
    uint32_t strideDwords = 0;
    uint32_t offsetDwords = 0;
    uint32_t indirect = 0;
};

struct ShaderMeta
{
    VkShaderModule module = VK_NULL_HANDLE;
    bool isVertex = false;
    std::vector<VertexAttribute> attributes; // vertex shaders only
    std::vector<uint32_t> interpolators;
    std::vector<uint32_t> tfetchConsts;
};

// A deliberately small JSON reader for a file this project writes itself.
//
// Pulling in a JSON library for four key names would be the larger risk: the sidecar's
// shape is fixed by tools/synth_shader_container.py, both ends live in this repo, and a
// malformed sidecar is a build-pipeline bug that should be loud here rather than
// tolerated. Anything unrecognised is ignored, and a missing sidecar drops the shader
// with a message (never silently — an orphaned .spv cost the previous port 25,364
// draws a run before anyone noticed the module count was seven short).
struct Json
{
    const std::string& s;
    size_t p = 0;

    explicit Json(const std::string& text) : s(text) {}

    void Skip()
    {
        while (p < s.size() && (isspace(uint8_t(s[p])) || s[p] == ',' || s[p] == ':'))
            ++p;
    }
    bool Find(const char* key, size_t from = 0)
    {
        const std::string pat = std::string("\"") + key + "\"";
        const size_t at = s.find(pat, from);
        if (at == std::string::npos)
            return false;
        p = at + pat.size();
        Skip();
        return true;
    }
    long Number()
    {
        Skip();
        return strtol(s.c_str() + p, nullptr, 10);
    }
};

long JsonIntField(const std::string& obj, const char* key, long fallback)
{
    Json j(obj);
    return j.Find(key) ? j.Number() : fallback;
}

std::vector<uint32_t> JsonIntArray(const std::string& s, const char* key)
{
    std::vector<uint32_t> out;
    Json j(s);
    if (!j.Find(key))
        return out;
    const size_t open = s.find('[', j.p);
    const size_t close = s.find(']', open);
    if (open == std::string::npos || close == std::string::npos)
        return out;
    const char* c = s.c_str() + open + 1;
    const char* end = s.c_str() + close;
    while (c < end)
    {
        while (c < end && !isdigit(uint8_t(*c)) && *c != '-')
            ++c;
        if (c >= end)
            break;
        out.push_back(uint32_t(strtol(c, const_cast<char**>(&c), 10)));
    }
    return out;
}

bool LoadShaderMeta(const std::filesystem::path& path, ShaderMeta& meta)
{
    std::ifstream f(path);
    if (!f)
        return false;
    const std::string text((std::istreambuf_iterator<char>(f)), {});

    meta.isVertex = text.find("\"vs\"") != std::string::npos;
    meta.interpolators = JsonIntArray(text, "interpolators");
    meta.tfetchConsts = JsonIntArray(text, "tfetchConsts");

    // The attribute array is objects, so it is walked object by object rather than
    // with the flat integer-array reader.
    size_t at = text.find("\"attributes\"");
    if (at != std::string::npos)
    {
        size_t open = text.find('[', at);
        size_t cursor = open;
        while (cursor != std::string::npos)
        {
            const size_t objOpen = text.find('{', cursor);
            if (objOpen == std::string::npos)
                break;
            const size_t objClose = text.find('}', objOpen);
            if (objClose == std::string::npos)
                break;
            const std::string obj = text.substr(objOpen, objClose - objOpen + 1);
            VertexAttribute a;
            a.location = int32_t(JsonIntField(obj, "location", -1));
            a.fetchSlot = uint32_t(JsonIntField(obj, "fetchSlot", 0));
            a.format = uint32_t(JsonIntField(obj, "format", 0));
            a.isSigned = uint32_t(JsonIntField(obj, "signed", 0));
            a.isInteger = uint32_t(JsonIntField(obj, "integer", 0));
            a.strideDwords = uint32_t(JsonIntField(obj, "strideDwords", 0));
            a.offsetDwords = uint32_t(JsonIntField(obj, "offsetDwords", 0));
            a.indirect = uint32_t(JsonIntField(obj, "indirect", 0));
            meta.attributes.push_back(a);
            cursor = objClose + 1;
            const size_t nextBrace = text.find('{', cursor);
            const size_t arrayEnd = text.find(']', cursor);
            if (nextBrace == std::string::npos || nextBrace > arrayEnd)
                break;
        }
    }
    return true;
}

// ===================================================================================
// Pipeline key
// ===================================================================================
// Everything that has to be baked into a VkPipeline. Kept as a POD compared with
// memcmp so that adding a field cannot be forgotten in an equality operator — the
// classic way to get two different states sharing one pipeline, which renders as a
// draw quietly using the previous draw's blend mode.
struct PipelineKey
{
    uint64_t vsHash;
    uint64_t psHash;
    uint32_t topology;
    uint32_t blendControl;
    uint32_t colorMask;
    uint32_t depthControl;
    uint32_t modeControl;
    uint32_t primRestart;

    bool operator<(const PipelineKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) < 0;
    }
};

// ===================================================================================
// The renderer
// ===================================================================================
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A texture the guest described with a fetch constant, uploaded once and reused.
// Keyed on the fetch constant's own dwords: if any of them changes the texture is a
// different texture, and if none of them changes it is the same one. That is a
// stronger key than the base address alone, which the title reuses.
struct TextureEntry
{
    Image image;
    uint32_t slot = 0;   // index into the bindless heap
    uint64_t key = 0;
};

// A RESOLVE SNAPSHOT: what one pass left in the EDRAM, kept as a host image under the
// guest address the pass copied it to.
//
// This is the mechanism that makes a post-processing chain work, and it exists because
// of what the resolve trace showed about this title. A title-screen frame issues about
// twenty resolves: a 1280x720 main pass, a 640x360 / 320x180 / ... / 1x1 downsample
// pyramid, some 1024x32 and 1024x1024 surfaces, and finally one resolve to the address
// VdSwap named. Every one of them renders into the SAME EDRAM and clears it afterwards
// (their RB_COPY_CONTROL has both clear bits set; the front-buffer one does not) — so
// the EDRAM at the end of a frame holds only the last pass, and the passes communicate
// exclusively through guest memory.
//
// We do not write resolved pixels back into guest memory: that would mean tiling them,
// and the consumer would then untile them again, for a round trip whose only purpose is
// to lose precision. Instead the destination address becomes the key, and a texture
// fetch that names it is served the host image directly.
struct Snapshot
{
    Image image;
    uint32_t slot = 0;   // bindless heap index, so a fetch can be served without a copy
    uint64_t frameSeen = 0;
};

struct Renderer
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool recording = false;
    bool rendering = false;

    // The EDRAM stand-in: one persistent colour target and one depth target.
    Image color;
    Image depth;
    Buffer readback;

    // Per-frame bump arena for constants, vertex copies and index copies. Device
    // address visible, because the translated shaders reach their constants through
    // vk::RawBufferLoad on a raw 64-bit address rather than through a descriptor.
    Buffer arena;
    VkDeviceSize arenaCursor = 0;
    VkDeviceSize arenaHighWater = 0;

    Buffer staging;
    VkDeviceSize stagingCursor = 0;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[5]{};
    VkDescriptorSet sets[5]{};
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;

    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler pointSampler = VK_NULL_HANDLE;
    Image dummy2D, dummy3D, dummyCube, dummy1D;

    std::map<uint64_t, ShaderMeta> shaders;
    std::map<PipelineKey, VkPipeline> pipelines;
    std::unordered_map<uint64_t, TextureEntry> textures;
    std::unordered_map<uint32_t, Snapshot> snapshots; // by resolve destination
    uint32_t nextTextureSlot = 1; // slot 0 is the dummy

    // Per-frame vertex/index stream cache: one guest buffer copied once per frame
    // however many draws read it.
    std::unordered_map<uint64_t, VkDeviceSize> streamCache;

    uint64_t frame = 0;
    uint64_t drawsThisFrame = 0;
    // Per-frame content fingerprints, for the frame-alignment metric.
    //
    // `drawFingerprint` is FNV over every draw's (vs, ps, primitive, index count) —
    // it identifies WHAT the guest asked for this frame. `cameraFingerprint` is FNV
    // over the vertex shader's ALU constants at the frame's first draw, which is where
    // the view-projection matrix lives — it identifies WHERE the camera was.
    //
    // Both exist because "frame 600" is not a point in this title's animation: the
    // title screen renders a live 3D background driven by guest time, and our frame
    // rate varies with host load, so two runs are looking at different camera angles at
    // the same frame index. Comparing pictures across runs needs frames matched by
    // CONTENT, which is exactly what tools/xtr_determinism.py does to the capture pair.
    uint64_t drawFingerprint = 0;
    uint64_t cameraFingerprint = 0;
    uint64_t verticesThisFrame = 0;
    // Draws recorded since the last resolve, i.e. the size of the pass that resolve is
    // closing. This is the number that separates "the pass rendered nothing because it
    // had no draws" from "the pass had 900 draws and they produced black" — two
    // completely different investigations that look identical in a snapshot.
    uint64_t drawsThisPass = 0;
    uint64_t verticesThisPass = 0;
    // Which resolve snapshots the draws of the current pass SAMPLED, and how many
    // textures they took from guest memory instead.
    //
    // This is the one question the existing counters cannot answer. "texture: served
    // from a resolve snapshot" proves snapshots are consumed — 450,488 a run — but not
    // by WHICH pass, and the whole of step 1 is "does the pass that writes the front
    // buffer sample the scene?". A global counter can never say that; a per-pass set
    // can, and it costs one insert per texture fetch.
    std::vector<uint32_t> snapshotsSampledThisPass;
    uint64_t guestTexturesThisPass = 0;
    // The first few draws of the pass, as (prim, indexCount, vs). A pass of ONE draw is
    // a post-processing blit, and when those are the passes producing nothing, the
    // question is which shader draws them.
    std::vector<std::string> firstDrawsThisPass;
    // The first texture the CURRENT draw bound, for the pass draw list. "This pass is
    // black" and "this pass's input was never produced" are the same picture until you
    // can name the surface each draw sampled (gotcha 140).
    uint32_t lastTexAddr = 0;
    uint32_t lastTexSlot = 0;
    uint32_t targetWidth = 1280, targetHeight = 720;
    uint32_t frontBuffer = 0;
    uint32_t lastResolveDest = 0;
    uint32_t frontWidth = 0, frontHeight = 0;
    bool haveFrontSnapshot = false;

    std::vector<uint8_t> presentPixels;
};

Renderer* R = nullptr;

#define VK_CHECK(expr, what)                                                           \
    do                                                                                 \
    {                                                                                  \
        const VkResult vkr_ = (expr);                                                  \
        if (vkr_ != VK_SUCCESS)                                                        \
        {                                                                              \
            fprintf(stderr, "[vk] %s failed: VkResult %d\n", what, int(vkr_));         \
            return false;                                                              \
        }                                                                              \
    } while (0)

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < R->memProps.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) &&
            (R->memProps.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags props, bool deviceAddress)
{
    b.size = size;
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(R->device, &ci, nullptr, &b.buffer), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(R->device, b.buffer, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, props);
    if (type == UINT32_MAX)
    {
        fprintf(stderr, "[vk] no memory type for buffer (props %u)\n", props);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = deviceAddress ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(R->device, &ai, nullptr, &b.memory), "vkAllocateMemory");
    VK_CHECK(vkBindBufferMemory(R->device, b.buffer, b.memory, 0), "vkBindBufferMemory");

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(R->device, b.memory, 0, size, 0,
                             reinterpret_cast<void**>(&b.mapped)),
                 "vkMapMemory");

    if (deviceAddress)
    {
        VkBufferDeviceAddressInfo di{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        di.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(R->device, &di);
    }
    return true;
}

bool CreateImage(Image& img, uint32_t w, uint32_t h, VkFormat format,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect,
                 VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D, uint32_t layers = 1,
                 uint32_t depthExtent = 1,
                 VkComponentMapping components = VkComponentMapping{})
{
    img.width = w;
    img.height = h;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = depthExtent > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, depthExtent };
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (viewType == VK_IMAGE_VIEW_TYPE_CUBE)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VK_CHECK(vkCreateImage(R->device, &ci, nullptr, &img.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R->device, img.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return false;
    VK_CHECK(vkAllocateMemory(R->device, &ai, nullptr, &img.memory), "vkAllocateMemory");
    VK_CHECK(vkBindImageMemory(R->device, img.image, img.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = viewType;
    vi.format = format;
    vi.components = components;
    vi.subresourceRange = { aspect, 0, 1, 0, layers };
    VK_CHECK(vkCreateImageView(R->device, &vi, nullptr, &img.view), "vkCreateImageView");
    return true;
}

void Barrier(VkCommandBuffer cmd, Image& img, VkImageLayout newLayout,
             VkImageAspectFlags aspect)
{
    if (img.layout == newLayout)
        return;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = img.layout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &b);
    img.layout = newLayout;
}

// --- the per-frame arena -------------------------------------------------------------
// A bump allocator reset at each swap. Everything a draw needs that is not a texture
// lives here: constants, the dword-swapped copies of the guest's vertex streams, and
// index buffers. Exhaustion is COUNTED and the draw is skipped rather than wrapped,
// because wrapping would overwrite data an already-recorded draw still points at — and
// the resulting corruption would appear in a draw that was fine.
VkDeviceSize ArenaAlloc(VkDeviceSize bytes, VkDeviceSize align = 256)
{
    const VkDeviceSize at = (R->arenaCursor + align - 1) & ~(align - 1);
    if (at + bytes > R->arena.size)
    {
        Count("arena: exhausted, draw skipped");
        return VkDeviceSize(-1);
    }
    R->arenaCursor = at + bytes;
    R->arenaHighWater = std::max(R->arenaHighWater, R->arenaCursor);
    return at;
}

// ===================================================================================
// Device bring-up
// ===================================================================================
bool CreateDevice()
{
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "cz_runtime";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;

    // CZ_VK_VALIDATION=1 turns on the validation layer. Off by default because it is
    // very slow at ~2,000 draws a frame, and on when a picture is wrong: this project
    // has twice had a "renderer bug" that was an API misuse the layer names in one line.
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const bool wantValidation = EnvOn("CZ_VK_VALIDATION");
    if (wantValidation)
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
        fprintf(stderr, "[vk] validation layer requested\n");
    }
    VkResult ir = vkCreateInstance(&ici, nullptr, &R->instance);
    if (ir == VK_ERROR_LAYER_NOT_PRESENT && wantValidation)
    {
        // Asking for an absent layer must not cost the renderer. It did: the instance
        // failed, Init returned false, and the run had no renderer at all — while the
        // log said "validation layer requested", which reads as though it was ON. An
        // instrument that silently disables the thing it instruments is worse than no
        // instrument (gotcha 7), so this retries and says exactly what happened.
        fprintf(stderr,
                "[vk] VK_LAYER_KHRONOS_validation is NOT INSTALLED — continuing "
                "WITHOUT it (Fedora: sudo dnf install vulkan-validation-layers)\n");
        ici.enabledLayerCount = 0;
        ir = vkCreateInstance(&ici, nullptr, &R->instance);
    }
    VK_CHECK(ir, "vkCreateInstance");

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(R->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(R->instance, &count, devices.data());
    if (devices.empty())
    {
        fprintf(stderr, "[vk] no Vulkan physical devices\n");
        return false;
    }
    // Prefer a discrete GPU, else take the first. Named in the log either way: a
    // renderer running on llvmpipe at two frames a minute is a configuration fact, not
    // a performance mystery.
    R->physical = devices[0];
    for (VkPhysicalDevice d : devices)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            R->physical = d;
            break;
        }
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(R->physical, &props);
    fprintf(stderr, "[vk] device: %s (Vulkan %u.%u.%u)\n", props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(R->physical, &R->memProps);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(R->physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(R->physical, &qcount, families.data());
    R->queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            R->queueFamily = i;
            break;
        }
    if (R->queueFamily == UINT32_MAX)
    {
        fprintf(stderr, "[vk] no graphics queue family\n");
        return false;
    }

    // The three features the translated shaders cannot run without, requested
    // explicitly so a device that lacks one fails HERE with a name rather than at the
    // first draw with a device-lost:
    //   bufferDeviceAddress — the shaders load constants through raw 64-bit addresses
    //   descriptorIndexing  — the bindless texture/sampler heaps
    //   dynamicRendering    — no render-pass objects; the target is one image
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };
    v12.bufferDeviceAddress = VK_TRUE;
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
    };
    v13.dynamicRendering = VK_TRUE;
    v13.pNext = &v12;

    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &v13;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.textureCompressionBC = VK_TRUE;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = R->queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    VK_CHECK(vkCreateDevice(R->physical, &dci, nullptr, &R->device), "vkCreateDevice");
    vkGetDeviceQueue(R->device, R->queueFamily, 0, &R->queue);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = R->queueFamily;
    VK_CHECK(vkCreateCommandPool(R->device, &pci, nullptr, &R->cmdPool),
             "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = R->cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(R->device, &cbi, &R->cmd),
             "vkAllocateCommandBuffers");

    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(R->device, &fi, nullptr, &R->fence), "vkCreateFence");
    return true;
}

bool CreateDescriptorPlumbing()
{
    // Five heaps, one per HLSL register space. Each is one binding holding an array
    // that the shader indexes with a value it read out of the shared constants.
    const VkDescriptorType types[5] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, // space0: Texture2D
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, // space1: Texture3D
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, // space2: TextureCube
        VK_DESCRIPTOR_TYPE_SAMPLER,       // space3: SamplerState
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, // space4: Texture1D
    };

    for (int i = 0; i < 5; i++)
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = types[i];
        b.descriptorCount = kMaxDescriptors;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        const VkDescriptorBindingFlags flags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
        };
        bf.bindingCount = 1;
        bf.pBindingFlags = &flags;

        VkDescriptorSetLayoutCreateInfo li{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        li.pNext = &bf;
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        li.bindingCount = 1;
        li.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, nullptr, &R->setLayouts[i]),
                 "vkCreateDescriptorSetLayout");
    }

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxDescriptors * 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kMaxDescriptors },
    };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 5;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(R->device, &pi, nullptr, &R->descPool),
             "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R->descPool;
    ai.descriptorSetCount = 5;
    ai.pSetLayouts = R->setLayouts;
    VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, R->sets),
             "vkAllocateDescriptorSets");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = 24; // three uint64 device addresses
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 5;
    pli.pSetLayouts = R->setLayouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, nullptr, &R->pipeLayout),
             "vkCreatePipelineLayout");
    return true;
}

// --- shader cache load ---------------------------------------------------------------
uint64_t HashFromName(const std::string& name)
{
    const size_t us = name.find('_');
    return us == std::string::npos ? 0 : strtoull(name.c_str() + us + 1, nullptr, 16);
}

bool LoadShaders()
{
    // The cache directory is CWD-relative and the launch CWD varies (the documented
    // way to run this runtime is from runtime/build/). Try the conventions rather
    // than pick one, and let CZ_SHADER_SPV override — a renderer that came up with
    // zero shaders because of a working directory would look like a translation
    // failure, which is a much more expensive thing to go and investigate.
    std::filesystem::path dir;
    if (const char* env = Env("CZ_SHADER_SPV"))
        dir = env;
    else
    {
        std::vector<std::filesystem::path> candidates = {
            "../../assets/shader_spv", // CWD = runtime/build/
            "../assets/shader_spv",    // CWD = runtime/
            "assets/shader_spv",       // CWD = repo root
        };
        char exe[4096];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0)
        {
            exe[n] = '\0';
            candidates.push_back(std::filesystem::path(exe)
                                     .parent_path()
                                     .parent_path()
                                     .parent_path() /
                                 "assets" / "shader_spv");
        }
        std::error_code ec;
        for (const auto& c : candidates)
            if (std::filesystem::is_directory(c, ec))
            {
                dir = c;
                break;
            }
        if (dir.empty())
        {
            fprintf(stderr,
                    "[vk] no shader cache found. Build one:\n"
                    "     (cd runtime/build && CZ_SHADER_DUMP=/tmp/ucode ./cz_runtime)\n"
                    "     tools/build_shader_spv.sh /tmp/ucode assets/shader_spv\n");
            return false;
        }
    }

    fprintf(stderr, "[vk] shader cache: %s\n", dir.string().c_str());
    uint32_t dropped = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
    {
        if (e.path().extension() != ".spv")
            continue;
        const std::string name = e.path().stem().string();

        ShaderMeta meta;
        if (!LoadShaderMeta(e.path().parent_path() / (name + ".meta.json"), meta))
        {
            // Never a bare continue. An orphaned .spv is a build-pipeline bug, and on
            // the previous port one hid for a session: seven shaders short of the file
            // count, 25,364 draws a run skipped as "unknown", and the only evidence
            // was a module count nobody was comparing.
            fprintf(stderr, "[vk] %s.spv has no readable .meta.json — DROPPED\n",
                    name.c_str());
            ++dropped;
            continue;
        }

        std::ifstream f(e.path(), std::ios::binary);
        std::vector<char> spv((std::istreambuf_iterator<char>(f)), {});
        if (spv.size() < 4 || (spv.size() % 4))
        {
            fprintf(stderr, "[vk] %s.spv is not a SPIR-V module (%zu bytes)\n",
                    name.c_str(), spv.size());
            ++dropped;
            continue;
        }

        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        if (vkCreateShaderModule(R->device, &ci, nullptr, &meta.module) != VK_SUCCESS)
        {
            fprintf(stderr, "[vk] vkCreateShaderModule failed for %s\n", name.c_str());
            ++dropped;
            continue;
        }
        R->shaders.emplace(HashFromName(name), std::move(meta));
    }
    fprintf(stderr, "[vk] %zu shader modules loaded%s\n", R->shaders.size(),
            dropped ? " (see the DROPPED lines above)" : "");
    return !R->shaders.empty();
}

// ===================================================================================
// Immediate submits (uploads outside the frame's own recording)
// ===================================================================================
template <typename Body>
bool RunImmediate(Body&& body)
{
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = R->cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(R->device, &ai, &cb), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    body(cb);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(R->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(R->queue);
    vkFreeCommandBuffers(R->device, R->cmdPool, 1, &cb);
    return true;
}

// ===================================================================================
// Textures
// ===================================================================================
// The tiled address swizzle the XDK exposes as XGAddress2DTiledOffset: the tiled UNIT
// index of (x, y) in a surface `widthUnits` across, where a unit is a texel for plain
// formats and a 4x4 block for the DXT family, and log2bpu is log2 of the unit's size in
// bytes.
//
// This is transcribed hardware behaviour, not something to re-derive from a picture.
// A1's own log states the layouts it loaded ("Loaded tiled 1024x32x1 2D k_8_8_8_8
// texture ... pitch 1024, size 0x00020000"), which is free ground truth for checking
// the sizes this produces.
inline uint32_t Tiled2DOffset(uint32_t x, uint32_t y, uint32_t widthUnits,
                              uint32_t log2bpu)
{
    const uint32_t macro = ((x >> 5) + (y >> 5) * (widthUnits >> 5)) << (log2bpu + 7);
    const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log2bpu;
    const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                            ((y & 8) << (3 + log2bpu)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
           log2bpu;
}

// The fetch constant's component swizzle, as a Vulkan image-view component mapping.
//
// WHY THE RUNTIME HAS TO DO THIS. The swizzle lives in the fetch CONSTANT, which is
// runtime data, so a shader compiled without it cannot bake it in — XenosRecomp emits a
// plain `Sample()` and the mapping has to come from the view.
//
// Where it shows first is TEXT. A font atlas is a single-channel image, and the guest
// routes that one channel to the component its shader reads — commonly alpha. Presented
// as `R8_UNORM` with an identity mapping, Vulkan reads alpha as a constant 1.0, so every
// glyph samples fully opaque and the text renders as SOLID BLOCKS of the right size and
// position. The quad is correct, the sample is not, which is why it looks like a font
// problem rather than a texture-decode one.
VkComponentMapping XenosSwizzle(uint32_t swz)
{
    auto one = [](uint32_t v) -> VkComponentSwizzle {
        switch (v & 7)
        {
            case 0: return VK_COMPONENT_SWIZZLE_R;
            case 1: return VK_COMPONENT_SWIZZLE_G;
            case 2: return VK_COMPONENT_SWIZZLE_B;
            case 3: return VK_COMPONENT_SWIZZLE_A;
            case 4: return VK_COMPONENT_SWIZZLE_ZERO;
            case 5: return VK_COMPONENT_SWIZZLE_ONE;
            // 6 and 7 are "keep", i.e. the component is left as fetched.
            default: return VK_COMPONENT_SWIZZLE_IDENTITY;
        }
    };
    return { one(swz), one(swz >> 3), one(swz >> 6), one(swz >> 9) };
}

// The Xenos texture format to a Vulkan format that reads the same bytes after the
// endian swap. `blockDim` is 4 for the compressed families and 1 otherwise;
// `bytesPerUnit` is the size of one texel or one 4x4 block.
//
// Returning UNDEFINED means "this title uses a format nobody has mapped" — the caller
// substitutes the dummy and names the format once. Guessing would produce a plausible
// wrong image, which is the expensive failure.
VkFormat XenosTextureFormat(uint32_t fmt, uint32_t& bytesPerUnit, uint32_t& blockDim)
{
    blockDim = 1;
    switch (fmt)
    {
        case xenos::kFmt_8:
        case xenos::kFmt_8_A:
        case xenos::kFmt_8_B:
            bytesPerUnit = 1;
            return VK_FORMAT_R8_UNORM;
        case xenos::kFmt_8_8:
            bytesPerUnit = 2;
            return VK_FORMAT_R8G8_UNORM;
        case xenos::kFmt_5_6_5:
            bytesPerUnit = 2;
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case xenos::kFmt_1_5_5_5:
            bytesPerUnit = 2;
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case xenos::kFmt_4_4_4_4:
            bytesPerUnit = 2;
            return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        case xenos::kFmt_8_8_8_8:
            bytesPerUnit = 4;
            return VK_FORMAT_R8G8B8A8_UNORM;
        case xenos::kFmt_2_10_10_10:
            bytesPerUnit = 4;
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case xenos::kFmt_16:
            bytesPerUnit = 2;
            return VK_FORMAT_R16_UNORM;
        case xenos::kFmt_16_16:
            bytesPerUnit = 4;
            return VK_FORMAT_R16G16_UNORM;
        case xenos::kFmt_16_16_16_16:
            bytesPerUnit = 8;
            return VK_FORMAT_R16G16B16A16_UNORM;
        case xenos::kFmt_16_FLOAT:
            bytesPerUnit = 2;
            return VK_FORMAT_R16_SFLOAT;
        case xenos::kFmt_16_16_FLOAT:
            bytesPerUnit = 4;
            return VK_FORMAT_R16G16_SFLOAT;
        case xenos::kFmt_16_16_16_16_FLOAT:
            bytesPerUnit = 8;
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case xenos::kFmt_32_FLOAT:
            bytesPerUnit = 4;
            return VK_FORMAT_R32_SFLOAT;
        case xenos::kFmt_32_32_FLOAT:
            bytesPerUnit = 8;
            return VK_FORMAT_R32G32_SFLOAT;
        case xenos::kFmt_32_32_32_32_FLOAT:
            bytesPerUnit = 16;
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case xenos::kFmt_DXT1:
            bytesPerUnit = 8;
            blockDim = 4;
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case xenos::kFmt_DXT2_3:
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case xenos::kFmt_DXT4_5:
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case xenos::kFmt_DXT5A:
            bytesPerUnit = 8;
            blockDim = 4;
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case xenos::kFmt_DXT3A:
            // DXT3A is a BC2 block with only its explicit-alpha half meaningful.
            // Presented as BC2 so the bytes land where the sampler expects them; the
            // colour half is whatever the asset stored, which for an alpha-only
            // texture the shader does not read.
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case xenos::kFmt_DXN:
            // Two-channel compressed normals. BC5 is the same block layout.
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case xenos::kFmt_16_EXPAND:
            bytesPerUnit = 2;
            return VK_FORMAT_R16_UNORM;
        case xenos::kFmt_16_16_EXPAND:
            bytesPerUnit = 4;
            return VK_FORMAT_R16G16_UNORM;
        case xenos::kFmt_16_16_16_16_EXPAND:
            bytesPerUnit = 8;
            return VK_FORMAT_R16G16B16A16_UNORM;
        case xenos::kFmt_8_8_8_8_A:
        case xenos::kFmt_8_8_8_8_AS_16_16_16_16:
            bytesPerUnit = 4;
            return VK_FORMAT_R8G8B8A8_UNORM;
        case xenos::kFmt_2_10_10_10_AS_16_16_16_16:
            bytesPerUnit = 4;
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case xenos::kFmt_DXT1_AS_16_16_16_16:
            bytesPerUnit = 8;
            blockDim = 4;
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case xenos::kFmt_DXT2_3_AS_16_16_16_16:
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case xenos::kFmt_DXT4_5_AS_16_16_16_16:
            bytesPerUnit = 16;
            blockDim = 4;
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case xenos::kFmt_24_8:
            // A depth surface sampled as a texture. Read the depth half only; the
            // stencil byte has no sampled meaning here.
            bytesPerUnit = 4;
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            bytesPerUnit = 0;
            return VK_FORMAT_UNDEFINED;
    }
}

} // namespace (the anonymous one; DecodeTextureFetch below is xenos.h's, and must
  // have external linkage or it will not be the function that header declared)

namespace xenos {
TextureFetch DecodeTextureFetch(const uint32_t* regs, uint32_t slot)
{
    const uint32_t d0 = regs[kFetchConstantBase + slot * 6 + 0];
    const uint32_t d1 = regs[kFetchConstantBase + slot * 6 + 1];
    const uint32_t d2 = regs[kFetchConstantBase + slot * 6 + 2];
    const uint32_t d3 = regs[kFetchConstantBase + slot * 6 + 3];
    const uint32_t d4 = regs[kFetchConstantBase + slot * 6 + 4];

    TextureFetch t{};
    // dword0: type:2, sign_x/y/z/w:2 each, clamp_x/y/z:3 each, pitch:9 @22, tiled:1 @31
    t.type = d0 & 3;
    t.signX = (d0 >> 2) & 3;
    t.signY = (d0 >> 4) & 3;
    t.signZ = (d0 >> 6) & 3;
    t.signW = (d0 >> 8) & 3;
    t.clampX = (d0 >> 10) & 7;
    t.clampY = (d0 >> 13) & 7;
    t.clampZ = (d0 >> 16) & 7;
    t.pitchBlocks = (d0 >> 22) & 0x1FF;
    t.tiled = ((d0 >> 31) & 1) != 0;
    // dword1: format:6, endian:2, request_size:2, stacked:1, clamp_policy:1, base:20 @12
    t.format = d1 & 0x3F;
    t.endian = (d1 >> 6) & 3;
    t.address = (d1 >> 12) << 12;
    // dword2 for a 2D texture: width:13, height:13
    t.width = (d2 & 0x1FFF) + 1;
    t.height = ((d2 >> 13) & 0x1FFF) + 1;
    t.depth = 1;
    // dword3: num_format:1, swizzle:12, exp_adjust:6, mag:2, min:2, mip:2, aniso:3
    t.swizzle = (d3 >> 1) & 0xFFF;
    t.filterMag = (d3 >> 19) & 3;
    t.filterMin = (d3 >> 21) & 3;
    t.filterMip = (d3 >> 23) & 3;
    // dword4: mip_min_level bits 2..5, mip_max_level bits 6..9
    t.mipMin = (d4 >> 2) & 0xF;
    t.mipMax = (d4 >> 6) & 0xF;
    t.dimension = 1; // see the note at the call site: dimension is taken from the shader
    return t;
}
} // namespace xenos

namespace {

// Upload the texture a fetch constant describes and return its bindless slot, or 0 for
// the dummy. Cached on the fetch constant's own six dwords: if none of them changed the
// texture is the same texture, and if any did it is a different one. Keying on the base
// address alone would be wrong in this title, which reuses addresses.
uint32_t UploadTexture(uint8_t* base, const uint32_t* regs, uint32_t constIdx)
{
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < 6; i++)
    {
        key ^= regs[xenos::kFetchConstantBase + constIdx * 6 + i];
        key *= 1099511628211ull;
    }
    const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
    if (t.type != 2)
    {
        Count("texture: fetch constant is not a texture");
        return 0;
    }

    // SERVED FROM A RESOLVE SNAPSHOT, when this fetch names a surface another pass in
    // this frame resolved to. This is not an optimisation — it is the only way the
    // fetch can succeed at all, because the resolved pixels were never written into
    // guest memory. Without it a post-processing chain samples whatever the guest's
    // allocator left at that address, which is usually zero, and the compose draws
    // black over the frame it was supposed to combine.
    //
    // Deliberately NOT cached in R->textures: a snapshot's contents change every
    // frame while its fetch constant does not, so caching it on the fetch constant
    // would freeze the first frame's version of the surface forever.
    //
    // THE SNAPSHOT IS CHECKED BEFORE THE CACHE, and that ordering is the whole point.
    // It used to be checked after, so the "not cached" rule only held for a surface
    // whose FIRST fetch already had a snapshot. This title's colour-grading LUT is
    // resolved LATE in a frame and sampled EARLY in the next one, so its very first
    // fetch — during the boot, before any pass had resolved it — fell through to guest
    // memory, uploaded whatever the allocator had left there, and cached that under
    // the fetch constant. The fetch constant never changed again, so every subsequent
    // frame took the cache-hit path and the tone map sampled a dead first-frame
    // upload for the rest of the run. One stale entry, and the entire scene composed
    // black while every instrument reported a healthy chain: the LUT's own snapshot
    // was 99.9% non-black, the tone map's four other inputs were live snapshots, its
    // colour mask was F and its constants were sane.
    //
    // CZ_VK_TEX_CACHE_FIRST=1 restores the old order — the same-binary control arm for
    // every claim about this fix.
    static const bool cacheFirst = EnvOn("CZ_VK_TEX_CACHE_FIRST");
    if (cacheFirst)
    {
        auto c = R->textures.find(key);
        if (c != R->textures.end())
        {
            Count("texture: cache hit");
            return c->second.slot;
        }
    }
    {
        auto snap = R->snapshots.find(t.address & 0x1FFFFFFF);
        if (g_texCensus)
        {
            TexSource& s = g_texSources[t.address & 0x1FFFFFFF];
            s.width = t.width;
            s.height = t.height;
            s.format = t.format;
            if (snap != R->snapshots.end())
            {
                s.everResolved = true;
                if (snap->second.frameSeen + 1 >= R->frame)
                    s.fromSnapshot++;
                else
                {
                    s.snapshotTooOld++;
                    s.maxAge = std::max(s.maxAge, R->frame - snap->second.frameSeen);
                }
            }
        }
        if (snap != R->snapshots.end() && snap->second.frameSeen + 1 >= R->frame)
        {
            Count("texture: served from a resolve snapshot");
            const uint32_t key = t.address & 0x1FFFFFFF;
            if (std::find(R->snapshotsSampledThisPass.begin(),
                          R->snapshotsSampledThisPass.end(),
                          key) == R->snapshotsSampledThisPass.end())
                R->snapshotsSampledThisPass.push_back(key);
            return snap->second.slot;
        }
        if (snap != R->snapshots.end())
            Count("texture: resolve snapshot too old, falling back to guest memory");
    }

    auto cached = R->textures.find(key);
    if (cached != R->textures.end())
    {
        Count("texture: cache hit");
        return cached->second.slot;
    }

    uint32_t bytesPerUnit = 0, blockDim = 1;
    const VkFormat format = XenosTextureFormat(t.format, bytesPerUnit, blockDim);
    if (format == VK_FORMAT_UNDEFINED)
    {
        static std::vector<uint32_t> seen;
        if (std::find(seen.begin(), seen.end(), t.format) == seen.end())
        {
            seen.push_back(t.format);
            fprintf(stderr,
                    "[vk] unmapped Xenos texture format %u (%ux%u) — using the dummy; "
                    "add it to XenosTextureFormat\n",
                    t.format, t.width, t.height);
        }
        Count("texture: unmapped format");
        return 0;
    }
    if (!t.width || !t.height || t.width > 4096 || t.height > 4096)
    {
        Count("texture: implausible extent");
        return 0;
    }

    // CZ_VK_NO_TEX_SWIZZLE=1 restores the identity mapping, so the change is
    // measurable in the same binary — and it is one of the few renderer changes a
    // human can adjudicate instantly, because the symptom is readable text or not.
    static const bool noSwizzle = EnvOn("CZ_VK_NO_TEX_SWIZZLE");

    const uint32_t unitW = (t.width + blockDim - 1) / blockDim;
    const uint32_t unitH = (t.height + blockDim - 1) / blockDim;
    // The stored pitch is in blocks of 32 units; a fetch constant with no pitch means
    // the surface is 32-unit aligned from its width.
    const uint32_t pitchUnits =
        t.pitchBlocks ? t.pitchBlocks * 32 / blockDim : ((unitW + 31) & ~31u);
    // A tiled surface is stored in 32x32-unit macro tiles, so its row count is rounded
    // up the same way its pitch is. Reading only `unitH` rows of a tiled surface reads
    // the right number of BYTES from the wrong PLACES, which produces a scrambled image
    // rather than a truncated one.
    const uint32_t srcRows = t.tiled ? ((unitH + 31) & ~31u) : unitH;
    const uint32_t srcPitchUnits = t.tiled ? ((pitchUnits + 31) & ~31u) : pitchUnits;
    const uint64_t srcBytes = uint64_t(srcPitchUnits) * srcRows * bytesPerUnit;

    const uint32_t va = PhysToVa(t.address);
    if (!GuestRangeOk(va, srcBytes))
    {
        Count("texture: source outside the physical arena");
        return 0;
    }

    // Untile (or copy) into a tightly packed staging image, swapping endianness as the
    // fetch constant asks. The destination is `unitW` wide because that is what the
    // Vulkan image is; the source is read at its own pitch.
    const uint64_t dstBytes = uint64_t(unitW) * unitH * bytesPerUnit;
    std::vector<uint8_t> pixels(dstBytes);
    const uint8_t* src = base + va;

    if (t.tiled)
    {
        uint32_t log2bpu = 0;
        while ((1u << log2bpu) < bytesPerUnit)
            ++log2bpu;
        if ((1u << log2bpu) != bytesPerUnit)
        {
            // The swizzle is defined in terms of a power-of-two unit size. A format
            // that is not one cannot be untiled by this routine, and pretending
            // otherwise would scramble it silently.
            Count("texture: tiled with a non-power-of-two unit, skipped");
            return 0;
        }
        for (uint32_t y = 0; y < unitH; y++)
            for (uint32_t x = 0; x < unitW; x++)
            {
                const uint32_t unit = Tiled2DOffset(x, y, srcPitchUnits, log2bpu);
                const uint64_t off = uint64_t(unit) * bytesPerUnit;
                if (off + bytesPerUnit > srcBytes)
                    continue;
                CopySwapped(&pixels[(uint64_t(y) * unitW + x) * bytesPerUnit], src + off,
                            bytesPerUnit, t.endian);
            }
        Count("texture: untiled");
    }
    else
    {
        for (uint32_t y = 0; y < unitH; y++)
            CopySwapped(&pixels[uint64_t(y) * unitW * bytesPerUnit],
                        src + uint64_t(y) * srcPitchUnits * bytesPerUnit,
                        uint64_t(unitW) * bytesPerUnit, t.endian);
        Count("texture: linear");
    }

    if (g_texCensus)
    {
        TexSource& s = g_texSources[t.address & 0x1FFFFFFF];
        s.width = t.width;
        s.height = t.height;
        s.format = t.format;
        s.uploads++;
        s.src = src;
        s.srcBytes = srcBytes;
        bool allZero = true;
        for (uint8_t b : pixels)
            if (b)
            {
                allZero = false;
                break;
            }
        if (allZero)
            s.zeroUploads++;
    }

    if (R->nextTextureSlot >= kMaxDescriptors)
    {
        Count("texture: bindless heap full");
        return 0;
    }

    TextureEntry entry;
    entry.key = key;
    entry.slot = R->nextTextureSlot++;
    // A COUNTER, NOT A REPAIR. An upload whose every texel is zero is this runtime
    // saying out loud that it had nothing to give, and one of those (0364B000, a 16x16
    // DXT1) is drawn over the save-slot thumbnails on the new-game screen as three
    // opaque black boxes. The obvious repair — treat it as provisional and re-upload
    // until the guest fills it — was built and MEASURED, and it fires zero times:
    // none of this boot's 58 all-black uploads ever becomes non-zero at its own texels.
    // See docs/phase5-notes.md 6aa; the counter stays because it is what named the
    // texture.
    {
        bool allZero = true;
        for (uint8_t b : pixels)
            if (b)
            {
                allZero = false;
                break;
            }
        if (allZero)
            Count("texture: uploaded entirely BLACK (the guest has not written it)");
    }
    if (!CreateImage(entry.image, t.width, t.height, format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1, 1,
                     noSwizzle ? VkComponentMapping{} : XenosSwizzle(t.swizzle)))
    {
        Count("texture: image creation failed");
        --R->nextTextureSlot;
        return 0;
    }

    // Stage through the upload buffer. Sized once at init; a texture larger than it
    // is counted and dropped rather than silently truncated.
    if (dstBytes > R->staging.size)
    {
        Count("texture: larger than the staging buffer");
        --R->nextTextureSlot;
        return 0;
    }
    ++R->guestTexturesThisPass;
    memcpy(R->staging.mapped, pixels.data(), dstBytes);

    RunImmediate([&](VkCommandBuffer cb) {
        Barrier(cb, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { t.width, t.height, 1 };
        vkCmdCopyBufferToImage(cb, R->staging.buffer, entry.image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cb, entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
    });

    VkDescriptorImageInfo ii{};
    ii.imageView = entry.image.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = R->sets[0];
    w.dstBinding = 0;
    w.dstArrayElement = entry.slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, nullptr);

    const uint32_t slot = entry.slot;
    R->textures.emplace(key, std::move(entry));
    Count("texture: uploaded");
    return slot;
}

// ===================================================================================
// Per-draw state decode
// ===================================================================================
// The Xenos primitive type to a Vulkan topology, plus whether the indices have to be
// rewritten to express it.
//
// Xenos has two topologies Vulkan does not: the QUAD LIST (four corners per quad) and
// the RECTANGLE LIST (three corners, hardware synthesises the fourth). Both are
// expressible as a triangle list with a rewritten index buffer, which is what
// ExpandIndices below does — and expressing them as a plain triangle list WITHOUT the
// rewrite is the trap, because it silently renders a fraction of every primitive: a
// quad list drawn as triangles produces one wrong triangle per quad rather than
// nothing, which looks like corrupt geometry instead of a missing feature.
enum class Expansion
{
    None,
    QuadList,      // 4 corners -> 2 triangles
    RectangleList, // 3 corners -> 2 triangles, the fourth corner reflected
};

VkPrimitiveTopology XenosTopology(uint32_t prim, bool& supported, Expansion& expand)
{
    supported = true;
    expand = Expansion::None;
    switch (prim)
    {
        case xenos::kPointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case xenos::kLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case xenos::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case xenos::kTriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case xenos::kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case xenos::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case xenos::kQuadList:
            expand = Expansion::QuadList;
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case xenos::kRectangleList:
            expand = Expansion::RectangleList;
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        default:
            supported = false;
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkBlendFactor XenosBlendFactor(uint32_t f)
{
    switch (f)
    {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp XenosBlendOp(uint32_t op)
{
    switch (op)
    {
        case 0: return VK_BLEND_OP_ADD;
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp XenosCompareOp(uint32_t f)
{
    switch (f & 7)
    {
        case 0: return VK_COMPARE_OP_NEVER;
        case 1: return VK_COMPARE_OP_LESS;
        case 2: return VK_COMPARE_OP_EQUAL;
        case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4: return VK_COMPARE_OP_GREATER;
        case 5: return VK_COMPARE_OP_NOT_EQUAL;
        case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

// ===================================================================================
// Pipelines
// ===================================================================================
VkPipeline GetPipeline(const PipelineKey& key, const ShaderMeta& vs, const ShaderMeta& ps)
{
    auto it = R->pipelines.find(key);
    if (it != R->pipelines.end())
        return it->second;

    // --- vertex input, straight out of the vertex shader's own declaration ---------
    // One Vulkan binding per attribute rather than one per stream. The Xenos vertex
    // fetch names an address, a stride and an offset per attribute, and two attributes
    // of one shader routinely come from different guest buffers — so "a stream" is not
    // a thing the shader declares, and inventing one would mean deciding which fetches
    // share a buffer from data that does not say.
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue; // dependent fetch: the shader reads the stream itself
        const VkFormat f = XenosVertexFormat(a.format, a.isSigned, a.isInteger);
        if (f == VK_FORMAT_UNDEFINED)
        {
            static std::vector<uint32_t> seen;
            if (std::find(seen.begin(), seen.end(), a.format) == seen.end())
            {
                seen.push_back(a.format);
                fprintf(stderr,
                        "[vk] REFUSED pipeline: unmapped Xenos vertex format %u "
                        "(vs=%016llx location=%d) — add it to XenosVertexFormat\n",
                        a.format, (unsigned long long)key.vsHash, a.location);
            }
            Count("pipeline: refused, unmapped vertex format");
            R->pipelines.emplace(key, VK_NULL_HANDLE);
            return VK_NULL_HANDLE;
        }
        // A mapped format the DEVICE cannot use as a vertex buffer is a different
        // failure from an unmapped one and has to say so by name. The SCALED formats
        // in particular are the ones drivers most often omit, and a pipeline created
        // with an unsupported vertex format is undefined behaviour that presents as
        // wrong geometry rather than as an error.
        {
            static std::vector<uint32_t> checked;
            if (std::find(checked.begin(), checked.end(), uint32_t(f)) == checked.end())
            {
                checked.push_back(uint32_t(f));
                VkFormatProperties fp{};
                vkGetPhysicalDeviceFormatProperties(R->physical, f, &fp);
                if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                    fprintf(stderr,
                            "[vk] DEVICE CANNOT USE VkFormat %u as a vertex buffer "
                            "(Xenos format %u, signed=%u integer=%u) — geometry using "
                            "it will be wrong\n",
                            uint32_t(f), a.format, a.isSigned, a.isInteger);
            }
        }
        const uint32_t binding = uint32_t(bindings.size());
        VkVertexInputBindingDescription b{};
        b.binding = binding;
        b.stride = a.strideDwords * 4;
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // A zero stride is legal in Vulkan and means "every vertex reads the same
        // element", which is exactly what a Xenos fetch with stride 0 does.
        bindings.push_back(b);

        VkVertexInputAttributeDescription at{};
        at.location = uint32_t(a.location);
        at.binding = binding;
        at.format = f;
        at.offset = 0; // the element offset is folded into the bind offset
        attributes.push_back(at);
    }

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    ia.topology = VkPrimitiveTopology(key.topology);
    ia.primitiveRestartEnable = key.primRestart ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    // Culling is deliberately DISABLED for now, and that is a decision rather than an
    // omission. PA_SU_SC_MODE_CNTL's front-face bit interacts with the viewport's Y
    // sign, and getting the combination wrong culls exactly the geometry that should
    // be visible — which reads as "the renderer draws nothing" rather than as a
    // winding bug. Draw both faces until there is a picture to check the winding
    // against, then turn it on as a measured change.
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // RB_DEPTHCONTROL: stencil_enable:1, z_enable:1, z_write_enable:1, ?:1,
    // zfunc:3 @4, backface_enable:1 @7.
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    // CZ_VK_NO_DEPTH_TEST=1 — an ARM, never a fix: draw everything regardless of
    // depth. It separates "this geometry was never submitted" from "this geometry was
    // submitted and rejected by depth left over from another pass", which look
    // identical in a snapshot and have completely different causes.
    static const bool noDepthTest = getenv("CZ_VK_NO_DEPTH_TEST") != nullptr;
    ds.depthTestEnable =
        (!noDepthTest && ((key.depthControl >> 1) & 1)) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ((key.depthControl >> 2) & 1) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = XenosCompareOp((key.depthControl >> 4) & 7);
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    // RB_BLENDCONTROL: color_srcblend:5, color_comb_fcn:3 @5, color_destblend:5 @8,
    // alpha_srcblend:5 @16, alpha_comb_fcn:3 @21, alpha_destblend:5 @24.
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0;
    if (key.colorMask & 1) cb.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (key.colorMask & 2) cb.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (key.colorMask & 4) cb.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (key.colorMask & 8) cb.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    cb.srcColorBlendFactor = XenosBlendFactor(key.blendControl & 0x1F);
    cb.colorBlendOp = XenosBlendOp((key.blendControl >> 5) & 7);
    cb.dstColorBlendFactor = XenosBlendFactor((key.blendControl >> 8) & 0x1F);
    cb.srcAlphaBlendFactor = XenosBlendFactor((key.blendControl >> 16) & 0x1F);
    cb.alphaBlendOp = XenosBlendOp((key.blendControl >> 21) & 7);
    cb.dstAlphaBlendFactor = XenosBlendFactor((key.blendControl >> 24) & 0x1F);
    // Blending "off" on Xenos is ONE/ZERO/ADD, which is what a disabled blend does —
    // so rather than track a separate enable bit, enable blending whenever the factors
    // are not the identity. Cheaper to reason about and impossible to get out of step.
    cb.blendEnable = !(cb.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
                       cb.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                       cb.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                       cb.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO)
                        ? VK_TRUE
                        : VK_FALSE;

    VkPipelineColorBlendStateCreateInfo bs{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    bs.attachmentCount = 1;
    bs.pAttachments = &cb;

    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                   VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dsi{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dsi.dynamicStateCount = 3;
    dsi.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps.module;
    stages[1].pName = "main";

    const VkFormat colorFormat = R->color.format;
    VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &colorFormat;
    rci.depthAttachmentFormat = R->depth.format;
    rci.stencilAttachmentFormat = R->depth.format;

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext = &rci;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &bs;
    pci.pDynamicState = &dsi;
    pci.layout = R->pipeLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r =
        vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "[vk] vkCreateGraphicsPipelines failed (%d) vs=%016llx ps=%016llx\n",
                int(r), (unsigned long long)key.vsHash, (unsigned long long)key.psHash);
        Count("pipeline: creation failed");
        pipeline = VK_NULL_HANDLE;
    }
    else
    {
        Count("pipeline: created");
    }
    R->pipelines.emplace(key, pipeline);
    return pipeline;
}

// ===================================================================================
// Frame lifecycle
// ===================================================================================
void BeginFrame()
{
    if (R->recording)
        return;
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(R->cmd, 0);
    vkBeginCommandBuffer(R->cmd, &bi);
    R->recording = true;
    R->rendering = false;
    R->arenaCursor = 0;
    R->streamCache.clear();
    R->drawsThisFrame = 0;
}

void BeginRendering()
{
    if (R->rendering)
        return;
    Barrier(R->cmd, R->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(R->cmd, R->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    VkRenderingAttachmentInfo colorAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    colorAtt.imageView = R->color.view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // LOAD, never CLEAR. The EDRAM keeps its contents between the packets that
    // reference it, and the title clears through the copy block's clear bits rather
    // than with a draw — so clearing here would be inventing a clear and discarding
    // content that later passes sample.
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depthAtt.imageView = R->depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { R->color.width, R->color.height } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(R->cmd, &ri);
    R->rendering = true;
}

void EndRendering()
{
    if (!R->rendering)
        return;
    vkCmdEndRendering(R->cmd);
    R->rendering = false;
}

// Submit whatever has been recorded and wait. Synchronous on purpose: the guest's own
// ring flow control already paces this runtime (findings 38-39), and a second,
// host-side pipelining scheme would make "which frame is on screen" a question with
// two answers.
void SubmitAndWait()
{
    if (!R->recording)
        return;
    EndRendering();
    vkEndCommandBuffer(R->cmd);
    R->recording = false;

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &R->cmd;
    vkResetFences(R->device, 1, &R->fence);
    vkQueueSubmit(R->queue, 1, &si, R->fence);
    vkWaitForFences(R->device, 1, &R->fence, VK_TRUE, UINT64_MAX);
}

// ===================================================================================
// The draw
// ===================================================================================
// Copy a guest vertex/index stream into the frame arena once, dword-swapped, and
// return its arena offset. Cached per frame by (address, size, endian): the frontend
// draws the same buffer dozens of times a frame and copying it each time would be the
// dominant cost of the renderer.
VkDeviceSize UploadStream(uint8_t* base, uint32_t va, uint64_t bytes, uint32_t endian)
{
    // The key must be an IDENTITY, not a hash. The first version was
    // `(uint64_t(va) << 24) ^ (bytes << 2) ^ endian`, and those fields OVERLAP: a
    // 32-bit address shifted 24 occupies bits 24..55 and a byte count shifted 2
    // occupies bits 2..31, so two different (address, size) pairs can collide. A
    // collision here does not corrupt memory — it hands a draw ANOTHER MESH'S vertex
    // stream, which draws triangles between unrelated vertices. Packing the fields into
    // disjoint bits instead makes the key exact rather than probably-unique.
    const uint64_t key = (uint64_t(va) << 32) | (uint64_t(bytes & 0x3FFFFFFFu) << 2) |
                         (endian & 3);
    auto it = R->streamCache.find(key);
    if (it != R->streamCache.end())
        return it->second;

    const VkDeviceSize at = ArenaAlloc(bytes, 16);
    if (at == VkDeviceSize(-1))
        return at;
    CopySwapped(R->arena.mapped + at, base + va, size_t(bytes), endian);
    R->streamCache.emplace(key, at);
    return at;
}

// Read one index from a guest index buffer, honouring the buffer's endian code, or
// return the vertex number itself for an auto-index draw.
inline uint32_t ReadIndex(const uint8_t* p, uint32_t i, bool index32, uint32_t endian,
                          bool haveBuffer)
{
    if (!haveBuffer)
        return i;
    if (index32)
    {
        uint32_t v;
        memcpy(&v, p + i * 4, 4);
        uint8_t tmp[4];
        memcpy(tmp, &v, 4);
        CopySwapped(reinterpret_cast<uint8_t*>(&v), tmp, 4, endian);
        return v;
    }
    uint16_t v;
    memcpy(&v, p + i * 2, 2);
    // A 16-bit index stream under an 8-in-32 code has its PAIRS swapped as well as
    // its bytes, because the code describes a dword-wide swizzle and the hardware
    // applies it to the dword. Reading the pair back at the same dword offset is what
    // reproduces that; treating the code as if it were per-index would silently
    // transpose every pair of triangles' vertices.
    if ((endian & 3) == 2)
    {
        uint32_t d;
        memcpy(&d, p + (i & ~1u) * 2, 4);
        d = __builtin_bswap32(d);
        return (i & 1) ? (d >> 16) : (d & 0xFFFF);
    }
    if ((endian & 3) == 1 || (endian & 3) == 3)
        v = uint16_t((v >> 8) | (v << 8));
    return v;
}

// Build a FOUR-vertex stream for one rectangle-list draw: the three real corners
// followed by the one the hardware synthesises, `r3 = r0 + r2 - r1`.
//
// `at` is where the guest stream already sits in the arena (dword-swapped), `stride` is
// the fetch's stride in dwords, and `corner` the three vertex indices this draw uses.
// Returns the arena offset of the new four-record stream, which is bound in place of
// the guest's — so the same attribute offsets still apply.
//
// The extrapolation is done on FLOAT dwords, which is exact for a 32-bit float
// attribute and is what hardware does to every attribute of a rect. A packed format
// would need its own arithmetic; that case copies r0 and counts itself, because a
// wrong fourth corner that says nothing is the failure mode this whole function exists
// to remove.
VkDeviceSize SynthRectStream(VkDeviceSize at, uint64_t streamBytes, uint32_t strideDwords,
                             const uint32_t corner[3], uint32_t format)
{
    const uint32_t stride = strideDwords * 4;
    for (uint32_t k = 0; k < 3; k++)
    {
        if (uint64_t(corner[k] + 1) * stride > streamBytes)
        {
            Count("draw: rect corner past the end of its stream");
            return VkDeviceSize(-1);
        }
    }
    const VkDeviceSize out = ArenaAlloc(uint64_t(stride) * 4, 16);
    if (out == VkDeviceSize(-1))
        return out;
    uint8_t* dst = R->arena.mapped + out;
    const uint8_t* src = R->arena.mapped + at;
    for (uint32_t k = 0; k < 3; k++)
        memcpy(dst + uint64_t(k) * stride, src + uint64_t(corner[k]) * stride, stride);
    // 57 = 32_32_32_FLOAT, 38 = 32_32_32_32_FLOAT, 37 = 32_32_FLOAT, 36 = 32_FLOAT.
    // Anything else in this record is not a float dword and the combination below is
    // not defined for it.
    const bool floatFormat = format == 36 || format == 37 || format == 38 || format == 57;
    if (!floatFormat)
    {
        Count("draw: rect fourth corner copied (attribute is not 32-bit float)");
        memcpy(dst + uint64_t(3) * stride, dst, stride);
        return out;
    }
    for (uint32_t d = 0; d < strideDwords; d++)
    {
        float a, b, c;
        memcpy(&a, dst + 0 * stride + d * 4, 4);
        memcpy(&b, dst + 1 * stride + d * 4, 4);
        memcpy(&c, dst + 2 * stride + d * 4, 4);
        const float v = a + c - b;
        memcpy(dst + 3 * stride + d * 4, &v, 4);
    }
    Count("draw: rect fourth corner synthesised");
    return out;
}

// Rewrite a quad or rectangle list as a triangle list. Returns the arena offset of a
// 32-bit index buffer and its count, or -1.
//
// A rectangle list stores three corners and the hardware generates the fourth, so the
// expanded indices for one rect are (0,1,2) and (0,2,3) into the FOUR-record stream
// SynthRectStream built for this draw — not into the guest's stream. That indirection
// is the whole reason the synthesised corner is possible at all: an index rewrite on
// its own cannot name a vertex that does not exist, which is why this used to emit the
// same triangle twice and cover half of every rect.
VkDeviceSize ExpandIndices(uint8_t* base, const Pm4Draw& draw, Expansion expand,
                           uint32_t& outCount)
{
    const bool haveBuffer = draw.indexed;
    const uint8_t* src = haveBuffer ? base + draw.indexVa : nullptr;
    const uint32_t perPrim = expand == Expansion::QuadList ? 4u : 3u;
    const uint32_t prims = draw.indexCount / perPrim;
    if (!prims)
    {
        Count("draw: expansion with no complete primitive");
        return VkDeviceSize(-1);
    }

    outCount = prims * 6;
    const VkDeviceSize at = ArenaAlloc(uint64_t(outCount) * 4, 16);
    if (at == VkDeviceSize(-1))
        return at;
    uint32_t* dst = reinterpret_cast<uint32_t*>(R->arena.mapped + at);

    for (uint32_t p = 0; p < prims; p++)
    {
        uint32_t v[4];
        for (uint32_t k = 0; k < perPrim; k++)
            v[k] = ReadIndex(src, p * perPrim + k, draw.index32, draw.indexEndian,
                             haveBuffer);
        if (expand == Expansion::QuadList)
        {
            dst[p * 6 + 0] = v[0]; dst[p * 6 + 1] = v[1]; dst[p * 6 + 2] = v[2];
            dst[p * 6 + 3] = v[0]; dst[p * 6 + 4] = v[2]; dst[p * 6 + 5] = v[3];
        }
        else
        {
            // Into the four-record synthetic stream, not the guest's: 0,1,2 are the
            // corners as fetched and 3 is the one SynthRectStream extrapolated.
            // CZ_VK_RECT_HALF=1 restores the old same-triangle-twice expansion — the
            // same-binary control arm for the missing clear.
            //
            // Only a SINGLE-rect draw gets the synthetic stream — every rect list this
            // title issues is exactly 3 indices. A multi-rect draw would need four
            // records per rect and falls back to the old half-covering expansion,
            // counted so it is a number rather than a surprise.
            static const bool half = EnvOn("CZ_VK_RECT_HALF");
            if (half || prims != 1)
            {
                if (prims != 1)
                    Count("draw: multi-rect list, fourth corners NOT synthesised");
                dst[p * 6 + 0] = v[0]; dst[p * 6 + 1] = v[1]; dst[p * 6 + 2] = v[2];
                dst[p * 6 + 3] = v[0]; dst[p * 6 + 4] = v[2]; dst[p * 6 + 5] = v[1];
            }
            else
            {
                dst[0] = 0; dst[1] = 1; dst[2] = 2;
                dst[3] = 0; dst[4] = 2; dst[5] = 3;
            }
        }
    }
    Count(expand == Expansion::QuadList ? "draw: quad list expanded"
                                        : "draw: rectangle list expanded");
    return at;
}

// The register file and shader bindings are PARAMETERS, not globals: the PM4 feed
// passes pm4.cpp's, the D3D feed (phase C) passes the private file its walker built
// from the title's own flush output. Everything below is feed-agnostic.
void DoDraw(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
            const Pm4ShaderBinding& vsBind, const Pm4ShaderBinding& psBind)
{
    if (!vsBind.hash || !psBind.hash)
    {
        Count("draw: no shader bound");
        return;
    }

    auto vsIt = R->shaders.find(vsBind.hash);
    auto psIt = R->shaders.find(psBind.hash);
    if (vsIt == R->shaders.end() || psIt == R->shaders.end())
    {
        // Naming the missing hash is what makes this actionable: the [imload] line
        // for that hash says which stage and how big, and the two together are enough
        // to add it to the cache without another run.
        static std::vector<uint64_t> reported;
        const uint64_t missing = vsIt == R->shaders.end() ? vsBind.hash : psBind.hash;
        if (std::find(reported.begin(), reported.end(), missing) == reported.end())
        {
            reported.push_back(missing);
            fprintf(stderr, "[vk] no translated shader for %s %016llx — draws skipped\n",
                    vsIt == R->shaders.end() ? "VS" : "PS",
                    (unsigned long long)missing);
        }
        Count("draw: shader not in the cache");
        return;
    }
    const ShaderMeta& vs = vsIt->second;
    const ShaderMeta& ps = psIt->second;

    // A per-primitive-type census, always on. Which topologies a title actually issues
    // is a fact about the title, and it is the difference between "quad lists are
    // unsupported" and "quad lists are 0.2% of the stream" — the second is a decision
    // and the first is only an alarm.
    {
        static char names[64][32];
        static bool built = false;
        if (!built)
        {
            built = true;
            for (uint32_t i = 0; i < 64; i++)
                snprintf(names[i], sizeof names[i], "prim %02u", i);
        }
        Count(names[draw.primType & 63]);
    }

    bool topologySupported = false;
    Expansion expand = Expansion::None;
    const VkPrimitiveTopology topology =
        XenosTopology(draw.primType, topologySupported, expand);
    if (!topologySupported)
    {
        static std::vector<uint32_t> seen;
        if (std::find(seen.begin(), seen.end(), draw.primType) == seen.end())
        {
            seen.push_back(draw.primType);
            fprintf(stderr, "[vk] unsupported Xenos primitive type %u — draws skipped\n",
                    draw.primType);
        }
        Count("draw: unsupported primitive type");
        return;
    }
    if (!draw.indexCount)
    {
        Count("draw: zero indices");
        return;
    }

    PipelineKey key{};
    key.vsHash = vsBind.hash;
    key.psHash = psBind.hash;
    key.topology = uint32_t(topology);
    key.blendControl = regs[xenos::kRbBlendControl0];
    // CZ_VK_FORCE_COLORMASK=1 — treat every draw as writing all four channels.
    //
    // The arm for "is RB_COLOR_MASK really at 0x2104, and is an empty mask really what
    // the guest meant?". 38.6% of this title's draws come through with an empty mask,
    // which is either a legitimate depth-only pass or a register read at the wrong
    // index, and those two are indistinguishable from the picture. A same-binary arm
    // separates them in one run each; reading the register table harder cannot.
    static const bool forceColorMask = EnvOn("CZ_VK_FORCE_COLORMASK");
    key.colorMask = forceColorMask ? 0xF : (regs[xenos::kRbColorMask] & 0xF);
    key.depthControl = regs[xenos::kRbDepthControl] & 0xFF;
    key.modeControl = regs[0x2208] & 7;

    // PRIMITIVE RESTART — OFF, and that is a measurement rather than an omission.
    //
    // Xenos can pack many strips into one draw separated by a reset index
    // (`VGT_MULTI_PRIM_IB_RESET_INDX`, 0x2103), and welded strips are an extremely good
    // fit for this title's remaining defect: long thin triangles stretching between
    // unrelated parts of a mesh look exactly like a broken vertex transform.
    //
    // So it was tried, with Vulkan's fixed reset index (0xFFFF / 0xFFFFFFFF). One run
    // each said it made the scene worse (81.3% non-black -> 67.6%) and that conclusion
    // was WRONG, because the metric it rests on is not stable: this title's title
    // screen renders an ANIMATED 3D background, so a snapshot taken at frame 600 is a
    // different camera angle every run. Alternated 3 against 3, the same binary gives
    // 100.0 / 64.1 / 97.5% with restart off and 64.4 / 94.8 / 79.6% with it on — ranges
    // that overlap completely. The A/B is INCONCLUSIVE, not negative.
    //
    // Off is therefore the conservative default (it is the pre-existing behaviour), and
    // CZ_VK_PRIM_RESTART=1 is the arm. Deciding this needs a frame-aligned comparison
    // rather than a coverage percentage — the same lesson gotcha 38 records for the GPU
    // gate, arriving from the renderer's side.
    const bool restartable = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                             topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                             topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    static const bool wantRestart = EnvOn("CZ_VK_PRIM_RESTART");
    key.primRestart = (wantRestart && restartable && draw.indexed) ? 1 : 0;

    // CZ_VK_ONLY_VS=<hex[,hex...]> / CZ_VK_SKIP_VS=<hex[,hex...]> — render only, or all
    // but, the draws using those vertex shaders.
    //
    // The bisection arms. When every INPUT to a draw has been verified individually and
    // the output is still wrong, the question stops being "which value is wrong" and
    // becomes "which draws are wrong" — and that is answered by rendering them one
    // shader at a time and looking, not by more reading.
    {
        static const char* only = Env("CZ_VK_ONLY_VS");
        static const char* skip = Env("CZ_VK_SKIP_VS");
        if (only || skip)
        {
            char hex[24];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)vsBind.hash);
            if (only && !strstr(only, hex))
            {
                Count("draw: filtered out (CZ_VK_ONLY_VS)");
                return;
            }
            if (skip && strstr(skip, hex))
            {
                Count("draw: filtered out (CZ_VK_SKIP_VS)");
                return;
            }
        }
    }

    // Index width, counted: a draw whose 16-bit indices are read as 32-bit (or the
    // reverse) addresses entirely wrong vertices, which is one of the few remaining
    // shapes that produces triangles between unrelated points.
    if (draw.indexed)
        Count(draw.index32 ? "draw: 32-bit indices" : "draw: 16-bit indices");

    // CZ_VK_SHADER_CENSUS=1 — draws per (vs, ps) pair, in the stats block. Which
    // shader pair does the work of a pass is the question that turns "the scene is
    // flat" into "THIS pixel shader is flat", and from there Xenia's disassembly of
    // that exact shader says what it was supposed to compute. Off by default because
    // it makes one counter per pair.
    if (EnvOn("CZ_VK_SHADER_CENSUS"))
    {
        char name[64];
        snprintf(name, sizeof name, "pair vs=%016llx ps=%016llx",
                 (unsigned long long)vsBind.hash, (unsigned long long)psBind.hash);
        Count(name);
    }

    // Two classes of draw that execute and produce nothing, counted because both are
    // invisible in a log and indistinguishable in a picture from a draw that never
    // happened: one whose colour write mask is empty, and one whose depth test can
    // never pass. If a whole pass is black, this says whether the geometry was
    // rejected by state we decoded or was never there.
    if (key.colorMask == 0)
        Count("draw: colour write mask is empty");
    // ...and the pair that says whether an empty mask is a DEPTH PREPASS or a register
    // read at the wrong index. RB_MODECONTROL's edram mode is the guest's own statement
    // of what a pass writes (4 = colour+depth, 5 = depth-only), so "mode 5, mask 0" is
    // a prepass and needs no explanation, while "mode 4, mask 0" is a draw that set up
    // a colour target and then asked for none of it — a shape worth counting rather
    // than assuming.
    {
        static char pairNames[8][48];
        static bool built = false;
        if (!built)
        {
            built = true;
            for (uint32_t m = 0; m < 8; m++)
                snprintf(pairNames[m], sizeof pairNames[m],
                         "draw: modeControl %u with an EMPTY colour mask", m);
        }
        if (key.colorMask == 0)
            Count(pairNames[key.modeControl & 7]);
    }
    if (((key.depthControl >> 1) & 1) && ((key.depthControl >> 4) & 7) == 0)
        Count("draw: depth compare is NEVER");

    VkPipeline pipeline = GetPipeline(key, vs, ps);
    if (pipeline == VK_NULL_HANDLE)
        return; // GetPipeline has already counted and named the reason

    BeginFrame();
    BeginRendering();

    // --- constants -----------------------------------------------------------------
    // The guest's ALU constant file is 512 float4 registers: 0..255 are the vertex
    // shader's, 256..479 the pixel shader's. They are big-endian in our register file
    // (the packets wrote them through the same accessors as everything else) and the
    // shaders want little-endian, so every dword is swapped on the way out.
    const VkDeviceSize vsConstAt = ArenaAlloc(kVsConstBytes);
    const VkDeviceSize psConstAt = ArenaAlloc(kPsConstBytes);
    const VkDeviceSize sharedAt = ArenaAlloc(kSharedSize);
    if (vsConstAt == VkDeviceSize(-1) || psConstAt == VkDeviceSize(-1) ||
        sharedAt == VkDeviceSize(-1))
        return;

    {
        uint32_t* dst = reinterpret_cast<uint32_t*>(R->arena.mapped + vsConstAt);
        for (uint32_t i = 0; i < 256 * 4; i++)
            dst[i] = regs[xenos::kAluConstantBase + i];
        dst = reinterpret_cast<uint32_t*>(R->arena.mapped + psConstAt);
        for (uint32_t i = 0; i < 256 * 4; i++)
            dst[i] = regs[xenos::kAluConstantBase + 256 * 4 + i];
    }

    uint8_t* shared = R->arena.mapped + sharedAt;
    memset(shared, 0, kSharedSize);

    // Texture and sampler descriptor indices, one per sampler slot the pixel shader
    // declared. A slot the shader does not use is left at 0, which is the dummy — a
    // defined white texel rather than an unbound descriptor, because a shader that
    // samples an unbound descriptor is undefined behaviour even when the result is
    // discarded.
    // CZ_VK_PSBIND=<pshash> — what each of this pixel shader's samplers is actually
    // BOUND to, printed once per distinct binding.
    //
    // Ported from Fable 2's [psbind], whose comment states the reason better than a
    // new one could: a post pass is `colour = f(constants, textures)`, so once the
    // constants are known good the answer has to be in the textures — and `slot=0` is
    // the 1x1 dummy, which stands in silently for whatever the pass meant to read.
    static const char* psbindEnv = Env("CZ_VK_PSBIND");
    char psbindWant[24];
    snprintf(psbindWant, sizeof psbindWant, "%016llx", (unsigned long long)psBind.hash);
    const bool psbind = psbindEnv && strstr(psbindEnv, psbindWant);
    char psbindLine[512];
    // The pass's WRITE state belongs on this line too. "colour = f(constants,
    // textures)" is only true of a draw that writes its colour at all: an empty
    // RB_COLOR_MASK makes a pipeline that discards every channel, and its output is
    // indistinguishable from a shader that computed black. 43% of this title's draws
    // arrive with an empty mask, so the question is live for every one of them.
    int psbindAt = psbind ? snprintf(psbindLine, sizeof psbindLine,
                                     "[psbind] frame=%llu ps=%016llx mask=%X blend=%08X",
                                     (unsigned long long)R->frame,
                                     (unsigned long long)psBind.hash,
                                     regs[xenos::kRbColorMask] & 0xF,
                                     regs[xenos::kRbBlendControl0])
                          : 0;

    R->lastTexAddr = 0;
    R->lastTexSlot = 0;
    auto bindTextures = [&](const std::vector<uint32_t>& consts) {
        for (uint32_t constIdx : consts)
        {
            if (constIdx >= 16)
                continue;
            const size_t snapsBefore = R->snapshotsSampledThisPass.size();
            const uint32_t slot = UploadTexture(base, regs, constIdx);
            if (!R->lastTexAddr)
            {
                R->lastTexAddr = xenos::DecodeTextureFetch(regs, constIdx).address;
                R->lastTexSlot = slot;
            }
            reinterpret_cast<uint32_t*>(shared + kSharedTex2D)[constIdx] = slot;
            reinterpret_cast<uint32_t*>(shared + kSharedSampler)[constIdx] = 0;
            if (psbind && psbindAt < int(sizeof psbindLine) - 96)
            {
                const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
                psbindAt += snprintf(
                    psbindLine + psbindAt, sizeof psbindLine - psbindAt,
                    "  s%u=%08X %ux%u fmt=%u swz=%03X slot=%u%s%s", constIdx, t.address,
                    t.width, t.height, t.format, t.swizzle, slot,
                    slot == 0 ? "(DUMMY)" : "",
                    R->snapshotsSampledThisPass.size() > snapsBefore ? "(snap)" : "");
            }
        }
    };
    bindTextures(ps.tfetchConsts);
    bindTextures(vs.tfetchConsts);

    // CZ_VK_ONLY_TEX / CZ_VK_SKIP_TEX=<hex[,hex...]> — render only, or all but, the
    // draws whose first bound texture is at that guest address.
    //
    // The bisection arms one level down from CZ_VK_ONLY_VS. A UI compose is a hundred
    // quads sharing two shaders, so "which shader draws this" cannot separate them and
    // "which TEXTURE does this draw sample" can. It is how a rectangle on screen gets
    // an identity: skip one address, look at what vanished. That turns "the save-slot
    // boxes are black" into "the save-slot boxes are texture 0364B000", which is a
    // question with an answer.
    {
        static const char* onlyTex = Env("CZ_VK_ONLY_TEX");
        static const char* skipTex = Env("CZ_VK_SKIP_TEX");
        if (onlyTex || skipTex)
        {
            char hex[16];
            snprintf(hex, sizeof hex, "%08X", R->lastTexAddr);
            if (onlyTex && !strstr(onlyTex, hex))
            {
                Count("draw: filtered out (CZ_VK_ONLY_TEX)");
                return;
            }
            if (skipTex && strstr(skipTex, hex))
            {
                Count("draw: filtered out (CZ_VK_SKIP_TEX)");
                return;
            }
        }
    }
    if (psbind)
    {
        // Dedupe on the BINDINGS, never on the whole line — the line carries the frame
        // number, so including it makes every frame distinct and the cap then shows
        // only the boot. That is the same first-occurrence trap the draw probe hit, and
        // it hit this instrument within a minute of it being written.
        static std::vector<std::string> seenBind;
        // The constants this pass's shader reads, alongside its bindings. A post pass
        // is colour = f(constants, textures) and both halves have to be in ONE line, or
        // they get measured on different draws — the VS-keyed probe reported c255 for
        // whichever pass happened to come first and it was not this one.
        if (psbindAt < int(sizeof psbindLine) - 128)
        {
            const char* list = Env("CZ_VK_PSBIND_PC");
            std::string spec = list ? list : "255";
            size_t at = 0;
            while (at < spec.size() && psbindAt < int(sizeof psbindLine) - 64)
            {
                const size_t comma = spec.find(',', at);
                const uint32_t r = uint32_t(strtoul(spec.c_str() + at, nullptr, 10));
                if (r < 256)
                {
                    const uint32_t* pc =
                        regs + xenos::kAluConstantBase + 256 * 4 + r * 4;
                    psbindAt += snprintf(psbindLine + psbindAt,
                                         sizeof psbindLine - psbindAt,
                                         "  pc%u=(%.4f,%.4f,%.4f,%.4f)", r, F32(pc[0]),
                                         F32(pc[1]), F32(pc[2]), F32(pc[3]));
                }
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
        }
        const char* bindings = strstr(psbindLine, "mask=");
        std::string key(bindings ? bindings : psbindLine);
        if (std::find(seenBind.begin(), seenBind.end(), key) == seenBind.end() &&
            seenBind.size() < 64)
        {
            seenBind.push_back(key);
            fprintf(stderr, "%s\n", psbindLine);
        }
    }

    // g_SwappedTexcoords — one bit per TEXCOORD semantic, and it is a correction for
    // something THIS RUNTIME does rather than something the guest does.
    //
    // Vertex data is copied out of guest memory by dword-swapping the whole stream
    // (CopySwapped with the fetch constant's endian code, which is 8-in-32 here). For
    // 32-bit components that is exactly right. For SIXTEEN-bit components it also
    // transposes the two halves of every dword, so a 16_16 attribute arrives as YX and
    // a 16_16_16_16 as YXWZ. XenosRecomp's generated `tfetchTexcoord` un-transposes it
    // when the matching bit is set here, and we were leaving the mask at zero — so
    // every 16-bit vertex attribute in the title had its components swapped, silently.
    //
    // The semantic index comes from the Vulkan location, because that is what the
    // container synthesizer keyed both sides on: TEXCOORD0..3 are locations 4..7 and
    // TEXCOORD4..23 are locations 12..31 (its USAGE_LOCATION table).
    {
        uint32_t swapped = 0;
        for (const VertexAttribute& a : vs.attributes)
        {
            if (a.location < 4 || a.indirect)
                continue;
            const bool sixteenBit = a.format == 25 || a.format == 26 || a.format == 31 ||
                                    a.format == 32;
            if (!sixteenBit)
                continue;
            const uint32_t texcoord =
                a.location < 12 ? uint32_t(a.location - 4) : uint32_t(a.location - 8);
            if (texcoord < 32)
                swapped |= 1u << texcoord;
        }
        // CZ_VK_NO_TEXCOORD_SWAP=1 restores the old always-zero mask, so the change is
        // measurable in the same binary rather than asserted.
        static const bool disable = EnvOn("CZ_VK_NO_TEXCOORD_SWAP");
        reinterpret_cast<uint32_t*>(shared + kSharedSwappedTexcoords)[0] =
            disable ? 0u : swapped;
        if (swapped)
            Count("draw: 16-bit texcoord unswizzle published");
    }

    // The bool and loop constant files, verbatim. The shaders index them themselves.
    for (uint32_t i = 0; i < 8; i++)
        reinterpret_cast<uint32_t*>(shared + kSharedBoolFile)[i] =
            regs[xenos::kBoolConstantBase + i];
    for (uint32_t i = 0; i < 32; i++)
        reinterpret_cast<uint32_t*>(shared + kSharedLoopConstants)[i] =
            regs[xenos::kLoopConstantBase + i];
    reinterpret_cast<uint32_t*>(shared + kSharedBooleans)[0] =
        regs[xenos::kBoolConstantBase];

    // --- the viewport, and the one transform that is ours to apply -----------------
    // PA_CL_VTE_CNTL says which of the six viewport terms the hardware applies. A term
    // whose enable bit is CLEAR is the identity — not the register's value, which is
    // stale from whenever it was last written. Reading it anyway is a silent geometry
    // bug, and it is the reason this is decoded rather than assumed.
    //
    // When the guest disables the XY transform it is emitting window coordinates and
    // expects the hardware to fold them; g_PosScale/g_PosOffset is where the shader
    // does that, so the fold is published rather than baked into a viewport.
    const uint32_t vte = regs[xenos::kPaClVteCntl];
    const float xs = (vte & 0x1) ? F32(regs[xenos::kPaClVportXScale]) : 1.0f;
    const float ys = (vte & 0x4) ? F32(regs[xenos::kPaClVportYScale]) : 1.0f;
    const float xo = (vte & 0x2) ? F32(regs[xenos::kPaClVportXOffset]) : 0.0f;
    const float yo = (vte & 0x8) ? F32(regs[xenos::kPaClVportYOffset]) : 0.0f;

    float posScale[2] = { 1.0f, 1.0f };
    float posOffset[2] = { 0.0f, 0.0f };
    if (!(vte & 0x1))
    {
        // Window coordinates: map [0, w] x [0, h] to clip [-1, 1] x [-1, 1]. Vulkan's
        // clip Y already points down like the window's, so there is no flip here — and
        // adding one is the double-flip that cost the previous port a session.
        posScale[0] = 2.0f / float(R->targetWidth);
        posScale[1] = 2.0f / float(R->targetHeight);
        posOffset[0] = -1.0f;
        posOffset[1] = -1.0f;
        // MSAA: window coordinates are in PIXELS and our EDRAM image is at sample
        // resolution, so a 4x pass's pixel is two of our columns wide. RB_SURFACE_INFO
        // bits 16..17: 0 = 1x, 1 = 2x, 2 = 4x, and on Xenos it is 4x that doubles the
        // surface's WIDTH.
        //
        // This is not a subtlety. The title clears its scene tile with a
        // rectangle-list draw of (0,0)-(320,720) while the tile is 640 wide, because
        // that clear runs with the surface declared 4x; mapped one-to-one it clears
        // half the tile, the previous pass's DEPTH survives in the other half, and the
        // whole right side of the 3D background is rejected by a depth test on stale
        // values. The geometry is submitted for all 640 columns either way — proved by
        // CZ_VK_NO_DEPTH_TEST=1, which fills every one of them.
        //
        // CZ_VK_NO_MSAA_WINDOW_SCALE=1 is the same-binary control arm.
        static const bool noMsaaScale = EnvOn("CZ_VK_NO_MSAA_WINDOW_SCALE");
        const uint32_t msaa = (regs[xenos::kRbSurfaceInfo] >> 16) & 3;
        if (!noMsaaScale && msaa == 2)
        {
            posScale[0] *= 2.0f;
            Count("draw: window coordinates scaled for a 4x MSAA surface");
        }
        // ...and the TILE ORIGIN, for the same reason the viewport path does NOT need
        // it. A window coordinate is relative to the EDRAM surface, and hardware moves
        // a tile's geometry into that surface with PA_SC_WINDOW_OFFSET (-640 for this
        // title's right-hand tile). Our EDRAM is full size and holds every tile at its
        // true screen position, so the offset has to be UNDONE here: window x 0 of the
        // right tile is screen x 640. The viewport path needs nothing because the
        // viewport already places geometry in screen space.
        //
        // HONEST ABOUT ITS OWN EFFECT: over a whole boot this counter reads ZERO.
        // Every window-coordinate draw this title issues runs with the window offset
        // at 0 — the offset is only ever set for the tiled scene, whose draws all take
        // the viewport path. So the code below is correct and has never yet executed;
        // it must not be credited with anything, and the counter is what says so
        // (gotcha 151). It stays because the alternative is rediscovering the rule the
        // next time a title puts a window-coordinate draw inside a tile.
        const uint32_t wo = regs[xenos::kPaScWindowOffset];
        auto signed15 = [](uint32_t v) {
            return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) ? 0x8000 : 0);
        };
        const int32_t tileX = -signed15(wo);
        const int32_t tileY = -signed15(wo >> 16);
        if (!noMsaaScale && (tileX || tileY))
        {
            posOffset[0] += 2.0f * float(tileX) / float(R->targetWidth);
            posOffset[1] += 2.0f * float(tileY) / float(R->targetHeight);
            Count("draw: window coordinates moved to the tile's screen origin");
        }
    }
    memcpy(shared + kSharedPosScale, posScale, sizeof posScale);
    memcpy(shared + kSharedPosOffset, posOffset, sizeof posOffset);

    // Half-pixel offset: the Xbox 360 samples pixel centres at integers, desktop APIs
    // at half-integers. The shaders apply this themselves; the runtime just states it.
    //
    // ...AND IT IS ZERO, since phase C part 11. The shader adds it to every vertex's
    // clip position (`oPos.xy += g_HalfPixelOffset * oPos.w`), so it is a subpixel
    // shift of ALL geometry, and a shift is not what the convention difference needs.
    // On Xenos a screen-space rect [0, W] samples pixel centres at the integers
    // 0..W-1 and covers W pixels; on Vulkan the same rect samples centres at
    // 0.5..W-0.5 and covers W pixels. The coverage already agrees. Shifting by half a
    // pixel on top of that moves the rect to [-0.5, W-0.5), whose last pixel centre
    // lands EXACTLY on the exclusive right edge — and the top-left fill rule drops it.
    //
    // Measured, and it is not a subtlety: the scene's left tile clears screen 0..640,
    // so with the shift its column 639 was never covered by ANYTHING. The resolved
    // scene surface had exactly one all-black column, at x=639, and the frame's blur
    // smeared that single column into a ~19 px darkening centred on the tile boundary
    // — a visible full-height line down the middle of the picture. Zeroed: zero
    // all-black columns, and the sky reads a flat 128.3 straight across the join.
    //
    // CZ_VK_HALF_PIXEL=1 restores the old value as the same-binary control arm.
    static const bool wantHalfPixel = EnvOn("CZ_VK_HALF_PIXEL");
    const float halfPixel[2] = { wantHalfPixel ? -1.0f / float(R->targetWidth) : 0.0f,
                                 wantHalfPixel ? -1.0f / float(R->targetHeight) : 0.0f };
    memcpy(shared + kSharedHalfPixelOffset, halfPixel, sizeof halfPixel);

    // The viewport itself. With the XY transform enabled the scale/offset ARE the
    // viewport; the y scale is negative in D3D convention, and taking its absolute
    // value here while leaving the sign to the clip-space fold is what keeps the two
    // conventions from cancelling each other out by accident.
    // THE Y FLIP, and it is a real one for this path.
    //
    // A Xenos vertex shader emits clip coordinates in D3D convention, where +y is UP in
    // NDC. Vulkan's NDC has +y DOWN. Passing the guest's clip position straight into a
    // positive-height viewport therefore renders the frame VERTICALLY MIRRORED.
    //
    // This was missed for the whole phase because it is invisible to every instrument
    // here: a vertical flip preserves coverage, mean luminance, distinct-colour count
    // and even the histogram exactly, so `tools/frame_compare.py` scores a flipped
    // frame as IDENTICAL. It took a human looking at the Blue Castle Games logo and
    // saying "that is upside down". A measurement that cannot see a transform is not a
    // weaker measurement, it is a blind one — and the fix is another instrument, not
    // more of this one.
    //
    // Expressed as a NEGATIVE-HEIGHT viewport (core since Vulkan 1.1) rather than a
    // matrix fold, so it applies to the viewport-transform path only and cannot
    // double up with the window-coordinate path's g_PosScale/g_PosOffset — which does
    // NOT need a flip, because there the runtime builds the mapping itself and maps
    // window y=0 to clip -1 directly. Getting those two confused is the double flip
    // that cost the previous port a session.
    static const bool noFlipY = EnvOn("CZ_VK_NO_FLIP_Y");
    VkViewport viewport{};
    if (vte & 0x1)
    {
        viewport.x = xo - std::fabs(xs);
        viewport.width = 2.0f * std::fabs(xs);
        if (noFlipY)
        {
            viewport.y = yo - std::fabs(ys);
            viewport.height = 2.0f * std::fabs(ys);
        }
        else
        {
            viewport.y = yo + std::fabs(ys);
            viewport.height = -2.0f * std::fabs(ys);
        }
    }
    else
    {
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = float(R->targetWidth);
        viewport.height = float(R->targetHeight);
    }
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    // Height may legitimately be NEGATIVE now (the Y flip above), so the check is on
    // magnitude. Testing `height <= 0` here would silently drop every single draw.
    if (viewport.width <= 0.0f || std::fabs(viewport.height) <= 0.0f)
    {
        Count("draw: degenerate viewport");
        return;
    }

    // THE SCISSOR IS THE TILE. Case Zero does not render its 1280x720 scene in one
    // pass: the EDRAM is 10 MB and a 1280x720 colour+depth target does not fit, so the
    // title splits the screen into two 640-wide tiles and renders each into a 640-pitch
    // EDRAM surface — which is what makes SET_BIN_MASK_LO the single most frequent
    // opcode in the whole stream (2,353,460 of B1's 8,283,322 type-3 packets).
    //
    // PA_SC_WINDOW_SCISSOR carries the tile in SCREEN coordinates (0..640 then
    // 640..1280), and PA_SC_WINDOW_OFFSET carries the -640 that hardware adds to move
    // the second tile's geometry down into the 640-wide surface. Our EDRAM image is
    // full size, so we deliberately do NOT apply the offset — the geometry is already
    // where we want it — but we must honour the scissor, or every tile paints the whole
    // screen and the last one wins.
    const uint32_t winX = regs[xenos::kPaScWindowScissorTl] & 0x7FFF;
    const uint32_t winY = (regs[xenos::kPaScWindowScissorTl] >> 16) & 0x7FFF;
    const uint32_t winX1 = regs[xenos::kPaScWindowScissorBr] & 0x7FFF;
    const uint32_t winY1 = (regs[xenos::kPaScWindowScissorBr] >> 16) & 0x7FFF;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = { R->color.width, R->color.height };
    if (winX1 > winX && winY1 > winY && winX < R->color.width && winY < R->color.height)
    {
        scissor.offset = { int32_t(winX), int32_t(winY) };
        scissor.extent = { std::min(winX1, R->color.width) - winX,
                           std::min(winY1, R->color.height) - winY };
    }

    // CZ_VK_VIEWPORT_TRACE=1 — every DISTINCT viewport setup, once each. A per-draw
    // trace of 1.1 M draws is unreadable and a per-frame one hides the outlier that
    // matters; the set of distinct states is small (a title screen uses a handful) and
    // it is what says whether the geometry is being placed by a viewport we computed
    // or by a transform the shader applied.
    if (EnvOn("CZ_VK_VIEWPORT_TRACE"))
    {
        // The SCISSOR is part of the setup and belongs in the key. This title tiles its
        // scene, so two draws with the same viewport and different scissors are two
        // different tiles — and reading a trace that cannot tell them apart is how
        // "which half of the screen is this pass painting?" stays unanswerable.
        static std::vector<std::string> seen;
        char line[512];
        snprintf(line, sizeof line,
                 "[vkvp] vte=%02X xs=%.1f xo=%.1f ys=%.1f yo=%.1f -> viewport "
                 "%.1f,%.1f %.1fx%.1f  scissor %d,%d %ux%u  winoff=%08X "
                 "posScale=%.5f,%.5f posOffset=%.2f,%.2f surfacePitch=%u msaa=%u "
                 "surfaceInfo=%08X",
                 vte & 0x3F, xs, xo, ys, yo, viewport.x, viewport.y, viewport.width,
                 viewport.height, scissor.offset.x, scissor.offset.y,
                 scissor.extent.width, scissor.extent.height,
                 regs[xenos::kPaScWindowOffset], posScale[0], posScale[1], posOffset[0],
                 posOffset[1], regs[xenos::kRbSurfaceInfo] & 0x3FFF,
                 (regs[xenos::kRbSurfaceInfo] >> 16) & 3, regs[xenos::kRbSurfaceInfo]);
        if (std::find(seen.begin(), seen.end(), line) == seen.end() && seen.size() < 64)
        {
            seen.push_back(line);
            fprintf(stderr, "%s\n", line);
        }
    }

    vkCmdBindPipeline(R->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(R->cmd, 0, 1, &viewport);
    vkCmdSetScissor(R->cmd, 0, 1, &scissor);
    const float blendConstants[4] = {
        F32(regs[xenos::kRbBlendRed]), F32(regs[xenos::kRbBlendRed + 1]),
        F32(regs[xenos::kRbBlendRed + 2]), F32(regs[xenos::kRbBlendRed + 3])
    };
    vkCmdSetBlendConstants(R->cmd, blendConstants);
    vkCmdBindDescriptorSets(R->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipeLayout, 0, 5,
                            R->sets, 0, nullptr);

    const uint64_t pushConstants[3] = { uint64_t(R->arena.address + vsConstAt),
                                        uint64_t(R->arena.address + psConstAt),
                                        uint64_t(R->arena.address + sharedAt) };
    vkCmdPushConstants(R->cmd, R->pipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24,
                       pushConstants);

    // CZ_VK_STATE_PROBE=1 — the distinct values of the state registers this renderer
    // ASSUMES rather than reads. Each of these is a place where a wrong assumption
    // produces a plausible wrong picture instead of an error, so the cheap version of
    // checking them is to print what the guest actually writes.
    if (EnvOn("CZ_VK_STATE_PROBE"))
    {
        static std::vector<uint64_t> seen;
        const uint64_t k = (uint64_t(regs[0x2307]) << 32) ^ regs[0x2308];
        if (std::find(seen.begin(), seen.end(), k) == seen.end() && seen.size() < 32)
        {
            seen.push_back(k);
            fprintf(stderr,
                    "[vkstate] SQ_VS_CONST=%08X (base=%u size=%u) "
                    "SQ_PS_CONST=%08X (base=%u size=%u)  RB_COLOR_INFO=%08X (fmt=%u) "
                    "RB_MODECONTROL=%08X RB_COLORCONTROL=%08X PA_SU_SC=%08X "
                    "VGT_INDX_OFFSET=%d VGT_MIN=%u VGT_MAX=%u\n",
                    regs[0x2307], regs[0x2307] & 0x1FF, (regs[0x2307] >> 12) & 0x1FF,
                    regs[0x2308], regs[0x2308] & 0x1FF, (regs[0x2308] >> 12) & 0x1FF,
                    regs[xenos::kRbColorInfo], (regs[xenos::kRbColorInfo] >> 16) & 0xF,
                    regs[0x2208], regs[xenos::kRbColorControl], regs[0x2280],
                    int32_t(regs[xenos::kVgtIndxOffset]), regs[xenos::kVgtMinVtxIndx],
                    regs[xenos::kVgtMaxVtxIndx]);
        }
    }

    // CZ_VK_FETCH_PROBE=1 — which vertex fetch slots does the guest actually populate?
    //
    // The question this exists to answer: a vfetch instruction's constant index is
    // `const_index * 3 + const_index_sel`, and Xenia's DISASSEMBLY prints the same
    // shaders' fetches as vf0/vf1/vf2 where that formula gives 95/94/93. One of the
    // two is a display convention and the other is the hardware's index, and no amount
    // of reading either tool settles it. The register file does: exactly one of the two
    // readings names slots the guest has written a plausible address and size into.
    if (EnvOn("CZ_VK_FETCH_PROBE") && !vs.attributes.empty())
    {
        static int left = 4;
        if (left-- > 0)
        {
            fprintf(stderr, "[vkfetch] draw vs=%016llx populated vertex fetch slots:\n",
                    (unsigned long long)vsBind.hash);
            for (uint32_t slot = 0; slot < 96; slot++)
            {
                const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, slot);
                if (!vf.sizeDwords || !vf.address)
                    continue;
                const uint32_t sva = PhysToVa(vf.address);
                fprintf(stderr, "[vkfetch]   slot %2u: addr=%08X size=%u dwords%s\n",
                        slot, vf.address, vf.sizeDwords,
                        GuestRangeOk(sva, uint64_t(vf.sizeDwords) * 4) ? "" : "  (OUT OF ARENA)");
            }
            fprintf(stderr, "[vkfetch]   shader wants slots:");
            for (const VertexAttribute& a : vs.attributes)
                fprintf(stderr, " %u", a.fetchSlot);
            fprintf(stderr, "\n");
        }
    }

    // --- dependent vertex fetches ----------------------------------------------------
    // A Xenos vfetch addresses its stream with a REGISTER. Only while that register
    // still holds the auto-loaded vertex index is the fetch expressible as a Vulkan
    // vertex attribute; a shader that computes an address — a bone palette, a
    // per-instance record index, particle state — is fetching from somewhere no vertex
    // input can describe. XenosRecomp emits those as in-shader raw loads (XeVfetchDep)
    // and reads the stream's address and size out of a table the RUNTIME publishes at
    // SharedConstants + 544, sixteen bytes per fetch slot.
    //
    // Not publishing it is not a partial result. The shader's own bounds check sees
    // size 0, returns float4(0,0,0,0), and every vertex of that mesh collapses to the
    // origin — so the draw executes, the pipeline is fine, no counter fires, and the
    // mesh is simply absent. 22 of this title's 67 vertex shaders take that path.
    for (const VertexAttribute& a : vs.attributes)
    {
        if (!a.indirect || a.fetchSlot >= 96)
            continue;
        const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, FetchSlot(a.fetchSlot));
        const uint32_t sva = PhysToVa(vf.address);
        const uint64_t bytes = uint64_t(vf.sizeDwords) * 4;
        if (!GuestRangeOk(sva, bytes))
        {
            Count("draw: dependent fetch stream outside the physical arena");
            continue;
        }
        const VkDeviceSize at = UploadStream(base, sva, bytes, vf.endian);
        if (at == VkDeviceSize(-1))
            continue;
        // {deviceAddress.lo, deviceAddress.hi, sizeDwords, 0} — the layout the
        // generated XeVfetchDep reads, transcribed from shader_common.h.
        const uint64_t addr = uint64_t(R->arena.address) + at;
        uint32_t* entry =
            reinterpret_cast<uint32_t*>(shared + kSharedVfetchTable + a.fetchSlot * 16);
        entry[0] = uint32_t(addr);
        entry[1] = uint32_t(addr >> 32);
        entry[2] = vf.sizeDwords;
        entry[3] = 0;
        Count("draw: dependent fetch stream published");
    }

    // --- vertex streams -------------------------------------------------------------
    //
    // RECTANGLE LISTS GET A SYNTHESISED FOURTH CORNER. A Xenos rect list stores three
    // vertices — this title's are TL, TR, BR, measured straight off the stream:
    // (0,0) (64,0) (64,64), (0,0) (480,0) (480,512), (960,0) (1024,0) (1024,1024) —
    // and the hardware generates BL = v0 + v2 - v1 to complete the quad. An index
    // rewrite alone cannot express that, because BL is a vertex that does not exist,
    // so the old expansion emitted (v0,v1,v2) and (v0,v2,v1): the SAME triangle twice,
    // covering the TL-TR-BR half and leaving the other half of every rect untouched.
    //
    // That is not a cosmetic loss here. These draws are the guest's per-pass CLEAR —
    // one at the head of nearly every pass, 28,743 a boot — and half of them cleared
    // DEPTH ONLY (modeControl 5, empty colour mask). With half the rect uncleared, the
    // previous pass's depth survives there and rejects the whole scene: the title
    // screen's 3D background appeared inside one triangle with a diagonal edge and
    // nothing outside it, which reads as broken geometry rather than a missing clear.
    //
    // So the fourth corner is built for real: four records are copied into the arena
    // and record 3 is `r0 + r2 - r1` per dword, read as float. That linear combination
    // is exactly what the hardware extrapolates, and it is correct for every 32-bit
    // float attribute — which is all of them in this title. A stream carrying packed
    // attributes would need per-format arithmetic; the fallback copies r0's bytes and
    // COUNTS itself rather than being silently wrong.
    static const bool rectHalf = EnvOn("CZ_VK_RECT_HALF");
    const bool rectSynth =
        expand == Expansion::RectangleList && draw.indexCount == 3 && !rectHalf;
    uint32_t rectCorner[3] = { 0, 1, 2 };
    if (rectSynth)
    {
        const uint8_t* isrc = draw.indexed ? base + draw.indexVa : nullptr;
        for (uint32_t k = 0; k < 3; k++)
            rectCorner[k] =
                ReadIndex(isrc, k, draw.index32, draw.indexEndian, draw.indexed);
    }

    uint32_t binding = 0;
    bool streamsOk = true;
    for (const VertexAttribute& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue;
        const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, FetchSlot(a.fetchSlot));
        const uint32_t va = PhysToVa(vf.address);
        const uint64_t bytes = uint64_t(vf.sizeDwords) * 4;
        if (!GuestRangeOk(va, bytes) || !bytes)
        {
            Count("draw: vertex stream outside the physical arena");
            streamsOk = false;
            break;
        }
        const VkDeviceSize at = UploadStream(base, va, bytes, vf.endian);
        if (at == VkDeviceSize(-1))
        {
            streamsOk = false;
            break;
        }
        if (rectSynth && a.strideDwords)
        {
            const VkDeviceSize four =
                SynthRectStream(at, bytes, a.strideDwords, rectCorner, a.format);
            if (four == VkDeviceSize(-1))
            {
                streamsOk = false;
                break;
            }
            const VkDeviceSize offset = four + uint64_t(a.offsetDwords) * 4;
            vkCmdBindVertexBuffers(R->cmd, binding, 1, &R->arena.buffer, &offset);
            ++binding;
            continue;
        }
        const VkDeviceSize offset = at + uint64_t(a.offsetDwords) * 4;
        if (offset >= R->arena.size)
        {
            Count("draw: vertex element offset past the stream");
            streamsOk = false;
            break;
        }
        vkCmdBindVertexBuffers(R->cmd, binding, 1, &R->arena.buffer, &offset);
        ++binding;
    }
    if (!streamsOk)
        return;

    // CZ_VK_DRAW_PROBE=<vsHash> — for the first few draws with that vertex shader,
    // print the constants and the vertex data it will actually read.
    //
    // This exists because the remaining geometry defect is bounded to two inputs and
    // both have been verified in the abstract: `oPos = vc(0..3) * (vc(8..10) *
    // iPosition0)`, the position format needs no conversion, and the fetch slot is
    // settled. When every input checks out and the output is wrong, the next move is
    // not another hypothesis — it is to look at the values.
    if (const char* probe = Env("CZ_VK_DRAW_PROBE"))
    {
        // CZ_VK_DRAW_PROBE_MINVERTS bounds the probe to the big meshes. The first three
        // draws of a shader are usually its smallest, and a defect that only shows on
        // large geometry is invisible in them — which is how the first pass of this
        // probe reported "everything is healthy" about a shader that visibly explodes.
        static const uint32_t minVerts =
            Env("CZ_VK_DRAW_PROBE_MINVERTS")
                ? uint32_t(atoi(Env("CZ_VK_DRAW_PROBE_MINVERTS")))
                : 0;
        // CZ_VK_DRAW_PROBE_MINFRAME bounds the probe to a steady-state frame, and it is
        // not optional for a state question. A shader's first three draws happen during
        // the BOOT, where the guest has not yet uploaded the constants that shader will
        // use — so probing them reports "every pixel-shader constant is zero" about a
        // shader whose constants are perfectly good by the title screen. That reading
        // cost an hour and was contradicted by watching the register itself, which
        // takes no zero writes at all after frame 400.
        static const uint64_t minFrame =
            Env("CZ_VK_DRAW_PROBE_MINFRAME")
                ? uint64_t(atoll(Env("CZ_VK_DRAW_PROBE_MINFRAME")))
                : 0;
        // CZ_VK_DRAW_PROBE_COUNT=N — how many draws to print (default 3). Three is
        // enough for "what does this shader read"; it is not enough when a shader is
        // issued many times per pass with different data, which is exactly what a
        // rectangle-list CLEAR does — this title clears a surface in 64-wide vertical
        // STRIPS, so the first three entries describe three strips of one pass and say
        // nothing about the pass you are actually looking at.
        static int left = Env("CZ_VK_DRAW_PROBE_COUNT")
                              ? atoi(Env("CZ_VK_DRAW_PROBE_COUNT"))
                              : 3;
        if (vsBind.hash == strtoull(probe, nullptr, 16) && draw.indexCount >= minVerts &&
            R->frame >= minFrame && left-- > 0)
        {
            const uint32_t* c = regs + xenos::kAluConstantBase;
            fprintf(stderr, "[vkprobe] vs=%016llx prim=%u indexCount=%u indexed=%d\n",
                    (unsigned long long)vsBind.hash, draw.primType, draw.indexCount,
                    draw.indexed ? 1 : 0);
            fprintf(stderr,
                    "[vkprobe]   vte=%02X xs=%.1f xo=%.1f ys=%.1f yo=%.1f -> viewport "
                    "%.1f,%.1f %.1fx%.1f  scissor %d,%d %ux%u  posScale=%.5f,%.5f "
                    "posOffset=%.2f,%.2f\n",
                    vte & 0x3F, xs, xo, ys, yo, viewport.x, viewport.y, viewport.width,
                    viewport.height, scissor.offset.x, scissor.offset.y,
                    scissor.extent.width, scissor.extent.height, posScale[0],
                    posScale[1], posOffset[0], posOffset[1]);
            fprintf(stderr,
                    "[vkprobe]   depthCtl=%02X (test=%u write=%u func=%u) blend=%08X "
                    "colorMask=%X modeCtl=%u  RB_COLORCONTROL=%08X alphaRef=%.3f\n",
                    key.depthControl, (key.depthControl >> 1) & 1,
                    (key.depthControl >> 2) & 1, (key.depthControl >> 4) & 7,
                    key.blendControl, key.colorMask, key.modeControl,
                    regs[xenos::kRbColorControl], F32(regs[xenos::kRbAlphaRef]));
            fprintf(stderr,
                    "[vkprobe]   SQ_VS_CONST=%08X (base=%u size=%u)  "
                    "SQ_PS_CONST=%08X (base=%u size=%u)\n",
                    regs[0x2307], regs[0x2307] & 0x1FF, (regs[0x2307] >> 12) & 0x1FF,
                    regs[0x2308], regs[0x2308] & 0x1FF, (regs[0x2308] >> 12) & 0x1FF);
            // The same window the guest just named, in case it is not ours.
            {
                const uint32_t base = regs[0x2308] & 0x1FF;
                for (uint32_t r : { 2u, 5u })
                {
                    const uint32_t* g =
                        regs + xenos::kAluConstantBase + base * 4 + r * 4;
                    fprintf(stderr,
                            "[vkprobe]   guest-base pc(%2u) = %10.4f %10.4f %10.4f "
                            "%10.4f\n",
                            r, F32(g[0]), F32(g[1]), F32(g[2]), F32(g[3]));
                }
            }
            // The PIXEL shader's constants, from its own window (ALU float4 256+n).
            // A post-processing blit is a weighted sum of taps, so zero weights are a
            // black output with every other piece of state looking perfect.
            // CZ_VK_DRAW_PROBE_PC=a,b,c — which pixel-shader constants to print.
            // Which ones matter is a property of the shader under investigation (read
            // its disassembly: a post-process blit names its taps and its scale), so
            // this cannot have a useful fixed default.
            {
                const char* list = Env("CZ_VK_DRAW_PROBE_PC");
                std::string spec = list ? list : "0,1,2,3,255";
                size_t at = 0;
                while (at < spec.size())
                {
                    const size_t comma = spec.find(',', at);
                    const uint32_t r = uint32_t(strtoul(spec.c_str() + at, nullptr, 10));
                    if (r < 256)
                    {
                        const uint32_t* pc =
                            regs + xenos::kAluConstantBase + 256 * 4 + r * 4;
                        fprintf(stderr,
                                "[vkprobe]   pc(%3u) = %10.4f %10.4f %10.4f %10.4f\n",
                                r, F32(pc[0]), F32(pc[1]), F32(pc[2]), F32(pc[3]));
                    }
                    if (comma == std::string::npos)
                        break;
                    at = comma + 1;
                }
            }
            // vc(255) is not an ordinary constant. The D3D shader compiler reserves the
            // last register as a source of known scalars, and several of this title's
            // vertex shaders build `w = 1` with `sges r.w, abs(r0.x), c255.x` — an
            // always-true comparison ONLY IF c255.x is zero. If it is not, w becomes 0,
            // the translation column drops out of the view-projection dot, and the mesh
            // explodes from a point.
            for (uint32_t r : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 255u })
                fprintf(stderr, "[vkprobe]   vc(%2u) = %12.4f %12.4f %12.4f %12.4f\n", r,
                        F32(c[r * 4 + 0]), F32(c[r * 4 + 1]), F32(c[r * 4 + 2]),
                        F32(c[r * 4 + 3]));
            // The constant window a skinned shader indexes dynamically. `vc(8 + a0)` is
            // a bone matrix palette, and vc() CLAMPS above register 255 to zero — so a
            // bone index that is too large silently produces a zero matrix and puts the
            // vertex at the origin, which is what a mesh exploding from a point is.
            // Printing the palette says whether it is populated at all.
            {
                // How much of the palette is actually DISTINCT. A skinned mesh blending
                // bones 6 and 9 needs vc(8+6..) and vc(8+9..) to differ from vc(8..);
                // if the whole window holds one repeated matrix the palette was never
                // uploaded, and every vertex gets bone 0 regardless of its index.
                uint32_t distinct = 0;
                for (uint32_t r = 8; r < 128; r++)
                {
                    bool dup = false;
                    for (uint32_t q = 8; q < r && !dup; q++)
                        dup = memcmp(c + r * 4, c + q * 4, 16) == 0;
                    if (!dup)
                        ++distinct;
                }
                fprintf(stderr,
                        "[vkprobe]   palette vc(8..127): %u distinct rows of 120\n",
                        distinct);
                for (uint32_t r = 8; r < 26; r += 3)
                    fprintf(stderr, "[vkprobe]     vc(%2u) = %9.3f %9.3f %9.3f %9.3f\n",
                            r, F32(c[r * 4 + 0]), F32(c[r * 4 + 1]), F32(c[r * 4 + 2]),
                            F32(c[r * 4 + 3]));
            }

            for (const VertexAttribute& a : vs.attributes)
            {
                // Every attribute's raw bytes for the first few vertices. For a skinned
                // mesh the interesting ones are the weights and the INDICES, and the
                // question they answer is whether an 8-bit integer attribute is
                // arriving as 0..255 (correct) or as a fraction (a normalized format
                // reaching an input that wanted the integer).
                if (a.location != 0 && !a.indirect)
                {
                    const xenos::VertexFetch vf =
                        xenos::DecodeVertexFetch(regs, FetchSlot(a.fetchSlot));
                    const uint32_t sva = PhysToVa(vf.address);
                    if (!GuestRangeOk(sva, uint64_t(vf.sizeDwords) * 4))
                        continue;
                    fprintf(stderr, "[vkprobe]   loc%-3d fmt=%2u int=%u off=%u:",
                            a.location, a.format, a.isInteger, a.offsetDwords);
                    for (uint32_t v = 0; v < 3; v++)
                    {
                        const uint64_t dw =
                            uint64_t(v) * a.strideDwords + a.offsetDwords;
                        if (dw >= vf.sizeDwords)
                            break;
                        uint32_t raw;
                        CopySwapped(reinterpret_cast<uint8_t*>(&raw),
                                    base + sva + dw * 4, 4, vf.endian);
                        fprintf(stderr, "  %08X[%u,%u,%u,%u]", raw, raw & 0xFF,
                                (raw >> 8) & 0xFF, (raw >> 16) & 0xFF, raw >> 24);
                    }
                    // For an 8-bit INTEGER attribute, scan the whole stream and report
                    // the range of each component. On a skinned mesh this is the bone
                    // index, and the range decides whether `vc(8 + a0)` stays inside
                    // the 256-register window the generated macro clamps at — an index
                    // that leaves it does not error, it silently reads as a ZERO
                    // matrix, and a zero matrix puts the vertex at the origin.
                    if (a.format == 6 && a.isInteger)
                    {
                        uint32_t lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
                        const uint32_t verts =
                            a.strideDwords ? vf.sizeDwords / a.strideDwords : 0;
                        for (uint32_t v = 0; v < verts; v++)
                        {
                            const uint64_t dw =
                                uint64_t(v) * a.strideDwords + a.offsetDwords;
                            if (dw >= vf.sizeDwords)
                                break;
                            uint32_t raw;
                            CopySwapped(reinterpret_cast<uint8_t*>(&raw),
                                        base + sva + dw * 4, 4, vf.endian);
                            for (uint32_t k = 0; k < 4; k++)
                            {
                                const uint32_t byte = (raw >> (k * 8)) & 0xFF;
                                lo[k] = std::min(lo[k], byte);
                                hi[k] = std::max(hi[k], byte);
                            }
                        }
                        fprintf(stderr,
                                "[vkprobe]   loc%-3d over %u vertices: x %u..%u  "
                                "y %u..%u  z %u..%u  w %u..%u   (vc(8+max) = %u, "
                                "the macro clamps at 255)\n",
                                a.location, verts, lo[0], hi[0], lo[1], hi[1], lo[2],
                                hi[2], lo[3], hi[3],
                                8 + std::max(std::max(hi[0], hi[1]),
                                             std::max(hi[2], hi[3])));
                    }
                }
                if (a.location != 0 || a.indirect)
                    continue;
                const xenos::VertexFetch vf =
                    xenos::DecodeVertexFetch(regs, FetchSlot(a.fetchSlot));
                fprintf(stderr,
                        "[vkprobe]   POSITION slot=%u addr=%08X size=%u dwords "
                        "stride=%u offset=%u endian=%u\n",
                        a.fetchSlot, vf.address, vf.sizeDwords, a.strideDwords,
                        a.offsetDwords, vf.endian);
                const uint32_t sva = PhysToVa(vf.address);
                if (!GuestRangeOk(sva, uint64_t(vf.sizeDwords) * 4))
                    continue;
                for (uint32_t v = 0; v < 4; v++)
                {
                    const uint64_t dw = uint64_t(v) * a.strideDwords + a.offsetDwords;
                    if (dw + 3 > vf.sizeDwords)
                        break;
                    uint32_t raw[3];
                    CopySwapped(reinterpret_cast<uint8_t*>(raw),
                                base + sva + dw * 4, 12, vf.endian);
                    fprintf(stderr, "[vkprobe]     v%u = %12.4f %12.4f %12.4f\n", v,
                            F32(raw[0]), F32(raw[1]), F32(raw[2]));
                }
            }
        }
    }

    // --- indices ---------------------------------------------------------------------
    if (expand != Expansion::None)
    {
        // Both expansions need the source indices, so an indexed one has to have a
        // readable buffer; an auto-index one synthesises them from the vertex number.
        if (draw.indexed)
        {
            const uint64_t bytes = uint64_t(draw.indexCount) * (draw.index32 ? 4 : 2);
            if (!GuestRangeOk(draw.indexVa, bytes))
            {
                Count("draw: index buffer outside the physical arena");
                return;
            }
        }
        uint32_t expandedCount = 0;
        const VkDeviceSize at = ExpandIndices(base, draw, expand, expandedCount);
        if (at == VkDeviceSize(-1))
            return;
        vkCmdBindIndexBuffer(R->cmd, R->arena.buffer, at, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(R->cmd, expandedCount, 1, 0, 0, 0);
    }
    else if (draw.indexed)
    {
        const uint32_t indexBytes = draw.index32 ? 4 : 2;
        const uint64_t bytes = uint64_t(draw.indexCount) * indexBytes;
        if (!GuestRangeOk(draw.indexVa, bytes))
        {
            Count("draw: index buffer outside the physical arena");
            return;
        }
        // CZ_VK_INDEX_ENDIAN=N overrides the packet's own swizzle code. Scrambled
        // triangles are the classic symptom of an index buffer read with the wrong
        // swizzle — a 16-bit stream under an 8-in-32 code has its PAIRS transposed as
        // well as its bytes — and an arm settles in one run what staring at the
        // geometry cannot.
        static const char* endianOverride = Env("CZ_VK_INDEX_ENDIAN");
        const uint32_t endian =
            endianOverride ? uint32_t(atoi(endianOverride)) : draw.indexEndian;
        {
            static char names[4][32];
            static bool built = false;
            if (!built)
            {
                built = true;
                for (uint32_t i = 0; i < 4; i++)
                    snprintf(names[i], sizeof names[i], "index endian code %u", i);
            }
            Count(names[draw.indexEndian & 3]);
        }
        const VkDeviceSize at = UploadStream(base, draw.indexVa, bytes, endian);
        if (at == VkDeviceSize(-1))
            return;
        vkCmdBindIndexBuffer(R->cmd, R->arena.buffer, at,
                             draw.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(R->cmd, draw.indexCount, 1, 0, 0, 0);
        Count("draw: indexed");
    }
    else
    {
        vkCmdDraw(R->cmd, draw.indexCount, 1, 0, 0);
        Count("draw: auto-index");
    }
    // Fingerprint the draw. Order matters and is included by construction, because the
    // accumulator is sequential — two frames with the same draws in a different order
    // are correctly different frames.
    auto mix = [](uint64_t h, uint64_t v) {
        h ^= v;
        return h * 0x100000001B3ull;
    };
    R->drawFingerprint = mix(R->drawFingerprint, vsBind.hash);
    R->drawFingerprint = mix(R->drawFingerprint, psBind.hash);
    R->drawFingerprint = mix(R->drawFingerprint, (uint64_t(draw.primType) << 32) |
                                                     draw.indexCount);
    R->verticesThisFrame += draw.indexCount;
    if (R->drawsThisFrame == 0)
    {
        // The camera, from the frame's FIRST draw: vc(0..15) covers the
        // view-projection and the world matrices every scene shader reads.
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < 16 * 4; i++)
            h = mix(h, regs[xenos::kAluConstantBase + i]);
        R->cameraFingerprint = h;
    }

    // CZ_VK_PASS_DRAWS=N — how many of a pass's draws the resolve trace lists. Four
    // says what KIND of pass it is; it cannot say what a 115-draw UI compose did, which
    // is where every "why is this rectangle black" question ends. The texture address
    // is on the line because a draw's identity for this purpose is its INPUT.
    static const size_t passDraws =
        Env("CZ_VK_PASS_DRAWS") ? strtoul(Env("CZ_VK_PASS_DRAWS"), nullptr, 10) : 4;
    if (R->firstDrawsThisPass.size() < passDraws)
    {
        char buf[128];
        snprintf(buf, sizeof buf, "prim%u/%uidx/vs=%016llx/ps=%016llx/tex=%08X%s",
                 draw.primType, draw.indexCount, (unsigned long long)vsBind.hash,
                 (unsigned long long)psBind.hash, R->lastTexAddr,
                 R->lastTexAddr && !R->lastTexSlot ? "(DUMMY)" : "");
        R->firstDrawsThisPass.push_back(buf);
    }
    ++R->drawsThisFrame;
    ++R->drawsThisPass;
    R->verticesThisPass += draw.indexCount;
}

// ===================================================================================
// Resolve
// ===================================================================================
// A resolve is a DRAW whose RB_MODECONTROL edram_mode is kCopy (6) — not a packet of
// its own, and not "a draw with a particular shader pair bound". Gating on the shader
// pair recognises only the one blit the present path happens to use and silently drops
// every intermediate resolve in a post-processing chain, which is how the previous port
// lost its bloom for a session.
//
// What we do with it is deliberately narrow for now: record the destination so the
// present seam knows which surface is the frame, and honour the clear bits. Writing the
// resolved pixels back into guest memory is NOT done, and that is a stated gap rather
// than an oversight — the title samples some of its own resolves as textures, and doing
// that correctly means serving them from the host image rather than round-tripping
// through guest memory.
void DoResolve(uint8_t* base, const uint32_t* regs)
{
    (void)base;
    const uint32_t control = regs[xenos::kRbCopyControl];
    const uint32_t dest = regs[xenos::kRbCopyDestBase] & 0xFFFFFFFCu;
    R->lastResolveDest = dest;
    Count("resolve");

    // RB_COPY_CONTROL bits 8/9: clear colour / clear depth after the copy. This is the
    // title's own clear, and honouring it is what makes a persistent EDRAM target
    // correct rather than an accumulating smear.
    // CZ_VK_RESOLVE_TRACE=1 — one line per resolve for a few frames, with the surface
    // the EDRAM is configured as, the region being copied out and where it lands.
    //
    // This is the instrument for the question "which of these is the frame?". A title
    // composes through several intermediate surfaces at several sizes and resolves each
    // one; presenting the EDRAM target wholesale shows all of them overlaid at
    // whatever size each pass happened to use, which is a picture that looks like a
    // scaling bug and is really a missing surface identity.
    // CZ_VK_RESOLVE_TRACE=N starts at frame N (1 = from the beginning). The boot's
    // first frames are not the frame anyone is investigating, and 60 lines of them is
    // all a from-the-start trace ever shows.
    if (EnvOn("CZ_VK_RESOLVE_TRACE") &&
        R->frame >= uint64_t(strtoul(Env("CZ_VK_RESOLVE_TRACE"), nullptr, 10)))
    {
        static int left = 60;
        if (left-- > 0)
            fprintf(stderr,
                    "[vkresolve] frame=%llu dest=%08X destPitch=%u destHeight=%u "
                    "surfacePitch=%u scissor=%u,%u..%u,%u win=%u,%u..%u,%u "
                    "winoff=%08X ctl=%08X info=%08X "
                    "front=%08X rtFmt=%u draws=%llu verts=%llu\n",
                    (unsigned long long)R->frame, dest,
                    regs[xenos::kRbCopyDestPitch] & 0x3FFF,
                    (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFF,
                    regs[xenos::kRbSurfaceInfo] & 0x3FFF,
                    regs[xenos::kPaScScreenScissorTl] & 0x7FFF,
                    (regs[xenos::kPaScScreenScissorTl] >> 16) & 0x7FFF,
                    regs[xenos::kPaScScreenScissorBr] & 0x7FFF,
                    (regs[xenos::kPaScScreenScissorBr] >> 16) & 0x7FFF,
                    regs[xenos::kPaScWindowScissorTl] & 0x7FFF,
                    (regs[xenos::kPaScWindowScissorTl] >> 16) & 0x7FFF,
                    regs[xenos::kPaScWindowScissorBr] & 0x7FFF,
                    (regs[xenos::kPaScWindowScissorBr] >> 16) & 0x7FFF,
                    regs[xenos::kPaScWindowOffset], control,
                    regs[xenos::kRbCopyDestInfo], R->frontBuffer,
                    (regs[xenos::kRbColorInfo] >> 16) & 0xF,
                    (unsigned long long)R->drawsThisPass,
                    (unsigned long long)R->verticesThisPass);
        // The pass's INPUTS, which is what says whether a compose is reading the scene.
        fprintf(stderr, "[vkresolve]     sampled snapshots:");
        if (R->snapshotsSampledThisPass.empty())
            fprintf(stderr, " (none)");
        for (uint32_t a : R->snapshotsSampledThisPass)
            fprintf(stderr, " %08X", a);
        fprintf(stderr, "   guest textures uploaded: %llu\n",
                (unsigned long long)R->guestTexturesThisPass);
        fprintf(stderr, "[vkresolve]     first draws:");
        for (const auto& d : R->firstDrawsThisPass)
            fprintf(stderr, "\n[vkresolve]       %s", d.c_str());
        fprintf(stderr, "\n");
    }
    R->drawsThisPass = 0;
    R->verticesThisPass = 0;
    R->snapshotsSampledThisPass.clear();
    R->guestTexturesThisPass = 0;
    R->firstDrawsThisPass.clear();

    const bool clearColor = ((control >> 8) & 1) != 0;
    const bool clearDepth = ((control >> 9) & 1) != 0;

    // SNAPSHOT THE EDRAM UNDER THE DESTINATION ADDRESS. See the Snapshot comment for
    // why the destination address is the right identity and why the pixels do not go
    // back to guest memory.
    // The SURFACE is RB_COPY_DEST_PITCH; the REGION being copied out of the EDRAM is
    // the window scissor. Those are different things and conflating them is what put
    // the whole picture in the top-left corner: a 640x720 tile copied as if it were the
    // full 1280x720 destination.
    const uint32_t surfW = regs[xenos::kRbCopyDestPitch] & 0x3FFF;
    const uint32_t surfH = (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFF;
    const uint32_t wx = regs[xenos::kPaScWindowScissorTl] & 0x7FFF;
    const uint32_t wy = (regs[xenos::kPaScWindowScissorTl] >> 16) & 0x7FFF;
    const uint32_t wx1 = regs[xenos::kPaScWindowScissorBr] & 0x7FFF;
    const uint32_t wy1 = (regs[xenos::kPaScWindowScissorBr] >> 16) & 0x7FFF;

    // AND THE TILES OF ONE SURFACE SHARE A KEY. The second tile's RB_COPY_DEST_BASE is
    // pre-offset into the SAME allocation — 06BF8000 is 06BE4000 + 0x14000, and 0x14000
    // is exactly the 20 macro-tiles that 640 pixels of a 4-byte tiled surface occupy
    // (20 x 4096). Keying on the raw base makes one surface look like two, so a
    // consumer fetching the surface's real base gets a snapshot holding only the left
    // half. Subtracting the tile offset puts both halves in one image, which is what
    // the guest's own memory layout does.
    auto macroTileOffset = [](uint32_t x, uint32_t y, uint32_t pitch) -> uint32_t {
        return ((x >> 5) + (y >> 5) * (std::max(pitch, 32u) >> 5)) * 4096u;
    };
    const uint32_t baseKey =
        (dest - macroTileOffset(wx, wy, surfW)) & 0x1FFFFFFF;

    const uint32_t w = std::min(surfW, R->color.width);
    const uint32_t h = std::min(surfH, R->color.height);
    const uint32_t copyX = std::min(wx, w);
    const uint32_t copyY = std::min(wy, h);
    const uint32_t copyW = wx1 > wx ? std::min(wx1, w) - copyX : w - copyX;
    const uint32_t copyH = wy1 > wy ? std::min(wy1, h) - copyY : h - copyY;
    if (w && h && copyW && copyH)
    {
        const uint32_t key = baseKey;
        auto it = R->snapshots.find(key);
        // A destination whose extent changed is a different surface reusing an
        // address, so the image is rebuilt rather than partially overwritten — a
        // partial overwrite leaves the previous surface's pixels around the edge of
        // the new one, which reads as a ghosting artefact with no obvious source.
        if (it != R->snapshots.end() &&
            (it->second.image.width != w || it->second.image.height != h))
        {
            vkDeviceWaitIdle(R->device);
            vkDestroyImageView(R->device, it->second.image.view, nullptr);
            vkDestroyImage(R->device, it->second.image.image, nullptr);
            vkFreeMemory(R->device, it->second.image.memory, nullptr);
            R->snapshots.erase(it);
            it = R->snapshots.end();
            Count("resolve: snapshot resized");
        }
        if (it == R->snapshots.end() && R->nextTextureSlot < kMaxDescriptors)
        {
            Snapshot s;
            s.slot = R->nextTextureSlot++;
            if (CreateImage(s.image, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT))
            {
                VkDescriptorImageInfo ii{};
                ii.imageView = s.image.view;
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                wr.dstSet = R->sets[0];
                wr.dstBinding = 0;
                wr.dstArrayElement = s.slot;
                wr.descriptorCount = 1;
                wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                wr.pImageInfo = &ii;
                vkUpdateDescriptorSets(R->device, 1, &wr, 0, nullptr);
                it = R->snapshots.emplace(key, std::move(s)).first;
                Count("resolve: snapshot created");
            }
            else
            {
                --R->nextTextureSlot;
                Count("resolve: snapshot image creation failed");
            }
        }
        if (it != R->snapshots.end())
        {
            BeginFrame();
            EndRendering();
            Barrier(R->cmd, R->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            Barrier(R->cmd, it->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            // Copy the TILE, at its own position in both images. Source and destination
            // offsets are the same because our EDRAM is full-screen-sized and the
            // window offset is deliberately not applied to the geometry (see the
            // scissor note in DoDraw), so a tile sits at its true screen position in
            // both.
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.srcOffset = { int32_t(copyX), int32_t(copyY), 0 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstOffset = { int32_t(copyX), int32_t(copyY), 0 };
            copy.extent = { copyW, copyH, 1 };
            vkCmdCopyImage(R->cmd, R->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           it->second.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
            // Back to SHADER_READ_ONLY immediately: a later pass in this same frame
            // samples this surface, and the layout it expects is the one the
            // descriptor was written with.
            Barrier(R->cmd, it->second.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            it->second.frameSeen = R->frame;

            if (R->frontBuffer && key == (R->frontBuffer & 0x1FFFFFFF))
            {
                R->frontWidth = w;
                R->frontHeight = h;
                R->haveFrontSnapshot = true;
                Count("resolve: this is the frame");
            }
        }
    }

    if (!clearColor && !clearDepth)
        return;

    BeginFrame();
    EndRendering();

    if (clearColor)
    {
        // RB_COLOR_CLEAR holds the clear value in the render target's own format. It
        // is read as 8888 here; a target in another format would clear to the wrong
        // colour, which is why the count is separate from the resolve count.
        const uint32_t c = regs[xenos::kRbColorClear];
        VkClearColorValue value{};
        value.float32[0] = float((c >> 16) & 0xFF) / 255.0f;
        value.float32[1] = float((c >> 8) & 0xFF) / 255.0f;
        value.float32[2] = float(c & 0xFF) / 255.0f;
        value.float32[3] = float((c >> 24) & 0xFF) / 255.0f;
        Barrier(R->cmd, R->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(R->cmd, R->color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &value, 1, &range);
        Count("resolve: colour cleared");
    }
    if (clearDepth)
    {
        VkClearDepthStencilValue value{};
        value.depth = float(regs[xenos::kRbDepthClear] >> 8) / float(0xFFFFFF);
        value.stencil = regs[xenos::kRbDepthClear] & 0xFF;
        Barrier(R->cmd, R->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1
        };
        vkCmdClearDepthStencilImage(R->cmd, R->depth.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                    &range);
        Count("resolve: depth cleared");
    }
}

} // namespace

// ===================================================================================
// The public seam
// ===================================================================================
bool VkRenderer_Active() { return g_active; }

namespace {

// Device bring-up, shared by both feeds. Sets g_active on success; which feed owns
// the renderer is the CALLER's declaration (g_d3dMode), not decided here.
bool InitCommon()
{
    R = new Renderer();
    if (!CreateDevice() || !CreateDescriptorPlumbing())
    {
        fprintf(stderr, "[vk] device bring-up FAILED — running without a renderer\n");
        return false;
    }

    // The EDRAM stand-in. Sized to the guest's own stated front-buffer dimensions,
    // which VdSwap carries in every swap packet; 1280x720 until the first one arrives.
    if (!CreateImage(R->color, R->targetWidth, R->targetHeight,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT) ||
        !CreateImage(R->depth, R->targetWidth, R->targetHeight,
                     VK_FORMAT_D24_UNORM_S8_UINT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
    {
        fprintf(stderr, "[vk] render target creation FAILED\n");
        return false;
    }

    // 128 MB of per-frame arena. The frontend's streams are small; gameplay is the
    // question, and the high-water mark is printed with the stats so the number can be
    // raised on evidence rather than guessed at again.
    if (!CreateBuffer(R->arena, 128ull << 20,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      /*deviceAddress=*/true) ||
        !CreateBuffer(R->staging, 64ull << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      false) ||
        !CreateBuffer(R->readback,
                      uint64_t(R->targetWidth) * R->targetHeight * 4,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      false))
    {
        fprintf(stderr, "[vk] buffer allocation FAILED\n");
        return false;
    }

    // Two samplers, and one global choice per draw is a stated simplification: the
    // fetch constant carries per-texture filter and address modes that this does not
    // yet honour. Named here so the next reader knows it is a gap with a location
    // rather than a mystery in the picture.
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(R->device, &si, nullptr, &R->linearSampler) != VK_SUCCESS)
        return false;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(R->device, &si, nullptr, &R->pointSampler) != VK_SUCCESS)
        return false;

    // The dummies. Slot 0 of every heap is a defined 1x1 white texel, so a shader that
    // samples a slot the runtime could not fill reads white rather than an unbound
    // descriptor — undefined behaviour even when the result is discarded.
    auto makeDummy = [&](Image& img, VkImageViewType type, uint32_t layers,
                         uint32_t depth, uint32_t setIndex) {
        if (!CreateImage(img, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, type, layers, depth))
            return false;
        const uint32_t white = 0xFFFFFFFFu;
        memcpy(R->staging.mapped, &white, 4);
        RunImmediate([&](VkCommandBuffer cb) {
            Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers };
            copy.imageExtent = { 1, 1, depth };
            vkCmdCopyBufferToImage(cb, R->staging.buffer, img.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
        });
        VkDescriptorImageInfo ii{};
        ii.imageView = img.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = R->sets[setIndex];
        w.dstBinding = 0;
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(R->device, 1, &w, 0, nullptr);
        return true;
    };
    if (!makeDummy(R->dummy2D, VK_IMAGE_VIEW_TYPE_2D, 1, 1, 0) ||
        !makeDummy(R->dummy3D, VK_IMAGE_VIEW_TYPE_3D, 1, 1, 1) ||
        !makeDummy(R->dummyCube, VK_IMAGE_VIEW_TYPE_CUBE, 6, 1, 2) ||
        !makeDummy(R->dummy1D, VK_IMAGE_VIEW_TYPE_1D, 1, 1, 4))
    {
        fprintf(stderr, "[vk] dummy texture creation FAILED\n");
        return false;
    }

    {
        VkDescriptorImageInfo si2{};
        si2.sampler = R->linearSampler;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = R->sets[3];
        w.dstBinding = 0;
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &si2;
        vkUpdateDescriptorSets(R->device, 1, &w, 0, nullptr);
    }

    if (!LoadShaders())
        return false;

    R->presentPixels.resize(size_t(R->targetWidth) * R->targetHeight * 4);
    g_texCensus = EnvOn("CZ_VK_TEX_CENSUS");
    g_active = true;
    fprintf(stderr, "[vk] renderer UP: %ux%u target, %zu shaders\n", R->targetWidth,
            R->targetHeight, R->shaders.size());
    return true;
}

} // namespace

bool VkRenderer_Init()
{
    if (g_initTried)
        return g_active && !g_d3dMode;
    g_initTried = true;

    if (!EnvOn("CZ_VKDRAW"))
    {
        fprintf(stderr, "[vk] renderer OFF (set CZ_VKDRAW=1 to enable it)\n");
        return false;
    }
    // InitCommon names its own failure on every path.
    return InitCommon();
}

void VkRenderer_Draw(uint8_t* base, const Pm4Draw& draw)
{
    if (!g_active || g_d3dMode)
        return;
    // The renderer's own count of draws it was HANDED, next to the per-primitive
    // census of draws it accepted. The command processor's `ring: ... draws=` counter
    // and the renderer's prim counters disagreed by half and there was no number in
    // between to say where the difference lived — a chain has to be counted link by
    // link (gotcha 162), including the link between two modules.
    Count("draw: handed to the renderer");
    const uint32_t* regs = Pm4_Registers();
    // The resolve discriminator, and the only one: RB_MODECONTROL's edram_mode.
    if ((regs[0x2208] & 7) == 6)
    {
        DoResolve(base, regs);
        return;
    }
    DoDraw(base, draw, regs, Pm4_BoundShader(0), Pm4_BoundShader(1));
}

namespace {

// The shared swap body — everything from "record the front buffer" to the frame
// stats line. The PM4 feed calls it from the XE_SWAP packet, the D3D feed from the
// Swap hook; the two callers gate on g_d3dMode so exactly one is live per run.
void DoSwapImpl(uint8_t* base, uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    (void)base;
    // Recorded BEFORE the early returns: the resolve that produces the frame happens
    // before the swap that announces it, so on frame N the comparison in DoResolve is
    // made against the address frame N-1 published. That is fine because the address
    // does not change, and it is the reason the first frame has no snapshot rather
    // than the wrong one.
    R->frontBuffer = frontBuffer;
    ++R->frame;

    if (!R->recording)
    {
        // A frame with no recorded work at all: present the previous contents rather
        // than nothing, so a stall in the draw path shows as a frozen picture instead
        // of a flicker between the real frame and black.
        Count("swap: nothing recorded");
        return;
    }

    // Read the colour target back and hand it to the window. A readback per frame is a
    // real cost and it is chosen deliberately: the alternative is a Vulkan swapchain on
    // the SDL window, which would put Vulkan on the window's thread and couple the
    // renderer to the windowing system that phase 3 deliberately kept at arm's length.
    // At the guest's own ~30 frames a second, 3.5 MB a frame is not what limits this.
    EndRendering();

    // Read back the front-buffer snapshot when there is one, and the raw EDRAM when
    // there is not. The fallback is deliberate and is announced by its own counter:
    // it is what a frame looks like before the surface identity is known, and seeing
    // it in the stats is how "the resolve match stopped working" stays visible
    // instead of turning into a picture that is subtly the wrong pass.
    auto frontSnap = R->snapshots.find(R->frontBuffer & 0x1FFFFFFF);
    if (frontSnap == R->snapshots.end())
        R->haveFrontSnapshot = false;
    Image& source = R->haveFrontSnapshot ? frontSnap->second.image : R->color;
    const uint32_t width0 = R->haveFrontSnapshot ? R->frontWidth : R->color.width;
    const uint32_t height0 = R->haveFrontSnapshot ? R->frontHeight : R->color.height;
    Count(R->haveFrontSnapshot ? "swap: presented the front-buffer resolve"
                               : "swap: presented raw EDRAM (no resolve matched)");

    Barrier(R->cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width0, height0, 1 };
    vkCmdCopyImageToBuffer(R->cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           R->readback.buffer, 1, &copy);
    SubmitAndWait();

    const size_t bytes = size_t(width0) * height0 * 4;
    if (R->presentPixels.size() < bytes)
        R->presentPixels.resize(bytes);
    memcpy(R->presentPixels.data(), R->readback.mapped, bytes);
    Host_PresentPixels(R->presentPixels.data(), width0, height0);

    // CZ_VK_FRAME_DUMP=<dir> writes every 64th frame as a PPM. This is the instrument
    // that makes the renderer checkable WITHOUT a window, which matters more than it
    // sounds: every other gate this project owns is a log diff, and "the picture is
    // right" is the one claim that needs an image. A headless run plus a directory of
    // frames is a self-servable version of the E-screenshot comparison.
    // CZ_VK_FRAME_DUMP_EVERY=N overrides the 64. A screen the synthetic-input arm walks
    // THROUGH rather than parks on can be shorter than 64 frames, and one dump of it is
    // one sample of a transition — the save-slot panel below appeared in exactly one
    // frame of a 180 s boot.
    static const char* dumpDir = Env("CZ_VK_FRAME_DUMP");
    static const uint64_t dumpEvery =
        Env("CZ_VK_FRAME_DUMP_EVERY")
            ? std::max<uint64_t>(1, strtoull(Env("CZ_VK_FRAME_DUMP_EVERY"), nullptr, 10))
            : 64;
    if (dumpDir && (R->frame % dumpEvery) == 0)
    {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dumpDir,
                 (unsigned long long)R->frame);
        if (FILE* f = fopen(path, "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", width0, height0);
            for (size_t i = 0; i < bytes; i += 4)
                fwrite(&R->presentPixels[i], 1, 3, f);
            fclose(f);
        }
    }

    // CZ_VK_FRAME_STATS=<file> — one line per frame: what the guest asked for, and what
    // came out. This is the raw material for tools/frame_compare.py, which aligns two
    // runs by CONTENT and only then compares their pictures.
    //
    // The output measurements are deliberately cheap and whole-image (coverage, mean
    // luminance, distinct colours, a pixel hash) rather than a per-pixel dump: the
    // question a renderer A/B asks is "did this frame change", and for that a small
    // vector of aggregates over the same content is enough — while a per-pixel dump at
    // 30 frames a second is 100 MB a run nobody reads.
    static FILE* statsFile = nullptr;
    static bool statsTried = false;
    if (!statsTried)
    {
        statsTried = true;
        if (const char* path = Env("CZ_VK_FRAME_STATS"))
        {
            statsFile = fopen(path, "w");
            if (statsFile)
                fprintf(statsFile,
                        "# frame draws vertices drawFingerprint cameraFingerprint "
                        "width height coveragePct meanLuma distinctColours pixelHash "
                        "surfW surfH surfCoveragePct surfMeanLuma surfDistinct "
                        "surfHash\n");
            else
                fprintf(stderr, "[vk] cannot write CZ_VK_FRAME_STATS=%s\n", path);
        }
    }
    // CZ_VK_FRAME_STATS_SURFACE=<hex> — measure THAT resolve surface as well as the
    // presented frame.
    //
    // This is not a refinement, it is the thing that makes the metric work at all. The
    // first version measured only the presented front buffer, which at the title screen
    // is the logo era: mostly UI, 2-36% covered. Disabling the 16-bit texcoord
    // unswizzle — a change that touches 476,858 draws a run — moved it by 0.1
    // percentage points, i.e. the metric could not see a defect it was built to catch,
    // because the defect lives on the SCENE surface and the metric was looking at the
    // overlay. Gotcha 30: a test that has never failed has not been shown capable of
    // failing, and this one was shown incapable.
    //
    // Set it to the scene's resolve destination (06BE4000 at the title screen; the
    // CZ_VK_RESOLVE_TRACE output names it for any era).
    static const char* surfaceEnv = Env("CZ_VK_FRAME_STATS_SURFACE");
    static const uint32_t statsSurface =
        surfaceEnv ? uint32_t(strtoul(surfaceEnv, nullptr, 16)) & 0x1FFFFFFF : 0;
    std::vector<uint8_t> surfacePixels;
    uint32_t surfaceW = 0, surfaceH = 0;
    if (statsFile && statsSurface)
    {
        auto sit = R->snapshots.find(statsSurface);
        if (sit != R->snapshots.end())
        {
            const uint64_t n =
                uint64_t(sit->second.image.width) * sit->second.image.height * 4;
            if (n <= R->readback.size)
            {
                RunImmediate([&](VkCommandBuffer cb) {
                    Barrier(cb, sit->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
                    VkBufferImageCopy c{};
                    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    c.imageExtent = { sit->second.image.width, sit->second.image.height,
                                      1 };
                    vkCmdCopyImageToBuffer(cb, sit->second.image.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           R->readback.buffer, 1, &c);
                    Barrier(cb, sit->second.image,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
                });
                surfacePixels.assign(R->readback.mapped, R->readback.mapped + n);
                surfaceW = sit->second.image.width;
                surfaceH = sit->second.image.height;
            }
        }
    }

    if (statsFile)
    {
        uint64_t lit = 0, lumaSum = 0, ph = 0xCBF29CE484222325ull;
        // Distinct colours exactly, without a hash set: the frame is RGBA8 and a
        // 2^24-bit bitmap is 2 MB, which is cheaper than a hash table per frame and
        // gives an exact count rather than an estimate.
        static std::vector<uint64_t> seenBits;
        seenBits.assign(1u << 18, 0); // 2^24 bits
        uint64_t distinct = 0;
        for (size_t i = 0; i < bytes; i += 4)
        {
            const uint32_t r = R->presentPixels[i], g = R->presentPixels[i + 1],
                           b = R->presentPixels[i + 2];
            const uint32_t rgb = (r << 16) | (g << 8) | b;
            if (rgb)
                ++lit;
            lumaSum += (r * 54 + g * 183 + b * 19) >> 8;
            const uint32_t word = rgb >> 6, bit = rgb & 63;
            if (!(seenBits[word] & (1ull << bit)))
            {
                seenBits[word] |= 1ull << bit;
                ++distinct;
            }
            ph ^= rgb;
            ph *= 0x100000001B3ull;
        }
        const uint64_t pixels = bytes / 4;
        // The named surface, measured the same way. Zeros when it was not requested or
        // does not exist this frame, which frame_compare.py reads as "no surface data"
        // rather than as an empty surface.
        uint64_t slit = 0, slumaSum = 0, sph = 0xCBF29CE484222325ull, sdistinct = 0;
        if (!surfacePixels.empty())
        {
            seenBits.assign(1u << 18, 0);
            for (size_t i = 0; i < surfacePixels.size(); i += 4)
            {
                const uint32_t r = surfacePixels[i], g = surfacePixels[i + 1],
                               b = surfacePixels[i + 2];
                const uint32_t rgb = (r << 16) | (g << 8) | b;
                if (rgb)
                    ++slit;
                slumaSum += (r * 54 + g * 183 + b * 19) >> 8;
                const uint32_t word = rgb >> 6, bit = rgb & 63;
                if (!(seenBits[word] & (1ull << bit)))
                {
                    seenBits[word] |= 1ull << bit;
                    ++sdistinct;
                }
                sph ^= rgb;
                sph *= 0x100000001B3ull;
            }
        }
        const uint64_t spixels = surfacePixels.size() / 4;
        fprintf(statsFile,
                "%llu %llu %llu %016llx %016llx %u %u %.4f %.3f %llu %016llx "
                "%u %u %.4f %.3f %llu %016llx\n",
                (unsigned long long)R->frame, (unsigned long long)R->drawsThisFrame,
                (unsigned long long)R->verticesThisFrame,
                (unsigned long long)R->drawFingerprint,
                (unsigned long long)R->cameraFingerprint, width0, height0,
                pixels ? 100.0 * double(lit) / double(pixels) : 0.0,
                pixels ? double(lumaSum) / double(pixels) : 0.0,
                (unsigned long long)distinct, (unsigned long long)ph,
                surfaceW, surfaceH,
                spixels ? 100.0 * double(slit) / double(spixels) : 0.0,
                spixels ? double(slumaSum) / double(spixels) : 0.0,
                (unsigned long long)sdistinct,
                spixels ? (unsigned long long)sph : 0ull);
        fflush(statsFile);
    }
    R->drawFingerprint = 0;
    R->cameraFingerprint = 0;
    R->verticesThisFrame = 0;

    // A frame that is entirely one colour is the single most common wrong result a
    // renderer produces, and it is invisible in a log. Counting it makes "the picture
    // is black" a number rather than a report — and separating "black" from "some
    // uniform colour" separates a missing draw from a clear that ran and nothing else.
    {
        uint32_t first = 0;
        memcpy(&first, R->presentPixels.data(), 4);
        bool uniform = true;
        for (size_t i = 4; i < bytes && uniform; i += 4)
            uniform = memcmp(&R->presentPixels[i], &first, 4) == 0;
        if (uniform)
            Count(first == 0xFF000000u || first == 0 ? "frame: uniformly black"
                                                     : "frame: uniformly one colour");
        else
            Count("frame: has content");
    }

    // CZ_VK_SNAP_DUMP=<dir> — write EVERY resolve snapshot of one frame as a PPM.
    //
    // The question this answers is "where in the chain did the picture go?", and it is
    // the only instrument that can: the frame is the last link, so a wrong frame is
    // consistent with every pass being wrong and with exactly one being wrong. Dumping
    // all of them turns that into a directory you can look at.
    // CZ_VK_SNAP_FRAME=N picks the frame. It was a hardcoded 600 for as long as the
    // instrument existed, which was fine while every question was about the title
    // screen — and useless the moment one was not. Phase C part 12's defect is on a
    // menu two presses past the title, i.e. at whatever frame the synthetic-input arm
    // happens to land on, and a dependency graph of the wrong frame answers nothing.
    static const char* snapDir = Env("CZ_VK_SNAP_DUMP");
    static const uint64_t snapFrame =
        Env("CZ_VK_SNAP_FRAME") ? strtoull(Env("CZ_VK_SNAP_FRAME"), nullptr, 10) : 600;
    if (snapDir && R->frame == snapFrame)
    {
        for (const auto& [dest, snap] : R->snapshots)
        {
            const size_t n = size_t(snap.image.width) * snap.image.height * 4;
            if (n > R->readback.size)
                continue;
            RunImmediate([&](VkCommandBuffer cb) {
                Image& img = const_cast<Image&>(snap.image);
                Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_ASPECT_COLOR_BIT);
                VkBufferImageCopy c{};
                c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                c.imageExtent = { img.width, img.height, 1 };
                vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       R->readback.buffer, 1, &c);
                Barrier(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_ASPECT_COLOR_BIT);
            });
            char path[512];
            snprintf(path, sizeof path, "%s/snap_%08X_%ux%u.ppm", snapDir, dest,
                     snap.image.width, snap.image.height);
            if (FILE* f = fopen(path, "wb"))
            {
                fprintf(f, "P6\n%u %u\n255\n", snap.image.width, snap.image.height);
                for (size_t i = 0; i < n; i += 4)
                    fwrite(R->readback.mapped + i, 1, 3, f);
                fclose(f);
            }
        }
        fprintf(stderr, "[vk] dumped %zu resolve snapshots to %s\n", R->snapshots.size(),
                snapDir);
    }

    static const uint64_t statsEvery =
        Env("CZ_VK_STATS") ? std::max(1L, strtol(Env("CZ_VK_STATS"), nullptr, 10)) : 0;
    if (statsEvery && (R->frame % statsEvery) == 0)
        VkRenderer_DumpStats();

    // The snapshot is per frame: a frame whose resolve chain never reaches the front
    // buffer must not present the previous frame's picture as if it were this one.
    R->haveFrontSnapshot = false;
    (void)width;
    (void)height;
}

} // namespace

void VkRenderer_OnSwap(uint8_t* base, uint32_t frontBuffer, uint32_t width,
                       uint32_t height)
{
    if (!g_active || g_d3dMode)
        return;
    DoSwapImpl(base, frontBuffer, width, height);
}

// --- the phase C feed --------------------------------------------------------------
bool VkRenderer_D3DInit()
{
    static bool tried = false, ok = false;
    if (tried)
        return ok;
    tried = true;
    if (g_active)
    {
        // The PM4 feed initialized first (CZ_VKDRAW). d3d_draw.cpp refuses the
        // combination before calling here, so reaching this is a wiring bug.
        fprintf(stderr, "[vk] D3D feed refused: the PM4 feed already owns the renderer\n");
        return false;
    }
    if (!InitCommon())
        return false;
    g_d3dMode = true;
    ok = true;
    fprintf(stderr, "[vk] renderer feed: D3D draw service (phase C)\n");
    return true;
}

void VkRenderer_D3DDraw(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                        const Pm4ShaderBinding& vs, const Pm4ShaderBinding& ps)
{
    if (!g_active || !g_d3dMode)
        return;
    // The same resolve discriminator as the PM4 feed, over the PRIVATE register
    // file: the copy-mode SET_CONSTANTs the Resolve body emits land there.
    if ((regs[0x2208] & 7) == 6)
    {
        DoResolve(base, regs);
        return;
    }
    DoDraw(base, draw, regs, vs, ps);
}

void VkRenderer_D3DSwap(uint8_t* base)
{
    if (!g_active || !g_d3dMode)
        return;
    // The front buffer is the destination of the resolve the title just performed
    // (PreSwapResolve immediately precedes every Swap), so no side channel names it.
    DoSwapImpl(base, R->lastResolveDest, R->targetWidth, R->targetHeight);
}

void VkRenderer_DumpStats()
{
    if (!g_active)
        return;
    fprintf(stderr, "[vk] --- renderer stats (frame %llu) ---\n",
            (unsigned long long)R->frame);
    fprintf(stderr, "[vk]   pipelines=%zu shaders=%zu textures=%zu arenaHighWater=%llu KB\n",
            R->pipelines.size(), R->shaders.size(), R->textures.size(),
            (unsigned long long)(R->arenaHighWater >> 10));
    for (const auto& [name, count] : g_stats)
        fprintf(stderr, "[vk]   %-52s %llu\n", name.c_str(),
                (unsigned long long)count);

    // The per-address table. Only the rows that say something are printed: a surface
    // this renderer resolved to, or an upload that came out entirely zero. Everything
    // else is an ordinary disc texture and the aggregate counters already cover it.
    if (g_texCensus)
    {
        fprintf(stderr, "[vk]   texture sources (addr, extent, fmt | uploads/zero, "
                        "snapshot, tooOld maxAge):\n");
        for (const auto& [addr, s] : g_texSources)
        {
            if (!s.everResolved && !s.zeroUploads)
                continue;
            // Re-read the source bytes NOW. A row that uploaded black and is still
            // black in guest memory is a texture the guest never wrote; one that
            // uploaded black and now reads non-zero is a texture that arrived AFTER
            // our one and only upload, and is frozen black by the cache.
            const char* note = "";
            if (s.zeroUploads && s.srcBytes && s.src)
            {
                const uint8_t* p = s.src;
                bool nowZero = true;
                for (uint64_t i = 0; i < s.srcBytes; i++)
                    if (p[i])
                    {
                        nowZero = false;
                        break;
                    }
                note = nowZero ? "   <- uploaded BLACK, guest memory STILL zero"
                               : "   <- uploaded BLACK, guest memory is NON-ZERO NOW";
            }
            fprintf(stderr,
                    "[vk]     %08X %4ux%-4u f%-2u | up %llu (zero %llu)  snap %llu  "
                    "tooOld %llu (max age %llu)%s\n",
                    addr, s.width, s.height, s.format, (unsigned long long)s.uploads,
                    (unsigned long long)s.zeroUploads,
                    (unsigned long long)s.fromSnapshot,
                    (unsigned long long)s.snapshotTooOld, (unsigned long long)s.maxAge,
                    note);
        }
    }
}

