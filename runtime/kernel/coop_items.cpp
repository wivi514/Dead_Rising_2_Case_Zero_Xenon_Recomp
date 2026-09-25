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
// THE SUBJECT MOVED, 2026-09-25 (two real machines, host + czwin). The trace above
// answered its own question and refuted the mechanism it was built for: the player
// index is RIGHT on both machines, seven placements for seven. What is wrong is the
// ITEM — the two machines hold different items at the same inventory slots, and the
// operator's own account names why: a guest's pickup of a world item never reaches
// the host, so the item can be taken twice and the host's copy of the guest's
// inventory is short exactly what the guest picked up.
//
// So this file now also watches INVENTORIES FOR CHANGE, which is the measurement
// that names the moment rather than its consequence twenty seconds later at the bike:
//
//   every ~250 ms (CZ_ITEM_WATCH_MS), walk all four user players' twelve slots and
//   print only what CHANGED — a slot gaining an item (a pickup), losing one (a drop
//   or a use), or KEEPING its pointer while the name hash changes (the identity
//   split the bike trace measured: object AABAD210 was BikeEngine on the host and
//   BikeForks on the joiner).
//
// Run it on both machines and diff. A guest pickup that prints on the guest and not
// on the host is the defect, photographed at the instant it happens. The watch needs
// no new guest addresses — it reuses the two accessors the bike path already calls —
// so it cannot crash on a wrong vtable guess, and it covers EVERY item rather than
// the five the bike knows.
//
// Hashes are printed raw AND named where the name is one of the six the bike
// path knows; `tools/name_hash.py --lookup <hex>` reverses any other against
// items.txt. Everything is gated on the variable and every hook is a straight
// pass-through when it is off; none of these are on the frame path.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

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

// ---------------------------------------------------------------------------
// THE INVENTORY WATCH — the pickup, at the instant it happens
// ---------------------------------------------------------------------------
//
// Driven from cMissionOnTrigger::Update, which already runs every frame for every
// mission trigger in the level and is already hooked; its update context carries the
// world at +0x1C (the same field the trigger fire hands an action as its `world`
// argument). So this costs no new guest address and no new call site.
//
// THE BILL, said out loud because an instrument that stalls the game manufactures the
// stability it reports (gotcha 7): eight guest calls and ~48 loads, at most four times
// a second, on a thread that is not the renderer's. CZ_ITEM_WATCH_MS tunes the period
// and 0 switches the watch off while leaving the rest of the trace on — which is also
// the control arm for "did the watch itself change the run".
constexpr uint32_t kMaxPlayers = 4;

struct SlotState
{
    uint32_t item;
    uint32_t hash;
};

struct InvState
{
    bool seen;
    uint32_t actor;
    uint32_t inv;
    int32_t selected;
    SlotState slot[kInvSlots];
};

int WatchPeriodMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_ITEM_WATCH_MS");
        const int n = (e && *e) ? std::atoi(e) : 250;
        if (Level() && n > 0)
            fprintf(stderr, "[item] inventory watch ON, every %d ms — a slot gaining an item is "
                            "a PICKUP, a slot keeping its pointer while its hash changes is the "
                            "identity split. CZ_ITEM_WATCH_MS=0 is the control arm.\n", n);
        else if (Level())
            fprintf(stderr, "[item] inventory watch OFF (CZ_ITEM_WATCH_MS=0)\n");
        return n;
    }();
    return ms;
}

