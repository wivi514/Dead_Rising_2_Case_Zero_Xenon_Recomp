#include "pm4.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

// The only cross-module include here, and it is one function: the present seam.
// Deliberately not the renderer's or the window's internals — this file's isolation
// (see the guest-memory note below: it must stay reusable by an offline replay
// harness) survives exactly as long as that stays true.
#include "../cpu/timebase.h"
#include "../host/window.h"
#include "vk_renderer.h"
#include "xenos.h"   // register indices, for the bin trace's window scissor

namespace {

// --- guest memory ---------------------------------------------------------------
// Deliberately not ppc_context.h's PPC_LOAD/STORE macros, which need a `base` named
// exactly that in scope: keeping the accessors local is what lets this translation
// unit be reused by an offline replay harness without dragging in the recompiled
// image or SIMDe. The semantics are identical — guest memory is big-endian.
inline uint32_t GuestLoad32(const uint8_t* base, uint32_t va)
{
    uint32_t raw;
    memcpy(&raw, base + va, 4);
    return __builtin_bswap32(raw);
}

inline void GuestStore32(uint8_t* base, uint32_t va, uint32_t value)
{
    const uint32_t raw = __builtin_bswap32(value);
    memcpy(base + va, &raw, 4);
}

// Command-stream addresses are physical. Our physical arena is the cached view at
// 0xA0000000 (kernel/heap.cpp), and this is the exact inverse of what our
// MmGetPhysicalAddress does (`address & 0x1FFFFFFF`), which is what makes the round
// trip self-consistent.
//
// A caveat that cost real time to establish, and that matters the moment anyone
// compares our physical addresses against a capture's: **Xenia's physical addresses
// are NOT `virtual & 0x1FFFFFFF`.** A1 shows `MmGetPhysicalAddress(E3D71000)`
// answering `03D72000` — a constant +0x1000 skew — and the ring-buffer geometry only
// makes sense with it. Under the naive mask the ring appears to start 0x1000 inside
// its own allocation and overrun the end by the same amount; with the skew it starts
// exactly at the allocation base and fits exactly. Our convention is internally
// consistent and needs no skew, but a physical address in our log is 0x1000 below
// the same object's address in a capture.
constexpr uint32_t kPhysArenaBase = 0xA0000000u;
constexpr uint32_t kPhysArenaEnd = 0xBFFF0000u;

inline uint32_t PhysToVa(uint32_t addr)
{
    return kPhysArenaBase | (addr & 0x1FFFFFFFu);
}

// The GPU's per-address endian swizzle, carried in the low 2 bits of every address
// dword a packet holds: 0 = none, 1 = 8-in-16, 2 = 8-in-32, 3 = 16-in-32.
inline uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
        default:
        case 0: return v;
        case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        case 2: return __builtin_bswap32(v);
        case 3: return (v >> 16) | (v << 16);
    }
}

// ...and the swap that is left to APPLY, which is not the same thing, because
// GuestLoad32/GuestStore32 already perform an 8-in-32 of their own (that is what
// "guest memory is big-endian" means on a little-endian host). The code in the
// packet describes the whole conversion from the GPU's native little-endian dword to
// what the PowerPC will read; our accessors have already done the 8-in-32 half of
// it. So the residual swap is the code composed with 8-in-32 — which makes code 2,
// the code these fences use, the IDENTITY.
//
// Getting this wrong is silent and looks like something else entirely. Asura's Wrath
// stored its fences with a full GpuSwap on top of the accessor's own, and the
// symptom was not "wrong endianness": it was a retired-fence counter reading
// 0B000000 instead of 0000000B and three WAIT_REG_MEM packets missing references
// they were each exactly one byte-swap away from. Every one of those reads as a
// plausible protocol problem rather than a swap.
inline uint32_t GpuSwapResidual(uint32_t v, uint32_t endian)
{
    return GpuSwap(v, (endian & 3) ^ 2);
}

// --- state ------------------------------------------------------------------------
uint32_t g_ringBase = 0;   // guest virtual address of the ring
uint32_t g_ringDwords = 0; // ring size in dwords
uint32_t g_cursor = 0;     // our read pointer, ring-relative, in dwords

// The brake: hold at an unsatisfied WAIT_REG_MEM instead of evaluating it once and
// carrying on. See the WAIT_REG_MEM case for the mechanism and StallPlan for the
// resume. Read once; an env lookup per packet would be measurable at 1.27 M packets
// per 25 s.
//
// ON BY DEFAULT since phase C part 6. This is what hardware does — the command
// processor cannot run ahead of the CPU — and it was off only because part 5 had one
// run per arm behind it. Promoted on 40 runs, 10 per configuration, arms alternated
// within each round, same binary (docs/d3d-translation-plan.md, "Phase C part 6"):
//
//   PM4 control  brake on   2,446 frames +-1 over 10 runs, swap queue head == tail
//                           10 of 10, boot to #83 cinezombie.big, 0 crashes,
//                           truncated=0, max hold streak 1 tick
//   PM4 control  brake off  3,680 frames, and the queue OVERFLOWS in 10 of 10 (head
//                           25-29 against tail ~3,679): the title free-running with
//                           nothing draining its flips
//   phase C draw brake on   3,614-3,670 frames, i.e. a 1x spread
//   phase C draw brake off  332 to 3,451,841 frames — a 10,397x BIMODAL spread, three
//                           near-stalled runs and seven runaway
//
// The cost is real and is not a loss: 2,446 frames against 3,680 is the title being
// paced at its own frame timing instead of the CP outrunning it.
//
// CZ_PM4_NO_STOP_ON_WAIT=1 is the same-binary control arm for every claim above, and
// the flag CZ_PM4_STOP_ON_WAIT still works so older recipes keep meaning what they
// said. The failure mode this could have had — a wait nothing ever satisfies, parking
// the ring forever — is measured rather than assumed: `ring: waits ... max=` in the
// ring trace is the longest run of consecutive ticks spent on one wait, and it reads
// 1 on the control arm and 2 on the draw arm against 5,491 for a deliberately parked
// ring (CZ_ISR_SINGLE_CPU=1 with the brake on).
const bool g_stopOnWait = getenv("CZ_PM4_NO_STOP_ON_WAIT") == nullptr;

// CZ_PM4_ZERO_IS_NOP — the rejected reading of a zero header, kept as a measurable
// arm rather than deleted; see ExecutePacket.
const bool g_zeroIsNop = getenv("CZ_PM4_ZERO_IS_NOP") != nullptr;

// The register file, covering the real registers plus the SET_CONSTANT windows
// (ALU constants at 0x4000, fetch at 0x4800, bools at 0x4900, loops at 0x4908).
constexpr uint32_t kRegCount = 0x8000;
uint32_t g_regs[kRegCount];

// Registers with side effects (indices from Xenia's register_table.inc).
constexpr uint32_t kRegScratchUmsk = 0x01DC;     // writeback enable, one bit per scratch reg
constexpr uint32_t kRegScratchAddr = 0x01DD;     // writeback base, physical
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31; // bit 31 = coherency action pending

// Predicated binning. SET_BIN_MASK_* tags the following packets with the EDRAM bins
// they touch and SET_BIN_SELECT_* picks the bin being rendered; the hardware ME runs
// a packet whose header bit 0 is set only when (mask & select) != 0. Both default to
// all-ones, i.e. no binning.
//
// Case Zero leans on this harder than the previous port did: SET_BIN_MASK_LO is its
// single most frequent type-3 opcode (2,353,460 of 8,283,322 in B1, over a quarter
// of all type-3 traffic). Getting the predication wrong here would silently drop or
// silently execute a large fraction of the stream.
uint64_t g_binMask = ~0ull;
uint64_t g_binSelect = ~0ull;

// The (mask, select) pair census, for exactly one comparison: `tools/xtr_bin_predication.py`
// prints the same table for capture B1, and the two tables side by side are the only
// way to ask "is discarding these draws what hardware does" — our command processor
// cannot be its own oracle for a rule it is the suspect in. A windowed trace cannot do
// this job: the pairs that dominate are a quarter of a million packets into the boot,
// and the pairs that dominate the PROLOGUE are identical on both sides.
//
// Bounded to 32 distinct pairs with an overflow count rather than a growing map,
// because a census that can allocate inside the command processor is a census that
// can change what it measures. B1 has 8 pairs in 24.5 M packets.
struct BinPair
{
    uint64_t mask = 0, select = 0;
    uint64_t offered = 0, skipped = 0;
    bool used = false;
};
constexpr uint32_t kBinPairs = 32;
BinPair g_binPairs[kBinPairs];
std::atomic<uint64_t> g_binPairOverflow{ 0 };
std::mutex g_binPairMutex;
bool g_binCensus = false;

void BinCensusRecord(uint64_t mask, uint64_t select, bool skipped)
{
    std::lock_guard<std::mutex> lock(g_binPairMutex);
    for (uint32_t i = 0; i < kBinPairs; i++)
    {
        if (!g_binPairs[i].used)
        {
            g_binPairs[i] = { mask, select, 1, skipped ? 1ull : 0ull, true };
            return;
        }
        if (g_binPairs[i].mask == mask && g_binPairs[i].select == select)
        {
            g_binPairs[i].offered++;
            g_binPairs[i].skipped += skipped ? 1 : 0;
            return;
        }
    }
    g_binPairOverflow.fetch_add(1, std::memory_order_relaxed);
}

// --- census -----------------------------------------------------------------------
std::atomic<uint64_t> g_packets{ 0 };
std::atomic<uint64_t> g_types[4];
std::atomic<uint64_t> g_opcodes[128];
std::atomic<uint64_t> g_draws{ 0 };
// Draws the bin mask discarded. Printed next to `draws=` because the ratio is the only
// thing that separates "this pass had few draws" from "this pass had many and hardware
// wanted them in the other tile".
std::atomic<uint64_t> g_drawsPredicatedOut{ 0 };
std::atomic<uint64_t> g_frames{ 0 };
std::atomic<uint64_t> g_interrupts{ 0 };
void (*g_interruptSink)() = nullptr;
void (*g_drawSink)(uint8_t*, const Pm4Draw&) = nullptr;

// The full Xenos type-3 table. A name here means "this command processor has such an
// opcode"; nullptr means it does not, which is load-bearing: the ring resync will
// only accept a named opcode as a plausible packet header, and an unnamed opcode
// appearing in our own stream is a reportable anomaly rather than something to skip.
//
// Written as explicit index assignments rather than a positional initialiser list on
// purpose: the positional form has 128 slots with 46 names scattered through it, and
// a single miscounted nullptr shifts every later opcode by one — which shows up not
// as a compile error but as a plausible-looking census with the wrong names on it.
// That is exactly the error that put SET_BIN_MASK at 0x39 and INTERRUPT at 0x40 in
// `tools/xtr_pm4_census.py`'s first draft.
struct OpcodeNames
{
    const char* name[128] = {};

