// Kernel-import HLE for the Dead Rising 2: Case Zero XenonRecomp runtime —
// phase 1 slice: memory, synchronisation, threads, TLS, time, the Rtl helpers, a
// mini printf, and the config/loader queries the CRT's bring-up ends on.
//
// SCOPE DISCIPLINE
// ----------------
// This file implements ONLY names Case Zero actually imports, verified against
// `ppc/ppc_recomp_shared.h`'s 244 `__imp__` externs (gotcha 10 — the image is the
// authority, not the previous port and not the capture). A1's own import dump
// agrees: 244 `F` entries and 13 `V` entries. `tools/gen_import_stubs.py` scans
// this file and refuses to emit a stub for anything defined here, so the two never
// collide.
//
// Where Asura's Wrath's runtime had an implementation for an import Case Zero does
// not declare, it is simply absent rather than carried along — nineteen of them,
// including `ExAllocatePool`, both `Interlocked*SList` entry points, the mutant
// pair and `RtlInitUnicodeString`. Carrying those would be dead code that reads
// like coverage.
//
// ORDER OF WORK
// -------------
// Implemented in the order A1 shows this title first calling them, not in the order
// another port needed them. That order is the phase 1 gate
// (`tools/kernel_call_diff.py`), and A1's first-occurrence sequence opens:
//
//   RtlImageXexHeaderField, NtAllocateVirtualMemory, RtlInitializeCriticalSection,
//   XexCheckExecutablePrivilege, KeTlsAlloc, KeTlsSetValue, KeQuerySystemTime,
//   XexLoadImage, XexGetProcedureAddress, ..., RtlRaiseException, MmAllocate...
//
// NOTHING HERE MAY FAKE SUCCESS
// -----------------------------
// Gotcha 5 — Fable 2 lost weeks to a stub that returned "OK" from an XMA context
// call. A kernel call we cannot service returns an error the guest can see. And per
// Asura's Wrath's finding 14, an error return is necessary but not sufficient: an
// import with an out-parameter must fill it, because the guest frequently ignores
// the status and reads the buffer anyway. Every function below that takes an
// out-pointer writes it on every path, including its failure paths.
//
// ABI facts (arg registers, struct layouts, the 49.875 MHz performance-counter
// frequency) follow Xenia and the public Xbox 360 kernel docs. Where a value came
// off our own ground truth the A1 line is quoted in the comment.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../cpu/crash_report.h"
#include "../cpu/guest_thread.h"
// For CZ_TIMEBASE_HZ, so KeQueryPerformanceFrequency and the guest's own `mftb`
// cannot drift apart — the header says they must agree, and sharing the constant is
// what makes that enforceable rather than aspirational. Including it here is safe
// despite gotcha 32 (the `#define __rdtsc()` must not be in scope when the system
// intrinsics headers are read) because timebase.h pulls <x86intrin.h> in itself
// before shadowing, which sets the guard; nothing below re-declares __rdtsc.
#include "../cpu/timebase.h"
#include "../host/window.h"   // XamInputGetState's device (phase 3)
#include "content.h"          // the save-data layer: enumerators and their message
#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "xex_imports.h"

// ---------------------------------------------------------------------------
// Stub helpers
// ---------------------------------------------------------------------------

// An import whose signature we do not model, returning `ret`. Use only where the
// return value is defensible; "we have not written this" belongs in the generated
// honest-failure stub file, not here.
#define STUB_RET(name, ret)                                          \
    PPC_FUNC(__imp__##name)                                          \
    {                                                                \
        KCALL(#name);                                                \
        ctx.r3.u64 = (ret);                                          \
    }

// ---------------------------------------------------------------------------
// Virtual memory / pools / physical memory
// ---------------------------------------------------------------------------

// CZ_MEM_TRACE=1 — every virtual-memory call, with its arguments AND its answer.
//
// The "big allocation" fprintfs below log only blocks over 4 MB and only the base,
// which is enough to spot a leak and useless for the question that actually arises:
// this title's heap manager RESERVEs a segment and then COMMITs sub-ranges into it,
// and when it later walks its block chain off the end of what it committed, nothing
// in a size-filtered log says which region the walk left. A trace has to carry the
// reserve/commit distinction and the returned extent, because those are exactly
// what the guest's bookkeeping is built from.
static bool MemTrace()
{
    static const bool on = getenv("CZ_MEM_TRACE") != nullptr;
    return on;
}

static uint32_t NtAllocateVirtualMemory_x(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
                                          uint32_t allocType, uint32_t protect,
                                          uint32_t debugMemory)
{
    if (!baseAddress || !regionSize)
        return STATUS_INVALID_PARAMETER;

    const uint32_t reqBaseIn = *baseAddress, reqSizeIn = *regionSize;

    uint32_t size = (*regionSize + 0xFFF) & ~0xFFFu;
    if (*baseAddress != 0)
    {
        constexpr uint32_t MEM_RESERVE = 0x2000;
        // Explicit-base RESERVE. A game's VM manager probes 64 KB-aligned bases
        // upward until one succeeds, so returning success unconditionally makes
        // every probe "win" at the first address and all of the title's
        // fixed-address buffers silently alias each other. The reservation arena
        // gives these real conflict semantics (Xenia returns C0000017 on conflict
        // and the guest's scan advances).
        const uint32_t reqBase = *baseAddress;
        if ((allocType & MEM_RESERVE) && reqBase >= 0x40000000 && reqBase < 0x7FE00000)
        {
            const uint32_t got = g_heap.ReserveVirtualAt(reqBase, size);
            if (MemTrace())
                fprintf(stderr,
                        "[mem] alloc RESERVE@%08X size=%08X type=%08X -> %s base=%08X "
                        "size=%08X\n",
                        reqBaseIn, reqSizeIn, allocType, got ? "OK" : "NO_MEMORY", got,
                        (size + 0xFFFF) & ~0xFFFFu);
            if (!got)
                return STATUS_NO_MEMORY;
            *regionSize = (size + 0xFFFF) & ~0xFFFFu;
            return STATUS_SUCCESS;
        }
        // Commit (or re-reserve) inside a region we already handed out. The whole
        // 4 GB space is host-committed on first touch, so the *mapping* is a no-op —
        // but the returned RegionSize is NOT.
        //
        // NT rounds [base, base+size) out to whole pages and reports the adjusted
        // size, and the page size here is the one the region was reserved with:
        // 64 KB for a MEM_LARGE_PAGES region, not 4 KB. The title's heap manager
        // builds its uncommitted-range bookkeeping out of this number, so a 4 KB
        // answer inside a 64 KB region puts its segment boundary somewhere the
        // console would never put it (gotcha 9: round every size the way the console
        // rounds it, because the out-parameter is the contract and this one is
        // believed).
        //
        // Case Zero exercises this path heavily and visibly. A1:
        //   NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0) = 40000000
        //   NtAllocateVirtualMemory(base=40000000, size=00010000, 60001000, 4, 0) = 40000000
        //   NtAllocateVirtualMemory(base=40010000, size=00020000, 60001000, 4, 0)
        // — a 1 MB large-page reservation followed by 64 KB-granular commits walking
        // up through it.
        const uint32_t gran =
            (reqBase >= 0x40000000 && reqBase < 0x7FE00000) ? 0x10000u : 0x1000u;
        size = uint32_t((uint64_t(reqBase) + reqSizeIn + gran - 1) & ~uint64_t(gran - 1)) -
               (reqBase & ~(gran - 1));
        if (MemTrace())
        {
            // Report whether the commit actually lands in something we handed out.
            // "Success" on a base we never reserved is the shape that lets a guest
            // heap believe it owns memory no arena is tracking.
            uint32_t rb = 0, rs = 0;
            const bool tracked = g_heap.QueryRegion(reqBaseIn, rb, rs);
            fprintf(stderr,
                    "[mem] alloc COMMIT@%08X size=%08X type=%08X -> OK size=%08X"
                    "  [region %s base=%08X size=%08X end=%08X]\n",
                    reqBaseIn, reqSizeIn, allocType, size, tracked ? "HIT" : "**MISS**", rb, rs,
                    rb + rs);
        }
        *regionSize = size;
        return STATUS_SUCCESS;
    }

    // base == 0: served from one of the two virtual arenas, NOT the 512 MB physical
    // space — real hardware keeps those separate, and a title's boot-time virtual
    // pools would otherwise eat the physical arena the renderer needs.
    //
    // MEM_LARGE_PAGES picks the arena, and the guest can tell: its heap manager asks
    // for one or the other and then reasons about the address range it gets back
    // (see the split's rationale in kernel/heap.h). A1's first call here is a 1 MB
    // reservation with type 0x60002000 — MEM_16MB_PAGES | MEM_LARGE_PAGES |
    // MEM_RESERVE — and Xenia answers 0x40000000, which is exactly where our
    // large-page arena begins.
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
    void* ptr = g_heap.AllocVirtual(size, (allocType & MEM_LARGE_PAGES) != 0);
    if (size >= 0x400000)
        fprintf(stderr, "[heap] big NtAllocateVirtualMemory: %u MB -> %08X\n", size >> 20,
                ptr ? g_memory.MapVirtual(ptr) : 0);
    if (MemTrace())
        fprintf(stderr,
                "[mem] alloc NEW size=%08X type=%08X large=%d -> base=%08X end=%08X\n",
                reqSizeIn, allocType, (allocType & MEM_LARGE_PAGES) != 0,
                ptr ? g_memory.MapVirtual(ptr) : 0, ptr ? g_memory.MapVirtual(ptr) + size : 0);
    if (!ptr)
        return STATUS_NO_MEMORY;
    *baseAddress = g_memory.MapVirtual(ptr);
    *regionSize = size;
    return STATUS_SUCCESS;
}

static uint32_t NtFreeVirtualMemory_x(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
                                      uint32_t freeType)
{
    constexpr uint32_t MEM_RELEASE = 0x8000;
    if (baseAddress && *baseAddress && (freeType & MEM_RELEASE))
    {
        // The alloc side logs big blocks; without the matching free log a leak and
        // legitimate churn look identical in the record.
        const size_t sz = g_heap.Size(g_memory.Translate(*baseAddress));
        if (sz >= 0x400000)
            fprintf(stderr, "[heap] big NtFreeVirtualMemory: %zu MB at %08X\n", sz >> 20,
                    uint32_t(*baseAddress));
        g_heap.Free(g_memory.Translate(*baseAddress));
    }
    return STATUS_SUCCESS;
}

// X_MEMORY_BASIC_INFORMATION: {BaseAddress, AllocationBase, AllocationProtect,
// RegionSize, State, Protect, Type} — 28 bytes, all big-endian.
//
// A1 never calls this, and it is implemented anyway. That is a deliberate exception
// to "implement what the capture shows", and Asura's Wrath is the reason: there its
// CRT heap manager called it 1,235 times in the boot and *ignored the return
// status*, reading the out-buffer regardless — left as an honest-failure stub it
// consumed uninitialised memory and hung walking a hash chain that had become
// circular. Case Zero imports it, so the guest has a code path that reaches it even
// if this drive did not; the cost of getting it right now is minutes, and the cost
// of the alternative is measured in that port's days.
static uint32_t NtQueryVirtualMemory_x(uint32_t baseAddress, be<uint32_t>* info,
                                       uint32_t infoLength)
{
    if (!info)
        return STATUS_INVALID_PARAMETER;

    uint32_t regionBase = 0, regionSize = 0;
    if (!g_heap.QueryRegion(baseAddress, regionBase, regionSize))
    {
        // Unmapped as far as we are concerned. Report a free region covering the
        // containing 64 KB granule rather than inventing a committed one.
        static std::atomic<int> warned{ 0 };
        if (warned.fetch_add(1) < 8)
            KLOG("NtQueryVirtualMemory(%08X): address is in no tracked arena\n", baseAddress);
        info[0] = baseAddress & ~0xFFFFu; // BaseAddress
        info[1] = 0;                      // AllocationBase
        info[2] = 0;                      // AllocationProtect
        info[3] = 0x10000;                // RegionSize
        info[4] = 0x10000;                // State = MEM_FREE
        info[5] = 0x01;                   // Protect = PAGE_NOACCESS
        info[6] = 0;                      // Type
        return STATUS_SUCCESS;
    }

    info[0] = baseAddress & ~0xFFFu; // BaseAddress: page containing the query
    info[1] = regionBase;            // AllocationBase: what the allocator handed out
    info[2] = 0x04;                  // AllocationProtect = PAGE_READWRITE
    info[3] = regionBase + regionSize - (baseAddress & ~0xFFFu); // RegionSize to the end
    info[4] = 0x1000;                // State = MEM_COMMIT
    info[5] = 0x04;                  // Protect = PAGE_READWRITE
    info[6] = 0x20000;               // Type = MEM_PRIVATE
    if (MemTrace())
        fprintf(stderr,
                "[mem] query %08X -> base=%08X allocBase=%08X regionSize=%08X end=%08X "
                "COMMIT\n",
                baseAddress, uint32_t(info[0]), regionBase, uint32_t(info[3]),
                regionBase + regionSize);
    return STATUS_SUCCESS;
}

static uint32_t ExAllocatePoolTypeWithTag_x(uint32_t size, uint32_t tag, uint32_t type)
{
    void* ptr = g_heap.Alloc(size);
    return ptr ? g_memory.MapVirtual(ptr) : 0;
}

static void ExFreePool_x(uint32_t ptr)
{
    if (ptr)
        g_heap.Free(g_memory.Translate(ptr));
}

// A1's first call is the big one: MmAllocatePhysicalMemoryEx(0, 1BF16000, 4, 0,
// FFFFFFFF, 1000) — a single 447 MB physical reservation, which is the title
// claiming essentially all of the console's 512 MB up front. Our physical arena is
// 0xA0000000..0xBFFF0000 = 511.94 MB, so that request fits with room to spare; if it
// ever does not, GuestHeap::ReportExhaustion says so with the block census rather
// than just failing.
static uint32_t MmAllocatePhysicalMemoryEx_x(uint32_t flags, uint32_t size, uint32_t protect,
                                             uint32_t minAddress, uint32_t maxAddress,
                                             uint32_t alignment)
{
    void* ptr = g_heap.AllocPhysical(size, alignment);
    if (size >= 0x400000)
        fprintf(stderr,
                "[heap] big MmAllocatePhysicalMemoryEx: %u MB align=%x -> %08X lr=%08X\n",
                size >> 20, alignment, ptr ? g_memory.MapVirtual(ptr) : 0,
                uint32_t(g_ppcContext ? g_ppcContext->lr : 0));
    return ptr ? g_memory.MapVirtual(ptr) : 0;
}

static void MmFreePhysicalMemory_x(uint32_t type, uint32_t guestAddress)
{
    if (guestAddress)
        g_heap.Free(g_memory.Translate(guestAddress));
}

// The 360 has 512 MB of physical memory visible through three virtual windows
// (0xA0000000 cached, 0xC0000000 write-combined, 0xE0000000 uncached), so a physical
// address is the low 29 bits of any of them. Answering with a real physical address
// rather than echoing the virtual one is not cosmetic: the guest programs GPU
// registers with these values and does its own masking, and it makes our log lines
// directly comparable with a capture's.
//
// Addresses outside the physical windows are echoed unchanged and counted: our
// arenas are not backed by a page table, so there is no honest physical address to
// give for them, and inventing one would hand the guest a number that aliases real
// physical memory.
static uint32_t MmGetPhysicalAddress_x(uint32_t address)
{
    if (address >= 0xA0000000u)
        return address & 0x1FFFFFFFu;

    static std::atomic<uint32_t> nonPhysical{ 0 };
    if (nonPhysical.fetch_add(1) < 8)
        KLOG("MmGetPhysicalAddress(%08X): not in a physical window, echoing the virtual "
             "address (lr=%08X)\n",
             address, uint32_t(g_ppcContext ? g_ppcContext->lr : 0));
    return address;
}

static uint32_t MmQueryAddressProtect_x(uint32_t)
{
    return 0x04; // PAGE_READWRITE
}

static uint32_t MmSetAddressProtect_x(uint32_t, uint32_t, uint32_t)
{
    return 0;
}

// A1: MmMapIoSpace(00000002, 1FCAA000, 00000040, 00000404) and a second one 0x40
// bytes further on — bus 2, two 64-byte windows onto physical 0x1FCAA000,
// PAGE_READWRITE|PAGE_NOCACHE.
//
// Note the address: 0x1FCAA000 is ordinary physical RAM (it sits in the same page
// neighbourhood as the ring buffer the driver allocated a moment earlier), not a
// device register aperture. So "mapping" it is just naming its virtual alias, which
// our flat address space already has. Returning the cached-view alias is therefore
// the real answer, not an approximation of one.
static uint32_t MmMapIoSpace_x(uint32_t busNumber, uint32_t physicalAddress, uint32_t size,
                               uint32_t protect)
{
    const uint32_t mapped = 0xA0000000u | (physicalAddress & 0x1FFFFFFFu);
    KLOG("MmMapIoSpace(bus=%u, phys=%08X, %u bytes, protect=%X) -> %08X\n", busNumber,
         physicalAddress, size, protect, mapped);
    return mapped;
}

static uint32_t XamAlloc_x(uint32_t flags, uint32_t size, be<uint32_t>* outPtr)
{
    if (!outPtr)
        return STATUS_INVALID_PARAMETER;
    void* ptr = g_heap.Alloc(size);
    if (!ptr)
    {
        *outPtr = 0; // finding 14: fill the out-parameter even on the failure path
        return STATUS_NO_MEMORY;
    }
    *outPtr = g_memory.MapVirtual(ptr);
    return STATUS_SUCCESS;
}

static uint32_t XamFree_x(uint32_t ptr)
{
    if (ptr)
        g_heap.Free(g_memory.Translate(ptr));
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory_x)
GUEST_FUNCTION_HOOK(__imp__ExAllocatePoolTypeWithTag, ExAllocatePoolTypeWithTag_x)
GUEST_FUNCTION_HOOK(__imp__ExFreePool, ExFreePool_x)
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx_x)
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory_x)
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress_x)
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, MmQueryAddressProtect_x)
GUEST_FUNCTION_HOOK(__imp__MmSetAddressProtect, MmSetAddressProtect_x)
GUEST_FUNCTION_HOOK(__imp__MmMapIoSpace, MmMapIoSpace_x)
GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc_x)
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree_x)

// ---------------------------------------------------------------------------
// Critical sections & spinlocks (guest-side state, host atomics)
// ---------------------------------------------------------------------------

// Is `addr` plausibly part of a guest stack we can read?
//
// This exists because the obvious bound is wrong here, and wrong SILENTLY. Both
// template ports scan a stalled thread's stack for return addresses and bound the
// scan with `addr < 0x80000000`, which is true of a console guest stack and true of
// their runtimes' stacks. It is NOT true of ours: guest thread blocks come from the
// o1heap user arena, which kernel/heap.cpp places at 0x88000000. The bound is
// therefore false on the very first word, the scan breaks immediately, and every
// stall and wait trace prints "callers:" with nothing after it — a diagnostic that
// looks like it ran and found nothing, rather than one that never ran. (Gotcha 25's
// shape exactly: before believing an empty result, confirm the thing could have
// produced a non-empty one.)
//
// The honest bound is the one that is actually true of our map: inside the 4 GB
// space, past the null page, and 4-byte aligned.
static bool GuestStackAddressLooksSane(uint32_t addr)
{
    return addr >= 0x1000 && (addr & 3) == 0 && uint64_t(addr) + 4 <= PPC_MEMORY_SIZE;
}

static uint32_t CurrentGuestThreadId()
{
    // r13 (the PCR address) is unique per guest thread and never 0 once
    // bootstrapped, which makes it a better identity here than any host TID: it is
    // the same value the guest itself sees.
    return g_ppcContext ? g_ppcContext->r13.u32 : GuestThread::GetCurrentThreadId();
}

static uint32_t RtlInitializeCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    if (!cs)
        return STATUS_INVALID_PARAMETER;
    cs->Header.Absolute = 0;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    return STATUS_SUCCESS;
}

static uint32_t RtlInitializeCriticalSectionAndSpinCount_x(XRTL_CRITICAL_SECTION* cs,
                                                           uint32_t spin)
{
    const uint32_t status = RtlInitializeCriticalSection_x(cs);
    if (status == STATUS_SUCCESS)
        cs->Header.Absolute = static_cast<uint8_t>((spin + 255) >> 8);
    return status;
}

