// Guest thread bootstrap. Every thread that runs recompiled code needs a guest-side
// thread block (r13 points at it) and a guest stack (r1). The block layout is the
// Xbox 360 kernel's: KPCR (0xAB0 bytes, +0x00 = BE pointer to the TLS area,
// +0x100 = BE pointer to the TEB, +0x10C = CPU number), then the XAPI TLS area
// (0x100 bytes = 64 slots x 4), then the TEB (0x2E0, +0x14C = BE thread ID), then
// the stack growing down from the top of the block.
//
// Both constants below come from Case Zero's own XEX header as Xenia prints it in
// A1, not from a round number and not from a template port:
//
//   XEX_HEADER_TLS_INFO:      Slot Count: 64        -> the 0x100 TLS area
//   XEX_HEADER_DEFAULT_STACK_SIZE: 262144           -> 0x40000
//
// and A1 corroborates the stack size independently: Xenia's main XThread runs on
// 70150000-70190000, which is exactly 0x40000. Using the header value rather than a
// generous round number is what makes any later stack-overflow symptom mean
// something.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "../kernel/kobject.h"

constexpr uint32_t kDefaultGuestStackSize = 0x40000;

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* block{};

    GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize = kDefaultGuestStackSize);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function; // guest entry point
    uint32_t arg0;     // r3
    uint32_t arg1;     // r4 (ExCreateThread's XAPI startup wrapper takes two args)
    uint32_t flags;
    uint32_t stackSize;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    uint32_t threadId; // assigned at creation; the spawned thread adopts it
    std::atomic<bool> suspended;
    std::thread thread;

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const { return threadId; }
    uint32_t Wait(uint32_t timeoutMs) override;
};

// Thrown by ExTerminateThread to unwind the guest stack back to the thread bootstrap.
struct GuestThreadExit
{
    uint32_t code;
};

struct GuestThread
{
    // Runs the guest function on the calling host thread; returns its r3.
    static uint32_t Run(const GuestThreadParams& params);
    // Spawns a host thread for the guest function (CREATE_SUSPENDED honored via
    // flags bit 0).
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();
};
