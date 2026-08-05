#!/usr/bin/env python3
"""Check our PM4 packet-length rule against the boundaries hardware itself used.

WHY THIS EXISTS
---------------
A command processor decides how many dwords each packet occupies purely from the
packet's own header. Get that arithmetic wrong for ONE packet type and the walk
desyncs: every dword after it is read as a header, the invented lengths eventually
run past the end of the buffer, and the walk stops — silently, in the middle of a
command list. Nothing is malformed, nothing is unknown, no counter looks wrong.

Finding 38 is what that costs. Our parser read a zero dword as a two-dword type-0
packet instead of a one-dword no-op, drifted, and stopped one packet short of the end
of indirect buffers whose LAST packet is the driver's ring-progress fence. The
renderer then waited forever for a counter only that packet advances, and a third to a
half of boots never reached the title screen.

The fix for that one was found by hand. This tool exists so the next one is not:
Xenia's `.xtr` trace records a `PacketStart {base_ptr, count}` for every packet it
executed, and `count` is that packet's true length in dwords — the boundary real
hardware used, for this exact title, including inside indirect buffers. So the length
rule can be checked against ground truth rather than against our own reading of a
specification.

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

    Note what is deliberately absent: a special case for a zero header. It reads like
    padding and every instinct says it should consume one dword, but this check is the
    thing that says otherwise — B1 contains exactly one zero-header packet in 24.5 M
    and hardware consumed it as TWO, i.e. as an ordinary type-0 "write one register at
    index 0". Keep this function a mirror of gpu/pm4.cpp or the gate measures nothing.
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

    for off, cmd in xtr.walk(data, n):
        if cmd != xtr.CMD_PACKET_START:
            continue
        header = xtr.packet_header_word(data, off)
        if header is None:
            continue                  # zero-length PacketStart: nothing to check
        recorded = xtr.packet_word_count(data, off)
        opcode = (header >> 8) & 0x7F if (header >> 30) == 3 else -1
        # INDIRECT_BUFFER is stored one dword short of its own header's claim; that
        # is a property of the RECORDING, established in docs/xtr-decoder.md, not a
        # disagreement about packet length.
        recorded += xtr.PM4_SHORT_RECORDED.get(opcode, 0)
        key = (header >> 30, opcode)
        checked += 1
        if recorded == predicted_length(header):
            agree[key] += 1
        else:
            disagree[key] += 1
            if len(examples[key]) < args.examples:
                examples[key].append((header, recorded, predicted_length(header)))

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
