// See xlive_session.h for what this is and why it is off by default.
//
// THE GUEST STRUCTS BELOW ARE THE WHOLE RISK SURFACE, so every one of them is
// asserted at the size the recovery proved from this image's own call sites
// (tools/xgi_recover.py, and XenonLive/proto/xgi_messages.md, which records
// where each number came from). Two independent sources agree on all of them:
// the length this title passes at the call site, and the size of Xenia's
// SDK-derived struct. Where they did not agree, there would be no code here.
//
// WHAT IS NOT DERIVED, and is therefore ours to choose: the contents of the
// XNADDR. On hardware it held a routable address and a security-gateway
// record. Here it holds an index into a table, because the title only ever
// does two things with it — hands it to XNetXnAddrToInAddr, and copies it
// around — and a table index survives both while a real address would leak
// somebody's IP into guest memory and into save files.

#include "xlive_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <xbox.h> // be<>

#include "content.h" // Xam_CompleteOverlapped
#include "heap.h"
#include "klog.h"
#include "memory.h"
#include "xlive_glue.h"
#include "xlive_social.h"

#include <xlive/client.h>

namespace {

// ---------------------------------------------------------------------------
// The guest structs
// ---------------------------------------------------------------------------

struct GuestXnkid
{
    uint8_t ab[8];  // big-endian: ab[0] is the most significant byte
};
static_assert(sizeof(GuestXnkid) == 8, "XNKID is 8 bytes");

// SGADDR IS THE ONE PACKED STRUCT, and it is packed to four bytes because its
// 64-bit xboxId would otherwise align the whole thing to eight and make it
// 0x18 instead of 0x14 — which would push every field of the XNADDR that
// follows it out of place. Xenia's header packs exactly this struct and
// nothing else around it, and the static_asserts below are what turned that
// from a detail into a checked fact: every one of them fired the first time
// this was written with natural alignment.
//
// A wrongly-padded struct here is the worst kind of bug available: it
// compiles, it runs, and it writes the host's address four bytes into the key.
#pragma pack(push, 4)
struct GuestSgAddr
{
    be<uint32_t> ina;                      // +0
    be<uint32_t> securityParameterIndex;   // +4
    be<uint64_t> xboxId;                   // +8   the machine account id
    uint8_t platformType;                  // +16
    uint8_t reserved[3];                   // +17
};
static_assert(sizeof(GuestSgAddr) == 0x14, "SGADDR is 0x14");
#pragma pack(pop)

struct GuestXnAddr
{
    be<uint32_t> ina;          // +0x00  local address
    be<uint32_t> inaOnline;    // +0x04  public address
    be<uint16_t> portOnline;   // +0x08
    uint8_t enet[6];           // +0x0A  MAC
    GuestSgAddr online;        // +0x10
};
static_assert(sizeof(GuestXnAddr) == 0x24, "XNADDR is 0x24");

struct GuestSessionInfo
{
    GuestXnkid sessionId;   // +0x00
    GuestXnAddr hostAddress;// +0x08
    uint8_t keyExchange[16];// +0x2C
};
static_assert(sizeof(GuestSessionInfo) == 0x3C, "XSESSION_INFO is 0x3C");

// 0x000B0010, buffer 0x1C — sub_82605E28.
struct GuestSessionCreate
{
    be<uint32_t> objectPtr;      // +0x00
    be<uint32_t> flags;          // +0x04
    be<uint32_t> publicSlots;    // +0x08
    be<uint32_t> privateSlots;   // +0x0C
    be<uint32_t> userIndex;      // +0x10
    be<uint32_t> sessionInfoPtr; // +0x14  XSESSION_INFO* to fill
    be<uint32_t> noncePtr;       // +0x18  be64* to fill
};
static_assert(sizeof(GuestSessionCreate) == 0x1C, "the call site passes 28");

// 0x000B0011 delete, 0x000B0014 start, 0x000B0015 end — buffer 0x10.
struct GuestSessionState
{
    be<uint32_t> objectPtr;   // +0x00
    be<uint32_t> flags;       // +0x04
    be<uint64_t> nonce;       // +0x08
};
static_assert(sizeof(GuestSessionState) == 0x10, "the call sites pass 16");

// 0x000B0012 join, 0x000B0013 leave — buffer 0x14.
struct GuestSessionManage
{
    be<uint32_t> objectPtr;          // +0x00
    be<uint32_t> count;              // +0x04
    be<uint32_t> xuidArrayPtr;       // +0x08
    be<uint32_t> indexArrayPtr;      // +0x0C
    be<uint32_t> privateSlotArrayPtr;// +0x10
};
static_assert(sizeof(GuestSessionManage) == 0x14, "the call sites pass 20");

// 0x000B001B — buffer 0x14. NOTE the alignment: XNKID is a byte array, not a
// doubleword, so it does NOT force 8-byte alignment and the struct really is
// 0x14 rather than 0x18. That is exactly what the call site's length says, and
// it is the kind of thing a hand-written struct gets wrong once.
struct GuestSessionSearchById
{
    be<uint32_t> userIndex;         // +0x00
    GuestXnkid sessionId;           // +0x04
    be<uint32_t> resultsBufferSize; // +0x0C
    be<uint32_t> searchResultsPtr;  // +0x10
};
static_assert(sizeof(GuestSessionSearchById) == 0x14, "the call site passes 20");

// 0x000B001C — buffer 0x24.
struct GuestSessionSearchEx
{
    be<uint32_t> procedureIndex;    // +0x00
    be<uint32_t> userIndex;         // +0x04
    be<uint32_t> maxResults;        // +0x08
    be<uint16_t> propertyCount;     // +0x0C
    be<uint16_t> contextCount;      // +0x0E
    be<uint32_t> propertiesPtr;     // +0x10
    be<uint32_t> contextsPtr;       // +0x14
    be<uint32_t> resultsBufferSize; // +0x18
    be<uint32_t> searchResultsPtr;  // +0x1C
    be<uint32_t> userCount;         // +0x20
};
static_assert(sizeof(GuestSessionSearchEx) == 0x24, "the call site passes 36");

// 0x000B001D — buffer 0x18.
struct GuestSessionDetails
{
    be<uint32_t> objectPtr;          // +0x00
    be<uint32_t> detailsBufferSize;  // +0x04
    be<uint32_t> sessionDetailsPtr;  // +0x08
    be<uint32_t> reserved[3];        // +0x0C
};
static_assert(sizeof(GuestSessionDetails) == 0x18, "the call site passes 24");

// 0x000B001E — buffer 0x18.
struct GuestSessionMigrate
{
    be<uint32_t> objectPtr;       // +0x00
    be<uint32_t> sessionInfoPtr;  // +0x04  XSESSION_INFO* to fill
    be<uint32_t> userIndex;       // +0x08
    be<uint32_t> reserved[3];     // +0x0C
};
static_assert(sizeof(GuestSessionMigrate) == 0x18, "the call site passes 24");

// 0x000B0007 — buffer 0x20. The trailing four bytes are the padding the be<u64>
// at +8 forces, which is why the guest passes 32 for a struct whose last field
// ends at 0x1C.
struct GuestUserSetProperty
{
    be<uint32_t> userIndex;    // +0x00
    be<uint32_t> unused;       // +0x04
    be<uint64_t> xuid;         // +0x08
    be<uint32_t> propertyId;   // +0x10
    be<uint32_t> dataSize;     // +0x14
    be<uint32_t> dataPtr;      // +0x18
};
static_assert(sizeof(GuestUserSetProperty) == 0x20, "the call site passes 32");

struct GuestSearchResult
{
    GuestSessionInfo info;         // +0x00
    be<uint32_t> openPublic;       // +0x3C
    be<uint32_t> openPrivate;      // +0x40
    be<uint32_t> filledPublic;     // +0x44
    be<uint32_t> filledPrivate;    // +0x48
    be<uint32_t> propertyCount;    // +0x4C
    be<uint32_t> contextCount;     // +0x50
    be<uint32_t> propertiesPtr;    // +0x54
    be<uint32_t> contextsPtr;      // +0x58
};
static_assert(sizeof(GuestSearchResult) == 0x5C, "XSESSION_SEARCHRESULT is 0x5C");

struct GuestSearchResultHeader
{
    be<uint32_t> count;       // +0x00
    be<uint32_t> resultsPtr;  // +0x04
};
static_assert(sizeof(GuestSearchResultHeader) == 0x08, "the header is 8 bytes");

// XUSER_CONTEXT and XUSER_PROPERTY, as a search result points at them. The
// property is the same 24-byte shape XSessionWriteStats sends (id, the
// X_USER_DATA type byte at +8, the 8-byte value union at +16), and the title's
// matchmaking walks them by 24 (sub_82599D80: `addi r11,r11,24`, looking for
// 0x20000002 and reading its value with `ld r27,16(r11)`).
struct GuestUserContext
{
    be<uint32_t> id;
    be<uint32_t> value;
};
static_assert(sizeof(GuestUserContext) == 0x08, "XUSER_CONTEXT is 8");

struct GuestUserProperty
{
    be<uint32_t> id;      // +0x00
    uint8_t pad0[4];
    uint8_t type;         // +0x08
    uint8_t pad1[7];
    uint8_t value[8];     // +0x10
};
static_assert(sizeof(GuestUserProperty) == 0x18, "XUSER_PROPERTY is 0x18");

struct GuestLocalDetails
{
    be<uint32_t> userIndexHost;          // +0x00
    be<uint32_t> gameType;               // +0x04
    be<uint32_t> gameMode;               // +0x08
    be<uint32_t> flags;                  // +0x0C
    be<uint32_t> maxPublicSlots;         // +0x10
    be<uint32_t> maxPrivateSlots;        // +0x14
    be<uint32_t> availablePublicSlots;   // +0x18
    be<uint32_t> availablePrivateSlots;  // +0x1C
    be<uint32_t> actualMemberCount;      // +0x20
    be<uint32_t> returnedMemberCount;    // +0x24
    be<uint32_t> state;                  // +0x28
    be<uint64_t> nonce;                  // +0x30  (the be<u64> aligns it to 0x30)
    GuestSessionInfo sessionInfo;        // +0x38
    GuestXnkid arbitrationId;            // +0x74
    be<uint32_t> sessionMembersPtr;      // +0x7C
};
static_assert(sizeof(GuestLocalDetails) == 0x80, "XSESSION_LOCAL_DETAILS is 0x80");

struct GuestSessionMember
{
    be<uint64_t> onlineXuid;  // +0x00
    be<uint32_t> userIndex;   // +0x08
    be<uint32_t> flags;       // +0x0C
};
static_assert(sizeof(GuestSessionMember) == 0x10, "XSESSION_MEMBER is 0x10");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorFunctionFailed = 1627;
constexpr uint32_t kErrorNotFound = 1168;
constexpr uint32_t kErrorInvalidParameter = 87;

// 198.18.0.0/15, the IANA benchmarking block. It is never routed, which is
// exactly what a stand-in address has to be: if a bug ever sends real traffic
// to one of these, it goes nowhere instead of somewhere.
//
// A player's address is 198.18.0.0 | the low 16 bits of their XUID, and that
// arithmetic is the whole point: EVERY machine computes the same address for
// a given player, this machine included. The title stamps each network event
// with the sender's own address (from XNetGetTitleXnAddr) and the receiver's
// link manager matches that stamp against the address it recorded for the
// link when it accepted it (from XNetXnAddrToInAddr of the peer's XNADDR).
// The first two-machine session handed out addresses in order of first
// sight instead — 198.18.0.1, .2, ... — so the guest's own address on the
// guest was not the guest's address on the host, and reported itself as
// 0.0.0.0 anyway because the import was not wired; the host dropped every
// handshake without a word and the guest hung on "attempting to join".
constexpr uint32_t kFakeNetBase = 0xC6120000u;  // 198.18.0.0

constexpr uint16_t kVirtualPort = 3074;  // what the console used for title traffic

// The UDP port the punched path actually binds. The console's 3074 by
// default; CZ_XLIVE_PEER_PORT=N moves it so two copies of the game can run
// on ONE machine (host and guest of the same session, for co-op work without
// a second box). Only the socket moves — the XNADDR the title sees keeps
// portOnline 3074, since the title compares that against its own constant.
uint16_t PeerPort()
{
    static const uint16_t port = [] {
        const char* e = std::getenv("CZ_XLIVE_PEER_PORT");
        const long n = e ? std::strtol(e, nullptr, 10) : 0;
        return (n > 0 && n < 65536) ? uint16_t(n) : kVirtualPort;
    }();
    return port;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::mutex g_mutex;
bool g_enabled = false;
std::atomic<bool> g_running{false};
std::thread g_completionThread;

// A session as the guest knows it. Keyed by the object pointer the guest
// passes in every message, which is its own XSESSION handle.
struct TrackedSession
{
    uint64_t sessionId = 0;
    uint8_t key[16] = {};
    uint64_t hostXuid = 0;
    uint32_t flags = 0;
    uint64_t nonce = 0;
    int publicSlots = 0;
    int privateSlots = 0;
    int openPublic = 0;
    int openPrivate = 0;
    xlive::Client::SessionState state = xlive::Client::SessionState::Lobby;
    std::vector<xlive::Client::SessionMember> members;
    bool weAreHost = false;
};
std::map<uint32_t, TrackedSession> g_sessions;      // guest object pointer -> session
std::map<uint64_t, uint32_t> g_objectForSession;    // session id -> guest object pointer

// Contexts and properties the title set before creating a session. The console
// carried these into XSessionCreate implicitly, which is why they are recorded
// here rather than read out of the create message: the create message does not
// have them.
std::map<uint32_t, uint32_t> g_contexts;
std::map<uint32_t, xlive::Client::StatProperty> g_properties;

// The peer address table: every player this run has computed an address for,
// so an address can be turned back into its XUID (recvfrom's source, the
// session's member list). The address itself is a pure function of the XUID —
// see kFakeNetBase.
std::map<uint64_t, uint32_t> g_addressForXuid;
std::map<uint32_t, uint64_t> g_xuidForAddress;

// One in-flight guest request.
struct Pending
{
    xlive::Client::Ticket ticket = 0;
    uint32_t message = 0;
    uint32_t overlappedVa = 0;
    // Where to write the answer, all guest addresses.
    uint32_t objectPtr = 0;
    uint32_t sessionInfoPtr = 0;
    uint32_t noncePtr = 0;
    uint32_t resultsPtr = 0;
    uint32_t resultsSize = 0;
    uint32_t detailsPtr = 0;
    uint32_t detailsSize = 0;
    uint64_t sessionId = 0;
    // XSessionCreate without XSESSION_CREATE_HOST: the title is registering
    // a session it FOUND — the XSESSION_INFO it passed in names it — and
    // nothing is created anywhere. Answered by joining it: the title's own
    // reliable layer connects to the host over the socket BEFORE it sends
    // XSessionJoinLocal, and the server hands out addresses to members only,
    // so the seat is taken here and the later JoinLocal has nothing to send.
    bool joiner = false;
};
std::vector<Pending> g_pending;

// A private message number for the invite prefetch, which is a request of
// ours rather than the guest's: no overlapped, and its answer goes to
// xlive_social.cpp instead of into guest memory.
constexpr uint32_t kInviteDetails = 0xFFFF0001;
std::map<uint64_t, xlive::Client::SessionInfo> g_inviteSessions;

// Social tickets handed over to be collected and dropped.
std::vector<xlive::Client::Ticket> g_socialTickets;

xlive::Client& Live() { return xlive::Client::Instance(); }

// Set by kernel/coop_friends.cpp for the JoinGame screen's "JOIN FRIENDS" row;
// read when a search completes. Cleared by the "JOIN XBOX LIVE GAME" row.
bool g_friendsOnlySearch = false;

// ---------------------------------------------------------------------------
// Address bookkeeping
// ---------------------------------------------------------------------------

uint32_t AddressForXuid(uint64_t xuid)
{
    if (xuid == 0)
        return 0;
    auto it = g_addressForXuid.find(xuid);
    if (it != g_addressForXuid.end())
        return it->second;
    // Deterministic across machines (see kFakeNetBase). Two accounts 65536
    // apart would share an address; a server has to be large before that is
    // reachable, and even then only two such players in one session collide.
    const uint32_t address = kFakeNetBase | uint32_t(xuid & 0xFFFFu);
    g_addressForXuid[xuid] = address;
    g_xuidForAddress[address] = xuid;
    return address;
}

void FillXnAddr(GuestXnAddr* out, uint64_t xuid)
{
    std::memset(out, 0, sizeof(*out));
    const uint32_t address = AddressForXuid(xuid);
    out->ina = address;
    out->inaOnline = address;
    out->portOnline = kVirtualPort;
    // A MAC that is locally administered (bit 1 of the first octet) and not
    // multicast, carrying the low 32 bits of the XUID. The title only ever
    // compares these, so what matters is that they are unique per peer and
    // never collide with real hardware.
    out->enet[0] = 0x02;
    out->enet[1] = 0x58;  // 'X'
    out->enet[2] = uint8_t(xuid >> 24);
    out->enet[3] = uint8_t(xuid >> 16);
    out->enet[4] = uint8_t(xuid >> 8);
    out->enet[5] = uint8_t(xuid);
    out->online.ina = address;
    out->online.securityParameterIndex = uint32_t(xuid >> 32);
    out->online.xboxId = xuid;
    out->online.platformType = 4;  // Xbox 360
}

void WriteXnkid(GuestXnkid* out, uint64_t sessionId)
{
    for (int i = 0; i < 8; i++)
        out->ab[i] = uint8_t(sessionId >> (56 - 8 * i));
}

uint64_t ReadXnkid(const GuestXnkid* in)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 8) | in->ab[i];
    return value;
}

