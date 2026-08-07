// The Vd* graphics kernel exports, the vblank interrupt, and the ring-buffer pump.
//
// Ground truth for every argument and every ordering claim below is A1's Vd block,
// quoted inline. Case Zero links D3D9 statically, so these exports are the ONLY
// visible seam between game code and the GPU — the D3D calls themselves are inlined
// into the title's own code and never cross a boundary we could hook.
//
// Phase 1 left every one of these an honest-failure stub, and the boot walked all of
// them in hardware's order anyway before stopping in the driver's ring free-space
// wait. That is what this module is for.
#include "vd.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <xbox.h>

#include "../cpu/chain_stats.h"
#include "../cpu/guest_thread.h"
#include "../kernel/guestcall.h"
#include "../kernel/heap.h"
#include "../kernel/klog.h"
#include "../kernel/memory.h"
#include "../kernel/xex_imports.h"
#include "pm4.h"
#include "vk_renderer.h"

// kernel/imports.cpp — the clock sources the timestamp bundle is refreshed from.
uint64_t KernelSystemTime();
uint64_t KernelInterruptTime();

namespace {

std::atomic<uint32_t> g_interruptCallback{ 0 };
std::atomic<uint32_t> g_interruptUserData{ 0 };
std::atomic<uint32_t> g_ringBufferBase{ 0 };
std::atomic<uint32_t> g_ringBufferSize{ 0 };
std::atomic<uint32_t> g_rptrWriteback{ 0 };
std::atomic<uint32_t> g_gpuIdentifier{ 0 };
std::atomic<bool> g_pumpRunning{ false };

// Physical addresses reach us through MmGetPhysicalAddress, which answers with
// `address & 0x1FFFFFFF`. Our physical arena is the cached view at 0xA0000000
// (kernel/heap.cpp), so the way back is a single OR — the exact inverse, which is
// what makes the round trip self-consistent.
//
// Note this is NOT Xenia's convention: A1 shows MmGetPhysicalAddress(E3D71000)
// answering 03D72000, a constant +0x1000 skew. Ours needs no skew because both ends
// are ours; the skew only matters when comparing a physical address in our log
// against the same object's address in a capture. See the note in gpu/pm4.cpp.
uint32_t PhysicalToVirtual(uint32_t physical)
{
    return 0xA0000000u | (physical & 0x1FFFFFFFu);
}

// The source-1 sink the command processor calls at each INTERRUPT packet.
//
// It has to be reachable from gpu/pm4.cpp, which knows nothing about guest threads
// and must stay that way. So the pump publishes what the ISR needs here before it
// walks the ring, and pm4.cpp calls back through a bare function pointer.
//
// Plain globals rather than atomics on purpose: only the pump thread ever writes
// them, and only the pump thread — inside its own Pm4_Execute call — ever reads
// them. Anything else reading these would be a bug, and an atomic would hide it.
struct PumpIsr
{
    PPCContext* context = nullptr;
    PPCFunc* func = nullptr;
    uint8_t* base = nullptr;
    uint32_t callback = 0;
    uint32_t userData = 0;
    uint64_t delivered = 0;
};
PumpIsr g_pumpIsr;

// What the guest's graphics ISR will read the instant we call it, for source 1.
//
// `sub_82844D38` is that ISR, and its source-1 path is four instructions long:
//
//     lwz  r11,10900(r4)    ; the scratch-register mirror's base
//     lwz  r10,16(r11)      ; SCRATCH_REG4 = a callback pointer
//     cmplwi cr6,r10,0
//     beq  <skip>           ; ZERO means nothing armed
//     lwz  r3,20(r11)       ; SCRATCH_REG5 = its argument
//     mtctr r10 ; bctrl
//
// Note what it does NOT check: anything but zero. The stream poisons REG4 with
// 0x0BADF00D once the callback has been consumed, and the ISR will happily call
// that. So "is the mirror armed or poisoned at the moment we deliver" is the whole
// question, and this is the instrument that answers it rather than reasoning about
// packet order.
void TraceIsrMirror(const char* when)
{
    if (!getenv("CZ_ISR_TRACE") || !g_pumpIsr.userData)
        return;
    static std::atomic<int> n{ 0 };
    const int i = n.fetch_add(1);
    if (i >= 24)
        return;
    uint8_t* base = g_memory.base;
    const uint32_t mirror = PPC_LOAD_U32(g_pumpIsr.userData + kDeviceIsrMirror);
    if (mirror < 0x1000)
        return;
    const uint32_t reg4 = PPC_LOAD_U32(mirror + 16);
    const uint32_t reg5 = PPC_LOAD_U32(mirror + 20);
    KLOG("isr[%d] %s mirror=%08X SCRATCH_REG4=%08X SCRATCH_REG5=%08X%s\n", i, when, mirror,
         reg4, reg5,
         reg4 == 0x0BADF00D ? "   <-- POISONED, the ISR will call 0x0BADF00D"
         : reg4 == 0        ? "   (unarmed, the ISR will skip)"
                            : "   (armed)");
}

// The graphics interrupt is addressed to a SET of hardware threads, not to one.
//
// The arm block's first packet is `SCRATCH_REG0 = mask`, and the mirror slot it writes
// is a six-bit acknowledge bitmap: the ISR clears `1 << PCR[0x10C]` out of it (at
// 82844D88-82844D98, under the dev+0x2A98 lock), and the arm block's TRAILING
// WAIT_REG_MEM holds the command processor until the whole word reads zero. So a
// delivery on one CPU acknowledges one bit, and an arm naming a CPU we never deliver on
// stalls the ring forever — which is exactly what a faithful CZ_PM4_STOP_ON_WAIT run
// does: it parks at `mem <mirror>|2 value=00000010 ref=0`, bit 4, while our single pump
// thread clears bit 2.
//
// The mask is chosen by whoever arms. `sub_82845BA0` takes it as `(flags >> 8) & 0x3F`
// and defaults to 4 (CPU 2) — which is where vd.cpp has always run the pump — but
// `sub_827D2FC0` arms with flags 0x1000, i.e. mask 0x10, CPU 4. The ISR's own body is
// per-CPU too: `sub_8284AAD0` pushes the token buffer onto the job ring at
// `dev + cpu*0x6C + 0x2C40`, so which CPU runs it decides which worker sees the kick.
//
// We have one guest thread for the graphics interrupt and are not going to grow six, so
// the honest stand-in is for that one thread to take the interrupt once per named CPU,
// reporting each CPU in its PCR while it does. CZ_ISR_SINGLE_CPU=1 is the same-binary
// control arm: the pre-fix behaviour, one delivery as whatever CPU the pump was
// constructed with.
const bool g_isrPerCpu = getenv("CZ_ISR_SINGLE_CPU") == nullptr;
std::atomic<uint64_t> g_isrPerCpuDeliveries{ 0 };

// See Vd_MirrorMutex in vd.h. Recursive: the walker's in-position delivery holds
// it across the guest ISR, whose callback can re-enter the walker's mirror writes
// on the same thread.
std::recursive_mutex g_mirrorMutex;

void DeliverCommandProcessorInterrupt()
{
    if (!g_pumpIsr.func)
        return;
    // Serialized against the D3D walker's mirror writes (engine thread): the ISR
    // reads the mirror, and a concurrent arm/poison from the other stream landing
    // mid-read is a race hardware's single stream never has.
    std::lock_guard<std::recursive_mutex> mlock(g_mirrorMutex);
    TraceIsrMirror("at INTERRUPT packet");
    // There used to be a guard here that DECLINED a source-1 delivery whose mirror held
    // the consumed-callback poison 0x0BADF00D, because our command processor could run
    // ahead of the CPU-side handshake and hand the guest a state hardware never
    // produces (`ctr=0BADF00D`, ~2 crashes in 10). Phase C parts 4-6 removed the
    // premise: the CP now STALLS at the hand-off block's WAIT_REG_MEM until the CPU has
    // acknowledged, so it cannot arrive early any more. The counter read ZERO across
    // all 40 runs of part 6's promotion table INCLUDING the brake-off arm, and zero
    // again on part 12's draw arm at #83 — so this is a guard against a race the brake
    // closed, deleted only after its zero had been re-confirmed in the CURRENT regime
    // (gotcha 182).
    if (g_pumpIsr.delivered++ == 0)
        KLOG("delivering first command-processor interrupt to %08X(1, %08X) — the ring "
             "is being consumed\n",
             g_pumpIsr.callback, g_pumpIsr.userData);

    // The interrupt is addressed to a SET of hardware threads, and every one of them
    // has to run the ISR — see g_isrPerCpu above for why one delivery is not enough.
    // The mask is the low six bits of the mirror's first word, written by the arm
    // block's own `SCRATCH_REG0 = mask` packet.
    uint8_t* base = g_memory.base;
    const uint32_t mirror = PPC_LOAD_U32(g_pumpIsr.userData + kDeviceIsrMirror);
    uint32_t mask = 0;
    if (g_isrPerCpu && mirror >= 0x1000 && mirror < PPC_MEMORY_SIZE - 24)
        mask = PPC_LOAD_U32(mirror) & 0x3F;

    g_pumpIsr.context->r3.u64 = 1; // source 1: command processor
    g_pumpIsr.context->r4.u64 = g_pumpIsr.userData;
    if (mask == 0)
    {
        ChainStats_CountIsr();
        g_pumpIsr.func(*g_pumpIsr.context, g_pumpIsr.base);
        return;
    }

    // One delivery per bit, with this thread's PCR reporting that CPU for the
    // duration. Restored afterwards so every other consumer of PCR+0x10C on this
    // thread — and there are several, the critical-section backoff among them —
    // sees the pump's real identity again.
    const uint32_t pcr = uint32_t(g_pumpIsr.context->r13.u32);
    const uint8_t saved = base[pcr + 0x10C];
    for (uint32_t cpu = 0; cpu < 6; ++cpu)
    {
        if ((mask & (1u << cpu)) == 0)
            continue;
        base[pcr + 0x10C] = uint8_t(cpu);
        g_pumpIsr.context->r3.u64 = 1;
        g_pumpIsr.context->r4.u64 = g_pumpIsr.userData;
        ChainStats_CountIsr();
        g_pumpIsr.func(*g_pumpIsr.context, g_pumpIsr.base);
        g_isrPerCpuDeliveries.fetch_add(1, std::memory_order_relaxed);
    }
    base[pcr + 0x10C] = saved;
}

// The guest graphics ISR, delivered on its own thread — and the thread the command
// processor runs on.
//
// The interrupt has a source argument: 0 = vblank, 1 = command-processor completion.
// Source 1 means "a packet you submitted has retired", so it is raised only from an
// INTERRUPT (0x54) packet actually reached in the stream — never from a timer.
// Case Zero issues 4,138 of them across B1's 1,089 frames.
//
// The thread gets its own GuestThreadContext (its own PCR/TLS/TEB and stack) because
// it runs recompiled guest code, and CPU 2 because that is where the 360 routes the
// graphics interrupt.
void GraphicsInterruptPump()
{
    GuestThreadContext threadContext(2);
    uint8_t* base = g_memory.base;

    // CZ_VBLANK_MS: the interrupt cadence, 16 ms ~= the 360's 60 Hz vblank. An env
    // knob rather than a constant because it is the first thing to vary when a timing
    // symptom appears: if a guest behaviour is quantised to a multiple of this
    // number, halving it says so in one run. Gotcha 7 — an instrument needs a
    // control, and here the control is the same binary at a different cadence.
    const char* env = getenv("CZ_VBLANK_MS");
    const int vblankMs = env ? std::max(1, atoi(env)) : 16;

    KLOG("graphics interrupt pump started (%d ms cadence)\n", vblankMs);

    // Registered here rather than at construction: the sink runs guest code on THIS
    // thread's context, so it must not be reachable before the context exists.
    //
    // CZ_PM4_NO_CP_INTERRUPT=1 leaves it unregistered, so the ring is still consumed
    // and the read pointer is still published but source 1 is never raised. That is
    // the control for every claim of the form "the ISR did this": consuming the ring
    // and running the guest's completion callback are two separate changes this
    // module makes at once, and gotcha 7's rule is that an instrument needs a way to
    // be turned off in the same binary.
    if (!getenv("CZ_PM4_NO_CP_INTERRUPT"))
        Pm4_SetInterruptSink(DeliverCommandProcessorInterrupt);
    else
        KLOG("CZ_PM4_NO_CP_INTERRUPT: ring will be consumed but source 1 stays down\n");

    // Phase 5's renderer, brought up on the SAME thread that will drive it. Vulkan
    // objects here are used from one thread only, and that is this one — the pump —
    // so creating them anywhere else would work until the day it did not.
    //
    // Registering the draw sink only when Init succeeds is what keeps CZ_VKDRAW a true
    // control arm: with the renderer off, DRAW packets take the same path they took in
    // phase 4 (counted, otherwise inert), and nothing in the executor branches on a
    // renderer that is not there.
    if (VkRenderer_Init())
        Pm4_SetDrawSink(VkRenderer_Draw);

    uint64_t ticks = 0;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(vblankMs));

