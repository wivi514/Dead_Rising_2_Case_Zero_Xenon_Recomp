// The host's "<player> wants to join your game" prompt. Co-op plan part 5
// (docs/coop-plan.md).
//
// WHY THIS EXISTS
// ---------------
// In the two-machine sessions of parts 3 and 4 the joiner was let into the
// host's game without the host being asked: no walkie-talkie call, no dialog,
// just "Pending client IS confirmed!" in the log and a second Chuck. The
// operator wants to SEE the request and answer it. Reading the title's own
// code, the host has two routes from "a client is pending" to
// sub_82582188(session, accept) — the confirm — and Case Zero takes the one
// that asks nobody:
//
//   sub_82587228, the session-details handler on the host: when the joiner's
//   member record arrives it stores the pending client (session+0xDC) and then
//   EITHER, if the game-state word it reads (0x82A57428->+0x78->+0xC->+0x30
//   ->+0x70) is 5 or 6, posts the event {7, hash("DlgOnHostConfirmCOOPJoin"),
//   hash("Yes")} to the game session — a synthetic "Yes" to a dialog that was
//   never shown, which the game session's event handler (sub_824C0958 at
//   0x824C10C0) turns into sub_82582188(session, 1) — OR, in any other state,
//   sub_8256FC60 ("EnablePendingStartOnlineCall"): session+0x93 = 1, which
//   the session's Update (sub_8257C388) turns into sub_8254B108 ->
//   sub_82224DF0, the walkie-talkie call. That call's "answered" handler
//   (sub_82224F30, D-pad RIGHT) is what shows the dialog — sub_8254B300
//   fetches the pending gamertag and sub_824BDBE8 formats string 11533
//   ("%s wants to join your game. Let the player join?") and raises
//   DlgOnHostConfirmCOOPJoin; the answer comes back through the same
//   0x824C10C0 path with the real button ("Yes", "No", or "SetToPrivate",
//   which sets privacy and declines). And the call's ring has no face here:
//   its HUD string, 11546 "Incoming co-op call...", is referenced by no
//   instruction in this build (coop-plan part 4), and an unanswered call is
//   auto-declined by sub_82224A38 on a timer.
//
// So both routes are wrong for a player: one never asks, the other asks
// silently and then says no for them. This file makes both routes ASK, with
// the title's own dialog and the title's own answer path:
//
//   * the synthetic "Yes" is caught where it lands, in the game session's
//     event handler (sub_824C0958): an event {DlgOnHostConfirmCOOPJoin, Yes}
//     arriving while nobody has been asked for this pending client is turned
//     into the prompt instead — the gamertag read with sub_8254B300, the
//     dialog raised with sub_824BDBE8, exactly as the answered call would —
//     and the handler is not run, so neither the confirm nor its tail (the
//     "EXCHANGING_DATA" wait dialog, sub_824BDA60) happens. When the player
//     answers, the answer arrives as the same event, now with the prompt on
//     record, and goes through untouched: accept, decline, or SetToPrivate.
//   * sub_8254B108 (start the walkie-talkie call) raises the dialog directly
//     instead of starting the ring nobody can see; its caller has nothing
//     after it.
//
// If the dialog cannot be raised (sub_824B6CF8: DlgOnHostConfirmCOOPJoin is
// already up), the confirm goes through as before, and says so. Nothing here
// runs until a session has a pending client, so a solo game never touches it.
//
// CZ_COOP_JOIN_PROMPT=0    the control arm: the shipped auto-accept
// CZ_COOP_CALL_TRACE=1     print every step of both routes (the instrument
//                          that established the above on a same-box pair)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"

extern "C" PPC_FUNC(__imp__sub_824C0958);   // (game session, event, ...): the event handler
extern "C" PPC_FUNC(__imp__sub_82582188);   // (session, accept): confirm/decline the pending client
extern "C" PPC_FUNC(__imp__sub_8254B108);   // (session): start the walkie-talkie co-op call
extern "C" PPC_FUNC(__imp__sub_82587228);   // (session, packet): the session-details handler
extern "C" PPC_FUNC(__imp__sub_8256FC60);   // (session): EnablePendingStartOnlineCall
extern "C" PPC_FUNC(__imp__sub_82224DF0);   // (player): the call element starts ringing
extern "C" PPC_FUNC(__imp__sub_82224F30);   // (player, event): the call was answered
extern "C" PPC_FUNC(__imp__sub_824BDBE8);   // (game session, name): raise the dialog
extern "C" PPC_FUNC(__imp__sub_825719E0);   // (session, &xuid, accept): "Pending client %s confirmed!"
extern "C" PPC_FUNC(__imp__sub_8257D298);   // (session, event): the client-event handler

using namespace coop;