// Guest lock words are not always 4-byte aligned (games embed critical sections in
// packed structs), which std::atomic_ref asserts on. x86's lock-prefixed cmpxchg
// handles unaligned addresses, so use the __atomic builtins and plain yield loops
// (futex-based waiting would also require alignment).
static bool LockCas(uint32_t* word, uint32_t expected, uint32_t desired, uint32_t* actual)
{
    if (__atomic_compare_exchange_n(word, &expected, desired, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return true;
    *actual = expected;
    return false;
}

// Deliberately NOT ported: Fable 2's FABLE2_WAIT_RELEASE_CS, which dropped a
// thread's critical sections across an infinite wait to break a deadlock. It is
// unfaithful, it intermittently corrupted a handle, and the deadlock it worked
// around turned out to have a real fix elsewhere. If Case Zero ever needs something
// like it, that is a finding to write up — not a knob to inherit.
static uint32_t LookupThreadEntry(uint32_t tid);
static uint32_t LookupThreadEntryByPcr(uint32_t r13);

// --- Parking contended critical sections (finding 41) ----------------------
//
// WHY THIS MACHINERY EXISTS
//
// Case Zero starts two threads — `DnsLookupThread` (entry 0x82554F28) and the
// session shutdown thread (0x825C8960) — whose first act is to enter a critical
// section the main thread takes during start-up and never releases. That is the
// TITLE's design, not a bug of ours: on console those two threads block in the
// kernel and cost nothing for the life of the process.
//
// This function used to spin on `std::this_thread::yield()` with no backoff, so
// those two threads each burned a core from early boot to exit. `sched_yield()` on
// an otherwise-idle 16-core host returns immediately, which makes a "yield loop" a
// busy loop wearing a polite name. It never broke anything, but it perturbed every
// timing measurement in the port and it would be ruinous on a smaller host.
//
// The replacement is a three-phase wait, cheapest first:
//
//   1. `pause`-spin. No syscall, no scheduler. Covers a section another core is
//      about to release, which is what nearly all real contention is.
//   2. `sched_yield`. Covers a section held by a thread that is runnable but not
//      running — lets the owner on so it can finish.
//   3. Park on a host condition variable until the owner leaves. This is where the
//      two permanently-blocked threads live, and it takes them from a core each to
//      a 1 ms heartbeat.
//
// Phase 3 is a PARK, not a sleep, because a fixed sleep would trade one measurable
// cost for another: any section held longer than the yield phase would pay the sleep
// quantum in latency on every acquisition. The condition variable is signalled by
// `RtlLeaveCriticalSection`, so a normally-contended section wakes in microseconds.
namespace
{
// One slot per hash bucket, keyed by the section's address.
//
// It is a HASH, not a map, and the collisions are deliberate: two sections sharing a
// slot costs a spurious wakeup, and the waiter re-tests the lock word — the only
// thing that ever decides ownership — immediately afterwards. A real map would need
// its own lock on the leave path, i.e. a lock to implement locking.
struct CsParkSlot
{
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<uint32_t> waiters{ 0 };
};

constexpr size_t kCsParkSlots = 64;
CsParkSlot g_csPark[kCsParkSlots];

CsParkSlot& CsParkSlotFor(const void* cs)
{
    // Shift by 4 first: guest critical sections are 16-byte objects and the title
    // embeds several in one struct, so hashing the low bits straight in would pile a
    // whole object's worth of neighbouring sections onto neighbouring slots.
    return g_csPark[(reinterpret_cast<uintptr_t>(cs) >> 4) % kCsParkSlots];
}

inline void CsCpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// Phase boundaries, in failed acquisition attempts. Small on purpose: the point of
// the spin phases is to cover a handoff already in flight, not to gamble that a
// section will free up eventually. Anything that outlives them is better off parked.
constexpr uint32_t kCsPauseSpins = 64;
constexpr uint32_t kCsYieldSpins = 256;
constexpr uint32_t kCsParkAfter = kCsPauseSpins + kCsYieldSpins;

// CZ_CS_STATS=1: how many acquisitions actually reach each phase. This is the
// instrument for the ONE risk the change carries — that parking adds latency to
// ordinary, briefly-contended locks. "Almost every enter is uncontended" was the
// assumption the design rests on, so it gets measured rather than asserted.
//
// All four are counted ON SUCCESS, which is the right choice for the latency
// question and needs saying out loud, because it makes the headline number look
// wrong: a 60 s run reports `parked=2` while two threads are parked for its entire
// duration. Those two never acquire, so they never count. `parked` therefore reads
// as "acquisitions that had to park and then got the lock" — the population that
// pays a latency cost — and not as "threads currently parked".
std::atomic<uint64_t> g_csEnters{ 0 };
std::atomic<uint64_t> g_csContended{ 0 };   // needed at least one retry
std::atomic<uint64_t> g_csYielded{ 0 };     // outlived the pause phase
std::atomic<uint64_t> g_csParked{ 0 };      // outlived the yield phase too
} // namespace

static void RtlEnterCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    const uint32_t self = CurrentGuestThreadId();
    static const bool csTrace = getenv("CZ_CS_TRACE") != nullptr;
    static const bool csStats = getenv("CZ_CS_STATS") != nullptr;
    // CZ_CS_NO_BACKOFF=1 restores the pure yield spin. It is the same-binary control
    // arm for every claim made about this change (gotcha 86): the honest comparison
    // for "did the backoff cost throughput" is this binary with the knob on, not a
    // remembered number from a binary that no longer exists.
    static const bool noBackoff = getenv("CZ_CS_NO_BACKOFF") != nullptr;

    auto reportedAt = std::chrono::steady_clock::now();
    // Take the sequence number from the increment itself, not from a later load. The
    // first version of this counter reported from inside the `attempt != 0` branch and
    // tested a re-read total against `% 100000`, so a report needed a CONTENDED
    // acquisition to land exactly on a multiple of 100,000 — it printed nothing across
    // six 60 s runs, and "nothing printed" read as "no contention" rather than as an
    // instrument that could not fire (gotcha 25, in our own tooling again). Every
    // sequence number is owned by exactly one thread, so this reports exactly once per
    // 100,000 enters, contended or not.
    const uint64_t seq =
        csStats ? g_csEnters.fetch_add(1, std::memory_order_relaxed) + 1 : 0;

    for (uint32_t attempt = 0;; attempt++)
    {
        uint32_t previous = 0;
        if (LockCas(&cs->OwningThread, 0, self, &previous) || previous == self)
        {
            cs->RecursionCount++;
            if (csStats)
            {
                if (attempt != 0)
                    g_csContended.fetch_add(1, std::memory_order_relaxed);
                if (attempt > kCsPauseSpins)
                    g_csYielded.fetch_add(1, std::memory_order_relaxed);
                if (attempt > kCsParkAfter)
                    g_csParked.fetch_add(1, std::memory_order_relaxed);
                if ((seq % 100000) == 0)
                {
                    const uint64_t contended =
                        g_csContended.load(std::memory_order_relaxed);
                    KLOG("cs stats: enters=%llu contended=%llu (%.4f%%) yielded=%llu "
                         "parked=%llu\n",
                         (unsigned long long)seq, (unsigned long long)contended,
                         100.0 * double(contended) / double(seq),
                         (unsigned long long)g_csYielded.load(std::memory_order_relaxed),
                         (unsigned long long)g_csParked.load(std::memory_order_relaxed));
                }
            }
            return;
        }

        // CZ_CS_TRACE=1: name the owner of a section this thread cannot get. A frozen
        // run is almost always one thread holding what another needs, and this is the
        // cheapest way to see the pair.
        //
        // Both threads are named by ENTRY POINT, not just by r13. The first version
        // printed the PCR addresses alone, which says two threads are stuck without
        // saying which two — and the whole question in finding 38 was whether the
        // thread the fence is waiting FOR is one of the threads spinning here (a
        // cycle) or an innocent bystander (a lost wakeup). A raw r13 cannot answer
        // that; an entry point answers it on sight.
        //
        // Reported on ELAPSED TIME, not on a spin count. The original fired every 4 M
        // failed attempts, which was a serviceable proxy while this function busy-
        // waited and is meaningless now that it parks: a parked waiter reaches 4 M
        // attempts approximately never, so a count-based trace would fall silent
        // exactly when the wait got interesting. The clock is only read once every
        // 256 attempts, so the pause phase (64) never reads it at all.
        if (csTrace && (attempt & 255) == 255)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - reportedAt >= std::chrono::seconds(4))
            {
                reportedAt = now;
                fprintf(stderr,
                        "[csspin] self r13=%08X (entry=%08X) wants cs=%08X ownedBy "
                        "r13=%08X (entry=%08X) rec=%d lr=%08X\n",
                        self, LookupThreadEntry(GuestThread::GetCurrentThreadId()),
                        g_memory.MapVirtual(cs), previous,
                        LookupThreadEntryByPcr(previous), cs->RecursionCount,
                        g_ppcContext ? uint32_t(g_ppcContext->lr) : 0);
                CzDumpGuestBacktrace("cs spin");
            }
        }

        if (noBackoff)
        {
            std::this_thread::yield();
            continue;
        }
        if (attempt < kCsPauseSpins)
        {
            CsCpuRelax();
            continue;
        }
        if (attempt < kCsParkAfter)
        {
            std::this_thread::yield();
            continue;
        }

        CsParkSlot& slot = CsParkSlotFor(cs);
        slot.waiters.fetch_add(1, std::memory_order_seq_cst);
        {
            std::unique_lock<std::mutex> lock(slot.mutex);
            // Re-test UNDER the slot mutex. Without this the section could be
            // released between the failed CAS above and the waiter count going up,
            // and the releaser — seeing no waiters — would not signal. The timeout
            // makes that a latency bug rather than a hang; this makes it not happen.
            //
            // The timeout is the backstop, not the mechanism. Every wait here is
            // expected to end in a notify from RtlLeaveCriticalSection; 1 ms bounds
            // what a missed one can cost, and for the two permanently blocked threads
            // it is the entire cost of the wait.
            if (__atomic_load_n(&cs->OwningThread, __ATOMIC_ACQUIRE) != 0)
                slot.cv.wait_for(lock, std::chrono::milliseconds(1));
        }
        slot.waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
}

static void RtlLeaveCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    // ALARM ONLY — this deliberately does not change what the call does.
    //
    // A leave by a thread that does not own the section would be serious: the
    // decrement below is what releases the lock, so an unmatched one drives the count
    // negative and the real owner's own leave then never reaches zero. The section is
    // never released again, silently, and every other thread spins on a lock whose
    // owner has long since walked away — which is exactly what finding 38's stall
    // looked like from the outside, and is why the check exists.
    //
    // It has never fired. Refusing to release on a condition we have never observed
    // would be a behaviour change made on a hypothesis, and this session already
    // retired one of those (see finding 38's zero-header rule). Print it; if it ever
    // shows up, THEN decide what the right answer is, with the call site in hand.
    const uint32_t self = CurrentGuestThreadId();
    if (__atomic_load_n(&cs->OwningThread, __ATOMIC_ACQUIRE) != self)
    {
        static std::atomic<uint64_t> seen{ 0 };
        const uint64_t n = seen.fetch_add(1);
        if (n < 8 || (n & 0xFFFu) == 0)
            KLOG("RtlLeaveCriticalSection on cs=%08X by r13=%08X, owned by r13=%08X rec=%d "
                 "(lr=%08X, occurrence %llu)\n",
                 g_memory.MapVirtual(cs), self,
                 __atomic_load_n(&cs->OwningThread, __ATOMIC_RELAXED), cs->RecursionCount,
                 uint32_t(g_ppcContext ? g_ppcContext->lr : 0),
                 static_cast<unsigned long long>(n));
    }
    if (--cs->RecursionCount != 0)
        return;
    __atomic_store_n(&cs->OwningThread, 0, __ATOMIC_RELEASE);

    // Wake anyone parked on this section (finding 41).
    //
    // The fence is not decoration. The release store above and the waiter-count load
    // below are a store followed by a load of a DIFFERENT location, and store-load is
    // the one ordering x86-64's TSO does not give — precisely the case that made
    // XenonRecomp's `sync` lowering a bug (gotcha 92). Without it the CPU may answer
    // the load from before the store retires, so a waiter that has just registered
    // itself is not seen and sleeps out its timeout instead of being woken.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // The load is the entire cost on the uncontended path: one atomic read of a
    // host-side counter, no syscall, no condvar touched, no mutex taken. That matters
    // because this title enters and leaves critical sections tens of thousands of
    // times a second and virtually none of them have a waiter.
    CsParkSlot& slot = CsParkSlotFor(cs);
    if (slot.waiters.load(std::memory_order_seq_cst) != 0)
    {
        // Taking the slot mutex around the notify is what closes the other half of
        // the window: it cannot land between a waiter's re-test and its wait_for,
        // because the waiter holds this mutex across both.
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.cv.notify_all();
    }
}

static uint32_t RtlTryEnterCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    const uint32_t self = CurrentGuestThreadId();
    uint32_t previous = 0;
    if (LockCas(&cs->OwningThread, 0, self, &previous) || previous == self)
    {
        cs->RecursionCount++;
        return 1;
    }
    return 0;
}

static void SpinAcquire(uint32_t* lock)
{
    uint32_t previous;
    while (!LockCas(lock, 0, CurrentGuestThreadId(), &previous))
        std::this_thread::yield();
}

static void SpinRelease(uint32_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static void KfAcquireSpinLock_x(uint32_t* lock) { SpinAcquire(lock); }
static void KfReleaseSpinLock_x(uint32_t* lock) { SpinRelease(lock); }
static void KeAcquireSpinLockAtRaisedIrql_x(uint32_t* lock) { SpinAcquire(lock); }
static void KeReleaseSpinLockFromRaisedIrql_x(uint32_t* lock) { SpinRelease(lock); }

// Kernel-mode preemption control. We do not preempt guest threads at all, so doing
// nothing here is the faithful behaviour, not a shortcut.
static void KeEnterCriticalRegion_x() {}
static void KeLeaveCriticalRegion_x() {}

GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSectionAndSpinCount,
                    RtlInitializeCriticalSectionAndSpinCount_x)
GUEST_FUNCTION_HOOK(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__RtlTryEnterCriticalSection, RtlTryEnterCriticalSection_x)
GUEST_FUNCTION_HOOK(__imp__KfAcquireSpinLock, KfAcquireSpinLock_x)
GUEST_FUNCTION_HOOK(__imp__KfReleaseSpinLock, KfReleaseSpinLock_x)
GUEST_FUNCTION_HOOK(__imp__KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql_x)
GUEST_FUNCTION_HOOK(__imp__KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql_x)
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion_x)
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion_x)

// ---------------------------------------------------------------------------
// Events, semaphores, waits, handles
// ---------------------------------------------------------------------------

static uint32_t GuestTimeoutToMs(be<int64_t>* timeout)
{
    if (!timeout)
        return WAIT_TIMEOUT_INFINITE;
    const int64_t t = *timeout;
    if (t >= 0)
        return 0; // absolute times: treat as a poll
    return static_cast<uint32_t>((-t) / 10000);
}

struct Event final : KernelObject
{
    std::mutex m;
    std::condition_variable cv;
    bool manualReset;
    bool signaled;

    Event(XKEVENT* header) : manualReset(header->Type == 0), signaled(header->SignalState != 0) {}
    Event(bool manualReset, bool initialState) : manualReset(manualReset), signaled(initialState) {}
    using guest_type = XKEVENT;

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(m);
        if (timeoutMs == WAIT_TIMEOUT_INFINITE)
            cv.wait(lock, [&] { return signaled; });
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return signaled; }))
            return STATUS_TIMEOUT;
        if (!manualReset)
            signaled = false;
        return STATUS_SUCCESS;
    }

    void Set()
    {
        std::lock_guard lock(m);
        signaled = true;
        // notify_all even for auto-reset events. Exactly-one-release is still
        // guaranteed by the mutex plus the `signaled` flip inside Wait, and
        // notify_one starves a waiter when other threads re-wait on the same object
        // every frame — Fable 2's finding 44 livelock, where the render workers kept
        // eating the wakeup meant for the init thread.
        cv.notify_all();
    }

    void Reset()
    {
        std::lock_guard lock(m);
        signaled = false;
    }
};

struct Semaphore final : KernelObject
{
    std::mutex m;
    std::condition_variable cv;
    uint32_t count;
    uint32_t maximum;

    Semaphore(XKSEMAPHORE* sem) : count(sem->Header.SignalState), maximum(sem->Limit) {}
    Semaphore(uint32_t count, uint32_t maximum) : count(count), maximum(maximum) {}
    using guest_type = XKSEMAPHORE;

    uint32_t Wait(uint32_t timeoutMs) override
    {
        std::unique_lock lock(m);
        auto ready = [&] { return count > 0; };
        if (timeoutMs == WAIT_TIMEOUT_INFINITE)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return STATUS_TIMEOUT;
        count--;
        return STATUS_SUCCESS;
    }

    uint32_t Release(uint32_t releaseCount)
    {
        std::lock_guard lock(m);
        const uint32_t previous = count;
        count += releaseCount;
        cv.notify_all();
        return previous;
    }
};

// A handle is just a guest pointer with bit 31 set, so a stale or garbage one
// translates to arbitrary memory and Set()/Reset() would take a mutex that was never
// a mutex — glibc aborts the process on the spot, with no guest context. Check the
// live registry first and report the caller instead.
static Event* ResolveEvent(uint32_t handle, const char* who)
{
    if (!handle || !IsKernelObject(handle))
        return nullptr;
    if (!IsLiveKernelHandle(handle))
    {
        static std::atomic<int> warned{ 0 };
        if (warned.fetch_add(1) < 32)
            fprintf(stderr, "[kobj] %s on dead/unknown handle %08X lr=%08X tid=%08X\n", who,
                    handle, g_ppcContext ? uint32_t(g_ppcContext->lr) : 0, CurrentGuestThreadId());
        return nullptr;
    }
    return GetKernelObject<Event>(handle);
}

// eventType 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent.
// A1's first one, with its object-attributes name intact:
//   NtCreateEvent(7018FA70, 7018FA80(FFFFFFFC,"Async Pending Event",00000080), 1, 0)
static uint32_t NtCreateEvent_x(be<uint32_t>* handle, void* attrs, uint32_t eventType,
                                uint32_t initialState)
{
    if (!handle)
        return STATUS_INVALID_PARAMETER;
    Event* event = CreateKernelObject<Event>(eventType == 0, initialState != 0);
    if (!event)
    {
        *handle = 0;
        return STATUS_NO_MEMORY;
    }
    *handle = GetKernelHandle(event);
    return STATUS_SUCCESS;
}

static uint32_t NtSetEvent_x(uint32_t handle, be<uint32_t>* previousState)
{
    Event* event = ResolveEvent(handle, "NtSetEvent");
    if (previousState)
        *previousState = event && event->signaled ? 1 : 0;
    if (!event)
        return STATUS_INVALID_HANDLE;
    event->Set();
    return STATUS_SUCCESS;
}

