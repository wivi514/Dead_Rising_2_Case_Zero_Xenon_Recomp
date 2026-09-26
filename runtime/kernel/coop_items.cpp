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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_link.h"
#include "coop_objects.h"
#include "xlive_session.h"

extern "C" PPC_FUNC(__imp__sub_82409900);
extern "C" PPC_FUNC(__imp__sub_823B0068);
extern "C" PPC_FUNC(__imp__sub_823E79B8);
extern "C" PPC_FUNC(__imp__sub_821AFE48);
extern "C" PPC_FUNC(__imp__sub_82245650);
extern "C" PPC_FUNC(__imp__sub_8247B020);
extern "C" PPC_FUNC(__imp__sub_823E7890);
extern "C" PPC_FUNC(__imp__sub_821A7550);
extern "C" PPC_FUNC(__imp__sub_8223B000);
extern "C" PPC_FUNC(__imp__sub_82243060);
extern "C" PPC_FUNC(__imp__sub_821A75B8);
extern "C" PPC_FUNC(__imp__sub_8223BBB8);
extern "C" PPC_FUNC(__imp__sub_8223CEF8);
extern "C" PPC_FUNC(__imp__sub_82408908);
extern "C" PPC_FUNC(__imp__sub_821A2200);
extern "C" PPC_FUNC(__imp__sub_82378FA0);
extern "C" PPC_FUNC(__imp__sub_821898B0);
extern "C" PPC_FUNC(__imp__sub_821898E8);

using namespace coop;

namespace
{
constexpr uint32_t kFnUserPlayer = 0x8247B020; // (userPlayerArray, index) -> actor
constexpr uint32_t kFnTriggerCtx = 0x823A4768;  // (missionObject) -> the mission ACTION CONTEXT
constexpr uint32_t kCtxPlayer = 0x10;           // ... whose +0x10 every action reads as "who"
constexpr uint32_t kFnPlayerInv = 0x8215D330;  // (inventoryMgr, actor) -> inventory block
constexpr uint32_t kFnHeldItem = 0x821A6C18;   // (inventoryMgr, actor) -> the SELECTED slot's item
                                               // (kFnPlayerInv plus the selected-slot arithmetic;
                                               // exactly what state 61 calls at 0x8240AF94)
constexpr uint32_t kPropNameHash = 0x98;       // a prop's INSTANCE name hash — the field
                                               // 0x821A2200 matches on. Measured: PropName="Bike2"
                                               // found +0x98 == hash("Bike2")
constexpr uint32_t kPropHolder = 0x1BC;        // sub_822D4870 reads this; 0 = nobody holds it

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

// Which player each inventory belongs to, cached by the watch (which already
// resolves all four every sweep). Declared here rather than beside the pool
// registry below because WatchPlayer, which fills it, comes first in this file.
// Consulting the guest for this inside the item-insert hook would mean guest
// calls on the pickup path; a cache costs nothing and a stale entry prints "?"
// rather than a wrong player.
uint32_t g_invOfPlayer[kMaxPlayers] = {};

// The same map keyed by ACTOR, for the effect-path hooks below: the remove and
// release calls are handed an actor, not an inventory. Filled by the same sweep.
uint32_t g_actorOfPlayer[kMaxPlayers] = {};

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
        if (index < kMaxPlayers)
        {
            g_invOfPlayer[index] = inv;
            g_actorOfPlayer[index] = actor;
        }
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
        if (index < kMaxPlayers)
        {
            g_invOfPlayer[index] = inv;
            g_actorOfPlayer[index] = actor;
        }
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

// ---------------------------------------------------------------------------
// THE EFFECT PATH — CZ_COOP_PLACE_TRACE
// ---------------------------------------------------------------------------
//
// Everything above traces the DECISION: which bike part state 61 decided was
// placed, and out of whose hands it read it. Three sessions on two real machines
// measured that side CORRECT — the host raises the right event for the guest's
// placement every time — and the operator still watches the part land on the
// ground while the HOST's Chuck drops what he was holding. So the defect is
// downstream of the decision, in the mission's RESPONSE, and four candidate
// mechanisms have now been refuted because each was inferred by reading upward
// through a call graph that dead-ends: this engine's interesting paths are
// virtual, and `sub_82378FA0`, `sub_8224AE20` and `sub_8250A8B8` all have zero
// static callers.
//
// So this arm does not reason. It hooks the three things that can actually
// TAKE AN ITEM OFF A CHUCK and prints, for each, WHICH ACTOR it was called on
// and the `lr` of the code that called it. Whichever inventory loses an item
// names the bug in one line, and the `lr` names the caller the call graph
// cannot.
//
//   sub_821A75B8  Inventory::RemoveItemAt(inv, slot, adjustSelected)
//                 The exact mirror of the InsertItemAt hooked above, read off
//                 its own body: bounds `slot` at 12, memmoves the slots above it
//                 down (0x821A7604), zeroes `inv + (count-1)*8 + 4`, decrements
//                 the count at `inv + 0x64` and — only when its third argument is
//                 non-zero — decrements the selected slot at `inv + 0x68`.
//                 EVERY item that leaves an inventory by any route comes
//                 through here.
//   sub_8223BBB8  the release into the world: (mgr, actor, item, .., f1).
//                 It is what the drop-everything loop in sub_8223CEF8 calls once
//                 per slot, so it is the standing candidate for *"it appears next
//                 to the bike"* — an item put into the world instead of consumed.
//   sub_8223CEF8  that drop-everything loop itself, (invMgr, actor), twelve
//                 iterations of sub_8215D330 + sub_8223BBB8. It is reached from
//                 CHUCK STATE 35, whose handler (0x8240A9C0) resolves its actor
//                 from **world->0x80** and not from the action context — the one
//                 place in this jump table measured to use a world-global player
//                 index rather than the acting one. If state 35 is what runs,
//                 that field is the defect and the trace will say so.
//
// AND IT PRINTS CHUCK STATES 34 AND 35, which nothing has ever observed. The
// bike's response in missions.txt is `cMissionSetChuckState ChuckState = "34"`
// (the place animation) and state 34's handler at 0x8240A930 takes its actor
// from `ctx + 0x10`, the same field state 61 reads. Every log this project holds
// was taken with CZ_ITEM_TRACE=1, which prints state 61 ONLY, so the index state
// 34 actually received during a real placement is unmeasured. It is the
// difference between the two surviving readings — the objective-event context's
// type tag 9 (refuted by census, so something else must reach the action) and a
// real but WRONG player index — and one line settles it.
//
// Free when off, and every hook is a straight pass-through. `CZ_ITEM_TRACE=1`
// should be set alongside it: the inventory watch is what maps an inventory
// object to a player number, and without it this trace prints "player ?".
// Defined with the pool registry further down; declared here because the
// descriptors below are written next to the hooks they serve rather than next to
// the tables they read.
int32_t PoolIdOf(uint32_t item, unsigned* whichPool);
int InvPlayer(uint32_t inv);

// The candidate fix, defined beside the lookup it steers (see CZ_COOP_PLACE_FIX);
// declared here because state 61's hook, which feeds it, comes first in this file.
int PlaceFix();
void RememberPlacedItem(PPCContext& ctx, uint8_t* base, uint32_t world, int32_t player);

// Which of the three network event listeners delivered the action currently
// executing. Set by the sub_82378FA0 hook further down; 0 means the action did
// not come through that dispatcher at all, which is itself an answer.
thread_local uint32_t t_dispatchLr = 0;

// The pool census, defined beside the fix it tests; declared here because the
// mission-update hook that drives it comes first.
int PoolCensusMs();
void PoolCensus(PPCContext& ctx, uint8_t* base, uint32_t world);

bool PlaceTrace()
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_PLACE_TRACE");
        const bool v = e && *e && *e != '0';
        if (v)
            fprintf(stderr, "[place] CZ_COOP_PLACE_TRACE=1 — the EFFECT path: every item removed "
                            "from any inventory, every item released into the world, and Chuck "
                            "states 34/35, each with the ACTOR it acted on and the CALLER's lr. "
                            "Set CZ_ITEM_TRACE=1 too or inventories print as \"player ?\".\n");
        return v;
    }();
    return on;
}

// An actor address -> player number (g_actorOfPlayer, filled by the sweep above).
// The remove and release hooks are handed an ACTOR, not an inventory, so they
// need this one and not the other; resolving it through the guest inside those
// hooks would mean guest calls on the drop path.
int ActorPlayer(uint32_t actor)
{
    for (uint32_t i = 0; i < kMaxPlayers; i++)
        if (actor && g_actorOfPlayer[i] == actor)
            return int(i);
    return -1;
}

// "player 1" or "player ? (actor AABB1234)" — never a bare number that might be
// a stale cache hit, because a wrong player number here would misroute the next
// session exactly as four inferred mechanisms already have.
void DescribeActor(uint32_t actor, char* out, size_t n)
{
    const int p = ActorPlayer(actor);
    if (p >= 0)
        std::snprintf(out, n, "player %d (actor %08X)", p, actor);
    else
        std::snprintf(out, n, "player ? (actor %08X)", actor);
}

void DescribeInv(uint32_t inv, char* out, size_t n)
{
    const int p = InvPlayer(inv);
    if (p >= 0)
        std::snprintf(out, n, "player %d (inv %08X)", p, inv);
    else
        std::snprintf(out, n, "player ? (inv %08X)", inv);
}

// The item, named where the bike knows the name and given its pool id either
// way — the pool id being the only field two machines can be compared on.
void DescribeItem(uint8_t* base, uint32_t item, char* out, size_t n)
{
    if (!item)
    {
        std::snprintf(out, n, "item 00000000 (EMPTY)");
        return;
    }
    unsigned pool = 0;
    const int32_t id = PoolIdOf(item, &pool);
    const uint32_t hash = LoadU32(base, item + kItemNameHash);
    if (id >= 0)
        std::snprintf(out, n, "item %08X POOL %u ID %d hash %08X (%s)", item, pool, id, hash,
                      NameOf(hash));
    else
        std::snprintf(out, n, "item %08X (no known pool) hash %08X (%s)", item, hash,
                      NameOf(hash));
}

// ---------------------------------------------------------------------------
// THE HARNESS — CZ_COOP_RAISE_EVENT. A placement's RESPONSE, on demand.
// ---------------------------------------------------------------------------
//
// WHY IT EXISTS. The whole remaining question is what the mission's response to
// `WheelPawnPlaced` does, and reaching that response by playing costs an operator
// session: the five interactable bike parts spawn out in Still Creek
// (`missions.txt`: `WheelPawnWorldSpawn` at -194.955,3.436,-33.441 and four more),
// the bike is in the safehouse garage, and no headless route has ever carried a
// part between the two. So four sessions in a row answered a question about the
// RESPONSE with reasoning about the DECISION, and all four were refuted.
//
// This raises the event directly — `sub_821AFE48(missionManager, hash, 0)`, the
// same call state 61 makes at `0x8240B084` with the same third argument — so the
// response runs exactly as it does after a real placement, on one machine, in
// half a minute, repeatably. What it does NOT reproduce is the acting player's
// held prop being in the pool, and that absence is the useful part: it isolates
// the one thing the response does by itself.
//
// IT IS A TEST HARNESS AND NOT A FIX. It manufactures a mission event, so a run
// carrying it is never evidence about progression, and it must never be on in a
// gate run (the same rule as CZ_FAKE_PRESS_SEQ, gotcha 78).
//
// Spelling: `CZ_COOP_RAISE_EVENT=WheelPawnPlaced@20` — a name from the six the
// bike path knows, or a raw 8-digit hex hash, and the seconds after the first
// successful inventory sweep at which to raise it. Several are comma-separated.
constexpr uint32_t kFnRaiseEvent = 0x821AFE48;   // RaiseMissionEvent(mgr, hash, param)

// THE LEVEL-ARRIVAL SIGNAL, and why it is this one. The harness's countdown used
// to start at "the first mission manager resolves", and with a driver that ticks
// properly that happens AT THE MAIN MENU — the menu is a level with missions
// (`cMissionDefinition MainMenuZombie`) and a mission manager of its own, so the
// first spelling raised WheelPawnPlaced into the title screen. Measured: the raise
// printed while `[pos] player 0` was still at the menu's (2.6, 0.0, 6.9) and the
// DebugJump keypresses had not even been delivered.
//
// The replacement is a signal the GAME states rather than one we infer:
// `PrologueCase0-4`'s own `cMissionLevelReady` runs two `cMissionSendCommandToProp`
// actions the moment the safehouse garage exists (`NonInteractBike` on `Bike2` and
// `NonInteractBike2` on `Bike4`, both `PropCommand = 22`). So the first prop
// command of the run IS "the bike is in the world now", and it is exact.
std::atomic<bool> g_levelPropSeen{false};

struct RaiseJob
{
    uint32_t hash;
    char name[40];
    int atSec;
    bool done;
};
RaiseJob g_raise[8];
unsigned g_raiseCount = 0;
std::chrono::steady_clock::time_point g_raiseEpoch{};

bool RaiseArmed()
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_RAISE_EVENT");
        if (!e || !*e)
            return false;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", e);
        for (char* tok = std::strtok(buf, ","); tok && g_raiseCount < 8;
             tok = std::strtok(nullptr, ","))
        {
            char* at = std::strchr(tok, '@');
            const int sec = at ? std::atoi(at + 1) : 20;
            if (at)
                *at = 0;
            uint32_t hash = 0;
            for (const Hashed& h : kKnown)
                if (std::strcmp(h.name, tok) == 0)
                    hash = h.hash;
            if (!hash)
                hash = uint32_t(std::strtoul(tok, nullptr, 16));
            if (!hash)
            {
                fprintf(stderr, "[raise] CZ_COOP_RAISE_EVENT: \"%s\" is neither a known event "
                                "name nor a hex hash — ignored\n", tok);
                continue;
            }
            RaiseJob& j = g_raise[g_raiseCount++];
            j.hash = hash;
            j.atSec = sec;
            j.done = false;
            std::snprintf(j.name, sizeof j.name, "%s", tok);
            fprintf(stderr, "[raise] CZ_COOP_RAISE_EVENT: will raise %08X (%s) %d s after the "
                            "first inventory sweep. THIS MANUFACTURES A MISSION EVENT — the run "
                            "is a harness, not evidence about progression.\n",
                    hash, NameOf(hash), sec);
        }
        return g_raiseCount > 0;
    }();
    return on;
}

