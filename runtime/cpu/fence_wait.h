// The Draw Thread's FENCE wait, PARKED instead of spun (perf plan part 107, item 2).
//
// WHY THIS EXISTS. The title's Draw Thread throttles itself against the GPU by waiting
// for a fence: sub_82845160 loops until the fence-completion word — the first dword of
// the D3D device's writeback block (dev+0x2A90 points at it; `kDeviceWritebackPtr`,
// the `g_fenceWord` gpu/pm4.cpp already recognises) — reaches a target sequence
// number, calling sub_8283C6C8 between checks, whose body is 32 `db16cyc` SMT-yield
// hints in a `bdnz` loop plus a 5,000-tick hang check. On the 360 those hints handed the
// core's other hardware thread the execution units; here they are no-ops, so the loop
// is a tight load/compare/call/return burning a whole core at ~94% (part 51,
// phase5-notes §6ch §1; phase1-notes finding 38 traced the same wait to a fence packet).
// Only OUR PM4 executor advances that word — it stores it when it executes the fence
// packet (`StoreGpuRaw`). On the operator's 8-core box the burning core is free. On the
// 4-core target it is a quarter of the machine, and on a 4c/8t part the spin sits on a
// SIBLING of a thread doing real work (the pump or a worker) and steals its execution
// resources — the one cost an 8-core measurement can never see (perf-plan-part107 §0.4).
//
// RETRACTION, RECORDED WHERE IT WAS CLAIMED. perf-plan-part107 item 2 and the first
// draft of this file called it the RING READ-POINTER wait and wired the wake to the
// read-pointer publication. A traced run showed the polled word at BC739A00 climbing by
// ones (FAC3, FAC9, FACF...) while the ring cursor sat at 0x460E: the word is the
// fence, the read pointer lives at +0x3C in the same block, and every park timed out
// because the wake was watching the wrong word. The guest's predicate
// `(W - target) >= (W - R)` is the same either way; only the store site differs.
//
// THE SHAPE. Two hooks, both call-through: sub_82845160 publishes what the wait is FOR
// (the device struct and the target) in a thread-local, and sub_8283C6C8 — the body the
// caller loops on — first spins briefly with `pause` (which DOES yield SMT resources on
// x86, i.e. what db16cyc meant), then parks on a futex over the very guest word the
// caller polls, with a bounded timeout, and then calls the original body so its own
// contract (return "keep waiting", the hang check, the abort flag) runs unchanged at
// <=1 ms granularity. The executor, after every GPU-side store, wakes the parked waiter
// if the store landed on the word it sleeps on — a few fence stores a frame, so the wake
// is one relaxed load per store while nobody is parked and one syscall per fence while
// somebody is. The guest re-checks its own condition after every wake, so the wake needs
// no predicate of its own.
//
// CORRECTNESS DOES NOT DEPEND ON THE WAKE. The park is bounded, the guest's own loop
// re-checks its own condition after every body call, and the body is called through
// unchanged; a missed wake can only make the wait slower, never wrong. The counters
// below say which path each episode took — the engagement gate (gotcha 151), and what
// showed the first draft's wake never firing (every park ending in a timeout).
//
// CZ_FENCE_PARK=0 is the control arm: both hooks call straight through and the thread
// spins exactly as it has since phase 1. CZ_FENCE_PARK_SPIN_US=N sets the paused-spin
// phase (default 50 us) — 0 parks immediately. CZ_FENCE_PARK_TRACE=N prints the first
// N episodes on both sides.
#pragma once

#include <atomic>
#include <cstdint>

namespace fencewait
{
extern std::atomic<uint32_t> g_parked;   // 1 while a waiter is asleep on the futex
void Wake(uint8_t* base, uint32_t va);
}   // namespace fencewait

// Call after EVERY store the GPU side makes into guest memory, with the guest VA. Inline
// so the un-parked case is one relaxed load.
inline void FenceWait_Stored(uint8_t* base, uint32_t va)
{
    if (fencewait::g_parked.load(std::memory_order_relaxed))
        fencewait::Wake(base, va);
}

struct FenceWaitStats
{
    uint64_t bodyCalls = 0;      // sub_8283C6C8 invocations under a published wait
    uint64_t readyAtEntry = 0;   // the fence had already passed (no spin, no park)
    uint64_t spinResolved = 0;   // resolved inside the paused spin
    uint64_t parks = 0;          // went to sleep on the futex
    uint64_t parkWoken = 0;      // ...and was woken by a store to the word
    uint64_t parkTimeouts = 0;   // ...and slept the whole bound with the fence still ahead
                                 //    (a long wait, re-parked after the body runs)
    uint64_t parkMissed = 0;     // ...and slept the whole bound although the fence HAD
                                 //    passed: a missed wake (the store landed unseen)
    uint64_t parkEagain = 0;     // ...and the word changed before the sleep began
    uint64_t contended = 0;      // a second thread wanted to park while one was parked
    uint64_t passthrough = 0;    // body called with no published wait (another caller)
    uint64_t wakeCalls = 0;      // executor side: futex wakes issued
    uint64_t storeChecks = 0;    // executor side: stores examined while a waiter was parked
};
FenceWaitStats FenceWait_Stats();
bool FenceWait_Enabled();
