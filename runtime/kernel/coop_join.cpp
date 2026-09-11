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
// It does NOT open the GameSelect screen. The screen is UI over the same
// call, and opening a frontend screen from a hook means owning its lifetime
// against the menu that is actually showing; the call is enough to learn
// whether the client flow works, and a row in the panel belongs with the
// privacy setting once part 4 says co-op is worth shipping.
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

using namespace coop;

namespace
{
int g_joinMode = -1; // -1 unread; 0 off; 1 on
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
        g_joinMode = (env && *env && *env != '0') ? 1 : 0;
        if (g_joinMode)
        {
            g_afterMs = EnvLong("CZ_XLIVE_JOIN_AFTER_MS", g_afterMs);
            g_retryMs = EnvLong("CZ_XLIVE_JOIN_RETRY_MS", g_retryMs);
            g_maxTries = EnvLong("CZ_XLIVE_JOIN_TRIES", 0);
            fprintf(stderr, "[coop] CZ_XLIVE_JOIN: this build will search for and join a "
                            "co-op session (after %ld ms, retry every %ld ms, %s)\n",
                    g_afterMs, g_retryMs, g_maxTries ? "limited tries" : "until it connects");
        }
    }
    return g_joinMode == 1;
}

long NowMs()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return long(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

// The LIVE_STATE the matchmaking machine last entered, by name, so a refusal
// can be read against it without CZ_ONLINE_LOG. The setter is sub_825CB2A8
// (this, newState); the name table is at 0x829DFD40.
constexpr uint32_t kLiveStateNames = 0x829DFD40;
int g_lastLiveState = -1;

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
    if (login > 0 || g_lastLiveState > 0)
    {
        snprintf(detail, cap, "login state %d, live state %s", login,
                 LiveStateName(base, g_lastLiveState));
        return "a walk is in progress";
    }
    const long now = NowMs();
    if (now < g_afterMs)
        return "before CZ_XLIVE_JOIN_AFTER_MS";
    if (g_lastAttemptMs >= 0 && now - g_lastAttemptMs < g_retryMs)
    {
        snprintf(detail, cap, "last live state %s", LiveStateName(base, g_lastLiveState));
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
    FireJoin(ctx, base, o);
}

// The LIVE_STATE setter, recorded so the refusal lines above can name the
// state the walk is in. Both arms of the co-op work read it.
PPC_FUNC(sub_825CB2A8)
{
    g_lastLiveState = int(ctx.r4.u32);
    __imp__sub_825CB2A8(ctx, base);
}