    OpcodeNames()
    {
        auto set = [&](uint32_t op, const char* n) { name[op] = n; };
        set(0x10, "NOP");
        set(0x21, "REG_RMW");
        set(0x22, "DRAW_INDX");
        set(0x23, "VIZ_QUERY");
        set(0x25, "SET_STATE");
        set(0x26, "WAIT_FOR_IDLE");
        set(0x27, "IM_LOAD");
        set(0x2B, "IM_LOAD_IMMEDIATE");
        set(0x2C, "IM_STORE");
        set(0x2D, "SET_CONSTANT");
        set(0x2E, "LOAD_CONSTANT_CONTEXT");
        set(0x2F, "LOAD_ALU_CONSTANT");
        set(0x34, "DRAW_INDX_BIN");
        set(0x35, "DRAW_INDX_2_BIN");
        set(0x36, "DRAW_INDX_2");
        set(0x37, "INDIRECT_BUFFER_PFD");
        set(0x3B, "INVALIDATE_STATE");
        set(0x3C, "WAIT_REG_MEM");
        set(0x3D, "MEM_WRITE");
        set(0x3E, "REG_TO_MEM");
        set(0x3F, "INDIRECT_BUFFER");
        set(0x44, "COND_EXEC");
        set(0x45, "COND_WRITE");
        set(0x46, "EVENT_WRITE");
        set(0x48, "ME_INIT");
        set(0x4A, "SET_SHADER_BASES");
        set(0x4B, "SET_BIN_BASE_OFFSET");
        set(0x4F, "MEM_WRITE_CNTR");
        set(0x50, "SET_BIN_MASK");
        set(0x51, "SET_BIN_SELECT");
        set(0x52, "WAIT_REG_EQ");
        set(0x53, "WAIT_REG_GTE");
        set(0x54, "INTERRUPT");
        set(0x55, "SET_CONSTANT2");
        set(0x56, "SET_SHADER_CONSTANTS");
        set(0x58, "EVENT_WRITE_SHD");
        set(0x59, "EVENT_WRITE_CFL");
        set(0x5A, "EVENT_WRITE_EXT");
        set(0x5B, "EVENT_WRITE_ZPD");
        set(0x5C, "WAIT_UNTIL_READ");
        set(0x5D, "WAIT_IB_PFD_COMPLETE");
        set(0x5E, "CONTEXT_UPDATE");
        set(0x60, "SET_BIN_MASK_LO");
        set(0x61, "SET_BIN_MASK_HI");
        set(0x62, "SET_BIN_SELECT_LO");
        set(0x63, "SET_BIN_SELECT_HI");
        set(0x64, "XE_SWAP");
    }
};
const OpcodeNames kOpcodeNames;

// --- shader loads -----------------------------------------------------------------
// IM_LOAD (0x27) and IM_LOAD_IMMEDIATE (0x2B) are how the guest binds microcode, and
// they are the renderer's entry point into the stream: everything the GPU does to a
// draw is decided by the two shaders bound when the draw packet arrives.
//
// WHY THE IDENTITY IS A HASH OF THE BYTES, AND NOT THE ADDRESS
// -----------------------------------------------------------
// The obvious key is the microcode's guest address, and it is wrong twice over. The
// driver recycles a small scratch region for uploads, so one address holds many
// different shaders over a run; and the same shader is uploaded to different addresses
// as buffers are cycled. A content hash is stable under both, and it is also the key
// the offline translation pipeline can compute — `tools/build_shader_spv.sh` hashes the
// same bytes with the same function, so a cache entry and a bound shader agree by
// construction rather than by a naming convention anyone has to maintain.
//
// The hash is FNV-1a over the microcode as a BIG-ENDIAN byte string, i.e. exactly the
// bytes as the guest holds them. That matters for IM_LOAD_IMMEDIATE, whose microcode
// arrives as already-swapped dwords through our accessors: it has to be written back
// out big-endian before hashing or the two load paths give different names to the same
// shader, and half the cache silently misses.
uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// A structural check that a buffer is microcode before adopting it as a shader.
//
// This exists because a load that is NOT microcode is worse than no load at all: it
// replaces the bound shader with a hash that no cache entry can match, so every draw
// until the next bind is dropped as "unknown shader" — a large, silent hole in the
// frame that looks like a translation gap. Rejecting it keeps the previous shader,
// which is at worst stale and which the guest is about to rebind anyway.
//
// The check is the microcode's own shape: the control-flow region is a sequence of
// 3-dword groups packing two 48-bit CF instructions, and the first Exec-family
// instruction names the address and count of an instruction block that must lie inside
// the buffer. Random dwords fail that almost always.
bool LooksLikeUcode(const uint8_t* p, uint32_t sizeDwords)
{
    if (sizeDwords < 3 || (sizeDwords % 3) != 0)
        return false;
    auto dw = [&](uint32_t i) {
        return (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    };
    for (uint32_t i = 0; i + 2 < sizeDwords; i += 3)
    {
        const uint32_t cf[2][2] = {
            { dw(i), dw(i + 1) & 0xFFFF },
            { ((dw(i + 1) >> 16) | (dw(i + 2) << 16)), dw(i + 2) >> 16 }
        };
        for (const auto& c : cf)
        {
            const uint32_t op = (c[1] >> 12) & 0xF;
            const uint32_t addr = c[0] & 0xFFF, cnt = (c[0] >> 12) & 7;
            const bool isExec = op == 1 || op == 2 || op == 3 || op == 4 || op == 5 ||
                                op == 6 || op == 13 || op == 14;
            if (isExec && cnt)
                return (addr + cnt) * 3 <= sizeDwords;
        }
    }
    return false;
}

// The currently bound pair. Index 0 = vertex, 1 = pixel, matching the type field the
// packet itself carries.
Pm4ShaderBinding g_boundShaders[2];

// CZ_SHADER_DUMP=<dir> — write one file per distinct microcode blob, named by the same
// hash the renderer looks up. This is how the SPIR-V cache is built: the dump is taken
// from OUR command processor, so the byte range and therefore the name are exactly what
// the runtime will ask for at draw time. Building the cache from Xenia's dumps instead
// would key it on Xenia's idea of where a shader ends, and any disagreement about the
// length is a total, silent cache miss.
const char* g_shaderDumpDir = getenv("CZ_SHADER_DUMP");
std::vector<uint64_t> g_dumpedShaders;

void DumpShader(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t sizeDwords)
{
    if (!g_shaderDumpDir)
        return;
    if (std::find(g_dumpedShaders.begin(), g_dumpedShaders.end(), hash) !=
        g_dumpedShaders.end())
        return;
    g_dumpedShaders.push_back(hash);

    char path[512];
    snprintf(path, sizeof path, "%s/%s_%016llx.ucode", g_shaderDumpDir,
             type == 0 ? "vs" : "ps", static_cast<unsigned long long>(hash));
    if (FILE* f = fopen(path, "wb"))
    {
        fwrite(code, 1, size_t(sizeDwords) * 4, f);
        fclose(f);
        fprintf(stderr, "[imload] dumped %s %016llx (%u dwords) -> %s\n",
                type == 0 ? "VS" : "PS", static_cast<unsigned long long>(hash),
                sizeDwords, path);
    }
}

// The shared body of both load packets: validate, hash, bind, dump, announce once.
void BindShader(uint32_t type, uint32_t ucodeVa, const uint8_t* code, uint32_t sizeDwords)
{
    if (type > 1 || !sizeDwords || sizeDwords > 0x10000)
        return;

    if (!LooksLikeUcode(code, sizeDwords))
    {
        static std::atomic<uint64_t> rejected{ 0 };
        const uint64_t n = rejected.fetch_add(1);
        if (n < 8)
            fprintf(stderr,
                    "[pm4] IM_LOAD target is not microcode, keeping the previous %s "
                    "(va=%08X size=%u first=%02X%02X%02X%02X, occurrence %llu)\n",
                    type == 0 ? "VS" : "PS", ucodeVa, sizeDwords, code[0], code[1],
                    code[2], code[3], static_cast<unsigned long long>(n));
        return;
    }

    const uint64_t hash = Fnv1a(code, size_t(sizeDwords) * 4);
    g_boundShaders[type] = { ucodeVa, sizeDwords, hash };

    // One line per distinct shader, always on. The renderer's own miss report says
    // "hash X is not in the cache"; this is what says which stage it was and how big,
    // and together they are enough to go and translate it without another run.
    static std::vector<uint64_t> announced;
    if (std::find(announced.begin(), announced.end(), hash) == announced.end())
    {
        announced.push_back(hash);
        fprintf(stderr, "[imload] %s va=%08X hash=%016llx size=%u\n",
                type == 0 ? "VS" : "PS", ucodeVa, static_cast<unsigned long long>(hash),
                sizeDwords);
    }

    DumpShader(type, hash, code, sizeDwords);
}

// --- packet source ----------------------------------------------------------------
// One indirection covering both ways a packet stream reaches us: the ring (a wrapping
// window into guest memory) and an indirect buffer (a linear one). A struct rather
// than std::function because the walk calls it once per dword.
struct Source
{
    const uint8_t* base = nullptr;
    uint32_t va = 0;
    uint32_t wrapDwords = 0; // != 0 for the ring

    uint32_t operator()(uint32_t i) const
    {
        const uint32_t index = wrapDwords ? (i % wrapDwords) : i;
        return GuestLoad32(base, va + index * 4);
    }
};

// ---------------------------------------------------------------------------------
// The deliberate WAIT_REG_MEM stall, and why it needs a resume plan.
//
// CZ_PM4_STOP_ON_WAIT makes the command processor do what hardware does: hold at an
// unsatisfied WAIT_REG_MEM instead of evaluating it once and carrying on. It was
// written for, and gated on, `depth == 0` — the ring itself — because at ring level
// "stop" is free: Pm4_Execute simply does not advance its cursor and retries next
// tick.
//
// That gate is why the flag has been measured and retired TWICE (phase C parts 2 and
// 3) without ever having been able to apply to the packets it was aimed at. Case
// Zero's GPU/CPU hand-off blocks — the callback arm, its three WAIT_REG_MEMs, its
// INTERRUPT and its re-poison — are emitted into COMMAND SEGMENTS, and segments reach
// the ring as INDIRECT_BUFFER packets. Every one of those waits is therefore
// evaluated at depth 1 or deeper, where the flag was a no-op. Gotcha 148 says a
// retired hypothesis is retired against a binary; this is the sharper form — it was
// retired against a code path that structurally excluded it.
//
// Stopping below the ring is not free, because unwinding to the ring and retrying
// re-walks the indirect buffer FROM THE START, re-executing every packet before the
// wait — including the arm and its INTERRUPT, which is precisely the duplication the
// stall exists to prevent. So a stall records, per depth, the buffer it stopped in
// and the dword it stopped at, and the next tick's walk of that same buffer resumes
// there. Depth 0 needs no entry: the ring cursor is the plan.
struct StallPlan
{
    uint32_t va[9] = {};  // the buffer identity at each depth, 0 = no entry
    uint32_t pos[9] = {}; // the dword to resume at
    bool pending = false;
};
StallPlan g_stallPlan;     // carried from the previous tick
StallPlan g_stallNext;     // being recorded this tick
bool g_stallHit = false;   // an unsatisfied wait stopped this tick's walk

// The three numbers the brake has to be judged on, as COUNTS rather than as the
// running index of a capped print.
//
// Both stall sites print sparsely (the first 8, then every 65,536; the first 4, then
// every 1,048,576), which is right for a log and useless as a measurement: a healthy
// paced boot and a ring parked forever both report "#4" and stop talking. Worse, the
// number that actually decides whether the brake is safe to make the default is the one
// nothing tracked at all — whether a stall is ever RELEASED. A wait that is never
// satisfied is the single failure mode promoting it risks, and `truncated=0` plus a
// plausible frame count looks identical to a healthy run right up until you notice the
// frames stopped climbing (gotcha 81: the missing instrument is the one whose silence
// reads as health).
//
// Reported once a second on the ring trace, beside truncated=, because that is where
// this project already keeps the counter whose nonzero value means a thread is waiting
// forever.
// The number that says "parked" is the CONSECUTIVE-HOLD STREAK, not a release count.
//
// The first version of this counted a release whenever the next tick's stall had a
// different (buffer, dword) identity, and that discriminator is not stable across the
// two arms: phase C re-emits its hand-off block at the SAME private-scratch address
// every frame, so a perfectly healthy re-stall looks identical to being stuck, while
// the PM4 arm's blocks land at rotating ring addresses and every re-stall looks like a
// fresh release. Same behaviour, opposite readings — it scored the control arm 100%
// and the draw arm 4.9% while both were advancing one swap-queue record per frame.
//
// How many ticks IN A ROW the ring has sat on one wait has no such dependency. A title
// pacing itself releases within a tick or three; a ring nothing will ever release grows
// the streak without bound. Max streak is the whole diagnosis in one number.
std::atomic<uint64_t> g_waitUnmet{ 0 };     // WAIT_REG_MEM evaluated and not satisfied
std::atomic<uint64_t> g_ringHeld{ 0 };      // ticks that ended held at such a wait
std::atomic<uint64_t> g_holdStreak{ 0 };    // consecutive ticks held at the SAME wait
std::atomic<uint64_t> g_holdStreakMax{ 0 };

// Returns the dword position the walk stopped at — `sizeDwords` if it consumed the
// whole buffer, less than that if a packet claimed more dwords than remained. Callers
// mostly ignore it; ExecuteLinearVerified below needs it to correlate a truncation
// with a concurrent write.
uint32_t ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth);
void ExecuteLinearVerified(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth);

