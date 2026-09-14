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
//
// CZ_COOP_FRIENDS_NATIVE=1 (2026-09-14): let the title's OWN X handler run.
// The re-route above was chosen because the native path had never been run,
// not because it had failed, and the player sees the re-route as "it searched
// for a random game" — it IS the random-search screen, filtered. The native
// path, read from the image: friends interface vt[25] has-invite? -> vt[26]
// accept / vt[27] join-by-xuid -> matchmaking vt[38](entry) -> session vt[11]
// sub_825CD500(session, 1, ?, XNKID, xuid), which stores the friend's session
// id at +0x328 and enters LIVE_STATE_SEARCHING_FOR_HOST_SESSION_BY_ID (6) ->
// XSessionSearchByID. Both halves are served by this kernel already (the
// friend's XNKID at +0x1C of XONLINE_FRIEND, XGI 0x000B001B). The arm prints
// the friend it resolved (the same walk, read-only), the handler's return,
// and the search-by-id call with its arguments and verdict.
#include <cstdio>
#include <cstdlib>
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
extern "C" PPC_FUNC(__imp__sub_825CD500);   // session vt[11]: (session, go, ?, XNKID, xuid) -> search by id
extern "C" PPC_FUNC(__imp__sub_82501880);   // frontend screen handler: (this, event, arg) — takes GameInvites
extern "C" PPC_FUNC(__imp__sub_824BC7D8);   // cFESynchronizer::UpdateInvite(this, dt): the invite machine's tick
extern "C" PPC_FUNC(__imp__sub_827F01B8);   // the frontend manager's event sink vt[1]: (sink, event, size) -> handled
void CoopJoin_RequestJoinScreen();

// The game session's handler for the LaunchFriends event is the ONLY thing
// this hook changes, and only for that event: coop_call.cpp also hooks
// sub_824C0958, so the two share one definition, here, and coop_call.cpp's
// half is called through this seam.
void CoopCall_GameSessionEvent(PPCContext& ctx, uint8_t* base);

using namespace coop;

static uint32_t g_pressStart = 0;   // the PressStart screen that took the first GameInvites

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
        static const bool native = getenv("CZ_COOP_FRIENDS_NATIVE") != nullptr;
        const char* why = nullptr;
        const uint64_t xuid = FocusedFriendXuid(ctx, base, screen, &why);
        if (native)
        {
            std::string tag = "?";
            bool joinable = false;
            uint64_t sessionId = 0;
            for (const auto& f : xlive::Client::Instance().friends())
                if (f.xuid == xuid)
                {
                    tag = f.gamertag;
                    joinable = f.presence.joinable;
                    sessionId = f.presence.session_id;
                }
            fprintf(stderr, "[coop] friends screen: X on '%s' (%016llX, %s, session %016llX) — "
                            "NATIVE path (CZ_COOP_FRIENDS_NATIVE): the title's own handler runs\n",
                    tag.c_str(), (unsigned long long)xuid, why ? why : (joinable ? "joinable" : "not joinable"),
                    (unsigned long long)sessionId);
            __imp__sub_824DC998(ctx, base);
            fprintf(stderr, "[coop] friends screen: the title's handler returned %u\n", ctx.r3.u32);
            return;
        }
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

// The session's search-by-id (vt[11] of the HW MM session, vtable 0x8208E474):
// the native friends-screen join lands here with the friend's XNKID. Traced
// only under the native arm; the title's function runs either way.
PPC_FUNC(sub_825CD500)
{
    static const bool native = getenv("CZ_COOP_FRIENDS_NATIVE") != nullptr;
    if (!native)
    {
        __imp__sub_825CD500(ctx, base);
        return;
    }
    const uint32_t session = ctx.r3.u32;
    const uint32_t go = ctx.r4.u32, arg5 = ctx.r5.u32;
    const uint64_t xnkid = ctx.r6.u64, xuid = ctx.r7.u64;
    const uint32_t stateWord = PPC_LOAD_U32(session + 0x110);
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_825CD500(ctx, base);
    fprintf(stderr, "[coop] session search-by-id (from %08X): go=%u arg5=%08X friend=%016llX self=%016llX "
                    "(+0x110 was %08X) -> %u%s\n",
            lr, go, arg5, (unsigned long long)xnkid, (unsigned long long)xuid, stateWord, ctx.r3.u32,
            stateWord ? " REFUSED: \"MM Session is unclear\"" : "");
}

