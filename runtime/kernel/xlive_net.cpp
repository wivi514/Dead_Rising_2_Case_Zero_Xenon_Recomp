// See xlive_net.h for what this is. The exports are hooked at the bottom of
// this file; everything above them is the table and the two translations.

#include "xlive_net.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "memory.h"
#include "xlive_glue.h"
#include "xlive_session.h"

#include <xlive/client.h>

// Defined in imports.cpp: signals a guest event by handle, for the QoS
// lookup's hEvent.
void SignalGuestEvent(uint32_t handle);

namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// What the generated stub returned for every one of these before there was an
// implementation: STATUS_NOT_IMPLEMENTED. With co-op off it still does.
constexpr uint32_t kUnimplemented = 0xC0000002u;

constexpr uint32_t kSocketError = 0xFFFFFFFFu; // SOCKET_ERROR / INVALID_SOCKET

// Winsock, as the title tests them.
constexpr uint32_t kWsaEINVAL = 10022;
constexpr uint32_t kWsaEWOULDBLOCK = 10035;
constexpr uint32_t kWsaEMSGSIZE = 10040;
constexpr uint32_t kWsaEPROTONOSUPPORT = 10043;
constexpr uint32_t kWsaEOPNOTSUPP = 10045;
constexpr uint32_t kWsaEAFNOSUPPORT = 10047;
constexpr uint32_t kWsaENETDOWN = 10050;
constexpr uint32_t kWsaENOTCONN = 10057;
constexpr uint32_t kWsaEHOSTUNREACH = 10065;
constexpr uint32_t kWsaENOTSOCK = 10038;

constexpr uint32_t AF_INET_ = 2;
constexpr uint32_t SOCK_STREAM_ = 1;
constexpr uint32_t SOCK_DGRAM_ = 2;
constexpr uint32_t FIONBIO_ = 0x8004667Eu; // sub_8259BCB0: lis 0x8004, ori 0x667E
constexpr uint32_t FIONREAD_ = 0x4004667Fu;

// XNET_XNQOSINFO_*
constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;
constexpr uint8_t kQosTargetDisabled = 0x04;

constexpr uint32_t kMaxSockets = 64;
// Winsock's FD_SETSIZE, which is what the guest's fd_set is sized to.
constexpr uint32_t kFdSetSize = 64;

// ---------------------------------------------------------------------------
// Guest structures
// ---------------------------------------------------------------------------

#pragma pack(push, 4)

// sockaddr_in. The port and address are in NETWORK order in memory, which on
// a big-endian guest is the same thing as a be<> field: sub_8259BCB0 does
// `li r9,1005; sth r9,98(r1)` and means port 1005.
struct GuestSockAddrIn
{
    be<uint16_t> family;
    be<uint16_t> port;
    be<uint32_t> addr;
    uint8_t zero[8];
};
static_assert(sizeof(GuestSockAddrIn) == 16, "sockaddr_in is 16; the title passes 16");

struct GuestFdSet
{
    be<uint32_t> count;
    be<uint32_t> sockets[kFdSetSize];
};
static_assert(sizeof(GuestFdSet) == 4 + 4 * kFdSetSize, "fd_set is 260");

struct GuestTimeval
{
    be<int32_t> seconds;
    be<int32_t> microseconds;
};
static_assert(sizeof(GuestTimeval) == 8, "timeval is 8");

// XNQOSINFO, 24 bytes: sub_825A2E00 steps them by 24 and reads bFlags at +0
// and wRttMedInMsecs at +14.
struct GuestQosInfo
{
    uint8_t flags;            // +0x00
    uint8_t reserved;         // +0x01
    be<uint16_t> probesSent;  // +0x02
    be<uint16_t> probesRecv;  // +0x04
    be<uint16_t> dataSize;    // +0x06
    be<uint32_t> dataPtr;     // +0x08
    be<uint16_t> rttMin;      // +0x0C
    be<uint16_t> rttMed;      // +0x0E
    be<uint32_t> upBitsPerSec;   // +0x10
    be<uint32_t> downBitsPerSec; // +0x14
};
static_assert(sizeof(GuestQosInfo) == 24, "XNQOSINFO is 24");

// XNQOS: the counts, then the entries. sub_825A2E00 polls +4 for zero.
struct GuestQos
{
    be<uint32_t> count;
    be<uint32_t> pending;
    GuestQosInfo info[1];
};
static_assert(offsetof(GuestQos, info) == 8, "XNQOS entries start at +8");

#pragma pack(pop)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

xlive::Client& Live() { return xlive::Client::Instance(); }

struct GuestSocket
{
    uint32_t type = 0;
    uint32_t protocol = 0;
    uint16_t boundPort = 0;
    bool nonblocking = false;
    // connect() on a datagram socket names a default peer for send()/recv().
    uint32_t connectedAddr = 0;
    uint16_t connectedPort = 0;
};

