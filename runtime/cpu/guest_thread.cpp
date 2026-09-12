#include "guest_thread.h"
#include <cstdio>
#include <exception>

#include <bit>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "../kernel/guestcall.h"
#include "../kernel/kobject.h"
#include "../kernel/heap.h"
#include "../kernel/memory.h"
#include "thread_budget.h"

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

// Guest thread id -> the host thread, for BindHostName. Filled at spawn (the
// std::thread's native handle exists before the new thread has run a single
// instruction) and by Run() for threads we did not spawn (the main guest thread).
static std::mutex g_hostThreadMutex;
static std::map<uint32_t, std::thread::native_handle_type> g_hostThreadFor;

static void RegisterHostThread(uint32_t threadId, std::thread::native_handle_type h)
{
    std::lock_guard lk(g_hostThreadMutex);
    g_hostThreadFor[threadId] = h;
}

// The named guest threads' host handles, for CpuSecondsOf. Keyed by the title's own
// name; a name reused for two threads (HavokWorkerThread) keeps the first.
static std::map<std::string, std::thread::native_handle_type> g_hostThreadByName;
static std::map<std::string, uint32_t> g_tidByName;                   // under g_hostThreadMutex

bool GuestThread::BindHostName(uint32_t threadId, const char* name)
{
    // The registry is kept on every platform (part 118: the Windows pin needs the
    // handle); only the OS-visible naming is platform-specific.
    std::thread::native_handle_type h;
    {
        std::lock_guard lk(g_hostThreadMutex);
        auto it = g_hostThreadFor.find(threadId);
        if (it == g_hostThreadFor.end())
            return false;
        h = it->second;
        g_hostThreadByName.emplace(name, h);
        g_tidByName.emplace(name, threadId);
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    char shortName[16]; // the kernel keeps 15 characters
    snprintf(shortName, sizeof shortName, "%s", name);
    return pthread_setname_np(h, shortName) == 0;
#else
    return true;
#endif
}

// Per-thread wait accumulators, registered under the guest tid at Run() (the thread
// itself, so no wait can precede the registration) and looked up by the title's name
// for the [fps] line. Never erased: a thread that ended keeps a stale-but-valid entry
// (the thread_local's storage outlives nothing here — the map holds a copy pointer to
// a thread_local, so ended threads are dropped by the same path that drops the PCR).
static thread_local GuestThread::WaitStats t_waitStats;
static std::map<uint32_t, GuestThread::WaitStats*> g_waitStatsByTid;   // under g_hostThreadMutex

GuestThread::WaitStats& GuestThread::MyWaitStats() { return t_waitStats; }

const GuestThread::WaitStats* GuestThread::WaitStatsOf(const char* name)
{
    std::lock_guard lk(g_hostThreadMutex);
    auto it = g_tidByName.find(name);
    if (it == g_tidByName.end())
        return nullptr;
    auto jt = g_waitStatsByTid.find(it->second);
    return jt == g_waitStatsByTid.end() ? nullptr : jt->second;
}

static inline uint64_t MonoNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// CZ_WAIT_CALLERS=1 (part 118): the same census keyed by the GUEST CALLER — the lr of
// the import call — per thread and kind, printed every 10 s from whichever wait ends
// the window. The [guestwait] line says the Main Thread spends 0.5 ms a frame in 5.7
// single-object waits; only the caller says which of the title's subsystems it is
// waiting FOR (a Havok step, a job, the Draw Thread), and that decides which lever
// moves it. Diagnostic arm only: a mutex on every wait exit.
static bool WaitCallersOn()
{
    static const bool on = getenv("CZ_WAIT_CALLERS") != nullptr;
    return on;
}
struct WaitCallerRow { uint64_t ns = 0, calls = 0; };
static std::mutex g_waitCallerMutex;
static std::map<std::string, WaitCallerRow> g_waitCallers;   // "name kind lr" -> row
static uint64_t g_waitCallerLastPrint = 0;
// The calling thread's comm (the title's name, bound by BindHostName), read once per
// thread; it is 15 characters at most and "?" where the platform cannot say.
static const char* CurrentThreadComm()
{
    static thread_local char comm[20] = {0};
    if (!comm[0])
    {
#if !defined(_WIN32) && !defined(__APPLE__)
        if (pthread_getname_np(pthread_self(), comm, sizeof comm) != 0 || !comm[0])
#endif
            snprintf(comm, sizeof comm, "?");
    }
    return comm;
}
static void WaitCallerRecord(GuestThread::WaitKind kind, uint64_t ns)
{
    static const char* const kindName[] = {"single", "multi", "sleep", "fence"};
    const uint32_t lr = g_ppcContext ? uint32_t(g_ppcContext->lr) : 0;
    // Two more frames up the guest's back chain (*(r1) = the caller's r1, its lr at
    // -8 from there), because the title's waits go through a WaitForMultipleObjects
    // wrapper (sub_82822548) and the lr at the import names only the wrapper.
    uint32_t lr2 = 0, lr3 = 0;
    if (g_ppcContext)
    {
        uint8_t* base = g_memory.base;
        uint32_t sp = g_ppcContext->r1.u32;
        for (int i = 0; i < 2 && sp >= 0x10000 && sp < PPC_MEMORY_SIZE - 8; ++i)
        {
            const uint32_t prev = PPC_LOAD_U32(sp);
            if (prev <= sp || prev - sp > 0x100000 || prev >= PPC_MEMORY_SIZE - 8)
                break;
            (i == 0 ? lr2 : lr3) = PPC_LOAD_U32(prev - 8);
            sp = prev;
        }
    }
    char key[128];
    // The comm alone aggregates: a thread inherits its creator's name until the title
    // names it, so four host threads read "Main Thread". The guest tid separates them.
    snprintf(key, sizeof(key), "%-12s %08X %-6s %08X<%08X<%08X", CurrentThreadComm(),
             GuestThread::GetCurrentThreadId(), kindName[kind], lr, lr2, lr3);
    std::lock_guard lk(g_waitCallerMutex);
    auto& r = g_waitCallers[key];
    r.ns += ns;
    r.calls += 1;
    const uint64_t now = MonoNs();
    if (g_waitCallerLastPrint == 0)
        g_waitCallerLastPrint = now;
    if (now - g_waitCallerLastPrint >= 10'000'000'000ull)
    {
        const double secs = double(now - g_waitCallerLastPrint) * 1e-9;
        fprintf(stderr, "[waitcallers] %.1f s window — thread kind caller: ms/s calls/s\n", secs);
        std::vector<std::pair<std::string, WaitCallerRow>> rows(g_waitCallers.begin(), g_waitCallers.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.ns > b.second.ns; });
        int n = 0;
        for (auto& [k, v] : rows)
        {
            if (v.ns * 1e-6 / secs < 0.5 || n++ >= 24)
                break;
            fprintf(stderr, "[waitcallers]   %s  %8.2f  %8.1f\n", k.c_str(), v.ns * 1e-6 / secs, v.calls / secs);
        }
        g_waitCallers.clear();
        g_waitCallerLastPrint = now;
    }
}

GuestThread::WaitScope::WaitScope(WaitKind k) : kind(k), t0(MonoNs()) {}
GuestThread::WaitScope::~WaitScope()
{
    const uint64_t ns = MonoNs() - t0;
    t_waitStats.ns[kind].fetch_add(ns, std::memory_order_relaxed);
    t_waitStats.calls[kind].fetch_add(1, std::memory_order_relaxed);
    if (WaitCallersOn())
        WaitCallerRecord(kind, ns);
}

void GuestThread::PinHostByName(const char* name)
{
    std::thread::native_handle_type h;
    {
        std::lock_guard lk(g_hostThreadMutex);
        auto it = g_hostThreadByName.find(name);
        if (it == g_hostThreadByName.end())
            return;
        h = it->second;
    }
    ThreadBudget_PinNamedThread(name, h);
}

double GuestThread::CpuSecondsOf(const char* name)
{
#if defined(__linux__) || defined(_WIN32)
    std::thread::native_handle_type h;
    {
        std::lock_guard lk(g_hostThreadMutex);
        auto it = g_hostThreadByName.find(name);
        if (it == g_hostThreadByName.end())
            return -1.0;
        h = it->second;
    }
#endif
#if defined(_WIN32)
    // The Windows spelling of the thread clock (owed since part 116): kernel + user
    // time in 100 ns units.
    FILETIME c, e, k, u;
    if (!GetThreadTimes(HANDLE(h), &c, &e, &k, &u))
        return -1.0;
    const uint64_t kt = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const uint64_t ut = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return double(kt + ut) * 1e-7;
#elif defined(__linux__)
    clockid_t cid;
    if (pthread_getcpuclockid(h, &cid) != 0)
        return -1.0;
    timespec ts{};
    if (clock_gettime(cid, &ts) != 0)
        return -1.0;
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
#else
    (void)name;
    return -1.0;
#endif
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
#if !defined(_WIN32)
    RegisterHostThread(GuestThread::GetCurrentThreadId(), pthread_self());
#else
    // GetCurrentThread() is a pseudo-handle valid only on this thread; the registry is
    // read from others (the pin, the thread clock), so a real one is duplicated. Without
    // this the title's own naming of its Main Thread — which runs on main.cpp's raw
    // std::thread, not a GuestThreadHandle — found nothing to bind (part 118, czwin).
    {
        HANDLE real = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, 0,
                            FALSE, DUPLICATE_SAME_ACCESS))
            RegisterHostThread(GuestThread::GetCurrentThreadId(), real);
    }
