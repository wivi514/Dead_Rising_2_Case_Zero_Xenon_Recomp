// Phase C: redirected emission. The design argument lives in d3d_draw.h; this file
// is the mechanism. Three pieces:
//
//   1. The REDIRECT: swap the device's command-buffer cursor (dev+0x30) and end
//      (dev+0x38) to a private guest scratch around each content API call-through.
//   2. The WALKER: a faithful subset of gpu/pm4.cpp's packet decode, over the
//      scratch, into a PRIVATE register file and shader bindings. pm4.cpp itself is
//      deliberately untouched — it is the control arm (gotcha 86), and sharing its
//      global register file would interleave two streams' state across two threads.
//      Where this file and pm4.cpp encode the same packet, pm4.cpp is the reference;
//      divergence is a bug HERE.
//   3. The DISPATCH: draws and resolves go to the phase-5 renderer's decode guts
//      through the VkRenderer_D3D* entries, which take the register file and shader
//      hashes as arguments instead of reading pm4.cpp's globals.
//
// Everything is inert unless CZ_D3D_DRAW=1.
#include "d3d_draw.h"

#include "pm4.h"
#include "vd.h"
#include "vk_renderer.h"
#include "../kernel/heap.h"
#include "../kernel/memory.h"

#include "ppc_recomp_shared.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

// --- guest memory, same conventions as pm4.cpp -------------------------------------
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

constexpr uint32_t kPhysArenaBase = 0xA0000000u;
constexpr uint32_t kPhysArenaEnd = 0xBFFF0000u;

inline uint32_t PhysToVa(uint32_t addr) { return kPhysArenaBase | (addr & 0x1FFFFFFFu); }

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

// The residual swap after GuestLoad32/Store32's own 8-in-32 — see pm4.cpp's comment
// (the one Asura's Wrath paid for): code 2 composes to the identity.
inline uint32_t GpuSwapResidual(uint32_t v, uint32_t endian)
{
    return GpuSwap(v, (endian & 3) ^ 2);
}

// --- diagnostics -------------------------------------------------------------------
// Same discipline as the renderer: every declined path is a named counter, because a
// walker that silently drops a packet class looks exactly like one that handled it.
std::map<std::string, uint64_t> g_stats;
void Count(const char* name) { ++g_stats[name]; }

// Wall time spent inside the phase C machinery, split at the two seams that matter:
// the whole redirected call (guest body + parse + render) and the swap present. If
// these track the wall clock, phase C IS the frame cost; if they are small while the
// frame rate is low, the title is waiting on its own protocol and the hunt moves to
// the lifecycle side. (An answer a profiler could give too, but this one is in the
// same stats block as everything else and costs two clock reads per call.)
uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
std::atomic<uint64_t> g_nsInRedirect{ 0 };
std::atomic<uint64_t> g_nsInSwap{ 0 };

// --- private stream state ----------------------------------------------------------
constexpr uint32_t kRegCount = 0x8000;
uint32_t g_regs[kRegCount];

Pm4ShaderBinding g_shader[2]; // 0 = vertex, 1 = pixel

// Predicated binning, exactly pm4.cpp's rule: a type-3 header with bit 0 set runs
// only when (mask & select) != 0. The flush emits its shader-variant IM_LOADs and
// copy-mode SET_CONSTANTs predicated, so skipping this rule would bind the wrong
// variant silently.
uint64_t g_binMask = ~0ull;
uint64_t g_binSelect = ~0ull;

// --- the scratch -------------------------------------------------------------------
constexpr uint32_t kScratchBytes = 4u << 20;
constexpr uint32_t kScratchHeadroom = 64u << 10; // published end sits this far short

uint32_t g_scratchVa = 0;

// One redirect at a time, owned by one thread. The guest serializes its own command
// buffer (the D3D worker takes the device lock before emitting), so cross-thread
// entry here means the guest was already broken — counted, never "handled".
std::atomic<uint64_t> g_redirectOwner{ 0 }; // host thread id (as uintptr) or 0
int g_depth = 0;                            // nesting depth on the owning thread
uint32_t g_dev = 0;
uint32_t g_save30 = 0, g_save34 = 0, g_save38 = 0;
uint32_t g_parseVa = 0; // next unparsed scratch dword (guest VA)

