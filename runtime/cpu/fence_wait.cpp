#include "fence_wait.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#elif defined(_WIN32)
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CZ_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define CZ_CPU_PAUSE() __asm__ __volatile__("yield")
#else
#define CZ_CPU_PAUSE() ((void)0)
#endif

#include "ppc_recomp_shared.h"

// The recompiler emits every guest function as `__imp__sub_X` plus a weak `sub_X`, so a
// strong `sub_X` here takes over every call site (gotcha 6; d3d_hooks.cpp is the worked
// example).
extern "C" PPC_FUNC(__imp__sub_82845160);
extern "C" PPC_FUNC(__imp__sub_8283C6C8);

namespace fencewait
{
std::atomic<uint32_t> g_parked{ 0 };
}

namespace
{
// The device-struct fields the wait reads, from sub_82845160's own loop (phase1-notes
// finding 38 quotes the five instructions; phase5-notes §6ch §1 repeats them):
//   lwz r11,0x2a90(r31)  ; pointer to the fence-completion word   (R lives at *this)
//   lwz r10,0x2a9c(r31)  ; W — the last fence value the driver issued
//   cmplw (W - target) >= (W - R)  ->  done
// 0x2A90 is gpu/vd.h's kDeviceWritebackPtr; the ring read-pointer writeback is a
// different word, 0x3C into the same block.
constexpr uint32_t kDevFencePtr = 0x2A90;   // 10896
constexpr uint32_t kDevIssued = 0x2A9C;     // 10908

inline uint32_t LoadBE(const uint8_t* base, uint32_t va)
{
    uint32_t v;
    memcpy(&v, base + va, 4);
    return __builtin_bswap32(v);
}

// The guest's own predicate, in the guest's own unsigned arithmetic — reproduced
// verbatim rather than simplified, because it is what the caller will evaluate next.
inline bool FencePassed(uint32_t W, uint32_t target, uint32_t R)
{
    return uint32_t(W - target) >= uint32_t(W - R);
}

inline uint64_t NowNs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

struct WaitCtx
{
    uint32_t dev = 0;
    uint32_t target = 0;
    bool active = false;
};
thread_local WaitCtx t_wait;

// The parked waiter's word (guest VA). One parked waiter at a time (g_parked is a 0/1
// slot); a second concurrent waiter spins through the original body, counted, so a title
// thread that ever waits alongside the Draw Thread loses nothing but the park.
std::atomic<uint32_t> g_waitSlot{ 0 };

std::atomic<uint64_t> s_bodyCalls{ 0 }, s_readyAtEntry{ 0 }, s_spinResolved{ 0 },
    s_parks{ 0 }, s_parkWoken{ 0 }, s_parkTimeouts{ 0 }, s_parkMissed{ 0 }, s_parkEagain{ 0 },
    s_contended{ 0 }, s_passthrough{ 0 }, s_wakeCalls{ 0 }, s_storeChecks{ 0 };

inline void Bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

// CZ_FENCE_PARK_TRACE=N: print the first N episodes on both sides, with the values the
// predicate saw. Bounded, because a print per episode on the frame path would be the
// instrument that stalls its subject (gotcha 7). This is what refuted the first draft.
int TraceLeft()
{
    static std::atomic<int> left{ [] {
        const char* e = getenv("CZ_FENCE_PARK_TRACE");
        return e ? int(strtol(e, nullptr, 10)) : 0;
    }() };
    return left.fetch_sub(1, std::memory_order_relaxed);
}
inline long Tid()
{
#if defined(__linux__)
    return long(syscall(SYS_gettid));
#elif defined(_WIN32)
    return long(GetCurrentThreadId());
#else
    return 0;
#endif
}

unsigned SpinUs()
{
    static const unsigned us = [] {
        if (const char* e = getenv("CZ_FENCE_PARK_SPIN_US"))
            return unsigned(strtoul(e, nullptr, 10));
        return 50u;
    }();
    return us;
}

// Bounded, so the body's own timeout and abort checks keep running at this granularity
// whatever the wake does.
constexpr unsigned kParkTimeoutUs = 1000;

// -1 = timed out, 0 = woken, 1 = the word had already changed (no sleep).
int ParkOn(uint32_t* word, uint32_t expectRaw)
{
#if defined(__linux__)
    timespec ts{ 0, long(kParkTimeoutUs) * 1000L };
    const long r = syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expectRaw, &ts, nullptr, 0);
    if (r == 0)
        return 0;
    if (errno == ETIMEDOUT)
        return -1;
    return 1;   // EAGAIN (value changed) or EINTR: treat both as "look again now"
#elif defined(_WIN32)
    uint32_t expect = expectRaw;
    if (WaitOnAddress(word, &expect, sizeof(expect), (kParkTimeoutUs + 999) / 1000))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? -1 : 1;
#else
    (void)word;
    (void)expectRaw;
    return 1;   // no park primitive: the paused spin is all this platform gets
#endif
}

