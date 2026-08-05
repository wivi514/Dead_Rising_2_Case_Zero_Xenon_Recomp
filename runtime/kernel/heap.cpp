#include "heap.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "memory.h"

GuestHeap g_heap;

// Guest address ranges (ends exclusive). Checked against Case Zero's own numbers
// rather than inherited: PPC_IMAGE_BASE 0x82000000 + PPC_IMAGE_SIZE 0x00B40000 puts
// the image end (and the start of the indirect-dispatch table) at 0x82B40000, and
// the table spans PPC_CODE_SIZE * 2 = 0x010E6AC8 bytes, ending at 0x83C26AC8. The
// host/user heap therefore starts at 0x88000000, clear of it by 68 MB. The physical
// arena lives in the CACHED view (0xA0000000) of the 512 MB physical memory;
// 0xC0000000/0xE0000000 alias the same pages (memory.cpp), so it must not extend
// past 0xC0000000.
//
// The two virtual arenas deliberately sit where the console (and Xenia) put them,
// so an address in one of our logs can be compared against an address in a capture
// with no translation step. See the header for the A1 measurement behind the split.
constexpr size_t kSmallBase = 0x00010000; // 4 KB-page virtual arena
constexpr size_t kSmallEnd  = 0x40000000;
constexpr size_t kLargeBase = 0x40000000; // 64 KB-page arena + explicit reservations
constexpr size_t kLargeEnd  = 0x7FE00000;
constexpr size_t kUserBase  = 0x88000000; // host-side allocations (o1heap)
constexpr size_t kUserEnd   = 0x9FF00000;
constexpr size_t kPhysBase  = 0xA0000000;
constexpr size_t kPhysEnd   = 0xBFFF0000;

// Recomputed from this image's constants, not hardcoded — if the function list ever
// changes PPC_CODE_SIZE, this is what notices the table has grown into the heap.
static_assert(kUserBase >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2,
              "user heap must start past the indirect-dispatch function table");

// Games hand back pointers through any of the three physical views; fold them onto
// the cached view the arena actually manages.
static void* NormalizePhysical(const void* ptr)
{
    size_t guest = static_cast<const uint8_t*>(ptr) - g_memory.base;
    if (guest >= 0xC0000000ull && guest < 0x100000000ull)
        guest = 0xA0000000ull + (guest & 0x1FFFFFFF);
    return g_memory.base + guest;
}

// The GPU register file is memory-mapped into the title's address space at
// 0x7FC80000 on the 360 — the D3D driver kicks the command ring by storing the
// write pointer straight to CP_RB_WPTR at 0x7FC80714, and Xenia registers its MMIO
// handler over the same range. That is a console fact, not a per-title one, and the
// aperture falls inside the large-page arena's span, so it has to be withheld from
// the allocator: a virtual allocation landing there would be silently overwritten
// by the driver's next ring kick, and the corruption would appear in whatever
// unrelated structure happened to be sitting there. Phase 4 will own these
// constants; the hole has to exist before then or phase 4 inherits a moving arena.
constexpr size_t kGpuRegisterBase = 0x7FC80000;
constexpr size_t kGpuRegisterSize = 0x00010000;
static_assert(kGpuRegisterBase >= kLargeBase && kGpuRegisterBase + kGpuRegisterSize <= kLargeEnd,
              "the GPU register aperture is expected to fall inside the large-page arena");

void GuestHeap::Init()
{
    heap = o1heapInit(g_memory.Translate(kUserBase), kUserEnd - kUserBase);
    assert(heap);
    physFree.emplace(kPhysBase, kPhysEnd - kPhysBase);
    smallFree.emplace(kSmallBase, kSmallEnd - kSmallBase);
    largeFree.emplace(kLargeBase, kGpuRegisterBase - kLargeBase);
    largeFree.emplace(kGpuRegisterBase + kGpuRegisterSize,
                      kLargeEnd - kGpuRegisterBase - kGpuRegisterSize);
}