void FillSessionInfo(GuestSessionInfo* out, const xlive::Client::SessionInfo& session)
{
    std::memset(out, 0, sizeof(*out));
    WriteXnkid(&out->sessionId, session.session_id);
    FillXnAddr(&out->hostAddress, session.host_xuid);
    std::memcpy(out->keyExchange, session.key, sizeof(out->keyExchange));
}

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------
//
// Every pointer below came from guest code, so each one is bounds-checked
// before it is written through. gotcha 73: a guest-supplied pointer and a
// guest-supplied count are two separate things to distrust.

template <typename T>
T* GuestPtr(uint32_t va)
{
    if (va == 0)
        return nullptr;
    return reinterpret_cast<T*>(g_memory.Translate(va));
}

void CopyContextsAndProperties(xlive::Client::SessionCreateRequest& out)
{
    for (const auto& [id, value] : g_contexts)
        out.contexts.push_back({id, value});
    for (const auto& [id, property] : g_properties)
        out.properties.push_back(property);
    (void)out;
}

// ---------------------------------------------------------------------------
// Completing a request
// ---------------------------------------------------------------------------

void Complete(const Pending& pending, uint32_t status, uint32_t length)
{
    if (pending.overlappedVa)
        Xam_CompleteOverlapped(pending.overlappedVa, status, length);
}

