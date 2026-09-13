// A prop attachment whose prop has been DESTROYED is dropped before the
// attachment updater moves it.
//
// WHY THIS EXISTS (co-op part 6, 2026-09-12). The first co-op run through the
// military arrival crashed the HOST on the frame the mission action `ArmyPA`
// ran: `DestroyProp: 24578-ArmyHelicopter2 0xaac1f140`, then a NULL write in
// sub_822CF898 (a prop's SetPosition: `this+0xB0` is the position it writes,
// and `this` was thirty-two bytes of zero — the destroyed helicopter, returned
// to its pool). The caller was sub_82295D20, the ATTACHMENT UPDATER: an object
// with five attachment slots (base +0x58, stride 0x2C; the attached prop's
// pointer at slot-0x14) that places each attached prop on a bone of its own
// skeleton every frame. DestroyProp (sub_8221E9C8) detaches a prop from every
// player's rig, local and remote, and this rig was none of them — a stale
// pointer the title's own path never clears, on a scene DR2's co-op never ran
// (Case Zero shipped single-player). The solo game runs the same scene without
// the crash; the difference is which rigs exist when the helicopters go.
//
// The predicate is exact for this class: a prop is virtual (DestroyProp calls
// vt[0x2C] on it before the release) so its first word is never 0 while it is
// alive, and the pool zeroes it on release — the crash dump's [r3] row. A slot
// whose prop reads a zero vtable is cleared, with the pointer printed, before
// the title's updater runs. CZ_NO_ATTACH_GUARD=1 is the control (the crash).
#include <cstdio>
#include <cstdlib>

#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82295D20);

namespace
{
constexpr uint32_t kSlotBase = 0x58;    // r31 = this + 0x58 in the title's loop
constexpr uint32_t kSlotStride = 0x2C;
constexpr uint32_t kSlotCount = 5;
constexpr uint32_t kSlotPropOff = 0x14; // the prop pointer is at slot - 0x14
}

PPC_FUNC(sub_82295D20)
{
    static const bool off = getenv("CZ_NO_ATTACH_GUARD") != nullptr;
    if (!off)
    {
        const uint32_t self = ctx.r3.u32;
        for (uint32_t i = 0; i < kSlotCount; i++)
        {
            const uint32_t at = self + kSlotBase + i * kSlotStride - kSlotPropOff;
            const uint32_t prop = PPC_LOAD_U32(at);
            if (!prop || PPC_LOAD_U32(prop) != 0)
                continue;
            PPC_STORE_U32(at, 0);
            fprintf(stderr, "[attach] rig %08X slot %u held DESTROYED prop %08X (zero vtable) — "
                            "dropped before the updater moved it (CZ_NO_ATTACH_GUARD=1 is the "
                            "control)\n", self, i, prop);
        }
    }
    __imp__sub_82295D20(ctx, base);
}

// THE SECOND HOLDER, AND THE CENSUS THAT FINDS THE REST.
//
// With the rig guard in, the same scene crashed one frame later through the
// player update (sub_82230170 -> ... -> sub_82290720): an ACTOR's mount
// reference — the struct at actorData(+0x28)+0x3794 {vtable 0x820446A4, prop
// +4, float +8, index +0xC} — still pointed at the destroyed helicopter, and the
// title's DestroyProp clears an actor's +0x59C and +0x40C0 but not that one.
// (A first version cleared that reference from every player's actor after the
// title's own body; it never matched — see the next paragraph for why — and
// clearing it is unsafe anyway. DestroyProp is hooked for the census only.)
//
// And because "which holders exist" was learned one crash at a time,
// CZ_PROP_HOLDER_SCAN=1 sweeps the title's whole heap (the 512 MB physical
// alias at A0000000) for the destroyed prop's pointer before the release and
// prints every holder's address — a census, so the next stale reference is read
// off a log line instead of a crash report.
#include "../kernel/coop_objects.h"

extern "C" PPC_FUNC(__imp__sub_8221E9C8);

namespace
{
constexpr uint32_t kActorData = 0x28;
constexpr uint32_t kMountRef = 0x3794;
constexpr uint32_t kHeapLo = 0xA0000000, kHeapHi = 0xC0000000;

void HolderScan(uint8_t* base, uint32_t prop, uint32_t id)
{
    const uint32_t needle = __builtin_bswap32(prop);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(base + kHeapLo);
    const uint32_t* e = reinterpret_cast<const uint32_t*>(base + kHeapHi);
    unsigned n = 0;
    for (; p < e; p++)
        if (*p == needle)
        {
            const uint32_t at = uint32_t(reinterpret_cast<const uint8_t*>(p) - base);
            if (at == prop) continue;   // its own vtable slot is not a holder
            if (n++ < 40)
                fprintf(stderr, "[attach] holder of prop %u (%08X): %08X\n", id, prop, at);
        }
    fprintf(stderr, "[attach] prop %u (%08X): %u holder(s) in the heap before DestroyProp\n",
            id, prop, n);
}
} // namespace