namespace
{
constexpr uint32_t kFnPendingName = 0x8254B300;   // (session, out[20]) -> bool
constexpr uint32_t kFnDialogUp = 0x824B6CF8;      // () -> DlgOnHostConfirmCOOPJoin is showing
constexpr uint32_t kFnRaiseDialog = 0x824BDBE8;   // (game session, name)
constexpr uint32_t kSessionPending = 0xDC;
// The hashes the handler compares against, read from the title's own globals
// (each is hash(name) stored by a static initialiser).
constexpr uint32_t kDlgConfirmJoinHash = 0x82A690F4;   // "DlgOnHostConfirmCOOPJoin"
constexpr uint32_t kYesHash = 0x82A68C74;              // "Yes"
constexpr uint32_t kSetToPrivateHash = 0x82A68F10;     // "SetToPrivate"

bool Trace()
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_COOP_CALL_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

bool PromptEnabled()
{
    static const int on = [] {
        const char* e = std::getenv("CZ_COOP_JOIN_PROMPT");
        const int v = (e && *e) ? (*e != '0') : 1;
        if (!v)
            fprintf(stderr, "[coop] CZ_COOP_JOIN_PROMPT=0: a joining player is accepted "
                            "without asking (the shipped behaviour)\n");
        return v;
    }();
    return on != 0;
}

// The pending client the prompt was raised for; a different one (or none)
// means the record is stale and the next confirm is a fresh request.
uint32_t g_promptedFor = 0;

const char* PendingName(uint8_t* base, uint32_t session)
{
    const uint32_t p = LoadU32(base, session + kSessionPending);
    return p ? reinterpret_cast<const char*>(base + p + 0x10) : "(none)";
}

// Raise the title's own dialog for the pending client. Returns false, having
// done nothing, when there is no pending client, the dialog is already up, or
// the game session is missing — the caller then lets the title's path run.
bool RaisePrompt(PPCContext& ctx, uint8_t* base, uint32_t session, const char* route)
{
    const uint32_t pending = LoadU32(base, session + kSessionPending);
    const uint32_t gameSession = LoadU32(base, LoadU32(base, kGameSessionOwner) + 0x14);
    if (!pending || !gameSession)
    {
        fprintf(stderr, "[coop] join prompt (%s): %s — letting the title's path run\n", route,
                pending ? "no game session object" : "no pending client");
        return false;
    }
    PPCContext call = ctx;
    const uint32_t scratch = (ctx.r1.u32 - 0x40) & ~0xFu;   // 20 bytes of name, zeroed
    call.r1.u64 = scratch - 0x100;
    for (uint32_t i = 0; i < 0x20; i += 4)
        PPC_STORE_U32(scratch + i, 0);
    if (!GuestCall(call, base, kFnDialogUp, "dialog-up"))
        return false;
    if (call.r3.u32 & 0xFF)
    {
        fprintf(stderr, "[coop] join prompt (%s): DlgOnHostConfirmCOOPJoin is already showing "
                        "— letting the title's path run\n", route);
        return false;
    }
    call.r3.u64 = session;
    call.r4.u64 = scratch;
    if (!GuestCall(call, base, kFnPendingName, "pending-name"))
        return false;
    if (!(call.r3.u32 & 0xFF))
    {
        fprintf(stderr, "[coop] join prompt (%s): sub_8254B300 has no pending name — letting "
                        "the title's path run\n", route);
        return false;
    }
    const char* name = reinterpret_cast<const char*>(base + scratch);
    call.r3.u64 = gameSession;
    call.r4.u64 = scratch;
    if (!GuestCall(call, base, kFnRaiseDialog, "raise-dialog"))
        return false;
    g_promptedFor = pending;
    fprintf(stderr, "[coop] JOIN PROMPT raised for '%s' (%s route): \"%s wants to join your "
                    "game. Let the player join?\" — the answer confirms or declines through "
                    "the title's own path\n", name, route, name);
    return true;
}
} // namespace

// The game session's event handler. A "Yes" that no player gave becomes the
// question; the player's own answer, later, goes through untouched. The
// PPC_FUNC hook itself lives in coop_friends.cpp (one function, two hooks);
// this is its half.
void CoopCall_GameSessionEvent(PPCContext& ctx, uint8_t* base)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = LoadU32(base, event + 4);
    const uint32_t answer = LoadU32(base, event + 8);
    if (id == LoadU32(base, kDlgConfirmJoinHash))
    {
        const Objects o = Resolve(ctx, base);
        const uint32_t pending = o.session ? LoadU32(base, o.session + kSessionPending) : 0;
        const bool yes = answer == LoadU32(base, kYesHash);
        if (Trace())
            fprintf(stderr, "[coop:call] sub_824C0958: DlgOnHostConfirmCOOPJoin answer %08X "
                            "(%s) for pending %08X '%s', prompted for %08X\n", answer,
                    yes ? "Yes" : answer == LoadU32(base, kSetToPrivateHash) ? "SetToPrivate"
                                                                              : "No",
                    pending, o.session ? PendingName(base, o.session) : "?", g_promptedFor);
        if (PromptEnabled() && yes && pending && g_promptedFor != pending)
        {
            // Nobody has been asked: this is sub_82587228's synthetic Yes.
            if (RaisePrompt(ctx, base, o.session, "synthetic-yes"))
            {
                ctx.r3.u64 = 1;   // handled
                return;
            }
        }
        if (pending && g_promptedFor == pending)
            fprintf(stderr, "[coop] join prompt answered: '%s' %s\n",
                    PendingName(base, o.session),
                    yes ? "ACCEPTED" : "DECLINED (SetToPrivate also sets the session private)");
        g_promptedFor = 0;
    }
    __imp__sub_824C0958(ctx, base);
}