// Records what came back so a later XSessionGetDetails can answer without a
// round trip, and so XSessionDelete knows which session id to delete.
void Remember(uint32_t objectPtr, const xlive::Client::SessionInfo& session)
{
    TrackedSession& tracked = g_sessions[objectPtr];
    tracked.sessionId = session.session_id;
    std::memcpy(tracked.key, session.key, sizeof(tracked.key));
    tracked.hostXuid = session.host_xuid;
    tracked.flags = session.flags;
    tracked.nonce = session.nonce;
    tracked.publicSlots = session.public_slots;
    tracked.privateSlots = session.private_slots;
    tracked.openPublic = session.open_public_slots;
    tracked.openPrivate = session.open_private_slots;
    tracked.state = session.state;
    tracked.members = session.members;
    tracked.weAreHost = session.host_xuid == Live().identity().xuid;
    g_objectForSession[session.session_id] = objectPtr;
}

// Writes a search answer into the buffer the guest allocated. Returns the
// status the overlapped should carry.
// The 8-byte X_USER_DATA union by type. A 32-bit member sits in the FIRST
// four bytes; a string has nowhere to live in a search result and is written
// as an empty one — every property these titles advertise is an int64.
void WritePropertyValue(uint8_t (&out)[8], const xlive::Client::StatProperty& property)
{
    using Type = xlive::Client::StatProperty::Type;
    std::memset(out, 0, 8);
    switch (property.type)
    {
    case Type::Int32:
    case Type::Context:
        *reinterpret_cast<be<uint32_t>*>(out) = uint32_t(int32_t(property.integer));
        break;
    case Type::Double:
        *reinterpret_cast<be<double>*>(out) = property.real;
        break;
    case Type::Float:
        *reinterpret_cast<be<float>*>(out) = float(property.real);
        break;
    case Type::Unicode:
    case Type::Binary:
        break;
    default: // Int64, DateTime
        *reinterpret_cast<be<int64_t>*>(out) = property.integer;
        break;
    }
}

uint32_t WriteSearchResults(const Pending& pending,
                            const std::vector<xlive::Client::SessionInfo>& results)
{
    if (pending.resultsPtr == 0)
        return kErrorInvalidParameter;

    // The header, the array, and then every result's contexts and properties
    // — the title's matchmaking reads those off the result (sub_82599D80
    // refuses a session whose property 0x20000002 it cannot see, which is
    // how the first two-machine session found "an error occurred while
    // connecting" with a search that had found the host).
    uint32_t needed =
        uint32_t(sizeof(GuestSearchResultHeader) + results.size() * sizeof(GuestSearchResult));
    for (const auto& result : results)
        needed += uint32_t(result.contexts.size() * sizeof(GuestUserContext) +
                           result.properties.size() * sizeof(GuestUserProperty));

    if (pending.resultsSize < needed)
    {
        // The console's two-call pattern: ask with a buffer that is too small
        // and be told how much to allocate. Writing the size into the first
        // word is what the guest reads back.
        auto* header = GuestPtr<GuestSearchResultHeader>(pending.resultsPtr);
        if (header)
            header->count = needed;
        KLOG("XSessionSearch: buffer is %u bytes, need %u\n", pending.resultsSize, needed);
        return kErrorInsufficientBuffer;
    }

    auto* header = GuestPtr<GuestSearchResultHeader>(pending.resultsPtr);
    if (!header)
        return kErrorInvalidParameter;
    header->count = uint32_t(results.size());
    // The array follows the header in the same buffer, which is how the guest
    // walks it: the pointer is into its own allocation.
    const uint32_t arrayVa = pending.resultsPtr + uint32_t(sizeof(GuestSearchResultHeader));
    header->resultsPtr = results.empty() ? 0 : arrayVa;

    // The contexts and properties go after the array, each result's pointers
    // into its own allocation.
    uint32_t tailVa = arrayVa + uint32_t(results.size() * sizeof(GuestSearchResult));
    for (size_t i = 0; i < results.size(); i++)
    {
        auto* entry = GuestPtr<GuestSearchResult>(arrayVa + uint32_t(i * sizeof(GuestSearchResult)));
        if (!entry)
            return kErrorInvalidParameter;
        std::memset(entry, 0, sizeof(*entry));
        FillSessionInfo(&entry->info, results[i]);
        entry->openPublic = uint32_t(results[i].open_public_slots);
        entry->openPrivate = uint32_t(results[i].open_private_slots);
        entry->filledPublic = uint32_t(results[i].filled_public_slots);
        entry->filledPrivate = uint32_t(results[i].filled_private_slots);

        entry->contextCount = uint32_t(results[i].contexts.size());
        entry->contextsPtr = results[i].contexts.empty() ? 0 : tailVa;
        for (const auto& context : results[i].contexts)
        {
            auto* out = GuestPtr<GuestUserContext>(tailVa);
            out->id = context.id;
            out->value = context.value;
            tailVa += uint32_t(sizeof(GuestUserContext));
        }
        entry->propertyCount = uint32_t(results[i].properties.size());
        entry->propertiesPtr = results[i].properties.empty() ? 0 : tailVa;
        for (const auto& property : results[i].properties)
        {
            auto* out = GuestPtr<GuestUserProperty>(tailVa);
            std::memset(out, 0, sizeof(*out));
            out->id = property.id;
            out->type = uint8_t(property.type);
            WritePropertyValue(out->value, property);
            tailVa += uint32_t(sizeof(GuestUserProperty));
        }
    }
    return kErrorSuccess;
}

uint32_t WriteDetails(const Pending& pending, const xlive::Client::SessionInfo& session)
{
    if (pending.detailsPtr == 0)
        return kErrorInvalidParameter;
    const uint32_t membersBytes = uint32_t(session.members.size() * sizeof(GuestSessionMember));
    const uint32_t needed = uint32_t(sizeof(GuestLocalDetails)) + membersBytes;
    if (pending.detailsSize < needed)
    {
        auto* size = GuestPtr<be<uint32_t>>(pending.detailsPtr);
        if (size)
            *size = needed;
        return kErrorInsufficientBuffer;
    }

    auto* details = GuestPtr<GuestLocalDetails>(pending.detailsPtr);
    if (!details)
        return kErrorInvalidParameter;
    std::memset(details, 0, sizeof(*details));
    details->userIndexHost = 0;
    details->flags = session.flags;
    details->maxPublicSlots = uint32_t(session.public_slots);
    details->maxPrivateSlots = uint32_t(session.private_slots);
    details->availablePublicSlots = uint32_t(session.open_public_slots);
    details->availablePrivateSlots = uint32_t(session.open_private_slots);
    details->actualMemberCount = uint32_t(session.members.size());
    details->returnedMemberCount = uint32_t(session.members.size());
    details->state = uint32_t(session.state);
    details->nonce = session.nonce;
    FillSessionInfo(&details->sessionInfo, session);

    const uint32_t membersVa = pending.detailsPtr + uint32_t(sizeof(GuestLocalDetails));
    details->sessionMembersPtr = session.members.empty() ? 0 : membersVa;
    for (size_t i = 0; i < session.members.size(); i++)
    {
        auto* member =
            GuestPtr<GuestSessionMember>(membersVa + uint32_t(i * sizeof(GuestSessionMember)));
        if (!member)
            return kErrorInvalidParameter;
        member->onlineXuid = session.members[i].xuid;
        member->userIndex = session.members[i].is_host ? 0u : 0xFFFFFFFFu;
        member->flags = session.members[i].is_private ? 1u : 0u;
    }
    return kErrorSuccess;
}

// Maps a libxlive failure onto something the title's own error handling knows.
uint32_t StatusFor(const std::string& error)
{
    if (error == "no_session")
        return kErrorNotFound;
    if (error == "session_full")
        return kErrorFunctionFailed;
    if (error == "not_host" || error == "not_a_member")
        return kErrorInvalidParameter;
    return kErrorFunctionFailed;
}