// --- shader binding ----------------------------------------------------------------
// FNV-1a over the big-endian microcode: the renderer's cache key, byte-identical to
// pm4.cpp's so both arms name a shader the same way (gotcha 115).
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

void BindShader(uint8_t* base, uint32_t type, uint32_t ucodeVa, const uint8_t* code,
                uint32_t sizeDwords)
{
    if (type > 1 || !sizeDwords || sizeDwords > 0x10000)
    {
        Count("walker: IM_LOAD with impossible stage/size");
        return;
    }
    const uint64_t hash = Fnv1a(code, size_t(sizeDwords) * 4);
    g_shader[type] = { ucodeVa, sizeDwords, hash };
    (void)base;

    static std::vector<uint64_t> announced;
    if (std::find(announced.begin(), announced.end(), hash) == announced.end())
    {
        announced.push_back(hash);
        fprintf(stderr, "[d3ddraw] %s va=%08X hash=%016llx size=%u\n",
                type == 0 ? "VS" : "PS", ucodeVa, (unsigned long long)hash, sizeDwords);
    }
}

// --- register writes ---------------------------------------------------------------
void StoreGpuRaw(uint8_t* base, uint32_t physAddr, uint32_t value)
{
    const uint32_t va = PhysToVa(physAddr & ~3u);
    // The walker's half of CZ_PM4_MEM_WATCH (see pm4.cpp): with both halves
    // printing, the log says which STREAM a watched write came from.
    {
        static const char* w = getenv("CZ_PM4_MEM_WATCH");
        static const uint32_t watch = w ? uint32_t(strtoul(w, nullptr, 16)) : 0;
        if (watch && (va & ~3u) == (watch & ~3u))
        {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "[d3ddraw] MEM_WATCH %08X <- %08X (t=%ld.%03ld)\n", va, value,
                    (long)ts.tv_sec, ts.tv_nsec / 1000000);
        }
    }
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
    {
        Count("walker: GPU store outside the physical arena DROPPED");
        return;
    }
    GuestStore32(base, va, value);
}

void WriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegCount)
        return;
    g_regs[index] = value;

    // Scratch-register writeback, pm4.cpp's rule (bare physical base, no endian
    // code): the guest ISR reads its callback out of this mirror, and the content
    // stream arms it right before its INTERRUPT packets. The mirror CONFIG
    // (UMSK/ADDR) was set once via the real ring at device init and our seed can
    // predate it, so pm4's registers are the fallback authority — with our own
    // stale zeros, none of the content's armings ever reached guest memory.
    if (index >= 0x0578 && index <= 0x057F)
    {
        const uint32_t reg = index - 0x0578;
        const uint32_t umsk = g_regs[0x01DC] ? g_regs[0x01DC] : Pm4_ScratchUmsk();
        const uint32_t addr = g_regs[0x01DD] ? g_regs[0x01DD] : Pm4_ScratchAddr();
        if (umsk & (1u << reg))
        {
            // Serialized against the pump's replay+delivery (Vd_MirrorMutex): a
            // write landing between its poison check and the guest ISR's mirror
            // load hands the ISR the poison as a callback. Measured as a crash
            // with ctr=0BADF00D and lr inside the ISR.
            std::lock_guard<std::recursive_mutex> lock(Vd_MirrorMutex());
            StoreGpuRaw(base, addr + reg * 4, value);
        }
    }
}

void StoreGpu(uint8_t* base, uint32_t addrDword, uint32_t value, uint32_t indexDwords = 0)
{
    // Through StoreGpuRaw (pm4.cpp's shape) so the memory watch sees packet stores.
    StoreGpuRaw(base, (addrDword & ~3u) + indexDwords * 4, GpuSwapResidual(value, addrDword));
}

uint32_t LoadGpu(const uint8_t* base, uint32_t addrDword)
{
    const uint32_t va = PhysToVa(addrDword & ~3u);
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
        return 0;
    return GpuSwapResidual(GuestLoad32(base, va), addrDword);
}

