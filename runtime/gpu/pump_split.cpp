// The pump on two cores — see pump_split.h for the design and the three consequences of
// executing in stream order. This file is the queue, the register-run log and the D
// thread; pm4.cpp is where the walk decides between executing and enqueueing.
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
#include <vector>

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

// --- the queues ----------------------------------------------------------------------
// Sizes are generous on purpose: W blocks when either ring is full, and a blocked W
// delays the vblank ISR it also delivers. The guest cannot run more than ~2 frames ahead
// of the fences D writes, so ~10 frames of capacity means "never" at any load this title
// reaches; `wSpaceWaits` in the stats block is the counter that says whether that held.
constexpr uint32_t kLogDwords = 1u << 23;        // 32 MB; ~815 k dwords a crowd frame
constexpr uint32_t kLogMask = kLogDwords - 1;
constexpr uint32_t kOps = 1u << 17;               // ~12 k ops a crowd frame
constexpr uint32_t kOpsMask = kOps - 1;
constexpr uint32_t kSkipMarker = 0xFFFF0000u;    // "the rest of this lap is unused"
// A guest-memory store rides the LOG rather than the op ring: header 0xFFFE0002, then
// (va, value). Stores are ~11,000 a crowd frame against ~8,600 draws, and a 128-byte op
// each was a quarter of what D streamed from W's core; as three log dwords they are
// ~130 KB a frame. Order is unchanged — the log is replayed in stream order up to each
// op's position, and past the last op (see PublishLog) so a fence with no draw behind
// it still lands.
constexpr uint32_t kStoreMarker = 0xFFFE0002u;

enum : uint8_t { kDraw = 1, kStore, kInterrupt, kSwap, kShader };

struct alignas(64) Op
{
    uint8_t kind;
    uint64_t logEnd;   // replay the log to here before executing
    union
    {
        struct
        {
            Pm4Draw d;
            Pm4ShaderBinding vs, ps;
            DrawCtx ctx;
        } draw;
        struct { uint32_t va, value; } store;
        struct { uint32_t front, w, h; } swap;
        struct { uint32_t type, sizeDwords; uint64_t hash; uint8_t* code; } shader;
    };
};
static_assert(sizeof(Op) <= 192, "Op grew; check the ring's footprint");

uint32_t* g_log = nullptr;
Op* g_ops = nullptr;
uint8_t* g_base = nullptr;
void (*g_deliver)() = nullptr;

// W-private cursors (only W writes them; D reads the published copies).
uint64_t g_logHead = 0;
uint64_t g_opHeadLocal = 0;
// Published: written by one side with release, read by the other with acquire.
std::atomic<uint64_t> g_opHead{ 0 };    // W -> D: ops available
std::atomic<uint64_t> g_opTail{ 0 };    // D -> W: ops consumed
std::atomic<uint64_t> g_logTail{ 0 };   // D -> W: log consumed
std::atomic<uint64_t> g_logPub{ 0 };    // W -> D: log dwords D may replay ahead of any op
// D-private
uint64_t g_logTailLocal = 0;
uint64_t g_opTailLocal = 0;

// The interrupt hand-off (design point 2).
std::atomic<uint64_t> g_irqRequested{ 0 };
std::atomic<uint64_t> g_irqDelivered{ 0 };

// D's park when the queue is empty (menus, loads, a guest waiting on us).
std::mutex g_parkMx;
std::condition_variable g_parkCv;
std::atomic<bool> g_dSleeping{ false };
// W's nap between ticks, made interruptible so an interrupt request from D does not
// wait out the whole tick (100 us a request, 3.3 a frame, before this existed).
std::mutex g_napMx;
std::condition_variable g_napCv;
std::atomic<bool> g_wNapping{ false };

// D's replica register file. 0x8000 dwords — pm4's kRegCount, restated here because the
// two must agree and pm4.cpp's is file-local; the static_assert in Start checks it
// against the index bound every run carries.
constexpr uint32_t kRegCount = 0x8000;
alignas(64) uint32_t g_regsD[kRegCount];