// Called from the mission-trigger update, which already has the world resolved and
// already runs every frame. The mission manager is `world->0x78->0x5C`, the same
// field state 61 reads into r28 at 0x8240AF90 before its raise.
void MaybeRaise(PPCContext& ctx, uint8_t* base, uint32_t world)
{
    if (!RaiseArmed())
        return;
    const uint32_t game = world ? LoadU32(base, world + 0x78) : 0;
    const uint32_t mgr = game ? LoadU32(base, game + 0x5C) : 0;
    if (!mgr || !g_levelPropSeen.load(std::memory_order_relaxed))
    {
        // Say it once. "Not ready yet" and "this harness is dead" are the same
        // silence otherwise, which is the thing that made four candidate fixes
        // unreadable (gotcha 151).
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[raise] not ready yet: world %08X game %08X missionMgr %08X, "
                            "levelProp %d — retrying, and saying nothing further\n",
                    world, game, mgr, int(g_levelPropSeen.load(std::memory_order_relaxed)));
        }
        return;
    }
    // THE CLOCK STARTS WHEN THE GAME SAYS THE LEVEL IS UP (g_levelPropSeen above),
    // not at the first resolved mission manager: the main menu has one of those.
    if (g_raiseEpoch.time_since_epoch().count() == 0)
    {
        g_raiseEpoch = std::chrono::steady_clock::now();
        fprintf(stderr, "[raise] mission manager %08X is up — the countdown starts now\n", mgr);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - g_raiseEpoch)
                             .count();
    // THE COUNTER. The first spelling of this harness printed its schedule, printed
    // "the countdown starts now", and then said nothing for two minutes — which is
    // the same silence as a dead hook (gotcha 151). Say how many times this has
    // been reached and what the clock reads, so "not yet" and "never again" are
    // different lines.
    static uint64_t calls = 0;
    static long long lastSaid = -1;
    calls++;
    if (elapsed != lastSaid && (elapsed % 10) == 0)
    {
        lastSaid = elapsed;
        fprintf(stderr, "[raise] %llds elapsed, %llu mission updates seen; next job at %ds\n",
                (long long)elapsed, (unsigned long long)calls,
                [] { for (unsigned i = 0; i < g_raiseCount; i++) if (!g_raise[i].done) return g_raise[i].atSec; return -1; }());
    }
    for (unsigned i = 0; i < g_raiseCount; i++)
    {
        RaiseJob& j = g_raise[i];
        if (j.done || elapsed < j.atSec)
            continue;
        j.done = true;
        fprintf(stderr, "[raise] raising %08X (%s) on mission manager %08X at %llds — the "
                        "response that follows is what this harness exists to show\n",
                j.hash, NameOf(j.hash), mgr, (long long)elapsed);
        PPCContext call = ctx;
        call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
        call.r3.u64 = mgr;
        call.r4.u64 = j.hash;
        call.r5.u64 = 0;
        GuestCall(call, base, kFnRaiseEvent, "harness-raise-mission-event");
    }
}

// ===========================================================================
// THE FIX — CZ_COOP_ITEM_SYNC. One field crosses the link, and the far machine
// stops answering out of a copy that has drifted.
// ===========================================================================
//
// THE DEFECT, stated as a mechanism rather than as a symptom. State 61 decides
// which bike part was placed by reading the acting player's SELECTED INVENTORY
// SLOT on the machine the code happens to be running on. Both machines run it —
// the trigger fire is broadcast (subtype 0) and each side re-derives the answer
// locally. That only works while the two copies agree, and they are never made
// to: nothing in Case Zero's co-op layer replicates an item or an inventory
// (no named event type, none of the eleven broadcast subtypes), and the pool
// index each copy is built on is handed out by a LIFO free list private to each
// machine (`sub_8223B000`). The two sides therefore start identical — ids issue
// 0,1,2,... from an identically initialised list, which is why the earliest
// placement in the 09-25 session resolved to the same object on both machines
// and worked exactly — and drift apart one way from the first spawn or release
// one machine performs and the other does not. Four placements, four readings:
// the same slot holding different items, a slot the host thought was empty, and
// finally the two machines disagreeing on which slot was even selected.
//
// THE FIX. The machine a player is LOCAL to is always right about what that
// player is holding: it owns the input, the pickup and the inventory. So each
// machine publishes one field — the name hash of its own player's selected item
// — every SyncPeriodMs, and when state 61 runs for a player who is REMOTE here,
// the answer the title computed out of the local copy is replaced with the one
// the owning machine published.
//
// WHY STATE AND NOT AN EVENT. The obvious shape is to send "I placed a
// WheelPawn" at the moment of the placement, and it races: our datagram and the
// title's own trigger-fire event travel by different mechanisms, so the far
// machine can run the placement before the answer arrives. Publishing the held
// item CONTINUOUSLY has no such moment — the value is already there when the
// placement runs, a lost datagram costs a tick of freshness, and the code has no
// ordering to get wrong. It is also the more useful field: anything else that
// needs to know what the other player is holding can read it.
//
// THE SUBSTITUTION IS ONE-WAY, AND THAT IS A SAFETY PROPERTY. It only ever
// turns "no part" or "the wrong part" into a named part. It will never turn a
// part the local machine recognised into NoPartsPlaced, and it never fires when
// the remote machine's answer is not one of the five the bike knows. So the
// worst case of a wrong reading here is the behaviour that already ships, and
// the failure mode cannot be "the fix removed a part that used to work".
//
// WHAT IT DOES NOT DO. It does not reconcile the inventories, the pool indices
// or the selected slot — those still drift, and the host's copy of the guest's
// bag is still wrong. It repairs the one decision the drift is VISIBLE in. The
// general repair is item replication, which Case Zero never had.
//
// CZ_COOP_ITEM_SYNC=0 is the control arm and restores the shipped behaviour
// exactly; every substitution prints one line either way.

constexpr uint32_t kSyncMagic = 0x435A4831u; // 'CZH1' — held-item, version 1
constexpr uint8_t kSyncVersion = 1;
constexpr size_t kSyncBytes = 16;
constexpr uint32_t kNoPartsPlaced = 0xF574775Au;

// The five (held item -> mission event) pairs state 61 switches on. Taken from
// the same table the trace names hashes with, so the two cannot drift apart.
struct PartEvent { uint32_t item; uint32_t event; };
constexpr PartEvent kPartEvents[] = {
    {0x878FC97Bu, 0x6AD9B344u}, // WheelPawn        -> WheelPawnPlaced
    {0xC32E815Bu, 0xD0DF94E4u}, // HandleBar        -> HandleBarPlaced
    {0x5F8D0521u, 0xD4AF6D06u}, // GasolineCanister -> GasCanPlaced
    {0xA55F8BABu, 0x34746C95u}, // BikeEngine       -> FuelTankPlaced
    {0x52EA0EA6u, 0x7D7806D9u}, // BikeForks        -> BikeForksPlaced
};

uint32_t EventForPart(uint32_t itemHash)
{
    for (const PartEvent& p : kPartEvents)
        if (p.item == itemHash)
            return p.event;
    return 0;
}

bool IsPlacementEvent(uint32_t eventHash)
{
    if (eventHash == kNoPartsPlaced)
        return true;
    for (const PartEvent& p : kPartEvents)
        if (p.event == eventHash)
            return true;
    return false;
}

// OFF BY DEFAULT SINCE 2026-09-26, and the reason is in docs/coop-plan.md: the
// operator's two-machine run REFUTED the premise. Both machines raised
// WheelPawnPlaced for the guest's wheel — the mission event AGREED — and the
// wheel still did not attach to the bike. So the event is not the mechanism, and
// an arm that substitutes it cannot be the fix. It stays because the channel and
// the published field are sound and are what the real repair will need, and
// because it is now a control: `=1` engages it, and a run where it changes
// nothing is evidence about the event rather than about the transport.
int SyncMode()
{
    static const int mode = [] {
        const char* e = std::getenv("CZ_COOP_ITEM_SYNC");
        const int on = (e && *e && *e != '0') ? 1 : 0;
        if (on)
            fprintf(stderr, "[itemsync] CZ_COOP_ITEM_SYNC=1 — the held item is published and a "
                            "remote placement's mission event is substituted. REFUTED as the fix "
                            "for issue #9 (the event agreed and the part still did not attach); "
                            "this is an arm, not a repair.\n");
        return on;
    }();
    return mode;
}

// The one predicate the whole fix hangs off. SINGLE PLAYER MATTERS HERE: the
// bike, state 61 and every mission event exist offline too, so without the
// session test the substitution path would open its window, make a guest call
// to ask which side of a session it is on, and warn about a peer that does not
// exist — on a frame path, in a mode this fix has nothing to say about.
bool SyncActive() { return SyncMode() && XliveSession_Enabled(); }

int SyncPeriodMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_ITEM_SYNC_MS");
        const int n = (e && *e) ? std::atoi(e) : 200;
        return n > 0 ? n : 200;
    }();
    return ms;
}

// How stale a published value may be and still be believed. Generous, because
// the thing it guards against is a peer that has GONE (a quit, a hang), not a
// slow link: a held item does not change on its own, so a value a second old is
// as true as one 20 ms old.
int SyncMaxAgeMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_ITEM_SYNC_MAX_AGE_MS");
        const int n = (e && *e) ? std::atoi(e) : 3000;
        return n > 0 ? n : 3000;
    }();
    return ms;
}

// The title's own answer to "am I hosting" (`sub_825530D0`, no arguments — the
// same one the default-outfit code asks). Cached after the first successful
// call: it cannot change inside a session, and this is read on the frame path.
constexpr uint32_t kFnIsHost = 0x825530D0;

// Which end of the session this machine is: 1 host, 0 joiner, -1 not asked yet.
// Written once by HostSide() on a guest thread and read by the receive half,
// which has no guest context of its own to ask with.
std::atomic<int> g_ourSide{-1};

int HostSide(PPCContext& ctx, uint8_t* base)
{
    static int cached = -1; // -1 unknown, 0 joiner, 1 host
    if (cached >= 0)
        return cached;
    PPCContext call = ctx;
    if (!GuestCall(call, base, kFnIsHost, "item-sync-is-host"))
        return -1;
    cached = (call.r3.u32 & 0xFF) ? 1 : 0;
    g_ourSide.store(cached, std::memory_order_relaxed);
    fprintf(stderr, "[itemsync] this machine is the %s\n", cached ? "HOST" : "JOINER");
    return cached;
}

// WHICH USER-PLAYER INDEX IS OURS. The host's Chuck is index 0 and the joiner's
// is 1, in the SAME numbering on both machines — that is what the `[pos]` lines
// measured in the 09-25 session, both sides printing the same positions for 0
// and 1, and it is also why the player index was cleared as a mechanism (it
// tracked correctly on both machines, 7 of 7 and 4 of 4).
//
// It is an ASSUMPTION all the same, so it is overridable and it is checked: a
// message from a peer that claims the same side we are cannot be trusted about
// which index it owns, and is rejected loudly rather than filed.
// The same answer WITHOUT calling guest code. The substitution runs part-way
// through a mission-event raise, and asking the title a question there means a
// guest call nested inside one — avoidable, because the publisher runs every
// frame from the mission update and has already cached the answer long before
// any placement happens. If it somehow has not, the substitution simply does
// not fire this once and the title's own answer stands.
int LocalPlayerIndexCached()
{
    static const int forced = [] {
        const char* e = std::getenv("CZ_COOP_LOCAL_PLAYER");
        return (e && *e) ? std::atoi(e) : -1;
    }();
    if (forced >= 0)
        return forced;
    const int host = g_ourSide.load(std::memory_order_relaxed);
    return host < 0 ? -1 : (host ? 0 : 1);
}

int LocalPlayerIndex(PPCContext& ctx, uint8_t* base)
{
    static const int forced = [] {
        const char* e = std::getenv("CZ_COOP_LOCAL_PLAYER");
        return (e && *e) ? std::atoi(e) : -1;
    }();
    if (forced >= 0)
        return forced;
    const int host = HostSide(ctx, base);
    return host < 0 ? -1 : (host ? 0 : 1);
}

struct RemoteHeld
{
    uint32_t hash = 0;
    uint32_t seq = 0;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
};

std::mutex g_heldMu;
RemoteHeld g_remoteHeld[kMaxPlayers];

// How many messages have been FILED. The self-test reads it, and it is the only
// way one of these guards can be shown to have held: a refused message that is
// merely never read looks exactly like one that was accepted into a slot the
// test does not check — and a broken BOUND writes off the end of the array,
// where no value check can see it at all.
uint32_t g_syncFiled = 0;

// What the receive half last PRINTED, so a value that keeps arriving unchanged
// does not print five times a second.
uint32_t g_lastLoggedIn[kMaxPlayers] = {};

void PutBE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}