// --- the walker --------------------------------------------------------------------
// Executes one packet at `va`; returns dwords consumed, 0 to stop (malformed). The
// opcode set is the D3D library's content-path vocabulary — a subset of B1's 21 —
// plus loud counters for anything else. An unknown TYPE-3 opcode is skippable by its
// own header count; a malformed header is not, and stopping there is safe because
// every parse window ends at a call boundary, so the next window starts on a fresh
// packet (gotcha 84: the stop is counted, never silent).
uint32_t ExecutePacket(PPCContext& ctx, uint8_t* base, uint32_t va, uint32_t availDwords)
{
    const uint32_t header = GuestLoad32(base, va);
    const uint32_t type = header >> 30;

    if (type == 2)
        return 1;

    if (type == 1)
    {
        if (availDwords < 3)
            return 0;
        WriteRegister(base, header & 0x7FF, GuestLoad32(base, va + 4));
        WriteRegister(base, (header >> 11) & 0x7FF, GuestLoad32(base, va + 8));
        return 3;
    }

    const uint32_t bodyCount = ((header >> 16) & 0x3FFF) + 1;
    if (availDwords < bodyCount + 1)
    {
        Count("walker: packet claims past the emitted end — STOPPED");
        static std::atomic<uint64_t> n{ 0 };
        if (n.fetch_add(1) < 6)
            fprintf(stderr, "[d3ddraw] packet header %08X at %08X claims %u dwords, %u "
                            "remain — stopping this window\n",
                    header, va, bodyCount + 1, availDwords);
        return 0;
    }

    if (type == 0)
    {
        const uint32_t reg = header & 0x7FFF;
        const bool oneReg = (header >> 15) & 1;
        for (uint32_t i = 0; i < bodyCount; i++)
            WriteRegister(base, oneReg ? reg : reg + i, GuestLoad32(base, va + 4 + i * 4));
        return bodyCount + 1;
    }

    // type 3
    const uint32_t opcode = (header >> 8) & 0x7F;
    auto body = [&](uint32_t i) { return GuestLoad32(base, va + 4 + i * 4); };

    // ME predication, pm4.cpp's rule verbatim.
    if ((header & 1) && (g_binMask & g_binSelect) == 0)
    {
        Count("walker: type-3 predicated skip");
        return bodyCount + 1;
    }

    switch (opcode)
    {
        case 0x10: // NOP
        case 0x23: // VIZ_QUERY
        case 0x26: // WAIT_FOR_IDLE — synchronous walk, instantly true
        case 0x3B: // INVALIDATE_STATE
            break;

        case 0x54: // INTERRUPT: the content path emits ~1 per frame (the resolve
                   // completion tick), and it is the token worker's kick.
        {
            const uint32_t user = Vd_InterruptUserData();
            if (!user || !Vd_InterruptCallbackVa())
            {
                Count("walker: INTERRUPT with no registered ISR");
                break;
            }
            // NOT delivered by calling the guest ISR. Three designs died on the
            // same crash (ctr=0x0BADF00D inside the ISR): deferring to the pump
            // arrived after the guest re-poisoned the mirror; deferring WITH a
            // mirror snapshot replayed at delivery still raced the guest CPU's own
            // poison stores; and calling the ISR in-position still let the ISR
            // RE-READ the mirror word after our guard, which a concurrent guest
            // store (the worker poisoning a consumed arming) can change. No host
            // lock intercepts plain guest stores. So the walker performs the ISR's
            // source-1 path ITSELF from ONE guarded read: if the mirror is armed
            // at this position, call that callback with that argument, then
            // replicate the ISR's tail (clear this CPU's pending bit under the ISR
            // spinlock, sub_82844D38's own sequence). An unarmed or poisoned
            // mirror at our position means the arm rode the other transport (an
            // unredirected emitter -> the real ring): skip, counted — the pairing
            // real-ring INTERRUPT still delivers on the pump.
            {
                std::lock_guard<std::recursive_mutex> mlock(Vd_MirrorMutex());
                // The arming state comes from THIS STREAM's register file, not the
                // guest-memory mirror. Our file is in-position coherent by
                // construction (one stream, one thread); the memory mirror is
                // where BOTH streams' armings and poisons land, and consulting it
                // skipped the movie era's job kicks whenever the other stream had
                // poisoned its own consumed arming there — the 600 s run
                // deadlocked at cinematics on exactly those skips.
                uint32_t armed = g_regs[0x057C];
                uint32_t arg = g_regs[0x057D];
                if (armed == 0 || armed == 0x0BADF00D)
                {
                    // The arm rode the OTHER transport: the movie player's
                    // unredirected emitters arm through the real ring while the
                    // INTERRUPT rides the content stream. pm4's register file is
                    // that transport's in-position state; a spurious extra kick is
                    // a no-op to the job queue, calling the poison is a crash, and
                    // skipping starves the worker (measured: the movie era stalls
                    // ~150 frames further in without this fallback).
                    const uint32_t* preg = Pm4_Registers();
                    armed = preg[0x057C];
                    arg = preg[0x057D];
                    Count("walker: INTERRUPT arm taken from the ring transport");
                }
                static std::atomic<uint64_t> disp{ 0 };
                const uint64_t dn = disp.fetch_add(1);
                if (dn < 16)
                    fprintf(stderr, "[d3ddraw] INTERRUPT #%llu at position: "
                                    "armed=%08X arg=%08X\n",
                            (unsigned long long)dn, armed, arg);
                if (armed == 0 || armed == 0x0BADF00D)
                {
                    Count("walker: INTERRUPT skipped (mirror unarmed/poisoned at position)");
                    break;
                }
                PPCFunc* cb = g_memory.FindFunction(armed);
                if (!cb)
                {
                    Count("walker: INTERRUPT callback not recompiled (SKIPPED)");
                    break;
                }
                Count("walker: INTERRUPT callback delivered in-position");
                PPCContext saved = ctx;
                ctx.r3.u64 = arg;
                cb(ctx, base);
                ctx = saved;

                PPCFunc* lk = g_memory.FindFunction(0x829C3354);
                PPCFunc* ulk = g_memory.FindFunction(0x829C3344);
                if (lk && ulk)
                {
                    const uint32_t cpuBit = 1u << (base[saved.r13.u32 + 0x10C] & 31);
                    ctx.r3.u64 = user + 0x2A98;
                    lk(ctx, base);
                    const uint32_t mb = GuestLoad32(base, user + 0x2A94);
                    if (mb >= 0x1000)
                        GuestStore32(base, mb, (GuestLoad32(base, mb) & ~cpuBit) & 0x3F);
                    ctx.r3.u64 = user + 0x2A98;
                    ulk(ctx, base);
                    ctx = saved;
                }
            }
            break;
        }

        case 0x60: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x61: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;
        case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;

        case 0x2D: // SET_CONSTANT
        {
            uint32_t index = body(0) & 0x7FF;
            switch ((body(0) >> 16) & 0xFF)
            {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = kRegCount;
            }
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, index + i - 1, body(i));
            break;
        }

        case 0x55: // SET_CONSTANT2: absolute index
        case 0x56:
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, (body(0) & 0xFFFF) + i - 1, body(i));
            break;

        case 0x2F: // LOAD_ALU_CONSTANT: addr, offset_type, size
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
            for (uint32_t i = 0; i < size; i++)
                WriteRegister(base, index + i, GuestLoad32(base, addr + i * 4));
            break;
        }

        case 0x27: // IM_LOAD: addr_type (low 2 bits = stage), start_size
            if (bodyCount >= 2 && (body(0) & 3) < 2)
            {
                const uint32_t sva = PhysToVa(body(0) & ~3u);
                const uint32_t size = body(1) & 0xFFFF;
                if (size && size <= 0x10000)
                {
                    // Snapshot before hashing, like pm4.cpp — the staging area
                    // belongs to the guest and this walk is synchronous, but the
                    // hash and any later read must still agree with each other.
                    std::vector<uint8_t> code(size_t(size) * 4);
                    memcpy(code.data(), base + sva, code.size());
                    BindShader(base, body(0) & 3, sva, code.data(), size);
                }
            }
            break;

        case 0x2B: // IM_LOAD_IMMEDIATE: type, start_size, then the code
            if (bodyCount >= 2 && (body(0) & 3) < 2)
            {
                const uint32_t size = body(1) & 0xFFFF;
                if (size && size + 2 <= bodyCount)
                {
                    std::vector<uint8_t> code(size_t(size) * 4);
                    for (uint32_t k = 0; k < size; k++)
                    {
                        const uint32_t w = body(2 + k);
                        code[k * 4 + 0] = uint8_t(w >> 24);
                        code[k * 4 + 1] = uint8_t(w >> 16);
                        code[k * 4 + 2] = uint8_t(w >> 8);
                        code[k * 4 + 3] = uint8_t(w);
                    }
                    BindShader(base, body(0) & 3, 0, code.data(), size);
                }
            }
            break;

        case 0x22: // DRAW_INDX:   dword0 = viz query info, dword1 = initiator
        case 0x36: // DRAW_INDX_2: dword0 = initiator
        {
            const uint32_t initiatorAt = (opcode == 0x22) ? 1 : 0;
            if (bodyCount <= initiatorAt)
                break;
            const uint32_t init = body(initiatorAt);

            Pm4Draw d{};
            d.primType = init & 0x3F;
            const uint32_t sourceSelect = (init >> 6) & 3;
            d.index32 = ((init >> 11) & 1) != 0;
            d.indexCount = init >> 16;
            d.indexed = sourceSelect == 0;
            if (d.indexed && bodyCount > initiatorAt + 2)
            {
                const uint32_t addrDword = body(initiatorAt + 1);
                d.indexEndian = addrDword & 3;
                d.indexVa = PhysToVa(addrDword & ~3u);
            }
            else if (d.indexed)
            {
                d.indexed = false;
            }
            Count("walker: draw dispatched");
            VkRenderer_D3DDraw(base, d, g_regs, g_shader[0], g_shader[1]);
            break;
        }

        // The fence family. Honoured (memory writes and all) because the guest may
        // poll what it just asked to be written — and this walk IS the execution, so
        // the honest completion time is now.
        case 0x46: // EVENT_WRITE
        case 0x58: // EVENT_WRITE_SHD
        case 0x59: // EVENT_WRITE_CFL
        case 0x5A: // EVENT_WRITE_EXT
            Count("walker: EVENT_WRITE executed");
            WriteRegister(base, 0x21F9 /* VGT_EVENT_INITIATOR */, body(0) & 0x3F);
            if (bodyCount >= 3)
                StoreGpu(base, body(1), body(2));
            break;

        case 0x3D: // MEM_WRITE
            for (uint32_t i = 1; i < bodyCount; i++)
                StoreGpu(base, body(0), body(i), i - 1);
            break;

        case 0x3E: // REG_TO_MEM
            if (bodyCount >= 2)
            {
                const uint32_t reg = body(0) & 0x7FFF;
                StoreGpu(base, body(1), reg < kRegCount ? g_regs[reg] : 0);
            }
            break;

        case 0x21: // REG_RMW
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

        case 0x3C: // WAIT_REG_MEM in a synchronous walk: evaluate once; if the
                   // condition fails, the stream is waiting on something this walk
                   // can never produce — count it loudly and move on rather than
                   // spin. It has not been observed on the content path.
        {
            if (bodyCount < 4)
                break;
            const uint32_t info = body(0);
            // Coherency requests complete instantly here too (pm4.cpp's rule): the
            // driver sets COHER_STATUS_HOST bit 31 and waits for it to clear.
            if (!(info & 0x10) && (body(1) & 0x7FFF) == 0x0A31)
                g_regs[0x0A31] &= ~0x80000000u;
            const uint32_t value = (info & 0x10) ? LoadGpu(base, body(1))
                                                 : ((body(1) & 0x7FFF) < kRegCount
                                                        ? g_regs[body(1) & 0x7FFF]
                                                        : 0);
            const uint32_t v = value & body(3);
            bool ok;
            switch (info & 7)
            {
                case 0: ok = false; break;
                case 1: ok = v < body(2); break;
                case 2: ok = v <= body(2); break;
                case 3: ok = v == body(2); break;
                case 4: ok = v != body(2); break;
                case 5: ok = v >= body(2); break;
                case 6: ok = v > body(2); break;
                default: ok = true; break;
            }
            Count(ok ? "walker: WAIT_REG_MEM satisfied"
                     : "walker: WAIT_REG_MEM UNSATISFIED (skipped)");
            break;
        }

        case 0x37: // INDIRECT_BUFFER_PFD
        case 0x3F: // INDIRECT_BUFFER — the content path has no business emitting
                   // one; if it ever does, that is a hole in the redirect argument.
            Count("walker: INDIRECT_BUFFER on the content path (SKIPPED, a defect)");
            break;

        default:
        {
            static std::atomic<uint64_t> seen[128];
            if (seen[opcode].fetch_add(1) == 0)
                fprintf(stderr, "[d3ddraw] unhandled type-3 opcode 0x%02X (header %08X, "
                                "%u body dwords) — skipped by its own count\n",
                        opcode, header, bodyCount);
            char name[64];
            snprintf(name, sizeof name, "walker: unhandled opcode 0x%02X", opcode);
            Count(name);
            break;
        }
    }
    return bodyCount + 1;
}

