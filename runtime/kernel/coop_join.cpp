// CZ_XLIVE_JOIN=1: make this build JOIN a co-op session, through the title's
// own join path. Co-op plan part 3 (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// In Dead Rising 2 a player joins co-op from the frontend's JoinGame screen:
// its "XboxLive" row (sub_824DAA10, on the event named "XboxLive") opens the
// GameSelect screen with mode 1, and GameSelect's Update (sub_825026B0, vt[5]
// of vtable 0x82073AA8) then does exactly two things —
//
//   if (sub_824BDD10(gameSession))                     // online ready
//       sub_824BD960(gameSession, mm_info{host=0, COOP, -1, 0, 0, 0});
//
// — and waits. sub_824BD960 copies the mm_info into the session-info object,
// sets IS-COOP for a COOP game type, and for a non-host runs sub_82553B80:
// the description builder (online vt[33], flags 0x42E for a joiner, which
// Case Zero left untouched) and the session interface's join entry, which
// pushes the SEARCHING_FOR_HOST_SESSION walk. Case Zero's frontend never
// opens GameSelect in mode 1 — the JoinGame screen is unreachable from its
// menus — but the screen, its Update, and everything below it are linked.
//
// This file does what that screen's Update does, from the game session's own
// per-frame Update (sub_824C2268, where the host's recreate path also runs),
// with the same predicate the screen uses and the same mm_info, on a copied
// context. It fires when the session's LOGIN_STATE is IDLE and online is
// ready, and re-fires after a cooldown if the walk comes back to IDLE without
// connecting (a search that found no host does that), so the joiner can be
// started before the host and still get in. Everything after the call is the
// title's: the search, the QoS probe, XSessionCreate as a joiner, JoinLocal,
// the reliable-layer handshake, and whatever the client flow does next —
// which is the question part 3 exists to answer.
//
// TWO FORMS, because the first two-machine session showed the call alone is
// not enough. CZ_XLIVE_JOIN=call makes the call directly: the joiner searched,
// found the host, took its seat, opened the reliable layer and reached
// LOGIN_STATE_CONNECTED — and then sat at the main menu, because the thing
// that would have moved it into the host's level is the GameSelect screen's
// own reaction to the connection, and no screen was open; 120 s later the
// host dropped it (TYPE_EVENT_PLAYER, then "Lost connection with server").
// CZ_XLIVE_JOIN=1 (the default form) opens the GameSelect screen in mode 1
// through the frontend transition manager, exactly as the JoinGame row does
// (sub_824DAA10: sub_827F6D40(manager, hash("GameSelect"), {1, 1})), and lets
// the screen make the call and own what follows. The screen's Update fires
// the call when its "tv_45" UI event has set the pending byte (+0x548), and
// its listener registration (ctor, event type 4) is what hears the result.
//
// CZ_XLIVE_JOIN_AFTER_MS=N   do not fire before N ms of uptime (default 15000:
//                            the title screen and the sign-in have to be up)
// CZ_XLIVE_JOIN_RETRY_MS=N   cooldown between attempts (default 12000)
// CZ_XLIVE_JOIN_TRIES=N      give up after N attempts (default 0 = never)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"

extern "C" PPC_FUNC(__imp__sub_824C2268);
extern "C" PPC_FUNC(__imp__sub_825CB2A8);
extern "C" PPC_FUNC(__imp__sub_8276E398);   // (name, length) -> the frontend's name hash
extern "C" PPC_FUNC(__imp__sub_827F6D40);   // (manager, screen hash, params)
uint32_t DebugTunables_FrontendManager();

using namespace coop;