// One player's twelve slots, compared against what we last saw. Prints only changes,
// and prints the FIRST sighting as a baseline block so two logs can be diffed from a
// common start rather than from whatever each machine happened to be doing.
void WatchPlayer(PPCContext& ctx, uint8_t* base, uint32_t userPlayers, uint32_t invMgr,
                 uint32_t index, InvState& st, const char* side)
{
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = userPlayers;
    call.r4.u64 = index;
    if (!GuestCall(call, base, kFnUserPlayer, "watch-user-player"))
        return;
    const uint32_t actor = call.r3.u32;
    if (!actor)
        return;
    call.r3.u64 = invMgr;
    call.r4.u64 = actor;
    if (!GuestCall(call, base, kFnPlayerInv, "watch-player-inventory"))
        return;
    const uint32_t inv = call.r3.u32;
    if (!inv)
        return;

    const int32_t sel = int32_t(LoadU32(base, inv + kInvSelected));
    SlotState now[kInvSlots];
    for (uint32_t i = 0; i < kInvSlots; i++)
    {
        now[i].item = LoadU32(base, inv + i * 8 + 4);
        now[i].hash = now[i].item ? LoadU32(base, now[i].item + kItemNameHash) : 0;
    }

    if (!st.seen)
    {
        st.seen = true;
        st.actor = actor;
        st.inv = inv;
        st.selected = sel;
        std::memcpy(st.slot, now, sizeof now);
        fprintf(stderr, "[item] INV BASELINE player %u (%s): actor %08X inv %08X selected %d\n",
                index, side, actor, inv, sel);
        for (uint32_t i = 0; i < kInvSlots; i++)
            if (now[i].item)
                fprintf(stderr, "[item]   base slot %2u: item %08X hash %08X (%s)\n", i,
                        now[i].item, now[i].hash, NameOf(now[i].hash));
        return;
    }

    // The actor or the inventory object being replaced is itself worth a line — a
    // respawn or a level change re-seats both, and a diff read across one of those
    // without knowing is how a pickup gets invented.
    if (actor != st.actor || inv != st.inv)
    {
        fprintf(stderr, "[item] INV RESEAT player %u (%s): actor %08X -> %08X, inv %08X -> %08X\n",
                index, side, st.actor, actor, st.inv, inv);
        st.actor = actor;
        st.inv = inv;
    }

    for (uint32_t i = 0; i < kInvSlots; i++)
    {
        const SlotState& was = st.slot[i];
        const SlotState& is = now[i];
        if (was.item == is.item && was.hash == is.hash)
            continue;
        if (!was.item && is.item)
            fprintf(stderr, "[item] PICKUP player %u slot %2u (%s): item %08X hash %08X (%s)\n",
                    index, i, side, is.item, is.hash, NameOf(is.hash));
        else if (was.item && !is.item)
            fprintf(stderr, "[item] LOSE   player %u slot %2u (%s): was item %08X hash %08X (%s)\n",
                    index, i, side, was.item, was.hash, NameOf(was.hash));
        else if (was.item == is.item)
            fprintf(stderr, "[item] IDENTITY player %u slot %2u (%s): item %08X KEPT but hash "
                            "%08X (%s) -> %08X (%s)  <- the same object is now a different item\n",
                    index, i, side, is.item, was.hash, NameOf(was.hash), is.hash,
                    NameOf(is.hash));
        else
            fprintf(stderr, "[item] REPLACE player %u slot %2u (%s): item %08X hash %08X (%s) -> "
                            "item %08X hash %08X (%s)\n",
                    index, i, side, was.item, was.hash, NameOf(was.hash), is.item, is.hash,
                    NameOf(is.hash));
    }
    if (sel != st.selected && Level() >= 2)
        fprintf(stderr, "[item] SELECT player %u (%s): slot %d -> %d\n", index, side,
                st.selected, sel);

    st.selected = sel;
    std::memcpy(st.slot, now, sizeof now);
}

// THE WORLD, resolved the way this project already resolves it.
//
// The first spelling of this read `updateCtx + 0x1C`, on the strength of the trigger
// fire handing an action `ctx->0x1C` as its world. It measured SILENT — the hook was
// alive, the watch was armed, and no baseline ever printed — so that field is not the
// world in the UPDATE context, and a silent instrument that looks armed is exactly the
// failure gotcha 30 is about. Replaced with the five-step chain
// `debug_tunables.cpp:LookupPlayerObject` has been using since the fall guard, vtable
// sanity checks and all: a vtable slot holding a heap pointer mid-transition is a real
// fault this project has already paid for once.
//
// The "is a level running" gate that chain needs is implicit here: this runs from
// cMissionOnTrigger::Update, which only ticks while a level's missions are updating.
uint32_t ResolveWorld(PPCContext& ctx, uint8_t* base, const char*& why)
{
    why = "no game-state manager";
    const uint32_t mgr = LoadU32(base, 0x82A57428);
    if (!mgr)
        return 0;
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = mgr;
    call.r4.u64 = 1;
    why = "session getter refused";
    if (!GuestCall(call, base, 0x82483230, "watch-session"))
        return 0;
    const uint32_t sess = call.r3.u32;
    why = "no session";
    if (!sess)
        return 0;
    const uint32_t vt = LoadU32(base, sess);
    why = "session vtable not in the image (mid-transition)";
    if (vt < 0x82000000 || vt >= 0x82B40000)
        return 0;
    const uint32_t getT = LoadU32(base, vt + 0x10);
    why = "vt[0x10] not code";
    if (getT < 0x82150000 || getT >= 0x829C3554)
        return 0;
    call.r3.u64 = sess;
    why = "world getter refused";
    if (!GuestCall(call, base, getT, "watch-world"))
        return 0;
    why = "no world";
    return call.r3.u32;
}

