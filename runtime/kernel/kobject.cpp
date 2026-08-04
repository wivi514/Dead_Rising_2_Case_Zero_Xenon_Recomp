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
