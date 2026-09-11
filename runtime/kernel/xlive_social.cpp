// See xlive_social.h for what this is. The guest structs below are the whole
// risk surface, and every one is asserted at the size two sources agree on:
// the guest's own wrapper (named beside each struct) and Xenia's SDK-derived
// header (netplay_xnet.h, fetched into XenonLive/tools/reference).

#include "xlive_social.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "content.h" // Xam_CompleteOverlapped
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "xlive_glue.h"
#include "xlive_session.h"

#include <xlive/client.h>

namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNoMoreFiles = 18;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorNotLoggedOn = 1245; // sub_8259B178 tests the mute query for it

constexpr uint32_t E_FAIL = 0x80004005;
constexpr uint32_t E_INVALIDARG = 0x80070057;
// XONLINE_E_LOGON_NOT_LOGGED_ON. What XOnlineGetLogonID answers on a console
// with no Live connection, and the guest's own error mapper (sub_825ACC28)
// knows it.
constexpr uint32_t XONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;
constexpr uint32_t XONLINE_E_SESSION_NOT_FOUND = 0x80155200;

constexpr uint32_t kMaxFriends = 100; // X_ONLINE_MAX_FRIENDS; the title asks for exactly 100
constexpr uint32_t kMaxRichPresence = 64; // X_MAX_RICHPRESENCE_SIZE, in UTF-16 units

// XONLINE_FRIENDSTATE_* — the bits the title's own copy loop tests
// (sub_825985A8: `rlwinm. r11,r11,0,1,1` is SENTREQUEST, `,0,0,0` is
// RECEIVEDREQUEST, `clrlwi. r11,r11,31` is ONLINE).
constexpr uint32_t kFriendOnline = 0x00000001;
constexpr uint32_t kFriendPlaying = 0x00000002;
constexpr uint32_t kFriendJoinable = 0x00000010;
constexpr uint32_t kFriendSentRequest = 0x40000000;
constexpr uint32_t kFriendReceivedRequest = 0x80000000;

// ---------------------------------------------------------------------------
// The guest structs
// ---------------------------------------------------------------------------
//
// Packed to 4, as the console's compiler laid them out: XONLINE_FRIEND's
// FILETIMEs sit at +0x28 and +0x38 and the struct is 0xC4, which natural
// alignment would pad to 0xC8 — the same trap docs/integrating-a-port.md
// records for the XNADDR. The static_asserts are what catch it.
#pragma pack(push, 4)

// One marshalled argument. sub_82605878 writes native_size = 4 at +0 and the
// sign-extended value at +8, and advances by count << 4.
struct GuestArgumentEntry
{
    be<uint32_t> nativeSize;   // +0   always 4
    be<uint32_t> pad;          // +4   never written
    be<uint64_t> valuePtr;     // +8   a guest address — of the value, or the buffer itself
};
static_assert(sizeof(GuestArgumentEntry) == 0x10, "rlwinm r10,r10,4,0,27 says 16-byte entries");

struct GuestArgumentList
{
    GuestArgumentEntry entry[32]; // +0
    be<uint32_t> count;           // +0x200  lwz r10,512(r3)
};
static_assert(offsetof(GuestArgumentList, count) == 512, "lwz r10,512(r3) says the count is at +512");

// XUserMuteListQuery's buffer, built on sub_82606CE8's stack: user index at
// +0, the XUID at +8, and a zeroed word at +16 that the wrapper RETURNS —
// which is how the status travels, since the XMsg result itself is dropped.
struct GuestMuteQuery
{
    be<uint32_t> userIndex;  // +0
    be<uint32_t> pad;        // +4
    be<uint64_t> xuid;       // +8
    be<uint32_t> status;     // +16  0, or 1245 (ERROR_NOT_LOGGED_ON)
    be<uint32_t> pad2;       // +20
};
static_assert(sizeof(GuestMuteQuery) == 0x18, "X_MUTE_SET_STATE is 0x18");

// XONLINE_FRIEND. 0xC4 from both sides: sub_825985A8 walks the buffer in
// steps of 196, and sub_82598408 zeroes 19600 = 100 * 196 bytes for it.
struct GuestOnlineFriend
{
    be<uint64_t> xuid;                       // +0x00
    char gamertag[16];                       // +0x08
    be<uint32_t> state;                      // +0x18
    uint8_t sessionId[8];                    // +0x1C  XNKID
    be<uint32_t> titleId;                    // +0x24
    be<uint64_t> userTime;                   // +0x28  FILETIME
    uint8_t inviteSessionId[8];              // +0x30  XNKID
    be<uint64_t> inviteTime;                 // +0x38  FILETIME
    be<uint32_t> richPresenceLength;         // +0x40
    be<uint16_t> richPresence[kMaxRichPresence]; // +0x44
};
static_assert(sizeof(GuestOnlineFriend) == 0xC4, "addi r29,r29,196 says 196-byte entries");