// Declared in file_imports.cpp, defined here because the Event type lives here.
// NtReadFile/NtWriteFile take an optional event handle that NT signals on
// completion; ours complete synchronously but still owe the signal, or an IO thread
// parked on that event never wakes and the file it wanted looks like a hang rather
// than a missing feature. dynamic_cast rather than ResolveEvent's static one: the
// handle comes straight from a guest argument and need not be an event at all.
void SignalGuestEvent(uint32_t handle)
{
    if (!handle || !IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return;
    if (auto* event = dynamic_cast<Event*>(GetKernelObject(handle)))
        event->Set();
}

// ONE argument, and that is not a detail — see below.
//
// NtSetEvent takes (handle, previousState); NtClearEvent takes (handle) alone. This
// was written with NtSetEvent's signature and an out-parameter fill added for
// finding 14 compliance, which meant it read r4 — a register the caller had left
// holding something else entirely — and stored through it. The result was a SIGSEGV
// inside our own kernel, on a write the guest never asked for.
//
// The lesson generalises past this call and belongs next to finding 14 rather than
// buried here: **"fill your out-parameters" is only safe on top of a correct
// signature.** Filling an out-parameter that does not exist converts a harmless
// leftover register into a wild store, and it fails in the one place the rule was
// supposed to protect. A5 settles the arity in one grep — `NtClearEvent(F8000020)`
// against `NtSetEvent(F8000014, 00000000)` — because both are kHighFrequency and
// appear nowhere else.
static uint32_t NtClearEvent_x(uint32_t handle)
{
    Event* event = ResolveEvent(handle, "NtClearEvent");
    if (!event)
        return STATUS_INVALID_HANDLE;
    event->Reset();
    return STATUS_SUCCESS;
}

static uint32_t KeSetEvent_x(XKEVENT* event, uint32_t increment, uint32_t wait)
{
    QueryKernelObject<Event>(*event)->Set();
    return 1;
}

// A1 calls this 1,879 times through the boot — the fourth most frequent kernel call
// in the whole capture — so it is on a genuinely hot path, not an init-time one.
static uint32_t KeResetEvent_x(XKEVENT* event)
{
    QueryKernelObject<Event>(*event)->Reset();
    return 0;
}

// A1: NtCreateSemaphore(7018FA40, 7018FA50, initial=0, maximum=0x10) — 26 of them
// through the boot, all with the same shape.
static uint32_t NtCreateSemaphore_x(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attrs,
                                    uint32_t initialCount, uint32_t maximumCount)
{
    if (!handle)
        return STATUS_INVALID_PARAMETER;
    Semaphore* sem = CreateKernelObject<Semaphore>(initialCount, maximumCount);
    if (!sem)
    {
        *handle = 0;
        return STATUS_NO_MEMORY;
    }
    *handle = GetKernelHandle(sem);
    return STATUS_SUCCESS;
}

static uint32_t NtReleaseSemaphore_x(Semaphore* sem, uint32_t releaseCount,
                                     be<int32_t>* previousCount)
{
    if (!sem)
    {
        if (previousCount)
            *previousCount = 0;
        return STATUS_INVALID_HANDLE;
    }
    const uint32_t previous = sem->Release(releaseCount);
    if (previousCount)
        *previousCount = static_cast<int32_t>(previous);
    return STATUS_SUCCESS;
}

// A wait on an object the guest handed us that we cannot use — currently only a
// null pointer. Distinct from STATUS_TIMEOUT so a caller can tell "not signalled
// yet" from "there was nothing here to wait on".
constexpr uint32_t kWaitObjectUnusable = 0xFFFFFFFEu;

// Wait on a kernel object, and under CZ_WAIT_TRACE=1 name it if it never returns.
//
// This exists in ONE place, shared by the handle-level (Nt) and header-level (Ke)
// paths, because it used to exist in only one of them. The trace covered
// NtWaitForSingleObjectEx and nothing else, so a frozen run printed the thread that
// was waiting on a handle and stayed completely silent about every thread parked in
// a Ke wait on a dispatcher header — including, it turned out, the one thread whose
// state decided whether finding 38 was a deadlock cycle or a lost wakeup. A trace
// that can only see half the wait surface answers "who else is stuck?" with silence
// that looks like "nobody".
static bool WaitTraceOn()
{
    static const bool on = getenv("CZ_WAIT_TRACE") != nullptr;
    return on;
}

// The multi-object waits are polling loops rather than blocking calls, so they get
// the same 5 s report from the inside of the poll. The Draw Thread's fence wait is
// one of these — a WaitForMultipleObjectsEx over four handles — so without this the
// single busiest wait in the title is the one the trace cannot see.
static void ReportStuckMultiWait(uint32_t count, const uint32_t* ids, int seconds)
{
    char list[128] = {};
    size_t used = 0;
    for (uint32_t i = 0; i < count && used + 10 < sizeof(list); i++)
        used += snprintf(list + used, sizeof(list) - used, "%s%08X", i ? "," : "", ids[i]);
    fprintf(stderr, "[wait] tid=%08X r13=%08X (entry=%08X) any-of[%s] lr=%08X stuck %ds\n",
            GuestThread::GetCurrentThreadId(),
            g_ppcContext ? uint32_t(g_ppcContext->r13.u32) : 0,
            LookupThreadEntry(GuestThread::GetCurrentThreadId()), list,
            g_ppcContext ? uint32_t(g_ppcContext->lr) : 0, seconds);
    CzDumpGuestBacktrace("blocked wait-any");
}

static uint32_t WaitObject(KernelObject* obj, uint32_t timeoutMs, uint32_t id)
{
    const bool waitTrace = WaitTraceOn();
    if (!waitTrace || timeoutMs != WAIT_TIMEOUT_INFINITE)
        return obj->Wait(timeoutMs);

    for (int slice = 0;; slice++)
    {
        const uint32_t r = obj->Wait(5000);
        if (r != STATUS_TIMEOUT)
            return r;
        const char* kind = dynamic_cast<Event*>(obj)       ? " EVENT"
                           : dynamic_cast<Semaphore*>(obj) ? " SEMAPHORE"
                                                           : "";
        fprintf(stderr,
                "[wait] tid=%08X r13=%08X (entry=%08X) obj=%08X lr=%08X stuck %ds%s\n",
                GuestThread::GetCurrentThreadId(),
                g_ppcContext ? uint32_t(g_ppcContext->r13.u32) : 0,
                LookupThreadEntry(GuestThread::GetCurrentThreadId()), id,
                g_ppcContext ? uint32_t(g_ppcContext->lr) : 0, (slice + 1) * 5, kind);
        // The exact LR back-chain, not a scan — see cpu/crash_report.cpp.
        CzDumpGuestBacktrace("blocked wait");
    }
}

// Dispatcher-header waits (Ke level): resolve by the header's Type field.
//
// The null check is not defensive padding. Until it existed, ANY guest that passed
// a null dispatcher object to a Ke wait took the host down with a SIGSEGV at
// address 0 — a fault the crash reporter correctly labels "outside the 4 GB guest
// space, a host-side bug", which is a true statement that names our kernel rather
// than the guest that provoked it. Case Zero does exactly this during audio
// bring-up: the render-driver callback reads its wait objects out of an object the
// mixer thread has not finished constructing (kernel/audio.cpp), and gets two
// zeros. On hardware that would bugcheck, so there is no faithful answer to copy;
// what there is instead is a rule — a guest-supplied pointer must never be
// dereferenced without a check, however impossible the null looks.
static uint32_t WaitDispatcher(XDISPATCHER_HEADER* header, uint32_t timeoutMs)
{
    if (!header)
    {
        static std::atomic<uint32_t> complained{ 0 };
        if (complained.fetch_add(1) < 4)
            KLOG("Ke wait on a NULL dispatcher object (lr=%08X) — reporting it as "
                 "unusable rather than faulting\n",
                 uint32_t(g_ppcContext ? g_ppcContext->lr : 0));
        return kWaitObjectUnusable;
    }
    switch (header->Type)
    {
        case 0: // NotificationEvent
        case 1: // SynchronizationEvent
            return WaitObject(QueryKernelObject<Event>(*header), timeoutMs,
                              g_memory.MapVirtual(header));
        case 5: // Semaphore
            return WaitObject(QueryKernelObject<Semaphore>(*header), timeoutMs,
                              g_memory.MapVirtual(header));
        default:
            KLOG("KeWait on unhandled dispatcher type %u\n", header->Type);
            return STATUS_TIMEOUT;
    }
}

static uint32_t KeWaitForSingleObject_x(XDISPATCHER_HEADER* object, uint32_t reason,
                                        uint32_t mode, uint32_t alertable, be<int64_t>* timeout)
{
    const uint32_t status = WaitDispatcher(object, GuestTimeoutToMs(timeout));
    return status == kWaitObjectUnusable ? STATUS_INVALID_PARAMETER : status;
}

static bool DrainThreadApcs();
static bool FireDueTimerApcs();

// tid -> guest entry point, for naming stuck threads in the wait trace.
static std::mutex g_threadEntryMutex;
static std::map<uint32_t, uint32_t> g_threadEntries;
static void RegisterThreadEntry(uint32_t tid, uint32_t entry)
{
    std::lock_guard lk(g_threadEntryMutex);
    g_threadEntries[tid] = entry;
}
static uint32_t LookupThreadEntry(uint32_t tid)
{
    std::lock_guard lk(g_threadEntryMutex);
    auto it = g_threadEntries.find(tid);
    return it != g_threadEntries.end() ? it->second : 0;
}

// r13 is what a lock word records as its owner; the thread registry is keyed by
// thread id. Bridge the two so a stall trace can name both sides of a contention.
static uint32_t LookupThreadEntryByPcr(uint32_t r13)
{
    return LookupThreadEntry(GuestThread::ThreadIdForPcr(r13));
}

static uint32_t NtWaitForSingleObjectEx_x(uint32_t handle, uint32_t mode, uint32_t alertable,
                                          be<int64_t>* timeout)
{
    if (alertable && (DrainThreadApcs() | (int)FireDueTimerApcs()))
        return STATUS_USER_APC;
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
    {
        KLOG("NtWaitForSingleObjectEx on non-kernel/dead handle 0x%X\n", handle);
        return STATUS_INVALID_HANDLE;
    }
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    // CZ_WAIT_TRACE=1 names this if it never returns; see WaitObject.
    return WaitObject(GetKernelObject(handle), timeoutMs, handle);
}

// The polling loop behind both wait-any paths (guest handles and dispatcher
// headers). It is one function because it was two, and the two had drifted.
//
// Three things it has to get right that the original pair did not:
//
//  - A FINITE timeout expires. Only `timeoutMs == 0` used to leave the loop, so a
//    caller asking to wait 500 ms waited forever. That is the same defect shape as a
//    missing signal and it would have been read as one. Infinite waits — which is
//    what every caller in this title's boot actually passes — are unaffected.
//  - CZ_WAIT_TRACE names it. See ReportStuckMultiWait.
//
// And one thing it deliberately does NOT do by default. An alertable wait should run
// pending APCs and report STATUS_USER_APC; NT does, and the guest expects it (the
// Draw Thread's wait loop opens with `cmplwi cr6, r3, 0xC0` and branches back to the
// wait). We drain APCs at the single-object waits and at KeDelayExecutionThread but
// not here, so an IO completion queued to a thread that then parks in a multi-object
// wait never runs — the queue is thread-local, so nobody else drains it either.
//
// Draining them here is almost certainly right in principle. It is off because
// nothing has yet shown it is needed: it was written to chase finding 38's stall, and
// that stall turned out to be a dropped GPU fence packet with no APC anywhere near it.
// An unmeasured change to when guest callbacks run is not something to enable by
// default on the strength of "NT does this" alone, however true that is.
//
// (It was briefly blamed for moving the A1 gate. It was not: the same permutation of
// positions 71-73 appears on the committed binary. See cpu/guest_thread.cpp.)
//
// Turn it on with CZ_MULTIWAIT_APC=1. If a real APC-starvation bug ever turns up,
// promote it then — with the gate numbers taken from both binaries on the same day.
template <typename Poll, typename Id>
static uint32_t WaitAnyPoll(uint32_t count, uint32_t timeoutMs, uint32_t alertable, Poll poll,
                            Id id)
{
    static const bool drainApcs = getenv("CZ_MULTIWAIT_APC") != nullptr;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t tick = 0;; tick++)
    {
        if (alertable && drainApcs && (DrainThreadApcs() | (int)FireDueTimerApcs()))
            return STATUS_USER_APC;
        for (uint32_t i = 0; i < count; i++)
            if (poll(i))
                return STATUS_WAIT_0 + i;
        if (timeoutMs == 0)
            return STATUS_TIMEOUT;
        if (timeoutMs != WAIT_TIMEOUT_INFINITE &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
        {
            // Announced the first few times, because this return did not exist before
            // and "a wait that now ends" is a behaviour change, not a bug fix, until
            // it is shown to happen. If these lines never appear, the change is inert
            // and cannot be behind any gate movement.
            static std::atomic<uint64_t> n{ 0 };
            if (n.fetch_add(1) < 4)
                KLOG("wait-any timed out after %u ms (lr=%08X) — this path used to "
                     "wait forever\n",
                     timeoutMs, uint32_t(g_ppcContext ? g_ppcContext->lr : 0));
            return STATUS_TIMEOUT;
        }
        if (WaitTraceOn() && tick && tick % 5000 == 0 && timeoutMs == WAIT_TIMEOUT_INFINITE)
        {
            uint32_t ids[8];
            const uint32_t n = std::min<uint32_t>(count, 8);
            for (uint32_t i = 0; i < n; i++)
                ids[i] = id(i);
            ReportStuckMultiWait(n, ids, int(tick / 1000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static uint32_t KeWaitForMultipleObjects_x(uint32_t count, xpointer<XDISPATCHER_HEADER>* objects,
                                           uint32_t waitType, uint32_t reason, uint32_t mode,
                                           uint32_t alertable, be<int64_t>* timeout)
{
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);

    // Validate the whole array up front. An unusable object cannot become usable
    // while we hold the caller's stack copy of it, so polling one forever would
    // wedge the calling thread for the life of the process — and this call site is
    // usually an infinite wait, which is exactly where that is unrecoverable.
    // Failing immediately hands control back to the guest, which is what lets the
    // audio callback retry on the next frame instead of hanging on the first.
    for (uint32_t i = 0; i < count; i++)
    {
        XDISPATCHER_HEADER* object = objects[i];
        if (!object)
        {
            // Rate-limited on purpose. A caller that retries every frame — which is
            // exactly what the audio pump does by design — turns an unbounded alarm
            // into thousands of identical lines a second, and an alarm that always
            // fires is one people learn to ignore (gotcha 41). Keep the alarm, bound
            // the noise: the first few, then one every 4096th.
            static std::atomic<uint64_t> seen{ 0 };
            const uint64_t n = seen.fetch_add(1);
            if (n < 4 || (n & 0xFFFu) == 0)
                KLOG("KeWaitForMultipleObjects: object %u of %u is NULL (lr=%08X, "
                     "occurrence %llu) — returning STATUS_INVALID_PARAMETER\n",
                     i, count, uint32_t(g_ppcContext ? g_ppcContext->lr : 0),
                     static_cast<unsigned long long>(n));
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (waitType == 0) // wait-all
    {
        for (uint32_t i = 0; i < count; i++)
            WaitDispatcher(objects[i], timeoutMs);
        return STATUS_SUCCESS;
    }
    // wait-any: poll. Simple and safe; revisit if it shows up hot in a profile.
    return WaitAnyPoll(
        count, timeoutMs, alertable,
        [&](uint32_t i) { return WaitDispatcher(objects[i], 0) == STATUS_SUCCESS; },
        [&](uint32_t i) { return g_memory.MapVirtual(static_cast<void*>(objects[i])); });
}

static uint32_t NtWaitForMultipleObjectsEx_x(uint32_t count, be<uint32_t>* handles,
                                             uint32_t waitType, uint32_t mode, uint32_t alertable,
                                             be<int64_t>* timeout)
{
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    if (waitType == 0)
    {
        for (uint32_t i = 0; i < count; i++)
            if (IsKernelObject(handles[i]) && IsLiveKernelHandle(handles[i]))
                GetKernelObject(handles[i])->Wait(timeoutMs);
        return STATUS_SUCCESS;
    }
    return WaitAnyPoll(
        count, timeoutMs, alertable,
        [&](uint32_t i) {
            return IsKernelObject(handles[i]) && IsLiveKernelHandle(handles[i]) &&
                   GetKernelObject(handles[i])->Wait(0) == STATUS_SUCCESS;
        },
        [&](uint32_t i) { return uint32_t(handles[i]); });
}

static uint32_t NtClose_x(uint32_t handle)
{
    if (handle == GUEST_INVALID_HANDLE_VALUE)
        return STATUS_INVALID_HANDLE;
    if (IsKernelObject(handle))
    {
        DestroyKernelObject(handle);
        return STATUS_SUCCESS;
    }
    KLOG("NtClose on unrecognized handle 0x%X\n", handle);
    return STATUS_INVALID_HANDLE;
}

// In our scheme a handle IS the object's guest address, so "referencing" it is the
// identity. A1 confirms the guest treats the result as a pointer, not a handle:
//   ObReferenceObjectByHandle(F8000018, 30000000, out)
//   KeSetAffinityThread(30058018, 00000001, ...)
//   ObDereferenceObject(...)
// — the thread handles the guest goes on to use (0x30058018 etc.) are the object
// addresses Xenia handed back, not the 0xF80000xx handles it was given.
// GetCurrentThread() does not return a handle — it returns this constant, meaning
// "whoever is asking". Nothing in a handle-validity check can spot it: our handles ARE
// guest addresses with bit 31 set, and 0xFFFFFFFE has bit 31 set, so it sails through
// IsKernelObject() and is then rejected as a dead handle. See finding 35.
constexpr uint32_t kCurrentThreadPseudoHandle = 0xFFFFFFFEu;

// Turn the pseudo-handle into this thread's real object; leave anything else alone.
static uint32_t ResolveThreadPseudoHandle(uint32_t handle)
{
    if (handle != kCurrentThreadPseudoHandle)
        return handle;
    GuestThreadSelf* self = GuestThread::Self();
    return self ? GetKernelHandle(self) : handle;
}

static uint32_t ObReferenceObjectByHandle_x(uint32_t handle, uint32_t objectType,
                                            be<uint32_t>* object)
{
    if (object)
        *object = ResolveThreadPseudoHandle(handle);
    return STATUS_SUCCESS;
}

static uint32_t NtDuplicateObject_x(uint32_t handle, be<uint32_t>* newHandle, uint32_t options)
{
    if (!newHandle)
        return STATUS_INVALID_PARAMETER;
    handle = ResolveThreadPseudoHandle(handle);
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
    {
        *newHandle = 0;
        return STATUS_INVALID_HANDLE;
    }
    GetKernelObject(handle)->refCount.fetch_add(1);
    *newHandle = handle; // same guest address; lifetime shared via the refcount
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateEvent, NtCreateEvent_x)
GUEST_FUNCTION_HOOK(__imp__NtSetEvent, NtSetEvent_x)
GUEST_FUNCTION_HOOK(__imp__NtClearEvent, NtClearEvent_x)
GUEST_FUNCTION_HOOK(__imp__KeSetEvent, KeSetEvent_x)
GUEST_FUNCTION_HOOK(__imp__KeResetEvent, KeResetEvent_x)
GUEST_FUNCTION_HOOK(__imp__NtCreateSemaphore, NtCreateSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__NtReleaseSemaphore, NtReleaseSemaphore_x)
GUEST_FUNCTION_HOOK(__imp__KeWaitForSingleObject, KeWaitForSingleObject_x)
GUEST_FUNCTION_HOOK(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx_x)
GUEST_FUNCTION_HOOK(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects_x)
GUEST_FUNCTION_HOOK(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx_x)
GUEST_FUNCTION_HOOK(__imp__NtClose, NtClose_x)
GUEST_FUNCTION_HOOK(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle_x)
GUEST_FUNCTION_HOOK(__imp__NtDuplicateObject, NtDuplicateObject_x)
// Reference counting on the object pointer itself. Our objects are quarantined on
// destroy rather than freed (kobject.cpp), so these are genuinely no-ops here — not
// unimplemented. A1 calls ObDereferenceObject 42 times, exactly matching its 42
// ObReferenceObjectByHandle calls.
GUEST_FUNCTION_STUB(__imp__ObReferenceObject)
GUEST_FUNCTION_STUB(__imp__ObDereferenceObject)
// A kernel-internal APC trampoline the guest only ever passes as a function pointer;
// when it is invoked it must do nothing and return.
GUEST_FUNCTION_STUB(__imp__KiApcNormalRoutineNop)

// ---------------------------------------------------------------------------
// Threads & TLS
// ---------------------------------------------------------------------------

// A1's first thread creation:
//   ExCreateThread(7018FA40, stack=00008000, tidOut=82AC4028,
//                  xApiStartup=82829BB0, start=82769D58, ctx=E41801B0, flags=0)
// and every later one reuses the same XAPI startup wrapper 0x82829BB0. Note the
// 0x8000 (32 KB) stack: worker threads here get an eighth of the XEX's default
// 0x40000, so honouring the argument rather than always using the default matters
// for how much address space the 15+ boot threads consume.
static uint32_t ExCreateThread_x(be<uint32_t>* handle, uint32_t stackSize,
                                 be<uint32_t>* threadId, uint32_t xApiThreadStartup,
                                 uint32_t startAddress, uint32_t startContext,
                                 uint32_t creationFlags)
{
    KLOG("ExCreateThread entry=0x%X ctx=0x%X flags=0x%X stack=0x%X\n", startAddress, startContext,
         creationFlags, stackSize);

    GuestThreadParams params{};
    if (xApiThreadStartup)
    {
        // XAPI wrapper: wrapper(startAddress, startContext).
        params.function = xApiThreadStartup;
        params.arg0 = startAddress;
        params.arg1 = startContext;
    }
    else
    {
        params.function = startAddress;
        params.arg0 = startContext;
    }
    params.flags = creationFlags;
    params.stackSize = stackSize;

    uint32_t hostId = 0;
    GuestThreadHandle* h = GuestThread::Start(params, &hostId);
    if (!h)
    {
        if (handle)
            *handle = 0;
        if (threadId)
            *threadId = 0;
        return STATUS_NO_MEMORY;
    }
    KLOG("ExCreateThread -> tid=0x%X entry=0x%X\n", hostId, startAddress);
    RegisterThreadEntry(hostId, startAddress);
    if (handle)
        *handle = GetKernelHandle(h);
    if (threadId)
        *threadId = hostId;
    return STATUS_SUCCESS;
}

static void ExTerminateThread_x(uint32_t code)
{
    throw GuestThreadExit{ code };
}

static uint32_t NtResumeThread_x(GuestThreadHandle* thread, be<uint32_t>* suspendCount)
{
    if (!thread)
    {
        if (suspendCount)
            *suspendCount = 0;
        return STATUS_INVALID_HANDLE;
    }
    if (suspendCount)
        *suspendCount = thread->suspended ? 1 : 0;
    thread->suspended = false;
    thread->suspended.notify_all();
    return STATUS_SUCCESS;
}

// Affinity is advisory here: we let the host scheduler place threads and only report
// a plausible previous mask. Case Zero does set it deliberately and it does spread
// its workers — A1 shows masks 1, 2 and 4 on consecutive JobThreads — so if a future
// timing investigation needs real pinning, this is where it goes.
static uint32_t KeSetAffinityThread_x(uint32_t thread, uint32_t affinity,
                                      be<uint32_t>* previous)
{
    if (previous)
        *previous = 1;
    return affinity;
}

static void KeSetBasePriorityThread_x(uint32_t thread, uint32_t priority) {}

static uint32_t KeQueryBasePriorityThread_x(uint32_t thread)
{
    return 0; // THREAD_PRIORITY_NORMAL
}

static uint32_t KeGetCurrentProcessType_x()
{
    return 1; // PROC_TITLE
}

// Per-thread IO-completion APCs (NtReadFile/NtWriteFile with an APC routine). NT
// runs them on the issuing thread at its next alertable wait; IO worker threads sit
// in alertable sleeps for exactly this. Phase 2's VFS is the first real producer —
// the machinery lives here because the alertable wait sites do.
struct PendingApc
{
    uint32_t routine;
    uint32_t context;
    uint32_t ioStatusBlock;
};
static thread_local std::vector<PendingApc> t_apcQueue;

void QueueThreadApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock)
{
    t_apcQueue.push_back({ routine, context, ioStatusBlock });
}

// NT waitable timers (NtCreateTimer/NtSetTimerEx/NtCancelTimer). A timer's APC fires
// on the thread that ARMED it, at that thread's next alertable wait past the due
// time — so armed timers sit on a thread-local list scanned from the same alertable
// wait sites that drain IO APCs. Fable 2's profile-settings reader wedged its whole
// boot behind a timer APC that never came, because the stub silently did nothing.
//
// A1 arms exactly one: NtCreateTimer(type=1) then
// NtSetTimerEx(F8000168, due=-0x18 6A0 (~0.16 s), apc=0, ..., period=0x0A) — a 10 ms
// periodic timer with no APC routine, i.e. one the guest waits on rather than one
// that calls back.
struct Timer final : KernelObject
{
    std::mutex m;
    bool manualReset; // type 0 = notification, 1 = synchronization
    bool armed = false;
    std::chrono::steady_clock::time_point due{};
    uint32_t periodMs = 0;
    uint32_t apcRoutine = 0;
    uint32_t apcContext = 0;

    explicit Timer(uint32_t type) : manualReset(type == 0) {}

    uint32_t Wait(uint32_t timeoutMs) override
    {
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            {
                std::lock_guard lock(m);
                if (armed && std::chrono::steady_clock::now() >= due)
                {
                    if (periodMs)
                        due += std::chrono::milliseconds(periodMs);
                    else if (!manualReset)
                        armed = false;
                    return STATUS_SUCCESS;
                }
            }
            if (timeoutMs != WAIT_TIMEOUT_INFINITE &&
                std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
                return STATUS_TIMEOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

static thread_local std::vector<Timer*> t_armedTimers;

static void KeQuerySystemTime_x(be<uint64_t>* time);

// Fire due timer APCs for the current thread. TIMER_APC_ROUTINE's signature is
// (context, timeLow, timeHigh). Returns true if any fired.
static bool FireDueTimerApcs()
{
    if (t_armedTimers.empty() || !g_ppcContext)
        return false;
    bool fired = false;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = t_armedTimers.begin(); it != t_armedTimers.end();)
    {
        Timer* timer = *it;
        uint32_t routine = 0, context = 0;
        bool keep = true;
        {
            std::lock_guard lock(timer->m);
            if (!timer->armed || now < timer->due)
            {
                keep = timer->armed;
                if (keep)
                    ++it;
                else
                    it = t_armedTimers.erase(it); // cancelled while queued
                continue;
            }
            routine = timer->apcRoutine;
            context = timer->apcContext;
            if (timer->periodMs)
                timer->due = now + std::chrono::milliseconds(timer->periodMs);
            else
            {
                timer->armed = false;
                keep = false;
            }
        }
        PPCFunc* func = g_memory.FindFunction(routine);
        if (func)
        {
            be<uint64_t> sysTime;
            KeQuerySystemTime_x(&sysTime);
            PPCContext& ctx = *g_ppcContext;
            ctx.r3.u64 = context;
            ctx.r4.u64 = static_cast<uint32_t>(sysTime.get());
            ctx.r5.u64 = static_cast<uint32_t>(sysTime.get() >> 32);
            func(ctx, g_memory.base);
            fired = true;
        }
        it = keep ? std::next(it) : t_armedTimers.erase(it);
    }
    return fired;
}

static bool DrainThreadApcs()
{
    if (t_apcQueue.empty() || !g_ppcContext)
        return false;
    auto queue = std::move(t_apcQueue);
    t_apcQueue.clear();
    for (const PendingApc& apc : queue)
    {
        // The low bit of the routine is a kernel flag (Xenia masks it too), not part
        // of the address.
        const uint32_t routine = apc.routine & ~1u;
        PPCFunc* func = g_memory.FindFunction(routine);
        if (!func)
        {
            KLOG("APC routine 0x%X is not a recompiled function — dropped\n", routine);
            continue;
        }
        // Volatile registers are scratch across calls in the PPC ABI, so reusing the
        // current context for a nested guest call is safe.
        PPCContext& ctx = *g_ppcContext;
        ctx.r3.u64 = apc.context;
        ctx.r4.u64 = apc.ioStatusBlock;
        ctx.r5.u64 = 0;
        func(ctx, g_memory.base);
    }
    return true;
}

// CZ_STALL_TRACE=<n>: every n-th sleep on a thread, print that thread's identity and
// a scan of its guest stack for image return addresses.
//
// A boot that has stopped making progress spins here, and this is the instrument
// that says WHICH GUEST FUNCTION is waiting. No host backtrace can: recompiled
// frames are inlined and tail-called under -O2, and Asura's Wrath measured its
// addr2line output naming a function that was never called. The frame back-chain is
// also unreliable on these paths (leaf frames), so this scans rather than walks.
// Off by default and free when off: one thread-local increment and a compare.
static uint32_t StallTraceInterval()
{
    static const uint32_t interval = [] {
        const char* v = getenv("CZ_STALL_TRACE");
        return v ? uint32_t(strtoul(v, nullptr, 0)) : 0u;
    }();
    return interval;
}

static uint32_t KeDelayExecutionThread_x(uint32_t mode, uint32_t alertable,
                                         be<int64_t>* interval)
{
    if (const uint32_t every = StallTraceInterval())
    {
        static thread_local uint32_t sleeps = 0;
        if (++sleeps % every == 0)
            CzDumpGuestBacktrace("KeDelayExecutionThread");
    }
    if (alertable && (DrainThreadApcs() | (int)FireDueTimerApcs()))
        return STATUS_USER_APC;

    const uint32_t ms = GuestTimeoutToMs(interval);
    if (ms == 0)
        std::this_thread::yield();
    else if (ms != WAIT_TIMEOUT_INFINITE)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return STATUS_SUCCESS;
}

// XAPI TLS (KeTls*): host-side per-thread slots, independent of the guest .tls
// section. Case Zero's XEX declares 64 TLS slots (A1's XEX_HEADER_TLS_INFO) and the
// guest-side TLS area in the thread block (cpu/guest_thread.cpp) is sized to match,
// but these APIs never touch it.
//
// A1: KeTlsAlloc() is the fifth kernel call the title ever makes, immediately
// followed by KeTlsSetValue(00000000, 400006A0) — index 0, a pointer into the
// large-page arena the title had just allocated.
static std::mutex g_tlsLock;
static uint32_t g_tlsNextIndex = 0;
static std::vector<uint32_t> g_tlsFreeIndices;

static uint32_t& TlsSlot(uint32_t index)
{
    thread_local std::vector<uint32_t> slots;
    if (slots.size() <= index)
        slots.resize(index + 1, 0);
    return slots[index];
}

static uint32_t KeTlsAlloc_x()
{
    std::lock_guard lock(g_tlsLock);
    if (!g_tlsFreeIndices.empty())
    {
        const uint32_t idx = g_tlsFreeIndices.back();
        g_tlsFreeIndices.pop_back();
        return idx;
    }
    return g_tlsNextIndex++;
}

static uint32_t KeTlsFree_x(uint32_t index)
{
    std::lock_guard lock(g_tlsLock);
    g_tlsFreeIndices.push_back(index);
    return 1;
}

static uint32_t KeTlsGetValue_x(uint32_t index) { return TlsSlot(index); }

static uint32_t KeTlsSetValue_x(uint32_t index, uint32_t value)
{
    TlsSlot(index) = value;
    return 1;
}

GUEST_FUNCTION_HOOK(__imp__ExCreateThread, ExCreateThread_x)
GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread_x)
GUEST_FUNCTION_HOOK(__imp__NtResumeThread, NtResumeThread_x)
GUEST_FUNCTION_HOOK(__imp__KeSetAffinityThread, KeSetAffinityThread_x)
GUEST_FUNCTION_HOOK(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread_x)
GUEST_FUNCTION_HOOK(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread_x)
GUEST_FUNCTION_HOOK(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType_x)
GUEST_FUNCTION_HOOK(__imp__KeDelayExecutionThread, KeDelayExecutionThread_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsAlloc, KeTlsAlloc_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsFree, KeTlsFree_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsGetValue, KeTlsGetValue_x)
GUEST_FUNCTION_HOOK(__imp__KeTlsSetValue, KeTlsSetValue_x)

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

uint64_t KernelSystemTime()
{
    // FILETIME: 100 ns ticks since 1601-01-01.
    constexpr int64_t kEpochDifference = 116444736000000000LL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t ticks =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(now)
            .count();
    return static_cast<uint64_t>(ticks + kEpochDifference);
}

// Monotonic since process start, in the same 100 ns units. Deliberately NOT derived
// from the system clock: the guest uses interrupt time for elapsed-time arithmetic,
// and a wall-clock jump (NTP, a suspend) would show up there as a frame that took an
// hour.
uint64_t KernelInterruptTime()
{
    // Under CZ_DETERMINISTIC_CLOCK this must follow the SAME virtual clock as `mftb`,
    // or the guest reads wall time through the back door and the animation is
    // non-reproducible again — while every other symptom says the mode is working.
    // Two clocks that are supposed to agree have to be derived from one source.
    if (cz_timebase::deterministic)
        return uint64_t((__uint128_t(cz_timebase::virtual_ticks) * 10'000'000ull) /
                        CZ_TIMEBASE_HZ);
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            std::chrono::steady_clock::now() - start)
            .count());
}

static void KeQuerySystemTime_x(be<uint64_t>* time)
{
    if (time)
        *time = KernelSystemTime();
}

static uint64_t KeQueryPerformanceFrequency_x()
{
    // The same 49.875 MHz the guest's own `mftb` is made to tick at
    // (cpu/timebase.h — gotcha 1). These two numbers MUST agree: every guest "how
    // long has it been" computation divides one by the other.
    return CZ_TIMEBASE_HZ;
}

// Days-from-civil (Howard Hinnant's algorithm) for the FILETIME <-> fields
// conversions. Written out rather than using the C library because the epoch is 1601
// and the fields are big-endian.
static void TimeToFields(int64_t fileTime, be<uint16_t>* f)
{
    const int64_t totalSeconds = fileTime / 10000000;
    const int32_t ms = static_cast<int32_t>((fileTime % 10000000) / 10000);
    const int64_t days = totalSeconds / 86400;
    const int64_t secs = totalSeconds % 86400;

    // civil from days since 1601-01-01; shift to the 0000-03-01 era
    const int64_t z = days + 584389; // days from 0000-03-01 to 1601-01-01
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;

    f[0] = static_cast<uint16_t>(y);           // Year
    f[1] = static_cast<uint16_t>(m);           // Month
    f[2] = static_cast<uint16_t>(d);           // Day
    f[3] = static_cast<uint16_t>(secs / 3600); // Hour
    f[4] = static_cast<uint16_t>((secs % 3600) / 60);
    f[5] = static_cast<uint16_t>(secs % 60);
    f[6] = static_cast<uint16_t>(ms);
    f[7] = static_cast<uint16_t>((days + 1) % 7); // 1601-01-01 was a Monday
}

static void RtlTimeToTimeFields_x(be<int64_t>* time, be<uint16_t>* fields)
{
    if (time && fields)
        TimeToFields(*time, fields);
}

static uint32_t RtlTimeFieldsToTime_x(be<uint16_t>* f, be<int64_t>* time)
{
    if (!f || !time)
        return 0;
    int64_t y = f[0];
    const int64_t m = f[1], d = f[2];
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint64_t yoe = static_cast<uint64_t>(y - era * 400);
    const uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 584389;
    const int64_t secs = days * 86400 + f[3] * 3600 + f[4] * 60 + f[5];
    *time = secs * 10000000 + static_cast<int64_t>(f[6]) * 10000;
    return 1;
}

static uint32_t NtCreateTimer_x(be<uint32_t>* handleOut, uint32_t objAttributes,
                                uint32_t timerType)
{
    (void)objAttributes;
    if (!handleOut)
        return STATUS_INVALID_PARAMETER;
    Timer* timer = CreateKernelObject<Timer>(timerType);
    if (!timer)
    {
        *handleOut = 0;
        return STATUS_NO_MEMORY;
    }
    *handleOut = GetKernelHandle(timer);
    return STATUS_SUCCESS;
}

// (handle, dueTime, apcRoutine, apcMode, apcContext, resume, periodMs, prevState) —
// dueTime uses the usual NT convention: negative = relative, in 100 ns units.
static uint32_t NtSetTimerEx_x(uint32_t handle, be<int64_t>* dueTime, uint32_t apcRoutine,
                               uint32_t apcMode, uint32_t apcContext, uint32_t resume,
                               uint32_t periodMs, uint32_t prevState)
{
    (void)apcMode;
    (void)resume;
    (void)prevState;
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return STATUS_INVALID_HANDLE;
    Timer* timer = GetKernelObject<Timer>(handle);

    const int64_t raw = dueTime ? dueTime->get() : 0;
    auto due = std::chrono::steady_clock::now();
    if (raw < 0)
        due += std::chrono::microseconds(-raw / 10);
    // Absolute due times (raw > 0) fire immediately: we do not track a guest wall
    // clock precisely enough to honour them, and "already due" is the safe reading.

    {
        std::lock_guard lock(timer->m);
        timer->armed = true;
        timer->due = due;
        timer->periodMs = periodMs;
        timer->apcRoutine = apcRoutine;
        timer->apcContext = apcContext;
    }
    // The APC belongs to the arming thread: park the timer on this thread's list.
    if (apcRoutine &&
        std::find(t_armedTimers.begin(), t_armedTimers.end(), timer) == t_armedTimers.end())
        t_armedTimers.push_back(timer);
    return STATUS_SUCCESS;
}

static uint32_t NtCancelTimer_x(uint32_t handle, be<uint32_t>* currentState)
{
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
    {
        if (currentState)
            *currentState = 0;
        return STATUS_INVALID_HANDLE;
    }
    Timer* timer = GetKernelObject<Timer>(handle);
    std::lock_guard lock(timer->m);
    if (currentState)
        *currentState = timer->armed ? 1 : 0;
    timer->armed = false; // FireDueTimerApcs drops disarmed timers from its list
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__KeQuerySystemTime, KeQuerySystemTime_x)
GUEST_FUNCTION_HOOK(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency_x)
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields_x)
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime_x)
GUEST_FUNCTION_HOOK(__imp__NtCreateTimer, NtCreateTimer_x)
GUEST_FUNCTION_HOOK(__imp__NtSetTimerEx, NtSetTimerEx_x)
GUEST_FUNCTION_HOOK(__imp__NtCancelTimer, NtCancelTimer_x)

// ---------------------------------------------------------------------------
// Rtl string / memory helpers
// ---------------------------------------------------------------------------

// A1: RtlInitAnsiString(7018F968, 820B51F4("Main Thread Blocked Event Handle")) —
// 329 calls, almost all of them naming a kernel object about to be created.
static void RtlInitAnsiString_x(XANSI_STRING* dst, char* src)
{
    if (!dst)
        return;
    const uint16_t len = src ? static_cast<uint16_t>(strlen(src)) : 0;
    dst->Length = len;
    dst->MaximumLength = src ? len + 1 : 0;
    dst->Buffer = src;
}

static uint32_t RtlMultiByteToUnicodeN_x(be<uint16_t>* dst, uint32_t maxBytes,
                                         be<uint32_t>* bytesOut, const char* src,
                                         uint32_t srcBytes)
{
    const uint32_t n = std::min(maxBytes / 2, srcBytes);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = static_cast<uint8_t>(src[i]);
    if (bytesOut)
        *bytesOut = n * 2;
    return STATUS_SUCCESS;
}

static uint32_t RtlUnicodeToMultiByteN_x(char* dst, uint32_t maxBytes, be<uint32_t>* bytesOut,
                                         const be<uint16_t>* src, uint32_t srcBytes)
{
    const uint32_t n = std::min(maxBytes, srcBytes / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t c = src[i];
        dst[i] = c < 256 ? static_cast<char>(c) : '?';
    }
    if (bytesOut)
        *bytesOut = n;
    return STATUS_SUCCESS;
}

static uint32_t RtlUpcaseUnicodeChar_x(uint32_t c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static uint32_t RtlCompareMemoryUlong_x(be<uint32_t>* src, uint32_t length, uint32_t pattern)
{
    uint32_t matched = 0;
    for (uint32_t i = 0; i < length / 4; i++)
    {
        if (src[i] != pattern)
            break;
        matched += 4;
    }
    return matched;
}

static void RtlFillMemoryUlong_x(be<uint32_t>* dst, uint32_t length, uint32_t pattern)
{
    for (uint32_t i = 0; i < length / 4; i++)
        dst[i] = pattern;
}

// The mapping matters more than it looks: a title that branches on the DOS error
// rather than the NTSTATUS takes a silently different path when a status falls
// through to the default. Phase 2's VFS is what will exercise the file-related ones;
// they are listed now because getting them wrong later is invisible.
static uint32_t RtlNtStatusToDosError_x(uint32_t status)
{
    switch (status)
    {
        case STATUS_SUCCESS: return 0;
        case STATUS_TIMEOUT: return 1460;            // ERROR_TIMEOUT
        case STATUS_NO_MEMORY: return 14;            // ERROR_OUTOFMEMORY
        case STATUS_OBJECT_NAME_NOT_FOUND: return 2; // ERROR_FILE_NOT_FOUND
        case STATUS_INVALID_HANDLE: return 6;        // ERROR_INVALID_HANDLE
        case 0xC000000F: return 2;                   // STATUS_NO_SUCH_FILE
        case 0x80000006: return 12;                  // STATUS_NO_MORE_FILES
        case 0xC000000E: return 3;                   // ERROR_PATH_NOT_FOUND
        case 0xC0000011: return 38;                  // ERROR_HANDLE_EOF
        case 0xC0000023: return 122;                 // ERROR_INSUFFICIENT_BUFFER
        default: return 317;                         // ERROR_MR_MID_NOT_FOUND
    }
}

GUEST_FUNCTION_HOOK(__imp__RtlInitAnsiString, RtlInitAnsiString_x)
GUEST_FUNCTION_HOOK(__imp__RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN_x)
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN_x)
GUEST_FUNCTION_HOOK(__imp__RtlUpcaseUnicodeChar, RtlUpcaseUnicodeChar_x)
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong_x)
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong_x)
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError_x)

