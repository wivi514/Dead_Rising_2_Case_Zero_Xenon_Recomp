#include "kobject.h"

#include <cstdio>
#include <unordered_set>

std::recursive_mutex g_kernelLock;

// Live registry of kernel-object handles.
//
// Ported from the two template ports with their findings intact, because neither
// is a per-title quirk — both follow from the handle scheme itself. A handle here
// IS a guest pointer, so a guest double-close asks us to destroy memory we already
// returned to the allocator, and the second free lands in the allocator's own
// bookkeeping. The failure surfaces as a SIGSEGV *inside the heap*, at whatever
// unrelated allocation happens next. Tracking the live set turns that into a
// logged no-op that names the offending handle.
static std::unordered_set<uint32_t> g_kernelHandles;

void RegisterKernelHandle(uint32_t handle)
{
    std::lock_guard guard(g_kernelLock);
    g_kernelHandles.insert(handle);
}

bool UnregisterKernelHandle(uint32_t handle)
{
    std::lock_guard guard(g_kernelLock);
    return g_kernelHandles.erase(handle) != 0;
}

bool IsLiveKernelHandle(uint32_t handle)
{
    std::lock_guard guard(g_kernelLock);
    return g_kernelHandles.count(handle) != 0;
}

void DestroyKernelObject(uint32_t handle)
{
    std::lock_guard guard(g_kernelLock);
    if (!g_kernelHandles.count(handle))
    {
        static int warned = 0;
        if (warned++ < 16)
            fprintf(stderr, "[kobj] destroy of unknown/stale handle %08X ignored\n", handle);
        return;
    }
    KernelObject* obj = GetKernelObject(handle);

    // THE VTABLE POINTER, CHECKED BEFORE IT IS CALLED THROUGH.
    //
    // ~KernelObject is VIRTUAL and the object lives in GUEST memory, so this line
    // dereferences a host pointer that the guest is free to scribble — and the comment
    // below has always said the guest does exactly that. Guarding the free and not the
    // destructor call left the more dangerous of the two unguarded: a bad free lands in
    // the allocator, but a bad vptr is an indirect call to an arbitrary address.
    //
    // The check is deliberately crude and one-sided. A vptr must point into a loaded
    // module, so it must NOT point inside the 4 GB guest arena and must not be null or
    // misaligned. That cannot prove a vptr good; it reliably catches the scribbled ones,
    // which is the whole population we are afraid of.
    const void* vptr = *reinterpret_cast<void* const*>(obj);
    const bool vptrSane = vptr != nullptr &&
                          (reinterpret_cast<uintptr_t>(vptr) & 7) == 0 &&
                          !g_memory.IsInMemoryRange(vptr);
    if (!vptrSane)
    {
        static int scribbled = 0;
        if (scribbled++ < 16)
            fprintf(stderr,
                    "[kobj] handle %08X has a SCRIBBLED vtable pointer (%p) — the guest "
                    "overwrote the object. Destructor NOT called; handle retired and the "
                    "memory left quarantined.\n",
                    handle, vptr);
        g_kernelHandles.erase(handle);
        return;
    }

    if (obj->refCount.fetch_sub(1) == 1)
    {
        g_kernelHandles.erase(handle);
        obj->~KernelObject();
        // Fable 2 finding 56p: run the destructor but do NOT return the memory to
        // the heap. Retail code closes handles whose backing memory the guest has
        // already scribbled or freed through another route; by then the allocator's
        // view of that block is garbage and giving it back corrupts the arena.
        // Kernel objects are small and bounded per session, so quarantining them is
        // the cheap correct answer.
    }
}
