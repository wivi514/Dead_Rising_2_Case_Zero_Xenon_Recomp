// The kernel-call trace — phase 1's only measurement instrument, and the thing
// every later gate is diffed against.
//
// WHY THIS EXISTS
// ---------------
// `docs/runtime-plan.md` gates each phase on "our kernel-call sequence matches
// Xenia's", never on "it seems to work". Xenia's side of that comparison comes out
// of the A1 level-3 log mechanically, so ours has to be equally mechanical.
//
// Every kernel import — real implementation and generated stub alike — emits
//
//     [kcall] <Name>
//
// exactly once, the first time it is called. `tools/kernel_call_diff.py` greps that
// and compares the first-occurrence sequence against A1's.
//
// FIRST-OCCURRENCE ORDER, NOT THE RAW CALL STREAM
// -----------------------------------------------
// The raw stream is dominated by polls — Case Zero calls XamInputGetCapabilities
// 5,501 times and XamUserGetXUID 4,441 times in the A1 boot alone — and our thread
// interleaving will never match an emulator's cycle-by-cycle anyway. The order in
// which subsystems are *first* touched is a real, reproducible property; the
// interleaving of the polls is not. So repeats are counted rather than printed,
// except for a sparse heartbeat (every 65,536th) that keeps a busy-polling hang
// identifiable in the log without drowning it.
//
// THE EXTRACTION PATTERN ON XENIA'S SIDE, AND THE TRAP IN IT
// ----------------------------------------------------------
// A1's kernel-call lines are
//
//     d> F8000008 NtAllocateVirtualMemory(7018FB90(00000000), ...)
//     d> F8000008 NtAllocateVirtualMemory = 40000000
//
// but the prefix is NOT always `d>`. This log carries `G>` `d>` `A>` `!>` `i>`
// `F>` `K>` and unprefixed continuations, and filtering on `d>` alone silently
// loses whole exports (gotcha 24 — `VdSwap` is logged at `i>`). The diff tool
// accepts every prefix; see its docstring.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

// Free-form kernel-side message. Deliberately a different prefix from [kcall] so
// the gate's grep can never pick these up by accident.
#define KLOG(...) std::fprintf(stderr, "[kernel] " __VA_ARGS__)

// `name` may carry the recompiler's `__imp__` prefix; it is stripped here so both
// sides of the diff speak Xenia's names.
void KernelCallTrace(const char* name, uint64_t hit);

// One relaxed atomic increment per kernel call. That cost is deliberate and it is
// the reason this is a counter rather than a printf: an instrument expensive enough
// to change the thing it measures manufactures the stability it reports (gotcha 7).
#define KCALL(sym)                                                                  \
    do                                                                              \
    {                                                                               \
        static std::atomic<uint64_t> _kcallCount{ 0 };                              \
        const uint64_t _kcallHit = _kcallCount.fetch_add(1, std::memory_order_relaxed); \
        if (_kcallHit == 0 || (_kcallHit & 0xFFFFu) == 0)                            \
            KernelCallTrace(sym, _kcallHit);                                         \
    } while (0)
