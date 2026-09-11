// The XGI session surface — co-op, as the title asks for it.
//
// Split out of imports.cpp for the same reason content.cpp is: it is one
// mechanism with a page of derivation behind it, and the derivation is the
// valuable part. imports.cpp keeps one call into here.
//
// WHAT THE TITLE ACTUALLY SENDS. Every one of these arrives through
// XMsgStartIORequest with an XOVERLAPPED — confirmed by reading the recompiled
// wrappers, all of which route through sub_825ACC08, not through
// XMsgInProcessCall. So none of them has to be answered synchronously, and none
// of them is: the overlapped is left reading ERROR_IO_PENDING, the request goes
// to the server on libxlive's worker, and a thread here completes the
// overlapped when the answer lands. No guest thread ever waits on a network.
//
// THE ADDRESSES ARE OURS. XSESSION_INFO carries an XNADDR, which on hardware
// was a real routable address plus a security-gateway record. We synthesize
// one: the peer's XUID goes in the field that on the console held the machine
// account id, because that is what it is, and the IPv4 fields carry an address
// out of 198.18.0.0/15 — the IANA benchmarking block, which is never routed and
// exists precisely so a system can use it as a stand-in. The title translates
// that with XNetXnAddrToInAddr and sends to it, and the socket layer maps it
// back to the peer it belongs to. Nothing here ever hands guest code a real IP.
//
// OFF BY DEFAULT. CZ_XLIVE_COOP=1 turns it on. Without it every message below
// is unhandled and returns E_FAIL exactly as it does today, which is what keeps
// the shipping build's behaviour identical while this is unexercised against
// the title's own co-op menu.
#pragma once

#include <cstdint>

// Starts the bridge: the completion thread and the peer-address table.
// Called from CzXlive_Start, and a no-op unless CZ_XLIVE_COOP=1.
void XliveSession_Start();
void XliveSession_Shutdown();

// True when the bridge is running, which is what makes the XGI session
// messages handled rather than E_FAIL.
bool XliveSession_Enabled();

// Records a context the title set, so XSessionCreate can advertise it. The
// console carried these into the session implicitly; the create message does
// not contain them, which is why they have to be remembered here.
void XliveSession_SetContext(uint32_t contextId, uint32_t value);

// Handles one XGI session message. Returns false when the message is not one
// of ours, in which case *result is untouched and the caller falls through to
// its own E_FAIL path.
//
// overlappedVa is the guest address of the XOVERLAPPED, or 0 for a caller that
// wants a synchronous answer. Every call site in this title supplies one.
bool XliveSession_Dispatch(uint32_t message, void* buffer, uint32_t bufferLength,
                           uint32_t overlappedVa, uint32_t* result);

// -- invitations -------------------------------------------------------------
//
// An invitation names a session by XNKID, but the X_INVITE_INFO the title asks
// for carries the whole XSESSION_INFO — key and host address included — and
// XInviteGetAcceptedInfo arrives with no overlapped, so the details have to be
// in memory before the title is told there is an invitation at all.

// Asks the server for the session behind an invitation, on the completion
// thread. Answers through XliveSocial_OnInviteSessionReady. Returns false when
// co-op is off, because then there is nothing to fetch it on and nothing to
// join with.
bool XliveSession_PrefetchInviteSession(uint64_t sessionId);

// Fills 0x3C bytes of guest memory with the XSESSION_INFO of a session the
// prefetch above brought in. False when it never arrived.
bool XliveSession_InviteSessionInfo(uint64_t sessionId, void* sessionInfoOut);

// Collects a social ticket nobody else will: the answer is logged and dropped.
// For the calls a game makes without needing the result — accepting an
// invitation on the player's behalf — where Forget() would cancel a request
// that has not left the queue yet.
void XliveSession_DrainSocialTicket(uint64_t ticket);

// -- the virtual XNet ------------------------------------------------------
//
// The title translates an XNADDR to an IN_ADDR and then uses ordinary sockets.
// These are the two halves of that translation, and they are the only place
// the fake address block is interpreted.

// Maps a synthesized XNADDR (0x24 bytes of guest memory) to the private IN_ADDR
// that stands for that peer. Returns 0 when the address is not one of ours.
uint32_t XliveSession_XnAddrToInAddr(const void* xnaddr);

// The reverse: fills 0x24 bytes of guest memory. Returns false when the address
// is not one we handed out.
bool XliveSession_InAddrToXnAddr(uint32_t inAddr, void* xnaddrOut);

// The peer behind a private address, or 0. The socket layer uses this to route
// a send to the punched path.
uint64_t XliveSession_PeerForInAddr(uint32_t inAddr);

// The reverse, allocating: the private address that stands for a peer, minted
// on first sight. The socket layer uses this to name the sender of a packet
// that arrived before the title ever saw that peer's XNADDR.
uint32_t XliveSession_InAddrForPeer(uint64_t xuid);

// Fills the local machine's XNADDR — what NetDll_XNetGetTitleXnAddr answers
// with once there is an account. Returns false when there is no identity yet,
// and the caller must then keep reporting XNET_GET_XNADDR_NONE rather than
// inventing one.
bool XliveSession_LocalXnAddr(void* xnaddrOut);

// A boot-time self-test of the guest ABI: builds each session message in guest
// memory with known values, pushes it through the decoder, and checks what came
// out — the same idea as FileImportsWriteSelfTest, and for the same reason.
// It runs without a server and without a second player, so the half of co-op
// that is guest-struct arithmetic is exercised on every developer boot rather
// than only when two people sit down to play. Off unless CZ_XLIVE_COOP_TEST=1.
void XliveSession_SelfTest();
