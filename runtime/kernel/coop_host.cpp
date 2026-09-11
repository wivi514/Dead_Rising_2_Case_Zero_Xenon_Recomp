// CZ_XLIVE_HOST=1: make this build HOST a co-op session, through the title's
// own recreate path. Co-op plan part 2 (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// Case Zero shipped single-player, but the whole DR2 co-op layer is linked and
// runs at boot; only the frontend's "start a co-op game" was removed. What is
// still in the image is the RECREATE path — the host re-creating its session
// each time the game flow re-enters gameplay (a level change tears the session
// down). It is three routines, all the title's own:
//
//   sub_82537FA0  GameplayFlow::Enter (flow table entry 0x8207A59C). If the
//                 online manager exists, the session is not already live
//                 (sub_8254AF30) and the session's IS-COOP byte (+0x98, read by
//                 sub_82547920) is set, it writes mm_info{host=1, COOP} into
//                 the session-info object (vt[34] of the matchmaking object,
//                 +0x1C) and sets the game session's HOST-REQUESTED byte
//                 (+0x90) — nothing else.
//   sub_824C0668  called every frame from the game session's Update
//                 (sub_824C2268 at 0x824C2484). It reads that +0x90 byte and,
//                 when eight more predicates hold, calls
//   sub_824BD960  (this, mm_info) — which for a host mm_info runs
//                 sub_82553A98: the matchmaking object's vt[33] creates the
//                 HW MM session and the session interface's vt[6]
//                 (sub_825CD478) copies the description in and pushes
//                 LIVE_STATE_SETTING_SESSION_CONTEXTS — the 1 -> 2 -> 3 walk
//                 Case West's host log shows.
//
// The one byte the frontend would have set and Case Zero's never does is
// IS-COOP. In DR2 proper the "play co-op" screen builds mm_info{host, COOP}
// and calls sub_824BD960 directly, which sets IS-COOP (sub_82547928) as a side
// effect; here no caller passes a host+COOP mm_info (the six frontend callers
// of sub_824BD960 are TIR matchmaking and the co-op JOIN — census in
// docs/coop-plan.md). So this file sets that byte just before
// GameplayFlow::Enter runs, and lets the title do the rest with its own
// checks, in its own order, on its own thread.
//
// The second hook is an INSTRUMENT, not a lever: sub_824C0668 declines
// silently — ten predicates and not one log line — so when the request is
// armed and nothing happens, this prints which predicate refused (gotcha 5's
// spirit: a refusal must be named). Every predicate is evaluated by calling
// the same guest getters the title calls, on a copied context, so the reading
// cannot drift from the title's own.
//
// mm_info (24 bytes, ctor sub_82547008):
//   +0  u8  IsHost      +4  u32 GameType (1 = DR2Online::GAME_TYPE_COOP)
//   +8  u32 (-1)        +C  u32          +10 u8          +14 u32
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_config.h>
#include <ppc_context.h>

#include "memory.h"

extern "C" PPC_FUNC(__imp__sub_82537FA0);
extern "C" PPC_FUNC(__imp__sub_824C0668);
extern "C" PPC_FUNC(__imp__sub_825C2E20);
extern "C" PPC_FUNC(__imp__sub_825C61B0);

