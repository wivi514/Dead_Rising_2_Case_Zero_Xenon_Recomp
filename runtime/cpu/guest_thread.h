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

    // threadId 0 removes the mapping (see GuestThread::ThreadIdForPcr).
    static void RegisterPcr(uint32_t pcr, uint32_t threadId);
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

// The kernel object standing for the CALLING guest thread.
//
// Win32's GetCurrentThread() does not return a handle — it returns the pseudo-handle
// 0xFFFFFFFE, a constant meaning "whoever is asking". Code then passes it straight to
// DuplicateHandle or ObReferenceObjectByHandle to turn it into something real, and
// Case Zero does both (docs/phase1-notes.md finding 35).
//
// It needs its own type rather than reusing GuestThreadHandle, because that type owns
// the std::thread it spawned and answers Wait() by joining it. A thread asking about
// *itself* is not the thread that spawned it — the main guest thread was never spawned
// by us at all — so this one carries an exit flag instead and polls it.
struct GuestThreadSelf final : KernelObject
{
    std::atomic<bool> exited{ false };

    uint32_t Wait(uint32_t timeoutMs) override;
};

struct GuestThread
{
    // Runs the guest function on the calling host thread; returns its r3.
    static uint32_t Run(const GuestThreadParams& params);
    // Spawns a host thread for the guest function (CREATE_SUSPENDED honored via
    // flags bit 0).
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();

    // Which thread owns a given PCR (r13)?
    //
    // Diagnostics see r13 and nothing else: it is what our critical sections record
    // as an owner, because it is the one value that is unique per guest thread and
    // visible to the guest itself. But every thread registry here is keyed by thread
    // id, so a stall trace could say "thread A is spinning on a section held by
    // thread B" without being able to say which threads those are — and that was
    // precisely the open question in finding 38. Returns 0 for an unknown PCR.
    static uint32_t ThreadIdForPcr(uint32_t pcr);

    // The calling thread's own kernel object, minted on first use and cached for the
    // life of the thread. Null only if the guest heap cannot satisfy it.
    static GuestThreadSelf* Self();
    // Called from the thread bootstrap once the guest entry point returns, so anyone
    // holding a handle to this thread stops waiting.
    static void MarkSelfExited();
};