// CZ_PM4_IB_VERIFY=1 — snapshot every indirect buffer before walking it and compare
// afterwards. See ExecuteLinearVerified for what question this answers and why it is
// not on by default.
const bool g_ibVerify = getenv("CZ_PM4_IB_VERIFY") != nullptr;
std::atomic<uint64_t> g_ibVerifyClean{ 0 };
std::atomic<uint64_t> g_ibVerifyDirty{ 0 };
std::atomic<uint64_t> g_ibTruncated{ 0 };

// The engine's fence completion word, published by gpu/vd.cpp (which is the only place
// that knows the device struct) so StoreGpuRaw can recognise it. Zero until the guest
// has registered a writeback pointer, which is the whole of the early boot.
std::atomic<uint32_t> g_fenceWord{ 0 };
const bool g_fenceMonotonic = [] {
    const char* v = getenv("CZ_PM4_FENCE_MONOTONIC");
    const bool on = v && *v && *v != '0';
    if (on)
        fprintf(stderr, "[pm4] CZ_PM4_FENCE_MONOTONIC=1 — GPU stores that move the fence "
                        "completion word BACKWARDS will be refused. This is a phase C "
                        "part 7 EXPERIMENT arm and must never be on for a gate run.\n");
    return on;
}();
std::atomic<uint64_t> g_fenceRegressions{ 0 };

// --- memory-writing helpers -------------------------------------------------------
// Refuse anything that does not land in the physical arena rather than storing
// through it: a malformed or half-written packet parsed as a fence would otherwise
// scribble an arbitrary guest address, and the resulting corruption surfaces nowhere
// near here.
bool StoreGpuRaw(uint8_t* base, uint32_t physAddr, uint32_t value)
{
    const uint32_t va = PhysToVa(physAddr & ~3u);
    // CZ_PM4_MEM_WATCH=<hex guest VA>: print every GPU store landing on that word,
    // with a monotonic timestamp. The memory sibling of CZ_PM4_CONST_WATCH, added
    // for the phase C fence-pacing hunt: "who writes the fence writeback, and at
    // what cadence" is a question only the store site can answer.
    {
        static const char* w = getenv("CZ_PM4_MEM_WATCH");
        static const uint32_t watch = w ? uint32_t(strtoul(w, nullptr, 16)) : 0;
        if (watch && (va & ~3u) == (watch & ~3u))
        {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "[pm4] MEM_WATCH %08X <- %08X (t=%ld.%03ld)\n", va, value,
                    (long)ts.tv_sec, ts.tv_nsec / 1000000);
        }
    }
    // CZ_PM4_FENCE_MONOTONIC=1 — an EXPERIMENT, not a fix, and it announces itself.
    //
    // Phase C part 7 measured the draw arm's fence completion word on a nine-value
    // CAROUSEL: the command processor is re-executing an old segment, so the segment's
    // EVENT_WRITE packets rewrite the word with values it has already passed, and the
    // engine's next fence wait becomes unsatisfiable rather than merely slow. Refusing
    // a store that moves that one word BACKWARDS makes the wait satisfiable again, so
    // a run with this on says whether the carousel is what stops the boot — and a run
    // with it off is the same binary's control.
    //
    // It is deliberately not a candidate fix. Hardware re-executes stale EVENT_WRITEs
    // too; it gets away with it because it never walks the same token stream twice, and
    // a command processor that second-guesses a packet's value is not a faithful one.
    // The cure has to be upstream, at whatever puts the arm block inside a segment the
    // worker resubmits.
    if (g_fenceMonotonic && va == g_fenceWord.load(std::memory_order_relaxed))
    {
        const uint32_t current = GuestLoad32(base, va);
        // A signed difference, not `value < current`: fence numbers are free to wrap,
        // and a wrap must not look like a regression forever after.
        if (int32_t(value - current) < 0)
        {
            const uint64_t n = g_fenceRegressions.fetch_add(1) + 1;
            if (n <= 4 || (n & 0xFFFF) == 0)
                fprintf(stderr, "[pm4] CZ_PM4_FENCE_MONOTONIC: REFUSED fence store "
                                "%08X <- %08X (word holds %08X) #%llu — this is the "
                                "EXPERIMENT arm, not a fix\n",
                        va, value, current, (unsigned long long)n);
            return true; // reported as stored: the packet is not malformed
        }
    }
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
    {
        static std::atomic<uint64_t> dropped{ 0 };
        if (dropped.fetch_add(1) < 8)
            fprintf(stderr,
                    "[pm4] GPU store outside the physical arena DROPPED "
                    "(phys=%08X -> va=%08X value=%08X)\n",
                    physAddr, va, value);
        return false;
    }
    GuestStore32(base, va, value);
    return true;
}

// A packet's memory write: `addrDword` is `physical | endian code`.
void StoreGpu(uint8_t* base, uint32_t addrDword, uint32_t value, uint32_t indexDwords = 0)
{
    StoreGpuRaw(base, (addrDword & ~3u) + indexDwords * 4, GpuSwapResidual(value, addrDword));
}

uint32_t LoadGpu(const uint8_t* base, uint32_t addrDword)
{
    const uint32_t va = PhysToVa(addrDword & ~3u);
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
        return 0;
    return GpuSwapResidual(GuestLoad32(base, va), addrDword);
}

// --- the screen-extent query ------------------------------------------------------
//
// EVENT_WRITE_EXT with event 0x1A and a single address argument is the Xenos SCREEN
// EXTENT query: the GPU writes the extent of what it has rasterized since the
// matching EVENT_WRITE 0x19 to that address. This title issues one per draw block —
// 818,507 of them in the first 6 M packets of B1, always body=2 and always event
// 0x1A, against 819,953 EVENT_WRITE 0x19 — and OUR command processor did nothing with
// them, because the fence family's handler only stores when the packet carries a
// value dword (bodyCount >= 3) and this form does not.
//
// THAT IS WHY THE SCENE'S RIGHT TILE WAS EMPTY (phase C part 11). The extent is not
// bookkeeping: it is the input to the bin-mask fix-up pass. The chain, all of it in
// the guest and all of it measured:
//
//   * a draw block is `SET_BIN_MASK_LO FFFFFFFF ; DRAW_INDX ; SET_BIN_MASK_LO
//     80000000 ; EVENT_WRITE_EXT{0x1A, &record+4} ; EVENT_WRITE{0x19}`, and the
//     emitter (82842998 and its two twins) records `record[0] = <the stream cursor>`
//     so the first mask dword is at `record[0] + 8`;
//   * the record is SIXTEEN bytes and the extent is the other twelve — six halfwords
//     at record+4, in units of 8 pixels, `{minX, maxX, minY, maxY, minZ, maxZ}`. The
//     order is fixed by which dword each of sub_8284A7F8's four comparisons loads;
//   * the D3D worker's token dispatcher (sub_8284B228, token 0x89......) calls
//     sub_8284A900 -> sub_8284A7F8, which intersects each record's extent against the
//     tile rects at `workerObj+0x6C+8` and writes `bit31 | (3 per overlapping tile)`
//     back to `record[0] + 8`;
//   * the next tile pass replays the same buffer with the patched masks.
//
// With the extent never written, that intersection ran on uninitialised memory. Our
// probe measured the result directly: 76% of 388,451 patched records came back
// `80000000` — "touches no tile" — where hardware's are `8000000F`, "touches both".
//
// WHAT WE WRITE, AND WHY IT IS NOT THE REAL EXTENT. We do not rasterize on the
// command-processor thread and cannot know what a draw covered. The conservative
// answer is the only honest one: "this draw may have touched anything", i.e. an
// extent that overlaps every tile, which makes bin predication a no-op rather than a
// wrong filter. That is also what the capture measures hardware doing in practice —
// B1 discards 0.3% of its draw packets, and both tiles are offered exactly 575,744
// draws and keep 573,124 of them. A too-large extent can only cost work (a draw
// submitted to a tile it does not touch, where the scissor rejects it); a too-small
// one silently deletes geometry, which is the failure we are fixing.
//
// CZ_PM4_NO_SCREEN_EXTENT=1 restores the pre-part-11 behaviour on BOTH streams, so
// every claim about this change has a same-binary control arm.
constexpr uint16_t kExtentMax = 0x1FFF; // * 8 px = 65,536: larger than any tile rect