// ---------------------------------------------------------------------------
// Mini printf: DbgPrint / sprintf / _snprintf
// ---------------------------------------------------------------------------

// Formats a guest printf call into `out`. Varargs are GPRs starting at some position
// then the caller's stack (guestcall::GetGpr handles the spill). Supports
// %s %d %i %u %x %X %p %c %% with basic width/zero-pad; unknown specifiers are
// copied through raw rather than guessed at, so a wrong format never desynchronises
// the argument cursor for the rest of the string.
//
// Cheap, and it is what makes a guest assert readable instead of a bare address.
static size_t GuestFormat(char* out, size_t cap, const char* fmt, PPCContext& ctx,
                          uint8_t* base, size_t firstArg)
{
    size_t o = 0, argIdx = firstArg;
    auto put = [&](char c) {
        if (o + 1 < cap)
            out[o] = c;
        o++;
    };
    auto nextArg = [&]() -> uint64_t { return guestcall::GetGpr(ctx, base, argIdx++); };

    for (const char* p = fmt; *p; p++)
    {
        if (*p != '%')
        {
            put(*p);
            continue;
        }
        const char* spec = p++;
        if (*p == '%')
        {
            put('%');
            continue;
        }
        char pad = ' ';
        int width = 0;
        if (*p == '0')
        {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        while (*p == 'l' || *p == 'h')
            p++; // length modifiers: all guest args arrive as 64-bit GPRs anyway

        char buf[32];
        const char* s = buf;
        int len = 0;
        switch (*p)
        {
            case 's':
            {
                const uint32_t g = static_cast<uint32_t>(nextArg());
                s = g ? reinterpret_cast<const char*>(base + g) : "(null)";
                len = static_cast<int>(strlen(s));
                break;
            }
            case 'c':
                buf[0] = static_cast<char>(nextArg());
                len = 1;
                break;
            case 'd':
            case 'i':
                len = snprintf(buf, sizeof buf, "%d", static_cast<int32_t>(nextArg()));
                break;
            case 'u':
                len = snprintf(buf, sizeof buf, "%u", static_cast<uint32_t>(nextArg()));
                break;
            case 'x':
                len = snprintf(buf, sizeof buf, "%x", static_cast<uint32_t>(nextArg()));
                break;
            case 'X':
                len = snprintf(buf, sizeof buf, "%X", static_cast<uint32_t>(nextArg()));
                break;
            case 'p':
                len = snprintf(buf, sizeof buf, "%08X", static_cast<uint32_t>(nextArg()));
                break;
            default:
                // Unknown (%f etc.): copy the raw specifier through, consume nothing.
                for (const char* q = spec; q <= p; q++)
                    put(*q);
                continue;
        }
        for (int i = len; i < width; i++)
            put(pad);
        for (int i = 0; i < len; i++)
            put(s[i]);
    }
    if (cap)
        out[std::min(o, cap - 1)] = 0;
    return o;
}

PPC_FUNC(__imp__DbgPrint)
{
    KCALL("DbgPrint");
    char buf[1024];
    const char* fmt = reinterpret_cast<const char*>(base + ctx.r3.u32);
    GuestFormat(buf, sizeof buf, fmt, ctx, base, 1);
    fprintf(stderr, "[DbgPrint] %s", buf);
    ctx.r3.u64 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__sprintf)
{
    KCALL("sprintf");
    char* dst = reinterpret_cast<char*>(base + ctx.r3.u32);
    const char* fmt = reinterpret_cast<const char*>(base + ctx.r4.u32);
    const size_t n = GuestFormat(dst, 0x10000, fmt, ctx, base, 2);
    ctx.r3.u64 = n;
}

PPC_FUNC(__imp___snprintf)
{
    KCALL("_snprintf");
    char* dst = reinterpret_cast<char*>(base + ctx.r3.u32);
    const uint32_t cap = ctx.r4.u32;
    const char* fmt = reinterpret_cast<const char*>(base + ctx.r5.u32);
    const size_t n = GuestFormat(dst, cap, fmt, ctx, base, 3);
    ctx.r3.u64 = n < cap ? n : static_cast<uint64_t>(-1);
}

// The va_list form would need the guest's own va_list layout walked; -1 is the
// honest "nothing was written" answer until it shows up on a live path.
STUB_RET(_vsnprintf, uint64_t(-1))

// ---------------------------------------------------------------------------
// Diagnostics / termination
// ---------------------------------------------------------------------------

PPC_FUNC(__imp__KeBugCheck)
{
    KCALL("KeBugCheck");
    fprintf(stderr,
            "[kernel] KeBugCheck(0x%X) lr=%08X r4=%08X r5=%08X — guest fatal, aborting\n",
            ctx.r3.u32, uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32);
    // Walk the guest stack back-chain ([sp] = caller frame, [callerframe+4] = saved
    // LR) to reveal who initiated the CRT exit/terminate that reached KeBugCheck.
    fprintf(stderr, "[kernel] guest backtrace: %08X", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 24 && sp >= 0x1000 && sp < 0x80000000u; i++)
    {
        const uint32_t caller = PPC_LOAD_U32(sp);
        if (caller <= sp || caller >= 0x80000000u)
            break;
        const uint32_t lr = PPC_LOAD_U32(caller + 4);
        if (lr >= uint32_t(PPC_IMAGE_BASE) && lr < uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE))
            fprintf(stderr, " <- %08X", lr);
        sp = caller;
    }
    fprintf(stderr, "\n");
    abort();
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    KCALL("KeBugCheckEx");
    fprintf(stderr, "[kernel] KeBugCheckEx(0x%X, 0x%X, 0x%X, 0x%X) — aborting\n", ctx.r3.u32,
            ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    abort();
}

PPC_FUNC(__imp__HalReturnToFirmware)
{
    KCALL("HalReturnToFirmware");
    fprintf(stderr, "[kernel] HalReturnToFirmware(%u) — title requested exit\n", ctx.r3.u32);
    exit(0);
}

// A1 raises 19 exceptions in the boot, and Xenia decodes every one of them as
// SetThreadName — the log lines pair up exactly:
//   RtlRaiseException(...) / SetThreadName(6, Main Thread)
//                           SetThreadName(7, cAsyncFileSystem)
//                           SetThreadName(8, JobThread0) ... JobThread5
//                           SetThreadName(E, BigFile Decompress Thread)
//                           SetThreadName(F, Controller Hardware Update)
// so on this title RtlRaiseException is, in the boot era, entirely a thread-naming
// channel. That is a statement about this drive, not about the export: any other
// exception code still reaches the abort below, which is where a real SEH
// requirement would announce itself.
PPC_FUNC(__imp__RtlRaiseException)
{
    KCALL("RtlRaiseException");
    // EXCEPTION_RECORD: {Code, Flags, Record, Address, NumberParameters, Information[]}.
    const uint8_t* record = base + ctx.r3.u32;
    const uint32_t code = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(record));

    if (code == 0x406D1388) // MS_VC_EXCEPTION: "SetThreadName", debugger-only, continuable
    {
        const uint32_t namePtr =
            __builtin_bswap32(*reinterpret_cast<const uint32_t*>(record + 0x18));
        const char* name = namePtr ? reinterpret_cast<const char*>(base + namePtr) : "?";
        KLOG("thread named '%s' (r13=%08X)\n", name, g_ppcContext ? g_ppcContext->r13.u32 : 0);
        return;
    }

    fprintf(stderr,
            "[kernel] RtlRaiseException(code=0x%X record=0x%X) — no SEH support, aborting\n",
            code, ctx.r3.u32);
    abort();
}

// L2 cache locking: a performance hint with no observable semantics for us.
GUEST_FUNCTION_STUB(__imp__KeLockL2)
GUEST_FUNCTION_STUB(__imp__KeUnlockL2)

// ---------------------------------------------------------------------------
// System info / config / loader queries
// ---------------------------------------------------------------------------

// A1 asks for exactly two settings, both in the user category:
//   ExGetXConfigSetting(0003, 000A, 40005F9C, 0004, 7018F980)  <- video flags
//   ExGetXConfigSetting(0003, 0009, 7018FA84, 0004, 7018FA80)  <- language
// The others are answered because the import exists and a title that asks for one
// mid-session should not get an unimplemented-import return where it expects a
// dword.
static uint32_t ExGetXConfigSetting_x(uint16_t category, uint16_t setting, void* buffer,
                                      uint16_t bufferSize, be<uint32_t>* requiredSize)
{
    uint32_t value = 0;
    switch (category)
    {
        case 0x0002: // XCONFIG_SECURED_CATEGORY
            switch (setting)
            {
                case 0x0002: value = 0x00001000; break; // AV region: USA/Canada
                default: return STATUS_UNSUCCESSFUL;
            }
            break;
        case 0x0003: // XCONFIG_USER_CATEGORY
            switch (setting)
            {
                case 0x0009: value = 1; break;          // language: English
                case 0x000A: value = 0x00040000; break; // video flags: widescreen
                case 0x000C: value = 1; break;          // retail flags
                case 0x000E: value = 103; break;        // country: US
                default: value = 0; break;              // timezone etc: zero is fine
            }
            break;
        default:
            return STATUS_UNSUCCESSFUL;
    }
    if (requiredSize)
        *requiredSize = 4;
    if (buffer)
    {
        be<uint32_t> data(value);
        memcpy(buffer, &data, std::min<size_t>(bufferSize, 4));
    }
    return STATUS_SUCCESS;
}