const DrawCtx* g_cur = nullptr;
// The palette accumulator (see TakePalette).
uint32_t g_palCover = 0, g_palPartial = 0, g_palCoverBursts = 0, g_palPartialBursts = 0;
uint32_t g_palHigh = 0;

Stats g_stats{};
// W: the log position of the last deferred store to each of the words the walk's
// WAIT_REG_MEMs poll (a handful of addresses in the device's writeback block). A wait
// that is unmet while D has not yet reached that position is a wait on OUR OWN store —
// the pipeline draining at a hand-off — as opposed to a wait on the guest CPU.
constexpr uint32_t kStoreTrack = 64;
struct StoreTrack { uint32_t va; uint32_t value; uint64_t pos; };
StoreTrack g_storeTrack[kStoreTrack];
uint32_t g_storeTrackN = 0;
#if !defined(_WIN32)
clockid_t g_wClock;
bool g_haveWClock = false;
#endif

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

// W: wait until the log has `need` dwords free and the op ring has a slot. Services the
// interrupt hand-off while it waits so D can never be waiting on W while W waits on D.
inline void WaitLogSpace(uint64_t need)
{
    for (;;)
    {
        const uint64_t tail = g_logTail.load(std::memory_order_acquire);
        if (g_logHead + need - tail <= kLogDwords)
            return;
        ++g_stats.wSpaceWaits;
        ServiceInterrupts();
        std::this_thread::yield();
    }
}
inline void WaitOpSpace()
{
    for (;;)
    {
        const uint64_t tail = g_opTail.load(std::memory_order_acquire);
        if (g_opHeadLocal - tail < kOps)
            return;
        ++g_stats.wSpaceWaits;
        ServiceInterrupts();
        std::this_thread::yield();
    }
}

inline Op& NextOp()
{
    WaitOpSpace();
    return g_ops[g_opHeadLocal & kOpsMask];
}

inline void WakeD()
{
    if (g_dSleeping.load(std::memory_order_seq_cst))
    {
        std::lock_guard<std::mutex> lk(g_parkMx);
        g_parkCv.notify_one();
    }
}

inline void Publish()
{
    g_ops[g_opHeadLocal & kOpsMask].logEnd = g_logHead;
    ++g_opHeadLocal;
    ++g_stats.ops;
    g_opHead.store(g_opHeadLocal, std::memory_order_seq_cst);
    g_logPub.store(g_logHead, std::memory_order_seq_cst);
    WakeD();
}

// W: let D replay the log this far even with no op behind it (a fence after the last
// draw of a batch must land, or the guest waits on it forever).
inline void PublishLog()
{
    g_logPub.store(g_logHead, std::memory_order_seq_cst);
    WakeD();
}

// D: bring the replica (and guest memory, for the stores in the log) up to `logEnd`.
inline void Replay(uint64_t logEnd)
{
    uint64_t t = g_logTailLocal;
    while (t < logEnd)
    {
        // The log is a sequential stream of lines DIRTY IN W's L2, and every header
        // depends on the one before it (the next position is this header's count) — a
        // dependent chain of cross-core misses that the hardware prefetcher did not
        // hide (the first build's replay loop was 9.4% of D's cycles by itself).
        // Prefetching two and four lines ahead breaks the chain.
        __builtin_prefetch(g_log + ((t + 32) & kLogMask));
        __builtin_prefetch(g_log + ((t + 64) & kLogMask));
        const uint32_t h = g_log[t & kLogMask];
        if (h == kSkipMarker)
        {
            t = (t | kLogMask) + 1;   // the next lap
            continue;
        }
        if (h == kStoreMarker)
        {
            const uint32_t va = g_log[(t + 1) & kLogMask];
            const uint32_t value = g_log[(t + 2) & kLogMask];
            GuestStore32(g_base, va, value);
            FenceWait_Stored(g_base, va);
            ++g_stats.stores;
            t += 3;
            continue;
        }
        const uint32_t index = h >> 16;
        const uint32_t count = h & 0xFFFFu;
        memcpy(g_regsD + index, g_log + ((t + 1) & kLogMask), size_t(count) * 4);
        t += count + 1;
    }
    g_logTailLocal = t;
    g_logTail.store(t, std::memory_order_release);
}

