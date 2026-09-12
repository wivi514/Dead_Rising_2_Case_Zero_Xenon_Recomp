// The pump on two cores — see pump_split.h for the design and the three consequences of
// executing in stream order. This file is the stream, the D thread and the run-ahead
// table; pm4.cpp is where the walk decides between executing and appending.
//
// ONE STREAM, NOT TWO. The first build carried the register runs and the stores in a
// log and the draws / swaps / interrupts in a separate op ring, each op naming the log
// position it had to be replayed to. That is two orders, and the seam between them was
// a real defect: D's idle path replayed the log "as far as W has published", which
// could be PAST an op W had just queued — a scratch-mirror poison store landed before
// the INTERRUPT op it followed in the stream, and the guest ISR called 0x0BADF00D
// (crowd run split4w, `ctr=0BADF00D`, exactly the crash the pre-phase-C guard existed
// for). Everything is one record stream now: a run, a store, a draw, a swap, an
// interrupt, a shader bind, each a header dword and a payload, consumed in order. There
// is no second position to disagree with.
#include "pump_split.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define SPLIT_PAUSE() _mm_pause()
#else
#define SPLIT_PAUSE() ((void)0)
#endif

#include "../cpu/fence_wait.h"
#include "../cpu/thread_budget.h"
#include "../cpu/timebase.h"
#include "../host/window.h"
#include "pm4.h"
#include "vk_renderer.h"