// The frontend handler that takes the "GameInvites" event cFESynchronizer's
// invite machine posts once the friend's session has been found by id. Under
// the native arm, the first run faulted INSIDE it at 0x82501BBC reading a null
// object: online vt[9] -> object R; R->vt[13]() returned 0, the else branch
// took R->vt[14]() and then dereferenced the null anyway. Trace what R is and
// what its two getters return, before the title's code runs.
PPC_FUNC(sub_82501880)
{
    static const bool native = getenv("CZ_COOP_FRIENDS_NATIVE") != nullptr;
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    if (native && id && id == NameHash(ctx, base, 0x82071D8C, 11))
    {
        g_pressStart = ctx.r3.u32;
        const uint32_t G = PPC_LOAD_U32(0x82AD6E90);
        PPCContext call = ctx;
        const uint32_t R = G ? VCall(call, base, G, 0x24, 0, "online vt[9]") : 0;
        const uint32_t Rvt = R ? PPC_LOAD_U32(R) : 0;
        const uint32_t r13 = R ? VCall(call, base, R, 0x34, 0, "R vt[13]") : 0;
        const uint32_t r14 = R ? VCall(call, base, R, 0x38, 0, "R vt[14]") : 0;
        // vt[13]/vt[14] walk: for pad 0..3, sub_82808F98(R, pad) -> vt[23]() ->
        // vt[6]() is a 64-bit value compared with R+0x130 / R+0x138.
        for (uint32_t pad = 0; R && pad < 4; ++pad)
        {
            PPCContext c2 = ctx;
            c2.r3.u64 = R;
            c2.r4.u64 = pad;
            if (!GuestCall(c2, base, 0x82808F98, "service-of-pad"))
                break;
            const uint32_t svc = c2.r3.u32;
            uint32_t inner = 0;
            uint64_t val = 0;
            if (svc)
            {
                inner = VCall(c2, base, svc, 0x5C, 0, "svc vt[23]");
                if (inner)
                {
                    const uint32_t vt = LoadU32(base, inner);
                    c2.r3.u64 = inner;
                    if (GuestCall(c2, base, LoadU32(base, vt + 0x18), "inner vt[6]"))
                        val = c2.r3.u64;
                }
            }
            fprintf(stderr, "[coop]   pad %u: service %08X -> vt[23] %08X -> vt[6] = %016llX "
                            "(R+0x130=%016llX R+0x138=%016llX)\n",
                    pad, svc, inner, (unsigned long long)val,
                    (unsigned long long)PPC_LOAD_U64(R + 0x130),
                    (unsigned long long)PPC_LOAD_U64(R + 0x138));
        }
        {
            const uint32_t mgrOwner = PPC_LOAD_U32(0x82A58C60);
            PPCContext c3 = ctx;
            const uint32_t mgr = mgrOwner ? VCall(c3, base, mgrOwner, 0x3C, 0, "manager vt[15]") : 0;
            fprintf(stderr, "[coop]   screen %08X has vtable %08X; manager owner %08X -> manager %08X, "
                            "event sink %08X vtable %08X\n",
                    ctx.r3.u32, PPC_LOAD_U32(ctx.r3.u32), mgrOwner, mgr,
                    mgr ? mgr + 0xC0 : 0, mgr ? PPC_LOAD_U32(mgr + 0xC0) : 0);
        }
        fprintf(stderr, "[coop] GameInvites event {%08X %08X %08X} on screen %08X: G=%08X R=%08X "
                        "(vtable %08X, vt[13]=%08X, vt[14]=%08X) -> vt[13]()=%08X vt[14]()=%08X; "
                        "0x82A59CD4->+C byte=%u\n",
                PPC_LOAD_U32(event), id, PPC_LOAD_U32(event + 8), ctx.r3.u32, G, R, Rvt,
                Rvt ? PPC_LOAD_U32(Rvt + 0x34) : 0, Rvt ? PPC_LOAD_U32(Rvt + 0x38) : 0, r13, r14,
                PPC_LOAD_U32(0x82A59CD4) ? PPC_LOAD_U8(PPC_LOAD_U32(0x82A59CD4) + 0xC) : 0);
        // The title's friend-join fills its X_INVITE_INFO copy the wrong way
        // round — invitee slot (+0x130) = the FRIEND, inviter slot (+0x138) =
        // us (sub_825C3950 -> sub_82553D18 -> the state-6 completion) — so
        // vt[13] ("the local player the invite is for") finds nobody, and the
        // handler's else branch dereferences that null (0x82501BBC: the
        // compiler folded `this` to 0 after the check — undefined behaviour in
        // the source, a crash on the console too). Under
        // CZ_COOP_FRIENDS_INVITEINFO swap the two so the record reads as the
        // guide's "join session in progress" would: invitee = a local player.
        static const bool supply = getenv("CZ_COOP_FRIENDS_INVITEINFO") != nullptr;
        if (supply && R && !r13 && r14)
        {
            const uint64_t a = PPC_LOAD_U64(R + 0x130), b = PPC_LOAD_U64(R + 0x138);
            PPC_STORE_U64(R + 0x130, b);
            PPC_STORE_U64(R + 0x138, a);
            const uint32_t again = VCall(call, base, R, 0x34, 0, "R vt[13] after swap");
            fprintf(stderr, "[coop] swapped the invite record's xuids (%016llX <-> %016llX); "
                            "vt[13]() now %08X\n",
                    (unsigned long long)a, (unsigned long long)b, again);
        }
    }
    __imp__sub_82501880(ctx, base);
}