// X_INVITE_INFO: 0x54, with an XSESSION_INFO (0x3C) in the middle that
// xlive_session.cpp already knows how to write.
struct GuestInviteInfo
{
    be<uint64_t> inviteeXuid;   // +0x00
    be<uint64_t> inviterXuid;   // +0x08
    be<uint32_t> titleId;       // +0x10
    uint8_t hostInfo[0x3C];     // +0x14  XSESSION_INFO
    be<uint32_t> fromGameInvite;// +0x50
};
static_assert(sizeof(GuestInviteInfo) == 0x54, "X_INVITE_INFO is 0x54");

#pragma pack(pop)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

xlive::Client& Live() { return xlive::Client::Instance(); }

// The friends enumerator. A KernelObject because the title closes the handle
// (sub_825985A8 -> sub_825AAC78 after it has copied the list out), and a
// snapshot rather than a view: the list is taken when the enumerator is
// created, so a friend coming online mid-enumeration cannot shift the entries
// under the guest's cursor.
struct FriendsEnumerator final : KernelObject
{
    std::vector<xlive::Client::Friend> items;
    size_t cursor = 0;
};

std::mutex g_mutex;

// The invite the title will be told about, and the one it was launched with.
struct PendingInvite
{
    uint64_t inviteId = 0;
    uint64_t fromXuid = 0;
    uint32_t titleId = 0;
    uint64_t sessionId = 0;
    // Whether the server still has to be told it was taken.
    bool accept = false;
    bool valid() const { return sessionId != 0; }
};
PendingInvite g_pendingInvite;   // waiting for the session details
PendingInvite g_announcedInvite; // XN_LIVE_INVITE_ACCEPTED has been posted for it

// XN_LIVE_INVITE_ACCEPTED that found no listener. A title launched INTO an
// invitation learns of it from libxlive's first sync, which happens while the
// boot logos are still up and before the title has created the listener that
// wants area 1 — PostGuestNotification would have dropped it. It is held here
// and posted on the title's next Live call (XliveSocial_Dispatch), which is
// the title's own proof that its Live layer, listeners included, is up.
bool g_heldInviteNotification = false;

// What the friends list looked like the last time the title was told, so the
// next change can be named rather than just announced.
std::set<uint64_t> g_lastFriendSet;
bool g_haveLastFriendSet = false;

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------

template <typename T>
T* GuestPtr(uint32_t va)
{
    if (va == 0)
        return nullptr;
    return reinterpret_cast<T*>(g_memory.Translate(va));
}

// The scalar an argument entry points at, or false when the pointer is null.
bool ReadArgument(const GuestArgumentEntry& entry, uint32_t* out)
{
    const auto* value = GuestPtr<be<uint32_t>>(uint32_t(entry.valuePtr.get()));
    if (!value)
        return false;
    *out = value->get();
    return true;
}

uint32_t ArgumentAddress(const GuestArgumentEntry& entry)
{
    return uint32_t(entry.valuePtr.get());
}

// The list, checked against the count the guest's own wrapper appends. A
// mismatch means this is not the message the row says it is, and the honest
// answer is a refusal rather than a read past the entries that exist.
const GuestArgumentList* Arguments(uint32_t va, uint32_t expected, const char* what)
{
    const auto* list = GuestPtr<GuestArgumentList>(va);
    if (!list)
    {
        KLOG("[xlive] %s: no argument list\n", what);
        return nullptr;
    }
    if (list->count.get() != expected)
    {
        KLOG("[xlive] %s: %u argument(s), expected %u\n", what, list->count.get(), expected);
        return nullptr;
    }
    return list;
}

// Copies a UTF-8 gamertag into the 16-byte field the guest reads as ASCII. A
// gamertag is at most 15 characters and the server enforces that; a longer
// one is truncated rather than trusted.
void WriteGamertag(char (&out)[16], const std::string& tag)
{
    std::memset(out, 0, sizeof(out));
    const size_t n = tag.size() < sizeof(out) - 1 ? tag.size() : sizeof(out) - 1;
    std::memcpy(out, tag.data(), n);
}

