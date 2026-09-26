// A side channel for the co-op layer, over the path the title's own datagrams
// already take.
//
// WHY THIS EXISTS
// ---------------
// Player issue #9. Case Zero's co-op layer replicates no item and no inventory:
// the engine's whole named network vocabulary has no entry for one, and none of
// the eleven broadcast-event subtypes carries one either (docs/coop-plan.md,
// "The broadcast-event wire, decoded"). So each machine answers "what is this
// player holding" out of a copy it maintains alone. The pool index behind that
// copy is handed out by a LIFO free list private to each machine
// (`sub_8223B000`, "WHO ALLOCATES A POOL ENTRY"), so the two copies drift one
// way and never re-converge.
//
// Repairing that needs one fact to cross the link that the title never sends.
// There is nowhere in the title's own wire to put it, and inventing an event
// subtype means writing into a serializer the title owns on both sides.
//
// The cheap alternative is that WE own the transport. Every guest datagram is
// already framed with a source and a destination GUEST PORT in front of the
// payload and carried over libxlive's one punched socket (kernel/xlive_net.cpp:
// "THE PORTS TRAVEL WITH THE BYTES"). A datagram addressed to a port no guest
// socket has bound is dropped today, and the drop is already a log line. So a
// reserved port is a whole channel for the cost of one branch on the receive
// path: same socket, same punched path, same NAT hole, nothing new to connect
// or tear down, and the title cannot see it. If the title ever binds this port
// the receive path says so and the channel stands down rather than eating the
// title's packets.
//
// UNRELIABLE AND UNORDERED, ON PURPOSE. This carries STATE, not events — the
// current value of a field, resent every tick — so a lost datagram costs one
// period of freshness and nothing else, and a reordered one is rejected by its
// sequence number. **Nothing here may carry a message whose loss matters.** If
// something ever has to, it needs a retransmit and an acknowledgement, and this
// paragraph is where to say that it grew them.
#pragma once

#include <cstddef>
#include <cstdint>

// The reserved destination port. Chosen above every ephemeral range and far
// from the two ports Case Zero actually binds (1005, and the second socket the
// host opens on accept), so a collision would have to be deliberate.
constexpr uint16_t kCoopLinkPort = 0xCF01;

// Sends one datagram to every peer with a live path. Returns how many it went
// to — 0 when there is no session, which is the ordinary offline answer and not
// an error. Never blocks and never queues: a peer that is still being punched
// simply misses this tick.
int CoopLink_Broadcast(const void* data, size_t length);

// Called by the socket layer for every datagram addressed to kCoopLinkPort,
// instead of dropping it. Runs on whichever guest thread was draining the path,
// under the socket layer's own lock, so it must not call back into it.
void CoopLink_Deliver(uint64_t fromXuid, const void* data, size_t length);

// A boot-time self-test of the held-item contract: the part/event tables, the
// message encoding, and every rule the receive half enforces (version, runt,
// sequence order, player bound, the same-side refusal). It runs with no session
// and no second machine, which is the point — the fix's receive half and its
// guards would otherwise be code that has never executed until two people sit
// down to play, and a guard that has never fired has not been shown capable of
// firing. Off unless CZ_COOP_ITEM_SYNC_TEST=1.
void CoopItems_SyncSelfTest();