        // Keep the exported KeTimeStampBundle current before waking the guest. The
        // kernel refreshes it on every clock interrupt, and titles read the struct
        // directly rather than calling KeQuerySystemTime per frame — left frozen at
        // zero, the guest's own clock never advances.
        if (const uint32_t bundle = g_keTimeStampBundle.load())
        {
            const uint64_t interruptTime = KernelInterruptTime();
            PPC_STORE_U64(bundle + 0, interruptTime);
            PPC_STORE_U64(bundle + 8, KernelSystemTime());
            PPC_STORE_U32(bundle + 16, static_cast<uint32_t>(interruptTime / 10000));
        }

        const uint32_t callback = g_interruptCallback.load();
        const uint32_t userData = g_interruptUserData.load();
        if (!callback)
            continue;

        // Resolved before the ring walk, not after it: the walk can itself raise
        // source-1 interrupts, so the ISR has to be in hand first.
        PPCFunc* func = g_memory.FindFunction(callback);
        if (!func)
        {
            static bool complained = false;
            if (!complained)
            {
                complained = true;
                KLOG("graphics interrupt callback %08X was not recompiled — no "
                     "interrupts will be delivered\n",
                     callback);
            }
            continue;
        }

        // -------------------------------------------------------------------
        // The command processor.
        //
        // Take the write pointer from the driver's own mirror at
        // [device + kDeviceKickedWptr], then publish where we actually got to. Note
        // this title's mirror already carries a ring-RELATIVE index (the kick site
        // masks it with the ring size before storing), which Pm4_Execute's `% ring`
        // handles either way.
        //
        // Publishing the parser's real position rather than the write pointer is the
        // whole discipline: the guest is entitled to recycle everything behind the
        // read pointer, so a read pointer that runs ahead of the parser hands it
        // permission to overwrite packets we have not read yet.
        if (userData && Pm4_RingInitialized())
        {
            g_pumpIsr = { &threadContext.ppcContext, func, base, callback, userData,
                          g_pumpIsr.delivered };
            // Republished every tick, not once: the guest can re-register the writeback
            // block, and pm4.cpp watching a stale address would make the fence
            // experiment arm silently watch nothing (gotcha 25's shape).
            Pm4_SetFenceWord(PPC_LOAD_U32(userData + kDeviceWritebackPtr));
            const uint32_t kickedWptr = PPC_LOAD_U32(userData + kDeviceKickedWptr);
            const uint32_t cursor = Pm4_Execute(base, kickedWptr);
            if (const uint32_t slot = g_rptrWriteback.load())
                PPC_STORE_U32(slot, cursor);

        }