const bool g_noScreenExtent = [] {
    const char* v = getenv("CZ_PM4_NO_SCREEN_EXTENT");
    return v && *v && *v != '0';
}();

void WriteScreenExtent(uint8_t* base, uint32_t addrDword)
{
    if (g_noScreenExtent)
        return;
    // GPU-NATIVE dword order: the GPU is little-endian, so the FIRST halfword of a
    // pair sits in the LOW half of the dword it writes, and the address's endian code
    // (1 = 8-in-16 here) is what turns that into halfwords a `lhz` reads. StoreGpu
    // already applies exactly that conversion, so passing the pair the other way round
    // would transpose minX with maxX — a silent swap that reads as an empty rect.
    StoreGpu(base, addrDword, (uint32_t(kExtentMax) << 16) | 0u, 0); // minX, maxX
    StoreGpu(base, addrDword, (uint32_t(kExtentMax) << 16) | 0u, 1); // minY, maxY
    StoreGpu(base, addrDword, (uint32_t(1) << 16) | 0u, 2);          // minZ, maxZ
}

// --- register writes --------------------------------------------------------------
// CZ_PM4_CONST_WATCH=<hex>[-<hex>] — log writes to one register of the file, or to a
// range of them, with the value as both a dword and a float.
//
// The instrument for "this constant holds sensible data early in the frame and zero by
// the time the shader that needs it draws". A register file has exactly three writers
// here (SET_CONSTANT, SET_CONSTANT2, LOAD_ALU_CONSTANT) and nothing clears it, so a
// value that becomes zero was WRITTEN as zero — and the only way to find out by whom is
// to watch the register rather than reason about the packet stream.
//
// It also answers the opposite question, which is the one part 16 needed: a shader
// constant that is WRONG and a shader constant the guest never wrote in this era look
// identical from inside the shader, and only the writer's silence separates them. So
// the watch reports a per-era WRITE COUNT unconditionally — a count of zero over the
// era under investigation is the finding, and it is one no sampling of the value
// could ever produce.
//
// CZ_PM4_CONST_WATCH_FRAME=N holds the report until frame N, because the era that
// matters is never the boot (gotcha 139); CZ_PM4_CONST_WATCH_ZEROS=1 restricts it to
// zero writes, which is what this instrument did when it only had one job.
const char* g_constWatchEnv = getenv("CZ_PM4_CONST_WATCH");
uint32_t g_constWatchLo = 0xFFFFFFFFu;
uint32_t g_constWatchHi = 0xFFFFFFFFu;
const bool g_constWatchInit = []
{
    if (!g_constWatchEnv)
        return false;
    char* end = nullptr;
    g_constWatchLo = uint32_t(strtoul(g_constWatchEnv, &end, 16));
    g_constWatchHi = (end && *end == '-') ? uint32_t(strtoul(end + 1, nullptr, 16))
                                          : g_constWatchLo;
    return true;
}();
const uint32_t g_constWatchFrame = []
{
    const char* e = getenv("CZ_PM4_CONST_WATCH_FRAME");
    return e ? uint32_t(strtoul(e, nullptr, 10)) : 0u;
}();
const bool g_constWatchZerosOnly = getenv("CZ_PM4_CONST_WATCH_ZEROS") != nullptr;
const char* g_constWatchSource = "?";

// A HISTOGRAM, not a sample. The first version of this printed the first 32 writes
// and then every 4096th, and that is worse than useless on a range: the head is
// eaten by whichever register the guest happens to write first, so the registers
// further up the range print NOTHING and read as "never written", and the thinned
// tail then samples one lane in four and invites "every write is zero". Both
// mistakes were made here within one session, on the same instrument, after writing
// gotcha 109's warning into its own comment. What a watch on a shader constant is
// actually asked is "which values, how often, per register" — so it has to count
// them all and report the distribution.
std::mutex g_constWatchMutex;
std::map<uint32_t, std::map<uint32_t, uint64_t>> g_constWatchHist;
std::chrono::steady_clock::time_point g_constWatchNext{};

void ConstWatchRecord(uint32_t index, uint32_t value)
{
    if (Pm4_FrameCount() < g_constWatchFrame)
        return;
    if (g_constWatchZerosOnly && value != 0)
        return;

    std::lock_guard<std::mutex> lock(g_constWatchMutex);
    auto& perValue = g_constWatchHist[index];
    // Bound the distinct-value set so a register carrying per-draw data cannot grow
    // this without limit; the cap is announced rather than silent.
    if (perValue.size() < 24 || perValue.count(value))
        perValue[value]++;
    else
        perValue[0xFFFFFFFEu]++;   // the "other" bucket, printed as such

    const auto now = std::chrono::steady_clock::now();
    if (now < g_constWatchNext)
        return;
    g_constWatchNext = now + std::chrono::seconds(15);

    fprintf(stderr, "[constwatch] frame %llu — value histogram per register\n",
            (unsigned long long)Pm4_FrameCount());
    for (const auto& [reg, values] : g_constWatchHist)
    {
        const uint32_t alu = reg - xenos::kAluConstantBase;
        fprintf(stderr, "[constwatch]   reg %04X (pc%d.%c):", reg,
                int(alu / 4) - 256, "xyzw"[alu % 4]);
        for (const auto& [v, n] : values)
        {
            if (v == 0xFFFFFFFEu)
            {
                fprintf(stderr, "  <other> x%llu", (unsigned long long)n);
                continue;
            }
            float f;
            memcpy(&f, &v, 4);
            fprintf(stderr, "  %08X(%.4f) x%llu", v, double(f), (unsigned long long)n);
        }
        fprintf(stderr, "\n");
    }
}

void WriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegCount)
        return;
    if (index >= g_constWatchLo && index <= g_constWatchHi)
        ConstWatchRecord(index, value);
    g_regs[index] = value;

    // Scratch-register writeback: when SCRATCH_UMSK enables a scratch register, each
    // write to it is mirrored to SCRATCH_ADDR + reg*4. This is a real reporting
    // channel the guest reads, and it costs us nothing to honour because the values
    // come from the stream itself.
    //
    // StoreGpuRaw, NOT StoreGpu: SCRATCH_ADDR is a bare physical base with no endian
    // code in it, and running it through the packet-address path reads its low bits
    // as one. On the previous port that mistake byte-swapped every mirrored register,
    // and what it looked like was not an endian bug — it was WAIT_REG_MEM packets in
    // its own stream failing forever, because those packets poll this very block and
    // their reference values are the scratch registers' own contents.
    if (index >= kRegScratch0 && index <= kRegScratch7)
    {
        const uint32_t reg = index - kRegScratch0;
        if (g_regs[kRegScratchUmsk] & (1u << reg))
            StoreGpuRaw(base, g_regs[kRegScratchAddr] + reg * 4, value);
    }
}

bool EvalWaitCondition(uint32_t func, uint32_t value, uint32_t mask, uint32_t ref)
{
    const uint32_t v = value & mask;
    switch (func & 7)
    {
        case 0: return false; // never
        case 1: return v < ref;
        case 2: return v <= ref;
        case 3: return v == ref;
        case 4: return v != ref;
        case 5: return v >= ref;
        case 6: return v > ref;
        default: return true; // always
    }
}

