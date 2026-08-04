#pragma once
// Guest timebase — force-included over the RECOMPILED sources only.
//
// WHY THIS EXISTS (transferable gotcha #1, paid for on Fable 2 and again on
// Asura's Wrath)
//
// XenonRecomp lowers the PowerPC `mftb` (move from timebase) to the host's
// `__rdtsc()`. That is a reasonable default and it is wrong on every desktop: the
// Xbox 360's timebase runs at a fixed **49.875 MHz**, while __rdtsc() on this
// machine counts host cycles in the GHz. Roughly 70x fast.
//
// Nothing about that failure looks like a clock bug. The guest computes deltas
// from `mftb` and compares them against a frequency it got from
// KeQueryPerformanceFrequency, which our kernel answers with the console's real
// 49.875 MHz. So every timeout expires instantly, every rate limiter thinks it is
// far behind, spin-waits give up on their first pass, and animation and streaming
// logic step by absurd deltas. It presents as "the game is racing" or as random
// early bail-outs deep inside guest code, not as a bad number in one place.
//
// The fix is to keep the console's *rate*, not its *epoch*: scale the host TSC
// into 49.875 MHz ticks. Guest code only ever uses differences, so the origin is
// free; what has to be right is that N seconds of wall clock produce N * 49875000
// ticks.
//
// This header is force-included over ppc/*.cpp ONLY (see runtime/CMakeLists.txt).
// The runtime's own sources still want the real __rdtsc — the point is to change
// what the *guest* observes, not to lose access to the host counter.

// Pull the host intrinsics in BEFORE shadowing __rdtsc. ppc_context.h reaches
// <immintrin.h> through simde, and that header *declares* `__rdtsc(void)`; if our
// function-like macro is already in scope when the compiler reads that
// declaration, it rewrites it and the build dies inside a system header with an
// error that names neither this file nor the guest. Including it here first sets
// the include guard, so the later simde include is a no-op and the macro below
// only ever rewrites call sites.
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include <cstdint>

// Xbox 360 timebase frequency. The same constant must be what the kernel reports
// from KeQueryPerformanceFrequency; if these two ever disagree the guest's own
// arithmetic silently breaks and nothing here will look wrong.
inline constexpr uint64_t CZ_TIMEBASE_HZ = 49875000ull;

namespace cz_timebase {

// Host TSC frequency, measured once at startup (runtime/cpu/timebase.cpp). Kept as
// a plain global rather than a function-local static so the hot path has no
// thread-safe-initialisation guard.
extern uint64_t host_hz;

uint64_t host_rdtsc();   // the real host counter, unshadowed
bool init();             // calibrate host_hz; false means measurement failed

// Scaled into guest ticks. The multiply is done in 128-bit to keep full precision:
// a 64-bit TSC times 49,875,000 overflows almost immediately, and doing the divide
// first would quantise away everything below a microsecond — which is exactly the
// range short spin-waits live in.
inline uint64_t guest_ticks()
{
    return uint64_t((__uint128_t(host_rdtsc()) * CZ_TIMEBASE_HZ) / host_hz);
}

}  // namespace cz_timebase

// Shadow __rdtsc for the recompiled sources. Force-include ordering puts this
// ahead of ppc_context.h, so every `mftb` in the image binds here.
#undef __rdtsc
#define __rdtsc() (cz_timebase::guest_ticks())
