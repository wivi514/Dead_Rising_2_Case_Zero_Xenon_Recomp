#!/usr/bin/env python3
"""Census every RESOLVE in a Xenia `.xtr` capture, by SOURCE and destination.

WHY THIS EXISTS
---------------
`runtime/gpu/vk_renderer.cpp`'s `DoResolve` snapshots the COLOUR target for every
resolve it sees, unconditionally. Phase 5 §6d already named that as a gap — a
`RB_MODECONTROL` of 5 is a depth-only pass and its resolve copies the DEPTH
buffer — but it was recorded as "four of the black surfaces in §7's table are
depth resolves being served an empty colour buffer" and never quantified, and
nothing has since asked how much of this title's frame depends on it.

It matters now because phase C part 14's biggest visible defect is that the whole
frame is uniformly out of focus (phase5-notes §6ad item 1), and the pass that
produces the blur reads a circle-of-confusion surface computed from DEPTH. If our
renderer hands that pass the colour buffer instead, "everything reads as far" is
exactly what you would expect — and the fix is a different subsystem from the one
the symptom points at.

Our own command processor is the suspect, so it cannot be its own oracle
(gotcha 178). The capture can: Xenia records every register write, so replaying
RB_COPY_CONTROL / RB_COPY_DEST_BASE / RB_MODECONTROL over the stream says what
hardware was asked to copy, from where, to where — with no emulator involved.

WHAT IT REPLAYS
---------------
The same register-write subset `xtr_bin_predication.py` replays (type-0, type-1,
SET_CONSTANT bank 4 and SET_CONSTANT2), plus the copy block. A resolve is a DRAW
packet issued while RB_MODECONTROL's edram_mode is 6 (kCopy) — it is not a packet
of its own, which is why gating on a shader pair or on a packet opcode finds only
the one blit the present path happens to use.

RB_COPY_CONTROL bit layout (the fields this tool reads):
    bits 0..2   copy_src_select   0..3 = colour target N, 4 = DEPTH
    bits 4..5   copy_sample_select
    bit  8      colour clear after copy
    bit  9      depth clear after copy
    bits 20..21 copy_command

PART 119 ADDED THE FORMAT PAIR AND THE SWAP. Per destination it also prints the
EDRAM source's RB_COLOR_INFO format (1 = k_8_8_8_8_GAMMA, a PWL encode on write) and
RB_COPY_DEST_INFO's format / number / exp_bias / endian, plus the XE_SWAP packet's front
buffer and whether anything resolved there. That is how "is the front buffer gamma
encoded" was answered (no: rt 0, dest 6, endian 0, in every capture — phase5-notes §6fb).

USAGE
    xtr_resolve_census.py <trace.xtr> [--limit-packets N]
"""

import argparse
import collections
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402

# Xenos register indices. Duplicated from runtime/gpu/xenos.h rather than shared,
# because this tool has to be able to disagree with the runtime.
RB_MODECONTROL = 0x2208
RB_SURFACE_INFO = 0x2000
RB_COPY_CONTROL = 0x2318
RB_COPY_DEST_BASE = 0x2319
RB_COPY_DEST_PITCH = 0x231A
RB_COPY_DEST_INFO = 0x231B
RB_COLOR_INFO = 0x2001
XE_SWAP = 0x64

# RB_COLOR_INFO bits 16..19: the EDRAM render-target format (ColorRenderTargetFormat).
# 1 is k_8_8_8_8_GAMMA — the pixel shader's output is PWL-gamma ENCODED on write, so a
# resolve of that target copies out gamma-encoded bytes. Part 119: this field decides
# whether the front buffer the swap presents is linear or encoded, and until then
# nothing in this repo had ever printed it for the resolve that IS the frame.
RT_FORMAT = {0: "8_8_8_8", 1: "8_8_8_8_GAMMA", 2: "2_10_10_10", 3: "2_10_10_10_FLOAT",
             4: "16_16", 5: "16_16_16_16", 6: "16_16_FLOAT", 7: "16_16_16_16_FLOAT",
             10: "2_10_10_10_AS_10_10_10_10", 12: "2_10_10_10_FLOAT_AS_16_16_16_16",
             14: "32_FLOAT", 15: "32_32_FLOAT"}