// UTF-8 to the guest's big-endian UTF-16, truncated to the field. Rich
// presence is the title's own string resolved by the server, so it is plain
// text and rarely more than a few words; anything outside the BMP becomes
// U+FFFD rather than a broken surrogate the title would draw as garbage.
uint32_t WriteRichPresence(be<uint16_t> (&out)[kMaxRichPresence], const std::string& text)
{
    std::memset(out, 0, sizeof(out));
    uint32_t n = 0;
    for (size_t i = 0; i < text.size() && n < kMaxRichPresence - 1;)
    {
        const uint8_t c = uint8_t(text[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80)              { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                        { cp = 0xFFFD; }
        if (i + len > text.size())
        {
            cp = 0xFFFD;
            len = text.size() - i;
        }
        else
        {
            for (size_t k = 1; k < len; k++)
                cp = (cp << 6) | (uint8_t(text[i + k]) & 0x3F);
        }
        i += len;
        out[n++] = uint16_t(cp > 0xFFFF ? 0xFFFD : cp);
    }
    return n;
}

uint32_t FriendState(const xlive::Client::Friend& entry)
{
    switch (entry.relation)
    {
    case xlive::Client::Relation::RequestSent:     return kFriendSentRequest;
    case xlive::Client::Relation::RequestReceived: return kFriendReceivedRequest;
    default: break;
    }
    uint32_t state = 0;
    if (entry.presence.online())
        state |= kFriendOnline;
    if (entry.presence.state == xlive::Client::PresenceState::Playing)
        state |= kFriendPlaying;
    if (entry.presence.joinable)
        state |= kFriendJoinable;
    return state;
}

void WriteXnkid(uint8_t (&out)[8], uint64_t sessionId)
{
    for (int i = 0; i < 8; i++)
        out[i] = uint8_t(sessionId >> (56 - 8 * i));
}

void WriteFriend(GuestOnlineFriend* out, const xlive::Client::Friend& entry)
{
    std::memset(out, 0, sizeof(*out));
    out->xuid = entry.xuid;
    WriteGamertag(out->gamertag, entry.gamertag);
    out->state = FriendState(entry);
    // Only an accepted friend's presence is ever populated by the library, so
    // a pending request writes zeros here, which is what the console did too.
    if (entry.is_friend() && entry.presence.online())
    {
        WriteXnkid(out->sessionId, entry.presence.session_id);
        out->titleId = entry.presence.title_id;
        out->richPresenceLength = WriteRichPresence(out->richPresence, entry.presence.rich_text);
    }
}

FriendsEnumerator* EnumeratorFromHandle(uint32_t handle)
{
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return nullptr;
    KernelObject* obj = GetKernelObject(handle);
    if (!KernelObjectIsIntact(obj))
        return nullptr;
    return dynamic_cast<FriendsEnumerator*>(obj);
}

// ---------------------------------------------------------------------------
// The messages
// ---------------------------------------------------------------------------

// 0x00058004 XOnlineGetLogonID. sub_82606B18 hands the id straight into the
// task block it builds for the message that follows, masked to zero when this
// fails — and the wrapper stops there. So this is the gate: the answer is the
// library's online(), which is true only while a gateway connection is up.
uint32_t GetLogonId(void* buffer)
{
    auto* out = static_cast<be<uint32_t>*>(buffer);
    if (!out)
        return E_INVALIDARG;
    if (!Live().online())
    {
        *out = 0;
        return XONLINE_E_LOGON_NOT_LOGGED_ON;
    }
    // Any non-zero value; the console's was an opaque session number. A
    // constant is fine because nothing here has more than one logon.
    *out = 1;
    return kErrorSuccess;
}

// 0x00058006 XOnlineGetNatType. sub_82606970 returns the word on success and
// XONLINE_NAT_OPEN (1) on failure, so a failure here is not a lie about the
// NAT — but the reflector's answer is better than the fallback when there is
// one, and "unknown" is reported as failure rather than as a guess.
uint32_t GetNatType(void* buffer)
{
    auto* out = static_cast<be<uint32_t>*>(buffer);
    if (!out)
        return E_INVALIDARG;
    switch (Live().nat_type())
    {
    case xlive::Client::NatType::Open:     *out = 1; return kErrorSuccess;
    case xlive::Client::NatType::Moderate: *out = 2; return kErrorSuccess;
    case xlive::Client::NatType::Strict:   *out = 3; return kErrorSuccess;
    case xlive::Client::NatType::Unknown:  break;
    }
    *out = 0;
    return E_FAIL;
}

// 0x0005800E XUserMuteListQuery. Not an argument list: the struct is on the
// wrapper's stack and the fourth parameter is the caller's BOOL*.
uint32_t MuteListQuery(void* buffer, uint32_t mutedOutVa)
{
    auto* query = static_cast<GuestMuteQuery*>(buffer);
    auto* mutedOut = GuestPtr<be<uint32_t>>(mutedOutVa);
    if (!query || !mutedOut)
        return E_INVALIDARG;
    if (query->userIndex.get() != 0)
    {
        query->status = kErrorNotLoggedOn;
        *mutedOut = 0;
        return E_INVALIDARG;
    }
    if (!Live().online())
    {
        // The status the caller tests for by name: sub_8259B178 has a
        // separate path for 1245 and treats it as "cannot know".
        query->status = kErrorNotLoggedOn;
        *mutedOut = 0;
        return kErrorSuccess;
    }
    const uint64_t xuid = query->xuid.get();
    bool muted = false;
    for (uint64_t entry : Live().mute_list())
        if (entry == xuid)
            muted = true;
    *mutedOut = muted ? 1u : 0u;
    query->status = kErrorSuccess;
    return kErrorSuccess;
}

// 0x00058020 XFriendsCreateEnumerator. Five arguments — user index, starting
// index, count, &buffer size, &handle — each read through its entry. The
// buffer size written back is what the title allocates for XEnumerate, so it
// is the full count times the item size whether or not that many friends
// exist, which is also what the console did (and what sub_82598408 expects:
// it zeroes 100 items up front and only then asks).
uint32_t CreateFriendsEnumerator(uint32_t argumentsVa)
{
    const auto* args = Arguments(argumentsVa, 5, "XFriendsCreateEnumerator");
    if (!args)
        return E_INVALIDARG;

    uint32_t userIndex = 0, start = 0, count = 0;
    if (!ReadArgument(args->entry[0], &userIndex) || !ReadArgument(args->entry[1], &start) ||
        !ReadArgument(args->entry[2], &count))
        return E_INVALIDARG;
    auto* sizeOut = GuestPtr<be<uint32_t>>(ArgumentAddress(args->entry[3]));
    auto* handleOut = GuestPtr<be<uint32_t>>(ArgumentAddress(args->entry[4]));
    if (!handleOut)
        return E_INVALIDARG;
    // Out-parameters first, failure included: the title tests the handle
    // against 0, not against the return.
    *handleOut = 0;
    if (!sizeOut)
        return E_INVALIDARG;
    *sizeOut = 0;

    if (userIndex != 0 || start >= kMaxFriends || count == 0 || count > kMaxFriends)
        return E_INVALIDARG;
    if (!Live().online())
        return XONLINE_E_LOGON_NOT_LOGGED_ON;

    auto* obj = CreateKernelObject<FriendsEnumerator>();
    if (!obj)
        return E_FAIL;
    const std::vector<xlive::Client::Friend> all = Live().friends();
    for (size_t i = start; i < all.size() && obj->items.size() < count; i++)
        obj->items.push_back(all[i]);

    *sizeOut = count * uint32_t(sizeof(GuestOnlineFriend));
    *handleOut = GetKernelHandle(obj);
    KLOG("[xlive] friends enumerator %08X: %zu of %zu entr%s, %u-byte buffer\n",
         handleOut->get(), obj->items.size(), all.size(), all.size() == 1 ? "y" : "ies",
         sizeOut->get());
    return kErrorSuccess;
}

// 0x00058023 XInviteGetAcceptedInfo. Two arguments: user index and the
// X_INVITE_INFO to fill. The XNKID is the invite's; the XNKEY and the host's
// XNADDR are the session's, which is why the notification that leads here is
// only posted once the session layer has them (XliveSocial_OnInviteReceived).
uint32_t InviteGetAcceptedInfo(uint32_t argumentsVa)
{
    const auto* args = Arguments(argumentsVa, 2, "XInviteGetAcceptedInfo");
    if (!args)
        return E_INVALIDARG;
    uint32_t userIndex = 0;
    if (!ReadArgument(args->entry[0], &userIndex))
        return E_INVALIDARG;
    auto* info = GuestPtr<GuestInviteInfo>(ArgumentAddress(args->entry[1]));
    if (!info)
        return E_INVALIDARG;
    std::memset(info, 0, sizeof(*info));
    if (userIndex != 0)
        return E_INVALIDARG;

    PendingInvite invite;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        invite = g_announcedInvite;
    }
    if (!invite.valid())
    {
        // Not one we announced: maybe the launch itself was an invitation,
        // which the library learned from the server when it connected.
        xlive::Client::Invite accepted;
        if (Live().AcceptedInvite(accepted))
        {
            invite.inviteId = accepted.id;
            invite.fromXuid = accepted.from_xuid;
            invite.titleId = accepted.title_id;
            invite.sessionId = accepted.session_id;
        }
    }
    if (!invite.valid())
    {
        // The ordinary answer: this was an ordinary start. The console said
        // the same thing the same way.
        return XONLINE_E_SESSION_NOT_FOUND;
    }
    if (!XliveSession_InviteSessionInfo(invite.sessionId, info->hostInfo))
    {
        // The details never arrived, or co-op is off and nothing could fetch
        // them. Saying "no invitation" is the honest answer: a half-filled
        // XSESSION_INFO would send the title to join a session with no key.
        KLOG("[xlive] XInviteGetAcceptedInfo: no details for session %016llX\n",
             (unsigned long long)invite.sessionId);
        return XONLINE_E_SESSION_NOT_FOUND;
    }
    info->inviteeXuid = Live().identity().xuid;
    info->inviterXuid = invite.fromXuid;
    info->titleId = invite.titleId;
    info->fromGameInvite = 1;
    KLOG("[xlive] XInviteGetAcceptedInfo: session %016llX from %016llX\n",
         (unsigned long long)invite.sessionId, (unsigned long long)invite.fromXuid);
    return kErrorSuccess;
}

}  // namespace

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