std::mutex g_mutex;
std::map<uint32_t, GuestSocket> g_sockets;
// Handles start high so a title that confuses one with a file handle or an
// index fails loudly rather than by coincidence.
uint32_t g_nextSocket = 0x1000;

// WSAGetLastError is per thread, and so is this.
thread_local uint32_t t_lastError = 0;

uint32_t Fail(uint32_t error)
{
    t_lastError = error;
    return kSocketError;
}

template <typename T>
T* GuestPtr(uint32_t va)
{
    if (va == 0)
        return nullptr;
    return reinterpret_cast<T*>(g_memory.Translate(va));
}

// The socket for a handle, or null. Under g_mutex.
GuestSocket* Find(uint32_t handle)
{
    auto it = g_sockets.find(handle);
    return it == g_sockets.end() ? nullptr : &it->second;
}

bool IsBroadcast(uint32_t addr)
{
    // Limited broadcast, or the stand-in block's own broadcast. The title's
    // system-link discovery (sub_8259D548) sends to one every second, and a
    // broadcast nobody is on the wire to hear is not an error.
    return addr == 0xFFFFFFFFu || addr == 0xC613FFFFu;
}

// ---------------------------------------------------------------------------
// The socket calls
// ---------------------------------------------------------------------------

uint32_t Socket(uint32_t family, uint32_t type, uint32_t protocol)
{
    if (family != AF_INET_)
        return Fail(kWsaEAFNOSUPPORT);
    if (type == SOCK_STREAM_)
    {
        KLOG("[xlive] socket(SOCK_STREAM) refused: no stream transport over the punched path\n");
        return Fail(kWsaEPROTONOSUPPORT);
    }
    if (type != SOCK_DGRAM_)
        return Fail(kWsaEPROTONOSUPPORT);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sockets.size() >= kMaxSockets)
        return Fail(kWsaENETDOWN);
    const uint32_t handle = g_nextSocket++;
    GuestSocket& sock = g_sockets[handle];
    sock.type = type;
    sock.protocol = protocol;
    KLOG("[xlive] socket(AF_INET, SOCK_DGRAM, proto %u) -> %08X\n", protocol, handle);
    return handle;
}

uint32_t CloseSocket(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Find(handle))
        return Fail(kWsaENOTSOCK);
    g_sockets.erase(handle);
    KLOG("[xlive] closesocket(%08X)\n", handle);
    return 0;
}

uint32_t Bind(uint32_t handle, const GuestSockAddrIn* name, uint32_t nameLength)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    GuestSocket* sock = Find(handle);
    if (!sock)
        return Fail(kWsaENOTSOCK);
    if (!name || nameLength < sizeof(GuestSockAddrIn) || name->family.get() != AF_INET_)
        return Fail(kWsaEINVAL);
    sock->boundPort = name->port.get();
    KLOG("[xlive] bind(%08X, port %u)\n", handle, sock->boundPort);
    return 0;
}

uint32_t IoctlSocket(uint32_t handle, uint32_t command, be<uint32_t>* argument)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    GuestSocket* sock = Find(handle);
    if (!sock)
        return Fail(kWsaENOTSOCK);
    if (!argument)
        return Fail(kWsaEINVAL);
    switch (command)
    {
    case FIONBIO_:
        sock->nonblocking = argument->get() != 0;
        KLOG("[xlive] ioctlsocket(%08X, FIONBIO, %u)\n", handle, argument->get());
        return 0;
    case FIONREAD_:
        // Bytes readable without blocking. The path does not peek sizes, so
        // this is the largest a waiting datagram can be, or nothing.
        *argument = Live().PendingDatagrams() > 0 ? uint32_t(xlive::Client::kMaxDatagram) : 0u;
        return 0;
    default:
        return Fail(kWsaEOPNOTSUPP);
    }
}

uint32_t SetSockOpt(uint32_t handle, uint32_t level, uint32_t option, const void* value,
                    uint32_t valueLength)
{
    (void)value;
    (void)valueLength;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Find(handle))
        return Fail(kWsaENOTSOCK);
    // Every option the title sets — SO_REUSEADDR, buffer sizes, the 360's own
    // 0x5801 family — describes a socket this is not. Accepted, so the title's
    // setup path completes; nothing here has the property being set.
    KLOG("[xlive] setsockopt(%08X, level %04X, option %04X) accepted\n", handle, level, option);
    return 0;
}

uint32_t Connect(uint32_t handle, const GuestSockAddrIn* name, uint32_t nameLength)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    GuestSocket* sock = Find(handle);
    if (!sock)
        return Fail(kWsaENOTSOCK);
    if (!name || nameLength < sizeof(GuestSockAddrIn))
        return Fail(kWsaEINVAL);
    // On a datagram socket connect() only names the default peer.
    sock->connectedAddr = name->addr.get();
    sock->connectedPort = name->port.get();
    return 0;
}