# RB_COPY_DEST_INFO: endian:3 @0, array:1 @3, slice:3 @4, format:6 @7 (a TEXTURE
# format code, k_8_8_8_8 = 6), number:3 @13, exp_bias:6 @16 (signed), swap:1 @24.
DEST_FORMAT = {2: "8", 3: "1_5_5_5", 4: "5_6_5", 6: "8_8_8_8", 7: "2_10_10_10",
               10: "8_8", 14: "8_8_8_8_A", 24: "16", 25: "16_16", 26: "16_16_16_16",
               30: "16_FLOAT", 31: "16_16_FLOAT", 32: "16_16_16_16_FLOAT",
               36: "32_FLOAT", 37: "32_32_FLOAT", 38: "32_32_32_32_FLOAT",
               62: "2_10_10_10_FLOAT"}
NUMBER_FORMAT = {0: "unorm", 1: "snorm", 2: "uint", 3: "sint", 4: "float",
                 6: "float(ieee)"}
PA_SC_WINDOW_SCISSOR_TL = 0x2081
PA_SC_WINDOW_SCISSOR_BR = 0x2082

DRAW_OPCODES = (0x22, 0x36)  # DRAW_INDX, DRAW_INDX_2

SRC_NAME = {0: "colour0", 1: "colour1", 2: "colour2", 3: "colour3", 4: "DEPTH"}

_BE = struct.Struct(">I")