uint32_t GetBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Resolve one player's selected item hash. 0 when the chain is not up yet or
// the hand is empty — the two are not distinguished because neither is
// publishable.
uint32_t SelectedItemHash(PPCContext& ctx, uint8_t* base, uint32_t userPlayers, uint32_t invMgr,
                          uint32_t playerIdx)
{
    PPCContext call = ctx;
    call.r3.u64 = userPlayers;
    call.r4.u64 = playerIdx;
    if (!GuestCall(call, base, kFnUserPlayer, "sync-user-player"))
        return 0;
    const uint32_t actor = call.r3.u32;
    if (!actor)
        return 0;
    call.r3.u64 = invMgr;
    call.r4.u64 = actor;
    if (!GuestCall(call, base, kFnPlayerInv, "sync-player-inventory"))
        return 0;
    const uint32_t inv = call.r3.u32;
    if (!inv)
        return 0;
    const int32_t sel = int32_t(LoadU32(base, inv + kInvSelected));
    if (sel < 0 || uint32_t(sel) >= kInvSlots)
        return 0;
    const uint32_t item = LoadU32(base, inv + uint32_t(sel) * 8 + 4);
    return item ? LoadU32(base, item + kItemNameHash) : 0;
}

// Publishes this machine's own player's held item. Driven from the mission
// trigger update, which runs every frame; throttled to SyncPeriodMs, and a
// straight return whenever there is no session to publish into.
void PublishHeldItem(PPCContext& ctx, uint8_t* base)
{
    if (!SyncActive())
        return;

    static thread_local bool inPublish = false;
    if (inPublish)
        return;

    static std::mutex mu;
    static std::chrono::steady_clock::time_point next{};
    static uint32_t seq = 0;

    const auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mu, std::try_to_lock);
    if (!lock.owns_lock() || now < next)
        return;
    next = now + std::chrono::milliseconds(SyncPeriodMs());

    inPublish = true;
    const char* why = "";
    const uint32_t world = ResolveWorld(ctx, base, why);
    const uint32_t userPlayers = world ? LoadU32(base, world + 0x7C) : 0;
    const uint32_t game = world ? LoadU32(base, world + 0x78) : 0;
    const uint32_t invMgr = game ? LoadU32(base, game + 0x30) : 0;
    const int local = (userPlayers && invMgr) ? LocalPlayerIndex(ctx, base) : -1;
    if (local < 0 || uint32_t(local) >= kMaxPlayers)
    {
        inPublish = false;
        return;
    }

    const uint32_t hash = SelectedItemHash(ctx, base, userPlayers, invMgr, uint32_t(local));
    const int host = HostSide(ctx, base);

    uint8_t msg[kSyncBytes] = {};
    PutBE32(msg + 0, kSyncMagic);
    msg[4] = kSyncVersion;
    msg[5] = uint8_t(local);
    msg[6] = uint8_t(host == 1 ? 1 : 0);
    msg[7] = 0;
    PutBE32(msg + 8, hash);
    PutBE32(msg + 12, ++seq);
    const int reached = CoopLink_Broadcast(msg, sizeof msg);

    // Once, when the channel first carries something, and once when the held
    // item changes after that. A per-tick line would be 5 a second forever.
    static uint32_t lastSent = 0xFFFFFFFFu;
    static bool announced = false;
    if (reached && !announced)
    {
        announced = true;
        fprintf(stderr, "[itemsync] publishing player %d's held item to %d peer(s) every %d ms "
                        "(CZ_COOP_ITEM_SYNC=0 is the control arm)\n",
                local, reached, SyncPeriodMs());
    }
    if (reached && hash != lastSent)
    {
        lastSent = hash;
        fprintf(stderr, "[itemsync] player %d now holds %08X (%s)\n", local, hash, NameOf(hash));
    }
    inPublish = false;
}

// The placement window: state 61's hook opens it so the mission-event hook can
// tell a bike placement from every other raise in the game, and for whom.
thread_local bool t_placing = false;
thread_local int32_t t_placingPlayer = -1;

// ===========================================================================
// THE FIX — CZ_COOP_ACTING_PLAYER. A context TYPE is not a player index.
// ===========================================================================
//
// THE DEFECT, read out of the image and out of the game's own missions.txt.
// `cMissionSetChuckState::Execute` takes its player from `ctx + 0x10`
// (`0x82409918: lwz r4, 0x10(r5)`). From a mission TRIGGER that field really is
// the acting player — measured correct on both machines for three sessions,
// which is why the player index kept clearing as a suspect. But the bike's
// placement response is not a trigger. It is data:
//
//     cMissionObjectiveEvent GetWheelPawn          (missions.txt:6970)
//         EventString = "WheelPawnPlaced"
//         cMissionSetChuckState PlaceItemAnimation11 { ChuckState = "34" }
//         ... cMissionSendCommandToProp { PropCommand = 17, PropName = "WheelPawn" }
//
// and the objective-event dispatch builds a STACK context whose `+0x10` is the
// context's TYPE TAG, not a player: `sub_8248B838` writes the constant **9**
// there (`0x8248B844`), with the raise's param going to `+0x14` — and state 61
// raises with param 0 anyway, so no player reaches the response at all.
//
// So the place animation asks for user player 9, and the lookup does not refuse
// it. `sub_8247B020` bounds the index at MAX_USER_PLAYERS = 4 and its assert
// ("index >= 0 && index < MAX_USER_PLAYERS", actormanager.cpp:749) is gated on
// the 0x829EC974 diag byte — 1 in every shipped build, gotcha 266 — and then
// FALLS THROUGH to the same load anyway (`0x8247B0F0`):
//
//     return *(players + (index + 3) * 4)
//
// For 9 that is five entries past the end of a four-entry array. Whatever object
// lives there is what the animation is applied to, so the item comes out of the
// wrong Chuck's hands, the acting player's item is never consumed and lands in
// the world instead of on the bike, and — because the wrong answer is a FIXED
// address — it looks fine whenever it happens to coincide with the acting
// player. That is why the host's own placements work, and why single player has
// never shown this.
//
// THE REPAIR. State 61's raise is SYNCHRONOUS: the response runs inside it, on
// this thread, before it returns. So the acting player is still knowable. Every
// `cMissionSetChuckState::Execute` whose context carries an IN-RANGE player
// publishes it for the duration of its own call, and when the user-player lookup
// is then handed an index that is out of range, it is given that player instead
// of being allowed to read past the array.
//
// WHAT IT CANNOT DO, which is what makes it safe: an IN-RANGE index is never
// touched, so every trigger-driven state change in the game — the whole rest of
// the mission system — behaves exactly as it does today. It engages only where
// the title was about to perform an out-of-bounds load, which is a defect
// wherever it happens, in co-op or not.
//
// CZ_COOP_ACTING_PLAYER=0 is the control arm. **=2 OBSERVES WITHOUT
// SUBSTITUTING** — it prints every out-of-range lookup and lets the title do
// what it does today, which is the measurement arm for "does this actually
// happen, and with which index", and the thing to run if the fix is ever
// suspected of changing something it should not.
constexpr int32_t kMaxUserPlayers = 4; // MAX_USER_PLAYERS, from the assert at 0x8247B0A8

int ActingPlayerMode()
{
    static const int mode = [] {
        const char* e = std::getenv("CZ_COOP_ACTING_PLAYER");
        const int n = (e && *e) ? std::atoi(e) : 1;
        if (n == 0)
            fprintf(stderr, "[acting] CZ_COOP_ACTING_PLAYER=0 — the out-of-range user-player "
                            "lookup is left alone (the control arm; the title reads past the "
                            "end of its player array)\n");
        else if (n >= 2)
            fprintf(stderr, "[acting] CZ_COOP_ACTING_PLAYER=2 — OBSERVE ONLY: every out-of-range "
                            "user-player lookup is printed and NOT substituted\n");
        return n;
    }();
    return mode;
}

// The acting player of the innermost cMissionSetChuckState whose context carried
// a real one. Thread-local because the raise and its response are one call stack
// on one thread, and because two mission updates on two threads must not share
// it.
thread_local int32_t t_actingPlayer = -1;

// AND THE SAME ANSWER THAT SURVIVES A DEFERRAL. The call-stack-only version of
// this fix measured ZERO substitutions on the operator's two-machine run, while
// the host raised WheelPawnPlaced for the guest correctly — so the objective-
// event response does NOT run inside the raise. `sub_823E7890` publishes a
// type-9 event object through `sub_82188488` and something drains it later,
// which is exactly what the plan warned was unverified.
//
// So the acting player is also recorded with a timestamp, outside any thread,
// and an out-of-range lookup falls back to it when the call stack no longer
// carries one. It is only ever consulted where the title was about to read past
// the end of its array, so a stale answer replaces a WRONG answer, never a right
// one.
//
// WHICH SOURCE WAS USED IS PRINTED, and that is the measurement this owes: "on
// the stack" means the dispatch was synchronous after all, "from the record"
// names how many milliseconds late the response ran.
struct RecentActing
{
    int32_t player = -1;
    std::chrono::steady_clock::time_point at{};
};
std::mutex g_recentMu;
RecentActing g_recentActing;

int ActingWindowMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_ACTING_WINDOW_MS");
        const int n = (e && *e) ? std::atoi(e) : 3000;
        return n > 0 ? n : 3000;
    }();
    return ms;
}

void RecordActing(int32_t player)
{
    std::lock_guard<std::mutex> lock(g_recentMu);
    g_recentActing.player = player;
    g_recentActing.at = std::chrono::steady_clock::now();
}

int32_t RecentActing_Get(long long* ageMsOut)
{
    std::lock_guard<std::mutex> lock(g_recentMu);
    if (g_recentActing.player < 0)
        return -1;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - g_recentActing.at)
                         .count();
    if (age > ActingWindowMs())
        return -1;
    *ageMsOut = age;
    return g_recentActing.player;
}

uint64_t g_actingSubs = 0;

// ---------------------------------------------------------------------------
// THE POOL REGISTRY — what turns "an item appeared" into "pool id N resolved to
// this item", which is the only form of the observation that can be COMPARED
// between two machines.
//
// The item-pool layout is established (see "WHO ALLOCATES A POOL ENTRY"): a
// manager holds 2,048 records of 664 bytes at `*(mgr + 0x6D00)`, and the id is
// popped off a LIFO free list private to that machine. So an item's id is pure
// arithmetic — `(item - poolBase) / 664` — once `poolBase` is known, and the
// spawner is where to learn it: `sub_8223B000`'s r3 IS the manager.
//
// Every distinct (manager, poolBase) is recorded rather than assuming there is
// one, because the 09-25 session measured item addresses in TWO pools and it was
// never settled whether that was two managers or something else. An item that
// matches no known pool is reported as such instead of being given a wrong id —
// a wrong id here would be worse than none, because the whole point is to
// compare ids across machines.
// The message type that grants a key item (0x822430D8), and the two ids
// that exist (items.txt).
constexpr uint32_t kKeyItemMessageType = 0x13;

constexpr uint32_t kPoolBaseField = 0x6D00;
constexpr uint32_t kPoolStride = 0x298;  // 664
constexpr uint32_t kPoolEntries = 2048;

struct Pool { uint32_t mgr = 0; uint32_t base = 0; };
std::mutex g_poolMu;
Pool g_pools[4];
unsigned g_poolCount = 0;

void RegisterPool(uint8_t* base, uint32_t mgr)
{
    if (!mgr)
        return;
    const uint32_t poolBase = LoadU32(base, mgr + kPoolBaseField);
    if (!poolBase)
        return;
    std::lock_guard<std::mutex> lock(g_poolMu);
    for (unsigned i = 0; i < g_poolCount; i++)
        if (g_pools[i].mgr == mgr && g_pools[i].base == poolBase)
            return;
    if (g_poolCount >= 4)
        return;
    g_pools[g_poolCount++] = Pool{mgr, poolBase};
    fprintf(stderr, "[pickup] item pool %u registered: manager %08X, records at %08X "
                    "(%u x %u bytes)\n",
            g_poolCount - 1, mgr, poolBase, kPoolEntries, kPoolStride);
}

// The pool id of an item, or -1 when it belongs to no pool we have seen.
int32_t PoolIdOf(uint32_t item, unsigned* whichPool)
{
    if (!item)
        return -1;
    std::lock_guard<std::mutex> lock(g_poolMu);
    for (unsigned i = 0; i < g_poolCount; i++)
    {
        const uint32_t b = g_pools[i].base;
        if (item < b)
            continue;
        const uint32_t off = item - b;
        if (off % kPoolStride)
            continue;
        const uint32_t id = off / kPoolStride;
        if (id >= kPoolEntries)
            continue;
        *whichPool = i;
        return int32_t(id);
    }
    return -1;
}

int InvPlayer(uint32_t inv)
{
    for (uint32_t i = 0; i < kMaxPlayers; i++)
        if (inv && g_invOfPlayer[i] == inv)
            return int(i);
    return -1;
}

// THE ASSUMPTION THIS FIX RESTS ON, AND THE ONLY ONE LEFT UNMEASURED:
// that the objective-event response runs SYNCHRONOUSLY inside the raise, on this
// thread, so the enclosing action's player is still published when the response
// asks for one. It is not obvious from the code — `sub_823E7890` hands the
// context to `sub_82188488(queue, &ctx, __FILE__, 51)`, which looks like a post,
// and the action dispatcher `sub_82378FA0` is reached only through vtables, so
// the static call graph cannot answer it either. If the response is DEFERRED to
// a later frame, the thread-local below is already restored by then and the fix
// silently does nothing.
//
// So it is instrumented rather than assumed. This counts objective-event
// responses that pass the host gate, and reports whether a
// cMissionSetChuckState ran INSIDE one — which is the whole question, and it is
// answerable on any route where a mission objective fires, not only at the bike.
thread_local unsigned t_inObjectiveEvent = 0;
uint64_t g_objEvents = 0, g_nestedStates = 0, g_nestedOutOfRange = 0;

bool ActingTraceOn()
{
    static const int on = [] {
        const char* e = std::getenv("CZ_COOP_ACTING_TRACE");
        return (e && *e && *e != '0') ? 1 : 0;
    }();
    return on == 1;
}
} // namespace

