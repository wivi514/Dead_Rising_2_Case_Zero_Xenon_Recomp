// OBSERVE-mode hooks over the title's statically-linked XDK D3D functions.
//
// WHY THIS EXISTS
// ---------------
// The D3D translation pivot (docs/d3d-translation-plan.md) replaces PM4-level GPU
// emulation with hooks at the D3D API line. Before any hook REPLACES anything, every
// identification in the Phase A table has to be validated against a live run — a
// function that never fires, or fires implausibly (two Swaps a frame, a draw that
// never draws), was misidentified, and finding that out costs one run here versus a
// hang with no consumer in replace mode. This file is that measurement arm: every
// identified function is hooked to LOG AND CALL THROUGH (`__imp__sub_X`), with the
// PM4 executor still consuming the ring. Zero behavioural change by construction.
//
// The hook seam is CLAUDE.md gotcha 6: XenonRecomp emits every guest function as
// `__imp__sub_X` plus a WEAK alias `sub_X`, so a strong `PPC_FUNC(sub_X)` takes over
// every call site — including indirect calls, because the dispatch table also points
// at the strong alias. That last part matters here: sub_8284B828 and sub_828494A0
// have ZERO direct callers and are only reachable indirectly.
//
// Everything is behind CZ_D3D_OBSERVE, read once; off, each hook costs one
// predictable branch (the standard from guest_probe.cpp — gotcha 7).
//
// NAMING: hook names ending in '?' in the table below are TENTATIVE — recorded in
// docs/d3d-kickoff.md's Phase A table with per-row evidence. The per-frame stream
// summary this file prints is what confirms or retracts each of them.
// REPLACE mode (CZ_D3D=1, phase B) services a subset of the hooks instead of
// calling through: the draws, clears, resolves, flushes and fences become no-ops
// (with the fence VALUE still advancing, because InsertFence's return feeds guest
// bookkeeping), and Swap presents directly to the host window. CreateDevice, device
// init and every state setter still call through — init is what keeps the
// kernel-call order the A1/A5 gates diff (kickoff trap 2), and the setters are what
// keep the device struct's register shadow current for phase C to read. The PM4
// executor stays ON as the missed-hook detector: with the emitting APIs serviced,
// per-frame ring traffic should collapse to almost nothing, and the per-frame
// pm4_packets delta printed at each Swap names the frame any unhooked emitter
// slips a packet in.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"
#include "pm4.h"
#include "../host/window.h"