// A1: XexCheckExecutablePrivilege(0000000A) — privilege 10, asked once. Xenia grants
// nothing and the title proceeds, so 0 (not held) is the measured answer, not a
// guess.
// XexCheckExecutablePrivilege is defined below, after RtlImageXexHeaderField_x,
// because it answers out of the XEX's own optional headers rather than a constant.

static uint32_t XGetLanguage_x() { return 1; }        // English
static uint32_t XGetAVPack_x() { return 0; }
static uint32_t XGetGameRegion_x() { return 0x03FF; } // region-free

// A1: XexGetModuleHandle(00000000, out) then XexGetModuleHandle(8209123C("xam.xex"),
// out). A null name means "this module", which is the XEX header block main.cpp
// published; the xam form is answered with the same handle XexLoadImage minted.
static uint32_t XexGetModuleHandle_x(char* name, be<uint32_t>* handle)
{
    if (!handle)
        return STATUS_INVALID_PARAMETER;
    if (name && strstr(name, "xam"))
    {
        *handle = 0x30002000; // see XexLoadImage_x
        return STATUS_SUCCESS;
    }
    *handle = g_xexHeaderBase.load();
    return STATUS_SUCCESS;
}

// RtlImageXexHeaderField is the FIRST kernel call Case Zero makes, so its answer
// steers the very first branch the CRT takes — which is why it is implemented for
// real here rather than stubbed to 0.
//
// A1: RtlImageXexHeaderField(30015000, 00020401), i.e. XEX_HEADER_DEFAULT_HEAP_SIZE,
// asked exactly once. This XEX has no such optional header — its 14 are
// RESOURCE_INFO, FILE_FORMAT_INFO, ENTRY_POINT, IMAGE_BASE_ADDRESS,
// IMPORT_LIBRARIES, CHECKSUM_TIMESTAMP, ORIGINAL_PE_NAME, STATIC_LIBRARIES,
// TLS_INFO, DEFAULT_STACK_SIZE, SYSTEM_FLAGS, EXECUTION_INFO, GAME_RATINGS and one
// more — so the faithful answer is NULL and the CRT falls back to its built-in
// default heap size. Walking the real headers gets that right *and* gets every other
// field right for free, which matters because TLS_INFO and EXECUTION_INFO are both
// queried later.
//
// The header block lives in guest memory because main.cpp copies it there
// (PublishXexHeaders) and publishes its address as XexExecutableModuleHandle.
// The walk itself now lives in xex_imports.cpp, because XamGetExecutionId and the
// content enumerator need the same answer and two copies of an 8-byte walk over a
// header block is how they drift.
static uint32_t RtlImageXexHeaderField_x(uint32_t headerBase, uint32_t key)
{
    return XexHeaderField(headerBase, key);
}

// XamGetExecutionId(out) -> a POINTER to this module's XEX_HEADER_EXECUTION_INFO,
// written through the out-parameter; the return is a status the guest tests with a
// SIGNED compare (`cmpwi r3,0; blt <fail>`).
//
// Both call sites want exactly one field. sub_825D8E60:
//     XamGetExecutionId(&p);  if (r3 < 0) return 0;
//     if (p->titleId /* +12 */ == r30) return 1;
// and that is the save enumerator's TITLE FILTER — r30 is the title id of the content
// item just enumerated. So this export is not bookkeeping: leave it a stub and every
// save this runtime enumerates is silently discarded by the title, with no error
// anywhere, because the comparison is against an out-parameter the stub never wrote
// (gotcha 42's exact shape).
static uint32_t XamGetExecutionId_x(be<uint32_t>* out)
{
    if (!out)
        return STATUS_INVALID_PARAMETER;
    const uint32_t info = XexHeaderField(0, 0x00040006 /* XEX_HEADER_EXECUTION_INFO */);
    *out = info; // written on the failure path too
    if (!info)
    {
        KLOG("XamGetExecutionId: this XEX has no EXECUTION_INFO header\n");
        return STATUS_NOT_FOUND;
    }
    return STATUS_SUCCESS;
}

// XexCheckExecutablePrivilege(n) — is bit n set in this XEX's system flags?
//
// This used to `return 0` for every privilege, which is a constant standing in for a
// question the image can answer (gotcha 10). It happened to be right: Case Zero's
// XEX_HEADER_SYSTEM_FLAGS is 0x00000200 — bit 9, XEX_SYSTEM_TITLE_USES_GAME_VOICE_
// CHANNEL, and nothing else — and the three privileges this title asks about (10 in
// A1, then 11 and 23 from inside the DVD-cache init) are all clear. Being right by
// luck is not a reason to keep it: Case West's flags will differ, and the failure
// mode is a silently wrong branch, not an error.
//
// The privileges that matter here, for the record: 11 = TITLE_INSECURE_UTILITY_DRIVE,
// 23 = TITLE_BOTH_UTILITY_PARTITIONS. Both control whether sub_82829098 will try to
// build a DVD cache on the utility partition.
static uint32_t XexCheckExecutablePrivilege_x(uint32_t privilege)
{
    if (privilege > 31)
        return 0;
    constexpr uint32_t XEX_HEADER_SYSTEM_FLAGS = 0x00030000;
    const uint32_t field = RtlImageXexHeaderField_x(0, XEX_HEADER_SYSTEM_FLAGS);
    if (!field)
        return 0;   // no such header — no privileges, which is the truthful answer
    uint8_t* base = g_memory.base;
    const uint32_t flags = PPC_LOAD_U32(field);
    return (flags & (1u << privilege)) != 0 ? 1u : 0u;
}

// The loader seam. A1 shows Case Zero doing this at boot, in this order:
//   XexLoadImage(8209123C("xam.xex"), 00000009, 00000000, out) -> handle 30002000
//   XexGetProcedureAddress(30002000, 0xAFF)  XamPartyGetUserList
//                          ... 0xB00         XamPartySendGameInvites
//                          ... 0xB0B         XamPartySetCustomData
//                          ... 0xB10         XamPartyGetBandwidth
//                          ... 0x305         XamShowPartyUI
//                          ... 0x30B         XamShowCommunitySessionsUI
//                          ... 0x279
// Seven resolutions, and — unlike Asura's Wrath, where Xenia refused ordinal 0x48C —
// Xenia resolves ALL SEVEN here; there is no "ordinal not found" line anywhere in
// A1. So the faithful behaviour is to resolve every one of these and refuse anything
// else loudly.
//
// Reporting "xam.xex is not loadable" would be a lie — xam is always resident on a
// 360 — and it would also make our first-occurrence sequence diverge from A1 by
// dropping XexGetProcedureAddress entirely, which is precisely what the phase 1 gate
// measures. So the loader succeeds and each resolved ordinal gets a minted guest
// address bound to a stub that fails honestly when actually CALLED.
constexpr uint32_t kXamModuleHandle = 0x30002000; // Xenia's value in A1, for log legibility

// WHERE THE MINTED THUNKS LIVE, AND WHY THIS IS COMPUTED RATHER THAN A CONSTANT
// -----------------------------------------------------------------------------
// A minted address must be inside [PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE),
// because that is the only range PPC_LOOKUP_FUNC can index — the dispatch table
// covers the CODE range, not the image range. Asura's Wrath learned this the
// expensive way (its finding 54): it picked a constant "past the last section, still
// inside the image", which was 4.7 MB beyond the table, so InsertFunction wrote 9.4
// MB past the table's end and the stub that was supposed to fail honestly never ran
// — the caller saw r3 = 0, i.e. success, from a call that did nothing.
//
// Case Zero leaves no comfortable constant to pick: its last mapped function starts
// at 0x829C3554 and the code range ends 16 bytes later at 0x829C3564, so there is no
// tail pad at all. Rather than hand-pick an interior gap and hope it stays free
// across a function-list change, the slot region is FOUND at first use: scan the
// dispatch table for the longest run of consecutive unmapped 4-byte slots and take
// the middle of it. The scan is one pass over 2.2 M slots, once per process, and it
// cannot go stale because there is no constant to go stale.
static std::mutex g_mintedMutex;
static std::map<uint32_t, uint32_t> g_mintedOrdinals; // guest addr -> ordinal
static uint32_t g_mintBase = 0, g_mintEnd = 0;

// Longest run of unmapped guest addresses in the code range. Called with
// g_mintedMutex held.
static void FindMintRegion()
{
    if (g_mintBase)
        return;
    const uint32_t lo = uint32_t(PPC_CODE_BASE), hi = uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE);
    uint32_t bestStart = 0, bestLen = 0, runStart = 0, runLen = 0;
    for (uint32_t a = lo; a < hi; a += 4)
    {
        if (g_memory.FindFunction(a) == nullptr)
        {
            if (runLen++ == 0)
                runStart = a;
            if (runLen > bestLen)
            {
                bestLen = runLen;
                bestStart = runStart;
            }
        }
        else
        {
            runLen = 0;
        }
    }
    // Take the middle of the run: the ends of an unmapped run abut real functions,
    // and a minted address adjacent to one is more likely to collide with something
    // that computes an address relative to it.
    const uint32_t bytes = bestLen * 4;
    if (bytes < 0x40)
    {
        KLOG("no unmapped run of at least 16 slots in the code range (best %u slots at "
             "%08X) — xam export minting is disabled\n",
             bestLen, bestStart);
        return;
    }
    g_mintBase = bestStart + (bytes / 4) * 2; // quarter-way in, 4-byte aligned
    g_mintBase &= ~3u;
    g_mintEnd = bestStart + bytes;
    KLOG("xam export thunks will be minted from %08X (longest unmapped run in the code "
         "range: %u slots at %08X..%08X)\n",
         g_mintBase, bestLen, bestStart, bestStart + bytes);
}

static uint32_t XexLoadImage_x(const char* name, uint32_t flags, uint32_t minVersion,
                               be<uint32_t>* handleOut)
{
    if (name && strstr(name, "xam"))
    {
        if (handleOut)
            *handleOut = kXamModuleHandle;
        KLOG("XexLoadImage('%s') -> %08X\n", name, kXamModuleHandle);
        return STATUS_SUCCESS;
    }
    if (handleOut)
        *handleOut = 0; // finding 14: fill the out-parameter on the failure path too
    KLOG("XexLoadImage('%s') -> NOT_FOUND\n", name ? name : "(null)");
    return STATUS_NOT_FOUND;
}

// One stub body serves every minted thunk; the ordinal is recovered from the entry
// address so the log names what the guest actually called.
static void MintedExportStub(PPCContext& ctx, uint8_t* base)
{
    (void)base;
    uint32_t ordinal = 0;
    {
        std::lock_guard lk(g_mintedMutex);
        // ctr still holds the address the guest branched to.
        auto it = g_mintedOrdinals.find(ctx.ctr.u32);
        if (it != g_mintedOrdinals.end())
            ordinal = it->second;
    }
    static std::atomic<int> n{ 0 };
    if (n.fetch_add(1) < 32)
        KLOG("xam export ordinal 0x%X called (lr=%08X) — unimplemented, returning failure\n",
             ordinal, uint32_t(ctx.lr));
    ctx.r3.u64 = STATUS_NOT_IMPLEMENTED;
}

static uint32_t XexGetProcedureAddress_x(uint32_t module, uint32_t ordinal, be<uint32_t>* out)
{
    // The exact set Xenia resolves for this title in A1. Anything else is refused
    // and logged loudly, so a new one is never silent.
    static const uint32_t kResolvable[] = { 0xAFF, 0xB00, 0xB0B, 0xB10, 0x305, 0x30B, 0x279 };
    const bool known = std::find(std::begin(kResolvable), std::end(kResolvable), ordinal) !=
                       std::end(kResolvable);
    if (!known)
    {
        KLOG("XexGetProcedureAddress module=%08X ord=0x%X -> NOT_FOUND (not one of the "
             "seven A1 resolves)\n",
             module, ordinal);
        if (out)
            *out = 0;
        return STATUS_NOT_FOUND;
    }

    std::lock_guard lk(g_mintedMutex);
    FindMintRegion();
    uint32_t addr = 0;
    for (const auto& [a, o] : g_mintedOrdinals)
        if (o == ordinal)
            addr = a;
    if (!addr)
    {
        addr = g_mintBase + static_cast<uint32_t>(g_mintedOrdinals.size()) * 4;
        if (!g_mintBase || addr >= g_mintEnd)
        {
            KLOG("XexGetProcedureAddress: out of mint slots at %08X (region %08X..%08X)\n",
                 addr, g_mintBase, g_mintEnd);
            if (out)
                *out = 0;
            return STATUS_NOT_FOUND;
        }
        // Re-check both assumptions at mint time rather than trusting the scan,
        // because this is exactly the kind of thing that goes stale silently.
        if (g_memory.FindFunction(addr) != nullptr)
        {
            KLOG("XexGetProcedureAddress: mint slot %08X is ALREADY a mapped guest "
                 "function — refusing to shadow it\n",
                 addr);
            if (out)
                *out = 0;
            return STATUS_NOT_FOUND;
        }
        // An ordinal we have actually implemented binds to it; everything else gets
        // the honest-failure stub. Ordinal 0x279 is the one that matters —
        // XamContentAggregateCreateEnumerator, which this title does not import and
        // resolves here instead (A1 line 111,986 names it), so this mint is the ONLY
        // seam it has. There is no `__imp__` symbol to hook.
        PPCFunc* impl = ContentMintedExportForOrdinal(ordinal);
        // Refuses (loudly) if the address is outside the dispatch table's range,
        // which is the failure Asura's Wrath hit silently.
        if (!g_memory.InsertFunction(addr, impl ? impl : MintedExportStub))
        {
            if (out)
                *out = 0;
            return STATUS_NOT_FOUND;
        }
        g_mintedOrdinals.emplace(addr, ordinal);
    }
    if (out)
        *out = addr;
    KLOG("XexGetProcedureAddress module=%08X ord=0x%X -> %08X\n", module, ordinal, addr);
    return STATUS_SUCCESS;
}

// A1 registers three of these during boot, all with create=1:
//   ExRegisterTitleTerminateNotification(829F5CA8(8284C790, 6E800001), 00000001)
//   ExRegisterTitleTerminateNotification(829F5CB8(8284C7F0, 6E800000), 00000001)
//   ExRegisterTitleTerminateNotification(829F9478(82886C18, 7D800000), 00000001)
// The argument is a guest structure whose first word is the notification routine and
// whose second is a flags/link word; the second argument is create(1)/remove(0).
//
// Recording them is not busywork: a title asked to shut down without them being
// called leaks the state it would otherwise hand back. Phase 1 has no orderly
// shutdown path yet (the runtime is ^C'd), so nothing walks the list — but the list
// exists, is logged, and the shutdown that walks it has one place to look.
struct TitleTerminateNotification
{
    uint32_t structAddress;
    uint32_t routine;
};
static std::vector<TitleTerminateNotification> g_titleTerminateNotifications;

static void ExRegisterTitleTerminateNotification_x(uint32_t registration, uint32_t create)
{
    if (!registration)
        return;
    uint8_t* base = g_memory.base;
    const uint32_t routine = PPC_LOAD_U32(registration);
    std::lock_guard lock(g_kernelLock);
    if (create)
    {
        g_titleTerminateNotifications.push_back({ registration, routine });
        KLOG("ExRegisterTitleTerminateNotification + %08X routine=%08X (%zu registered)\n",
             registration, routine, g_titleTerminateNotifications.size());
    }
    else
    {
        std::erase_if(g_titleTerminateNotifications,
                      [&](const auto& n) { return n.structAddress == registration; });
        KLOG("ExRegisterTitleTerminateNotification - %08X (%zu remain)\n", registration,
             g_titleTerminateNotifications.size());
    }
}

// A1: XexGetModuleSection(30014000, 820B0FB4("Serial"), 7018FA94, 7018FA90) — the
// title asking a module for a named section. The handle 0x30014000 is NOT the xam
// handle our XexLoadImage minted (0x30002000); on hardware it is the title's own
// module, and the names are XEX *resources*, which A1's own header dump lists:
//
//     Serial2  82AF0000-82AF0020, 32b
//     Serial   82AF0080-82AF00BF, 63b
//     Digest   82AF0100-82AF011C, 28b
//     58410A8D 82B00000-82B3072B, 198443b
//
// This returned STATUS_NOT_FOUND with both out-parameters zeroed, on the reasoning
// that "we have no module image to hand back a section from". We do: the resource
// table is an optional header of the XEX we already publish, and every byte it
// points at is inside the image the loader already mapped. The comment was written
// about a loader that did not exist yet and was never re-asked (gotcha 13).
//
// What it cost is finding 50: `Digest` is the digest manager's hash table, and a
// null table is not a soft failure. `sub_82788478` calls this for "Digest", tests
// only the returned POINTER, and on zero runs
// `dbAssert(0 && "Bad file digest.  Please re-link the executable and try again.")`
// from `digestmanager.cpp` — whose tail is `twi 31,r0,22` and `stw r26,0(0)`. That
// is the SIGSEGV at file #137 `audio\Prologue.txt`, 53 files deeper than any gate
// here, and it presents as a null-pointer bug in guest code because XenonRecomp
// lowers `twi` to nothing and the store one instruction later is what faults.
//
// The module handle is ignored: this process has exactly one module with resources,
// and answering the title's own is right for every caller observed. A second module
// would need a real registry, and inventing one before a caller exists is the
// speculation gotcha 5 forbids.
//
// Both out-parameters are written on every path. Finding 14: an error return does
// not protect a caller that ignores the status and reads the buffer.
static uint32_t XexGetModuleSection_x(uint32_t module, const char* name, be<uint32_t>* dataOut,
                                      be<uint32_t>* sizeOut)
{
    uint32_t address = 0, size = 0;
    const bool found = XexFindResource(name, address, size);
    if (dataOut)
        *dataOut = address;
    if (sizeOut)
        *sizeOut = size;
    KLOG("XexGetModuleSection module=%08X name='%s' -> %08X %u bytes%s\n", module,
         name ? name : "", address, size, found ? "" : " (NOT FOUND)");
    return found ? 0 : STATUS_NOT_FOUND;
}

// Filesystem cache sizing hint. A1: FscSetCacheElementCount(00000000, 00000400) then
// (00000000, 00000020) — the title sizes the cache up for the boot load and back
// down afterwards. We do not model the 360's file cache at all, so doing nothing is
// faithful; the return is the previous element count.
static uint32_t FscSetCacheElementCount_x(uint32_t flags, uint32_t count) { return 0; }

// KeInitializeDpc(dpc, routine, context) — initialise a guest KDPC object.
//
// Case Zero imports this and A1 calls it twice, but it imports NEITHER
// KeInsertQueueDpc nor KeRemoveQueueDpc, so nothing in this title can ever queue one
// of these objects through the kernel. That makes the whole DPC dispatch machinery
// the previous port carries unnecessary here — a good example of gotcha 10 paying
// off in the other direction: the import list says what NOT to build.
//
// The struct still has to be initialised, because the guest reads it back. XDPC is
// {Type/Importance/Number (4), ListEntry (8), Routine, Context, Arg1, Arg2}.
static void KeInitializeDpc_x(be<uint32_t>* dpc, uint32_t routine, uint32_t context)
{
    if (!dpc)
        return;
    dpc[0] = 19 << 24; // DpcObject
    dpc[1] = 0;        // ListEntry.Flink — an empty list, i.e. not queued
    dpc[2] = 0;        // ListEntry.Blink
    dpc[3] = routine;
    dpc[4] = context;
    dpc[5] = 0;
    dpc[6] = 0;
}

GUEST_FUNCTION_HOOK(__imp__ExGetXConfigSetting, ExGetXConfigSetting_x)
GUEST_FUNCTION_HOOK(__imp__KeInitializeDpc, KeInitializeDpc_x)
GUEST_FUNCTION_HOOK(__imp__XexCheckExecutablePrivilege, XexCheckExecutablePrivilege_x)
GUEST_FUNCTION_HOOK(__imp__XGetLanguage, XGetLanguage_x)
GUEST_FUNCTION_HOOK(__imp__XGetAVPack, XGetAVPack_x)
GUEST_FUNCTION_HOOK(__imp__XGetGameRegion, XGetGameRegion_x)
GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle_x)
GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField_x)
GUEST_FUNCTION_HOOK(__imp__XexLoadImage, XexLoadImage_x)
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress_x)
GUEST_FUNCTION_HOOK(__imp__XamGetExecutionId, XamGetExecutionId_x)
GUEST_FUNCTION_HOOK(__imp__FscSetCacheElementCount, FscSetCacheElementCount_x)
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification,
                    ExRegisterTitleTerminateNotification_x)

