#!/usr/bin/env python3
"""Census the PM4 packets inside a Xenia `.xtr` GPU trace.

WHY THIS EXISTS
---------------
`xtr_walk.py stats` counts trace *commands* — how Xenia framed the capture.
This counts what the guest actually told the GPU to do. That is the number that
sizes phase 4: which packets the command processor must implement, how many
draws a frame costs, and — most importantly — whether this title uses a packet
we cannot name.

An unnamed opcode is reported by number and never bucketed as "other". A packet
we cannot name is precisely the thing phase 4 needs to see; hiding it in an
"other" row converts a gap in our knowledge into a tidy-looking total.

THE SELF-CHECK IS THE POINT
---------------------------
Two independent things in the stream encode the same length: the trace's own
`PacketStart.count` (dwords Xenia recorded) and the PM4 type-3 header's
count-1 field (dwords the GPU expects). If our bit layout for the PM4 header is
wrong, those disagree. `--verify` compares them on every type-3 packet.

This matters because every other number here would look completely plausible
with a wrong shift: opcodes would land on other real opcodes, and the histogram
would be confidently wrong. A vector test that has never failed has not been
shown capable of failing (gotcha 30), and the same applies to a decoder.

USAGE
    xtr_pm4_census.py <trace.xtr> [--verify] [--frames] [--frames-csv out.csv]
"""

import argparse
import collections
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402