// ---------------------------------------------------------------------------------
// The Phase A hook table. One X-macro row per identified function:
//   X(address, Name, confirmed-or-tentative)
// Evidence per row lives in docs/d3d-kickoff.md; do not rename here without
// updating there.
// ---------------------------------------------------------------------------------
#define CZ_D3D_HOOKS(X)                                                              \
    /* device lifecycle */                                                           \
    X(8283CCE8, CreateDevice)               /* alloc 0x5F00, out-param device */     \
    X(8283C7F0, DeviceInit_TypeB)           /* devtype==2 alt init path */           \
    X(8284CF88, DeviceInit_Stage2)          /* ring init (calls Vd wrapper) */       \
    X(8284D270, DeviceInit_Interrupt)       /* registers gfx interrupt cb */         \
    /* frame */                                                                      \
    X(82841F00, Swap)                       /* VdSwap's only call site */            \
    X(82841AD0, PreSwapResolve)             /* 82841EF8 thunks here with r4=0 */     \
    X(82845230, InsertCallback_q)           /* 6-dword emit carrying a pointer */    \
    X(82846068, InsertFence_q)              /* flush; returns dev+0x2A9C */          \
    X(82846288, Fence_82846288_q)           /* fence/throttle-shaped */              \
    X(8283C898, SuspendNotify_q)            /* walks cb list at 0x82000764 */        \
    X(82838088, FlushState_82838088_q)      /* clears dev+0x325C flush mask */       \
    X(8283A110, FlushGpuCache_q)            /* builds 0xC001xxxx from mask */        \
    X(82838568, Unknown_82838568)           /* caller 827A00B8 (movie player?) */    \
    X(82838D10, Unknown_82838D10)           /* polls two queues, same caller */      \
    /* draws */                                                                      \
    X(82843A98, DrawIndexedVertices)        /* decodes 16/32-bit index buffer */     \
    X(82842A88, DrawVerticesUP)             /* stages via dev+0x780 scratch */       \
    X(82842E78, DrawIndexedVerticesUP)      /* same scratch, +1 arg */               \
    X(82842570, DrawUP_82842570_q)          /* count*stride math, index halving */   \
    X(82838858, Resolve)                    /* float ClearZ + internal rect draw */  \
    X(82841630, Clear)                      /* rects + float Z, wraps 82841508 */    \
    X(82841508, ClearF)                     /* the worker */                         \
    /* state */                                                                      \
    X(8283FD28, SetRenderTarget)            /* dev+0x3148+index*4 (82840350 thunk) */\
    X(82840078, SetDepthStencilSurface)     /* dev+0x3158 = RT slot 4 */             \
    X(8283FBF8, SetViewport)                /* int struct -> float worker */         \
    X(8283F990, SetViewportF)               /* the worker */                         \
    X(8283E950, SetShaderConstantF_A_q)     /* (reg+0x30)*0x18 shadow; VS or PS */   \
    X(8283EAF8, SetShaderConstantF_B_q)     /* the twin */                           \
    X(82839830, SetTexture)                 /* fetch consts, dev+(smp+0x78)*16 */    \
    X(82836958, SetStreamSource)            /* vb ptr at dev+0x31B0+stream*4 */      \
    X(82839D38, SetVertexShader_q)          /* dev+0x3248 — the flush bails if 0 */  \
    X(82839B78, SetPixelShader_q)           /* dev+0x3244 — nullable at the flush */ \
    X(82839F08, SetVertexDeclaration_q)     /* dev+0x2ED8, 5-instr passive setter */ \
    X(8283F848, SetClipPlane_q)             /* float4 into small dirty-masked tbl */ \
    /* resources */                                                                  \
    X(82837CF0, DestructResource)           /* wraps 82837788 type switch */         \
    X(82837D70, GpuBusyTrack_A_q)           /* addr/size from res+0x18/+0x1C */      \
    X(82837DC0, GpuBusyTrack_B_q)           /* the flags|2 twin */                   \
    X(82837E08, Unknown_82837E08)           /* GPU-address classification */         \
    X(82836630, CreateResource_A_q)         /* thunk family into 82836038 */         \
    X(82836640, CreateResource_B_q)                                                  \
    X(82836648, CreateResource_C_q)                                                  \
    X(82836038, CreateResource_Worker_q)                                             \
    /* the command-buffer/worker layer (finding 40's threads). NB the token        */\
    /* interpreter sub_8284B568 is NOT here: guest_probe.cpp already owns that     */\
    /* hook (CZ_JOBQ_PROBE), and two strong definitions cannot link.               */\
    X(8284B828, WorkerSwapPath)             /* indirect-only; may call Swap */       \
    X(828494A0, IndirectDrawPath)           /* indirect-only; reaches draw internal */

// ---------------------------------------------------------------------------------