        // CZ_RING_TRACE=1: the words the command processor runs on, sampled once a
        // second from the driver's own device struct — including the MMIO dword we
        // deliberately do NOT use, so "the mirror and the register agree" stays a
        // measurement rather than an inherited assumption (see vd.h).
        if (getenv("CZ_RING_TRACE") && userData && (ticks % (1000 / vblankMs)) == 0)
        {
            const uint32_t kickedWptr = PPC_LOAD_U32(userData + kDeviceKickedWptr);
            const uint32_t writebackPtr = PPC_LOAD_U32(userData + kDeviceWritebackPtr);
            const uint32_t rptr =
                writebackPtr >= 0x1000 ? PPC_LOAD_U32(writebackPtr) : 0xFFFFFFFFu;
            // Both slots, and the parser's own cursor. The first version of this line
            // printed only the address the DRIVER dereferences and the address the
            // guest REGISTERED, never noticing they are not the same address — the
            // driver reads [dev+10896], the registration hands us that block plus
            // 0x3C, and a trace that shows one number for "the read pointer" cannot
            // show a write landing in the wrong one.
            const uint32_t registered = g_rptrWriteback.load();
            KLOG("ring: kickedWptr=%08X (dev+%u)  writebackPtr=%08X (dev+%u) [wb+0]=%08X | "
                 "registered=%08X [reg+0]=%08X | cursor=%u scratch=%08X umsk=%08X | "
                 "mmio CP_RB_WPTR=%08X\n",
                 kickedWptr, kDeviceKickedWptr, writebackPtr, kDeviceWritebackPtr, rptr,
                 registered, registered >= 0x1000 ? PPC_LOAD_U32(registered) : 0xFFFFFFFFu,
                 Pm4_Cursor(), Pm4_ScratchAddr(), Pm4_ScratchUmsk(),
                 PPC_LOAD_U32(kCpRbWptrAddress));
            // What the command processor has made of it. "rptr chasing wptr" is the
            // health check: equal means caught up, frozen behind a rising wptr means
            // the parser is stuck — which gpu/pm4.cpp reports separately and loudly
            // rather than skipping ahead.
            // `predicated` is on this line and not in a probe because it is a third of
            // the draw packets in this title: the scene is rendered in two tiles and the
            // bin mask decides which tile each draw belongs to, so "the renderer only
            // got N draws" is meaningless without it.
            KLOG("ring: pm4 packets=%llu frames(XE_SWAP)=%llu draws=%llu "
                 "(predicated out=%llu) interrupts=%llu\n",
                 (unsigned long long)Pm4_PacketCount(), (unsigned long long)Pm4_FrameCount(),
                 (unsigned long long)Pm4_DrawCount(),
                 (unsigned long long)Pm4_DrawsPredicatedOut(),
                 (unsigned long long)Pm4_InterruptCount());
            // Truncated indirect buffers, on the same line as the healthy counters
            // because that is the pairing that matters: three green numbers and a
            // silent fourth is what let finding 38's dropped fences run for a whole
            // port unnoticed. Any nonzero value is a hang waiting for a thread to
            // wait on it.
            // The bin census, when it is on. On the ring trace rather than in its own
            // flag because its only use is to be read next to `predicated out=` above:
            // a third of this title's draw packets are discarded here and B1 says
            // hardware discards 0.3%, so the pair table is where that gap is localised.
            Pm4_BinCensusReport();
            KLOG("ring: indirect buffers truncated=%llu | verify clean=%llu dirty=%llu\n",
                 (unsigned long long)Pm4_IbTruncatedCount(),
                 (unsigned long long)Pm4_IbVerifyCleanCount(),
                 (unsigned long long)Pm4_IbVerifyDirtyCount());
            // The brake's own health. `held` is how often it stopped a walk; `streak`
            // is how many ticks in a row it has been stopped at the SAME wait, which is
            // the number that separates a title pacing itself (streak of a few) from a
            // ring nothing will ever release (streak without bound). Every other
            // counter on these lines reads identically in both cases (gotcha 81).
            KLOG("ring: waits unmet=%llu held=%llu streak=%llu max=%llu%s\n",
                 (unsigned long long)Pm4_WaitUnmetCount(),
                 (unsigned long long)Pm4_RingHeldCount(),
                 (unsigned long long)Pm4_HoldStreak(),
                 (unsigned long long)Pm4_HoldStreakMax(),
                 Pm4_HoldStreak() > 60 ? "   <-- the ring has sat on ONE wait for over a "
                                         "second: nothing is going to release it"
                                       : "");
            // The GPU/CPU hand-off chain, link by link (cpu/chain_stats.h). Read it as
            // a chain of RATIOS: arms -> ints is how many times the command processor
            // executed each arm block, ints -> isr is the per-CPU acknowledge's own
            // multiplier, kicks -> walks -> ringsub is what one delivery regenerates.
            // `distinct` is whether the token-buffer pointer ADVANCES, which is the
            // difference between a pipeline and a runaway (gotcha 150).
            const ChainStats cs = ChainStats_Read();
            KLOG("ring: chain arms=%llu ints=%llu isr=%llu kicks=%llu (distinct=%llu%s "
                 "repeat=%llu) walks=%llu drains=%llu segsub=%llu/queued=%llu "
                 "ringsub=%llu/ents=%llu resolve=%llu/reseed=%llu\n",
                 (unsigned long long)cs.arms, (unsigned long long)Pm4_InterruptCount(),
                 (unsigned long long)cs.isr, (unsigned long long)cs.kicks,
                 (unsigned long long)cs.kickDistinct, cs.kickDistinct >= 4096 ? "+" : "",
                 (unsigned long long)cs.kickRepeat, (unsigned long long)cs.walks,
                 (unsigned long long)cs.drains, (unsigned long long)cs.segSubmit,
                 (unsigned long long)cs.segQueued, (unsigned long long)cs.ringsub,
                 (unsigned long long)cs.ringsubEnts, (unsigned long long)cs.resolves,
                 (unsigned long long)cs.asyncSubmit);
            // The counter the engine's frame sync SPINS on, printed SIGNED — because
            // two sessions described it as "not yet back to zero" and the word held
            // -552, so the loop was unsatisfiable rather than slow (gotcha 145). It
            // belongs on the same line as the chain because the chain's producer half
            // freezing and this going negative are one event seen from two sides.
            //
            // dev+0x2B00 is the token interpreter's nesting depth and dev+0x2B04 the
            // outstanding-segment count; the ISR's user data IS the device struct, so
            // both are free here.
            //
            // The refusal count is here rather than only in the experiment arm's own
            // print, because that print is CAPPED at four and a capped print is not a
            // count (gotcha 109). It reads 0 unless CZ_PM4_FENCE_MONOTONIC is on.
            KLOG("ring: engine counter[dev+2B04]=%d depth[dev+2B00]=%d "
                 "fenceRegressionsRefused=%llu\n",
                 int(PPC_LOAD_U32(userData + 0x2B04)), int(PPC_LOAD_U32(userData + 0x2B00)),
                 (unsigned long long)Pm4_FenceRegressionCount());
        }