// The throttle and the reentrancy guard. The sweep makes guest calls, and a guest call
// can re-enter the hook that drives it; without the guard that is unbounded recursion
// rather than a wrong number, so it is a correctness guard and not an optimisation.
void WatchInventories(PPCContext& ctx, uint8_t* base)
{
    if (WatchPeriodMs() <= 0)
        return;

    static thread_local bool inSweep = false;
    if (inSweep)
        return;

    static std::mutex mu;
    static InvState state[kMaxPlayers];
    static std::chrono::steady_clock::time_point next{};

    // try_lock, not lock: the sweep makes guest calls while holding this, and if the
    // mission update ever runs on more than one thread a blocking lock would park a
    // GUEST thread inside an instrument. Skipping a sweep costs a quarter second of
    // resolution; stalling the game manufactures the stability the trace reports
    // (gotcha 7).
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mu, std::try_to_lock);
    if (!lock.owns_lock() || now < next)
        return;
    next = now + std::chrono::milliseconds(WatchPeriodMs());

    inSweep = true;
    const char* why = "";
    const uint32_t world = ResolveWorld(ctx, base, why);
    const uint32_t userPlayers = world ? LoadU32(base, world + 0x7C) : 0;
    const uint32_t game = world ? LoadU32(base, world + 0x78) : 0;
    const uint32_t invMgr = game ? LoadU32(base, game + 0x30) : 0;
    if (!userPlayers || !invMgr)
    {
        // Say it once. A watch that is armed and silent is indistinguishable from a
        // watch that found nothing, which is the whole of gotcha 30.
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[item] inventory watch cannot sweep yet: %s (world %08X "
                            "userPlayers %08X invMgr %08X) — it retries every period and "
                            "says nothing further\n",
                    world ? "world resolved but players/inventory not up" : why, world,
                    userPlayers, invMgr);
        }
        inSweep = false;
        return;
    }

    const char* side = Side(ctx, base);
    for (uint32_t i = 0; i < kMaxPlayers; i++)
        WatchPlayer(ctx, base, userPlayers, invMgr, i, state[i], side);
    inSweep = false;
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
        WatchInventories(ctx, base);
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
    if (Level() && ctx.r5.u32)
    {
        // TWO CENSUSES, one line per newly-seen value, because the decode below is
        // FILTERED and a filter is a hypothesis (gotcha 25: a grep that cannot match is
        // not a clean result). The `+5 == 0x68` test was written for the trigger-fire
        // event; if an item or inventory message travels as a different event class or
        // subtype, the old code could not have printed it however often it arrived. So
        // say what classes and subtypes actually cross this listener, before filtering.
        const uint32_t ev = ctx.r5.u32;
        const uint8_t cls = PPC_LOAD_U8(ctx.r4.u32 + 5);
        const uint32_t sub = PPC_LOAD_U32(ev + 0x10);

        {
            static bool clsSeen[256] = {};
            if (!clsSeen[cls])
            {
                clsSeen[cls] = true;
                fprintf(stderr, "[item] broadcast event CLASS %02X seen for the first time "
                                "(%s)%s\n", cls, Side(ctx, base),
                        cls == 0x68 ? " — the trigger-fire class this trace decodes" : "");
            }
        }
        if (cls == 0x68)
        {
            static bool subSeen[64] = {};
            if (sub < 64 && !subSeen[sub])
            {
                subSeen[sub] = true;
                fprintf(stderr, "[item] broadcast event subtype %u seen for the first time "
                                "(%s)\n", sub, Side(ctx, base));
            }
            // EVERY event, not just the fire, and with the payload words the handler
            // for that subtype actually reads (the wire map is in docs/coop-plan.md).
            // A first-sighting census cannot be correlated in TIME with a pickup, and
            // correlation is the whole question: if a guest's pickup is replicated at
            // all, a message lands within a frame or two of it on the host. ~950
            // events in a whole session, so the volume is nil.
            const uint32_t p14 = PPC_LOAD_U32(ev + 0x14);
            switch (sub)
            {
            case 0:
                fprintf(stderr, "[item] Event subtype 0 (%s): player %d trigger %08X\n",
                        Side(ctx, base), int32_t(p14), PPC_LOAD_U32(ev + 0x18));
                break;
            case 1:
            case 2:
                fprintf(stderr, "[item] Event subtype %u (%s): player %d args %08X %08X "
                                "%08X %08X\n", sub, Side(ctx, base), int32_t(p14),
                        PPC_LOAD_U32(ev + 0x1C), PPC_LOAD_U32(ev + 0x20),
                        PPC_LOAD_U32(ev + 0x24), PPC_LOAD_U32(ev + 0x28));
                break;
            case 3:
            case 4:
                fprintf(stderr, "[item] Event subtype %u (%s): player %d obj %08X b30 %02X "
                                "b31 %02X\n", sub, Side(ctx, base), int32_t(p14),
                        PPC_LOAD_U32(ev + 0x2C), PPC_LOAD_U8(ev + 0x30),
                        PPC_LOAD_U8(ev + 0x31));
                break;
            case 6:
                fprintf(stderr, "[item] Event subtype 6 (%s): player %d, players %d and %d\n",
                        Side(ctx, base), int32_t(p14), int32_t(PPC_LOAD_U32(ev + 0x3C)),
                        int32_t(PPC_LOAD_U32(ev + 0x40)));
                break;
            case 9:
            case 10:
                fprintf(stderr, "[item] Event subtype %u (%s): player %d f60 %08X f64 %08X\n",
                        sub, Side(ctx, base), int32_t(p14), PPC_LOAD_U32(ev + 0x60),
                        PPC_LOAD_U32(ev + 0x64));
                break;
            default:
                fprintf(stderr, "[item] Event subtype %u (%s): player %d%s\n", sub,
                        Side(ctx, base), int32_t(p14),
                        sub == 8 ? " — subtype 8's handler IS the exit: the title does "
                                   "nothing with this" : "");
                break;
            }
        }
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