// ---------------------------------------------------------------------------------
// XeCrypt SHA-1. Real, because a hash has no honest failure value.
//
// These were three of the generated honest-failure stubs, and finding 50 is what they
// cost: `sub_82822420/28/30` are one-instruction tail-call thunks onto them
// (`b 0x829C3084`, `b 0x829C3094`, `li r5,0x14; b 0x829C30A4` — gotcha 64, the callee
// is invisible in the caller's disassembly), and the digest manager calls them to hash
// a loaded file before use. A stub left the 20-byte output untouched, so the guest
// compared twenty ZERO bytes against a real digest, failed, and ran
// `dbAssert("Bad file digest...")`, whose tail is `twi 31,r0,22` and a store to
// address 0 — the SIGSEGV at file #137 `audio\Prologue.txt`.
//
// This is gotcha 59's family. "Fail honestly" is not available to a function whose
// result is a VALUE the caller consumes rather than a status it can test: there is no
// SHA-1 digest that means "not implemented", and the stub's silence reads as a
// specific, wrong answer. Implementing it is the only correct option.
//
// The state layout is the console's, because it is a guest stack object shared between
// the three entry points: `{ be32 count; be32 state[5]; u8 buffer[64] }`, 88 bytes.
// The guest reserves 0xA0 of frame for it, so the size is not tight.
namespace
{
constexpr uint32_t kShaCount = 0;
constexpr uint32_t kShaState = 4;
constexpr uint32_t kShaBuffer = 24;

inline uint32_t Rol32(uint32_t v, uint32_t n) { return (v << n) | (v >> (32 - n)); }

// One 64-byte block into the five-word state. Plain FIPS 180-1.
void Sha1Block(uint32_t h[5], const uint8_t* block)
{
    uint32_t w[80];
    for (uint32_t i = 0; i < 16; i++)
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    for (uint32_t i = 16; i < 80; i++)
        w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (uint32_t i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        const uint32_t t = Rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
} // namespace

static void XeCryptShaInit_x(uint32_t state)
{
    if (!state)
        return;
    uint8_t* base = g_memory.base;
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                                    0xC3D2E1F0 };
    PPC_STORE_U32(state + kShaCount, 0);
    for (uint32_t i = 0; i < 5; i++)
        PPC_STORE_U32(state + kShaState + i * 4, iv[i]);
}

// The message is a BYTE stream, so the guest's buffer is read straight out of guest
// memory with no swap — only the state's dwords are big-endian. The partial block has
// to live in the state across calls, because this title hashes the file and then the
// file's LENGTH as a SEPARATE four-byte update: an implementation that only handled a
// single call would produce a confident wrong digest on every real use.
static void XeCryptShaUpdate_x(uint32_t state, uint32_t input, uint32_t count)
{
    if (!state || (!input && count))
        return;
    uint8_t* base = g_memory.base;
    uint32_t h[5];
    for (uint32_t i = 0; i < 5; i++)
        h[i] = PPC_LOAD_U32(state + kShaState + i * 4);

    const uint32_t total = PPC_LOAD_U32(state + kShaCount);
    uint32_t held = total % 64;
    uint8_t* buffer = base + state + kShaBuffer;
    const uint8_t* src = base + input;

    for (uint32_t i = 0; i < count; i++)
    {
        buffer[held] = src[i];
        if (++held == 64)
        {
            Sha1Block(h, buffer);
            held = 0;
        }
    }
    PPC_STORE_U32(state + kShaCount, total + count);
    for (uint32_t i = 0; i < 5; i++)
        PPC_STORE_U32(state + kShaState + i * 4, h[i]);
}

static void XeCryptShaFinal_x(uint32_t state, uint32_t out, uint32_t outCount)
{
    if (!state || !out)
        return;
    uint8_t* base = g_memory.base;
    uint32_t h[5];
    for (uint32_t i = 0; i < 5; i++)
        h[i] = PPC_LOAD_U32(state + kShaState + i * 4);
    const uint32_t total = PPC_LOAD_U32(state + kShaCount);
    const uint32_t held = total % 64;

    uint8_t block[64] = {};
    memcpy(block, base + state + kShaBuffer, held);
    block[held] = 0x80;
    if (held >= 56)
    {
        Sha1Block(h, block);
        memset(block, 0, sizeof block);
    }
    const uint64_t bits = uint64_t(total) * 8;
    for (uint32_t i = 0; i < 8; i++)
        block[56 + i] = uint8_t(bits >> (56 - i * 8));
    Sha1Block(h, block);

    // outCount is the caller's buffer size; this title passes 0x14. A shorter one gets
    // the leading bytes, as the console does. A longer one is NOT padded, because
    // writing past the end of the digest would be inventing data.
    const uint32_t n = outCount < 20 ? outCount : 20;
    for (uint32_t i = 0; i < n; i++)
        PPC_STORE_U8(out + i, uint8_t(h[i / 4] >> (24 - (i % 4) * 8)));
}

// The one-shot: up to three buffers hashed in order into one digest. It keeps its
// state on the HOST stack and reuses the block function, so there is one SHA-1 in this
// file rather than two that can drift. Not yet reached by this title (gotcha 67 —
// implemented is a prediction, not a result); it is here because leaving it stubbed
// leaves exactly the same silent-wrong-value trap the other three just cost us.
static void XeCryptSha_x(uint32_t in1, uint32_t in1Size, uint32_t in2, uint32_t in2Size,
                         uint32_t in3, uint32_t in3Size, uint32_t out, uint32_t outSize)
{
    if (!out)
        return;
    uint8_t* base = g_memory.base;
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t block[64] = {};
    uint32_t held = 0;
    uint64_t total = 0;

    const uint32_t ins[3] = { in1, in2, in3 };
    const uint32_t sizes[3] = { in1Size, in2Size, in3Size };
    for (uint32_t s = 0; s < 3; s++)
    {
        if (!ins[s] || !sizes[s])
            continue;
        const uint8_t* src = base + ins[s];
        for (uint32_t i = 0; i < sizes[s]; i++)
        {
            block[held] = src[i];
            if (++held == 64)
            {
                Sha1Block(h, block);
                held = 0;
            }
        }
        total += sizes[s];
    }
    memset(block + held, 0, sizeof block - held);
    block[held] = 0x80;
    if (held >= 56)
    {
        Sha1Block(h, block);
        memset(block, 0, sizeof block);
    }
    const uint64_t bits = total * 8;
    for (uint32_t i = 0; i < 8; i++)
        block[56 + i] = uint8_t(bits >> (56 - i * 8));
    Sha1Block(h, block);

    const uint32_t n = outSize < 20 ? outSize : 20;
    for (uint32_t i = 0; i < n; i++)
        PPC_STORE_U8(out + i, uint8_t(h[i / 4] >> (24 - (i % 4) * 8)));
}

GUEST_FUNCTION_HOOK(__imp__XexGetModuleSection, XexGetModuleSection_x)
GUEST_FUNCTION_HOOK(__imp__XeCryptShaInit, XeCryptShaInit_x)
GUEST_FUNCTION_HOOK(__imp__XeCryptShaUpdate, XeCryptShaUpdate_x)
GUEST_FUNCTION_HOOK(__imp__XeCryptShaFinal, XeCryptShaFinal_x)
GUEST_FUNCTION_HOOK(__imp__XeCryptSha, XeCryptSha_x)
// Unloading a module we never really loaded is a genuine no-op in this runtime.
GUEST_FUNCTION_STUB(__imp__XexUnloadImage)

// ---------------------------------------------------------------------------
// System version — the value that decides which code paths the title takes
// ---------------------------------------------------------------------------

// XamGetSystemVersion returns the dashboard version as a packed dword, and Case
// Zero uses it as a **feature gate** in seven places. Each one is
// `cmplw` (unsigned) against a constant, taking the older-system branch below it:
//
//     825D7E68 -> 0x20096B00      825F209C -> 0x200CE900
//     825DFB34 -> 0x200A3200      825F2218 -> 0x200CE900
//     825DFD28 -> 0x200A3200      828A0F2C -> 0x0008A100
//                                 828A1004 -> 0x0008A100
//
// Above the threshold the title resolves newer XAM entry points **dynamically**:
// `XexGetModuleHandle("xam.xex")` then `XexGetProcedureAddress(handle, <ordinal>)`,
// and it calls whatever comes back. We do not have those exports, so claiming a
// version at or above any threshold is exactly the "faking success" gotcha 5
// forbids — the title would ask us for an entry point we cannot supply, and (with
// our honest-failure XexGetProcedureAddress) silently lose the feature instead.
//
// Returning **0** is the truthful statement "this system does not have those newer
// XAM entry points", and it is also what the ground truth shows: in A1 the title
// takes the static branch at 825DFB34 and calls `NetDll_WSAStartup` directly, with
// no XexGetModuleHandle anywhere near it. As an unimplemented stub this returned
// STATUS_NOT_IMPLEMENTED (0xC0000002), which compares ABOVE every threshold, so we
// took the dynamic path at every one of the seven sites — the whole gate-position-57
// divergence, and the first-order reason our boot lost NetDll_WSAStartup,
// XamUserGetSigninInfo and XamUserCheckPrivilege.
//
// If a later phase implements a real XAM export table, raising this is the right
// move — but it must be raised WITH those exports, never before them.
static uint32_t XamGetSystemVersion_x()
{
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XamGetSystemVersion, XamGetSystemVersion_x)

// RtlCompareStringN(s1, len1, s2, len2, caseInsensitive) — memcmp semantics over the
// shorter length, then by length; 0 means equal.
//
// The argument shape is read off the guest, not assumed: at 0x82822670 Case Zero
// calls it with r3 = a counted string's buffer, r4 = 2, r5 = a literal, r6 = 2,
// r7 = 1, and tests the result with `cmpwi r3,0`. That is five arguments with the
// lengths interleaved, which is the xboxkrnl form.
//
// This was an honest-failure stub returning STATUS_NOT_IMPLEMENTED, and that is a
// case the "fail honestly" rule does not cover: 0xC0000002 is a perfectly valid
// *answer* to a comparison — it means "not equal" — so the guest never saw an error,
// it saw a confident wrong result. Every comparison silently answered "different".
// A predicate-shaped import has no honest failure value, which makes implementing it
// the only correct option (gotcha 5's blind spot).
static uint32_t RtlCompareStringN_x(const char* s1, uint32_t len1, const char* s2,
                                    uint32_t len2, uint32_t caseInsensitive)
{
    if (!s1 || !s2)
        return s1 == s2 ? 0 : (s1 ? 1 : uint32_t(-1));

    const uint32_t len = len1 < len2 ? len1 : len2;
    for (uint32_t i = 0; i < len; i++)
    {
        unsigned char a = uint8_t(s1[i]), b = uint8_t(s2[i]);
        if (caseInsensitive)
        {
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
        }
        if (a != b)
            return uint32_t(int32_t(a) - int32_t(b));
    }
    return uint32_t(int32_t(len1) - int32_t(len2));
}

GUEST_FUNCTION_HOOK(__imp__RtlCompareStringN, RtlCompareStringN_x)

// ---------------------------------------------------------------------------
// Networking — enough of it to keep the boot on hardware's path
// ---------------------------------------------------------------------------
//
// There is no network here and there will not be one. The question these answer is
// not "can we connect" but "does the title's network init run to completion", because
// on hardware it does, and the code after it is not optional — A1 goes straight from
// this block into XamUserGetName and the profile reads at gate positions 61-68.
//
// Every arity below is taken from A1, not assumed (gotcha 48). All NetDll_* exports
// carry Xenia's leading `caller` argument, which A1 logs as 00000001 everywhere:
//
//     NetDll_XNetStartup(00000001, 7018F810)
//     NetDll_WSAStartup(00000001, 0002, 7018F620)   and (00000001, 0202, 7018F820)
//     NetDll_XNetGetTitleXnAddr(00000001, 7018F5D0)
//     NetDll_XNetRandom(00000001, 7018F740, 00000002)
//
// The guest thunks agree: sub_825DFBD0 is `mr r4,r3; li r3,1; b sub_825DFB10`, i.e.
// it inserts the caller argument itself.

// XNetStartup returns 0 on success, and sub_8280D748 tests exactly that:
//
//     bl sub_825DFBD0            ; XNetStartup(1, params)
//     cmpwi r3,0
//     beq  loc_8280D7B0          ; 0 -> carry on to WSAStartup(0x202, &wsadata)
//     bl   sub_825DFBE0          ; non-zero -> tear down and skip the whole block
//
// As an honest-failure stub this returned 0xC0000002, so the title tore the stack
// down every boot and never called WSAStartup at all. Returning 0 is not claiming a
// network exists — it is claiming the socket layer initialised, which is true in the
// only sense the title can observe here.
static uint32_t NetDll_XNetStartup_x(uint32_t caller, uint32_t params)
{
    (void)caller;
    (void)params;
    return 0;
}

static uint32_t NetDll_XNetCleanup_x(uint32_t caller, uint32_t params)
{
    (void)caller;
    (void)params;
    return 0;
}

// WSAStartup(caller, wVersionRequested, lpWSAData). The out parameter is a 398-byte
// WSADATA, and the guest's own code proves the size: at 0x8280D7C8 it stores one
// halfword at r1+96 and memsets 398 bytes at r1+98 before passing r1+96. So
// 2 + 2 + 257 + 129 + 2 + 2 + 4 = 398, the classic Winsock layout.
//
// The guest zeroes it first, so only the fields it can act on are written. Filling it
// matters anyway: a caller that trusts an untouched out-buffer is finding 15's first
// failure mode, and this one IS read — sub_828C1BD8 compares the result against -1.
struct GuestWsaData
{
    be<uint16_t> wVersion;
    be<uint16_t> wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    be<uint16_t> iMaxSockets;
    be<uint16_t> iMaxUdpDg;
    be<uint32_t> lpVendorInfo;
};
static_assert(sizeof(GuestWsaData) == 398 + 2, "WSADATA is 398 bytes plus tail padding");

static uint32_t NetDll_WSAStartup_x(uint32_t caller, uint32_t version, GuestWsaData* data)
{
    (void)caller;
    if (data)
    {
        // Echo the requested version back, as a real stack does when it can satisfy
        // it. A1 shows both 0x0002 and 0x0202 requested, and both succeed there.
        data->wVersion = uint16_t(version);
        data->wHighVersion = 0x0202;
        data->iMaxSockets = 64;
        data->iMaxUdpDg = 1024;
        data->lpVendorInfo = 0;
    }
    return 0;
}

static uint32_t NetDll_WSACleanup_x(uint32_t caller)
{
    (void)caller;
    return 0;
}

// XNetGetTitleXnAddr(caller, XNADDR*) returns a BITMASK of what the adapter has, and
// the title reads it bit by bit. sub_825C6DA0 and sub_8280D970 both test, in order,
// bits for DHCP (0x08), PPPoE (0x10), STATIC (0x04) and ETHERNET (0x02), and return 0
// when none are set.
//
// The value that must NOT be returned is 0: XNET_GET_XNADDR_PENDING is zero, and
// `mr. r31,r3; beq ...` at 0x8280D7FC sends the title back to ask again — a poll with
// no exit. XNET_GET_XNADDR_NONE (0x0001) says "the answer is final, and there is no
// address", which is both true here and terminating.
constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;

static uint32_t NetDll_XNetGetTitleXnAddr_x(uint32_t caller, be<uint32_t>* xnaddr)
{
    (void)caller;
    // XNADDR is 36 bytes: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]. With
    // no address to report every field is genuinely zero, but it has to be WRITTEN —
    // the title reads abEnet out of it regardless of the status bits.
    if (xnaddr)
        memset(xnaddr, 0, 36);
    return XNET_GET_XNADDR_NONE;
}

// XNetRandom(caller, buffer, length) fills a buffer with random bytes; A1 asks for 2.
//
// Deliberately a fixed-seed PRNG rather than the host's entropy. Everything in this
// project is measured by re-running the same binary and diffing (the phase gate, the
// crash reports, the A/B arms), and an import that injects real entropy makes two runs
// legitimately different for reasons nobody can see in a log. If a later phase needs
// unpredictability it can seed this from somewhere and say so.
static uint32_t NetDll_XNetRandom_x(uint32_t caller, uint8_t* buffer, uint32_t length)
{
    (void)caller;
    if (!buffer)
        return 0;
    static uint32_t state = 0x9E3779B9;
    for (uint32_t i = 0; i < length; i++)
    {
        state = state * 1664525u + 1013904223u;   // Numerical Recipes LCG
        buffer[i] = uint8_t(state >> 24);
    }
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetCleanup, NetDll_XNetCleanup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACleanup, NetDll_WSACleanup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetRandom, NetDll_XNetRandom_x)

// ---------------------------------------------------------------------------
// The signed-in user — one local profile, no Live
// ---------------------------------------------------------------------------
//
// THE POLICY, STATED ONCE. This runtime presents exactly one user: index 0, signed
// in **locally**, with no online identity. That is a choice, and it is the choice A1
// documents — the capture goes XamUserGetName, XamUserGetSigninInfo, XamUserGetName,
// then XamUserReadProfileSettings and XamUserCheckPrivilege, which is the flow of a
// title that found a profile. Reporting "nobody is signed in" instead is defensible
// on its own but is NOT what the ground truth shows, and the frontend past this point
// would take a different path with nothing to check it against.
//
// Users 1..3 are absent, and say so honestly.
constexpr uint32_t kLocalUserIndex = 0;
constexpr uint32_t ERROR_NO_SUCH_USER = 0x00000525;

// Offline XUIDs have the top nibble 0xE; online ones do not. Using an offline-shaped
// value is the part of this that is not arbitrary — it is how the title can tell,
// without asking, that this profile has no Live identity.
constexpr uint64_t kLocalOfflineXuid = 0xE000000000000001ull;

// The gamertag is NOT observable in any capture — Xenia logs the buffer's contents
// before the call, never after — and nothing in the image branches on it: every
// consumer of XamUserGetName just copies the string out (sub_824BEA50, sub_82589C50,
// sub_825C7B18, sub_825C9608). So this is a free choice, and it is flagged as one.
constexpr const char* kLocalUserName = "Player";

// XamUserGetSigninState returns the state itself, not a status: 0 = not signed in,
// 1 = signed in locally, 2 = signed in to Live.
static uint32_t XamUserGetSigninState_x(uint32_t userIndex)
{
    return userIndex == kLocalUserIndex ? 1u : 0u;
}

// XamUserGetName(userIndex, buffer, bufferLen) -> 0 on success. A1 always asks for
// 0x10 bytes, which is XUSER_NAME_SIZE. Consumers test `cmplwi r3,0`.
static uint32_t XamUserGetName_x(uint32_t userIndex, char* buffer, uint32_t bufferLen)
{
    if (!buffer || bufferLen == 0)
        return ERROR_NO_SUCH_USER;
    if (userIndex != kLocalUserIndex)
    {
        buffer[0] = '\0';       // an absent user still gets a defined buffer
        return ERROR_NO_SUCH_USER;
    }
    memset(buffer, 0, bufferLen);
    const size_t n = strlen(kLocalUserName);
    memcpy(buffer, kLocalUserName, n < bufferLen ? n : bufferLen - 1);
    return 0;
}

// XamUserGetSigninInfo(userIndex, flags, out) -> 0 on success.
//
// THE OUT PARAMETER IS EIGHT BYTES, and that is read off the guest rather than taken
// from the SDK's XUSER_SIGNIN_INFO. sub_825C2F88 is the only consumer in the image: it
// zeroes an 8-byte slot with `std r11,80(r1)`, passes `r1+80`, and reads the result
// back with `ld r3,80(r1)`. Writing a larger struct there would be writing past what
// the caller reserved, on the strength of a layout nothing here confirms (gotcha 48).
//
// WHICH XUID: A1 calls it twice, `(0, 00000001, ...)` then `(0, 00000000, ...)`, and
// the guest only makes the second call when the first returned a ZERO xuid
// (`cmpldi cr6,r3,0; bne cr6,<done>`). So on hardware flags=1 asks for the ONLINE xuid
// and there is none. Reproducing that is what makes our call sequence match A1's,
// and it is also true of us.
static uint32_t XamUserGetSigninInfo_x(uint32_t userIndex, uint32_t flags, be<uint64_t>* out)
{
    if (!out)
        return ERROR_NO_SUCH_USER;
    if (userIndex != kLocalUserIndex)
    {
        *out = 0;
        return ERROR_NO_SUCH_USER;
    }
    *out = (flags & 1) ? 0ull : kLocalOfflineXuid;
    return 0;
}

// XamUserGetXUID(userIndex, type, out) -> 0 on success; the out is 8 bytes (A1 logs a
// 16-hex-digit pre-call value). This profile has one XUID whatever the type asked for.
static uint32_t XamUserGetXUID_x(uint32_t userIndex, uint32_t type, be<uint64_t>* out)
{
    (void)type;
    if (!out)
        return ERROR_NO_SUCH_USER;
    if (userIndex != kLocalUserIndex)
    {
        *out = 0;
        return ERROR_NO_SUCH_USER;
    }
    *out = kLocalOfflineXuid;
    return 0;
}

// XamUserCheckPrivilege(userIndex, privilege, out) -> 0 on success, with a 4-byte
// BOOL out: sub_825E4E88 does `lwz r11,96(r1); cmpwi cr6,r11,1`.
//
// A1 asks for privilege 0x000000FC = XPRIVILEGE_COMMUNICATIONS. With no Live identity
// (see the XUID above) the truthful answer is "not granted", and saying otherwise
// would invite the title into an online path this runtime cannot follow.
static uint32_t XamUserCheckPrivilege_x(uint32_t userIndex, uint32_t privilege,
                                        be<uint32_t>* result)
{
    (void)privilege;
    if (!result)
        return ERROR_NO_SUCH_USER;
    *result = 0;
    return userIndex == kLocalUserIndex ? 0u : ERROR_NO_SUCH_USER;
}

GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState_x)
GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName_x)
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo_x)
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID_x)
GUEST_FUNCTION_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege_x)

// ---------------------------------------------------------------------------
// The profile settings block — a layout read off the guest, not off the SDK
// ---------------------------------------------------------------------------
//
// XamUserReadProfileSettings is the domino past the user block: A1 calls it five
// times during boot, and until it works the title tears its frontend down and calls
// XMsgCancelIORequest, which hardware never does there (docs/phase1-notes.md
// finding 30).
//
// The capture contains no return value and no post-call buffer for ANY of this, so
// every number below is derived from one of two things that cannot lie: the sizes
// A1 shows the title itself computing, and the guest code that walks the result.
//
// THE SIZE IS THE FIRST WITNESS. A1's calls come in pairs. The query call passes a
// null buffer and a zero size and the title then re-calls with the size the kernel
// wrote back:
//
//     ReadProfileSettings(FFFE07D1, FF, 0,0, 3, 829E05C8, 4017B400(00000000), 0, 0)
//     ReadProfileSettings(FFFE07D1, 00, 0,0, 3, 829E05C8, 4017B400(00000080), 4017B530, 4017B3E0)
//     ReadProfileSettings(00000000, 00, 0,0, 2, 7018F068, 7018F060(00000058), E7367A00, 0)
//
// 3 settings -> 0x80 and 2 settings -> 0x58 have exactly one solution in integers:
// an 8-byte header plus 40 bytes per setting. Two independent points, and neither
// leaves room for variable-length payload — this title only ever asks for
// fixed-size settings at boot, which is what makes the flat arithmetic safe.
//
// THE GUEST'S OWN WALK IS THE SECOND. sub_825E4E88 (ppc_recomp.110.cpp) reads the
// buffer back, and its loop names every offset that matters:
//
//     count = [results + 0]                   header +0  = setting count
//     for i in 0..count:
//         p  = [results + 4] + i*40           header +4  = pointer to the array
//         id = [p + 16]                       setting    +16 = setting id
//         switch (id - 0x1004000C) ...        VOICE_MUTED / _THRU_SPEAKERS / _VOLUME
//         case 2: v = [p + 32]                setting    +32 = the value
//                 if (v > 100) v = 100        ... and it is a 0..100 volume
//
// +16 and +32 with a stride of 40 pin the whole struct: `from` at +0, the
// xuid/user-index union at +8 (8-aligned, hence the pad at +4), the id at +16, and
// an X_USER_DATA at +24 whose type byte is at +24 and whose 8-byte value union is at
// +32. That is the same layout Xenia uses, but it is written here because the guest
// confirmed it, not because Xenia asserts it.
struct GuestProfileSetting
{
    be<uint32_t> from;        // +0   0 = unset, 1 = a global setting, 2 = title-specific
    be<uint32_t> pad0;        // +4   the union below is 8-aligned
    be<uint64_t> xuid;        // +8   or the user index, when the call passed no xuids
    be<uint32_t> settingId;   // +16
    be<uint32_t> pad1;        // +20  X_USER_DATA is 8-aligned too
    uint8_t      type;        // +24
    uint8_t      pad2[7];
    // The value union, written as two explicit halves rather than a C++ union: a
    // be<uint64_t> here would put a 32-bit setting's bytes at +36..39, where the
    // guest's `lwz r10,32(r11)` reads zero. Big-endian unions are exactly the place
    // a silent wrong value comes from, so the halves are named.
    be<uint32_t> value0;      // +32  s32/u32/float, the high half of s64/double
    be<uint32_t> value1;      // +36  the low half of s64/double
};
static_assert(sizeof(GuestProfileSetting) == 40, "the 0x80/0x58 sizes A1 shows require 40");

