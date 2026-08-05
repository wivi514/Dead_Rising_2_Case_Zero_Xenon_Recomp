#!/usr/bin/env python3
"""Check our PM4 packet-length rule against the boundaries hardware itself used.

WHY THIS EXISTS
---------------
A command processor decides how many dwords each packet occupies purely from the
packet's own header. Get that arithmetic wrong for ONE packet type and the walk
desyncs: every dword after it is read as a header, the invented lengths eventually
run past the end of the buffer, and the walk stops — silently, in the middle of a
command list. Nothing is malformed, nothing is unknown, no counter looks wrong.

Findings 38-39 are what that costs. Our walks of the title's indirect buffers ended
early, dropping every packet after the stop — including the driver's own ring-progress
fence, which is the LAST packet in those buffers. The renderer then waited forever for
a counter only that packet advances, and a third to a half of boots never reached the
title screen.

This tool exists because that hunt began by blaming the length rule, and it took two
wrong answers to stop doing so. Xenia's `.xtr` records a `PacketStart {base_ptr, count}`
for every packet it executed, and `count` is that packet's true length in dwords — the
boundary real hardware used, for this exact title, inside indirect buffers included. So
the rule can be checked against ground truth instead of against our reading of a
specification, and it was already right on all 24,527,474 of them. The real cause was
upstream of any parsing: our VdSwap left 52 dwords of its reservation unwritten, so the
parser was handed the previous frame's packets and desynced on correct arithmetic
(finding 39).

Keep both halves of that in view. This tool answers "is our length rule right?" — it
cannot answer "are these the bytes hardware saw?", and a clean result here is not a
clean bill of health for the command processor. Its companion
`tools/pm4_indirect_walks.py` checks the other half of a walk (start address and every
internal boundary, chained); neither covers the bytes.

Exit 1 if any opcode disagrees.

Usage:
    python3 tools/pm4_packet_lengths.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
"""
import argparse
import collections
import sys

import xtr


