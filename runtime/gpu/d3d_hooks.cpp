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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>

#include "ppc_recomp_shared.h"
#include "pm4.h"
#include "d3d_draw.h"
#include "../cpu/chain_stats.h"
#include "../cpu/fence_probe.h"
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
    X(828494A0, IndirectDrawPath)           /* indirect-only; reaches draw internal */\
    /* phase C's safety net: the command-buffer reserve. Never serviced — hooked   */\
    /* only so a reserve that fires while a draw-service redirect is active is a   */\
    /* LOUD counter instead of silent segment bookkeeping against our scratch      */\
    /* cursor (see d3d_draw.h's safety argument).                                  */\
    X(82845F68, RingReserve)                                                         \
    /* the frame-end async submit, reached from Resolve (which IS redirected) and  */\
    /* from EndTiling. It is the only site in the image that arms sub_8284AAD0 —   */\
    /* the D3D worker's kick — and the only caller that ever passes r7=1 to        */\
    /* sub_82845AC0, i.e. the only +1 dev+0x2B04 ever gets. Like sub_82846288 and  */\
    /* sub_82841AD0 it emits protocol and no content, so it belongs on the ring.   */\
    X(8284B9C0, FrameEndAsyncSubmit)

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

bool DrawMode()
{
    static const bool on = [] {
        const char* v = std::getenv("CZ_D3D_DRAW");
        return v && *v && *v != '0';
    }();
    return on;
}