struct GuestProfileResults
{
    be<uint32_t> settingCount;  // +0
    be<uint32_t> settingsPtr;   // +4  guest address of the array, i.e. results + 8
};
static_assert(sizeof(GuestProfileResults) == 8, "8 + 40n must reproduce 0x80 and 0x58");

// XOVERLAPPED, as read by sub_825D83A8 — which is the title's own hand-rolled
// XGetOverlappedResult and therefore an authoritative statement of the layout:
//   `lwz r11,0(r3)` / `cmplwi r11,997`  -> +0  is the result, 997 = still pending
//   `lwz r3,12(r3)` then wait on it     -> +12 is the completion event handle
//   `lwz r11,4(r31); stw r11,0(r30)`    -> +4  is the transferred length
struct GuestOverlapped
{
    be<uint32_t> result;             // +0
    be<uint32_t> length;             // +4
    be<uint32_t> context;            // +8
    be<uint32_t> event;              // +12
    be<uint32_t> completionRoutine;  // +16
    be<uint32_t> completionContext;  // +20
    be<uint32_t> extendedError;      // +24
};

constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t ERROR_IO_PENDING          = 997;

// The data type lives in the top nibble of the setting id — 0x1004000C is an INT32,
// 0x5004000B a FLOAT, 0x4… a UNICODE string. Deriving it means a setting this title
// asks for and we have never seen still gets a well-formed record, instead of a
// hand-maintained table silently returning the wrong shape for id number twelve.
enum : uint8_t
{
    XUSER_DATA_TYPE_CONTEXT  = 0,
    XUSER_DATA_TYPE_INT32    = 1,
    XUSER_DATA_TYPE_INT64    = 2,
    XUSER_DATA_TYPE_DOUBLE   = 3,
    XUSER_DATA_TYPE_UNICODE  = 4,
    XUSER_DATA_TYPE_FLOAT    = 5,
    XUSER_DATA_TYPE_BINARY   = 6,
    XUSER_DATA_TYPE_DATETIME = 7,
};

// The settings this runtime has a defensible answer for. Everything else is
// reported as present-but-zero, which is a value the title can act on; the
// alternative — from = 0, "not set" — is also honest but sends the voice subsystem
// down a path A1 never took, and there is nothing to check that path against.
//
// The voice trio is what boot asks for, and only the volume has a non-obvious
// answer: the guest clamps anything above 100 down to 100, so 100 is the top of the
// range it will accept, and it is the console default.
struct ProfileDefault { uint32_t id; uint32_t value; };
static const ProfileDefault kProfileDefaults[] = {
    { 0x1004000C, 0   },  // XPROFILE_OPTION_VOICE_MUTED         — not muted
    { 0x1004000D, 0   },  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS — headset, not TV
    { 0x1004000E, 100 },  // XPROFILE_OPTION_VOICE_VOLUME        — 0..100, full
};

static uint32_t ProfileDefaultValue(uint32_t settingId)
{
    for (const auto& d : kProfileDefaults)
        if (d.id == settingId)
            return d.value;
    return 0;
}

// Finish an async XAM request in place. The title's own XGetOverlappedResult only
// waits when +0 still reads 997, so completing here means it never blocks — but the
// event is signalled anyway, because a *different* caller may wait on the handle
// directly and gotcha 46 is precisely about the notification half of an async
// contract being the half that gets dropped.
void CompleteOverlapped(GuestOverlapped* ovl, uint32_t result, uint32_t length)
{
    if (!ovl)
        return;
    ovl->result = result;
    ovl->length = length;
    ovl->extendedError = 0;

    // NOT dispatched: the completion routine at +16. No boot path in Case Zero sets
    // one (this warns if that ever stops being true), and calling back into guest
    // code from inside an import needs the APC machinery NtReadFile uses, not a
    // direct call. If this line ever prints, that is the work it is asking for.
    if (ovl->completionRoutine != 0)
    {
        static std::atomic<int> warned{ 0 };
        if (warned.fetch_add(1, std::memory_order_relaxed) < 8)
            fprintf(stderr, "[xam] overlapped completion routine %08X NOT dispatched\n",
                    ovl->completionRoutine.get());
    }

    SignalGuestEvent(ovl->event);
}

// The same thing addressed by GUEST ADDRESS, for the exports that live in another
// TU. content.cpp needs it because XamContentCreateEx is asynchronous whenever the
// caller supplies an overlapped, and that is the half of the contract this runtime
// has dropped twice now (gotcha 46).
void Xam_CompleteOverlapped(uint32_t overlappedPtr, uint32_t result, uint32_t length)
{
    if (!overlappedPtr)
        return;
    CompleteOverlapped(
        reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlappedPtr)), result,
        length);
}

// XamUserReadProfileSettings(titleId, userIndex, numXuids, xuids, numSettingIds,
//                            settingIds, sizePtr, buffer, overlapped) -> status
//
// NINE arguments, so the last one arrives from the caller's parameter save area at
// r1+0x54 rather than a register — which guestcall.h already handles, and which the
// image confirms: sub_825E4E88 does `addi r7,r31,160; stw r7,84(r1)` before loading
// r3..r10, and 0x54 is 84.
//
// RETURN VALUES, from the two call sites that test them:
//   sub_825E5D28 (the size query): `cmplwi cr6,r3,122; beq` — it REQUIRES
//       ERROR_INSUFFICIENT_BUFFER. Any other answer, including a success, sends it
//       to the error path that sets 0x80004005 and cancels the request.
//   sub_825E4E88 (the real read): `r3 == 0` or `r3 == 997` both proceed; anything
//       else stores the error code 0x00026404 and gives up.
// So a query call must fail with 122 and a read call may report itself pending. We
// do the work synchronously and return ERROR_IO_PENDING only when the caller
// supplied an overlapped, which is the XAM contract and is inside what both sites
// accept.
//
// userIndex 0xFF appears in all four query calls and means "no particular user" —
// answering NO_SUCH_USER to it would fail the size query and stall the boot before
// the read ever happens.
static uint32_t XamUserReadProfileSettings_x(uint32_t titleId, uint32_t userIndex,
                                             uint32_t numXuids, be<uint64_t>* xuids,
                                             uint32_t numSettingIds,
                                             be<uint32_t>* settingIds,
                                             be<uint32_t>* sizePtr, void* buffer,
                                             uint32_t overlapped)
{
    (void)titleId;   // FFFE07D1 = the dashboard's global settings, 0 = this title's
    (void)numXuids;  // always 0 here; the results are addressed by user index
    (void)xuids;

    auto* ovl = overlapped ? reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped))
                           : nullptr;

    if (!settingIds || !sizePtr)
    {
        if (sizePtr)
            *sizePtr = 0;
        CompleteOverlapped(ovl, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }

    const uint32_t required =
        uint32_t(sizeof(GuestProfileResults) + numSettingIds * sizeof(GuestProfileSetting));

    // The query call, and the same answer if the caller's buffer is too small.
    if (!buffer || sizePtr->get() < required)
    {
        *sizePtr = required;
        CompleteOverlapped(ovl, ERROR_INSUFFICIENT_BUFFER, 0);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    if (userIndex != 0xFF && userIndex != kLocalUserIndex)
    {
        *sizePtr = required;
        CompleteOverlapped(ovl, ERROR_NO_SUCH_USER, 0);
        return ERROR_NO_SUCH_USER;
    }

    auto* results = reinterpret_cast<GuestProfileResults*>(buffer);
    auto* array = reinterpret_cast<GuestProfileSetting*>(results + 1);

    results->settingCount = numSettingIds;
    results->settingsPtr = g_memory.MapVirtual(array);

    for (uint32_t i = 0; i < numSettingIds; i++)
    {
        const uint32_t id = settingIds[i].get();
        const uint8_t type = uint8_t((id >> 28) & 0xF);

        memset(&array[i], 0, sizeof(GuestProfileSetting));
        array[i].from = 1;                 // present, and not title-specific
        array[i].xuid = 0;                 // no xuids were passed; user index 0
        array[i].settingId = id;
        array[i].type = type;

        switch (type)
        {
            case XUSER_DATA_TYPE_INT32:
            case XUSER_DATA_TYPE_CONTEXT:
            case XUSER_DATA_TYPE_FLOAT:
                array[i].value0 = ProfileDefaultValue(id);
                break;

            case XUSER_DATA_TYPE_INT64:
            case XUSER_DATA_TYPE_DOUBLE:
            case XUSER_DATA_TYPE_DATETIME:
                array[i].value0 = 0;       // high half
                array[i].value1 = ProfileDefaultValue(id);
                break;

            // A string or a blob would need its payload appended after the array,
            // and the required size computed above says there is none. Reporting a
            // zero-length, null-pointer blob is the shape a caller can survive; the
            // warning is there because the size arithmetic would silently be wrong
            // (gotcha 5's corollary — say so rather than quietly hand back garbage).
            case XUSER_DATA_TYPE_UNICODE:
            case XUSER_DATA_TYPE_BINARY:
            default:
                array[i].value0 = 0;       // size
                array[i].value1 = 0;       // pointer
                fprintf(stderr, "[xam] profile setting %08X is type %u (string/blob) — "
                                "returned empty; the size arithmetic does not cover it\n",
                        id, type);
                break;
        }
    }

    *sizePtr = required;
    CompleteOverlapped(ovl, 0, required);
    KLOG("XamUserReadProfileSettings(title=%08X, user=%02X, %u settings) -> %u bytes\n",
         titleId, userIndex, numSettingIds, required);
    return ovl ? ERROR_IO_PENDING : 0u;
}

GUEST_FUNCTION_HOOK(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings_x)

// ---------------------------------------------------------------------------
// Notifications — and a predicate that was answering "yes" 3,000 times a boot
// ---------------------------------------------------------------------------
//
// XNotifyGetNext(listener, matchId, &id, &param) is a BOOL: `cmpwi r3,0; beq skip`
// at all five of its call sites. As a generated honest-failure stub it returned
// STATUS_NOT_IMPLEMENTED (0xC0000002), which is not zero — so every poll told the
// title a notification HAD arrived, and it then read the id and param out of stack
// slots the stub never touched. A5 shows the real title polling one listener 10,480
// times in a boot; ours was manufacturing that many phantom events out of stale
// stack.
//
// This is gotcha 59 exactly, a second time: an import whose return value is a
// predicate has no honest failure value, so implementing it is the only correct
// option. It is also gotcha 42's other half — the out-parameters must be written on
// the FALSE path too, because the caller reads them regardless.
//
// WHAT WE POST: nothing, yet. An empty queue is the truthful statement that nothing
// has happened since boot, and it is the only statement we can currently support —
// there is no input layer, no storage layer and no system UI to generate an event.
// PostGuestNotification below is the seam those layers will use. Inventing a
// sign-in or storage event here to "look more alive" would be faking success at the
// one place the title is asking us a direct question.
//
// The ids the title cares about are readable in sub_825E4380, which compares the id
// against 10 and 14 — XN_SYS_SIGNINCHANGED and XN_SYS_PROFILESETTINGCHANGED. So
// when a real event source appears, those two are what it should raise.
struct NotifyListener final : KernelObject
{
    uint64_t mask;
    std::mutex m;
    std::deque<std::pair<uint32_t, uint32_t>> queue;   // (id, param), oldest first

    explicit NotifyListener(uint64_t areaMask) : mask(areaMask) {}
};

// The notification area is the id's high halfword; the listener mask has one bit per
// area. Every id this title tests for is in area 0 (the system area), so in practice
// this only ever asks "did you subscribe to bit 0" — but the shift is written out
// because a listener created with mask 0x20 (A1 shows one) is subscribing to
// something else entirely, and silently delivering system events to it would be
// wrong in a way nothing would report.
static bool ListenerWants(const NotifyListener* l, uint32_t id)
{
    return (l->mask & (1ull << (id >> 16))) != 0;
}

static std::vector<NotifyListener*> g_notifyListeners;

// The seam for a future input/storage/UI layer: post an event to every listener
// subscribed to its area. Unused today, and deliberately kept rather than deferred —
// the queue is only testable if something can fill it.
void PostGuestNotification(uint32_t id, uint32_t param)
{
    std::lock_guard guard(g_kernelLock);
    for (NotifyListener* l : g_notifyListeners)
    {
        if (!ListenerWants(l, id))
            continue;
        std::lock_guard q(l->m);
        l->queue.emplace_back(id, param);
    }
}

// A1: XamNotifyCreateListener(0000000000000001, 00000005) and four more with masks
// 3, 4, 5 and 0x20. The first argument is a 64-bit area mask in ONE register (the
// Xenon ABI passes a doubleword in a single GPR); the second is 5 in every call this
// title makes, and nothing here depends on it.
static uint32_t XamNotifyCreateListener_x(uint64_t mask, uint32_t flags)
{
    (void)flags;
    NotifyListener* listener = CreateKernelObject<NotifyListener>(mask);
    if (!listener)
        return 0;   // a handle-returning call fails by returning 0, not an NTSTATUS
    {
        std::lock_guard guard(g_kernelLock);
        g_notifyListeners.push_back(listener);
    }
    return GetKernelHandle(listener);
}

// matchId 0 means "any notification" — A5 shows this title only ever passing 0.
static uint32_t XNotifyGetNext_x(uint32_t handle, uint32_t matchId, be<uint32_t>* idOut,
                                 be<uint32_t>* paramOut)
{
    // Written first and unconditionally: the FALSE path is the common one, taken
    // thousands of times a boot, and it is the path whose out-parameters the stub
    // left as stack garbage.
    if (idOut)
        *idOut = 0;
    if (paramOut)
        *paramOut = 0;

    if (!handle || !IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return 0;
    auto* listener = dynamic_cast<NotifyListener*>(GetKernelObject(handle));
    if (!listener)
        return 0;

    std::lock_guard q(listener->m);
    for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
    {
        if (matchId != 0 && it->first != matchId)
            continue;
        if (idOut)
            *idOut = it->first;
        if (paramOut)
            *paramOut = it->second;
        listener->queue.erase(it);
        return 1;
    }
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XamNotifyCreateListener, XamNotifyCreateListener_x)
GUEST_FUNCTION_HOOK(__imp__XNotifyGetNext, XNotifyGetNext_x)

// ---------------------------------------------------------------------------
// The controller — one connected pad, fed by the host window (phase 3)
// ---------------------------------------------------------------------------
//
// POLICY, STATED ONCE, same shape as the single local user: player 1 holds a
// standard wired gamepad; players 2-4 hold nothing and say so. A1 polls
// XamInputGetCapabilities 1,108 times for user 0 AND 1,108 times for user 1 in a
// single boot, so "not connected" is an answer the title is built to receive
// constantly and is not an error path.
//
// Player 1's buttons come from `runtime/host/window.cpp` — a real keyboard and, when
// one is attached, a real SDL game controller. Before phase 3 this reported neutral
// forever, which was a *missing feature* rather than a claim that no buttons were
// pressed, and it is what parked the boot at the press-start screen (finding 37).
//
// The pad is still reported connected when the runtime is headless (CZ_NO_WINDOW=1
// or -DCZ_WINDOW=OFF). That is deliberate and it is the honest reading of the
// hardware: a console with a pad plugged in and nobody touching it is precisely a
// connected device reporting nothing pressed. Reporting NOT_CONNECTED instead would
// send the title down its "please reconnect the controller" path, which is a
// different boot, and the headless arm exists to be the SAME boot minus input.
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x0000048F;
constexpr uint32_t ERROR_EMPTY                = 0x00000490;

struct GuestInputGamepad      // 12 bytes
{
    be<uint16_t> buttons;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    be<int16_t> thumbLX, thumbLY, thumbRX, thumbRY;
};

struct GuestInputState        // 16 bytes
{
    be<uint32_t> packetNumber;
    GuestInputGamepad gamepad;
};

struct GuestInputVibration    // 4 bytes
{
    be<uint16_t> leftMotor;
    be<uint16_t> rightMotor;
};

// Confirmed against the guest: sub_825D7AC8 passes a buffer at r1+96 and then reads
// `lbz r11,97(r1)` (sub-type at +1) and `lhz r11,98(r1)` (flags at +2).
struct GuestInputCapabilities // 20 bytes
{
    uint8_t type;             // +0  XINPUT_DEVTYPE_GAMEPAD = 1
    uint8_t subType;          // +1
    be<uint16_t> flags;       // +2
    GuestInputGamepad gamepad;
    GuestInputVibration vibration;
};
static_assert(sizeof(GuestInputCapabilities) == 20, "XINPUT_CAPABILITIES is 20 bytes");

constexpr uint32_t kLocalPadIndex = 0;

// WHY subType 1 AND NOT 2, which is what one call site tests for. sub_825D7AC8 is
// the rumble path, and on an old kernel it looks for a device with subType == 2 and
// capability flags 0b11 — and when it finds one it passes the two motor speeds to
// XamInputSetState *swapped* (`lhz r11,2(r31)` stored at +0, `lhz r10,0(r31)` at
// +2). That is a workaround for one accessory whose motors are reversed. Reporting
// subType 2 would claim to be that accessory and get our rumble inverted, so a plain
// gamepad it is; the title then takes the ordinary path, which is correct for us.
static uint32_t XamInputGetCapabilities_x(uint32_t userIndex, uint32_t flags,
                                          GuestInputCapabilities* caps)
{
    (void)flags;   // 0 = any device, 1 = XINPUT_FLAG_GAMEPAD; A1 passes both
    if (!caps)
        return STATUS_INVALID_PARAMETER;
    memset(caps, 0, sizeof(*caps));
    if (userIndex != kLocalPadIndex)
        return ERROR_DEVICE_NOT_CONNECTED;
    caps->type = 1;      // XINPUT_DEVTYPE_GAMEPAD
    caps->subType = 1;   // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->flags = 0;     // no headset, no voice
    // A capabilities record reports which bits the device *can* produce, so the
    // gamepad and vibration members are all-ones on a real pad rather than zero.
    caps->gamepad.buttons = 0xFFFF;
    caps->gamepad.leftTrigger = 0xFF;
    caps->gamepad.rightTrigger = 0xFF;
    caps->gamepad.thumbLX = caps->gamepad.thumbLY = int16_t(0xFFC0);
    caps->gamepad.thumbRX = caps->gamepad.thumbRY = int16_t(0xFFC0);
    caps->vibration.leftMotor = 0xFF;
    caps->vibration.rightMotor = 0xFF;
    return 0;
}

constexpr uint16_t XINPUT_GAMEPAD_START = 0x0010;

// CZ_FAKE_START_MS=N — a synthetic START press every N milliseconds, held for 150 ms.
//
// THIS IS A MEASUREMENT ARM, NOT A FEATURE, AND IT MUST NEVER BE ON FOR A GATE RUN.
// Its whole purpose is to answer one question that nothing else can: when the boot
// settles at the title screen with the renderer running and no new files being
// opened, is it *finished and waiting for a human*, or is it stuck? Those two look
// identical from outside — same steady frame rate, same file count, same kernel-call
// profile — and the only difference is whether input makes it move.
//
// The danger is equally specific, which is why the injection logs every press: a
// fake button press MANUFACTURES progress. A run that quietly had this on would show
// the boot advancing past the title screen and invite the conclusion that some import
// we just wrote unblocked it. Nothing in this project may progress on the strength of
// a run whose input was invented, so it is loud, off by default, and named for what
// it is.
//
// The packet number is the other half. XInput's contract is that it changes only when
// the state changes, so a title can skip re-reading; a constant packet number with a
// changing button field would hand the guest a press it is entitled to ignore.
static uint32_t XamInputGetState_x(uint32_t userIndex, uint32_t flags,
                                   GuestInputState* state)
{
    (void)flags;
    if (!state)
        return STATUS_INVALID_PARAMETER;
    memset(state, 0, sizeof(*state));
    if (userIndex != kLocalPadIndex)
        return ERROR_DEVICE_NOT_CONNECTED;

    static const int fakeStartMs = []() {
        const char* e = getenv("CZ_FAKE_START_MS");
        const int ms = e ? atoi(e) : 0;
        if (ms > 0)
            KLOG("CZ_FAKE_START_MS=%d — SYNTHETIC INPUT IS ON. This run's progress is "
                 "not evidence about any import; do not gate on it.\n",
                 ms);
        return ms;
    }();

    // The real device, and it takes precedence over nothing: the synthetic arm below
    // is checked FIRST only when it is switched on, so an ordinary run cannot have
    // its input quietly overridden by a leftover environment variable. When both are
    // on the arm wins and says so on every press, which is the loudness gotcha 78 is
    // about.
    HostPadState pad{};
    if (fakeStartMs <= 0 && Host_PadState(pad))
    {
        state->packetNumber = pad.packet;
        state->gamepad.buttons = pad.buttons;
        state->gamepad.leftTrigger = pad.leftTrigger;
        state->gamepad.rightTrigger = pad.rightTrigger;
        state->gamepad.thumbLX = pad.thumbLX;
        state->gamepad.thumbLY = pad.thumbLY;
        state->gamepad.thumbRX = pad.thumbRX;
        state->gamepad.thumbRY = pad.thumbRY;
        return 0;
    }

    // Headless, and no synthetic input: a connected pad reporting nothing pressed,
    // forever. The packet number is then a constant because the state genuinely never
    // changes — which is the contract, not a shortcut.
    if (fakeStartMs <= 0)
    {
        state->packetNumber = 1;
        return 0;
    }

    // CZ_FAKE_PRESS_SEQ=START,A,A — the SEQUENCE the arm walks, one button per
    // interval, holding the last one thereafter. Unset means START every interval,
    // which is exactly what this arm did before and what every recipe written against
    // it expects.
    //
    // It exists because a screen you cannot reach without a human is a screen nobody
    // can measure (gotcha 103), and phase C part 11 hit that: the operator reported
    // black panels and malformed text on the new-game screen, which is two menu
    // levels past the title and therefore past everything CZ_FAKE_START_MS can reach.
    // A press of START alone lands on the logo screen and stops. The same rule as
    // before still governs it — this MANUFACTURES progress, so it is loud, off by
    // default, and never on for a gate run.
    // NONE is a real entry, not a gap: the sequence holds its last element forever,
    // so without it every recipe that reaches a screen KEEPS pressing at that screen
    // for the rest of the run. That makes "the title froze here" and "the title is
    // being poked every 8 s and cannot leave" indistinguishable — an arm that
    // manufactures progress needs a way to stop manufacturing it, or it has no
    // control of its own (gotcha 78).
    struct NamedButton { const char* name; uint16_t mask; };
    static constexpr NamedButton kButtons[] = {
        { "A", 0x1000 },        { "B", 0x2000 },     { "X", 0x4000 },
        { "Y", 0x8000 },        { "START", 0x0010 }, { "BACK", 0x0020 },
        { "UP", 0x0001 },       { "DOWN", 0x0002 },  { "LEFT", 0x0004 },
        { "RIGHT", 0x0008 },    { "NONE", 0x0000 },
    };
    static const std::vector<uint16_t> sequence = [] {
        std::vector<uint16_t> seq;
        const char* e = getenv("CZ_FAKE_PRESS_SEQ");
        if (!e || !*e)
            return seq;
        std::string all(e), one;
        for (size_t i = 0; i <= all.size(); i++)
        {
            if (i == all.size() || all[i] == ',')
            {
                for (const auto& b : kButtons)
                    if (one == b.name)
                        seq.push_back(b.mask);
                one.clear();
            }
            else if (!isspace(static_cast<unsigned char>(all[i])))
            {
                one.push_back(char(toupper(static_cast<unsigned char>(all[i]))));
            }
        }
        KLOG("CZ_FAKE_PRESS_SEQ: %zu synthetic presses queued, one per interval. "
             "SYNTHETIC INPUT IS ON — this run's progress is not evidence.\n",
             seq.size());
        return seq;
    }();

    static std::atomic<uint32_t> packet{ 1 };
    static std::atomic<bool> pressedNow{ false };
    // Measured from the first poll rather than from process start: the title only
    // starts polling once its frontend is up, so this keeps the delay meaningful
    // regardless of how long loading took.
    static const auto firstPoll = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - firstPoll)
                               .count();
    const bool press = elapsedMs > fakeStartMs && (elapsedMs % fakeStartMs) < 150;

    // Which button this interval belongs to. Interval 0 is the first press after the
    // initial delay; the sequence HOLDS its last entry rather than wrapping, because a
    // wrap would walk back out of the screen it was aimed at and the run would
    // oscillate between two menus with no way to tell from a frame dump.
    uint16_t button = XINPUT_GAMEPAD_START;
    const char* buttonName = "START";
    if (!sequence.empty())
    {
        const size_t interval = size_t(elapsedMs / fakeStartMs) - 1;
        const size_t idx = std::min(interval, sequence.size() - 1);
        button = sequence[idx];
        for (const auto& b : kButtons)
            if (b.mask == button)
                buttonName = b.name;
    }

    if (press != pressedNow.exchange(press))
    {
        const uint32_t n = packet.fetch_add(1) + 1;
        KLOG("CZ_FAKE_START_MS: synthetic %s %s at %llds (packet %u)\n", buttonName,
             press ? "DOWN" : "up", static_cast<long long>(elapsedMs / 1000), n);
    }
    state->packetNumber = packet.load();
    state->gamepad.buttons = press ? button : 0;
    return 0;
}

// Rumble. Accepted and discarded: there is no motor, and reporting failure would
// send sub_825D7AC8's callers down an error path over an effect that does not
// matter. The values are logged so the eventual input layer has a witness that the
// title does drive them.
static uint32_t XamInputSetState_x(uint32_t userIndex, uint32_t unk,
                                   GuestInputVibration* vibration)
{
    (void)unk;
    if (userIndex != kLocalPadIndex)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (vibration)
        KLOG("XamInputSetState(user=%u, motors %u/%u)\n", userIndex,
             vibration->leftMotor.get(), vibration->rightMotor.get());
    return 0;
}

// No keyboard is attached, and ERROR_EMPTY is the defined way to say "no keystroke
// is queued" — not a failure, the normal answer on a console with no chatpad.
struct GuestInputKeystroke    // 8 bytes
{
    be<uint16_t> virtualKey;
    be<uint16_t> unicode;
    be<uint16_t> flags;
    uint8_t userIndex;
    uint8_t hidCode;
};

static uint32_t XamInputGetKeystrokeEx_x(be<uint32_t>* userIndex, uint32_t flags,
                                         GuestInputKeystroke* keystroke)
{
    (void)flags;
    if (keystroke)
        memset(keystroke, 0, sizeof(*keystroke));
    if (userIndex)
        *userIndex = kLocalPadIndex;
    return ERROR_EMPTY;
}

GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, XamInputGetCapabilities_x)
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, XamInputGetState_x)
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, XamInputSetState_x)
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystrokeEx, XamInputGetKeystrokeEx_x)

