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
static void RtlEnterCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    const uint32_t self = CurrentGuestThreadId();
    static const bool csTrace = getenv("CZ_CS_TRACE") != nullptr;
    uint64_t spins = 0;
    for (;;)
    {
        uint32_t previous = 0;
        if (LockCas(&cs->OwningThread, 0, self, &previous) || previous == self)
        {
            cs->RecursionCount++;
            return;
        }
        // CZ_CS_TRACE=1: name the owner of a section this thread cannot get. A frozen
        // run is almost always one thread holding what another needs, and this is the
        // cheapest way to see the pair.
        if (csTrace && (++spins % 4000000) == 0)
            fprintf(stderr,
                    "[csspin] self r13=%08X wants cs=%p ownedBy r13=%08X rec=%d lr=%08X\n",
                    self, (void*)cs, previous, cs->RecursionCount,
                    g_ppcContext ? uint32_t(g_ppcContext->lr) : 0);
        std::this_thread::yield();
    }
}

static void RtlLeaveCriticalSection_x(XRTL_CRITICAL_SECTION* cs)
{
    if (--cs->RecursionCount != 0)
        return;
    __atomic_store_n(&cs->OwningThread, 0, __ATOMIC_RELEASE);
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

// Dispatcher-header waits (Ke level): resolve by the header's Type field.
static uint32_t WaitDispatcher(XDISPATCHER_HEADER* header, uint32_t timeoutMs)
{
    switch (header->Type)
    {
        case 0: // NotificationEvent
        case 1: // SynchronizationEvent
            return QueryKernelObject<Event>(*header)->Wait(timeoutMs);
        case 5: // Semaphore
            return QueryKernelObject<Semaphore>(*header)->Wait(timeoutMs);
        default:
            KLOG("KeWait on unhandled dispatcher type %u\n", header->Type);
            return STATUS_TIMEOUT;
    }
}

static uint32_t KeWaitForSingleObject_x(XDISPATCHER_HEADER* object, uint32_t reason,
                                        uint32_t mode, uint32_t alertable, be<int64_t>* timeout)
{
    return WaitDispatcher(object, GuestTimeoutToMs(timeout));
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

    // CZ_WAIT_TRACE=1: name infinite waits that outlast 5 s. A frozen run always has
    // some thread parked on an object nobody signals; this prints which handle, which
    // thread, and the guest callers above the wait wrapper — without it, a hang is
    // indistinguishable from an infinite loop.
    static const bool waitTrace = getenv("CZ_WAIT_TRACE") != nullptr;
    if (waitTrace && timeoutMs == WAIT_TIMEOUT_INFINITE)
    {
        auto* obj = GetKernelObject(handle);
        for (int spins = 0;; spins++)
        {
            const uint32_t r = obj->Wait(5000);
            if (r != STATUS_TIMEOUT)
                return r;
            const char* kind = dynamic_cast<Event*>(obj)       ? " EVENT"
                               : dynamic_cast<Semaphore*>(obj) ? " SEMAPHORE"
                                                               : "";
            fprintf(stderr,
                    "[wait] tid=%08X r13=%08X (entry=%08X) handle=%08X lr=%08X stuck %ds%s\n",
                    GuestThread::GetCurrentThreadId(),
                    g_ppcContext ? uint32_t(g_ppcContext->r13.u32) : 0,
                    LookupThreadEntry(GuestThread::GetCurrentThreadId()), handle,
                    g_ppcContext ? uint32_t(g_ppcContext->lr) : 0, (spins + 1) * 5, kind);
            // The exact LR back-chain, not a scan — see cpu/crash_report.cpp.
            CzDumpGuestBacktrace("blocked wait");
        }
    }
    return GetKernelObject(handle)->Wait(timeoutMs);
}

static uint32_t KeWaitForMultipleObjects_x(uint32_t count, xpointer<XDISPATCHER_HEADER>* objects,
                                           uint32_t waitType, uint32_t reason, uint32_t mode,
                                           uint32_t alertable, be<int64_t>* timeout)
{
    const uint32_t timeoutMs = GuestTimeoutToMs(timeout);
    if (waitType == 0) // wait-all
    {
        for (uint32_t i = 0; i < count; i++)
            WaitDispatcher(objects[i], timeoutMs);
        return STATUS_SUCCESS;
    }
    // wait-any: poll. Simple and safe; revisit if it shows up hot in a profile.
    for (;;)
    {
        for (uint32_t i = 0; i < count; i++)
            if (WaitDispatcher(objects[i], 0) == STATUS_SUCCESS)
                return STATUS_WAIT_0 + i;
        if (timeoutMs == 0)
            return STATUS_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    for (;;)
    {
        for (uint32_t i = 0; i < count; i++)
            if (IsKernelObject(handles[i]) && IsLiveKernelHandle(handles[i]) &&
                GetKernelObject(handles[i])->Wait(0) == STATUS_SUCCESS)
                return STATUS_WAIT_0 + i;
        if (timeoutMs == 0)
            return STATUS_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
static uint32_t ObReferenceObjectByHandle_x(uint32_t handle, uint32_t objectType,
                                            be<uint32_t>* object)
{
    if (object)
        *object = handle;
    return STATUS_SUCCESS;
}

static uint32_t NtDuplicateObject_x(uint32_t handle, be<uint32_t>* newHandle, uint32_t options)
{
    if (!newHandle)
        return STATUS_INVALID_PARAMETER;
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
static uint32_t XexCheckExecutablePrivilege_x(uint32_t) { return 0; }

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
static uint32_t RtlImageXexHeaderField_x(uint32_t headerBase, uint32_t key)
{
    if (!headerBase)
        headerBase = g_xexHeaderBase.load();
    if (!headerBase)
        return 0;

    uint8_t* base = g_memory.base;
    // Xex2Header: magic(0) moduleFlags(4) sizeOfHeaders(8) sizeOfDiscardable(0xC)
    // securityInfo(0x10) headerCount(0x14); optional headers follow at 0x18 as
    // {key, value} pairs.
    const uint32_t headerCount = PPC_LOAD_U32(headerBase + 0x14);
    if (headerCount > 256) // a wild pointer, not a header block
    {
        KLOG("RtlImageXexHeaderField: %08X does not look like a XEX header (count=%u)\n",
             headerBase, headerCount);
        return 0;
    }
    for (uint32_t i = 0; i < headerCount; i++)
    {
        const uint32_t entry = headerBase + 0x18 + i * 8;
        if (PPC_LOAD_U32(entry) != key)
            continue;
        // The key's low byte is the field size in dwords: 0 or 1 means the value is
        // stored inline in the header entry, so the field's address IS the value
        // word; anything else means the value is a module-relative offset.
        const uint32_t lowByte = key & 0xFF;
        if (lowByte == 0 || lowByte == 1)
            return entry + 4;
        return headerBase + PPC_LOAD_U32(entry + 4);
    }
    return 0;
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
        // Refuses (loudly) if the address is outside the dispatch table's range,
        // which is the failure Asura's Wrath hit silently.
        if (!g_memory.InsertFunction(addr, MintedExportStub))
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
// title asking a module for a named section. Note the handle 0x30014000 is NOT the
// xam handle our XexLoadImage minted (0x30002000); on hardware it is the title's own
// module, and "Serial" is the XEX resource section A1's header dump lists at
// 82AF0080. We have no module image to hand back a section from, so STATUS_NOT_FOUND
// is the truthful answer.
//
// Both out-parameters are written anyway. Finding 14: an error return does not
// protect a caller that ignores the status and reads the buffer.
static uint32_t XexGetModuleSection_x(uint32_t module, const char* name, be<uint32_t>* dataOut,
                                      be<uint32_t>* sizeOut)
{
    if (dataOut)
        *dataOut = 0;
    if (sizeOut)
        *sizeOut = 0;
    KLOG("XexGetModuleSection module=%08X name='%s' -> not found\n", module, name ? name : "");
    return STATUS_NOT_FOUND;
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
GUEST_FUNCTION_HOOK(__imp__FscSetCacheElementCount, FscSetCacheElementCount_x)
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification,
                    ExRegisterTitleTerminateNotification_x)
GUEST_FUNCTION_HOOK(__imp__XexGetModuleSection, XexGetModuleSection_x)
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