// The one translation every send makes: the stand-in address the title was
// given, to the peer it stands for, to the path. Returns 0 on success, or
// the Winsock error to report.
uint32_t SendToAddress(const GuestSocket& sock, uint32_t addr, const void* data, uint32_t length,
                       uint32_t* sent)
{
    (void)sock;
    *sent = 0;
    if (IsBroadcast(addr))
    {
        // Dropped on the floor, reported as sent: a broadcast has no
        // delivery to fail, and there is nobody on this wire to hear one.
        *sent = length;
        return 0;
    }
    const uint64_t xuid = XliveSession_PeerForInAddr(addr);
    if (xuid == 0)
        return kWsaEHOSTUNREACH;
    if (length > xlive::Client::kMaxDatagram)
        return kWsaEMSGSIZE;

    const int n = Live().SendTo(xuid, data, length);
    if (n >= 0)
    {
        *sent = uint32_t(n);
        return 0;
    }
    // No path. While the punch is still running that is a moment's
    // condition, and the title's own send loop treats 10035 as one; once it
    // has failed, or the peer was never in the session, the address really
    // is unreachable.
    switch (Live().peer_state(xuid))
    {
    case xlive::Client::PeerState::Unknown:
    case xlive::Client::PeerState::Punching:
        return kWsaEWOULDBLOCK;
    default:
        return kWsaEHOSTUNREACH;
    }
}

uint32_t SendTo(uint32_t handle, const void* data, uint32_t length, uint32_t flags,
                const GuestSockAddrIn* to, uint32_t toLength)
{
    (void)flags;
    GuestSocket sock;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        GuestSocket* found = Find(handle);
        if (!found)
            return Fail(kWsaENOTSOCK);
        sock = *found;
    }
    if (!data || !to || toLength < sizeof(GuestSockAddrIn))
        return Fail(kWsaEINVAL);
    uint32_t sent = 0;
    const uint32_t error = SendToAddress(sock, to->addr.get(), data, length, &sent);
    if (error)
        return Fail(error);
    return sent;
}

uint32_t Send(uint32_t handle, const void* data, uint32_t length, uint32_t flags)
{
    (void)flags;
    GuestSocket sock;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        GuestSocket* found = Find(handle);
        if (!found)
            return Fail(kWsaENOTSOCK);
        sock = *found;
    }
    if (!data)
        return Fail(kWsaEINVAL);
    if (sock.connectedAddr == 0)
        return Fail(kWsaENOTCONN);
    uint32_t sent = 0;
    const uint32_t error = SendToAddress(sock, sock.connectedAddr, data, length, &sent);
    if (error)
        return Fail(error);
    return sent;
}

// One receive, honouring the socket's blocking mode. A blocking socket waits
// in short sleeps rather than on a condition, because the datagrams arrive on
// libxlive's socket and the only signal is asking; the title's own sockets are
// all non-blocking (sub_8259BCB0 sets FIONBIO before anything else).
uint32_t Receive(uint32_t handle, void* data, uint32_t capacity, GuestSockAddrIn* from,
                 be<int32_t>* fromLength)
{
    GuestSocket sock;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        GuestSocket* found = Find(handle);
        if (!found)
            return Fail(kWsaENOTSOCK);
        sock = *found;
    }
    if (!data || capacity == 0)
        return Fail(kWsaEINVAL);

    uint64_t xuid = 0;
    int n = Live().RecvFrom(xuid, data, capacity);
    if (n <= 0 && !sock.nonblocking)
    {
        // Blocking: wait for one, giving up only if the socket is closed
        // under us.
        while (n <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!Find(handle))
                    return Fail(kWsaENOTSOCK);
            }
            n = Live().RecvFrom(xuid, data, capacity);
        }
    }
    if (n <= 0)
        return Fail(kWsaEWOULDBLOCK);

    if (from)
    {
        // The sender, as the stand-in address the title knows it by, and the
        // port it bound itself — both sides of a session bind the same one,
        // and it is the only port there is.
        std::memset(from, 0, sizeof(*from));
        from->family = AF_INET_;
        from->port = sock.boundPort;
        from->addr = XliveSession_InAddrForPeer(xuid);
        if (fromLength)
            *fromLength = int32_t(sizeof(GuestSockAddrIn));
    }
    return uint32_t(n);
}

