// The title's online object graph, resolved the way the title resolves it.
// Shared by the co-op host lever (coop_host.cpp) and the joiner (coop_join.cpp).
//
// Every walk here is a call into the title's own getters on a COPY of the
// guest context, so a reading cannot drift from what the title itself would
// read in the same frame — the discipline coop_host.cpp was built on. Nothing
// in this header is a lever; it only reads.
#pragma once

#include <cstddef>
#include <cstdint>

#include <ppc_config.h>
#include <ppc_context.h>

namespace coop
{
constexpr uint32_t kOnlineManager = 0x82AD6E90;   // the online object (sub_8258D310 creates it)
constexpr uint32_t kGameSessionOwner = 0x82A58C64; // +0x14 = the game session object
constexpr uint32_t kGameStateOwner = 0x82A57428;   // +0x24 -> +0x2C = current game state
constexpr uint32_t kFnMatchmakingOf = 0x82546A80;  // (online) -> matchmaking object, or 0
constexpr uint32_t kFnSessionIsLive = 0x8254AF30;  // (session) -> login state == 3
constexpr uint32_t kFnSessionIsCoop = 0x82547920;  // (session) -> +0x98
constexpr uint32_t kFnOnlineReady = 0x824BDD10;    // (game session) -> bool
constexpr uint32_t kFnStartSession = 0x824BD960;   // (game session, mm_info): host or join
constexpr uint32_t kFnMmInfoCtor = 0x82547008;     // (mm_info, isHost, gameType, 0, 0, -1, 0)

constexpr uint32_t kSessionIsCoopByte = 0x98;
constexpr uint32_t kSessionLoginObject = 0x94;     // -> object whose +0xC is the LOGIN_STATE
constexpr uint32_t kGameSessionHostRequested = 0x90;
constexpr uint32_t kMatchmakingVtSession = 0x78;   // vt[30]: the session object
constexpr uint32_t kMatchmakingVtSessionInfo = 0x88; // vt[34]: holds mm_info at +0x1C
constexpr uint32_t kMatchmakingVtSignedIn = 0x18;  // vt[6]
constexpr uint32_t kMatchmakingVtService = 0x54;   // vt[21](kind) -> state, 2 = ready

uint32_t LoadU32(uint8_t* base, uint32_t addr);
uint8_t LoadU8(uint8_t* base, uint32_t addr);

// Call a guest function by address on a COPY of the context. Refuses an
// address the dispatch table does not know, and says so.
bool GuestCall(PPCContext& call, uint8_t* base, uint32_t fn, const char* what);
uint32_t VCall(PPCContext& call, uint8_t* base, uint32_t obj, uint32_t slotOff,
               uint32_t arg, const char* what);

// online -> matchmaking -> session, the walk every online routine begins with.
struct Objects
{
    uint32_t online{}, matchmaking{}, session{}, sessionInfo{}, gameSession{};
};
Objects Resolve(PPCContext& ctx, uint8_t* base);

// The session's LOGIN_STATE (3 = CONNECTED), or -1 with no login object.
int LoginState(uint8_t* base, const Objects& o);
uint32_t GameState(uint8_t* base);
} // namespace coop