// cMissionSetChuckState::Execute(action, world, ctx, ...) — state 61 is the bike.
PPC_FUNC(sub_82409900)
{
    { static bool seen = false; if (Level()) FirstCall("sub_82409900 cMissionSetChuckState::Execute", seen); }
    // THE RESPONSE'S OWN STATE, WHICH NOTHING HAS EVER OBSERVED. Every log this
    // project holds was taken at CZ_ITEM_TRACE=1, which prints state 61 and
    // nothing else, so the player index state 34 — the bike's place animation,
    // `cMissionSetChuckState ChuckState = "34"` in missions.txt — actually
    // receives during a real placement is unmeasured. It decides between the two
    // surviving readings: the objective-event context's type tag 9 (refuted by
    // census, so something else must be reaching the action) and a real but WRONG
    // player. Printed unconditionally for 34 and 35, in range or not, because an
    // arm that only prints the case it expects cannot refute itself.
    if (PlaceTrace())
    {
        const uint32_t action = ctx.r3.u32, world = ctx.r4.u32, actionCtx = ctx.r5.u32;
        const uint32_t state = action ? PPC_LOAD_U32(action + 0x40) : 0;
        if (state == 34 || state == 35)
        {
            const int32_t idx = actionCtx ? int32_t(PPC_LOAD_U32(actionCtx + kCtxPlayer)) : -1;
            const uint32_t worldLocal = world ? PPC_LOAD_U32(world + 0x80) : 0;
            char who[64];
            DescribeActor(idx >= 0 && uint32_t(idx) < kMaxPlayers ? g_actorOfPlayer[idx] : 0, who,
                          sizeof who);
            fprintf(stderr, "[place] STATE %u (%s) ctx %08X -> ctx+0x10 = %d%s, that is %s; "
                            "world+0x80 = %d, r6=%08X, from lr %08X, dispatched from %08X (%s)\n",
                    state, Side(ctx, base), actionCtx, idx,
                    (idx < 0 || uint32_t(idx) >= kMaxPlayers) ? " <-- OUT OF RANGE" : "", who,
                    int32_t(worldLocal), ctx.r6.u32, uint32_t(ctx.lr), t_dispatchLr,
                    t_dispatchLr == 0x821899B8   ? "event class 0x6B"
                    : t_dispatchLr == 0x8218A1BC ? "event class 0x6C, index straight off the wire"
                    : t_dispatchLr == 0          ? "NOT through sub_82378FA0's entry — the "
                                                   "class-0x6A tail branch, or this hook never "
                                                   "ran; check for its `hook alive` line"
                                                 : "class 0x6A (the tail branch) or unknown");
        }
    }
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
    // The window the mission-event hook needs to tell a bike placement from
    // every other raise in the game. It is opened for state 61 only, it carries
    // the player the action will act AS (the field state 61 itself reads), and
    // it is closed unconditionally so a raise on any later frame cannot inherit
    // it. Thread-local: two mission updates on two threads would otherwise
    // share one window.
    const uint32_t action61 = ctx.r3.u32, ctx61 = ctx.r5.u32;
    // THE CANDIDATE FIX'S RECORD. State 61 is about to resolve the acting player's
    // held item and raise an event for it; the mission's response then looks the
    // prop up BY NAME half a second later and takes the lowest pool id. Remember
    // the object state 61 actually read, so that lookup can be pinned to it when —
    // and only when — it is ambiguous. Two guest calls, a few times a session.
    if (PlaceFix() && action61 && PPC_LOAD_U32(action61 + 0x40) == kChuckStateTryPlaceItem)
        RememberPlacedItem(ctx, base, ctx.r4.u32,
                           ctx61 ? int32_t(PPC_LOAD_U32(ctx61 + kCtxPlayer)) : -1);
    const bool isPlace = SyncActive() && action61 &&
                         PPC_LOAD_U32(action61 + 0x40) == kChuckStateTryPlaceItem;
    const bool wasPlacing = t_placing;
    const int32_t wasPlayer = t_placingPlayer;
    if (isPlace)
    {
        t_placing = true;
        t_placingPlayer = ctx61 ? int32_t(PPC_LOAD_U32(ctx61 + kCtxPlayer)) : -1;
    }

    // PUBLISH THE ACTING PLAYER for the duration of this action, whatever its
    // Chuck state. A context that carries an IN-RANGE player is the only
    // evidence that a real acting player exists; an out-of-range one is the
    // objective-event context's type tag and must not overwrite the enclosing
    // action's answer, so it is left alone and inherited.
    const int32_t here = ctx61 ? int32_t(PPC_LOAD_U32(ctx61 + kCtxPlayer)) : -1;
    if (t_inObjectiveEvent)
    {
        g_nestedStates++;
        const bool bad = here < 0 || here >= kMaxUserPlayers;
        if (bad)
            g_nestedOutOfRange++;
        if (ActingTraceOn())
        {
            static uint64_t said = 0;
            if (++said <= 30)
                fprintf(stderr, "[acting] SetChuckState state %u ran INSIDE an objective-event "
                                "response (depth %u), ctx+0x10 = %d%s\n",
                        PPC_LOAD_U32(action61 + 0x40), t_inObjectiveEvent, here,
                        bad ? "  <-- OUT OF RANGE: the fix engages here" : "");
        }
    }
    const int32_t wasActing = t_actingPlayer;
    if (here >= 0 && here < kMaxUserPlayers)
    {
        t_actingPlayer = here;
        RecordActing(here);
    }

    __imp__sub_82409900(ctx, base);

    t_actingPlayer = wasActing;
    if (isPlace)
    {
        t_placing = wasPlacing;
        t_placingPlayer = wasPlayer;
    }
}

// GetUserPlayer(userPlayerArray, index) — `0x8247B020`. THE POINT OF THE DEFECT:
// the title bounds the index, logs an assert that a shipped build silences, and
// then performs the load anyway. This is the one place the out-of-bounds read
// can be stopped without touching guest memory: hand the guest a VALID index and
// let it do its own lookup.
// ---------------------------------------------------------------------------
// THE HARNESS/CENSUS DRIVER — and a defect in an instrument that has shipped
// since the first co-op session.
// ---------------------------------------------------------------------------
//
// The inventory watch, the harness and the census were all driven from
// `cMissionOnTrigger::Update` (`sub_823E79B8`) on the reasoning that it "already
// runs every frame for every mission trigger in the level". MEASURED, in the
// safehouse garage at Case 0-4, that hook is entered **once**: the harness's own
// counter printed `0s elapsed, 1 mission updates seen` and never printed again,
// two minutes after the level was up. So in a level whose missions are not
// trigger-driven the watch is armed and silent, which is gotcha 30 in the
// instrument this investigation has leaned on hardest — and it is why a counter
// belongs on every driver and not only on every arm.
//
// The replacement driver is `GetUserPlayer` (`sub_8247B020`), which the census in
// this very file measured at **5,820,000 calls in 240 s**. It is a guest thread
// with a usable stack, and `ResolveWorld` is self-contained, so nothing else is
// needed. Throttled to one resolve per period, with a thread-local reentrancy
// guard because `ResolveWorld` makes guest calls that can re-enter this hook —
// without the guard that is unbounded recursion rather than a wrong number.
void HarnessTick(PPCContext& ctx, uint8_t* base)
{
    if (!RaiseArmed() && PoolCensusMs() <= 0)
        return;
    static thread_local bool inTick = false;
    if (inTick)
        return;
    static std::mutex mu;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mu, std::try_to_lock);
    if (!lock.owns_lock() || now < next)
        return;
    next = now + std::chrono::milliseconds(200);
    inTick = true;
    const char* why = "";
    const uint32_t world = ResolveWorld(ctx, base, why);
    MaybeRaise(ctx, base, world);
    PoolCensus(ctx, base, world);
    inTick = false;
}

PPC_FUNC(sub_8247B020)
{
    const int32_t idx = int32_t(ctx.r4.u32);
    // AN UNCONDITIONAL COUNTER, because the operator's run printed nothing and
    // "the index is never out of range" and "this hook is dead" are the same
    // silence otherwise — gotcha 151. With the census, 0 substitutions is a
    // MEASUREMENT instead of an absence.
    if (ActingTraceOn())
    {
        static std::atomic<uint64_t> calls{0}, outOfRange{0};
        const uint64_t n = ++calls;
        if (idx < 0 || idx >= kMaxUserPlayers)
            ++outOfRange;
        if ((n % 20000) == 0)
            fprintf(stderr, "[acting] GetUserPlayer census: %llu calls, %llu out of range\n",
                    (unsigned long long)n, (unsigned long long)outOfRange.load());
    }
    HarnessTick(ctx, base);
    if (idx < 0 || idx >= kMaxUserPlayers)
    {
        const int mode = ActingPlayerMode();
        long long ageMs = -1;
        int32_t acting = t_actingPlayer;
        const bool onStack = acting >= 0;
        if (!onStack)
            acting = RecentActing_Get(&ageMs);
        if (mode >= 1 && acting >= 0 && mode < 2)
        {
            // Once per (bad index -> acting player) pair, then counted. The
            // first line is what proves the fix engaged at all; a silent fix is
            // indistinguishable from an absent one.
            static std::mutex saidMu;
            static uint32_t saidPairs[16] = {};
            static unsigned saidCount = 0;
            const uint32_t pair = (uint32_t(idx) << 8) | uint32_t(acting);
            bool fresh = true;
            {
                std::lock_guard<std::mutex> lock(saidMu);
                for (unsigned i = 0; i < saidCount; i++)
                    if (saidPairs[i] == pair)
                        fresh = false;
                if (fresh && saidCount < 16)
                    saidPairs[saidCount++] = pair;
                g_actingSubs++;
            }
            // Deliberately NOT Side(): that resolves session objects, and this
            // runs inside a mission action's own call stack. A log line is not
            // worth a re-entry.
            if (fresh && onStack)
                fprintf(stderr, "[acting] user-player index %d is out of range "
                                "(MAX_USER_PLAYERS=%d) and would read past the end of the array "
                                "— giving it acting player %d, found ON THE STACK (so this "
                                "dispatch was synchronous)\n",
                        idx, kMaxUserPlayers, acting);
            else if (fresh)
                fprintf(stderr, "[acting] user-player index %d is out of range "
                                "(MAX_USER_PLAYERS=%d) and would read past the end of the array "
                                "— giving it acting player %d from the RECORD, set %lld ms ago "
                                "(so the dispatch is DEFERRED)\n",
                        idx, kMaxUserPlayers, acting, ageMs);
            ctx.r4.u64 = uint32_t(acting);
        }
        else if (mode >= 2)
        {
            static uint64_t seen = 0;
            if (++seen <= 20 || (seen % 500) == 0)
                fprintf(stderr, "[acting] OBSERVE: out-of-range user-player index %d (acting "
                                "player %d, %s, occurrence %llu) — NOT substituted\n",
                        idx, acting, onStack ? "on the stack" : "from the record",
                        (unsigned long long)seen);
        }
    }
    __imp__sub_8247B020(ctx, base);
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
    // The harness, outside the trace gate because it is armed by its own variable
    // and has to run for a session that does not want the whole item trace.
    if (RaiseArmed() || PoolCensusMs() > 0)
    {
        const char* why = "";
        const uint32_t w = ResolveWorld(ctx, base, why);
        MaybeRaise(ctx, base, w);
        PoolCensus(ctx, base, w);
    }
    // Outside the trace gate: this is the FIX, not an instrument, and it has to
    // run for a player who never sets CZ_ITEM_TRACE. It is a straight return
    // when there is no co-op session.
    PublishHeldItem(ctx, base);
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
    // THE SUBSTITUTION (CZ_COOP_ITEM_SYNC). Inside a state-61 placement, for a
    // player who is REMOTE on this machine, the hash the title just computed
    // came out of a local inventory copy that has drifted. Replace it with the
    // event for the item the OWNING machine says that player is holding.
    //
    // Three guards, and each of them is the difference between a repair and a
    // new defect: the raise must be one of the six state 61 can produce (so an
    // unrelated mission event raised from inside the same call is untouched),
    // the remote value must name one of the five parts (so a peer that is
    // holding nothing, or something else entirely, cannot cause a placement),
    // and the substitution must actually change the answer (so the ordinary
    // agreeing case is silent and costs nothing).
    if (SyncActive() && t_placing && IsPlacementEvent(hash))
    {
        const int local = LocalPlayerIndexCached();
        const int32_t who = t_placingPlayer;
        if (local >= 0 && who >= 0 && uint32_t(who) < kMaxPlayers && who != local)
        {
            uint32_t remoteItem = 0;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lock(g_heldMu);
                const RemoteHeld& r = g_remoteHeld[who];
                if (r.valid)
                {
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - r.at)
                                         .count();
                    fresh = age <= SyncMaxAgeMs();
                    remoteItem = r.hash;
                }
            }
            const uint32_t want = fresh ? EventForPart(remoteItem) : 0;
            if (want && want != hash)
            {
                fprintf(stderr, "[itemsync] placement by player %d (remote here): this machine "
                                "read %08X (%s) out of its own copy, player %d's own machine "
                                "says %08X (%s) -> raising %08X (%s)\n",
                        who, hash, NameOf(hash), who, remoteItem, NameOf(remoteItem), want,
                        NameOf(want));
                ctx.r4.u64 = want;
            }
            else if (!fresh)
            {
                fprintf(stderr, "[itemsync] placement by player %d (remote here): NO FRESH "
                                "held-item from that machine — the title's own answer %08X (%s) "
                                "stands. Is the peer running this build, and is the channel up?\n",
                        who, hash, NameOf(hash));
            }
            else if (!want)
            {
                // The far machine says that player holds something that is not one
                // of the five parts — including "nothing at all" (hash 0). This
                // used to be silent, and it is the case that made the 09-25 logs
                // unreadable: it looks exactly like agreement.
                fprintf(stderr, "[itemsync] placement by player %d (remote here): that machine "
                                "says it holds %08X (%s), which is NOT a bike part — the title's "
                                "own answer %08X (%s) stands\n",
                        who, remoteItem, NameOf(remoteItem), hash, NameOf(hash));
            }
            else
            {
                fprintf(stderr, "[itemsync] placement by player %d (remote here): both machines "
                                "agree on %08X (%s) — nothing to substitute\n",
                        who, hash, NameOf(hash));
            }
        }
    }
    __imp__sub_821AFE48(ctx, base);
}