namespace split {

bool g_on = false;

namespace {

// --- the stream ----------------------------------------------------------------------
// 32 MB of dwords: ~1.2 M a crowd frame (815 k of register runs, 11 k stores, 8.7 k
// draws at 29 dwords). W blocks when it is full, and a blocked W delays the vblank ISR it
// also delivers — but the guest cannot run more than ~2 frames ahead of the fences D
// writes, so ~7 frames of capacity means "never"; `wSpaceWaits` says whether that held.
constexpr uint32_t kLogDwords = 1u << 23;
constexpr uint32_t kLogMask = kLogDwords - 1;

// Record headers. A register run is `(index << 16) | count` with index < 0x8000; every
// other kind has a top halfword no register index can reach.
constexpr uint32_t kSkip = 0xFFFF0000u;       // the rest of this lap is unused
constexpr uint32_t kStore = 0xFFFE0002u;      // va, value
constexpr uint32_t kIrq = 0xFFFC0000u;        // (no payload)
constexpr uint32_t kSwap = 0xFFFB0003u;       // front, w, h
constexpr uint32_t kShader = 0xFFFA0006u;     // type, sizeDwords, hash lo/hi, ptr lo/hi
constexpr uint32_t kDrawKind = 0xFFFDu;       // low halfword = payload dwords

struct DrawRec
{
    Pm4Draw d;
    Pm4ShaderBinding vs, ps;
    DrawCtx ctx;
};
constexpr uint32_t kDrawDwords = uint32_t(sizeof(DrawRec) / 4);
static_assert(sizeof(DrawRec) % 4 == 0, "DrawRec must be whole dwords");
static_assert(kDrawDwords < 0x10000, "DrawRec header halfword");

uint32_t* g_log = nullptr;
uint8_t* g_base = nullptr;
void (*g_deliver)() = nullptr;

uint64_t g_head = 0;                       // W-private write position
// The trailing register run, if the last record is one and it is unpublished: a run
// contiguous with it is appended to it rather than started as a new record. The guest
// writes its constants in many adjacent runs (part 109's census: 64.6% of runs are 1-7
// dwords), and every record costs D a dependent header load.
uint64_t g_lastRunHdr = ~0ull;
uint32_t g_lastRunEnd = 0;                 // index just past the trailing run
std::atomic<uint64_t> g_pub{ 0 };          // W -> D: records complete up to here
std::atomic<uint64_t> g_tail{ 0 };         // D -> W: consumed up to here
uint64_t g_tailLocal = 0;                  // D-private

// The interrupt hand-off (design point 2).
std::atomic<uint64_t> g_irqRequested{ 0 };
std::atomic<uint64_t> g_irqDelivered{ 0 };

// D's park when the stream is empty (menus, loads, a guest waiting on us).
std::mutex g_parkMx;
std::condition_variable g_parkCv;
std::atomic<bool> g_dSleeping{ false };
// W's nap between ticks, made interruptible so an interrupt request from D does not
// wait out the whole tick (100 us a request, 3.3 a frame, before this existed).
std::mutex g_napMx;
std::condition_variable g_napCv;
std::atomic<bool> g_wNapping{ false };

// D's replica register file. 0x8000 dwords — pm4's kRegCount, restated here because the
// two must agree and pm4.cpp's is file-local; a run header's index cannot exceed it.
constexpr uint32_t kRegCount = 0x8000;
alignas(64) uint32_t g_regsD[kRegCount];

const DrawCtx* g_cur = nullptr;
// D: the kind of the last record consumed (for attributing idle time — what did D run
// dry AFTER: a swap, an interrupt, a store, a draw?). Index by g_lastKind: 0 run/other,
// 1 draw, 2 store, 3 irq, 4 swap.
uint32_t g_lastKind = 0;
// The palette accumulator (see TakePalette).
uint32_t g_palCover = 0, g_palPartial = 0, g_palCoverBursts = 0, g_palPartialBursts = 0;
uint32_t g_palHigh = 0;

Stats g_stats{};
#if !defined(_WIN32)
clockid_t g_wClock;
bool g_haveWClock = false;
#endif

// W: the stream position just past the last deferred store to each of the words the
// walk's WAIT_REG_MEMs poll (a handful of addresses in the device's writeback block),
// and the value it carries. A wait that is unmet while D has not reached that position
// is a wait on OUR OWN store — see PendingStoreValue.
constexpr uint32_t kStoreTrack = 64;
struct StoreTrack { uint32_t va; uint32_t value; uint64_t pos; };
StoreTrack g_storeTrack[kStoreTrack];
uint32_t g_storeTrackN = 0;

inline uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

inline void GuestStore32(uint8_t* base, uint32_t va, uint32_t value)
{
    const uint32_t raw = __builtin_bswap32(value);
    memcpy(base + va, &raw, 4);
}

inline void WakeD()
{
    if (g_dSleeping.load(std::memory_order_seq_cst))
    {
        std::lock_guard<std::mutex> lk(g_parkMx);
        g_parkCv.notify_one();
    }
}

// W: reserve `need` dwords that do not straddle the physical end (padding the lap out
// with a skip record if they would), waiting for D to free space if it must. Services
// the interrupt hand-off while it waits so D can never be waiting on W while W waits
// on D.
inline uint32_t* Reserve(uint64_t need)
{
    const uint32_t off = uint32_t(g_head & kLogMask);
    const uint64_t pad = (off + need > kLogDwords) ? (kLogDwords - off) : 0;
    for (;;)
    {
        const uint64_t tail = g_tail.load(std::memory_order_acquire);
        if (g_head + pad + need - tail <= kLogDwords)
            break;
        ++g_stats.wSpaceWaits;
        ServiceInterrupts();
        std::this_thread::yield();
    }
    if (pad)
    {
        g_log[off] = kSkip;
        g_head += pad;
    }
    return g_log + (g_head & kLogMask);
}

// W: the record is complete; let D have it. Called after every record that D must act
// on promptly (a store, a draw, a swap, an interrupt); register runs are published by
// the next such record, since nothing observes a run before the draw that follows it.
inline void Publish()
{
    g_lastRunHdr = ~0ull;   // whatever follows is a new record
    g_pub.store(g_head, std::memory_order_seq_cst);
    WakeD();
}

void RunDraw(const DrawRec& r)
{
    g_palCover = std::max(g_palCover, r.ctx.palCoverExtent);
    g_palPartial = std::max(g_palPartial, r.ctx.palPartialExtent);
    g_palCoverBursts += r.ctx.palCoverBursts;
    g_palPartialBursts += r.ctx.palPartialBursts;
    g_palHigh = r.ctx.palHighWater;
    g_cur = &r.ctx;
    VkRenderer_DrawQueued(g_base, r.d, g_regsD, r.vs, r.ps);
    g_cur = nullptr;
    ++g_stats.draws;
}

void RunInterrupt()
{
    // Ask W and wait (design point 2). W services the request at the top of its tick,
    // between packets, in its nap and inside its own space waits.
    const uint64_t ticket = g_irqRequested.fetch_add(1, std::memory_order_seq_cst) + 1;
    if (g_wNapping.load(std::memory_order_seq_cst))
    {
        std::lock_guard<std::mutex> lk(g_napMx);
        g_napCv.notify_one();
    }
    const uint64_t t0 = NowNs();
    uint32_t spins = 0;
    while (g_irqDelivered.load(std::memory_order_acquire) < ticket)
    {
        if (++spins < 20000)
            SPLIT_PAUSE();
        else
            std::this_thread::yield();
    }
    g_stats.dIrqWaitNs += NowNs() - t0;
    ++g_stats.interrupts;
}

// D: consume records up to `limit`.
void Consume(uint64_t limit)
{
    uint64_t t = g_tailLocal;
    while (t < limit)
    {
        // The stream is a sequence of lines DIRTY IN W's L2, and every header depends
        // on the one before it (the next position is this header's length) — a
        // dependent chain of cross-core misses the hardware prefetcher did not hide
        // (the first build's replay loop was 9.4% of D's cycles by itself).
        // Prefetching two and four lines ahead breaks the chain.
        __builtin_prefetch(g_log + ((t + 32) & kLogMask));
        __builtin_prefetch(g_log + ((t + 64) & kLogMask));
        const uint32_t h = g_log[t & kLogMask];
        const uint32_t kind = h >> 16;
        if (kind < kRegCount)
        {
            const uint32_t count = h & 0xFFFFu;
            memcpy(g_regsD + kind, g_log + ((t + 1) & kLogMask), size_t(count) * 4);
            t += count + 1;
            continue;
        }
        switch (h)
        {
            case kSkip:
                t = (t | kLogMask) + 1;   // the next lap
                continue;
            case kStore:
            {
                const uint32_t va = g_log[(t + 1) & kLogMask];
                const uint32_t value = g_log[(t + 2) & kLogMask];
                GuestStore32(g_base, va, value);
                FenceWait_Stored(g_base, va);
                ++g_stats.stores;
                g_lastKind = 2;
                t += 3;
                break;
            }
            case kIrq:
                t += 1;
                g_tailLocal = t;
                g_tail.store(t, std::memory_order_release);
                RunInterrupt();
                g_lastKind = 3;
                break;
            case kSwap:
            {
                const uint32_t front = g_log[(t + 1) & kLogMask];
                const uint32_t w = g_log[(t + 2) & kLogMask];
                const uint32_t hh = g_log[(t + 3) & kLogMask];
                t += 4;
                VkRenderer_OnSwap(g_base, front, w, hh);
                Host_Present(front, w, hh);
                cz_timebase::AdvanceFrame();
                ++g_stats.swaps;
                g_lastKind = 4;
                break;
            }
            case kShader:
            {
                const uint32_t type = g_log[(t + 1) & kLogMask];
                const uint32_t size = g_log[(t + 2) & kLogMask];
                const uint64_t hash = uint64_t(g_log[(t + 3) & kLogMask]) |
                                      (uint64_t(g_log[(t + 4) & kLogMask]) << 32);
                const uint64_t ptr = uint64_t(g_log[(t + 5) & kLogMask]) |
                                     (uint64_t(g_log[(t + 6) & kLogMask]) << 32);
                t += 7;
                uint8_t* code = reinterpret_cast<uint8_t*>(uintptr_t(ptr));
                VkRenderer_OnShaderBind(type, hash, code, size);
                free(code);
                break;
            }
            default:
                if (kind == kDrawKind)
                {
                    const uint32_t n = h & 0xFFFFu;   // == kDrawDwords
                    DrawRec r;
                    memcpy(&r, g_log + ((t + 1) & kLogMask), size_t(n) * 4);
                    t += n + 1;
                    RunDraw(r);
                    g_lastKind = 1;
                    break;
                }
                // An unknown header is a corrupt stream; stop rather than guess.
                fprintf(stderr, "[split] CORRUPT STREAM at %llu: header %08X — D stops\n",
                        (unsigned long long)t, h);
                g_tailLocal = t;
                for (;;)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        // Publish consumption after every non-run record: W's space check and the
        // run-ahead table read it, and a run is never the last record before a draw.
        g_tailLocal = t;
        g_tail.store(t, std::memory_order_release);
    }
    g_tailLocal = t;
    g_tail.store(t, std::memory_order_release);
}

void DrawThread()
{
    ThreadBudget_NameSelf("cz-draw");
    for (;;)
    {
        uint64_t pub = g_pub.load(std::memory_order_acquire);
        if (pub != g_tailLocal)
        {
            Consume(pub);
            continue;
        }
        // Nothing to do. ~200 us of spinning before the futex: at load the stream runs
        // dry a few times a frame for tens of microseconds each (W is gated by the guest
        // producing packets), and a park/unpark round trip costs more than the gap.
        const uint64_t tIdle = NowNs();
        for (int i = 0; i < 16000 && pub == g_tailLocal; ++i)
        {
            SPLIT_PAUSE();
            pub = g_pub.load(std::memory_order_acquire);
        }
        if (pub == g_tailLocal)
        {
            ++g_stats.dEmptyWaits;
            g_dSleeping.store(true, std::memory_order_seq_cst);
            std::unique_lock<std::mutex> lk(g_parkMx);
            g_parkCv.wait(lk, [&] {
                return g_pub.load(std::memory_order_seq_cst) != g_tailLocal;
            });
            g_dSleeping.store(false, std::memory_order_seq_cst);
        }
        const uint64_t idle = NowNs() - tIdle;
        g_stats.dIdleNs += idle;
        g_stats.dIdleByKindNs[g_lastKind < 5 ? g_lastKind : 0] += idle;
    }
}

} // namespace

bool Start(uint8_t* base, void (*deliverInterrupt)())
{
    // THE DEFAULT (part 117 §4): ON where the machine has the cores for a fifth busy
    // thread — six physical cores or more — and OFF below that, where W + D + the
    // guest's two + the guard pool would oversubscribe the box (the part-107 Ryzen 3
    // stand-in is 4c/8t). CZ_PUMP_SPLIT=1 forces it on anywhere, =0 forces the
    // one-thread pump anywhere; either spelling is the same-binary control arm.
    const char* e = getenv("CZ_PUMP_SPLIT");
    const unsigned physical = ThreadBudget_PhysicalCores();
    const bool on = (e && *e) ? (*e != '0') : (physical >= 6);
    if (!on)
    {
        if (!(e && *e))
            fprintf(stderr, "[split] one-thread pump: %u physical cores (< 6); "
                            "CZ_PUMP_SPLIT=1 forces the two-core pump\n", physical);
        return false;
    }
    if (getenv("CZ_D3D_DRAW"))
    {
        fprintf(stderr, "[split] CZ_PUMP_SPLIT refused under CZ_D3D_DRAW: the split is the "
                        "PM4 feed's; the API feed has its own thread\n");
        return false;
    }
    g_base = base;
    g_deliver = deliverInterrupt;
    g_log = static_cast<uint32_t*>(calloc(kLogDwords, sizeof(uint32_t)));
    if (!g_log)
    {
        fprintf(stderr, "[split] stream allocation failed — one-thread pump\n");
        return false;
    }
    // D's replica starts as W's file is now (all zero before the first walk).
    memcpy(g_regsD, Pm4_Registers(), sizeof g_regsD);
#if !defined(_WIN32)
    g_haveWClock = pthread_getcpuclockid(pthread_self(), &g_wClock) == 0;
#endif
    ThreadBudget_Note("draw", 1,
                      "CZ_PUMP_SPLIT=1: the renderer on its own core; the pump thread keeps "
                      "the walk, the register file, the waits and the ISRs");
    std::thread(DrawThread).detach();
    g_on = true;
    fprintf(stderr, "[split] two-core pump %s (%u physical cores) — the PM4 walk stays on "
                    "cz-pump; draws, stores, swaps and interrupts execute in stream order "
                    "on cz-draw (one %u MB stream). CZ_PUMP_SPLIT=0 is the one-thread "
                    "control\n",
            (e && *e) ? "by CZ_PUMP_SPLIT=1" : "by default", physical,
            unsigned(kLogDwords * 4 / (1024 * 1024)));
    return true;
}

void LogRun(uint32_t index, uint32_t count, const uint32_t* src)
{
    // Extend the trailing run when this one continues it and fits before the lap's end.
    if (g_lastRunHdr != ~0ull && index == g_lastRunEnd)
    {
        const uint32_t have = g_log[g_lastRunHdr & kLogMask] & 0xFFFFu;
        const uint32_t off = uint32_t(g_head & kLogMask);
        if (have + count <= 0xFFFFu && off + count <= kLogDwords)
        {
            for (;;)
            {
                const uint64_t tail = g_tail.load(std::memory_order_acquire);
                if (g_head + count - tail <= kLogDwords)
                    break;
                ++g_stats.wSpaceWaits;
                ServiceInterrupts();
                std::this_thread::yield();
            }
            memcpy(g_log + off, src, size_t(count) * 4);
            g_log[g_lastRunHdr & kLogMask] = (g_lastRunEnd - have) << 16 | (have + count);
            g_head += count;
            g_lastRunEnd += count;
            g_stats.logDwords += count;
            ++g_stats.runsMerged;
            return;
        }
    }
    while (count)
    {
        const uint32_t n = count > 0xFFFFu ? 0xFFFFu : count;
        uint32_t* p = Reserve(uint64_t(n) + 1);
        p[0] = (index << 16) | n;
        memcpy(p + 1, src, size_t(n) * 4);
        g_lastRunHdr = g_head;
        g_lastRunEnd = index + n;
        g_head += uint64_t(n) + 1;
        g_stats.logDwords += n;
        ++g_stats.runs;
        index += n;
        src += n;
        count -= n;
    }
}

void EnqueueDraw(const Pm4Draw& d, const Pm4ShaderBinding& vs, const Pm4ShaderBinding& ps,
                 const DrawCtx& ctx)
{
    uint32_t* p = Reserve(kDrawDwords + 1);
    p[0] = (kDrawKind << 16) | kDrawDwords;
    DrawRec r;
    r.d = d;
    r.vs = vs;
    r.ps = ps;
    r.ctx = ctx;
    memcpy(p + 1, &r, sizeof r);
    g_head += kDrawDwords + 1;
    ++g_stats.ops;
    Publish();
}

void EnqueueStore(uint32_t va, uint32_t value)
{
    uint32_t* p = Reserve(3);
    p[0] = kStore;
    p[1] = va;
    p[2] = value;
    g_head += 3;
    {
        uint32_t i = 0;
        for (; i < g_storeTrackN; ++i)
            if (g_storeTrack[i].va == va)
                break;
        if (i == g_storeTrackN && i < kStoreTrack)
            g_storeTrack[g_storeTrackN++].va = va;
        if (i < kStoreTrack)
        {
            g_storeTrack[i].pos = g_head;
            g_storeTrack[i].value = value;
        }
    }
    ++g_stats.storesQueued;
    Publish();
}

void EnqueueInterrupt()
{
    uint32_t* p = Reserve(1);
    p[0] = kIrq;
    g_head += 1;
    ++g_stats.ops;
    Publish();
}

void EnqueueSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    uint32_t* p = Reserve(4);
    p[0] = kSwap;
    p[1] = frontBuffer;
    p[2] = width;
    p[3] = height;
    g_head += 4;
    ++g_stats.ops;
    Publish();
}

void EnqueueShaderBind(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t sizeDwords)
{
    uint8_t* copy = static_cast<uint8_t*>(malloc(size_t(sizeDwords) * 4));
    if (!copy)
        return;
    memcpy(copy, code, size_t(sizeDwords) * 4);
    const uint64_t ptr = uint64_t(uintptr_t(copy));
    uint32_t* p = Reserve(7);
    p[0] = kShader;
    p[1] = type;
    p[2] = sizeDwords;
    p[3] = uint32_t(hash);
    p[4] = uint32_t(hash >> 32);
    p[5] = uint32_t(ptr);
    p[6] = uint32_t(ptr >> 32);
    g_head += 7;
    ++g_stats.ops;
    Publish();
}

void Nap(int us)
{
    if (!g_on)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
        return;
    }
    g_wNapping.store(true, std::memory_order_seq_cst);
    if (g_irqRequested.load(std::memory_order_seq_cst) ==
        g_irqDelivered.load(std::memory_order_relaxed))
    {
        std::unique_lock<std::mutex> lk(g_napMx);
        g_napCv.wait_for(lk, std::chrono::microseconds(us), [] {
            return g_irqRequested.load(std::memory_order_seq_cst) !=
                   g_irqDelivered.load(std::memory_order_relaxed);
        });
    }
    g_wNapping.store(false, std::memory_order_seq_cst);
}