        // Log the first delivery BEFORE the call, not after. A guest ISR that never
        // returns is a real possibility (it takes driver locks and can wait), and an
        // "after" line cannot tell "the interrupt never fired" apart from "the
        // interrupt fired and hung" — which are opposite bugs.
        if (ticks == 0)
            KLOG("delivering first vblank to %08X(0, %08X)\n", callback, userData);

        // The display controller's gate (vd.h). Asserted before the ISR runs, because
        // the ISR reads it and the whole point is that the swap-queue walker should
        // execute on this delivery. Re-asserted every tick rather than once: nothing
        // in the image ever writes this address, so a single store would do — and a
        // store that only ever happened once is a store that stops being true the day
        // something else clears the page.
        //
        // CZ_NO_VBLANK_GATE=1 is the same-binary control arm: the pre-fix runtime, in
        // which the walker never runs. Every claim about this change is measured
        // against it rather than against a remembered number (gotcha 86).
        static const bool vblankGate = getenv("CZ_NO_VBLANK_GATE") == nullptr;
        if (vblankGate)
            PPC_STORE_U32(kDisplayControllerGate,
                          PPC_LOAD_U32(kDisplayControllerGate) | 1);

        // CZ_SWAPQ_TRACE=1 — the swap queue once a second. Head and tail are the
        // measurement that separates "the walker has nothing to do" from "the walker
        // never ran": a tail climbing away from a pinned head is a queue of flips the
        // title is waiting on. `wait` is the rendezvous word the command processor's
        // WAIT_REG_MEM polls, printed beside it because the walker's zero-surface case
        // is what clears it.
        if (getenv("CZ_SWAPQ_TRACE") && userData && (ticks % (1000 / vblankMs)) == 0)
        {
            const uint32_t mirror = PPC_LOAD_U32(userData + kDeviceIsrMirror);
            const uint32_t head = PPC_LOAD_U32(userData + kDeviceSwapHead);
            // The head record's own fields. The walker stops at the first record whose
            // due tick is in the future, so "why did head stop moving" is answerable
            // only from the record it stopped ON — a head/tail pair alone says the
            // queue is stuck and nothing about what is holding it.
            const uint32_t rec = userData + kDeviceSwapQueue + (head & 15) * 8;
            KLOG("swapq: gate=%08X tick=%u done=%u head=%u tail=%u "
                 "head{surface=%08X due=%u} | mirror=%08X wait[mirror+4]=%08X | "
                 "D1GRPH_PRIMARY_SURFACE=%08X | ack[mirror+0]=%08X percpu=%llu\n",
                 PPC_LOAD_U32(kDisplayControllerGate),
                 PPC_LOAD_U32(userData + kDeviceVblankTick),
                 PPC_LOAD_U32(userData + kDeviceFlipsDone), head,
                 PPC_LOAD_U32(userData + kDeviceSwapTail), PPC_LOAD_U32(rec),
                 PPC_LOAD_U32(rec + 4), mirror,
                 mirror >= 0x1000 ? PPC_LOAD_U32(mirror + 4) : 0xFFFFFFFFu,
                 PPC_LOAD_U32(kD1GrphPrimarySurfaceAddress),
                 mirror >= 0x1000 ? PPC_LOAD_U32(mirror) : 0xFFFFFFFFu,
                 (unsigned long long)g_isrPerCpuDeliveries.load());
        }

