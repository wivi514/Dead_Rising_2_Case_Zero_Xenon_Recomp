// Host TSC calibration for the guest timebase. See cpu/timebase.h for why the
// guest must not see the raw host counter.
//
// This file is NOT force-included over the recompiled sources, so `__rdtsc` here
// is the real one.

#include <chrono>
#include <cstdint>
#include <thread>

#include "timebase.h"

namespace cz_timebase {

uint64_t host_hz = 0;

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
uint64_t host_rdtsc() { return __builtin_ia32_rdtsc(); }
#elif defined(__aarch64__)
uint64_t host_rdtsc()
{
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
}
#else
#error "no host cycle counter for this architecture"
#endif

namespace {

// Measure against the steady clock. 20 ms is enough for well under 0.1% error and
// is not worth optimising: this runs once, and a wrong *rate* here reproduces the
// exact bug the header exists to prevent, just less dramatically — which is worse,
// because a 2x-fast clock is obvious and a 1.02x-fast one is not.
uint64_t calibrate()
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const uint64_t c0 = host_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t c1 = host_rdtsc();
    const auto t1 = clock::now();

    const uint64_t ns =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (ns == 0 || c1 <= c0)
        return 0;   // caller reports; never silently substitute a plausible number
    return uint64_t((__uint128_t(c1 - c0) * 1'000'000'000ull) / ns);
}

}  // namespace

bool init()
{
    host_hz = calibrate();
    return host_hz != 0;
}

}  // namespace cz_timebase