// Each sink's +0x18 is the screen that takes what nothing else did (its
// vt[9] — the PressStart screen's is sub_82501880, vtable 0x82073994); +4 is
// the nested-sink list (next at +8), +0xC the propagate list.
static void DumpSinkTree(uint8_t* base, uint32_t sink)
{
    std::string line;
    char buf[64];
    auto tgt = [&](uint32_t sk) {
        const uint32_t t = LoadU32(base, sk + 0x18);
        snprintf(buf, sizeof buf, " %08X[+18 %08X vt %08X]", sk, t, t ? LoadU32(base, t) : 0);
        return std::string(buf);
    };
    line += tgt(sink);
    for (uint32_t n = LoadU32(base, sink + 4), k = 0; n && k < 12; n = LoadU32(base, n + 8), ++k)
    {
        line += " >" + tgt(n);
        for (uint32_t m = LoadU32(base, n + 4), j = 0; m && j < 12; m = LoadU32(base, m + 8), ++j)
        {
            line += " >>" + tgt(m);
            for (uint32_t q = LoadU32(base, m + 4), i = 0; q && i < 12; q = LoadU32(base, q + 8), ++i)
                line += " >>>" + tgt(q);
        }
    }
    fprintf(stderr, "[coop] sink tree:%s\n", line.c_str());
}
static uint32_t g_pendingSink = 0, g_pendingEvent[4] = {0, 0, 0, 0}, g_pendingSize = 0;
static bool g_redispatching = false;