        threadContext.ppcContext.r3.u64 = 0; // source 0: vblank
        threadContext.ppcContext.r4.u64 = userData;
        func(threadContext.ppcContext, base);

        if (++ticks == 1 || (ticks % (1000 / vblankMs)) == 0)
            KLOG("vblank #%llu delivered to %08X(0, %08X)\n", (unsigned long long)ticks,
                 callback, userData);
    }
}

// ---------------------------------------------------------------------------
// Device / engine setup
// ---------------------------------------------------------------------------

// A1: VdInitializeEngines(24700000, 8284C770, 00000000, 820C0EA8(00C60400), ...)
// The second argument is a guest callback the kernel would invoke on engine events.
// Xenia does nothing with any of them and the title proceeds, so doing nothing is a
// measured answer rather than a shrug — but log the arguments, because the callback
// address is the only record of a second graphics entry point existing at all.
void VdInitializeEngines_x(uint32_t unk0, uint32_t callback, uint32_t unk1, uint32_t pfnUnk0,
                           uint32_t pfnUnk1)
{
    KLOG("VdInitializeEngines(%08X, callback=%08X, %08X, %08X, %08X)\n", unk0, callback, unk1,
         pfnUnk0, pfnUnk1);
}

void VdShutdownEngines_x() { KLOG("VdShutdownEngines\n"); }

// A1: VdSetGraphicsInterruptCallback(82844D38, 40001D80). The second argument is the
// driver's device struct, handed back to the ISR in r4 — and the struct vd.h reads
// the ring pointers out of.
void VdSetGraphicsInterruptCallback_x(uint32_t callback, uint32_t userData)
{
    KLOG("VdSetGraphicsInterruptCallback cb=%08X userData=%08X\n", callback, userData);
    g_interruptUserData = userData;
    g_interruptCallback = callback;

    bool expected = false;
    if (g_pumpRunning.compare_exchange_strong(expected, true))
        std::thread(GraphicsInterruptPump).detach();
}

// A1: MmGetPhysicalAddress(E3D71000) -> VdInitializeRingBuffer(03D72000, 14).
//
// THE SIZE ARGUMENT, DERIVED RATHER THAN GUESSED. The guest computes it right in
// front of the call (`sub_82846210`):
//
//     cntlzw r11,r25        ; r11 = clz(ring size in BYTES)
//     subfic r23,r11,28     ; r23 = 28 - clz(size)
//     ...                   ; -> VdInitializeRingBuffer(phys, r23)
//
// For a power-of-two size S, clz(S) = 31 - log2(S), so the argument is
// log2(S) - 3 = log2(S/8) — the log2 of the ring size in QUADWORDS. Hence
// size = 1 << (arg + 3). Case Zero passes 14, so the ring is 0x20000 = 128 KB, and
// A1 confirms it independently: the MmAllocatePhysicalMemoryEx immediately before is
// for exactly 0x20000 bytes. Getting the factor of 8 wrong here is silent until the
// command processor wraps.
//
// (The same numbers appear to overrun the allocation by 0x1000 if you assume Xenia's
// physical addresses are `virtual & 0x1FFFFFFF`. They are not — there is a +0x1000
// skew, and with it the ring starts exactly at the allocation base. See pm4.cpp.)
void VdInitializeRingBuffer_x(uint32_t basePhysical, uint32_t sizeLog2)
{
    const uint32_t base = PhysicalToVirtual(basePhysical);
    const uint32_t size = 1u << (sizeLog2 + 3);
    KLOG("VdInitializeRingBuffer phys=%08X (virtual %08X) sizeLog2=%u -> %u bytes\n",
         basePhysical, base, sizeLog2, size);
    g_ringBufferBase = base;
    g_ringBufferSize = size;
    // Handing the ring to the command processor resets its cursor: a re-init is a new
    // stream, not a continuation of the old one.
    Pm4_SetRingBuffer(base, size);
    if (getenv("CZ_PM4_BIN_CENSUS"))
        Pm4_BinCensusEnable();
}