// Applies a result that has already been collected. Poll() collects, so the
// completion thread does the polling and this does the work — one place that
// takes the ticket's answer, rather than two that race to take it.
void SettleWith(const Pending& pending, const xlive::Client::SessionResult& result)
{
    if (pending.message == kInviteDetails)
    {
        if (result.ok && result.session.valid())
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inviteSessions[pending.sessionId] = result.session;
        }
        else
        {
            KLOG("[xlive] invite session %016llX: %s\n",
                 (unsigned long long)pending.sessionId,
                 result.ok ? "no such session" : result.error.c_str());
        }
        XliveSocial_OnInviteSessionReady(pending.sessionId, result.ok && result.session.valid());
        return;
    }

    if (!result.ok)
    {
        KLOG("[xlive] message %08X failed: %s\n", pending.message, result.error.c_str());
        Complete(pending, StatusFor(result.error), 0);
        return;
    }

    uint32_t status = kErrorSuccess;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        switch (pending.message)
        {
        case 0x000B0010: // XSessionCreate
            Remember(pending.objectPtr, result.session);
            if (auto* info = GuestPtr<GuestSessionInfo>(pending.sessionInfoPtr))
                FillSessionInfo(info, result.session);
            if (auto* nonce = GuestPtr<be<uint64_t>>(pending.noncePtr))
                *nonce = result.session.nonce;
            if (pending.joiner)
                KLOG("[xlive] registered session %016llX to join (host %s, %d open slot(s))\n",
                     (unsigned long long)result.session.session_id,
                     result.session.host_gamertag.c_str(), result.session.open_public_slots);
            else
                KLOG("[xlive] hosting session %016llX (%d public slot(s))\n",
                     (unsigned long long)result.session.session_id, result.session.public_slots);
            break;

        case 0x000B0012: // XSessionJoinLocal
            Remember(pending.objectPtr, result.session);
            KLOG("[xlive] in session %016llX with %zu member(s)\n",
                 (unsigned long long)result.session.session_id, result.session.members.size());
            break;

        case 0x000B0011: // XSessionDelete
        case 0x000B0013: // XSessionLeaveLocal
            g_sessions.erase(pending.objectPtr);
            g_objectForSession.erase(pending.sessionId);
            break;

        case 0x000B0014: // XSessionStart
        case 0x000B0015: // XSessionEnd
            if (result.session.valid())
                Remember(pending.objectPtr, result.session);
            break;

        case 0x000B001B: // XSessionSearchByID
        {
            std::vector<xlive::Client::SessionInfo> one;
            if (result.session.valid())
                one.push_back(result.session);
            status = WriteSearchResults(pending, one);
            break;
        }

        case 0x000B001C: // XSessionSearchEx
        {
            // The JoinGame screen's "JOIN FRIENDS" row (co-op part 5,
            // kernel/coop_friends.cpp): the same search, kept to the sessions
            // a friend is hosting. The library's friends list carries each
            // friend's xuid; a session names its host. Nothing else about the
            // join changes, so the result the title reads is a normal search
            // result that happens to hold only friends' games.
            std::vector<xlive::Client::SessionInfo> results = result.results;
            if (g_friendsOnlySearch)
            {
                std::vector<xlive::Client::SessionInfo> friendsOnly;
                const auto friends = Live().friends();
                for (const auto& session : results)
                    for (const auto& f : friends)
                        if (f.is_friend() && f.xuid == session.host_xuid)
                        {
                            friendsOnly.push_back(session);
                            break;
                        }
                KLOG("[xlive] search found %zu session(s), %zu hosted by a friend "
                     "(friends-only search; %zu friend(s) on the list)\n",
                     results.size(), friendsOnly.size(), friends.size());
                results.swap(friendsOnly);
            }
            else
                KLOG("[xlive] search found %zu session(s)\n", results.size());
            status = WriteSearchResults(pending, results);
            break;
        }

        case 0x000B001D: // XSessionGetDetails
            if (result.session.valid())
                Remember(pending.objectPtr, result.session);
            status = WriteDetails(pending, result.session);
            break;

        case 0x000B001E: // XSessionMigrateHost
            Remember(pending.objectPtr, result.session);
            if (auto* info = GuestPtr<GuestSessionInfo>(pending.sessionInfoPtr))
                FillSessionInfo(info, result.session);
            break;

        default:
            break;
        }
    }

    // Peering changes are made OUTSIDE g_mutex: StartPeering reaches into
    // libxlive, which takes its own locks, and holding two locks in two orders
    // across two libraries is how a deadlock gets built.
    switch (pending.message)
    {
    case 0x000B0010:
    case 0x000B0012:
        // Start opening paths as soon as we are in a session. Punching takes
        // seconds; starting it when the title first sends a packet would put
        // those seconds in front of the player.
        Live().StartPeering(result.session.session_id, PeerPort());
        [[fallthrough]];
    case 0x000B0014:
    case 0x000B0015:
    case 0x000B001E:
        // And tell the friends list, so an invite can name this session.
        // "Joinable" is a seat being open in a lobby, which is the only state
        // in which the title's own join path would let anyone in.
        if (result.session.valid())
            CzXlive_SetPresenceSession(result.session.session_id,
                                       result.session.open_public_slots > 0 &&
                                       result.session.state ==
                                           xlive::Client::SessionState::Lobby);
        break;
    case 0x000B0011:
    case 0x000B0013:
        Live().StopPeering();
        CzXlive_SetPresenceSession(0, false);
        break;
    default:
        break;
    }

    Complete(pending, status, 0);
}

void CompletionThread()
{
    while (g_running.load())
    {
        std::vector<Pending> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snapshot = g_pending;
        }

        std::vector<xlive::Client::Ticket> settled;
        for (const auto& pending : snapshot)
        {
            xlive::Client::SessionResult result;
            const auto status = Live().Poll(pending.ticket, result);
            if (status == xlive::Client::OpStatus::Pending)
                continue;
            settled.push_back(pending.ticket);

            if (status == xlive::Client::OpStatus::Unknown)
            {
                // The ticket is gone and nobody took its answer. Completing
                // with a failure is the only honest move: leaving the
                // overlapped reading ERROR_IO_PENDING hangs whatever the title
                // does next, forever, with no error to find.
                KLOG("[xlive] session ticket %llu vanished; failing the request\n",
                     (unsigned long long)pending.ticket);
                if (pending.message == kInviteDetails)
                    XliveSocial_OnInviteSessionReady(pending.sessionId, false);
                else
                    Complete(pending, kErrorFunctionFailed, 0);
                continue;
            }
            SettleWith(pending, result);
        }

        // The fire-and-forget social calls. Collected so the library's result
        // table does not grow, and logged so a refusal is not silent.
        std::vector<xlive::Client::Ticket> social;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            social = g_socialTickets;
        }
        for (auto ticket : social)
        {
            xlive::Client::SocialResult result;
            const auto status = Live().Poll(ticket, result);
            if (status == xlive::Client::OpStatus::Pending)
                continue;
            if (status == xlive::Client::OpStatus::Failed)
                KLOG("[xlive] social request refused: %s\n", result.error.c_str());
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_socialTickets.begin(); it != g_socialTickets.end(); ++it)
                if (*it == ticket)
                {
                    g_socialTickets.erase(it);
                    break;
                }
        }

        if (!settled.empty())
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                bool done = false;
                for (auto ticket : settled)
                    if (it->ticket == ticket)
                        done = true;
                it = done ? g_pending.erase(it) : std::next(it);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// Queues a request and tells the caller what to return. Every session message
// in this title arrives with an overlapped, so the answer is almost always
// "accepted, ask again later".
uint32_t Begin(Pending pending)
{
    if (pending.ticket == 0)
        return kErrorFunctionFailed;

    if (pending.overlappedVa == 0)
    {
        // A synchronous caller. No call site in this image does this — every
        // session wrapper routes through sub_825ACC08 with an overlapped — but
        // if one ever did, the honest answer is that the request was accepted
        // and its result is not knowable yet, not a fabricated success.
        Live().Forget(pending.ticket);
        KLOG("[xlive] message %08X was sent without an overlapped; refusing rather "
             "than blocking a guest thread\n", pending.message);
        return kErrorFunctionFailed;
    }

    // Mark the overlapped pending BEFORE the request can complete, or a fast
    // answer would be overwritten by this and the title would wait forever on
    // a result that already arrived.
    if (auto* overlapped = GuestPtr<be<uint32_t>>(pending.overlappedVa))
        *overlapped = kErrorIoPending;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(pending));
    return kErrorSuccess;
}

}  // namespace

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

bool XliveSession_Enabled() { return g_enabled; }

void XliveSession_Start()
{
    if (g_enabled)
        return;
    const char* on = std::getenv("CZ_XLIVE_COOP");
    if (!on || on[0] != '1')
        return;
    g_enabled = true;
    g_running.store(true);
    g_completionThread = std::thread(CompletionThread);
    KLOG("[xlive] co-op enabled: the XGI session messages are handled\n");
}

void XliveSession_Shutdown()
{
    if (!g_enabled)
        return;
    g_running.store(false);
    if (g_completionThread.joinable())
        g_completionThread.join();
    Live().StopPeering();
    g_enabled = false;
}

void XliveSession_SetContext(uint32_t contextId, uint32_t value)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_contexts[contextId] = value;
}

bool XliveSession_PrefetchInviteSession(uint64_t sessionId)
{
    if (!g_enabled || sessionId == 0)
        return false;
    Pending pending;
    pending.message = kInviteDetails;
    pending.sessionId = sessionId;
    pending.ticket = Live().GetSessionDetails(sessionId);
    if (pending.ticket == 0)
        return false;
    // Not through Begin(): that refuses a request with no overlapped, and
    // rightly, because a guest request with no overlapped has nowhere to put
    // its answer. This one has: the map above.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(pending));
    return true;
}

