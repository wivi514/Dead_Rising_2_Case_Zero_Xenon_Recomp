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
// So DestroyProp is hooked here too: after the title's own body, every player's
// actor (the same getters it uses: local sub_82482AD8 x4, remote sub_82482AF0
// x10, actor = player vt[0xB4]) has that reference cleared if it names the
// destroyed prop.
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
constexpr uint32_t kFnLocalPlayer = 0x82482AD8;   // (playerMgr, i<4)
constexpr uint32_t kFnRemotePlayer = 0x82482AF0;  // (playerMgr, i<10)
constexpr uint32_t kPlayerVtActor = 0xB4;
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

PPC_FUNC(sub_8221E9C8)
{
    const uint32_t self = ctx.r3.u32, id = ctx.r4.u32;
    const uint32_t prop = (id < 0x800) ? PPC_LOAD_U32(self + (id + 0xC) * 4) : 0;
    static const bool scan = getenv("CZ_PROP_HOLDER_SCAN") != nullptr;
    if (prop && scan)
        HolderScan(base, prop, id);
    __imp__sub_8221E9C8(ctx, base);
    static const bool off = getenv("CZ_NO_ATTACH_GUARD") != nullptr;
    if (!prop || off)
        return;
    const uint32_t playerMgr = PPC_LOAD_U32(self + 8);
    if (!playerMgr)
        return;
    for (uint32_t i = 0; i < 14; i++)
    {
        PPCContext call = ctx;
        call.r3.u64 = playerMgr;
        call.r4.u64 = i < 4 ? i : i - 4;
        if (!coop::GuestCall(call, base, i < 4 ? kFnLocalPlayer : kFnRemotePlayer, "player"))
            return;
        const uint32_t player = call.r3.u32;
        if (!player) continue;
        const uint32_t actor = coop::VCall(call, base, player, kPlayerVtActor, 0, "actor");
        if (!actor) continue;
        const uint32_t data = PPC_LOAD_U32(actor + kActorData);
        if (!data) continue;
        if (PPC_LOAD_U32(data + kMountRef + 4) != prop) continue;
        PPC_STORE_U32(data + kMountRef + 4, 0);
        PPC_STORE_U32(data + kMountRef + 0xC, 0xFFFFFFFF);
        fprintf(stderr, "[attach] player %u (%s) actor %08X was mounted on DESTROYED prop %u "
                        "(%08X) — the mount reference cleared\n", i < 4 ? i : i - 4,
                i < 4 ? "local" : "remote", actor, id, prop);
    }
}

// THE THIRD RUN: the census found NO actor holding the helicopter at
// DestroyProp time on either side, and the host still crashed one frame later
// in sub_82290720 with an actor's mount reference (actorData+0x3798) naming the
// destroyed prop — so the reference is written AFTER the destroy, by something
// that resolved the prop earlier (the census did show four queued event
// records per helicopter carrying its pointer: the joiner's `PropAction
// mAction=0` for that prop, received before the host's own destroy and executed
// after). Until that writer is named, the reader is guarded where it
// dereferences: a mount reference whose prop reads a zero vtable is cleared and
// the actor printed — its address, vtable, and whether it is one of the
// players — so the writer can be found from the log rather than from a crash.
extern "C" PPC_FUNC(__imp__sub_82290720);

PPC_FUNC(sub_82290720)
{
    static const bool off = getenv("CZ_NO_ATTACH_GUARD") != nullptr;
    const uint32_t actor = ctx.r3.u32;
    const uint32_t data = actor ? PPC_LOAD_U32(actor + kActorData) : 0;
    const uint32_t prop = data ? PPC_LOAD_U32(data + kMountRef + 4) : 0;
    if (!off && prop && PPC_LOAD_U32(prop) == 0)
    {
        const uint32_t index = PPC_LOAD_U32(data + kMountRef + 0xC);
        PPC_STORE_U32(data + kMountRef + 4, 0);
        PPC_STORE_U32(data + kMountRef + 0xC, 0xFFFFFFFF);
        fprintf(stderr, "[attach] actor %08X (vtable %08X, data %08X) is mounted on DESTROYED "
                        "prop %08X at index %d — the mount reference cleared before the "
                        "updater moved it (caller lr %08X)\n",
                actor, PPC_LOAD_U32(actor), data, prop, int(index), uint32_t(ctx.lr));
    }
    __imp__sub_82290720(ctx, base);
}