namespace
{
int g_joinMode = -1; // -1 unread; 0 off; 1 open the GameSelect screen; 2 make the call
constexpr uint32_t kStrGameSelect = 0x82071A60; // "GameSelect", 10 chars
long g_afterMs = 15000;
long g_retryMs = 12000;
long g_maxTries = 0;

long EnvLong(const char* name, long fallback)
{
    const char* e = std::getenv(name);
    if (!e || !*e)
        return fallback;
    const long v = std::strtol(e, nullptr, 10);
    return v > 0 ? v : fallback;
}

bool JoinRequested()
{
    if (g_joinMode < 0)
    {
        const char* env = std::getenv("CZ_XLIVE_JOIN");
        g_joinMode = (env && *env && *env != '0') ? (std::strcmp(env, "call") == 0 ? 2 : 1) : 0;
        if (g_joinMode)
        {
            g_afterMs = EnvLong("CZ_XLIVE_JOIN_AFTER_MS", g_afterMs);
            g_retryMs = EnvLong("CZ_XLIVE_JOIN_RETRY_MS", g_retryMs);
            g_maxTries = EnvLong("CZ_XLIVE_JOIN_TRIES", 0);
            fprintf(stderr, "[coop] CZ_XLIVE_JOIN: this build will search for and join a "
                            "co-op session %s (after %ld ms, retry every %ld ms, %s)\n",
                    g_joinMode == 1 ? "through the title's GameSelect screen"
                                    : "by calling sub_824BD960 directly",
                    g_afterMs, g_retryMs, g_maxTries ? "limited tries" : "until it connects");
        }
    }
    return g_joinMode > 0;
}

long NowMs()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return long(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

// The LIVE_STATE of the HW MM session object, read from the object itself
// (+0x118, the field sub_825CB2A8 stores to) rather than remembered from the
// setter: the machine's own reset writes IDLE without going through the
// setter ("entering LIVE_STATE_IDLE" with no "state transition to"), and a
// remembered value stayed at DELETING_SESSION for a whole run. The setter is
// hooked only to learn the object's address; the name table is at 0x829DFD40.
constexpr uint32_t kLiveStateNames = 0x829DFD40;
constexpr uint32_t kHwSessionLiveState = 0x118;
uint32_t g_hwSession = 0;

int LiveState(uint8_t* base)
{
    return g_hwSession ? int(LoadU32(base, g_hwSession + kHwSessionLiveState)) : -1;
}

const char* LiveStateName(uint8_t* base, int state)
{
    if (state < 0 || state > 40)
        return "?";
    const uint32_t p = LoadU32(base, kLiveStateNames + uint32_t(state) * 4);
    return p ? reinterpret_cast<const char*>(base + p) : "?";
}

long g_lastAttemptMs = -1;
long g_tries = 0;
unsigned g_frames = 0;
char g_lastWhy[200] = "";
bool g_announcedConnected = false;

// Build mm_info{host=0, COOP, -1, 0, 0, 0} with the title's own ctor on a
// scratch area below the guest stack, and hand it to sub_824BD960. Both calls
// run on a copy of the context whose r1 sits below the scratch, so neither
// the hooked function's frame nor the scratch is touched by the callees.
void FireJoin(PPCContext& ctx, uint8_t* base, const Objects& o)
{
    PPCContext call = ctx;
    const uint32_t scratch = (ctx.r1.u32 - 0x40) & ~0xFu;
    call.r1.u64 = scratch - 0x100;
    call.r3.u64 = scratch;
    call.r4.u64 = 0;            // IsHost
    call.r5.u64 = 1;            // GAME_TYPE_COOP
    call.r6.u64 = 0;
    call.r7.u64 = 0;
    call.r8.u64 = 0xFFFFFFFFu;  // the -1 the JoinGame path passes
    call.r9.u64 = 0;
    if (!GuestCall(call, base, kFnMmInfoCtor, "mm_info ctor"))
        return;
    call.r3.u64 = o.gameSession;
    call.r4.u64 = scratch;
    g_tries++;
    g_lastAttemptMs = NowMs();
    fprintf(stderr, "[coop] JOIN attempt %ld: sub_824BD960(game session %08X, mm_info{host=0, "
                    "COOP}) at %ld ms (game state %u, login state %d)\n",
            g_tries, o.gameSession, g_lastAttemptMs, GameState(base), LoginState(base, o));
    GuestCall(call, base, kFnStartSession, "start-session");
}

// The JoinGame row's form: open GameSelect in mode 1 through the frontend
// transition manager (captured by the title's own first screen change), with
// the {1, 1} params the row passes. The screen then makes the call above
// itself and reacts to what follows.
void FireJoinScreen(PPCContext& ctx, uint8_t* base, const Objects& o)
{
    const uint32_t manager = DebugTunables_FrontendManager();
    if (!manager)
    {
        fprintf(stderr, "[coop] join: no frontend transition manager captured yet\n");
        return;
    }
    PPCContext call = ctx;
    const uint32_t params = (ctx.r1.u32 - 0x20) & ~0xFu;
    call.r1.u64 = params - 0x100;
    PPC_STORE_U32(params, 1);
    PPC_STORE_U32(params + 4, 1);   // mode 1: join a co-op game
    call.r3.u64 = kStrGameSelect;
    call.r4.u64 = 10;
    __imp__sub_8276E398(call, base);
    const uint32_t hash = call.r3.u32;
    g_tries++;
    g_lastAttemptMs = NowMs();
    fprintf(stderr, "[coop] JOIN attempt %ld: GameSelect (hash %08X) mode 1 through frontend "
                    "manager %08X at %ld ms (game session %08X, game state %u, login state %d)\n",
            g_tries, hash, manager, g_lastAttemptMs, o.gameSession, GameState(base),
            LoginState(base, o));
    call.r3.u64 = manager;
    call.r4.u64 = hash;
    call.r5.u64 = params;
    __imp__sub_827F6D40(call, base);
    // 1 = queued (+0x17C hash, +0x180 params); 0 = the current screen (manager+0x120,
    // its vt[5]) refused the transition or one is already pending.
    fprintf(stderr, "[coop] join: transition request returned %u (current screen %08X, "
                    "pending hash %08X, state %u/%u)\n", call.r3.u32,
            LoadU32(base, manager + 0x120), LoadU32(base, manager + 0x17C),
            LoadU8(base, manager + 0x16C), LoadU8(base, manager + 0x16D));
}

// Why the join is not fired this frame, or nullptr to fire. Printed only when
// the reason changes, so a long wait is one line.
const char* WhyNotJoining(PPCContext& ctx, uint8_t* base, const Objects& o, char* detail,
                          size_t cap)
{
    detail[0] = '\0';
    if (g_maxTries && g_tries >= g_maxTries)
        return "out of tries";
    if (!o.gameSession)
        return "no game session object";
    if (!o.online)
        return "no online manager";
    if (!o.matchmaking)
        return "no matchmaking object";
    if (!o.session)
        return "no session object (vt[30])";
    // The login object (session+0x94) is null until the first walk creates
    // it, so -1 reads as idle here, the same as LOGIN_STATE_IDLE.
    const int login = LoginState(base, o);
    if (login == 3)
        return "LOGIN_STATE_CONNECTED — in a session";
    const int live = LiveState(base);
    if (login > 0 || live > 0)
    {
        snprintf(detail, cap, "login state %d, live state %s", login,
                 LiveStateName(base, live));
        return "a walk is in progress";
    }
    const long now = NowMs();
    if (now < g_afterMs)
        return "before CZ_XLIVE_JOIN_AFTER_MS";
    if (g_lastAttemptMs >= 0 && now - g_lastAttemptMs < g_retryMs)
    {
        return "cooling down after an attempt that came back to IDLE";
    }
    // No game-state gate: the main menu after START reads state 3 here, and
    // GameSelect's own ctor only conditions its listener registration on the
    // state (5/7 at 0x824C8958), not the join call. sub_824BDD10 is the
    // title's gate; the state is printed with the attempt.
    PPCContext call = ctx;
    call.r3.u64 = o.gameSession;
    GuestCall(call, base, kFnOnlineReady, "online-ready");
    if (!(call.r3.u32 & 0xFF))
        return "sub_824BDD10 (online ready) says no";
    return nullptr;
}
} // namespace

// The game session's per-frame Update. The title's own join call goes after
// the update it would have followed on the JoinGame screen.
PPC_FUNC(sub_824C2268)
{
    __imp__sub_824C2268(ctx, base);
    if (!JoinRequested())
        return;
    // Every 15th frame: the predicate chain is several guest calls, and the
    // decision does not change faster than that.
    if ((g_frames++ % 15) != 0)
        return;
    const Objects o = Resolve(ctx, base);
    char detail[160];
    const char* why = WhyNotJoining(ctx, base, o, detail, sizeof detail);
    if (why)
    {
        char line[200];
        snprintf(line, sizeof line, "%s%s%s", why, detail[0] ? ": " : "", detail);
        if (std::strcmp(line, g_lastWhy) != 0)
        {
            std::strncpy(g_lastWhy, line, sizeof g_lastWhy - 1);
            fprintf(stderr, "[coop] join: %s\n", line);
        }
        if (!g_announcedConnected && LoginState(base, o) == 3)
        {
            g_announcedConnected = true;
            fprintf(stderr, "[coop] JOINED: LOGIN_STATE_CONNECTED after %ld attempt(s), "
                            "%ld ms after the last one\n",
                    g_tries, NowMs() - g_lastAttemptMs);
        }
        return;
    }
    g_lastWhy[0] = '\0';
    if (g_joinMode == 1)
        FireJoinScreen(ctx, base, o);
    else
        FireJoin(ctx, base, o);
}

// The LIVE_STATE setter: its `this` is the HW MM session object whose +0x118
// the predicate above reads.
PPC_FUNC(sub_825CB2A8)
{
    g_hwSession = ctx.r3.u32;
    __imp__sub_825CB2A8(ctx, base);
}