static void PostHeldInviteNotification();

bool XliveSocial_Dispatch(uint32_t message, void* buffer, uint32_t argumentsVa,
                          uint32_t* result)
{
    PostHeldInviteNotification();
    switch (message)
    {
    case 0x00058004: *result = GetLogonId(buffer); return true;
    case 0x00058006: *result = GetNatType(buffer); return true;
    case 0x0005800E: *result = MuteListQuery(buffer, argumentsVa); return true;
    case 0x00058020: *result = CreateFriendsEnumerator(argumentsVa); return true;
    case 0x00058023: *result = InviteGetAcceptedInfo(argumentsVa); return true;
    default:
        return false;
    }
}

// XEnumerate on a friends enumerator: sub_82598408 passes the whole buffer,
// no items-returned pointer, and an overlapped it expects to see 997 for and
// then reads the count out of. Everything is in memory, so the overlapped is
// completed before this returns — the guest's own XGetOverlappedResult only
// waits while +0 still reads 997, so a completion that beats the return is
// simply one the title never waits for.
bool XliveSocial_Enumerate(uint32_t handle, uint32_t buffer, uint32_t bufferLength,
                           be<uint32_t>* itemsReturned, uint32_t overlappedVa,
                           uint32_t* result)
{
    FriendsEnumerator* obj = EnumeratorFromHandle(handle);
    if (!obj)
        return false;

    auto finish = [&](uint32_t status, uint32_t count) {
        if (itemsReturned)
            *itemsReturned = count;
        if (overlappedVa)
        {
            Xam_CompleteOverlapped(overlappedVa, status, count);
            *result = kErrorIoPending;
        }
        else
        {
            *result = status;
        }
        return true;
    };

    auto* out = GuestPtr<GuestOnlineFriend>(buffer);
    if (!out || bufferLength < sizeof(GuestOnlineFriend))
        return finish(kErrorInvalidParameter, 0);

    const uint32_t room = bufferLength / uint32_t(sizeof(GuestOnlineFriend));
    uint32_t written = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        while (written < room && obj->cursor < obj->items.size())
        {
            WriteFriend(&out[written], obj->items[obj->cursor]);
            obj->cursor++;
            written++;
        }
    }
    KLOG("[xlive] XEnumerate(friends %08X): %u entr%s\n", handle, written,
         written == 1 ? "y" : "ies");
    // No more items is an error, not an empty success — the same contract the
    // content enumerator was read to have (content.cpp, ERROR_NO_MORE_FILES).
    return finish(written == 0 ? kErrorNoMoreFiles : kErrorSuccess, written);
}