// A1: VdEnableRingBufferRPtrWriteBack(03D7103C, 8) — again a physical address,
// pointing 0x3C into the 0x1000 block allocated just after the interrupt callback
// was registered.
//
// This slot is where real hardware publishes how far it has read into the ring, and
// it is what the driver's free-space wait dereferences. Writing it with the position
// gpu/pm4.cpp actually parsed to — never with the write pointer, which is the shape
// the fake would have taken — is what releases that wait honestly.
void VdEnableRingBufferRPtrWriteBack_x(uint32_t slotPhysical, uint32_t blockSizeLog2)
{
    const uint32_t slot = PhysicalToVirtual(slotPhysical);
    KLOG("VdEnableRingBufferRPtrWriteBack phys=%08X (virtual %08X) blockSizeLog2=%u\n",
         slotPhysical, slot, blockSizeLog2);
    g_rptrWriteback = slot;
}

// A1 calls this twice: first with 00000000 (before the block exists) and then with
// E3D70008 — note that one is a *virtual* address, not a physical one, unlike the two
// calls above. Recording it verbatim is therefore correct; do not mask it.
void VdSetSystemCommandBufferGpuIdentifierAddress_x(uint32_t address)
{
    KLOG("VdSetSystemCommandBufferGpuIdentifierAddress addr=%08X\n", address);
    g_gpuIdentifier = address;
}

} // namespace

// ---------------------------------------------------------------------------
// Display queries
// ---------------------------------------------------------------------------

void Vd_FillVideoMode(XVIDEO_MODE* mode)
{
    if (!mode)
        return;
    memset(mode, 0, sizeof(*mode));
    mode->DisplayWidth = 1280;
    mode->DisplayHeight = 720;
    mode->IsInterlaced = 0;
    mode->IsWidescreen = 1;
    mode->IsHighDefinition = 1;
    mode->RefreshRate = 0x42700000; // 60.0f
    mode->VideoStandard = 1;        // NTSC-M
    mode->Unknown4A = 0x4A;
    mode->Unknown01 = 0x01;
}