// select(). Readable is "a datagram is waiting", writable is always, and
// nothing is ever exceptional. A timeout is honoured by polling, for the
// reason Receive gives.
uint32_t Select(GuestFdSet* readSet, GuestFdSet* writeSet, GuestFdSet* exceptSet,
                const GuestTimeval* timeout)
{
    auto ours = [](uint32_t handle) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return Find(handle) != nullptr;
    };
    auto keep = [](GuestFdSet* set, auto predicate) {
        if (!set)
            return 0u;
        uint32_t kept = 0;
        const uint32_t count = std::min(set->count.get(), kFdSetSize);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t handle = set->sockets[i].get();
            if (predicate(handle))
                set->sockets[kept++] = handle;
        }
        set->count = kept;
        return kept;
    };

    const auto deadline = timeout
        ? std::chrono::steady_clock::now() +
              std::chrono::seconds(timeout->seconds.get()) +
              std::chrono::microseconds(timeout->microseconds.get())
        : std::chrono::steady_clock::time_point::max();

    for (;;)
    {
        // Evaluate without modifying, so a wait can re-evaluate the same sets.
        bool readable = false;
        if (readSet && Live().PendingDatagrams() > 0)
        {
            const uint32_t count = std::min(readSet->count.get(), kFdSetSize);
            for (uint32_t i = 0; i < count && !readable; i++)
                readable = ours(readSet->sockets[i].get());
        }
        bool writable = false;
        if (writeSet)
        {
            const uint32_t count = std::min(writeSet->count.get(), kFdSetSize);
            for (uint32_t i = 0; i < count && !writable; i++)
                writable = ours(writeSet->sockets[i].get());
        }
        if (readable || writable || std::chrono::steady_clock::now() >= deadline)
        {
            uint32_t ready = 0;
            ready += keep(readSet, [&](uint32_t h) { return readable && ours(h); });
            ready += keep(writeSet, [&](uint32_t h) { return ours(h); });
            keep(exceptSet, [](uint32_t) { return false; });
            return ready;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint32_t FdIsSet(uint32_t handle, const GuestFdSet* set)
{
    if (!set)
        return 0;
    const uint32_t count = std::min(set->count.get(), kFdSetSize);
    for (uint32_t i = 0; i < count; i++)
        if (set->sockets[i].get() == handle)
            return 1;
    return 0;
}

// inet_addr("a.b.c.d") -> the address as the guest stores it into sin_addr:
// a<<24 | b<<16 | c<<8 | d in r3, which a big-endian store makes network
// order. INADDR_NONE on anything that is not four numbers.
uint32_t InetAddr(const char* text)
{
    if (!text)
        return 0xFFFFFFFFu;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255)
        return 0xFFFFFFFFu;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// ---------------------------------------------------------------------------
// QoS
// ---------------------------------------------------------------------------

// Allocates an XNQOS with `count` entries in guest memory. Every entry is
// complete before the title sees it — there is no probe to run, so there is
// nothing to be pending on.
uint32_t AllocateQos(uint32_t count)
{
    const size_t size = offsetof(GuestQos, info) + size_t(count) * sizeof(GuestQosInfo);
    void* host = g_heap.Alloc(size);
    if (!host)
        return 0;
    std::memset(host, 0, size);
    auto* qos = static_cast<GuestQos*>(host);
    qos->count = count;
    qos->pending = 0;
    return g_memory.MapVirtual(host);
}

void FillQosInfo(GuestQosInfo* info, uint32_t rttMs, bool contacted)
{
    std::memset(info, 0, sizeof(*info));
    info->flags = kQosComplete | (contacted ? kQosTargetContacted : 0);
    info->probesSent = 1;
    info->probesRecv = contacted ? 1 : 0;
    info->rttMin = uint16_t(std::min<uint32_t>(rttMs, 0xFFFF));
    info->rttMed = info->rttMin;
    // A path either exists or it does not; the console's bandwidth estimate
    // has no analogue here, and a zero would read as "no bandwidth" to a title
    // that thresholds on it. This is a plausible broadband figure, and the
    // one number in this file that is not measured.
    info->upBitsPerSec = 1000000;
    info->downBitsPerSec = 1000000;
}

uint32_t QosLookup(uint32_t count, const be<uint32_t>* xnaddrs, const be<uint32_t>* xnkids,
                   const be<uint32_t>* xnkeys, uint32_t hostCount, uint32_t probes,
                   uint32_t eventHandle, be<uint32_t>* qosOut)
{
    (void)xnkids;
    (void)xnkeys;
    (void)probes;
    if (!qosOut)
        return kWsaEINVAL;
    *qosOut = 0;
    if (count == 0 || count > 64 || !xnaddrs || hostCount != 0)
        return kWsaEINVAL;
    if (!Live().online())
        return kWsaENETDOWN;

    const uint32_t qosVa = AllocateQos(count);
    auto* qos = GuestPtr<GuestQos>(qosVa);
    if (!qos)
        return kWsaENETDOWN;

    for (uint32_t i = 0; i < count; i++)
    {
        const void* xnaddr = GuestPtr<void>(xnaddrs[i].get());
        const uint32_t inAddr = XliveSession_XnAddrToInAddr(xnaddr);
        const uint64_t xuid = inAddr ? XliveSession_PeerForInAddr(inAddr) : 0;
        uint32_t rtt = 0;
        xlive::Client::PeerEndpointInfo endpoint;
        if (xuid && Live().PeerEndpoint(xuid, endpoint))
            rtt = endpoint.rtt_ms;
        // A host we cannot name is not one the title should join.
        FillQosInfo(&qos->info[i], rtt, xuid != 0);
        if (xuid == 0)
            qos->info[i].flags = kQosComplete | kQosTargetDisabled;
    }
    KLOG("[xlive] XNetQosLookup: %u target(s) answered at once\n", count);
    *qosOut = qosVa;
    SignalGuestEvent(eventHandle);
    return 0;
}

uint32_t QosServiceLookup(uint32_t eventHandle, be<uint32_t>* qosOut)
{
    if (!qosOut)
        return kWsaEINVAL;
    *qosOut = 0;
    if (!Live().online())
        return kWsaENETDOWN;
    const uint32_t qosVa = AllocateQos(1);
    auto* qos = GuestPtr<GuestQos>(qosVa);
    if (!qos)
        return kWsaENETDOWN;
    // "The service", contacted: the gateway is up, which is what the question
    // means here. sub_825969E0 reads bit 1 of the flags and nothing else.
    FillQosInfo(&qos->info[0], 0, true);
    *qosOut = qosVa;
    SignalGuestEvent(eventHandle);
    return 0;
}

uint32_t QosRelease(uint32_t qosVa)
{
    void* host = GuestPtr<void>(qosVa);
    if (!host)
        return kWsaEINVAL;
    g_heap.Free(host);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

bool XliveNet_Enabled() { return XliveSession_Enabled(); }

// ---------------------------------------------------------------------------
// The exports
// ---------------------------------------------------------------------------
//
// Every arity is the guest thunk's (ppc_recomp.111.cpp): the NetDll_* calls
// take the XNCALLER_TYPE in r3 and shift the real arguments up one, except
// inet_addr, WSAGetLastError and __WSAFDIsSet, whose thunks pass straight
// through. XNetQosLookup has thirteen, five of them on the stack, which
// guestcall.h reads from r1+0x54.

#define XLIVE_NET_GATE()                                                         \
    do {                                                                         \
        if (!XliveNet_Enabled())                                                 \
            return kUnimplemented;                                               \
    } while (0)

static uint32_t NetDll_socket_x(uint32_t caller, uint32_t family, uint32_t type, uint32_t protocol)
{
    (void)caller;
    XLIVE_NET_GATE();
    return Socket(family, type, protocol);
}

static uint32_t NetDll_closesocket_x(uint32_t caller, uint32_t handle)
{
    (void)caller;
    XLIVE_NET_GATE();
    return CloseSocket(handle);
}

static uint32_t NetDll_shutdown_x(uint32_t caller, uint32_t handle, uint32_t how)
{
    (void)caller;
    (void)how;
    XLIVE_NET_GATE();
    std::lock_guard<std::mutex> lock(g_mutex);
    return Find(handle) ? 0 : Fail(kWsaENOTSOCK);
}

static uint32_t NetDll_ioctlsocket_x(uint32_t caller, uint32_t handle, uint32_t command,
                                     be<uint32_t>* argument)
{
    (void)caller;
    XLIVE_NET_GATE();
    return IoctlSocket(handle, command, argument);
}

static uint32_t NetDll_setsockopt_x(uint32_t caller, uint32_t handle, uint32_t level,
                                    uint32_t option, const void* value, uint32_t valueLength)
{
    (void)caller;
    XLIVE_NET_GATE();
    return SetSockOpt(handle, level, option, value, valueLength);
}

static uint32_t NetDll_bind_x(uint32_t caller, uint32_t handle, const GuestSockAddrIn* name,
                              uint32_t nameLength)
{
    (void)caller;
    XLIVE_NET_GATE();
    return Bind(handle, name, nameLength);
}

static uint32_t NetDll_connect_x(uint32_t caller, uint32_t handle, const GuestSockAddrIn* name,
                                 uint32_t nameLength)
{
    (void)caller;
    XLIVE_NET_GATE();
    return Connect(handle, name, nameLength);
}

static uint32_t NetDll_listen_x(uint32_t caller, uint32_t handle, uint32_t backlog)
{
    (void)caller;
    (void)backlog;
    XLIVE_NET_GATE();
    std::lock_guard<std::mutex> lock(g_mutex);
    return Find(handle) ? Fail(kWsaEOPNOTSUPP) : Fail(kWsaENOTSOCK);
}

static uint32_t NetDll_accept_x(uint32_t caller, uint32_t handle, void* address,
                                be<int32_t>* addressLength)
{
    (void)caller;
    (void)address;
    (void)addressLength;
    XLIVE_NET_GATE();
    std::lock_guard<std::mutex> lock(g_mutex);
    return Find(handle) ? Fail(kWsaEOPNOTSUPP) : Fail(kWsaENOTSOCK);
}

static uint32_t NetDll_select_x(uint32_t caller, uint32_t nfds, GuestFdSet* readSet,
                                GuestFdSet* writeSet, GuestFdSet* exceptSet,
                                const GuestTimeval* timeout)
{
    (void)caller;
    (void)nfds;
    XLIVE_NET_GATE();
    return Select(readSet, writeSet, exceptSet, timeout);
}

static uint32_t NetDll_recv_x(uint32_t caller, uint32_t handle, void* data, uint32_t capacity,
                              uint32_t flags)
{
    (void)caller;
    (void)flags;
    XLIVE_NET_GATE();
    return Receive(handle, data, capacity, nullptr, nullptr);
}

static uint32_t NetDll_recvfrom_x(uint32_t caller, uint32_t handle, void* data,
                                  uint32_t capacity, uint32_t flags, GuestSockAddrIn* from,
                                  be<int32_t>* fromLength)
{
    (void)caller;
    (void)flags;
    XLIVE_NET_GATE();
    return Receive(handle, data, capacity, from, fromLength);
}

static uint32_t NetDll_send_x(uint32_t caller, uint32_t handle, const void* data,
                              uint32_t length, uint32_t flags)
{
    (void)caller;
    XLIVE_NET_GATE();
    return Send(handle, data, length, flags);
}

static uint32_t NetDll_sendto_x(uint32_t caller, uint32_t handle, const void* data,
                                uint32_t length, uint32_t flags, const GuestSockAddrIn* to,
                                uint32_t toLength)
{
    (void)caller;
    XLIVE_NET_GATE();
    return SendTo(handle, data, length, flags, to, toLength);
}

static uint32_t NetDll_inet_addr_x(const char* text)
{
    XLIVE_NET_GATE();
    return InetAddr(text);
}

static uint32_t NetDll_WSAGetLastError_x()
{
    XLIVE_NET_GATE();
    return t_lastError;
}

static uint32_t NetDll___WSAFDIsSet_x(uint32_t handle, const GuestFdSet* set)
{
    XLIVE_NET_GATE();
    return FdIsSet(handle, set);
}

static uint32_t NetDll_XNetXnAddrToInAddr_x(uint32_t caller, const void* xnaddr,
                                            const void* xnkid, be<uint32_t>* inAddrOut)
{
    (void)caller;
    (void)xnkid;
    XLIVE_NET_GATE();
    if (!inAddrOut)
        return kWsaEINVAL;
    const uint32_t inAddr = XliveSession_XnAddrToInAddr(xnaddr);
    *inAddrOut = inAddr;
    return inAddr ? 0 : kWsaEINVAL;
}

static uint32_t NetDll_XNetInAddrToXnAddr_x(uint32_t caller, uint32_t inAddr, void* xnaddrOut,
                                            be<uint64_t>* xnkidOut)
{
    (void)caller;
    XLIVE_NET_GATE();
    if (!XliveSession_InAddrToXnAddr(inAddr, xnaddrOut))
        return kWsaEINVAL;
    // The session the address belongs to is the one being peered; there is
    // never more than one.
    if (xnkidOut)
        *xnkidOut = Live().peering_session();
    return 0;
}

static uint32_t NetDll_XNetQosListen_x(uint32_t caller, const void* xnkid, const void* data,
                                       uint32_t dataLength, uint32_t bitsPerSec, uint32_t flags)
{
    (void)caller;
    (void)xnkid;
    (void)data;
    (void)bitsPerSec;
    XLIVE_NET_GATE();
    // The host advertising QoS data for its session. Nobody probes it — a
    // lookup here answers from the session record — so there is nothing to
    // keep; the call succeeds so the host's own setup does.
    KLOG("[xlive] XNetQosListen(flags %02X, %u bytes) accepted\n", flags, dataLength);
    return 0;
}

static uint32_t NetDll_XNetQosLookup_x(uint32_t caller, uint32_t count, const be<uint32_t>* xnaddrs,
                                       const be<uint32_t>* xnkids, const be<uint32_t>* xnkeys,
                                       uint32_t hostCount, const void* hosts,
                                       const void* serviceIds, uint32_t probes,
                                       uint32_t bitsPerSec, uint32_t flags, uint32_t eventHandle,
                                       be<uint32_t>* qosOut)
{
    (void)caller;
    (void)hosts;
    (void)serviceIds;
    (void)bitsPerSec;
    (void)flags;
    XLIVE_NET_GATE();
    return QosLookup(count, xnaddrs, xnkids, xnkeys, hostCount, probes, eventHandle, qosOut);
}

static uint32_t NetDll_XNetQosServiceLookup_x(uint32_t caller, uint32_t flags,
                                              uint32_t eventHandle, be<uint32_t>* qosOut)
{
    (void)caller;
    (void)flags;
    XLIVE_NET_GATE();
    return QosServiceLookup(eventHandle, qosOut);
}

static uint32_t NetDll_XNetQosRelease_x(uint32_t caller, uint32_t qosVa)
{
    (void)caller;
    XLIVE_NET_GATE();
    return QosRelease(qosVa);
}

GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_socket_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_closesocket_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_shutdown, NetDll_shutdown_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_ioctlsocket, NetDll_ioctlsocket_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_setsockopt_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_bind_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_connect_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_listen, NetDll_listen_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_accept, NetDll_accept_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_select_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_recv_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_recvfrom, NetDll_recvfrom_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_send_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_sendto, NetDll_sendto_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetLastError, NetDll_WSAGetLastError_x)
GUEST_FUNCTION_HOOK(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetInAddrToXnAddr, NetDll_XNetInAddrToXnAddr_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosListen, NetDll_XNetQosListen_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosLookup, NetDll_XNetQosLookup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosServiceLookup, NetDll_XNetQosServiceLookup_x)
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosRelease, NetDll_XNetQosRelease_x)

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
//
// The socket table and every call's answer with no session: the guest struct
// sizes, a socket set up the way sub_8259BCB0 sets it up, a send to nowhere,
// a receive with nothing waiting, a select that times out, the QoS shape,
// and inet_addr. On real guest memory, signed out. CZ_XLIVE_NET_TEST=1.

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

void TestSocketLifecycle()
{
    // sub_8259BCB0, step by step.
    const uint32_t s = NetDll_socket_x(1, AF_INET_, SOCK_DGRAM_, 254);
    XLIVE_EXPECT(s != kSocketError);
    GuestScratch one(4);
    *GuestPtr<be<uint32_t>>(one.va) = 1;
    XLIVE_EXPECT(NetDll_ioctlsocket_x(1, s, FIONBIO_, GuestPtr<be<uint32_t>>(one.va)) == 0);
    XLIVE_EXPECT(NetDll_setsockopt_x(1, s, 0xFFFF, 4, GuestPtr<void>(one.va), 4) == 0);
    GuestScratch name(sizeof(GuestSockAddrIn));
    auto* addr = GuestPtr<GuestSockAddrIn>(name.va);
    addr->family = AF_INET_;
    addr->port = 1005;
    XLIVE_EXPECT(NetDll_bind_x(1, s, addr, sizeof(GuestSockAddrIn)) == 0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        GuestSocket* sock = Find(s);
        XLIVE_EXPECT(sock && sock->nonblocking && sock->boundPort == 1005);
    }

    // A send to an address nobody stands for is unreachable, not swallowed;
    // one to a broadcast is swallowed, not an error.
    GuestScratch payload(16);
    addr->addr = 0xC6120001u; // 198.18.0.1, never handed out in this boot
    XLIVE_EXPECT(NetDll_sendto_x(1, s, GuestPtr<void>(payload.va), 16, 0, addr, 16) == kSocketError);
    XLIVE_EXPECT(NetDll_WSAGetLastError_x() == kWsaEHOSTUNREACH);
    addr->addr = 0xFFFFFFFFu;
    XLIVE_EXPECT(NetDll_sendto_x(1, s, GuestPtr<void>(payload.va), 16, 0, addr, 16) == 16);

    // Nothing waiting on a non-blocking socket is kWsaEWOULDBLOCK, which is
    // what sub_8259EAC0 tests for and treats as "fine".
    GuestScratch fromLength(4);
    *GuestPtr<be<int32_t>>(fromLength.va) = 16;
    XLIVE_EXPECT(NetDll_recvfrom_x(1, s, GuestPtr<void>(payload.va), 16, 0, addr,
                                   GuestPtr<be<int32_t>>(fromLength.va)) == kSocketError);
    XLIVE_EXPECT(NetDll_WSAGetLastError_x() == kWsaEWOULDBLOCK);

    // select() with a zero timeout: not readable, but writable, and the set
    // is rewritten to say which.
    GuestScratch sets(sizeof(GuestFdSet) * 2 + sizeof(GuestTimeval));
    auto* readSet = GuestPtr<GuestFdSet>(sets.va);
    auto* writeSet = GuestPtr<GuestFdSet>(sets.va + sizeof(GuestFdSet));
    auto* timeout = GuestPtr<GuestTimeval>(sets.va + 2 * sizeof(GuestFdSet));
    readSet->count = 1;
    readSet->sockets[0] = s;
    writeSet->count = 1;
    writeSet->sockets[0] = s;
    XLIVE_EXPECT(NetDll_select_x(1, 0, readSet, writeSet, nullptr, timeout) == 1);
    XLIVE_EXPECT(readSet->count.get() == 0);
    XLIVE_EXPECT(writeSet->count.get() == 1);
    XLIVE_EXPECT(NetDll___WSAFDIsSet_x(s, writeSet) == 1);
    XLIVE_EXPECT(NetDll___WSAFDIsSet_x(s, readSet) == 0);

    // A stream socket is refused, with the error the title's telemetry client
    // handles; a closed handle is not a socket.
    XLIVE_EXPECT(NetDll_socket_x(1, AF_INET_, SOCK_STREAM_, 6) == kSocketError);
    XLIVE_EXPECT(NetDll_WSAGetLastError_x() == kWsaEPROTONOSUPPORT);
    XLIVE_EXPECT(NetDll_closesocket_x(1, s) == 0);
    XLIVE_EXPECT(NetDll_closesocket_x(1, s) == kSocketError);
    XLIVE_EXPECT(NetDll_WSAGetLastError_x() == kWsaENOTSOCK);
}

void TestQosShape()
{
    XLIVE_EXPECT(offsetof(GuestQosInfo, rttMed) == 14);
    XLIVE_EXPECT(offsetof(GuestQosInfo, dataPtr) == 8);
    // Offline, a lookup is refused and hands back no block to release.
    GuestScratch out(4);
    GuestScratch addrs(4);
    XLIVE_EXPECT(NetDll_XNetQosLookup_x(1, 1, GuestPtr<be<uint32_t>>(addrs.va), nullptr, nullptr,
                                        0, nullptr, nullptr, 8, 0, 0, 0,
                                        GuestPtr<be<uint32_t>>(out.va)) == kWsaENETDOWN);
    XLIVE_EXPECT(GuestPtr<be<uint32_t>>(out.va)->get() == 0);
    XLIVE_EXPECT(NetDll_XNetQosServiceLookup_x(1, 0, 0, GuestPtr<be<uint32_t>>(out.va)) ==
                 kWsaENETDOWN);
    // The block itself, allocated and released, with its counts where
    // sub_825A2E00 reads them.
    const uint32_t qosVa = AllocateQos(3);
    XLIVE_EXPECT(qosVa != 0);
    if (qosVa)
    {
        auto* qos = GuestPtr<GuestQos>(qosVa);
        XLIVE_EXPECT(qos->count.get() == 3 && qos->pending.get() == 0);
        FillQosInfo(&qos->info[2], 42, true);
        XLIVE_EXPECT(qos->info[2].flags == (kQosComplete | kQosTargetContacted));
        XLIVE_EXPECT(qos->info[2].rttMed.get() == 42);
        XLIVE_EXPECT(NetDll_XNetQosRelease_x(1, qosVa) == 0);
    }
    XLIVE_EXPECT(NetDll_XNetQosListen_x(1, nullptr, nullptr, 0, 0, 0x10) == 0);
}

void TestAddresses()
{
    GuestScratch text(32);
    std::strcpy(static_cast<char*>(text.host), "198.18.0.7");
    XLIVE_EXPECT(NetDll_inet_addr_x(GuestPtr<char>(text.va)) == 0xC6120007u);
    std::strcpy(static_cast<char*>(text.host), "not an address");
    XLIVE_EXPECT(NetDll_inet_addr_x(GuestPtr<char>(text.va)) == 0xFFFFFFFFu);

    // An address nobody was given is not translated to an XNADDR.
    GuestScratch xnaddr(0x24 + 8);
    XLIVE_EXPECT(NetDll_XNetInAddrToXnAddr_x(1, 0xC6120001u, GuestPtr<void>(xnaddr.va),
                                             GuestPtr<be<uint64_t>>(xnaddr.va + 0x24)) == kWsaEINVAL);
    // And a peer that is named gets one, whose translation back is itself.
    const uint32_t mine = XliveSession_InAddrForPeer(0x0009000000000123ull);
    XLIVE_EXPECT((mine & 0xFFFE0000u) == 0xC6120000u);
    XLIVE_EXPECT(NetDll_XNetInAddrToXnAddr_x(1, mine, GuestPtr<void>(xnaddr.va),
                                             GuestPtr<be<uint64_t>>(xnaddr.va + 0x24)) == 0);
    GuestScratch back(4);
    XLIVE_EXPECT(NetDll_XNetXnAddrToInAddr_x(1, GuestPtr<void>(xnaddr.va), nullptr,
                                             GuestPtr<be<uint32_t>>(back.va)) == 0);
    XLIVE_EXPECT(GuestPtr<be<uint32_t>>(back.va)->get() == mine);
}

}  // namespace

void XliveNet_SelfTest()
{
    const char* on = std::getenv("CZ_XLIVE_NET_TEST");
    if (!on || on[0] != '1')
        return;
    if (!XliveNet_Enabled())
    {
        fprintf(stderr, "[xlive] net self-test skipped: CZ_XLIVE_COOP is not set\n");
        return;
    }
    if (Live().online())
    {
        fprintf(stderr, "[xlive] net self-test skipped: signed in to Live\n");
        return;
    }
    g_selfTestFailures = 0;
    TestSocketLifecycle();
    TestQosShape();
    TestAddresses();
    if (g_selfTestFailures == 0)
        fprintf(stderr, "[xlive] net self-test: the socket family answers\n");
    else
        fprintf(stderr, "[xlive] net self-test: %d FAILURE(S)\n", g_selfTestFailures);
}
