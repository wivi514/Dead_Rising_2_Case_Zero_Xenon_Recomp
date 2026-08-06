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

// CZ_DETERMINISTIC_CLOCK — a guest clock that advances a fixed quantum per PRESENTED
// FRAME instead of tracking the host TSC.
//
// WHY A RUNTIME NEEDS THIS AT ALL, and it is a measurement instrument rather than a
// feature. Case Zero's title screen renders a live 3D scene whose camera is a function
// of guest time. Our frame rate varies with host load, so two runs are looking at
// different points in that animation at the same frame index — which makes a single
// rendered frame a RANDOM SAMPLE. Phase 5 retracted three separate conclusions to that
// one cause, the last of them a per-shader bisection whose evidence was resampled every
// run (docs/phase5-notes.md §6k, §6n, §6o).
//
// With this on, frame N is the same moment of the animation in every run, so a picture
// means something and a bisection can converge.
//
// It is OFF by default and must never be on for a gate run: it changes what the guest
// observes about time, which is the one thing findings 38-41 were all about. Same class
// of instrument as CZ_FAKE_START_MS — it manufactures a condition rather than reporting
// one, so it announces itself at startup.
extern bool deterministic;
extern uint64_t virtual_ticks;  // guest ticks, advanced only by AdvanceFrame()

// Advance the virtual clock by one frame. Called from the PM4 executor's XE_SWAP, i.e.
// from the guest's own frame boundary — the same signal the present seam uses, so the
// clock and the picture step together by construction.
void AdvanceFrame();

// Scaled into guest ticks. The multiply is done in 128-bit to keep full precision:
// a 64-bit TSC times 49,875,000 overflows almost immediately, and doing the divide
// first would quantise away everything below a microsecond — which is exactly the
// range short spin-waits live in.
inline uint64_t guest_ticks()
{
    if (deterministic)
        return virtual_ticks;
    return uint64_t((__uint128_t(host_rdtsc()) * CZ_TIMEBASE_HZ) / host_hz);
}

}  // namespace cz_timebase

// Shadow __rdtsc for the recompiled sources. Force-include ordering puts this
// ahead of ppc_context.h, so every `mftb` in the image binds here.
#undef __rdtsc
#define __rdtsc() (cz_timebase::guest_ticks())