// Shared first-fit range allocation with alignment. Returns 0 on exhaustion.
//
// `topDown` walks the free map backwards and places the block at the END of the
// first block that fits, which is how a runtime-owned allocation gets out of the
// guest's way. It matters because the addresses this arena hands out are observable
// to the title: it asks for 447 MB of the console's 512 MB up front, and anything we
// take from the low end first moves that block. Xenia puts its own XMA context array
// immediately ABOVE the title's reservation for the same reason (A1: the title gets
// physical 0x03D93000 + 0x1BF16000, and the context array sits at 0x1FCAA000, one
// page past its end).
static uint32_t RangeAlloc(std::map<uint32_t, uint32_t>& freeMap,
                           std::unordered_map<uint32_t, uint32_t>& usedMap,
                           uint32_t need, uint32_t align, bool topDown = false)
{
    // Carve `need` bytes out of one free block, or return 0 if it does not fit.
    // Erases `it` on success, so every caller returns immediately afterwards.
    auto carve = [&](std::map<uint32_t, uint32_t>::iterator it) -> uint32_t {
        const uint32_t blockAddr = it->first, blockSize = it->second;
        uint32_t aligned = (blockAddr + align - 1) & ~(align - 1);
        if (topDown && blockSize >= need)
        {
            // The highest aligned start that still leaves `need` bytes inside the
            // block. Guarded by `high >= aligned` because rounding an already
            // tight fit downward can land below the block.
            const uint32_t high = (blockAddr + blockSize - need) & ~(align - 1);
            if (high >= aligned)
                aligned = high;
        }
        if (uint64_t(aligned) + need > uint64_t(blockAddr) + blockSize)
            return 0;
        freeMap.erase(it);
        if (aligned > blockAddr)
            freeMap.emplace(blockAddr, aligned - blockAddr);
        if (aligned + need < blockAddr + blockSize)
            freeMap.emplace(aligned + need, blockAddr + blockSize - aligned - need);
        usedMap.emplace(aligned, need);
        return aligned;
    };

    if (topDown)
    {
        // rit.base() is one PAST rit's element in forward order, so the element
        // itself is std::prev(rit.base()).
        for (auto rit = freeMap.rbegin(); rit != freeMap.rend(); ++rit)
            if (const uint32_t addr = carve(std::prev(rit.base())))
                return addr;
    }
    else
    {
        for (auto it = freeMap.begin(); it != freeMap.end(); ++it)
            if (const uint32_t addr = carve(it))
                return addr;
    }
    return 0;
}

// Shared free-with-coalesce for the range arenas. Returns false if addr unknown.
static bool RangeFree(std::map<uint32_t, uint32_t>& freeMap,
                      std::unordered_map<uint32_t, uint32_t>& usedMap, uint32_t guest)
{
    auto it = usedMap.find(guest);
    if (it == usedMap.end())
        return false;
    uint32_t addr = it->first, size = it->second;
    usedMap.erase(it);
    // Coalesce forward then backward, so a long-running session does not shred the
    // arena into unusable slivers.
    auto next = freeMap.lower_bound(addr);
    if (next != freeMap.end() && addr + size == next->first)
    {
        size += next->second;
        next = freeMap.erase(next);
    }
    if (next != freeMap.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr)
        {
            addr = prev->first;
            size += prev->second;
            freeMap.erase(prev);
        }
    }
    freeMap.emplace(addr, size);
    return true;
}

// Dump the largest live blocks of an arena once it fails. "Out of memory" alone
// cannot distinguish a leak from fragmentation from a genuinely tight budget, and
// each of those has a different fix.
static void ReportExhaustion(const char* what, size_t size,
                             const std::map<uint32_t, uint32_t>& freeMap,
                             const std::unordered_map<uint32_t, uint32_t>& usedMap)
{
    size_t used = 0, freeTotal = 0, freeMax = 0;
    for (const auto& [a, s] : usedMap)
        used += s;
    for (const auto& [a, s] : freeMap)
    {
        freeTotal += s;
        freeMax = std::max<size_t>(freeMax, s);
    }
    fprintf(stderr,
            "[heap] %s arena exhausted allocating %zu bytes: %zu blocks / %zu MB used, "
            "%zu MB free (largest %zu MB)\n",
            what, size, usedMap.size(), used >> 20, freeTotal >> 20, freeMax >> 20);
    std::vector<std::pair<uint32_t, uint32_t>> top(usedMap.begin(), usedMap.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < top.size() && i < 16; ++i)
        fprintf(stderr, "[heap]   %s top[%zu] guest=%08X size=%u KB\n", what, i,
                top[i].first, top[i].second >> 10);
}

