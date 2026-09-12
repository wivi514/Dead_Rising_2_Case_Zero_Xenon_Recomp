#include "xlive_glue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <xlive/client.h>

#include "content.h"
#include "klog.h"
#include "xlive_overlay_glue.h"
#include "xlive_session.h"
#include "xlive_social.h"
#include "xlive_net.h"
#include "xlive_stats.h"

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

// CZ_XLIVE_ONLINE=1, read once. See CzXlive_SignedInToLive.
bool g_onlineAllowed = false;

// A GATEWAY DROP IS NOT A SIGN-OUT UNTIL IT HAS LASTED. libxlive's access
// token expires hourly; the gateway websocket is closed on the expiry,
// refused once with 401, and reopened a few seconds later once the token is
// refreshed. The first co-op session ended exactly there: the runtime posted
// XN_SYS_SIGNINCHANGED on the close, the title re-read XamUserGetSigninState,
// got 1, logged "HW MM session found account: 0 is not signed in to xbox
// live!" and closed the session on both machines — every ~60-90 s of play,
// which had read as a 120 s join timeout in the headless runs. On the
// console a Live blip that short never reached the title either.
//
// So a drop is held for kSigninGraceMs: the title keeps reading 2 and hears
// nothing; if the gateway is back inside the grace the drop never happened
// from its point of view; only a drop that outlasts the grace is announced,
// by the first sign-in read after it expires (a guest thread, like the
// worker thread the immediate post used). CZ_XLIVE_SIGNIN_GRACE_MS=N sets
// it; 0 restores the immediate post as the control arm.
std::atomic<long long> g_dropSinceMs{-1};   // -1: no drop pending
std::atomic<bool> g_dropAnnounced{false};

long long NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

long long SigninGraceMs()
{
    static const long long v = [] {
        const char* e = std::getenv("CZ_XLIVE_SIGNIN_GRACE_MS");
        return e && *e ? std::strtoll(e, nullptr, 10) : 30000LL;
    }();
    return v;
}

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
    // The overlay draws its own notifications from the same events.
    CwOverlay_OnEvent(event);
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
        // XN_SYS_SIGNINCHANGED, with the mask of users whose state changed.
        // Only when the state the title reads can actually change: without
        // CZ_XLIVE_ONLINE it would re-read a 1 and the notification would
        // announce nothing.
        if (g_onlineAllowed)
            PostGuestNotification(XN_SYS_SIGNINCHANGED, 1);
        break;

    case xlive::EventKind::ConnectionChanged:
        KLOG("[xlive] %s\n", xlive::Client::Instance().status().c_str());
        // The gateway is what "signed in to Live" means here, so its coming
        // and going IS the signin state changing, and the title is told both
        // ways: the system notification it re-reads the state on, and the
        // Live one its own listener handles beside the invite.
        if (g_onlineAllowed)
        {
            const bool online = xlive::Client::Instance().online();
            const long long grace = SigninGraceMs();
            if (grace <= 0)
            {
                PostGuestNotification(XN_SYS_SIGNINCHANGED, 1);
            }
            else if (!online)
            {
                // Hold it. CzXlive_SignedIn keeps answering true meanwhile,
                // and XamUserGetSigninState announces the drop if it lasts.
                g_dropAnnounced.store(false);
                g_dropSinceMs.store(NowMs());
                KLOG("[xlive] gateway dropped; the title is not told for %lld ms "
                     "(CZ_XLIVE_SIGNIN_GRACE_MS)\n", grace);
            }
            else
            {
                const long long since = g_dropSinceMs.exchange(-1);
                const bool announced = g_dropAnnounced.exchange(false);
                if (since >= 0 && !announced)
                    KLOG("[xlive] gateway back after %lld ms; inside the grace, the title "
                         "never heard it drop\n", NowMs() - since);
                else
                    PostGuestNotification(XN_SYS_SIGNINCHANGED, 1);
            }
            XliveSocial_OnConnectionChanged(online);
        }
        break;

    // The social events. Each becomes the notification the title's own
    // listener is polling for; the ids were read off those listeners
    // (kernel/xlive_social.h).
    case xlive::EventKind::FriendsChanged:
        XliveSocial_OnFriendsChanged();
        break;

    case xlive::EventKind::InviteReceived:
        // Delivered only when no launcher is connected to ask the player.
        KLOG("[xlive] invite from %s to session %016llX\n", event.gamertag.c_str(),
             (unsigned long long)event.session_id);
