// The JoinGame screen's "JOIN FRIENDS" row: the title's own friends list,
// and joining the friend you pick on it. Co-op plan part 5 (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// DR2's JoinGame screen (data/frontend/mainmenu.big, joingame.txt) has two
// rows. "JOIN XBOX LIVE GAME" raises ACT:XboxLive, and the screen's handler
// (sub_824DAA10) opens GameSelect in mode 1 — the search-and-join co-op parts
// 3-5 proved. "JOIN FRIENDS" raises ACT:Friends, which the same handler turns
// into a "LaunchFriends" event on the game session, whose handler
// (sub_824C0958 at 0x824C0C88) raises DlgOnFriends — the title's FRIENDS
// SCREEN (fecmn.big, on_friends.txt; class at 0x824DBFxx-0x824DD2xx): a
// scrolling list of the player's friends read from the friends enumerator
// (kernel/xlive_social.cpp serves it from libxlive, presence included), with
// A = send invite, X = "join_session_or_accept_invite", Y = gamercard, B =
// back. It is the same screen the pause menu uses to invite, and on the
// console its X button joined the friend's session in progress.
//
// Two things stood between Case Zero's player and that screen:
//
//   1. The LaunchFriends handler returns at once when the tunable
//      `enable_prolog_experience` (0x82A57BFA) is set, and Case Zero IS the
//      prologue. The byte is cleared for the duration of that one event.
//   2. The X button's join goes through the friends interface (matchmaking
//      vt[31] -> vt[25] has-invite / vt[27] join-by-xuid) and matchmaking
//      vt[38], a join path this port has never run. Rather than trust it,
//      the X press is answered with the path that IS proven: the friend the
//      cursor is on is resolved exactly as the title's handler resolves it
//      (0x824DCC54-0x824DCD38: focused row -> "c0" -> "text" -> its record ->
//      sub_825479E8 -> the entry, xuid at +8), the session search is told to
//      keep only that host (XliveSession_SetSearchHostFilter mode 2), the
//      friends screen is closed the way its own B does, and GameSelect mode
//      1 is requested for the next frame (coop_join.cpp), which searches,
//      finds that friend's game, and joins it; the host then gets the prompt
//      (coop_call.cpp). A friend who is not in a joinable public session
//      gets the title's own "Join Session Failed!" dialog, raised the way
//      the handler raises it (0x824DCDA8), before any search — libxlive's
//      friends list says who is joinable.
//
// The LIVE row resets the filter to "any". A friend's PRIVATE session
// (SetToPrivate) is not searchable and would need the invite path
// (xlive_social.cpp) — a separate job.
//
// The row itself ships in joingame.txt; tools/patch_coop_menu.py leaves it
// in place (an earlier form of that tool removed it as dead, before this).
#include <cstdio>
#include <string>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"
#include "xlive_session.h"

#include <xlive/client.h>

extern "C" PPC_FUNC(__imp__sub_824DAA10);   // JoinGame screen: (this, event) -> handled
extern "C" PPC_FUNC(__imp__sub_824C0958);   // game session: (this, event, ...) -> handled
extern "C" PPC_FUNC(__imp__sub_824DC998);   // friends screen: (this, event) -> handled
extern "C" PPC_FUNC(__imp__sub_8276E398);   // (name, length) -> the frontend's name hash
extern "C" PPC_FUNC(__imp__sub_827EA7C8);   // (list widget, index) -> row widget
extern "C" PPC_FUNC(__imp__sub_825479E8);   // (friends interface, record) -> friend entry
extern "C" PPC_FUNC(__imp__sub_827F6EE0);   // (manager, 0, 0x10): close the current screen
extern "C" PPC_FUNC(__imp__sub_824B5C50);   // (dialog hash, owner, text, 0, 0): raise a dialog
void CoopJoin_RequestJoinScreen();