void ServiceInterrupts()
{
    while (g_irqDelivered.load(std::memory_order_relaxed) <
           g_irqRequested.load(std::memory_order_acquire))
    {
        if (g_deliver)
            g_deliver();
        ++g_stats.irqDelivered;
        g_irqDelivered.fetch_add(1, std::memory_order_release);
    }
}

const DrawCtx* CurrentDraw() { return g_cur; }

Pm4VsPaletteWrites TakePalette()
{
    const Pm4VsPaletteWrites w{ g_palCover, g_palPartial, g_palCoverBursts, g_palPartialBursts };
    g_palCover = g_palPartial = 0;
    g_palCoverBursts = g_palPartialBursts = 0;
    return w;
}

uint32_t PaletteHighWater() { return g_palHigh; }

double WalkCpuSeconds()
{
#if !defined(_WIN32)
    if (!g_on || !g_haveWClock)
        return -1.0;
    timespec ts{};
    if (clock_gettime(g_wClock, &ts) != 0)
        return -1.0;
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#else
    return -1.0;
#endif
}

Stats GetStats() { return g_stats; }

bool PendingStoreValue(uint32_t va, uint32_t* value)
{
    if (!g_on)
        return false;
    for (uint32_t i = 0; i < g_storeTrackN; ++i)
        if (g_storeTrack[i].va == va)
        {
            if (g_storeTrack[i].pos <= g_tail.load(std::memory_order_acquire))
                return false;   // D has landed it; memory is the truth
            *value = g_storeTrack[i].value;
            return true;
        }
    return false;
}

void NoteWaitSatisfiedByPending() { ++g_stats.waitsByPending; }

void NoteWaitUnmet(uint32_t va)
{
    if (!g_on)
        return;
    ++g_stats.waitsUnmet;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (g_stats.waitVa[i] == va || g_stats.waitVa[i] == 0)
        {
            g_stats.waitVa[i] = va;
            ++g_stats.waitVaCount[i];
            break;
        }
    }
    for (uint32_t i = 0; i < g_storeTrackN; ++i)
        if (g_storeTrack[i].va == va)
        {
            if (g_storeTrack[i].pos > g_tail.load(std::memory_order_acquire))
                ++g_stats.waitsOnOurStore;
            return;
        }
}

} // namespace split