namespace {

enum HookId {
#define X(addr, name) kH_##name,
    CZ_D3D_HOOKS(X)
#undef X
    kHookCount
};

const char* const kHookName[] = {
#define X(addr, name) #name,
    CZ_D3D_HOOKS(X)
#undef X
};

std::atomic<uint64_t> g_count[kHookCount];
std::atomic<uint64_t> g_sinceSwap[kHookCount];
std::atomic<uint64_t> g_swaps{0};

bool Observe()
{
    static const bool on = [] {
        const char* v = std::getenv("CZ_D3D_OBSERVE");
        bool en = v && *v && *v != '0';
        if (en)
            fprintf(stderr, "[d3d] OBSERVE mode: logging %d hooked D3D functions, all calls pass through\n",
                    (int)kHookCount);
        return en;
    }();
    return on;
}

bool Replace()
{
    static const bool on = [] {
        const char* v = std::getenv("CZ_D3D");
        bool en = v && *v && *v != '0';
        if (en)
            fprintf(stderr, "[d3d] REPLACE mode (phase B): draws/clears/resolves serviced (no-op) at the "
                            "API line; frame lifecycle (Swap/fences/flushes) still passes through\n");
        return en;
    }();
    return on;
}

// First few calls of each hook print their raw args (r3..r7 covers every identified
// signature's interesting prefix); after that only the counters move.
constexpr uint64_t kVerboseCalls = 6;

void Note(HookId id, PPCContext& ctx)
{
    uint64_t n = g_count[id].fetch_add(1, std::memory_order_relaxed);
    g_sinceSwap[id].fetch_add(1, std::memory_order_relaxed);
    if (n < kVerboseCalls) {
        fprintf(stderr, "[d3d] %s(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X) call #%llu\n",
                kHookName[id], ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                (unsigned long long)(n + 1));
    }
}

// A per-frame stream summary at the Swap hook: for the first frames every frame,
// then every 64th, print what the engine sent this frame. One Swap per frame with a
// plausible mix (state sets, N draws) is the OBSERVE gate; a hook at zero forever or
// a count wildly off the PM4 executor's own numbers is a misidentification.
void SwapSummary()
{
    static uint64_t lastPackets = 0;   // only touched here, on the presenting thread
    uint64_t f = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
    bool print = f <= 8 || (f & 63) == 0;
    uint64_t packets = Pm4_PacketCount();
    if (print) {
        char line[1024];
        int off = snprintf(line, sizeof line, "[d3d] frame %llu:", (unsigned long long)f);
        for (int i = 0; i < kHookCount; i++) {
            uint64_t c = g_sinceSwap[i].load(std::memory_order_relaxed);
            if (c && i != kH_Swap && off < (int)sizeof line - 48)
                off += snprintf(line + off, sizeof line - off, " %s=%llu",
                                kHookName[i], (unsigned long long)c);
        }
        // In replace mode the delta since the last PRINTED frame is the missed-hook
        // detector: the serviced APIs emit nothing, so sustained nonzero traffic
        // here means an unhooked entry is still submitting packets.
        fprintf(stderr, "%s | pm4_packets=%llu (+%llu)\n", line,
                (unsigned long long)packets, (unsigned long long)(packets - lastPackets));
    }
    if (print)
        lastPackets = packets;
    for (auto& c : g_sinceSwap)
        c.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------
// REPLACE-mode services. Return true = serviced, do not call through.
// Guest device fields are read/written through the same endian-swapping macros the
// recompiled code uses, so every value here is what the guest sees.
// ---------------------------------------------------------------------------------
bool Service(HookId id, PPCContext& ctx, uint8_t* base)
{
    (void)base;
    switch (id) {
    // Phase B services CONTENT only: the draws, clears and resolves become no-ops
    // (returning S_OK for callers that look). Everything belonging to the frame
    // LIFECYCLE — Swap, fences, flushes, busy-tracking — still calls through, and
    // the PM4 arm keeps consuming the skeleton stream it produces. Two failed
    // stricter attempts are recorded here so they are not retried naively:
    //   * Servicing Swap directly hung the boot at frame 1: the D3D worker thread
    //     (sub_8284B828) waits on an event embedded in the device struct that the
    //     real swap-completion protocol signals, the engine's render thread waits
    //     on that worker, and the main thread polls the render queue's ticket
    //     (predicate sub_82766760) forever. Replacing Swap means implementing that
    //     completion protocol at the API line — deferred until the OBSERVE data
    //     and the worker's disassembly pin every field it touches.
    //   * Servicing GpuBusyTrack_A crashed the boot: it is a Lock-style entry that
    //     RETURNS A CPU POINTER (the engine dereferenced our 0 minus 8 while
    //     filling a resource from boot.bct). Fence_82846288 likewise returns a
    //     cursor the engine stores.
    case kH_Clear:
    case kH_ClearF:
    case kH_Resolve:
    case kH_PreSwapResolve:
    case kH_DrawIndexedVertices:
    case kH_DrawVerticesUP:
    case kH_DrawIndexedVerticesUP:
    case kH_DrawUP_82842570_q:
        ctx.r3.u64 = 0;
        return true;
    default:
        return false;   // everything else calls through
    }
}

} // namespace

// Every hook wraps the real body; ppc_recomp_shared.h declares only the weak alias.
#define X(addr, name) extern "C" PPC_FUNC(__imp__sub_##addr);
CZ_D3D_HOOKS(X)
#undef X

#define X(addr, name)                                                                \
    PPC_FUNC(sub_##addr)                                                             \
    {                                                                                \
        if (Observe()) {                                                             \
            Note(kH_##name, ctx);                                                    \
            if (kH_##name == kH_Swap)                                                \
                SwapSummary();                                                       \
        }                                                                            \
        if (Replace() && Service(kH_##name, ctx, base))                              \
            return;                                                                  \
        __imp__sub_##addr(ctx, base);                                                \
    }
CZ_D3D_HOOKS(X)
#undef X
