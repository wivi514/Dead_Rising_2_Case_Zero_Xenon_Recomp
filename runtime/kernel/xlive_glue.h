// The seam between this runtime and libxlive (fake_xbox_live).
//
// WHAT THIS IS FOR. Until now the signed-in user was three constants and the
// achievements the title earned lived in a std::set that died with the process
// (imports.cpp, "In memory only: persisting them belongs with the save layer").
// libxlive gives them somewhere to go: a real account, a durable local record,
// and a server to sync with when there is one.
//
// WHAT IT DELIBERATELY DOES NOT DO YET. XamUserGetSigninState still reports
// 1 (signed in locally) and XamUserGetSigninInfo(flags=1) still reports no
// online XUID, even for a player with an account. Both are load-bearing:
//
//   * The A1 capture's call sequence depends on the online XUID being zero.
//     sub_825C2F88 only makes its second XamUserGetSigninInfo call when the
//     first returned zero, so answering it would silently delete a call from
//     the sequence tools/kernel_call_diff.py compares against.
//
//   * Saying "signed in to Live" invites the title down the session and
//     matchmaking paths, and every one of those messages still returns E_FAIL
//     (imports.cpp's dispatcher). That is the same reason
//     XamUserCheckPrivilege still grants nothing.
//
// So this first step changes WHO the player is, not WHAT the title believes is
// available. The signin state follows once the XLB logon messages are real.
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

// Gives queued writes a bounded chance to reach the server before the process
// goes away. Durability does not depend on this — every achievement is on disk
// before CzXlive_RecordAchievements returns — but without it a player who quits
// straight after earning something waits until their next launch for it to
// appear online. Called from the window's Shutdown(), beside LogFile::Flush,
// for exactly the reason part 38 learned about counters.
void CzXlive_Shutdown(int timeoutMs);