// Execute one packet at `pos`. Returns the dwords consumed, or 0 when the packet
// claims more than `avail` — which at ring level means the driver has written the
// header but not yet the body, and the right answer is to come back next tick rather
// than to guess.
uint32_t ExecutePacket(uint8_t* base, const Source& fetch, uint32_t pos, uint32_t avail,
                       int depth)
{
    const uint32_t header = fetch(pos);
    const uint32_t type = header >> 30;
    g_packets.fetch_add(1, std::memory_order_relaxed);
    g_types[type].fetch_add(1, std::memory_order_relaxed);

    if (type == 2) // filler
        return 1;

    // An all-zero dword at ring level is memory the driver has not written yet.
    // Parsing it as a type-0 "write one register at index 0" would silently consume
    // two dwords of a packet still being written and desync the stream.
    if (header == 0 && depth == 0)
    {
        g_packets.fetch_sub(1, std::memory_order_relaxed);
        g_types[type].fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    // Below the ring, a zero dword falls through to the type-0 path and is read as
    // "write one register at index 0", consuming the dword after it as data.
    //
    // That looks wrong, and finding 38 spent a while believing it was: it would mean
    // every odd-length run of padding shifts the walk by one dword, which is exactly
    // the symptom the indirect-buffer truncations show. It is not wrong. Xenia's B1
    // capture records the true length of all 24,527,474 packets it executed, and
    // `tools/pm4_packet_lengths.py` says our arithmetic matches every one of them —
    // including its single zero-header packet, which hardware consumed as TWO dwords,
    // not one. B1 containing exactly one such packet in 24.5 M is the other half of
    // the story: a correctly aligned walk of this title's streams essentially never
    // meets a zero header, so the zeros our truncated walks trip over are data being
    // read by a walk that is already lost.
    //
    // CZ_PM4_ZERO_IS_NOP=1 selects the rejected reading, so the question can be
    // re-measured without a rebuild (gotcha 50 — a rate needs two arms of the same
    // binary). Measured at 10 runs a side: no difference in the stall rate, which is
    // what the capture predicts for a rule that only fires after the walk is lost.
    if (header == 0 && g_zeroIsNop)
        return 1;

    if (type == 1) // two register writes, 11-bit indices
    {
        if (avail < 3)
            return 0;
        g_constWatchSource = "TYPE1";
        WriteRegister(base, header & 0x7FF, fetch(pos + 1));
        WriteRegister(base, (header >> 11) & 0x7FF, fetch(pos + 2));
        return 3;
    }

    const uint32_t bodyCount = ((header >> 16) & 0x3FFF) + 1;
    if (avail < bodyCount + 1)
        return 0;

    if (type == 0) // a run of register writes
    {
        const uint32_t reg = header & 0x7FFF;
        const bool oneReg = (header >> 15) & 1;
        g_constWatchSource = oneReg ? "TYPE0(one-reg)" : "TYPE0(run)";
        for (uint32_t i = 0; i < bodyCount; i++)
            WriteRegister(base, oneReg ? reg : reg + i, fetch(pos + 1 + i));
        return bodyCount + 1;
    }

    // type 3
    const uint32_t opcode = (header >> 8) & 0x7F;
    g_opcodes[opcode].fetch_add(1, std::memory_order_relaxed);
    if (opcode == 0x22 || opcode == 0x36)
        g_draws.fetch_add(1, std::memory_order_relaxed);

    // An opcode this command processor does not have is reported, once each. B1's
    // census says there are exactly 21 and all are named, so any of these is either a
    // parser desync or a packet the captures never contained — both worth knowing
    // about, and neither worth guessing at.
    if (!kOpcodeNames.name[opcode])
    {
        static std::atomic<uint64_t> seen[128];
        if (seen[opcode].fetch_add(1) == 0)
            fprintf(stderr, "[pm4] UNKNOWN type-3 opcode 0x%02X (header %08X, %u body "
                            "dwords) at dword %u — B1's census says this title uses 21 "
                            "opcodes and this is not one of them\n",
                    opcode, header, bodyCount, pos);
    }

    // ME predication: header bit 0 marks a packet the hardware runs only when the
    // current bin mask and bin select overlap. Skipping the body wholesale is what
    // the ME does, for every type-3 opcode alike.
    //
    // CZ_PM4_NO_PREDICATION=1 executes predicated packets anyway — the same-binary arm
    // for every claim about what the bin mask is discarding. It is NOT a candidate fix:
    // running a packet hardware skipped draws a tile's geometry into another tile. It
    // exists because "this pass has 96 draws" and "this pass had 900 draws and 804 of
    // them were predicated away" are the same picture and different faults, and the
    // counters below are what tell them apart.
    static const bool noPredication = getenv("CZ_PM4_NO_PREDICATION") != nullptr;
    const bool predicated = (header & 1) && (g_binMask & g_binSelect) == 0;

    // CZ_PM4_BIN_TRACE=N — N lines of the predication story in stream ORDER: every
    // SET_BIN_MASK/SELECT packet with the value it carries and whether the ME skipped
    // it, and every draw with the pair the ME compared.
    //
    // The WRITES are in here, not just the draws, because the census over B1 says the
    // rule is right and the mask VALUE is wrong (0.3% of hardware's draw packets are
    // discarded against a third of ours), and "wrong value" is a question about order
    // and about which writes landed. A draw-only trace can only ever restate the
    // symptom. Note in particular that a mask write can itself be predicated away,
    // which is self-sustaining: skip the write that would restore the overlap and
    // every later packet in the pass is skipped too.
    //
    // CZ_PM4_BIN_TRACE_ARM=hex holds the trace until the bin select first takes that
    // value, because the interesting era is a quarter of a million packets into the
    // boot and a budget spent on the untiled prologue says nothing (gotcha 139).
    if (getenv("CZ_PM4_BIN_TRACE"))
    {
        static int left = atoi(getenv("CZ_PM4_BIN_TRACE"));
        static const char* armEnv = getenv("CZ_PM4_BIN_TRACE_ARM");
        static const char* armMaskEnv = getenv("CZ_PM4_BIN_TRACE_ARMMASK");
        static const uint64_t armSelect = armEnv ? strtoull(armEnv, nullptr, 16) : 0;
        static const uint64_t armMask = armMaskEnv ? strtoull(armMaskEnv, nullptr, 16) : 0;
        static bool armed = !armEnv && !armMaskEnv;
        if (!armed && ((armEnv && g_binSelect == armSelect) ||
                       (armMaskEnv && g_binMask == armMask)))
            armed = true;
        if (armed && left > 0)
        {
            const char* what = nullptr;
            switch (opcode)
            {
                case 0x50: what = "SET_BIN_MASK";    break;
                case 0x51: what = "SET_BIN_SELECT";  break;
                case 0x60: what = "MASK_LO";         break;
                case 0x61: what = "MASK_HI";         break;
                case 0x62: what = "SELECT_LO";       break;
                case 0x63: what = "SELECT_HI";       break;
                default: break;
            }
            if (what)
            {
                left--;
                fprintf(stderr, "[pm4bin] %-15s %08X%s\n", what, fetch(pos + 1),
                        predicated ? "   (SKIPPED)" : "");
            }
            else if (opcode == 0x22 || opcode == 0x36)
            {
                left--;
                const uint32_t tl = g_regs[xenos::kPaScWindowScissorTl];
                const uint32_t br = g_regs[xenos::kPaScWindowScissorBr];
                fprintf(stderr,
                        "[pm4bin]     DRAW pred=%u mask=%016llX select=%016llX "
                        "scissor=%u,%u..%u,%u -> %s\n",
                        header & 1, (unsigned long long)g_binMask,
                        (unsigned long long)g_binSelect,
                        tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF,
                        predicated ? "SKIP" : "run");
            }
        }
    }

    if (g_binCensus && (opcode == 0x22 || opcode == 0x36))
        BinCensusRecord(g_binMask, g_binSelect, predicated);

    if (predicated)
    {
        if (opcode == 0x22 || opcode == 0x36)
            g_drawsPredicatedOut.fetch_add(1, std::memory_order_relaxed);
        if (!noPredication)
            return bodyCount + 1;
    }

    auto body = [&](uint32_t i) { return fetch(pos + 1 + i); };

    switch (opcode)
    {
        case 0x10: // NOP
        case 0x23: // VIZ_QUERY
        case 0x26: // WAIT_FOR_IDLE — instantly true; we execute synchronously
        case 0x3B: // INVALIDATE_STATE
        case 0x48: // ME_INIT
        case 0x5E: // CONTEXT_UPDATE
            break;

        // THE DRAW. Counted above; here it is decoded and handed to the renderer.
        //
        // Both forms carry one VGT_DRAW_INITIATOR dword, at a different offset:
        // DRAW_INDX puts a viz-query id in front of it, DRAW_INDX_2 does not. The
        // initiator names the primitive type, the index count and where the indices
        // come from — DMA (a guest buffer whose address follows), immediate (inline in
        // the packet), or auto-index (no indices at all, the vertex shader indexes the
        // streams itself off its vertex id).
        case 0x22: // DRAW_INDX:   dword0 = viz query info, dword1 = VGT_DRAW_INITIATOR
        case 0x36: // DRAW_INDX_2: dword0 = VGT_DRAW_INITIATOR
        {
            if (!g_drawSink)
                break;
            const uint32_t initiatorAt = (opcode == 0x22) ? 1 : 0;
            if (bodyCount <= initiatorAt)
                break;
            const uint32_t init = body(initiatorAt);

            Pm4Draw d{};
            d.primType = init & 0x3F;
            const uint32_t sourceSelect = (init >> 6) & 3;
            d.index32 = ((init >> 11) & 1) != 0;
            d.indexCount = init >> 16;
            d.indexed = sourceSelect == 0; // 0 = DMA; 2 = auto-index; 1 = immediate

            // THE INDEX BUFFER'S ADDRESS AND ITS SWIZZLE ARE IN DIFFERENT DWORDS, and
            // getting that wrong was this renderer's exploded geometry.
            //
            // Most addresses in a PM4 stream carry their endian swizzle in the low two
            // bits, and DRAW_INDX is the exception: `dword[n+1]` is a bare physical
            // address and `dword[n+2]` is `endian:2 | 0 | count:24`. Reading the
            // swizzle off the address instead cost TWO defects in one line —
            //
            //   * the swizzle came out as 0 or 2 depending on nothing but alignment,
            //     when the packet says **1 (8-in-16) on every draw in this title**,
            //     which is the only swizzle that makes sense for a big-endian 16-bit
            //     index buffer; and
            //   * masking the low two bits off the address MOVED IT. 16-bit index
            //     buffers are 2-byte aligned, so bit 1 is a real address bit — about
            //     40% of this title's draws have it set, and every one of those was
            //     read one index early.
            //
            // The count is in both dwords (`init >> 16` and `size & 0xFFFFFF`) and
            // they agree on every draw of a boot, which is the packet's own check that
            // this reading of the size dword is the right one.
            if (d.indexed && bodyCount > initiatorAt + 2)
            {
                const uint32_t addrDword = body(initiatorAt + 1);
                const uint32_t sizeDword = body(initiatorAt + 2);
                // CZ_PM4_INDEX_ADDR_SWIZZLE=1 restores the old reading — the
                // same-binary control arm for the fix.
                static const bool oldReading = getenv("CZ_PM4_INDEX_ADDR_SWIZZLE") != nullptr;
                d.indexEndian = oldReading ? (addrDword & 3) : (sizeDword >> 30);
                d.indexVa = PhysToVa(oldReading ? (addrDword & ~3u) : addrDword);
                d.indexEndianTop = sizeDword >> 30;
                d.indexSizeDword = sizeDword;
                // The packet states the index count TWICE, and the two readings
                // agreeing is the standing proof that this decode of the size dword is
                // the right one. Silent on a healthy stream; the moment it is not, the
                // layout assumption above has stopped holding and the geometry that
                // follows is not to be trusted.
                if ((sizeDword & 0xFFFFFFu) != d.indexCount)
                {
                    static uint32_t complained = 0;
                    if (complained++ < 8)
                        fprintf(stderr,
                                "[pm4] DRAW_INDX index count disagrees: init>>16=%u "
                                "size&0xFFFFFF=%u (size=%08X) — the size dword's "
                                "layout is not endian:2|count:24 as assumed\n",
                                d.indexCount, sizeDword & 0xFFFFFFu, sizeDword);
                }
                // CZ_PM4_DRAW_TRACE=1 — the raw DRAW_INDX body for the first few
                // draws. Which dword carries the index swizzle is the whole question
                // (low two bits of the ADDRESS, as every other address in this stream
                // does it, or the top two bits of the SIZE dword), and only the packet
                // can answer it.
                static int left = getenv("CZ_PM4_DRAW_TRACE") ? 24 : 0;
                if (left-- > 0)
                    fprintf(stderr,
                            "[pm4draw] init=%08X prim=%u count=%u i32=%u addr=%08X "
                            "size=%08X  addr&3=%u  size>>30=%u  size&0xFFFFFF=%u\n",
                            init, d.primType, d.indexCount, d.index32 ? 1u : 0u,
                            addrDword, sizeDword, addrDword & 3, sizeDword >> 30,
                            sizeDword & 0xFFFFFF);
            }
            else if (d.indexed)
            {
                d.indexed = false; // a DMA draw with no address is not one we can honour
            }
            g_drawSink(base, d);
            break;
        }

        case 0x60: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x61: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;
        case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;

        case 0x3D: // MEM_WRITE: addr(+endian), then the dwords to write
            for (uint32_t i = 1; i < bodyCount; i++)
                StoreGpu(base, body(0), body(i), i - 1);
            break;

        case 0x3E: // REG_TO_MEM: reg, addr(+endian)
            if (bodyCount >= 2)
            {
                const uint32_t reg = body(0) & 0x7FFF;
                StoreGpu(base, body(1), reg < kRegCount ? g_regs[reg] : 0);
            }
            break;

        case 0x21: // REG_RMW: rmw_info, and_mask, or_mask (info bits 31/30: masks are regs)
            if (bodyCount >= 3)
            {
                const uint32_t info = body(0), andM = body(1), orM = body(2);
                const uint32_t dst = info & 0x1FFF;
                uint32_t v = dst < kRegCount ? g_regs[dst] : 0;
                v &= ((info >> 31) & 1) ? g_regs[andM & 0x1FFF] : andM;
                v |= ((info >> 30) & 1) ? g_regs[orM & 0x1FFF] : orM;
                WriteRegister(base, dst, v);
            }
            break;

        // THE FENCES. This is the packet family the whole module turns on: the
        // driver's ring-progress waits poll words that nothing but these ever write,
        // and every value comes out of the stream rather than out of us. Case Zero
        // issues 2.35 million of them across B1 (EVENT_WRITE 1,203,473 +
        // EVENT_WRITE_EXT 1,141,008 + EVENT_WRITE_SHD 8,026).
        //
        // Fable 2 suppressed the bit-31 form of these. That is exactly the kind of
        // thing not to inherit: it was a workaround for its fences landing in an
        // o1heap-managed physical arena, which this runtime deliberately does not use
        // for physical memory (kernel/heap.h explains why).
        case 0x46: // EVENT_WRITE
        case 0x58: // EVENT_WRITE_SHD  (VS/PS-done fence)
        case 0x59: // EVENT_WRITE_CFL  (cache-flush-done fence)
            WriteRegister(base, 0x21F9 /* VGT_EVENT_INITIATOR */, body(0) & 0x3F);
            if (bodyCount >= 3)
                StoreGpu(base, body(1), body(2));
            break;

        // EVENT_WRITE_EXT — the SCREEN EXTENT QUERY, and the whole of phase C part 11.
        case 0x5A:
            WriteRegister(base, 0x21F9 /* VGT_EVENT_INITIATOR */, body(0) & 0x3F);
            if (bodyCount >= 3)
                StoreGpu(base, body(1), body(2));
            else if (bodyCount >= 2)
                WriteScreenExtent(base, body(1));
            break;

        case 0x3C: // WAIT_REG_MEM: wait_info, addr/reg(+endian), ref, mask, [interval]
        {
            if (bodyCount < 4)
                break;
            const uint32_t info = body(0), poll = body(1), ref = body(2), mask = body(3);
            const bool isMemory = (info & 0x10) != 0;
            // Never SPUN on, whatever the brake says. Everything this executor can
            // satisfy it has already satisfied by the time it gets here, because we
            // execute the stream strictly in order and synchronously; and a wait that
            // only a guest thread can satisfy must not be blocked on inside this call,
            // because that guest thread is very likely the one waiting for us to make
            // progress. Holding means returning and retrying next tick — see below.
            uint32_t value;
            if (isMemory)
            {
                value = LoadGpu(base, poll);
            }
            else
            {
                const uint32_t reg = poll & 0x7FFF;
                // Coherency requests complete instantly on a GPU with no caches: the
                // driver sets bit 31 (pending) and waits for it to clear.
                if (reg == kRegCoherStatusHost)
                    g_regs[reg] &= ~0x80000000u;
                value = reg < kRegCount ? g_regs[reg] : 0;
            }
            if (!EvalWaitCondition(info, value, mask, ref))
            {
                const uint64_t n = g_waitUnmet.fetch_add(1) + 1;
                if (n <= 8 || (n & 0xFFFF) == 0)
                    fprintf(stderr,
                            "[pm4] WAIT_REG_MEM #%llu not satisfied, %s: %s %08X "
                            "value=%08X mask=%08X ref=%08X func=%u\n",
                            (unsigned long long)n, g_stopOnWait ? "STOPPING" : "continuing",
                            isMemory ? "mem" : "reg", poll, value, mask, ref, info & 7);

                // The brake, on by default since part 6 (see g_stopOnWait for the 40
                // runs behind that). CZ_PM4_NO_STOP_ON_WAIT=1 is the control arm.
                //
                // Real hardware STALLS the command processor here until the condition
                // holds; we evaluate once and carry on, which is how our CP gets
                // ahead of the CPU and found the scratch mirror poisoned at a later
                // INTERRUPT — the race whose guard part 12 deleted, because the brake
                // below is what closed it. Stopping the ring
                // walk at this packet — not spinning inside it, which would deadlock
                // against the very guest thread that has to satisfy the wait, but
                // returning and retrying next tick — is what the console does.
                //
                // The risk this carried while it was off: a condition only ever
                // satisfied by work appearing LATER in the same stream parks the ring
                // permanently. It is no longer a hypothetical either way — the hold
                // streak in the ring trace counts consecutive ticks on one wait, and
                // it reads 1 (control arm) and 2 (draw arm) against 5,491 for a ring
                // deliberately parked with CZ_ISR_SINGLE_CPU=1. Both stall sites also
                // stay loud: Pm4_Execute reports a frozen cursor after 60 ticks.
                //
                // It stalls at ANY depth now (see StallPlan above). The `depth == 0`
                // this used to carry made the flag unable to affect a single one of
                // this title's hand-off waits, all of which sit inside indirect
                // buffers — so both of its retirements measured a no-op.
                if (g_stopOnWait)
                {
                    if (depth > 0 && depth < 9)
                    {
                        g_stallNext.va[depth] = fetch.va;
                        g_stallNext.pos[depth] = pos;
                    }
                    g_stallHit = true;
                    return 0;
                }
            }
            break;
        }

        case 0x45: // COND_WRITE: wait_info, poll addr/reg, ref, mask, write addr/reg, data
        {
            if (bodyCount < 6)
                break;
            const uint32_t info = body(0), poll = body(1), ref = body(2), mask = body(3);
            const uint32_t value =
                (info & 0x10) ? LoadGpu(base, poll)
                              : ((poll & 0x7FFF) < kRegCount ? g_regs[poll & 0x7FFF] : 0);
            if (EvalWaitCondition(info, value, mask, ref))
            {
                if (info & 0x100)
                    StoreGpu(base, body(4), body(5));
                else
                    WriteRegister(base, body(4) & 0x7FFF, body(5));
            }
            break;
        }

        case 0x54: // INTERRUPT: delivered HERE rather than after the walk — see pm4.h.
            g_interrupts.fetch_add(1, std::memory_order_relaxed);
            if (g_interruptSink)
                g_interruptSink();
            break;

        case 0x64: // XE_SWAP: the frame boundary, exactly one per frame in B1
                   // (1,089 swaps over 1,089 frames), and the runtime's only frame
                   // clock — VdSwap is kHighFrequency and appears in no kernel log.
        {
            g_frames.fetch_add(1, std::memory_order_relaxed);

            // The present seam (phase 3). The body is what gpu/vd.cpp's VdSwap wrote:
            // 'SWAP', front buffer, width, height — so the descriptor the host window
            // presents comes from the GUEST's own swap call, routed through the
            // command stream exactly as hardware would receive it, rather than from a
            // side channel out of VdSwap.
            //
            // That routing is not ceremony. It is what makes the frame count in the
            // window title the same number the ring trace prints, and it means a
            // present can only happen at a point the command processor actually
            // reached — which is the property findings 38-39 were about.
            if (bodyCount >= 4 && body(0) == 0x53574150 /* 'SWAP' */)
            {
                // The renderer first: it turns the frame it has recorded into pixels
                // and publishes them, and Host_Present is the signal that a frame
                // boundary happened. Reversing the two would present the PREVIOUS
                // frame's pixels with this frame's descriptor once per swap — a
                // one-frame lag that is invisible in a still and looks like input lag
                // in motion.
                VkRenderer_OnSwap(base, body(1), body(2), body(3));
                Host_Present(body(1), body(2), body(3));
                // The deterministic-clock instrument steps here, at the guest's own
                // frame boundary, so the clock and the picture advance together by
                // construction rather than by two schedules that have to agree.
                // A no-op unless CZ_DETERMINISTIC_CLOCK is set.
                cz_timebase::AdvanceFrame();
            }
            break;
        }

        case 0x2D: // SET_CONSTANT: offset_type, then data
        {
            uint32_t index = body(0) & 0x7FF;
            switch ((body(0) >> 16) & 0xFF)
            {
                case 0: index += 0x4000; break; // ALU constants
                case 1: index += 0x4800; break; // fetch constants
                case 2: index += 0x4900; break; // booleans
                case 3: index += 0x4908; break; // loops
                case 4: index += 0x2000; break; // registers
                default: index = kRegCount;     // unknown bank: drop
            }
            g_constWatchSource = "SET_CONSTANT";
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, index + i - 1, body(i));
            break;
        }

        case 0x55: // SET_CONSTANT2 / SET_SHADER_CONSTANTS: absolute index in body(0)
        case 0x56:
            g_constWatchSource = "SET_CONSTANT2";
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, (body(0) & 0xFFFF) + i - 1, body(i));
            break;

        case 0x2F: // LOAD_ALU_CONSTANT: addr, offset_type, size — constants from memory
        {
            if (bodyCount < 3)
                break;
            uint32_t index = body(1) & 0x7FF;
            switch ((body(1) >> 16) & 0xFF)
            {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = kRegCount;
            }
            const uint32_t addr = PhysToVa(body(0) & 0x3FFFFFFC);
            const uint32_t size = body(2) & 0xFFF;
            g_constWatchSource = "LOAD_ALU_CONSTANT";
            for (uint32_t i = 0; i < size; i++)
                WriteRegister(base, index + i, GuestLoad32(base, addr + i * 4));
            break;
        }

        // Shader uploads. Case Zero issues 836,994 IM_LOAD and 204,151
        // IM_LOAD_IMMEDIATE in B1, so a length bug here would be immediate and total —
        // IM_LOAD_IMMEDIATE carries its microcode inline, and mis-reading its length
        // desyncs the whole stream rather than just losing a shader.
        case 0x27: // IM_LOAD: addr_type (low 2 bits = stage), start_size
            if (bodyCount >= 2 && (body(0) & 3) < 2)
            {
                const uint32_t type = body(0) & 3;
                const uint32_t va = PhysToVa(body(0) & ~3u);
                const uint32_t size = body(1) & 0xFFFF;
                // Snapshot before hashing. The driver keeps writing its upload
                // staging area while our executor works through the ring, so reading
                // the target twice — once to hash, once to translate — can see two
                // different shaders and produce a name that matches neither.
                if (size && size <= 0x10000)
                {
                    std::vector<uint8_t> code(size_t(size) * 4);
                    memcpy(code.data(), base + va, code.size());
                    BindShader(type, va, code.data(), size);
                }
            }
            break;

        case 0x2B: // IM_LOAD_IMMEDIATE: type, start_size, then the code
            if (bodyCount >= 2 && (body(0) & 3) < 2)
            {
                const uint32_t type = body(0) & 3;
                const uint32_t size = body(1) & 0xFFFF;
                if (size && size + 2 <= bodyCount)
                {
                    // Written back out big-endian on purpose: `body()` has already
                    // swapped, and the hash must be over the bytes as the guest holds
                    // them or this path names a shader differently from the pointer
                    // path above (see Fnv1a's comment).
                    std::vector<uint8_t> code(size_t(size) * 4);
                    for (uint32_t k = 0; k < size; k++)
                    {
                        const uint32_t w = body(2 + k);
                        code[k * 4 + 0] = uint8_t(w >> 24);
                        code[k * 4 + 1] = uint8_t(w >> 16);
                        code[k * 4 + 2] = uint8_t(w >> 8);
                        code[k * 4 + 3] = uint8_t(w);
                    }
                    BindShader(type, 0, code.data(), size);
                }
            }
            break;

        case 0x37: // INDIRECT_BUFFER_PFD
        case 0x3F: // INDIRECT_BUFFER: physical address, size in dwords
            if (bodyCount >= 2 && depth < 8)
            {
                const uint32_t addr = body(0);
                const uint32_t size = body(1) & 0xFFFFF;
                if (getenv("CZ_PM4_IB_TRACE"))
                {
                    static std::atomic<uint64_t> n{ 0 };
                    if (n.fetch_add(1) < 64)
                        fprintf(stderr,
                                "[pm4] INDIRECT_BUFFER header=%08X body0=%08X body1=%08X "
                                "-> addr=%08X size=%u (bodyCount=%u)\n",
                                header, body(0), body(1), addr, size, bodyCount);
                }
                if (size && (addr & 0x1FFFFFFFu) + uint64_t(size) * 4 <= 0x20000000ull)
                {
                    if (g_ibVerify)
                        ExecuteLinearVerified(base, PhysToVa(addr), size, depth + 1);
                    else
                        ExecuteLinear(base, PhysToVa(addr), size, depth + 1);

                    // A stall inside the buffer has to unwind all the way to the ring
                    // cursor, or the enclosing walk would carry on past a packet the
                    // GPU has not reached yet. Recording this level's position too is
                    // what lets the next tick re-enter this exact INDIRECT_BUFFER and
                    // then resume inside it, rather than replaying the packets before
                    // it (see StallPlan).
                    if (g_stallHit)
                    {
                        if (depth > 0 && depth < 9)
                        {
                            g_stallNext.va[depth] = fetch.va;
                            g_stallNext.pos[depth] = pos;
                        }
                        return 0;
                    }
                }
                else
                {
                    // Same reasoning as the truncation report above: a buffer we
                    // decline to walk takes its trailing fence with it.
                    static std::atomic<uint64_t> skipped{ 0 };
                    const uint64_t n = skipped.fetch_add(1);
                    if (n < 16 || (n & 0xFFFu) == 0)
                        fprintf(stderr,
                                "[pm4] indirect buffer SKIPPED (addr=%08X size=%u, "
                                "occurrence %llu)\n",
                                addr, size, static_cast<unsigned long long>(n));
                }
            }
            break;

        default: // counted in the census above; the body is skipped safely
            break;
    }

    return bodyCount + 1;
}

