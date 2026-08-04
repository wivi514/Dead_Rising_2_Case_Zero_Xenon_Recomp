#include "memory.h"

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

Memory g_memory;

void Memory::Init()
{
    // MAP_NORESERVE: pages commit on first touch, so the 4 GB reservation is cheap.
    base = static_cast<uint8_t*>(mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (base == MAP_FAILED)
    {
        perror("runtime: mmap 4GB guest space");
        abort();
    }

    // Page 0. PROT_NONE during bring-up, so a guest null dereference faults AT the
    // dereference rather than reading plausible zeros and failing later.
    //
    // This is NOT how the console behaves, and both template ports eventually had
    // to say so (Fable 2's finding 63): on real hardware low memory is plain RAM,
    // so a read of [0x0] returns zero, and code that dereferences a null pointer
    // for a field read is legal and common — retail frontends strcmp NULL strings
    // and read fields off null objects during teardown. When Case Zero starts
    // faulting on null reads that hardware tolerates, flip this with
    // CZ_NULL_PAGE_READABLE rather than deleting the trap: the A/B is the evidence
    // that a given null read is benign.
    //
    // Three states, because "does the guest READ or WRITE through null" is what
    // decides whether a null access is benign, and a page that is either dead or
    // fully live cannot tell them apart:
    //
    //   unset  PROT_NONE  — any null access faults. The bring-up default.
    //   =1/ro  PROT_READ  — null reads succeed (as on the console), writes fault.
    //   =rw               — page 0 fully mapped, i.e. the console's behaviour.
    const char* nullPage = getenv("CZ_NULL_PAGE_READABLE");
    const int nullProt = !nullPage                       ? PROT_NONE
                         : (strcmp(nullPage, "rw") == 0) ? (PROT_READ | PROT_WRITE)
                                                         : PROT_READ;
    if (nullProt != (PROT_READ | PROT_WRITE))
    {
        if (mprotect(base, 0x1000, nullProt) != 0)
            perror("runtime: mprotect null page");
    }
    fprintf(stderr, "[mem] guest page 0 is %s\n",
            nullProt == PROT_NONE   ? "PROT_NONE (any null access faults)"
            : nullProt == PROT_READ ? "PROT_READ (null reads OK, null writes fault)"
                                    : "read/write (console behaviour)");

    // Xbox 360 memory model: 0xA0000000 (cached), 0xC0000000 (write-combined) and
    // 0xE0000000 (uncached) are three views of the SAME 512 MB of physical memory,
    // and games convert pointers between them with plain address arithmetic
    // ((addr & 0x1FFFFFFF) | 0xC0000000). Back all three with one shared memfd so a
    // write through any view is visible through the others. Without this an
    // allocator corrupts itself the first time it frees through a converted
    // pointer, and the corruption surfaces nowhere near the conversion.
    const int fd = memfd_create("xbox_physical", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 0x20000000) != 0)
    {
        perror("runtime: memfd for physical memory");
        abort();
    }
    static const size_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };
    for (size_t viewBase : kPhysicalViews)
    {
        if (mmap(base + viewBase, 0x20000000, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) == MAP_FAILED)
        {
            perror("runtime: mmap physical view");
            abort();
        }
    }
    close(fd);

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
