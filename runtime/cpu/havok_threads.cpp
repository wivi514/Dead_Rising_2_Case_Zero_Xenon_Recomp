// Havok's worker count (part 118) — the one piece of the guest's Main Thread that the
// TITLE already knows how to put on other cores, and ships hard-wired to two.
//
// WHY THIS EXISTS. Part 117 left the frame bound by the guest's own Main Thread (8.1-8.4
// ms CPU at the operator's crowd), and part 116's profile of it says ~11% is
// `cLibPhysics::Update` -> `hkJobQueue::processAllJobs` (sub_828AD798): the Main Thread
// taking Havok jobs off the queue and running them itself, alongside two
// `HavokWorkerThread`s that each sit at ~5% of a core. That is Havok's design — the
// calling thread joins the pool for the step — and the split is decided by how many
// workers exist. The title's physics init (sub_827E6440..) builds the pool's cinfo with
// `li r29, 2` -> `m_numThreads` (0x827E64D0/0x827E64D8) and the job queue's cinfo with
// `li r30, 3` -> `m_jobQueueHwSetup.m_numCpuThreads` (0x827E6488/0x827E65C4): main +
// two, the right number for an Xbox 360 with six hardware threads and a Draw Thread and
// six JobThreads already placed. On an 8-core host the workers are idle 95% of the time
// and the Main Thread is the longest term in the frame.
//
// The two cinfos are stack locals built in one function, so the hook is on their
// CONSUMERS — the pool constructor (sub_828B5B60, r4 = cinfo, m_numThreads at +0) and
// the job queue constructor (sub_828ADA60, r4 = cinfo, m_numCpuThreads at +8) — and it
// only rewrites the exact stock values (2 and 3): any other value means a different
// caller or a different build and is left alone. The pool constructor caps its own count
// at six slots (0x828B5C08); its hardware-thread-id array holds two entries and is read
// by index without a bound (0x828B5C5C-0x828B5C70), so a third worker reads a stale
// stack word as its 360 core id — harmless here, KeSetAffinityThread is a no-op on the
// host, but it is why the count is capped at 5 below and not 6.
//
// CZ_HAVOK_WORKERS=N sets the worker count (0..6; 2 = stock, the same-binary control).
// The default is decided from the physical core count in HavokWorkers(): the whole
// point of part 117's split default was that a 4-core machine has nothing to give.
#include "havok_threads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"
#include "thread_budget.h"

extern "C" PPC_FUNC(__imp__sub_828B5B60);
extern "C" PPC_FUNC(__imp__sub_828ADA60);

namespace
{
constexpr uint32_t kStockWorkers = 2;

uint32_t ReadBE32(uint8_t* base, uint32_t va)
{
    uint32_t b;
    memcpy(&b, base + va, 4);
    return __builtin_bswap32(b);
}
void WriteBE32(uint8_t* base, uint32_t va, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(base + va, &v, 4);
}
}   // namespace

unsigned HavokWorkers()
{
    static const unsigned n = [] {
        if (const char* e = getenv("CZ_HAVOK_WORKERS"))
        {
            const long v = atol(e);
            return unsigned(v < 0 ? 0 : v > 6 ? 6 : v);
        }
        // Stock until the measurement says otherwise (part 118 §1 is the A/B).
        return unsigned(kStockWorkers);
    }();
    return n;
}

// hkCpuJobThreadPool::hkCpuJobThreadPool(const hkCpuJobThreadPoolCinfo&) — r4 = cinfo.
PPC_FUNC(sub_828B5B60)
{
    const unsigned want = HavokWorkers();
    const uint32_t cinfo = uint32_t(ctx.r4.u32);
    const uint32_t stock = ReadBE32(base, cinfo);
    if (want != kStockWorkers && stock == kStockWorkers)
    {
        WriteBE32(base, cinfo, want);
        // m_hardwareThreadIds (hkArray at +0x10: data, size, capacity) holds two
        // entries and the constructor indexes it by worker without a bound; an empty
        // array takes its default path (a core id computed from the index), which is
        // what a no-op affinity call receives either way. Size 0, data left alone.
        WriteBE32(base, cinfo + 0x14, 0);
        fprintf(stderr, "[havok] thread pool: %u workers (stock %u), CZ_HAVOK_WORKERS\n",
                want, stock);
    }
    else
        fprintf(stderr, "[havok] thread pool: %u workers (stock)\n", stock);
    __imp__sub_828B5B60(ctx, base);
}

// hkJobQueue::hkJobQueue(const hkJobQueueCinfo&) — r4 = cinfo, m_numCpuThreads at +8
// (the calling thread counts; stock 3 = main + 2 workers).
PPC_FUNC(sub_828ADA60)
{
    const unsigned want = HavokWorkers();
    const uint32_t cinfo = uint32_t(ctx.r4.u32);
    const uint32_t stock = ReadBE32(base, cinfo + 8);
    if (want != kStockWorkers && stock == kStockWorkers + 1)
    {
        WriteBE32(base, cinfo + 8, want + 1);
        fprintf(stderr, "[havok] job queue: %u cpu threads (stock %u)\n", want + 1, stock);
    }
    __imp__sub_828ADA60(ctx, base);
}