bool XliveSession_InviteSessionInfo(uint64_t sessionId, void* sessionInfoOut)
{
    if (!sessionInfoOut)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_inviteSessions.find(sessionId);
    if (it == g_inviteSessions.end())
        return false;
    FillSessionInfo(static_cast<GuestSessionInfo*>(sessionInfoOut), it->second);
    return true;
}

void XliveSession_DrainSocialTicket(uint64_t ticket)
{
    if (ticket == 0)
        return;
    if (!g_enabled)
    {
        // No thread to collect it on. The request still goes out; only its
        // result is left in the library's table, which is bounded by how many
        // invitations a player can accept in one run.
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_socialTickets.push_back(ticket);
}

bool XliveSession_Dispatch(uint32_t message, void* buffer, uint32_t bufferLength,
                           uint32_t overlappedVa, uint32_t* result)
{
    if (!g_enabled || !result)
        return false;

    switch (message)
    {
    // -- XUserSetProperty --------------------------------------------------
    //
    // Not a session message, but it is what matchmaking filters on, and the
    // title sets its properties before it ever creates a session.
    case 0x000B0007:
    {
        if (!buffer || bufferLength < sizeof(GuestUserSetProperty))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestUserSetProperty*>(buffer);
        const uint32_t id = msg->propertyId.get();
        const uint32_t size = msg->dataSize.get();
        const uint32_t dataVa = msg->dataPtr.get();

        xlive::Client::StatProperty property;
        property.id = id;
        // The type is the top nibble of the id, the same encoding the profile
        // settings use — see the note above XamUserReadProfileSettings.
        property.type = xlive::Client::StatProperty::Type((id >> 28) & 0xF);

        const auto* data = GuestPtr<const uint8_t>(dataVa);
        if (data && size)
        {
            switch (property.type)
            {
            case xlive::Client::StatProperty::Type::Context:
            case xlive::Client::StatProperty::Type::Int32:
                if (size >= 4)
                    property.integer = int32_t(reinterpret_cast<const be<uint32_t>*>(data)->get());
                break;
            case xlive::Client::StatProperty::Type::Int64:
            case xlive::Client::StatProperty::Type::DateTime:
                if (size >= 8)
                    property.integer = int64_t(reinterpret_cast<const be<uint64_t>*>(data)->get());
                break;
            case xlive::Client::StatProperty::Type::Float:
                if (size >= 4)
                {
                    const uint32_t bits = reinterpret_cast<const be<uint32_t>*>(data)->get();
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    property.real = value;
                }
                break;
            case xlive::Client::StatProperty::Type::Double:
                if (size >= 8)
                {
                    const uint64_t bits = reinterpret_cast<const be<uint64_t>*>(data)->get();
                    std::memcpy(&property.real, &bits, sizeof(property.real));
                }
                break;
            case xlive::Client::StatProperty::Type::Unicode:
            {
                // The guest counts UTF-16 code units. Narrow the ASCII range
                // and drop the rest rather than hand a half-decoded string to
                // a server.
                const auto* units = reinterpret_cast<const be<uint16_t>*>(data);
                const uint32_t count = size / 2 > 512 ? 512 : size / 2;
                for (uint32_t i = 0; i < count && units[i].get(); i++)
                    if (units[i].get() < 0x80)
                        property.text.push_back(char(units[i].get()));
                break;
            }
            case xlive::Client::StatProperty::Type::Binary:
                property.text.assign(reinterpret_cast<const char*>(data),
                                     size > 4096 ? 4096 : size);
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_properties[id] = std::move(property);
        }
        KLOG("XGI property %08X set (%u bytes)\n", id, size);
        *result = kErrorSuccess;
        return true;
    }

    // -- XSessionCreate ----------------------------------------------------
    case 0x000B0010:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionCreate))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionCreate*>(buffer);

        // XSESSION_CREATE_HOST clear: the title is not making a session, it
        // is registering one it found — the joiner's XSessionCreate, with the
        // host's XSESSION_INFO (from a search, an invitation or a friend's
        // presence) in the buffer it would otherwise be asking us to fill.
        // The first two-machine session found this: the joiner's create
        // opened a second, empty lobby on the server and its packets went to
        // a session the host was never in.
        constexpr uint32_t kSessionCreateHost = 0x00000001;
        if ((msg->flags.get() & kSessionCreateHost) == 0)
        {
            const auto* info = GuestPtr<const GuestSessionInfo>(msg->sessionInfoPtr.get());
            const uint64_t sessionId = info ? ReadXnkid(&info->sessionId) : 0;
            if (sessionId == 0)
            {
                KLOG("XSessionCreate: not the host, and no session named to join\n");
                *result = kErrorInvalidParameter;
                return true;
            }
            Pending pending;
            pending.message = message;
            pending.overlappedVa = overlappedVa;
            pending.objectPtr = msg->objectPtr.get();
            pending.sessionInfoPtr = msg->sessionInfoPtr.get();
            pending.noncePtr = msg->noncePtr.get();
            pending.sessionId = sessionId;
            pending.joiner = true;
            // The seat, now: the title connects to the host over its socket
            // before it sends JoinLocal, and only a member is told where the
            // host is. The server's join is idempotent, so the JoinLocal that
            // follows costs nothing.
            pending.ticket = Live().JoinSession(sessionId, false);
            *result = Begin(std::move(pending));
            return true;
        }

        xlive::Client::SessionCreateRequest request;
        request.flags = msg->flags.get();
        request.public_slots = int(msg->publicSlots.get());
        request.private_slots = int(msg->privateSlots.get());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            CopyContextsAndProperties(request);
        }
        // Slot counts come from guest code, so they are bounded before they
        // reach a server that would otherwise store whatever it was told.
        if (request.public_slots < 0 || request.public_slots > 32 ||
            request.private_slots < 0 || request.private_slots > 32 ||
            request.public_slots + request.private_slots == 0)
        {
            KLOG("XSessionCreate: implausible slots (%d public, %d private)\n",
                 request.public_slots, request.private_slots);
            *result = kErrorInvalidParameter;
            return true;
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.objectPtr = msg->objectPtr.get();
        pending.sessionInfoPtr = msg->sessionInfoPtr.get();
        pending.noncePtr = msg->noncePtr.get();
        pending.ticket = Live().CreateSession(request);
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionDelete / Start / End --------------------------------------
    case 0x000B0011:
    case 0x000B0014:
    case 0x000B0015:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionState))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionState*>(buffer);
        const uint32_t objectPtr = msg->objectPtr.get();

        uint64_t sessionId = 0;
        bool weAreHost = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sessions.find(objectPtr);
            if (it != g_sessions.end())
            {
                sessionId = it->second.sessionId;
                weAreHost = it->second.weAreHost;
            }
        }
        if (sessionId == 0)
        {
            // A message about a session we never created. Saying so beats
            // succeeding at nothing.
            KLOG("XGI %08X: no session for object %08X\n", message, objectPtr);
            *result = kErrorNotFound;
            return true;
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.objectPtr = objectPtr;
        pending.sessionId = sessionId;

        if (message == 0x000B0011)
        {
            // A joiner "deleting" its session object is leaving, not deleting
            // the host's lobby — and the server would refuse it anyway,
            // because only a host can delete.
            pending.ticket = weAreHost ? Live().DeleteSession(sessionId)
                                       : Live().LeaveSession(sessionId);
        }
        else
        {
            xlive::Client::SessionUpdateRequest update;
            update.state_set = true;
            update.state = message == 0x000B0014 ? xlive::Client::SessionState::InGame
                                                 : xlive::Client::SessionState::Reporting;
            if (!weAreHost)
            {
                // Only the host owns the session's state. A joiner's Start and
                // End are local bookkeeping, and there is nothing to send.
                *result = kErrorSuccess;
                if (overlappedVa)
                    Xam_CompleteOverlapped(overlappedVa, kErrorSuccess, 0);
                return true;
            }
            pending.ticket = Live().ModifySession(sessionId, update);
        }
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionJoinLocal / LeaveLocal ------------------------------------
    case 0x000B0012:
    case 0x000B0013:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionManage))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionManage*>(buffer);
        const uint32_t objectPtr = msg->objectPtr.get();

        uint64_t sessionId = 0;
        bool weAreHost = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sessions.find(objectPtr);
            if (it != g_sessions.end())
            {
                sessionId = it->second.sessionId;
                weAreHost = it->second.weAreHost;
            }
        }
        if (sessionId == 0)
        {
            KLOG("XGI %08X: no session for object %08X\n", message, objectPtr);
            *result = kErrorNotFound;
            return true;
        }

        // The host is already seated by CreateSession — the server puts it
        // there, because on the console XSessionCreate is followed by
        // XSessionJoinLocal and a lobby that advertised both of two slots
        // would refuse its second player. So the host's own JoinLocal has
        // nothing to send. Neither has a joiner's: its create took the seat
        // (see Pending::joiner), and the server's join is idempotent anyway.
        bool alreadySeated = weAreHost;
        if (!alreadySeated && message == 0x000B0012)
        {
            const uint64_t me = Live().identity().xuid;
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sessions.find(objectPtr);
            if (it != g_sessions.end())
                for (const auto& member : it->second.members)
                    if (member.xuid == me)
                        alreadySeated = true;
        }
        if (alreadySeated && message == 0x000B0012)
        {
            *result = kErrorSuccess;
            if (overlappedVa)
                Xam_CompleteOverlapped(overlappedVa, kErrorSuccess, 0);
            return true;
        }

        // A private slot when the guest asked for one. The array is one byte
        // per player and the title only ever joins itself here.
        bool wantPrivate = false;
        if (const auto* flags = GuestPtr<const uint8_t>(msg->privateSlotArrayPtr.get()))
            wantPrivate = flags[0] != 0;

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.objectPtr = objectPtr;
        pending.sessionId = sessionId;
        pending.ticket = message == 0x000B0012 ? Live().JoinSession(sessionId, wantPrivate)
                                               : Live().LeaveSession(sessionId);
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionSearchByID ------------------------------------------------
    case 0x000B001B:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionSearchById))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionSearchById*>(buffer);
        const uint64_t sessionId = ReadXnkid(&msg->sessionId);
        if (sessionId == 0)
        {
            *result = kErrorInvalidParameter;
            return true;
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.sessionId = sessionId;
        pending.resultsPtr = msg->searchResultsPtr.get();
        pending.resultsSize = msg->resultsBufferSize.get();
        pending.ticket = Live().GetSessionDetails(sessionId);
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionSearchEx --------------------------------------------------
    case 0x000B001C:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionSearchEx))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionSearchEx*>(buffer);

        xlive::Client::SessionSearchRequest request;
        request.max_results = int(msg->maxResults.get());
        if (request.max_results <= 0 || request.max_results > 200)
            request.max_results = 25;

        // The filters the title actually wants. Both arrays are guest-supplied
        // and both counts are bounded before they are walked (gotcha 73).
        const uint32_t contextCount = msg->contextCount.get();
        const uint32_t propertyCount = msg->propertyCount.get();
        if (contextCount > 64 || propertyCount > 64)
        {
            KLOG("XSessionSearchEx: implausible filter counts (%u contexts, %u properties)\n",
                 contextCount, propertyCount);
            *result = kErrorInvalidParameter;
            return true;
        }
        const uint32_t contextsVa = msg->contextsPtr.get();
        for (uint32_t i = 0; contextsVa && i < contextCount; i++)
        {
            const auto* entry = GuestPtr<const be<uint32_t>>(contextsVa + i * 8);
            if (!entry)
                break;
            request.contexts.push_back({entry[0].get(), entry[1].get()});
        }
        const uint32_t propertiesVa = msg->propertiesPtr.get();
        for (uint32_t i = 0; propertiesVa && i < propertyCount; i++)
        {
            const uint32_t propertyVa = propertiesVa + i * 24;
            const auto* words = GuestPtr<const be<uint32_t>>(propertyVa);
            if (!words)
                break;
            xlive::Client::StatProperty property;
            property.id = words[0].get();
            property.type = xlive::Client::StatProperty::Type((property.id >> 28) & 0xF);
            // The value is an 8-byte union at +16, so a 32-bit member sits in
            // the FIRST four bytes of it — the same trap XSessionWriteStats
            // has, and the same answer.
            if (property.type == xlive::Client::StatProperty::Type::Int64 ||
                property.type == xlive::Client::StatProperty::Type::DateTime)
            {
                const auto* wide = GuestPtr<const be<uint64_t>>(propertyVa + 16);
                if (wide)
                    property.integer = int64_t(wide->get());
            }
            else
            {
                property.integer = int32_t(words[4].get());
            }
            request.properties.push_back(std::move(property));
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.resultsPtr = msg->searchResultsPtr.get();
        pending.resultsSize = msg->resultsBufferSize.get();
        pending.ticket = Live().SearchSessions(request);
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionGetDetails ------------------------------------------------
    case 0x000B001D:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionDetails))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionDetails*>(buffer);
        const uint32_t objectPtr = msg->objectPtr.get();

        uint64_t sessionId = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sessions.find(objectPtr);
            if (it != g_sessions.end())
                sessionId = it->second.sessionId;
        }
        if (sessionId == 0)
        {
            *result = kErrorNotFound;
            return true;
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.objectPtr = objectPtr;
        pending.sessionId = sessionId;
        pending.detailsPtr = msg->sessionDetailsPtr.get();
        pending.detailsSize = msg->detailsBufferSize.get();
        pending.ticket = Live().GetSessionDetails(sessionId);
        *result = Begin(std::move(pending));
        return true;
    }

    // -- XSessionMigrateHost -----------------------------------------------
    case 0x000B001E:
    {
        if (!buffer || bufferLength < sizeof(GuestSessionMigrate))
        {
            *result = kErrorInvalidParameter;
            return true;
        }
        const auto* msg = static_cast<const GuestSessionMigrate*>(buffer);
        const uint32_t objectPtr = msg->objectPtr.get();

        uint64_t sessionId = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_sessions.find(objectPtr);
            if (it != g_sessions.end())
                sessionId = it->second.sessionId;
        }
        if (sessionId == 0)
        {
            *result = kErrorNotFound;
            return true;
        }

        Pending pending;
        pending.message = message;
        pending.overlappedVa = overlappedVa;
        pending.objectPtr = objectPtr;
        pending.sessionId = sessionId;
        pending.sessionInfoPtr = msg->sessionInfoPtr.get();
        // Migrating to ourselves is what the title asks for: the host went
        // away and this machine is taking over.
        pending.ticket = Live().MigrateHost(sessionId, Live().identity().xuid);
        *result = Begin(std::move(pending));
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// The virtual XNet
// ---------------------------------------------------------------------------

