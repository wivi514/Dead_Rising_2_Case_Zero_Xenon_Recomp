// A JOINER WITH NO SAVE IS DRESSED IN CHUCK'S DEFAULT OUTFIT. Co-op part 6, the
// invisible second player (2026-09-13).
//
// WHAT HAPPENED. A new XenonLive account joins with an empty save folder (saves are
// per profile since v1.1.0), and his Chuck is INVISIBLE on both machines: the outfit
// report the joiner sends at join carries seven EMPTY piece names (the host's trace:
// `CoopSetPart clothing B9288350 part 0..6 ''`), because nothing ever dressed the
// joiner's own player. Case Zero's level-start dressing (the code at 0x8249D924..DA48)
// reads: in a co-op session the HOST dresses both players (sub_8254BB70, rows 0 and
// 16) and a non-host does NOTHING — it relies on the joiner's SAVE having been applied
// to his player; in single player, the same code checks the save's seven piece names
// and, if any is empty, dresses player 0 in row 17 (OUTFIT_DEFAULT_UNDER). DR2 proper
// never met this: a DR2 joiner always has a save, because the story must be started
// before co-op exists. Through the launcher a brand-new account has none.
//
// THE RULE, mirrored from the title's own single-player fallback: at level start, in a
// co-op session, when this machine is NOT the host and the save's outfit is empty,
// dress the local player (session slot 1 — DR2 co-op is two players and the joiner is
// always 1) in row 17 the way 0x8249DA14..DA30 dresses player 0:
//     sub_821B5880(outfitMgr, player=1, row=17, 0, 1, 0)
// The row applier posts one change-part event per piece, which is exactly what the
// operator's manual outfit pick did (`SetPart clothing B9288350 ... young_chuck` on
// both machines, lr 821B4B74), so the host dresses him too. The pieces are Chuck's
// standard set: young_chuck head/chest/legs/face, naked hands, young_chuck_under feet
// — the outfit the operator chose by hand when asked "give him these clothes".
//
// Objects, all from the title's own globals at that site:
//   *(0x82A57428) -> +0x2C = the level/player owner (r26 there)
//   outfitMgr     = *(*(owner + 0x78) + 0x2C)
//   save data     = *(*(0x82A59CD4) + 8); piece names at +0x978 + part*0x24 (SSO)
// The hook sits on sub_82553130 ("is there a session"), the first predicate of that
// block, keyed on the return address so it runs once per level start and nowhere else.
// CZ_NO_DEFAULT_OUTFIT=1 is the control (the invisible joiner).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <ppc_config.h>
#include <ppc_context.h>

#include "content.h"
#include "coop_objects.h"
#include "memory.h"

extern "C" PPC_FUNC(__imp__sub_82553130);

namespace
{
constexpr uint32_t kLevelStartCaller = 0x8249D928;  // lr of the bl at 0x8249D924
constexpr uint32_t kOwnerGlobal = 0x82A57428;
constexpr uint32_t kSaveGlobal = 0x82A59CD4;
constexpr uint32_t kFnIsHost = 0x825530D0;
constexpr uint32_t kFnSetOutfit = 0x821B5880;
constexpr uint32_t kDefaultUnderRow = 17;            // OUTFIT_DEFAULT_UNDER
constexpr uint32_t kJoinerSlot = 1;

bool PieceEmpty(uint8_t* base, uint32_t save, uint32_t part)
{
    const uint32_t s = save + 0x978 + part * 0x24;
    const uint8_t len = PPC_LOAD_U8(s + 0x20);
    return len == 0;
}
} // namespace

