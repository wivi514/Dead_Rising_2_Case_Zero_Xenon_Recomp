#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "../host/settings.h"
#include "ppc_recomp_shared.h"

// BUILDINGS, ROOFTOP PROPS AND SIGN LETTERING SWITCH TO THEIR DETAILED VERSION TOO CLOSE
// (operator, 2026-10-08: the pawnshop's cactus, hydrant and "PAWN STILL CREEK SHOP"
// lettering only appear from across the street). The title's own behaviour, and now a
// player setting: WORLD DETAIL.
//
// THE MECHANISM. Every zone carries a list of static-geometry volumes (cZone, count at
// +0x120, 0xD0-byte records at [+0x124]): a sphere at +0x80, a skip bit at +0x90 and a
// switch distance at +0xA8. sub_82175040 is the per-volume vote — NEAR iff
// |camera - centre| - 0.01 - radius < threshold — and the zone async loader
// (zoneasyncloader.cpp: sub_821C6608, sub_8226A398, sub_8226F778, cZone::Update
// sub_82272890) streams the detailed static geometry in for the NEAR volumes; the zone's
// COMMON_TEXTURE vs COMMON_TEXTURE_LOD choice at zone load is the same vote
// (phase5-notes §6bw). Before comparing, the vote boosts the threshold:
//
//     if (threshold < cutoff(level)) threshold *= multiplier(level)
//
// with cutoff = sub_82373DC0 and multiplier = sub_82373E00, two leaf accessors over
// per-level tables at 0x82042C18 / 0x82042D68 (or two debug floats at 0x829DD3D0/D4 when
// the shipped-off byte 0x82A58623 is set). Dead Rising 2's own levels boost every switch
// distance under 20-30 m by 2-2.5x; Case Zero's level (14) is 9999 / 1.0, no boost at
// all. Both accessors are called from exactly one function, the vote, so hooking them
// changes these switch distances and nothing else.
//
// THE HOOK: at a WORLD DETAIL of S != 1.0 the cutoff becomes at least 9999 (every
// volume) and the multiplier is the level's own times S, so every switch distance is
// S times the title's. Verified live before this was written, by poking the debug pair
// (cutoff 9999, x2.0) into a running game: "way better". Dead Rising 2's narrower rule
// (only distances under 25 m, x2) was tried live the same evening and did NOT remove
// the pop/flicker the operator also saw at chunk boundaries, so the full scale shipped.
// CZ_WORLD_LOD=<value> overrides the setting for an A/B; CZ_WORLD_LOD=0 is the control
// (the title's own accessors, untouched). Applies LIVE: the vote reads the pair every
// time it runs, though a zone's texture-set choice is only re-made when it reloads.

extern "C" PPC_FUNC(__imp__sub_82373DC0);
extern "C" PPC_FUNC(__imp__sub_82373E00);

namespace
{
// -1 = no override (use the setting), 0 = control (pass through), >0 = forced value.
double EnvOverride()
{
    static const double v = [] {
        const char* e = std::getenv("CZ_WORLD_LOD");
        if (!e || !*e)
            return -1.0;
        const double f = std::atof(e);
        if (f <= 0.0)
            fprintf(stderr, "[worldlod] CZ_WORLD_LOD=0 — the title's own LOD switch "
                            "distances (1.0), the control\n");
        else
            fprintf(stderr, "[worldlod] CZ_WORLD_LOD=%.2f overrides the WORLD DETAIL "
                            "setting\n", f);
        return f <= 0.0 ? 0.0 : f;
    }();
    return v;
}

// The scale in force, or 1.0 when the title's own values should pass through.
double Scale()
{
    const double forced = EnvOverride();
    if (forced == 0.0)
        return 1.0;
    const double s = forced > 0.0 ? forced : Settings_WorldLodX10() * 0.1;
    // Printed once per distinct value; the zone streamer may vote from more than one
    // thread, hence the exchange.
    static std::atomic<int> announced{-1};
    const int tenths = int(s * 10.0 + 0.5);
    if (announced.exchange(tenths, std::memory_order_relaxed) != tenths)
    {
        fprintf(stderr, "[worldlod] world LOD scale %.1f (title default 1.0): static "
                        "geometry switches to its detailed version at %.1fx the "
                        "distance\n", s, s);
    }
    return s;
}
} // namespace

// The cutoff below which a switch distance is boosted. Raised to cover every volume
// whenever the scale is not 1, so the scale applies to all of them alike.
PPC_FUNC(sub_82373DC0)
{
    __imp__sub_82373DC0(ctx, base);
    if (Scale() != 1.0 && ctx.f1.f64 < 9999.0)
        ctx.f1.f64 = 9999.0;
}

// The boost itself: the level's own multiplier times the player's scale.
PPC_FUNC(sub_82373E00)
{
    __imp__sub_82373E00(ctx, base);
    const double s = Scale();
    if (s != 1.0)
        ctx.f1.f64 = double(float(ctx.f1.f64 * s));   // lfs result: keep it single
}