void WakeOne(uint32_t* word)
{
#if defined(__linux__)
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(_WIN32)
    WakeByAddressSingle(word);
#else
    (void)word;
#endif
}
}   // namespace

bool FenceWait_Enabled()
{
    static const bool on = [] {
        const char* e = getenv("CZ_FENCE_PARK");
        const bool enabled = !(e && *e == '0');
        if (enabled)
            fprintf(stderr,
                    "[fencewait] the Draw Thread's fence wait PARKS: %u us of paused spin, "
                    "then a futex on the fence word woken by the executor's store "
                    "(CZ_FENCE_PARK=0 restores the spin)\n",
                    SpinUs());
        else
            fprintf(stderr, "[fencewait] the Draw Thread's fence wait SPINS "
                            "(CZ_FENCE_PARK=0 — the pre-part-107 behaviour)\n");
        return enabled;
    }();
    return on;
}

FenceWaitStats FenceWait_Stats()
{
    FenceWaitStats s;
    s.bodyCalls = s_bodyCalls.load();
    s.readyAtEntry = s_readyAtEntry.load();
    s.spinResolved = s_spinResolved.load();
    s.parks = s_parks.load();
    s.parkWoken = s_parkWoken.load();
    s.parkTimeouts = s_parkTimeouts.load();
    s.parkMissed = s_parkMissed.load();
    s.parkEagain = s_parkEagain.load();
    s.contended = s_contended.load();
    s.passthrough = s_passthrough.load();
    s.wakeCalls = s_wakeCalls.load();
    s.storeChecks = s_storeChecks.load();
    return s;
}

// The executor's side. Reached (through the inline fast path) after every GPU-side
// store, only while a waiter is parked. Deliberately NO fence on the store path: a
// waiter that parks in the nanoseconds between the store and this check is caught by
// its own bounded timeout, and the futex compare-and-sleep already refuses to sleep on a
// word that has changed by the time the syscall looks.
void fencewait::Wake(uint8_t* base, uint32_t va)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    Bump(s_storeChecks);
    const uint32_t slot = g_waitSlot.load(std::memory_order_relaxed);
    if (!slot || (va & ~3u) != slot)
        return;
    if (TraceLeft() > 0)
        fprintf(stderr, "[fencewait] executor tid=%ld stored the fence word %08X (now %08X) -> WAKE\n",
                Tid(), va, LoadBE(base, slot));
    Bump(s_wakeCalls);
    WakeOne(reinterpret_cast<uint32_t*>(base + slot));
}

// The loop: publish what the wait is for, then run the original. Saved and restored
// rather than set and cleared, because sub_82845F68 both calls this function and is
// called from inside it (the r7 path), so the wait nests.
PPC_FUNC(sub_82845160)
{
    if (!FenceWait_Enabled())
    {
        __imp__sub_82845160(ctx, base);
        return;
    }
    const WaitCtx saved = t_wait;
    t_wait.dev = ctx.r3.u32;
    t_wait.target = ctx.r4.u32;
    t_wait.active = true;
    __imp__sub_82845160(ctx, base);
    t_wait = saved;
}