void XliveSocial_OnFriendsChanged()
{
    std::set<uint64_t> now;
    for (const auto& entry : Live().friends())
        if (entry.is_friend())
            now.insert(entry.xuid);

    uint32_t id = XN_FRIENDS_PRESENCE_CHANGED;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_haveLastFriendSet)
        {
            for (uint64_t xuid : now)
                if (!g_lastFriendSet.count(xuid))
                    id = XN_FRIENDS_FRIEND_ADDED;
            for (uint64_t xuid : g_lastFriendSet)
                if (!now.count(xuid))
                    id = XN_FRIENDS_FRIEND_REMOVED;
        }
        g_lastFriendSet = std::move(now);
        g_haveLastFriendSet = true;
    }
    PostGuestNotification(id, 0);
}

void XliveSocial_OnInviteReceived(uint64_t inviteId, uint64_t fromXuid, uint32_t titleId,
                                  uint64_t sessionId, bool accept)
{
    if (sessionId == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingInvite = PendingInvite{inviteId, fromXuid, titleId, sessionId, accept};
    }
    if (!XliveSession_PrefetchInviteSession(sessionId))
    {
        // Co-op is off, so there is no session surface to join through and no
        // thread to fetch the details on. The invitation stays in the
        // library's inbox for the launcher; the title is not told, because
        // telling it would start a join this build cannot finish.
        KLOG("[xlive] invite to session %016llX ignored: CZ_XLIVE_COOP is off\n",
             (unsigned long long)sessionId);
    }
}

