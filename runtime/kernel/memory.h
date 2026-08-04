// Guest memory: one flat 4 GB mmap; guest 32-bit address x lives at host `base + x`.
//
// Case Zero's layout. The arena boundaries are NOT copied blind from the two
// template ports — they are checked against this image's own `ppc_config.h`
// constants and against what A1 shows this title asking for (see heap.cpp):
//
//   0x00000000              page 0 — see the note in Init(); the console has plain
//                           RAM here, we trap it during bring-up
//   0x00010000-0x40000000   4 KB-page virtual arena
//   0x40000000-0x7FE00000   64 KB-page virtual arena (MEM_LARGE_PAGES) + explicit-
//                           base reservations, minus the GPU register aperture
//   0x82000000-0x82B40000   XEX image        (PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
//   0x82B40000-0x83C26AC8   indirect-dispatch function table (PPC_LOOKUP_FUNC:
//                           PPC_CODE_SIZE * 2 bytes = 0x10E6AC8)
//   0x88000000-0x9FF00000   host/user heap (o1heap)
//   0xA0000000-0xBFFF0000   physical arena (cached view); also hosts kernel
//                           objects so their guest address doubles as a handle
//                           with bit 31 set (see kobject.h)
//   0xA0000000 / 0xC0000000 / 0xE0000000   three views of ONE memfd
//
// Note the dispatch table is 8 MB smaller here than on Asura's Wrath and the image
// 8 MB smaller still, so 0x88000000 clears it by a wider margin than there — the
// static_assert in heap.cpp is what actually checks it, per image.
#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "ppc_recomp_shared.h"

struct Memory
{
    uint8_t* base{};

    // Reserves the 4 GB space and registers every recompiled function in the
    // indirect-dispatch table. Aborts on failure.
    void Init();

    bool IsInMemoryRange(const void* host) const noexcept
    {
        return host >= base && host < base + PPC_MEMORY_SIZE;
    }

    void* Translate(size_t guest) const noexcept
    {
        if (guest)
            assert(guest < PPC_MEMORY_SIZE);
        return base + guest;
    }

    uint32_t MapVirtual(const void* host) const noexcept
    {
        if (host)
            assert(IsInMemoryRange(host));
        return static_cast<uint32_t>(static_cast<const uint8_t*>(host) - base);
    }

    PPCFunc* FindFunction(uint32_t guest) const noexcept
    {
        if (guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
            return nullptr;
        return PPC_LOOKUP_FUNC(base, guest);
    }

    // PPC_LOOKUP_FUNC computes `table + (guest - PPC_CODE_BASE) * 2` with no bounds
    // check, so inserting an out-of-range address does not fail — it writes an
    // 8-byte host pointer somewhere else in the 4 GB arena. Asura's Wrath minted
    // xam export thunks outside the code range and put six of them 9.4 MB past the
    // table (its finding 54); nothing broke only because that range happened to be
    // unused. Refuse loudly instead: an unbounded write driven by a guest-supplied
    // address is a bug even while its landing zone is empty, and an out-of-range
    // entry could never be dispatched to anyway.
    bool InsertFunction(uint32_t guest, PPCFunc* host)
    {
        if (guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            fprintf(stderr,
                    "[cpu] InsertFunction(%08X) REFUSED: outside the dispatch table's "
                    "range %08llX..%08llX — it would write %lld bytes past the table "
                    "and could never be called anyway\n",
                    guest, (unsigned long long)PPC_CODE_BASE,
                    (unsigned long long)(PPC_CODE_BASE + PPC_CODE_SIZE),
                    (long long)(int64_t(guest) - int64_t(PPC_CODE_BASE + PPC_CODE_SIZE)) * 2);
            return false;
        }
        PPC_LOOKUP_FUNC(base, guest) = host;
        return true;
    }
};

extern Memory g_memory;
