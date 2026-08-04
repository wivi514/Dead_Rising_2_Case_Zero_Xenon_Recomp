// Kernel objects (events, semaphores, mutants, timers, threads) live as host C++
// objects allocated in the guest *physical* heap (0xA0000000+), so their guest
// address is a valid 32-bit handle with bit 31 set — that is the whole handle
// scheme, and it is why NtClose can find its object with no table lookup.
//
// Ke* dispatcher APIs operate on guest XDISPATCHER_HEADER structs instead of
// handles; QueryKernelObject lazily attaches a host object to the header, stamping
// a signature into WaitListHead.Flink and the host object's guest address into
// .Blink.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <type_traits>

#include <xbox.h>

#include "heap.h"
#include "memory.h"

#define KOBJECT_SIGNATURE          0x584F424Au // 'XOBJ'
#define GUEST_INVALID_HANDLE_VALUE 0xFFFFFFFFu

// NTSTATUS / Win32 values the imports traffic in.
#define STATUS_SUCCESS                0x00000000
#define STATUS_TIMEOUT                0x00000102
#define STATUS_PENDING                0x00000103
#define STATUS_USER_APC               0x000000C0
#define STATUS_WAIT_0                 0x00000000
#define STATUS_INVALID_HANDLE         0xC0000008
#define STATUS_INVALID_PARAMETER      0xC000000D
#define STATUS_NO_MEMORY              0xC0000017
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034
#define STATUS_NOT_FOUND              0xC0000225
#define STATUS_UNSUCCESSFUL           0xC0000001
#define STATUS_NOT_IMPLEMENTED        0xC0000002
#define WAIT_TIMEOUT_INFINITE         0xFFFFFFFFu

struct KernelObject
{
    // NtDuplicateObject shares one host object across handles; NtClose destroys at 0.
    std::atomic<int> refCount{ 1 };

    virtual ~KernelObject() = default;

    virtual uint32_t Wait(uint32_t timeoutMs)
    {
        assert(false && "Wait not implemented for this kernel object.");
        return STATUS_TIMEOUT;
    }
};

extern std::recursive_mutex g_kernelLock;

void RegisterKernelHandle(uint32_t handle);
bool UnregisterKernelHandle(uint32_t handle);
bool IsLiveKernelHandle(uint32_t handle);

template<typename T, typename... Args>
T* CreateKernelObject(Args&&... args)
{
    static_assert(std::is_base_of_v<KernelObject, T>);
    T* obj = g_heap.AllocObject<T>(std::forward<Args>(args)...);
    if (obj)
        RegisterKernelHandle(g_memory.MapVirtual(obj));
    return obj;
}

inline uint32_t GetKernelHandle(KernelObject* obj)
{
    return g_memory.MapVirtual(obj);
}

inline bool IsKernelObject(uint32_t handle)
{
    return (handle & 0x80000000) != 0 && handle != GUEST_INVALID_HANDLE_VALUE;
}

template<typename T = KernelObject>
T* GetKernelObject(uint32_t handle)
{
    assert(IsKernelObject(handle));
    return reinterpret_cast<T*>(g_memory.Translate(handle));
}

// Defined in kobject.cpp: validates the handle against the live registry, because
// a handle is just a guest pointer and a double-close otherwise becomes heap
// corruption inside the allocator, thousands of calls from the actual bug.
void DestroyKernelObject(uint32_t handle);

// Attach-or-fetch the host object backing a guest dispatcher header.
template<typename T>
T* QueryKernelObject(XDISPATCHER_HEADER& header)
{
    std::lock_guard guard(g_kernelLock);
    if (header.WaitListHead.Flink != KOBJECT_SIGNATURE)
    {
        header.WaitListHead.Flink = KOBJECT_SIGNATURE;
        T* obj = CreateKernelObject<T>(reinterpret_cast<typename T::guest_type*>(&header));
        header.WaitListHead.Blink = g_memory.MapVirtual(obj);
        return obj;
    }
    return static_cast<T*>(g_memory.Translate(header.WaitListHead.Blink.get()));
}
