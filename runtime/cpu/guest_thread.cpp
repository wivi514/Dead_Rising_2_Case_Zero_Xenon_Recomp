#include "guest_thread.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>

#include <unistd.h>

#include "../kernel/guestcall.h"
#include "../kernel/heap.h"
#include "../kernel/memory.h"

constexpr size_t kPcrSize = 0xAB0;
constexpr size_t kTlsSize = 0x100; // 64 slots x 4 bytes — matches the XEX header
constexpr size_t kTebSize = 0x2E0;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize)
{
    if (stackSize == 0)
        stackSize = kDefaultGuestStackSize;
    stackSize = (stackSize + 0xFFF) & ~0xFFFu;

    const size_t total = kPcrSize + kTlsSize + kTebSize + stackSize;
    block = static_cast<uint8_t*>(g_heap.Alloc(total));
    if (!block)
    {
        fprintf(stderr, "[cpu] out of user heap allocating a %zu-byte thread block\n", total);
        abort();
    }
    memset(block, 0, total);

    const uint32_t pcr = g_memory.MapVirtual(block);
    *reinterpret_cast<be<uint32_t>*>(block) = pcr + kPcrSize;                     // PCR+0x00: TLS area
    *reinterpret_cast<be<uint32_t>*>(block + 0x100) = pcr + kPcrSize + kTlsSize;  // PCR+0x100: TEB
    block[0x10C] = static_cast<uint8_t>(cpuNumber);                               // PCR+0x10C: CPU number
    *reinterpret_cast<be<uint32_t>*>(block + kPcrSize + 0x10) = 0xFFFFFFFF;       // TLS sentinel entry
    *reinterpret_cast<be<uint32_t>*>(block + kPcrSize + kTlsSize + 0x14C) =
        GuestThread::GetCurrentThreadId();                                        // TEB+0x14C: thread ID

    ppcContext.r1.u64 = pcr + total;      // stack grows down from the block's end
    ppcContext.r13.u64 = pcr;
    ppcContext.fpscr.loadFromHost();

    g_ppcContext = &ppcContext;
    RegisterPcr(pcr, GuestThread::GetCurrentThreadId());
}

GuestThreadContext::~GuestThreadContext()
{
    RegisterPcr(g_memory.MapVirtual(block), 0);
    g_ppcContext = nullptr;
    g_heap.Free(block);
}

// PCR -> thread id, for the diagnostics that only ever see r13. Thread blocks are
// recycled through the same heap, so the entry is dropped when the block is freed:
// a stale mapping would name a thread that no longer exists, which is worse than
// naming none.
static std::mutex g_pcrMutex;
static std::map<uint32_t, uint32_t> g_pcrToThreadId;

void GuestThreadContext::RegisterPcr(uint32_t pcr, uint32_t threadId)
{
    std::lock_guard lk(g_pcrMutex);
    if (threadId)
        g_pcrToThreadId[pcr] = threadId;
    else
        g_pcrToThreadId.erase(pcr);
}

uint32_t GuestThread::ThreadIdForPcr(uint32_t pcr)
{
    std::lock_guard lk(g_pcrMutex);
    auto it = g_pcrToThreadId.find(pcr);
    return it != g_pcrToThreadId.end() ? it->second : 0;
}