// The game session's handler for the LaunchFriends event is the ONLY thing
// this hook changes, and only for that event: coop_call.cpp also hooks
// sub_824C0958, so the two share one definition, here, and coop_call.cpp's
// half is called through this seam.
void CoopCall_GameSessionEvent(PPCContext& ctx, uint8_t* base);

using namespace coop;

namespace
{
constexpr uint32_t kStrXboxLive = 0x82073B20;      // "XboxLive", 8
constexpr uint32_t kStrFriends = 0x82073B2C;       // "Friends", 7
constexpr uint32_t kStrLaunchFriends = 0x82072A00; // "LaunchFriends", 13
constexpr uint32_t kStrJoinSession = 0x82076558;   // "join_session_or_accept_invite", 29
constexpr uint32_t kStrC0 = 0x820765B8;            // "c0", 2
constexpr uint32_t kStrText = 0x820722E4;          // "text", 4
constexpr uint32_t kStrJoinFailed = 0x820765A0;    // "Join Session Failed!"
constexpr uint32_t kDlgJoinFailedHash = 0x82A6939C; // the global the handler raises with
constexpr uint32_t kTunableProlog = 0x82A57BFA;    // enable_prolog_experience
constexpr uint32_t kMatchmakingVtFriends = 0x7C;   // vt[31]: the friends interface
constexpr uint32_t kWidgetVtFindChild = 0x48;      // vt[18]: (widget, hash) -> child
constexpr uint32_t kWidgetVtValue = 0x90;          // vt[36]: list -> focused index; text -> its object

uint32_t NameHash(PPCContext& ctx, uint8_t* base, uint32_t name, uint32_t length)
{
    PPCContext call = ctx;
    call.r3.u64 = name;
    call.r4.u64 = length;
    __imp__sub_8276E398(call, base);
    return call.r3.u32;
}

// The friend under the cursor, resolved the way the title's own X handler
// resolves it. 0 with a reason when any link is missing.
uint64_t FocusedFriendXuid(PPCContext& ctx, uint8_t* base, uint32_t screen, const char** why)
{
    *why = nullptr;
    const uint32_t list = LoadU32(base, screen + 0x14);
    if (!list)
        return *why = "no list widget", 0;
    PPCContext call = ctx;
    const uint32_t index = VCall(call, base, list, kWidgetVtValue, 0, "focused-index");
    call.r3.u64 = list;
    call.r4.u64 = index;
    __imp__sub_827EA7C8(call, base);
    const uint32_t row = call.r3.u32;
    if (!row)
        return *why = "no focused row", 0;
    const uint32_t c0 = VCall(call, base, row, kWidgetVtFindChild,
                              NameHash(ctx, base, kStrC0, 2), "row c0");
    if (!c0)
        return *why = "row has no c0", 0;
    const uint32_t text = VCall(call, base, c0, kWidgetVtFindChild,
                                NameHash(ctx, base, kStrText, 4), "c0 text");
    if (!text)
        return *why = "c0 has no text", 0;
    const uint32_t obj = VCall(call, base, text, kWidgetVtValue, 0, "text value");
    const uint32_t record = obj ? LoadU32(base, obj + 4) : 0;
    const Objects o = Resolve(ctx, base);
    if (!o.matchmaking)
        return *why = "no matchmaking object", 0;
    const uint32_t iface = VCall(call, base, o.matchmaking, kMatchmakingVtFriends, 0,
                                 "friends interface");
    if (!iface)
        return *why = "no friends interface", 0;
    call.r3.u64 = iface;
    call.r4.u64 = record;
    __imp__sub_825479E8(call, base);
    const uint32_t entry = call.r3.u32;
    if (!entry)
        return *why = "no friend entry for the row", 0;
    return (uint64_t(LoadU32(base, entry + 8)) << 32) | LoadU32(base, entry + 12);
}

// The title's own failure dialog, raised the way 0x824DCDA8 raises it.
void RaiseJoinFailed(PPCContext& ctx, uint8_t* base, uint32_t screen)
{
    PPCContext call = ctx;
    const uint32_t manager = LoadU32(base, screen + 4);
    call.r3.u64 = LoadU32(base, kDlgJoinFailedHash);
    call.r4.u64 = manager ? manager + 0xC0 : 0;
    call.r5.u64 = kStrJoinFailed;
    call.r6.u64 = 0;
    call.r7.u64 = 0;
    __imp__sub_824B5C50(call, base);
}
} // namespace