// ---------------------------------------------------------------------------
// The content licence — finding 1, arriving in the runtime
// ---------------------------------------------------------------------------
//
// XamContentGetLicenseMask(maskPtr, overlapped) is the import behind the phase gate's
// oldest open divergence, and it took three wrong guesses to find because nothing
// about it looks like a licence question from the outside.
//
// WHAT IT DECIDED. `sub_82788F48` calls it through a ONE-INSTRUCTION tail-call thunk
// (`sub_825D7A50` is literally `b XamContentGetLicenseMask`), then branches on the
// STATUS, not the mask:
//
//     sub_825D7A50()            // = XamContentGetLicenseMask(r1+112, 0)
//     cmplwi r3,0
//     beq    loc_82789134       // success -> skip everything below
//     ... sub_82829098 ...      // failure -> go build a DVD cache
//
// As a generated honest-failure stub it returned 0xC0000002, so we took the failure
// branch and went looking for a disc: `\Device\Image`, then
// `\Device\Harddisk0\partition0`, then a string comparison against "cdrom0:" that
// showed up in the gate as `RtlCompareStringN` at position 57. Every one of those was
// a *symptom*. The subsystem the trail pointed at had nothing to do with the cause.
//
// WHY IT WAS INVISIBLE. `XamContentGetLicenseMask` is `kHighFrequency`, so it appears
// **nowhere in A1** — it is only in A5, three calls, which is how the argument shape
// and the null overlapped were confirmed. Gotcha 47 again: a capture set needs a
// high-frequency arm or its quietest exports are unfalsifiable.
//
// WHY THE MASK IS 1. A second call site decides the trial-versus-full question with
// the mask's *value*, at 0x8250191C:
//
//     r11 = [r1+80]                    // the mask we wrote
//     r9  = (mask == 0)                // cntlzw/rlwinm
//     [0x82505FFE] &= r9               // a global byte, cleared when the mask is set
//
// so a zero mask leaves that flag standing and a non-zero mask clears it. That is
// findings-ledger finding 1 from the other side: Xenia's `license_mask` defaults to 0
// and boots the **trial**, and both A1 and A5 were captured with `license_mask = 1`.
// Our package is the full game, so bit 0 — "this content is purchased" — is set. This
// is the one place in the runtime where getting the value wrong would silently boot a
// different game.
static uint32_t XamContentGetLicenseMask_x(be<uint32_t>* mask, uint32_t overlapped)
{
    if (!mask)
        return STATUS_INVALID_PARAMETER;
    *mask = 1;
    if (overlapped)
        CompleteOverlapped(
            reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped)), 0, 4);
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XamContentGetLicenseMask, XamContentGetLicenseMask_x)

// ---------------------------------------------------------------------------
// XAM app messages, tasks, and the storage device
// ---------------------------------------------------------------------------
//
// This is the block at gate positions 82-93, and it hangs together as one mechanism
// rather than as a list of imports, which is why it is written in one place:
//
//   XMsgStartIORequest(app, message, overlapped, buffer, len)  routes a message to
//       an XAM "application" (0xFA media player, 0xFB game invite/context, 0xFC Live
//       base). It is a DISPATCHER, so a blanket error from it fails every XAM
//       message the title will ever send — which is what a generated stub did.
//   XamTaskSchedule(callback, context, 0, handleOut)           runs a GUEST function
//       on a XAM worker thread. Not bookkeeping: `XamTaskSchedule(825D9358, ...)` in
//       A1 schedules `sub_825D9358`, and that function is the only caller of
//       XMsgCompleteIORequest in the image. Stub the scheduler and the title's async
//       operations are started and never finished — gotcha 46's shape exactly.
//   XMsgCompleteIORequest / XamGetOverlappedResult              the two ends of the
//       overlapped that the scheduled callback completes.
//
// The title pre-arms the overlapped itself before scheduling — `[r29+0] = 997` and
// `[r29+8] = task block` in sub_825D91E0 — so our side owes only the completion.

constexpr uint32_t ERROR_FUNCTION_FAILED = 1627;      // what the guest reports upward
constexpr uint32_t E_FAIL                = 0x80004005;

// Every call site tests the result with a SIGNED compare (`cmpwi r3,0; bge ok`), so a
// failure has to be negative. 0x80004005 is; a positive Win32 code like 1627 would be
// read as success and the title would go on to use an overlapped nobody filled.
constexpr uint32_t kAppXmp       = 0xFA;   // media player
constexpr uint32_t kAppXgi       = 0xFB;   // game invite / user context & properties
constexpr uint32_t kAppXLiveBase = 0xFC;   // Live base services
constexpr uint32_t kAppUser      = 0xFE;   // A1 shows XMsgInProcessCall(FE, 0002000E)

// XGI 0x000B0006 — XamUserSetContext. The layout is read off `sub_825D7D20`, which
// builds the 24-byte buffer field by field before the call:
//     [r1+80]  = r3   user index      -> buffer +0
//     [r1+88]  = 0    (std, 8 bytes)  -> buffer +8
//     [r1+96]  = r4   context id      -> buffer +16
//     [r1+100] = r5   context value   -> buffer +20
// and the buffer it passes is r1+80 with length 24.
struct GuestXgiUserContext
{
    be<uint32_t> userIndex;   // +0
    be<uint32_t> unused;      // +4  never written by the guest
    be<uint64_t> reserved;    // +8  written as a zero doubleword
    be<uint32_t> contextId;   // +16
    be<uint32_t> contextValue;// +20
};
static_assert(sizeof(GuestXgiUserContext) == 24, "sub_825D7D20 passes length 24");

// Presence contexts are per-user key/value bookkeeping — the system remembers what
// the title last said it was doing. Storing them is a real implementation, not a
// faked success: nothing here is being sent anywhere, and nothing needs to be.
static std::map<uint64_t, uint32_t> g_userContexts;

// Route one (app, message) pair. Anything not handled returns E_FAIL and says so
// ONCE, naming the pair — because the useful output of a dispatcher we have not
// finished is a list of exactly what to write next.
//
// Recovered statically from the image (replaying the r3/r4 setup at all 18 call
// sites), the full surface this title can send is:
//   XGI  0xFB: 000B0006 0007 0008 0010 0011 0012 0013 0014 0015 001B 001C 001D 001E
//              0021 0025 0026
//   XLB  0xFC: 00000000 00058004 00058006 0005800E 00058020 00058023
//   XMP  0xFA: 00070009 0007001B
// Every one of those past 000B0008 is Xbox Live session, matchmaking, presence or
// media-player work that this runtime has no way to perform, so failing them is the
// honest answer rather than a gap. A1 only ever sends 000B0006 during boot.
static uint32_t DispatchAppMessage(uint32_t app, uint32_t message, void* buffer,
                                   uint32_t bufferLength)
{
    if (app == kAppXgi && message == 0x000B0006)
    {
        if (!buffer || bufferLength < sizeof(GuestXgiUserContext))
            return E_FAIL;
        auto* msg = static_cast<GuestXgiUserContext*>(buffer);
        const uint32_t user = msg->userIndex.get();
        const uint32_t id = msg->contextId.get();
        g_userContexts[(uint64_t(user) << 32) | id] = msg->contextValue.get();
        KLOG("XGI user %u context %04X = %u\n", user, id, msg->contextValue.get());
        return 0;
    }

    // The content layer owns (0xFE, 0x0002000E), the enumeration step. It lives in
    // content.cpp because the protocol behind it is a page of derivation from the
    // guest's own XamEnumerate wrapper, not because it is a different kind of message.
    uint32_t contentResult = 0;
    if (ContentDispatchAppMessage(app, message, buffer, bufferLength, &contentResult))
        return contentResult;

    static std::map<uint64_t, int> seen;
    const uint64_t key = (uint64_t(app) << 32) | message;
    if (seen.find(key) == seen.end())
    {
        seen[key] = 1;
        fprintf(stderr, "[xam] no handler for app %02X message %08X (%u-byte buffer) — "
                        "returning E_FAIL\n", app, message, bufferLength);
    }
    return E_FAIL;
}

// The async form. When the caller supplies an overlapped the status travels in the
// overlapped and the return says only "the request was accepted" — which is why a
// failed message still returns 0 here. The guest's own wrapper then reports
// ERROR_IO_PENDING (997) upward and reads the real answer later.
static uint32_t XMsgStartIORequest_x(uint32_t app, uint32_t message, uint32_t overlapped,
                                     void* buffer, uint32_t bufferLength)
{
    const uint32_t result = DispatchAppMessage(app, message, buffer, bufferLength);
    if (!overlapped)
        return result;
    CompleteOverlapped(reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped)),
                       result, bufferLength);
    return 0;
}

// The synchronous form: no overlapped, the status is the return value.
static uint32_t XMsgInProcessCall_x(uint32_t app, uint32_t message, void* buffer,
                                    uint32_t unused)
{
    (void)unused;
    return DispatchAppMessage(app, message, buffer, 0);
}

// XMsgCompleteIORequest(overlapped, result, extendedError, length) — the title
// finishing its OWN overlapped from inside a scheduled task. A1:
//   XMsgCompleteIORequest(7018F3C0, 0000065B, 80070012, 00000000)
// i.e. result 1627 (ERROR_FUNCTION_FAILED) with the HRESULT for ERROR_NO_MORE_FILES
// in the extended slot — so the extended error is meaningful here and must be written
// through rather than zeroed the way CompleteOverlapped() does it.
static void XMsgCompleteIORequest_x(uint32_t overlapped, uint32_t result,
                                    uint32_t extendedError, uint32_t length)
{
    if (!overlapped)
        return;
    auto* ovl = reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped));
    ovl->result = result;
    ovl->length = length;
    ovl->extendedError = extendedError;
    SignalGuestEvent(ovl->event);

    // The first few completions, printed, because this is the one line in the whole
    // async surface that A1 gives us verbatim to compare against:
    //     XMsgCompleteIORequest(7018F3C0, 0000065B, 80070012, 00000000)
    // i.e. ERROR_FUNCTION_FAILED with ERROR_NO_MORE_FILES extended — the empty save
    // enumeration. Without this the difference between "the title accepted our
    // enumerated item" and "the title filtered it out and ran off the end" is
    // invisible: both produce exactly one enumerate step and then a completion.
    static std::atomic<int> completions{ 0 };
    if (completions.fetch_add(1) < 8)
        KLOG("XMsgCompleteIORequest(%08X, result=%u, extended=%08X, length=%u)\n", overlapped,
             result, extendedError, length);
}

// XamGetOverlappedResult(overlapped, lengthOut, wait) -> the stored result.
//
// The wait is real, because the work is on another thread now (see XamTaskSchedule).
// It is bounded and complains rather than hanging silently: an overlapped that never
// completes is a bug in whatever was supposed to complete it, and a runtime that
// blocks forever hides which one.
static uint32_t XamGetOverlappedResult_x(uint32_t overlapped, be<uint32_t>* lengthOut,
                                         uint32_t wait)
{
    if (!overlapped)
        return STATUS_INVALID_PARAMETER;
    auto* ovl = reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped));

    if (wait && ovl->result.get() == ERROR_IO_PENDING)
    {
        constexpr int kMaxWaitMs = 10000;
        int waited = 0;
        while (ovl->result.get() == ERROR_IO_PENDING && waited < kMaxWaitMs)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waited++;
        }
        if (ovl->result.get() == ERROR_IO_PENDING)
            fprintf(stderr, "[xam] overlapped %08X still pending after %d ms — nothing "
                            "completed it\n", overlapped, kMaxWaitMs);
    }

    if (lengthOut)
        *lengthOut = ovl->length;
    return ovl->result;
}

// XamTaskSchedule(callback, context, 0, handleOut) — run a guest function on a XAM
// worker thread.
//
// The argument shape is off the guest, not the SDK: sub_825D91E0 sets r3 = the
// callback, r4 = the task block, r5 = 0, r6 = block+24 for the handle, and A1 agrees
// — XamTaskSchedule(825D9358, 300E9000, 00000000, 300E9018), where 300E9018 is
// 300E9000+0x18. The callback then reads its context straight out of r3
// (`lwz r30,12(r3)` is sub_825D9358's first real instruction), which is what settles
// that argument 2 is the callback's context and not a task-parameters struct.
//
// The stack is 0x8000, matching what this title gives its own worker threads through
// ExCreateThread, rather than the XEX's 0x40000 default — the callbacks here are
// leaf-ish work items and fifteen of them at a quarter-megabyte each would be a
// meaningful slice of the address space.
static uint32_t XamTaskSchedule_x(uint32_t callback, uint32_t context, uint32_t unused,
                                  be<uint32_t>* handleOut)
{
    (void)unused;   // 0 at both call sites in the image
    if (!callback)
    {
        if (handleOut)
            *handleOut = 0;
        return E_FAIL;
    }

    GuestThreadParams params{};
    params.function = callback;
    params.arg0 = context;
    params.stackSize = 0x8000;

    uint32_t threadId = 0;
    GuestThreadHandle* handle = GuestThread::Start(params, &threadId);
    if (!handle)
    {
        if (handleOut)
            *handleOut = 0;
        return E_FAIL;
    }
    KLOG("XamTaskSchedule callback=%08X context=%08X -> tid=0x%X\n", callback, context,
         threadId);
    if (handleOut)
        *handleOut = GetKernelHandle(handle);
    return 0;
}

// Closing a task handle does not stop the task — GuestThreadHandle's destructor
// detaches rather than joining, which is the NT thread-handle semantic the rest of
// this kernel already follows.
static uint32_t XamTaskCloseHandle_x(uint32_t handle)
{
    if (handle && IsKernelObject(handle))
        DestroyKernelObject(handle);
    return 0;
}

// --- the storage device -----------------------------------------------------
//
// POLICY, and it is the same shape as the single local user and the single pad:
// exactly one storage device, always present, and the selector picks it without
// showing UI. The device id is not invented — A1 shows the selector's answer being
// used immediately afterwards as `XamContentGetDeviceData(00000001, ...)`.
constexpr uint32_t kStorageDeviceId = 1;

// XDEVICE_DATA. The size is stated by the guest: sub_825D3648 zeroes `[r1+80]` and
// then memsets `r1+84` for 76 bytes, i.e. an 80-byte structure at r1+80. The one
// field it reads is the doubleword at `[r1+96]` = offset +16, which it compares
// against a required byte count — so +16 is free space, and total therefore sits at
// +8 where an 8-aligned pair puts it.
struct GuestDeviceData
{
    be<uint32_t> deviceId;      // +0
    be<uint32_t> deviceType;    // +4   1 = hard disk
    be<uint64_t> totalBytes;    // +8
    be<uint64_t> freeBytes;     // +16  the only field this title reads
    char16_t name[28];          // +24
};
static_assert(sizeof(GuestDeviceData) == 80, "the guest memsets 4 + 76 bytes");

// NOT MEASURED, and flagged as such. Nothing checks these against the host
// filesystem, and the honest fix once saves are actually written is to report the
// real free space of whatever directory the VFS maps. Until then the numbers only
// have to be large enough that the title's "is there room" test passes, and small
// enough to be a plausible console hard disk.
constexpr uint64_t kDeviceTotalBytes = 16ull * 1024 * 1024 * 1024;
constexpr uint64_t kDeviceFreeBytes  = 8ull * 1024 * 1024 * 1024;

static uint32_t XamContentGetDeviceData_x(uint32_t deviceId, GuestDeviceData* data)
{
    if (!data)
        return STATUS_INVALID_PARAMETER;
    memset(data, 0, sizeof(*data));
    if (deviceId != kStorageDeviceId)
        return ERROR_DEVICE_NOT_CONNECTED;   // 1167, which sub_825D3648 tests for by name
    data->deviceId = kStorageDeviceId;
    data->deviceType = 1;                    // hard disk
    data->totalBytes = kDeviceTotalBytes;
    data->freeBytes = kDeviceFreeBytes;
    const char16_t* name = u"HDD";
    for (int i = 0; name[i] && i < 27; i++)
        data->name[i] = be<uint16_t>(uint16_t(name[i])).get();
    return 0;
}

static uint32_t XamContentGetDeviceState_x(uint32_t deviceId, uint32_t overlapped)
{
    const uint32_t result = deviceId == kStorageDeviceId ? 0u : ERROR_DEVICE_NOT_CONNECTED;
    if (overlapped)
    {
        CompleteOverlapped(
            reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped)), result, 0);
        return ERROR_IO_PENDING;
    }
    return result;
}

// XamShowDeviceSelectorUI(user, contentType, contentFlags, totalRequested, deviceIdOut,
//                         overlapped). A1: (0, 1, 0, 0, 82A5C348, ...) — content type 1
// is SAVEDGAME, and 82A5C348 is a global the title reads back later as the device id
// it hands to XamContentGetDeviceData.
//
// There is one device and no UI to show, so this answers immediately. Showing nothing
// is the truthful behaviour of a system with a single storage device; it is not a
// stand-in for a dialog we have not built.
static uint32_t XamShowDeviceSelectorUI_x(uint32_t userIndex, uint32_t contentType,
                                          uint32_t contentFlags, uint64_t totalRequested,
                                          be<uint32_t>* deviceIdOut, uint32_t overlapped)
{
    (void)userIndex;
    (void)contentType;
    (void)contentFlags;
    (void)totalRequested;
    if (deviceIdOut)
        *deviceIdOut = kStorageDeviceId;
    if (overlapped)
    {
        CompleteOverlapped(
            reinterpret_cast<GuestOverlapped*>(g_memory.Translate(overlapped)), 0, 0);
        return ERROR_IO_PENDING;
    }
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest_x)
GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall_x)
GUEST_FUNCTION_HOOK(__imp__XMsgCompleteIORequest, XMsgCompleteIORequest_x)
GUEST_FUNCTION_HOOK(__imp__XamGetOverlappedResult, XamGetOverlappedResult_x)
GUEST_FUNCTION_HOOK(__imp__XamTaskSchedule, XamTaskSchedule_x)
GUEST_FUNCTION_HOOK(__imp__XamTaskCloseHandle, XamTaskCloseHandle_x)
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceData, XamContentGetDeviceData_x)
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceState, XamContentGetDeviceState_x)
GUEST_FUNCTION_HOOK(__imp__XamShowDeviceSelectorUI, XamShowDeviceSelectorUI_x)