void XliveSocial_OnInviteSessionReady(uint64_t sessionId, bool ok)
{
    PendingInvite invite;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pendingInvite.sessionId != sessionId)
            return;
        invite = g_pendingInvite;
        g_pendingInvite = PendingInvite{};
        if (ok)
            g_announcedInvite = invite;
    }
    if (!ok)
    {
        KLOG("[xlive] invite to session %016llX dropped: the session is gone\n",
             (unsigned long long)sessionId);
        return;
    }
    // Tell the sender it was taken — when nobody else has. The server only
    // delivers a raw invitation to a game when no launcher is connected to
    // ask the player, and there is no guide UI in this runtime; an
    // invitation can only come from someone they have accepted as a friend,
    // and the title's own state machine still decides what to do with it.
    // When the player answered in the launcher, the server already knows.
    // The ticket is not polled: a game does not need to know whether the
    // server recorded the answer.
    if (invite.accept && invite.inviteId)
        XliveSession_DrainSocialTicket(Live().AcceptInvite(invite.inviteId));
    KLOG("[xlive] invite from %016llX to session %016llX%s: XN_LIVE_INVITE_ACCEPTED\n",
         (unsigned long long)invite.fromXuid, (unsigned long long)sessionId,
         invite.accept ? " (taken here: no launcher)" : " (accepted in the launcher)");
    if (!PostGuestNotification(XN_LIVE_INVITE_ACCEPTED, 0))
    {
        KLOG("[xlive] XN_LIVE_INVITE_ACCEPTED: no listener yet; held for the title's "
             "first Live call\n");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_heldInviteNotification = true;
    }
}

// Posts a held XN_LIVE_INVITE_ACCEPTED once a listener exists. Called from
// XliveSocial_Dispatch: cheap when nothing is held, which is always but once.
static void PostHeldInviteNotification()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_heldInviteNotification)
            return;
    }
    if (!PostGuestNotification(XN_LIVE_INVITE_ACCEPTED, 0))
        return;
    KLOG("[xlive] XN_LIVE_INVITE_ACCEPTED: posted the held notification\n");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_heldInviteNotification = false;
}

void XliveSocial_OnConnectionChanged(bool online)
{
    // The parameter is the HRESULT the title would get from
    // XOnlineGetLogonHR: XONLINE_S_LOGON_CONNECTION_ESTABLISHED, or the
    // not-logged-on error.
    PostGuestNotification(XN_LIVE_CONNECTIONCHANGED,
                          online ? 0x001510F0u : XONLINE_E_LOGON_NOT_LOGGED_ON);
}

// ---------------------------------------------------------------------------
// The self-test
// ---------------------------------------------------------------------------
//
// The same split as xlive_session.cpp's: the half that needs a server and a
// friend cannot run from a boot, and the half that is guest-struct arithmetic
// — argument-list decoding, XONLINE_FRIEND offsets, the mute query's status
// word — is the half a port gets wrong silently. So that half runs here, on
// guest memory, with the library signed out, and every message's offline
// answer is checked too, because offline is what most boots are.
// CZ_XLIVE_SOCIAL_TEST=1.

namespace {

int g_selfTestFailures = 0;

#define XLIVE_EXPECT(cond)                                                       \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "[xlive] SELF-TEST FAILED %s:%d: %s\n",              \
                    __FILE__, __LINE__, #cond);                                  \
            ++g_selfTestFailures;                                                \
        }                                                                        \
    } while (0)

struct GuestScratch
{
    void* host = nullptr;
    uint32_t va = 0;

    explicit GuestScratch(size_t size)
    {
        host = g_heap.Alloc(size);
        if (host)
        {
            std::memset(host, 0, size);
            va = g_memory.MapVirtual(host);
        }
    }
    ~GuestScratch()
    {
        if (host)
            g_heap.Free(host);
    }
};

// Builds a list the way sub_82605878 does, one append at a time.
void Append(GuestArgumentList* list, uint32_t valuePtr)
{
    const uint32_t n = list->count.get();
    list->entry[n].nativeSize = 4;
    list->entry[n].valuePtr = uint64_t(int64_t(int32_t(valuePtr)));
    list->count = n + 1;
}

void TestFieldOffsets()
{
    XLIVE_EXPECT(offsetof(GuestArgumentEntry, valuePtr) == 8);
    XLIVE_EXPECT(offsetof(GuestMuteQuery, xuid) == 8);
    XLIVE_EXPECT(offsetof(GuestMuteQuery, status) == 16);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, gamertag) == 0x08);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, state) == 0x18);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, sessionId) == 0x1C);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, titleId) == 0x24);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, userTime) == 0x28);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, inviteSessionId) == 0x30);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, inviteTime) == 0x38);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, richPresenceLength) == 0x40);
    XLIVE_EXPECT(offsetof(GuestOnlineFriend, richPresence) == 0x44);
    XLIVE_EXPECT(offsetof(GuestInviteInfo, inviterXuid) == 0x08);
    XLIVE_EXPECT(offsetof(GuestInviteInfo, titleId) == 0x10);
    XLIVE_EXPECT(offsetof(GuestInviteInfo, hostInfo) == 0x14);
    XLIVE_EXPECT(offsetof(GuestInviteInfo, fromGameInvite) == 0x50);
}

