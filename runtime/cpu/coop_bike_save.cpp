#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"

// THE CO-OP MOTORCYCLE MUST NOT CHANGE THE SAVE LAYOUT (2026-10-07, open item 0zg).
//
// Overlay v7 adds a second motorcycle for the co-op partner at the military arrival:
// a `cMissionSpawnItem CoopBike` block (with its `cMissionSetChuckState OnCoopBike`
// mount as a child) in missions.txt — see overlay_gen.cpp RewriteMissions. That broke
// EVERY save written before it: the title reported a 2026-09-23 save as damaged with
// the v7 layer on, and the same file loaded with a v6-equivalent datafile.big whose
// only difference was missions.txt (operator, A/B on one binary; reproduced headless:
// v6 reaches cMissionZombieFactory, v7 never does).
//
// WHY: the save does not describe the missions, it walks them. The mission manager's
// serializer (0x8223EBE0, sentinels 0x3579/0x9753) visits a fixed table of 0x159
// missions at mgr+0x4D8 and calls each one's serialize (sub_821623B0), which writes a
// small header and then walks the mission's action tree — first child at +0x34, next
// sibling at +0x10 — calling every action's vt[0x1C]. Every action writes its own
// bytes, IN DEFINITION ORDER. An action added to missions.txt therefore inserts bytes
// into the middle of the mission block, and a save written without it is read
// misaligned from that point on; the first stream read past the end fails and the
// load reports "damaged" without any assertion (none printed under CZ_GUEST_DIAG).
//
// THE FIX: the CoopBike spawn serializes NOTHING — neither its own byte nor its
// children — so the stream is byte-identical to the stock game's in both directions.
// vt[0x1C] of cMissionSpawnItem is sub_823A4878, a SHARED base-class method (one byte
// at this+0x19, then the children), so the hook keys on the class AND the flag:
//   * *(this) == 0x8204BE3C — cMissionSpawnItem's vtable (stored by its constructor at
//     0x823A505C);
//   * byte this+0x63 != 0 — CoopOnly, the byte the spawn's Execute reads at 0x823A5258.
// The stock missions.txt has 146 spawn items and NONE is CoopOnly; v7 adds exactly one
// (CoopBike). So on stock data this hook never fires, and on v7 data it fires for
// CoopBike only.
//
// What is NOT saved is the CoopBike spawn's one state byte and its mount's; both run
// in the arrival's LevelReady block, which re-runs on a load, so nothing persistent is
// lost. Saves written by a v7 build BEFORE this hook carry the extra bytes and will
// now read as damaged — that build was never released (v1.1.2 predates v7).
//
// CZ_COOP_BIKE_SERIALIZE=1 restores the stock call (the control arm: old saves read
// as damaged again on v7 data).

extern "C" PPC_FUNC(__imp__sub_823A4878);

namespace
{
constexpr uint32_t kSpawnItemVtable = 0x8204BE3Cu;
constexpr uint32_t kCoopOnlyByte = 0x63;

uint32_t Ld32(uint8_t* base, uint32_t va)
{
    uint32_t v;
    std::memcpy(&v, base + va, 4);
    return __builtin_bswap32(v);
}
}   // namespace

PPC_FUNC(sub_823A4878)
{
    static const bool control = [] {
        const char* e = std::getenv("CZ_COOP_BIKE_SERIALIZE");
        const bool on = e && *e && std::strcmp(e, "0") != 0;
        if (on)
            std::fprintf(stderr, "[coopbike] CZ_COOP_BIKE_SERIALIZE=1 — the CoopOnly spawn "
                                 "serializes as shipped (pre-v7 saves read as damaged)\n");
        return on;
    }();
    const uint32_t self = ctx.r3.u32;
    if (!control && self && Ld32(base, self) == kSpawnItemVtable &&
        base[self + kCoopOnlyByte] != 0)
    {
        static std::atomic<uint32_t> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 4)
            std::fprintf(stderr, "[coopbike] CoopOnly spawn %08X kept OUT of the save stream "
                                 "(stock layout; CZ_COOP_BIKE_SERIALIZE=1 restores it)\n",
                         self);
        ctx.r3.u64 = 1;   // "serialized fine", zero bytes
        return;
    }
    __imp__sub_823A4878(ctx, base);
}
