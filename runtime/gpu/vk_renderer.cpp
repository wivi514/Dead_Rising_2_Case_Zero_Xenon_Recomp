#include "vk_renderer.h"

#include "pm4.h"
#include "pump_stats.h"
#include "drawid_ps_spv.h"
#include "xenos.h"
#include "../host/window.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

// The bindless heap's size, and the number this renderer ran out of.
//
// It was 4096 with the comment "the frontend uses a few dozen", which was true of
// every screen this port could reach at the time and stopped being true the moment it
// reached Still Creek. Slots are handed out monotonically and never recycled
// (`entry.slot = R->nextTextureSlot++`), so a long session in a texture-rich area
// fills the heap and every texture after that is served slot 0 — the 1x1 WHITE dummy
// — with only a counter to say so. `R->nextTextureSlot` read exactly 4096 out of a
// live game via `gdb -p ... print`, with white buildings, a white NPC, white road
// decals, white blood and white inventory icons on screen.
//
// The rule the operator's pictures establish is worth keeping, because it is not the
// obvious one: it is NOT "things that appear late go white", it is "anything needing
// a NEW SLOT after the heap filled goes white". This title streams textures BY
// DISTANCE, so walking toward a building requests a higher-resolution version — a new
// fetch constant, a new cache entry, a new slot — which is why EVERY building whitens
// on approach while its distant version stays correct.
//
// RAISING THIS IS A MITIGATION, NOT THE FIX, and the distinction is the point: a cap
// is only ever a bigger number, and this one now trades "textures silently turn white"
// for "the texture working set grows without bound". The real fix is recycling — an
// LRU over the texture cache with deferred destruction so an in-flight frame cannot
// lose its image — and it is on the list. What this does buy is a session long enough
// to find the NEXT defect, and a same-binary way to prove the causal chain end to end:
// if the buildings stop whitening when the cap goes up, the chain is confirmed.
//
// Sized from the DEVICE rather than from a new magic number, clamped to something
// sane, because "how many sampled images may a shader see" is a property of the host
// and not something to guess twice.
uint32_t g_maxDescriptors = 4096;   // replaced at init from the device's own limit

// --- diagnostics --------------------------------------------------------------------
// Every path that declines to do something increments one of these. The alternative —
// returning quietly — is what makes a renderer that draws 80% of a frame look exactly
// like one that draws all of it, and this project has already paid for that lesson in
// the command processor (gotcha 84: a parser that stops early must say so).
std::map<std::string, uint64_t> g_stats;
void Count(const char* name) { ++g_stats[name]; }

// ...and the same counters, reached without paying for them, for the handful of call
// sites that run on EVERY draw.
//
// `Count` constructs a `std::string` from the literal (a heap allocation for any name
// over 15 characters, and most of ours are) and walks a red-black tree comparing
// strings, per call. That is fine at a few hundred calls and it is not fine at the
// ~5 unconditional counters a draw times 6,600 draws a frame: it is instrumentation
// overhead inside the phase being instrumented, which is the same defect that made
// part 18's state cache first measure as a dead heat (`docs/perf-cpu-plan.md` §1a,
// hypothesis B — the one item there filed as needing no measurement to justify).
//
// A `std::map` node is stable for the life of the map and `g_stats` is never cleared,
// so the address of a counter can be resolved ONCE per call site and then incremented
// directly. `COUNT(literal)` does exactly that with a function-local static; the
// printing interface, the names and the ordering are untouched, so nothing downstream
// of `VkRenderer_DumpStats` can tell the difference.
//
// `Count` stays, and is still the right thing for every path that declines a draw:
// those run rarely, and a cold call site is not worth a static.
uint64_t* CounterSlot(const char* name) { return &g_stats[name]; }
#define COUNT(lit)                                                                     \
    do                                                                                 \
    {                                                                                  \
        static uint64_t* _czSlot = CounterSlot(lit);                                   \
        ++*_czSlot;                                                                    \
    } while (0)

// --- CZ_VK_PROFILE=N — where a FRAME's CPU time actually goes ------------------------
//
// Every frame-rate number this project owned before today divided a whole run's frames
// by its wall time, and every one of them was taken at the TITLE SCREEN. "Gameplay runs
// at 8-12 fps" was an operator's stopwatch, and the three suspects on the board (the
// synchronous submit, the per-frame readback, the per-draw constant upload) had never
// been separated — so any work on them would have been optimising whichever one came to
// mind first. This splits the frame into named phases and prints milliseconds.
//
// The phases are CPU wall time on the thread that records the frame, which is the right
// quantity while the renderer is CPU-bound and would be the wrong one if it were not.
// `submit` is the honest check on that: it is the wait for the GPU to finish, so a
// frame whose time is in `submit` is GPU-bound and the rest of this table is noise.
//
// It costs one clock read per phase entry and exit. At ~2,000 draws a frame that is a
// few thousand vDSO reads, well under a millisecond of a ~100 ms frame — but it is off
// by default anyway, because an instrument expensive enough to change the thing it
// measures reports its own overhead (gotcha 7).
struct ProfilePhases
{
    uint64_t constants = 0;   // the per-draw ALU constant copy into mapped memory
    uint64_t streams = 0;     // vertex/index stream copy + dword swap
    uint64_t textures = 0;    // texture untile + upload
    uint64_t record = 0;      // the vkCmd calls of a draw
    uint64_t submit = 0;      // vkQueueSubmit + the fence wait (i.e. the GPU)
    // ...and that split in two, because they are different subsystems wearing one
    // number. `submitCall` is the driver translating a command buffer of ~1,900 draws
    // on THIS cpu; `fenceWait` is the GPU actually executing it. 24 ms for that many
    // small draws on an RTX 3070 is far too slow to be fill, so which half it is
    // decides whether the next question is about barriers or about the host.
    uint64_t submitCall = 0;
    uint64_t fenceWait = 0;
    uint64_t readback = 0;    // image -> host buffer -> window
    // DoDraw's own untimed work — register decode, the pipeline-key build and its
    // lookup, the fetch-constant walk, and the always-on censuses. EXCLUSIVE of every
    // phase above, which it was not until part 20; see the ProfScope comment.
    uint64_t drawOther = 0;
    uint64_t draws = 0;       // how many draws those numbers are spread over
    // Pipeline creation, which lives INSIDE `drawOther` and is the only thing in there
    // that costs milliseconds. Separated because a first-visit stutter and a per-draw
    // overhead are different defects that were sharing one column. See GetPipeline.
    uint64_t pipelineNs = 0;
    uint64_t pipelinesCreated = 0;
};
ProfilePhases g_prof;
bool g_profileOn = false;

inline uint64_t ProfNow()
{
    return g_profileOn ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count())
                       : 0;
}
// Scoped accumulator, EXCLUSIVE of any scope nested inside it. Compiles to nothing
// measurable when the profile is off, because every call short-circuits on one
// already-hot bool.
//
// THE EXCLUSIVITY IS A PART-20 BUG FIX, and it silently re-ordered this project's own
// performance plan. These scopes nest: `record` opens partway down DoDraw and lives to
// the end of it, so the three `UploadStream` calls below it ran INSIDE `record` and
// their `streams` time was counted twice — once in `streams` and once in `record`.
// `submit` encloses `submitCall` and `fenceWait` the same way. The print then computed
// DoDraw's residual as `drawTotal - (constants + streams + textures + record)`, which
// subtracts `streams` twice, so the residual came out that much too small.
//
// The arithmetic is not subtle once written down, but the effect was: it made `record`
// look like the draw path's dominant term and the residual look like its smallest, and
// `docs/perf-cpu-plan.md` was written on those numbers — filing the residual as "the
// cheapest item in this document".
//
// The mechanism is worth stating generally, because a profiler is exactly the kind of
// code whose defects are invisible: a nested scope moves time from the OUTER scope's
// residual into the inner scope's name, and both columns still add up to the same
// total. Nothing looks wrong. The fix is for every scope to subtract what its children
// took, which makes each column mean "time in THIS phase and no other" — and then the
// columns and the total become independent statements that can disagree.
//
// `g_profileOn` is set once at init, before any draw, and never changes; the ctor and
// dtor would otherwise have to agree about a flag that moved between them.
struct ProfScope
{
    uint64_t* sink;
    uint64_t t0 = 0;
    uint64_t childNs = 0;   // what scopes opened inside this one consumed
    ProfScope* parent = nullptr;
    static thread_local ProfScope* current;

    explicit ProfScope(uint64_t* s) : sink(s)
    {
        if (!g_profileOn)
            return;
        t0 = ProfNow();
        parent = current;
        current = this;
    }
    ~ProfScope()
    {
        if (!g_profileOn)
            return;
        const uint64_t total = ProfNow() - t0;
        *sink += total - childNs;
        if (parent)
            parent->childNs += total;
        current = parent;
    }
};
thread_local ProfScope* ProfScope::current = nullptr;

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

// --- CZ_VK_TEX_GUARD / CZ_VK_TEX_REVALIDATE ------------------------------------------
//
// THE OPERATOR'S REPORT THIS EXISTS FOR: "almost all the textures in the game are wrong
// and got the texture of something else — a building getting the repeated texture of a
// moose head item". That is not a scaling defect and not the bindless heap running out
// (which serves the white dummy); it is object A being drawn with object B's PIXELS,
// which means the cache handed out an image that no longer belongs to that fetch.
//
// The mechanism the code already half-admits, in the CZ_VK_TEX_REFRESH comment: the
// texture cache is keyed on the fetch constant's six dwords and is NEVER INVALIDATED.
// Those dwords are a descriptor — address, extent, format, tiling, swizzle, pitch — and
// this title STREAMS ITS TEXTURES BY DISTANCE (open-items 3z), so it is constantly
// loading a new texture into a heap address a previous one has been freed from. When
// the new occupant has the same extent and format as the old one, every dword matches,
// the key matches, and the draw gets the old image forever.
//
// `CZ_VK_TEX_REFRESH` was built for one instance of this (a font atlas the CPU keeps
// writing) and it takes an ADDRESS, so it could only ever answer about a texture
// somebody had already identified. The question here is a census over all of them.
//
// GUARD is the measurement: on a cache HIT, hash a bounded sample of the guest bytes the
// entry was uploaded from and compare it with the hash taken at upload. A mismatch is
// the cache serving pixels the guest has since replaced. Counted globally and per
// address, so the answer is a number and a list rather than an impression.
//
// REVALIDATE is the repair the measurement would justify: on a mismatch, re-upload into
// the SAME image and the SAME bindless slot — which is exact and allocation-free,
// because the dimensions are part of the key. Kept behind its own switch and OFF until
// the census says the mismatch is real, because a per-fetch re-upload is expensive and
// "it looked better" is not a reason to ship one.
//
// POISON is the control (gotcha 30). It folds the frame number into the guard so every
// hit MUST mismatch: a census that cannot report 100% has not been shown capable of
// reporting anything, and this project has shipped a comparison that only ever read
// 100% before (gotcha 234) — so both ends need exercising.
bool g_texGuard = false;
bool g_texRevalidate = false;
bool g_texGuardPoison = false;
struct TexGuardStats
{
    uint64_t hits = 0;        // cache hits the guard was computed for
    uint64_t changed = 0;     // ...whose guest bytes had changed since upload
    uint64_t reuploaded = 0;  // ...and were re-uploaded (REVALIDATE only)
    uint64_t guardBytes = 0;  // what the guard itself read, i.e. its own cost
} g_texGuardStats;
// Per address, because "17% of hits are stale" and "one atlas is stale every frame" are
// completely different defects and a single ratio cannot tell them apart.
struct TexGuardAddr
{
    uint64_t hits = 0, changed = 0;
    uint32_t width = 0, height = 0, format = 0;
};
std::map<uint32_t, TexGuardAddr> g_texGuardAddrs;

// CZ_VK_DIM_CENSUS=1 — WHERE IS THE DIMENSION IN THE TEXTURE FETCH CONSTANT?
//
// The renderer needs the dimension twice over and from two different places. The SHADER
// says which descriptor heap a slot is sampled from (its fetch instruction picks
// `tfetch2D` or `tfetchCube`), and part 25 put that in the sidecar. But the GUEST has to
// say how the image is laid out in memory — a cube map is six faces, and reading one is
// reading a sixth of it — and that can only come from the fetch constant, whose
// `dimension` field this renderer never decoded at all: `DecodeTextureFetch` hardcoded
// `t.dimension = 1`.
//
// The bit position is not something to remember. It is something to MEASURE, and this
// title provides the oracle for free: the shader-declared dimension partitions every
// fetch into two classes that must differ in exactly the bits of that field. So for each
// class this accumulates the AND and the OR of all six dwords across every fetch. A bit
// that is set in every fetch of a class has AND=1; one clear in every fetch has OR=0.
// The field is where the two classes' always-set / always-clear patterns disagree — and
// if no two-bit field disagrees cleanly, the dimension is not in the fetch constant here
// and the shader is the only source, which is an answer too.
//
// It also censuses dword2's top six bits, which Xenia's layout says is the stack DEPTH
// for a stacked or cube surface: the prediction is 5 (six faces, stored minus one) for
// every cube fetch and 0 for every 2D one. A prediction stated before the run, so the
// run can refute it (the project's own evidence rule).
bool g_dimCensus = false;
struct DimClass
{
    uint64_t fetches = 0;
    uint32_t andMask[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
    uint32_t orMask[6] = {};
    std::map<uint32_t, uint64_t> d2Top;   // dword2 >> 26 -> count
};
std::map<uint32_t, DimClass> g_dimClasses;  // shader-declared dimension -> the census

// CZ_VK_DIM_DISAGREE=N — PRINT THE FIRST N SHADER-VERSUS-CONSTANT DISAGREEMENTS IN FULL.
//
// The cross-check in `UploadTexture` says a disagreement HAPPENED — ~14,670 cube fetches
// a run declined to the white dummy because the shader indexes the cube array while the
// fetch constant describes a 2D surface. It cannot say WHY, and the round-2 captures
// turned that from a curiosity into a defect with a known victim: over the gas-station
// frame, 414 of 414 cube-declared draws on HARDWARE read stack depth 5 and dimension 3,
// with no disagreement at all (docs/open-items.md 00g). So the disagreement is ours, and
// it is the confirmed mechanism behind the white glass and the blown-out bathroom window.
//
// Two candidate causes, and they need different fixes: our register file has LOST a
// constant the guest set (so the slot holds something else — stale, or another slot's
// texture), or our dimension DECODE misreads a case the capture does not contain. This
// prints what separates them: the six raw dwords of the offending slot, and then the
// whole 32-slot fetch-constant file as we hold it at that draw, so an off-by-one in the
// slot index or a stale neighbour is visible rather than inferred.
// The one-liners are capped, because the useful part is the CENSUS underneath them: which
// shaders disagree, at which slot, about which texture. The first version printed only
// the first 25 and every one of them was the same shader at the same slot on two frames —
// which reads as "there is one case" and is equally consistent with "the cap was reached
// inside one draw batch". A capped log line is not a count (gotcha 109), so the census is
// unbounded and prints at shutdown.
bool g_dimDisagree = false;
int g_dimDisagreeLeft = 0;
struct DimDisagree
{
    uint64_t psHash = 0, vsHash = 0;
    uint32_t slot = 0, shaderDim = 0, constDim = 0, addr = 0, w = 0, h = 0, fmt = 0;
    uint64_t fetches = 0;
};
std::map<uint64_t, DimDisagree> g_dimDisagreements;   // keyed on shader+slot+address

// CZ_VK_STREAM_CENSUS=1|2 — what the per-frame vertex/index stream cache actually does.
//
// `streams` is the largest draw-path term in a real crowd (12.3-14.3% of a frame, twice
// what the headless recipe shows), and the plan could not say what to do about it,
// because two opposite readings fit the same millisecond count:
//
//   * nearly all HITS — then the cost is the lookup and the fix is a cheaper key;
//   * mostly MISSES — then it is real copying and the fix is a different cache LIFETIME.
//
// No amount of reading the code decides that, so this counts it. One thing the code DOES
// decide: `ProfScope(streams)` wraps only the `CopySwapped`, so the `streams` column is
// copy time and a hit costs it nothing — a hit's lookup is charged to `other`.
//
// Level 2 additionally answers the question a cross-frame cache stands or falls on:
// whether the bytes at a repeated (address, size, endian) are the SAME bytes next frame.
// A persistent cache keyed on the address is wrong if the guest rewrites the buffer in
// place, and "the key repeats" is not evidence that "the content repeats". Level 2 hashes
// every stream, which costs about as much as the copy — it is a diagnostic run, never a
// frame-time measurement (gotcha 223).
int g_streamCensus = 0;
struct StreamCensus
{
    uint64_t hits = 0, misses = 0;      // per-draw cache outcomes
    uint64_t bytesCopied = 0;           // what the misses actually swapped
    uint64_t bytesHit = 0;              // what the hits avoided swapping
    // Of the misses, the ones whose key was present in the PREVIOUS frame — i.e. what a
    // cache that survived the frame boundary would have hit instead.
    uint64_t prevFrameKeyHits = 0;
    uint64_t prevFrameKeyBytes = 0;
    // ...and of THOSE, the ones whose bytes were unchanged since last frame (level 2).
    // The gap between this and the line above is the guest rewriting a buffer in place,
    // which is exactly the traffic a persistent cache would serve stale.
    uint64_t prevFrameSameContent = 0;
    uint64_t prevFrameSameBytes = 0;
    // Of the streams the full hash says DID change, the ones the cross-frame store served
    // from its own copy anyway — i.e. the ones its bounded-cost guard missed. This is the
    // correctness measurement for the whole persistent cache and it must read zero.
    uint64_t guardMissed = 0;
    // Copied bytes and misses split by what the stream IS: [0] declared vertex binding,
    // [1] index buffer, [2] shader-side dependent fetch.
    uint64_t kindBytes[3] = { 0, 0, 0 };
    uint64_t kindMisses[3] = { 0, 0, 0 };
    uint64_t kindRepeatBytes[3] = { 0, 0, 0 };
};
StreamCensus g_streamCensus_c;

// WHICH streams change in place, not just how many. Level 2 only, cumulative over the
// whole run rather than per profile window.
//
// Part 21 established that 164 of 10,154,820 repeated keys really do get rewritten by the
// guest — 0.0016%, and it described the changing set as "a recurring ~26". That number
// decides how a persistent cache invalidates, and the two answers cost wildly different
// amounts of work: if the rewritten streams are a NAMED set — one address range, or one
// `kind` — then invalidation is an exclusion rule and costs nothing, while if they are
// scattered across the geometry it has to be guest-page write tracking (`mprotect` plus a
// `SIGSEGV` handler sharing the process with `cpu/crash_report.cpp`'s). The census
// already detects each mismatch; it just threw the identity away. This keeps it.
//
// Cumulative on purpose: the claim under test is that the same keys recur, and a counter
// reset every five seconds cannot show recurrence. Unbounded in principle, bounded in
// practice by the finding it is testing — and if it is NOT bounded, that is the finding.
struct StreamChange
{
    uint64_t times = 0;   // how many frames this key was seen rewritten
    uint64_t bytes = 0;   // its size, which is part of the key and so constant
    int kind = 0;         // 0 vertex binding, 1 index buffer, 2 dependent fetch
    uint64_t firstFrame = 0, lastFrame = 0;
};
std::unordered_map<uint64_t, StreamChange> g_streamChanged;

// Last frame's keys, and (level 2 only) a content hash for each. Rebuilt in BeginFrame
// out of the cache that is about to be cleared, so the draw path never walks it.
std::unordered_map<uint64_t, uint64_t> g_prevStreamKeys;
// This frame's hashes, level 2 only. Separate from `streamCache` because that map is on
// the hot path and must not grow a field an instrument is the only reader of.
std::unordered_map<uint64_t, uint64_t> g_streamHashes;

// CZ_VK_STREAM_CENSUS_POISON=1 — THE CONTROL ARM FOR THE CONTENT CHECK, and it exists
// because that check reports 100.0% and nothing else.
//
// The level-2 line says "of the streams whose key repeated last frame, N of N had
// identical bytes". On this title it reads 149,925 of 149,925 in a crowd, which is either
// a real and very useful fact about the guest's geometry or a comparison that cannot
// fail. Those are indistinguishable from the output. With this on, the hash is salted
// with the FRAME NUMBER, so the same bytes hash differently in consecutive frames and
// the line MUST read 0.0%. If it still reads 100%, the instrument is broken and the
// finding built on it is worthless (gotchas 30, 94, 158).
bool g_streamPoison = false;

// FNV-1a over a stream's GUEST bytes. Level 2 only, and deliberately over the source
// rather than the arena copy: the question is whether the guest rewrote the buffer, and
// the arena copy is our own output.
uint64_t StreamHash(const uint8_t* p, size_t bytes, uint64_t salt)
{
    uint64_t h = 1469598103934665603ull ^ salt;
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// THE GUARD: a bounded-cost fingerprint of a stream's guest bytes, and the thing that
// makes a cross-frame cache a cache rather than an assumption.
//
// The problem it solves is measured, not hypothetical. 30 distinct streams a run really
// are rewritten in place by the guest at an address a previous frame already cached, and
// serving the old copy draws the wrong mesh. Detecting that by hashing the whole stream
// costs about what the copy costs, which would give the saving straight back.
//
// So the cost is capped at `kGuardBytes` regardless of stream size:
//
//   * a stream of `kGuardBytes` or fewer is hashed IN FULL, so the check is exact for it;
//   * a larger one is hashed at eight evenly spread blocks, always including the first
//     and the last, so a rewrite has to miss all eight windows to go unnoticed.
//
// The threshold is not arbitrary. Every rewritten stream this title has ever been seen
// to produce is EXACTLY 80 bytes (30 of them, in two narrow guest ranges, all declared
// vertex bindings — see the census's REWRITTEN IN PLACE line), so 512 puts the entire
// observed population on the exact branch with a 6x margin. But "we have never seen a
// big one change" is a zero, and a zero is a detection failure until something could
// have detected it (gotcha 3) — hence the sampled branch rather than a size cutoff that
// simply declines to check. `CZ_VK_STREAM_CENSUS=2` measures what the sampling misses:
// it computes the FULL hash as well and counts every change the guard did not catch.
//
// Cost in a crowd frame: ~2,000 first-touch streams x <=512 B = under 1 MB, against the
// 61-77 MB of copying it is there to avoid.
// THE EXACT BOUND, AND WHY IT IS NO LONGER 512.
//
// 512 was fitted to a census that found every rewritten stream was exactly 80 bytes —
// but that census could only see streams rewritten between two consecutive frames in a
// recipe that never changed a HUD number, so the bound was fitted to the population the
// instrument could reach (gotcha 235, second instance). The streams it could not see
// are the ones that broke: a HUD is batched into one multi-KB vertex buffer in which
// only the digit quads change, those quads fall outside the 8x64 sampled windows, and
// the store serves the previous frame's numbers. That is open item 00c, and the
// operator confirmed the exact guard fixes it outright across several minutes and two
// weapons.
//
// Exact-everywhere is not the ship-able form. Measured on the outdoor recipe:
//
//   guard read   0.41 MB/frame -> 30.70 MB/frame   (75x)
//   `record`     4.8% -> 16.7% of frame            (+11.9 points, ~3.8 ms)
//
// So the bound is raised rather than removed: hash EXACTLY up to kGuardBytes, sample
// above it. Cost is bounded by (streams/frame x kGuardBytes) — about 1,000 x 16 KB
// worst case here, against the 26-30 MB/frame of copying the store avoids.
//
// 16 KB is deliberately generous rather than tuned. The HUD buffer's true size has not
// been measured, and the failure mode of a bound that is too small is the defect coming
// back silently, while the failure mode of one too large is frame time this title's
// vblank floor absorbed entirely at 2,000 draws (32.0 ms on BOTH arms). Given that
// asymmetry, guess high. `CZ_VK_STREAM_GUARD_BYTES=N` overrides it without a rebuild so
// the bound can be tuned against the defect instead of against a model of it, and
// `CZ_VK_STREAM_GUARD_EXACT=1` still means unlimited.
//
// NOT MEASURED: the crowd. The A/B above parked at ~2,000 draws; the recipe peaks near
// 8,300-8,700 and there is no profile window at that depth. A crowd frame moves 61-77 MB
// of stream bytes, so if anything is going to hurt it is there — measure before calling
// this free.
constexpr size_t kGuardBytesDefault = 16384;
size_t g_guardBytes = kGuardBytesDefault;
// Streams that exceeded the bound and were therefore only SAMPLED, cumulative.
uint64_t g_guardSampled = 0;
constexpr size_t kGuardBlocks = 8;

// EIGHT BYTES A STEP, not one, and the reason is the dependency chain rather than the
// memory. FNV-1a is `h = (h ^ byte) * prime`, so every byte waits on the previous byte's
// MULTIPLY — about four cycles each, serial, no matter how fast the loads are. At 512
// bytes that is ~0.55 us per stream and ~1 ms per crowd frame, a fifth of what the store
// saves, spent inside the check that makes the store safe. Folding a whole uint64 per
// step keeps the mixing (xor then multiply by an odd constant is injective in the input
// word either way) and cuts the chain to 64 steps.
inline uint64_t GuardFold(uint64_t h, const uint8_t* p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t v;
        memcpy(&v, p + i, 8);   // unaligned-safe and compiles to one load
        h = (h ^ v) * 1099511628211ull;
    }
    for (; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// CZ_VK_STREAM_GUARD_EXACT=1 — hash EVERY byte, whatever the stream's size.
//
// The sampling below is exact only up to 512 bytes; above that it reads 8 blocks of 64
// and can therefore miss a small edit in a large stream. That is precisely the shape of
// the HUD defect in open item 00c: a UI vertex buffer of a few KB in which only the two
// quads carrying the ammo digits change, and 512 sampled bytes that never land on them.
// A missed change means the guard calls the stream unchanged and the store serves the
// PREVIOUS frame's buffer — the old number — which is what "flickers between 26 and 27
// regardless of the real ammo" looks like when the guest double-buffers its UI.
//
// This is the arm that separates "the store is guilty" from "the store's GUARD is
// guilty", which `CZ_VK_NO_PERSIST_STREAMS=1` cannot: that one disables the store
// wholesale and costs 4.7 ms of a crowd frame, so it can never be the fix even when it
// makes the symptom go away. If the symptom goes away HERE, the fix is a better guard
// for UI-sized streams and the 4.7 ms stays bought.
//
// Not the default, because the cost is unbounded in stream size and unmeasured on a
// crowd frame — establish the picture first, then decide what it is worth.
bool g_guardExact = false;

uint64_t StreamGuard(const uint8_t* p, size_t bytes, size_t* readOut)
{
    if (g_guardExact)
    {
        if (readOut)
            *readOut += bytes;
        return GuardFold((1469598103934665603ull ^ bytes) * 1099511628211ull, p, bytes);
    }
    // The size is folded in first. Without it a stream that shrinks to a prefix of
    // itself would hash the same, and size is part of the key only for the streams the
    // key came from — a re-copy into the same slot keeps the slot's size.
    uint64_t h = (1469598103934665603ull ^ bytes) * 1099511628211ull;
    if (bytes <= g_guardBytes)
    {
        if (readOut)
            *readOut += bytes;
        return GuardFold(h, p, bytes);
    }
    // Above the bound the guard is a SAMPLE and can therefore miss a small edit. Count
    // the exposure rather than leaving it silent: this is the population that item 00c's
    // defect lived in, and a bound raised until this counter is zero for the streams that
    // matter is a bound chosen by measurement instead of by guess.
    ++g_guardSampled;
    const size_t block = g_guardBytes / kGuardBlocks;
    // Eight starts spread over [0, bytes - block], the first exactly at 0 and the last
    // exactly at the end. `bytes > kGuardBytes` guarantees the span is positive, and the
    // last block therefore ends on the final byte rather than near it.
    const size_t span = bytes - block;
    for (size_t b = 0; b < kGuardBlocks; ++b)
        h = GuardFold(h, p + (span * b) / (kGuardBlocks - 1), block);
    if (readOut)
        *readOut += g_guardBytes;
    return h;
}

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
// The pose capture's player half — defined in cpu/debug_tunables.cpp, which owns the
// guest pointer and guest memory access. Declared rather than headered because that
// translation unit has no header and one function does not justify inventing one.
extern "C" uint32_t CZ_DebugWritePlayerObject(FILE* f, uint32_t bytes);
// The player's world position, read through the title's own `getplayerinfo` path on a
// GUEST thread and cached; this returns the cache plus its age, because the render
// thread must not make guest calls. Age travels with the value so a stale one cannot
// pass for fresh.
extern "C" int CZ_DebugPlayerPos(float out[3], long long* ageMs);

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
        //
        // R32_SFLOAT, not R32_UINT, because of what the SHADER-side input is typed as.
        // This title wraps every packed normal in a TEXCOORD usage, whose input variable
        // is float4; binding R32_UINT against that is a pipeline type mismatch
        // (VUID-VkGraphicsPipelineCreateInfo-Input-08733, ten pipelines on the outdoor
        // route) whose practical effect was the packed dword's bits read AS a float —
        // NaN whenever bits 30..23 are all ones. Those NaNs, laundered by the tone
        // epilogue's max/saturate into exactly rgb(180,180,180), were the white-surface
        // plateau (open-items 00f). An SFLOAT attribute is a plain 32-bit load, the bits
        // arrive intact, and the emitter's XeUnpack_10_11_11 recovers them with asuint —
        // see XenosRecomp shader_common.h. If a title ever declares fmt16 under a uint4
        // usage (Fable 2 wraps normals as NORMAL), that path wants R32_UINT again.
        case 16: return VK_FORMAT_R32_SFLOAT;
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
    // PARALLEL to tfetchConsts: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube, and it decides which
    // of the four descriptor-index arrays in the shared constants this slot's index has
    // to be published into. Empty when the sidecar predates part 25, which the binder
    // counts rather than papering over — see the note at bindTextures.
    std::vector<uint32_t> tfetchDims;
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
    meta.tfetchDims = JsonIntArray(text, "tfetchDims");
    // A sidecar written before part 25 has no dimensions at all, and one whose arrays
    // disagree in length is a build-pipeline defect rather than something to index into.
    // Both cases are dropped to "no dimension information" here, ONCE, so the binder's
    // fallback is reached deliberately and the message names the shader.
    if (!meta.tfetchDims.empty() && meta.tfetchDims.size() != meta.tfetchConsts.size())
    {
        fprintf(stderr,
                "[vk] %s: tfetchDims has %zu entries for %zu tfetchConsts — the arrays "
                "are POSITIONAL; ignoring the dimensions and treating every slot as 2D\n",
                path.filename().string().c_str(), meta.tfetchDims.size(),
                meta.tfetchConsts.size());
        meta.tfetchDims.clear();
    }

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
    // RB_COLORCONTROL's alpha test, as a pipeline dimension because the generated
    // shaders implement it behind a SPECIALIZATION constant (SPEC_CONSTANT_ALPHA_TEST
    // -> clip(oC0.w - g_AlphaThreshold)), and a spec constant is baked at pipeline
    // creation. 1 = the clip is compiled in. The THRESHOLD stays per-draw (shared
    // constants +272), so one pipeline serves every ref value.
    uint32_t alphaTest;
    // THE DRAW-ID PASS (part 39). 1 = this draw's fragment stage is replaced by
    // drawid_ps.hlsl, which writes the draw's own index instead of its colour. It is a
    // pipeline dimension for the same reason alphaTest is: the fragment module and the
    // blend state are baked at creation. Off by default and on for exactly one frame.
    uint32_t drawIdPass;

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

// Where UploadStream put a stream's bytes. Two buffers are now possible — the per-frame
// arena and the cross-frame store — and every consumer needs the handle, the device
// address and the host pointer, so returning a bare offset no longer says enough.
// `buf == nullptr` is the failure return that `VkDeviceSize(-1)` used to be.
struct StreamLoc
{
    Buffer* buf = nullptr;
    VkDeviceSize at = 0;
    bool ok() const { return buf != nullptr; }
    VkBuffer handle() const { return buf->buffer; }
    uint8_t* bytes() const { return buf->mapped + at; }
    VkDeviceAddress address() const { return buf->address + at; }
    VkDeviceSize capacity() const { return buf->size; }
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    // ARRAY LAYERS, and it is here because `Barrier` needs it. A barrier's
    // subresourceRange had `layerCount = 1` hardcoded, which is correct for every image
    // this renderer had until cube maps arrived and then silently wrong: the five faces
    // past layer 0 would never leave TRANSFER_DST, so the sampler would read them in the
    // wrong layout. That is undefined behaviour whose most likely presentation is a cube
    // with one correct face, which reads as a decode bug rather than a barrier one.
    uint32_t layers = 1;
    // MIP LEVELS, here for exactly the reason `layers` is: `Barrier` names a
    // subresource RANGE, and a range that stops at level 0 leaves every level below it
    // in TRANSFER_DST while the sampler reads it. The presentation of that would be a
    // texture that is correct until the camera backs away from it, which reads as an
    // LOD bug rather than a barrier one — the same trap cube maps set in part 25.
    uint32_t levels = 1;
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
    // WHERE the pixels came from, and WHAT THEY WERE. The cache key is the fetch
    // constant's six dwords — a DESCRIPTOR — and this is the CONTENT that descriptor
    // pointed at when the image was uploaded.
    //
    // Those are not the same thing, and the gap between them is a whole class of
    // rendering defect: a title that streams textures reuses a heap address, so a
    // building's texture can arrive at the address a dropped item's texture used, with
    // the same width, height and format. Every dword of the fetch constant is then
    // identical, the cache says "same texture", and the draw is handed the previous
    // occupant's image. Keeping the source range and a content guard is what lets the
    // cache ask the second question instead of assuming the answer.
    uint32_t va = 0;
    uint64_t srcBytes = 0;
    uint64_t guard = 0;
    // Six for a cube map, one for everything else. Needed on the REFRESH path, which
    // re-copies into the same image and would otherwise write face 0 and leave the
    // other five holding their first-upload pixels.
    uint32_t layers = 1;
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
//
// A resolve's SOURCE is RB_COPY_CONTROL's low three bits — 0..3 name a colour target
// and 4 names the DEPTH buffer — and 18.4% of this title's resolves are depth ones
// (10,448 of 56,925 in B1, `tools/xtr_resolve_census.py`). They are its shadow
// cascades and the scene depth its depth-of-field pass reads back, so a snapshot taken
// from the colour target for those is not an approximation: it is a different picture
// entirely, handed to a pass that then computes a circle of confusion out of it.
// Set in a snapshot map key to mean "this is the DEPTH resolve to that address, not
// the colour one". A guest address is above 0x1FFFFFFF nowhere in this runtime, so
// bit 31 is free for the purpose and the key stays one integer.
constexpr uint32_t kSnapshotDepthBit = 0x80000000u;

// The largest resolve destination we will build a snapshot image for. This title's
// biggest is its shadow cascade at 4096x1024; the cap exists so a garbage
// RB_COPY_DEST_PITCH cannot ask for a terabyte, and it has its own counter so hitting
// it is visible rather than silent.
constexpr uint32_t kMaxSurfaceExtent = 4096;

// The EDRAM stand-in's size, which is NOT the presented frame's size and was the same
// number for five phases. The scene is 1280x720, but the shadow pass renders a
// 1024x1024 cascade — so with a 720-row target every cascade lost its bottom 304 rows
// before anything downstream had a chance to sample them. Heights are what this title
// needs more of; the width stays at the presentation width because no pass here renders
// wider than the screen.
constexpr uint32_t kEdramHeight = 1024;

// A right-sized copy of a snapshot's top-left corner.
//
// A resolve snapshot is created at the destination surface's PITCH, because
// RB_COPY_DEST_PITCH is what the resolve registers carry and its low field IS the pitch.
// The fetch that later samples that address declares the surface's REAL width, and a
// sampler normalises over the image it is handed — so whenever pitch != width every
// texture coordinate is scaled by width/pitch and everything past that fraction reads
// the padding, which is zero.
//
// It is invisible while both are multiples of 32, which is the entire scene chain
// (1280, 640, 320, 160), and it destroys the tail of this title's luminance reduction,
// whose surfaces are 80, 40, 20, 10, 5 and 2 wide in pitches of 96, 64, 32, 32, 32, 32.
// Measured lit-column counts of five consecutive links fit that model exactly, and the
// last link — the 2x1 scene-average luminance the tone map reads — came out EMPTY.
// docs/phase5-notes.md §6ao.
//
// The height needs no such treatment: RB_COPY_DEST_PITCH's high field is the real
// height, and the snapshots are already the right height everywhere.
struct SnapshotView
{
    Image image;
    uint32_t slot = 0;
};

struct Snapshot
{
    Image image;
    uint32_t slot = 0;   // bindless heap index, so a fetch can be served without a copy
    uint64_t frameSeen = 0;
    // Keyed (width << 16) | height. Refreshed from `image` by whatever resolve next
    // writes it, in that resolve's own command buffer — so a view never costs a submit
    // after the one that creates it, and it is never staler than its source.
    std::unordered_map<uint32_t, SnapshotView> views;
    // Which buffer this snapshot was taken FROM. It is part of the identity because
    // one guest address can be the destination of both a colour resolve and a depth
    // one (B1's 1812F000 is: 890 depth, 852 colour), and the two need different image
    // formats — so a change of source has to rebuild the image, exactly as a change of
    // extent does.
    bool fromDepth = false;
};

// A cube map the TITLE RENDERS ITSELF: six resolve snapshots assembled into the six
// layers of one `VK_IMAGE_VIEW_TYPE_CUBE` image in descriptor set 2.
//
// WHY THIS IS ITS OWN TYPE and not a Snapshot with six layers. A Snapshot is created by
// a resolve, keyed on that resolve's destination address, and lives in set 0 where its
// slot means "the 2D texture at this address". A rendered cube is SIX of those addresses
// standing for ONE texture in a different descriptor heap, and nothing about the resolve
// says which — the guest only says so later, when a fetch constant names the base
// address with a shader that declares a cube. So the six snapshots stay exactly as they
// are (other passes sample them as 2D surfaces, and do) and this is a second view of
// them, assembled on the first cube fetch and refreshed by each face's own resolve.
//
// It is 55% of all cube sampling in this game's opening hour — 409,911 of 746,355
// cube-declared draws in part 25's census — and until this existed every one of them
// read the 1x1 white dummy, in BOTH arms of every A/B, because guest memory at a resolve
// destination is whatever the allocator left there and for `06805000` that is zeros.
struct CubeSnapshot
{
    Image image;               // six layers, CUBE view, registered in set 2
    uint32_t slot = 0;         // index into set 2's heap, NOT set 0's
    uint32_t faceExtent = 0;   // one face is faceExtent x faceExtent
    uint32_t faceStride = 0;   // guest bytes between one face's base and the next
    // Which faces have ever been copied in, as a bitmask. A COUNTER, because "the cube
    // is bound" and "the cube has six faces in it" are different claims and the second
    // is the one a reflection depends on — five faces and a stale sixth is a picture
    // defect with no other symptom (gotcha 151).
    uint32_t facesFilled = 0;
    uint64_t frameSeen = 0;
};

// --- one frame's worth of resources, so the CPU can record frame N+1 while the GPU is
// --- still executing frame N ---------------------------------------------------------
//
// Everything in here is something the CPU WRITES and the GPU READS, or the reverse.
// Nothing else needs duplicating: the EDRAM stand-in, the snapshots and the textures are
// only ever touched by the device, and a single queue executes submissions in order, so
// the barriers already in the recorder cover every GPU-to-GPU hazard across the frame
// boundary. It is the host-visible things — the command buffer being recorded, the bump
// arena the draws' vertices and constants live in, and the buffer the presented image is
// read back into — that would otherwise be rewritten under a frame still in flight.
//
// The per-frame METADATA is here for a reason that is easy to miss and would have made
// every A/B this change is measured with wrong. The present-side instruments (frame
// stats, the PPM dump, the uniform-colour counter) read `presentPixels` and label it with
// `R->frame`, `R->drawFingerprint` and the draw count — which with a deferred present
// describe the frame being RECORDED, not the pixels being looked at. `frame_compare.py`
// aligns two runs by exactly those fingerprints, so an off-by-one here does not look like
// a bug, it looks like a picture regression. Captured at submit, read at present.
struct FrameSlot
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer present;             // the presented image, read back for the window
    bool inFlight = false;      // has been submitted and not yet waited on
    bool presentable = false;   // ...and had a real image copied into it

    uint64_t frame = 0;
    uint64_t draws = 0;
    uint64_t vertices = 0;
    uint64_t drawFingerprint = 0;
    uint64_t cameraFingerprint = 0;
    uint32_t width = 0, height = 0;
    size_t bytes = 0;
};
// Two is the whole design: the CPU records one frame while the GPU executes one. Deeper
// pipelining buys nothing here and costs a whole arena each — the GPU is 16.5 ms against
// the CPU's 27.7, so one frame of overlap already hides all of it (§6ar).
constexpr uint32_t kMaxFramesInFlight = 3;

struct Renderer
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // Null unless CZ_VK_VALIDATION=1 brought VK_EXT_debug_utils in with the layer. See
    // NameObject: it is what makes a validation message name one of OUR objects.
    PFN_vkSetDebugUtilsObjectNameEXT setObjectName = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    // The slot the frame currently being recorded belongs to, and that slot's command
    // buffer and fence hoisted out so the ~30 `R->cmd` call sites do not each have to
    // know the ring exists. Assigned in BeginFrame and nowhere else.
    FrameSlot frames[kMaxFramesInFlight];
    uint32_t frameSlot = 0;
    uint32_t framesInFlight = 1;
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
    // With frames in flight the arena is cut into `framesInFlight` equal regions and a
    // frame bumps inside its own. A region rather than a second buffer, deliberately:
    // `GrowArenaIfNeeded`'s buffer swap, the exhaustion path and every device address
    // the draws record all stay exactly as they were, and the only line that changes is
    // where the cursor starts and stops. The buffer is allocated `framesInFlight` times
    // larger at startup so a frame's own capacity — the number that decided the
    // whole-frame black (§6ap) — is unchanged between the arms.
    Buffer arena;
    VkDeviceSize arenaCursor = 0;
    VkDeviceSize arenaBase = 0;    // this slot's region start
    VkDeviceSize arenaLimit = 0;   // ...and its end
    VkDeviceSize arenaHighWater = 0;
    // Set by ArenaAlloc when it has to refuse a draw; acted on at the next frame
    // boundary, which is the only place the old buffer is provably not in use.
    VkDeviceSize arenaWant = 0;

    // --- the CROSS-FRAME stream store -------------------------------------------------
    //
    // A SECOND buffer, deliberately not a reserved region of the arena. The arena is a
    // bump allocator reset at every swap and its exhaustion path is load-bearing — it is
    // what turned a too-small arena into six parts of "view-dependent whole-frame black"
    // (§6ap) — so carving a persistent region out of the bottom of it would put a new
    // moving floor under that machinery and under `GrowArenaIfNeeded`'s buffer swap. A
    // separate buffer leaves every line of that alone, at the cost of `UploadStream`
    // having to say WHICH buffer it put the bytes in. That is `StreamLoc`.
    //
    // Why this exists at all: 94% of stream lookups already hit within a frame, and the
    // ~2,000 that miss still copy 61-77 MB — 94-97% of it byte-identical to what the
    // previous frame copied to the same guest address (§6at). The cache was doing its job
    // and being thrown away at the frame boundary.
    Buffer persist;
    VkDeviceSize persistCursor = 0;
    // Raised when a persistent allocation did not fit; acted on at the frame boundary,
    // which is the only place the buffer is provably not being read by the GPU.
    VkDeviceSize persistWant = 0;
    struct PersistEntry
    {
        VkDeviceSize at = 0;
        // The PING-PONG TWIN, allocated the first time this stream is caught being
        // rewritten in place and never before.
        //
        // Overwriting `at` was correct for exactly as long as the submit was
        // synchronous: the draws reading it were recorded in an earlier frame whose
        // fence had been waited on. With frames in flight that stops being true, and the
        // failure is a wrong mesh with no error anywhere. Two slots and an alternation
        // are enough — when frame N+1 is being recorded, frame N-1 has provably retired
        // (that is the ring's invariant), so the only slot that can still be read is the
        // one frame N used, which is the other one.
        //
        // Lazy because it is rare: ~20 streams a frame go stale out of a store holding
        // thousands, so allocating a twin for every entry would double the store to
        // protect 1% of it.
        VkDeviceSize alt = VkDeviceSize(-1);
        uint64_t guard = 0;      // StreamGuard over the guest bytes when it was copied
        uint64_t lastFrame = 0;  // for the age report; not an eviction policy yet
        uint32_t bytes = 0;
    };
    std::unordered_map<uint64_t, PersistEntry> persistCache;
    // Counted, not sampled, because a cache that silently serves stale data looks exactly
    // like a rendering bug twenty frames later and this project has spent whole parts
    // chasing those. Plain adds on a hot struct — never Count(), which is a
    // std::map<std::string> lookup and would cost more than the copy it is measuring
    // (gotcha 230).
    struct PersistStats
    {
        uint64_t hits = 0;          // served across the frame boundary: bytes NOT copied
        uint64_t hitBytes = 0;
        uint64_t fills = 0;         // copied into the store for the first time
        uint64_t fillBytes = 0;
        uint64_t stale = 0;         // the guard caught a rewrite and we re-copied
        uint64_t staleBytes = 0;
        uint64_t overflow = 0;      // did not fit; fell back to the per-frame arena
        uint64_t flushes = 0;       // whole store dropped at a frame boundary
        uint64_t guardBytes = 0;    // what the guard itself read, i.e. its own cost
        // A rewritten stream needed a ping-pong twin and the store could not give it
        // one. The entry is DROPPED and the stream falls back to the per-frame arena —
        // never overwritten in place, which with frames in flight is the silent wrong
        // mesh this whole mechanism exists to prevent. Counted because "the safe
        // fallback fired" is a performance fact and its absence is the correctness one.
        uint64_t staleEvicted = 0;
    } persistStats;
    // ON by default, with `CZ_VK_NO_PERSIST_STREAMS=1` as the control arm — the same
    // shape as CZ_VK_NO_ARENA_GROWTH and CZ_VK_NO_SUBMIT, so one binary is both arms of
    // its own A/B and the old renderer stays reachable for as long as it is useful.
    bool persistOn = true;

    Buffer staging;
    VkDeviceSize stagingCursor = 0;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[5]{};
    VkDescriptorSet sets[5]{};
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;

    // --- what is already bound on `cmd`, so a draw can skip re-binding it ------------
    //
    // Vulkan's pipeline binding, dynamic state and descriptor sets are properties of
    // the COMMAND BUFFER and persist across draws and across render-pass instances, so
    // re-issuing them per draw is pure overhead. It went unnoticed for four phases
    // because it is proportional to the draw count and the draw count was ~1,900:
    // `record` was 5% of an 85 ms frame. A Still Creek zombie crowd issues **4,900 to
    // 6,300 draws a frame**, where the same code is 21.6% of a 56 ms frame and the
    // renderer's CPU is the largest single term in it (operator session, part 18).
    //
    // Safe to cache against the command buffer because every pipeline this renderer
    // creates declares the SAME three dynamic states (viewport, scissor, blend
    // constants) and shares ONE pipeline layout — so a pipeline bind never invalidates
    // the dynamic state or the descriptor bindings. Reset in BeginFrame, which is the
    // one place a fresh command buffer starts. CZ_VK_NO_STATE_CACHE=1 is the arm.
    struct BoundState
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkViewport viewport{};
        VkRect2D scissor{};
        float blend[4]{};
        bool haveViewport = false, haveScissor = false, haveBlend = false;
        bool setsBound = false;
        // The vertex and index bindings, tracked but NOT yet acted on — see
        // `BindSkips` for why the counter comes before the change. 16 is above the
        // highest binding this title has ever used; a draw with more is simply not
        // counted rather than counted wrongly.
        static constexpr uint32_t kMaxTrackedBindings = 16;
        VkDeviceSize vertexOffset[kMaxTrackedBindings]{};
        VkBuffer vertexBuffer[kMaxTrackedBindings]{};
        bool haveVertex[kMaxTrackedBindings]{};
        VkDeviceSize indexOffset = 0;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
        bool haveIndex = false;
    } bound;
    // How often each bind was skipped, across the whole run. An arm needs a counter or
    // its absence proves nothing (gotcha 151) — but the counter must not cost more than
    // the thing it measures. The first version used Count(), which is a
    // std::map<std::string> lookup, so the cached arm paid five map lookups per draw to
    // save five vkCmd calls and the A/B came out a dead heat BY CONSTRUCTION. These are
    // plain adds on a struct that is already hot, printed once with the stats.
    struct BindSkips
    {
        uint64_t pipeline = 0, viewport = 0, scissor = 0, blend = 0, sets = 0, draws = 0;
        // The vertex and index binds, COUNTED ONLY. `docs/perf-cpu-plan.md` §1a
        // hypothesis A is that a crowd — many copies of a few zombie meshes — rebinds
        // the same buffer at the same offset draw after draw, and that extending the
        // state cache to cover them is a dozen lines. It says to add the counters and
        // run without acting on them, because a low repeat rate kills the idea for
        // free and a high one is the justification. These are that measurement: the
        // `Repeat` counters are what a skip WOULD have saved, and they are reset
        // exactly where `BoundState` is, so they cannot promise a saving that a fresh
        // command buffer would take back.
        uint64_t vertexBinds = 0, vertexBindRepeats = 0;
        uint64_t indexBinds = 0, indexBindRepeats = 0;
    } skips;

    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler pointSampler = VK_NULL_HANDLE;
    // 0 = the device has no samplerAnisotropy feature (checked at device creation);
    // otherwise the device's maxSamplerAnisotropy limit, read so a sampler never
    // asks for more than the device names.
    float anisoLimit = 0.0f;
    // Part 41 item 1b: per-fetch samplers. Key = the fetch constant's own
    // mag/min/mip/aniso fields (dword3 bits 19..27); value = the sampler's index in
    // the set-3 heap. Index 0 stays the plain trilinear REPEAT sampler, which is
    // both the fallback and the pre-part-41 behaviour. Samplers live for the
    // process, like every other sampler here.
    std::map<uint32_t, uint32_t> samplerBySpec;
    uint32_t samplerCount = 1;
    Image dummy2D, dummy3D, dummyCube, dummy1D;

    std::map<uint64_t, ShaderMeta> shaders;
    std::map<PipelineKey, VkPipeline> pipelines;
    // The draw-ID pass: the substitute fragment module, and the frame it is armed for
    // (0 = disarmed, which is every frame unless CZ_VK_DRAW_ID is set and F9 pressed).
    VkShaderModule drawIdModule = VK_NULL_HANDLE;
    // ARMED AS A FLAG, NOT AS A FRAME NUMBER, and that is the whole lesson of building
    // this: `R->frame` is incremented by the SWAP, so the draws of a frame are recorded
    // while the counter still holds the previous frame's value. Arming "frame + 1" from
    // the present path therefore named a number the draw path never saw, and the pass
    // silently never ran — for three test runs, while its output was being read as if it
    // were a map. A flag consumed by the first draw that sees it cannot be off by one.
    bool drawIdArmed = false;      // set by F9, cleared when the frame is presented
    bool drawIdActive = false;     // set by the draw path: THIS recorded frame is the map
    uint64_t drawIdRanOnFrame = 0;
    std::unordered_map<uint64_t, TextureEntry> textures;
    // By resolve destination, with bit 31 of the key set for a DEPTH resolve.
    //
    // The address alone is NOT an identity. `1439B000` is a shadow cascade's depth
    // destination early in a frame and the tone map's colour output late in the SAME
    // frame — the trace shows the final compose sampling it — and the capture agrees
    // (B1's 1812F000: 890 depth resolves, 852 colour). Keyed on the address alone the
    // two evict each other twice a frame, which is a device-wait and a fresh bindless
    // slot each time, i.e. gotcha 192's descriptor-heap exhaustion. A fetch picks the
    // one it meant by its own FORMAT: `k_24_8` and `k_24_8_FLOAT` are depth surfaces.
    std::unordered_map<uint32_t, Snapshot> snapshots;
    uint32_t nextTextureSlot = 1; // slot 0 is the dummy
    // Descriptor set 2 is its OWN unbounded array of TextureCube views, so it has its own
    // slot space. Sharing `nextTextureSlot` would work but would waste the sparser heap's
    // indices against the denser one's exhaustion — and this project has already had the
    // 2D heap fill mid-session and serve white (gotcha 192), so the two counters are kept
    // apart to keep that failure legible.
    uint32_t nextCubeSlot = 1;   // slot 0 is the 1x1 white cube dummy

    // CZ_VK_DRAW_CENSUS — the frame whose every draw is being listed, and the file it
    // goes to. Zero means disarmed, which is every frame until F9 is pressed.
    uint64_t drawCensusFrame = 0;
    uint64_t capturePictureFrame = 0;   // CZ_CAPTURE_KEY: write this frame's picture
    uint64_t captureSnapFrame = 0;      // ... and its resolve snapshots
    FILE* drawCensusFile = nullptr;
    uint64_t drawCensusLines = 0;

    // CZ_VK_EXPOSURE_TRACE — this frame's spread of the title's own exposure scalar,
    // `pc(14).w`. It exists because part 31 made that number load-bearing and there was
    // no way to read it for a NAMED frame: `CZ_VK_PSBIND` prints it, but it dedupes on
    // a key that includes the constants and caps at 64 lines, so a scalar that drifts by
    // 1e-4 a frame spends the whole budget in the first few hundred frames and says
    // nothing about the frame a snapshot was taken on.
    //
    // Min AND max, not a single value: the tone curve reads `x = colour * pc(14).w`, and
    // whether one exposure is in force for the whole frame or several are decides
    // whether a whole-frame histogram can be inverted at all.
    float expMin = 0.0f;
    float expMax = 0.0f;
    uint32_t expDraws = 0;

    // The cube maps the title renders itself, keyed on the BASE face's address, plus the
    // reverse index a resolve needs: face address -> (base address, face number). The
    // second map is what makes the refresh free — a resolve knows only where it wrote,
    // and without it every resolve would have to search six candidate strides.
    std::unordered_map<uint32_t, CubeSnapshot> cubeSnapshots;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> cubeFaceOwner;

    // Per-frame vertex/index stream cache: one guest buffer resolved once per frame
    // however many draws read it. It records WHERE the bytes are, which since the
    // cross-frame store exists is either buffer — a stream served across the frame
    // boundary is registered here too, so the second and subsequent draws of that frame
    // do not even pay its guard.
    std::unordered_map<uint64_t, StreamLoc> streamCache;

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
    // The constants the fingerprint above HASHES, kept rather than only summarised.
    // A hash can say two frames differ; only the values can say where the camera was,
    // and reproducing a shot is the whole point of the pose capture (see the .pose
    // writer at the F9 block). 16 float4 = the view-projection plus the world matrices.
    uint32_t camConsts[64] = {};
    // The same constants from the frame's LARGEST draw — the scene camera, where
    // camConsts is whatever the first draw used (the shadow pass's light).
    uint32_t camConstsBig[64] = {};
    uint32_t camBigVerts = 0;
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
    // The first bound texture's DIMENSIONS, so a probe printing normalized texture
    // coordinates can state them in texels. A UV is meaningless without the size it is
    // normalized by — that is the whole question when two atlases differ only in size.
    uint32_t lastTexW = 0;
    uint32_t lastTexH = 0;
    uint32_t targetWidth = 1280, targetHeight = 720;
    // The EDRAM stand-in's extent, which is NOT the presented frame's (it is taller —
    // this title's shadow cascade is 1024 rows). Set in InitCommon beside the images.
    uint32_t edramWidth = 1280, edramHeight = 720;
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

// The memory properties a READBACK buffer wants — which are not the ones every other
// host-visible buffer here wants, and that difference is worth a function.
//
// `FindMemoryType` returns the FIRST type satisfying the mask, and on a discrete GPU
// the first HOST_VISIBLE|HOST_COHERENT type is WRITE-COMBINED: writes stream to the
// device beautifully and reads are UNCACHED, running at a few hundred MB/s. Every other
// mapped buffer in this renderer is write-only from the CPU (the arena, the staging
// buffer), so that type is right for them. The readback buffer is the one buffer the
// CPU READS, and it had inherited the same mask — so presenting a frame meant reading
// 3.7 MB back over an uncached mapping. Measured at 15.7% of a 103 ms gameplay frame,
// which is ~230 MB/s and is what uncached reads look like.
//
// HOST_CACHED is the fix and it is the whole fix. Asking for it is a preference rather
// than a requirement because an integrated GPU may not offer the combination, and a
// renderer that refuses to start is worse than one that reads slowly.
VkMemoryPropertyFlags ReadbackMemoryProps()
{
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // CZ_VK_READBACK_UNCACHED=1 — the pre-fix arm, i.e. the write-combined readback.
    // The same-binary control for the frame-rate claim this change makes.
    if (EnvOn("CZ_VK_READBACK_UNCACHED"))
    {
        fprintf(stderr, "[vk] readback buffer forced UNCACHED (the pre-fix arm)\n");
        return base;
    }
    for (uint32_t i = 0; i < R->memProps.memoryTypeCount; i++)
    {
        const VkMemoryPropertyFlags f = R->memProps.memoryTypes[i].propertyFlags;
        if ((f & base) == base && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            return base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    fprintf(stderr, "[vk] no HOST_CACHED memory type — the readback stays uncached\n");
    return base;
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

// Attach a human name to a Vulkan object, so the validation layer prints it instead of a
// handle. A no-op without CZ_VK_VALIDATION — the function pointer is null then, and the
// arguments are not even formatted.
//
// This exists because of what part 25's first validation session could NOT say. Four of
// its five defects were identifiable by reading the code; the fifth — a sampled image
// still VK_IMAGE_LAYOUT_UNDEFINED when a draw reads it — names an image, and this
// renderer creates images as EDRAM targets, guest textures, resolve snapshots, sized
// views of snapshots and dummies. A handle distinguishes none of those, and an undefined
// layout is undefined CONTENT: a wrong picture with no counter anywhere.
void NameObject(uint64_t handle, VkObjectType type, const char* fmt, ...)
{
    if (!R->setObjectName || !handle)
        return;
    char name[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    VkDebugUtilsObjectNameInfoEXT ni{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    R->setObjectName(R->device, &ni);
}

void NameImage(const Image& img, const char* fmt, ...)
{
    if (!R->setObjectName)
        return;
    char name[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    NameObject(uint64_t(img.image), VK_OBJECT_TYPE_IMAGE, "%s", name);
    NameObject(uint64_t(img.view), VK_OBJECT_TYPE_IMAGE_VIEW, "%s view", name);
}

bool CreateImage(Image& img, uint32_t w, uint32_t h, VkFormat format,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect,
                 VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D, uint32_t layers = 1,
                 uint32_t depthExtent = 1,
                 VkComponentMapping components = VkComponentMapping{},
                 uint32_t levels = 1)
{
    img.width = w;
    img.height = h;
    img.layers = layers;
    img.levels = levels;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    // THE IMAGE TYPE COMES FROM THE VIEW TYPE, not from the depth extent. It used to be
    // `depthExtent > 1 ? 3D : 2D`, which is right for every image built out of a guest
    // surface and WRONG for the 1x1 dummies: `makeDummy(dummy3D, VIEW_TYPE_3D, 1, 1, 1)`
    // and `makeDummy(dummy1D, VIEW_TYPE_1D, ...)` both pass depth 1, so both got a
    // VK_IMAGE_TYPE_2D image under a view that Vulkan requires to match. That is
    // VUID-VkImageViewCreateInfo-subResourceRange-01021, one of the five the validation
    // layer reported the hour it was installed (open item 00d) — and it had been failing
    // since phase 5, silently, because the layer was not present to say so.
    ci.imageType = (viewType == VK_IMAGE_VIEW_TYPE_3D || depthExtent > 1)
                       ? VK_IMAGE_TYPE_3D
                       : (viewType == VK_IMAGE_VIEW_TYPE_1D ||
                          viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY)
                             ? VK_IMAGE_TYPE_1D
                             : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, depthExtent };
    ci.mipLevels = levels;
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
    vi.subresourceRange = { aspect, 0, levels, 0, layers };
    VK_CHECK(vkCreateImageView(R->device, &vi, nullptr, &img.view), "vkCreateImageView");
    return true;
}

// True for the depth formats that carry a stencil aspect as well. Used by Barrier: a
// LAYOUT is a property of the whole image, so a barrier on one of these must name both
// aspects even when the caller only cares about depth.
bool FormatHasStencil(VkFormat f)
{
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}

void Barrier(VkCommandBuffer cmd, Image& img, VkImageLayout newLayout,
             VkImageAspectFlags aspect)
{
    if (img.layout == newLayout)
        return;
    // VUID-VkImageMemoryBarrier-image-03320, 20 of the 32 messages the validation layer
    // reported in its first session (open item 00d). Without `separateDepthStencilLayouts`
    // a barrier on a depth/stencil image must name DEPTH **and** STENCIL — the layout
    // transition applies to the image, not to the aspect the caller happens to be reading.
    // Two callers pass DEPTH alone quite reasonably (`RefreshSnapshotView` copies only the
    // depth aspect, and a depth snapshot's view is created with DEPTH alone so it can be
    // sampled), so the correction belongs HERE rather than at each site: a barrier that
    // forgets the stencil aspect leaves it in an undeclared layout, which is the same
    // undefined-content class as `vkCmdDraw-None-09600`, not a cosmetic complaint.
    if ((aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
        FormatHasStencil(img.format))
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = img.layout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    // ALL the layers AND all the levels, not just the first — see Image::layers and
    // Image::levels.
    b.subresourceRange = { aspect, 0, img.levels, 0, img.layers };
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
    // `arenaLimit`, not `arena.size`: with frames in flight this frame owns one region of
    // the buffer and the region past it belongs to a frame the GPU may still be reading.
    if (at + bytes > R->arenaLimit)
    {
        Count("arena: exhausted, draw skipped");
        // Ask for twice the size at the next frame boundary. A fixed number is what
        // caused this — see GrowArenaIfNeeded, called at the END of this frame.
        static const bool noGrowth = EnvOn("CZ_VK_NO_ARENA_GROWTH");
        if (!noGrowth)
            R->arenaWant = std::max(R->arenaWant, R->arena.size * 2);
        // Named ONCE PER FRAME, uncapped over the run.
        //
        // A total is the wrong shape for this: exhaustion is a property of one frame
        // (the arena resets at every swap), it happens on the BIGGEST frames, and the
        // draws it skips are the ones LATE in the frame — which in this title is the
        // whole post-process chain. A counter that only aggregates says "some draws were
        // skipped somewhere" where the interesting claim is "frame N lost its post
        // chain", and those are the frames that present BLACK. See docs/phase5-notes.md
        // §6ap; the frame number here is what lets a frame-stats line be joined to it.
        static uint64_t saidForFrame = ~0ull;
        if (saidForFrame != R->frame)
        {
            saidForFrame = R->frame;
            fprintf(stderr,
                    "[vk] arena EXHAUSTED on frame %llu (%llu MB used of %llu MB) — every "
                    "remaining draw of this frame is skipped\n",
                    (unsigned long long)R->frame,
                    (unsigned long long)((R->arenaCursor - R->arenaBase) >> 20),
                    (unsigned long long)((R->arenaLimit - R->arenaBase) >> 20));
        }
        return VkDeviceSize(-1);
    }
    R->arenaCursor = at + bytes;
    // The high water is what ONE FRAME used, so it is measured from this slot's base and
    // stays comparable across the arms. Reading it against the whole buffer would make
    // the second frame in flight look like a doubling of the title's demand.
    R->arenaHighWater = std::max(R->arenaHighWater, R->arenaCursor - R->arenaBase);
    return at;
}

// --- the cross-frame stream store ------------------------------------------------------
// A bump allocator too, but one that is NOT reset at the swap — that is the whole point.
// It has no exhaustion-skips-the-draw path, because running out here is not a failure:
// the stream falls back to the per-frame arena and renders exactly as it did before this
// store existed. Only the saving is lost, and the counter says so.
VkDeviceSize PersistAlloc(VkDeviceSize bytes, VkDeviceSize align = 16)
{
    const VkDeviceSize at = (R->persistCursor + align - 1) & ~(align - 1);
    if (at + bytes > R->persist.size)
    {
        // `at + bytes` in the max, not just a doubling: doubling alone cannot make
        // progress from a zero-sized store, and a single stream larger than the whole
        // store would otherwise ask for a size that still cannot hold it, forever.
        R->persistWant =
            std::max(R->persistWant, std::max(R->persist.size * 2, at + bytes));
        return VkDeviceSize(-1);
    }
    R->persistCursor = at + bytes;
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
    // VK_EXT_debug_utils goes with it, and it is the difference between a validation
    // message that says `VkImage 0x2f7a8c0000000117` and one that says
    // `snapshot 1439B000 1280x720`. Part 25 installed the layer and got five defects;
    // four of them were identifiable by reading, and the fifth — `vkCmdDraw-None-09600`,
    // a sampled image still UNDEFINED when a draw reads it — was not, because this
    // renderer creates images in five different places and the handle names none of them.
    // Naming is free, off with the layer, and turns that message into an address.
    const char* instExts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    const bool wantValidation = EnvOn("CZ_VK_VALIDATION");
    if (wantValidation)
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = instExts;
        fprintf(stderr, "[vk] validation layer requested\n");
    }
    VkResult ir = vkCreateInstance(&ici, nullptr, &R->instance);
    if (ir == VK_ERROR_EXTENSION_NOT_PRESENT && wantValidation)
    {
        // The naming extension is a convenience, not the instrument. Losing it must not
        // cost the layer — the same rule as the retry below, one level in.
        fprintf(stderr, "[vk] VK_EXT_debug_utils is absent — validation messages will "
                        "name raw handles rather than our objects\n");
        ici.enabledExtensionCount = 0;
        ir = vkCreateInstance(&ici, nullptr, &R->instance);
    }
    if (ir == VK_ERROR_LAYER_NOT_PRESENT && wantValidation)
    {
        ici.enabledExtensionCount = 0;
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

    // Size the bindless heap from the device, not from a constant (see g_maxDescriptors).
    // The clamp is a memory decision rather than a Vulkan one: a slot costs nothing on
    // its own, but every slot that gets USED is a VkImage, so an unbounded heap trades
    // white textures for unbounded VRAM. 65536 is ~16x the working set that filled the
    // old 4096 cap, which is room to find the next defect without pretending to be the
    // LRU this still needs. CZ_VK_MAX_TEXTURES overrides it — including DOWNWARD, which
    // is the same-binary arm that reproduces the exhaustion on demand.
    {
        const uint32_t deviceCap =
            std::min(props.limits.maxPerStageDescriptorSampledImages,
                     props.limits.maxDescriptorSetSampledImages / 4);
        uint32_t want = std::min(deviceCap, 65536u);
        if (const char* env = getenv("CZ_VK_MAX_TEXTURES"))
            want = std::min(deviceCap, uint32_t(std::max(16, atoi(env))));
        g_maxDescriptors = std::max(want, 256u);
        fprintf(stderr,
                "[vk] bindless heap: %u slots (device allows %u). Slots are not "
                "recycled yet, so a session that exhausts this serves the 1x1 white "
                "dummy from then on.\n",
                g_maxDescriptors, deviceCap);
    }

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
    // ANISOTROPIC FILTERING (part 41 item 1). Xenos filters up to 16:1 and the fetch
    // constants carry a per-texture aniso field; until part 41 both samplers were
    // plain trilinear, so every grazing-angle surface — the whole road at distance —
    // went to mush well before the horizon. The feature is enabled whenever the
    // device has it (the sampler decides whether to USE it, which is where
    // CZ_VK_NO_ANISO acts); asked for blindly it would fail device creation on a
    // device that lacks it, so it is checked first and its absence is a named
    // configuration fact, not a silent picture change.
    {
        VkPhysicalDeviceFeatures haveF{};
        vkGetPhysicalDeviceFeatures(R->physical, &haveF);
        if (haveF.samplerAnisotropy)
        {
            f2.features.samplerAnisotropy = VK_TRUE;
            R->anisoLimit = props.limits.maxSamplerAnisotropy;
        }
        else
            fprintf(stderr, "[vk] device lacks samplerAnisotropy — distance "
                            "filtering stays trilinear on this device\n");
    }
    // CZ_VK_ROBUST=1 — bound out-of-range buffer reads instead of undefined behaviour.
    // A Xenos vfetch past a stream's declared size returns ZERO (the fetch-constant
    // contract, already quoted at XeVfetchDep); a Vulkan vertex-attribute fetch past the
    // bound range without robustBufferAccess returns arbitrary bump-arena bytes, and an
    // FP32 attribute (fmt 37/57 — this title's texcoords and normals ride in those)
    // decodes ~0.8% of arbitrary bytes as NaN.
    //
    // BUT KNOW WHAT THIS ARM CAN AND CANNOT TEST (part 33, and it is gotcha 279's
    // shape): every stream is sub-allocated from ONE arena VkBuffer and
    // vkCmdBindVertexBuffers carries no size, so the robust bound is the WHOLE ARENA —
    // a fetch past its own stream but inside the arena is exactly as undefined-in-effect
    // as before. The plateau reading unchanged under this arm (890 px vs a 1,092
    // baseline) therefore says NOTHING about per-stream overruns; CZ_VK_RANGE_CENSUS is
    // the instrument that can. What this arm does bound is reads past the arena itself.
    if (Env("CZ_VK_ROBUST"))
    {
        VkPhysicalDeviceFeatures2 have{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        vkGetPhysicalDeviceFeatures2(R->physical, &have);
        if (have.features.robustBufferAccess)
        {
            f2.features.robustBufferAccess = VK_TRUE;
            fprintf(stderr, "[vk] CZ_VK_ROBUST: robustBufferAccess ENABLED\n");
        }
        else
            fprintf(stderr, "[vk] CZ_VK_ROBUST asked, but the device does not report "
                            "robustBufferAccess — running WITHOUT it\n");
    }

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
    // Resolves to null when the extension was not enabled, which is every run without
    // CZ_VK_VALIDATION — NameObject is then a branch on a null pointer and nothing else.
    R->setObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(R->instance, "vkSetDebugUtilsObjectNameEXT"));

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = R->queueFamily;
    VK_CHECK(vkCreateCommandPool(R->device, &pci, nullptr, &R->cmdPool),
             "vkCreateCommandPool");

    // One command buffer and one fence PER FRAME SLOT. `framesInFlight` is read here
    // because it decides how many of each exist; the buffers themselves are cheap, so
    // all `kMaxFramesInFlight` are created and the arm only decides how many are used.
    // That keeps the two arms one binary and keeps `CZ_VK_FRAMES_IN_FLIGHT=1` byte-for-
    // byte the old renderer: slot 0 only, submitted and immediately waited on.
    VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = R->cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        VK_CHECK(vkAllocateCommandBuffers(R->device, &cbi, &R->frames[i].cmd),
                 "vkAllocateCommandBuffers");
        VK_CHECK(vkCreateFence(R->device, &fi, nullptr, &R->frames[i].fence),
                 "vkCreateFence");
    }
    R->cmd = R->frames[0].cmd;
    R->fence = R->frames[0].fence;
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
        b.descriptorCount = g_maxDescriptors;
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
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, g_maxDescriptors * 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, g_maxDescriptors },
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
    // 32, not 24: three uint64 device addresses plus the DRAW INDEX the draw-ID pass
    // reads at offset 24 (and four bytes of padding). The translated shaders declare
    // only the first 24 and are unaffected — a larger range is not a larger block.
    pcr.size = 32;
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
    const uint32_t d5 = regs[kFetchConstantBase + slot * 6 + 5];

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
    t.filterAniso = (d3 >> 25) & 7;
    // dword4: mip_min_level bits 2..5, mip_max_level bits 6..9
    t.mipMin = (d4 >> 2) & 0xF;
    t.mipMax = (d4 >> 6) & 0xF;
    // dword5 bits 11 and 12..31: packed_mips, and THE MIP CHAIN'S OWN ADDRESS. Both sit
    // immediately above the dimension field measured below, and that adjacency is what
    // makes them readable at all: dimension at 9..10 fixes the rest of the dword's
    // layout, so packed_mips lands at 11 and the 20-bit page-aligned mip address at
    // 12..31, exactly as the base address sits at 12..31 of dword1.
    t.packedMips = ((d5 >> 11) & 1) != 0;
    t.mipAddress = (d5 >> 12) << 12;
    // dword5 bits 9..10: the DIMENSION, in the same encoding the shader uses
    // (0 = 1D, 1 = 2D, 2 = 3D, 3 = cube). This field was `t.dimension = 1;` with a
    // comment saying the dimension is taken from the shader, and the shader metadata had
    // no dimension in it — so it was taken from nowhere, and every cube map in the game
    // read the 1x1 white dummy for the whole of phase 5 (docs/open-items.md item 00).
    //
    // THE BIT POSITION WAS MEASURED, NOT REMEMBERED, and the measurement is worth
    // repeating for Case West because it costs one run. `CZ_VK_DIM_CENSUS=1` partitions
    // every fetch by the dimension the SHADER declares — an independent oracle — and
    // accumulates the AND and the OR of all six dwords per class. Over 842,556 2D and
    // 47,574 cube fetches exactly two bits separated the classes: dword2 bits 26/28 and
    // dword5 bit 10. dword5 reads 1 for every 2D fetch and 3 for every cube one, so the
    // field is bits 9..10; my recollection had said bits 7..8 and was wrong.
    t.dimension = (d5 >> 9) & 3;
    // dword2's top six bits are the STACK DEPTH, stored minus one. Predicted before the
    // run to be 5 for a cube (six faces) and 0 for a 2D surface; the census read exactly
    // that, 47,574 of 47,574 and 842,556 of 842,556, from a different dword than the one
    // above — which is what makes the dimension a measurement rather than a fit.
    if (t.dimension == 3 || ((d2 >> 26) & 0x3F))
        t.depth = ((d2 >> 26) & 0x3F) + 1;
    return t;
}
} // namespace xenos

namespace {

// IS THIS SNAPSHOT STILL THE RIGHT ANSWER FOR THAT ADDRESS?
//
// It always is, and for a structural reason: a resolve's pixels are never written back
// into guest memory (see the Snapshot comment), so for an address the GPU has resolved
// to, the snapshot is the ONLY copy of what that surface holds. Guest memory there is
// whatever the allocator left, which is normally zero.
//
// This used to be `frameSeen + 1 >= frame` — the snapshot had to have been taken this
// frame or last. That window was written when every known consumer was a
// post-processing pass reading the surface a pass earlier in the SAME frame had just
// resolved, and it is silently wrong for a surface the title resolves ONCE and then
// samples for the rest of the run. Case Zero's colour-grading LUT is exactly that: at
// the title screen it re-renders all three LUTs every frame, so the window never
// bound; at the prologue the grade is static, the LUT stops being resolved, and from
// the second frame onward the tone map's LUT fetch fell out of the snapshot path into
// guest memory and sampled zeros. §6s already established that a black LUT is a black
// frame, so the whole prologue presented 0.00% non-black while every other input to
// the compose was live — the same shape as §6s and part 9's ordering bug, one link
// further along.
//
// The risk the window was implicitly guarding against is real but different: the guest
// could free a former resolve destination and put a CPU-uploaded texture there, and we
// would serve the stale snapshot. Nothing in this title does that — its resolve
// destinations are a fixed set of render targets — and the census counts the age of
// every snapshot it serves, so the day one does the number is on screen rather than in
// a picture. CZ_VK_SNAPSHOT_MAX_AGE=N restores a bounded window (1 = the pre-part-15
// behaviour) as the same-binary control arm.
uint64_t SnapshotMaxAge()
{
    static const uint64_t age =
        Env("CZ_VK_SNAPSHOT_MAX_AGE")
            ? strtoull(Env("CZ_VK_SNAPSHOT_MAX_AGE"), nullptr, 10)
            : 0;   // 0 = no limit
    return age;
}

bool SnapshotUsable(const Snapshot& s)
{
    const uint64_t maxAge = SnapshotMaxAge();
    return maxAge == 0 || s.frameSeen + maxAge >= R->frame;
}

// Copy a snapshot's top-left w x h corner into `view`, in the command buffer given.
// Both images end in SHADER_READ_ONLY, which is the layout their descriptors were
// written with; the caller is responsible for not being inside a render pass.
void RefreshSnapshotView(VkCommandBuffer cb, Image& src, SnapshotView& view,
                         VkImageAspectFlags aspect)
{
    Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
    Barrier(cb, view.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);
    VkImageCopy c{};
    c.srcSubresource = { aspect, 0, 0, 1 };
    c.dstSubresource = { aspect, 0, 0, 1 };
    c.extent = { view.image.width, view.image.height, 1 };
    vkCmdCopyImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, view.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cb, view.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);
    Barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);
}

// Copy one resolve snapshot into one FACE of a cube snapshot, in the command buffer
// given. Both images end back in SHADER_READ_ONLY, which is the layout their descriptors
// were written with — the snapshot because other passes sample it as an ordinary 2D
// surface in the same frame, the cube because a draw may sample it immediately.
//
// The extent is the intersection of the two. A face is bounded by the snapshot that
// feeds it: if the guest resolved a 32x32 region into a 64x64 face's address we copy
// what exists rather than reading 64 rows out of a 32-row image, and the shortfall is
// visible as a face that is only partly filled rather than as undefined content.
void CopyFaceIntoCube(VkCommandBuffer cb, Image& src, CubeSnapshot& cube, uint32_t face)
{
    const uint32_t w = std::min(src.width, cube.faceExtent);
    const uint32_t h = std::min(src.height, cube.faceExtent);
    if (!w || !h || face >= 6)
        return;
    Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(cb, cube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy c{};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
    c.extent = { w, h, 1 };
    vkCmdCopyImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cube.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cb, cube.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    cube.facesFilled |= 1u << face;
}

// Serve a cube fetch whose base address is a RESOLVE DESTINATION — i.e. a cube map the
// title renders rather than loads. Returns a slot in set 2's heap, or 0 for the dummy.
//
// `faceStride` is the guest byte distance between one face's base and the next, computed
// by the caller exactly as the guest-memory cube path computes it (the tiled footprint of
// one face). It is a MODEL of the guest's layout, and the census this function prints on
// its first call is the check on it: if the six face addresses it derives are not the six
// addresses the resolves actually wrote, the log says so with both lists and the fill
// comes up short rather than silently assembling a cube out of the wrong surfaces.
//
// CZ_VK_NO_CUBE_SNAPSHOT=1 declines to the dummy, i.e. the pre-part-26 renderer, in the
// same binary — the control arm for every claim made about this path.
uint32_t CubeSnapshotSlot(const xenos::TextureFetch& t, uint32_t faceStride)
{
    static const bool disabled = EnvOn("CZ_VK_NO_CUBE_SNAPSHOT");
    if (disabled)
    {
        Count("texture: CUBE at a resolve destination declined (CZ_VK_NO_CUBE_SNAPSHOT)");
        return 0;
    }
    const uint32_t basePhys = t.address & 0x1FFFFFFF;
    auto it = R->cubeSnapshots.find(basePhys);
    if (it != R->cubeSnapshots.end())
    {
        // A face's extent is part of its identity for the same reason a snapshot's is:
        // a different extent at the same address is a different surface, and copying
        // into the old image would leave the previous one's pixels around the edge.
        if (it->second.faceExtent != t.width)
        {
            Count("texture: CUBE snapshot extent changed — declined");
            return 0;
        }
        it->second.frameSeen = R->frame;
        Count("texture: CUBE served from resolve snapshots");
        return it->second.slot;
    }
    if (R->nextCubeSlot >= g_maxDescriptors)
    {
        Count("texture: CUBE snapshot refused — cube heap full");
        return 0;
    }
    if (t.width != t.height || !faceStride)
    {
        Count("texture: CUBE snapshot refused — non-square face or no stride");
        return 0;
    }

    CubeSnapshot cube;
    cube.faceExtent = t.width;
    cube.faceStride = faceStride;
    cube.slot = R->nextCubeSlot++;
    // R8G8B8A8_UNORM, matching what a colour resolve snapshot is stored as: vkCmdCopyImage
    // requires compatible formats, and this image exists only to be filled from those.
    // The fetch constant's own format is deliberately NOT consulted — a rendered surface's
    // pixels are whatever the render target held, not whatever the fetch declares.
    if (!CreateImage(cube.image, t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6, 1))
    {
        --R->nextCubeSlot;
        Count("texture: CUBE snapshot image creation FAILED");
        return 0;
    }
    NameImage(cube.image, "cube snapshot %08X %ux%u slot %u", basePhys, t.width, t.height,
              cube.slot);

    // Fill it BEFORE the descriptor is written, and transition every layer out of
    // UNDEFINED whether or not a face was found for it. The order matters for the same
    // reason it does in DoResolve: the descriptor becomes visible to the frame's whole
    // command buffer the moment it is written, and a descriptor claiming
    // SHADER_READ_ONLY on an image still in UNDEFINED is undefined CONTENT. That is
    // `vkCmdDraw-None-09600`, and this is the third time this project has met the shape —
    // the first was `Barrier`'s hardcoded `layerCount = 1`, which left five of the dummy
    // cube's six faces sampled undefined for the whole of phase 5 (open item 00d).
    RunImmediate([&](VkCommandBuffer cb) {
        Barrier(cb, cube.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        for (uint32_t f = 0; f < 6; f++)
        {
            auto s = R->snapshots.find(basePhys + f * faceStride);
            if (s == R->snapshots.end())
                continue;
            CopyFaceIntoCube(cb, s->second.image, cube, f);
        }
    });

    VkDescriptorImageInfo ii{};
    ii.imageView = cube.image.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = R->sets[2];
    wr.dstBinding = 0;
    wr.dstArrayElement = cube.slot;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &wr, 0, nullptr);

    for (uint32_t f = 0; f < 6; f++)
        R->cubeFaceOwner[basePhys + f * faceStride] = { basePhys, f };
    cube.frameSeen = R->frame;

    // The census, once per cube, and it is the check on the stride model above. Six
    // "yes" lines mean the layout is what the guest-memory path assumes; anything else
    // is the measurement that says so, with the addresses, rather than a cube quietly
    // assembled out of five faces and a hole.
    fprintf(stderr,
            "[vk] CUBE SNAPSHOT %08X %ux%u stride %08X -> set 2 slot %u, faces:\n",
            basePhys, t.width, t.height, faceStride, cube.slot);
    for (uint32_t f = 0; f < 6; f++)
    {
        auto s = R->snapshots.find(basePhys + f * faceStride);
        fprintf(stderr, "[vk]   face %u at %08X: %s\n", f, basePhys + f * faceStride,
                s == R->snapshots.end()
                    ? "NO RESOLVE SNAPSHOT — this face is whatever the image was cleared to"
                    : "filled from its resolve snapshot");
    }
    if (cube.facesFilled == 0x3F)
        Count("texture: CUBE snapshot assembled from all six faces");
    else
        Count("texture: CUBE snapshot assembled with FEWER THAN SIX faces");

    const uint32_t slot = cube.slot;
    R->cubeSnapshots.emplace(basePhys, std::move(cube));
    return slot;
}

// The bindless slot for a w x h view of `snap`, creating it on first use.
//
// The creating copy goes through RunImmediate — a submit and a wait — because this is
// reached from inside DoDraw, where the render pass is open and vkCmdCopyImage is not
// legal. It happens once per (address, size) pair for the life of the process (a few
// dozen times in a whole run), and thereafter the view is refreshed for free inside
// whatever resolve next writes its source. Doing the FIRST fill here rather than
// deferring it to that resolve is deliberate: a surface the title resolves ONCE and then
// samples forever — this title's colour-grading LUT is exactly that (§6s) — would
// otherwise hand out an empty view for the rest of the run.
uint32_t SnapshotViewSlot(Snapshot& snap, uint32_t w, uint32_t h)
{
    const uint32_t key = (w << 16) | h;
    auto it = snap.views.find(key);
    if (it != snap.views.end())
        return it->second.slot;
    if (R->nextTextureSlot >= g_maxDescriptors)
    {
        Count("texture: snapshot view refused — bindless heap full");
        return 0;
    }

    SnapshotView view;
    view.slot = R->nextTextureSlot++;
    const VkComponentMapping depthSwizzle{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                           VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
    const VkImageAspectFlags aspect =
        snap.fromDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (!CreateImage(view.image, w, h, snap.image.format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     aspect, VK_IMAGE_VIEW_TYPE_2D, 1, 1,
                     snap.fromDepth ? depthSwizzle : VkComponentMapping{}))
    {
        --R->nextTextureSlot;
        Count("texture: snapshot view image creation FAILED");
        return 0;
    }
    NameImage(view.image, "snapshot view %ux%u slot %u%s", w, h, view.slot,
              snap.fromDepth ? " DEPTH" : "");

    VkDescriptorImageInfo ii{};
    ii.imageView = view.image.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = R->sets[0];
    wr.dstBinding = 0;
    wr.dstArrayElement = view.slot;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &wr, 0, nullptr);

    RunImmediate([&](VkCommandBuffer cb) {
        RefreshSnapshotView(cb, snap.image, view, aspect);
    });
    Count("texture: snapshot view created at the fetch's declared size");
    const uint32_t slot = view.slot;
    snap.views.emplace(key, std::move(view));
    return slot;
}

// Upload the texture a fetch constant describes and return its bindless slot, or 0 for
// the dummy. Cached on the fetch constant's own six dwords: if none of them changed the
// texture is the same texture, and if any did it is a different one. Keying on the base
// address alone would be wrong in this title, which reuses addresses.
// `shaderDim` is what the SHADER said this slot is (0 = 1D, 1 = 2D, 2 = 3D, 3 = cube),
// and the returned slot is an index into THAT dimension's descriptor heap. The two
// cannot be conflated: set 0 holds `Texture2D` views and set 2 holds `TextureCube` ones,
// so a 2D slot number published into the cube array indexes a descriptor that was never
// written.
uint32_t UploadTexture(uint8_t* base, const uint32_t* regs, uint32_t constIdx,
                       uint32_t shaderDim)
{
    ProfScope _p(&g_prof.textures);
    // THE DENOMINATOR, and it goes FIRST. Every other cube counter here is a share of
    // this, and a count with no denominator is the shape of claim this project keeps
    // having to retract: part 25 published "114 of 337,716, 0.03%" off one recipe and a
    // deeper run of the same binary declined 90,984 with no total to divide by
    // (gotcha 242).
    //
    // It used to sit AFTER the `t.type != 2` early return, so every cube fetch whose slot
    // the guest never set was missing from the denominator as well as from the numerator —
    // 207 of them on the operator's route and 2,182 on the headless one. A denominator
    // that skips exactly the failures it is meant to be a denominator FOR is the same
    // early-return-shadows-a-counter defect as gotcha 171, one level up.
    if (shaderDim == 3)
        Count("texture: CUBE fetch");
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < 6; i++)
    {
        key ^= regs[xenos::kFetchConstantBase + constIdx * 6 + i];
        key *= 1099511628211ull;
    }
    // THE DIMENSION IS PART OF THE KEY, because the cached value is a slot number and a
    // slot number only means something against one heap. Two shaders could in principle
    // sample the same fetch constant as a 2D texture and as a cube; without this the
    // second one would be served the first one's slot, indexing the wrong descriptor
    // array. It costs one multiply and removes a whole class of impossible-to-read bug.
    key ^= shaderDim;
    key *= 1099511628211ull;
    const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
    if (t.type != 2)
    {
        Count("texture: fetch constant is not a texture");
        // SPLIT OUT FOR THE CUBE CASE, because "cube fetch got the dummy" had no
        // breakdown and part 26 attributed all of it to the shader/constant
        // disagreement. It is not: the disagreement is 1,349 of 2.25 M cube fetches on
        // the outdoor route while 18,057 cube fetches are served the dummy, so ~93% of
        // the declines had an unnamed cause. Every early return that a cube fetch can
        // reach now says which one it was — an unnamed decline is the shape of thing
        // this project keeps having to re-measure (gotcha 171).
        if (shaderDim == 3)
            Count("texture: CUBE fetch whose constant is NOT A TEXTURE — served the "
                  "dummy");
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
    //
    // A CUBE FETCH NEVER TAKES THIS PATH. Every snapshot is a 2D image registered in set
    // 0, so serving one to a cube fetch would publish a set-0 slot number into the cube
    // array and index a descriptor that was never written — undefined, not merely wrong.
    // **This title DOES resolve to a cube map** — see the note at the decline below.
    const bool cubeFetch = shaderDim == 3;

    // THE STANDING CROSS-CHECK, and it costs one compare. The dimension now has two
    // independent sources — the shader's fetch instruction (via the sidecar) and the
    // guest's own fetch constant — and they must agree. They do here, on every one of
    // 890,130 fetches in the run that established the decode. The shader stays the
    // AUTHORITY, because it is the shader that indexes a particular descriptor array and
    // a disagreement resolved the other way would publish a slot into a heap nothing
    // reads; but a silent disagreement would mean one of the two decodes is wrong, and
    // that is precisely the kind of thing this project has learned not to leave uncounted
    // (gotcha 3). The counter names the case rather than the fix.
    //
    // NOT on the CZ_VK_NO_CUBE arm, where the caller has deliberately lied about the
    // shader's answer: every cube fetch would then "disagree" by construction and the
    // counter would read 3,431,182 in a ten-minute run, which is a measurement of the arm
    // and not of the decode. An instrument that saturates under its own control arm cannot
    // be read on either side of the A/B.
    // Set when the shader asks for a cube and the guest describes ONE 2D surface. The six
    // layers are then all read from the same face rather than from a stride the constant
    // does not claim exists. See the comment at the assignment below.
    bool cubeFromOneFace = false;
    static const bool noCubeArm = EnvOn("CZ_VK_NO_CUBE");
    if (t.dimension != shaderDim && !noCubeArm)
    {
        Count("texture: the SHADER and the FETCH CONSTANT disagree about the dimension");
        // MEASURED: 114 cube-declared fetches in a boot-to-gameplay run have a fetch
        // constant that says 2D, against 337,602 that say cube and carry a stack depth of
        // 5 — but the SAME BINARY on the deeper outdoor recipe declined 90,984, which is
        // why the counter above exists. For these we do not know what the memory
        // holds, and reading six faces out of a surface the guest describes as one would
        // build a cube map from five slabs of whatever follows it. Declining is the
        // honest failure and it is also exactly the picture those draws got before part
        // 25, so this cannot make anything worse — but it is COUNTED, so if the share
        // ever grows it is a number and not a mystery.
        // WHAT HARDWARE DOES HERE IS BIND THE SURFACE ANYWAY, so we build the cube out of
        // the ONE face the guest described instead of serving a fabricated white texel.
        //
        // Part 26 concluded the opposite — that hardware never shows this disagreement, so
        // we must be manufacturing it and the fix belongs upstream of the decline. That
        // rested on one capture frame. The operator's own route makes this **2.06% of all
        // cube fetches** (33,608 of 1,629,525, against 0.05% on the headless route — the
        // same statistic-fitted-to-the-reachable-population error as gotcha 242), and
        // re-asking the agreement census on `w1_spawn` instead of `w2_gasstation` finds
        // hardware doing exactly this on **4 of 5,886 fetches — on the very two shaders
        // that account for 91% of ours** (`ps_af40b02e26617a15` slot 1 and
        // `ps_8eddd0fd8de516f0` slot 3, both a 4x4 `k_8_8_8_8` placeholder in a
        // cube-declared slot). Hardware renders those surfaces correctly, so the guest's
        // own data is a sufficient input and the decline was the defect.
        //
        // Replicating one face is the honest reading of a single-face surface sampled with
        // cube addressing, and it is what the guest's data supports: a stack depth of 1
        // says there is one slab there, and reading six would build a cube out of five
        // slabs of whatever follows it — which is what part 25 correctly refused to do.
        // The white dummy was never a third option, it was a fabrication, and it maximises
        // a multiplicative reflection term (open item 00f: the white glass and the
        // blown-out bathroom window are confirmed dummy-samplers by the magenta test).
        //
        // CZ_VK_NO_CUBE_REPLICATE=1 restores the decline — the same-binary control arm,
        // and the thing to hand an operator for a side-by-side.
        //
        // PREDICTS: the white glass, the white bathroom window and the white newspaper
        // boxes stop being white, the ground band does NOT change (it is not cube-related
        // and three arms of the operator's own A/B agree it does not move), and
        // `draw: cube fetch got the dummy` falls by ~33,600 on that route while
        // `draw: bound a REAL cube map` rises by the same amount.
        if (cubeFetch)
        {
            static const bool noReplicate = EnvOn("CZ_VK_NO_CUBE_REPLICATE");
            if (noReplicate)
            {
                Count("texture: CUBE fetch whose constant says otherwise — served the "
                      "dummy (CZ_VK_NO_CUBE_REPLICATE)");
                return 0;
            }
            Count("texture: CUBE fetch whose constant says otherwise — ONE FACE "
                  "replicated across six");
            cubeFromOneFace = true;
        }
    }

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
    // A CUBE MAP THE TITLE RENDERS ITSELF — assembled out of its six faces' resolve
    // snapshots, because guest memory at a resolve destination is known NOT to hold it.
    //
    // Exactly one of this title's cube maps is at an address this renderer holds a resolve
    // snapshot for: `06805000`, 64x64 `k_8_8_8_8`. The census settles what that means —
    // `up 1 (zero 1) <- uploaded BLACK, guest memory STILL zero` — so it is a dynamically
    // rendered environment map, and reading it out of guest memory reads nothing. That is
    // the Snapshot doctrine restated (gotcha 113): a resolve's pixels are never written
    // back, so for an address the GPU resolved to, guest memory is whatever the allocator
    // left.
    //
    // Part 25 could not serve the snapshot either — one snapshot is a 2D image in set 0 and
    // its slot number is meaningless in set 2 — so it declined to the dummy, which is what
    // that surface had had since phase 5. Part 26 builds the thing that was missing: SIX
    // snapshots copied into the six layers of one cube image in set 2 (CubeSnapshotSlot),
    // refreshed by each face's own resolve. Two same-binary arms survive the change:
    // `CZ_VK_NO_CUBE_SNAPSHOT=1` is the dummy, i.e. the pre-part-26 picture, and
    // `CZ_VK_CUBE_FROM_GUEST=1` is the zeros in guest memory.
    //
    // The decision is DEFERRED to just below the face-stride computation rather than taken
    // here, because the stride is what turns a base address into six face addresses and it
    // is derived from the format and extent a few dozen lines down.
    bool cubeAtResolveDest = false;
    if (cubeFetch && R->snapshots.count(t.address & 0x1FFFFFFF))
    {
        // NAMED, not just counted. A large count here has two completely different
        // readings — one cube map at an address that happens to have been resolved to
        // once, sampled every frame; or many of them — and only the address list separates
        // them.
        static std::vector<uint32_t> seenCubeSnap;
        const uint32_t a = t.address & 0x1FFFFFFF;
        if (std::find(seenCubeSnap.begin(), seenCubeSnap.end(), a) == seenCubeSnap.end())
        {
            seenCubeSnap.push_back(a);
            fprintf(stderr,
                    "[vk] cube fetch at %08X (%ux%u fmt=%u) names a RESOLVE DESTINATION — "
                    "the title renders this cube map itself and guest memory there is not "
                    "it\n",
                    a, t.width, t.height, t.format);
            // THE SNAPSHOT TABLE, ONCE, THE FIRST TIME THIS HAPPENS.
            //
            // The cube snapshot path needs one fact that nothing in this repo has ever
            // measured: WHERE the six faces land. A cube the title renders itself is six
            // resolves, and either they go to six addresses at a regular stride from this
            // base (in which case a face index is `(dest - base) / stride` and the fill is
            // mechanical) or they do not — and the design of the fix is different in each
            // case. Guessing the stride from the format and extent would be exactly the
            // recollection-over-census error part 25 made about the dimension field
            // (gotcha 244); the resolve destinations are already in a map, so print them.
            //
            // Sorted by address, with the extent, because the question is a pattern in the
            // gaps: six 64x64 entries 0x4000 apart is an answer, and one 64x64 entry
            // alone is a different answer that says the faces are somewhere else.
            std::vector<std::pair<uint32_t, const Snapshot*>> table;
            for (const auto& [k, s] : R->snapshots)
                table.emplace_back(k, &s);
            std::sort(table.begin(), table.end(),
                      [](const auto& x, const auto& y) { return x.first < y.first; });
            fprintf(stderr, "[vk] resolve-destination census at that moment (%zu):\n",
                    table.size());
            uint32_t prev = 0;
            for (const auto& [k, s] : table)
            {
                const uint32_t addr = k & 0x1FFFFFFF;
                fprintf(stderr, "[vk]   %08X %ux%u%s  +%08X from previous%s\n", addr,
                        s->image.width, s->image.height,
                        (k & kSnapshotDepthBit) ? " DEPTH" : "",
                        prev ? addr - prev : 0u,
                        addr == a ? "   <-- the cube fetch's own base" : "");
                prev = addr;
            }
        }
        static const bool cubeFromGuest = EnvOn("CZ_VK_CUBE_FROM_GUEST");
        if (!cubeFromGuest)
            cubeAtResolveDest = true;
        else
            Count("texture: CUBE fetch at a resolve destination, uploaded from guest "
                  "memory anyway (CZ_VK_CUBE_FROM_GUEST)");
    }
    if (!cubeFetch)
    {
        // Which snapshot of this address the fetch means, when there are two. The
        // guest says so in the fetch constant's own format field: `k_24_8` and
        // `k_24_8_FLOAT` are the two depth surface formats, and a pass sampling a
        // shadow cascade or a scene depth declares one of them. Fall back to the other
        // kind if only one exists, so a title that resolves depth to an address and
        // reads it back with a colour format still gets its pixels rather than nothing.
        const bool wantsDepth =
            t.format == xenos::kFmt_24_8 || t.format == xenos::kFmt_24_8_FLOAT;
        const uint32_t snapKey =
            (t.address & 0x1FFFFFFF) | (wantsDepth ? kSnapshotDepthBit : 0u);
        auto snap = R->snapshots.find(snapKey);
        if (snap == R->snapshots.end())
        {
            snap = R->snapshots.find((t.address & 0x1FFFFFFF) |
                                     (wantsDepth ? 0u : kSnapshotDepthBit));
            if (snap != R->snapshots.end())
                Count(wantsDepth
                          ? "texture: depth fetch served by a COLOUR resolve snapshot"
                          : "texture: colour fetch served by a DEPTH resolve snapshot");
        }
        if (g_texCensus)
        {
            // Keyed with the depth bit, for the same reason the snapshot map is
            // (gotcha 203): one address can be a colour surface and a depth one in the
            // same frame, and keyed on the address alone the two overwrite each other's
            // extent and format so the row shows whichever fetched last. 1439B000 is
            // exactly that — the tone map's output AND a shadow cascade — and the
            // aliased row read `1280x720 f6`, which is the colour use and says nothing
            // about the shadow map's real dimensions.
            TexSource& s = g_texSources[(t.address & 0x1FFFFFFF) |
                                        (wantsDepth ? kSnapshotDepthBit : 0u)];
            s.width = t.width;
            s.height = t.height;
            s.format = t.format;
            if (snap != R->snapshots.end())
            {
                s.everResolved = true;
                // maxAge is tracked on the SERVED path too, now that a snapshot is
                // served at any age: it is the number that says how far a fetch is
                // reaching back, and therefore the only visible sign if the guest ever
                // reuses a resolve destination for something else.
                s.maxAge = std::max(s.maxAge, R->frame - snap->second.frameSeen);
                if (SnapshotUsable(snap->second))
                    s.fromSnapshot++;
                else
                    s.snapshotTooOld++;
            }
        }
        // CZ_VK_NO_DEPTH_FETCH=1 — serve EVERY depth-format fetch the 1x1 white dummy.
        // An ARM, never a fix. It is the cheap way to ask whether a dark mark in the
        // picture comes from a DEPTH surface being sampled (a shadow, an occlusion term)
        // or from the surface's own texture — two investigations that look identical in
        // a screenshot (gotcha 173's rule, pointed at shading rather than geometry).
        //
        // It is deliberately NOT called "no shadow", because this title has two
        // consumers of depth fetches and the arm hits both: the shadow cascade AND the
        // scene depth the depth-of-field pass reads. Turning it on re-blurs the whole
        // frame exactly as the pre-part-14 renderer did, which is a second, independent
        // confirmation of §6ae — and a reminder to read what an arm actually disables
        // before reading a result off it. To isolate one consumer, name its address
        // with CZ_VK_SKIP_TEX instead.
        static const bool noDepthFetch = EnvOn("CZ_VK_NO_DEPTH_FETCH");
        if (noDepthFetch && wantsDepth)
        {
            Count("texture: depth fetch forced to the white dummy "
                  "(CZ_VK_NO_DEPTH_FETCH)");
            return 0;
        }
        if (snap != R->snapshots.end() && SnapshotUsable(snap->second))
        {
            Count(snap->second.fromDepth
                      ? "texture: served from a DEPTH resolve snapshot"
                      : "texture: served from a resolve snapshot");
            // MEASUREMENT ONLY, and the thing it measures is a real defect with a
            // quantitative fit — see docs/phase5-notes.md §6ao.
            //
            // A resolve snapshot's image is created at the destination surface's PITCH,
            // because that is what the resolve registers give. The fetch that samples it
            // declares the surface's REAL width, and a sampler normalises over the image
            // it is given — so whenever pitch != width, every texture coordinate is
            // scaled by width/pitch and everything past that fraction reads the padding,
            // which is zero. It is invisible while both are multiples of 32 (the whole
            // scene chain: 640, 320, 160) and it destroys the tail of this title's
            // luminance reduction, where the surfaces are 80, 40, 20, 10, 5 and 2 wide
            // in pitches of 96, 64, 32, 32, 32, 32. The lit-column counts of five
            // consecutive links match that model exactly.
            //
            // The measurement that decided the shape of the fix: 25,092 of 764,575
            // snapshot fetches in a boot mismatch (3.3%, ~12 a frame), and every one of
            // them is NARROWER than its snapshot. A dozen small copies a frame is
            // affordable; a general per-fetch scaling mechanism would not have been, and
            // the counter is what said which (gotcha 80). CZ_VK_NO_SNAPSHOT_VIEWS=1 is
            // the same-binary control arm.
            static const bool noViews = EnvOn("CZ_VK_NO_SNAPSHOT_VIEWS");
            if (t.width && t.height &&
                (t.width != snap->second.image.width ||
                 t.height != snap->second.image.height))
            {
                Count("texture: snapshot served at the surface PITCH, not the fetch's "
                      "declared size — texture coordinates would be scaled wrong");
                if (!noViews && t.width <= snap->second.image.width &&
                    t.height <= snap->second.image.height)
                {
                    const uint32_t slot = SnapshotViewSlot(snap->second, t.width, t.height);
                    if (slot)
                        return slot;
                    Count("texture: snapshot view could not be created — serving the "
                          "pitch-sized image, coordinates ARE scaled wrong");
                }
            }
            // The pass-inputs list keeps the depth bit, because "this pass sampled the
            // scene colour" and "this pass sampled the scene DEPTH" are the two
            // different answers the dependency graph exists to separate.
            const uint32_t key =
                (t.address & 0x1FFFFFFF) |
                (snap->second.fromDepth ? kSnapshotDepthBit : 0u);
            if (std::find(R->snapshotsSampledThisPass.begin(),
                          R->snapshotsSampledThisPass.end(),
                          key) == R->snapshotsSampledThisPass.end())
                R->snapshotsSampledThisPass.push_back(key);
            return snap->second.slot;
        }
        if (snap != R->snapshots.end())
            Count("texture: resolve snapshot too old, falling back to guest memory");
    }

    // CZ_VK_TEX_REFRESH=<hex[,hex...]> — re-read these textures' pixels on every fetch,
    // into the SAME image and the SAME bindless slot.
    //
    // The texture cache is keyed on the fetch constant's six dwords, which is right for
    // a texture that arrives from disc once. It is wrong for one the CPU keeps writing:
    // this title rasterizes its fonts into glyph atlases at runtime, so the atlas the
    // first draw sees is the atlas as it stood at that instant, and the key does not
    // change when the guest adds a glyph. This arm asks the question — point it at an
    // address and see whether the picture changes — without a general dirty-tracking
    // mechanism, which is a much larger piece of work and should not be built on a
    // hunch. The dimensions cannot have changed, because they are part of the key, so
    // updating in place is exact.
    // CZ_VK_TEX_REFRESH_ALL=1 — the same thing for EVERY texture, which is the arm the
    // operator's report needs: it makes the cache incapable of serving a stale image, at
    // a cost nobody would ship, so a picture taken under it is what the picture SHOULD
    // look like. If the wrong textures persist under this arm, the cache is not the
    // mechanism and the whole hypothesis below is dead.
    static const char* refreshEnv = Env("CZ_VK_TEX_REFRESH");
    static const bool refreshAll = EnvOn("CZ_VK_TEX_REFRESH_ALL");
    bool refresh = refreshAll;
    // The `snprintf` is INSIDE the test now. It used to run on every call whether or not
    // the instrument was on — ~13,900 string formats a frame charged to the `textures`
    // column that open-items 0a-ii is about. Exactly the part-20 psbind fix, in the one
    // place that was missed.
    if (refreshEnv && !refresh)
    {
        char addrHex[16];
        snprintf(addrHex, sizeof addrHex, "%08X", t.address);
        refresh = strstr(refreshEnv, addrHex) != nullptr;
    }

    auto cached = R->textures.find(key);
    if (cached != R->textures.end() && !refresh)
    {
        // THE GUARD: are the bytes this image was built from still the bytes at that
        // address? The key says the fetch constant is unchanged; only this says the
        // TEXTURE is. See the CZ_VK_TEX_GUARD comment for why the two differ.
        if ((g_texGuard || g_texRevalidate) && cached->second.srcBytes)
        {
            uint64_t read = 0;
            uint64_t g = StreamGuard(base + cached->second.va,
                                     size_t(cached->second.srcBytes), &read);
            // The poison perturbs only the COMPUTED guard, never the stored one, so
            // every hit is forced to mismatch and the census must read 100%.
            if (g_texGuardPoison)
                g ^= R->frame * 0x9E3779B97F4A7C15ull;
            g_texGuardStats.guardBytes += read;
            ++g_texGuardStats.hits;
            TexGuardAddr& a = g_texGuardAddrs[t.address & 0x1FFFFFFF];
            ++a.hits;
            a.width = t.width;
            a.height = t.height;
            a.format = t.format;
            if (g != cached->second.guard)
            {
                ++g_texGuardStats.changed;
                ++a.changed;
                Count("texture: cache hit but the GUEST BYTES CHANGED — this draw is "
                      "being served an image built from pixels that are gone");
                if (g_texRevalidate)
                    refresh = true;   // fall through to the in-place re-upload below
            }
        }
        if (!refresh)
        {
            Count("texture: cache hit");
            return cached->second.slot;
        }
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
    const uint64_t faceBytes = uint64_t(srcPitchUnits) * srcRows * bytesPerUnit;

    // A CUBE MAP IS SIX FACES, laid out one after another at the stride a single face
    // occupies. Everything above this line describes ONE of them: the fetch constant's
    // width and height are the face's, and its pitch is the face's pitch.
    //
    // The stride is `faceBytes` — the tiled footprint of one face, i.e. the pitch
    // rounded to 32 units by the rows rounded to 32 — and that is a MODEL, not a
    // quotation. It is the one the 2D path already computes, applied six times, and the
    // check on it is the census below plus `CZ_VK_TEX_DUMP`, which writes each face out
    // to be looked at. Say it out loud here so a wrong sky is traced to this line rather
    // than to the sampler: if the faces come out sheared or offset from each other, the
    // slice stride is the suspect and nothing else in this function is.
    const uint32_t layers = (shaderDim == 3) ? 6 : 1;
    // THE SOURCE STRIDE IS ZERO WHEN THE SIX FACES ARE ONE FACE. Everything downstream —
    // the bounds check, the content guard, the untile loop, the dump — is driven by these
    // two, so this is the whole of the replicate path and there is no second copy of the
    // untiler. `srcBytes` is what we READ, so it stays at one face: bounding it at six
    // would fail `GuestRangeOk` on a surface the guest only allocated one of, which is the
    // very reason declining looked like the safe option.
    const uint64_t faceSrcStride = cubeFromOneFace ? 0 : faceBytes;
    const uint64_t srcBytes = faceBytes + faceSrcStride * (layers - 1);

    // The rendered cube, now that the stride exists. Note it uses the SAME `faceBytes` the
    // guest-memory path uses, deliberately: if the two ever needed different strides one of
    // them would be wrong, and a rendered cube gives the model a second, independent check
    // — its six face addresses have to be six addresses the guest actually resolved to,
    // which CubeSnapshotSlot prints face by face.
    if (cubeAtResolveDest)
    {
        if (faceBytes > 0xFFFFFFFFull)
        {
            Count("texture: CUBE snapshot refused — face stride does not fit 32 bits");
            return 0;
        }
        return CubeSnapshotSlot(t, uint32_t(faceBytes));
    }

    const uint32_t va = PhysToVa(t.address);
    if (!GuestRangeOk(va, srcBytes))
    {
        Count(layers == 6 ? "texture: CUBE source outside the physical arena"
                          : "texture: source outside the physical arena");
        return 0;
    }

    // Untile (or copy) into a tightly packed staging image, swapping endianness as the
    // fetch constant asks. The destination is `unitW` wide because that is what the
    // Vulkan image is; the source is read at its own pitch.
    const uint64_t faceDstBytes = uint64_t(unitW) * unitH * bytesPerUnit;
    const uint64_t dstBytes = faceDstBytes * layers;
    std::vector<uint8_t> pixels(dstBytes);

    // The whole untile below is written for one face. Six faces is that loop six times
    // over, with both cursors advanced by their own stride — so the loop was lifted out
    // rather than the body being duplicated, and a 2D texture takes exactly the path it
    // always did with `face` fixed at 0.
    for (uint32_t face = 0; face < layers; face++)
    {
    const uint8_t* src = base + va + face * faceSrcStride;
    uint8_t* dstFace = pixels.data() + face * faceDstBytes;

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
        // THE OUT-OF-FOOTPRINT SKIP IS COUNTED, because a skipped unit is not a
        // no-op: `pixels` is zero-initialised, so the unit stays ZERO, and an all-zero
        // DXT1 block decodes to OPAQUE BLACK. The symptom is therefore black rectangles
        // scattered through an otherwise correct texture — the tiled swizzle interleaves,
        // so the units that fall outside are not a truncated bottom edge but a pattern
        // of blocks — and the operator sees it as "the Still Creek sign has smears on
        // it" while every counter in the renderer reads healthy. It was a bare
        // `continue` for the whole of phase 5 (gotcha 171: a counter behind an early
        // return counts the times the early return did not happen).
        uint64_t skipped = 0;
        for (uint32_t y = 0; y < unitH; y++)
            for (uint32_t x = 0; x < unitW; x++)
            {
                const uint32_t unit = Tiled2DOffset(x, y, srcPitchUnits, log2bpu);
                const uint64_t off = uint64_t(unit) * bytesPerUnit;
                // Bounded by ONE FACE, not by the whole source: `src` already points at
                // this face, so a unit past `faceBytes` would be read out of the next
                // face and produce a cube whose seams are each other's pixels.
                if (off + bytesPerUnit > faceBytes)
                {
                    ++skipped;
                    continue;
                }
                CopySwapped(&dstFace[(uint64_t(y) * unitW + x) * bytesPerUnit], src + off,
                            bytesPerUnit, t.endian);
            }
        Count("texture: untiled");
        if (skipped)
        {
            Count("texture: units left BLACK — tiled offset outside the footprint");
            static int left = 20;
            if (left-- > 0)
                fprintf(stderr,
                        "[vk] untile %08X %ux%u fmt=%u bpu=%u pitchUnits=%u "
                        "srcRows=%u srcBytes=%llu: %llu of %llu units (%.1f%%) fell "
                        "OUTSIDE the footprint and are left black\n",
                        t.address, t.width, t.height, t.format, bytesPerUnit,
                        srcPitchUnits, srcRows, (unsigned long long)srcBytes,
                        (unsigned long long)skipped,
                        (unsigned long long)(uint64_t(unitW) * unitH),
                        100.0 * double(skipped) / double(uint64_t(unitW) * unitH));
        }
    }
    else
    {
        for (uint32_t y = 0; y < unitH; y++)
            CopySwapped(&dstFace[uint64_t(y) * unitW * bytesPerUnit],
                        src + uint64_t(y) * srcPitchUnits * bytesPerUnit,
                        uint64_t(unitW) * bytesPerUnit, t.endian);
        Count("texture: linear");
    }
    } // for each cube face

    // THE MIP CHAIN, levels 1..n — the input this renderer declared and then discarded
    // for the whole of phase 5 (part 39).
    //
    // A Xenos fetch constant names TWO addresses. `t.address` holds level 0 and nothing
    // else; `t.mipAddress` holds levels 1..t.mipMax. Uploading a one-level image and
    // leaving the sampler's mipmapMode at LINEAR is not "no mipmapping is needed here",
    // it is "there is no level below 0 to select", so every minified surface in the game
    // has been sampling full-resolution texels at whatever rate the rasteriser happened
    // to land on. Item 00i — buildings that read as flat panels until you walk up to
    // them, which the R4 hardware traces show fully textured at every distance — is the
    // symptom that sent us looking, and the fetch constants say hardware has a chain
    // here on the majority of its fetches.
    //
    // WHERE EACH LEVEL LIVES, and every clause of this was checked against hardware's
    // own bytes rather than reasoned about (docs/phase5-notes.md 6bq):
    //   * level 1 starts at `mipAddress` exactly;
    //   * each subsequent level starts at the accumulated TILED FOOTPRINT of the levels
    //     before it — its own pitch rounded up to 32 units by its own rows rounded up to
    //     32, which for a level smaller than one tile is a whole tile;
    //   * a level's pitch is derived from ITS OWN width, not from the base level's
    //     `pitchBlocks`, which describes level 0 only.
    // Decoded out of the R4 trace, level 1 of a 256x64 sign and levels 1..2 of a 512x512
    // wall are clean half- and quarter-size copies of their base — same mean colour,
    // steadily fewer distinct colours, which is what a mip chain looks like and what a
    // wrong offset does not.
    //
    // WHERE IT STOPS, and it stops LOUDLY. `packedMips` says the tail of the chain —
    // the levels of texel extent <= 16 — shares one tile at sub-tile offsets. From
    // part 39 to part 41 those were declined wholesale; part 41 DERIVED the square
    // DXT offsets from hardware's own bytes (the block comment at the tail check in
    // the loop below carries the census), so square DXT1/DXT5 tail levels down to
    // 4x4 texels are taken now. Everything the census could not derive — non-square
    // tails, other formats, sub-block levels — is still DECLINED AND COUNTED rather
    // than guessed at, because a guessed low mip is a wrong colour on a distant
    // surface, which is indistinguishable from the defect being fixed (gotcha 5).
    //
    // CZ_VK_NO_MIPS=1 uploads level 0 alone — the pre-part-39 renderer, same binary.
    static const bool noMips = EnvOn("CZ_VK_NO_MIPS");
    std::vector<VkBufferImageCopy> copies;
    {
        VkBufferImageCopy c0{};
        c0.bufferOffset = 0;
        c0.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers };
        c0.imageExtent = { t.width, t.height, 1 };
        copies.push_back(c0);
    }
    uint32_t levelCount = 1;
    if (!noMips && layers == 1 && t.mipAddress && t.mipMax >= 1)
    {
        const uint32_t mipVa = PhysToVa(t.mipAddress);
        uint64_t chainOff = 0;      // byte offset of the level being read, from mipAddress
        for (uint32_t level = 1; level <= t.mipMax && level < 16; level++)
        {
            const uint32_t lw = std::max(1u, t.width >> level);
            const uint32_t lh = std::max(1u, t.height >> level);
            const uint32_t luW = (lw + blockDim - 1) / blockDim;
            const uint32_t luH = (lh + blockDim - 1) / blockDim;
            const uint32_t lPitch = t.tiled ? ((luW + 31) & ~31u) : luW;
            const uint32_t lRows = t.tiled ? ((luH + 31) & ~31u) : luH;
            const uint64_t lFootprint = uint64_t(lPitch) * lRows * bytesPerUnit;
            // THE PACKED TAIL (part 41, item 2). Levels of texel extent <= 16 share
            // ONE tile at the accumulated offset, each at a block offset inside it.
            // The offsets were BRUTE-FORCED against hardware's own bytes, not taken
            // from a remembered table (tools/packed_mip_derive.py over all eight R4
            // traces): a square level of width W blocks sits at block (W, 0) — the
            // 16-texel level at (4,0), 8 at (2,0), 4 at (1,0) — with 7,466 of 7,515
            // informative votes agreeing across DXT1 and DXT5. What the census could
            // NOT derive is declined and counted exactly as the whole tail was
            // before: non-square tail levels (9 votes, inconsistent), formats other
            // than DXT1/DXT5 (never sampled by the scorer), and sub-block levels
            // (below 4 texels the offset cannot be block-aligned at all).
            // CZ_VK_NO_MIP_TAIL=1 is the tail-only same-binary arm: isTail never
            // fires, so the walk reads the tail tile at (0,0) and advances past it,
            // which is byte-for-byte the part-39/40 behaviour (the chain then ends
            // on the mostly-empty or divergence check below). CZ_VK_NO_MIPS=1
            // remains the whole-feature arm.
            static const bool noTail = EnvOn("CZ_VK_NO_MIP_TAIL");
            const bool isTail = !noTail && t.tiled && std::max(lw, lh) <= 16u;
            uint32_t tailBlockX = 0;
            if (isTail)
            {
                if (t.format != xenos::kFmt_DXT1 && t.format != xenos::kFmt_DXT4_5)
                {
                    Count("mip: packed tail UNDERIVED for this format — chain ends");
                    break;
                }
                if (lw != lh)
                {
                    Count("mip: packed tail NON-SQUARE — underived, chain ends");
                    break;
                }
                if (lw < 4)
                {
                    Count("mip: sub-block tail level — chain ends");
                    break;
                }
                tailBlockX = luW;
            }
            if (!GuestRangeOk(mipVa + uint32_t(chainOff), lFootprint))
            {
                Count("mip: level source outside the physical arena");
                break;
            }
            // Append this level's untiled pixels to the same staging image the base
            // level went into; the copy regions below name where each one starts.
            const uint64_t lDstBytes = uint64_t(luW) * luH * bytesPerUnit;
            const size_t at = pixels.size();
            pixels.resize(at + size_t(lDstBytes));
            const uint8_t* lsrc = base + mipVa + chainOff;
            uint8_t* ldst = pixels.data() + at;
            if (t.tiled)
            {
                uint32_t log2bpu = 0;
                while ((1u << log2bpu) < bytesPerUnit)
                    ++log2bpu;
                for (uint32_t y = 0; y < luH; y++)
                    for (uint32_t x = 0; x < luW; x++)
                    {
                        // tailBlockX shifts a packed-tail level to its derived
                        // position inside the shared tile; 0 for unpacked levels.
                        const uint64_t off =
                            uint64_t(Tiled2DOffset(tailBlockX + x, y, lPitch,
                                                   log2bpu)) * bytesPerUnit;
                        if (off + bytesPerUnit > lFootprint)
                            continue;
                        CopySwapped(&ldst[(uint64_t(y) * luW + x) * bytesPerUnit],
                                    lsrc + off, bytesPerUnit, t.endian);
                    }
            }
            else
            {
                for (uint32_t y = 0; y < luH; y++)
                    CopySwapped(&ldst[uint64_t(y) * luW * bytesPerUnit],
                                lsrc + uint64_t(y) * lPitch * bytesPerUnit,
                                uint64_t(luW) * bytesPerUnit, t.endian);
            }
            // IS THERE ACTUALLY A LEVEL HERE? Count the blocks that came back non-empty.
            //
            // This replaces the extent rule the first implementation stopped on — "break
            // at the first level narrower than a macro tile" — which threw away levels
            // that are demonstrably present, and they are the ones distant geometry needs
            // most. Read out of hardware's own chains, a 512x512 DXT1's levels 1..4 sit at
            // the ACCUMULATED FULL-TILE offsets this loop already computes, and each tile
            // contains exactly that level's block count — 4096, 1024, 256, 64 — with the
            // luma holding (83.7, 82.2, 80.1, 76.0, 71.0) and distinct blocks falling
            // monotonically. Two independent textures agree clause for clause. Level 5 is
            // where the genuinely packed tail starts, and there the count stops matching:
            // one chain reads 27 blocks where 16 are expected, another 533.
            //
            // So the terminator is THE DATA, not the extent. A level whose tile comes back
            // mostly empty is padding or somebody else's, and it ends the chain. That is
            // self-limiting per texture, which a fixed extent threshold cannot be — the
            // packed tail begins at a different level depending on how the guest laid the
            // texture out, so this asks each chain where its own tail starts.
            //
            // It runs BEFORE the divergence guard below deliberately: a mostly-empty tile
            // also reads as "diverges from the level above", and calling that a rule
            // violation would hide an ordinary end-of-chain behind an alarm.
            {
                size_t nonEmpty = 0;
                for (size_t o = 0; o + bytesPerUnit <= size_t(lDstBytes); o += bytesPerUnit)
                {
                    bool zero = true;
                    for (uint32_t k = 0; k < bytesPerUnit; k++)
                        if (ldst[o + k]) { zero = false; break; }
                    if (!zero)
                        ++nonEmpty;
                }
                if (nonEmpty * 2 < size_t(luW) * luH)
                {
                    Count("mip: PACKED TAIL REACHED — level mostly empty, chain ends here");
                    pixels.resize(at);
                    break;
                }
            }

            // IS THIS LEVEL PLAUSIBLY THE SAME PICTURE, one octave down?
            //
            // The offset rule above was verified by hand against exactly TWO of
            // hardware's chains. That is enough to believe it and not enough to ship it
            // silently: a wrong offset serves a neighbouring texture's bytes as a mip
            // level, and the symptom — a distant surface in the wrong colour — is
            // indistinguishable from the defect this whole change is aimed at, so it
            // would be invisible in exactly the measurement meant to judge it.
            //
            // The check is the same INVARIANT the layout was confirmed with (§6bq): a
            // correct level holds the same average as the level above it, with fewer
            // distinct colours. For DXT1/DXT5 the two RGB565 colour ENDPOINTS of each
            // block are that average cheaply — the 2-bit indices are near-uniform noise
            // and would swamp a plain byte mean, which is why this reads endpoints and
            // not bytes. It REJECTS (see below); it began life as a counter, and the
            // first thing it counted was a real defect.
            if (t.format == xenos::kFmt_DXT1 || t.format == xenos::kFmt_DXT4_5)
            {
                const uint32_t blockBytes = (t.format == xenos::kFmt_DXT1) ? 8u : 16u;
                const uint32_t endpointAt = (t.format == xenos::kFmt_DXT1) ? 0u : 8u;
                auto endpointLuma = [&](const uint8_t* p, size_t bytes) {
                    double sum = 0;
                    size_t n = 0;
                    for (size_t off = 0; off + blockBytes <= bytes; off += blockBytes)
                        for (uint32_t e = 0; e < 2; e++)
                        {
                            const uint32_t c565 = p[off + endpointAt + e * 2] |
                                                  (p[off + endpointAt + e * 2 + 1] << 8);
                            sum += ((c565 >> 11) & 31) * (255.0 / 31) * 0.299 +
                                   ((c565 >> 5) & 63) * (255.0 / 63) * 0.587 +
                                   (c565 & 31) * (255.0 / 31) * 0.114;
                            ++n;
                        }
                    return n ? sum / double(n) : -1.0;
                };
                const size_t prevAt = copies.back().bufferOffset;
                const double prev = endpointLuma(pixels.data() + prevAt, at - prevAt);
                const double cur = endpointLuma(ldst, size_t(lDstBytes));
                if (prev >= 0 && cur >= 0 && std::fabs(prev - cur) > 32.0)
                {
                    // AND IT REJECTS, because the guard found real failures the moment
                    // it ran: eight textures on the outdoor route whose level 1 reads
                    // about a THIRD of its base's luma, every one of them a surface
                    // whose level 1 is narrower than a tile. A consistent factor rather
                    // than noise says we are reading a sparse scatter of a tightly
                    // packed level at the wrong pitch — i.e. the accumulation rule does
                    // not describe these shapes, exactly as `packedMips` warns. Binding
                    // them anyway would paint distant small-textured surfaces too dark,
                    // which is the defect class this change exists to fix.
                    //
                    // Dropping the level and stopping the chain is the conservative
                    // answer: this texture keeps the levels that passed, and the ones
                    // below are declined like any other packed tail.
                    Count("mip: level REJECTED — diverges from the level above");
                    static int left = 8;
                    if (left-- > 0)
                        fprintf(stderr,
                                "[vk] mip %08X %ux%u fmt=%u level %u: endpoint luma "
                                "%.1f vs %.1f one level up — this level is probably not "
                                "this texture, dropping it and the rest of the chain. "
                                "chain=%08X off=%llu\n",
                                t.address, t.width, t.height, t.format, level, cur, prev,
                                t.mipAddress, (unsigned long long)chainOff);
                    pixels.resize(at);
                    break;
                }
            }
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            c.imageExtent = { lw, lh, 1 };
            copies.push_back(c);
            levelCount = level + 1;
            // The engagement evidence: a tail level that passed both guards and is
            // in the upload. Without this the whole change is invisible in a log
            // (gotcha 151 — and 308's alpha-test counter sat at zero for two parts
            // because nobody could read a counter that did not exist).
            if (isTail)
                Count("mip: packed tail level TAKEN");
            // Tail levels SHARE their tile, so the walk must not advance past it —
            // advancing per level was exactly what made the pre-part-41 read land on
            // empty blocks and end every chain at "PACKED TAIL REACHED".
            if (!isTail)
                chainOff += lFootprint;
        }
        Count(levelCount > 1 ? "mip: chain uploaded" : "mip: chain declared but no level taken");
    }
    else if (!noMips && layers == 6 && t.mipMax >= 1)
    {
        // A cube map's chain is six chains, and the face stride for the mip levels is a
        // second model on top of the one the base level already assumes. Not attempted,
        // and counted so the omission is a number rather than a silence.
        Count("mip: CUBE chain not uploaded");
    }

    if (g_texCensus)
    {
        TexSource& s = g_texSources[t.address & 0x1FFFFFFF];
        s.width = t.width;
        s.height = t.height;
        s.format = t.format;
        s.uploads++;
        s.src = base + va;   // the whole source, i.e. all six faces for a cube map
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

    // CZ_VK_MIP_TINT=1 — replace every uploaded chain level's blocks with a solid
    // colour code (L1 red, L2 green, L3 blue, L4 yellow, L5 magenta, L6 cyan, deeper
    // white), level 0 untouched. The picture then names the mip level every surface
    // samples, which no amount of reasoning about gradients can (part 44: data
    // verified correct at every level, all bias fields zero on both platforms, and
    // the flat-at-range class still there — the next fact needed is WHICH level the
    // flat wall actually reads). Diagnostic arm only; DXT1/DXT5 formats only, others
    // left untinted and counted.
    static const bool mipTint = EnvOn("CZ_VK_MIP_TINT");
    if (mipTint && (t.format == xenos::kFmt_DXT1 || t.format == xenos::kFmt_DXT4_5))
    {
        static const uint16_t kC565[7] = { 0xF800, 0x07E0, 0x001F, 0xFFE0,
                                           0xF81F, 0x07FF, 0xFFFF };
        for (size_t ci = 1; ci < copies.size(); ci++)
        {
            const VkBufferImageCopy& c = copies[ci];
            if (c.imageSubresource.mipLevel == 0)
                continue;
            const uint16_t col =
                kC565[std::min<uint32_t>(c.imageSubresource.mipLevel - 1, 6)];
            const uint32_t luW2 = (c.imageExtent.width + blockDim - 1) / blockDim;
            const uint32_t luH2 = (c.imageExtent.height + blockDim - 1) / blockDim;
            const uint64_t bytes = uint64_t(luW2) * luH2 * bytesPerUnit;
            if (c.bufferOffset + bytes > pixels.size())
                continue;
            uint8_t* p = pixels.data() + c.bufferOffset;
            for (uint64_t o = 0; o + bytesPerUnit <= bytes; o += bytesPerUnit)
            {
                uint8_t* b = p + o;
                if (bytesPerUnit == 16)
                {
                    b[0] = b[1] = 0xFF;                  // solid alpha
                    memset(b + 2, 0, 6);
                    b += 8;
                }
                b[0] = uint8_t(col & 0xFF);
                b[1] = uint8_t(col >> 8);
                b[2] = uint8_t(col & 0xFF);
                b[3] = uint8_t(col >> 8);
                memset(b + 4, 0, 4);                     // indices -> colour 0
            }
        }
        Count("texture: mip levels TINTED (CZ_VK_MIP_TINT)");
    }

    // CZ_VK_TEX_DUMP=<dir> plus CZ_VK_TEX_DUMP_ADDR=<hex[,hex]> — write the UNTILED
    // bytes of those textures out, once per upload: a greyscale PGM for an 8-bit
    // texture, and a raw .bin of the block payload for everything else.
    //
    // It is the only way to separate "our untiling scrambled this texture" from "the
    // texture is fine and the draw samples it wrong", and those are different
    // subsystems. A font atlas is the ideal subject: a human can tell a sheet of
    // glyphs from a sheet of noise instantly, which no aggregate over it can.
    //
    // THE .bin PATH IS PART 40's, AND THE GAP IT CLOSES IS WHY IT IS WORTH A COMMENT.
    // For its whole life this instrument was gated on `bytesPerUnit == 1`, i.e. it could
    // only ever dump an 8-bit texture — and this title is DXT almost everywhere (of the
    // 23-frame outdoor census, every foliage, building and character texture is fmt 18 or
    // 20). So the one instrument whose stated purpose is "did our untiling scramble this"
    // was blind to the formats that carry the picture, and part 39 answered a tree
    // question by INFERENCE from a screenshot because of it. The block payload is
    // untiled and endian-swapped by the loop above exactly as the sampler will see it,
    // which is what makes the dump decodable offline by tools/tex_decode.py with neither
    // --tiled nor --swap16 — those two are for raw guest memory, and this is not that.
    static const char* texDumpDir = Env("CZ_VK_TEX_DUMP");
    static const char* texDumpAddr = Env("CZ_VK_TEX_DUMP_ADDR");
    // Formatted here rather than at the top of the function: this is the one call site
    // that needs it and it is behind the instrument's own gate, so a run without
    // CZ_VK_TEX_DUMP does not format a string per fetch.
    char dumpAddrHex[16] = {};
    if (texDumpDir && texDumpAddr)
        snprintf(dumpAddrHex, sizeof dumpAddrHex, "%08X", t.address);
    if (texDumpDir && (!texDumpAddr || strstr(texDumpAddr, dumpAddrHex)))
    {
        // ONE FILE PER FACE for a cube map, named by face index. A cube written as one
        // tall strip would be unreadable exactly where it matters — the question a dump
        // of a cube answers is "is face 3 the same sky as face 2, or is it face 2 shifted
        // by the slice stride", and that needs six pictures side by side.
        //
        // The name carries the TEXEL extent and the format for a .bin, because that is
        // what a decoder needs and neither is recoverable from the byte count alone: a
        // 16 KB DXT5 payload is 256x64 or 128x128 or 512x16, and guessing wrong produces
        // a plausible picture of the wrong thing (gotcha 302's failure mode exactly).
        // The PGM keeps the UNIT extent it always had, since that is its own geometry.
        for (uint32_t face = 0; face < layers; face++)
        {
            char path[512];
            const char* faceSuffix = (layers == 6) ? "_face" : "";
            char faceNum[16] = {};
            if (layers == 6)
                snprintf(faceNum, sizeof faceNum, "%u", face);
            if (bytesPerUnit == 1)
                snprintf(path, sizeof path, "%s/tex_%08X_%ux%u%s%s.pgm", texDumpDir,
                         t.address, unitW, unitH, faceSuffix, faceNum);
            else
                snprintf(path, sizeof path, "%s/tex_%08X_%ux%u_fmt%u%s%s.bin", texDumpDir,
                         t.address, t.width, t.height, t.format, faceSuffix, faceNum);
            // LEVEL 0 ONLY, and bounded. `pixels` grew a mip chain in part 39, so the
            // face stride no longer spans the buffer and an unbounded write here would
            // spill the chain into the file — a dump that decodes as a texture with
            // garbage past the first level, which is exactly the kind of artifact this
            // instrument exists to rule out rather than create.
            const uint64_t at = uint64_t(face) * faceDstBytes;
            const uint64_t n = (at + faceDstBytes <= pixels.size())
                                   ? faceDstBytes
                                   : (at < pixels.size() ? pixels.size() - at : 0);
            if (FILE* f = n ? fopen(path, "wb") : nullptr)
            {
                if (bytesPerUnit == 1)
                    fprintf(f, "P5\n%u %u\n255\n", unitW, unitH);
                fwrite(pixels.data() + at, 1, size_t(n), f);
                fclose(f);
            }
        }
        Count("texture: dumped for CZ_VK_TEX_DUMP");
        // Part 44: ALSO write each uploaded chain level, one file per level, named
        // with its own texel extent. The flat-at-range hunt needs to see the bytes
        // the SAMPLER sees at each level — the guest chain was verified correct for
        // every texture checked, so the remaining data suspect is this staging
        // buffer, and only a dump of it can clear (or convict) the upload.
        for (size_t ci = 1; ci < copies.size(); ci++)
        {
            const VkBufferImageCopy& c = copies[ci];
            if (c.imageSubresource.baseArrayLayer != 0 || c.imageSubresource.mipLevel == 0)
                continue;
            const uint32_t lw = c.imageExtent.width, lh = c.imageExtent.height;
            const uint32_t luW2 = (lw + blockDim - 1) / blockDim;
            const uint32_t luH2 = (lh + blockDim - 1) / blockDim;
            const uint64_t bytes = uint64_t(luW2) * luH2 * bytesPerUnit;
            if (c.bufferOffset + bytes > pixels.size())
                continue;
            char path[512];
            snprintf(path, sizeof path, "%s/tex_%08X_L%u_%ux%u_fmt%u.bin", texDumpDir,
                     t.address, c.imageSubresource.mipLevel, lw, lh, t.format);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(pixels.data() + c.bufferOffset, 1, size_t(bytes), f);
                fclose(f);
            }
        }
    }

    // The refresh arm: same image, same slot, new pixels. No allocation, so it can run
    // every fetch without exhausting the bindless heap.
    if (refresh && cached != R->textures.end())
    {
        // Re-stamp the guard from the bytes we have just read, or a revalidating run
        // re-uploads this texture on every single fetch for the rest of the run —
        // which would read as "the fix is ruinously slow" when what is slow is the
        // instrument never being satisfied.
        cached->second.va = va;
        cached->second.srcBytes = srcBytes;
        cached->second.guard = StreamGuard(base + va, size_t(srcBytes), nullptr);
        ++g_texGuardStats.reuploaded;
        if (pixels.size() <= R->staging.size)
        {
            memcpy(R->staging.mapped, pixels.data(), pixels.size());
            Image& img = cached->second.image;
            // A REFRESH WRITES EVERY LEVEL THE IMAGE HAS, not just the base. The cached
            // image was built with whatever level count its first upload could locate,
            // and a re-upload that refilled level 0 alone would leave the levels below
            // it holding the bytes the recycled address used to carry — which is
            // precisely the stale-texture class part 38 closed, reintroduced one mip
            // down where nothing close up would ever show it.
            std::vector<VkBufferImageCopy> use(
                copies.begin(),
                copies.begin() + std::min<size_t>(copies.size(), img.levels));
            RunImmediate([&](VkCommandBuffer cb) {
                Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_ASPECT_COLOR_BIT);
                vkCmdCopyBufferToImage(cb, R->staging.buffer, img.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       uint32_t(use.size()), use.data());
                Barrier(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_ASPECT_COLOR_BIT);
            });
            Count("texture: refreshed in place (CZ_VK_TEX_REFRESH)");
        }
        return cached->second.slot;
    }

    // Set 2 has its own array of TextureCube views and therefore its own slot space; the
    // two counters are never interchangeable, because a slot number is only meaningful
    // against the heap it was allocated from.
    const bool isCube = layers == 6;
    uint32_t& nextSlot = isCube ? R->nextCubeSlot : R->nextTextureSlot;
    if (nextSlot >= g_maxDescriptors)
    {
        Count(isCube ? "texture: CUBE bindless heap full" : "texture: bindless heap full");
        return 0;
    }

    TextureEntry entry;
    entry.key = key;
    entry.slot = nextSlot++;
    entry.layers = layers;
    // The content this image is about to be built from, alongside the descriptor it is
    // keyed on. See TextureEntry for why the cache needs both.
    entry.va = va;
    entry.srcBytes = srcBytes;
    entry.guard = StreamGuard(base + va, size_t(srcBytes), nullptr);
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
        // AND UNIFORM, WHICH IS NOT THE SAME QUESTION AND HAD NO COUNTER.
        //
        // Part 26 chased a flat (180,180,180) ground for a session with a counter that
        // could only see BLACK uploads. A texture that decodes to one constant colour —
        // white, grey, anything — is exactly as broken as one that decodes to zero, and it
        // produces precisely the symptom being chased: a large surface lit correctly,
        // shadowed correctly, and carrying no detail at all. Uniformity is tested on the
        // payload as uploaded, which works for the compressed formats too: a DXT1 image
        // whose every 8-byte block is identical IS a single-colour image.
        bool uniform = !pixels.empty();
        for (size_t i = 8; uniform && i < pixels.size(); i++)
            if (pixels[i] != pixels[i % 8])
                uniform = false;
        if (uniform && !allZero)
        {
            Count("texture: uploaded a SINGLE REPEATED BLOCK — one flat colour");
            static int left = 12;
            if (left-- > 0)
                fprintf(stderr,
                        "[vk] texture %08X %ux%u fmt=%u uploaded UNIFORM: every block is "
                        "%02X%02X%02X%02X%02X%02X%02X%02X — this surface can only render "
                        "one flat colour\n",
                        t.address, t.width, t.height, t.format, pixels[0], pixels[1],
                        pixels[2], pixels[3], pixels[4], pixels[5], pixels[6], pixels[7]);
        }
        if (allZero)
        {
            Count("texture: uploaded entirely BLACK (the guest has not written it)");
            // A BLACK CUBE MAP IS ITS OWN CASE, and it gets its own line with its address.
            // The aggregate above sits at ~250 in a long run, so a cube joining it moves a
            // number nobody would notice — and a black cube map is a whole surface class
            // losing its reflection, not one 16x16 icon. `01330000` (4x4) is one:
            // `uploaded BLACK, guest memory is NON-ZERO NOW`, i.e. the texture arrived
            // after our single upload and the fetch-constant cache froze it black.
            if (layers == 6)
                fprintf(stderr,
                        "[vk] cube %08X %ux%u fmt=%u uploaded ENTIRELY BLACK — every "
                        "reflection sampling it is dead\n",
                        t.address, t.width, t.height, t.format);
        }
    }
    // Six array layers plus CUBE_COMPATIBLE, viewed as a VK_IMAGE_VIEW_TYPE_CUBE, which
    // is what set 2's `TextureCube[]` binding requires — a 2D-array view in that heap is
    // a validation error, not a wrong picture. Vulkan's layer order is +X,-X,+Y,-Y,+Z,-Z
    // and so is D3D's, so the guest's face order carries across untouched.
    if (!CreateImage(entry.image, t.width, t.height, format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     isCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, layers, 1,
                     noSwizzle ? VkComponentMapping{} : XenosSwizzle(t.swizzle),
                     levelCount))
    {
        Count("texture: image creation failed");
        --nextSlot;
        return 0;
    }
    NameImage(entry.image, "texture %08X %ux%u fmt=%u %s slot %u", t.address, t.width,
              t.height, t.format, isCube ? "CUBE" : "2D", entry.slot);

    // Stage through the upload buffer. Sized once at init; a texture larger than it
    // is counted and dropped rather than silently truncated.
    if (pixels.size() > R->staging.size)
    {
        Count(isCube ? "texture: CUBE larger than the staging buffer"
                     : "texture: larger than the staging buffer");
        --nextSlot;
        return 0;
    }
    ++R->guestTexturesThisPass;
    memcpy(R->staging.mapped, pixels.data(), pixels.size());

    RunImmediate([&](VkCommandBuffer cb) {
        Barrier(cb, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        // One region per mip level, and the base level's region names all six faces of a
        // cube: the staging buffer holds them tightly packed and in order, which is
        // exactly what a multi-layer copy expects.
        vkCmdCopyBufferToImage(cb, R->staging.buffer, entry.image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(copies.size()), copies.data());
        Barrier(cb, entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
    });

    VkDescriptorImageInfo ii{};
    ii.imageView = entry.image.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = R->sets[isCube ? 2 : 0];
    w.dstBinding = 0;
    w.dstArrayElement = entry.slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, nullptr);

    const uint32_t slot = entry.slot;
    R->textures.emplace(key, std::move(entry));
    if (isCube)
    {
        Count("texture: CUBE MAP uploaded (six faces)");
        static int left = 8;
        if (left-- > 0)
            fprintf(stderr,
                    "[vk] cube %08X %ux%u fmt=%u tiled=%u pitchBlk=%u faceBytes=%llu "
                    "-> set 2 slot %u\n",
                    t.address, t.width, t.height, t.format, t.tiled ? 1u : 0u,
                    t.pitchBlocks, (unsigned long long)faceBytes, slot);
    }
    else
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
    // CZ_VK_DEPTH_ALWAYS=1 — the arm that CZ_VK_NO_DEPTH_TEST above cannot be.
    //
    // Vulkan ties depth WRITES to the depth TEST: with `depthTestEnable` false the
    // attachment is not written at all, whatever `depthWriteEnable` says. So on a
    // DEPTH-ONLY pass — this title's shadow cascades are exactly that — the no-test arm
    // does not "draw everything regardless of depth", it produces an entirely empty
    // buffer. Part 32 ran it expecting to separate "never submitted" from "submitted and
    // rejected" and got 100% zero, which is the very symptom under investigation: the
    // arm's failure mode is indistinguishable from the defect it was aimed at.
    //
    // Keeping the test enabled and forcing the comparison to ALWAYS makes the same
    // distinction and keeps the writes. If a region fills under this arm and not under
    // the null, the geometry WAS submitted and the depth already in the buffer rejected
    // it — which for a region nothing ever cleared means it is being tested against the
    // zero the image was created with.
    static const bool depthAlways = EnvOn("CZ_VK_DEPTH_ALWAYS");
    if (depthAlways)
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
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
    // BLENDING OFF for the ID pass: an ID is a number, and a blended number is a
    // different number.
    //
    // THE COLOUR WRITE MASK IS DELIBERATELY *NOT* TOUCHED, and the first version of this
    // forced it open — which was wrong in a way the instrument itself revealed. The
    // depth-only prepass draws this title issues carry `mask=0`; with the mask forced
    // open they painted their indices over 31.5% of the map and the top three "visible"
    // draws were all draws that write no colour at all. An ID map is a map of what was
    // PAINTED, so a draw that writes no colour must write no ID (gotcha 30: the check
    // that catches this is looking at the instrument's own first output and asking
    // whether it could be wrong).
    cb.blendEnable = !key.drawIdPass && !(cb.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
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
    // THE DRAW-ID PASS substitutes its own fragment stage. Everything else about the
    // pipeline is left exactly as the draw would normally have it — same vertex shader,
    // same vertex input, same depth test and write, same cull — so the ID image has the
    // SAME VISIBILITY as the picture it is explaining. Change any of that and the map
    // stops describing the frame it is supposed to describe.
    stages[1].module = key.drawIdPass ? R->drawIdModule : ps.module;
    stages[1].pName = "main";

    // g_SpecConstants (constant_id 0) on the FRAGMENT stage. Only the alpha-test bit is
    // driven today; every other bit stays 0, which is byte-identical to the pre-part-38
    // default (no VkSpecializationInfo at all == every spec constant at its declared
    // default of 0), so pipelines without alpha test are unchanged.
    const uint32_t specValue = key.alphaTest ? 0x2u /* SPEC_CONSTANT_ALPHA_TEST */ : 0u;
    const VkSpecializationMapEntry specMap{ 0, 0, sizeof(uint32_t) };
    VkSpecializationInfo specInfo{};
    specInfo.mapEntryCount = 1;
    specInfo.pMapEntries = &specMap;
    specInfo.dataSize = sizeof specValue;
    specInfo.pData = &specValue;
    if (key.alphaTest && !key.drawIdPass)
        stages[1].pSpecializationInfo = &specInfo;

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

    // TIMED, and counted per reporting window, because this is the one thing inside
    // `other` that costs MILLISECONDS rather than nanoseconds — and an operator session
    // spent an evening making that matter.
    //
    // `other` (DoDraw's untimed work) sat at ~6% of a crowd frame all session and then
    // spiked to 25.8% — 16.7 ms a frame — on first arrival at the gas station, at the
    // SAME draw count where another area cost 3.1 ms. Revisiting the same spot later
    // read 6.1-6.3% across six consecutive windows, so the cost is first-visit-only and
    // does not recur: something is built once and then reused. Pipeline creation is the
    // only candidate in this function with that shape, and the arithmetic fits (~5 new
    // pipelines a frame at ~3 ms each is ~15 ms against 16.7 measured).
    //
    // It was inferred three times and never measured, and the inference then FAILED a
    // pre-registered prediction at the casino — `other` stayed flat where new material
    // should have spiked it. That could be rescued ("no new shaders loaded there"), but
    // new pipelines come from new STATE combinations too, so the rescue is unfalsifiable
    // and the honest response is this counter rather than a fourth argument. Gotcha 30:
    // a hypothesis that has not been given a way to fail is not evidence.
    //
    // Cost when the profile is off: one bool test on a path that already builds a whole
    // VkGraphicsPipelineCreateInfo, i.e. nothing. This is not a hot path — that is the
    // entire hypothesis.
    const uint64_t t0 = ProfNow();
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r =
        vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (g_profileOn)
    {
        g_prof.pipelineNs += ProfNow() - t0;
        g_prof.pipelinesCreated++;
    }
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
// GROW THE ARENA IF A FRAME OVERRAN IT.
//
// CALLED AT THE END OF A FRAME, FROM `DoSwapImpl`, AND THAT PLACEMENT IS THE POINT.
// It used to live at the top of `BeginFrame`, which is called from inside `DoDraw` —
// so every growth charged its `vkDeviceWaitIdle`, its allocation and its map to the
// draw path's `other` column. The operator session measured one such frame at 29.8%
// of a frame in `other`, the largest single spike of the session, and the mechanism
// was the CALL GRAPH rather than anything about drawing.
//
// This is a MEASUREMENT fix before it is a performance one, and saying which matters:
// the work still happens, it is just charged to the frame boundary where it belongs
// instead of to the draw that happened to be first. The one real saving is the
// `vkDeviceWaitIdle` below, which here follows a fence wait that has already idled the
// device and so costs nothing, where at the top of a frame it was a genuine stall.
//
// WHY THE END OF A FRAME IS SAFE, which is the whole question. The old comment said
// "here is safe and nowhere else is: the command buffer has just been reset, which is
// only legal once its previous submission has completed". The end of `DoSwapImpl` meets
// that condition more directly — the swap has waited on the fence, so the
// submission is not merely complete, it has been observed to be. `R->recording` is
// false, nothing holds a device address into the old buffer, and the arena's consumers
// are all per-frame (the stream cache is cleared in `BeginFrame`; the constants are
// reached through device addresses recorded in the push constants of the frame that has
// just finished executing).
//
// The history the size is about: the arena was a fixed 128 MB, `ArenaAlloc` SKIPS every
// draw it cannot satisfy, and this title's post-process chain is at the END of the frame
// — so a frame that overran presented a completely BLACK picture with a correctly
// rendered scene sitting in EDRAM behind it. That was the port's top rendering defect
// for six parts, reported as a view-dependent whole-frame black, and it is
// view-dependent for the obvious reason once you know the mechanism: which way the
// camera points decides how much geometry is in the frame. Measured: 160 black frames in
// 8,216 gameplay frames at 128 MB and every one of them the frame after an exhaustion;
// zero of either at 512 MB, with a true peak of 161 MB. §6ap.
//
// Growing rather than picking a bigger number, because a bigger number is what this was.
// The ceiling is a backstop against a runaway, not a tuned value; it is announced when it
// bites so a future frame that needs more says so out loud.
// Defined next to the submit, because that is where the ring's state is maintained.
void WaitAllFramesIdle();

void GrowArenaIfNeeded()
{
    if (R->arenaWant > R->arena.size)
    {
        constexpr VkDeviceSize kArenaCeiling = 2048ull << 20;
        const VkDeviceSize want = std::min(R->arenaWant, kArenaCeiling);
        if (want > R->arena.size)
        {
            WaitAllFramesIdle();
            vkDeviceWaitIdle(R->device);
            const Buffer old = R->arena;
            Buffer grown{};
            if (CreateBuffer(grown, want,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             /*deviceAddress=*/true))
            {
                R->arena = grown;
                vkDestroyBuffer(R->device, old.buffer, nullptr);
                vkFreeMemory(R->device, old.memory, nullptr);
                fprintf(stderr, "[vk] arena grown to %llu MB\n",
                        (unsigned long long)(want >> 20));
            }
            else
            {
                // Keep the old one rather than running with none. The frames that
                // overrun will keep presenting black, and the per-frame line above
                // keeps saying so.
                fprintf(stderr, "[vk] arena could NOT be grown to %llu MB — frames that "
                                "overrun %llu MB will keep losing their post chain\n",
                        (unsigned long long)(want >> 20),
                        (unsigned long long)(old.size >> 20));
                R->arenaWant = 0;
            }
        }
        else
        {
            fprintf(stderr, "[vk] arena is at its %llu MB ceiling and a frame still "
                            "overran it\n",
                    (unsigned long long)(kArenaCeiling >> 20));
            R->arenaWant = 0;
        }
    }
}

// The cross-frame store's frame-boundary maintenance: grow it, or drop it, or neither.
// Called from exactly where `GrowArenaIfNeeded` is — the end of `DoSwapImpl`, after the
// fence has been WAITED on, which is the only moment the GPU is provably not reading
// anything in here. This is load-bearing: every offset the store hands out is recorded
// into a command buffer, so reusing its memory a moment early is a wrong mesh, not a
// crash, and would surface frames later as a rendering bug.
//
// EVICTION IS A WHOLE DROP, deliberately, and it is a v1 decision with a counter on it.
// The alternative — an LRU with compaction — has to MOVE live streams to close the gaps,
// which is copying, which is the cost this store exists to remove. A drop instead pays
// exactly one frame at the pre-store cost and then runs warm again. If `flushes` turns
// out to be more than a handful per area transition, that is the evidence for building
// the harder thing; until then it is not.
void PersistMaintenance()
{
    if (!R->persistOn || !R->persistWant)
        return;
    constexpr VkDeviceSize kPersistCeiling = 1024ull << 20;
    const VkDeviceSize want = std::min(R->persistWant, kPersistCeiling);
    R->persistWant = 0;
    // Every path below either destroys the buffer or resets the cursor so its bytes are
    // handed out again, and both are read by draws recorded in frames that may still be
    // executing. Before part 23 the caller's fence wait made that impossible; it does
    // not any more, so this idles explicitly. It runs at most a handful of times a run.
    WaitAllFramesIdle();
    if (want > R->persist.size)
    {
        vkDeviceWaitIdle(R->device);
        const Buffer old = R->persist;
        Buffer grown{};
        if (CreateBuffer(grown, want,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         /*deviceAddress=*/true))
        {
            R->persist = grown;
            vkDestroyBuffer(R->device, old.buffer, nullptr);
            vkFreeMemory(R->device, old.memory, nullptr);
            fprintf(stderr, "[vk] stream store grown to %llu MB\n",
                    (unsigned long long)(want >> 20));
        }
        else
        {
            // Not fatal and not even degraded past the old renderer: without a store
            // every stream takes the per-frame path, which is what this port did for
            // twenty-one parts.
            fprintf(stderr, "[vk] stream store could NOT be grown to %llu MB — streams "
                            "fall back to the per-frame arena\n",
                    (unsigned long long)(want >> 20));
        }
        // The contents are dropped rather than copied across. Copying up to a gigabyte to
        // preserve a cache that refills itself in one frame would be the same mistake in
        // a different place.
    }
    else
    {
        fprintf(stderr, "[vk] stream store is at its %llu MB ceiling and a frame still "
                        "overran it — dropping and refilling\n",
                (unsigned long long)(kPersistCeiling >> 20));
    }
    R->persistCache.clear();
    R->persistCursor = 0;
    ++R->persistStats.flushes;
}

void BeginFrame()
{
    if (R->recording)
        return;
    // The slot this frame will record into. It was advanced at the previous swap, AFTER
    // that swap waited on this slot's fence — so `vkResetCommandBuffer` below is legal
    // and this slot's arena region is provably not being read. There is no wait here on
    // purpose: a wait at the START of a frame would put the GPU back in series with the
    // CPU and undo the whole change.
    FrameSlot& fs = R->frames[R->frameSlot];
    R->cmd = fs.cmd;
    R->fence = fs.fence;

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(R->cmd, 0);
    vkBeginCommandBuffer(R->cmd, &bi);
    R->recording = true;
    R->rendering = false;
    // The arena is GROWN at the end of a frame, in `GrowArenaIfNeeded` — not here. What
    // remains here is the reset, which is the cheap half and has to be per frame. With
    // frames in flight the reset is to this SLOT's region rather than to zero.
    const VkDeviceSize region = R->arena.size / R->framesInFlight;
    R->arenaBase = VkDeviceSize(R->frameSlot) * region;
    R->arenaLimit = R->arenaBase + region;
    R->arenaCursor = R->arenaBase;
    // Remember what this frame cached before dropping it, so the next frame's misses can
    // say how many of them a cache that outlived the frame would have served. Off by
    // default and free when off; when on it is one walk of a few hundred entries per
    // frame, off the draw path.
    if (g_streamCensus)
    {
        g_prevStreamKeys.clear();
        for (const auto& kv : R->streamCache)
        {
            uint64_t h = 0;
            if (g_streamCensus >= 2)
            {
                auto hit = g_streamHashes.find(kv.first);
                if (hit != g_streamHashes.end())
                    h = hit->second;
            }
            g_prevStreamKeys.emplace(kv.first, h);
        }
        g_streamHashes.clear();
    }
    R->streamCache.clear();
    R->drawsThisFrame = 0;
    // The scene-camera pick is PER FRAME. Left latched, it would hold the largest draw
    // of the whole RUN, so a .pose would carry a camera from some frame minutes earlier
    // while looking exactly like this frame's — a stale value that announces nothing
    // (the failure shape gotcha 13 is about, at frame scale).
    R->camBigVerts = 0;
    // A fresh command buffer binds nothing. This must be here and nowhere else: a
    // stale `bound` across a vkResetCommandBuffer would skip binds that ARE needed,
    // and the symptom would be a draw rendering with the previous frame's pipeline.
    R->bound = Renderer::BoundState{};
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

// Submit whatever has been recorded into this frame's slot, and DO NOT wait for it
// unless there is only one slot.
//
// This was `SubmitAndWait` for twenty-two parts and the wait was deliberate — "a second,
// host-side pipelining scheme would make 'which frame is on screen' a question with two
// answers". That reasoning was about the PICTURE and it is still respected: the answer
// stays single-valued because the ring is strictly in order and a frame is presented
// exactly when its own fence signals, one frame later. What the reasoning did not price
// was the GPU, which under a synchronous submit is idle 68% of every crowd frame while
// the CPU records the next one (§6ar). Part 23 is that price being paid.
void SubmitFrame()
{
    if (!R->recording)
        return;
    EndRendering();
    vkEndCommandBuffer(R->cmd);
    R->recording = false;

    // CZ_VK_NO_SUBMIT=1 — record the whole frame and then DO NOT EXECUTE IT.
    //
    // A CEILING MEASUREMENT, not a rendering arm, and its picture is knowingly invalid.
    //
    // The question it answers: this renderer's CPU and GPU never run at the same moment,
    // because the line below submits a command buffer and then blocks on its fence. A
    // crowd frame is ~27.7 ms of CPU followed by ~16.5 ms of GPU, strictly in series,
    // and the GPU is consequently idle 68% of every frame — which is why the driver
    // governs the card to a mid clock, correctly (gotcha 231, `docs/phase5-notes.md`
    // §6ar). Overlapping them should give max(CPU, GPU) instead of CPU + GPU, but
    // "should" is a model, and building frames-in-flight to test a model is the wrong
    // order of work: it needs a real swapchain present and a second per-frame arena.
    // **That was written before part 23 and the second half of it turned out to be
    // wrong**: the overlap needed a second per-frame arena, which it got, and it did NOT
    // need a swapchain — a per-slot readback buffer presented one frame later keeps the
    // renderer/window separation phase 3 built and costs one frame of latency. The arm
    // survives anyway, as the ceiling this is measured against.
    //
    // Dropping the submit makes the GPU's contribution zero while EVERY byte of CPU work
    // still happens — the PM4 walk, the register decode, the pipeline lookups, the
    // stream copies and all ~6.4 `vkCmd*` calls a draw are recorded exactly as before.
    // The frame time that remains IS the CPU-only time, and no amount of overlapping can
    // beat it. So this is an upper bound on the win, measured in one run per arm instead
    // of a session of rework.
    //
    // WHY NOT "submit but do not wait", which is the obvious version: skipping only the
    // wait lets the next frame reset this command buffer while the GPU is still reading
    // it, and — worse — lets it overwrite the arena holding the INDEX buffers of a draw
    // in flight. Out-of-range indices are undefined behaviour on the device, so the
    // likely outcome is a lost device or a hung GPU rather than a corrupted picture, and
    // an instrument that can take the machine down is not one to reach for when a safe
    // version measures the same quantity. Nothing here executes, so nothing can fault.
    //
    // What this arm is NOT: a comparison of two pictures. The draw set recorded is
    // identical between the arms, which is what makes the TIMES comparable, but the
    // frame presented on this arm is stale garbage and every picture statistic derived
    // from it (`CZ_VK_FRAME_STATS`'s coverage, luma and hashes) is meaningless. Read the
    // `msec` column and nothing else.
    static const bool noSubmit = EnvOn("CZ_VK_NO_SUBMIT");
    if (noSubmit)
    {
        Count("submit: SKIPPED (CZ_VK_NO_SUBMIT — ceiling measurement, picture invalid)");
        return;
    }

    FrameSlot& fs = R->frames[R->frameSlot];
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fs.cmd;
    ProfScope _p(&g_prof.submit);
    ProfScope _c(&g_prof.submitCall);
    vkResetFences(R->device, 1, &fs.fence);
    vkQueueSubmit(R->queue, 1, &si, fs.fence);
    fs.inFlight = true;
}

// Wait for the OLDEST frame still in flight and hand back its slot, or -1 if there is
// none yet. Called immediately after `SubmitFrame`, which is what makes this the whole
// of the change: with one slot the oldest frame IS the one just submitted and this is
// the old `SubmitAndWait` exactly; with two, it is the frame before it, whose GPU work
// has had the entirety of this frame's CPU time to finish.
//
// `fenceWait` IS THE COUNTER THAT SAYS THE OVERLAP ENGAGED (gotcha 151). It is the
// renderer's measure of "time blocked on the GPU", and the prediction this change makes
// is that it collapses towards zero in a crowd while `submitCall` and every draw-path
// column stay where they were. If `fenceWait` does not move, the frames are not
// overlapping and nothing else in the profile is worth reading.
int RetireOldestFrame()
{
    // Slots are used strictly in ring order, so when the frame just submitted is `s` the
    // oldest one not yet waited on is `s + 1` — which for a single slot is `s` itself.
    // CZ_VK_NO_SUBMIT recorded a frame and executed none of it, so no fence will ever
    // signal. It forces one slot (see the init), and the present path below then reads
    // whatever was left in that slot's buffer — which is exactly what that arm has always
    // done, and exactly why its picture statistics are documented as meaningless.
    static const bool noSubmit = EnvOn("CZ_VK_NO_SUBMIT");
    if (noSubmit)
        return int(R->frameSlot);

    const uint32_t oldest = (R->frameSlot + 1) % R->framesInFlight;
    FrameSlot& fs = R->frames[oldest];
    if (!fs.inFlight)
        return -1;
    {
        ProfScope _p(&g_prof.submit);
        ProfScope _w(&g_prof.fenceWait);
        vkWaitForFences(R->device, 1, &fs.fence, VK_TRUE, UINT64_MAX);
    }
    fs.inFlight = false;
    return int(oldest);
}

// Everything in flight has finished. The frame boundary's two destructive maintenance
// steps (`GrowArenaIfNeeded`, `PersistMaintenance`) reuse or destroy memory whose
// offsets are recorded in command buffers, and with frames in flight "the fence has been
// waited on" is no longer enough — one of them may still be executing.
void WaitAllFramesIdle()
{
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        if (R->frames[i].inFlight)
            vkWaitForFences(R->device, 1, &R->frames[i].fence, VK_TRUE, UINT64_MAX);
    // `inFlight` is deliberately NOT cleared. It means "submitted and not yet PRESENTED",
    // and a frame waited on here still owes the window its pixels; clearing it would drop
    // that frame silently at every arena growth. Waiting twice on a signalled fence costs
    // nothing.
}

// ===================================================================================
// The draw
// ===================================================================================
// Make a guest vertex/index stream available to the GPU, dword-swapped, and say where it
// ended up. Two caches, in order:
//
//   1. `streamCache`, per frame, by (address, size, endian). The frontend draws the same
//      buffer dozens of times a frame — 94% of lookups are this — and copying it each
//      time would be the dominant cost of the renderer.
//   2. `persistCache`, ACROSS frames, same key plus a content guard. The remaining 6%
//      still copied 61-77 MB a frame, 94-97% of it byte-identical to what the previous
//      frame put at the same address (§6at). This is that measurement acted on.
//
// A cross-frame hit costs one `StreamGuard` of at most 512 bytes and no copy. A miss
// costs the guard plus exactly what it always cost.
//
// `kind` is census-only: 0 = a declared vertex binding, 1 = an index buffer, 2 = a
// shader-side dependent fetch (XeVfetchDep). It exists because the three have different
// answers to "could this be cached across frames" — an index buffer for static geometry
// is a different proposition from a stream a shader raw-loads — and a single total
// cannot be read that way. A constant at every call site, so it costs nothing when the
// census is off.
StreamLoc UploadStream(uint8_t* base, uint32_t va, uint64_t bytes, uint32_t endian,
                       int kind)
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
    {
        if (g_streamCensus)
        {
            ++g_streamCensus_c.hits;
            g_streamCensus_c.bytesHit += bytes;
        }
        return it->second;
    }

    // Below here runs at most ONCE per (key, frame) — ~2,000 times in a crowd frame
    // against ~33,000 lookups. It is the only place a guard hash or a copy can happen,
    // which is what keeps both affordable.
    const uint8_t* const src = base + va;
    StreamLoc loc;
    bool copied = true;

    if (R->persistOn)
    {
        uint64_t guardRead = 0;
        const uint64_t guard = StreamGuard(src, size_t(bytes), &guardRead);
        R->persistStats.guardBytes += guardRead;
        auto pit = R->persistCache.find(key);
        if (pit != R->persistCache.end())
        {
            Renderer::PersistEntry& e = pit->second;
            e.lastFrame = R->frame;
            if (e.guard == guard)
            {
                // The whole point: the bytes are already in device memory, dword-swapped,
                // from some earlier frame. Nothing is copied.
                ++R->persistStats.hits;
                R->persistStats.hitBytes += bytes;
                copied = false;
                loc = StreamLoc{ &R->persist, e.at };
            }
            else
            {
                // The guard caught the guest rewriting this buffer in place. The size is
                // part of the key, so a slot is exactly the right size and the re-copy
                // needs no allocation — but it must not be THE SLOT AN IN-FLIGHT FRAME IS
                // READING.
                //
                // Until part 23 it was, and that was correct: the submit was synchronous,
                // so every draw pointing at this slot came from a frame whose fence had
                // been waited on. With frames in flight it is a wrong mesh, silently. So
                // the write goes to the entry's twin and the two alternate — see
                // `PersistEntry::alt` for why two is exactly enough.
                //
                // When no twin can be had (the store is full) the entry is DROPPED rather
                // than overwritten. That costs this stream a per-frame copy and puts it
                // back on the pre-store path, which is the same trade the `overflow`
                // counter below already makes; the alternative is trading correctness for
                // a copy, which is never the trade to make silently.
                bool safe = true;
                if (R->framesInFlight > 1)
                {
                    if (e.alt == VkDeviceSize(-1))
                    {
                        const VkDeviceSize a = PersistAlloc(bytes);
                        if (a != VkDeviceSize(-1))
                            e.alt = a;
                    }
                    if (e.alt != VkDeviceSize(-1))
                        std::swap(e.at, e.alt);
                    else
                        safe = false;
                }
                if (safe)
                {
                    {
                        ProfScope _p(&g_prof.streams);
                        CopySwapped(R->persist.mapped + e.at, src, size_t(bytes), endian);
                    }
                    e.guard = guard;
                    ++R->persistStats.stale;
                    R->persistStats.staleBytes += bytes;
                    loc = StreamLoc{ &R->persist, e.at };
                }
                else
                {
                    // `e` dies with this line. Nothing below may touch it, and `loc`
                    // stays empty so the per-frame arena path takes this stream.
                    R->persistCache.erase(pit);
                    ++R->persistStats.staleEvicted;
                }
            }
        }
        else
        {
            const VkDeviceSize at = PersistAlloc(bytes);
            if (at != VkDeviceSize(-1))
            {
                {
                    ProfScope _p(&g_prof.streams);
                    CopySwapped(R->persist.mapped + at, src, size_t(bytes), endian);
                }
                Renderer::PersistEntry e;
                e.at = at;
                e.guard = guard;
                e.lastFrame = R->frame;
                e.bytes = uint32_t(bytes);
                R->persistCache.emplace(key, e);
                ++R->persistStats.fills;
                R->persistStats.fillBytes += bytes;
                loc = StreamLoc{ &R->persist, at };
            }
            else
            {
                // The store is full. This stream takes the per-frame path below, exactly
                // as it did before the store existed, and the frame boundary decides
                // whether to grow or to drop the store.
                ++R->persistStats.overflow;
            }
        }
    }

    if (!loc.ok())
    {
        const VkDeviceSize at = ArenaAlloc(bytes, 16);
        if (at == VkDeviceSize(-1))
            return StreamLoc{};
        {
            ProfScope _p(&g_prof.streams);
            CopySwapped(R->arena.mapped + at, src, size_t(bytes), endian);
        }
        loc = StreamLoc{ &R->arena, at };
    }
    R->streamCache.emplace(key, loc);

    // The census, entirely on the first-touch path — which already costs a guard and
    // usually a copy, so the instrument is small against what it is measuring, and the
    // hit path above pays two increments. Both are behind one already-hot int, which is
    // what part 20's removal of the per-draw counters was about (gotcha 230).
    if (g_streamCensus)
    {
        ++g_streamCensus_c.misses;
        const int k = (kind >= 0 && kind < 3) ? kind : 0;
        ++g_streamCensus_c.kindMisses[k];
        // `bytesCopied` means BYTES ACTUALLY COPIED, so that the same instrument reads
        // the cost in both arms rather than reading the workload. With the store on and
        // warm this collapses towards zero, and that collapse IS the measurement.
        if (copied)
        {
            g_streamCensus_c.bytesCopied += bytes;
            g_streamCensus_c.kindBytes[k] += bytes;
        }
        auto pit = g_prevStreamKeys.find(key);
        if (pit != g_prevStreamKeys.end())
        {
            ++g_streamCensus_c.prevFrameKeyHits;
            g_streamCensus_c.prevFrameKeyBytes += bytes;
            if (copied)
                g_streamCensus_c.kindRepeatBytes[k] += bytes;
        }
        if (g_streamCensus >= 2)
        {
            const uint64_t h =
                StreamHash(src, size_t(bytes), g_streamPoison ? R->frame : 0);
            g_streamHashes[key] = h;
            if (pit != g_prevStreamKeys.end())
            {
                if (pit->second == h)
                {
                    ++g_streamCensus_c.prevFrameSameContent;
                    g_streamCensus_c.prevFrameSameBytes += bytes;
                }
                else
                {
                    // The identity of the rewritten stream, which is what chose the
                    // invalidation mechanism. Note this branch is ALSO the whole
                    // population when the poison arm is on, which is the point of that
                    // arm — under poison this map fills with every repeated key and the
                    // list below is meaningless, so it says so.
                    StreamChange& c = g_streamChanged[key];
                    if (!c.times)
                    {
                        c.bytes = bytes;
                        c.kind = (kind >= 0 && kind < 3) ? kind : 0;
                        c.firstFrame = R->frame;
                    }
                    ++c.times;
                    c.lastFrame = R->frame;
                    // THE GUARD'S POWER, MEASURED RATHER THAN ASSUMED. The full hash says
                    // this stream's bytes changed since last frame. If the store served
                    // it as a hit this frame, the sampled guard did NOT notice — which is
                    // a stale buffer handed to a draw, the exact defect the guard exists
                    // to prevent. It must read zero, and unlike the content line it is
                    // capable of reading otherwise: `CZ_VK_STREAM_CENSUS_POISON=1` makes
                    // every repeat land here while the guard (unsalted) still says
                    // unchanged, so under poison this counter equals the repeat count.
                    if (!copied)
                        ++g_streamCensus_c.guardMissed;
                }
            }
        }
    }
    return loc;
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
// `src` points at the guest stream's already dword-swapped bytes — which since the
// cross-frame store exists may be in either buffer, hence a pointer rather than an
// offset. `stride` is the fetch's stride in dwords and `corner` the three vertex indices
// this draw uses. The OUTPUT is always in the per-frame arena, because it depends on this
// draw's corner indices and so is not shared with any other draw.
//
// The extrapolation is done on FLOAT dwords, which is exact for a 32-bit float
// attribute and is what hardware does to every attribute of a rect. A packed format
// would need its own arithmetic; that case copies r0 and counts itself, because a
// wrong fourth corner that says nothing is the failure mode this whole function exists
// to remove.
VkDeviceSize SynthRectStream(const uint8_t* src, uint64_t streamBytes,
                             uint32_t strideDwords, const uint32_t corner[3],
                             uint32_t format)
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

// Count a vertex or index bind against what the state cache WOULD have skipped.
// Counting only: the bind is still issued by the caller. See Renderer::BindSkips.
// The BUFFER is part of the comparison, not just the offset. Since the cross-frame store
// exists there are two buffers a stream can be in, and offset 0 of one is a different
// bind from offset 0 of the other — without this the repeat counters would over-report
// exactly at the boundary between them, which is a measurement quietly telling a lie
// about the change that introduced it.
void NoteVertexBind(uint32_t binding, VkBuffer buffer, VkDeviceSize offset)
{
    ++R->skips.vertexBinds;
    if (binding >= Renderer::BoundState::kMaxTrackedBindings)
        return;
    if (R->bound.haveVertex[binding] && R->bound.vertexOffset[binding] == offset &&
        R->bound.vertexBuffer[binding] == buffer)
        ++R->skips.vertexBindRepeats;
    R->bound.haveVertex[binding] = true;
    R->bound.vertexOffset[binding] = offset;
    R->bound.vertexBuffer[binding] = buffer;
}

void NoteIndexBind(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
    ++R->skips.indexBinds;
    if (R->bound.haveIndex && R->bound.indexOffset == offset &&
        R->bound.indexType == type && R->bound.indexBuffer == buffer)
        ++R->skips.indexBindRepeats;
    R->bound.haveIndex = true;
    R->bound.indexOffset = offset;
    R->bound.indexType = type;
    R->bound.indexBuffer = buffer;
}

// Part 41 item 1b: the sampler a fetch ASKS FOR, by descriptor index in set 3.
//
// Until part 41 every fetch published sampler index 0 — one global trilinear REPEAT
// sampler — a stated simplification that was measured wrong two ways in one session:
// aniso applied globally speckles the shadow term (hardware fetches the 4096x1024
// shadow atlas with aniso=0 and POINT filters), and trilinear applied globally is
// why the ground goes to mush at distance (hardware fetches the world's albedo
// textures at 4:1 and 8:1 — 500 of 621 distinct textures in the R4 census carry a
// non-zero aniso field). So the fetch constant's own fields are honoured, one
// VkSampler per distinct spec, created on first sight and cached for the process.
//
// Address modes stay REPEAT in this change ON PURPOSE: the clamp fields are a
// separate experiment (the cyan edge fringes, part41-kickoff item 5) with its own
// prediction, and bundling them here would make the two inseparable.
//
// CZ_VK_NO_FETCH_SAMPLERS=1 is the whole-feature arm (every fetch reads sampler 0,
// the part-40 renderer, same binary). CZ_VK_ANISO=N caps the degree; =0 keeps the
// per-fetch FILTERS while disabling aniso, which separates the two halves of this
// change for diagnosis.
static uint32_t SamplerIndexForFetch(const uint32_t* regs, uint32_t constIdx)
{
    static const bool off = EnvOn("CZ_VK_NO_FETCH_SAMPLERS");
    if (off)
        return 0;
    const uint32_t d3 = regs[xenos::kFetchConstantBase + constIdx * 6 + 3];
    const uint32_t key = (d3 >> 19) & 0x1FF;          // mag:2 min:2 mip:2 aniso:3
    auto it = R->samplerBySpec.find(key);
    if (it != R->samplerBySpec.end())
        return it->second;
    if (R->samplerCount >= g_maxDescriptors)
    {
        Count("sampler: set-3 heap FULL — fetch served the default");
        return 0;
    }
    const uint32_t mag = (d3 >> 19) & 3;
    const uint32_t mn = (d3 >> 21) & 3;
    const uint32_t mip = (d3 >> 23) & 3;
    const uint32_t an = (d3 >> 25) & 7;
    // Filter values 2 (basemap) and 3 (keep) never appear in the R4 census — the
    // 621 distinct hardware fetches read 0 or 1 on all three fields. If a run
    // produces one it is COUNTED and filtered as linear, never guessed at silently.
    if (mag > 1 || mn > 1 || mip > 1)
        Count("sampler: filter field above LINEAR — treated as linear");
    if (an > 5)
        Count("sampler: aniso field above 16:1 — treated as disabled");
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = mn == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = mip == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                             : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    static const int cap = Env("CZ_VK_ANISO") ? atoi(Env("CZ_VK_ANISO")) : 16;
    if (an >= 2 && an <= 5 && cap > 0 && R->anisoLimit > 0.0f)
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(std::min(float(1u << (an - 1)), float(cap)),
                                    R->anisoLimit);
    }
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(R->device, &si, nullptr, &s) != VK_SUCCESS)
    {
        Count("sampler: vkCreateSampler FAILED — fetch served the default");
        return 0;
    }
    const uint32_t idx = R->samplerCount++;
    VkDescriptorImageInfo ii{};
    ii.sampler = s;
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = R->sets[3];
    w.dstBinding = 0;
    w.dstArrayElement = idx;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, nullptr);
    R->samplerBySpec[key] = idx;
    // One line per DISTINCT spec for the process — a handful, and each is the
    // engagement evidence the census can be checked against.
    fprintf(stderr, "[vk] sampler #%u: mag=%u min=%u mip=%u anisoField=%u -> "
                    "maxAniso %.0f\n",
            idx, mag, mn, mip, an,
            si.anisotropyEnable ? si.maxAnisotropy : 0.0f);
    return idx;
}

// The register file and shader bindings are PARAMETERS, not globals: the PM4 feed
// passes pm4.cpp's, the D3D feed (phase C) passes the private file its walker built
// from the title's own flush output. Everything below is feed-agnostic.
void DoDraw(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
            const Pm4ShaderBinding& vsBind, const Pm4ShaderBinding& psBind)
{
    // DoDraw's OWN work, exclusive of the named phases nested inside it. Without a
    // scope here the profile's unaccounted column would mix this function's untimed
    // work (register decode, the pipeline-key build and lookup, the fetch-constant
    // walk) with the guest's simulation and the command processor — three completely
    // different investigations behind one number. The whole draw is the sum of this and
    // the phases, computed at print time; it is not measured separately, because a sum
    // and a second measurement of the same interval can only ever disagree.
    ProfScope _pDraw(&g_prof.drawOther);
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
    // Resolved to 64 counter ADDRESSES on first use rather than 64 names looked up per
    // draw, for the reason `COUNT` exists; the names and their order are identical.
    {
        static uint64_t* slots[64];
        static bool built = false;
        if (!built)
        {
            built = true;
            char name[32];
            for (uint32_t i = 0; i < 64; i++)
            {
                snprintf(name, sizeof name, "prim %02u", i);
                slots[i] = CounterSlot(name);
            }
        }
        ++*slots[draw.primType & 63];
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
    // CZ_VK_DRAW_ID: for ONE armed frame every draw paints its own index. See
    // tools/drawid_ps.hlsl for why this exists and tools/drawid_read.py for reading it.
    key.drawIdPass = R->drawIdArmed ? 1u : 0u;
    if (key.drawIdPass)
        R->drawIdActive = true;
    // AN ARM WITH NO COUNTER CANNOT BE SHOWN TO HAVE ENGAGED (gotcha 151). The first
    // version of this instrument had none, and its first output was read for twenty
    // minutes as if it were a map before a same-address comparison against a normal run
    // showed the two were IDENTICAL — the pass had never run.
    if (key.drawIdPass)
        Count("draw: painted its INDEX (CZ_VK_DRAW_ID)");
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

    // ALPHA TEST (part 38, LIVE AS OF PART 40). RB_COLORCONTROL bits 0..2 are the
    // compare func (0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL,
    // 6 GEQUAL, 7 ALWAYS), bit 3 the enable, bit 4 ALPHA_TO_MASK. The shaders'
    // clip(oC0.w - ref) keeps w >= ref, which IS GEQUAL and is GREATER everywhere but
    // exact equality — both map to the same clip. Every OTHER enabled func is counted
    // BY NAME and left un-emulated rather than guessed (gotcha 5).
    //
    // "As of part 40" because for the whole of parts 38-39 this block read register
    // 0x2205, which is RB_BLENDCONTROL1, so it NEVER fired — see kRbColorControl in
    // xenos.h for the evidence that settled the index. With the right register,
    // hardware's R4 traces enable this test exactly where the picture said it was
    // missing: the leaf-card foliage, the chain-link fences, and the shadow-caster
    // pass whose pixel shader samples the material's alpha for no other purpose.
    //
    // ALPHA_TO_MASK on a single-sampled target is NOT emulated as such: the draws
    // hardware sets it on (0x1C) also set the alpha test, so the clip covers them;
    // a draw with A2M alone would be counted below, never silently approximated.
    {
        static const bool noAlphaTest = EnvOn("CZ_VK_NO_ALPHA_TEST");
        const uint32_t cc = regs[xenos::kRbColorControl];
        if (!noAlphaTest && (cc & 0x8))
        {
            const uint32_t func = cc & 0x7;
            if (func == 4 || func == 6)
            {
                key.alphaTest = 1;
                Count("draw: alpha test (GREATER/GEQUAL) enabled");
            }
            else if (func == 2 && F32(regs[xenos::kRbAlphaRef]) >= 1.0f)
            {
                // EQUAL at ref = 1.0 — "keep only the fully solid texels". The clip is
                // >=-shaped, but nothing an alpha channel produces exceeds 1.0, so
                // >= (1 - half an 8-bit step) IS equality here; the threshold write
                // below supplies that value. This is the caster flavor of the foliage
                // cutout (174 of our shadow-pass draws, ref always exactly 1.0 in
                // hardware's traces too) and the two-pass core redraw. EQUAL at any
                // LOWER ref cannot be spelled with one >= clip and stays counted below.
                key.alphaTest = 1;
                Count("draw: alpha test EQUAL@1.0 (emulated as >= 1-eps)");
            }
            else if (func != 7)
            {
                static const char* kFuncNames[8] = { "NEVER", "LESS", "EQUAL", "LEQUAL",
                                                     "GREATER", "NOTEQUAL", "GEQUAL",
                                                     "ALWAYS" };
                char msg[64];
                snprintf(msg, sizeof msg, "draw: alpha test func %s UNEMULATED",
                         kFuncNames[func]);
                Count(msg);
            }
        }
        else if (!noAlphaTest && (cc & 0x10))
            Count("draw: ALPHA-TO-MASK without alpha test — UNEMULATED");
    }

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
    {
        if (draw.index32)
            COUNT("draw: 32-bit indices");
        else
            COUNT("draw: 16-bit indices");
    }

    // CZ_VK_SHADER_CENSUS=1 — draws per (vs, ps) pair, in the stats block. Which
    // shader pair does the work of a pass is the question that turns "the scene is
    // flat" into "THIS pixel shader is flat", and from there Xenia's disassembly of
    // that exact shader says what it was supposed to compute. Off by default because
    // it makes one counter per pair.
    static const bool shaderCensus = EnvOn("CZ_VK_SHADER_CENSUS");
    if (shaderCensus)
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
        COUNT("draw: colour write mask is empty");
    // ...and the pair that says whether an empty mask is a DEPTH PREPASS or a register
    // read at the wrong index. RB_MODECONTROL's edram mode is the guest's own statement
    // of what a pass writes (4 = colour+depth, 5 = depth-only), so "mode 5, mask 0" is
    // a prepass and needs no explanation, while "mode 4, mask 0" is a draw that set up
    // a colour target and then asked for none of it — a shape worth counting rather
    // than assuming.
    if (key.colorMask == 0)
    {
        static uint64_t* slots[8];
        static bool built = false;
        if (!built)
        {
            built = true;
            char name[48];
            for (uint32_t m = 0; m < 8; m++)
            {
                snprintf(name, sizeof name,
                         "draw: modeControl %u with an EMPTY colour mask", m);
                slots[m] = CounterSlot(name);
            }
        }
        ++*slots[key.modeControl & 7];
    }
    if (((key.depthControl >> 1) & 1) && ((key.depthControl >> 4) & 7) == 0)
        COUNT("draw: depth compare is NEVER");

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

    uint8_t* shared = R->arena.mapped + sharedAt;
    {
        ProfScope _p(&g_prof.constants);
        g_prof.draws++;
        // THE WINDOW THE GUEST NAMED, NOT THE ONE WE ASSUMED. SQ_VS_CONST (0x2307) and
        // SQ_PS_CONST (0x2308) each carry a BASE in their low 9 bits, and this copy used
        // to hardcode 0 and 256. A draw that moves its window reads someone else's
        // constants, and the comment on kPsConstBytes above records what that looks like
        // when it happens: not a tint, but "930 draws producing three distinct colours" —
        // every pixel of the surface collapsed to a constant. Part 26 is chasing exactly
        // that symptom on the ground, so the assumption gets a counter rather than a
        // benefit of the doubt (gotcha 3: the zero we have is one draw, not a census).
        const uint32_t vsBase = regs[0x2307] & 0x1FF;
        const uint32_t psBase = regs[0x2308] & 0x1FF;
        if (vsBase != 0 || psBase != 256)
            Count("draw: the guest moved its ALU constant WINDOW away from 0/256");
        uint32_t* dst = reinterpret_cast<uint32_t*>(R->arena.mapped + vsConstAt);
        for (uint32_t i = 0; i < 256 * 4; i++)
            dst[i] = regs[xenos::kAluConstantBase + vsBase * 4 + i];
        dst = reinterpret_cast<uint32_t*>(R->arena.mapped + psConstAt);
        for (uint32_t i = 0; i < 256 * 4; i++)
            dst[i] = regs[xenos::kAluConstantBase + psBase * 4 + i];

        // The exposure this draw will use, recorded BEFORE any arm perturbs it, so the
        // trace reports what the GUEST asked for rather than what an experiment did.
        {
            const float e = F32(regs[xenos::kAluConstantBase + psBase * 4 + 14 * 4 + 3]);
            if (R->expDraws == 0)
                R->expMin = R->expMax = e;
            else
            {
                R->expMin = std::min(R->expMin, e);
                R->expMax = std::max(R->expMax, e);
            }
            ++R->expDraws;
        }

        // CZ_VK_PS_CONST_SCALE="14.w=4,18.y=0.5" — multiply chosen PIXEL constant
        // components by a factor, after the copy and before any draw reads them.
        //
        // WHY THIS EXISTS. The white-surface item (open-items 00f) ends at a tone curve
        // whose constants are now known to be correct on both sides, so the remaining
        // question is about the COLOUR arriving at it: `x = colour * pc(14).w` sits at
        // exactly full exposure on the white surfaces and never above it anywhere in the
        // frame. The output cannot distinguish "the colour varies and the curve is flat
        // here" from "the colour is pinned", because `d(out)/dx` vanishes at `x = 1` —
        // a 10% spread in the colour quantises to ONE 8-bit value there (gotcha 273).
        //
        // Scaling the exposure moves the surfaces to a part of the curve where it does
        // not vanish: at `x = 4` the same 10% spread is about seven 8-bit levels. So the
        // arm is a magnifying glass on the input, not a change anyone wants to keep —
        // a plateau that stays a single spike under 4x is a pinned colour, and one that
        // spreads is an ordinary shaded surface the curve was hiding.
        //
        // It scales rather than sets, deliberately: this title's exposure is scene
        // adaptive and differs per draw (0.2 to 1.0 in one run), so a fixed value would
        // flatten a real signal into a constant and manufacture the very uniformity the
        // arm exists to test for.
        struct PsConstScale { uint32_t index, comp; float factor; };
        static const std::vector<PsConstScale> psConstScale = []
        {
            std::vector<PsConstScale> out;
            const char* spec = Env("CZ_VK_PS_CONST_SCALE");
            if (!spec)
                return out;
            for (const char* p = spec; *p;)
            {
                char* end = nullptr;
                const uint32_t index = uint32_t(strtoul(p, &end, 10));
                // Announce every rejected clause rather than skipping it. An arm that
                // silently parses to nothing is an arm that cannot be shown to have
                // engaged, and this one's whole output is "the picture changed".
                uint32_t comp = 4;
                if (end && *end == '.')
                    comp = uint32_t(std::string("xyzw").find(end[1]));
                const char* eq = (end && *end) ? strchr(end, '=') : nullptr;
                if (index < 256 && comp < 4 && eq)
                {
                    out.push_back({index, comp, strtof(eq + 1, nullptr)});
                    fprintf(stderr, "[vk] CZ_VK_PS_CONST_SCALE: pc(%u).%c *= %g\n", index,
                            "xyzw"[comp], double(out.back().factor));
                }
                else
                {
                    fprintf(stderr, "[vk] CZ_VK_PS_CONST_SCALE: cannot parse \"%s\" — "
                                    "expected <0..255>.<xyzw>=<factor>\n", p);
                }
                const char* comma = strchr(p, ',');
                if (!comma)
                    break;
                p = comma + 1;
            }
            return out;
        }();
        if (!psConstScale.empty())
        {
            float* f = reinterpret_cast<float*>(dst);
            for (const PsConstScale& s : psConstScale)
                f[s.index * 4 + s.comp] *= s.factor;
            Count("draw: a PIXEL constant was scaled by CZ_VK_PS_CONST_SCALE");
        }
        memset(shared, 0, kSharedSize);
    }

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
    //
    // The `snprintf` that formats this shader's hash is INSIDE the `psbindEnv` test,
    // and that is a part-20 performance fix rather than a tidy-up: it used to run on
    // every draw whether or not the instrument was on, so a diagnostic nobody had
    // enabled was formatting 6,600 strings a frame. An instrument is only free when off
    // if its cost is behind its own gate — see `docs/instruments.md`, which promises
    // exactly that of every arm in this runtime.
    static const char* psbindEnv = Env("CZ_VK_PSBIND");
    bool psbind = false;
    if (psbindEnv)
    {
        char psbindWant[24];
        snprintf(psbindWant, sizeof psbindWant, "%016llx",
                 (unsigned long long)psBind.hash);
        psbind = strstr(psbindEnv, psbindWant) != nullptr;
    }
    // CZ_VK_DRAW_CENSUS=<file> plus F9 — EVERY draw of ONE frame, with what each one
    // bound. The same line as `[psbind]` above, without its two preconditions: that you
    // already know which pixel shader to name, and that only the first distinct 64
    // bindings are printed.
    //
    // Both preconditions are fatal for the question this exists to answer. An operator can
    // see that a surface is wrong and cannot possibly know its shader hash, and the thing
    // being looked for — a draw that covers half the screen and binds `slot=0`, the 1x1
    // white dummy — is one line somewhere in the middle of several thousand. Part 26 got
    // as far as "the ground is untextured in the scene buffer and no counted dummy path
    // fired", which is exactly the point where a per-draw list is the only way forward.
    //
    // Armed by the F9 edge so the frame dumped is the frame someone is LOOKING at, and it
    // covers exactly one frame: at ~6,800 draws this writes ~6,800 lines, which is a file
    // to grep and not a log to read.
    const bool drawCensus = R->drawCensusFrame && R->frame == R->drawCensusFrame;
    psbind = psbind || drawCensus;
    if (drawCensus && !R->drawCensusFile)
    {
        // CZ_CAPTURE_KEY supplies this path too. The arming site and the OPEN site read
        // the destination independently, and this one read the environment directly — so
        // a capture armed by CZ_CAPTURE_KEY announced a census, ran the frame, and wrote
        // no file. Two places deciding where output goes is one place too many.
        static std::string capturePath;
        if (capturePath.empty())
            if (const char* d = Env("CZ_CAPTURE_KEY"))
                capturePath = std::string(d) + "/capture.census";
        const char* censusEnv =
            capturePath.empty() ? Env("CZ_VK_DRAW_CENSUS") : capturePath.c_str();
        if (const char* path = censusEnv)
        {
            // ONE FILE PER FRAME. The first version wrote to the path as given, so a
            // second F9 destroyed the first press's census — which is exactly what
            // happened the first time anyone used it in anger: an operator pressed F9 at
            // five defects in five minutes and the fourth overwrote the third before it
            // had been read. The frame number goes in the name, so pressing it again can
            // only ever ADD evidence.
            char named[512];
            const char* dot = strrchr(path, '.');
            if (dot && !strchr(dot, '/'))
                snprintf(named, sizeof named, "%.*s_f%llu%s", int(dot - path), path,
                         (unsigned long long)R->frame, dot);
            else
                snprintf(named, sizeof named, "%s_f%llu", path,
                         (unsigned long long)R->frame);
            R->drawCensusFile = fopen(named, "w");
            if (R->drawCensusFile)
                fprintf(R->drawCensusFile,
                        "# every draw of frame %llu. sN=<fetch slot> then the guest "
                        "address, extent, format, and the bindless slot it resolved to.\n"
                        "# slot=0 IS THE 1x1 WHITE DUMMY — a draw that covers a lot of "
                        "screen and reads it is the thing to look for.\n",
                        (unsigned long long)R->frame);
            else
                fprintf(stderr, "[vk] cannot write CZ_VK_DRAW_CENSUS -> %s\n", named);
        }
    }
    // 2 KB, not 512 bytes, AND A MARKER WHEN IT STILL OVERFLOWS. At 512 a line held
    // about six fetch slots, and the draws with more than that were silently cut short —
    // 215 of 2,967 lines in one operator census, every one of them missing its tail. The
    // census is read by grepping for `DUMMY`, so a truncated line does not look truncated:
    // it looks like a draw that binds nothing wrong, and "zero draws bind the dummy in
    // this frame" was about to be reported as a measurement when it was a buffer size
    // (gotchas 25 and 109 — a capped line is not a count).
    // 8192, not 2048: part 31 asks this line for the ground shader's THIRTY-TWO declared
    // pixel constants at once (~45 chars each), and they have to be on ONE line for the
    // same reason the bindings do — a second draw is a different piece of geometry. At
    // 2048 the constant loop would have stopped silently around register 20, which is
    // the same failure the comment above describes, one field over.
    char psbindLine[8192];
    bool psbindFull = false;
    // The pass's WRITE state belongs on this line too. "colour = f(constants,
    // textures)" is only true of a draw that writes its colour at all: an empty
    // RB_COLOR_MASK makes a pipeline that discards every channel, and its output is
    // indistinguishable from a shader that computed black. 43% of this title's draws
    // arrive with an empty mask, so the question is live for every one of them.
    // The census line carries the VERTEX COUNT and the vertex shader as well, because
    // that is how a surface is identified without being able to click on it: the ground
    // is a small number of very large draws, and the HUD is a great many tiny ones.
    int psbindAt =
        drawCensus
            ? snprintf(psbindLine, sizeof psbindLine,
                       "draw %llu verts=%u prim=%u vs=%016llx ps=%016llx mask=%X "
                       "blend=%08X",
                       (unsigned long long)R->drawsThisFrame, draw.indexCount, draw.primType,
                       (unsigned long long)vsBind.hash, (unsigned long long)psBind.hash,
                       regs[xenos::kRbColorMask] & 0xF, regs[xenos::kRbBlendControl0])
        : psbind ? snprintf(psbindLine, sizeof psbindLine,
                            "[psbind] frame=%llu ps=%016llx mask=%X blend=%08X",
                            (unsigned long long)R->frame,
                            (unsigned long long)psBind.hash,
                            regs[xenos::kRbColorMask] & 0xF,
                            regs[xenos::kRbBlendControl0])
                 : 0;

    R->lastTexAddr = 0;
    R->lastTexSlot = 0;
    // WHICH DESCRIPTOR-INDEX ARRAY A SLOT'S INDEX GOES INTO IS THE SHADER'S ANSWER, and
    // for the whole of phase 5 this lambda gave the same one for every fetch: the
    // Texture2D array at +0. The shared constants carry four arrays — Texture2D at +0,
    // Texture3D at +64, TextureCube at +128, Texture1D at +288, matching descriptor sets
    // 0/1/2/4 — and the block is memset to zero every draw, so a cube fetch read index 0,
    // which is a defined 1x1 WHITE texel. 92 of this cache's 397 shaders sample a cube
    // map, so every reflective surface in this game multiplied its specular by white from
    // the first frame phase 5 ever drew. docs/open-items.md item 00.
    //
    // The dimension is per SLOT and comes from the sidecar (part 25). A sidecar written
    // before that has none, and the fallback is 2D — the old behaviour exactly — but it
    // is COUNTED, because a silent fallback here is indistinguishable from the defect it
    // replaces.
    auto bindTextures = [&](const std::vector<uint32_t>& consts,
                            const std::vector<uint32_t>& dims) {
        for (size_t i = 0; i < consts.size(); i++)
        {
            const uint32_t constIdx = consts[i];
            if (constIdx >= 16)
                continue;
            uint32_t dim = 1; // 2D
            if (dims.size() == consts.size())
                dim = dims[i];
            else
                Count("texture: shader sidecar has no tfetchDims — slot bound as 2D");
            // COUNTED BEFORE THE ARM CAN REWRITE IT. The first version of the two cube
            // counters below sat after the CZ_VK_NO_CUBE forcing, so on the very arm the
            // A/B is read against they could not fire at all — and a poisoned-dummy run
            // that showed no magenta was then unreadable, because nothing said whether any
            // draw had asked for a cube in that era. This is the denominator for every
            // cube claim about a given RECIPE, as opposed to about a whole run.
            if (dim == 3)
                Count("draw: shader asked for a CUBE map");
            // CZ_VK_NO_CUBE=1 — bind every cube fetch the way the renderer did before
            // part 25: publish its slot into the Texture2D array, leaving the cube array
            // at zero so the shader samples the white dummy. The same-binary control arm
            // for every claim this change makes about the picture.
            static const bool noCube = EnvOn("CZ_VK_NO_CUBE");
            if (noCube && dim == 3)
            {
                dim = 1;
                Count("texture: cube fetch forced back to the 2D array (CZ_VK_NO_CUBE)");
            }
            const size_t snapsBefore = R->snapshotsSampledThisPass.size();
            const uint32_t slot = UploadTexture(base, regs, constIdx, dim);
            if (!R->lastTexAddr)
            {
                const xenos::TextureFetch t0 = xenos::DecodeTextureFetch(regs, constIdx);
                R->lastTexAddr = t0.address;
                R->lastTexSlot = slot;
                R->lastTexW = t0.width;
                R->lastTexH = t0.height;
            }
            // The disagreement, printed rather than merely counted. Recomputed here and
            // not inside UploadTexture because only this scope knows which SHADERS are
            // bound, and a slot number means nothing without the shader that declared it.
            if (g_dimDisagree)
            {
                const xenos::TextureFetch td =
                    xenos::DecodeTextureFetch(regs, constIdx);
                if (td.type == 2 && td.dimension != dim)
                {
                    uint64_t k = psBind.hash ^ (uint64_t(constIdx) << 56)
                                 ^ (uint64_t(td.address) << 20) ^ td.dimension;
                    DimDisagree& e = g_dimDisagreements[k];
                    if (!e.fetches)
                    {
                        e.psHash = psBind.hash;
                        e.vsHash = vsBind.hash;
                        e.slot = constIdx;
                        e.shaderDim = dim;
                        e.constDim = td.dimension;
                        e.addr = td.address;
                        e.w = td.width;
                        e.h = td.height;
                        e.fmt = td.format;
                    }
                    ++e.fetches;
                }
                if (td.type == 2 && td.dimension != dim && g_dimDisagreeLeft > 0)
                {
                    --g_dimDisagreeLeft;
                    const uint32_t* fc =
                        regs + xenos::kFetchConstantBase + constIdx * 6;
                    fprintf(stderr,
                            "[dimdis] frame=%llu draw=%llu vs=%016llx ps=%016llx "
                            "slot=%u shaderDim=%u constDim=%u depth=%u addr=%08X "
                            "%ux%u fmt=%u | %08X %08X %08X %08X %08X %08X\n",
                            (unsigned long long)R->frame,
                            (unsigned long long)R->drawsThisFrame,
                            (unsigned long long)vsBind.hash,
                            (unsigned long long)psBind.hash, constIdx, dim,
                            td.dimension, td.depth, td.address, td.width, td.height,
                            td.format, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5]);
                    // THE WHOLE FILE, because the two candidate causes differ in what the
                    // OTHER slots hold. A lost constant leaves the slot reading as some
                    // neighbour's 2D texture; a decode error leaves a slot somewhere that
                    // does read as a cube. Either is visible here and neither is
                    // inferable from the offending slot alone.
                    for (uint32_t s = 0; s < 32; s++)
                    {
                        const xenos::TextureFetch o =
                            xenos::DecodeTextureFetch(regs, s);
                        if (o.type != 2 || !o.address)
                            continue;
                        fprintf(stderr,
                                "[dimdis]     s%-2u %08X %4ux%-4u fmt=%-3u dim=%u "
                                "depth=%u tiled=%u\n",
                                s, o.address, o.width, o.height, o.format, o.dimension,
                                o.depth, o.tiled ? 1u : 0u);
                    }
                }
            }
            if (g_dimCensus)
            {
                DimClass& c = g_dimClasses[dim];
                ++c.fetches;
                for (uint32_t d = 0; d < 6; d++)
                {
                    const uint32_t v =
                        regs[xenos::kFetchConstantBase + constIdx * 6 + d];
                    c.andMask[d] &= v;
                    c.orMask[d] |= v;
                }
                ++c.d2Top[regs[xenos::kFetchConstantBase + constIdx * 6 + 2] >> 26];
            }
            // The four arrays are 16 uints each and the index is the fetch-constant
            // slot, so a switch on the dimension is the whole publication step. An
            // unmapped dimension is COUNTED and falls back to 2D rather than writing
            // outside the block it was handed.
            uint32_t arrayBase = kSharedTex2D;
            switch (dim)
            {
                case 0: arrayBase = kSharedTex1D; break;
                case 1: arrayBase = kSharedTex2D; break;
                case 2: arrayBase = kSharedTex3D; break;
                case 3: arrayBase = kSharedTexCube; break;
                default:
                    Count("texture: shader declared an unknown dimension — bound as 2D");
                    break;
            }
            // DID THE CUBE BINDING ACTUALLY REACH A DRAW? Counted on both sides, because
            // the first picture A/B of this change came back pixel-identical on every
            // admissible frame and there was no way to tell "the cube maps look like the
            // dummy" from "no draw in these frames ever received one" (gotcha 151).
            if (dim == 3)
                Count(slot ? "draw: bound a REAL cube map"
                           : "draw: cube fetch got the dummy");
            reinterpret_cast<uint32_t*>(shared + arrayBase)[constIdx] = slot;
            reinterpret_cast<uint32_t*>(shared + kSharedSampler)[constIdx] =
                SamplerIndexForFetch(regs, constIdx);
            if (psbind && psbindAt >= int(sizeof psbindLine) - 96)
                psbindFull = true;
            if (psbind && psbindAt < int(sizeof psbindLine) - 96)
            {
                const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
                psbindAt += snprintf(
                    psbindLine + psbindAt, sizeof psbindLine - psbindAt,
                    // `dim` and `depth` are here so this line and
                    // `tools/xtr_draw_bindings.py`'s carry the same fields: the capture
                    // and the runtime describing one draw in one vocabulary is what makes
                    // the two diffable without a human transcribing columns, and part 27
                    // did that transcription by hand for every comparison it made.
                    // mip=lo..hi and mipAddr are here for the same reason, and were added
                    // in part 39: the guest names a SEPARATE mip-chain address and a
                    // level clamp, this renderer uploads exactly one level, and until
                    // both censuses printed the fields nobody on either side could say
                    // how much of the chain was being thrown away.
                    "  cc=%08X ar=%.3f"
                    "  s%u=%08X %ux%u fmt=%u dim=%u depth=%u swz=%03X tiled=%u "
                    "pitchBlk=%u end=%u mip=%u..%u mipAddr=%08X slot=%u%s%s",
                    // RB_COLORCONTROL and RB_ALPHA_REF beside every texture binding
                    // (part 40): the register that decides whether a cutout happens was
                    // not in the census, so every alpha-test question had to be answered
                    // by cross-referencing a hardware trace instead of by reading the
                    // line. Repeated per fetch slot like everything else on the line —
                    // redundant, greppable.
                    regs[xenos::kRbColorControl], F32(regs[xenos::kRbAlphaRef]),
                    constIdx, t.address, t.width, t.height, t.format, t.dimension,
                    t.depth, t.swizzle,
                    t.tiled ? 1u : 0u, t.pitchBlocks, t.endian, t.mipMin, t.mipMax,
                    t.mipAddress, slot,
                    slot == 0 ? "(DUMMY)" : "",
                    R->snapshotsSampledThisPass.size() > snapsBefore ? "(snap)" : "");
            }
        }
    };
    bindTextures(ps.tfetchConsts, ps.tfetchDims);
    bindTextures(vs.tfetchConsts, vs.tfetchDims);

    // CZ_VK_TEX_DUMP=<dir> plus CZ_VK_TEX_DUMP_PS=<pixel shader hash> — write out the raw
    // guest bytes of every texture the draws using that SHADER sample, once per address.
    //
    // WHY BY SHADER AND NOT BY ADDRESS (part 40). The address form of this instrument is
    // unusable for anything the streaming system owns. Part 39 identified the foliage
    // material from an operator capture, took its six texture addresses, replayed the
    // route headlessly with CZ_VK_TEX_DUMP_ADDR pointed at them — and got back a picture
    // of BARBED WIRE. A guest address is a fact about one boot's streaming heap; the
    // shader hash is a fact about the material and is stable across boots by
    // construction, because it is a hash of the microcode. So the shader is the handle
    // that survives the trip from "the operator saw this" to "reproduce it headlessly",
    // and it is the one this project keeps needing (gotchas 291, 302 are both the same
    // error: naming a draw by something that is not its identity).
    //
    // The bytes are written TILED, exactly as they sit in guest memory, because that is
    // what can be checked against a hardware capture's own MemoryRead without either side
    // having decoded anything first. Decode offline with tools/tex_decode.py --tiled
    // --swap16 --pitchblk, all three of which the filename carries.
    static const char* texDumpDirPs = Env("CZ_VK_TEX_DUMP");
    static const char* texDumpPs = Env("CZ_VK_TEX_DUMP_PS");
    if (texDumpDirPs && texDumpPs)
    {
        char psHex[24];
        snprintf(psHex, sizeof psHex, "%016llx", (unsigned long long)psBind.hash);
        if (strstr(texDumpPs, psHex))
        {
            for (uint32_t constIdx : ps.tfetchConsts)
            {
                const xenos::TextureFetch t = xenos::DecodeTextureFetch(regs, constIdx);
                // A dumped address is remembered so a material drawn 700 times in a frame
                // writes 6 files and not 4,200 — and so the file on disk is the FIRST
                // sighting, which is the one the census line beside it describes.
                static std::set<uint32_t> dumped;
                if (!t.address || t.width == 0 || t.height == 0 ||
                    !dumped.insert(t.address).second)
                    continue;
                // The tiled footprint, the same rule the untiler uses: pitch and rows
                // both round up to 32 units. Sizing at width*height short-reads every
                // tiled surface whose extent is not a multiple of the tile, and a short
                // dump does not announce itself — it decodes as a texture with its
                // right-hand blocks missing, which reads as a decode defect (gotcha 296).
                uint32_t bpu = 1, unit = 1;
                switch (t.format)
                {
                    case xenos::kFmt_DXT1: bpu = 8; unit = 4; break;
                    case xenos::kFmt_DXT2_3:
                    case xenos::kFmt_DXT4_5: bpu = 16; unit = 4; break;
                    case xenos::kFmt_8_8_8_8: bpu = 4; break;
                    case xenos::kFmt_8: bpu = 1; break;
                    default: continue;   // never guess a stride; say nothing instead
                }
                const uint32_t uw = (t.width + unit - 1) / unit;
                const uint32_t uh = (t.height + unit - 1) / unit;
                uint32_t pitch = t.pitchBlocks ? t.pitchBlocks * 32 / unit : uw;
                uint32_t rows = uh;
                if (t.tiled)
                {
                    pitch = (pitch + 31) & ~31u;
                    rows = (rows + 31) & ~31u;
                }
                const uint64_t bytes = uint64_t(pitch) * rows * bpu;
                const uint32_t va = PhysToVa(t.address);
                if (bytes == 0 || bytes > (8u << 20) || !GuestRangeOk(va, bytes))
                    continue;
                char path[512];
                snprintf(path, sizeof path,
                         "%s/psdump_%s_%08X_%ux%u_fmt%u_tiled%u_pitchblk%u_end%u.bin",
                         texDumpDirPs, psHex, t.address, t.width, t.height, t.format,
                         t.tiled ? 1u : 0u, t.pitchBlocks, t.endian);
                if (FILE* f = fopen(path, "wb"))
                {
                    fwrite(base + va, 1, size_t(bytes), f);
                    fclose(f);
                    Count("texture: dumped for CZ_VK_TEX_DUMP_PS");
                }
            }
        }
    }

    // CZ_VK_ONLY_TEX / CZ_VK_SKIP_TEX=<hex[,hex...]> — render only, or all but, the
    // draws whose first bound texture is at that guest address.
    //
    // The bisection arms one level down from CZ_VK_ONLY_VS. A UI compose is a hundred
    // quads sharing two shaders, so "which shader draws this" cannot separate them and
    // "which TEXTURE does this draw sample" can. It is how a rectangle on screen gets
    // an identity: skip one address, look at what vanished. That turns "the save-slot
    // boxes are black" into "the save-slot boxes are texture 0364B000", which is a
    // question with an answer.
    //
    // CZ_VK_TEX_FILTER_FILE=<path> is the same two arms, RE-READ WHILE THE GAME RUNS.
    // The env forms are latched once per process, which is unusable for the defect this
    // was built for: the striped-material class picks a different streamed quality level
    // on every boot, so the address to isolate is only known from a census taken INSIDE
    // the boot that shows it, and by then the process has already read its environment.
    // The file holds one line, `only=<hex[,hex...]>` or `skip=<hex[,hex...]>` (empty
    // file or missing = no filtering), and is re-read when its mtime changes — one stat
    // per frame, not per draw, so it costs nothing on the draw path. It is what lets an
    // operator standing in front of the blotch have textures isolated under them.
    {
        static const char* onlyTex = Env("CZ_VK_ONLY_TEX");
        static const char* skipTex = Env("CZ_VK_SKIP_TEX");
        static const char* filterFile = Env("CZ_VK_TEX_FILTER_FILE");
        static std::string fileOnly, fileSkip;
        if (filterFile)
        {
            // ONCE PER FRAME, NOT PER DRAW. `frame` advances in Host_Present, so this
            // stats the file a few hundred times a minute rather than a few hundred
            // thousand — and a filter that cost frame time would change the picture it
            // is being used to read (gotcha 7).
            static uint64_t lastFrame = ~0ull;
            static int64_t lastMtime = -1;
            if (R->frame != lastFrame)
            {
                lastFrame = R->frame;
                std::error_code ec;
                const auto mt = std::filesystem::last_write_time(filterFile, ec);
                const int64_t now = ec ? -1 : mt.time_since_epoch().count();
                if (now != lastMtime)
                {
                    lastMtime = now;
                    fileOnly.clear();
                    fileSkip.clear();
                    if (FILE* f = fopen(filterFile, "rb"))
                    {
                        char line[512];
                        while (fgets(line, sizeof line, f))
                        {
                            char* nl = strpbrk(line, "\r\n");
                            if (nl) *nl = 0;
                            if (!strncmp(line, "only=", 5)) fileOnly = line + 5;
                            else if (!strncmp(line, "skip=", 5)) fileSkip = line + 5;
                        }
                        fclose(f);
                    }
                    // ANNOUNCE EVERY CHANGE. A filter that silently failed to parse
                    // looks exactly like a texture that is not drawn — the arm would
                    // fail AS the symptom it is used to find (gotcha 279).
                    fprintf(stderr, "[vk] tex filter reloaded: only='%s' skip='%s'\n",
                            fileOnly.c_str(), fileSkip.c_str());
                }
            }
        }
        const char* onlyEff = !fileOnly.empty() ? fileOnly.c_str() : onlyTex;
        const char* skipEff = !fileSkip.empty() ? fileSkip.c_str() : skipTex;
        if (onlyEff || skipEff)
        {
            char hex[16];
            snprintf(hex, sizeof hex, "%08X", R->lastTexAddr);
            if (onlyEff && !strstr(onlyEff, hex))
            {
                Count("draw: filtered out (CZ_VK_ONLY_TEX)");
                return;
            }
            if (skipEff && strstr(skipEff, hex))
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
            bool pcTruncated = false;
            while (at < spec.size())
            {
                if (psbindAt >= int(sizeof psbindLine) - 64)
                {
                    // A constant list cut short reads as a shorter list, not as an
                    // error, and the reader then concludes the shader does not use the
                    // registers that fell off the end. Say it out loud (gotcha 109).
                    pcTruncated = true;
                    break;
                }
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
            if (pcTruncated && psbindAt < int(sizeof psbindLine) - 32)
                psbindAt += snprintf(psbindLine + psbindAt,
                                     sizeof psbindLine - psbindAt,
                                     "  (PC LIST TRUNCATED)");
        }
        // The census wants EVERY draw, so it bypasses the distinct-binding filter that
        // makes `[psbind]` readable. That filter is what would hide the one line being
        // looked for: a second draw with the same shader and the same bindings is a
        // different piece of geometry, and "which draw covers the ground" is precisely a
        // question about geometry.
        if (psbindFull && psbindAt < int(sizeof psbindLine) - 24)
            snprintf(psbindLine + psbindAt, sizeof psbindLine - psbindAt,
                     "  (LINE TRUNCATED)");
        if (drawCensus)
        {
            if (R->drawCensusFile)
            {
                fprintf(R->drawCensusFile, "%s\n", psbindLine);
                ++R->drawCensusLines;
            }
        }
        else
        {
            const char* bindings = strstr(psbindLine, "mask=");
            std::string key(bindings ? bindings : psbindLine);
            if (std::find(seenBind.begin(), seenBind.end(), key) == seenBind.end() &&
                seenBind.size() < 64)
            {
                seenBind.push_back(key);
                fprintf(stderr, "%s\n", psbindLine);
            }
        }
    }

    // g_SwappedTexcoords — one bit per TEXCOORD semantic that the generated
    // `tfetchTexcoord` uses to apply an EXTRA .yxwz unswizzle. THE MASK IS ZERO NOW,
    // ON PURPOSE, and the reasoning is worth the paragraph because this exact spot has
    // flip-flopped twice (§6h set it, part 37 zeroed it — phase5-notes §6bo):
    //
    // CopySwapped's 8-in-32 dword reverse leaves a 16-bit pair (a) per-component
    // little-endian, which Vulkan wants, and (b) TRANSPOSED in order — which is
    // byte-for-byte the state the real Xenos fetch pipe hands the shader after its own
    // endian stage. The Xenos shader compiler knows that, which is why ~87% of 16-bit
    // vfetches in this title's microcode carry a compensating yx/yxwz DESTINATION
    // swizzle (the Fable 2 census, confirmed live here). XenosRecomp translates that
    // swizzle faithfully, so the shader's own code is already the complete correction:
    // with the mask at zero, our result equals hardware's for every fetch, swizzled or
    // not. With a mask bit SET, the pair is corrected TWICE — i.e. transposed again —
    // which painted baked-lightmap prop shadows across the tanker, Dick's far LOD and
    // the pawnshop boards (the item-0s striped-material class): the lightmap UV is a
    // 16_16 TEXCOORD2 read through a yx-swizzled vfetch. §6h's "63.8% -> 81.3%"
    // justification for the mask was measured on the animated-title-camera metric §6k
    // later retracted; §6n's null (mask off = no measurable frame-wide change) was
    // true because the damage is localized to lightmapped props, which no whole-frame
    // statistic can see.
    //
    // CZ_VK_TEXCOORD_SWAP=1 republishes the old mask — the same-binary control arm
    // that repaints the blotches. (CZ_VK_NO_TEXCOORD_SWAP is accepted and now a no-op,
    // so old recipes keep meaning what they meant.)
    //
    // The semantic index comes from the Vulkan location, because that is what the
    // container synthesizer keyed both sides on: TEXCOORD0..3 are locations 4..7 and
    // TEXCOORD4..23 are locations 12..31 (its USAGE_LOCATION table).
    {
        static const bool oldMask = EnvOn("CZ_VK_TEXCOORD_SWAP");
        uint32_t swapped = 0;
        if (oldMask)
        {
            for (const VertexAttribute& a : vs.attributes)
            {
                if (a.location < 4 || a.indirect)
                    continue;
                const bool sixteenBit = a.format == 25 || a.format == 26 ||
                                        a.format == 31 || a.format == 32;
                if (!sixteenBit)
                    continue;
                const uint32_t texcoord =
                    a.location < 12 ? uint32_t(a.location - 4) : uint32_t(a.location - 8);
                if (texcoord < 32)
                    swapped |= 1u << texcoord;
            }
            if (swapped)
                COUNT("draw: 16-bit texcoord DOUBLE-unswizzle republished (control arm)");
        }
        reinterpret_cast<uint32_t*>(shared + kSharedSwappedTexcoords)[0] = swapped;
    }

    // g_AlphaThreshold — RB_ALPHA_REF, with func GREATER made STRICT (part 40).
    //
    // The shaders' emitted test is `clip(oC0.w - g_AlphaThreshold)`, which keeps
    // w >= threshold. For func GEQUAL that is exact. For func GREATER it differs at
    // exactly w == ref — and this title leans on that difference with its whole
    // weight: the leaf-card foliage draws OPAQUE with GREATER at ref = 0.0, meaning
    // "discard the alpha-0 background, keep every lit leaf texel". A >= 0 clip keeps
    // the background too, and the canopy renders as solid sheets of leaf pattern —
    // the operator's "still shards" report, five minutes after the register fix made
    // the test fire at all. Publishing ref + half an 8-bit step (1/512) turns the
    // emitted >= into the strict > for every value an 8-bit-sourced alpha can take,
    // without touching the shared XenosRecomp emitter. GEQUAL keeps the exact ref.
    {
        const uint32_t cc = regs[xenos::kRbColorControl];
        float thr = F32(regs[xenos::kRbAlphaRef]);
        if ((cc & 0x8) && (cc & 0x7) == 4)
            thr += 1.0f / 512.0f;
        // EQUAL@1.0 (see the pipeline-key block): >= (ref - eps) is equality when
        // nothing can exceed ref. Same half-8-bit-step epsilon as the strict GREATER.
        else if ((cc & 0x8) && (cc & 0x7) == 2 && thr >= 1.0f)
            thr -= 1.0f / 512.0f;
        reinterpret_cast<float*>(shared + kSharedAlphaThreshold)[0] = thr;
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

    // NOT the presented frame's extent — and this fallback is NOT where that matters.
    //
    // Part 14 tried computing a per-pass extent here from PA_SC_WINDOW_SCISSOR's
    // bottom-right corner, on the theory that the shadow pass renders a 1024x1024
    // cascade and was being folded and clipped as if it were 1280x720. The reading was
    // wrong and the measurement says so in one line: the cascade's real draws set
    // `vte=3F` with `xs=512, ys=-512`, i.e. they take the REAL viewport path above and
    // already get 1024x1024. Only its clears land here. The change moved 358,993 draws
    // and left the cascade bit-identical (mean 120.4, 48.7% zero, before and after), so
    // it was reverted rather than kept on a plausible story.
    //
    // What the cascade's half-empty look actually is remains open; it is NOT this.
    float posScale[2] = { 1.0f, 1.0f };
    float posOffset[2] = { 0.0f, 0.0f };
    if (!(vte & 0x1))
    {
        // Window coordinates: map [0, w] x [0, h] to clip [-1, 1] x [-1, 1]. Vulkan's
        // clip Y already points down like the window's, so there is no flip here — and
        // adding one is the double-flip that cost the previous port a session.
        //
        // w AND h ARE THE EDRAM'S, NOT THE PRESENTED FRAME'S. A window coordinate is
        // relative to the surface the pass is rendering into, and that surface can be
        // taller than the screen: this title's shadow cascade is 1024 rows. Mapped
        // through the front buffer's 720 the arithmetic still comes out as an identity
        // (the fallback viewport below divides it straight back out) — but the CLIP
        // does not, because everything past window y = 720 lands outside the clip
        // volume and is thrown away before it reaches the framebuffer. The title
        // clears its cascade in 64-wide vertical STRIPS of (x, 0)-(x+64, 1024), so
        // every one of them was being cut off at row 719 and the bottom 304 rows of
        // every shadow cascade were left at whatever the image was created with.
        // Same identity, same 1280x720 result for every screen-sized pass; the only
        // draws that move are the ones that were being clipped.
        //
        // CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1 restores the front buffer's extent — the
        // pre-part-15 behaviour, and the same-binary control arm for every claim about
        // the shadow cascade's bottom rows.
        static const bool windowCoordsFront = EnvOn("CZ_VK_WINDOW_COORDS_FRONT_BUFFER");
        const uint32_t winW = windowCoordsFront ? R->targetWidth : R->edramWidth;
        const uint32_t winH = windowCoordsFront ? R->targetHeight : R->edramHeight;
        posScale[0] = 2.0f / float(winW);
        posScale[1] = 2.0f / float(winH);
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
        // Y is scaled for a 4x surface as well as X — THE DEFAULT since part 34.
        //
        // Xenos 4x MSAA is a 2x2 sample grid, so a 4x surface is twice as wide AND twice
        // as tall in samples; our EDRAM stand-in is at sample resolution in X, so both
        // axes take the draw's OWN declared sample factor. The case that forced it is
        // the SHADOW CASCADE's clear, `(0,0)-(480,512)` on a 520-pitch 4x surface:
        // doubled in both axes it tiles the 1024x1024 cascade EXACTLY with the title's
        // other rect `(960,0)-(1024,1024)`; doubled in X only, the union is the 53.125%
        // coverage part 32 measured, and the rejected half read as always-occluded.
        //
        // Part 32 held this back because the SCENE tile's 4x clear `(0,0)-(320,720)`
        // seemed to want X and not Y. The reconciliation (§6bf): the title re-declares
        // a surface as 4x purely to clear it, so a clear rect is in the CLEAR
        // declaration's pixel space, not the render's — the scene tile renders 2x
        // (Y-doubled in samples already), so its 4x clear's Y-doubled rect covers the
        // same rows, just expressed differently. Doubling Y here OVER-clears past the
        // tile's rows into the shared stand-in (rows 720..1024) exactly as the X factor
        // has always over-cleared the 640x360 post surface to 1280 wide — the same
        // approximation, harmless for the same reason (measured: title screen unmoved,
        // cascade atlas 46.875% -> ~0.004% zero). The exact form is a stand-in at
        // sample resolution in BOTH axes with downsampling resolves; until then the
        // per-axis factor is carried here, at the one window-coordinate site.
        //
        // CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1 is the same-binary control arm (the part-33
        // renderer). The part-32 arm variable CZ_VK_MSAA_WINDOW_SCALE_Y is RETIRED and
        // no longer read: setting it asks for what is now the default, and reading two
        // variables for one axis invites the contradictory pair.
        static const bool noMsaaScaleY = EnvOn("CZ_VK_NO_MSAA_WINDOW_SCALE_Y");
        if (!noMsaaScaleY && !noMsaaScale && msaa == 2)
        {
            posScale[1] *= 2.0f;
            Count("draw: window Y also scaled for a 4x MSAA surface");
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
            posOffset[0] += 2.0f * float(tileX) / float(winW);
            posOffset[1] += 2.0f * float(tileY) / float(winH);
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
        // The EDRAM's extent, matching the posScale above — the two are one mapping and
        // splitting them would put a scale factor between window coordinates and
        // pixels. See the vte==0 branch for why it is not the presented frame's.
        static const bool windowCoordsFront = EnvOn("CZ_VK_WINDOW_COORDS_FRONT_BUFFER");
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = float(windowCoordsFront ? R->targetWidth : R->edramWidth);
        viewport.height = float(windowCoordsFront ? R->targetHeight : R->edramHeight);
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
    static const bool viewportTrace = EnvOn("CZ_VK_VIEWPORT_TRACE");
    if (viewportTrace)
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
                 "surfaceInfo=%08X depthControl=%02X",
                 vte & 0x3F, xs, xo, ys, yo, viewport.x, viewport.y, viewport.width,
                 viewport.height, scissor.offset.x, scissor.offset.y,
                 scissor.extent.width, scissor.extent.height,
                 regs[xenos::kPaScWindowOffset], posScale[0], posScale[1], posOffset[0],
                 posOffset[1], regs[xenos::kRbSurfaceInfo] & 0x3FFF,
                 (regs[xenos::kRbSurfaceInfo] >> 16) & 3, regs[xenos::kRbSurfaceInfo],
                 regs[xenos::kRbDepthControl] & 0xFF);
        if (std::find(seen.begin(), seen.end(), line) == seen.end() && seen.size() < 64)
        {
            seen.push_back(line);
            fprintf(stderr, "%s\n", line);
        }
    }

    ProfScope _pRecord(&g_prof.record);
    // Only what has actually changed since the last draw on this command buffer. See
    // Renderer::BoundState for why that is sound; CZ_VK_NO_STATE_CACHE=1 re-issues
    // everything every draw, which is the pre-part-18 renderer and the control arm.
    static const bool noStateCache = Env("CZ_VK_NO_STATE_CACHE") != nullptr;
    ++R->skips.draws;
    const float blendConstants[4] = {
        F32(regs[xenos::kRbBlendRed]), F32(regs[xenos::kRbBlendRed + 1]),
        F32(regs[xenos::kRbBlendRed + 2]), F32(regs[xenos::kRbBlendRed + 3])
    };
    if (noStateCache || pipeline != R->bound.pipeline)
    {
        vkCmdBindPipeline(R->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        R->bound.pipeline = pipeline;
    }
    else
        ++R->skips.pipeline;
    if (noStateCache || !R->bound.haveViewport ||
        memcmp(&viewport, &R->bound.viewport, sizeof(viewport)) != 0)
    {
        vkCmdSetViewport(R->cmd, 0, 1, &viewport);
        R->bound.viewport = viewport;
        R->bound.haveViewport = true;
    }
    else
        ++R->skips.viewport;
    if (noStateCache || !R->bound.haveScissor ||
        memcmp(&scissor, &R->bound.scissor, sizeof(scissor)) != 0)
    {
        vkCmdSetScissor(R->cmd, 0, 1, &scissor);
        R->bound.scissor = scissor;
        R->bound.haveScissor = true;
    }
    else
        ++R->skips.scissor;
    if (noStateCache || !R->bound.haveBlend ||
        memcmp(blendConstants, R->bound.blend, sizeof(blendConstants)) != 0)
    {
        vkCmdSetBlendConstants(R->cmd, blendConstants);
        memcpy(R->bound.blend, blendConstants, sizeof(blendConstants));
        R->bound.haveBlend = true;
    }
    else
        ++R->skips.blend;
    // The five bindless heaps never change address, so this is once per command
    // buffer rather than once per draw — and it is the most expensive of the five.
    if (noStateCache || !R->bound.setsBound)
    {
        vkCmdBindDescriptorSets(R->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipeLayout,
                                0, 5, R->sets, 0, nullptr);
        R->bound.setsBound = true;
    }
    else
        ++R->skips.sets;

    // The three constant-buffer addresses, then THE DRAW INDEX at offset 24 for the
    // draw-ID pass. The index is pushed on every draw, armed or not: it costs four bytes
    // in a call that is already being made, and a value that is only correct when an
    // instrument is enabled is a trap for the next person to use it.
    struct { uint64_t vs, ps, shared; uint32_t drawIndex, pad; } pushConstants = {
        uint64_t(R->arena.address + vsConstAt),
        uint64_t(R->arena.address + psConstAt),
        uint64_t(R->arena.address + sharedAt),
        uint32_t(R->drawsThisFrame), 0 };
    vkCmdPushConstants(R->cmd, R->pipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32,
                       &pushConstants);

    // CZ_VK_STATE_PROBE=1 — the distinct values of the state registers this renderer
    // ASSUMES rather than reads. Each of these is a place where a wrong assumption
    // produces a plausible wrong picture instead of an error, so the cheap version of
    // checking them is to print what the guest actually writes.
    static const bool stateProbe = EnvOn("CZ_VK_STATE_PROBE");
    if (stateProbe)
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
    static const bool fetchProbe = EnvOn("CZ_VK_FETCH_PROBE");
    if (fetchProbe && !vs.attributes.empty())
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
        const StreamLoc loc = UploadStream(base, sva, bytes, vf.endian, 2);
        if (!loc.ok())
            continue;
        // {deviceAddress.lo, deviceAddress.hi, sizeDwords, 0} — the layout the
        // generated XeVfetchDep reads, transcribed from shader_common.h. A raw device
        // address, so which buffer the stream landed in is invisible to the shader.
        const uint64_t addr = loc.address();
        uint32_t* entry =
            reinterpret_cast<uint32_t*>(shared + kSharedVfetchTable + a.fetchSlot * 16);
        entry[0] = uint32_t(addr);
        entry[1] = uint32_t(addr >> 32);
        entry[2] = vf.sizeDwords;
        entry[3] = 0;
        COUNT("draw: dependent fetch stream published");
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
    // VGT_INDX_OFFSET — the base vertex every index of this draw is measured from.
    //
    // A draw packet carries no base-vertex field, so this register is the ONLY way a
    // title sub-allocates one vertex buffer between draws — and this one does exactly
    // that for its whole UI. Its save-slot screen fills a single ~400 KB dynamic buffer
    // per frame and issues 115 draws whose fetch constant never changes address: only
    // the offset moves, by precisely the previous draw's index count (0, 16, 20, 68,
    // 84, 88, 136, ...). Ignoring it does not drop anything or fail anywhere — every
    // draw renders the FIRST run's vertices, so one text run comes out correct and
    // every other one is that same run's glyphs sampled through whichever atlas the
    // draw happened to bind. That reads as a font or texture-coordinate bug, which is
    // where two sessions looked, and the atlas and the UVs are both perfect.
    //
    // CZ_VK_NO_INDX_OFFSET=1 is the same-binary control arm: the pre-fix renderer.
    static const bool noIndxOffset = EnvOn("CZ_VK_NO_INDX_OFFSET");
    const int32_t indxOffset =
        noIndxOffset ? 0 : int32_t(regs[xenos::kVgtIndxOffset] & 0xFFFFFF);

    static const bool rectHalf = EnvOn("CZ_VK_RECT_HALF");
    const bool rectSynth =
        expand == Expansion::RectangleList && draw.indexCount == 3 && !rectHalf;
    uint32_t rectCorner[3] = { 0, 1, 2 };
    if (rectSynth)
    {
        const uint8_t* isrc = draw.indexed ? base + draw.indexVa : nullptr;
        // The offset applies here too, and it has to be folded in RATHER than passed to
        // the draw call: this path resolves the three real corners into a private
        // four-vertex stream, so by draw time the indices are 0..3 of a buffer that has
        // no base to offset from.
        for (uint32_t k = 0; k < 3; k++)
            rectCorner[k] =
                uint32_t(int32_t(ReadIndex(isrc, k, draw.index32, draw.indexEndian,
                                           draw.indexed)) +
                         indxOffset);
    }

    uint32_t binding = 0;
    bool streamsOk = true;
    // CZ_VK_RANGE_CENSUS=1 — per indexed draw, the two questions the part-33 NaN chain
    // left: (1) do this draw's INDEX VALUES reach vertices past the fetch constant's
    // declared size — the guard above bounds indxOffset + indexCount, which is the
    // number of indices, not the vertices they name — and (2) do the IN-RANGE bytes of
    // any float-format attribute already decode to NaN. On Xenos an out-of-range vfetch
    // returns zero; our streams live in one shared arena buffer, so even
    // robustBufferAccess cannot bound them (the binding's range is the whole arena),
    // and an overrun reads a neighbouring stream's bytes as this draw's floats.
    // A DIAGNOSTIC ARM: it walks every index and every vertex of every draw (gotcha 7).
    static const bool rangeCensus = [] {
        const char* e = getenv("CZ_VK_RANGE_CENSUS");
        return e && *e && *e != '0';
    }();
    struct RangeAttr { uint32_t strideDw, offsetDw, format; uint64_t bytes; const uint8_t* p; };
    RangeAttr rangeAttrs[32];
    uint32_t rangeAttrCount = 0;
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
        // The whole fetch buffer is uploaded, so a base vertex stays inside it — but
        // only if the guest's own numbers say it does. A draw whose offset plus count
        // runs off the end would read another draw's vertices (or, past the arena, walk
        // off a Vulkan buffer), and that has to be LOUD rather than quietly clamped
        // back to zero: clamping is the defect this offset exists to fix, reintroduced
        // wearing a safety check's name.
        if (indxOffset && a.strideDwords)
        {
            const uint64_t need =
                (uint64_t(uint32_t(indxOffset) + draw.indexCount) * a.strideDwords) * 4;
            if (need > bytes)
            {
                Count("draw: VGT_INDX_OFFSET runs past the vertex stream");
                streamsOk = false;
                break;
            }
        }
        const StreamLoc loc = UploadStream(base, va, bytes, vf.endian, 0);
        if (!loc.ok())
        {
            streamsOk = false;
            break;
        }
        if (rectSynth && a.strideDwords)
        {
            // CZ_VK_RECT_TRACE=<surfacePitch> — the CORNERS of every distinct clear
            // rect on one EDRAM surface, printed once per distinct rect.
            //
            // A rect-list draw at the head of a pass IS the guest's clear, and the only
            // way to know what it clears is to read its three corners. Part 32 needed
            // this for the shadow cascade: the cascade's depth buffer comes out half
            // empty, the geometry is provably submitted for all of it (CZ_VK_DEPTH_ALWAYS
            // fills it), so the question is which rectangles the title actually asked to
            // be cleared — and that is data, not something to infer from the result.
            static const char* const rectTraceEnv = Env("CZ_VK_RECT_TRACE");
            // A pitch of 0 means EVERY surface. Naming one pitch is right when the
            // question is "what does this pass clear"; it is wrong when the question is
            // "where does that rect live", and part 32 needed the second — a clear rect
            // recorded in part 15 as the cascade's turned out to be on another surface
            // entirely, which is only visible if the trace prints the pitch.
            if (rectTraceEnv &&
                (strtoul(rectTraceEnv, nullptr, 10) == 0 ||
                 (regs[xenos::kRbSurfaceInfo] & 0x3FFF) ==
                     uint32_t(strtoul(rectTraceEnv, nullptr, 10))))
            {
                const uint8_t* p = loc.bytes();
                float c[3][2] = {};
                bool ok = p != nullptr;
                for (uint32_t k = 0; ok && k < 3; k++)
                {
                    const uint64_t at =
                        (uint64_t(rectCorner[k]) * a.strideDwords + a.offsetDwords) * 4;
                    if (at + 8 > bytes) { ok = false; break; }
                    // The arena copy is ALREADY little-endian — the stream uploader
                    // swaps on the way in. Byte-swapping again here read every corner as
                    // 0.0, which reads as "the title asks for nothing to be cleared" and
                    // is the opposite of what the data says.
                    memcpy(&c[k][0], p + at, 8);
                }
                if (ok)
                {
                    static std::vector<std::string> seenRect;
                    // OCCURRENCES AS WELL AS DISTINCT RECTS, and the distinction is the
                    // whole question. "One distinct clear rect" and "one clear rect a
                    // frame" are different facts: the first is consistent with the title
                    // issuing sixteen strips that all look alike, the second says it
                    // issues one. Part 32 needed the second to conclude that the cascade
                    // passes clear nothing of their own.
                    static int rectOccurrences = 0;
                    if (rectOccurrences < 40)
                    {
                        ++rectOccurrences;
                        fprintf(stderr,
                                "[vkrect] occurrence frame=%llu (%.1f,%.1f)-(%.1f,%.1f) "
                                "idx=%u,%u,%u\n",
                                (unsigned long long)R->frame, c[0][0], c[0][1], c[2][0],
                                c[2][1], rectCorner[0], rectCorner[1], rectCorner[2]);
                    }
                    char line[256];
                    snprintf(line, sizeof line,
                             "[vkrect] pitch=%u msaa=%u loc=%d fmt=%u off=%u "
                             "stride=%u idx=%u,%u,%u  "
                             "(%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f)  -> BL (%.2f,%.2f)  "
                             "depthControl=%02X vte=%02X",
                             regs[xenos::kRbSurfaceInfo] & 0x3FFF,
                             (regs[xenos::kRbSurfaceInfo] >> 16) & 3,
                             a.location, a.format, a.offsetDwords, a.strideDwords,
                             rectCorner[0], rectCorner[1], rectCorner[2],
                             c[0][0], c[0][1], c[1][0], c[1][1], c[2][0], c[2][1],
                             c[0][0] + c[2][0] - c[1][0], c[0][1] + c[2][1] - c[1][1],
                             regs[xenos::kRbDepthControl] & 0xFF,
                             regs[xenos::kPaClVteCntl] & 0x3F);
                    if (std::find(seenRect.begin(), seenRect.end(), line) ==
                            seenRect.end() && seenRect.size() < 64)
                    {
                        seenRect.push_back(line);
                        fprintf(stderr, "%s\n", line);
                    }
                }
            }
            // The synthesised four-corner stream is always in the per-frame arena, even
            // when its source is in the cross-frame store — it is built from THIS draw's
            // corner indices, so it is not shared and must not be persisted.
            const VkDeviceSize four =
                SynthRectStream(loc.bytes(), bytes, a.strideDwords, rectCorner, a.format);
            if (four == VkDeviceSize(-1))
            {
                streamsOk = false;
                break;
            }
            const VkDeviceSize offset = four + uint64_t(a.offsetDwords) * 4;
            NoteVertexBind(binding, R->arena.buffer, offset);
            vkCmdBindVertexBuffers(R->cmd, binding, 1, &R->arena.buffer, &offset);
            ++binding;
            continue;
        }
        const VkDeviceSize offset = loc.at + uint64_t(a.offsetDwords) * 4;
        if (offset >= loc.capacity())
        {
            Count("draw: vertex element offset past the stream");
            streamsOk = false;
            break;
        }
        if (rangeCensus && a.strideDwords && rangeAttrCount < 32)
            rangeAttrs[rangeAttrCount++] = { a.strideDwords, a.offsetDwords,
                                             uint32_t(a.format), bytes, loc.bytes() };
        NoteVertexBind(binding, loc.handle(), offset);
        vkCmdBindVertexBuffers(R->cmd, binding, 1, &loc.buf->buffer, &offset);
        ++binding;
    }
    if (!streamsOk)
        return;
    // The evaluation half of CZ_VK_RANGE_CENSUS, shared by the indexed branch
    // (maxIdx = largest index VALUE) and the auto-index branch (maxIdx = count-1):
    // the reachable-vertex question is the same, only the source of maxIdx differs.
    auto RangeCensusEval = [&](uint32_t maxIdx) {
            const uint32_t lastV = uint32_t(int64_t(maxIdx) + indxOffset);
            bool over = false, nan = false;
            uint64_t nanVerts = 0;
            uint32_t overFmt = 0, nanFmt = 0;
            uint64_t overBy = 0;
            for (uint32_t k = 0; k < rangeAttrCount; k++)
            {
                const RangeAttr& at = rangeAttrs[k];
                uint32_t attrDw = 1;
                switch (at.format)
                {
                case xenos::kFmt_32_32_FLOAT:          attrDw = 2; break;
                case xenos::kFmt_32_32_32_FLOAT:       attrDw = 3; break;
                case xenos::kFmt_32_32_32_32_FLOAT:    attrDw = 4; break;
                case xenos::kFmt_32_32:                attrDw = 2; break;
                case xenos::kFmt_32_32_32_32:          attrDw = 4; break;
                case xenos::kFmt_16_16_16_16:
                case xenos::kFmt_16_16_16_16_FLOAT:    attrDw = 2; break;
                default:                               attrDw = 1; break;
                }
                const uint64_t need =
                    (uint64_t(lastV) * at.strideDw + at.offsetDw + attrDw) * 4;
                if (need > at.bytes)
                {
                    over = true;
                    overFmt = at.format;
                    overBy = std::max(overBy, need - at.bytes);
                }
                // In-range NaN scan, float formats only — these bytes go into the
                // shader as IEEE floats with no conversion to hide behind. FP16 formats
                // are floats too: a NaN half in a 16_16_16_16_FLOAT attribute expands to
                // a NaN float in the shader, and the first census missed them.
                uint32_t floats = 0, halves = 0;
                if (at.format == xenos::kFmt_32_FLOAT) floats = 1;
                else if (at.format == xenos::kFmt_32_32_FLOAT) floats = 2;
                else if (at.format == xenos::kFmt_32_32_32_FLOAT) floats = 3;
                else if (at.format == xenos::kFmt_32_32_32_32_FLOAT) floats = 4;
                else if (at.format == xenos::kFmt_16_FLOAT) halves = 1;
                else if (at.format == xenos::kFmt_16_16_FLOAT) halves = 2;
                else if (at.format == xenos::kFmt_16_16_16_16_FLOAT) halves = 4;
                if ((floats || halves) && at.p)
                {
                    const uint64_t availV =
                        at.bytes / 4 >= at.offsetDw + attrDw
                            ? (at.bytes / 4 - at.offsetDw - attrDw) / at.strideDw + 1
                            : 0;
                    const uint64_t scanV = std::min<uint64_t>(availV, uint64_t(lastV) + 1);
                    for (uint64_t v = 0; v < scanV; v++)
                    {
                        const uint8_t* fp = at.p + (v * at.strideDw + at.offsetDw) * 4;
                        bool vNan = false;
                        for (uint32_t c = 0; c < floats; c++)
                        {
                            uint32_t bits;
                            memcpy(&bits, fp + c * 4, 4);
                            if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu))
                                vNan = true;
                        }
                        for (uint32_t c = 0; c < halves; c++)
                        {
                            uint16_t bits;
                            memcpy(&bits, fp + c * 2, 2);
                            if ((bits & 0x7C00u) == 0x7C00u && (bits & 0x3FFu))
                                vNan = true;
                        }
                        if (vNan)
                        {
                            nan = true;
                            nanFmt = at.format;
                            ++nanVerts;
                        }
                    }
                }
            }
            Count("rangecensus: indexed draw walked");
            if (over) Count("rangecensus: index values reach past a stream");
            if (nan)  Count("rangecensus: NaN bytes IN RANGE in a float attribute");
            static int printed = 0;
            if ((over || nan) && printed < 40)
            {
                ++printed;
                fprintf(stderr,
                        "[range] frame=%llu vs=%016llx idx=%u maxIdx=%u base=%d "
                        "%s%s overFmt=%u overBy=%llu nanFmt=%u nanVerts=%llu\n",
                        (unsigned long long)R->frame,
                        (unsigned long long)vsBind.hash, draw.indexCount, maxIdx,
                        int(indxOffset), over ? "OVERRUN " : "", nan ? "NAN-IN-RANGE " : "",
                        overFmt, (unsigned long long)overBy, nanFmt,
                        (unsigned long long)nanVerts);
            }
    };

    // CZ_VK_DRAW_PROBE=<vsHash> — for the first few draws with that vertex shader,
    // print the constants and the vertex data it will actually read.
    //
    // This exists because the remaining geometry defect is bounded to two inputs and
    // both have been verified in the abstract: `oPos = vc(0..3) * (vc(8..10) *
    // iPosition0)`, the position format needs no conversion, and the fetch slot is
    // settled. When every input checks out and the output is wrong, the next move is
    // not another hypothesis — it is to look at the values.
    static const char* const drawProbeEnv = Env("CZ_VK_DRAW_PROBE");
    if (const char* probe = drawProbeEnv)
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
            // The bound texture belongs on the header line. Two draws through one shader
            // pair can differ ONLY in which texture they sample — that is exactly what
            // CZ_VK_ONLY_TEX exists for — and a probe that does not name it produces two
            // transcripts nobody can tell apart afterwards.
            fprintf(stderr,
                    "[vkprobe] frame=%llu vs=%016llx prim=%u indexCount=%u indexed=%d "
                    "tex=%08X %ux%u VGT_INDX_OFFSET=%d min=%u max=%u\n",
                    (unsigned long long)R->frame, (unsigned long long)vsBind.hash,
                    draw.primType, draw.indexCount, draw.indexed ? 1 : 0, R->lastTexAddr,
                    R->lastTexW, R->lastTexH, int32_t(regs[xenos::kVgtIndxOffset]),
                    regs[xenos::kVgtMinVtxIndx], regs[xenos::kVgtMaxVtxIndx]);
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
                    // Every COMPONENT of the attribute, and as a float when the format
                    // is one. The first version printed a single dword per vertex as
                    // hex, which for a float2 texture coordinate shows only `u` — so
                    // two draws sampling different atlases produced transcripts that
                    // agreed on every printed value and disagreed on the one that
                    // mattered. A texture coordinate is the pair or it is nothing.
                    const uint32_t comps = VertexFormatDwords(a.format);
                    const bool isF32 = a.format == 36 || a.format == 37 ||
                                       a.format == 57 || a.format == 38;
                    fprintf(stderr, "[vkprobe]   loc%-3d fmt=%2u int=%u off=%u:",
                            a.location, a.format, a.isInteger, a.offsetDwords);
                    // CZ_VK_DRAW_PROBE_VERTS=N — how many vertices to print (default 4).
                    // Four is one quad, i.e. ONE glyph of a text run, and one glyph
                    // cannot show whether a run's cells advance sensibly across the
                    // sheet. That is the actual question for a font atlas.
                    static const uint32_t probeVerts =
                        Env("CZ_VK_DRAW_PROBE_VERTS")
                            ? uint32_t(atoi(Env("CZ_VK_DRAW_PROBE_VERTS")))
                            : 4;
                    for (uint32_t v = 0; v < probeVerts; v++)
                    {
                        const uint64_t dw =
                            uint64_t(v) * a.strideDwords + a.offsetDwords;
                        if (dw + comps > vf.sizeDwords)
                            break;
                        uint32_t raw[4] = {};
                        CopySwapped(reinterpret_cast<uint8_t*>(raw), base + sva + dw * 4,
                                    comps * 4, vf.endian);
                        fprintf(stderr, "  v%u(", v);
                        for (uint32_t k = 0; k < comps; k++)
                        {
                            if (isF32)
                                fprintf(stderr, "%s%.5f", k ? "," : "", F32(raw[k]));
                            else
                                fprintf(stderr, "%s%08X", k ? "," : "", raw[k]);
                        }
                        fprintf(stderr, ")");
                    }
                    fprintf(stderr, "\n");
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
        NoteIndexBind(R->arena.buffer, at, VK_INDEX_TYPE_UINT32);
        vkCmdBindIndexBuffer(R->cmd, R->arena.buffer, at, VK_INDEX_TYPE_UINT32);
        // rectSynth already folded the base vertex into its three corners, and its
        // expanded indices name a private four-vertex stream — so offsetting again
        // would apply it twice.
        vkCmdDrawIndexed(R->cmd, expandedCount, 1, 0, rectSynth ? 0 : indxOffset, 0);
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
            static uint64_t* slots[4];
            static bool built = false;
            if (!built)
            {
                built = true;
                char name[32];
                for (uint32_t i = 0; i < 4; i++)
                {
                    snprintf(name, sizeof name, "index endian code %u", i);
                    slots[i] = CounterSlot(name);
                }
            }
            ++*slots[draw.indexEndian & 3];
        }
        const StreamLoc loc = UploadStream(base, draw.indexVa, bytes, endian, 1);
        if (!loc.ok())
            return;
        // The CZ_VK_RANGE_CENSUS read-out. The index copy is little-endian by here, so
        // the walk is a plain array scan; 0xFFFF/0xFFFFFFFF is primitive restart and is
        // not a vertex.
        if (rangeCensus && rangeAttrCount)
        {
            const uint8_t* ip = loc.bytes();
            uint32_t maxIdx = 0;
            for (uint32_t i = 0; i < draw.indexCount; i++)
            {
                const uint32_t v = draw.index32
                    ? reinterpret_cast<const uint32_t*>(ip)[i]
                    : reinterpret_cast<const uint16_t*>(ip)[i];
                if (v != (draw.index32 ? 0xFFFFFFFFu : 0xFFFFu) && v > maxIdx)
                    maxIdx = v;
            }
            RangeCensusEval(maxIdx);
        }
        const VkIndexType itype =
            draw.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        NoteIndexBind(loc.handle(), loc.at, itype);
        vkCmdBindIndexBuffer(R->cmd, loc.handle(), loc.at, itype);
        vkCmdDrawIndexed(R->cmd, draw.indexCount, 1, 0, indxOffset, 0);
        COUNT("draw: indexed");
    }
    else
    {
        // Auto-index reaches vertices [indxOffset, indxOffset + count); the census
        // question is identical, with maxIdx implicit.
        if (rangeCensus && rangeAttrCount && draw.indexCount)
            RangeCensusEval(draw.indexCount - 1);
        vkCmdDraw(R->cmd, draw.indexCount, 1, uint32_t(indxOffset), 0);
        COUNT("draw: auto-index");
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
    if (indxOffset)
        COUNT("draw: VGT_INDX_OFFSET applied (nonzero base vertex)");
    R->verticesThisFrame += draw.indexCount;
    if (R->drawsThisFrame == 0)
    {
        // The camera, from the frame's FIRST draw: vc(0..15) covers the
        // view-projection and the world matrices every scene shader reads.
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < 16 * 4; i++)
        {
            h = mix(h, regs[xenos::kAluConstantBase + i]);
            R->camConsts[i] = regs[xenos::kAluConstantBase + i];
        }
        R->cameraFingerprint = h;
    }
    // ...AND AGAIN FROM THE FRAME'S BIGGEST DRAW, which is a different matrix.
    //
    // The frame's FIRST draw is the SHADOW pass, so the view matrix above is the
    // LIGHT's, not the player's: solving it put one capture's eye 36 units below
    // ground, and a memory scan for world coordinates near that point found nothing
    // where the same scan around a scene camera finds the crowd. The fingerprint does
    // not care (it only has to differ when the view differs) but the POSE does, and a
    // pose that records the light is a shot nobody can retake.
    //
    // The biggest draw of a frame is the ground or a large scene mesh — never a shadow
    // cascade's geometry pass in practice, and never the UI, which is small quads. It
    // is a heuristic, so both matrices are written to the .pose and the reader decides:
    // an eye below the world is the light's, whatever produced it.
    if (draw.indexCount > R->camBigVerts)
    {
        R->camBigVerts = draw.indexCount;
        for (uint32_t i = 0; i < 16 * 4; i++)
            R->camConstsBig[i] = regs[xenos::kAluConstantBase + i];
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

    // RB_COPY_CONTROL bits 0..2 — copy_src_select. 0..3 name a colour target, 4 names
    // the DEPTH buffer. This renderer read that field nowhere until phase C part 14 and
    // snapshotted the colour target for every resolve, which is wrong for close to a
    // fifth of them. `CZ_VK_NO_DEPTH_RESOLVE=1` is the same-binary control arm for
    // every claim about the change.
    static const bool noDepthResolve = EnvOn("CZ_VK_NO_DEPTH_RESOLVE");
    const uint32_t srcSelect = control & 7;
    const bool fromDepth = srcSelect == 4 && !noDepthResolve;
    if (srcSelect == 4)
        Count("resolve: source is the DEPTH buffer");
    else if (srcSelect != 0)
        Count("resolve: source is a colour target other than 0");

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
    //
    // CZ_VK_RESOLVE_TRACE_PASSES=N is the budget, in PASSES. It has been in CLAUDE.md
    // since part 12 with a stated default and was read NOWHERE in the tree until part
    // 14 (gotcha 193, the second documented-but-absent knob in three sessions) — and
    // the hardcoded budget it replaces guarded only the HEADER line, so the two
    // follow-up lines printed uncapped for the rest of the run. That is the identical
    // defect part 9's own note says it fixed by "putting the budget in passes"; the
    // note was written and the code was not. Both halves are the point: the budget has
    // to be in passes, and it has to cover every line a pass prints, or the trace runs
    // out of headers while still printing thousands of orphan input lines.
    static const int tracePasses =
        Env("CZ_VK_RESOLVE_TRACE_PASSES")
            ? int(strtol(Env("CZ_VK_RESOLVE_TRACE_PASSES"), nullptr, 10))
            : 20;
    static int passesLeft = tracePasses;
    // Kept as a value rather than tested inline, because the DERIVED copy geometry is
    // printed further down — after the fold has had its say — and the two lines have to
    // be about the same pass. The registers alone were never enough: part 32 spent an
    // hour deciding whether a 1024x1024 cascade was being clipped, and every input the
    // trace printed said it was not. What is missing from a register dump is the
    // DECISION the renderer made from them.
    const bool traceThisPass =
        EnvOn("CZ_VK_RESOLVE_TRACE") && passesLeft > 0 &&
        R->frame >= uint64_t(strtoul(Env("CZ_VK_RESOLVE_TRACE"), nullptr, 10));
    if (traceThisPass)
    {
        --passesLeft;
        {
            fprintf(stderr,
                    "[vkresolve] frame=%llu dest=%08X src=%s destPitch=%u destHeight=%u "
                    "surfacePitch=%u scissor=%u,%u..%u,%u win=%u,%u..%u,%u "
                    "winoff=%08X ctl=%08X info=%08X "
                    "front=%08X rtFmt=%u draws=%llu verts=%llu\n",
                    (unsigned long long)R->frame, dest,
                    srcSelect == 4 ? "DEPTH" : "colour",
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
        }
        // The pass's INPUTS, which is what says whether a compose is reading the scene.
        fprintf(stderr, "[vkresolve]     sampled snapshots:");
        if (R->snapshotsSampledThisPass.empty())
            fprintf(stderr, " (none)");
        for (uint32_t a : R->snapshotsSampledThisPass)
            fprintf(stderr, " %08X%s", a & 0x1FFFFFFF,
                    (a & kSnapshotDepthBit) ? "(depth)" : "");
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
    // Not const: the address-offset fold below may retarget it to the surface this
    // copy is a sub-region OF, once the copy extent is known.
    uint32_t baseKey = (dest - macroTileOffset(wx, wy, surfW)) & 0x1FFFFFFF;

    // THE SNAPSHOT IS THE SIZE OF THE DESTINATION SURFACE, not of our EDRAM.
    //
    // It used to be `min(surface, EDRAM)`, which is wrong whenever the two differ and
    // silently so: a consumer samples the surface with NORMALIZED coordinates computed
    // for the extent the FETCH CONSTANT declares, so an image of any other size hands it
    // different texels. This title's shadow cascade is the case that matters — the guest
    // declares it 4096x1024 and fetches it 629,023 times a boot, more than any other
    // texture in the frame, and it was being stored 1280x720. Nothing about the shadow
    // lookup could work, which is exactly what the picture showed: no shadows anywhere,
    // and the power-line shadow across the Still Creek forecourt missing.
    //
    // The COPY is still bounded by what the EDRAM actually holds — we cannot copy pixels
    // we never rendered — and the shortfall is counted rather than silently absorbed,
    // because "the snapshot is the right size" and "the snapshot is FULL" are different
    // claims and only the second one makes a shadow map usable.
    static const bool smallEdram = EnvOn("CZ_VK_SMALL_EDRAM");
    const uint32_t w = smallEdram ? std::min(surfW, R->color.width)
                                  : std::min(surfW, kMaxSurfaceExtent);
    const uint32_t h = smallEdram ? std::min(surfH, R->color.height)
                                  : std::min(surfH, kMaxSurfaceExtent);
    if (!smallEdram && (surfW > kMaxSurfaceExtent || surfH > kMaxSurfaceExtent))
        Count("resolve: destination surface larger than the snapshot cap");
    const uint32_t copyX = std::min(wx, w);
    const uint32_t copyY = std::min(wy, h);
    uint32_t copyW = wx1 > wx ? std::min(wx1, w) - copyX : w - copyX;
    uint32_t copyH = wy1 > wy ? std::min(wy1, h) - copyY : h - copyY;
    // Bound by the EDRAM we can read from, and say so when that bites.
    const uint32_t availW = copyX < R->color.width ? R->color.width - copyX : 0;
    const uint32_t availH = copyY < R->color.height ? R->color.height - copyY : 0;
    if (copyW > availW || copyH > availH)
        Count("resolve: copy region clipped by the EDRAM stand-in's size");
    copyW = std::min(copyW, availW);
    copyH = std::min(copyH, availH);

    // WHERE THE COPY LANDS IN THE SNAPSHOT, which is not always where it was read from.
    //
    // The scissor says where in the EDRAM the pass rendered; it is the SOURCE offset and
    // always right. The DESTINATION offset is where in the destination surface those
    // pixels belong, and this title has TWO ways of saying that — move the scissor, or
    // pre-offset RB_COPY_DEST_BASE. The `baseKey` subtraction above understands the
    // first. This understands the second, and until part 31 nothing did.
    //
    // The case that needs it is the SHADOW ATLAS (§6bc). Four cascades share one
    // 4096x1024 surface; each resolves a 1024x1024 region with the scissor at the
    // origin, and they are told apart only by a destination address 0x20000 apart. In
    // Xenos tiled address space that is exactly +1024 texels in X: a 32bpp macro tile is
    // 32x32 texels = 4096 bytes, a 4096-wide surface is 128 tiles per tile row, so
    // +32 tiles = 32 * 4096 = 0x20000. Without this the four become four disjoint
    // snapshots each holding its own quarter, the consumer fetches the base address, and
    // three quarters of every shadow lookup reads zero. Measured: our atlas was 86.7%
    // zero where hardware's, dumped from the same capture, is 3.5%.
    uint32_t dstX = copyX;
    uint32_t dstY = copyY;
    static const bool noAddrFold = EnvOn("CZ_VK_NO_ADDR_TILE_FOLD");
    // Only when the copy does not cover the surface's width can a horizontal offset
    // exist at all, and only when the scissor is at the origin is the address the thing
    // carrying it — otherwise `baseKey` has already accounted for it and folding again
    // would double-count.
    if (!noAddrFold && copyW < surfW && wx == 0 && wy == 0 && surfW >= 32 && copyW)
    {
        const uint32_t tilesPerRow = surfW >> 5;
        bool found = false;
        uint32_t bestBase = 0, bestX = 0, bestY = 0;
        for (const auto& [k, s] : R->snapshots)
        {
            // Same kind of surface, same extent, at a LOWER address: those three
            // together are what make "this is a sub-region of that" a decode rather
            // than a guess. Two unrelated surfaces of identical shape 0x20000 apart
            // would still fold, so the arm above exists and the fold is counted.
            if (((k & kSnapshotDepthBit) != 0) != fromDepth)
                continue;
            const uint32_t b = k & 0x1FFFFFFF;
            if (b >= baseKey || s.image.width != w || s.image.height != h)
                continue;
            const uint32_t delta = baseKey - b;
            if (delta & 0xFFF)          // not a whole number of macro tiles
                continue;
            // A sub-region has to be inside the allocation. Without this the decode
            // relies on the `ty + copyH > surfH` test below to reject far-apart
            // surfaces of the same shape, which it does — but by arithmetic accident
            // rather than by saying what it means. `0684B000` and `1439B000` are both
            // 1280x720 in this title and 0xD150000 apart, and the frame's first tile
            // (scissor at the origin, 640 of 1280 wide) asks this question of them
            // every frame.
            if (uint64_t(delta) >= uint64_t(surfW) * surfH * 4)
                continue;
            const uint32_t tile = delta >> 12;
            const uint32_t tx = (tile % tilesPerRow) << 5;
            const uint32_t ty = (tile / tilesPerRow) << 5;
            if (tx + copyW > surfW || ty + copyH > surfH)
                continue;
            // Nearest base below, so a surface that is itself a sub-region of a bigger
            // one folds into its immediate parent rather than the earliest match.
            if (!found || b > bestBase)
            {
                found = true;
                bestBase = b;
                bestX = tx;
                bestY = ty;
            }
        }
        if (found && (bestX || bestY))
        {
            baseKey = bestBase;
            dstX = bestX;
            dstY = bestY;
            Count("resolve: destination address folded into an existing surface as a "
                  "tile offset");
        }
    }

    // THE DECISION, printed next to the registers it came from. `avail` is the EDRAM
    // stand-in's extent, and it is the term that silently truncates a copy: a pass may
    // legitimately ask for more rows than our EDRAM has, and the only symptom is a
    // snapshot that is the right SIZE and partly empty — which reads downstream as a
    // shadow map full of zeros, i.e. as fully occluded rather than as missing.
    if (traceThisPass)
        fprintf(stderr,
                "[vkresolve]     -> snapshot %08X%s %ux%u  copy %ux%u from EDRAM(%u,%u) "
                "to dst(%u,%u)  avail %ux%u  clr c=%d d=%d depthClear=%08X\n",
                baseKey, fromDepth ? "(depth)" : "", w, h, copyW, copyH, copyX, copyY,
                dstX, dstY, availW, availH, int(clearColor), int(clearDepth),
                regs[xenos::kRbDepthClear]);

    if (w && h && copyW && copyH)
    {
        const uint32_t key = baseKey | (fromDepth ? kSnapshotDepthBit : 0u);
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
            // The views go with it: they are copies of an image that no longer exists,
            // and a surface whose extent changed is a different surface. Their bindless
            // slots are NOT recycled, for the same reason the snapshot's is not — slot
            // recycling is open-items 3b and needs deferred destruction to be safe.
            for (auto& [size, view] : it->second.views)
            {
                (void)size;
                vkDestroyImageView(R->device, view.image.view, nullptr);
                vkDestroyImage(R->device, view.image.image, nullptr);
                vkFreeMemory(R->device, view.image.memory, nullptr);
            }
            R->snapshots.erase(it);
            it = R->snapshots.end();
            Count("resolve: snapshot resized");
        }
        if (it == R->snapshots.end() && R->nextTextureSlot < g_maxDescriptors)
        {
            Snapshot s;
            s.slot = R->nextTextureSlot++;
            s.fromDepth = fromDepth;
            // A depth snapshot keeps the EDRAM depth buffer's own format, because
            // vkCmdCopyImage is only defined between identical depth formats — there
            // is no copy from a depth image into a colour one. It is viewed through
            // the DEPTH aspect with every component reading that value, so a shader
            // sampling it gets the 24-bit depth in .r (which is what a Xenos `tfetch`
            // of a `k_24_8` surface returns) and a defined value in .gba rather than
            // Vulkan's undefined non-red components of a depth view.
            const VkComponentMapping depthSwizzle{
                VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE
            };
            if (CreateImage(s.image, w, h,
                            fromDepth ? R->depth.format : VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            fromDepth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                      : VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_VIEW_TYPE_2D, 1, 1,
                            fromDepth ? depthSwizzle : VkComponentMapping{}))
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
                // TRANSITION IT BEFORE ANYTHING CAN SEE THE DESCRIPTOR, in its own
                // immediate submit. The copy a few lines below already leaves it in
                // SHADER_READ_ONLY, so this looks redundant — and it is not, because the
                // copy is recorded into the FRAME's command buffer while the descriptor
                // becomes visible to that whole command buffer the instant it is written.
                // Every draw recorded EARLIER in the same frame is bound to the same
                // bindless heap, and a descriptor claiming SHADER_READ_ONLY on an image
                // that is still UNDEFINED is undefined CONTENT for anything that indexes
                // it. Nothing does — a draw can only learn this slot number from a lookup
                // that would have missed — but "nothing indexes it" is an argument and
                // this is a guarantee.
                //
                // It is also all 14 of `vkCmdDraw-None-09600`, the validation defect part
                // 25's hand-off said to chase first (open item 00d). The layer named them
                // once images carried names: six resolve snapshots in a halving chain
                // (96x45, 64x22, 32x11, 32x5, 32x2, 32x1 — a bloom pyramid) plus their
                // second and third occurrences, every one created mid-frame.
                RunImmediate([&](VkCommandBuffer cb) {
                    Barrier(cb, s.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            fromDepth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                      : VK_IMAGE_ASPECT_COLOR_BIT);
                });
                vkUpdateDescriptorSets(R->device, 1, &wr, 0, nullptr);
                NameImage(s.image, "resolve snapshot %08X %ux%u%s slot %u", baseKey, w, h,
                          fromDepth ? " DEPTH" : "", s.slot);
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
            // The source EDRAM buffer, and the aspect that goes with it. A depth
            // resolve copies out of R->depth: the whole point of reading
            // copy_src_select is that these two are different pictures.
            Image& src = fromDepth ? R->depth : R->color;
            const VkImageAspectFlags aspect =
                fromDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            BeginFrame();
            EndRendering();
            // The EDRAM depth image is tracked with both aspects everywhere else, so
            // its barriers carry both here too; the snapshot has only depth.
            Barrier(R->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    fromDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                              : VK_IMAGE_ASPECT_COLOR_BIT);
            Barrier(R->cmd, it->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    aspect);
            // Copy the TILE, at its own position in each image. For a scissor-offset
            // tile the two offsets are the same, because our EDRAM is full-screen-sized
            // and the window offset is deliberately not applied to the geometry (see the
            // scissor note in DoDraw), so a tile sits at its true screen position in
            // both. For an ADDRESS-offset sub-region they differ: the pass rendered at
            // the EDRAM origin and the pixels belong somewhere else in the destination
            // surface, which is what `dstX`/`dstY` carry.
            VkImageCopy copy{};
            copy.srcSubresource = { aspect, 0, 0, 1 };
            copy.srcOffset = { int32_t(copyX), int32_t(copyY), 0 };
            copy.dstSubresource = { aspect, 0, 0, 1 };
            copy.dstOffset = { int32_t(dstX), int32_t(dstY), 0 };
            copy.extent = { copyW, copyH, 1 };
            vkCmdCopyImage(R->cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           it->second.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
            // Back to SHADER_READ_ONLY immediately: a later pass in this same frame
            // samples this surface, and the layout it expects is the one the
            // descriptor was written with.
            Barrier(R->cmd, it->second.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    aspect);
            // The right-sized views of this surface, refreshed in this same command
            // buffer so they cost no submit and cannot be staler than their source.
            for (auto& [size, view] : it->second.views)
            {
                (void)size;
                RefreshSnapshotView(R->cmd, it->second.image, view, aspect);
                Count("resolve: snapshot view refreshed");
            }
            it->second.frameSeen = R->frame;
            Count(fromDepth ? "resolve: snapshot taken from the DEPTH buffer"
                            : "resolve: snapshot taken from the colour buffer");

            // A FACE OF A RENDERED CUBE MAP, copied into its layer in this same command
            // buffer. Same reasoning as the sized views above: it costs no submit and it
            // cannot be staler than its source. A cube map the title renders is redrawn as
            // the world changes, so a cube assembled once at its first fetch and never
            // refreshed would freeze whatever the environment looked like at that instant
            // — the exact defect the LUT had in §6s, one descriptor set over.
            if (!fromDepth)
            {
                auto owner = R->cubeFaceOwner.find(baseKey);
                if (owner != R->cubeFaceOwner.end())
                {
                    auto cube = R->cubeSnapshots.find(owner->second.first);
                    if (cube != R->cubeSnapshots.end())
                    {
                        CopyFaceIntoCube(R->cmd, it->second.image, cube->second,
                                         owner->second.second);
                        cube->second.frameSeen = R->frame;
                        Count("resolve: refreshed a face of a rendered CUBE MAP");
                    }
                }
            }

            if (!fromDepth && R->frontBuffer && key == (R->frontBuffer & 0x1FFFFFFF))
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
        // CZ_VK_CLEAR_POISON=1 — clear the colour target to MAGENTA instead of the guest's
        // value. The positive control for "these pixels were never written by any draw".
        //
        // Part 26's operator report is large ground patches of a single flat colour, and
        // that colour is EXACTLY (180,180,180) over 39.6% of one frame — 49,195 pixels of
        // one value with a standard deviation of 3. A constant that precise is not a
        // texture and not lighting; it is either a clear or a shader writing a constant.
        // The two look identical in a screenshot and have nothing in common as bugs, so
        // this separates them: under the arm, every pixel still showing the clear is
        // magenta and every pixel some draw actually wrote keeps its colour.
        //
        // The clear VALUE is printed once per distinct value too, because if the guest's
        // own clear is 0xB4B4B4 the question is answered without running the arm at all.
        static const bool clearPoison = EnvOn("CZ_VK_CLEAR_POISON");
        {
            static std::vector<uint32_t> seenClear;
            if (std::find(seenClear.begin(), seenClear.end(), c) == seenClear.end() &&
                seenClear.size() < 16)
            {
                seenClear.push_back(c);
                fprintf(stderr, "[vk] RB_COLOR_CLEAR = %08X  (a=%u r=%u g=%u b=%u)\n", c,
                        (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            }
        }
        VkClearColorValue value{};
        value.float32[0] = float((c >> 16) & 0xFF) / 255.0f;
        value.float32[1] = float((c >> 8) & 0xFF) / 255.0f;
        value.float32[2] = float(c & 0xFF) / 255.0f;
        value.float32[3] = float((c >> 24) & 0xFF) / 255.0f;
        if (clearPoison)
        {
            value.float32[0] = 1.0f;
            value.float32[1] = 0.0f;
            value.float32[2] = 1.0f;
            value.float32[3] = 1.0f;
        }
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
        // CZ_VK_DEPTH_CLEAR_FAR=1 — clear depth to 1.0 whatever RB_DEPTH_CLEAR says.
        // A DIAGNOSTIC ARM: it asks whether the cascade's empty half is empty because
        // the buffer it is tested against was cleared to 0 (this title leaves
        // RB_DEPTH_CLEAR at 00000000 for nearly every pass, and our clear covers the
        // whole EDRAM stand-in). If the atlas fills under this arm, the clear VALUE is
        // the whole story; if it does not, the zeros come from somewhere else.
        static const bool clearFar = EnvOn("CZ_VK_DEPTH_CLEAR_FAR");
        if (clearFar)
            value.depth = 1.0f;
        // The VALUE, once per distinct one, for the same reason the colour clear prints
        // its own: a depth buffer that starts at the wrong end is not a wrong picture,
        // it is a pass whose every fragment fails the test — and the symptom of that is
        // an EMPTY surface, which reads as "the geometry was never submitted".
        {
            static std::vector<uint32_t> seenDepthClear;
            const uint32_t dc = regs[xenos::kRbDepthClear];
            if (std::find(seenDepthClear.begin(), seenDepthClear.end(), dc) ==
                    seenDepthClear.end() &&
                seenDepthClear.size() < 16)
            {
                seenDepthClear.push_back(dc);
                fprintf(stderr, "[vk] RB_DEPTH_CLEAR = %08X  (depth %.6f, stencil %u)\n",
                        dc, value.depth, value.stencil);
            }
        }
        Barrier(R->cmd, R->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1
        };
        // CZ_VK_SCOPED_CLEAR=1 — clear only the region THIS pass rendered, not the whole
        // EDRAM stand-in.
        //
        // On Xenos a copy block's clear bits clear the tiles of the CURRENT SURFACE. Our
        // EDRAM is one 1280x1024 image shared by every pass, so clearing all of it makes
        // a 64x64 post-chain pass wipe the 1024x1024 shadow cascade — and it wipes it to
        // RB_DEPTH_CLEAR, which this title leaves at 00000000 for almost every pass. A
        // depth buffer at 0 with a LESS test rejects every fragment, so the cascade comes
        // out empty, and a shadow lookup that reads 0 reads as OCCLUDED. Part 32 measured
        // exactly that: 46.875% of every cascade band is zero, and forcing the compare to
        // ALWAYS (CZ_VK_DEPTH_ALWAYS) fills it, so the geometry was always being
        // submitted and always being rejected.
        //
        // An ARM until it is measured, because it cuts both ways: a pass that legitimately
        // expects the whole surface cleared now gets only its scissor's worth.
        static const bool scopedClear = EnvOn("CZ_VK_SCOPED_CLEAR");
        if (scopedClear && copyW && copyH)
        {
            VkClearRect rect{};
            rect.rect.offset = { int32_t(copyX), int32_t(copyY) };
            rect.rect.extent = { copyW, copyH };
            rect.baseArrayLayer = 0;
            rect.layerCount = 1;
            // vkCmdClearAttachments needs a render pass; outside one the region form is
            // a clear of the image with a scissor, which Vulkan has no direct call for —
            // so this does it inside a rendering scope over just that rectangle.
            VkRenderingAttachmentInfo depthAtt{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO
            };
            depthAtt.imageView = R->depth.view;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = value;
            Barrier(R->cmd, R->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { int32_t(copyX), int32_t(copyY) }, { copyW, copyH } };
            ri.layerCount = 1;
            ri.pDepthAttachment = &depthAtt;
            ri.pStencilAttachment = &depthAtt;
            vkCmdBeginRendering(R->cmd, &ri);
            vkCmdEndRendering(R->cmd);
            Count("resolve: depth cleared (scoped to the pass)");
        }
        else
        {
            vkCmdClearDepthStencilImage(R->cmd, R->depth.image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                        &range);
            Count("resolve: depth cleared");
        }
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
    // The EDRAM stand-in is TALLER than the presented frame, and that is the point.
    // `targetWidth/targetHeight` is what VdSwap says the FRONT BUFFER is; the EDRAM has
    // to hold the largest surface any PASS renders into, and this title's shadow pass
    // renders a 1024x1024 cascade. At 720 rows every cascade lost its bottom 304 rows
    // before anything could sample them. `CZ_VK_SMALL_EDRAM=1` restores the old size and
    // is the same-binary control arm.
    static const bool smallEdram = EnvOn("CZ_VK_SMALL_EDRAM");
    const uint32_t edramH =
        smallEdram ? R->targetHeight : std::max(R->targetHeight, kEdramHeight);
    // Kept on the Renderer because the WINDOW-COORDINATE draw path needs it: a window
    // coordinate is relative to the EDRAM surface, not to the presented frame, so the
    // clip volume it maps into has to be the EDRAM's (see the vte==0 branch).
    R->edramWidth = R->targetWidth;
    R->edramHeight = edramH;
    if (!CreateImage(R->color, R->targetWidth, edramH,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT) ||
        // TRANSFER_SRC because 18.4% of this title's resolves copy out of the DEPTH
        // buffer rather than the colour one (its shadow cascades and the scene depth
        // its depth-of-field pass reads back) — see DoResolve.
        !CreateImage(R->depth, R->targetWidth, edramH,
                     VK_FORMAT_D24_UNORM_S8_UINT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
    {
        fprintf(stderr, "[vk] render target creation FAILED\n");
        return false;
    }
    NameImage(R->color, "EDRAM colour %ux%u", R->targetWidth, edramH);
    NameImage(R->depth, "EDRAM depth %ux%u", R->targetWidth, edramH);

    // A depth snapshot is sampled through the same bindless heap and the same single
    // linear sampler as every other texture, so the device has to be able to filter
    // that format. Checked rather than assumed: if it cannot, the picture would be
    // undefined rather than wrong, and silently — say so once, loudly.
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(R->physical, R->depth.format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            fprintf(stderr, "[vk] WARNING: %u is not sampleable on this device — "
                            "depth resolves will not be readable by the guest\n",
                    unsigned(R->depth.format));
        else if (!(fp.optimalTilingFeatures &
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            fprintf(stderr, "[vk] NOTE: depth format %u has no linear filtering; "
                            "depth snapshots are sampled with the linear sampler\n",
                    unsigned(R->depth.format));
    }

    // 128 MB of per-frame arena. The frontend's streams are small; gameplay is the
    // question, and the high-water mark is printed with the stats so the number can be
    // raised on evidence rather than guessed at again.
    //
    // It GROWS now (see BeginFrame), so this is the STARTING size rather than the limit.
    // 128 is kept as the start deliberately: it is the size every measurement in this
    // port up to part 18 was taken at, so `CZ_VK_NO_ARENA_GROWTH=1` reproduces the old
    // renderer exactly and remains a usable control arm. CZ_VK_ARENA_MB=N sets the
    // start, which is how the 128-vs-512 A/B that identified the black frames was run.
    static const uint64_t arenaMb =
        Env("CZ_VK_ARENA_MB") ? strtoull(Env("CZ_VK_ARENA_MB"), nullptr, 10) : 128;
    // The CROSS-FRAME stream store, same usage and memory type as the arena because the
    // GPU cannot tell them apart — the only difference is that this one is not reset at
    // the swap. It grows on the same evidence-not-guesswork principle: 128 MB is where the
    // arena starts too, and `PersistMaintenance` doubles it when a frame overruns it.
    // CZ_VK_PERSIST_MB=N sets the start.
    static const uint64_t persistMb =
        Env("CZ_VK_PERSIST_MB") ? strtoull(Env("CZ_VK_PERSIST_MB"), nullptr, 10) : 128;
    R->persistOn = !EnvOn("CZ_VK_NO_PERSIST_STREAMS");

    // Announce itself, because an arm nobody can see in the log is an arm that cannot be
    // shown to have engaged (gotcha 151).
    // The bound, and its two overrides. Announced whenever it is not the default, so an
    // arm that did not engage cannot be mistaken for one that did (gotcha 151).
    if (const char* gb = Env("CZ_VK_STREAM_GUARD_BYTES"))
    {
        const unsigned long long v = strtoull(gb, nullptr, 10);
        // Below kGuardBlocks the block arithmetic degenerates; refuse rather than
        // silently sampling nothing.
        if (v >= kGuardBlocks)
        {
            g_guardBytes = size_t(v);
            fprintf(stderr, "[vk] CZ_VK_STREAM_GUARD_BYTES=%llu — the store's guard is "
                            "EXACT up to %llu bytes and samples above it (default %zu)\n",
                    v, v, kGuardBytesDefault);
        }
        else
        {
            fprintf(stderr, "[vk] CZ_VK_STREAM_GUARD_BYTES=%llu REFUSED — must be >= %zu; "
                            "keeping %zu\n", v, kGuardBlocks, g_guardBytes);
        }
    }
    else
    {
        fprintf(stderr, "[vk] stream guard exact to %zu bytes, sampled above "
                        "(CZ_VK_STREAM_GUARD_BYTES=N to change, item 00c)\n",
                g_guardBytes);
    }

    g_guardExact = EnvOn("CZ_VK_STREAM_GUARD_EXACT");
    if (g_guardExact)
        fprintf(stderr, "[vk] CZ_VK_STREAM_GUARD_EXACT=1 — the cross-frame store's guard "
                        "hashes EVERY byte, so it cannot miss a small edit in a large "
                        "stream. Diagnostic for open item 00c.\n");

    // --- CZ_VK_FRAMES_IN_FLIGHT ---------------------------------------------------------
    //
    // How many frames the CPU may be ahead of the GPU. 1 is the renderer this port ran
    // for twenty-two parts: submit the frame, block on its fence, read it back, present.
    //
    // Why more than 1 is the largest item in the performance plan: a crowd frame is
    // ~27.7 ms of CPU followed by ~16.5 ms of GPU, strictly in series, so the card is
    // idle 68% of every frame and the driver correctly governs it down to a mid clock
    // (gotcha 231, §6ar). `CZ_VK_NO_SUBMIT=1` measured the ceiling on removing that
    // serialisation — CPU-only time, ~1.45x — without building it. This is building it.
    //
    // TWO IS THE DEFAULT AND THREE IS AVAILABLE, and neither is a guess about which
    // wins: one frame of overlap already covers a GPU shorter than the CPU, so 3 should
    // read as noise, and if it does not then the model of where the time goes is wrong
    // and that is worth knowing. Both are one binary, which is what makes the A/B legal.
    static const char* fifEnv = Env("CZ_VK_FRAMES_IN_FLIGHT");
    R->framesInFlight = fifEnv ? uint32_t(strtoul(fifEnv, nullptr, 10)) : 2;
    if (R->framesInFlight < 1)
        R->framesInFlight = 1;
    if (R->framesInFlight > kMaxFramesInFlight)
        R->framesInFlight = kMaxFramesInFlight;

    // The instruments that CANNOT be one frame late, and are therefore allowed to veto
    // the pipelining rather than silently produce misaligned evidence.
    //
    // A deferred present hands the window frame N-1's pixels while the renderer's
    // snapshot images, its resolve chain and its register state are all frame N's. The
    // frame-stats and PPM paths are fixed properly below — they carry the presented
    // frame's own metadata in its slot — but three instruments read the LIVE resolve
    // chain next to the presented pixels and cannot be fixed that way:
    // CZ_VK_SNAP_ON_BLACK and CZ_VK_SNAP_ON_DARK trigger a dump of the current
    // snapshots from a pixel test on the presented frame, and CZ_VK_FRAME_STATS_SURFACE
    // reads back a named snapshot to sit in the same stats line as the presented one.
    // A one-frame skew there is a diagnostic that quietly answers about the wrong frame,
    // which is worse than a slower diagnostic (gotcha 7's cousin: an instrument that
    // reports about something other than what it names).
    if (Env("CZ_VK_SNAP_ON_BLACK") || Env("CZ_VK_SNAP_ON_DARK") ||
        Env("CZ_VK_FRAME_STATS_SURFACE") || Env("CZ_VK_SNAP_DUMP") ||
        EnvOn("CZ_VK_NO_SUBMIT"))
    {
        if (R->framesInFlight != 1)
            fprintf(stderr, "[vk] frames-in-flight forced to 1: a snapshot-chain or "
                            "no-submit instrument is on and those read the resolve state "
                            "of the frame being RECORDED, not the one being presented\n");
        R->framesInFlight = 1;
    }
    fprintf(stderr, "[vk] frames in flight: %u%s\n", R->framesInFlight,
            R->framesInFlight == 1 ? " (submit and wait; the pre-part-23 renderer)" : "");

    if (!CreateBuffer(R->arena, (arenaMb << 20) * R->framesInFlight,
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
                      // Big enough for the presented frame AND for the largest snapshot
                      // CZ_VK_SNAP_DUMP might read back — this title's shadow cascade
                      // is 4096x1024, and the dump SKIPS anything that does not fit,
                      // which would have made the one surface under investigation the
                      // one surface absent from the directory.
                      std::max(uint64_t(R->targetWidth) * R->targetHeight,
                               uint64_t(4096) * 1024) * 4,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, ReadbackMemoryProps(), false))
    {
        fprintf(stderr, "[vk] buffer allocation FAILED\n");
        return false;
    }

    // One present readback buffer per slot. It is deliberately NOT a region of
    // `R->readback`: that buffer is also the snapshot-dump target, and sharing it would
    // mean an instrument's readback could land on top of a frame the window has not
    // fetched yet — a corrupted picture that appears only when a diagnostic is on, which
    // is the worst kind. Sized like `R->readback` so a front-buffer resolve larger than
    // the frame extent still fits rather than being silently truncated.
    for (uint32_t i = 0; i < R->framesInFlight; ++i)
    {
        if (!CreateBuffer(R->frames[i].present,
                          std::max(uint64_t(R->targetWidth) * R->targetHeight,
                                   uint64_t(4096) * 1024) * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, ReadbackMemoryProps(), false))
        {
            fprintf(stderr, "[vk] present readback buffer %u allocation FAILED\n", i);
            return false;
        }
    }

    // The cross-frame store is allocated SEPARATELY from the chain above, and its failure
    // DEGRADES rather than kills the renderer. It is an optimisation: without it every
    // stream takes the per-frame path, which is what this port did for twenty-one parts
    // and which still renders the game correctly. Refusing to start at all because a
    // machine could not spare another 128 MB would trade a 30% frame-time saving for the
    // whole picture, which is not a trade anything here should make silently.
    if (R->persistOn &&
        !CreateBuffer(R->persist, persistMb << 20,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      /*deviceAddress=*/true))
    {
        fprintf(stderr, "[vk] the %llu MB cross-frame stream store could not be "
                        "allocated — running without it, which is slower and correct\n",
                (unsigned long long)persistMb);
        R->persistOn = false;
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
    // Sampler 0 is PLAIN TRILINEAR, deliberately and permanently. Part 41's first
    // attempt put 16x aniso here, reasoning "every fetch publishes index 0, so this
    // is where aniso goes" — and the very first capture showed dark speckle across
    // the whole frame, because index 0 also serves the SHADOW ATLAS lookups, which
    // hardware fetches with aniso=0 and point filters. The per-fetch sampler cache
    // (SamplerIndexForFetch) is where the fetch constant's own filter fields are
    // honoured; this sampler remains the fallback and the CZ_VK_NO_FETCH_SAMPLERS
    // arm's whole world.
    fprintf(stderr, "[vk] per-fetch samplers %s (aniso device limit %.0fx)\n",
            Env("CZ_VK_NO_FETCH_SAMPLERS") ? "OFF (CZ_VK_NO_FETCH_SAMPLERS)" : "ON",
            R->anisoLimit);
    if (vkCreateSampler(R->device, &si, nullptr, &R->linearSampler) != VK_SUCCESS)
        return false;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(R->device, &si, nullptr, &R->pointSampler) != VK_SUCCESS)
        return false;

    // The draw-ID fragment module, embedded rather than loaded (see drawid_ps.hlsl). It
    // is created unconditionally and costs a few hundred bytes: an instrument that has
    // to be enabled at BUILD time is one nobody has when they need it.
    {
        VkShaderModuleCreateInfo smi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = sizeof kDrawIdPixelShaderSpv;
        smi.pCode = kDrawIdPixelShaderSpv;
        if (vkCreateShaderModule(R->device, &smi, nullptr, &R->drawIdModule) != VK_SUCCESS)
        {
            // Not fatal: the renderer works without the instrument, and saying so is
            // better than refusing to start because a diagnostic failed to compile.
            R->drawIdModule = VK_NULL_HANDLE;
            fprintf(stderr, "[vk] the draw-ID shader module failed to create — "
                            "CZ_VK_DRAW_ID will not work this run\n");
        }
    }

    // The dummies. Slot 0 of every heap is a defined 1x1 white texel, so a shader that
    // samples a slot the runtime could not fill reads white rather than an unbound
    // descriptor — undefined behaviour even when the result is discarded.
    auto makeDummy = [&](Image& img, VkImageViewType type, uint32_t layers,
                         uint32_t depth, uint32_t setIndex) {
        if (!CreateImage(img, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, type, layers, depth))
            return false;
        NameImage(img, "dummy set %u (%u layers)", setIndex, layers);
        // CZ_VK_CUBE_POISON=1 — make the CUBE dummy opaque MAGENTA instead of white.
        //
        // THE POSITIVE CONTROL, and this instrument exists because the change it tests
        // came back invisible. Part 25's picture A/B — cube maps bound versus
        // CZ_VK_NO_CUBE — was PIXEL-IDENTICAL on all 44 frames where both arms had the
        // same camera and the same draw set, and a null like that has two readings that
        // no amount of looking can separate: the cube samples do not reach the output at
        // all, or they reach it and happen to be indistinguishable from white.
        //
        // Poisoning the DUMMY answers it, because the dummy is what the old renderer's
        // cube fetches read. If a poisoned run is still identical, the cube sample is
        // discarded somewhere downstream and the whole item is mis-scoped; if the frame
        // fills with magenta, the path is live and the null above is a statement about
        // the CONTENT of those cube maps. Same shape as CZ_VK_TEX_GUARD_POISON
        // (gotcha 30) — a comparison that has never reported a positive proves nothing
        // by reporting a negative.
        // CZ_VK_DUMMY_POISON=1 poisons ALL FOUR dummies, not just the cube one.
        //
        // CZ_VK_CUBE_POISON answers "does a cube fetch reach the picture", and part 26
        // used it to prove the cube dummy tints the crowd. It cannot answer the question
        // the ground patches pose, because the ground reads no cube. The 1x1 dummy in
        // EVERY heap is white, and a white texel times a diffuse term of 0.706 is exactly
        // the (180,180,180) those patches are — 49,195 pixels of one value with a standard
        // deviation of 3, against a guest clear colour of BLACK, so they are written by
        // something rather than left unwritten. The declared-fetch census cannot see this
        // case: a shader reading a descriptor index the runtime never wrote gets slot 0
        // silently, because the shared-constant block is memset to zero every draw.
        static const bool cubePoison = EnvOn("CZ_VK_CUBE_POISON");
        static const bool allPoison = EnvOn("CZ_VK_DUMMY_POISON");
        const bool poisoned =
            allPoison || (cubePoison && type == VK_IMAGE_VIEW_TYPE_CUBE);
        const uint32_t white = poisoned ? 0xFFFF00FFu : 0xFFFFFFFFu;
        if (poisoned)
            fprintf(stderr,
                    "[vk] %s: the set-%u dummy is MAGENTA (0xFFFF00FF), %u layer(s)\n",
                    allPoison ? "CZ_VK_DUMMY_POISON" : "CZ_VK_CUBE_POISON", setIndex,
                    layers);
        // ALL SIX FACES, not just the first. The copy below has always had
        // `layerCount = layers`, but only four bytes were ever written into the staging
        // buffer — so faces 1..5 of every 1x1 dummy were filled from whatever the staging
        // buffer last held. It was invisible while the only multi-layer image was a dummy
        // nobody could see, and it would have made a poisoned run report a nonsense
        // colour on five faces out of six.
        for (uint32_t f = 0; f < layers; f++)
            memcpy(R->staging.mapped + f * 4, &white, 4);
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
    g_dimCensus = EnvOn("CZ_VK_DIM_CENSUS");
    if (const char* n = Env("CZ_VK_DIM_DISAGREE"))
    {
        g_dimDisagree = true;
        g_dimDisagreeLeft = atoi(n);       // how many get printed as they happen
    }
    // GUARD + REVALIDATE ARE THE DEFAULT AS OF PART 38. The cache previously uploaded a
    // texture ONCE per (address, extent, format) and served it forever, which is wrong
    // the moment streaming recycles an address — and an operator session recycles them
    // constantly: the tanker wore a BRICK WALL, and "almost everything up close wears a
    // random texture" (the operator's words) the longer the session ran. Part 35's
    // "4 stale of 92M hits" that justified leaving the repair off was measured on a
    // 400 s headless run at one location — a fact about that route, not about play
    // (the gotcha-50 family). A full operator session on the repair: every prop
    // correct, no reported slowdown. CZ_VK_NO_TEX_REVALIDATE=1 is the same-binary
    // control arm that brings the random-texture defect back.
    {
        static const bool noRevalidate = EnvOn("CZ_VK_NO_TEX_REVALIDATE");
        g_texGuard = !noRevalidate || EnvOn("CZ_VK_TEX_GUARD");
        g_texRevalidate = !noRevalidate;
    }
    g_texGuardPoison = EnvOn("CZ_VK_TEX_GUARD_POISON");
    if (g_texGuardPoison)
        fprintf(stderr, "[vk] texture guard POISONED — the changed share must now read "
                        "100.0%%; anything else means the census cannot fire\n");
    if (g_texRevalidate)
        fprintf(stderr, "[vk] texture cache REVALIDATES on content: a cache hit whose "
                        "guest bytes changed is re-uploaded in place\n");
    if (EnvOn("CZ_VK_TEX_REFRESH_ALL"))
        fprintf(stderr, "[vk] EVERY texture is re-read on EVERY fetch "
                        "(CZ_VK_TEX_REFRESH_ALL) — a picture arm, ruinously slow, and "
                        "the cache cannot serve a stale image under it\n");
    g_profileOn = EnvOn("CZ_VK_PROFILE");
    // Reported through the profile window, so it needs the profile on to say anything.
    // Saying so out loud rather than silently counting into a report nobody prints.
    g_streamCensus =
        Env("CZ_VK_STREAM_CENSUS") ? atoi(Env("CZ_VK_STREAM_CENSUS")) : 0;
    if (g_streamCensus && !g_profileOn)
    {
        fprintf(stderr, "[vk] CZ_VK_STREAM_CENSUS needs CZ_VK_PROFILE — it reports "
                        "through that window; census OFF\n");
        g_streamCensus = 0;
    }
    g_streamPoison = EnvOn("CZ_VK_STREAM_CENSUS_POISON");
    if (g_streamPoison)
        fprintf(stderr, "[vk] stream census POISONED — the content check must now read "
                        "0.0%%; anything else means it cannot fail\n");
    g_active = true;
    fprintf(stderr, "[vk] renderer UP: %ux%u target, %zu shaders\n", R->targetWidth,
            R->targetHeight, R->shaders.size());
    if (g_profileOn)
        fprintf(stderr, "[vkprof] frame CPU profile ON\n");
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
    // The raw-EDRAM fallback presents the FRAME's extent, not the EDRAM image's. Those
    // stopped being the same number when the EDRAM grew to hold the 1024-row shadow
    // cascade, and reading back the whole image would hand the window a 1280x1024
    // buffer as if it were the 1280x720 frame.
    const uint32_t width0 = R->haveFrontSnapshot ? R->frontWidth : R->targetWidth;
    const uint32_t height0 = R->haveFrontSnapshot ? R->frontHeight : R->targetHeight;
    Count(R->haveFrontSnapshot ? "swap: presented the front-buffer resolve"
                               : "swap: presented raw EDRAM (no resolve matched)");

    // Into THIS SLOT's readback buffer, not a shared one: with a frame in flight the
    // window has not necessarily fetched the previous frame's pixels yet.
    FrameSlot& rec = R->frames[R->frameSlot];
    Barrier(R->cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width0, height0, 1 };
    vkCmdCopyImageToBuffer(R->cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rec.present.buffer, 1, &copy);

    // What this frame WAS, recorded next to the pixels it produced. Every present-side
    // instrument below reads this and not `R->frame`/`R->drawFingerprint`, which from
    // here on describe the frame being recorded rather than the one being shown.
    rec.frame = R->frame;
    rec.draws = R->drawsThisFrame;
    rec.vertices = R->verticesThisFrame;
    rec.drawFingerprint = R->drawFingerprint;
    rec.cameraFingerprint = R->cameraFingerprint;
    rec.width = width0;
    rec.height = height0;
    rec.bytes = size_t(width0) * height0 * 4;
    rec.presentable = true;

    SubmitFrame();
    const int presentSlot = RetireOldestFrame();

    // Advance the ring for the next frame. It happens HERE, after the wait above, so
    // that when `BeginFrame` picks up this slot its fence has been observed to signal —
    // which is what makes resetting its command buffer and reusing its arena region
    // legal without a second wait at the top of the frame.
    R->frameSlot = (R->frameSlot + 1) % R->framesInFlight;

    // Reset the recorded frame's own accumulators here rather than after the stats line:
    // they belong to the frame that has just been submitted, and the stats line below is
    // now about a different one.
    R->drawFingerprint = 0;
    R->cameraFingerprint = 0;
    R->verticesThisFrame = 0;

    // Grow the arena HERE, at the frame boundary, rather than inside the next frame's
    // first draw. Both of these idle every frame still in flight before they touch
    // anything, which before part 23 the caller's fence wait did for them. See
    // GrowArenaIfNeeded for why the old placement was a measurement defect: it charged a
    // device-wait and an allocation to `other`.
    GrowArenaIfNeeded();
    // Same site, and for the store the reason is stronger: its offsets are recorded into
    // command buffers, so reusing its memory before the GPU is done hands an in-flight
    // draw somebody else's vertices.
    PersistMaintenance();

    // Nothing to show yet — the first frame of a run with two slots, because the second
    // slot has never been submitted. One frame of a run.
    if (presentSlot < 0)
        return;
    FrameSlot& pres = R->frames[presentSlot];
    if (!pres.presentable)
        return;
    const uint32_t width1 = pres.width, height1 = pres.height;
    const size_t bytes = pres.bytes;
    {
        ProfScope _p(&g_prof.readback);
        if (R->presentPixels.size() < bytes)
            R->presentPixels.resize(bytes);
        memcpy(R->presentPixels.data(), pres.present.mapped, bytes);
        Host_PresentPixels(R->presentPixels.data(), width1, height1);
    }

    // CZ_VK_SNAP_ON_BLACK[=pct] — dump the whole resolve chain of the frame the picture
    // DIED on, triggered by the picture dying.
    //
    // The view-dependent whole-frame black is the port's top rendering defect and it has
    // never been captured, because CZ_VK_SNAP_DUMP fires on a frame NUMBER and this
    // event happens when a human turns a camera. Asking an operator to hit a frame index
    // is not a request anyone can fulfil, so every report of this defect has been the
    // black frame alone — which is consistent with every pass being wrong and with
    // exactly one being wrong (the reason CZ_VK_SNAP_DUMP exists at all).
    //
    // The trigger is a TRANSITION, not a threshold, and that is what makes it usable:
    // this runtime presents plenty of legitimately black frames during boot and loading,
    // so "coverage below x%" alone would fire on the first one and dump a chain nobody
    // wants. Requiring a LIT frame first means the dump lands on the frame where a
    // working picture stopped working, which is the only frame that can distinguish the
    // hypotheses.
    //
    // Both thresholds are settable, and that is what makes the instrument testable at
    // all. The first version folded arming and firing into one if/else, so a frame
    // could never do both — and its positive control (a 99% floor, which should fire on
    // essentially any frame) sat silent through a whole boot, because reaching the fire
    // branch still required coverage under the hard-coded 20% arming bar. An instrument
    // whose control cannot reach its own trigger has not been shown capable of firing
    // (gotcha 30). With `CZ_VK_SNAP_ON_BLACK=99 CZ_VK_SNAP_ON_BLACK_LIT=20` the second
    // lit frame of any run fires it.
    // And there is a HARD CAP on total episodes, because the arming logic re-arms on
    // every lit frame and therefore has no natural bound. Its own positive control
    // proved that the expensive way: a 99% floor fires on essentially every frame, which
    // dumped **9,833 PPMs** and refilled a tmpfs whose exhaustion kills this machine's
    // shell. The defect it exists to catch happens a handful of times in a session, so a
    // low cap costs nothing real and turns a mis-set threshold from a filled disk into a
    // few wasted files. CZ_VK_SNAP_ON_BLACK_MAX raises it.
    static const char* onBlackEnv = Env("CZ_VK_SNAP_ON_BLACK");
    static const double litPct =
        Env("CZ_VK_SNAP_ON_BLACK_LIT") ? atof(Env("CZ_VK_SNAP_ON_BLACK_LIT")) : 20.0;
    static int episodesLeft =
        Env("CZ_VK_SNAP_ON_BLACK_MAX") ? atoi(Env("CZ_VK_SNAP_ON_BLACK_MAX")) : 4;
    static bool sawLitFrame = false;
    static int onBlackBudget = 2;   // the transition frame, and the next one
    // CZ_VK_SNAP_ON_DARK=<meanLuma> — the same trigger on the metric the defect ACTUALLY
    // moves, and it dumps a BRIGHT reference chain to sit beside the dark one.
    //
    // The first headless run to sweep the camera in Still Creek found the defect
    // immediately and SNAP_ON_BLACK could not fire on it: sweeping the camera through
    // ~360 degrees swings the presented frame's mean luminance between ~27 and ~4.5
    // while its COVERAGE stays 30-75%. The frame goes very dark, not empty, so a
    // coverage floor of 0.5% never trips. Coverage was the right metric for the
    // operator's report — a whole-frame black — and it is the wrong one for the thing
    // that is actually measurable here; both are kept because the two thresholds
    // answer different questions and an instrument whose meaning silently changed
    // would invalidate every run taken with it.
    //
    // The PAIR is the point. One dark chain is consistent with "this pass is broken"
    // and with "the scene really is dark here"; a bright chain from the same location
    // seconds later, with the same surfaces at the same addresses, is the control that
    // separates them (gotcha 133 — one frame of an animated scene is one sample). So a
    // dark episode owes a bright reference, and the next frame that re-arms pays it.
    static const char* onDarkEnv = Env("CZ_VK_SNAP_ON_DARK");
    static const double darkLitLuma =
        Env("CZ_VK_SNAP_ON_DARK_LIT") ? atof(Env("CZ_VK_SNAP_ON_DARK_LIT")) : 20.0;
    static int darkEpisodesLeft =
        Env("CZ_VK_SNAP_ON_DARK_MAX") ? atoi(Env("CZ_VK_SNAP_ON_DARK_MAX")) : 3;
    static bool sawBrightFrame = false;
    static bool oweBrightReference = false;

    bool blackTransition = false;
    if (onBlackEnv || onDarkEnv)
    {
        // Sampled every 16th pixel: this runs on the present path of every frame, and
        // the quantity is a whole-frame fraction that a 1-in-16 sample estimates to far
        // better than the 0.5 percentage points anyone cares about here.
        uint64_t lit = 0, seen = 0, luma = 0;
        for (size_t i = 0; i + 4 <= bytes; i += 64)
        {
            const uint8_t* p = R->presentPixels.data() + i;
            if (p[0] || p[1] || p[2])
                lit++;
            luma += (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
            seen++;
        }
        const double covPct = seen ? 100.0 * double(lit) / double(seen) : 0.0;
        const double meanLuma = seen ? double(luma) / double(seen) : 0.0;

        if (onBlackEnv)
        {
            const double floorPct = atof(onBlackEnv) > 0.0 ? atof(onBlackEnv) : 0.5;
            // Fire first, then arm — two independent tests rather than an if/else, so a
            // control whose thresholds overlap can exercise the trigger.
            if (sawLitFrame && covPct < floorPct && onBlackBudget > 0 && episodesLeft > 0)
            {
                blackTransition = true;
                onBlackBudget--;
                episodesLeft--;
                sawLitFrame = false;
                fprintf(stderr,
                        "[vk] SNAP_ON_BLACK: frame %llu went black (%.3f%% lit, floor "
                        "%.2f%%) — dumping the resolve chain (%d episode dumps left)\n",
                        (unsigned long long)pres.frame, covPct, floorPct, episodesLeft);
            }
            if (covPct >= litPct)
            {
                sawLitFrame = true;
                onBlackBudget = 2;      // re-arm, so a second episode is caught too
            }
        }

        if (onDarkEnv)
        {
            const double floorLuma = atof(onDarkEnv) > 0.0 ? atof(onDarkEnv) : 8.0;
            // CZ_VK_SNAP_ON_DARK_AFTER_MS=N — ignore everything before N ms of wall
            // clock. Not a refinement: the boot, the title screen and every loading
            // screen fade, so an episode budget aimed at gameplay is spent before
            // gameplay starts. Wall time rather than a frame index because the recipes
            // that reach gameplay are written in seconds (CZ_FAKE_START_MS intervals)
            // and a frame index for the same moment moves with the frame rate.
            static const auto darkT0 = std::chrono::steady_clock::now();
            static const long long afterMs =
                Env("CZ_VK_SNAP_ON_DARK_AFTER_MS")
                    ? atoll(Env("CZ_VK_SNAP_ON_DARK_AFTER_MS")) : 0;
            const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - darkT0).count();
            const bool live = nowMs >= afterMs;
            if (live && sawBrightFrame && meanLuma < floorLuma && darkEpisodesLeft > 0)
            {
                blackTransition = true;
                darkEpisodesLeft--;
                sawBrightFrame = false;
                oweBrightReference = true;
                fprintf(stderr,
                        "[vk] SNAP_ON_DARK: frame %llu went DARK (mean luma %.2f, floor "
                        "%.2f, %.2f%% lit) — dumping the resolve chain (%d left)\n",
                        (unsigned long long)pres.frame, meanLuma, floorLuma, covPct,
                        darkEpisodesLeft);
            }
            if (meanLuma >= darkLitLuma)
            {
                if (live && oweBrightReference)
                {
                    oweBrightReference = false;
                    blackTransition = true;
                    fprintf(stderr,
                            "[vk] SNAP_ON_DARK: frame %llu is the BRIGHT REFERENCE for "
                            "the episode above (mean luma %.2f, %.2f%% lit)\n",
                            (unsigned long long)pres.frame, meanLuma, covPct);
                }
                sawBrightFrame = true;
            }
        }
    }

    // CZ_VK_FRAME_DUMP=<dir> writes every 64th frame as a PPM. This is the instrument
    // that makes the renderer checkable WITHOUT a window, which matters more than it
    // sounds: every other gate this project owns is a log diff, and "the picture is
    // right" is the one claim that needs an image. A headless run plus a directory of
    // frames is a self-servable version of the E-screenshot comparison.
    // CZ_VK_FRAME_DUMP_EVERY=N overrides the 64. A screen the synthetic-input arm walks
    // THROUGH rather than parks on can be shorter than 64 frames, and one dump of it is
    // one sample of a transition — the save-slot panel below appeared in exactly one
    // frame of a 180 s boot.
    // The CZ_CAPTURE_KEY picture, written from the same readback the periodic dump uses.
    // Separate from the loop below rather than folded into its interval test, because
    // this one has to fire on EXACTLY the armed frame — the interval test would either
    // miss it or, if the interval were forced to 1, write every frame of the run.
    if (R->capturePictureFrame && pres.frame == R->capturePictureFrame)
    {
        R->capturePictureFrame = 0;
        char path[512];
        snprintf(path, sizeof path, "%s/capture_%06llu.ppm", Env("CZ_CAPTURE_KEY"),
                 (unsigned long long)pres.frame);
        if (FILE* f = fopen(path, "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", width1, height1);
            for (size_t i = 0; i < bytes; i += 4)
                fwrite(&R->presentPixels[i], 1, 3, f);
            fclose(f);
            fprintf(stderr, "[vk] capture: wrote %s (%ux%u)%s\n", path, width1, height1,
                    R->drawIdRanOnFrame == pres.frame
                        ? " — NOT A PICTURE: CZ_VK_DRAW_ID painted this frame's draw "
                          "indices, so read the drawid_* snapshot instead"
                        : "");
        }
        else
        {
            // A capture that silently writes nothing is worse than no capture: the
            // operator walks away believing the evidence exists (gotchas 25, 151).
            fprintf(stderr, "[vk] capture: CANNOT WRITE %s — the picture is LOST\n", path);
        }

        // ...AND THE POSE, so the shot can be taken again.
        //
        // Every picture finding in this port has been anchored to "the operator walked
        // somewhere and pressed F9", which is not a reproducible experiment: nothing
        // headless can return to that spot, and the striped-material class picks a
        // different quality level on each boot, so the second visit is a different
        // measurement. What makes a shot repeatable is the CAMERA and the PLAYER, so
        // both are recorded beside the picture.
        //
        // Both are written RAW — the 16 float4 vertex constants the camera fingerprint
        // hashes (view-projection and world matrices) and the head of the player's game
        // object. Deriving an eye position or naming a position field here would be
        // guessing at a layout; two .pose files taken in different places name those
        // fields by what CHANGED, and that analysis belongs in a tool that can be fixed
        // without a rebuild (tools/pose_read.py).
        snprintf(path, sizeof path, "%s/capture_%06llu.pose", Env("CZ_CAPTURE_KEY"),
                 (unsigned long long)pres.frame);
        if (FILE* f = fopen(path, "w"))
        {
            fprintf(f, "# frame %llu  cameraFingerprint %016llx  drawFingerprint %016llx\n",
                    (unsigned long long)pres.frame,
                    (unsigned long long)R->cameraFingerprint,
                    (unsigned long long)R->drawFingerprint);
            fprintf(f, "# vc[i]  = constants at the frame's FIRST draw (usually the SHADOW "
                       "pass: its view matrix is the LIGHT's)\n");
            fprintf(f, "# bvc[i] = constants at the frame's BIGGEST draw (%u verts) — the "
                       "SCENE camera. Prefer these.\n", R->camBigVerts);
            for (int which = 0; which < 2; which++)
            {
                const uint32_t* src = which ? R->camConstsBig : R->camConsts;
                for (uint32_t i = 0; i < 64; i += 4)
                {
                    float v[4];
                    for (uint32_t k = 0; k < 4; k++)
                    {
                        const uint32_t bits = src[i + k];
                        memcpy(&v[k], &bits, 4);
                    }
                    fprintf(f, "%s%-2u %.6f %.6f %.6f %.6f\n", which ? "bvc" : "vc",
                            i / 4, v[0], v[1], v[2], v[3]);
                }
            }
            // The object dump lives in debug_tunables.cpp, which already owns guest
            // memory access and the pointer itself; GuestRangeOk here would be the
            // wrong check anyway, since it validates only the physical texture arena
            // and a game object is an ordinary virtual address.
            // THE POSITION ITSELF, which is the whole point of the pose: read via
            // the guest's own getplayerinfo path (obj->vtable[0x18]), not inferred
            // from the object dump below. The dump stays because it is what named
            // this field's neighbours, and because an unexplained struct is worth
            // keeping while the layout is still being learned.
            float pos[3];
            long long ageMs = -1;
            if (CZ_DebugPlayerPos(pos, &ageMs))
                fprintf(f, "player_pos %.4f %.4f %.4f   # read %lld ms before this "
                           "capture, via getplayerinfo's vtable[0x18]\n",
                        pos[0], pos[1], pos[2], ageMs);
            else
                fprintf(f, "# player_pos UNAVAILABLE — no level running, or the "
                           "lookup failed\n");
            const uint32_t obj = CZ_DebugWritePlayerObject(f, 2048);
            fclose(f);
            fprintf(stderr, "[vk] capture: wrote %s (camera + player object %08X)\n",
                    path, obj);
        }
        else
            fprintf(stderr, "[vk] capture: CANNOT WRITE %s — the pose is LOST\n", path);
    }
    static const char* dumpDir = Env("CZ_VK_FRAME_DUMP");
    static const uint64_t dumpEvery =
        Env("CZ_VK_FRAME_DUMP_EVERY")
            ? std::max<uint64_t>(1, strtoull(Env("CZ_VK_FRAME_DUMP_EVERY"), nullptr, 10))
            : 64;
    if (dumpDir && (pres.frame % dumpEvery) == 0)
    {
        // Create the directory, and SAY SO if the frames cannot be written. This used to
        // be a bare fopen whose failure was silent, so a run pointed at a directory that
        // did not exist produced an empty result that looked exactly like a run whose
        // renderer drew nothing — and the picture check is the one gate in this project
        // that has no log-diff substitute. An instrument that can produce nothing without
        // complaining is not an instrument (gotchas 25, 151).
        static bool dirReady = false;
        static bool complained = false;
        if (!dirReady)
        {
            std::error_code ec;
            std::filesystem::create_directories(dumpDir, ec);
            dirReady = true;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dumpDir,
                 (unsigned long long)pres.frame);
        if (FILE* f = fopen(path, "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", width1, height1);
            for (size_t i = 0; i < bytes; i += 4)
                fwrite(&R->presentPixels[i], 1, 3, f);
            fclose(f);
        }
        else if (!complained)
        {
            complained = true;
            fprintf(stderr, "[vk] CZ_VK_FRAME_DUMP cannot write %s — no frames will be "
                            "dumped this run\n",
                    path);
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
    // CZ_VK_EXPOSURE_TRACE=<file> — one line per frame: how many draws set an exposure
    // and the range of values they set. Written here, at the present, so its frame
    // numbers are the same ones CZ_VK_SNAP_FRAME and CZ_VK_FRAME_STATS use.
    static FILE* expFile = nullptr;
    static bool expTried = false;
    if (!expTried)
    {
        expTried = true;
        if (const char* path = Env("CZ_VK_EXPOSURE_TRACE"))
        {
            expFile = fopen(path, "w");
            if (expFile)
                fprintf(expFile, "# frame draws expMin expMax\n");
            else
                fprintf(stderr, "[vk] cannot write CZ_VK_EXPOSURE_TRACE=%s\n", path);
        }
    }
    if (expFile)
    {
        fprintf(expFile, "%llu %u %.6f %.6f\n", (unsigned long long)R->frame,
                R->expDraws, double(R->expMin), double(R->expMax));
        // Reset unconditionally, including when the file could not be opened, so the
        // counters never accumulate across frames in a run that is not tracing.
    }
    R->expDraws = 0;

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
                        "surfHash msec\n");
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
    // Set it to the scene's resolve destination. At the title screen that is
    // **0684B000** — NOT 06BE4000, which was written here and quoted project-wide from
    // phase 5 to part 13 and is the scene DEPTH's first tile. It held colour pixels
    // only because our resolve copied the colour buffer for depth resolves too, so the
    // wrong label was confirmed every time it was checked (part 14, gotcha 205).
    // CZ_VK_RESOLVE_TRACE names the right address for any era.
    static const char* surfaceEnv = Env("CZ_VK_FRAME_STATS_SURFACE");
    static const uint32_t statsSurface =
        surfaceEnv ? uint32_t(strtoul(surfaceEnv, nullptr, 16)) & 0x1FFFFFFF : 0;
    std::vector<uint8_t> surfacePixels;
    uint32_t surfaceW = 0, surfaceH = 0;
    if (statsFile && statsSurface)
    {
        // No depth bit in the key, deliberately: "coverage" and "mean luminance" do not
        // mean anything over a depth surface, and the snapshot map's key carries the
        // distinction so the metric cannot accidentally read one through the wrong
        // image aspect.
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
        // Milliseconds since the first measured frame — APPENDED, so every column index
        // any existing tool reads is unchanged.
        //
        // It exists because the frame rate of an ERA is not a number this project could
        // previously state. Every frame-rate figure it owns divides a whole run's frame
        // count by its wall time, which for a run that boots, walks four menus, loads,
        // and only then plays is an average over eras that differ by more than the
        // effect anyone wants to measure. "8-12 fps in gameplay" was an operator's
        // stopwatch. With a timestamp per frame, any era the camera fingerprint or the
        // draw count can delimit has its own measurable rate, from a run that was
        // already being made for another reason.
        static const auto statsT0 = std::chrono::steady_clock::now();
        const long long msec = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - statsT0).count();
        fprintf(statsFile,
                "%llu %llu %llu %016llx %016llx %u %u %.4f %.3f %llu %016llx "
                "%u %u %.4f %.3f %llu %016llx %lld\n",
                (unsigned long long)pres.frame, (unsigned long long)pres.draws,
                (unsigned long long)pres.vertices,
                (unsigned long long)pres.drawFingerprint,
                (unsigned long long)pres.cameraFingerprint, width1, height1,
                pixels ? 100.0 * double(lit) / double(pixels) : 0.0,
                pixels ? double(lumaSum) / double(pixels) : 0.0,
                (unsigned long long)distinct, (unsigned long long)ph,
                surfaceW, surfaceH,
                spixels ? 100.0 * double(slit) / double(spixels) : 0.0,
                spixels ? double(slumaSum) / double(spixels) : 0.0,
                (unsigned long long)sdistinct,
                spixels ? (unsigned long long)sph : 0ull, msec);
        fflush(statsFile);
    }
    // The fingerprint and vertex accumulators are reset at the SUBMIT above, not here:
    // they belong to the frame that was just recorded, and everything from the present
    // onwards is about a different one.

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
    // CZ_CAPTURE_KEY=<dir> — ONE PRESS, ONE PLACE, EVERY ARTIFACT, into one directory.
    //
    // The three instruments that answer "what does this surface look like and why" were
    // three environment variables writing to three places, and two of them fire on a
    // frame NUMBER rather than on the operator. Standing in front of a defect with a
    // zombie chewing on you is not the moment to be reading a frame counter out of the
    // title bar, and the parts of this project that need an operator are the parts that
    // have to cost them the least (gotcha 190). So this sets all three at once and names
    // every file after the same frame:
    //
    //   capture_<frame>.ppm        the presented picture
    //   capture_<frame>.census     every draw: shaders, fetch slots, addresses, DIMENSION
    //   snap_*.ppm                 every resolve snapshot of that frame
    //
    // The census columns are deliberately the same fields `tools/xtr_draw_bindings.py`
    // prints for a Xenia `.xtr`, so ours and hardware's can be read side by side — which
    // is the whole point, and it is what part 27 had to do by hand.
    static const char* captureDir = Env("CZ_CAPTURE_KEY");
    static const bool captureDirReady = [] {
        if (const char* d = Env("CZ_CAPTURE_KEY"))
        {
            std::error_code ec;
            std::filesystem::create_directories(d, ec);
            fprintf(stderr, "[vk] CZ_CAPTURE_KEY armed: press F9 to capture the picture, "
                            "the per-draw census and every resolve snapshot of one frame "
                            "into %s\n", d);
        }
        return true;
    }();
    (void)captureDirReady;
    static const char* snapDir = captureDir ? captureDir : Env("CZ_VK_SNAP_DUMP");
    static const uint64_t snapFrame =
        Env("CZ_VK_SNAP_FRAME") ? strtoull(Env("CZ_VK_SNAP_FRAME"), nullptr, 10) : 600;
    // F9 — the operator's own trigger, consumed here. Asked for from inside the game,
    // standing on a defect, waiting for the frame counter in the title bar to reach a
    // number chosen before the run started: a fixed `CZ_VK_SNAP_FRAME` is a fine trigger
    // for a boot-time question and the wrong one for any question about a PLACE. The edge
    // is consumed unconditionally so a press cannot sit latched and fire on some later
    // frame, and a press with no destination SAYS SO rather than doing nothing visible —
    // an instrument that silently declines is the failure shape this project keeps paying
    // for (gotchas 7, 151).
    const bool snapKey = Host_ConsumeSnapDumpPressed();
    bool snapKeyNow = snapKey;   // see the CZ_CAPTURE_KEY note at the dump
    // The SAME press also arms the per-draw census for the NEXT frame — next, not this
    // one, because this frame's draws are already recorded by the time a present is
    // reached. One press therefore yields two views of one place: every surface in the
    // frame (the snapshots) and every draw that built it.
    static std::string captureCensus =
        captureDir ? std::string(captureDir) + "/capture.census" : std::string();
    static const char* censusPath =
        captureDir ? captureCensus.c_str() : Env("CZ_VK_DRAW_CENSUS");
    if (snapKey && censusPath && !R->drawCensusFrame)
    {
        R->drawCensusFrame = R->frame + 1;
        // The PICTURE of the same frame, which is the artifact the other two exist to
        // explain and the only one that was still on a fixed interval. Armed for the
        // next frame for the same reason the census is: this frame's draws are already
        // recorded by the time a present is reached, so a picture taken now and a census
        // taken next frame would be two different moments described as one.
        R->capturePictureFrame = R->frame + 1;
        // CZ_VK_DRAW_ID=1 — THE CENSUS FRAME ITSELF paints draw indices instead of
        // colours, and it must be the same frame: a draw index is only meaningful
        // against the draw list it was numbered in. The first version armed the NEXT
        // frame so that one press could yield both a picture and a map, and the very
        // first read showed why that is wrong — the top "visible" draws resolved to
        // census lines with `mask=0`, draws that write no colour at all, because index
        // 254 of one frame is not index 254 of the next. One frame, one numbering.
        //
        // The cost is that this press yields no usable PICTURE (the post chain is made
        // of draws too, so it paints its own indices over everything). That is said out
        // loud below rather than left for someone to discover in the file.
        if (EnvOn("CZ_VK_DRAW_ID") && R->drawIdModule)
        {
            R->drawIdArmed = true;
            fprintf(stderr, "[vk] F9: the next recorded frame will be a DRAW-ID map "
                            "(read it with tools/drawid_read.py)\n");
        }
        fprintf(stderr, "[vk] F9: capturing frame %llu -> picture, %llu-draw census and "
                        "every resolve snapshot\n",
                (unsigned long long)R->drawCensusFrame,
                (unsigned long long)R->drawsThisFrame);
    }
    if (snapKey && !snapDir && !censusPath)
    {
        static bool complained = false;
        if (!complained)
        {
            complained = true;
            fprintf(stderr, "[vk] F9 pressed but neither CZ_VK_SNAP_DUMP nor "
                            "CZ_VK_DRAW_CENSUS is set — nothing was dumped\n");
        }
    }
    // Close the census at the END of the frame it covered, and say how many draws it saw
    // — a census whose file exists but is short is otherwise indistinguishable from one
    // that ran on a frame with nothing in it.
    if (R->drawCensusFrame && R->frame > R->drawCensusFrame && R->drawCensusFile)
    {
        fclose(R->drawCensusFile);
        R->drawCensusFile = nullptr;
        // The count is the census's OWN line counter, not `drawsThisFrame` — that has
        // already been reset by the frame boundary we are standing on, and printing it
        // here would report zero for a census that worked perfectly.
        fprintf(stderr, "[vk] draw census written: %llu draws of frame %llu\n",
                (unsigned long long)R->drawCensusLines,
                (unsigned long long)R->drawCensusFrame);
        R->drawCensusLines = 0;
        R->drawCensusFrame = 0;
    }
    // ONE PRESS MUST MEAN ONE FRAME. The snapshot dump used to fire on the press itself,
    // i.e. on the frame BEFORE the one the census and the picture cover — so a capture
    // produced three artifacts labelled as one place and describing two consecutive
    // frames. In a crowd those are two different pictures, and the whole value of the
    // capture is that its three views are of the same moment. So under CZ_CAPTURE_KEY the
    // press arms the dump for the next frame like the other two; the standalone
    // CZ_VK_SNAP_DUMP path keeps its old immediate behaviour, which several recorded
    // measurements were taken with.
    if (captureDir && snapKey)
    {
        R->captureSnapFrame = R->frame + 1;
        snapKeyNow = false;
    }
    if (R->captureSnapFrame && R->frame == R->captureSnapFrame)
    {
        R->captureSnapFrame = 0;
        snapKeyNow = true;
    }
    // And the fixed-frame dump is OFF under CZ_CAPTURE_KEY: that variable means "the
    // operator decides when", and 130 files from frame 600 in the capture directory is
    // noise the operator then has to tell apart from their own press.
    // THE DRAW-ID FRAME DUMPS ITS SNAPSHOTS TOO, and it must: the ID image only exists
    // in the SCENE COLOUR, before the post chain. The presented picture cannot carry it,
    // because the post passes are draws as well and would paint their own indices over
    // the whole screen — so the map has to be read off the resolve, and this is the
    // dump that writes resolves out.
    const bool drawIdNow = R->drawIdActive;
    if (drawIdNow)
    {
        R->drawIdArmed = false;
        R->drawIdActive = false;
        R->drawIdRanOnFrame = R->frame;
    }
    if (snapDir && ((R->frame == snapFrame && !captureDir) || blackTransition ||
                    snapKeyNow || drawIdNow))
    {
        if (drawIdNow)
            fprintf(stderr, "[vk] DRAW-ID: frame %llu rendered %llu draws as indices; its "
                            "resolve snapshots ARE the ID map\n",
                    (unsigned long long)R->frame, (unsigned long long)R->drawsThisFrame);
        if (snapKeyNow)
            fprintf(stderr, "[vk] F9: dumping every resolve snapshot of frame %llu\n",
                    (unsigned long long)R->frame);
        // CREATE THE DIRECTORY, and say so if the first file still cannot be written.
        // Without this the dump announces "dumping every resolve snapshot of frame N",
        // iterates every surface, and writes NOTHING when the directory is absent — which
        // is what happened the first time it was pointed at a fresh path, and it looks
        // exactly like a renderer that had no snapshots to give. `CZ_SHADER_DUMP` had this
        // same defect and part 25 fixed it there; the fix belongs at every dump site.
        {
            std::error_code ec;
            std::filesystem::create_directories(snapDir, ec);
        }
        bool wroteOne = false;
        for (const auto& [dest, snap] : R->snapshots)
        {
            const size_t n = size_t(snap.image.width) * snap.image.height * 4;
            if (n > R->readback.size)
                continue;
            const VkImageAspectFlags aspect = snap.fromDepth
                                                  ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                  : VK_IMAGE_ASPECT_COLOR_BIT;
            RunImmediate([&](VkCommandBuffer cb) {
                Image& img = const_cast<Image&>(snap.image);
                Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
                VkBufferImageCopy c{};
                c.imageSubresource = { aspect, 0, 0, 1 };
                c.imageExtent = { img.width, img.height, 1 };
                vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       R->readback.buffer, 1, &c);
                Barrier(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);
            });
            // The FRAME is in the name because CZ_VK_SNAP_ON_BLACK can fire on more
            // than one frame, and a chain that silently overwrote the transition frame
            // with the one after it would destroy the only frame worth having.
            char path[512];
            // The ID frame's files are named apart so a directory of snapshots cannot
            // be misread: an ID map looks like a garish colour noise image, and mistaking
            // one for a picture is exactly the sort of confusion this instrument exists
            // to end.
            snprintf(path, sizeof path, "%s/%sf%06llu_snap_%08X_%ux%u%s.ppm", snapDir,
                     drawIdNow ? "drawid_" : "",
                     (unsigned long long)R->frame, dest & 0x1FFFFFFF, snap.image.width,
                     snap.image.height, snap.fromDepth ? "_depth" : "");
            FILE* f = fopen(path, "wb");
            if (!f)
            {
                static bool complained = false;
                if (!complained)
                {
                    complained = true;
                    fprintf(stderr, "[vk] CZ_VK_SNAP_DUMP cannot write %s — NO snapshots "
                                    "will be dumped this run\n", path);
                }
            }
            if (f)
            {
                wroteOne = true;
                fprintf(f, "P6\n%u %u\n255\n", snap.image.width, snap.image.height);
                if (snap.fromDepth)
                {
                    // The depth aspect of D24_UNORM_S8_UINT comes back one 32-bit word
                    // per texel with the value in the low 24 bits. A perspective depth
                    // buffer's values all sit within a hair of 1.0, so a linear grey
                    // would be a white rectangle whatever it contained — the image is
                    // therefore stretched between the surface's OWN min and max, and
                    // the filename says `_depth` so nobody reads it as a colour
                    // surface. The range is printed with it, because the stretch is a
                    // display choice and the numbers are the measurement.
                    uint32_t lo = 0xFFFFFFFFu, hi = 0;
                    for (size_t i = 0; i < n; i += 4)
                    {
                        uint32_t v;
                        memcpy(&v, R->readback.mapped + i, 4);
                        v &= 0xFFFFFFu;
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                    const double span = hi > lo ? double(hi - lo) : 1.0;
                    for (size_t i = 0; i < n; i += 4)
                    {
                        uint32_t v;
                        memcpy(&v, R->readback.mapped + i, 4);
                        v &= 0xFFFFFFu;
                        const uint8_t g = uint8_t(255.0 * double(v - lo) / span);
                        const uint8_t rgb[3] = { g, g, g };
                        fwrite(rgb, 1, 3, f);
                    }
                    fprintf(stderr, "[vk]   %08X is a DEPTH snapshot, 24-bit range "
                                    "%u..%u (%.6f..%.6f)\n",
                            dest & 0x1FFFFFFF, lo, hi, double(lo) / 16777215.0,
                            double(hi) / 16777215.0);
                }
                else
                {
                    for (size_t i = 0; i < n; i += 4)
                        fwrite(R->readback.mapped + i, 1, 3, f);
                }
                fclose(f);
            }
        }
        fprintf(stderr, "[vk] dumped %zu resolve snapshots to %s%s\n",
                R->snapshots.size(), snapDir,
                wroteOne ? "" : "  — NONE OF THEM WERE WRITTEN");
    }

    static const uint64_t statsEvery =
        Env("CZ_VK_STATS") ? std::max(1L, strtol(Env("CZ_VK_STATS"), nullptr, 10)) : 0;
    if (statsEvery && (R->frame % statsEvery) == 0)
        VkRenderer_DumpStats();

    // CZ_VK_PROFILE=N — the frame's CPU time by phase, every N seconds.
    //
    // On a CLOCK rather than a frame count, and the reason is this project's own
    // history: a report every N frames samples a different amount of wall time in every
    // era, so the boot's fast frames and gameplay's slow ones would be averaged by
    // whatever the interval happened to buy (gotcha 186). Reporting per second makes
    // the fps column mean the same thing everywhere, which is the entire point of
    // having it.
    if (g_profileOn)
    {
        static const auto t0 = std::chrono::steady_clock::now();
        static auto last = t0;
        static uint64_t lastFrame = 0;
        static double period =
            Env("CZ_VK_PROFILE") ? std::max(1.0, atof(Env("CZ_VK_PROFILE"))) : 5.0;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= period)
        {
            const uint64_t frames = R->frame - lastFrame;
            const double ms = dt * 1000.0;
            // Every phase as a percentage of the WALL time of the window, so the
            // columns are comparable and their shortfall from 100% is the frame time
            // this instrument does not yet account for — which is a number worth
            // seeing rather than hiding.
            auto pct = [&](uint64_t ns) { return 100.0 * (double(ns) * 1e-6) / ms; };
            const double perFrame = frames ? ms / double(frames) : 0.0;
            // Every phase below is EXCLUSIVE of every other (see ProfScope), so the
            // totals are sums rather than subtractions. That is the whole difference:
            // a subtraction hides an error in one term inside another term's residual,
            // where a sum leaves it visible in `outside`.
            //
            // `other` is DoDraw's own untimed work — the register decode, the
            // pipeline-key build and its lookup, the fetch-constant walk, and the
            // always-on censuses. `outside` is everything that is not the renderer at
            // all: the guest's simulation, the command processor, and any wait between
            // frames.
            const uint64_t drawTotal = g_prof.constants + g_prof.streams +
                                       g_prof.textures + g_prof.record +
                                       g_prof.drawOther;
            const uint64_t submitTotal =
                g_prof.submit + g_prof.submitCall + g_prof.fenceWait;
            const uint64_t known = drawTotal + submitTotal + g_prof.readback;
            fprintf(stderr,
                    "[vkprof] %.1f fps (%.1f ms/frame, %llu draws/frame) | draw %.1f%% "
                    "[constants %.1f streams %.1f textures %.1f record %.1f other "
                    "%.1f] submit %.1f%% [call %.1f gpu %.1f] readback %.1f%% "
                    "outside %.1f%%\n",
                    frames / dt, perFrame,
                    (unsigned long long)(frames ? g_prof.draws / frames : 0),
                    pct(drawTotal), pct(g_prof.constants), pct(g_prof.streams),
                    pct(g_prof.textures), pct(g_prof.record), pct(g_prof.drawOther),
                    pct(submitTotal), pct(g_prof.submitCall), pct(g_prof.fenceWait),
                    pct(g_prof.readback), 100.0 - pct(known));

            // Pipeline creation, broken out of `other`. Printed only when it happened,
            // because a line of zeroes every window would train the eye to skip it —
            // and the whole point is that this is rare and expensive rather than
            // steady. `of other` is the share it explains: if a spike in `other` is
            // compilation, that number is most of it, and if it is not, the counter
            // says so just as clearly.
            if (g_prof.pipelinesCreated)
                fprintf(stderr,
                        "[vkprof] pipelines %llu created (%.2f/frame, %.1f ms total, "
                        "%.2f ms each) = %.1f%% of frame, %.0f%% of `other`\n",
                        (unsigned long long)g_prof.pipelinesCreated,
                        frames ? double(g_prof.pipelinesCreated) / double(frames) : 0.0,
                        double(g_prof.pipelineNs) * 1e-6,
                        double(g_prof.pipelineNs) * 1e-6 /
                            double(g_prof.pipelinesCreated),
                        pct(g_prof.pipelineNs),
                        g_prof.drawOther ? 100.0 * double(g_prof.pipelineNs) /
                                               double(g_prof.drawOther) : 0.0);

            // ...and what `outside` actually IS. The renderer runs on the graphics
            // pump's thread, so everything the pump does between two presents is in
            // that column — including the sleep at the top of its loop, which is not
            // work and which no cycles profile can see (gpu/pump_stats.h). `ticks` is
            // the number that makes the rest readable: the ring walk stops at every
            // unsatisfied WAIT_REG_MEM and resumes on the NEXT tick, so a frame
            // costs at least one sleep period per hand-off wait in it.
            static PumpStats lastPump{};
            const PumpStats p = PumpStats_Read();
            const uint64_t dTicks = p.ticks - lastPump.ticks;
            const uint64_t walkNs = p.walkNs - lastPump.walkNs;
            // `pm4` is the command processor's OWN cost, and it needs saying because
            // the walk is where the renderer is called from: `walk` contains every
            // draw, every submit and the readback, so reading it as the command
            // processor's cost over-states that by the whole of the renderer. This is
            // the 10.98 ms term `docs/perf-cpu-plan.md` §2 is about, and until now it
            // could only be got by subtracting two lines of this report by hand.
            const uint64_t pm4Ns = walkNs > known ? walkNs - known : 0;
            fprintf(stderr,
                    "[vkprof] pump %llu ticks (%.2f/frame) | sleep %.1f%% walk %.1f%% "
                    "[pm4 %.1f] vblank-isr %.1f%% | unaccounted %.1f%%\n",
                    (unsigned long long)dTicks,
                    frames ? double(dTicks) / double(frames) : 0.0,
                    pct(p.sleepNs - lastPump.sleepNs), pct(walkNs), pct(pm4Ns),
                    pct(p.isrNs - lastPump.isrNs),
                    100.0 - pct((p.sleepNs - lastPump.sleepNs) + walkNs +
                                (p.isrNs - lastPump.isrNs)));

            // ...and what the walk was WALKING. `pm4` above is a number of
            // milliseconds; on its own it supports no hypothesis about what to change,
            // which is the state §2 of `docs/perf-cpu-plan.md` describes as
            // "completely uninstrumented inside". These two counts turn it into a cost
            // per packet and a cost per register-write dword, and `WriteRegister` — the
            // section's leading suspect, called once per dword of every SET_CONSTANT —
            // is testable the moment the dword rate is known.
            //
            // Read off the same window as everything above, so the arithmetic is
            // ns/packet = pm4Ns / dPackets with no cross-window mixing.
            static uint64_t lastPackets = 0, lastRegWrites = 0;
            const uint64_t packets = Pm4_PacketCount();
            const uint64_t regWrites = Pm4_RegisterWriteCount();
            const uint64_t dPackets = packets - lastPackets;
            const uint64_t dRegWrites = regWrites - lastRegWrites;
            lastPackets = packets;
            lastRegWrites = regWrites;
            fprintf(stderr,
                    "[vkprof] pm4 %llu packets (%llu/frame, %.0f ns each) | %llu "
                    "register dwords (%llu/frame, %.1f/packet)\n",
                    (unsigned long long)dPackets,
                    (unsigned long long)(frames ? dPackets / frames : 0),
                    dPackets ? double(pm4Ns) / double(dPackets) : 0.0,
                    (unsigned long long)dRegWrites,
                    (unsigned long long)(frames ? dRegWrites / frames : 0),
                    dPackets ? double(dRegWrites) / double(dPackets) : 0.0);
            lastPump = p;

            // The stream cache, when asked for. Printed inside the profile window so the
            // rates are per-frame over the SAME frames the `streams` percentage above is
            // averaged over — a census counted over the whole run and a percentage
            // counted over five seconds cannot be divided into each other. The divisor is
            // PRESENTED frames, like every other rate on these lines, not frames that
            // recorded a draw; a frame with no draws never calls BeginFrame at all.
            // The cross-frame store, ALWAYS — not behind the census, because this is the
            // line that says whether the thing is working and whether it is serving stale
            // data, and a counter nobody looks at by default is a counter that reports a
            // silent regression to nobody (gotcha 151). Rates are per PRESENTED frame,
            // like everything else on these lines.
            if (R->persistOn)
            {
                const Renderer::PersistStats& p = R->persistStats;
                const uint64_t touched =
                    p.hits + p.fills + p.stale + p.overflow + p.staleEvicted;
                fprintf(stderr,
                        "[vkprof] store %llu first-touch/frame: %.1f%% served across the "
                        "frame boundary, %.2f MB/frame NOT copied | fills %llu stale %llu "
                        "(%llu evicted, no twin) overflow %llu | guard read %.2f MB/frame\n",
                        (unsigned long long)(frames ? touched / frames : 0),
                        touched ? 100.0 * double(p.hits) / double(touched) : 0.0,
                        frames ? double(p.hitBytes) / double(frames) / 1048576.0 : 0.0,
                        (unsigned long long)(frames ? p.fills / frames : 0),
                        (unsigned long long)p.stale,
                        (unsigned long long)p.staleEvicted,
                        (unsigned long long)p.overflow,
                        frames ? double(p.guardBytes) / double(frames) / 1048576.0 : 0.0);
                // The exposure the bound leaves behind: streams too large to hash
                // exactly, which are therefore only sampled and CAN hide a small edit.
                // This is the population item 00c's defect lived in, so it is reported
                // rather than assumed to be empty.
                fprintf(stderr,
                        "[vkprof] guard exact to %zu B; %llu streams/frame exceeded it "
                        "and were SAMPLED (a small edit inside one of these is invisible "
                        "-- item 00c)\n",
                        g_guardBytes,
                        (unsigned long long)(frames ? g_guardSampled / frames : 0));
                g_guardSampled = 0;
                fprintf(stderr,
                        "[vkprof] store %zu entries, %llu MB of %llu MB used, %llu flushes"
                        " this window\n",
                        R->persistCache.size(),
                        (unsigned long long)(R->persistCursor >> 20),
                        (unsigned long long)(R->persist.size >> 20),
                        (unsigned long long)p.flushes);
                R->persistStats = Renderer::PersistStats{};
            }
            if (g_streamCensus)
            {
                const StreamCensus& s = g_streamCensus_c;
                const uint64_t n = s.hits + s.misses;
                fprintf(stderr,
                        "[vkprof] streams %llu lookups/frame: %.1f%% hit | copied "
                        "%.2f MB/frame (%llu misses/frame, %llu B each) | hits saved "
                        "%.2f MB/frame\n",
                        (unsigned long long)(frames ? n / frames : 0),
                        n ? 100.0 * double(s.hits) / double(n) : 0.0,
                        frames ? double(s.bytesCopied) / double(frames) / 1048576.0 : 0.0,
                        (unsigned long long)(frames ? s.misses / frames : 0),
                        s.misses ? (unsigned long long)(s.bytesCopied / s.misses) : 0ull,
                        frames ? double(s.bytesHit) / double(frames) / 1048576.0 : 0.0);
                // What a cache that survived the frame boundary would have done. The
                // second line only appears at level 2, because only level 2 knows whether
                // it would have been CORRECT to serve it.
                fprintf(stderr,
                        "[vkprof] streams cross-frame: %.1f%% of misses repeat last "
                        "frame's key (%.2f MB/frame)%s\n",
                        s.misses ? 100.0 * double(s.prevFrameKeyHits) / double(s.misses)
                                 : 0.0,
                        frames ? double(s.prevFrameKeyBytes) / double(frames) / 1048576.0
                               : 0.0,
                        g_streamCensus >= 2 ? "" : "  [level 2 for the content check]");
                static const char* kindName[3] = { "vertex", "index ", "vfetch" };
                for (int k = 0; k < 3; ++k)
                    fprintf(stderr,
                            "[vkprof] streams   %s: %llu misses/frame, %.2f MB/frame "
                            "copied, %.1f%% of those bytes repeat last frame\n",
                            kindName[k],
                            (unsigned long long)(frames ? s.kindMisses[k] / frames : 0),
                            frames ? double(s.kindBytes[k]) / double(frames) / 1048576.0
                                   : 0.0,
                            s.kindBytes[k] ? 100.0 * double(s.kindRepeatBytes[k]) /
                                                 double(s.kindBytes[k])
                                           : 0.0);
                if (g_streamCensus >= 2)
                    fprintf(stderr,
                            "[vkprof] streams cross-frame CONTENT UNCHANGED: %llu of "
                            "%llu repeated keys (%.1f%%), %.2f MB/frame — the rest is the "
                            "guest rewriting the buffer in place\n",
                            (unsigned long long)s.prevFrameSameContent,
                            (unsigned long long)s.prevFrameKeyHits,
                            s.prevFrameKeyHits ? 100.0 * double(s.prevFrameSameContent) /
                                                     double(s.prevFrameKeyHits)
                                               : 0.0,
                            frames ? double(s.prevFrameSameBytes) / double(frames) /
                                         1048576.0
                                   : 0.0);
                // THE GUARD'S POWER. The persistent store decides staleness with a
                // bounded-cost fingerprint (at most 512 bytes, exact below that); this
                // line is the full hash checking its work. Anything but zero is a stale
                // vertex buffer handed to a draw, and this is the ONLY thing in the
                // runtime that can see it. It reads zero when the store is off too — for
                // the trivial reason that nothing was served across a frame — so read it
                // alongside the `store` line above, never on its own.
                if (g_streamCensus >= 2)
                    fprintf(stderr,
                            "[vkprof] streams GUARD MISSED: %llu of %llu real content "
                            "changes served STALE by the cross-frame store%s\n",
                            (unsigned long long)s.guardMissed,
                            (unsigned long long)(s.prevFrameKeyHits -
                                                 s.prevFrameSameContent),
                            g_streamPoison
                                ? "  [POISON ON — the full hash calls every repeat a "
                                  "change while the guard correctly does not, so this "
                                  "SHOULD equal the repeat count]"
                                : "");
                // ...and WHICH ones, because that is what picks the invalidation
                // mechanism. Cumulative over the run, so this list is the answer to "is
                // the rewritten set a recurring few, and are they contiguous in guest
                // memory". Capped at 32 lines with the remainder NAMED rather than
                // dropped silently (gotcha 109) — and the guest address range is printed
                // whether or not the list is capped, since a range is the thing an
                // exclusion rule would be written against.
                if (g_streamCensus >= 2 && !g_streamChanged.empty())
                {
                    std::vector<std::pair<uint64_t, StreamChange>> v(
                        g_streamChanged.begin(), g_streamChanged.end());
                    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                        return a.second.times > b.second.times;
                    });
                    uint32_t lo = 0xFFFFFFFFu, hi = 0;
                    uint64_t total = 0;
                    for (const auto& e : v)
                    {
                        const uint32_t va = uint32_t(e.first >> 32);
                        lo = std::min(lo, va);
                        hi = std::max(hi, uint32_t(va + e.second.bytes));
                        total += e.second.times;
                    }
                    fprintf(stderr,
                            "[vkprof] streams REWRITTEN IN PLACE: %zu distinct keys, %llu "
                            "occurrences, guest range %08X..%08X%s\n",
                            v.size(), (unsigned long long)total, lo, hi,
                            g_streamPoison ? "  [POISON ON — every repeat lands here by "
                                             "construction; this list is meaningless]"
                                           : "");
                    static const char* kindName2[3] = { "vertex", "index ", "vfetch" };
                    const size_t shown = std::min<size_t>(v.size(), 32);
                    for (size_t i = 0; i < shown; ++i)
                    {
                        const uint32_t va = uint32_t(v[i].first >> 32);
                        const StreamChange& c = v[i].second;
                        fprintf(stderr,
                                "[vkprof] streams   va=%08X size=%llu endian=%llu %s  "
                                "x%llu  frames %llu..%llu\n",
                                va, (unsigned long long)c.bytes,
                                (unsigned long long)(v[i].first & 3), kindName2[c.kind],
                                (unsigned long long)c.times,
                                (unsigned long long)c.firstFrame,
                                (unsigned long long)c.lastFrame);
                    }
                    if (v.size() > shown)
                        fprintf(stderr,
                                "[vkprof] streams   ...and %zu more distinct keys not "
                                "listed\n",
                                v.size() - shown);
                }
                g_streamCensus_c = StreamCensus{};
            }

            g_prof = ProfilePhases{};
            last = now;
            lastFrame = R->frame;
        }
    }

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
    // The state cache's own engagement, as fractions of the draws it was offered.
    // Printed unconditionally, including on the CZ_VK_NO_STATE_CACHE arm where every
    // figure is 0 by construction — an arm whose "on" run is indistinguishable from its
    // "off" run is exactly what a missing counter hides (gotcha 151).
    if (const uint64_t d = R->skips.draws)
        fprintf(stderr,
                "[vk]   binds skipped per draw: pipeline %.1f%% viewport %.1f%% "
                "scissor %.1f%% blend %.1f%% descriptor-sets %.1f%% (of %llu draws)\n",
                100.0 * double(R->skips.pipeline) / double(d),
                100.0 * double(R->skips.viewport) / double(d),
                100.0 * double(R->skips.scissor) / double(d),
                100.0 * double(R->skips.blend) / double(d),
                100.0 * double(R->skips.sets) / double(d), (unsigned long long)d);
    // ...and the two the cache does NOT cover yet, as the repeat rate a cache over them
    // would convert into skips. Reported as counts as well as percentages because the
    // absolute number is what multiplies by the ~340 ns a `vkCmd*` costs here; a high
    // percentage of a small count is not worth a line of code.
    if (R->skips.vertexBinds || R->skips.indexBinds)
        fprintf(stderr,
                "[vk]   binds NOT cached: vertex %llu of %llu repeat the previous "
                "offset (%.1f%%), index %llu of %llu (%.1f%%)\n",
                (unsigned long long)R->skips.vertexBindRepeats,
                (unsigned long long)R->skips.vertexBinds,
                R->skips.vertexBinds
                    ? 100.0 * double(R->skips.vertexBindRepeats) /
                          double(R->skips.vertexBinds)
                    : 0.0,
                (unsigned long long)R->skips.indexBindRepeats,
                (unsigned long long)R->skips.indexBinds,
                R->skips.indexBinds ? 100.0 * double(R->skips.indexBindRepeats) /
                                          double(R->skips.indexBinds)
                                    : 0.0);
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
                    "[vk]     %08X%-7s %4ux%-4u f%-2u | up %llu (zero %llu)  snap %llu  "
                    "tooOld %llu (max age %llu)%s\n",
                    addr & 0x1FFFFFFF, (addr & kSnapshotDepthBit) ? "(depth)" : "",
                    s.width, s.height, s.format, (unsigned long long)s.uploads,
                    (unsigned long long)s.zeroUploads,
                    (unsigned long long)s.fromSnapshot,
                    (unsigned long long)s.snapshotTooOld, (unsigned long long)s.maxAge,
                    note);
        }
    }

    // WHERE THE DIMENSION LIVES IN THE FETCH CONSTANT, read off the two classes the
    // shader partitions every fetch into. `always1` is the AND, `always0` is the
    // complement of the OR; a field that encodes the dimension must be inside the bits
    // where the two classes' patterns differ, and the report prints that disagreement
    // mask directly so the answer is a bit position rather than an argument.
    if (g_dimCensus)
    {
        static const char* kDimName[4] = { "1D", "2D", "3D", "Cube" };
        fprintf(stderr, "[vk]   fetch-constant dimension census — dwords per "
                        "shader-declared dimension:\n");
        for (const auto& [dim, c] : g_dimClasses)
        {
            fprintf(stderr, "[vk]     %-4s  %llu fetches\n",
                    dim < 4 ? kDimName[dim] : "?", (unsigned long long)c.fetches);
            for (uint32_t d = 0; d < 6; d++)
                fprintf(stderr, "[vk]       dword%u always1=%08X always0=%08X\n", d,
                        c.andMask[d], ~c.orMask[d]);
            fprintf(stderr, "[vk]       dword2>>26 (the stack depth Xenia's layout "
                            "predicts is 5 for a cube):");
            for (const auto& [v, n] : c.d2Top)
                fprintf(stderr, " %u x%llu", v, (unsigned long long)n);
            fprintf(stderr, "\n");
        }
        // The pairwise disagreement, which is the actual answer. Only computed between
        // classes that both saw fetches — a class with none has AND=~0 and OR=0, which
        // would disagree with everything and mean nothing (gotcha 3).
        for (const auto& [a, ca] : g_dimClasses)
            for (const auto& [b, cb] : g_dimClasses)
            {
                if (a >= b || !ca.fetches || !cb.fetches)
                    continue;
                for (uint32_t d = 0; d < 6; d++)
                {
                    // Bits one class always sets and the other always clears, either
                    // way round. Anything else varies within a class and cannot be a
                    // constant per-dimension field.
                    const uint32_t sep = (ca.andMask[d] & ~cb.orMask[d]) |
                                         (cb.andMask[d] & ~ca.orMask[d]);
                    if (sep)
                        fprintf(stderr,
                                "[vk]     %s vs %s: dword%u separates on bits %08X\n",
                                a < 4 ? kDimName[a] : "?", b < 4 ? kDimName[b] : "?", d,
                                sep);
                }
            }
    }

    // WHICH SHADERS DISAGREE WITH THEIR OWN FETCH CONSTANTS, and about which texture.
    // Unbounded, unlike the per-occurrence print above, because the population is the
    // question: one shader disagreeing about one placeholder texture and fifty shaders
    // disagreeing about fifty real ones are the same counter and completely different
    // defects. Compare the shader hashes here against `tools/xtr_cube_agreement.py` on a
    // capture — a shader that disagrees here and agrees there is OUR register file; a
    // shader that appears in no capture is a case hardware has never been asked about.
    if (g_dimDisagree)
    {
        static const char* kDimName[4] = { "1D", "2D", "3D", "Cube" };
        uint64_t total = 0;
        for (const auto& [k, e] : g_dimDisagreements)
            total += e.fetches;
        fprintf(stderr,
                "[vk]   shader/constant dimension disagreements: %llu fetches over %zu "
                "distinct (shader, slot, texture) cases\n",
                (unsigned long long)total, g_dimDisagreements.size());
        for (const auto& [k, e] : g_dimDisagreements)
            fprintf(stderr,
                    "[vk]     ps=%016llx vs=%016llx s%-2u shader=%-4s constant=%-4s "
                    "%08X %ux%u fmt=%u  x%llu\n",
                    (unsigned long long)e.psHash, (unsigned long long)e.vsHash, e.slot,
                    e.shaderDim < 4 ? kDimName[e.shaderDim] : "?",
                    e.constDim < 4 ? kDimName[e.constDim] : "?", e.addr, e.w, e.h, e.fmt,
                    (unsigned long long)e.fetches);
    }

    // The texture-content guard. The question is the operator's: is a draw being served
    // an image built from pixels that are no longer at that address?
    if (g_texGuardStats.hits)
    {
        const TexGuardStats& g = g_texGuardStats;
        fprintf(stderr,
                "[vk]   texture guard: %llu cache hits checked, **%llu served an image "
                "whose guest bytes had CHANGED (%.2f%%)**, %llu re-uploaded | guard read "
                "%.1f MB\n",
                (unsigned long long)g.hits, (unsigned long long)g.changed,
                100.0 * double(g.changed) / double(g.hits),
                (unsigned long long)g.reuploaded,
                double(g.guardBytes) / 1048576.0);
        if (g_texGuardPoison)
            fprintf(stderr, "[vk]   (POISONED: that share MUST be 100.00%% — the census "
                            "is only trustworthy if it can also report a positive)\n");
        // The addresses, worst first. A ratio alone cannot separate "one atlas the CPU
        // rewrites every frame" from "a third of the world's textures are wrong", and
        // those are different defects with different fixes.
        std::vector<std::pair<uint32_t, TexGuardAddr>> rows(g_texGuardAddrs.begin(),
                                                            g_texGuardAddrs.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.changed > b.second.changed;
        });
        size_t shown = 0, withChange = 0;
        for (const auto& r : rows)
            if (r.second.changed)
                ++withChange;
        fprintf(stderr, "[vk]   %zu of %zu cached texture addresses served changed "
                        "bytes at least once; worst 24:\n",
                withChange, rows.size());
        for (const auto& [addr, a] : rows)
        {
            if (!a.changed || shown++ >= 24)
                break;
            fprintf(stderr, "[vk]     %08X %4ux%-4u f%-2u  %llu of %llu hits stale "
                            "(%.1f%%)\n",
                    addr, a.width, a.height, a.format, (unsigned long long)a.changed,
                    (unsigned long long)a.hits,
                    100.0 * double(a.changed) / double(a.hits));
        }
    }
}

