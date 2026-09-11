// The socket family — NetDll_socket .. sendto/recvfrom/select — over the
// path the punch opened, plus the XNet QoS calls the title's matchmaking
// needs before it will join anything.
//
// WHAT A SOCKET IS HERE. Not a socket. The title's UDP socket (sub_8259BCB0:
// AF_INET, SOCK_DGRAM, IPPROTO_VDP, FIONBIO, SO_REUSEADDR, bound to 1005) is
// a table entry, and every datagram it sends or receives goes through
// libxlive's one real socket — the one the punch opened, which is the ONLY
// socket a NAT has a hole in front of (XenonLive/client/src/punch.h). The
// address the title sends to is one of the 198.18/15 stand-ins XSESSION_INFO
// handed it; xlive_session.cpp maps it back to the peer's XUID and the bytes
// go down the punched path with the session's framing around them. No real
// address ever reaches guest memory.
//
// THE ERRORS ARE WINSOCK'S, because the title's own code tests them
// (sub_8259EF18: 10035 is "try again", 10065 is "log it", anything else
// drops the peer). A peer that is still being punched answers WSAEWOULDBLOCK
// — transient, keep going — and one that failed to punch or was never in the
// session answers WSAEHOSTUNREACH.
//
// QoS IS WHAT MAKES QUICKMATCH JOIN. sub_825A2E00 runs XNetQosLookup over
// every search result and joins only among the ones whose XNQOSINFO reads
// COMPLETE, ordered by wRttMedInMsecs. Without an answer the title has
// nothing to join. The answer here is honest as far as it can be: a host we
// have a punched path to reports the RTT the punch measured; one we do not
// — every search result, before a join — reports contacted with no
// measurement, because there is no probe to send before there is a session.
//
// TCP IS REFUSED. socket(SOCK_STREAM) fails with WSAEPROTONOSUPPORT: the
// title's stream sockets are a telemetry client (sub_8258CBC0, behind
// XNetDnsLookup) and a listener nothing here can serve, and a refusal the
// title's error paths already handle beats a handle that goes nowhere.
//
// ON WITH CO-OP. CZ_XLIVE_COOP=1 — the sockets carry a session's traffic and
// there is no session without it. Off, every export answers exactly as the
// generated stub did, so a boot without the switch is byte-for-byte what it
// was.
#pragma once

#include <cstdint>

#include <xbox.h> // be<>

bool XliveNet_Enabled();

// A boot-time self-test: the guest struct sizes, the socket table, every
// call's offline answer and the QoS shape, on real guest memory with no
// session. Off unless CZ_XLIVE_NET_TEST=1.
void XliveNet_SelfTest();
