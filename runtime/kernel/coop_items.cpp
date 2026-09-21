// CZ_ITEM_TRACE=1: who placed WHICH bike part, and out of WHOSE hands.
// Player issue #9 (`~/XenonLive/Player Issues/#9`), co-op.
//
// WHY THIS EXISTS
// ---------------
// *"After the Client used a key item on the bike most key items were either
// missing or replaced with other key items."* — i.e. the joining player hands
// the bike a part and a DIFFERENT part ticks off on the Case 0-4 HUD.
//
// The title decides which part was placed in exactly one place, and the whole
// decision is local arithmetic on the item the player is holding. Chuck state
// **61** (`TryPlaceItem`, the `ExamineBike1` trigger's action in
// `missions.txt`) lands at `0x8240AF7C` inside `sub_82409900`, the
// `cMissionSetChuckState` executor, and does this:
//
//     playerIdx = *(u32*)(ctx + 0x10)                    // the ACTION's context
//     actor     = sub_8247B020(world->0x7C, playerIdx)   // the user-player array
//     inv       = sub_8215D330(world->0x78->0x30, actor) // that actor's inventory
//     item      = inv[ inv->0x68 ]                       // the SELECTED slot
//     switch (item->0x100)                               // the item's name hash
//        hash("WheelPawn")        -> raise "WheelPawnPlaced"
//        hash("HandleBar")        -> raise "HandleBarPlaced"
//        hash("GasolineCanister") -> raise "GasCanPlaced"
//        hash("BikeEngine")       -> raise "FuelTankPlaced"
//        hash("BikeForks")        -> raise "BikeForksPlaced"
//        none                     -> raise "NoPartsPlaced"
//
// The hash comparison itself cannot go wrong — it is the same table on both
// machines and the names are hashed from the image. So a part placed as the
// WRONG part means the code read the wrong ITEM, and there are only two ways
// to get there: the wrong `playerIdx` (the host running the action for its own
// Chuck when the client pressed the button), or the right player with a
// different `inv->0x68`/slot contents on the two machines (a replica whose
// inventory is ordered differently). Those two predict different logs, so this
// prints both: the index the action used, every player slot's selected item,
// and every slot of every inventory.
//
// It also prints the two steps upstream, because if `playerIdx` is wrong the
// question is immediately where it came from:
//   sub_823B0068(trigger, playerIdx)  the mission trigger's fire, whose second
//                                     argument becomes that context field
//   sub_82245650(.., event, ..)       the broadcast-event listener; subtype 0
//                                     is "fire trigger", carrying the player at
//                                     `+0x14` and the trigger at `+0x18`
//   sub_821AFE48(missionMgr, hash, p) the mission event raise itself — the
//                                     ANSWER, i.e. which part the title decided
//
// Hashes are printed raw AND named where the name is one of the six the bike
// path knows; `tools/name_hash.py --lookup <hex>` reverses any other against
// items.txt. Everything is gated on the variable and every hook is a straight
// pass-through when it is off; none of these are on the frame path.
#include <cstdio>
#include <cstdlib>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"
#include "xlive_session.h"

extern "C" PPC_FUNC(__imp__sub_82409900);
extern "C" PPC_FUNC(__imp__sub_823B0068);
extern "C" PPC_FUNC(__imp__sub_823E79B8);
extern "C" PPC_FUNC(__imp__sub_821AFE48);
extern "C" PPC_FUNC(__imp__sub_82245650);

using namespace coop;

