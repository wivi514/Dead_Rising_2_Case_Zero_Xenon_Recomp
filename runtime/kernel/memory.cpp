#include "memory.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
// NOMINMAX / WIN32_LEAN_AND_MEAN before windows.h, always. This translation unit does
// not use min/max, but a header that pulls windows.h in without them makes the next
// one that does someone else's problem.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

Memory g_memory;

namespace
{
// The Xbox 360's three views of one 512 MB physical range. Games convert between them
// with plain address arithmetic — ((addr & 0x1FFFFFFF) | 0xC0000000) — so a write
// through any one must be visible through the other two. Backing them separately is
// not a subtle bug: an allocator corrupts itself the first time it frees through a
// converted pointer, and the corruption surfaces nowhere near the conversion.
constexpr size_t kPhysSize = 0x20000000;
constexpr size_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

// The three views sit contiguously at the top of the 4 GB space and end exactly at it.
// That is what lets the Windows placeholder carve below be four pieces rather than an
// interleaved mess, so it is asserted rather than remembered.
static_assert(kPhysicalViews[0] + kPhysSize == kPhysicalViews[1], "views not contiguous");
static_assert(kPhysicalViews[1] + kPhysSize == kPhysicalViews[2], "views not contiguous");
static_assert(kPhysicalViews[2] + kPhysSize == PPC_MEMORY_SIZE, "views do not reach 4 GB");

// THE ALIASING SELF-TEST, and it is the gate for this whole function on BOTH platforms.
// The entire point of the mapping is that these three addresses are one piece of
// memory, and nothing else in this runtime checks it — a broken alias produces an
// allocator that corrupts itself hours later and megabytes away, which is precisely
// the class of defect that costs a session to trace back.
//
// Three stores and nine loads, once, at startup: cheap enough to run on every launch
// rather than living in a test somebody has to remember. It probes an offset past
// anything Init() has touched and restores zero, so it cannot disturb what it tests.
bool CheckPhysicalAliasing(uint8_t* base)
{
    constexpr size_t kProbe = 0x1000;
    bool ok = true;
    for (size_t i = 0; i < 3; ++i)
    {
        const uint32_t magic = 0xA5A50000u | uint32_t(i);
        *reinterpret_cast<volatile uint32_t*>(base + kPhysicalViews[i] + kProbe) = magic;
        for (size_t j = 0; j < 3; ++j)
        {
            const uint32_t got =
                *reinterpret_cast<volatile uint32_t*>(base + kPhysicalViews[j] + kProbe);
            if (got != magic)
            {
                fprintf(stderr,
                        "[mem] PHYSICAL ALIASING BROKEN: wrote %08X through %08zX, read "
                        "%08X back through %08zX. The three views are not one piece of "
                        "memory, and a guest allocator will corrupt itself the first "
                        "time it frees through a converted pointer.\n",
                        magic, kPhysicalViews[i], got, kPhysicalViews[j]);
                ok = false;
            }
        }
    }
    for (size_t viewBase : kPhysicalViews)
        *reinterpret_cast<volatile uint32_t*>(base + viewBase + kProbe) = 0;
    return ok;
}
} // namespace