namespace {

void VdQueryVideoMode_x(XVIDEO_MODE* mode) { Vd_FillVideoMode(mode); }

// XGetVideoMode lives here, next to VdQueryVideoMode, and both call one filler.
// A1 shows the guest calling XGetVideoMode twice and VdQueryVideoMode three times
// during the same display bring-up, and the driver's letterbox arithmetic straddles
// the two — two independent copies is how they drift. (It was temporarily in
// kernel/imports.cpp during phase 1, before this module existed.)
void XGetVideoMode_x(XVIDEO_MODE* mode) { Vd_FillVideoMode(mode); }

// Bit 0 = widescreen, bit 1 = display width >= 1024. Both hold for the 1280x720 mode
// we report; the two answers have to agree or the guest's letterbox arithmetic
// disagrees with its own render target size.
uint32_t VdQueryVideoFlags_x() { return 1 | 2; }

// X_DISPLAY_INFO, 0x58 bytes, layout from Xenia's xboxkrnl_video.cc.
// A1: VdGetCurrentDisplayInformation(7018F520), immediately after VdSetDisplayMode.
void VdGetCurrentDisplayInformation_x(uint8_t* info)
{
    if (!info)
        return;
    auto at16 = [&](size_t off) { return reinterpret_cast<be<uint16_t>*>(info + off); };
    auto at32 = [&](size_t off) { return reinterpret_cast<be<uint32_t>*>(info + off); };

    memset(info, 0, 0x58);
    *at16(0x00) = 1280;        // front buffer width
    *at16(0x02) = 720;         // front buffer height
    *at32(0x08 + 0x08) = 1280; // scaler source rect x2
    *at32(0x08 + 0x0C) = 720;  // scaler source rect y2
    *at32(0x08 + 0x10) = 1280; // scaled output width
    *at32(0x08 + 0x14) = 720;  // scaled output height
    *at32(0x08 + 0x18) = 1;    // vertical filter type
    *at32(0x08 + 0x28) = 1;    // horizontal filter type
    *at16(0x40) = 320;         // overscan left
    *at16(0x42) = 180;         // overscan top
    *at16(0x44) = 320;         // overscan right
    *at16(0x46) = 180;         // overscan bottom
    *at16(0x48) = 1280;        // display width
    *at16(0x4A) = 720;         // display height
    *reinterpret_cast<be<float>*>(info + 0x4C) = 60.0f;
    *at16(0x56) = 1280;        // actual display width
}

// A1: VdGetCurrentDisplayGamma(7018EB14(00000000), ...). Xenia's kernel config
// reports kernel_display_gamma_type = 2 and kernel_display_gamma_power = 2.22222233,
// which is where these two numbers come from rather than from a guess.
void VdGetCurrentDisplayGamma_x(be<uint32_t>* type, be<float>* gamma)
{
    if (type)
        *type = 2;
    if (gamma)
        *gamma = 2.22222233f;
}

// A1: VdSetDisplayMode(40000000). Returns success; the mode we would program is the
// one VdQueryVideoMode already reports.
uint32_t VdSetDisplayMode_x(uint32_t mode)
{
    KLOG("VdSetDisplayMode(%08X)\n", mode);
    return 0;
}

// A1: VdPersistDisplay(7018F648, 7018F630(00000000)), and the very next Vd-era call
// is MmFreePhysicalMemory.
//
// That pairing is the whole specification, and it is not what the name suggests. The
// out-parameter is not a handle to anything the caller keeps — it is a PHYSICAL
// ALLOCATION THE CALLER IMMEDIATELY FREES. (Xenia's implementation carries the same
// note: "unk1_ptr needs to be populated with a pointer passed to
// MmFreePhysicalMemory(1, *unk1_ptr)".) Persisting the front buffer across a title
// transition is something we genuinely cannot do, but the memory half of the contract
// is real and is the half the guest can observe.
//
// Asura's Wrath shipped the "we persist nothing, so say so" answer — return 0 with a
// null out-parameter — and it left the guest with nothing to free; the missing
// MmFreePhysicalMemory was the single divergence in an otherwise exact gate match.
// Finding 14's rule again: the out-parameter, not the status, is what the caller acts
// on.
uint32_t VdPersistDisplay_x(uint32_t unk, be<uint32_t>* blockOut)
{
    if (!blockOut)
        return 0;
    void* host = g_heap.AllocPhysical(64, 32);
    if (!host)
    {
        *blockOut = 0;
        return 0;
    }
    const uint32_t block = g_memory.MapVirtual(host);
    *blockOut = block;
    KLOG("VdPersistDisplay(%08X) -> caller-owned block %08X (nothing is actually "
         "persisted; the guest frees this immediately)\n",
         unk, block);
    return 1;
}

// ---------------------------------------------------------------------------
// EDRAM / command buffers
// ---------------------------------------------------------------------------

// A1: VdRetrainEDRAMWorker(00000000) then
//     VdRetrainEDRAM(00000001, 7018F0F0, 00001000, 7018F0F4, 7018F100, 00000800),
// and later the same call with a leading 0 — 1,131 calls in the boot. On hardware
// this recalibrates the EDRAM link; there is nothing to calibrate here. Returning 0
// (no retraining needed) is what Xenia answers and what the following
// VdIsHSIOTrainingSucceeded() query is consistent with.
uint32_t VdRetrainEDRAM_x(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                          uint32_t f)
{
    return 0;
}

uint32_t VdRetrainEDRAMWorker_x(uint32_t unk) { return 0; }

// The high-speed I/O link between GPU and EDRAM trained successfully. Answering 0
// here sends the driver down a hardware-fault path it has no way to recover from.
uint32_t VdIsHSIOTrainingSucceeded_x() { return 1; }

// A1: VdGetSystemCommandBuffer(7018F660, 7018F604) — 1,130 calls, tracking
// VdRetrainEDRAM's 1,131 almost exactly, so the two are on the same loop.
//
// There is no system command buffer in this runtime yet, so both out-parameters get a
// distinctive constant rather than being left as whatever was on the guest stack: if
// a value ever escapes into the PM4 stream, 0xBEEF0000 in a packet header is
// unmistakable in a census, where uninitialised stack garbage is not.
void VdGetSystemCommandBuffer_x(be<uint32_t>* bufferOut, be<uint32_t>* identifierOut)
{
    if (bufferOut)
    {
        memset(bufferOut, 0, 0x94);
        bufferOut[0] = 0xBEEF0000;
    }
    if (identifierOut)
        *identifierOut = 0xBEEF0001;
}

// A1: VdCallGraphicsNotificationRoutines(00000001, 7018F0E0), which Xenia annotates
// "scale 1280x720 -> 1280x720". Nothing has registered a notification routine with
// us, so there is nobody to call.
void VdCallGraphicsNotificationRoutines_x(uint32_t unk, uint32_t argsPtr)
{
    KLOG("VdCallGraphicsNotificationRoutines(%08X, %08X)\n", unk, argsPtr);
}

void VdEnableDisableClockGating_x(uint32_t enable) {}

} // namespace