def census(path, verify=False, per_frame=False, frames_csv=None):
    data, hdr = xtr.open_trace(path)
    n = len(data)
    print(f"file   : {path}")
    print(f"header : version {hdr['version']}  title {hdr['title']}  "
          f"build {hdr['sha'][:12]}...  {hdr['size'] / 2**30:.2f} GiB")

    opcodes = collections.Counter()
    ptypes = collections.Counter()
    frames = 0
    empty_packets = 0
    draws_this_frame = 0
    packets_this_frame = 0
    frame_draws = []
    frame_packets = []
    desyncs = []
    tail = [0]
    length_mismatch = 0
    length_checked = 0
    mismatch_by_op = collections.Counter()

    t0 = time.time()
    for off, cmd in xtr.walk(data, n,
                             on_desync=lambda o, w: desyncs.append((o, w)),
                             on_tail=lambda o, w: tail.__setitem__(0, w)):
        if cmd == xtr.CMD_PACKET_START:
            w0 = xtr.packet_header_word(data, off)
            if w0 is None:
                empty_packets += 1
                continue
            packets_this_frame += 1
            t = xtr.pm4_type(w0)
            ptypes[t] += 1
            if t == 3:
                op = xtr.pm4_type3_opcode(w0)
                opcodes[op] += 1
                if op in xtr.DRAW_OPCODES:
                    draws_this_frame += 1
                if verify:
                    # Xenia's recorded dword count vs the PM4 header's own
                    # count-1 field. header + data == recorded, or either our
                    # bit layout is wrong or the packet is one Xenia records
                    # deliberately short (see xtr.PM4_SHORT_RECORDED).
                    recorded = xtr._U32.unpack_from(data, off + 8)[0]
                    length_checked += 1
                    short = xtr.PM4_SHORT_RECORDED.get(op, 0)
                    if recorded != xtr.pm4_type3_count(w0) + 1 - short:
                        length_mismatch += 1
                        mismatch_by_op[op] += 1
        elif cmd == xtr.CMD_EVENT:
            frames += 1
            frame_draws.append(draws_this_frame)
            frame_packets.append(packets_this_frame)
            draws_this_frame = 0
            packets_this_frame = 0
    dt = time.time() - t0

    total_pkts = sum(ptypes.values())
    print(f"\npackets: {total_pkts:,}   frames: {frames:,}   "
          f"walked in {dt:.1f}s")
    if empty_packets:
        print(f"  ({empty_packets:,} zero-length PacketStart commands, not counted)")
    if desyncs:
        print(f"  !! {len(desyncs)} desync region(s), "
              f"{sum(w for _, w in desyncs):,} bytes — see xtr_walk.py stats")
    if tail[0]:
        print(f"  ({tail[0]:,} trailing bytes: the file stops mid-command, which the "
              f"capture\n   notes lead us to expect — GPU-trace shutdown preempts the "
              f"final flush)")

    print("\npacket types:")
    labels = {0: "type0  register write run", 1: "type1  register pair",
              2: "type2  nop", 3: "type3  opcode"}
    for t in sorted(ptypes):
        print(f"  {ptypes[t]:12,}  {labels.get(t, f'type{t}')}")

    print(f"\ntype-3 opcodes ({len(opcodes)} distinct):")
    unknown = 0
    for op, c in opcodes.most_common():
        name = xtr.PM4_NAMES.get(op)
        if name is None:
            unknown += 1
        print(f"  {c:12,}  0x{op:02X}  {name or '<UNKNOWN>'}")
    if unknown:
        print(f"\n  !! {unknown} opcode(s) this decoder cannot name. Phase 4 has to "
              f"handle\n     every one of them; do not proceed as if the list above "
              f"were complete.")

    if verify:
        print(f"\npm4 header self-check: {length_checked:,} type-3 packets, "
              f"{length_mismatch:,} unexplained length mismatches")
        if xtr.PM4_SHORT_RECORDED:
            known = ", ".join(f"0x{op:02X} {xtr.pm4_name(op)} (-{d} dword)"
                              for op, d in xtr.PM4_SHORT_RECORDED.items())
            print(f"  allowing for opcodes Xenia records short: {known}")
        if length_mismatch:
            pct = length_mismatch / max(length_checked, 1) * 100
            print(f"  !! {pct:.2f}% disagree, by opcode:")
            for op, c in mismatch_by_op.most_common():
                print(f"       {c:10,}  0x{op:02X}  {xtr.pm4_name(op)}")
            print("     The trace's recorded dword count and the PM4 header's count-1 "
                  "field\n     describe the same packet. Read the shape of this list "
                  "before concluding:\n"
                  "       * mismatches spread across MANY opcodes => the bit layout in "
                  "xtr.py is\n         wrong, and every opcode count above is suspect.\n"
                  "       * ALL of one opcode and NONE of any other => that opcode is "
                  "recorded\n         differently, as INDIRECT_BUFFER is. Establish the "
                  "mechanism, then add\n         it to xtr.PM4_SHORT_RECORDED rather "
                  "than widening the check.")
        else:
            print("  every type-3 packet's recorded length matches its header's "
                  "count-1 field.")

    if frame_draws:
        nonzero = sorted(d for d in frame_draws if d)
        print(f"\ndraws: {sum(frame_draws):,} over {frames:,} frames")
        if nonzero:
            print(f"  frames with >0 draws : {len(nonzero):,} "
                  f"({len(nonzero) / frames * 100:.1f}%)")
            print(f"  min / median / max   : {nonzero[0]:,} / "
                  f"{nonzero[len(nonzero) // 2]:,} / {nonzero[-1]:,}")
            print(f"  mean over drawing frames: "
                  f"{sum(nonzero) / len(nonzero):,.1f}")

    if frames_csv:
        with open(frames_csv, "w") as fh:
            fh.write("frame,packets,draws\n")
            for i, (p, d) in enumerate(zip(frame_packets, frame_draws)):
                fh.write(f"{i},{p},{d}\n")
        print(f"\nper-frame counts -> {frames_csv}")
    if per_frame:
        print("\nper-frame (frame: packets, draws):")
        for i, (p, d) in enumerate(zip(frame_packets, frame_draws)):
            print(f"  {i:6d}: {p:8,} {d:7,}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--verify", action="store_true",
                    help="cross-check the PM4 header bit layout against Xenia's "
                         "recorded packet length")
    ap.add_argument("--frames", action="store_true", help="dump every frame's counts")
    ap.add_argument("--frames-csv", metavar="OUT", help="write per-frame counts to CSV")
    args = ap.parse_args()
    census(args.trace, args.verify, args.frames, args.frames_csv)


if __name__ == "__main__":
    main()