void Memory::Init()
{
#if defined(_WIN32)
    // WINDOWS: a placeholder reservation, then pieces of it replaced.
    //
    // There is no MAP_FIXED here and no equivalent: you cannot map over part of an
    // existing reservation. The supported way to get one contiguous 4 GB span with
    // three aliased windows inside it is the placeholder API (Windows 10 1803+), which
    // is what Xenia does for exactly this, so the pattern is proven rather than
    // invented here.
    //
    //   reserve  [0, 4 GB)             as ONE placeholder
    //   split    [0, 0xA0000000)       -> commit as ordinary private memory
    //   split    the rest into three 512 MB placeholders
    //   map      one section over each of the three
    //
    // Splitting first is mandatory: MEM_REPLACE_PLACEHOLDER accepts a range that is
    // EXACTLY a placeholder, never a sub-range of one.
    HANDLE proc = GetCurrentProcess();
    base = static_cast<uint8_t*>(VirtualAlloc2(proc, nullptr, PPC_MEMORY_SIZE,
                                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                               PAGE_NOACCESS, nullptr, 0));
    if (!base)
    {
        fprintf(stderr, "runtime: VirtualAlloc2 4GB placeholder failed (%lu)\n",
                GetLastError());
        abort();
    }

    // Split off the low 2.5 GB and commit it. Windows has no MAP_NORESERVE for a
    // writable region — reserved-but-uncommitted memory faults on access — so this
    // charges 2.5 GB of COMMIT. Not RAM: pages stay untouched until written. It is the
    // one place this mapping is measurably less thrifty than the POSIX one, and it is
    // written down here rather than discovered from a task manager.
    if (!VirtualFree(base, kPhysicalViews[0], MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
        fprintf(stderr, "runtime: placeholder split at %08zX failed (%lu)\n",
                kPhysicalViews[0], GetLastError());
        abort();
    }
    if (!VirtualAlloc2(proc, base, kPhysicalViews[0],
                       MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                       PAGE_READWRITE, nullptr, 0))
    {
        fprintf(stderr, "runtime: committing the low %zu MB failed (%lu)\n",
                kPhysicalViews[0] >> 20, GetLastError());
        abort();
    }

    // One anonymous section (INVALID_HANDLE_VALUE is the memfd equivalent), mapped
    // three times. SEC_COMMIT so the pages exist.
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE | SEC_COMMIT,
                                        DWORD(uint64_t(kPhysSize) >> 32),
                                        DWORD(kPhysSize & 0xFFFFFFFFu), nullptr);
    if (!section)
    {
        fprintf(stderr, "runtime: CreateFileMapping for physical memory failed (%lu)\n",
                GetLastError());
        abort();
    }

    for (size_t i = 0; i < 3; ++i)
    {
        // The last view needs no split: it is already exactly the remaining
        // placeholder, and splitting a placeholder into itself-plus-nothing fails.
        if (i != 2 &&
            !VirtualFree(base + kPhysicalViews[i], kPhysSize,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            fprintf(stderr, "runtime: placeholder split at %08zX failed (%lu)\n",
                    kPhysicalViews[i], GetLastError());
            abort();
        }
        if (!MapViewOfFile3(section, proc, base + kPhysicalViews[i], 0, kPhysSize,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
        {
            fprintf(stderr, "runtime: MapViewOfFile3 at %08zX failed (%lu)\n",
                    kPhysicalViews[i], GetLastError());
            abort();
        }
    }
    // The views hold their own reference; closing the handle does not unmap them.
    CloseHandle(section);
#else
    // MAP_NORESERVE: pages commit on first touch, so the 4 GB reservation is cheap.
    base = static_cast<uint8_t*>(mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (base == MAP_FAILED)
    {
        perror("runtime: mmap 4GB guest space");
        abort();
    }

    // Back all three views with one shared memfd, so a write through any view is
    // visible through the others.
    const int fd = memfd_create("xbox_physical", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(kPhysSize)) != 0)
    {
        perror("runtime: memfd for physical memory");
        abort();
    }
    for (size_t viewBase : kPhysicalViews)
    {
        if (mmap(base + viewBase, kPhysSize, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) == MAP_FAILED)
        {
            perror("runtime: mmap physical view");
            abort();
        }
    }
    close(fd);
#endif

    // THE POSITIVE CONTROL, and it is not optional. A self-test that has never failed
    // has not been shown capable of failing (gotcha 30), and this one guards a property
    // whose absence is invisible for hours. CZ_MEM_POISON_ALIAS=1 re-maps the middle
    // view as private memory, so the three views stop being one piece of memory and
    // CheckPhysicalAliasing must abort. It costs nothing when unset and it means the
    // check can be re-proved on any machine, on either platform, in one run.
    if (const char* poison = getenv("CZ_MEM_POISON_ALIAS"); poison && *poison != '0')
    {
        fprintf(stderr, "[mem] CZ_MEM_POISON_ALIAS=%s — deliberately breaking the "
                        "%08zX view. The aliasing self-test MUST abort.\n",
                poison, kPhysicalViews[1]);
#if defined(_WIN32)
        UnmapViewOfFileEx(base + kPhysicalViews[1], MEM_PRESERVE_PLACEHOLDER);
        VirtualAlloc2(GetCurrentProcess(), base + kPhysicalViews[1], kPhysSize,
                      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                      PAGE_READWRITE, nullptr, 0);
#else
        mmap(base + kPhysicalViews[1], kPhysSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
#endif
    }

    if (!CheckPhysicalAliasing(base))
        abort();
    fprintf(stderr, "[mem] physical aliasing OK: %08zX/%08zX/%08zX are one 512 MB "
                    "region (CZ_MEM_POISON_ALIAS=1 is the positive control)\n",
            kPhysicalViews[0], kPhysicalViews[1], kPhysicalViews[2]);

    // Page 0. No access during bring-up, so a guest null dereference faults AT the
    // dereference rather than reading plausible zeros and failing later.
    //
    // This is NOT how the console behaves, and both template ports eventually had to
    // say so (Fable 2's finding 63): on real hardware low memory is plain RAM, so a
    // read of [0x0] returns zero, and code that dereferences a null pointer for a
    // field read is legal and common. When Case Zero starts faulting on null reads
    // that hardware tolerates, flip this with CZ_NULL_PAGE_READABLE rather than
    // deleting the trap: the A/B is the evidence that a given null read is benign.
    //
    //   unset  no access — any null access faults. The bring-up default.
    //   =1/ro  read-only — null reads succeed (as on the console), writes fault.
    //   =rw               — page 0 fully mapped, i.e. the console's behaviour.
    const char* nullPage = getenv("CZ_NULL_PAGE_READABLE");
    const int nullMode = !nullPage ? 0 : (strcmp(nullPage, "rw") == 0) ? 2 : 1;
    if (nullMode != 2)
    {
#if defined(_WIN32)
        DWORD old = 0;
        if (!VirtualProtect(base, 0x1000,
                            nullMode == 0 ? PAGE_NOACCESS : PAGE_READONLY, &old))
            fprintf(stderr, "runtime: VirtualProtect null page failed (%lu)\n",
                    GetLastError());
#else
        if (mprotect(base, 0x1000, nullMode == 0 ? PROT_NONE : PROT_READ) != 0)
            perror("runtime: mprotect null page");
#endif
    }
    fprintf(stderr, "[mem] guest page 0 is %s\n",
            nullMode == 0   ? "no access (any null access faults)"
            : nullMode == 1 ? "read-only (null reads OK, null writes fault)"
                            : "read/write (console behaviour)");

    // The indirect-dispatch table lives inside the 4 GB mapping (see memory.h for
    // where), so in-range targets need no extra mapping.
    size_t inserted = 0, refused = 0;
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
    {
        if (InsertFunction(static_cast<uint32_t>(PPCFuncMappings[i].guest),
                           PPCFuncMappings[i].host))
            ++inserted;
        else
            ++refused;
    }
    fprintf(stderr, "[mem] dispatch table: %zu entries installed, %zu refused\n",
            inserted, refused);
}

// xbox.h's xpointer<T> resolves guest pointers through this.
extern "C" void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