#if CZ_HAVE_XLIVE_OVERLAY
        // With the overlay built there IS a place to ask: it has toasted the
        // invitation, and Accept there tells the server, which answers with
        // invite_taken — the InviteAccepted path below. Nothing is taken on
        // the player's behalf.
        break;
#else
        XliveSocial_OnInviteReceived(event.invite_id, event.xuid, event.title_id,
                                     event.session_id, /*accept=*/true);
        break;
#endif

    case xlive::EventKind::InviteAccepted:
        // The player said yes in the launcher; this is the title's cue.
        KLOG("[xlive] invite from %s to session %016llX accepted in the launcher\n",
             event.gamertag.c_str(), (unsigned long long)event.session_id);
        XliveSocial_OnInviteReceived(event.invite_id, event.xuid, event.title_id,
                                     event.session_id, /*accept=*/false);
        break;

    case xlive::EventKind::InviteAnswered:
        KLOG("[xlive] %s %s the invitation\n", event.gamertag.c_str(),
             event.accepted ? "accepted" : "declined");
        break;

    case xlive::EventKind::MessageReceived:
        // The overlay shows it as a toast (and lets the player reply); the
        // log keeps it for a run without one.
        KLOG("[xlive] message from %s: %s\n", event.gamertag.c_str(), event.message.c_str());
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

    // THE XENONLIVE LAUNCHER IS THE WAY ONLINE (operator decision, v1.1.0). The
    // launcher signs the player in and starts the game with CZ_XLIVE_ONLINE=1
    // (and CZ_XLIVE_COOP=1); a game started any other way is the DEFAULT
    // PROFILE, offline — libxlive is not even started, so no cached account
    // leaks in as a gamertag, no achievement is recorded against anyone, and
    // nothing online can be reached. It used to load the cached identity and
    // show its gamertag while telling the title it was signed out, which was
    // neither one thing nor the other.
    if (const char* on = std::getenv("CZ_XLIVE_ONLINE"); on && on[0] == '1')
    {
        g_onlineAllowed = true;
        KLOG("[xlive] CZ_XLIVE_ONLINE: the title will be told it is signed in to Live\n");
    }
    else
    {
        KLOG("[xlive] not started through the XenonLive launcher (CZ_XLIVE_ONLINE is "
             "unset): the default profile, offline — no account, no co-op\n");
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
    CwOverlay_SetClient(&xlive::Client::Instance(), titleId);

    // A cached identity is already loaded by Start, so a player who has signed
    // in before sees their own gamertag from the first frame rather than after
    // the first successful round trip.
    const xlive::Identity identity = xlive::Client::Instance().identity();
    if (identity.xuid != xlive::kOfflineXuid)
    {
        PublishGamertag(identity.gamertag);
        // SAVES ARE PER PROFILE (operator decision, v1.1.0): this account's
        // saves live in its own folder under the saved-games location, named
        // by the gamertag; the offline default profile keeps "default". Set
        // here, before any guest code runs, so the title's first save
        // enumeration already looks in the right place.
        ContentSetProfile(identity.gamertag);
    }

    KLOG("[xlive] %s\n", xlive::Client::Instance().status().c_str());

    // Co-op, if it was asked for. It is a separate switch from this one
    // because identity and achievements are finished work and the session
    // surface is not: a player who wants their gamertag back should not have
    // to opt into unexercised matchmaking to get it.
    XliveSession_Start();
    XliveSession_SelfTest();
    XliveSocial_SelfTest();
    // Leaderboard reads ride the online switch: a title only asks for a board
    // once it believes it is signed in to Live, and CZ_XLIVE_ONLINE is what
    // lets it believe that.
    XliveStats_Start();
    XliveStats_SelfTest();
    XliveNet_SelfTest();
}