uint32_t ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth)
{
    Source fetch{ base, va, 0 };
    uint32_t pos = 0;
    // Resume where a stall in a previous tick left this buffer. Keyed on the buffer's
    // own address so an unrelated buffer arriving at the same depth is not skipped
    // into; the entry is consumed once, because the plan is rebuilt each tick.
    if (g_stallPlan.pending && depth > 0 && depth < 9 && g_stallPlan.va[depth] == va &&
        g_stallPlan.pos[depth] < sizeDwords)
    {
        pos = g_stallPlan.pos[depth];
        g_stallPlan.va[depth] = 0;
    }
    // The last few packets walked, so a truncation can name the packet whose LENGTH
    // was wrong rather than the innocent dword it eventually tripped over. A stopped
    // walk always reports the position where the arithmetic ran out, which is data by
    // then — one of the headers this caught was 3F800000, the float 1.0 — so without
    // the trail every report points at the wrong packet.
    struct Step
    {
        uint32_t pos, header, consumed;
    };
    Step trail[8] = {};
    uint32_t steps = 0;
    while (pos < sizeDwords)
    {
        const uint32_t header = fetch(pos);
        const uint32_t consumed = ExecutePacket(base, fetch, pos, sizeDwords - pos, depth);
        trail[steps % 8] = { pos, header, consumed };
        steps++;
        if (!consumed)
        {
            // A DELIBERATE stall is not a truncation, and counting it as one would put
            // the load-stall alarm (`truncated=`) permanently at a nonzero value and
            // retire the one live gate finding 39 left behind.
            if (g_stallHit)
                return pos;

            // A packet claiming more dwords than the buffer holds. Stopping is right —
            // guessing would desync — but stopping SILENTLY is not, and that is what
            // this did until finding 38.
            //
            // The driver's own progress fence is the LAST packet in these buffers, so
            // anything that cuts a buffer short drops the fence with it. The read
            // pointer the driver polls then stops advancing while our parser reports
            // itself fully caught up, and the next thread that waits on that fence
            // waits for the rest of the process's life. A truncation here is therefore
            // not a cosmetic parse issue; it is a hang, and it deserves to say so.
            const uint64_t n = g_ibTruncated.fetch_add(1);
            if (n < 16 || (n & 0xFFFu) == 0)
            {
                fprintf(stderr,
                        "[pm4] indirect buffer TRUNCATED at dword %u of %u (va=%08X, "
                        "header=%08X, depth=%d, occurrence %llu) — every packet after "
                        "this one is lost, including any trailing fence\n",
                        pos, sizeDwords, va, fetch(pos), depth,
                        static_cast<unsigned long long>(n));
                const uint32_t shown = steps < 8 ? steps : 8;
                for (uint32_t i = shown; i-- > 0;)
                {
                    const Step& s = trail[(steps - 1 - i) % 8];
                    fprintf(stderr, "[pm4]   -%u: dword %u header %08X type %u op %02X "
                                    "count %u -> consumed %u\n",
                            i, s.pos, s.header, s.header >> 30, (s.header >> 8) & 0x7F,
                            ((s.header >> 16) & 0x3FFF) + 1, s.consumed);
                }
                // And the raw tail, because the question a truncation raises is what
                // the dwords we could not parse actually ARE. Capped: hardware's own
                // indirect buffers reach 65,522 dwords, so an early truncation in a
                // big one would otherwise dump 8,000 lines and bury the report it
                // belongs to. Use CZ_PM4_DUMP_TRUNCATED for the whole thing.
                const uint32_t tailEnd = std::min(sizeDwords, pos + 64);
                for (uint32_t i = pos; i < tailEnd; i++)
                    fprintf(stderr, "%s%08X", ((i - pos) % 8) ? " " : "\n[pm4]   tail ",
                            fetch(i));
                fprintf(stderr, "%s\n", tailEnd < sizeDwords ? " ..." : "");

                // CZ_PM4_DUMP_TRUNCATED=<path>: the whole buffer, once. The trail above
                // reaches a few packets back; the dword that was actually mis-sized can
                // be thousands of packets earlier, and only a full re-walk offline can
                // find it.
                if (const char* path = getenv("CZ_PM4_DUMP_TRUNCATED"))
                {
                    static std::atomic<uint32_t> dumps{ 0 };
                    const uint32_t index = dumps.fetch_add(1);
                    if (index < 6)
                    {
                        char name[512];
                        snprintf(name, sizeof(name), "%s.%u.%u.bin", path, index, sizeDwords);
                        if (FILE* f = fopen(name, "wb"))
                        {
                            for (uint32_t i = 0; i < sizeDwords; i++)
                            {
                                const uint32_t w = __builtin_bswap32(fetch(i));
                                fwrite(&w, 4, 1, f);
                            }
                            fclose(f);
                            fprintf(stderr, "[pm4] dumped %u dwords to %s\n", sizeDwords, name);
                        }
                    }
                }
            }
            break;
        }
        pos += consumed;
    }
    return pos;
}

