#include "xlive_glue.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <xlive/client.h>

#include "klog.h"

namespace
{

// The gamertag is handed to guest code as a `const char*` by XamUserGetName_x,
// which copies it immediately. Holding it in a std::string that the worker
// thread could reassign underneath that copy would be a data race for the sake
// of nothing, so the value is snapshotted into a fixed buffer whenever it
// changes and read from there.
//
// XUSER_NAME_SIZE is 16, which is the size A1 always asks for, and a gamertag
// is at most 15 characters plus its terminator. A longer one from a server that
// does not enforce that is truncated rather than trusted.
constexpr size_t kNameSize = 16;

std::mutex g_nameMutex;
char g_gamertag[kNameSize] = {0};
bool g_haveGamertag = false;

// Said once, the first time the title is actually handed an account's gamertag
// rather than the fallback. Without it "the player is signed in" and "the game
// knows it" are two different claims and only the first one is visible: the
// sign-in happens on a worker thread long before the title asks, and if that
// ordering ever inverted, nothing would say so.
bool g_loggedHandout = false;

bool g_started = false;

void PublishGamertag(const std::string& tag)
{
    std::lock_guard lock(g_nameMutex);
    if (tag.empty())
    {
        g_haveGamertag = false;
        g_gamertag[0] = '\0';
        return;
    }
    memset(g_gamertag, 0, sizeof(g_gamertag));
    const size_t n = tag.size() < kNameSize - 1 ? tag.size() : kNameSize - 1;
    memcpy(g_gamertag, tag.c_str(), n);
    g_haveGamertag = true;
}

const char* LevelName(xlive::LogLevel level)
{
    switch (level)
    {
    case xlive::LogLevel::Debug:   return "debug";
    case xlive::LogLevel::Info:    return "info";
    case xlive::LogLevel::Warning: return "warning";
    case xlive::LogLevel::Error:   return "error";
    }
    return "?";
}

void OnLog(xlive::LogLevel level, const std::string& line)
{
    KLOG("[xlive] %s: %s\n", LevelName(level), line.c_str());
}

void OnEvent(const xlive::Event& event)
{
    switch (event.kind)
    {
    case xlive::EventKind::AchievementUnlocked:
        // The console showed a popup here. This runtime has no overlay to draw
        // one on — host/overlay_gen.cpp generates game ASSETS, not a UI layer —
        // so the log is where it goes until the launcher can show it.
        if (!event.achievement_name.empty())
            KLOG("[xlive] achievement unlocked: %s (%u G)\n",
                 event.achievement_name.c_str(), event.score);
        else
            KLOG("[xlive] achievement unlocked: id %u\n", event.achievement_id);
        break;

    case xlive::EventKind::SigninChanged:
        PublishGamertag(xlive::Client::Instance().identity().gamertag);
        KLOG("[xlive] signed in as %s\n",
             xlive::Client::Instance().identity().gamertag.c_str());
        // XN_SYS_SIGNINCHANGED belongs here, through PostGuestNotification.
        // It is deliberately not sent yet: the title would re-read a signin
        // state this step does not change, so the notification would announce
        // nothing. It arrives with the state, once the XLB logon messages are
        // real.
        break;

    case xlive::EventKind::ConnectionChanged:
        KLOG("[xlive] %s\n", xlive::Client::Instance().status().c_str());
        break;
    }
}

} // namespace

void CzXlive_Start(uint32_t titleId)
{
    if (g_started)
        return;

    // The off switch every automatic step in this runtime has.
    if (const char* off = std::getenv("CZ_NO_XLIVE"); off && off[0] == '1')
    {
        KLOG("[xlive] disabled by CZ_NO_XLIVE\n");
        return;
    }
    if (titleId == 0)
    {
        KLOG("[xlive] no title id, not starting\n");
        return;
    }

    xlive::Options options;
    options.title_id = titleId;
    options.log = OnLog;
    options.on_event = OnEvent;

    if (!xlive::Client::Instance().Start(options))
    {
        KLOG("[xlive] failed to start\n");
        return;
    }
    g_started = true;

    // A cached identity is already loaded by Start, so a player who has signed
    // in before sees their own gamertag from the first frame rather than after
    // the first successful round trip.
    const xlive::Identity identity = xlive::Client::Instance().identity();
    if (identity.xuid != xlive::kOfflineXuid)
        PublishGamertag(identity.gamertag);

    KLOG("[xlive] %s\n", xlive::Client::Instance().status().c_str());
}

bool CzXlive_SignedIn()
{
    return g_started && xlive::Client::Instance().online();
}

uint64_t CzXlive_Xuid(uint64_t fallback)
{
    if (!g_started)
        return fallback;
    const uint64_t xuid = xlive::Client::Instance().identity().xuid;
    return xuid == xlive::kOfflineXuid ? fallback : xuid;
}

const char* CzXlive_Gamertag(const char* fallback)
{
    if (!g_started)
        return fallback;
    std::lock_guard lock(g_nameMutex);
    if (!g_haveGamertag)
        return fallback;
    if (!g_loggedHandout)
    {
        g_loggedHandout = true;
        KLOG("[xlive] the title asked for the user's name and got '%s' (xuid %016llX)\n",
             g_gamertag,
             (unsigned long long)xlive::Client::Instance().identity().xuid);
    }
    return g_gamertag;
}

void CzXlive_RecordAchievements(const std::vector<uint16_t>& achievementIds)
{
    if (!g_started || achievementIds.empty())
        return;
    xlive::Client::Instance().Unlock(achievementIds);
}

void CzXlive_Shutdown(int timeoutMs)
{
    if (!g_started)
        return;
    const size_t pending = xlive::Client::Instance().pending_writes();
    if (pending != 0)
    {
        KLOG("[xlive] %zu write(s) still queued, giving them %d ms\n", pending, timeoutMs);
        xlive::Client::Instance().Flush(timeoutMs);
    }
    // Deliberately no Stop(): this is called from the window's Shutdown, which
    // ends in std::_Exit while guest threads are still running. Joining a
    // worker thread there is the same hazard running static destructors would
    // be, and there is nothing to lose by skipping it — everything is already
    // on disk.
}