uint32_t GuestThread::Run(const GuestThreadParams& params)
{
    // Top set bit of the processor mask picks the CPU number (matches
    // ExCreateThread's flags>>24 processor-mask convention). A1 shows Case Zero
    // spawning at least 11 named guest threads — cAsyncFileSystem, JobThread0..5,
    // BigFile Decompress Thread, Controller Hardware Update, and two unnamed — so
    // the number the guest reads back from its PCR should be the one it asked for.
    const auto procMask = static_cast<uint8_t>(params.flags >> 24);
    const uint32_t cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber, params.stackSize);
    ctx.ppcContext.r3.u64 = params.arg0;
    ctx.ppcContext.r4.u64 = params.arg1;

    PPCFunc* func = g_memory.FindFunction(params.function);
    if (!func)
    {
        fprintf(stderr,
                "[cpu] guest thread entry %08X was not recompiled — thread not started\n",
                params.function);
        return 0;
    }

    // The HOST thread id, printed once per guest thread. Our own traces all speak
    // guest ids, but the instrument of last resort for a thread that is stuck without
    // being in any wait — a guest-level spin — is an outside debugger, and gdb only
    // knows host ids. Without this line the two views cannot be joined and every
    // stack a debugger prints is anonymous (finding 38).
    //
    // Behind the trace flags because it is only useful when a debugger is attached,
    // and because nineteen extra writes to stderr during the thread-creation storm
    // are not free.
    //
    // It was gated during a false alarm that is worth recording. Unconditional, this
    // build showed the A1 gate permuting positions 71-73 in about half its runs,
    // against 6 of 6 clean on the committed binary — which reads as an obvious
    // regression from the new logging. It is not: re-running the COMMITTED binary
    // under the same conditions produced the same permutation. Those positions are
    // scheduling-sensitive and always were; the 6-of-6 sample was smaller than it
    // looked. Gotcha 51 — a rate measured once is a fact about that afternoon — and
    // the correct control for "did my change do this" is the old binary run NOW, not
    // the old binary's remembered numbers.
    if (getenv("CZ_THREAD_TRACE") || getenv("CZ_WAIT_TRACE") || getenv("CZ_CS_TRACE"))
        fprintf(stderr, "[kernel] guest thread tid=%08X entry=%08X host tid=%d cpu=%u\n",
                GuestThread::GetCurrentThreadId(), params.function, int(gettid()),
                cpuNumber);
    bool terminated = false;
    try
    {
        func(ctx.ppcContext, g_memory.base);
    }
    catch (const GuestThreadExit&)
    {
        // ExTerminateThread unwinds to here.
        terminated = true;
    }

    // Always logged, never behind a flag. This title starts eleven guest threads and
    // essentially none of them are supposed to end during a boot, so the volume is a
    // dozen lines at most — and a thread that ends when it should not is otherwise
    // completely silent. A dead producer and a producer that has simply not got round
    // to signalling look identical from the consumer's side: the consumer is parked on
    // an event either way. One line here tells the two apart at a glance.
    fprintf(stderr, "[kernel] guest thread tid=%08X entry=%08X ENDED (%s, r3=%08X)\n",
            GuestThread::GetCurrentThreadId(), params.function,
            terminated ? "ExTerminateThread" : "returned", ctx.ppcContext.r3.u32);

    return ctx.ppcContext.r3.u32;
}

// Deterministic guest thread IDs: games store them, compare them, and use them as
// registry keys — the real kernel hands out small, stable values, so we do too.
static std::atomic<uint32_t> g_nextThreadId{ 0xF00 };
static thread_local uint32_t t_guestThreadId = 0;

static void GuestThreadFunc(GuestThreadHandle* handle)
{
    t_guestThreadId = handle->threadId;
    handle->suspended.wait(true);
    GuestThread::Run(handle->params);
    // Anyone holding a handle to *this* thread (see GuestThreadSelf) is waiting on
    // this flag. Setting it here rather than in a destructor matters: the object
    // outlives the thread whenever the guest kept the handle.
    GuestThread::MarkSelfExited();
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params),
      threadId(g_nextThreadId.fetch_add(4)),
      suspended((params.flags & 0x1) != 0), // CREATE_SUSPENDED
      thread(GuestThreadFunc, this)
{
}

GuestThreadHandle::~GuestThreadHandle()
{
    // NT semantics: closing a thread handle never stops or waits for the thread.
    if (thread.joinable())
        thread.detach();
}

uint32_t GuestThreadHandle::Wait(uint32_t)
{
    if (thread.joinable())
        thread.join();
    return STATUS_WAIT_0;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto* handle = CreateKernelObject<GuestThreadHandle>(params);
    if (threadId)
        *threadId = handle ? handle->GetThreadId() : 0;
    return handle;
}

uint32_t GuestThread::GetCurrentThreadId()
{
    if (t_guestThreadId == 0)
        t_guestThreadId = g_nextThreadId.fetch_add(4); // main/host-created threads
    return t_guestThreadId;
}

// The calling thread's own object, for the GetCurrentThread() pseudo-handle. One
// extra reference is taken on behalf of this cache, so a guest that closes its
// duplicated handle cannot leave the thread_local dangling.
static thread_local GuestThreadSelf* t_self = nullptr;

GuestThreadSelf* GuestThread::Self()
{
    if (!t_self)
    {
        t_self = CreateKernelObject<GuestThreadSelf>();
        if (t_self)
            t_self->refCount.fetch_add(1, std::memory_order_relaxed);
    }
    return t_self;
}

void GuestThread::MarkSelfExited()
{
    if (t_self)
        t_self->exited.store(true, std::memory_order_release);
}

// Polled rather than joined, because this object can outlive its thread and because
// the waiter is usually not the thread that created it. A 1 ms tick is far below any
// timeout a guest sets on a thread and costs nothing while nobody is waiting.
uint32_t GuestThreadSelf::Wait(uint32_t timeoutMs)
{
    const auto start = std::chrono::steady_clock::now();
    while (!exited.load(std::memory_order_acquire))
    {
        if (timeoutMs != WAIT_TIMEOUT_INFINITE)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= timeoutMs)
                return STATUS_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return STATUS_WAIT_0;
}
