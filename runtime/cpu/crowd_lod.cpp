#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../host/settings.h"
#include "../kernel/heap.h"
#include "../kernel/memory.h"
#include "ppc_recomp_shared.h"

// ZOMBIES STAY ON THEIR FAR MODEL UNTIL YOU ARE NEARLY TOUCHING THEM (operator,
// 2026-10-08). The title's own behaviour, and now a player setting: ZOMBIE DETAIL.
//
// THE MECHANISM, read out of the crowd renderer (sub_825B8680). Every frame the crowd is
// scored by sub_825A6B38: key = cot(fov/2) / (view depth * camera+0x424), i.e. how large
// the zombie is on screen, and the list is sorted largest first. LOD1 (the close model,
// `<type>_2.big`) then takes the first N1 zombies whose key is >= T1, LOD2 the next N2
// with key >= T2, LOD3 the next N3 (key >= T3). The constants are the title's:
//
//     counts  0x8208A050 = { 1, 30, 70, 400 }          (LOD0..3)
//     keys    0x829DF708 = { 9999, 0.13, 0.01, 0.001 }
//
// and BOTH are scaled by one quality value Q: count_i * Q (clamped 1..500, the sum to
// 500 by the title itself) and key_i / Q. So Q = 2 gives 60 close-model zombies and
// holds the close model to twice the distance. Q comes from the engine's option table
// (0x82AD0480, 0x14-byte entries) through sub_8279A488(settings, group): the settings
// object at 0x82A52D88 picks entry 11 for group 2, whose value is 1.0, and its two
// neighbours in the same group are 0.5 and 2.0 — a low/medium/high switch the 360 left
// on medium. Group 2 is read at exactly ONE call site in the image (0x825B8884; a census
// of every `bl sub_8279A488` and the `li r4,N` before it), so this value moves the
// crowd LOD and nothing else.
//
// Verified live before this was written: poking the settings object's group-2 index
// from 11 to 12 (Q = 2.0) in a running game, the operator: "looks way better".
//
// THE HOOK answers the group-2 lookup with a pointer to a guest float we own, holding
// the setting (0.5..5.0, default 2.0, live — the crowd renderer reads it every frame).
// Every other group passes through. CZ_CROWD_LOD=<value> overrides the setting for an
// A/B, and CZ_CROWD_LOD=0 is the control: the title's own lookup, i.e. its 1.0.

extern "C" PPC_FUNC(__imp__sub_8279A488);

namespace
{
// -1 = no override (use the setting), 0 = control (pass through), >0 = forced value.
float EnvOverride()
{
    static const float v = [] {
        const char* e = std::getenv("CZ_CROWD_LOD");
        if (!e || !*e)
            return -1.0f;
        const float f = float(std::atof(e));
        if (f <= 0.0f)
            fprintf(stderr, "[crowdlod] CZ_CROWD_LOD=0 — the title's own crowd LOD "
                            "scale (1.0), the control\n");
        else
            fprintf(stderr, "[crowdlod] CZ_CROWD_LOD=%.2f overrides the ZOMBIE DETAIL "
                            "setting\n", double(f));
        return f <= 0.0f ? 0.0f : f;
    }();
    return v;
}

uint32_t ScaleSlot()
{
    static const uint32_t guest = [] {
        void* host = g_heap.Alloc(16);
        if (!host)
        {
            fprintf(stderr, "[crowdlod] could not allocate the scale slot — the title's "
                            "own value stays in force\n");
            return 0u;
        }
        std::memset(host, 0, 16);
        return g_memory.MapVirtual(host);
    }();
    return guest;
}
} // namespace

PPC_FUNC(sub_8279A488)
{
    const uint32_t group = ctx.r4.u32;
    __imp__sub_8279A488(ctx, base);
    if (group != 2)
        return;
    const float forced = EnvOverride();
    if (forced == 0.0f)
        return;
    const uint32_t slot = ScaleSlot();
    if (!slot)
        return;
    const float q = forced > 0.0f ? forced : float(Settings_CrowdLodX10()) * 0.1f;
    uint32_t bits;
    std::memcpy(&bits, &q, 4);
    bits = __builtin_bswap32(bits);
    std::memcpy(base + slot, &bits, 4);
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        fprintf(stderr, "[crowdlod] crowd LOD scale %.1f (title default 1.0): close "
                        "models for up to %d zombies, %.1fx the distance\n",
                double(q), int(30 * q + 0.5f), double(q));
    }
    ctx.r3.u64 = slot;
}
