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

constexpr uint32_t kVsConstBytes = 256 * 16;
constexpr uint32_t kPsConstBytes = 224 * 16;

constexpr uint32_t kMaxDescriptors = 4096; // per heap; the frontend uses a few dozen

// --- diagnostics --------------------------------------------------------------------
// Every path that declines to do something increments one of these. The alternative —
// returning quietly — is what makes a renderer that draws 80% of a frame look exactly
// like one that draws all of it, and this project has already paid for that lesson in
// the command processor (gotcha 84: a parser that stops early must say so).
std::map<std::string, uint64_t> g_stats;
void Count(const char* name) { ++g_stats[name]; }

bool g_active = false;
bool g_initTried = false;

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
    switch (fmt)
    {
        case 6:  return isSigned ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
        case 25: return isSigned ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
        case 26: return isSigned ? VK_FORMAT_R16G16B16A16_SNORM
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
    uint32_t nextTextureSlot = 1; // slot 0 is the dummy

    // Per-frame vertex/index stream cache: one guest buffer copied once per frame
    // however many draws read it.
    std::unordered_map<uint64_t, VkDeviceSize> streamCache;

    uint64_t frame = 0;
    uint64_t drawsThisFrame = 0;
    uint32_t targetWidth = 1280, targetHeight = 720;
    uint32_t frontBuffer = 0;
    uint32_t lastResolveDest = 0;
    bool haveResolvedThisFrame = false;

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
                 uint32_t depthExtent = 1)
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
    if (EnvOn("CZ_VK_VALIDATION"))
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
        fprintf(stderr, "[vk] validation layer requested\n");
    }
    VK_CHECK(vkCreateInstance(&ici, nullptr, &R->instance), "vkCreateInstance");

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
    auto cached = R->textures.find(key);
    if (cached != R->textures.end())
    {
        Count("texture: cache hit");
        return cached->second.slot;
    }

    const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
    if (t.type != 2)
    {
        Count("texture: fetch constant is not a texture");
        return 0;
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

    if (R->nextTextureSlot >= kMaxDescriptors)
    {
        Count("texture: bindless heap full");
        return 0;
    }

    TextureEntry entry;
    entry.key = key;
    entry.slot = R->nextTextureSlot++;
    if (!CreateImage(entry.image, t.width, t.height, format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT))
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
VkPrimitiveTopology XenosTopology(uint32_t prim, bool& supported)
{
    supported = true;
    switch (prim)
    {
        case xenos::kPointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case xenos::kLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case xenos::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case xenos::kTriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case xenos::kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case xenos::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        // A Xenos rectangle list is three corners per rect and hardware synthesises
        // the fourth. Vulkan has no such topology; drawing it as a triangle list gives
        // the correct upper-left triangle of every rect and loses the other half. That
        // is visibly wrong for full-screen quads, so it is counted separately and
        // fixed by an index rewrite below rather than left as a lie.
        case xenos::kRectangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
    ds.depthTestEnable = ((key.depthControl >> 1) & 1) ? VK_TRUE : VK_FALSE;
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
    const uint64_t key = (uint64_t(va) << 24) ^ (bytes << 2) ^ endian;
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

void DoDraw(uint8_t* base, const Pm4Draw& draw)
{
    const uint32_t* regs = Pm4_Registers();

    const Pm4ShaderBinding& vsBind = Pm4_BoundShader(0);
    const Pm4ShaderBinding& psBind = Pm4_BoundShader(1);
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

    bool topologySupported = false;
    const VkPrimitiveTopology topology = XenosTopology(draw.primType, topologySupported);
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
    key.colorMask = regs[xenos::kRbColorMask] & 0xF;
    key.depthControl = regs[xenos::kRbDepthControl] & 0xFF;
    key.modeControl = regs[0x2208] & 7;
    key.primRestart = 0;

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
        for (uint32_t i = 0; i < 224 * 4; i++)
            dst[i] = regs[xenos::kAluConstantBase + 256 * 4 + i];
    }

    uint8_t* shared = R->arena.mapped + sharedAt;
    memset(shared, 0, kSharedSize);

    // Texture and sampler descriptor indices, one per sampler slot the pixel shader
    // declared. A slot the shader does not use is left at 0, which is the dummy — a
    // defined white texel rather than an unbound descriptor, because a shader that
    // samples an unbound descriptor is undefined behaviour even when the result is
    // discarded.
    for (uint32_t constIdx : ps.tfetchConsts)
    {
        if (constIdx >= 16)
            continue;
        const uint32_t slot = UploadTexture(base, regs, constIdx);
        reinterpret_cast<uint32_t*>(shared + kSharedTex2D)[constIdx] = slot;
        reinterpret_cast<uint32_t*>(shared + kSharedSampler)[constIdx] = 0;
    }
    for (uint32_t constIdx : vs.tfetchConsts)
    {
        if (constIdx >= 16)
            continue;
        const uint32_t slot = UploadTexture(base, regs, constIdx);
        reinterpret_cast<uint32_t*>(shared + kSharedTex2D)[constIdx] = slot;
        reinterpret_cast<uint32_t*>(shared + kSharedSampler)[constIdx] = 0;
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
    }
    memcpy(shared + kSharedPosScale, posScale, sizeof posScale);
    memcpy(shared + kSharedPosOffset, posOffset, sizeof posOffset);

    // Half-pixel offset: the Xbox 360 samples pixel centres at integers, desktop APIs
    // at half-integers. The shaders apply this themselves; the runtime just states it.
    const float halfPixel[2] = { -1.0f / float(R->targetWidth),
                                 -1.0f / float(R->targetHeight) };
    memcpy(shared + kSharedHalfPixelOffset, halfPixel, sizeof halfPixel);

    // The viewport itself. With the XY transform enabled the scale/offset ARE the
    // viewport; the y scale is negative in D3D convention, and taking its absolute
    // value here while leaving the sign to the clip-space fold is what keeps the two
    // conventions from cancelling each other out by accident.
    VkViewport viewport{};
    if (vte & 0x1)
    {
        viewport.x = xo - std::fabs(xs);
        viewport.y = yo - std::fabs(ys);
        viewport.width = 2.0f * std::fabs(xs);
        viewport.height = 2.0f * std::fabs(ys);
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
    if (viewport.width <= 0.0f || viewport.height <= 0.0f)
    {
        Count("draw: degenerate viewport");
        return;
    }

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = { R->color.width, R->color.height };

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

    // --- vertex streams -------------------------------------------------------------
    uint32_t binding = 0;
    bool streamsOk = true;
    for (const VertexAttribute& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue;
        const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, a.fetchSlot);
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

    // --- indices ---------------------------------------------------------------------
    if (draw.indexed)
    {
        const uint32_t indexBytes = draw.index32 ? 4 : 2;
        const uint64_t bytes = uint64_t(draw.indexCount) * indexBytes;
        if (!GuestRangeOk(draw.indexVa, bytes))
        {
            Count("draw: index buffer outside the physical arena");
            return;
        }
        const VkDeviceSize at = UploadStream(base, draw.indexVa, bytes, draw.indexEndian);
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
    ++R->drawsThisFrame;
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
    R->haveResolvedThisFrame = true;
    Count("resolve");

    // RB_COPY_CONTROL bits 8/9: clear colour / clear depth after the copy. This is the
    // title's own clear, and honouring it is what makes a persistent EDRAM target
    // correct rather than an accumulating smear.
    const bool clearColor = ((control >> 8) & 1) != 0;
    const bool clearDepth = ((control >> 9) & 1) != 0;
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

bool VkRenderer_Init()
{
    if (g_initTried)
        return g_active;
    g_initTried = true;

    if (!EnvOn("CZ_VKDRAW"))
    {
        fprintf(stderr, "[vk] renderer OFF (set CZ_VKDRAW=1 to enable it)\n");
        return false;
    }

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
    g_active = true;
    fprintf(stderr, "[vk] renderer UP: %ux%u target, %zu shaders\n", R->targetWidth,
            R->targetHeight, R->shaders.size());
    return true;
}

void VkRenderer_Draw(uint8_t* base, const Pm4Draw& draw)
{
    if (!g_active)
        return;
    const uint32_t* regs = Pm4_Registers();
    // The resolve discriminator, and the only one: RB_MODECONTROL's edram_mode.
    if ((regs[0x2208] & 7) == 6)
    {
        DoResolve(base, regs);
        return;
    }
    DoDraw(base, draw);
}

void VkRenderer_OnSwap(uint8_t* base, uint32_t frontBuffer, uint32_t width,
                       uint32_t height)
{
    (void)base;
    if (!g_active)
        return;
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
    Barrier(R->cmd, R->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { R->color.width, R->color.height, 1 };
    vkCmdCopyImageToBuffer(R->cmd, R->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           R->readback.buffer, 1, &copy);
    SubmitAndWait();

    const size_t bytes = size_t(R->color.width) * R->color.height * 4;
    if (R->presentPixels.size() < bytes)
        R->presentPixels.resize(bytes);
    memcpy(R->presentPixels.data(), R->readback.mapped, bytes);
    Host_PresentPixels(R->presentPixels.data(), R->color.width, R->color.height);

    // CZ_VK_FRAME_DUMP=<dir> writes every 64th frame as a PPM. This is the instrument
    // that makes the renderer checkable WITHOUT a window, which matters more than it
    // sounds: every other gate this project owns is a log diff, and "the picture is
    // right" is the one claim that needs an image. A headless run plus a directory of
    // frames is a self-servable version of the E-screenshot comparison.
    static const char* dumpDir = Env("CZ_VK_FRAME_DUMP");
    if (dumpDir && (R->frame % 64) == 0)
    {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dumpDir,
                 (unsigned long long)R->frame);
        if (FILE* f = fopen(path, "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", R->color.width, R->color.height);
            for (size_t i = 0; i < bytes; i += 4)
                fwrite(&R->presentPixels[i], 1, 3, f);
            fclose(f);
        }
    }

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

    static const uint64_t statsEvery =
        Env("CZ_VK_STATS") ? std::max(1L, strtol(Env("CZ_VK_STATS"), nullptr, 10)) : 0;
    if (statsEvery && (R->frame % statsEvery) == 0)
        VkRenderer_DumpStats();

    Count("swap: presented");
    R->haveResolvedThisFrame = false;
    (void)width;
    (void)height;
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
}

