// The seam between this runtime and libxlive (XenonLive).
//
// WHAT THIS IS FOR. Until now the signed-in user was three constants and the
// achievements the title earned lived in a std::set that died with the process
// (imports.cpp, "In memory only: persisting them belongs with the save layer").
// libxlive gives them somewhere to go: a real account, a durable local record,
// and a server to sync with when there is one.
//
// WHAT IT DOES ONLY WHEN ASKED. XamUserGetSigninState reports 1 (signed in
// locally) and XamUserGetSigninInfo(flags=1) reports no online XUID unless
// CZ_XLIVE_ONLINE=1 is set AND a gateway connection is up. Both defaults are
// load-bearing:
//
//   * The A1 capture's call sequence depends on the online XUID being zero.
//     sub_825C2F88 only makes its second XamUserGetSigninInfo call when the
//     first returned zero, so answering it deletes a call from the sequence
//     tools/kernel_call_diff.py compares against.
//
//   * Saying "signed in to Live" sends the title down XOnlineStartup, the
//     friends list and the invite state machine — kernel/xlive_social.cpp —
//     and, with CZ_XLIVE_COOP=1, the session and matchmaking messages. Those
//     answer honestly now, but the A5 gate was captured offline and shows the
//     divergence (XMsgInProcessCall, XamEnumerate, XamFree from position 104),
//     which is why the state moves only behind its own flag.
//
// Three switches, then, each for a different kind of change: CZ_NO_XLIVE=1
// (who the player is), CZ_XLIVE_COOP=1 (what a session message answers, and
// with it the socket family and QoS — kernel/xlive_net.cpp — since sockets
// carry a session's traffic), CZ_XLIVE_ONLINE=1 (what the title believes is
// available, and with it the leaderboard reads — kernel/xlive_stats.cpp —
// since a title only asks for a board once it believes it is online).
// Presence — what the title says it is doing, published to accepted friends
// — has no switch of its own beyond the first: it is on whenever an account
// is signed in.
//
// The self-tests, one per file, each behind its own CZ_XLIVE_*_TEST=1:
// SESSION, SOCIAL, STATS, NET.
//
// EVERYTHING HERE IS OPTIONAL AT RUNTIME. CZ_NO_XLIVE=1 turns it off, and with
// it off every function below behaves as if no account exists, which is
// byte-for-byte what this runtime did before.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Starts the client. titleId comes from XexTitleId(), so this must run after
// the XEX is loaded. Never blocks: a player with no account, no network or no
// writable data directory is a success, reported as "signed out".
void CzXlive_Start(uint32_t titleId);

// True when an account is signed in AND the server has confirmed it.
bool CzXlive_SignedIn();

// True when the title may be TOLD that: CZ_XLIVE_ONLINE=1 and a gateway
// connection is up. This is the switch behind XamUserGetSigninState saying 2,
// the online XUID, and every privilege — and so behind every Live code path
// the title has: XOnlineStartup, the friends list, the invite state machine.
// It is a separate switch from CZ_NO_XLIVE and CZ_XLIVE_COOP because it is
// the riskiest of the three: the first two change who the player is and what
// a message answers, this one changes what the title believes is available.
bool CzXlive_SignedInToLive();

// The account's XUID and gamertag, or the fallback when signed out. The
// fallbacks are this runtime's own constants, so a signed-out player sees no
// change at all.
uint64_t CzXlive_Xuid(uint64_t fallback);
const char* CzXlive_Gamertag(const char* fallback);

// Records achievements the title just wrote. Returns immediately: the local
// record is updated before the call returns and the server write is queued, so
// this is safe to call from the guest thread that is in the middle of a save.
void CzXlive_RecordAchievements(const std::vector<uint16_t>& achievementIds);

// One statistic the title wrote, mirroring the guest's XUSER_PROPERTY. The
// types are the guest's own X_USER_DATA_TYPE values.
struct CzXliveStatProperty
{
    uint32_t id = 0;
    uint8_t type = 2;      // 0 context, 1 int32, 2 int64, 3 double,
                           // 4 unicode, 5 float, 6 binary, 7 datetime
    int64_t integer = 0;   // context, int32, int64, datetime
    double real = 0;       // double, float
    std::string text;      // unicode as UTF-8, binary as raw bytes
};

struct CzXliveStatView
{
    uint32_t viewId = 0;
    std::vector<CzXliveStatProperty> properties;
};

// Records an XSessionWriteStats call. Returns immediately, like the achievement
// path: the title writes stats at the end of a run and must not wait for a
// server. Which property ranks a board, and how repeated writes combine, are
// NOT decided here — they come from the title's own SPA by way of the server,
// because a client that chose its own placement on a leaderboard would not be
// one worth trusting.
void CzXlive_RecordStats(const std::vector<CzXliveStatView>& views);

// Rich presence. The title sets X_CONTEXT_PRESENCE (0x8001) through
// XUserSetContext whenever what the player is doing changes — menu, story,
// co-op lobby — and on the console that value reached the friends list as a
// line of text. Forwarding it is a few lines because libxlive already owns
// the rest: the SERVER turns the value into the string using the title's own
// SPA, so nothing here carries the strings. Fire and forget, never blocks.
//
// The session half comes from xlive_session.cpp, which knows when the player
// is in a lobby a friend could be invited into. The two are composed here
// because libxlive's SetPresence replaces the whole update, and the title
// sets the context long before (and long after) it has a session.
void CzXlive_SetPresence(uint32_t presenceValue);
void CzXlive_SetPresenceSession(uint64_t sessionId, bool joinable);

// Gives queued writes a bounded chance to reach the server before the process
// goes away. Durability does not depend on this — every achievement is on disk
// before CzXlive_RecordAchievements returns — but without it a player who quits
// straight after earning something waits until their next launch for it to
// appear online. Called from the window's Shutdown(), beside LogFile::Flush,
// for exactly the reason part 38 learned about counters.
void CzXlive_Shutdown(int timeoutMs);
