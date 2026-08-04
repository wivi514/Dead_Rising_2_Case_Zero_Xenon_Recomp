#include "guest_thread.h"

#include <bit>
#include <cstring>

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
}

GuestThreadContext::~GuestThreadContext()
{
    g_ppcContext = nullptr;
    g_heap.Free(block);
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
    try
    {
        func(ctx.ppcContext, g_memory.base);
    }
    catch (const GuestThreadExit&)
    {
        // ExTerminateThread unwinds to here.
    }

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
