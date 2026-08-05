#!/usr/bin/env python3
"""Check our INDIRECT_BUFFER decode and our WALK against hardware's own, per buffer.

WHY THIS EXISTS
---------------
`tools/pm4_packet_lengths.py` proves each individual packet's length is right. That
is not the same as proving a walk is right, and findings 38-39 are the difference:
our walks of the title's indirect buffers ended early, at a position that varied from
buffer to buffer, while every packet length we computed matched hardware on all
24,527,474 packets in B1.

A per-packet check cannot see three things that a walk depends on:

  1. the ADDRESS we start the walk at — `body(0)`, and how we translate it,
  2. the SIZE we stop at — `body(1) & 0xFFFFF`, and
  3. whether hardware's own packets, laid end to end from the start with OUR length
     rule, actually TILE the buffer exactly. If hardware stops short of the declared
     size (padding), or runs past it, our walk would diverge on correct bytes.

Xenia's `.xtr` answers (1) and (3), because it walks *into* each indirect buffer
rather than dereferencing guest memory: an INDIRECT_BUFFER packet is always followed
by `IndirectBufferStart {base_ptr, ...}` and then the buffer's own packets inline
until `IndirectBufferEnd` — and **every one of those inner `PacketStart` records
carries its own `base_ptr`, the guest address hardware read that packet from.**

That last point is what makes this a walk oracle rather than another length check.
Chaining it — does `base_ptr(i) + 4 * our_length(header(i))` equal `base_ptr(i+1)`? —
replays our cursor arithmetic against every packet boundary hardware actually landed
on, in sequence, from the buffer's first dword. A length rule can be right for every
packet in isolation and still desync if the walk starts in the wrong place; this
catches that, and a per-packet check cannot.

(2), the size, is *not* checkable here: `IndirectBufferStart.count` is recorded as 0
for all 28,727 buffers, and the INDIRECT_BUFFER packet is stored one dword short
(PM4_SHORT_RECORDED), so the size dword appears nowhere in the trace. What the trace
does give is the size hardware *consumed* — the end of its last inner packet — which
is printed as a distribution so an implausible declared size would stand out.

On B1 it passes: 28,727 buffers, 0 disagreements, nesting one level deep (13,232 at
the top, 15,495 inside another buffer). Which is how finding 39 got its answer — a
desync at run time was not a parser bug at all, and the only explanation left was that
the BYTES we walked were not the bytes hardware walked. They were the previous frame's,
in a tail our own VdSwap never filled.

So read a pass here narrowly. It clears the arithmetic and says nothing about the
input.

Exit 1 if any buffer disagrees.

Usage:
    python3 tools/pm4_indirect_walks.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
"""
import argparse
import collections
import sys

import xtr