// Parse [g_parseVa, endVa). Called at every hook boundary inside a redirect, so
// state staged by one inner call is consumed before the next inner call overwrites
// its staging (the UP scratch at dev+0x780 is reused per draw).
void ParseUpTo(PPCContext& ctx, uint8_t* base, uint32_t endVa)
{
    while (g_parseVa < endVa)
    {
        const uint32_t avail = (endVa - g_parseVa) / 4;
        const uint32_t used = ExecutePacket(ctx, base, g_parseVa, avail);
        if (!used)
        {
            // Malformed or truncated: resync at the call boundary (see ExecutePacket).
            g_parseVa = endVa;
            break;
        }
        g_parseVa += used * 4;
    }
}

// --- init --------------------------------------------------------------------------
enum class Mode { Unknown, Off, On, Failed };
Mode g_mode = Mode::Unknown;

bool Init()
{
    if (!getenv("CZ_D3D_DRAW") || *getenv("CZ_D3D_DRAW") == '0')
    {
        g_mode = Mode::Off;
        return false;
    }
    if (getenv("CZ_VKDRAW"))
    {
        // Two renderers over one Vulkan state is not an arm, it is a collision.
        fprintf(stderr, "[d3ddraw] CZ_D3D_DRAW and CZ_VKDRAW are mutually exclusive; "
                        "CZ_D3D_DRAW DISABLED for this run\n");
        g_mode = Mode::Failed;
        return false;
    }

    // The scratch, from the TOP of the physical arena so the title's own map is
    // undisturbed (gotcha 74). It only ever holds packets we parse ourselves, but it
    // must be guest-addressable because the guest's own emitters write it.
    void* host = g_heap.AllocPhysical(kScratchBytes, 0x1000, /*topDown=*/true);
    if (!host)
    {
        fprintf(stderr, "[d3ddraw] scratch allocation FAILED — draw service disabled\n");
        g_mode = Mode::Failed;
        return false;
    }
    g_scratchVa = g_memory.MapVirtual(host);

    // Seed the private register file from the PM4 executor's: device init emitted
    // its register defaults into the REAL ring at boot (before any redirect
    // existed), and the CP has consumed them by the time the first draw arrives.
    // From here on the two files diverge on purpose — ours follows the content
    // stream, pm4's follows the lifecycle stream.
    memcpy(g_regs, Pm4_Registers(), sizeof g_regs);

    if (!VkRenderer_D3DInit())
    {
        fprintf(stderr, "[d3ddraw] renderer bring-up FAILED — draw service disabled\n");
        g_mode = Mode::Failed;
        return false;
    }

    fprintf(stderr, "[d3ddraw] Phase C draw service UP: scratch %u KB at %08X, "
                    "register file seeded\n",
            kScratchBytes >> 10, g_scratchVa);
    g_mode = Mode::On;
    return true;
}