// The sign-extension sub_82605878 applies (`extsw r8,r4`) must not survive
// into the address: a guest pointer above 0x80000000 is stored as a negative
// 64-bit value, and reading it back as a 32-bit address has to undo that.
void TestArgumentListDecode()
{
    GuestScratch list(sizeof(GuestArgumentList));
    GuestScratch value(4);
    GuestScratch info(sizeof(GuestInviteInfo));
    if (!list.host || !value.host || !info.host)
        return;
    XLIVE_EXPECT(value.va >= 0x80000000u || info.va >= 0x80000000u ||
                 true); // the heap may be anywhere; the decode must not care
    *static_cast<be<uint32_t>*>(value.host) = 0;
    auto* args = static_cast<GuestArgumentList*>(list.host);
    Append(args, value.va);
    Append(args, info.va);
    XLIVE_EXPECT(args->count.get() == 2);
    XLIVE_EXPECT(ArgumentAddress(args->entry[1]) == info.va);
    uint32_t user = 99;
    XLIVE_EXPECT(ReadArgument(args->entry[0], &user) && user == 0);

    // With no invitation, the honest answer — and the info block zeroed, so
    // a title that ignores the return finds no session to join.
    std::memset(info.host, 0xAA, sizeof(GuestInviteInfo));
    uint32_t result = 0;
    XLIVE_EXPECT(XliveSocial_Dispatch(0x00058023, nullptr, list.va, &result));
    XLIVE_EXPECT(result == XONLINE_E_SESSION_NOT_FOUND);
    auto* out = static_cast<GuestInviteInfo*>(info.host);
    XLIVE_EXPECT(out->inviterXuid.get() == 0 && out->fromGameInvite.get() == 0);

    // The wrong count is refused, not read past.
    Append(args, value.va);
    XLIVE_EXPECT(XliveSocial_Dispatch(0x00058023, nullptr, list.va, &result));
    XLIVE_EXPECT(result == E_INVALIDARG);
}

void TestOfflineAnswers()
{
    // Signed out, every message says so the way the console did, and writes
    // its out-parameter on the failure path.
    GuestScratch word(4);
    if (!word.host)
        return;
    auto* out = static_cast<be<uint32_t>*>(word.host);
    uint32_t result = 0;

    *out = 0xAAAAAAAA;
    XLIVE_EXPECT(XliveSocial_Dispatch(0x00058004, word.host, 0, &result));
    XLIVE_EXPECT(result == XONLINE_E_LOGON_NOT_LOGGED_ON && out->get() == 0);

    GuestScratch list(sizeof(GuestArgumentList));
    GuestScratch scalars(12);
    GuestScratch size(4);
    GuestScratch handle(4);
    if (!list.host || !scalars.host || !size.host || !handle.host)
        return;
    auto* s = static_cast<be<uint32_t>*>(scalars.host);
    s[0] = 0;   // user index
    s[1] = 0;   // starting index
    s[2] = 100; // count — what sub_82598408 passes
    auto* args = static_cast<GuestArgumentList*>(list.host);
    Append(args, scalars.va);
    Append(args, scalars.va + 4);
    Append(args, scalars.va + 8);
    Append(args, size.va);
    Append(args, handle.va);
    *static_cast<be<uint32_t>*>(handle.host) = 0xAAAAAAAA;
    XLIVE_EXPECT(XliveSocial_Dispatch(0x00058020, nullptr, list.va, &result));
    XLIVE_EXPECT(result == XONLINE_E_LOGON_NOT_LOGGED_ON);
    XLIVE_EXPECT(static_cast<be<uint32_t>*>(handle.host)->get() == 0);
    XLIVE_EXPECT(static_cast<be<uint32_t>*>(size.host)->get() == 0);
}

void TestMuteQuery()
{
    GuestScratch query(sizeof(GuestMuteQuery));
    GuestScratch muted(4);
    if (!query.host || !muted.host)
        return;
    auto* q = static_cast<GuestMuteQuery*>(query.host);
    q->userIndex = 0;
    q->xuid = 0x0009000000000123ull;
    q->status = 0;
    *static_cast<be<uint32_t>*>(muted.host) = 0xAAAAAAAA;
    uint32_t result = 0;
    XLIVE_EXPECT(XliveSocial_Dispatch(0x0005800E, query.host, muted.va, &result));
    // Offline: the wrapper returns the status word, and sub_8259B178 tests
    // it against 1245.
    XLIVE_EXPECT(result == kErrorSuccess);
    XLIVE_EXPECT(q->status.get() == kErrorNotLoggedOn);
    XLIVE_EXPECT(static_cast<be<uint32_t>*>(muted.host)->get() == 0);
}