namespace
{
constexpr uint32_t kOnlineManager = 0x82AD6E90;   // the online object (sub_8258D310 creates it)
constexpr uint32_t kGameSessionOwner = 0x82A58C64; // +0x14 = the game session object
constexpr uint32_t kGameStateOwner = 0x82A57428;   // +0x24 -> +0x2C = current game state
constexpr uint32_t kFnMatchmakingOf = 0x82546A80;  // (online) -> matchmaking object, or 0
constexpr uint32_t kFnSessionIsLive = 0x8254AF30;  // (session) -> +0x94 state == 3
constexpr uint32_t kFnSessionIsCoop = 0x82547920;  // (session) -> +0x98
constexpr uint32_t kFnOnlineReady = 0x824BDD10;    // (game session) -> bool

constexpr uint32_t kSessionIsCoopByte = 0x98;
constexpr uint32_t kGameSessionHostRequested = 0x90;
constexpr uint32_t kMatchmakingVtSession = 0x78;   // vt[30]: the session object
constexpr uint32_t kMatchmakingVtSessionInfo = 0x88; // vt[34]: holds mm_info at +0x1C
constexpr uint32_t kMatchmakingVtSignedIn = 0x18;  // vt[6]
constexpr uint32_t kMatchmakingVtService = 0x54;   // vt[21](kind) -> state, 2 = ready

int g_hostMode = -1; // -1 unread; 0 off; 1 on

bool HostRequested()
{
    if (g_hostMode < 0)
    {
        const char* env = std::getenv("CZ_XLIVE_HOST");
        g_hostMode = (env && *env && *env != '0') ? 1 : 0;
        if (g_hostMode)
            fprintf(stderr, "[coop] CZ_XLIVE_HOST: this build will host a co-op session "
                            "when the game flow enters gameplay\n");
    }
    return g_hostMode == 1;
}

uint32_t LoadU32(uint8_t* base, uint32_t addr)
{
    return addr ? PPC_LOAD_U32(addr) : 0;
}

uint8_t LoadU8(uint8_t* base, uint32_t addr)
{
    return addr ? PPC_LOAD_U8(addr) : 0;
}

// Call a guest function by address on a COPY of the context, so the caller's
// registers — including the lr the hooked function's own prologue is about to
// save — are untouched. Refuses an address the dispatch table does not know.
bool GuestCall(PPCContext& call, uint8_t* base, uint32_t fn, const char* what)
{
    PPCFunc* f = g_memory.FindFunction(fn);
    if (!f)
    {
        fprintf(stderr, "[coop] %s: %08X is not a known function start — REFUSED\n",
                what, fn);
        return false;
    }
    f(call, base);
    return true;
}

uint32_t VCall(PPCContext& call, uint8_t* base, uint32_t obj, uint32_t slotOff,
               uint32_t arg, const char* what)
{
    const uint32_t vt = LoadU32(base, obj);
    const uint32_t fn = LoadU32(base, vt + slotOff);
    call.r3.u64 = obj;
    call.r4.u64 = arg;
    if (!GuestCall(call, base, fn, what))
        return 0;
    return call.r3.u32;
}

// online -> matchmaking -> session, the walk every routine above begins with.
struct Objects
{
    uint32_t online{}, matchmaking{}, session{}, sessionInfo{}, gameSession{};
};

Objects Resolve(PPCContext& ctx, uint8_t* base)
{
    Objects o;
    o.online = LoadU32(base, kOnlineManager);
    o.gameSession = LoadU32(base, LoadU32(base, kGameSessionOwner) + 0x14);
    if (!o.online)
        return o;
    PPCContext call = ctx;
    call.r3.u64 = o.online;
    if (!GuestCall(call, base, kFnMatchmakingOf, "matchmaking-of"))
        return o;
    o.matchmaking = call.r3.u32;
    if (!o.matchmaking)
        return o;
    o.session = VCall(call, base, o.matchmaking, kMatchmakingVtSession, 0, "session");
    o.sessionInfo = VCall(call, base, o.matchmaking, kMatchmakingVtSessionInfo, 0,
                          "session-info");
    return o;
}

// The predicate chain of sub_824C0668, in its order, each read the way the title
// reads it. Returns the name of the first refusing predicate, or nullptr.
const char* WhyNotHosting(PPCContext& ctx, uint8_t* base, const Objects& o, char* detail,
                          size_t cap)
{
    detail[0] = '\0';
    if (!o.gameSession)
        return "no game session object";
    if (!LoadU8(base, o.gameSession + kGameSessionHostRequested))
        return "host not requested (game session +0x90 == 0)";
    if (!o.online)
        return "no online manager";
    if (!o.matchmaking)
        return "no matchmaking object";
    PPCContext call = ctx;
    if (!(VCall(call, base, o.matchmaking, kMatchmakingVtSignedIn, 0, "signed-in") & 0xFF))
    {
        // vt[6] (0x82554598) forwards to the object at +0x74; name both.
        const uint32_t vt = LoadU32(base, o.matchmaking);
        const uint32_t inner = LoadU32(base, o.matchmaking + 0x74);
        const uint32_t ivt = LoadU32(base, inner);
        // The inner's vt[6] (0x8280DCD0) is XamUserGetSigninState(inner+8) == 2.
        snprintf(detail, cap, "vtable %08X vt[6] %08X -> inner %08X vtable %08X vt[6] %08X, "
                 "user index at inner+8 = %u", vt,
                 LoadU32(base, vt + kMatchmakingVtSignedIn), inner, ivt,
                 LoadU32(base, ivt + kMatchmakingVtSignedIn), LoadU32(base, inner + 8));
        return "matchmaking vt[6] says no";
    }
    const uint32_t svc6 = VCall(call, base, o.matchmaking, kMatchmakingVtService, 6, "svc6");
    if (svc6 != 2)
    {
        snprintf(detail, cap, "state %u", svc6);
        return "service 6 not ready (vt[21](6) != 2)";
    }
    const uint32_t svc3 = VCall(call, base, o.matchmaking, kMatchmakingVtService, 3, "svc3");
    if (svc3 != 2)
    {
        snprintf(detail, cap, "state %u", svc3);
        return "service 3 not ready (vt[21](3) != 2)";
    }
    if (!o.session)
        return "no session object (vt[30])";
    call.r3.u64 = o.session;
    GuestCall(call, base, kFnSessionIsLive, "session-is-live");
    if (call.r3.u32 & 0xFF)
        return "session already live (sub_8254AF30)";
    call.r3.u64 = o.session;
    GuestCall(call, base, kFnSessionIsCoop, "session-is-coop");
    if (!(call.r3.u32 & 0xFF))
        return "session IS-COOP byte (+0x98) is 0";
    if (LoadU8(base, o.session + 0x90))
        return "session +0x90 busy byte set";
    const uint32_t gs = LoadU32(base, LoadU32(base, LoadU32(base, kGameStateOwner) + 0x24) + 0x2C);
    if (gs != 4 && gs != 5)
    {
        snprintf(detail, cap, "state %u", gs);
        return "game state not 4/5";
    }
    call.r3.u64 = o.gameSession;
    GuestCall(call, base, kFnOnlineReady, "online-ready");
    if (!(call.r3.u32 & 0xFF))
        return "sub_824BDD10 (online ready) says no";
    if (!o.sessionInfo)
        return "no session-info object (vt[34])";
    const uint32_t mm = o.sessionInfo + 0x1C;
    if (!LoadU8(base, mm))
        return "mm_info.IsHost() false";
    if (LoadU32(base, mm + 4) != 1)
    {
        snprintf(detail, cap, "gametype %u", LoadU32(base, mm + 4));
        return "mm_info.GetGameType() != COOP";
    }
    return nullptr;
}
} // namespace