void RunDraw(const Op& op)
{
    g_palCover = std::max(g_palCover, op.draw.ctx.palCoverExtent);
    g_palPartial = std::max(g_palPartial, op.draw.ctx.palPartialExtent);
    g_palCoverBursts += op.draw.ctx.palCoverBursts;
    g_palPartialBursts += op.draw.ctx.palPartialBursts;
    g_palHigh = op.draw.ctx.palHighWater;
    g_cur = &op.draw.ctx;
    VkRenderer_DrawQueued(g_base, op.draw.d, g_regsD, op.draw.vs, op.draw.ps);
    g_cur = nullptr;
    ++g_stats.draws;
}

void DrawThread()
{
    ThreadBudget_NameSelf("cz-draw");
    for (;;)
    {
        uint64_t head = g_opHead.load(std::memory_order_acquire);
        if (head == g_opTailLocal)
        {
            // Nothing queued: apply whatever the log holds past the last op (stores
            // that must land now; register runs that are harmless early), then spin
            // briefly — the queue is rarely empty at load — then park.
            const uint64_t tIdle = NowNs();
            Replay(g_logPub.load(std::memory_order_acquire));
            // ~200 us of spinning before the futex: at load the queue runs dry ~6 times
            // a frame for tens of microseconds each (W is gated by the guest producing
            // packets), and a park/unpark round trip costs more than the gap.
            for (int i = 0; i < 16000 && head == g_opTailLocal; ++i)
            {
                SPLIT_PAUSE();
                if ((i & 63) == 0)
                    Replay(g_logPub.load(std::memory_order_acquire));
                head = g_opHead.load(std::memory_order_acquire);
            }
            if (head == g_opTailLocal)
            {
                Replay(g_logPub.load(std::memory_order_acquire));
                ++g_stats.dEmptyWaits;
                g_dSleeping.store(true, std::memory_order_seq_cst);
                std::unique_lock<std::mutex> lk(g_parkMx);
                g_parkCv.wait(lk, [&] {
                    return g_opHead.load(std::memory_order_seq_cst) != g_opTailLocal ||
                           g_logPub.load(std::memory_order_seq_cst) != g_logTailLocal;
                });
                g_dSleeping.store(false, std::memory_order_seq_cst);
                g_stats.dIdleNs += NowNs() - tIdle;
                continue;
            }
            g_stats.dIdleNs += NowNs() - tIdle;
        }
        while (g_opTailLocal != head)
        {
            Op& op = g_ops[g_opTailLocal & kOpsMask];
            Replay(op.logEnd);
            switch (op.kind)
            {
                case kDraw:
                    RunDraw(op);
                    break;
                case kInterrupt:
                {
                    // Ask W and wait (design point 2). W services the request at the top
                    // of its tick, between packets, and inside its own space waits.
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
                    break;
                }
                case kSwap:
                    VkRenderer_OnSwap(g_base, op.swap.front, op.swap.w, op.swap.h);
                    Host_Present(op.swap.front, op.swap.w, op.swap.h);
                    cz_timebase::AdvanceFrame();
                    ++g_stats.swaps;
                    break;
                case kShader:
                    VkRenderer_OnShaderBind(op.shader.type, op.shader.hash, op.shader.code,
                                            op.shader.sizeDwords);
                    free(op.shader.code);
                    break;
                default:
                    break;
            }
            ++g_opTailLocal;
            g_opTail.store(g_opTailLocal, std::memory_order_release);
        }
    }
}

} // namespace