// VdSwap — the frame boundary, and the one Vd* export that WRITES to the command
// stream rather than answering a question.
//
// It appears in no kernel log at any level: it is one of the 35 kHighFrequency
// exports Xenia hides from Case Zero (A1 contains the string exactly once, in the
// import table). So the only ground truth for it is the B1 GPU stream — where
// XE_SWAP appears **1,089 times across 1,089 frames**, exactly one per frame.
//
// The guest side is what makes leaving it alone dangerous: the caller reserves a
// block of command buffer, passes it in, and afterwards advances its own write
// pointer by the full reservation **regardless of what we return**. So the kernel is
// expected to fill that space, and the generated stub's "do nothing" submitted
// whatever the heap happened to hold as if it were packets.
//
// Ten arguments, so the last two arrive in the caller's parameter save area past the
// r3..r10 register set; written against the raw context for that reason.
PPC_FUNC(__imp__VdSwap)
{
    KCALL("VdSwap");

    const uint32_t buffer = ctx.r3.u32;
    const uint32_t fetchPtr = ctx.r4.u32;
    const uint32_t frontBufferPtr = ctx.r8.u32;
    // Slot n of the parameter save area is at r1 + 0x54 + n*8.
    const uint32_t widthPtr = *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 0 * 8);
    const uint32_t heightPtr = *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 1 * 8);

    if (!buffer)
    {
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t frontBuffer = frontBufferPtr ? PPC_LOAD_U32(frontBufferPtr) : 0;
    const uint32_t width = widthPtr ? PPC_LOAD_U32(widthPtr) : 1280;
    const uint32_t height = heightPtr ? PPC_LOAD_U32(heightPtr) : 720;

    uint32_t at = buffer;
    auto emit = [&](uint32_t dword) {
        PPC_STORE_U32(at, dword);
        at += 4;
    };

    // The front-buffer texture fetch constant, copied through verbatim rather than
    // rebuilt: the guest composed those six dwords itself and they encode the front
    // buffer's address, tiling and format. Re-deriving them would be us asserting a
    // surface layout we have not measured.
    emit(0x00054800); // type0, register 0x4800, six dwords, not single-register
    for (uint32_t i = 0; i < 6; i++)
        emit(fetchPtr ? PPC_LOAD_U32(fetchPtr + i * 4) : 0);

    emit(0xC0036400); // type3, opcode 0x64 XE_SWAP, four body dwords
    emit(0x53574150); // 'SWAP'
    emit(frontBuffer);
    emit(width);
    emit(height);

    // ...and then NO-OP THE REST OF THE RESERVATION. This is not tidiness; it is the
    // whole export (finding 39).
    //
    // The caller reserves a fixed 64 dwords and advances its write pointer by the full
    // reservation whether or not we fill it — its very next instructions after the
    // call are `addi r11,r29,256` / `stw r11,48(r31)`, 256 bytes, r3 never read. So
    // every dword between our last packet and the end of that block is submitted to
    // the command processor as if the kernel had put it there. Command buffers are
    // recycled, so what is actually there is the previous frame's packets, and the
    // parser walks into them and desyncs: it reads real headers at wrong offsets,
    // invents a length that runs past the end of the buffer, and stops — dropping
    // every remaining packet including the driver's own ring-progress fence, which is
    // the LAST packet in these buffers. One unfilled tail, one thread waiting forever
    // (findings 37-38).
    //
    // 0x80000000 is a type-2 PM4 packet: a one-dword no-op, the only encoding that
    // lets the parser cross an arbitrary run of dwords without interpreting any of
    // them. The title already knows this idiom — it prefills its scaler buffer with
    // exactly this value via RtlFillMemoryUlong (see
    // VdInitializeScalerCommandBuffer below).
    //
    // Two independent witnesses agree on 64, which is why it is written as a constant
    // rather than derived from an argument: the guest's own `addi r11,r29,256` above,
    // and B1, where every one of the 43 indirect buffers containing an XE_SWAP has
    // exactly **52** consecutive 0x80000000 packets immediately after it — 12 dwords
    // of real content plus 52 of padding.
    //
    // CZ_NO_SWAP_PAD=1 leaves the tail unfilled — the behaviour before finding 39, kept
    // as a same-binary control arm so "the padding is what fixed the stall" stays a
    // measurement (gotchas 7 and 50). It is an arm, not an option: with it on, the
    // command processor walks the previous frame's packets.
    static const bool noPad = getenv("CZ_NO_SWAP_PAD") != nullptr;
    constexpr uint32_t kSwapReservationDwords = 64;
    const uint32_t written = (at - buffer) / 4;
    if (!noPad)
        for (uint32_t i = written; i < kSwapReservationDwords; i++)
            emit(0x80000000);

    static std::atomic<uint64_t> swaps{ 0 };
    if (swaps.fetch_add(1) == 0)
        KLOG("VdSwap: first swap packet written to %08X (front buffer %08X, %ux%u, %u "
             "dwords + %u no-op dwords = %u reserved)\n",
             buffer, frontBuffer, width, height, written,
             kSwapReservationDwords - written, kSwapReservationDwords);

    ctx.r3.u64 = kSwapReservationDwords; // dwords written
}

// VdInitializeScalerCommandBuffer takes TWELVE arguments, four past the r3..r10
// register set, so the rest arrive in the caller's parameter save area. Written
// against the raw context rather than through GUEST_FUNCTION_HOOK deliberately: the
// marshaller's spill path has never been exercised by this port, and a phase whose
// gate is "the call sequence matches" is the wrong place to find out that argument
// nine was garbage.
//
// A1: VdInitializeScalerCommandBuffer(00000000, 050002D0, 00000000, 050002D0,
//     050002D0, 00000007, 7018EB9C, 00000007, 7018EBAC, 7018EEE0, 7018EBC0,
//     000000C8) — the last two are the destination buffer (which the title has just
//     filled with 0x80000000 via RtlFillMemoryUlong) and its size in dwords,
//     0xC8 == 200. The return value is the number of dwords actually written.
//
// We write none. The title's own prefill of 0x80000000 is a PM4 type-2 packet, i.e. a
// no-op the command processor skips, so an empty scaler buffer is inert rather than
// malformed — which is the property that makes returning 0 safe.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    KCALL("VdInitializeScalerCommandBuffer");
    const uint32_t destination =
        *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 2 * 8);
    const uint32_t dwords = *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 3 * 8);
    KLOG("VdInitializeScalerCommandBuffer(scaler=%08X, src=%08X, dst=%08X, %u dwords) -> "
         "0 dwords written (no scaler yet)\n",
         ctx.r3.u32, ctx.r4.u32, destination, dwords);
    ctx.r3.u64 = 0;
}

GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines_x)
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines_x)
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback_x)
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer_x)
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack_x)
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress,
                    VdSetSystemCommandBufferGpuIdentifierAddress_x)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, VdQueryVideoMode_x)
GUEST_FUNCTION_HOOK(__imp__XGetVideoMode, XGetVideoMode_x)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags_x)
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation_x)
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma_x)
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode_x)
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, VdPersistDisplay_x)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, VdRetrainEDRAM_x)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker_x)
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded_x)
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer_x)
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines,
                    VdCallGraphicsNotificationRoutines_x)
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating_x)

VdGraphicsState Vd_GetState()
{
    return VdGraphicsState{
        g_interruptCallback.load(), g_interruptUserData.load(), g_ringBufferBase.load(),
        g_ringBufferSize.load(),    g_rptrWriteback.load(),     g_gpuIdentifier.load(),
    };
}

bool Vd_PumpRunning() { return g_pumpRunning.load(); }

std::recursive_mutex& Vd_MirrorMutex() { return g_mirrorMutex; }

uint32_t Vd_InterruptCallbackVa() { return g_interruptCallback.load(); }
uint32_t Vd_InterruptUserData() { return g_interruptUserData.load(); }
