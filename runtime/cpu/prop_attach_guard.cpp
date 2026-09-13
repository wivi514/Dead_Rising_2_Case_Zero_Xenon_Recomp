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