uint32_t XliveSession_XnAddrToInAddr(const void* xnaddr)
{
    if (!xnaddr)
        return 0;
    const auto* addr = static_cast<const GuestXnAddr*>(xnaddr);
    const uint64_t xuid = addr->online.xboxId.get();
    if (xuid == 0)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_addressForXuid.find(xuid);
    return it == g_addressForXuid.end() ? 0 : it->second;
}

bool XliveSession_InAddrToXnAddr(uint32_t inAddr, void* xnaddrOut)
{
    if (!xnaddrOut)
        return false;
    uint64_t xuid = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_xuidForAddress.find(inAddr);
        if (it == g_xuidForAddress.end())
            return false;
        xuid = it->second;
    }
    FillXnAddr(static_cast<GuestXnAddr*>(xnaddrOut), xuid);
    return true;
}

uint32_t XliveSession_InAddrForPeer(uint64_t xuid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return AddressForXuid(xuid);
}

uint64_t XliveSession_PeerForInAddr(uint32_t inAddr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_xuidForAddress.find(inAddr);
    return it == g_xuidForAddress.end() ? 0 : it->second;
}

bool XliveSession_LocalXnAddr(void* xnaddrOut)
{
    if (!xnaddrOut || !g_enabled)
        return false;
    const uint64_t xuid = Live().identity().xuid;
    // No account means no online identity, and inventing one here would make
    // XNetGetTitleXnAddr claim this machine is on Live when it is not.
    if (!Live().online() || xuid == xlive::kOfflineXuid)
        return false;

    // This machine's XNADDR is the one every peer computes for our XUID —
    // the same FillXnAddr the session's member list is built with.
    std::lock_guard<std::mutex> lock(g_mutex);
    FillXnAddr(static_cast<GuestXnAddr*>(xnaddrOut), xuid);
    return true;
}

// ---------------------------------------------------------------------------
// The self-test
// ---------------------------------------------------------------------------
//
// Co-op has two halves. One is a server and a second player, and there is no
// way to exercise it from a boot. The other is guest-struct arithmetic —
// offsets, byte order, buffer sizing — and that half is exactly where a port
// gets it wrong silently, because a field written four bytes late still looks
// like a field. So that half is tested here, on every developer boot, with no
// network and nobody to play with.
//
// The precedent is FileImportsWriteSelfTest in main.cpp, and the reason is the
// same one part 38 learned about counters.

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

// A scratch block in guest memory, so the test writes through exactly the path
// the real code does — g_memory.Translate of a genuine guest address — rather
// than a host buffer that would hide an address-space mistake.
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