uint64_t SelfThreadId()
{
    // Distinct per thread, stable, cheap; the value itself is only compared.
    static thread_local int marker;
    return uint64_t(reinterpret_cast<uintptr_t>(&marker));
}

} // namespace

bool D3dDraw_Enabled()
{
    if (g_mode == Mode::Unknown)
        Init();
    return g_mode == Mode::On;
}

bool D3dDraw_ServiceContent(PPCContext& ctx, uint8_t* base, CzGuestFunc through)
{
    const uint32_t dev = ctx.r3.u32;
    const uint64_t self = SelfThreadId();

    uint64_t owner = g_redirectOwner.load(std::memory_order_acquire);
    if (owner != 0 && owner != self)
    {
        // A second thread inside the content API while a redirect is active: the
        // guest device is supposed to be serialized (the worker takes the device
        // lock). Counted, and called through untouched — the emissions land in the
        // scratch the OTHER thread owns, which its parse will read; interleaved but
        // not lost. If this ever counts nonzero, the serialization assumption needs
        // a real lock here.
        Count("redirect: CROSS-THREAD content call (assumption violated)");
        through(ctx, base);
        return true;
    }

    if (owner == self && g_depth > 0)
    {
        // Nested content call (Resolve/Clear calling a draw entry internally):
        // consume what the outer body emitted so far, then run the inner body in the
        // already-redirected stream.
        if (dev == g_dev)
            ParseUpTo(ctx, base, GuestLoad32(base, dev + 0x30) + 4);
        else
            Count("redirect: nested call on a DIFFERENT device");
        ++g_depth;
        through(ctx, base);
        --g_depth;
        if (dev == g_dev)
            ParseUpTo(ctx, base, GuestLoad32(base, dev + 0x30) + 4);
        return true;
    }

    const uint64_t t0 = NowNs();
    // Outermost: install the redirect. THREE fields, not two — the first run's spin
    // proved the cursor block is {+0x30 cursor, +0x34 hard segment end, +0x38 soft
    // reserve threshold}: the simple emitters check `cursor > [dev+0x38]` before a
    // bounded burst, but the chunked bulk emitter (sub_8284DAF8, LOAD_ALU-style
    // uploads) computes its per-chunk capacity as `([dev+0x34] - cursor) >> 2` and
    // loops on the reserve while that is <= 0. Redirecting only +0x38 left +0x34 at
    // the real segment's end, far below the scratch cursor: capacity negative, zero
    // dwords emitted per iteration, reserve forever.
    g_redirectOwner.store(self, std::memory_order_release);
    g_depth = 1;
    g_dev = dev;
    g_save30 = GuestLoad32(base, dev + 0x30);
    g_save34 = GuestLoad32(base, dev + 0x34);
    g_save38 = GuestLoad32(base, dev + 0x38);
    // stwu writes at cursor+4, so first dword lands exactly at the scratch base.
    GuestStore32(base, dev + 0x30, g_scratchVa - 4);
    GuestStore32(base, dev + 0x34, g_scratchVa + kScratchBytes);
    GuestStore32(base, dev + 0x38, g_scratchVa + kScratchBytes - kScratchHeadroom);
    g_parseVa = g_scratchVa;

    through(ctx, base);

    const uint32_t emittedEnd = GuestLoad32(base, dev + 0x30) + 4;
    if (emittedEnd > g_scratchVa + kScratchBytes - kScratchHeadroom)
        Count("redirect: emission entered the headroom (raise kScratchBytes)");
    ParseUpTo(ctx, base, emittedEnd);

    GuestStore32(base, dev + 0x30, g_save30);
    GuestStore32(base, dev + 0x34, g_save34);
    GuestStore32(base, dev + 0x38, g_save38);
    g_depth = 0;
    g_dev = 0;
    g_redirectOwner.store(0, std::memory_order_release);
    Count("redirect: content call serviced");
    g_nsInRedirect.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    return true;
}