// GameplayFlow::Enter. Set IS-COOP first; the title arms the host request itself.
PPC_FUNC(sub_82537FA0)
{
    if (HostRequested())
    {
        const Objects o = Resolve(ctx, base);
        if (o.session)
        {
            const uint8_t was = LoadU8(base, o.session + kSessionIsCoopByte);
            PPC_STORE_U8(o.session + kSessionIsCoopByte, 1);
            fprintf(stderr, "[coop] GameplayFlow::Enter: session %08X IS-COOP %u -> 1 "
                            "(online %08X matchmaking %08X game session %08X)\n",
                    o.session, was, o.online, o.matchmaking, o.gameSession);
        }
        else
        {
            fprintf(stderr, "[coop] GameplayFlow::Enter: NO SESSION OBJECT yet (online %08X "
                            "matchmaking %08X) — the title will not arm a host request "
                            "this time\n", o.online, o.matchmaking);
        }
    }
    __imp__sub_82537FA0(ctx, base);
    if (HostRequested())
    {
        const uint32_t gsObj = LoadU32(base, LoadU32(base, kGameSessionOwner) + 0x14);
        fprintf(stderr, "[coop] GameplayFlow::Enter done: game session %08X host-requested "
                        "byte = %u\n", gsObj, gsObj ? LoadU8(base, gsObj + kGameSessionHostRequested) : 0);
    }
}

// The per-frame create path. Name the refusal, once per distinct reason.
PPC_FUNC(sub_824C0668)
{
    if (HostRequested())
    {
        static char lastReason[320] = "";
        static unsigned frames = 0;
        const Objects o = Resolve(ctx, base);
        // The global we read the game session from must be the `this` the title
        // passes here, or every line below is about the wrong object. Said once.
        static bool checkedThis = false;
        if (!checkedThis && o.gameSession)
        {
            checkedThis = true;
            if (o.gameSession != ctx.r3.u32)
                fprintf(stderr, "[coop] OBJECT MISMATCH: sub_824C0668 this=%08X but "
                                "*(*%08X+0x14)=%08X — the diagnostics below read the "
                                "wrong object\n", ctx.r3.u32, kGameSessionOwner, o.gameSession);
        }
        // The title's own first predicate is the request byte; only diagnose an
        // armed request, and only every 30th frame so the chain (nine guest
        // calls) is not on the frame path when it does not matter.
        if (o.gameSession && LoadU8(base, o.gameSession + kGameSessionHostRequested) &&
            (frames++ % 30) == 0)
        {
            char detail[160];
            const char* why = WhyNotHosting(ctx, base, o, detail, sizeof detail);
            char line[320];
            snprintf(line, sizeof line, "%s%s%s", why ? why : "ALL PREDICATES HOLD",
                     detail[0] ? ": " : "", detail);
            if (std::strcmp(line, lastReason) != 0)
            {
                std::strncpy(lastReason, line, sizeof lastReason - 1);
                fprintf(stderr, "[coop] host request armed; sub_824C0668: %s\n", line);
            }
        }
    }
    __imp__sub_824C0668(ctx, base);
}