def census(path, limit_packets=None):
    data, hdr = xtr.open_trace(path)
    n = len(data)
    print(f"file   : {path}")
    print(f"header : version {hdr['version']}  title {hdr['title']}  "
          f"{hdr['size'] / 2**30:.2f} GiB")

    regs = {}
    packets = 0
    draws = 0
    resolves = 0

    # A resolve's identity for this purpose is (source, destination). The extent
    # and the clear bits come along because they are what says whether two
    # resolves to one address are the same surface (gotcha 121) and whether a
    # pass wipes the EDRAM behind itself (gotcha 113).
    by_src = collections.Counter()
    by_dest = collections.defaultdict(lambda: collections.Counter())
    dest_extent = {}
    dest_clear = collections.defaultdict(collections.Counter)
    # The FORMAT PAIR at each destination: (RB_COLOR_INFO format of the EDRAM source,
    # RB_COPY_DEST_INFO format/number/exp_bias of the memory destination). A
    # destination that sees more than one pair is printed with all of them.
    dest_fmt = collections.defaultdict(collections.Counter)
    swaps = []
    # The order the destinations are first seen in — a frame's dependency chain
    # reads far better in stream order than sorted by count.
    first_seen = {}

    t0 = time.time()

    def word(off, i):
        return _BE.unpack_from(data, off + 12 + 4 * i)[0]

    for off, cmd in xtr.walk(data, n):
        if cmd != xtr.CMD_PACKET_START:
            continue
        count = struct.unpack_from("<I", data, off + 8)[0]
        if not count:
            continue
        packets += 1
        if limit_packets and packets > limit_packets:
            break

        header = word(off, 0)
        ptype = header >> 30

        if ptype == 0:
            reg = header & 0x7FFF
            one_reg = (header >> 15) & 1
            for i in range(count - 1):
                regs[reg if one_reg else reg + i] = word(off, 1 + i)
            continue
        if ptype == 1:
            if count >= 3:
                regs[header & 0x7FF] = word(off, 1)
                regs[(header >> 11) & 0x7FF] = word(off, 2)
            continue
        if ptype != 3:
            continue

        opcode = (header >> 8) & 0x7F

        if opcode == 0x2D:  # SET_CONSTANT — bank 4 is the register file
            if count >= 2 and ((word(off, 1) >> 16) & 0xFF) == 4:
                index = (word(off, 1) & 0x7FF) + 0x2000
                for i in range(2, count):
                    regs[index + i - 2] = word(off, i)
            continue
        if opcode in (0x55, 0x56):  # SET_CONSTANT2 — absolute index
            if count >= 2:
                index = word(off, 1) & 0xFFFF
                for i in range(2, count):
                    regs[index + i - 2] = word(off, i)
            continue

        if opcode == XE_SWAP:
            # VdSwap's body: 'SWAP', front buffer, width, height (see pm4.cpp 0x64).
            if count >= 5 and word(off, 1) == 0x53574150:
                swaps.append((word(off, 2), word(off, 3), word(off, 4)))
            continue
        if opcode not in DRAW_OPCODES:
            continue
        draws += 1

        # edram_mode lives in RB_MODECONTROL's low 3 bits; 6 is kCopy.
        if (regs.get(RB_MODECONTROL, 0) & 7) != 6:
            continue
        resolves += 1

        control = regs.get(RB_COPY_CONTROL, 0)
        src = control & 7
        dest = regs.get(RB_COPY_DEST_BASE, 0) & 0xFFFFFFFC
        pitch = regs.get(RB_COPY_DEST_PITCH, 0)
        tl = regs.get(PA_SC_WINDOW_SCISSOR_TL, 0)
        br = regs.get(PA_SC_WINDOW_SCISSOR_BR, 0)

        by_src[SRC_NAME.get(src, f"src{src}")] += 1
        by_dest[dest][SRC_NAME.get(src, f"src{src}")] += 1
        dest_extent[dest] = (pitch & 0x3FFF, (pitch >> 16) & 0x3FFF,
                             tl & 0x7FFF, (tl >> 16) & 0x7FFF,
                             br & 0x7FFF, (br >> 16) & 0x7FFF)
        dest_clear[dest][((control >> 8) & 1, (control >> 9) & 1)] += 1
        info = regs.get(RB_COPY_DEST_INFO, 0)
        cinfo = regs.get(RB_COLOR_INFO, 0)
        exp_bias = (info >> 16) & 0x3F
        if exp_bias >= 32:
            exp_bias -= 64
        dest_fmt[dest][((cinfo >> 16) & 0xF, (info >> 7) & 0x3F, (info >> 13) & 7,
                        exp_bias, info & 7)] += 1
        first_seen.setdefault(dest, resolves)

    dt = time.time() - t0
    print(f"walked : {packets:,} packets in {dt:.0f} s")
    print()
    print(f"draw packets : {draws:,}")
    print(f"resolves     : {resolves:,}")
    print()
    print("by copy source — the field our DoResolve does not read:")
    for name, c in by_src.most_common():
        print(f"  {name:<10} {c:12,}  ({100.0 * c / max(resolves, 1):5.1f}%)")

    print()
    print("by destination, in first-seen order "
          "(surface = RB_COPY_DEST_PITCH, region = window scissor):")
    print(f"  {'dest':>8}  {'surface':>10}  {'region':>20}  "
          f"{'clr c/d':>8}  {'count':>9}   sources")
    for dest in sorted(first_seen, key=lambda d: first_seen[d]):
        w, h, x0, y0, x1, y1 = dest_extent[dest]
        clears = ",".join(f"{c}{d}" for (c, d) in dest_clear[dest])
        srcs = " ".join(f"{k}={v:,}" for k, v in by_dest[dest].most_common())
        print(f"  {dest:08X}  {w:5}x{h:<4}  {x0:5},{y0:<4}..{x1:5},{y1:<4}  "
              f"{clears:>8}  {sum(by_dest[dest].values()):9,}   {srcs}")

    print()
    print("by destination, the FORMAT PAIR — the EDRAM source's RB_COLOR_INFO format and")
    print("RB_COPY_DEST_INFO's texture format / number / exp_bias / endian (part 119):")
    for dest in sorted(first_seen, key=lambda d: first_seen[d]):
        for (rt, df, num, eb, en), c in dest_fmt[dest].most_common():
            print(f"  {dest:08X}  rt={rt:2} {RT_FORMAT.get(rt, '?'):<16} "
                  f"dest={df:2} {DEST_FORMAT.get(df, '?'):<12} "
                  f"num={NUMBER_FORMAT.get(num, num):<6} exp_bias={eb:+d} endian={en}"
                  f"  x{c:,}")

    print()
    print(f"XE_SWAP packets: {len(swaps):,}")
    for fb, w, h in sorted(set(swaps)):
        srcs = " ".join(f"{k}={v:,}" for k, v in by_dest.get(fb & 0x1FFFFFFF, {}).items())
        print(f"  front buffer {fb:08X}  {w}x{h}  "
              f"{'RESOLVED here: ' + srcs if srcs else 'no resolve to this address'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--limit-packets", type=int, default=None,
                    help="stop after N packets (the census is then over a PREFIX)")
    args = ap.parse_args()
    census(args.trace, args.limit_packets)


if __name__ == "__main__":
    main()