void D3dDraw_OnSwap(uint8_t* base)
{
    if (g_mode != Mode::On)
        return;
    Count("swap: presented");
    const uint64_t t0 = NowNs();
    VkRenderer_D3DSwap(base);
    g_nsInSwap.fetch_add(NowNs() - t0, std::memory_order_relaxed);

    // The frame-pacing question, as numbers: the engine's per-frame wait
    // (sub_82845230 -> sub_82845160) polls `completed >= target` where `completed`
    // is [[dev+0x2A90]] — the fence writeback the stream's EVENT_WRITEs advance —
    // and dev+0x2A9C is the last fence EMITTED. If completed lags emitted by whole
    // segments here, the pacing is segment-close cadence, not tick latency.
    {
        static std::atomic<uint64_t> n{ 0 };
        const uint64_t k = n.fetch_add(1);
        if ((k & 31) == 0 && k < 512)
        {
            // The title-process device slot: [[0x82000758]] (sub_82845230's own load).
            const uint32_t slot = GuestLoad32(base, 0x82000758);
            const uint32_t dev = slot ? GuestLoad32(base, slot) : 0;
            if (dev)
            {
                const uint32_t wbBlock = GuestLoad32(base, dev + 0x2A90);
                const uint32_t emitted = GuestLoad32(base, dev + 0x2A9C);
                const uint32_t completed = wbBlock ? GuestLoad32(base, wbBlock) : 0;
                fprintf(stderr, "[d3ddraw] swap %llu: fence emitted=%u completed=%u (wb=%08X)\n",
                        (unsigned long long)k, emitted, completed, wbBlock);
            }
        }
    }

    // The walker's counters ride the same cadence as the renderer's (CZ_VK_STATS=N),
    // so one env var reports both halves of the phase C pipeline.
    static const long every = [] {
        const char* v = getenv("CZ_VK_STATS");
        return v ? std::max(1L, strtol(v, nullptr, 10)) : 0L;
    }();
    static uint64_t frames = 0;
    ++frames;
    if (every && (frames % uint64_t(every)) == 0)
        D3dDraw_DumpStats();
}