// The receive half of the side channel (kernel/coop_link.h). Runs on whichever
// guest thread was draining the punched path, under the socket layer's own
// lock, so it does exactly one thing: validate and file. Nothing here calls
// back into the network, into guest code, or into anything that could block.
void CoopLink_Deliver(uint64_t fromXuid, const void* data, size_t length)
{
    (void)fromXuid;
    if (length < kSyncBytes)
        return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (GetBE32(p) != kSyncMagic || p[4] != kSyncVersion)
        return;

    const uint32_t who = p[5];
    const bool senderIsHost = p[6] != 0;
    const uint32_t hash = GetBE32(p + 8);
    const uint32_t seq = GetBE32(p + 12);
    if (who >= kMaxPlayers)
        return;

    // THE CHECK THAT KEEPS THE INDEX ASSUMPTION HONEST. Each side derives its
    // own player index from which end of the session it is, so two machines
    // that both believe they are the host would both publish index 0 and each
    // would overwrite the other's view of its own player. That is a
    // misconfiguration rather than a race, it would be invisible as a wrong
    // part rather than as an error, and it costs one byte to refuse.
    const int ourSide = g_ourSide.load(std::memory_order_relaxed);
    if (ourSide >= 0 && senderIsHost == (ourSide == 1))
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[itemsync] REFUSED a held-item message from a peer that claims the "
                            "same side of the session as this machine (both %s). Item sync is "
                            "off: the player indices cannot be trusted.\n",
                    senderIsHost ? "host" : "joiner");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(g_heldMu);
    RemoteHeld& r = g_remoteHeld[who];
    // Unordered transport: an older datagram overtaking a newer one must not
    // roll the value back. Wraparound is handled by the signed difference.
    if (r.valid && int32_t(seq - r.seq) <= 0)
        return;
    r.hash = hash;
    r.seq = seq;
    r.at = std::chrono::steady_clock::now();
    const bool first = !r.valid;
    r.valid = true;
    g_syncFiled++;
    // THE BLIND SPOT THIS CLOSES. The first version logged nothing on receipt, so
    // "the peer is publishing and we are filing it" and "nothing has ever arrived"
    // printed exactly the same thing — and the 09-25 run could not be read because
    // of it. Once when the channel first carries anything, then only on change.
    uint32_t& lastLogged = g_lastLoggedIn[who];
    if (first || hash != lastLogged)
    {
        lastLogged = hash;
        fprintf(stderr, "[itemsync] <- player %u holds %08X (%s)%s\n", who, hash, NameOf(hash),
                first ? " — the inbound channel is alive" : "");
    }
}

// -- the self-test (kernel/coop_link.h) --------------------------------------
//
// Every case here is one the live path depends on and none of them is reachable
// on one machine, so without this the whole receive half ships unexecuted. Each
// check is written so that BREAKING the implementation makes it fail: the
// sequence test feeds a stale datagram and requires the old value to survive,
// the bound test feeds player 9 and requires nothing to be filed, and the
// same-side test requires a REFUSAL rather than merely "no crash".
namespace
{
int g_syncTestFailures = 0;

void SyncExpect(bool ok, const char* what)
{
    if (!ok)
    {
        g_syncTestFailures++;
        fprintf(stderr, "[itemsync] SELF-TEST FAILED: %s\n", what);
    }
}

void MakeSyncMsg(uint8_t* out, uint8_t player, bool host, uint32_t hash, uint32_t seq,
                 uint32_t magic = kSyncMagic, uint8_t version = kSyncVersion)
{
    PutBE32(out + 0, magic);
    out[4] = version;
    out[5] = player;
    out[6] = host ? 1 : 0;
    out[7] = 0;
    PutBE32(out + 8, hash);
    PutBE32(out + 12, seq);
}

uint32_t SyncFiled()
{
    std::lock_guard<std::mutex> lock(g_heldMu);
    return g_syncFiled;
}

uint32_t SyncHeldOf(uint32_t player)
{
    std::lock_guard<std::mutex> lock(g_heldMu);
    return g_remoteHeld[player].valid ? g_remoteHeld[player].hash : 0;
}

void SyncClear()
{
    std::lock_guard<std::mutex> lock(g_heldMu);
    for (RemoteHeld& r : g_remoteHeld)
        r = RemoteHeld{};
}
} // namespace

void CoopItems_SyncSelfTest()
{
    const char* env = std::getenv("CZ_COOP_ITEM_SYNC_TEST");
    if (!env || !*env || *env == '0')
        return;

    g_syncTestFailures = 0;
    const int savedSide = g_ourSide.load(std::memory_order_relaxed);
    SyncClear();

    // The tables. Five parts, five distinct events, and the sixth event the
    // title can raise is NoPartsPlaced — which must be recognised as a
    // placement (so it can be replaced) but must never be produced.
    for (const PartEvent& p : kPartEvents)
    {
        SyncExpect(EventForPart(p.item) == p.event, "EventForPart does not round-trip its table");
        SyncExpect(IsPlacementEvent(p.event), "a part's event is not a placement event");
        SyncExpect(p.event != kNoPartsPlaced, "a part maps to NoPartsPlaced");
    }
    SyncExpect(IsPlacementEvent(kNoPartsPlaced), "NoPartsPlaced is not a placement event");
    SyncExpect(EventForPart(kNoPartsPlaced) == 0, "NoPartsPlaced resolves as a held part");
    SyncExpect(EventForPart(0) == 0, "an empty hand resolves as a held part");
    SyncExpect(EventForPart(0xDEADBEEFu) == 0, "an unrelated item resolves as a bike part");
    SyncExpect(!IsPlacementEvent(0xDEADBEEFu), "an unrelated event counts as a placement");

    // We are the host for the rest of this, so a joiner's messages are the ones
    // that must be accepted.
    g_ourSide.store(1, std::memory_order_relaxed);
    uint8_t msg[kSyncBytes];

    MakeSyncMsg(msg, 1, /*host*/ false, 0x878FC97Bu, 10);
    CoopLink_Deliver(1, msg, sizeof msg);
    SyncExpect(SyncHeldOf(1) == 0x878FC97Bu, "a well-formed message was not filed");

    // Unordered transport: an older datagram must not roll the value back.
    MakeSyncMsg(msg, 1, false, 0x5F8D0521u, 9);
    CoopLink_Deliver(1, msg, sizeof msg);
    SyncExpect(SyncHeldOf(1) == 0x878FC97Bu, "a STALE message overwrote a newer one");

    MakeSyncMsg(msg, 1, false, 0x5F8D0521u, 11);
    CoopLink_Deliver(1, msg, sizeof msg);
    SyncExpect(SyncHeldOf(1) == 0x5F8D0521u, "a newer message was not accepted");

    // Malformed input must be refused rather than filed or read past.
    {
        const uint32_t before = SyncFiled();
        MakeSyncMsg(msg, 1, false, 0x52EA0EA6u, 12, /*magic*/ 0x12345678u);
        CoopLink_Deliver(1, msg, sizeof msg);
        MakeSyncMsg(msg, 1, false, 0x52EA0EA6u, 13, kSyncMagic, /*version*/ 99);
        CoopLink_Deliver(1, msg, sizeof msg);
        MakeSyncMsg(msg, 1, false, 0x52EA0EA6u, 14);
        CoopLink_Deliver(1, msg, sizeof msg - 1);
        SyncExpect(SyncFiled() == before, "a malformed message was filed");
        SyncExpect(SyncHeldOf(1) == 0x5F8D0521u, "a malformed message changed the held item");
    }

    // A player index the game does not have must not be written ANYWHERE, and
    // that cannot be checked by reading the slots: the failure it guards
    // against is a write past the end of the array, which no in-range value
    // test can see. The filed COUNT can.
    {
        const uint32_t before = SyncFiled();
        MakeSyncMsg(msg, 9, false, 0xC32E815Bu, 20);
        CoopLink_Deliver(1, msg, sizeof msg);
        SyncExpect(SyncFiled() == before, "an out-of-range player index was filed");
        for (uint32_t i = 0; i < kMaxPlayers; i++)
            SyncExpect(SyncHeldOf(i) != 0xC32E815Bu, "an out-of-range message landed in a slot");
    }

    // The same-side refusal: a peer claiming to be the host while we are.
    SyncClear();
    {
        const uint32_t before = SyncFiled();
        MakeSyncMsg(msg, 0, /*host*/ true, 0xA55F8BABu, 30);
        CoopLink_Deliver(1, msg, sizeof msg);
        SyncExpect(SyncFiled() == before && SyncHeldOf(0) == 0,
                   "a message from a peer on OUR side of the session was filed");
    }

    SyncClear();
    g_ourSide.store(savedSide, std::memory_order_relaxed);
    if (g_syncTestFailures == 0)
        fprintf(stderr, "[itemsync] self-test: the held-item contract holds (tables, encoding, "
                        "ordering, bounds, side check)\n");
    else
        fprintf(stderr, "[itemsync] self-test: %d FAILURE(S)\n", g_syncTestFailures);
}

// cMissionObjectiveEvent's event handler (`0x823E7890`) — the response dispatch.
// Hooked ONLY to answer whether the actions it runs are synchronous, because the
// whole acting-player fix depends on that and nothing else can settle it: the
// context goes to what looks like a queue, and the action dispatcher is virtual.
PPC_FUNC(sub_823E7890)
{
    ++t_inObjectiveEvent;
    const uint64_t statesBefore = g_nestedStates;
    __imp__sub_823E7890(ctx, base);
    --t_inObjectiveEvent;

    ++g_objEvents;
    if (ActingTraceOn())
    {
        static uint64_t said = 0;
        const bool ranSomething = g_nestedStates != statesBefore;
        // Say it the first few times, then only when a response actually ran a
        // state change — the interesting event. A response that runs NOTHING
        // synchronously is the answer that kills the fix, so it must be visible
        // too, not only its opposite.
        if (++said <= 20 || ranSomething)
            fprintf(stderr, "[acting] objective-event response returned: %s a "
                            "cMissionSetChuckState synchronously (responses %llu, nested states "
                            "%llu, of which out of range %llu)\n",
                    ranSomething ? "RAN" : "did NOT run", (unsigned long long)g_objEvents,
                    (unsigned long long)g_nestedStates, (unsigned long long)g_nestedOutOfRange);
    }
}

// ===========================================================================
// THE PICKUP TRACE — CZ_COOP_PICKUP_TRACE=1
// ===========================================================================
//
// WHY THIS EXISTS, and why it is an INSTRUMENT and not another fix. Three
// attempts at the placement half of issue #9 were built from inference and all
// three were refuted by the operator's own sessions. The reason is structural,
// not carelessness: **this engine's interesting paths are virtual**, so a static
// call graph dead-ends on exactly the questions that matter. `sub_8224AE20` and
// `sub_8224AFB8` — the two callers of the give-item path — have ZERO static
// callers, as does the mission action dispatcher `sub_82378FA0`. Reading upward
// cannot work here. So: measure at the point of the write and print the caller.
//
// THE HOOK POINT. `sub_821A7550(inv, slot, item)` is `Inventory::InsertItemAt`,
// and it is unambiguous from its own body: it bounds `slot` at 12, shifts the
// slots above it up by one (`0x821A757C`-`0x821A7598`), stores the item at
// `inv + slot*8 + 4` (`0x821A75A4`) and increments the count at `inv + 0x64`.
// Every item that enters an inventory by any route comes through here.
//
// WHAT IT IS FOR. The operator's report, corrected by them: picking up a bike
// part as the guest puts **an item the HOST had already taken** — a shed key —
// into play, not a random one. That is the item-pool free list drifting
// (`sub_8223B000` pops ids from a 2,048-entry LIFO private to each machine), and
// it predicts exactly this: the far machine resolves an incoming id against its
// own table and finds whatever it allocated there.
//
// So run this on BOTH machines and do ONE pickup. The guest's log will show a
// `GasolineCanister` inserted; the host's will show whatever its own table had at
// that id. The `lr` names the code that chose it, which is the thing three
// sessions of reading upward could not find. Pair it with `CZ_ITEM_WATCH_MS`,
// whose `INV BASELINE` lines map an inventory address to a player.
PPC_FUNC(sub_821A7550)
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_PICKUP_TRACE");
        const bool v = e && *e && *e != '0';
        if (v)
            fprintf(stderr, "[pickup] CZ_COOP_PICKUP_TRACE=1 — every insertion into any "
                            "inventory, with the item's name hash and the CALLER. Pair with "
                            "CZ_ITEM_WATCH_MS to map an inventory to a player.\n");
        return v;
    }();
    if (on)
    {
        const uint32_t inv = ctx.r3.u32;
        const int32_t slot = int32_t(ctx.r4.u32);
        const uint32_t item = ctx.r5.u32;
        const uint32_t hash = item ? PPC_LOAD_U32(item + kItemNameHash) : 0;
        unsigned pool = 0;
        const int32_t id = PoolIdOf(item, &pool);
        const int player = InvPlayer(inv);
        char who[16];
        if (player >= 0)
            std::snprintf(who, sizeof who, "player %d", player);
        else
            std::snprintf(who, sizeof who, "player ?");
        char idbuf[48];
        if (id >= 0)
            std::snprintf(idbuf, sizeof idbuf, "POOL %u ID %d", pool, id);
        else
            std::snprintf(idbuf, sizeof idbuf, "in NO known pool");
        // THE POOL ID IS THE FIELD THIS TRACE EXISTS FOR. Two machines can be
        // compared on it and on nothing else here: the addresses are per-run, the
        // slot is local bookkeeping, and the hash is the ANSWER rather than the
        // key. If the same id names a different hash on the two sides, the free
        // list has drifted and that is the defect, stated in one line.
        fprintf(stderr, "[pickup] %s inv %08X slot %d <- item %08X %s, hash %08X (%s), count %u, "
                        "from lr %08X\n",
                who, inv, slot, item, idbuf, hash, NameOf(hash),
                inv ? PPC_LOAD_U32(inv + 0x64) : 0u, uint32_t(ctx.lr));
    }
    __imp__sub_821A7550(ctx, base);
}