// The instrument that decides finding 38's last open question.
//
// By the end of task #16 everything about our PARSER had been checked against the B1
// capture and cleared: each packet's length against the boundary hardware used on all
// 24,527,474 of them (tools/pm4_packet_lengths.py), and — the check that actually
// covers a walk rather than a packet — every indirect buffer's start address and every
// internal packet boundary, chained from the buffer's first dword, on all 28,726 of
// them (tools/pm4_indirect_walks.py). Both clean. Yet our walks of those same buffers
// still ended early at run time, on dwords that are plainly data (one header we tripped
// over was 3F800000, the float 1.0).
//
// If the arithmetic is right and the result is wrong, the input is wrong: the bytes we
// walk are not the bytes the driver wrote. This asks that directly — copy the buffer,
// walk it exactly as we normally would, then compare the copy against guest memory
// afterwards and report the first dword that moved. A difference means the guest is
// rewriting a command buffer we are still reading, and the fix is about WHEN we consume
// the ring, not how we parse it.
//
// Off by default and deliberately expensive-when-on: it doubles the reads over every
// buffer. Gotcha 7 says a probe that slows the game manufactures its own result, and
// the asymmetry here is worth stating, because it decides how each outcome may be read:
// slowing the parser down makes a concurrent write MORE likely, so "differences found"
// is suggestive while "no differences found" — the parser dawdling and still never
// being overtaken — is the strong result.
void ExecuteLinearVerified(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth)
{
    // One reusable buffer per depth. The pump is the only thread that walks, so these
    // need no locking; allocating per buffer would add malloc traffic to a measurement
    // whose whole subject is timing.
    static std::vector<uint32_t> snapshots[8];
    std::vector<uint32_t>& snapshot = snapshots[depth & 7];
    snapshot.resize(sizeDwords);
    memcpy(snapshot.data(), base + va, size_t(sizeDwords) * 4);

    const uint32_t stopped = ExecuteLinear(base, va, sizeDwords, depth);

    if (memcmp(snapshot.data(), base + va, size_t(sizeDwords) * 4) == 0)
    {
        g_ibVerifyClean.fetch_add(1);
        return;
    }

    const uint64_t n = g_ibVerifyDirty.fetch_add(1);
    if (n >= 16 && (n & 0xFFu) != 0)
        return;

    uint32_t first = 0, changed = 0;
    bool haveFirst = false;
    for (uint32_t i = 0; i < sizeDwords; i++)
    {
        uint32_t live;
        memcpy(&live, base + va + i * 4, 4);
        if (live == snapshot[i])
            continue;
        changed++;
        if (!haveFirst)
        {
            first = i;
            haveFirst = true;
        }
    }

    // The relationship between the two positions is the whole point of printing both.
    // A write BEFORE the truncation is a candidate cause of it; a write only after it
    // is the guest legitimately reusing memory we had already stopped reading.
    fprintf(stderr,
            "[pm4] indirect buffer CHANGED UNDER US (va=%08X size=%u depth=%d): %u dwords "
            "differ, first at %u; our walk stopped at %u (%s) — occurrence %llu\n",
            va, sizeDwords, depth, changed, first, stopped,
            stopped < sizeDwords
                ? (first <= stopped ? "TRUNCATED, write is at or before the stop"
                                    : "TRUNCATED, write is after the stop")
                : "walk completed",
            static_cast<unsigned long long>(n));
    fprintf(stderr, "[pm4]   dword %u: was %08X now %08X\n", first,
            __builtin_bswap32(snapshot[first]), GuestLoad32(base, va + first * 4));
}

} // namespace