#endif
    {
        std::lock_guard lk(g_hostThreadMutex);
        g_waitStatsByTid[GuestThread::GetCurrentThreadId()] = &t_waitStats;
    }
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
    {
        // The thread_local dies with this thread; drop the pointer before it does.
        std::lock_guard lk(g_hostThreadMutex);
        g_waitStatsByTid.erase(GuestThread::GetCurrentThreadId());
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
    RegisterHostThread(threadId, thread.native_handle());
    // CZ_GUEST_PIN: a thread spawned by a PINNED thread inherits its one-CPU mask (Linux;
    // on Windows the process's), and the Main Thread spawns everything (the Havok
    // workers, the job pool, audio) after it is named — the first build of the arm ran
    // all of them on the Main Thread's core and the frame went to 25 fps. Every spawn
    // gets the "rest" mask.
    ThreadBudget_PinRest(thread.native_handle());
}

GuestThreadHandle::~GuestThreadHandle()
{
    // NT semantics: closing a thread handle never stops or waits for the thread.
    //
    // AND IT MUST NOT THROW, which `if (joinable()) detach();` alone does not
    // guarantee. This object lives in GUEST memory, so its std::thread member is
    // whatever the guest last left in those bytes. joinable() only tests the id field,
    // so it can read true off a scribbled _Thrd_t whose OS handle is garbage —
    // std::thread::detach() then throws std::system_error, the exception escapes a
    // destructor that is implicitly noexcept, and std::terminate() calls __fastfail.
    //
    // __fastfail bypasses SEH entirely: no vectored handler, no unhandled filter, no
    // output. On Windows this presented as the process vanishing at 0xC0000409 with
    // WER blaming ucrtbase.dll and the crash reporter printing nothing — a fatal error
    // whose own diagnosis had been destroyed by the mechanism that caused it. Linux
    // survives the same scribble because libstdc++'s detach() on a stale-but-plausible
    // id happens not to fail.
    //
    // Catching is not papering over it: the thread, if it exists at all, is detached by
    // NT semantics anyway, and there is nothing to clean up. What we lose is a handle
    // we were never going to close; what we gain is that a scribbled object cannot kill
    // the process without saying so.
    try
    {
        if (thread.joinable())
            thread.detach();
    }
    catch (const std::exception& e)
    {
        static int failed = 0;
        if (failed++ < 16)
            fprintf(stderr,
                    "[kobj] thread handle %08X: detach failed (%s). Its std::thread "
                    "state is not ours any more — the guest overwrote the object. "
                    "Ignored; nothing to clean up under NT handle semantics.\n",
                    threadId, e.what());
    }
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