// The online object's active-user setter (sub_825C2E20, via the frontend's
// ProfileChange event, which the title-screen START handler raises for the
// pad that pressed it). The matchmaking object the create path consults is
// the one for THIS index, and its "signed in" predicate is
// XamUserGetSigninState(index) == 2 — which this runtime answers only for
// pad 0. Printed so a host refusal on "vt[6] says no" can be read against
// it: co-op part 2 lost an afternoon to the synthetic-input arm pressing
// START on all four pads (see XamInputGetState in kernel/imports.cpp).
PPC_FUNC(sub_825C2E20)
{
    if (HostRequested())
        fprintf(stderr, "[coop] online %08X SetActiveUser(%u) (was %u) from lr %08X\n",
                ctx.r3.u32, ctx.r4.u32, LoadU32(base, ctx.r3.u32 + 0x5C), uint32_t(ctx.lr));
    __imp__sub_825C2E20(ctx, base);
}

// THE ONE PLACE CASE ZERO ACTUALLY DISABLED CO-OP: the host's XSession flags.
//
// sub_825C61B0 (online vt[33]) builds the session description the state machine
// creates from: (this, isHost, gameType, a, b, privacy, systemlink, ...). The
// flags word at result+0x34 is, for a host:
//
//   Case West (0x82597A0C):   privacy 0 -> 0x42F  HOST|PRESENCE|STATS|MATCHMAKING|
//                                              PEER_NETWORK|JOIN_IN_PROGRESS_DISABLED
//                             privacy 1 -> 0x827  HOST|PRESENCE|STATS|PEER_NETWORK|
//                                              JOIN_VIA_PRESENCE_FRIENDS_ONLY
//                             privacy 2 -> 0x227  HOST|PRESENCE|STATS|PEER_NETWORK|
//                                              JOIN_VIA_PRESENCE_DISABLED (invite only)
//   Case Zero (0x825C6414):   privacy 0, 1, 2 -> 0x706  PRESENCE|STATS|INVITES_DISABLED|
//                                              JOIN_VIA_PRESENCE_DISABLED|
//                                              JOIN_IN_PROGRESS_DISABLED — no HOST bit,
//                                              no PEER_NETWORK, no MATCHMAKING
//
// Everything else on the host path is byte-for-byte the DR2 code (the joiner's
// 0x42E and the system-link 0x21 are untouched), so a solo Case Zero game
// signed in to Live already creates a presence-only session that nobody can
// join. With CZ_XLIVE_HOST=1 this hook rewrites the word to Case West's value
// for the same privacy, after the title's own builder has run.
PPC_FUNC(sub_825C61B0)
{
    const bool isHost = (ctx.r4.u32 & 0xFF) != 0;
    const uint32_t privacy = ctx.r8.u32;
    const bool systemLink = (ctx.r9.u32 & 0xFF) != 0;
    const bool privateGame = ctx.r7.u32 == 1;   // the builder ORs 0x300 for it
    __imp__sub_825C61B0(ctx, base);
    const uint32_t desc = ctx.r3.u32;
    if (!HostRequested() || !desc || !isHost || systemLink)
        return;
    uint32_t flags = 0x42F;
    if (privacy == 1) flags = 0x827;
    else if (privacy == 2) flags = 0x227;
    if (privateGame) flags |= 0x300;
    const uint32_t was = LoadU32(base, desc + 0x34);
    PPC_STORE_U32(desc + 0x34, flags);
    fprintf(stderr, "[coop] host session flags: Case Zero's %03X -> Case West's %03X "
                    "(privacy %u%s)\n", was, flags, privacy, privateGame ? ", private" : "");
}
