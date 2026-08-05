// The guest heaps carved out of the 4 GB space (see memory.h for the full layout).
//
// Four arenas, because the Xbox 360 kernel hands out four distinguishable kinds of
// memory and games notice when they are conflated:
//
//  - small-page virtual arena (0x00010000..0x40000000)
//      NtAllocateVirtualMemory WITHOUT MEM_LARGE_PAGES.
//  - large-page virtual arena (0x40000000..0x7FE00000)
//      NtAllocateVirtualMemory WITH MEM_LARGE_PAGES, and explicit-base
//      reservations — games probe fixed 64 KB-aligned bases upward until one
//      succeeds and need real conflict semantics, or every reservation aliases the
//      first one. Also the overflow region for the small-page arena.
//  - host/user heap (o1heap, 0x88000000..0x9FF00000)
//      ExAllocatePool*-class allocations, guest thread blocks, and the storage
//      behind the kernel variable imports. Guest code never picks addresses here.
//  - physical arena (0xA0000000..0xBFFF0000)
//      MmAllocatePhysicalMemoryEx-class allocations, and kernel objects — whose
//      guest address serves as their handle (bit 31 set, see kobject.h).
//
// THE SMALL/LARGE SPLIT IS MEASURED, NOT INHERITED
// ------------------------------------------------
// Fable 2's runtime had one virtual arena; Asura's Wrath had to split it when its
// CRT heap manager took a different branch because the address it got back was in
// the wrong region (its finding 14). Case Zero's A1 shows the same two-region
// discipline, and shows it more explicitly than Asura's Wrath did — this title
// reserves, then commits inside its own reservation:
//
//   NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0) = 40000000
//   NtAllocateVirtualMemory(base=40000000, size=00010000, 60001000, 4, 0) = 40000000
//   NtAllocateVirtualMemory(base=40010000, size=00020000, 60001000, 4, 0)
//   ...
//   NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0)  <- next 1 MB
//   NtAllocateVirtualMemory(base=40100000, size=00020000, 60001000, 4, 0)
//
// 0x60002000 is MEM_16MB_PAGES | MEM_LARGE_PAGES | MEM_RESERVE, 0x60001000 the same
// with MEM_COMMIT. Two things follow, and both are checkable rather than assumed:
// the large-page arena must start at exactly 0x40000000 (Xenia's answer to the
// first call), and the second reservation must come back at 0x40100000 (which it
// does iff our arena hands out 64 KB-granular blocks in address order). That pair
// is the cheapest confirmation that our map matches the console's for this title.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "o1heap.h"

struct GuestHeap
{
    std::mutex mutex;
    O1HeapInstance* heap{};

    // One lock for all three host-side range arenas (they are only touched on
    // allocation/free, never in a hot loop).
    std::mutex rangeMutex;

    // Fable 2 finding 65, and the reason none of these three is an o1heap: the
    // physical arena is guest-writable memory that both the guest and the GPU
    // scribble. o1heap stores its 64-bit host-pointer fragment headers INLINE in
    // the memory it manages, so a single stray guest store over a free fragment's
    // next/prev pointer corrupted the free list and killed the *next* allocation
    // with a SIGSEGV inside the allocator — miles from the actual bug. These arenas
    // keep every byte of their bookkeeping host-side, where no guest write reaches.
    std::map<uint32_t, uint32_t> physFree;             // guest addr -> size
    std::unordered_map<uint32_t, uint32_t> physUsed;

    std::map<uint32_t, uint32_t> smallFree;            // 4 KB-page virtual arena
    std::unordered_map<uint32_t, uint32_t> smallUsed;

    std::map<uint32_t, uint32_t> largeFree;            // 64 KB-page virtual arena
    std::unordered_map<uint32_t, uint32_t> largeUsed;  // + explicit-base reservations

    void Init();

    void* Alloc(size_t size);
    // topDown places the block as high in the arena as it fits. Reserved for
    // allocations the RUNTIME owns rather than the guest: the title asks for 447 MB
    // of the 512 MB arena up front, so anything we take from the low end first moves
    // the address it gets back, and gotcha 9 says the guest builds its own map out
    // of those numbers.
    void* AllocPhysical(size_t size, size_t alignment = 0, bool topDown = false);
    // largePages mirrors the guest's MEM_LARGE_PAGES request bit; it picks the
    // arena, which is observable to the guest as the address range it gets back.
    void* AllocVirtual(size_t size, bool largePages);
    // Reserve exactly [base, base+size) in the explicit-reservation range.
    // Returns base on success, 0 on conflict or out-of-range.
    uint32_t ReserveVirtualAt(uint32_t base, size_t size);
    void Free(void* ptr);
    size_t Size(void* ptr);

    // Which block contains `addr`? Backs NtQueryVirtualMemory, which a title's own
    // heap manager uses to discover the true base and extent of a region it was
    // handed. Returns false if the address belongs to no arena we track.
    bool QueryRegion(uint32_t addr, uint32_t& regionBase, uint32_t& regionSize);

    template<typename T, typename... Args>
    T* AllocObject(Args&&... args)
    {
        T* obj = static_cast<T*>(AllocPhysical(sizeof(T), alignof(T)));
        if (!obj)
            return nullptr;
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }
};

extern GuestHeap g_heap;