bool Replace()
{
    static const bool on = [] {
        const char* v = std::getenv("CZ_D3D");
        bool en = v && *v && *v != '0';
        if (DrawMode())
            en = true; // phase C implies the replace harness
        if (en)
            fprintf(stderr,
                    DrawMode()
                        ? "[d3d] REPLACE mode (phase C): content APIs serviced by REDIRECTED "
                          "EMISSION into the host renderer; frame lifecycle still passes through\n"
                        : "[d3d] REPLACE mode (phase B): draws/clears/resolves serviced (no-op) at the "
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

// ---------------------------------------------------------------------------------
// THE BIN-MASK PROBE — CZ_BINMASK_PROBE=1
//
// Phase C part 10. The scene's right tile renders almost nothing because a third of
// this title's draw packets are discarded by ME bin predication, and capture B1 says
// hardware discards 0.3% (tools/xtr_bin_predication.py). The pair census
// (CZ_PM4_BIN_CENSUS) localised that to one number: at the right tile's draws
// hardware's bin mask is 8000000F — "this draw touches all four bins" — and ours is
// 80000000 ("no bins") or 80000003 ("the left tile only").
//
// That mask is not something our command processor computes. It arrives as an
// UNPREDICATED SET_BIN_MASK_LO packet the guest emits from sub_82838088, whose body
// is "store r4 at dev+0x3254, emit it" — so the wrong number is decided by guest code
// ABOVE this function, out of something our runtime feeds it.
//
// This names that code. A line per call would be millions, so it is a census keyed on
// (mask, return address): the caller identifies the computation, and the set of masks
// each caller produces says whether a caller is deciding wrongly or never running.
// The return address is read from ctx.lr on ENTRY, the one moment it belongs to the
// caller — this function saves and restores lr itself.
void BinMaskNote(PPCContext& ctx)
{
    static const bool on = getenv("CZ_BINMASK_PROBE") != nullptr;
    if (!on)
        return;

    struct Site { uint32_t mask, lr; uint64_t count; bool used; };
    constexpr size_t kSites = 64;
    static Site sites[kSites];
    static std::mutex mutex;
    static std::atomic<uint64_t> overflow{ 0 };
    static std::atomic<uint64_t> calls{ 0 };

    const uint32_t mask = ctx.r4.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t i = 0;
        for (; i < kSites; i++) {
            if (!sites[i].used) { sites[i] = { mask, lr, 1, true }; break; }
            if (sites[i].mask == mask && sites[i].lr == lr) { sites[i].count++; break; }
        }
        if (i == kSites)
            overflow.fetch_add(1, std::memory_order_relaxed);
    }
    // Reported on a schedule rather than at exit: this boot parks rather than
    // terminating, so an atexit report would never print.
    //
    // The schedule is a CLOCK, not a call count. It was `n % 200000` and this function
    // is called a few thousand times a boot, so the only line a run ever printed was
    // the one at call #1 — whose census necessarily reads "x1". Part 10 quoted that as
    // "the other mask setter ran once, with mask 0". Gotcha 109 in our own probe.
    const uint64_t n = calls.fetch_add(1) + 1;
    static std::atomic<uint64_t> nextNs{ 0 };
    bool due = false;
    {
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        uint64_t deadline = nextNs.load(std::memory_order_relaxed);
        due = now >= deadline && nextNs.compare_exchange_strong(
                  deadline, now + 15ull * 1000000000ull, std::memory_order_relaxed);
    }
    if (n == 1 || due) {
        std::lock_guard<std::mutex> lock(mutex);
        fprintf(stderr, "[binmask] after %llu calls (overflow=%llu):\n",
                (unsigned long long)n, (unsigned long long)overflow.load());
        for (size_t i = 0; i < kSites && sites[i].used; i++)
            fprintf(stderr, "[binmask]   mask=%08X from lr=%08X  x%llu\n",
                    sites[i].mask, sites[i].lr, (unsigned long long)sites[i].count);
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
// REPLACE-mode services.
//   CallThrough — not serviced, run the guest body as usual.
//   Serviced    — handled here, do not run the guest body.
//   Redirect    — phase C: the wrapper hands the call to D3dDraw_ServiceContent,
//                 which runs the guest body itself under a redirected command-buffer
//                 cursor and renders what it emitted.
// ---------------------------------------------------------------------------------
//   RealRing    — phase C: the opposite of Redirect. The guest body runs with the REAL
//                 cursor restored, so its packets reach the CP and the title's own ISR
//                 instead of our walker (see d3d_draw.h's D3dDraw_ServiceRealRing).
enum class ServiceResult { CallThrough, Serviced, Redirect, RealRing };

// The reserve's real body, used directly by the sync-wait service below. The X-macro
// extern block further down declares the rest; this one is needed before it.
extern "C" PPC_FUNC(__imp__sub_82845F68);

ServiceResult Service(HookId id, PPCContext& ctx, uint8_t* base)
{
    switch (id) {
    // Phase C's extra seams, live only in draw mode. The Swap present happens BEFORE
    // the guest Swap runs — same order as the PM4 arm (renderer first, then the
    // frame descriptor from the stream) — and Swap itself always calls through
    // because the completion protocol is the guest's (kickoff trap 2).
    case kH_Swap:
        if (DrawMode() && D3dDraw_Enabled())
            D3dDraw_OnSwap(base);
        return ServiceResult::CallThrough;
    case kH_RingReserve:
        // Serviced ONLY while a draw-service redirect is active on this thread; the
        // real reserve runs untouched everywhere else (see d3d_draw.h).
        if (DrawMode() && D3dDraw_ServiceReserve(ctx, base))
            return ServiceResult::Serviced;
        return ServiceResult::CallThrough;
    case kH_PreSwapResolve:
        // RETRACTION of the Phase A name and of phase C's routing. sub_82841AD0
        // RESOLVES NOTHING. Read end to end it emits exactly four things, and every
        // one of them is the GPU/CPU hand-off protocol of part 2's rule:
        //     type-0 write reg 0x0579 = 1
        //     WAIT_REG_MEM on [[dev+0x2A94]]+4          (the ISR mirror)
        //     sub_82845BA0 — the callback ARM + INTERRUPT + re-poison block
        //     a second WAIT_REG_MEM on the same mirror word
        // There is no draw, no clear, no copy, no state. It was in the Redirect group
        // because the Phase A table called it a resolve, and phase C grouped it with
        // the content it was named after rather than with the packets it emits.
        //
        // The cost of that was measurable and was measured: with it redirected, every
        // one of a boot's 405 callback armings landed at cursor BFBEB024 — inside the
        // private scratch (`[fence] arm ... SCRATCH`) — so the walker had to deliver
        // them, and every walker delivery was the frame tick, which is precisely the
        // picture part 2 diagnosed and fixed for sub_82846288 alone. Four functions in
        // this image emit that block; part 2 moved one of them.
        //
        // Same-binary control arm, same discipline as CZ_D3D_NO_RESERVE_KICK: with
        // CZ_D3D_REDIRECT_PRESWAP=1 the call goes back into the Redirect group, so
        // "does emitting this block where its reader lives matter" is one environment
        // variable rather than one rebuild.
        {
            static const bool preFix = [] {
                const char* v = getenv("CZ_D3D_REDIRECT_PRESWAP");
                const bool on = v && *v && *v != '0';
                if (on)
                    fprintf(stderr, "[d3d] CZ_D3D_REDIRECT_PRESWAP=1 — sub_82841AD0's "
                                    "callback-arm block goes into the private SCRATCH "
                                    "again (the pre-fix arm)\n");
                return on;
            }();
            if (DrawMode() && D3dDraw_Enabled() && !preFix)
                return ServiceResult::RealRing;
        }
        break; // phase B keeps its no-op service, in the content switch below
    case kH_FrameEndAsyncSubmit:
        // Measured, and the reason this row exists: over one boot the fence probe
        // caught all six of its calls running with cursor=BFBEB014 — the PRIVATE
        // SCRATCH — because its only redirected caller is Resolve. Everything it does
        // downstream measures against dev+0x30, including sub_82845DE0's segment
        // extent ([dev+0x3B20], [dev+0x30]+4), so under a redirect it reasons about a
        // buffer whose only consumer is our walker while handing the results to the
        // title's own worker.
        if (DrawMode() && D3dDraw_Enabled())
        {
            if (FenceProbe_Line())
            {
                const uint32_t dev = ctx.r3.u32;
                const uint32_t cursor = PPC_LOAD_U32(dev + 0x30);
                uint32_t sva = 0, sbytes = 0;
                D3dDraw_ScratchRange(sva, sbytes);
                fprintf(stderr, "[fence] fsubmit dev=%08X tiles=%u cursor=%08X%s "
                                "counter=%d 3460=%08X 2ABD=%02X\n",
                        dev, ctx.r4.u32, cursor,
                        (sva && cursor >= sva && cursor < sva + sbytes) ? " SCRATCH" : "",
                        int32_t(PPC_LOAD_U32(dev + 0x2B04)), PPC_LOAD_U32(dev + 0x3460),
                        PPC_LOAD_U8(dev + 0x2ABD));
            }
            return ServiceResult::RealRing;
        }
        return ServiceResult::CallThrough;
    case kH_Fence_82846288_q:
        // The Phase A label was "fence/throttle-shaped"; the disassembly says it is the
        // CALLBACK ARMER — it forwards to sub_82845BA0, which lays down the
        // arm / WAIT_REG_MEM x3 / INTERRUPT / re-poison block that hands a callback to
        // the graphics ISR. Its three callbacks over a boot are the frame tick
        // (82841878), the job ticks (827CC628/827CC640) and, once tiled rendering
        // starts, sub_8284AAD0 — the one that wakes the D3D worker. That block belongs
        // in the ring, never in our scratch.
        if (DrawMode() && D3dDraw_Enabled())
            return ServiceResult::RealRing;
        return ServiceResult::CallThrough;
    case kH_InsertCallback_q:
        // The engine's per-frame GPU sync (retraction of the Phase A label: this is
        // a WAIT wrapper over sub_82845160, not an emitter — its r3 is the fence
        // TARGET).
        //
        // In draw mode, CLOSE AND KICK the pending command segment first, with the
        // guest's own reserve. The wait's own head does this only when the target is
        // the NEWEST fence; on hardware that suffices because ordinary emission
        // fills and closes segments continuously. Redirected emission changes that
        // arithmetic: the content bytes never enter the real segments, so at
        // content-heavy eras (the boot movie) the segment holding the last few
        // fences' EVENT_WRITEs stays open forever and the wait deadlocks — measured
        // as `target=1307 completed=1301`, twelve emitted-but-unkicked fences, with
        // the CP fully caught up. Forcing the guest's own close/kick here preserves
        // ordering (everything in the segment precedes the kick) and invents no
        // values; it is the same call the wait head itself makes in its own case.
        if (DrawMode() && D3dDraw_Enabled()) {
            const uint32_t slot = PPC_LOAD_U32(0x82000758);
            const uint32_t dev = slot ? PPC_LOAD_U32(slot) : 0;
            if (dev) {
                PPCContext saved = ctx;
                ctx.r3.u64 = dev;
                __imp__sub_82845F68(ctx, base);
                ctx = saved;
            }
        }
        if (DrawMode()) {
            static std::atomic<uint64_t> n{0};
            const uint64_t k = n.fetch_add(1);
            const uint32_t slot = PPC_LOAD_U32(0x82000758);
            const uint32_t dev = slot ? PPC_LOAD_U32(slot) : 0;
            const uint32_t emitted = dev ? PPC_LOAD_U32(dev + 0x2A9C) : 0;
            const uint32_t completed = dev && PPC_LOAD_U32(dev + 0x2A90)
                                           ? PPC_LOAD_U32(PPC_LOAD_U32(dev + 0x2A90)) : 0;
            // First few for the cadence, plus every entry whose fence lag is
            // anomalous — the entry that never returns is the one that matters.
            // dev+0x3460 is the async-submission flag: nonzero makes the reserve
            // SKIP its close/kick (segments queue for the worker instead).
            if (k < 12 || emitted - completed > 8)
                fprintf(stderr, "[d3d] sync-wait #%llu target=%u emitted=%u completed=%u "
                                "async3460=%08X\n",
                        (unsigned long long)k, ctx.r3.u32, emitted, completed,
                        dev ? PPC_LOAD_U32(dev + 0x3460) : 0);
        }
        return ServiceResult::CallThrough;
    default:
        break;
    }
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
        // Phase C: run the guest body under a redirected cursor and render its
        // emission. Falls back to phase B's no-op when the draw service could not
        // come up (it is loud about why).
        if (DrawMode() && D3dDraw_Enabled())
            return ServiceResult::Redirect;
        ctx.r3.u64 = 0;
        return ServiceResult::Serviced;
    default:
        return ServiceResult::CallThrough;   // everything else calls through
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
        /* Unconditional, and deliberately ABOVE both gates: these two feed the      */\
        /* always-on `ring: chain` line, and the PM4 control arm runs with neither   */\
        /* Observe() nor Replace() true. The conditions are compile-time constants,  */\
        /* so every other row in the table pays nothing (see cpu/chain_stats.h).     */\
        if (kH_##name == kH_Resolve)                                                 \
            ChainStats_CountResolve();                                               \
        if (kH_##name == kH_FrameEndAsyncSubmit)                                     \
            ChainStats_CountAsyncSubmit();                                           \
        /* Phase C part 10: who computes the per-draw bin mask, and what does it     */\
        /* decide? Behind CZ_BINMASK_PROBE, and only on this one row.                */\
        if (kH_##name == kH_FlushState_82838088_q)                                   \
            BinMaskNote(ctx);                                                        \
        if (Observe()) {                                                             \
            Note(kH_##name, ctx);                                                    \
            if (kH_##name == kH_Swap)                                                \
                SwapSummary();                                                       \
        }                                                                            \
        if (Replace()) {                                                             \
            switch (Service(kH_##name, ctx, base)) {                                 \
            case ServiceResult::Serviced:                                            \
                return;                                                              \
            case ServiceResult::Redirect:                                            \
                if (D3dDraw_ServiceContent(ctx, base, __imp__sub_##addr))            \
                    return;                                                          \
                break;                                                               \
            case ServiceResult::RealRing:                                            \
                if (D3dDraw_ServiceRealRing(ctx, base, __imp__sub_##addr))           \
                    return;                                                          \
                break;                                                               \
            case ServiceResult::CallThrough:                                         \
                break;                                                               \
            }                                                                        \
        }                                                                            \
        __imp__sub_##addr(ctx, base);                                                \
    }
CZ_D3D_HOOKS(X)
#undef X