namespace { extern unsigned g_readerCalls; }

PPC_FUNC(sub_8221E9C8)
{
    const uint32_t self = ctx.r3.u32, id = ctx.r4.u32;
    const uint32_t prop = (id < 0x800) ? PPC_LOAD_U32(self + (id + 0xC) * 4) : 0;
    static const bool scan = getenv("CZ_PROP_HOLDER_SCAN") != nullptr;
    if (prop && scan)
        HolderScan(base, prop, id);
    __imp__sub_8221E9C8(ctx, base);
    if (prop && scan)
    {
        fprintf(stderr, "[attach] after DestroyProp (mount reader called %u times so far):\n",
                g_readerCalls);
        HolderScan(base, prop, id);
    }
}

// THE THIRD, FOURTH AND FIFTH RUNS, in one paragraph. The census found NO actor
// holding the helicopter at DestroyProp time on either side, and the host still
// crashed one frame later in sub_82290720 — the update of an actor in its
// MOUNTED mode. What that update reads is actorData+0x3798, which is not the
// prop: it is a SEAT object (0x410 bytes, vtable 0x8202A190) whose +4 is the
// prop; sub_822D5350/58 forward SetPosition/SetRotation from the seat to the
// prop (0x822CF898/0x822CF958). The mode block at actorData+0x3794 {vtable
// 0x820446A4, seat, float, index} is 0x1C0 bytes that the title REPLICATES RAW
// — vt[1] copies it out to the wire, vt[2] (0x82278468) memcpy's it back in,
// seat pointer included, on the strength of the 360's deterministic heap (the
// census showed the same seat addresses on both machines). So the remote
// player's actor on the host is mounted wherever the joiner's is: the joiner's
// Chuck sits in the landed helicopter when the host's ArmyPA destroys it (the
// joiner, behind in the flow, has not run its own ArmyPA yet). Nothing on the
// host ever wrote that reference — it arrived.
//
// Clearing the reference is WRONG: the mounted update's caller (0x822A4874)
// dereferences the seat unconditionally (run 5 crashed there, on my own
// clearing). What is safe is the prop's own SetPosition/SetRotation refusing a
// prop the pool has zeroed — the seat stays, the remote Chuck sits on a ghost
// seat until the joiner's own flow dismounts him and the next replicated block
// says so. The reader hook below is now a TRACE (CZ_ATTACH_TRACE=1) and a
// counter; the guards are on the two prop methods.
extern "C" PPC_FUNC(__imp__sub_82290720);
namespace { unsigned g_readerCalls = 0; }

PPC_FUNC(sub_82290720)
{
    const uint32_t actor = ctx.r3.u32;
    const uint32_t data = actor ? PPC_LOAD_U32(actor + kActorData) : 0;
    const uint32_t seat = data ? PPC_LOAD_U32(data + kMountRef + 4) : 0;
    g_readerCalls++;
    static const bool trace = getenv("CZ_ATTACH_TRACE") != nullptr;
    static unsigned traced = 0;
    if (trace && seat && traced < 16)
    {
        traced++;
        const uint32_t prop = PPC_LOAD_U32(seat + 4);
        fprintf(stderr, "[attach] trace: mounted update actor %08X (vtable %08X) seat %08X "
                        "(vtable %08X) prop %08X (vtable %08X) index %d lr %08X\n",
                actor, PPC_LOAD_U32(actor), seat, PPC_LOAD_U32(seat), prop,
                prop ? PPC_LOAD_U32(prop) : 0, int(PPC_LOAD_U32(data + kMountRef + 0xC)),
                uint32_t(ctx.lr));
    }
    __imp__sub_82290720(ctx, base);
}

// The prop's SetPosition / SetRotation: refuse a prop the pool has released
// (first word zero — it is virtual while alive). Printed once per prop.
extern "C" PPC_FUNC(__imp__sub_822CF898);
extern "C" PPC_FUNC(__imp__sub_822CF958);

namespace
{
bool DeadProp(PPCContext& ctx, uint8_t* base, const char* what)
{
    static const bool off = getenv("CZ_NO_ATTACH_GUARD") != nullptr;
    const uint32_t prop = ctx.r3.u32;
    if (off || !prop || PPC_LOAD_U32(prop) != 0)
        return false;
    static uint32_t last = 0;
    if (last != prop)
    {
        last = prop;
        fprintf(stderr, "[attach] %s on DESTROYED prop %08X (zero vtable) refused — caller lr "
                        "%08X (CZ_NO_ATTACH_GUARD=1 is the control)\n", what, prop,
                uint32_t(ctx.lr));
    }
    return true;
}
} // namespace

PPC_FUNC(sub_822CF898)
{
    if (DeadProp(ctx, base, "SetPosition"))
        return;
    __imp__sub_822CF898(ctx, base);
}

PPC_FUNC(sub_822CF958)
{
    if (DeadProp(ctx, base, "SetRotation"))
        return;
    __imp__sub_822CF958(ctx, base);
}