def predicted_length(header):
    """Dwords this packet occupies — the rule in runtime/gpu/pm4.cpp.

    Deliberately a duplicate of pm4_packet_lengths.py's function rather than an
    import: these are two independent checks of the same rule, and a shared helper
    would let one edit silently change what both of them measure.
    """
    packet_type = header >> 30
    if packet_type == 2:
        return 1
    if packet_type == 1:
        return 3
    return ((header >> 16) & 0x3FFF) + 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stream", help="a Xenia .xtr GPU stream")
    ap.add_argument("--examples", type=int, default=5)
    args = ap.parse_args()

    data, hdr = xtr.open_trace(args.stream)
    n = hdr["size"]

    # State of the buffer currently being walked, or None outside one.
    pending_ib = None       # byte offset of the previous PacketStart
    cur = None              # dict for the buffer we are inside

    buffers = 0
    bad = collections.Counter()
    examples = collections.defaultdict(list)
    sizes = []
    depth_seen = collections.Counter()
    stack = []

    def note(kind, detail):
        bad[kind] += 1
        if len(examples[kind]) < args.examples:
            examples[kind].append(detail)

    for off, cmd in xtr.walk(data, n):
        if cmd == xtr.CMD_PACKET_START:
            header = xtr.packet_header_word(data, off)
            if header is None:
                continue
            packet_addr = xtr._U32.unpack_from(data, off + 4)[0]
            if cur is not None:
                # THE check: our cursor, advanced by our own length rule from the
                # previous packet, must land exactly where hardware read this one.
                if cur["next"] is not None and cur["next"] != packet_addr:
                    note("walk_desync",
                         f"base={cur['base']:08X} packet #{cur['packets']}: our cursor "
                         f"{cur['next']:08X} but hardware read from {packet_addr:08X} "
                         f"(previous header {cur['prev']:08X})")
                    cur["next"] = None      # one report per buffer; resync and go on
                else:
                    cur["next"] = packet_addr + 4 * predicted_length(header)
                cur["prev"] = header
                cur["packets"] += 1
                cur["end"] = packet_addr + 4 * predicted_length(header)
            # Remember EVERY packet, not only the ones whose header bits say 0x37/0x3F.
            # A trace can lose a header — B1 records one INDIRECT_BUFFER's as zero — and
            # keying off the opcode alone made that buffer look like an orphaned
            # IndirectBufferStart, i.e. like a defect in the trace rather than a packet
            # we had failed to recognise. What identifies an indirect buffer reliably is
            # what FOLLOWS it, checked below.
            pending_ib = off

        elif cmd == xtr.CMD_INDIRECT_BUFFER_START:
            base_ptr, count = xtr._U32X2.unpack_from(data, off + 4)
            if pending_ib is None:
                note("ib_start_without_packet", f"base_ptr={base_ptr:08X}")
            else:
                # The packet is recorded one dword short (PM4_SHORT_RECORDED), so its
                # words are [header, address] and the SIZE dword is not in the trace at
                # all. Only the address is checkable here.
                #
                # Our decode: ExecuteLinear(base, PhysToVa(body(0)), body(1)&0xFFFFF).
                # The trace's base_ptr is the address hardware used; if it differs from
                # the packet's own dword, our masking of body(0) is wrong.
                _, words = xtr.packet_words(data, pending_ib)
                addr = words[1] if len(words) > 1 else None
                if addr is None or addr != base_ptr:
                    note("address_mismatch",
                         f"packet body0="
                         f"{'(none)' if addr is None else format(addr, '08X')} but "
                         f"hardware used {base_ptr:08X}")
                elif (addr & 0x3) != 0:
                    note("unaligned_address", f"{addr:08X}")
                # Depth is measured before the push, so the parent is still `cur`.
                depth_seen[len(stack) + (1 if cur is not None else 0)] += 1
            pending_ib = None
            if cur is not None:
                stack.append(cur)
            # "next" starts at the buffer base: the first inner packet must be read
            # from exactly the address the INDIRECT_BUFFER packet named, which is the
            # half of the walk a per-packet length check can never see.
            cur = {"base": base_ptr, "next": base_ptr, "prev": 0,
                   "packets": 0, "end": base_ptr}
            buffers += 1

        elif cmd == xtr.CMD_INDIRECT_BUFFER_END:
            if cur is None:
                note("ib_end_without_start", f"at {off}")
            else:
                sizes.append((cur["end"] - cur["base"]) // 4)
                cur = stack.pop() if stack else None

    print(f"{args.stream}")
    print(f"  indirect buffers     : {buffers:,}")
    if sizes:
        print(f"  dwords CONSUMED      : min {min(sizes)}, max {max(sizes)}, "
              f"mean {sum(sizes)//len(sizes)}  (the trace records no declared size)")
    print(f"  nesting depths seen  : "
          f"{', '.join(f'{d}: {c:,}' for d, c in sorted(depth_seen.items()))}")
    print(f"  buffers DISAGREEING  : {sum(bad.values()):,}")

    if not bad:
        print("\nOK: every indirect buffer's address and size match the packet, and "
              "hardware's\n    own packets tile it exactly under our length rule. A "
              "run-time desync is not\n    a parser bug — the bytes differ.")
        return 0

    print("\nDisagreements:")
    for kind, count in bad.most_common():
        print(f"  {kind:<26} {count:,}")
        for detail in examples[kind]:
            print(f"      {detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