// The two halves: DETECT at level start (the save is readable there, and it is the
// title's own moment for the question), APPLY when the game flow enters gameplay —
// the first attempt applied at level start and the row applier's per-piece events
// went nowhere (no SetPart ever followed): the joiner's player object does not exist
// yet at that point, and events for a player that is not there are dropped. By
// GameplayFlow::Enter both players are alive, which is where the operator's manual
// outfit pick worked from.
bool g_pendingDefault = false;
bool g_hostDressPending = false;
uint32_t g_hostDressSlot = 1;
std::chrono::steady_clock::time_point g_hostDressAt{};
// THE HOST'S HALF. The row applier dresses the joiner's own Chuck but broadcasts
// nothing (the wardrobe's put-on action is what sends PlayerPutOnClothing, and this is
// not that), and RE-SENDING the join-time outfit report is not an option: the host
// answers it with FLOW_COMMAND_DONE_OUTFIT_TRANSFER, which the joiner's flow takes as
// the join step completing — it re-ran CONNMESH_CONNECTED and the whole 87 KB state
// transfer while already in gameplay, and crashed (run 11). So the host dresses the
// remote player ITSELF: the report it receives from a save-less joiner carries seven
// EMPTY piece names (`CoopSetPart clothing ... part 0..6 ''`), and on the seventh the
// host applies the same row 17 to that player slot. Both sides end up in the same
// outfit by the same rule, and nothing extra goes over the wire.

void CoopOutfit_ApplyPendingDefault(PPCContext& ctx, uint8_t* base)
{
    if (!g_pendingDefault)
        return;
    g_pendingDefault = false;
    const uint32_t ownerRoot = PPC_LOAD_U32(kOwnerGlobal);
    const uint32_t owner = ownerRoot ? PPC_LOAD_U32(ownerRoot + 0x2C) : 0;
    const uint32_t mgrOwner = owner ? PPC_LOAD_U32(owner + 0x78) : 0;
    const uint32_t mgr = mgrOwner ? PPC_LOAD_U32(mgrOwner + 0x2C) : 0;
    if (!mgr)
    {
        fprintf(stderr, "[outfit] joiner has no save outfit but the outfit manager could not be "
                        "reached (owner %08X) — he stays undressed\n", owner);
        return;
    }
    PPCContext call = ctx;
    call.r3.u64 = mgr;
    call.r4.u64 = kJoinerSlot;
    call.r5.u64 = kDefaultUnderRow;
    call.r6.u64 = 0;
    call.r7.u64 = 1;
    call.r8.u64 = 0;
    fprintf(stderr, "[outfit] joiner with no save outfit: dressing player %u in row %u "
                    "(OUTFIT_DEFAULT_UNDER) at gameplay entry, the way the title dresses a new "
                    "game's player 0 (CZ_NO_DEFAULT_OUTFIT=1 is the control)\n",
            kJoinerSlot, kDefaultUnderRow);
    coop::GuestCall(call, base, kFnSetOutfit, "set-outfit");
}

void CoopOutfit_OnReportPiece(PPCContext& ctx, uint8_t* base, uint32_t clothing, uint32_t part,
                              bool nameEmpty)
{
    static const bool off = getenv("CZ_NO_DEFAULT_OUTFIT") != nullptr;
    static uint32_t lastClothing = 0;
    static unsigned emptyCount = 0;
    if (off || part >= 7)
        return;
    if (part == 0 || clothing != lastClothing)
    {
        lastClothing = clothing;
        emptyCount = 0;
    }
    emptyCount += nameEmpty ? 1 : 0;
    if (part != 6 || emptyCount < 7)
        return;
    // Seven empty pieces: the sender has no outfit. Which slot is he? The report is
    // about a REMOTE player, and DR2 co-op has two, so the slot is the one that is
    // not ours: *(owner + 0x80) is the local index.
    const uint32_t ownerRoot = PPC_LOAD_U32(kOwnerGlobal);
    const uint32_t owner = ownerRoot ? PPC_LOAD_U32(ownerRoot + 0x2C) : 0;
    const uint32_t mgrOwner = owner ? PPC_LOAD_U32(owner + 0x78) : 0;
    const uint32_t mgr = mgrOwner ? PPC_LOAD_U32(mgrOwner + 0x2C) : 0;
    const uint32_t local = owner ? PPC_LOAD_U32(owner + 0x80) : 0;
    const uint32_t slot = local == 0 ? 1 : 0;
    if (!mgr)
    {
        fprintf(stderr, "[outfit] a player reported an EMPTY outfit but the outfit manager could "
                        "not be reached (owner %08X) — he stays undressed here\n", owner);
        return;
    }
    // NOT NOW. Applied here, during the join handshake, the row's per-piece events went
    // nowhere on the host too (SetOutfit/ApplyRow printed, no SetPart followed — run 12),
    // exactly as they did on the joiner at level start: the remote player's clothing
    // loader is not up yet. The joiner's own dressing works from GameplayFlow::Enter,
    // which the host does not pass again; so the host applies it from its per-frame
    // hook once the joiner has been in the game for a while.
    (void)mgr;
    g_hostDressSlot = slot;
    g_hostDressAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    g_hostDressPending = true;
    fprintf(stderr, "[outfit] the other player's outfit report is EMPTY (no save on his side): "
                    "slot %u will be dressed in row %u (OUTFIT_DEFAULT_UNDER) here in 10 s, the "
                    "same row his own machine applies (CZ_NO_DEFAULT_OUTFIT=1 is the control)\n",
            slot, kDefaultUnderRow);
}