// ===========================================================================
// THE EFFECT PATH'S HOOKS. Three calls that can take an item off a Chuck, each
// printing the ACTOR and the CALLER. See PlaceTrace() above for why this shape
// and not another reading of the call graph.
// ===========================================================================

// Inventory::RemoveItemAt(inv, slot, adjustSelected) — `0x821A75B8`.
//
// THE ONE MEASUREMENT THE WHOLE ISSUE IS WAITING FOR. Four candidate mechanisms
// died because each answered "who loses the item?" by reading code; this answers
// it by watching the store. The item is read at `inv + slot*8 + 4` BEFORE the
// guest runs, because the guest's first act on the taken slot is to shift the
// ones above it down and zero the tail — read afterwards, the answer is gone.
//
// What to look for with two machines and ONE placement by the guest:
//   * `player 1` loses the part on both machines  -> the effect is correct and
//     the defect is the ATTACH, not the removal;
//   * `player 0` loses an item on the HOST        -> the effect is applied to
//     the local player and the `lr` names where that player was chosen. That is
//     the operator's *"it makes the first player drop his currently held item"*,
//     photographed at the instruction that does it.
PPC_FUNC(sub_821A75B8)
{
    if (PlaceTrace())
    {
        static bool seen = false;
        FirstCall("sub_821A75B8 Inventory::RemoveItemAt", seen);
        const uint32_t inv = ctx.r3.u32;
        const int32_t slot = int32_t(ctx.r4.u32);
        const uint32_t adjustSel = ctx.r5.u32 & 0xFF;
        const uint32_t item = (inv && slot >= 0 && uint32_t(slot) < kInvSlots)
                                  ? PPC_LOAD_U32(inv + uint32_t(slot) * 8 + 4)
                                  : 0;
        char who[64], what[128];
        DescribeInv(inv, who, sizeof who);
        DescribeItem(base, item, what, sizeof what);
        fprintf(stderr, "[place] REMOVE %s slot %d loses %s (%s), count %u, selected %d, "
                        "adjustSelected %u, from lr %08X\n",
                who, slot, what, Side(ctx, base), inv ? PPC_LOAD_U32(inv + 0x64) : 0u,
                inv ? int32_t(PPC_LOAD_U32(inv + kInvSelected)) : -1, adjustSel,
                uint32_t(ctx.lr));
    }
    __imp__sub_821A75B8(ctx, base);
}

// The release into the world — `0x8223BBB8(mgr, actor, item, ..., r7, r8, f1)`.
//
// Identified from its one legible caller: the drop-everything loop in
// `sub_8223CEF8` calls it once per inventory slot with `(mgr->..., actor, item)`,
// so `r4` is the Chuck the item leaves and `r5` is the item. That makes it the
// standing candidate for the operator's second symptom — *"it appears next to the
// bike but is not added to the bike parts"* — an item put into the world rather
// than consumed by the placement.
//
// Stated as what it can refute: if a guest placement produces NO line here, the
// part was not released by this path and the ground copy is something else (the
// world prop the guest picked up, never removed on the host, for instance). If it
// produces one naming `player 0`, the release is being applied to the wrong Chuck
// and the `lr` says by whom.
PPC_FUNC(sub_8223BBB8)
{
    if (PlaceTrace())
    {
        static bool seen = false;
        FirstCall("sub_8223BBB8 release-item-into-world", seen);
        char who[64], what[128];
        DescribeActor(ctx.r4.u32, who, sizeof who);
        DescribeItem(base, ctx.r5.u32, what, sizeof what);
        fprintf(stderr, "[place] RELEASE %s drops %s (%s), r6=%08X r7=%08X r8=%08X, "
                        "from lr %08X\n",
                who, what, Side(ctx, base), ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                uint32_t(ctx.lr));
    }
    __imp__sub_8223BBB8(ctx, base);
}

// The drop-EVERYTHING loop — `0x8223CEF8(invMgr, actor)`, twelve iterations of
// `sub_8215D330` + `sub_8223BBB8`, i.e. empty this Chuck's whole bag into the
// world.
//
// WHY IT IS HOOKED, AND IT IS NOT AN INFERENCE ABOUT THE BIKE. It is what CHUCK
// STATE 35 runs, and state 35's handler is the one entry in the state jump table
// (`0x82043388`, verified against the image: state 61 -> 0x8240AF7C and state 34
// -> 0x8240A930, exactly as recorded) that resolves its actor from a WORLD field
// instead of the action's context:
//
//     8240A9C0  lwz r11, 0x78(r31)   ; game
//     8240A9C4  lwz r4,  0x80(r31)   ; <<-- world->0x80, NOT ctx+0x10
//     8240A9C8  lwz r3,  0x7C(r31)   ; the user-player array
//     8240A9D0  bl  0x8247B020       ; GetUserPlayer(players, world->0x80)
//     8240A9DC  bl  0x8223CEF8       ; empty THAT Chuck
//
// `world + 0x80` is the field the plan named as the standing candidate for a
// local-player index. If a guest's bike placement reaches here, the wrong Chuck
// being emptied is the whole of symptom 3 and the field is named. If it does not,
// state 35 is not involved and that candidate is closed by measurement rather
// than left open — which is worth the one hook either way (gotcha 151: an arm
// with no counter cannot be shown to have engaged).
PPC_FUNC(sub_8223CEF8)
{
    if (PlaceTrace())
    {
        static bool seen = false;
        FirstCall("sub_8223CEF8 drop-whole-inventory", seen);
        char who[64];
        DescribeActor(ctx.r4.u32, who, sizeof who);
        fprintf(stderr, "[place] DROP-ALL %s (%s), invMgr %08X, from lr %08X\n", who,
                Side(ctx, base), ctx.r3.u32, uint32_t(ctx.lr));
    }
    __imp__sub_8223CEF8(ctx, base);
}

namespace
{
// ---------------------------------------------------------------------------
// THE POOL CENSUS — CZ_COOP_POOL_CENSUS_MS. Does an instance name ever name two
// live props at once?
// ---------------------------------------------------------------------------
//
// This is the hypothesis behind CZ_COOP_PLACE_FIX stated as a number, and it can
// be read WITHOUT a placement, on either machine, at any moment. `sub_821A2200`
// resolves `PropName` to the first live pool entry whose instance name hash
// matches; if an instance name never names two entries at once, the lookup is
// exact and the whole reading is refuted. If it does, the lookup is a coin toss
// wherever the mission relies on it.
//
// It prints the FIRST TWENTY live entries once, with BOTH name fields, because
// that is the cross-check on the decode: for a scripted spawn the two differ
// (`Bike2` / `BikeBody`), and for the generic instance a player carries they
// should be equal — which is the step the fix's reasoning rests on and the one
// thing in it that had not been read off a running game.
//
// Then, on change only, it names every instance-name hash with more than one live
// entry. Costs one 2,048-entry walk per period on a mission-update thread.
int PoolCensusMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_POOL_CENSUS_MS");
        const int n = (e && *e) ? std::atoi(e) : 0;
        if (n > 0)
            fprintf(stderr, "[census] CZ_COOP_POOL_CENSUS_MS=%d — every %d ms, which instance-name "
                            "hashes name MORE THAN ONE live pool entry. That number is the whole "
                            "of the prop-lookup hypothesis: 1 everywhere refutes it.\n", n, n);
        return n;
    }();
    return ms;
}

void PoolCensus(PPCContext& ctx, uint8_t* base, uint32_t world)
{
    const int period = PoolCensusMs();
    if (period <= 0 || !world)
        return;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next)
        return;
    next = now + std::chrono::milliseconds(period);

    // The item-pool manager is the object the prop lookup is handed: game->0x20.
    const uint32_t game = LoadU32(base, world + 0x78);
    const uint32_t mgr = game ? LoadU32(base, game + 0x20) : 0;
    if (!mgr)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[census] no prop manager yet (world %08X game %08X) — retrying\n",
                    world, game);
        }
        return;
    }

    struct Live { uint32_t id, obj, name, type, holder; };
    static std::vector<Live> live;
    live.clear();
    for (uint32_t i = 0; i < kPoolEntries; i++)
    {
        const uint32_t obj = LoadU32(base, mgr + 0x30 + i * 4);
        if (!obj)
            continue;
        live.push_back({i, obj, LoadU32(base, obj + kPropNameHash),
                        LoadU32(base, obj + kItemNameHash), LoadU32(base, obj + kPropHolder)});
    }

    static bool dumped = false;
    if (!dumped && !live.empty())
    {
        dumped = true;
        fprintf(stderr, "[census] %zu live pool entries; the first 20, with BOTH name fields — "
                        "equal means the instance is generic (named after its type), unequal means "
                        "a scripted spawn:\n", live.size());
        for (size_t i = 0; i < live.size() && i < 20; i++)
            fprintf(stderr, "[census]   id %4u obj %08X instance %08X (%s) type %08X (%s) "
                            "holder %08X%s\n",
                    live[i].id, live[i].obj, live[i].name, NameOf(live[i].name), live[i].type,
                    NameOf(live[i].type), live[i].holder,
                    live[i].name == live[i].type ? "  <-- generic" : "");
    }

    // Duplicates, reported on change so a steady state is silent.
    std::string dups;
    unsigned dupNames = 0;
    for (size_t i = 0; i < live.size(); i++)
    {
        unsigned n = 0;
        bool first = true;
        for (size_t j = 0; j < live.size(); j++)
        {
            if (live[j].name != live[i].name)
                continue;
            if (j < i)
            {
                first = false;
                break;
            }
            n++;
        }
        if (!first || n < 2)
            continue;
        dupNames++;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s%08X (%s) x%u", dups.empty() ? "" : ", ", live[i].name,
                      NameOf(live[i].name), n);
        dups += buf;
    }
    static std::string lastDups = "\x01";
    if (dups != lastDups)
    {
        lastDups = dups;
        if (dupNames)
            fprintf(stderr, "[census] %u instance name(s) now name MORE THAN ONE live pool entry: "
                            "%s   <<<< every one of these makes a PropName lookup ambiguous\n",
                    dupNames, dups.c_str());
        else
            fprintf(stderr, "[census] no instance name names more than one live pool entry "
                            "(%zu live) — a PropName lookup is exact right now\n", live.size());
    }
}
} // namespace

// ---------------------------------------------------------------------------
// THE CANDIDATE FIX — CZ_COOP_PLACE_FIX. Pin the prop command to the item the
// placement actually read.
// ---------------------------------------------------------------------------
//
// READ PlaceTrace() ABOVE FIRST. This is a candidate and it ships OFF, because
// the measurement that convicts or acquits it has not been run on two machines
// yet and four fixes have already been refuted here for exactly that reason. What
// makes it worth shipping anyway is that ITS OWN LOG LINE IS THE DIAGNOSIS: it
// can only engage where `FindPropByNameHash` had MORE THAN ONE live candidate, so
// "the arm fired" and "the lookup was ambiguous" are the same event, and a run
// where it never fires has refuted the mechanism rather than merely failed.
//
// THE MECHANISM IT ADDRESSES, stated so it can be refuted.
//
//   * A prop's `+0x98` is its INSTANCE name hash and `+0x100` its item TYPE hash.
//     Measured, not assumed: a `PROPCMD 22` on the bike found
//     `id 147 ... itemHash 19CB1675` for `PropName = "Bike2"`, and
//     `tools/name_hash.py` gives `04E332D7 Bike2` / `19CB1675 BikeBody` — the
//     spawn action's own name and the item type, exactly as `missions.txt`
//     declares them.
//   * `missions.txt` contains NO spawn action named `WheelPawn`, `HandleBar`,
//     `GasolineCanister`, `BikeEngine` or `BikeForks`. Those five names appear as
//     a `PropName` in exactly five places — the five bike-part destroy commands.
//     So the prop the command looks for is not a scripted spawn: it is a GENERIC
//     item instance, whose instance name is its type name, i.e. the one a player
//     is carrying.
//   * `sub_821A2200` returns the FIRST such instance by pool id and consults
//     nothing else. One player carries one at a time, so single player is exact.
//     Two players can carry two — and the operator measured that they do, since a
//     bike part the guest took could still be picked up a second time — and then
//     the command takes the part out of whichever Chuck holds the lower pool id
//     and releases it into the world with `actor = 0`.
//
// That accounts for all three of the operator's symptoms without needing a wrong
// player index or a drifted inventory: *"it makes the first player drop his
// currently held item"* is the detach at `0x82409194` applied to the holder the
// lookup landed on, *"it appears next to the bike but is not added"* is the
// release at `0x824091B4`, and *"everything works if the host does it"* is the
// host's own instance usually being the older, lower id.
//
// THE REPAIR. State 61 already knows exactly which item object it decided on — it
// is `r30` at `0x8240AF98`, the return of `sub_821A6C18(game->0x30, actor)`. So
// remember that object across the mission's half-second timer, and when the prop
// command's lookup is AMBIGUOUS, hand back the remembered one instead of the
// lowest id. Nothing is written to guest memory; one return value of one call is
// replaced.
//
// FIVE GUARDS, each the difference between a repair and a new defect:
//   1. only inside `PropCommand = 17` (the bike's destroy), so no other prop
//      command in the game is touched;
//   2. only when the guest's own search found MORE THAN ONE live candidate, so an
//      unambiguous lookup — every lookup in single player — is bit-identical;
//   3. only when the remembered object's own `+0x98` still equals the hash asked
//      for, and only when it is still LIVE in the pool table, so a recycled id
//      cannot be resurrected;
//   4. only within CZ_COOP_PLACE_FIX_MS of the state-61 that recorded it (default
//      5000, against the mission's 500 ms timer), so a stale memory cannot steer
//      an unrelated command later;
//   5. one-way — it can only choose a DIFFERENT member of the set the title was
//      already going to choose from, never a prop outside it.
//
// THE PREDICTION, so a run can refute it: the guest places a part, the GUEST's
// Chuck loses it, the HOST keeps what he was holding, and the part is added. And
// it must be a measured null in single player — where guard 2 makes it one by
// construction, which the log states as `1 live pool entry match`.
namespace
{
struct PlacedItem
{
    uint32_t object;
    uint32_t nameHash;                                  // the instance name, +0x98
    int32_t player;
    std::chrono::steady_clock::time_point at;
};
std::mutex g_placedMu;
PlacedItem g_placed{};

int PlaceFix()
{
    static const int mode = [] {
        const char* e = std::getenv("CZ_COOP_PLACE_FIX");
        const int n = (e && *e) ? std::atoi(e) : 0;
        if (n)
            fprintf(stderr, "[placefix] CZ_COOP_PLACE_FIX=%d — the bike's destroy command will "
                            "take the item STATE 61 READ, not the lowest pool id, and ONLY where "
                            "the lookup had more than one candidate. %s\n",
                    n,
                    n >= 2 ? "=2 is OBSERVE ONLY: it reports what it would have done and changes "
                             "nothing."
                           : "=0 is the control arm and restores the shipped behaviour exactly.");
        return n;
    }();
    return mode;
}

int PlaceFixWindowMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_PLACE_FIX_MS");
        const int n = (e && *e) ? std::atoi(e) : 5000;
        return n > 0 ? n : 5000;
    }();
    return ms;
}