void TestFieldOffsets()
{
    // The sizes are static_asserts above. The OFFSETS are what a hand-written
    // struct gets wrong, and a wrong offset inside a right-sized struct is
    // invisible until two machines disagree about who is in a lobby.
    XLIVE_EXPECT(offsetof(GuestXnAddr, ina) == 0x00);
    XLIVE_EXPECT(offsetof(GuestXnAddr, inaOnline) == 0x04);
    XLIVE_EXPECT(offsetof(GuestXnAddr, portOnline) == 0x08);
    XLIVE_EXPECT(offsetof(GuestXnAddr, enet) == 0x0A);
    XLIVE_EXPECT(offsetof(GuestXnAddr, online) == 0x10);
    XLIVE_EXPECT(offsetof(GuestSgAddr, xboxId) == 0x08);

    XLIVE_EXPECT(offsetof(GuestSessionInfo, sessionId) == 0x00);
    XLIVE_EXPECT(offsetof(GuestSessionInfo, hostAddress) == 0x08);
    XLIVE_EXPECT(offsetof(GuestSessionInfo, keyExchange) == 0x2C);

    XLIVE_EXPECT(offsetof(GuestSessionCreate, sessionInfoPtr) == 0x14);
    XLIVE_EXPECT(offsetof(GuestSessionCreate, noncePtr) == 0x18);
    XLIVE_EXPECT(offsetof(GuestSessionState, nonce) == 0x08);
    XLIVE_EXPECT(offsetof(GuestSessionManage, privateSlotArrayPtr) == 0x10);
    XLIVE_EXPECT(offsetof(GuestSessionSearchById, sessionId) == 0x04);
    XLIVE_EXPECT(offsetof(GuestSessionSearchById, searchResultsPtr) == 0x10);
    XLIVE_EXPECT(offsetof(GuestSessionSearchEx, propertyCount) == 0x0C);
    XLIVE_EXPECT(offsetof(GuestSessionSearchEx, contextCount) == 0x0E);
    XLIVE_EXPECT(offsetof(GuestSessionSearchEx, searchResultsPtr) == 0x1C);
    XLIVE_EXPECT(offsetof(GuestSessionDetails, sessionDetailsPtr) == 0x08);
    XLIVE_EXPECT(offsetof(GuestUserSetProperty, propertyId) == 0x10);
    XLIVE_EXPECT(offsetof(GuestUserSetProperty, dataPtr) == 0x18);
    XLIVE_EXPECT(offsetof(GuestSearchResult, openPublic) == 0x3C);
    XLIVE_EXPECT(offsetof(GuestSearchResult, contextsPtr) == 0x58);
    XLIVE_EXPECT(offsetof(GuestLocalDetails, sessionInfo) == 0x38);
    XLIVE_EXPECT(offsetof(GuestLocalDetails, sessionMembersPtr) == 0x7C);
}

void TestXnkidRoundTrip()
{
    // ab[0] is the MOST significant byte. Get this backwards and every session
    // id we hand the title fails its own IsValidXNKID, which checks the top
    // nibble — and fails silently, because the title just does not join.
    const uint64_t id = 0x8123456789ABCDEFull;
    GuestXnkid xnkid{};
    WriteXnkid(&xnkid, id);
    XLIVE_EXPECT(xnkid.ab[0] == 0x81);
    XLIVE_EXPECT(xnkid.ab[7] == 0xEF);
    XLIVE_EXPECT(ReadXnkid(&xnkid) == id);
    // The top nibble the guest checks for a peer-to-peer session.
    XLIVE_EXPECT((ReadXnkid(&xnkid) >> 60) == 0x8);
}

void TestAddressTranslation()
{
    const uint64_t xuid = 0x0009000000000019ull;
    GuestScratch scratch(sizeof(GuestXnAddr));
    XLIVE_EXPECT(scratch.va != 0);
    if (!scratch.va)
        return;

    auto* addr = GuestPtr<GuestXnAddr>(scratch.va);
    FillXnAddr(addr, xuid);

    XLIVE_EXPECT(addr->online.xboxId.get() == xuid);
    XLIVE_EXPECT(addr->portOnline.get() == kVirtualPort);
    // In the benchmarking block, so a stray packet goes nowhere.
    XLIVE_EXPECT((addr->ina.get() & 0xFFFE0000u) == kFakeNetBase);
    // Locally administered, not multicast.
    XLIVE_EXPECT((addr->enet[0] & 0x03) == 0x02);

    const uint32_t inAddr = XliveSession_XnAddrToInAddr(addr);
    XLIVE_EXPECT(inAddr == addr->ina.get());
    XLIVE_EXPECT(XliveSession_PeerForInAddr(inAddr) == xuid);

    GuestScratch back(sizeof(GuestXnAddr));
    if (back.va)
    {
        XLIVE_EXPECT(XliveSession_InAddrToXnAddr(inAddr, GuestPtr<GuestXnAddr>(back.va)));
        XLIVE_EXPECT(GuestPtr<GuestXnAddr>(back.va)->online.xboxId.get() == xuid);
    }

    // The same XUID must map to the same address every time, or a title that
    // cached one would start sending to somebody else.
    XLIVE_EXPECT(AddressForXuid(xuid) == inAddr);
    // And a different XUID must not collide with it.
    XLIVE_EXPECT(AddressForXuid(xuid + 1) != inAddr);
    // An address we never handed out belongs to nobody.
    XLIVE_EXPECT(XliveSession_PeerForInAddr(0x08080808u) == 0);
}

xlive::Client::SessionInfo SampleSession()
{
    xlive::Client::SessionInfo session;
    session.session_id = 0x8ABCDEF012345678ull;
    for (int i = 0; i < 16; i++)
        session.key[i] = uint8_t(0xA0 + i);
    session.host_xuid = 0x0009000000000019ull;
    session.host_gamertag = "Chuck Greene";
    session.flags = 0x00000011;
    session.nonce = 0x1122334455667788ull;
    session.public_slots = 2;
    session.private_slots = 0;
    session.open_public_slots = 1;
    session.filled_public_slots = 1;
    session.state = xlive::Client::SessionState::Lobby;
    session.members.push_back({0x0009000000000019ull, "Chuck Greene", true, false});
    session.members.push_back({0x000900000000001Aull, "Frank West", false, false});
    return session;
}

void TestSearchResultBuffer()
{
    const auto session = SampleSession();
    const std::vector<xlive::Client::SessionInfo> results{session};
    const uint32_t needed =
        uint32_t(sizeof(GuestSearchResultHeader) + sizeof(GuestSearchResult));

    // Too small: the guest is told how much to allocate, and nothing is
    // written past the first word. This is the console's two-call pattern and
    // the title depends on it.
    {
        GuestScratch scratch(needed);
        if (!scratch.va)
            return;
        Pending pending;
        pending.resultsPtr = scratch.va;
        pending.resultsSize = 4;
        XLIVE_EXPECT(WriteSearchResults(pending, results) == kErrorInsufficientBuffer);
        XLIVE_EXPECT(GuestPtr<GuestSearchResultHeader>(scratch.va)->count.get() == needed);
    }

    // Big enough: header, then the array immediately after it, in the guest's
    // own buffer.
    {
        GuestScratch scratch(needed);
        if (!scratch.va)
            return;
        Pending pending;
        pending.resultsPtr = scratch.va;
        pending.resultsSize = needed;
        XLIVE_EXPECT(WriteSearchResults(pending, results) == kErrorSuccess);

        const auto* header = GuestPtr<GuestSearchResultHeader>(scratch.va);
        XLIVE_EXPECT(header->count.get() == 1);
        XLIVE_EXPECT(header->resultsPtr.get() ==
                     scratch.va + uint32_t(sizeof(GuestSearchResultHeader)));

        const auto* entry = GuestPtr<GuestSearchResult>(header->resultsPtr.get());
        XLIVE_EXPECT(ReadXnkid(&entry->info.sessionId) == session.session_id);
        XLIVE_EXPECT(entry->info.keyExchange[0] == 0xA0);
        XLIVE_EXPECT(entry->info.keyExchange[15] == 0xAF);
        XLIVE_EXPECT(entry->info.hostAddress.online.xboxId.get() == session.host_xuid);
        XLIVE_EXPECT(entry->openPublic.get() == 1);
        XLIVE_EXPECT(entry->filledPublic.get() == 1);
        // An empty list is zero count AND a null pointer, not a pointer into
        // memory nobody allocated.
        XLIVE_EXPECT(entry->propertyCount.get() == 0);
        XLIVE_EXPECT(entry->propertiesPtr.get() == 0);
    }

    // No results at all: a well-formed empty answer.
    {
        GuestScratch scratch(needed);
        if (!scratch.va)
            return;
        Pending pending;
        pending.resultsPtr = scratch.va;
        pending.resultsSize = needed;
        XLIVE_EXPECT(WriteSearchResults(pending, {}) == kErrorSuccess);
        const auto* header = GuestPtr<GuestSearchResultHeader>(scratch.va);
        XLIVE_EXPECT(header->count.get() == 0);
        XLIVE_EXPECT(header->resultsPtr.get() == 0);
    }

    // A host that advertised what Case Zero's lobby advertises: two contexts
    // and three int64 properties. They follow the array, the size the guest
    // is told to allocate covers them, and the title's own walk — 24 bytes a
    // property, the value at +16 — finds 0x20000002.
    {
        xlive::Client::SessionInfo advertised = session;
        advertised.contexts = {{0x800A, 1}, {0x800B, 0}};
        for (uint32_t id : {0x20000001u, 0x20000002u, 0x20000003u})
        {
            xlive::Client::StatProperty property;
            property.id = id;
            property.type = xlive::Client::StatProperty::Type::Int64;
            property.integer = int64_t(id & 0xF) * 1000;
            advertised.properties.push_back(property);
        }
        const std::vector<xlive::Client::SessionInfo> one{advertised};
        const uint32_t withTail = needed + 2 * 8 + 3 * 24;
        GuestScratch small(withTail);
        GuestScratch scratch(withTail);
        if (!small.va || !scratch.va)
            return;
        Pending pending;
        pending.resultsPtr = small.va;
        pending.resultsSize = needed; // the old size, without the tail
        XLIVE_EXPECT(WriteSearchResults(pending, one) == kErrorInsufficientBuffer);
        XLIVE_EXPECT(GuestPtr<GuestSearchResultHeader>(small.va)->count.get() == withTail);

        pending.resultsPtr = scratch.va;
        pending.resultsSize = withTail;
        XLIVE_EXPECT(WriteSearchResults(pending, one) == kErrorSuccess);
        const auto* header = GuestPtr<GuestSearchResultHeader>(scratch.va);
        const auto* entry = GuestPtr<GuestSearchResult>(header->resultsPtr.get());
        XLIVE_EXPECT(entry->contextCount.get() == 2);
        XLIVE_EXPECT(entry->propertyCount.get() == 3);
        const auto* contexts = GuestPtr<GuestUserContext>(entry->contextsPtr.get());
        XLIVE_EXPECT(contexts && contexts[0].id.get() == 0x800A && contexts[0].value.get() == 1);
        const auto* properties = GuestPtr<GuestUserProperty>(entry->propertiesPtr.get());
        XLIVE_EXPECT(properties != nullptr);
        if (properties)
        {
            // sub_82599D80's walk.
            bool found = false;
            for (uint32_t i = 0; i < entry->propertyCount.get(); i++)
            {
                if (properties[i].id.get() != 0x20000002)
                    continue;
                found = true;
                XLIVE_EXPECT(properties[i].type == 2); // INT64
                XLIVE_EXPECT(reinterpret_cast<const be<int64_t>*>(properties[i].value)->get() == 2000);
            }
            XLIVE_EXPECT(found);
            // Everything landed inside the buffer the guest gave.
            XLIVE_EXPECT(entry->propertiesPtr.get() + 3 * 24 <= scratch.va + withTail);
        }
    }
}