namespace
{
constexpr uint32_t kFnUserPlayer = 0x8247B020; // (userPlayerArray, index) -> actor
constexpr uint32_t kFnTriggerCtx = 0x823A4768;  // (missionObject) -> the mission ACTION CONTEXT
constexpr uint32_t kCtxPlayer = 0x10;           // ... whose +0x10 every action reads as "who"
constexpr uint32_t kFnPlayerInv = 0x8215D330;  // (inventoryMgr, actor) -> inventory block

constexpr uint32_t kChuckStateTryPlaceItem = 61;
constexpr uint32_t kInvSlots = 12;             // the guest's own bound at 0x821A6C3C
constexpr uint32_t kInvSelected = 0x68;        // the selected slot index
constexpr uint32_t kItemNameHash = 0x100;      // GetItemDefHashname()

// The six names the bike path hashes, hashed here the way sub_8276E398 does it
// (h = h*33 ^ (signed char)c) so the trace can print a NAME. tools/name_hash.py
// is the same function and is how any other hash in this trace gets a name.
struct Hashed { uint32_t hash; const char* name; };
constexpr Hashed kKnown[] = {
    {0x878FC97Bu, "WheelPawn"},        {0xC32E815Bu, "HandleBar"},
    {0x5F8D0521u, "GasolineCanister"}, {0xA55F8BABu, "BikeEngine"},
    {0x52EA0EA6u, "BikeForks"},        {0x6AD9B344u, "WheelPawnPlaced"},
    {0xD0DF94E4u, "HandleBarPlaced"},  {0xD4AF6D06u, "GasCanPlaced"},
    {0x34746C95u, "FuelTankPlaced"},   {0x7D7806D9u, "BikeForksPlaced"},
    {0xF574775Au, "NoPartsPlaced"},
};

const char* NameOf(uint32_t hash)
{
    for (const Hashed& h : kKnown)
        if (h.hash == hash)
            return h.name;
    return "?";
}

int Level()
{
    static const int level = [] {
        const char* e = std::getenv("CZ_ITEM_TRACE");
        const int n = (e && *e) ? std::atoi(e) : 0;
        if (n)
            fprintf(stderr, "[item] CZ_ITEM_TRACE=%d — the bike-part path is traced. Each hook "
                            "says so the first time it runs, so a SILENT hook is visible as "
                            "\"never called\" rather than as \"nothing wrong\" (gotcha 30).\n", n);
        return n;
    }();
    return level;
}

// A hook that never runs and a hook that runs and finds nothing print the same
// thing — nothing. One line per hook, the first time it is entered.
void FirstCall(const char* what, bool& seen)
{
    if (seen)
        return;
    seen = true;
    fprintf(stderr, "[item] hook alive: %s\n", what);
}

// Which side of the link this is. It prints the RAW FIELDS rather than a verdict,
// because a label is a hypothesis (the first spelling of this returned "host" in a
// SOLO run: the title keeps a session object with no link, so "session exists" is
// not "co-op"). `coop` is the session's own is-co-op byte — the same +0x98 the
// title's `sub_825530D0` reads — and `isHost` is mm_info's first byte, mm_info
// living at sessionInfo + 0x1C (coop_host.cpp). -1 means the object was absent.
const char* Side(PPCContext& ctx, uint8_t* base)
{
    if (!XliveSession_Enabled())
        return "solo (no session layer)";
    thread_local char buf[64];
    Objects o = Resolve(ctx, base);
    const int coop = o.session ? int(LoadU8(base, o.session + kSessionIsCoopByte)) : -1;
    const int host = o.sessionInfo ? int(LoadU8(base, o.sessionInfo + 0x1C)) : -1;
    std::snprintf(buf, sizeof buf, "coop=%d isHost=%d", coop, host);
    return buf;
}

// One player slot: the actor, the index the actor believes it has, the selected
// inventory slot, and every slot's item. Printed for all four so the two
// machines' logs can be diffed line for line.
void DumpPlayer(PPCContext& ctx, uint8_t* base, uint32_t userPlayers, uint32_t invMgr,
                uint32_t index, bool acting)
{
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = userPlayers;
    call.r4.u64 = index;
    if (!GuestCall(call, base, kFnUserPlayer, "user-player"))
        return;
    const uint32_t actor = call.r3.u32;
    if (!actor)
    {
        fprintf(stderr, "[item]   player %u: empty slot\n", index);
        return;
    }
    const uint32_t ownIndex = LoadU32(base, actor + 0x3A8);
    call.r3.u64 = invMgr;
    call.r4.u64 = actor;
    if (!GuestCall(call, base, kFnPlayerInv, "player-inventory"))
        return;
    const uint32_t inv = call.r3.u32;
    if (!inv)
    {
        fprintf(stderr, "[item]   player %u%s: actor %08X (self-index %d) — NO INVENTORY\n",
                index, acting ? " <- ACTING" : "", actor, int32_t(ownIndex));
        return;
    }
    const uint32_t sel = LoadU32(base, inv + kInvSelected);
    const uint32_t held = sel < kInvSlots ? LoadU32(base, inv + sel * 8 + 4) : 0;
    const uint32_t heldHash = held ? LoadU32(base, held + kItemNameHash) : 0;
    fprintf(stderr, "[item]   player %u%s: actor %08X (self-index %d) inv %08X selected %d "
                    "-> item %08X hash %08X (%s)\n",
            index, acting ? " <- ACTING" : "", actor, int32_t(ownIndex), inv, int32_t(sel),
            held, heldHash, NameOf(heldHash));
    for (uint32_t s = 0; s < kInvSlots; s++)
    {
        const uint32_t it = LoadU32(base, inv + s * 8 + 4);
        if (!it)
            continue;
        const uint32_t h = LoadU32(base, it + kItemNameHash);
        fprintf(stderr, "[item]     slot %2u: item %08X hash %08X (%s)\n", s, it, h, NameOf(h));
    }
}
} // namespace