// Per frame (the game session's Update hook in coop_host.cpp): the host's deferred dress.
void CoopOutfit_Tick(PPCContext& ctx, uint8_t* base)
{
    if (!g_hostDressPending || std::chrono::steady_clock::now() < g_hostDressAt)
        return;
    g_hostDressPending = false;
    const uint32_t ownerRoot = PPC_LOAD_U32(kOwnerGlobal);
    const uint32_t owner = ownerRoot ? PPC_LOAD_U32(ownerRoot + 0x2C) : 0;
    const uint32_t mgrOwner = owner ? PPC_LOAD_U32(owner + 0x78) : 0;
    const uint32_t mgr = mgrOwner ? PPC_LOAD_U32(mgrOwner + 0x2C) : 0;
    if (!mgr)
    {
        fprintf(stderr, "[outfit] cannot dress slot %u: the outfit manager could not be reached "
                        "(owner %08X)\n", g_hostDressSlot, owner);
        return;
    }
    PPCContext call = ctx;
    call.r3.u64 = mgr;
    call.r4.u64 = g_hostDressSlot;
    call.r5.u64 = kDefaultUnderRow;
    call.r6.u64 = 0;
    call.r7.u64 = 1;
    call.r8.u64 = 0;
    fprintf(stderr, "[outfit] dressing slot %u in row %u (OUTFIT_DEFAULT_UNDER) now — the other "
                    "player joined with no outfit\n", g_hostDressSlot, kDefaultUnderRow);
    coop::GuestCall(call, base, kFnSetOutfit, "set-outfit");
}

PPC_FUNC(sub_82553130)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82553130(ctx, base);
    if (lr != kLevelStartCaller || !(ctx.r3.u32 & 0xFF))
        return;
    static const bool off = getenv("CZ_NO_DEFAULT_OUTFIT") != nullptr;
    if (off)
        return;
    PPCContext call = ctx;
    if (!coop::GuestCall(call, base, kFnIsHost, "is-host"))
        return;
    if (call.r3.u32 & 0xFF)
        return;                                   // the host dresses both itself
    // THE TEST IS THE PROFILE'S SAVE FOLDER, not the save-data object's piece names:
    // the object at *(0x82A59CD4)+8 mirrors player 0's boot outfit whether or not a
    // file exists (it read "1 of 7 empty" — the face — with and without a save), so it
    // cannot tell a new account from an old one. A folder with no save in it can.
    const uint32_t saveOwner = PPC_LOAD_U32(kSaveGlobal);
    const uint32_t save = saveOwner ? PPC_LOAD_U32(saveOwner + 8) : 0;
    unsigned empty = 0;
    for (uint32_t part = 0; part < 7 && save; part++)
        empty += PieceEmpty(base, save, part) ? 1 : 0;
    if (ContentHasAnySave())
    {
        fprintf(stderr, "[outfit] joiner: this profile has a save (save data %08X, %u of 7 piece "
                        "names empty) — its own outfit stands\n", save, empty);
        return;
    }
    fprintf(stderr, "[outfit] joiner with NO SAVE in this profile (save data %08X, %u of 7 piece "
                    "names empty): the default outfit will be applied when the game flow enters "
                    "gameplay\n", save, empty);
    g_pendingDefault = true;
}