void TestDetailsBuffer()
{
    const auto session = SampleSession();
    const uint32_t needed =
        uint32_t(sizeof(GuestLocalDetails) + 2 * sizeof(GuestSessionMember));

    {
        GuestScratch scratch(needed);
        if (!scratch.va)
            return;
        Pending pending;
        pending.detailsPtr = scratch.va;
        pending.detailsSize = 8;
        XLIVE_EXPECT(WriteDetails(pending, session) == kErrorInsufficientBuffer);
        XLIVE_EXPECT(GuestPtr<be<uint32_t>>(scratch.va)->get() == needed);
    }

    {
        GuestScratch scratch(needed);
        if (!scratch.va)
            return;
        Pending pending;
        pending.detailsPtr = scratch.va;
        pending.detailsSize = needed;
        XLIVE_EXPECT(WriteDetails(pending, session) == kErrorSuccess);

        const auto* details = GuestPtr<GuestLocalDetails>(scratch.va);
        XLIVE_EXPECT(details->maxPublicSlots.get() == 2);
        XLIVE_EXPECT(details->availablePublicSlots.get() == 1);
        XLIVE_EXPECT(details->actualMemberCount.get() == 2);
        XLIVE_EXPECT(details->nonce.get() == session.nonce);
        XLIVE_EXPECT(details->flags.get() == session.flags);
        XLIVE_EXPECT(ReadXnkid(&details->sessionInfo.sessionId) == session.session_id);
        XLIVE_EXPECT(details->sessionMembersPtr.get() ==
                     scratch.va + uint32_t(sizeof(GuestLocalDetails)));

        const auto* members = GuestPtr<GuestSessionMember>(details->sessionMembersPtr.get());
        XLIVE_EXPECT(members[0].onlineXuid.get() == session.members[0].xuid);
        XLIVE_EXPECT(members[1].onlineXuid.get() == session.members[1].xuid);
    }
}

void TestPropertyDecode()
{
    // Each type, built exactly as guest code would build it, and read back
    // through the real decoder. The union at +16 is the trap here as much as
    // it is in XSessionWriteStats: a 32-bit member sits in the FIRST four
    // bytes of the eight, and reading it from +20 returns zero every time.
    struct Sample
    {
        uint32_t id;
        uint32_t size;
        const char* what;
    };
    const Sample samples[] = {
        {0x10000001, 4, "int32"},
        {0x20000004, 8, "int64"},
        {0x50008104, 4, "float"},
        {0x30008105, 8, "double"},
    };

    for (const auto& sample : samples)
    {
        GuestScratch data(16);
        GuestScratch message(sizeof(GuestUserSetProperty));
        if (!data.va || !message.va)
            return;

        if (sample.size == 4)
            *GuestPtr<be<uint32_t>>(data.va) = 0x0000002Au;  // 42, or 5.9e-44f
        else
            *GuestPtr<be<uint64_t>>(data.va) = 0x000000000000002Aull;

        auto* msg = GuestPtr<GuestUserSetProperty>(message.va);
        msg->userIndex = 0;
        msg->xuid = 0;
        msg->propertyId = sample.id;
        msg->dataSize = sample.size;
        msg->dataPtr = data.va;

        uint32_t result = 0xFFFFFFFFu;
        const bool handled =
            XliveSession_Dispatch(0x000B0007, msg, sizeof(GuestUserSetProperty), 0, &result);
        XLIVE_EXPECT(handled);
        XLIVE_EXPECT(result == kErrorSuccess);

        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_properties.find(sample.id);
        XLIVE_EXPECT(it != g_properties.end());
        if (it == g_properties.end())
            continue;
        XLIVE_EXPECT(uint8_t(it->second.type) == uint8_t((sample.id >> 28) & 0xF));
        if (sample.id == 0x10000001 || sample.id == 0x20000004)
            XLIVE_EXPECT(it->second.integer == 42);
    }
}

// A message about a session the title never created must be refused, not
// silently succeeded at. This is the honest-failure rule in the one place it
// is easiest to break.
void TestUnknownSessionIsRefused()
{
    GuestScratch scratch(sizeof(GuestSessionState));
    if (!scratch.va)
        return;
    auto* msg = GuestPtr<GuestSessionState>(scratch.va);
    msg->objectPtr = 0xDEADBEEFu;  // never created

    uint32_t result = 0;
    XLIVE_EXPECT(XliveSession_Dispatch(0x000B0014, msg, sizeof(GuestSessionState), 0, &result));
    XLIVE_EXPECT(result == kErrorNotFound);
}

// A buffer shorter than the message is a corrupt request, and every handler
// has to say so rather than read past it.
void TestShortBuffersAreRefused()
{
    GuestScratch scratch(64);
    if (!scratch.va)
        return;
    const struct { uint32_t message; uint32_t size; } cases[] = {
        {0x000B0010, sizeof(GuestSessionCreate) - 1},
        {0x000B0011, sizeof(GuestSessionState) - 1},
        {0x000B0012, sizeof(GuestSessionManage) - 1},
        {0x000B001B, sizeof(GuestSessionSearchById) - 1},
        {0x000B001C, sizeof(GuestSessionSearchEx) - 1},
        {0x000B001D, sizeof(GuestSessionDetails) - 1},
        {0x000B001E, sizeof(GuestSessionMigrate) - 1},
        {0x000B0007, sizeof(GuestUserSetProperty) - 1},
    };
    for (const auto& c : cases)
    {
        uint32_t result = 0;
        XLIVE_EXPECT(XliveSession_Dispatch(c.message, GuestPtr<void>(scratch.va), c.size, 0,
                                           &result));
        XLIVE_EXPECT(result == kErrorInvalidParameter);
    }

    // And a null buffer, which is the same class of mistake.
    for (const auto& c : cases)
    {
        uint32_t result = 0;
        XLIVE_EXPECT(XliveSession_Dispatch(c.message, nullptr, 64, 0, &result));
        XLIVE_EXPECT(result == kErrorInvalidParameter);
    }
}

}  // namespace

void XliveSession_SelfTest()
{
    const char* on = std::getenv("CZ_XLIVE_COOP_TEST");
    if (!on || on[0] != '1')
        return;

    // The decode paths need the bridge on. Turning it on for the duration is
    // safe: the tests that reach Dispatch either fail before any request is
    // made, or set a property, which never touches the network.
    const bool wasEnabled = g_enabled;
    g_enabled = true;
    g_selfTestFailures = 0;

    TestFieldOffsets();
    TestXnkidRoundTrip();
    TestAddressTranslation();
    TestSearchResultBuffer();
    TestDetailsBuffer();
    TestPropertyDecode();
    TestUnknownSessionIsRefused();
    TestShortBuffersAreRefused();

    // Leave nothing behind: the self-test's fake peers and properties must not
    // be what a real session starts from.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_properties.clear();
        g_addressForXuid.clear();
        g_xuidForAddress.clear();
    }
    g_enabled = wasEnabled;

    if (g_selfTestFailures == 0)
        fprintf(stderr, "[xlive] session self-test: the guest ABI is intact\n");
    else
        fprintf(stderr, "[xlive] session self-test: %d FAILURE(S)\n", g_selfTestFailures);
}

void XliveSession_SetFriendsOnlySearch(bool on)
{
    g_friendsOnlySearch = on;
    KLOG("[xlive] session search: %s\n",
         on ? "FRIENDS ONLY (the JoinGame screen's JOIN FRIENDS row)" : "any joinable session");
}