bool D3dDraw_ServiceReserve(PPCContext& ctx, uint8_t* base)
{
    if (g_redirectOwner.load(std::memory_order_acquire) != SelfThreadId() || g_depth <= 0)
        return false; // no redirect on this thread: the real reserve runs untouched

    // Mid-redirect the answer is always "you have space": hand back the scratch
    // cursor exactly as the real reserve hands back [dev+0x30], and keep the guest's
    // segment-close/kick/park machinery away from a cursor it does not own. This is
    // also a natural parse boundary — the emitter asked for a fresh block, so
    // everything before the ask is complete packets.
    const uint32_t dev = ctx.r3.u32;
    if (dev == g_dev)
        ParseUpTo(ctx, base, GuestLoad32(base, dev + 0x30) + 4);
    else
        Count("redirect: reserve on a DIFFERENT device (serviced anyway)");
    ctx.r3.u64 = GuestLoad32(base, g_dev + 0x30);
    Count("redirect: reserve serviced from the scratch");
    {
        static std::atomic<uint64_t> n{ 0 };
        const uint64_t k = n.fetch_add(1);
        if (k < 12)
            fprintf(stderr, "[d3ddraw] reserve serviced #%llu: caller lr=%08X cursor=%08X\n",
                    (unsigned long long)(k + 1), uint32_t(ctx.lr), uint32_t(ctx.r3.u32));
    }
    return true;
}

void D3dDraw_DumpStats()
{
    if (g_stats.empty())
        return;
    fprintf(stderr, "[d3ddraw] --- walker counters ---\n");
    fprintf(stderr, "[d3ddraw]   time in redirects %llu ms, in swap presents %llu ms\n",
            (unsigned long long)(g_nsInRedirect.load() / 1000000),
            (unsigned long long)(g_nsInSwap.load() / 1000000));
    for (const auto& [name, value] : g_stats)
        fprintf(stderr, "[d3ddraw]   %-58s %12llu\n", name.c_str(),
                (unsigned long long)value);
}