// cMissionSetChuckState::Execute(action, world, ctx, ...) — state 61 is the bike.
PPC_FUNC(sub_82409900)
{
    { static bool seen = false; if (Level()) FirstCall("sub_82409900 cMissionSetChuckState::Execute", seen); }
    if (Level())
    {
        const uint32_t action = ctx.r3.u32, world = ctx.r4.u32, actionCtx = ctx.r5.u32;
        const uint32_t state = PPC_LOAD_U32(action + 0x40);
        if (state == kChuckStateTryPlaceItem || Level() >= 2)
        {
            const uint32_t playerIdx = PPC_LOAD_U32(actionCtx + 0x10);
            fprintf(stderr, "[item] SetChuckState %u%s (%s) action %08X world %08X ctx %08X "
                            "-> playerIdx %d, r6=%08X lr %08X\n",
                    state, state == kChuckStateTryPlaceItem ? " TryPlaceItem" : "",
                    Side(ctx, base), action, world, actionCtx, int32_t(playerIdx),
                    ctx.r6.u32, uint32_t(ctx.lr));
            if (state == kChuckStateTryPlaceItem && world)
            {
                const uint32_t userPlayers = PPC_LOAD_U32(world + 0x7C);
                const uint32_t game = PPC_LOAD_U32(world + 0x78);
                const uint32_t invMgr = game ? PPC_LOAD_U32(game + 0x30) : 0;
                if (userPlayers && invMgr)
                    for (uint32_t i = 0; i < 4; i++)
                        DumpPlayer(ctx, base, userPlayers, invMgr, i, i == playerIdx);
            }
        }
    }
    __imp__sub_82409900(ctx, base);
}

// cMissionOnTrigger::Update(trigger, missionOwner, updateCtx). The POSITIVE CONTROL
// for this whole file — it runs every frame for every mission trigger in the level,
// so a run that prints nothing else still proves the hooks are alive — and it is
// also the one line that answers the question on its own: `[updateCtx+0x10]` is the
// player index this mission update is running AS, and it is what the trigger hands
// to the action that reads the held item. One line per distinct value, because the
// interesting event is it CHANGING (or never being anything but 0 on a host whose
// partner just pressed the button).
PPC_FUNC(sub_823E79B8)
{
    if (Level())
    {
        static bool seen = false;
        FirstCall("sub_823E79B8 cMissionOnTrigger::Update", seen);
        static int32_t lastIdx = -999;
        const int32_t idx = ctx.r5.u32 ? int32_t(PPC_LOAD_U32(ctx.r5.u32 + 0x10)) : -1;
        if (idx != lastIdx)
        {
            lastIdx = idx;
            fprintf(stderr, "[item] mission update context player index is now %d (%s)\n", idx,
                    Side(ctx, base));
        }
    }
    __imp__sub_823E79B8(ctx, base);
}