// Called from the state-61 branch below, with the world it was handed. Two guest
// calls, a few times a session — it is not on any frame path.
void RememberPlacedItem(PPCContext& ctx, uint8_t* base, uint32_t world, int32_t player)
{
    if (!PlaceFix() || !world || player < 0 || uint32_t(player) >= kMaxUserPlayers)
        return;
    const uint32_t userPlayers = LoadU32(base, world + 0x7C);
    const uint32_t game = LoadU32(base, world + 0x78);
    const uint32_t invMgr = game ? LoadU32(base, game + 0x30) : 0;
    if (!userPlayers || !invMgr)
        return;
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = userPlayers;
    call.r4.u64 = uint32_t(player);
    if (!GuestCall(call, base, kFnUserPlayer, "placefix-user-player"))
        return;
    const uint32_t actor = call.r3.u32;
    if (!actor)
        return;
    call.r3.u64 = invMgr;
    call.r4.u64 = actor;
    if (!GuestCall(call, base, kFnHeldItem, "placefix-held-item"))
        return;
    const uint32_t item = call.r3.u32;
    if (!item)
        return;
    std::lock_guard<std::mutex> lock(g_placedMu);
    g_placed.object = item;
    g_placed.nameHash = LoadU32(base, item + kPropNameHash);
    g_placed.player = player;
    g_placed.at = std::chrono::steady_clock::now();
    fprintf(stderr, "[placefix] state 61: player %d is holding item %08X, instance name %08X "
                    "(%s), type %08X (%s) — remembered for %d ms\n",
            player, item, g_placed.nameHash, NameOf(g_placed.nameHash),
            LoadU32(base, item + kItemNameHash), NameOf(LoadU32(base, item + kItemNameHash)),
            PlaceFixWindowMs());
}
} // namespace

// ===========================================================================
// THE PROP COMMAND — how the mission's response actually reaches an item, and
// the one step in the chain that is AMBIGUOUS BY CONSTRUCTION.
// ===========================================================================
//
// The bike's response in `missions.txt` ends with
//
//     cMissionTimer WaitforPlacement7 { DeltaTimeSecondsRealTime = "0.5"
//         cMissionSendCommandToProp Destroy3 { PropCommand = "17"
//                                              PropName = "WheelPawn" } }
//
// and the plan recorded that as *"the prop is found by NAME, so pool drift
// cannot break this one"*. Read out of the image, that reassurance is exactly
// backwards, and this is the first thing in the chain that is wrong by
// construction rather than by a missing feature.
//
// `cMissionSendCommandToProp::Execute` is `sub_82408908` — identified from the
// vtable, not guessed: the class-name accessor `0x823A8B68` sits in the table at
// `0x8204C870`, `cMissionSetChuckState`'s sits at the same slot of `0x8204D390`,
// and the two tables agree slot for slot, so slot 14 — `0x82409900` for
// SetChuckState — is `0x82408908` here. The following bytes spell `TargetNPCName`
// and `PropCommand`.
//
// Its lookup is:
//
//     82408BA8  bl  0x8276E398        ; hash PropName, the usual h = h*33 ^ c
//     82408BB8  lwz r3, 0x20(game)    ; the prop manager
//     82408BBC  bl  0x821A2200        ; FindPropByNameHash
//     82408BC0  or. r31, r3, r3
//
// and `sub_821A2200` is nine instructions long and decides nothing:
//
//     r10 = mgr + 0x30                 ; the 2,048-entry object table
//     for i in 0 .. 0x7FF:
//         obj = objTable[i]
//         if (obj && obj->0x98 == hash) return obj     <-- THE FIRST MATCH WINS
//
// `mgr + 0x30 + id*4` is the SAME object table the item pool's LIFO free list
// hands ids out of (`coop-plan.md`, "WHO ALLOCATES A POOL ENTRY"). So "find the
// prop called WheelPawn" means **"return the live pool entry with the lowest id
// whose name hash is WheelPawn"**, and it is a coin toss the moment more than one
// exists. In single player there is exactly one at a time and the lookup is
// exact, which is why this has never been seen.
//
// Then, for `PropCommand = 17`:
//
//     8240916C  bl  0x822D4870              ; holder = prop->0x1BC ...
//     82409194  game->vt[0x38C](game, holder, prop, 0)   ; TAKE IT OFF THAT HOLDER
//     824091B4  bl  0x8223BBB8(.., 0, prop, ..)          ; release it, actor = 0
//
// — so the command takes the prop **out of whoever's hands the lookup landed
// in**, and the acting player is never consulted. That is the operator's third
// symptom, *"it makes the first player drop his currently held item"*, spelled as
// two instructions, and it needs no wrong player index and no drifted inventory
// to happen: it needs only a second prop of the same name in the pool.
//
// NOTHING ABOVE IS THE FIX AND IT IS NOT YET THE DIAGNOSIS EITHER — it is a
// mechanism that predicts a NUMBER, which is the point. The hook below counts
// how many live pool entries match the hash and prints every one with its id and
// its holder, so:
//
//   * `1 match` at a co-op placement REFUTES this whole reading, on one line;
//   * `2+ matches` names it, and says which one the title took and which one the
//     placing player was actually holding.
//
// It is the shape four refuted attempts did not have: a census at the point of
// the decision, whose two outcomes are different log lines rather than the same
// silence (gotcha 151).
// The action's inline PropName. The engine's string is a small-string
// optimisation: a capacity byte at +0x20 from the string object decides whether
// the characters are inline or behind a pointer, which is the branch
// 0x82408B58-0x82408B6C reads. Printed rather than trusted, because a wrong
// decode here would attribute the command to the wrong part.
void PropNameOf(uint8_t* base, uint32_t strObj, char* out, size_t n)
{
    const uint32_t cap = LoadU8(base, strObj + 0x20);
    uint32_t chars = (cap < 0x1F) ? strObj : LoadU32(base, strObj);
    size_t i = 0;
    for (; i + 1 < n; i++)
    {
        const uint8_t c = LoadU8(base, chars + uint32_t(i));
        if (!c)
            break;
        out[i] = (c >= 32 && c < 127) ? char(c) : '?';
    }
    out[i] = 0;
}

// The window that makes the census below unconditional for the lookups that
// matter. A prop command's lookup is the one this issue turns on, and a throttle
// that hides it is the same defect as no instrument at all: the first version of
// this hook spent its whole allowance on eight zero-match lookups from an
// unrelated caller and then suppressed the "Bike2" command it was built for.
thread_local uint32_t t_propCmd = 0;          // the PropCommand currently executing, 0 = none
thread_local char t_propCmdName[64] = {};

// The mission-action dispatcher — `sub_82378FA0(action, world, ctx)`. Four useful
// instructions: name the action for a debug print, then
// `action->vt[0x38](action, world, ctx)`.
//
// Hooked ONLY to record who entered it, because part 9 recorded it as having "no
// static callers — reached only through vtables", and a census over every `b`/`bl`
// in the image finds three, all of them network event listeners gated on the class
// byte at `header + 5` in the same shape as `sub_82245650`'s `0x68`:
//
//   0x821898E0  b   -> class 0x6A (`sub_821898B0`). `ExecuteAction(event->0x14,
//                      world, event)` — the EVENT OBJECT ITSELF is the context.
//   0x821899B4  bl  -> class 0x6B. A COUNT at `event+0x10` and an array of
//                      0x1C-byte entries at `event+0x14`: several actions per
//                      message.
//   0x8218A1B8  bl  -> class 0x6C (`sub_8218A070`). Builds a stack context via
//                      `sub_8248BE28(ctx, .., 0, event->0x10)`, and that function is
//                      three stores — `+0x10 = r6`, `+0x14 = r4`, `+0x18 = r5`. So
//                      the player index the action reads comes off the wire.
//
// THE FIRST CLASS IS A TAIL BRANCH, so it does not pass this entry at all. That
// means `t_dispatchLr == 0` has TWO readings — "entered by the tail branch" and
// "this hook is not running" — and they are the same silence. Which is why this
// carries a FirstCall: once the `hook alive` line has printed, a later zero is a
// measurement.
PPC_FUNC(sub_82378FA0)
{
    if (!PlaceTrace())
    {
        __imp__sub_82378FA0(ctx, base);
        return;
    }
    static bool seen = false;
    FirstCall("sub_82378FA0 mission action dispatcher (its ENTRY; the class-0x6A path "
              "tail-branches past it)", seen);
    const uint32_t was = t_dispatchLr;
    t_dispatchLr = uint32_t(ctx.lr);
    __imp__sub_82378FA0(ctx, base);
    t_dispatchLr = was;
}

// The class-0x6A action listener — `sub_821898B0`, five useful instructions:
//
//     if (header[5] != 0x6A) return;
//     action = event->0x14;  if (!action) return;
//     ExecuteAction(action, world, event)          <-- a TAIL BRANCH, 0x821898E0
//
// **THE EVENT OBJECT IS THE ACTION'S CONTEXT**, so `event + 0x10` is the player
// index `cMissionSetChuckState::Execute` reads, and this is the listener the bike's
// response actually goes through: the first observed state 34 printed
// `dispatched from 00000000`, i.e. not through `sub_82378FA0`'s entry at all, which
// is what a tail branch looks like from a hook on that entry.
//
// So this is the last link, and the field it prints is the one the whole issue now
// turns on. On the host, with the guest placing a part: `+0x10 = 1` means the
// response is running for the right Chuck and the prop lookup is the defect;
// `+0x10 = 0` means it is running for the LOCAL Chuck and this field is. The `lr`
// says who delivered the event, which separates "the title replicated the wrong
// index" from "our layer delivered it to the wrong place".
//
// Printed for every class-0x6A event, including the ones this listener REFUSES,
// because "no event arrived" and "an event arrived for another class" are otherwise
// the same silence.
PPC_FUNC(sub_821898B0)
{
    if (PlaceTrace())
    {
        static bool seen = false;
        FirstCall("sub_821898B0 class-0x6A mission-action listener", seen);
        const uint32_t header = ctx.r4.u32, event = ctx.r5.u32;
        const uint32_t cls = header ? PPC_LOAD_U8(header + 5) : 0xFFu;
        const uint32_t action = (cls == 0x6A && event) ? PPC_LOAD_U32(event + 0x14) : 0;
        const int32_t player = (cls == 0x6A && event) ? int32_t(PPC_LOAD_U32(event + 0x10)) : -1;
        static uint64_t said = 0;
        if (cls == 0x6A || ++said <= 5)
            fprintf(stderr, "[place] EVENT class %02X (%s) event %08X -> +0x10 = %d (the player "
                            "the action will run AS), +0x14 = %08X (the action)%s, from lr %08X\n",
                    cls, Side(ctx, base), event, player, action,
                    cls != 0x6A ? "  [refused: not class 6A]"
                                : action ? "" : "  [no action, nothing runs]",
                    uint32_t(ctx.lr));
    }
    __imp__sub_821898B0(ctx, base);
}