void TestFriendEntry()
{
    xlive::Client::Friend entry;
    entry.xuid = 0x0009000000000181ull;
    entry.gamertag = "Watch44557320";
    entry.relation = xlive::Client::Relation::Friend;
    entry.presence.state = xlive::Client::PresenceState::Playing;
    entry.presence.title_id = 0x58410B00;
    entry.presence.session_id = 0x8000000000000ABCull;
    entry.presence.joinable = true;
    entry.presence.rich_text = "In facility east";

    GuestScratch item(sizeof(GuestOnlineFriend));
    if (!item.host)
        return;
    auto* out = static_cast<GuestOnlineFriend*>(item.host);
    WriteFriend(out, entry);
    const auto* bytes = static_cast<const uint8_t*>(item.host);
    XLIVE_EXPECT(out->xuid.get() == entry.xuid);
    XLIVE_EXPECT(std::memcmp(out->gamertag, "Watch44557320\0\0", 16) == 0);
    XLIVE_EXPECT(out->state.get() == (kFriendOnline | kFriendPlaying | kFriendJoinable));
    // The XNKID is big-endian bytes: the top nibble the title checks for a
    // peer session (8) has to be the FIRST byte.
    XLIVE_EXPECT(bytes[0x1C] == 0x80 && bytes[0x23] == 0xBC);
    XLIVE_EXPECT(out->titleId.get() == 0x58410B00);
    XLIVE_EXPECT(out->richPresenceLength.get() == 16);
    XLIVE_EXPECT(bytes[0x44] == 0 && bytes[0x45] == 'I' && bytes[0x46] == 0 && bytes[0x47] == 'n');

    // A pending request carries its flag and no presence, whatever the
    // presence fields say.
    entry.relation = xlive::Client::Relation::RequestReceived;
    WriteFriend(out, entry);
    XLIVE_EXPECT(out->state.get() == kFriendReceivedRequest);
    XLIVE_EXPECT(out->titleId.get() == 0 && out->richPresenceLength.get() == 0);
}

// The whole enumerate path through a real kernel object: create, drain in
// two calls, then the no-more-files the title's loop ends on.
void TestEnumerate()
{
    auto* obj = CreateKernelObject<FriendsEnumerator>();
    if (!obj)
        return;
    for (int i = 0; i < 3; i++)
    {
        xlive::Client::Friend entry;
        entry.xuid = 0x0009000000000100ull + i;
        entry.gamertag = "Friend" + std::to_string(i);
        entry.relation = xlive::Client::Relation::Friend;
        obj->items.push_back(entry);
    }
    const uint32_t handle = GetKernelHandle(obj);

    GuestScratch buffer(2 * sizeof(GuestOnlineFriend));
    GuestScratch overlapped(28);
    if (!buffer.host || !overlapped.host)
        return;
    auto* ovl = static_cast<be<uint32_t>*>(overlapped.host);
    ovl[0] = kErrorIoPending;

    uint32_t result = 0;
    XLIVE_EXPECT(XliveSocial_Enumerate(handle, buffer.va, 2 * sizeof(GuestOnlineFriend),
                                       nullptr, overlapped.va, &result));
    XLIVE_EXPECT(result == kErrorIoPending);
    XLIVE_EXPECT(ovl[0].get() == kErrorSuccess && ovl[1].get() == 2);
    auto* items = static_cast<GuestOnlineFriend*>(buffer.host);
    XLIVE_EXPECT(items[0].xuid.get() == 0x0009000000000100ull);
    XLIVE_EXPECT(items[1].xuid.get() == 0x0009000000000101ull);

    ovl[0] = kErrorIoPending;
    XLIVE_EXPECT(XliveSocial_Enumerate(handle, buffer.va, 2 * sizeof(GuestOnlineFriend),
                                       nullptr, overlapped.va, &result));
    XLIVE_EXPECT(ovl[0].get() == kErrorSuccess && ovl[1].get() == 1);
    XLIVE_EXPECT(items[0].xuid.get() == 0x0009000000000102ull);

    ovl[0] = kErrorIoPending;
    XLIVE_EXPECT(XliveSocial_Enumerate(handle, buffer.va, 2 * sizeof(GuestOnlineFriend),
                                       nullptr, overlapped.va, &result));
    XLIVE_EXPECT(ovl[0].get() == kErrorNoMoreFiles && ovl[1].get() == 0);

    // Not ours: a content handle, or garbage, falls through to content.cpp.
    XLIVE_EXPECT(!XliveSocial_Enumerate(0, buffer.va, 0, nullptr, 0, &result));

    DestroyKernelObject(handle);
}

}  // namespace

void XliveSocial_SelfTest()
{
    const char* on = std::getenv("CZ_XLIVE_SOCIAL_TEST");
    if (!on || on[0] != '1')
        return;
    if (Live().online())
    {
        // The offline answers are half the test, and they are only honest
        // offline. A boot with a live gateway is the other half's job.
        fprintf(stderr, "[xlive] social self-test skipped: signed in to Live\n");
        return;
    }
    g_selfTestFailures = 0;
    TestFieldOffsets();
    TestArgumentListDecode();
    TestOfflineAnswers();
    TestMuteQuery();
    TestFriendEntry();
    TestEnumerate();
    if (g_selfTestFailures == 0)
        fprintf(stderr, "[xlive] social self-test: the guest ABI is intact\n");
    else
        fprintf(stderr, "[xlive] social self-test: %d FAILURE(S)\n", g_selfTestFailures);
}