// cFESynchronizer::UpdateInvite — the invite state machine's per-frame tick
// (state at +0x80: 1 RECEIVED_INVITE, 2 wait, 3 QUITING_ONLINE_GAME, 4
// SWITCH_PROFILE_LOADING, then GETTING_DETAIL / _DONE / CHAR_LOADING ...,
// 0 idle). The friends-screen join and an accepted invite both run through it,
// and while it is running, entering gameplay is a JOIN, not a host: the
// GameplayFlow::Enter hook (coop_host.cpp) asks here before arming a host
// request. Run 6 of the native test armed hosting on the joiner and the
// joiner created its own session mid-invite.
static uint32_t g_inviteState = 0;
PPC_FUNC(sub_824BC7D8)
{
    g_inviteState = PPC_LOAD_U32(ctx.r3.u32 + 0x80);
    static const bool native = getenv("CZ_COOP_FRIENDS_NATIVE") != nullptr;
    if (native)
    {
        // The frontend state the machine waits on (sub_827E69B0: *(*(g+0x10)+0x8C)
        // of the global 0x82AD5EF8; 0xD/0xE are the values it tests).
        const uint32_t g = PPC_LOAD_U32(0x82AD5EF8);
        const uint32_t inner = g ? PPC_LOAD_U32(g + 0x10) : 0;
        const uint32_t fe = inner ? PPC_LOAD_U32(inner + 0x8C) : 0xFFFFFFFF;
        static uint32_t lastInvite = ~0u, lastFe = ~0u;
        if (g_inviteState != lastInvite || fe != lastFe)
        {
            fprintf(stderr, "[coop] invite machine: state %u, frontend state %u\n", g_inviteState, fe);
            lastInvite = g_inviteState;
            lastFe = fe;
        }
    }
    __imp__sub_824BC7D8(ctx, base);
    if (native && g_pendingSink && g_inviteState)
    {
        // Re-deliver the unhandled GameInvites (see the sink hook). The event
        // is rebuilt on our stack copy of the context's stack.
        PPCContext call = ctx;
        const uint32_t ev = uint32_t(call.r1.u32 - 0x40);
        for (int i = 0; i < 4; ++i)
            PPC_STORE_U32(ev + 4 * i, g_pendingEvent[i]);
        call.r3.u64 = g_pendingSink;
        call.r4.u64 = ev;
        call.r5.u64 = g_pendingSize;
        static unsigned dumps = 0;
        if (dumps < 3)
        {
            ++dumps;
            // Who is registered with the sink right now: the handler list at
            // sink+4 (node vt[3] handles; node+4 nested; node+8 next) and the
            // propagate list at sink+0xC.
            DumpSinkTree(base, g_pendingSink);
        }
        g_redispatching = true;
        __imp__sub_827F01B8(call, base);
        g_redispatching = false;
        if (call.r3.u32 & 0xFF)
        {
            fprintf(stderr, "[coop] re-dispatched the invite machine's GameInvites: handled "
                            "(invite state %u)\n", g_inviteState);
            g_pendingSink = 0;
        }
        else if (g_pressStart && getenv("CZ_COOP_FRIENDS_DIRECT"))
        {
            // EXPERIMENT: the screen the second post is for — the PressStart
            // screen that handled the first — is no longer the frontend's
            // top-level target (it handed over to the main menu before the
            // invite machine posted). Hand the event to its handler directly.
            call.r3.u64 = g_pressStart;
            call.r4.u64 = ev;
            call.r5.u64 = 0;
            g_redispatching = true;
            __imp__sub_82501880(call, base);
            g_redispatching = false;
            fprintf(stderr, "[coop] GameInvites handed to the PressStart screen %08X directly -> %u "
                            "(invite state %u)\n", g_pressStart, call.r3.u32, g_inviteState);
            g_pendingSink = 0;
        }
    }
    else if (!g_inviteState)
        g_pendingSink = 0;
}
uint32_t CoopFriends_InviteState() { return g_inviteState; }

// The frontend manager's event sink (manager+0xC0, vtable 0x820B8BA8), vt[1]:
// synchronous dispatch of a frontend event. Under the native arm, say where
// the invite machine's two GameInvites posts went — the first reaches the
// TitleScreen's handler (sub_82501880), the second (state 8 -> 9) did not.
PPC_FUNC(sub_827F01B8)
{
    static const bool native = getenv("CZ_COOP_FRIENDS_NATIVE") != nullptr;
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event ? PPC_LOAD_U32(event + 4) : 0;
    const bool ours = native && id && id == NameHash(ctx, base, 0x82071D8C, 11);
    const uint32_t sink = ctx.r3.u32;
    if (ours && !g_redispatching)
        DumpSinkTree(base, sink);
    __imp__sub_827F01B8(ctx, base);
    if (ours)
    {
        fprintf(stderr, "[coop] event sink: GameInvites {%08X %08X %08X} dispatched -> handled=%u\n",
                PPC_LOAD_U32(event), id, PPC_LOAD_U32(event + 8), ctx.r3.u32 & 0xFF);
        // The invite machine's second post (state 8 -> 9) lands while the
        // frontend is mid-transition and NO screen takes it: the machine then
        // sits in CHAR_LOADING for ever. Keep the event and re-dispatch it once
        // a frame from the machine's tick until a screen handles it.
        if (!(ctx.r3.u32 & 0xFF) && !g_redispatching)
        {
            g_pendingSink = sink;
            for (int i = 0; i < 4; ++i)
                g_pendingEvent[i] = PPC_LOAD_U32(event + 4 * i);
            g_pendingSize = ctx.r5.u32 ? ctx.r5.u32 : 16;
        }
    }
}
