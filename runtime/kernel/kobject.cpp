#include "kobject.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
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

// The vtable pointers we installed, one per kernel object type. See kobject.h.
static std::unordered_set<const void*> g_kernelVtables;

void NoteKernelVtable(const void* vptr)
{
    if (!vptr)
        return;
    std::lock_guard guard(g_kernelLock);
    g_kernelVtables.insert(vptr);
}

bool KernelObjectIsIntact(const KernelObject* obj)
{
    if (!obj)
        return false;
    const void* vptr = *reinterpret_cast<void* const*>(obj);
    std::lock_guard guard(g_kernelLock);
    return g_kernelVtables.count(vptr) != 0;
}

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

// For CZ_KOBJ_DUMP (part 100): a frozen boot's diagnosis needs "every semaphore's
// count and every thread parked on it" read out of the LIVE state, and the registry
// is the only enumeration of the objects that exists. A snapshot, not an iterator,
// so the dump walks without holding the kernel lock across guest-memory reads.
std::vector<uint32_t> SnapshotKernelHandles()
{
    std::lock_guard guard(g_kernelLock);
    return std::vector<uint32_t>(g_kernelHandles.begin(), g_kernelHandles.end());
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

    // The vtable must be one we installed. ~KernelObject is VIRTUAL, so this call
    // dispatches through whatever is in the object's first eight bytes — and the guest
    // writes there. See KernelObjectIsIntact in kobject.h.
    if (!KernelObjectIsIntact(obj))
    {
        static int scribbled = 0;
        if (scribbled++ < 16)
            fprintf(stderr,
                    "[kobj] handle %08X: vtable pointer %p is not one we installed — the "
                    "guest overwrote the object. Destructor NOT called; handle retired, "
                    "memory left quarantined.\n",
                    handle, *reinterpret_cast<void* const*>(obj));
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

// The any-signal generation (kobject.h). One mutex, one condvar, one counter; the
// counter is read under the mutex by waiters and written under it by signallers so a
// signal between "read generation" and "wait" cannot be lost.
namespace
{
std::mutex g_sigMx;
std::condition_variable g_sigCv;
uint64_t g_sigGen = 0;
} // namespace

uint64_t KobjSignal_Generation()
{
    std::lock_guard lk(g_sigMx);
    return g_sigGen;
}

void KobjSignal_Broadcast()
{
    {
        std::lock_guard lk(g_sigMx);
        ++g_sigGen;
    }
    g_sigCv.notify_all();
}

uint64_t KobjSignal_WaitForChange(uint64_t seen, unsigned ms)
{
    std::unique_lock lk(g_sigMx);
    g_sigCv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return g_sigGen != seen; });
    return g_sigGen;
}