void* GuestHeap::AllocVirtual(size_t size, bool largePages)
{
    size = std::max<size_t>(1, size);
    // 64 KB granularity in both arenas: the Xbox 360's virtual *allocation*
    // granularity, which is independent of the page size the request asks for.
    // Games' allocators align addresses down by it to find their page-pool headers,
    // so a finer granularity here would hand out addresses whose rounded-down base
    // belongs to somebody else's block.
    const uint32_t need = static_cast<uint32_t>((size + 0xFFFF) & ~size_t(0xFFFF));
    std::lock_guard lock(rangeMutex);
    uint32_t addr = largePages ? RangeAlloc(largeFree, largeUsed, need, 0x10000)
                               : RangeAlloc(smallFree, smallUsed, need, 0x10000);
    // Overflow into the other arena rather than failing: a base==0 caller has told
    // us its preferred page size, not a requirement about where the pages live, and
    // running out of one region while the other is empty would be a self-inflicted
    // out-of-memory.
    if (!addr)
        addr = largePages ? RangeAlloc(smallFree, smallUsed, need, 0x10000)
                          : RangeAlloc(largeFree, largeUsed, need, 0x10000);
    if (!addr)
    {
        static int shown = 0;
        if (shown++ < 8)
        {
            ReportExhaustion("small-page", size, smallFree, smallUsed);
            ReportExhaustion("large-page", size, largeFree, largeUsed);
        }
        return nullptr;
    }
    // The 360 kernel returns ZEROED pages from NtAllocateVirtualMemory and games
    // rely on it: Fable 2's package deserializer trusted zeroed count fields, and
    // recycled dirty blocks produced container-too-long aborts from stale bytes.
    memset(g_memory.Translate(addr), 0, need);
    return g_memory.Translate(addr);
}

uint32_t GuestHeap::ReserveVirtualAt(uint32_t base, size_t size)
{
    if (base < kLargeBase || base >= kLargeEnd || (base & 0xFFFF))
        return 0;
    const uint32_t need = static_cast<uint32_t>((size + 0xFFFF) & ~size_t(0xFFFF));
    if (uint64_t(base) + need > kLargeEnd)
        return 0;
    std::lock_guard lock(rangeMutex);
    // Find the free block containing [base, base+need); carve it out exactly.
    auto it = largeFree.upper_bound(base);
    if (it == largeFree.begin())
        return 0;
    --it;
    const uint32_t blockAddr = it->first, blockSize = it->second;
    if (uint64_t(base) + need > uint64_t(blockAddr) + blockSize)
        return 0; // conflict: overlaps an existing reservation (or the range end)
    largeFree.erase(it);
    if (base > blockAddr)
        largeFree.emplace(blockAddr, base - blockAddr);
    if (base + need < blockAddr + blockSize)
        largeFree.emplace(base + need, blockAddr + blockSize - base - need);
    largeUsed.emplace(base, need);
    memset(g_memory.Translate(base), 0, need); // 360 kernel zeroes fresh pages
    return base;
}

void* GuestHeap::Alloc(size_t size)
{
    std::lock_guard lock(mutex);
    void* ptr = o1heapAllocate(heap, std::max<size_t>(1, size));
    if (!ptr)
        fprintf(stderr, "[heap] user heap exhausted allocating %zu bytes\n", size);
    return ptr;
}

