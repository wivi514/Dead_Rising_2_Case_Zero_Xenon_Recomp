// The transport-level site Case Zero's RELEASE byte disables. Co-op plan
// part 3 (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// The first two-machine session got the whole way through the title's own
// reliable-layer handshake — the joiner's endpoint went Syn Sent -> Open and
// the host's Listen -> Syn Recvd -> Open — and then the HOST crashed reading
// guest address 0x80: sub_82545DF0(NULL), called from sub_8256E6E8, the
// connection listener's per-frame update, on the frame the connection
// became CONNECTED.
//
// sub_8256E6E8 (this) in Case Zero:
//
//   if (this->0x64 != 1) return;                       // not CONNECTING
//   list = (diagByte 0x829EC974 != 0) ? NULL            // <-- Case Zero only
//                                     : this->0x68->0xEC;
//   if (!this->0x68) return;
//   if (!sub_82545DF0(list)) return;                   // derefs list+0x80
//   this->0x64 = 2;                                    // CONNECTED
//   if (diagByte == 0) endpoint = pop(list);           // <-- Case Zero only
//   log(2, "Connlistener2 connection CONNECTED has valid endpoint %s");
//   if (endpoint) { this->0x70 = endpoint; this->0x74 = 1000; }
//   sub_82555840(&ev, this->0x14, 3); ev.0x18 = this->0x48->0x18;
//   sub_8280D2E0(this->0x28, &ev);
//
// Case West's (sub_8253EC68, byte-for-byte the same routine otherwise) has
// neither test: it always reads the endpoint list and always pops it. The
// byte is the release kill switch gotcha 266 describes — 2,013 readers,
// shipped as 1 — and at this one site it does not silence a print, it
// replaces the endpoint list with NULL, so a retail Case Zero host can never
// accept a connection: the predicate faults on the console too, or returns
// false forever if the null page reads as zeros there. Either way it is the
// transport half of "co-op was removed from Case Zero", beside the flags word
// in coop_host.cpp.
//
// This file runs the Case West form of the routine, in C++, calling the same
// guest helpers in the same order. It is a re-implementation rather than a
// patch because clearing the byte for the call would change 2,012 other
// sites for the duration on every thread (CZ_GUEST_DIAG's whole bill,
// gotcha 7, and un-silenced asserts trap). Engaged only when a co-op arm is
// on; CZ_COOP_LISTENER_STOCK=1 runs the title's own code (the crash) as the
// control.
#include <cstdio>
#include <cstdlib>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"

extern "C" PPC_FUNC(__imp__sub_8256E6E8);

using namespace coop;

namespace
{
constexpr uint32_t kFnAnyEndpointReady = 0x82545DF0; // (list) -> bool
constexpr uint32_t kFnOnlineLog = 0x8255B968;        // (logger, level, fmt, ...)
constexpr uint32_t kFnMakeEvent = 0x82555840;        // (&ev, this->0x14, 3)
constexpr uint32_t kFnPostEvent = 0x8280D2E0;        // (this->0x28, &ev)
constexpr uint32_t kFmtConnected = 0x8208196C;       // "Connlistener2 connection CONNECTED has valid endpoint %s\n"
constexpr uint32_t kStrTrue = 0x8201DD6C;
constexpr uint32_t kStrFalse = 0x8200F5C0;

int g_mode = -1; // -1 unread; 0 stock; 1 Case West form

bool CaseWestForm()
{
    if (g_mode < 0)
    {
        auto on = [](const char* n) { const char* e = std::getenv(n); return e && *e && *e != '0'; };
        g_mode = (on("CZ_XLIVE_HOST") || on("CZ_XLIVE_JOIN")) && !on("CZ_COOP_LISTENER_STOCK") ? 1 : 0;
        if (g_mode)
            fprintf(stderr, "[coop] connection listener (sub_8256E6E8): running the Case West "
                            "form — the endpoint list is read, not NULLed by the release byte\n");
    }
    return g_mode == 1;
}
} // namespace

PPC_FUNC(sub_8256E6E8)
{
    if (!CaseWestForm())
    {
        __imp__sub_8256E6E8(ctx, base);
        return;
    }
    const uint32_t self = ctx.r3.u32;
    if (LoadU32(base, self + 0x64) != 1)
        return;
    const uint32_t owner = LoadU32(base, self + 0x68);
    if (!owner)
        return;
    const uint32_t list = LoadU32(base, owner + 0xEC);

    PPCContext call = ctx;
    const uint32_t scratch = (ctx.r1.u32 - 0x60) & ~0xFu; // the 0x40-byte event
    call.r1.u64 = scratch - 0x100;
    call.r3.u64 = list;
    if (!GuestCall(call, base, kFnAnyEndpointReady, "any-endpoint-ready"))
        return;
    if (!(call.r3.u32 & 0xFF))
        return;

    PPC_STORE_U32(self + 0x64, 2);
    // pop(list): head at +0x80, count at +0x7C.
    const uint32_t endpoint = LoadU32(base, list + 0x80);
    PPC_STORE_U32(list + 0x80, 0);
    PPC_STORE_U32(list + 0x7C, LoadU32(base, list + 0x7C) - 1);
    fprintf(stderr, "[coop] connection listener %08X: CONNECTED, endpoint %08X popped from list "
                    "%08X (%u left)\n", self, endpoint, list, LoadU32(base, list + 0x7C));

    call.r3.u64 = self + 8;
    call.r4.u64 = 2;
    call.r5.u64 = kFmtConnected;
    call.r6.u64 = endpoint ? kStrTrue : kStrFalse;
    GuestCall(call, base, kFnOnlineLog, "online-log");

    if (endpoint)
    {
        PPC_STORE_U32(self + 0x70, endpoint);
        PPC_STORE_U16(self + 0x74, 1000);
    }
    call.r3.u64 = scratch;
    call.r4.u64 = LoadU32(base, self + 0x14);
    call.r5.u64 = 3;
    if (!GuestCall(call, base, kFnMakeEvent, "make-event"))
        return;
    PPC_STORE_U32(scratch + 0x18, LoadU32(base, LoadU32(base, self + 0x48) + 0x18));
    call.r3.u64 = LoadU32(base, self + 0x28);
    call.r4.u64 = scratch;
    GuestCall(call, base, kFnPostEvent, "post-event");
}