def predicted_length(header):
    """Dwords this packet occupies, by the rule gpu/pm4.cpp uses.

    Keep this function a mirror of gpu/pm4.cpp or the gate measures nothing.

    RETRACTION, and it is worth reading before trusting anything about zero dwords.
    An earlier version of this docstring said B1 contains exactly one zero-header
    packet and that hardware consumed it as TWO dwords — an ordinary type-0 "write one
    register at index 0" — and cited that as proof that a zero must not be treated as a
    one-dword no-op. The observation was real and the conclusion was wrong.

    That packet is an INDIRECT_BUFFER whose header dword the trace records as zero. It
    has every marker of one and none of a register write: `PacketStart.count` is 2 (the
    short recording every 0x3F gets, PM4_SHORT_RECORDED), its word[1] is a plausible
    command-buffer address, and the very next trace command is `IndirectBufferStart`
    with `base_ptr` equal to that word — the exact mechanism documented in xtr.py and
    measured across all 28,726 indirect buffers. It sits at guest address 03D71FFC, the
    last dword of a 4 KB block; why the header records as zero there is not established
    and does not need to be, because the classification does not depend on it.

    So this tool now classifies it by that mechanism rather than by its header bits,
    and the honest summary is: **B1 contains no genuine zero-header packet at all.** The
    capture is SILENT on what hardware does with a zero dword. It cannot support the
    two-dword reading and it cannot support the one-dword reading.

    That silence turned out to be the point. The zeros our parser kept tripping over
    were never hardware's — they were the unwritten tail of a VdSwap reservation our
    kernel left as whatever the heap held (finding 39). Fill the tail with the 0x80000000
    no-ops hardware puts there and the question stops arising.
    """
    packet_type = header >> 30
    if packet_type == 2:
        return 1                      # type 2 is a one-dword no-op
    if packet_type == 1:
        return 3                      # header + two register writes
    return ((header >> 16) & 0x3FFF) + 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stream", help="a Xenia .xtr GPU stream")
    ap.add_argument("--examples", type=int, default=3,
                    help="disagreeing packets to print per opcode (default 3)")
    args = ap.parse_args()

    data, header = xtr.open_trace(args.stream)
    n = header["size"]

    checked = 0
    agree = collections.Counter()
    disagree = collections.Counter()
    examples = collections.defaultdict(list)

    # A packet is only classified once the FOLLOWING trace command is known, because
    # an INDIRECT_BUFFER is identified by what comes after it (an IndirectBufferStart
    # at the address in its word[1]) and not only by its header bits. Classifying on
    # the header alone is what let a mis-recorded zero header pass for a register
    # write — see predicted_length's retraction.
    pending = None          # (header, recorded, word1) awaiting the next command

    def classify(header, recorded, is_indirect):
        nonlocal checked
        opcode = (header >> 8) & 0x7F if (header >> 30) == 3 else -1
        if is_indirect:
            opcode = 0x3F
        # INDIRECT_BUFFER is stored one dword short of its own header's claim; that
        # is a property of the RECORDING, established in docs/xtr-decoder.md, not a
        # disagreement about packet length.
        recorded += xtr.PM4_SHORT_RECORDED.get(opcode, 0)
        # An INDIRECT_BUFFER is header + address + size however its header records, so
        # the one whose header the trace lost is still 3 dwords. Written as an explicit
        # override rather than folded into predicted_length() because that function must
        # stay a literal mirror of gpu/pm4.cpp, which has no such case and needs none:
        # a correctly aligned walk reads the real header out of guest memory.
        ours = 3 if is_indirect and header == 0 else predicted_length(header)
        key = (header >> 30, opcode)
        checked += 1
        if recorded == ours:
            agree[key] += 1
        else:
            disagree[key] += 1
            if len(examples[key]) < args.examples:
                examples[key].append((header, recorded, ours))

    for off, cmd in xtr.walk(data, n):
        if pending is not None:
            packet_off, header, recorded = pending
            pending = None
            is_indirect = False
            if cmd == xtr.CMD_INDIRECT_BUFFER_START:
                # Confirm rather than assume: the packet's own word[1] must be the
                # address the trace then walks into. Decoded only on this branch —
                # 28,726 times rather than 24.5 M — because unpacking every packet's
                # body to look at one dword triples the tool's runtime.
                base_ptr = xtr._U32.unpack_from(data, off + 4)[0]
                _, words = xtr.packet_words(data, packet_off)
                is_indirect = len(words) > 1 and words[1] == base_ptr
            classify(header, recorded, is_indirect)
        if cmd != xtr.CMD_PACKET_START:
            continue
        header = xtr.packet_header_word(data, off)
        if header is None:
            continue                  # zero-length PacketStart: nothing to check
        pending = (off, header, xtr.packet_word_count(data, off))
    if pending is not None:
        classify(pending[1], pending[2], False)

    print(f"{args.stream}")
    print(f"  packets checked      : {checked:,}")
    print(f"  lengths agreeing     : {sum(agree.values()):,}")
    print(f"  lengths DISAGREEING  : {sum(disagree.values()):,}")

    if not disagree:
        print("\nOK: every packet's recorded length matches the rule in gpu/pm4.cpp.")
        return 0

    print("\nOpcodes whose length we compute wrongly:")
    for key in sorted(disagree, key=lambda k: -disagree[k]):
        packet_type, opcode = key
        # Only type 3 has an opcode; -1 is this script's "not applicable", and
        # printing it as one produced a first draft that said "op -1".
        what = (f"type {packet_type} op {opcode:02X} {xtr.pm4_name(opcode)}"
                if packet_type == 3 else f"type {packet_type}")
        print(f"  {what:<34} {disagree[key]:,} wrong / {agree[key]:,} right")
        for header, recorded, ours in examples[key]:
            print(f"      header {header:08X}: hardware used {recorded}, we use {ours}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
