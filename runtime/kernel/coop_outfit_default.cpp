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
#include <cstdio>
#include <cstdlib>

#include <ppc_config.h>
#include <ppc_context.h>

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
    const uint32_t saveOwner = PPC_LOAD_U32(kSaveGlobal);
    const uint32_t save = saveOwner ? PPC_LOAD_U32(saveOwner + 8) : 0;
    unsigned empty = 0;
    for (uint32_t part = 0; part < 7 && save; part++)
        empty += PieceEmpty(base, save, part) ? 1 : 0;
    if (save && empty == 0)
    {
        fprintf(stderr, "[outfit] joiner: the save carries an outfit (7 pieces) — nothing to do\n");
        return;
    }
    const uint32_t ownerRoot = PPC_LOAD_U32(kOwnerGlobal);
    const uint32_t owner = ownerRoot ? PPC_LOAD_U32(ownerRoot + 0x2C) : 0;
    const uint32_t mgrOwner = owner ? PPC_LOAD_U32(owner + 0x78) : 0;
    const uint32_t mgr = mgrOwner ? PPC_LOAD_U32(mgrOwner + 0x2C) : 0;
    if (!mgr)
    {
        fprintf(stderr, "[outfit] joiner has no save outfit (%u of 7 pieces empty) but the outfit "
                        "manager could not be reached (owner %08X) — he stays undressed\n",
                empty, owner);
        return;
    }
    call = ctx;
    call.r3.u64 = mgr;
    call.r4.u64 = kJoinerSlot;
    call.r5.u64 = kDefaultUnderRow;
    call.r6.u64 = 0;
    call.r7.u64 = 1;
    call.r8.u64 = 0;
    fprintf(stderr, "[outfit] joiner with no save outfit (%u of 7 pieces empty, save %08X): "
                    "dressing player %u in row %u (OUTFIT_DEFAULT_UNDER) the way the title "
                    "dresses a new game's player 0 (CZ_NO_DEFAULT_OUTFIT=1 is the control)\n",
            save ? empty : 7, save, kJoinerSlot, kDefaultUnderRow);
    coop::GuestCall(call, base, kFnSetOutfit, "set-outfit");
}