// Every physical allocation — large pools and small kernel objects alike — comes
// from the host-side range allocator (see the finding-65 note in heap.h). Large
// requests round to a page so multi-MB pools stay page-exact; small ones round to
// 16 bytes so a kernel object does not burn a whole page.
void* GuestHeap::AllocPhysical(size_t size, size_t alignment, bool topDown)
{
    size = std::max<size_t>(1, size);
    alignment = alignment == 0 ? 0x1000 : std::max<size_t>(16, alignment);

    const bool large = size >= 0x1000 || alignment > 0x1000;
    const uint32_t gran = large ? 0x1000u : 16u;
    const uint32_t need = static_cast<uint32_t>((size + gran - 1) & ~size_t(gran - 1));

    std::lock_guard lock(rangeMutex);
    const uint32_t addr =
        RangeAlloc(physFree, physUsed, need, static_cast<uint32_t>(alignment), topDown);
    if (!addr)
    {
        static bool dumped = false;
        if (!dumped)
        {
            dumped = true;
            ReportExhaustion("physical", size, physFree, physUsed);
        }
        return nullptr;
    }
    // Fresh physical pages are zeroed by the 360 kernel (see AllocVirtual).
    memset(g_memory.Translate(addr), 0, need);
    return g_memory.Translate(addr);
}

void GuestHeap::Free(void* ptr)
{
    if (!ptr)
        return;
    ptr = NormalizePhysical(ptr);
    const size_t guest = static_cast<uint8_t*>(ptr) - g_memory.base;

    if (guest >= kPhysBase && guest < kPhysEnd)
    {
        std::lock_guard lock(rangeMutex);
        if (!RangeFree(physFree, physUsed, static_cast<uint32_t>(guest)))
            fprintf(stderr, "[heap] free of unknown physical block 0x%zx ignored\n", guest);
    }
    else if (guest >= kSmallBase && guest < kSmallEnd)
    {
        std::lock_guard lock(rangeMutex);
        if (!RangeFree(smallFree, smallUsed, static_cast<uint32_t>(guest)))
            fprintf(stderr, "[heap] free of unknown small-page block 0x%zx ignored\n", guest);
    }
    else if (guest >= kLargeBase && guest < kLargeEnd)
    {
        std::lock_guard lock(rangeMutex);
        if (!RangeFree(largeFree, largeUsed, static_cast<uint32_t>(guest)))
            fprintf(stderr, "[heap] free of unknown large-page block 0x%zx ignored\n", guest);
    }
    else
    {
        std::lock_guard lock(mutex);
        o1heapFree(heap, ptr);
    }
}

bool GuestHeap::QueryRegion(uint32_t addr, uint32_t& regionBase, uint32_t& regionSize)
{
    std::lock_guard lock(rangeMutex);
    for (const auto* used : { &smallUsed, &largeUsed, &physUsed })
    {
        for (const auto& [blockAddr, blockSize] : *used)
        {
            if (addr >= blockAddr && addr < blockAddr + blockSize)
            {
                regionBase = blockAddr;
                regionSize = blockSize;
                return true;
            }
        }
    }
    // The o1heap user arena keeps its bookkeeping inline and offers no
    // address-to-block query, so report the arena itself. Callers of
    // NtQueryVirtualMemory are asking about pages they got from
    // NtAllocateVirtualMemory, which never come from here.
    if (addr >= kUserBase && addr < kUserEnd)
    {
        regionBase = kUserBase;
        regionSize = kUserEnd - kUserBase;
        return true;
    }
    return false;
}

size_t GuestHeap::Size(void* ptr)
{
    if (!ptr)
        return 0;
    ptr = NormalizePhysical(ptr);
    const size_t guest = static_cast<uint8_t*>(ptr) - g_memory.base;

    if (guest >= kPhysBase && guest < kPhysEnd)
    {
        std::lock_guard lock(rangeMutex);
        auto it = physUsed.find(static_cast<uint32_t>(guest));
        return it != physUsed.end() ? it->second : 0;
    }
    if (guest >= kSmallBase && guest < kSmallEnd)
    {
        std::lock_guard lock(rangeMutex);
        auto it = smallUsed.find(static_cast<uint32_t>(guest));
        return it != smallUsed.end() ? it->second : 0;
    }
    if (guest >= kLargeBase && guest < kLargeEnd)
    {
        std::lock_guard lock(rangeMutex);
        auto it = largeUsed.find(static_cast<uint32_t>(guest));
        return it != largeUsed.end() ? it->second : 0;
    }
    // o1heap's FragmentHeader is {next, prev, size, used}; the allocation starts at
    // fragment + O1HEAP_ALIGNMENT (32), so `size` sits at (size_t*)ptr - 2.
    return reinterpret_cast<size_t*>(ptr)[-2] - O1HEAP_ALIGNMENT;
}