bool CzXlive_SignedIn()
{
    if (!g_started)
        return false;
    if (xlive::Client::Instance().online())
        return true;
    // Inside a held gateway drop the answer is still yes; past it, the first
    // reader announces the sign-out the immediate post would have made.
    const long long since = g_dropSinceMs.load();
    if (since < 0)
        return false;
    if (NowMs() - since < SigninGraceMs())
        return true;
    if (!g_dropAnnounced.exchange(true))
    {
        KLOG("[xlive] gateway drop outlasted the grace; telling the title it is signed out\n");
        if (g_onlineAllowed)
            PostGuestNotification(XN_SYS_SIGNINCHANGED, 1);
    }
    return false;
}

bool CzXlive_SignedInToLive()
{
    return g_onlineAllowed && CzXlive_SignedIn();
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

void CzXlive_RecordStats(const std::vector<CzXliveStatView>& views)
{
    if (!g_started || views.empty())
        return;

    std::vector<xlive::Client::StatView> out;
    out.reserve(views.size());
    for (const auto& view : views)
    {
        xlive::Client::StatView converted;
        converted.view_id = view.viewId;
        converted.properties.reserve(view.properties.size());
        for (const auto& property : view.properties)
        {
            xlive::Client::StatProperty p;
            p.id = property.id;
            p.type = static_cast<xlive::Client::StatProperty::Type>(property.type);
            p.integer = property.integer;
            p.real = property.real;
            p.text = property.text;
            converted.properties.push_back(std::move(p));
        }
        out.push_back(std::move(converted));
    }
    xlive::Client::Instance().WriteStats(out);
}

// The composed presence. Written from two guest threads (the one setting a
// context and the session completion thread), so it sits under its own lock.
std::mutex g_presenceMutex;
xlive::Client::PresenceUpdate g_presence;
// "The title has not said anything yet" is not the same as "the title said
// 0", and 0 is what it says first ("Navigating the menus"). Without this the
// first value would be dropped as a no-op and the one log line that proves
// the path from the dispatcher to here would never print.
bool g_presenceValueSet = false;

void PublishPresence()
{
    xlive::Client::PresenceUpdate snapshot;
    {
        std::lock_guard lock(g_presenceMutex);
        snapshot = g_presence;
    }
    // Outside the lock: SetPresence takes libxlive's own mutex, and holding
    // two locks in two orders across two libraries is how a deadlock is built.
    xlive::Client::Instance().SetPresence(snapshot);
}

void CzXlive_SetPresence(uint32_t presenceValue)
{
    if (!g_started)
        return;
    {
        std::lock_guard lock(g_presenceMutex);
        if (g_presenceValueSet && g_presence.presence_value == presenceValue)
            return;
        g_presenceValueSet = true;
        g_presence.presence_value = presenceValue;
    }
    KLOG("[xlive] presence value %u\n", presenceValue);
    PublishPresence();
}

void CzXlive_SetPresenceSession(uint64_t sessionId, bool joinable)
{
    if (!g_started)
        return;
    {
        std::lock_guard lock(g_presenceMutex);
        if (g_presence.session_id == sessionId && g_presence.joinable == joinable)
            return;
        g_presence.session_id = sessionId;
        g_presence.joinable = joinable;
    }
    PublishPresence();
}

void CzXlive_Shutdown(int timeoutMs)
{
    if (!g_started)
        return;
    // The session thread first: it writes into guest memory and completes
    // overlappeds, and it must not still be doing that while the rest of the
    // process is being torn down.
    XliveSession_Shutdown();
    XliveStats_Shutdown();
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