// cMissionOnTrigger::Fire(trigger, playerIndex).
//
// THE ARGUMENT IS DEAD, and that is the whole of the reported defect if the trace
// confirms it. The fire resolves the mission's ACTION CONTEXT (`sub_823A4768` ->
// `mission->0x104`) and calls `trigger->vt[0x2C](trigger, ctx->0x1C, ctx, playerIndex,
// 0)`; slot 0x2C is `sub_823A4878`, the action-list loop, which hands each action
// `(action, world, ctx, playerIndex)`. And `cMissionSetChuckState::Execute`
// (`sub_82409900`) does not read the argument — its first act is
// `playerIdx = *(u32*)(ctx + 0x10)`. So whatever the fire was told, state 61 places
// the part it finds in the hands of the player named by that FIELD, and on a co-op
// host that field measured 0 for the whole session.
//
// The trace prints both, so "the argument is dead" is a reading and not a claim:
// `arg` is who fired, `ctx+0x10` is who the action will act as. CZ_COOP_TRIGGER_PLAYER=1
// makes the field follow the argument for the duration of the fire and restores it
// after — the minimal shape of the fix, OFF by default because the mechanism is not
// yet confirmed with two players (co-op plan, player issue #9).
PPC_FUNC(sub_823B0068)
{
    { static bool seen = false; if (Level()) FirstCall("sub_823B0068 trigger fire", seen); }
    static const bool applyFix = [] {
        const char* e = std::getenv("CZ_COOP_TRIGGER_PLAYER");
        return e && *e && *e != '0';
    }();
    const uint32_t arg = ctx.r4.u32;

    // The context, resolved the way the fire itself resolves it — a guest call on a
    // COPY of the context, so nothing here can perturb the caller.
    uint32_t actionCtx = 0;
    if (Level() || applyFix)
    {
        PPCContext call = ctx;
        call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
        call.r3.u64 = ctx.r3.u32;
        if (GuestCall(call, base, kFnTriggerCtx, "trigger-action-context"))
            actionCtx = call.r3.u32;
    }
    const uint32_t was = actionCtx ? PPC_LOAD_U32(actionCtx + kCtxPlayer) : 0xFFFFFFFFu;
    if (Level())
        fprintf(stderr, "[item] TriggerFire trigger %08X arg playerIdx %d, action context "
                        "%08X says %d (%s) lr %08X\n",
                ctx.r3.u32, int32_t(arg), actionCtx, int32_t(was), Side(ctx, base),
                uint32_t(ctx.lr));

    const bool patch = applyFix && actionCtx && arg < 4 && arg != was;
    if (patch)
    {
        PPC_STORE_U32(actionCtx + kCtxPlayer, arg);
        if (Level())
            fprintf(stderr, "[item]   CZ_COOP_TRIGGER_PLAYER: context player %d -> %d for this "
                            "fire\n", int32_t(was), int32_t(arg));
    }
    __imp__sub_823B0068(ctx, base);
    if (patch)
        PPC_STORE_U32(actionCtx + kCtxPlayer, was);
}

// The broadcast-event listener. Subtype 0 is "fire this trigger", and it is the
// one path that carries a player index across a machine boundary.
PPC_FUNC(sub_82245650)
{
    { static bool seen = false; if (Level()) FirstCall("sub_82245650 broadcast-event listener", seen); }
    if (Level() && ctx.r5.u32 && (PPC_LOAD_U8(ctx.r4.u32 + 5) == 0x68))
    {
        const uint32_t ev = ctx.r5.u32;
        const uint32_t sub = PPC_LOAD_U32(ev + 0x10);
        if (sub == 0 || Level() >= 2)
            fprintf(stderr, "[item] Event subtype %u (%s): player %d trigger %08X\n", sub,
                    Side(ctx, base), int32_t(PPC_LOAD_U32(ev + 0x14)), PPC_LOAD_U32(ev + 0x18));
    }
    __imp__sub_82245650(ctx, base);
}

// The answer: which mission event the title actually raised.
PPC_FUNC(sub_821AFE48)
{
    { static bool seen = false; if (Level()) FirstCall("sub_821AFE48 RaiseMissionEvent", seen); }
    const uint32_t hash = ctx.r4.u32;
    if (Level())
    {
        const char* name = NameOf(hash);
        if (*name != '?' || Level() >= 2)
            fprintf(stderr, "[item] RaiseMissionEvent %08X (%s) param %08X (%s) lr %08X\n",
                    hash, name, ctx.r5.u32, Side(ctx, base), uint32_t(ctx.lr));
    }
    __imp__sub_821AFE48(ctx, base);
}