// The walkie-talkie route: ask now, instead of ringing a phone with no face.
PPC_FUNC(sub_8254B108)
{
    const uint32_t session = ctx.r3.u32;
    if (Trace())
        fprintf(stderr, "[coop:call] sub_8254B108(session %08X): the title would start the "
                        "co-op call for '%s'\n", session, PendingName(base, session));
    if (PromptEnabled() && RaisePrompt(ctx, base, session, "walkie-talkie"))
        return;
    __imp__sub_8254B108(ctx, base);
}

// --- the instrument: every step of both routes, on CZ_COOP_CALL_TRACE=1 ---

PPC_FUNC(sub_82582188)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_82582188(session %08X, accept %u) from %08X: pending "
                        "'%s'\n", ctx.r3.u32, ctx.r4.u32 & 0xFF, uint32_t(ctx.lr),
                PendingName(base, ctx.r3.u32));
    __imp__sub_82582188(ctx, base);
}

PPC_FUNC(sub_82587228)
{
    const uint32_t session = ctx.r3.u32;
    const uint32_t before = LoadU32(base, session + kSessionPending);
    __imp__sub_82587228(ctx, base);
    const uint32_t after = LoadU32(base, session + kSessionPending);
    if (Trace() && after != before)
    {
        // The word the branch reads, re-read the way 0x82587540 reads it.
        // Every link may be null (the first same-box run faulted at guest 0xC
        // here, from an instrument: LoadU32 guards only a zero ADDRESS).
        const uint32_t owner = LoadU32(base, kGameStateOwner);
        const uint32_t a = owner ? LoadU32(base, owner + 0x78) : 0;
        const uint32_t b = a ? LoadU32(base, a + 0xC) : 0;
        const uint32_t c = b ? LoadU32(base, b + 0x30) : 0;
        const uint32_t word = c ? LoadU32(base, c + 0x70) : 0xFFFFFFFFu;
        fprintf(stderr, "[coop:call] sub_82587228: pending client %08X -> %08X '%s'; state word "
                        "%u (5/6 = synthetic Yes, else the call), game state %u\n",
                before, after, PendingName(base, session), word, GameState(base));
    }
}

PPC_FUNC(sub_8256FC60)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_8256FC60 EnablePendingStartOnlineCall(session %08X) "
                        "from %08X: pending '%s'\n", ctx.r3.u32, uint32_t(ctx.lr),
                PendingName(base, ctx.r3.u32));
    __imp__sub_8256FC60(ctx, base);
}

PPC_FUNC(sub_82224DF0)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_82224DF0: the call element starts (player %08X, state "
                        "%u)\n", ctx.r3.u32, LoadU32(base, ctx.r3.u32 + 0x1A40));
    __imp__sub_82224DF0(ctx, base);
}

PPC_FUNC(sub_82224F30)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_82224F30: the call was answered (player %08X)\n",
                ctx.r3.u32);
    __imp__sub_82224F30(ctx, base);
}

PPC_FUNC(sub_824BDBE8)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_824BDBE8(game session %08X, '%s') from %08X: raise "
                        "DlgOnHostConfirmCOOPJoin\n", ctx.r3.u32,
                reinterpret_cast<const char*>(base + ctx.r4.u32), uint32_t(ctx.lr));
    __imp__sub_824BDBE8(ctx, base);
}

PPC_FUNC(sub_825719E0)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_825719E0(session %08X, xuid %016llX, accept %u) from "
                        "%08X\n", ctx.r3.u32,
                (unsigned long long)(((uint64_t)LoadU32(base, ctx.r4.u32) << 32) |
                                     LoadU32(base, ctx.r4.u32 + 4)),
                ctx.r5.u32 & 0xFF, uint32_t(ctx.lr));
    __imp__sub_825719E0(ctx, base);
}

PPC_FUNC(sub_8257D298)
{
    if (Trace())
        fprintf(stderr, "[coop:call] sub_8257D298(session %08X, event kind %u)\n", ctx.r3.u32,
                LoadU32(base, ctx.r4.u32 + 0x1C));
    __imp__sub_8257D298(ctx, base);
}
