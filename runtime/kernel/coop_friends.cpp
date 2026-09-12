// The JoinGame screen's "JOIN FRIENDS" row: join a friend's game. Co-op plan
// part 5 (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// DR2's JoinGame screen (data/frontend/mainmenu.big, joingame.txt) has two
// rows. "JOIN XBOX LIVE GAME" raises ACT:XboxLive, and the screen's event
// handler (sub_824DAA10) answers it by opening GameSelect in mode 1 — the
// search-and-join flow that co-op parts 3-5 proved. "JOIN FRIENDS" raises
// ACT:Friends, and the same handler answers THAT by posting "LaunchFriends"
// to the game session, whose handler (sub_824C0958 at 0x824C0C88) raises
// DlgOnFriends — the title's friends list (fecmn.big, on_friends.txt), whose
// rows are the INVITE list the pause menu uses (feature the matchmaking
// object's vt[37], "invite") and not a way to join anyone. On the console the
// row's real work was the Guide's friends list ("join session in progress"),
// which this runtime does not have.
//
// The operator wants the row to join a friend's game. The cheapest correct
// shape reuses everything that already works: the row becomes the SAME
// search-and-join as the LIVE row, with the search kept to sessions a friend
// is hosting. libxlive's friends list names every friend's xuid; a session
// names its host; kernel/xlive_session.cpp filters the search result on that
// (XliveSession_SetFriendsOnlySearch). So:
//
//   * ACT:Friends  -> friends-only search ON, then the event is rewritten in
//                     place to ACT:XboxLive and the title's own handler runs:
//                     the same sound, the same GameSelect(mode 1) transition.
//   * ACT:XboxLive -> friends-only search OFF, then the handler runs.
//
// Everything after — the "unsaved progress" dialog, the save slot, the
// search, the QoS probe, the join, the host's prompt — is unchanged. With no
// friend hosting a public session the search comes back empty, exactly as a
// LIVE search with nobody hosting does. A friend's PRIVATE session (privacy
// "SetToPrivate", flags 0x227) is not searchable and needs the invite path
// (kernel/xlive_social.cpp) instead — not this row's job.
//
// The row itself ships in joingame.txt; tools/patch_coop_menu.py leaves it
// in place (an earlier form of that tool removed it as dead, before this).
#include <cstdio>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"
#include "xlive_session.h"

extern "C" PPC_FUNC(__imp__sub_824DAA10);   // JoinGame screen: (this, event) -> handled
extern "C" PPC_FUNC(__imp__sub_8276E398);   // (name, length) -> the frontend's name hash

namespace
{
constexpr uint32_t kStrXboxLive = 0x82073B20;   // "XboxLive", 8
constexpr uint32_t kStrFriends = 0x82073B2C;    // "Friends", 7

uint32_t NameHash(PPCContext& ctx, uint8_t* base, uint32_t name, uint32_t length)
{
    PPCContext call = ctx;
    call.r3.u64 = name;
    call.r4.u64 = length;
    __imp__sub_8276E398(call, base);
    return call.r3.u32;
}
} // namespace

PPC_FUNC(sub_824DAA10)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    if (id && id == NameHash(ctx, base, kStrFriends, 7))
    {
        const uint32_t live = NameHash(ctx, base, kStrXboxLive, 8);
        fprintf(stderr, "[coop] JOIN FRIENDS: searching for a friend's game — the LIVE row's "
                        "path with the search kept to friends' sessions (event %08X -> %08X)\n",
                id, live);
        XliveSession_SetFriendsOnlySearch(true);
        PPC_STORE_U32(event + 4, live);
    }
    else if (id && id == NameHash(ctx, base, kStrXboxLive, 8))
    {
        XliveSession_SetFriendsOnlySearch(false);
    }
    __imp__sub_824DAA10(ctx, base);
}