// --- public interface ---------------------------------------------------------------

void Pm4_SetRingBuffer(uint32_t base, uint32_t sizeBytes)
{
    g_ringBase = base;
    g_ringDwords = sizeBytes / 4;
    g_cursor = 0;
}

bool Pm4_RingInitialized() { return g_ringBase != 0 && g_ringDwords != 0; }

uint32_t Pm4_Execute(uint8_t* base, uint32_t writePtr)
{
    if (!Pm4_RingInitialized())
        return writePtr;

    // The kicked write pointer counts linearly and never wraps; the cursor is
    // ring-relative. Both must be brought into the same space before anything is
    // compared, or the available-dword arithmetic starves the moment the ring
    // completes its first lap.
    const uint32_t target = writePtr % g_ringDwords;
    if (g_cursor == target)
        return g_cursor;

    Source fetch{ base, g_ringBase, g_ringDwords };

    // One tick = one attempt at the plan the previous tick left behind. Rebuilt from
    // scratch each tick so a stall that has cleared does not leave a stale resume
    // point behind for a recycled command buffer to land on.
    // Snapshotted, not read at the end: the resume path CONSUMES its entry (it zeroes
    // g_stallPlan.va[depth] once it has re-entered that buffer), so a comparison made
    // after the walk sees a cleared slot and scores a release on every tick — including
    // the parked ones, which is the one case this counter exists to name.
    const StallPlan prevPlan = g_stallPlan;
    const bool wasHeld = prevPlan.pending;
    const uint32_t cursorAtEntry = g_cursor; // "did the ring move at all this tick"
    g_stallHit = false;
    g_stallNext = StallPlan{};

    uint32_t guard = g_ringDwords + 1; // never walk more than one lap per call
    while (g_cursor != target && guard--)
    {
        const uint32_t avail = (target + g_ringDwords - g_cursor) % g_ringDwords;
        const uint32_t consumed = ExecutePacket(base, fetch, g_cursor, avail, 0);
        if (!consumed)
            break; // tail packet not fully written yet, or a deliberate wait stall —
                   // either way the cursor stays put and we come back next tick
        g_cursor = (g_cursor + consumed) % g_ringDwords;
    }

    g_stallNext.pending = g_stallHit;
    // The consecutive-hold streak: how many ticks in a row the ring has sat on ONE
    // wait. Progress of any kind — the cursor moved, or the stall moved to a different
    // packet — ends the streak. See the counters above for why this and not a release
    // count.
    if (g_stallHit)
    {
        bool sameWait = wasHeld && g_cursor == cursorAtEntry;
        if (sameWait)
            for (int d = 1; d < 9; ++d)
                if (g_stallNext.va[d] != prevPlan.va[d] || g_stallNext.pos[d] != prevPlan.pos[d])
                {
                    sameWait = false;
                    break;
                }
        const uint64_t streak = sameWait ? g_holdStreak.load() + 1 : 1;
        g_holdStreak.store(streak);
        if (streak > g_holdStreakMax.load())
            g_holdStreakMax.store(streak);
    }
    else
        g_holdStreak.store(0);
    g_stallPlan = g_stallNext; // AFTER the comparison above, which reads the old plan
    if (g_stallHit)
    {
        const uint64_t n = g_ringHeld.fetch_add(1) + 1;
        if (n <= 4 || (n & 0xFFFFF) == 0)
            fprintf(stderr, "[pm4] CZ_PM4_STOP_ON_WAIT: ring held at an unsatisfied "
                            "WAIT_REG_MEM (#%llu), resuming next tick\n",
                    (unsigned long long)n);
    }

    // A stall here is reported, not papered over. Skipping ahead to the write pointer
    // would make the boot proceed while telling the driver we consumed packets we
    // never parsed — after which it is entitled to recycle ring memory we are still
    // reading, and the resulting corruption looks nothing like its cause.
    // CZ_PM4_RESYNC=1 enables a forward scan to the next plausible packet header for
    // when that trade is worth making deliberately; it is off by default precisely so
    // a parser bug shows up as a stall rather than as mystery garbage.
    static uint32_t s_lastCursor = ~0u;
    static uint32_t s_staleTicks = 0;
    if (g_cursor == s_lastCursor && g_cursor != target)
    {
        if (++s_staleTicks >= 60)
        {
            static int dumps = 0;
            if (dumps++ < 4)
            {
                fprintf(stderr, "[pm4] STALLED at dword %u of %u (wptr %u); ring:", g_cursor,
                        g_ringDwords, target);
                for (uint32_t i = 0; i < 16; i++)
                    fprintf(stderr, "%s%08X", (i % 8) ? " " : "\n[pm4]   ",
                            fetch(g_cursor + i));
                fprintf(stderr, "\n");
            }
            if (getenv("CZ_PM4_RESYNC"))
            {
                const uint32_t avail = (target + g_ringDwords - g_cursor) % g_ringDwords;
                for (uint32_t k = 1; k < avail && k <= 64; k++)
                {
                    const uint32_t h = fetch(g_cursor + k);
                    const uint32_t cnt = ((h >> 16) & 0x3FFF) + 1;
                    // Type-3 with a known opcode only: a type-0 match is far too easy
                    // to fake — an ordinary body dword parses as one — and a wrong
                    // resync eats real packets.
                    if ((h >> 30) == 3 && kOpcodeNames.name[(h >> 8) & 0x7F] &&
                        cnt + 1 <= avail - k)
                    {
                        fprintf(stderr, "[pm4] resynced +%u dwords past the stall\n", k);
                        g_cursor = (g_cursor + k) % g_ringDwords;
                        break;
                    }
                }
            }
            s_staleTicks = 0;
        }
    }
    else
    {
        s_staleTicks = 0;
    }
    s_lastCursor = g_cursor;

    return g_cursor;
}

void Pm4_SetInterruptSink(void (*sink)()) { g_interruptSink = sink; }
void Pm4_SetDrawSink(void (*sink)(uint8_t*, const Pm4Draw&)) { g_drawSink = sink; }

const Pm4ShaderBinding& Pm4_BoundShader(uint32_t stage)
{
    static const Pm4ShaderBinding kNone;
    return stage < 2 ? g_boundShaders[stage] : kNone;
}

const uint32_t* Pm4_Registers() { return g_regs; }

uint32_t Pm4_Cursor() { return g_cursor; }
uint32_t Pm4_ScratchAddr() { return g_regs[kRegScratchAddr]; }
uint32_t Pm4_ScratchUmsk() { return g_regs[kRegScratchUmsk]; }

uint64_t Pm4_IbTruncatedCount() { return g_ibTruncated.load(); }
uint64_t Pm4_IbVerifyCleanCount() { return g_ibVerifyClean.load(); }
uint64_t Pm4_IbVerifyDirtyCount() { return g_ibVerifyDirty.load(); }

uint64_t Pm4_WaitUnmetCount() { return g_waitUnmet.load(); }
uint64_t Pm4_RingHeldCount() { return g_ringHeld.load(); }
uint64_t Pm4_HoldStreak() { return g_holdStreak.load(); }
uint64_t Pm4_HoldStreakMax() { return g_holdStreakMax.load(); }

// Published by gpu/vd.cpp each pump tick — see g_fenceWord. A store rather than a
// read-back because the guest can re-register the writeback block, and a stale address
// here would make the experiment arm silently watch nothing.
void Pm4_SetFenceWord(uint32_t va) { g_fenceWord.store(va, std::memory_order_relaxed); }
uint64_t Pm4_FenceRegressionCount() { return g_fenceRegressions.load(); }

uint64_t Pm4_PacketCount() { return g_packets.load(); }
uint64_t Pm4_TypeCount(uint32_t type) { return type < 4 ? g_types[type].load() : 0; }
uint64_t Pm4_OpcodeCount(uint32_t opcode) { return opcode < 128 ? g_opcodes[opcode].load() : 0; }
uint64_t Pm4_DrawCount() { return g_draws.load(); }
uint64_t Pm4_DrawsPredicatedOut() { return g_drawsPredicatedOut.load(); }

// The (mask, select) pair table, in the same shape `tools/xtr_bin_predication.py`
// prints for capture B1. Enabled by CZ_PM4_BIN_CENSUS=1 and printed by the ring trace.
void Pm4_BinCensusEnable() { g_binCensus = true; }
void Pm4_BinCensusReport()
{
    if (!g_binCensus)
        return;
    std::lock_guard<std::mutex> lock(g_binPairMutex);
    fprintf(stderr, "[kernel] ring: bin census (mask, select) -> offered / skipped   "
                    "[B1: 0.3%% of draw packets are discarded; overflow=%llu]\n",
            (unsigned long long)g_binPairOverflow.load());
    for (uint32_t i = 0; i < kBinPairs && g_binPairs[i].used; i++)
        fprintf(stderr,
                "[kernel] ring:   %016llX %016llX  offered %10llu  skipped %10llu  "
                "kept %5.1f%%\n",
                (unsigned long long)g_binPairs[i].mask,
                (unsigned long long)g_binPairs[i].select,
                (unsigned long long)g_binPairs[i].offered,
                (unsigned long long)g_binPairs[i].skipped,
                100.0 * double(g_binPairs[i].offered - g_binPairs[i].skipped) /
                    double(g_binPairs[i].offered ? g_binPairs[i].offered : 1));
}
uint64_t Pm4_FrameCount() { return g_frames.load(); }
uint64_t Pm4_InterruptCount() { return g_interrupts.load(); }
const char* Pm4_OpcodeName(uint32_t opcode)
{
    return opcode < 128 ? kOpcodeNames.name[opcode] : nullptr;
}
