// The XLiveBase surface — friends, invites, mutes, and the two logon questions.
//
// Split out of imports.cpp for the reason xlive_session.cpp was: it is one
// mechanism with a page of derivation behind it, and the derivation is the
// part worth keeping. XenonLive/proto/xgi_messages.md, "XLiveBase does not
// take a buffer length at all", is that page; this file is its implementation.
//
// WHAT THE TITLE ACTUALLY SENDS. Every XLiveBase message arrives through
// XMsgInProcessCall — no XOVERLAPPED, no permission to wait — and the fourth
// parameter is NOT a length. For the two argument-list messages it is a
// pointer to the marshalled list the guest builds with sub_82605878; for the
// mute query it is the caller's BOOL*; for the logon id and the NAT type it
// is zero. So every handler here answers from memory that is already there,
// which is exactly the shape libxlive's cache reads have: friends(),
// mute_list() and AcceptedInvite() never block, and that is not a nicety,
// it is the only shape these messages accept.
//
// THE ANSWERS ARE TRUTHFUL, INCLUDING THE FAILURES. Signed out, or with no
// gateway, XOnlineGetLogonID fails the way it does on a console with no Live
// connection, and the guest then never sends the messages that follow it —
// its own wrappers stop at the first failure. Nothing here says "online" on
// the library's behalf.
//
// ALWAYS ON. There is no separate switch: nothing here runs until the title
// believes it is signed in to Live (kernel/imports.cpp, XamUserGetSigninState),
// and that belief is what has the switch.
#pragma once

#include <cstdint>

#include <xbox.h> // be<>

// Handles one XLiveBase (app 0xFC) message. Returns false when the message is
// not one of ours, in which case *result is untouched and the caller falls
// through to its own E_FAIL path.
//
// buffer is the host pointer the hook already translated; argumentsVa is the
// raw fourth parameter, which each message interprets differently (see above).
bool XliveSocial_Dispatch(uint32_t message, void* buffer, uint32_t argumentsVa,
                          uint32_t* result);

// The XamEnumerate seam. The friends enumerator is a kernel object like the
// content enumerator, and the title drains it through the imported
// XamEnumerate with an overlapped — a call path content.cpp's handler had
// never seen. Returns false when the handle is not a friends enumerator.
bool XliveSocial_Enumerate(uint32_t handle, uint32_t buffer, uint32_t bufferLength,
                           be<uint32_t>* itemsReturned, uint32_t overlappedVa,
                           uint32_t* result);

// -- events from libxlive, forwarded by xlive_glue.cpp ---------------------

// The friends list changed. Diffs against the last list it saw and posts the
// notification the title's own listener compares against: FRIEND_ADDED,
// FRIEND_REMOVED, or PRESENCE_CHANGED when the membership is the same.
void XliveSocial_OnFriendsChanged();

// An invitation arrived. The title's own state machine wants
// XN_LIVE_INVITE_ACCEPTED and then asks XInviteGetAcceptedInfo, whose answer
// includes the session's key and host address — which the invite alone does
// not carry. So this asks the session layer for the details first and posts
// the notification only once they are in memory, because the handler is not
// allowed to wait for them.
//
// Two events arrive here, and which one says who answered the invitation:
//   InviteReceived — no launcher was connected, so nobody could ask the
//                    player; the invitation is taken on their behalf and the
//                    server is told (`accept` = true).
//   InviteAccepted — the player already said yes, in the launcher; the server
//                    knows, and this is only the title's cue (`accept` = false).
void XliveSocial_OnInviteReceived(uint64_t inviteId, uint64_t fromXuid, uint32_t titleId,
                                  uint64_t sessionId, bool accept);

// The session layer's answer to the request above.
void XliveSocial_OnInviteSessionReady(uint64_t sessionId, bool ok);

// The push connection came up or went down. XN_LIVE_CONNECTIONCHANGED, with
// the guest's own HRESULT for the state as the parameter.
void XliveSocial_OnConnectionChanged(bool online);

// A boot-time self-test of the guest ABI, the twin of XliveSession_SelfTest:
// argument-list decoding, XONLINE_FRIEND offsets, the enumerate protocol and
// every message's offline answer, on real guest memory with no server. Off
// unless CZ_XLIVE_SOCIAL_TEST=1.
void XliveSocial_SelfTest();

// -- the notification ids, read off the guest --------------------------------
//
// sub_8259DC38 is the title's notification poll for the listener it created
// with mask 3 (SYSTEM | LIVE). Its switch subtracts 0x02000000 from the id and
// dispatches on the remainder: 2 goes to sub_8259A7F8, which is the function
// that calls XInviteGetAcceptedInfo. So XN_LIVE_INVITE_ACCEPTED is 0x02000002
// in this title, and the area is the id's bits 25 and up — XNID(version,
// area, index) = (version << 16) | (area << 25) | index, the encoding Xenia
// also uses. The listener masks A1 shows (1, 3, 4, 5, 0x20) are one bit per
// area under exactly that shift: 0x20 is area 5, the media player, and its
// listener is the one sub_827F6D98 polls for 0x0A000001.
constexpr uint32_t XN_SYS_SIGNINCHANGED       = 0x0000000A; // sub_825E4380 tests 10
constexpr uint32_t XN_LIVE_CONNECTIONCHANGED  = 0x02000001;
constexpr uint32_t XN_LIVE_INVITE_ACCEPTED    = 0x02000002;
constexpr uint32_t XN_FRIENDS_PRESENCE_CHANGED = 0x04000001;
constexpr uint32_t XN_FRIENDS_FRIEND_ADDED     = 0x04000002; // sub_825F8EB8 tests it
constexpr uint32_t XN_FRIENDS_FRIEND_REMOVED   = 0x04000003; // and this

// Defined in imports.cpp: the seam into the title's notification queues. False
// when no listener for the id's area exists yet, in which case nothing heard it.
bool PostGuestNotification(uint32_t id, uint32_t param);