bool Start(uint8_t* base, void (*deliverInterrupt)())
{
    const char* e = getenv("CZ_PUMP_SPLIT");
    if (!e || !*e || *e == '0')
        return false;
    if (getenv("CZ_D3D_DRAW"))
    {
        fprintf(stderr, "[split] CZ_PUMP_SPLIT refused under CZ_D3D_DRAW: the split is the "
                        "PM4 feed's; the API feed has its own thread\n");
        return false;
    }
    g_base = base;
    g_deliver = deliverInterrupt;
    g_log = static_cast<uint32_t*>(calloc(kLogDwords, sizeof(uint32_t)));
    g_ops = static_cast<Op*>(calloc(kOps, sizeof(Op)));
    if (!g_log || !g_ops)
    {
        fprintf(stderr, "[split] queue allocation failed — one-thread pump\n");
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
    fprintf(stderr, "[split] CZ_PUMP_SPLIT=1 — the PM4 walk stays on cz-pump, draws / "
                    "stores / swaps / interrupts execute in stream order on cz-draw "
                    "(log %u MB, %u ops)\n",
            unsigned(kLogDwords * 4 / (1024 * 1024)), unsigned(kOps));
    return true;
}

void LogRun(uint32_t index, uint32_t count, const uint32_t* src)
{
    while (count)
    {
        const uint32_t n = count > 0xFFFFu ? 0xFFFFu : count;
        const uint64_t need = uint64_t(n) + 1;
        const uint32_t off = uint32_t(g_logHead & kLogMask);
        const uint64_t pad = (off + need > kLogDwords) ? (kLogDwords - off) : 0;
        WaitLogSpace(pad + need);
        if (pad)
        {
            g_log[off] = kSkipMarker;
            g_logHead += pad;
        }
        g_log[g_logHead & kLogMask] = (index << 16) | n;
        memcpy(g_log + ((g_logHead + 1) & kLogMask), src, size_t(n) * 4);
        g_logHead += need;
        g_stats.logDwords += n;
        index += n;
        src += n;
        count -= n;
    }
}

void EnqueueDraw(const Pm4Draw& d, const Pm4ShaderBinding& vs, const Pm4ShaderBinding& ps,
                 const DrawCtx& ctx)
{
    Op& op = NextOp();
    op.kind = kDraw;
    op.draw.d = d;
    op.draw.vs = vs;
    op.draw.ps = ps;
    op.draw.ctx = ctx;
    Publish();
}

void EnqueueStore(uint32_t va, uint32_t value)
{
    const uint64_t need = 3;
    const uint32_t off = uint32_t(g_logHead & kLogMask);
    const uint64_t pad = (off + need > kLogDwords) ? (kLogDwords - off) : 0;
    WaitLogSpace(pad + need);
    if (pad)
    {
        g_log[off] = kSkipMarker;
        g_logHead += pad;
    }
    g_log[g_logHead & kLogMask] = kStoreMarker;
    g_log[(g_logHead + 1) & kLogMask] = va;
    g_log[(g_logHead + 2) & kLogMask] = value;
    g_logHead += need;
    {
        uint32_t i = 0;
        for (; i < g_storeTrackN; ++i)
            if (g_storeTrack[i].va == va)
                break;
        if (i == g_storeTrackN && i < kStoreTrack)
            g_storeTrack[g_storeTrackN++].va = va;
        if (i < kStoreTrack)
        {
            g_storeTrack[i].pos = g_logHead;
            g_storeTrack[i].value = value;
        }
    }
    ++g_stats.storesQueued;
    PublishLog();
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

void EnqueueInterrupt()
{
    Op& op = NextOp();
    op.kind = kInterrupt;
    Publish();
}

void EnqueueSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    Op& op = NextOp();
    op.kind = kSwap;
    op.swap.front = frontBuffer;
    op.swap.w = width;
    op.swap.h = height;
    Publish();
}

void EnqueueShaderBind(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t sizeDwords)
{
    uint8_t* copy = static_cast<uint8_t*>(malloc(size_t(sizeDwords) * 4));
    if (!copy)
        return;
    memcpy(copy, code, size_t(sizeDwords) * 4);
    Op& op = NextOp();
    op.kind = kShader;
    op.shader.type = type;
    op.shader.sizeDwords = sizeDwords;
    op.shader.hash = hash;
    op.shader.code = copy;
    Publish();
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
            if (g_storeTrack[i].pos <= g_logTail.load(std::memory_order_acquire))
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
    for (uint32_t i = 0; i < g_storeTrackN; ++i)
        if (g_storeTrack[i].va == va)
        {
            if (g_storeTrack[i].pos > g_logTail.load(std::memory_order_acquire))
                ++g_stats.waitsOnOurStore;
            return;
        }
}

} // namespace split
