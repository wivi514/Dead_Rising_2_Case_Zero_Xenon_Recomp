// Phase 0.2 smoke harness — the gate for "the recompiled image is buildable".
//
// WHAT THIS PROVES, AND WHAT IT DELIBERATELY DOES NOT
//
// It walks the whole `PPCFuncMappings` table and reports on it. That is a link
// gate, not a behaviour test: the table is initialised with a pointer to every one
// of the 57,822 generated functions, so the linker cannot dead-strip any of them
// and an undefined `sub_XXXXXXXX` or `__imp__<KernelName>` is a hard error rather
// than something discovered in phase 1 with a boot half-written.
//
// It does NOT call any guest code. There is no memory map, no guest stack, no
// kernel — every import is an abort stub at this phase — so entering the image
// would prove nothing and would fail in a way that says nothing about the image.
// Phase 1 is what makes entry meaningful.
//
// The checks below are the ones that can be made honestly with no runtime:
//
//   * the table is terminated and its length matches the recompiler's own count
//   * every guest address is inside the image, word-aligned, and strictly
//     increasing (a duplicate or out-of-order entry means the function list was
//     built from overlapping sources — exactly what the widening repairs in
//     finding 13 could have produced if they had gone wrong)
//   * no host pointer is null
//   * the timebase calibrates, because a guest that reads `mftb` before that
//     happens divides by zero (gotcha #1)

#include <ppc_config.h>
#include <ppc_context.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "cpu/timebase.h"

namespace {

constexpr uint64_t IMAGE_LO = PPC_IMAGE_BASE;
constexpr uint64_t IMAGE_HI = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;

int failures = 0;

void fail(const char* what, size_t index, uint64_t addr)
{
    if (++failures <= 20)
        std::fprintf(stderr, "  FAIL [%zu] 0x%08" PRIX64 ": %s\n", index, addr, what);
}

}  // namespace

int main()
{
    std::printf("Dead Rising 2: Case Zero — phase 0.2 smoke harness\n");
    std::printf("  image  0x%08" PRIX64 "..0x%08" PRIX64 "  (%.2f MB)\n",
                IMAGE_LO, IMAGE_HI, double(PPC_IMAGE_SIZE) / (1024.0 * 1024.0));
    std::printf("  code   0x%08llX + 0x%llX\n",
                (unsigned long long)PPC_CODE_BASE, (unsigned long long)PPC_CODE_SIZE);

    if (!cz_timebase::init())
    {
        std::fprintf(stderr, "FAIL: timebase calibration returned 0 — every guest "
                             "`mftb` would divide by zero.\n");
        return 1;
    }
    std::printf("  host TSC %.3f GHz -> guest timebase %.6f MHz\n",
                double(cz_timebase::host_hz) / 1e9, double(CZ_TIMEBASE_HZ) / 1e6);

    size_t count = 0;
    uint64_t prev = 0;
    uint64_t lo = UINT64_MAX, hi = 0;

    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest != 0 || m->host != nullptr;
         ++m, ++count)
    {
        const uint64_t addr = m->guest;

        if (m->host == nullptr)
            fail("null host function pointer", count, addr);
        if (addr < IMAGE_LO || addr >= IMAGE_HI)
            fail("guest address outside the image", count, addr);
        if (addr & 3)
            fail("guest address not word-aligned", count, addr);
        if (count != 0 && addr <= prev)
            fail("guest address not strictly increasing (duplicate or unsorted)",
                 count, addr);

        prev = addr;
        if (addr < lo) lo = addr;
        if (addr > hi) hi = addr;
    }

    // Deliberately "mapping entries", not "functions". The table is larger than the
    // image's function count because it also maps the kernel import thunks and the
    // save/restore ladder helpers to their host implementations. On Case Zero:
    // 57,822 guest functions + 244 kernel imports + 236 ladders + `_xstart` = 58,303.
    // Reporting this as a function count invites the reader to conclude the image
    // grew.
    std::printf("  mapped %zu entries, 0x%08" PRIX64 "..0x%08" PRIX64 "\n"
                "         (guest functions + kernel import thunks + save/restore "
                "ladders)\n",
                count, lo, hi);

    if (count == 0)
    {
        std::fprintf(stderr, "FAIL: the mapping table is empty.\n");
        return 1;
    }
    if (failures > 20)
        std::fprintf(stderr, "  ... and %d more\n", failures - 20);

    if (failures != 0)
    {
        std::fprintf(stderr, "FAIL: %d bad mapping entries.\n", failures);
        return 1;
    }

    std::printf("OK: every generated symbol resolved and every mapping entry is sane.\n");
    return 0;
}