// The JoinGame screen: the LIVE row resets the search filter; the Friends row
// is left to the title (it posts LaunchFriends).
PPC_FUNC(sub_824DAA10)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    if (id && id == NameHash(ctx, base, kStrXboxLive, 8))
        XliveSession_SetSearchHostFilter(0);
    else if (id && id == NameHash(ctx, base, kStrFriends, 7))
    {
        XliveSession_SetSearchHostFilter(0);
        fprintf(stderr, "[coop] JOIN FRIENDS: opening the title's friends screen (LaunchFriends "
                        "-> DlgOnFriends); X on a friend joins their game\n");
    }
    __imp__sub_824DAA10(ctx, base);
}

// The game session's event handler: LaunchFriends is let through the
// prologue gate; everything else goes to coop_call.cpp's half and the title.
PPC_FUNC(sub_824C0958)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    if (id && id == NameHash(ctx, base, kStrLaunchFriends, 13))
    {
        const uint8_t prolog = PPC_LOAD_U8(kTunableProlog);
        if (prolog)
            PPC_STORE_U8(kTunableProlog, 0);
        fprintf(stderr, "[coop] LaunchFriends: raising DlgOnFriends (enable_prolog_experience "
                        "was %u, cleared for this event)\n", prolog);
        __imp__sub_824C0958(ctx, base);
        if (prolog)
            PPC_STORE_U8(kTunableProlog, prolog);
        return;
    }
    CoopCall_GameSessionEvent(ctx, base);
}

// The friends screen: X on a friend joins their game through the proven path.
PPC_FUNC(sub_824DC998)
{
    const uint32_t screen = ctx.r3.u32;
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    if (id && id == NameHash(ctx, base, kStrJoinSession, 29))
    {
        const char* why = nullptr;
        const uint64_t xuid = FocusedFriendXuid(ctx, base, screen, &why);
        if (!xuid)
        {
            fprintf(stderr, "[coop] friends screen: join pressed but %s — letting the title's "
                            "handler run\n", why);
            __imp__sub_824DC998(ctx, base);
            return;
        }
        // Is that friend somewhere a search can find?
        std::string tag = "?";
        bool joinable = false;
        for (const auto& f : xlive::Client::Instance().friends())
            if (f.xuid == xuid)
            {
                tag = f.gamertag;
                joinable = f.presence.joinable && f.presence.session_id != 0;
            }
        if (!joinable)
        {
            fprintf(stderr, "[coop] friends screen: '%s' (%016llX) is not in a joinable session "
                            "— \"Join Session Failed!\"\n", tag.c_str(),
                    (unsigned long long)xuid);
            RaiseJoinFailed(ctx, base, screen);
            ctx.r3.u64 = 1;
            return;
        }
        fprintf(stderr, "[coop] friends screen: joining '%s' (%016llX) — closing the list, "
                        "GameSelect (mode 1) next frame, search kept to that host\n",
                tag.c_str(), (unsigned long long)xuid);
        XliveSession_SetSearchHostFilter(2, xuid);
        // Close the friends screen the way its own B does (0x824DCC24).
        PPCContext call = ctx;
        call.r3.u64 = LoadU32(base, screen + 4);
        call.r4.u64 = 0;
        call.r5.u64 = 0x10;
        __imp__sub_827F6EE0(call, base);
        CoopJoin_RequestJoinScreen();
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_824DC998(ctx, base);
}