// The body: wait for the fence FIRST (paused spin, then a bounded park), then run the
// original body so its bookkeeping and its timeout logic are exactly the title's.
PPC_FUNC(sub_8283C6C8)
{
    if (!FenceWait_Enabled() || !t_wait.active)
    {
        if (!t_wait.active && FenceWait_Enabled())
            Bump(s_passthrough);
        __imp__sub_8283C6C8(ctx, base);
        return;
    }
    const uint32_t w = ctx.r3.u32;         // the caller's wait record; w[0] = device
    const uint32_t dev = LoadBE(base, w);
    const uint32_t slot = dev == t_wait.dev ? LoadBE(base, dev + kDevFencePtr) : 0;
    if (!slot || (slot & 3) != 0)
    {
        // Not the wait we published for, or a word a futex cannot take: original body.
        Bump(s_passthrough);
        __imp__sub_8283C6C8(ctx, base);
        return;
    }
    Bump(s_bodyCalls);
    uint32_t* word = reinterpret_cast<uint32_t*>(base + slot);
    const uint32_t target = t_wait.target;
    auto rawR = [&] { return *reinterpret_cast<volatile uint32_t*>(word); };
    auto ready = [&] {
        return FencePassed(LoadBE(base, dev + kDevIssued), target, __builtin_bswap32(rawR()));
    };

    const bool trace = TraceLeft() > 0;
    if (trace)
        fprintf(stderr,
                "[fencewait] wait tid=%ld body: dev=%08X word=%08X W=%08X target=%08X R=%08X "
                "ready=%d\n",
                Tid(), dev, slot, LoadBE(base, dev + kDevIssued), target,
                __builtin_bswap32(rawR()), int(ready()));
    if (ready())
        Bump(s_readyAtEntry);
    else
    {
        // Phase 1: the paused spin. `pause` is the x86 spelling of what db16cyc meant —
        // hand the sibling hardware thread the core — and the clock is read once per
        // sixteen of them, not once per iteration.
        const uint64_t spinNs = uint64_t(SpinUs()) * 1000u;
        const uint64_t t0 = NowNs();
        bool resolved = false;
        for (;;)
        {
            for (int i = 0; i < 16; ++i)
                CZ_CPU_PAUSE();
            if (ready())
            {
                resolved = true;
                break;
            }
            if (spinNs == 0 || NowNs() - t0 > spinNs)
                break;
        }
        if (resolved)
            Bump(s_spinResolved);
        else
        {
            // Phase 2: park. Take the single parked slot, publish the word, then re-read
            // it and sleep only if the fence is still not there.
            uint32_t expected = 0;
            if (fencewait::g_parked.compare_exchange_strong(expected, 1u,
                                                            std::memory_order_seq_cst))
            {
                g_waitSlot.store(slot, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const uint32_t raw = rawR();
                if (!FencePassed(LoadBE(base, dev + kDevIssued), target, __builtin_bswap32(raw)))
                {
                    Bump(s_parks);
                    const int r = ParkOn(word, raw);
                    if (trace)
                        fprintf(stderr,
                                "[fencewait] wait tid=%ld park -> %s; now W=%08X R=%08X ready=%d\n",
                                Tid(), r == 0 ? "woken" : r < 0 ? "timeout" : "eagain",
                                LoadBE(base, dev + kDevIssued), __builtin_bswap32(rawR()),
                                int(ready()));
                    if (r == 0)
                        Bump(s_parkWoken);
                    else if (r < 0)
                        Bump(ready() ? s_parkMissed : s_parkTimeouts);
                    else
                        Bump(s_parkEagain);
                }
                g_waitSlot.store(0, std::memory_order_relaxed);
                fencewait::g_parked.store(0, std::memory_order_seq_cst);
            }
            else
                Bump(s_contended);
        }
    }
    __imp__sub_8283C6C8(ctx, base);
}
