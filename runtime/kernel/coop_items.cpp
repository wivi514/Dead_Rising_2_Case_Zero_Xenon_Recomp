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

// Which player each inventory belongs to, cached by the watch (which already
// resolves all four every sweep). Declared here rather than beside the pool
// registry below because WatchPlayer, which fills it, comes first in this file.
// Consulting the guest for this inside the item-insert hook would mean guest
// calls on the pickup path; a cache costs nothing and a stale entry prints "?"
// rather than a wrong player.
uint32_t g_invOfPlayer[kMaxPlayers] = {};

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
            g_invOfPlayer[index] = inv;
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
            g_invOfPlayer[index] = inv;
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