// The class-0x6B BATCH listener — `sub_821898E8`, and the one the bike's response
// measurably goes through: the first honest `STATE 34` printed
// `dispatched from 821899B8`, which is the `bl` inside this function.
//
//     if (header[5] != 0x6B) return;
//     count = event->0x10;                        (a BYTE)
//     for i in 0 .. count-1:
//         entry = event + 0x14 + i*0x1C;          (guarded on entry->0x08 == 0x6A)
//         action = entry->0x14;  if (!action) continue;
//         ExecuteAction(action, world, entry)     <-- 0x821899B4
//
// **So a class-0x6B message is a BATCH of class-0x6A action records, 0x1C bytes each,
// and each record carries its own player index at `+0x10`** — which is what
// `cMissionSetChuckState::Execute` reads and hands to `GetUserPlayer`. The record's
// constructor (`0x82161A18`, 25 records, entry vtable `0x8200B300`) initialises that
// field to **4**, i.e. out of range for MAX_USER_PLAYERS and therefore a "nobody"
// sentinel, so a 0 in it was WRITTEN by whoever queued the action.
//
// This prints every record of every batch: the class, the player and the action. On
// the host with the guest placing a part, the record that carries the place animation
// says in one field whose Chuck the response will run for — and if it says 0 while the
// guest is player 1, the sender wrote the local player and that is the defect.
PPC_FUNC(sub_821898E8)
{
    if (PlaceTrace())
    {
        static bool seen = false;
        FirstCall("sub_821898E8 class-0x6B batch listener", seen);
        const uint32_t header = ctx.r4.u32, event = ctx.r5.u32;
        const uint32_t cls = header ? PPC_LOAD_U8(header + 5) : 0xFFu;
        static uint64_t said = 0;
        if (cls == 0x6B && event && ++said <= 200)
        {
            const uint32_t count = PPC_LOAD_U8(event + 0x10);
            fprintf(stderr, "[place] BATCH class 6B (%s) event %08X, %u record(s), from lr %08X\n",
                    Side(ctx, base), event, count, uint32_t(ctx.lr));
            for (uint32_t i = 0; i < count && i < 25; i++)
            {
                const uint32_t e = event + 0x14 + i * 0x1C;
                fprintf(stderr, "[place]   record %u at %08X: class %02X, +0x10 = %d (the player "
                                "the action runs AS), +0x14 = %08X (the action)%s\n",
                        i, e, PPC_LOAD_U32(e + 8), int32_t(PPC_LOAD_U32(e + 0x10)),
                        PPC_LOAD_U32(e + 0x14),
                        int32_t(PPC_LOAD_U32(e + 0x10)) == 4
                            ? "  [4 = the constructor's \"nobody\" sentinel, never written]"
                            : "");
            }
        }
    }
    __imp__sub_821898E8(ctx, base);
}

// cMissionSendCommandToProp::Execute(action, world, ...) — `0x82408908`.
PPC_FUNC(sub_82408908)
{
    g_levelPropSeen.store(true, std::memory_order_relaxed);
    const bool on = PlaceTrace();
    const uint32_t wasCmd = t_propCmd;
    char wasName[64];
    std::memcpy(wasName, t_propCmdName, sizeof wasName);
    if (on)
    {
        static bool seen = false;
        FirstCall("sub_82408908 cMissionSendCommandToProp::Execute", seen);
        const uint32_t action = ctx.r3.u32;
        const uint32_t command = action ? PPC_LOAD_U32(action + 0xAC) : 0;
        char name[64] = "?";
        if (action)
            PropNameOf(base, action + 0x40, name, sizeof name);
        fprintf(stderr, "[place] PROPCMD %u on prop named \"%s\" (%s), action %08X, from lr %08X\n",
                command, name, Side(ctx, base), action, uint32_t(ctx.lr));
        t_propCmd = command ? command : 0xFFFFFFFFu;
        std::snprintf(t_propCmdName, sizeof t_propCmdName, "%s", name);
    }
    __imp__sub_82408908(ctx, base);
    t_propCmd = wasCmd;
    std::memcpy(t_propCmdName, wasName, sizeof t_propCmdName);
}

// FindPropByNameHash(propMgr, nameHash) — `0x821A2200`. THE CENSUS.
//
// Runs the guest's own search first, then repeats it to the END of the table so
// the log can state how many candidates there were rather than only which one
// won. Both halves read the same nine-instruction loop, so a disagreement between
// them would mean this decode is wrong — which is the cross-check that makes the
// count a measurement.
//
// Throttled to the prop commands that matter: printing every lookup in the game
// would bury the placement, so the line is emitted only when the winner is
// ambiguous, when the name is one of the five bike parts, or for the first few
// calls (which is the positive control — a hook that never prints is
// indistinguishable from one that is dead).
PPC_FUNC(sub_821A2200)
{
    const uint32_t mgr = ctx.r3.u32, hash = ctx.r4.u32;
    __imp__sub_821A2200(ctx, base);
    if (!PlaceTrace())
        return;
    static bool seen = false;
    FirstCall("sub_821A2200 FindPropByNameHash", seen);

    const uint32_t won = ctx.r3.u32;
    uint32_t matches = 0;
    char list[400];
    int n = 0;
    list[0] = 0;
    for (uint32_t i = 0; i < kPoolEntries && mgr; i++)
    {
        const uint32_t obj = LoadU32(base, mgr + 0x30 + i * 4);
        if (!obj || LoadU32(base, obj + kPropNameHash) != hash)
            continue;
        matches++;
        if (n < int(sizeof list) - 64)
            n += std::snprintf(list + n, sizeof list - size_t(n),
                               "%sid %u obj %08X holder %08X itemHash %08X%s", n ? ", " : "", i,
                               obj, LoadU32(base, obj + kPropHolder),
                               LoadU32(base, obj + kItemNameHash), obj == won ? " <-- TAKEN" : "");
    }

    // THE SUBSTITUTION. Guard 2 is the load-bearing one: an unambiguous lookup is
    // left exactly as the title computed it, which makes single player a null by
    // construction rather than by hope.
    bool substituted = false;
    uint32_t wouldBe = 0;
    if (PlaceFix() && t_propCmd == 17 && matches > 1)
    {
        std::lock_guard<std::mutex> lock(g_placedMu);
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - g_placed.at)
                             .count();
        const bool fresh = g_placed.object && g_placed.at.time_since_epoch().count() &&
                           age <= PlaceFixWindowMs();
        // Guard 3: the remembered object must still BE the thing that was asked
        // for. The pool is a LIFO free list, so an id released in between is handed
        // straight back out as something else; checking the live instance name is
        // what stops a recycled entry being resurrected.
        const bool live = fresh && g_placed.nameHash == hash &&
                          LoadU32(base, g_placed.object + kPropNameHash) == hash &&
                          LoadU32(base, g_placed.object + kPropHolder) != 0;
        if (live && g_placed.object != won)
        {
            wouldBe = won;
            if (PlaceFix() < 2)
            {
                ctx.r3.u64 = g_placed.object;
                substituted = true;
            }
            fprintf(stderr, "[placefix] PROPCMD 17 \"%s\": the title would take %08X (lowest pool "
                            "id) but state 61 read %08X out of player %d's hands %lld ms ago — "
                            "%s\n",
                    t_propCmdName, wouldBe, g_placed.object, g_placed.player, (long long)age,
                    substituted ? "SUBSTITUTED" : "OBSERVE ONLY (CZ_COOP_PLACE_FIX=2), unchanged");
        }
        else if (!live)
            fprintf(stderr, "[placefix] PROPCMD 17 \"%s\": %u candidates and NO usable memory of "
                            "the placement (object %08X, hash %08X vs %08X, age %lld ms) — left "
                            "alone\n",
                    t_propCmdName, matches, g_placed.object, g_placed.nameHash, hash,
                    (long long)age);
    }

    // ALWAYS print inside a prop command — that lookup is the subject. Otherwise
    // print an ambiguous one, a bike-part one, and the first few of anything else
    // (the positive control: a hook that never prints cannot be told from a dead
    // one).
    static uint64_t said = 0;
    const bool known = std::strcmp(NameOf(hash), "?") != 0;
    if (t_propCmd || matches > 1 || known || ++said <= 8)
    {
        char why[96] = "";
        if (t_propCmd)
            std::snprintf(why, sizeof why, " for PROPCMD %u \"%s\"",
                          t_propCmd == 0xFFFFFFFFu ? 0u : t_propCmd, t_propCmdName);
        fprintf(stderr, "[place] PROPFIND hash %08X (%s)%s -> %08X, %u live pool entr%s match (%s) "
                        "[%s], from lr %08X%s\n",
                hash, NameOf(hash), why, won, matches, matches == 1 ? "y" : "ies", Side(ctx, base),
                matches ? list : "none", uint32_t(ctx.lr),
                substituted ? "   <<<< AMBIGUOUS, and CZ_COOP_PLACE_FIX substituted the item "
                              "state 61 read"
                            : matches > 1
                                  ? "   <<<< AMBIGUOUS: the lowest pool id wins and the acting "
                                    "player was never consulted"
                                  : "");
    }
}

// The item spawner (`sub_8223B000`). Hooked only to learn where the pools are:
// its r3 is the manager, and `*(mgr + 0x6D00)` is the 2,048 x 664 record array
// every item instance lives in. Registering it here rather than guessing means
// the pickup trace can state a pool id, which is the one field that is
// comparable between two machines.
PPC_FUNC(sub_8223B000)
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_PICKUP_TRACE");
        return e && *e && *e != '0';
    }();
    if (on)
        RegisterPool(base, ctx.r3.u32);
    __imp__sub_8223B000(ctx, base);
}

// THE OBTAIN-ITEM DISPATCHER (`sub_82243060`) — where a KEY ITEM is granted.
//
// The operator's capture settled what the pickup trace could not: the shed key
// arrives in **OBJETS CLÉS**, the key-item list on the status screen, which is a
// different structure from the twelve inventory slots — which is exactly why
// `Inventory::InsertItemAt` was silent for it.
//
// Key items are a tiny, closed set. `items.txt` declares exactly TWO `cKeyItem`s
// — `Zombrex` (KeyItemID 85001) and `Key_MasterKey` (85038, the French "Clé de
// la remise") — and this function is the dispatcher that switches on that id:
// `0x822439E0` compares against `0x00014C09`, which is 85001 (and subtype 7 of
// the broadcast-event wire compares a payload word against the same constant,
// which is worth remembering). One branch builds the "ObtainKeyItem"
// notification at `0x82243A18`.
//
// IT TAKES AN OBJECT, NOT AN ID (`r4`), and derives the id from it — so this is
// the one place that can answer the question the pool-id trace raised. The
// gas-canister pickup replicated its IDENTITY correctly (both machines said
// GasolineCanister) while the pool ids differed by two, 1055 against 1053. So
// the drift is real but was not what produced the shed key. If the object handed
// to this dispatcher on the HOST is a pool entry whose id matches what the guest
// sent but whose name hash is `Key_MasterKey`, the drift IS the mechanism after
// all, one layer up. If instead the id is simply wrong, it is not.
//
// Printed on entry, before the title acts on it, so a grant that is refused
// downstream still shows.
PPC_FUNC(sub_82243060)
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_PICKUP_TRACE");
        return e && *e && *e != '0';
    }();
    if (on)
    {
        const uint32_t obj = ctx.r4.u32;
        unsigned pool = 0;
        const int32_t id = PoolIdOf(obj, &pool);
        const uint32_t hash = (obj && id >= 0) ? PPC_LOAD_U32(obj + kItemNameHash) : 0;
        char idbuf[48];
        if (id >= 0)
            std::snprintf(idbuf, sizeof idbuf, "POOL %u ID %d", pool, id);
        else
            std::snprintf(idbuf, sizeof idbuf, "not a pool item");
        // DUMP THE HEAD OF THE OBJECT rather than a field I have guessed at.
        // The verification run showed this is NOT a pool item — it is a message
        // on the guest heap (vtable 82060FD8) whose small integer fields are
        // ids, and the dispatcher's switch compares one of them against
        // 0x00014C09 (85001, Zombrex's KeyItemID). Which offset that is has not
        // been established, and guessing it wrong is how the last three days
        // went, so print the first eight words and let the co-op pair say which
        // one differs between the machines.
        // THE FIELDS ARE KNOWN NOW, READ OFF THE IMAGE — no dereferencing and
        // no guessing, which is what the previous version did.
        //
        //   8224307C  lwz   r11, 4(r4)        ; the message TYPE
        //   822430D8  cmpwi cr6, r11, 0x13
        //   822430DC  beq   cr6, 0x822439d8   ; type 0x13 is the KEY-ITEM branch
        //   822439DC  lwz   r31, 8(r31)       ; r31 is still the message
        //   822439E4  cmplw r31, 0x14C09      ; 85001 = Zombrex's KeyItemID
        //
        // So a key item is granted by message TYPE 0x13, and its id is at +8.
        // Only two exist: 85001 (Zombrex) and 85038 (Key_MasterKey, the French
        // "Cle de la remise").
        const uint32_t msgType = obj ? PPC_LOAD_U32(obj + 4) : 0;
        const uint32_t msgId = obj ? PPC_LOAD_U32(obj + 8) : 0;
        char words[220];
        int n = std::snprintf(words, sizeof words, "type %02X", msgType);
        if (msgType == kKeyItemMessageType)
            n += std::snprintf(words + n, sizeof words - size_t(n),
                               " <<< KEY ITEM GRANT, id %u (%s)", msgId,
                               msgId == 85001u   ? "Zombrex"
                               : msgId == 85038u ? "Key_MasterKey / shed key"
                                                 : "unknown key item");
        else
            for (uint32_t i = 2; i < 6 && obj; i++)
                n += std::snprintf(words + n, sizeof words - size_t(n), " +%X=%08X", i * 4,
                                   PPC_LOAD_U32(obj + i * 4));
        if (!obj)
            std::snprintf(words, sizeof words, "(null)");
        fprintf(stderr, "[keyitem] ObtainItem(this %08X, object %08X) %s hash %08X (%s) lr %08X "
                        "| %s\n",
                ctx.r3.u32, obj, idbuf, hash, NameOf(hash), uint32_t(ctx.lr), words);
    }
    __imp__sub_82243060(ctx, base);
}
