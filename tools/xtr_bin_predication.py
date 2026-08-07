#!/usr/bin/env python3
"""Replay the Xenos ME bin-predication rule over a Xenia `.xtr` capture.

WHY THIS EXISTS
---------------
Case Zero renders its scene as two 640-wide EDRAM tiles (gotcha 118) and tags
every draw packet with a bin mask, selecting the tile with a bin select; the
hardware micro-engine runs a packet whose header bit 0 is set only when
`(mask & select) != 0`. Our command processor has implemented that since phase 1
and had never counted it. When it finally was counted (phase C part 9), the right
tile's pass executed **23** draws against the left tile's **931**, and a third of
the title's draw packets over a whole boot were being discarded.

That leaves exactly one question, and it is not answerable from our own runtime:
is discarding them what the hardware does? Our runtime is the thing under
suspicion, so it cannot be its own oracle. But the capture is a recording of the
same command stream, and Xenia writes a `PacketStart` for every packet it parses
*before* it evaluates predication — so the capture contains the skipped packets
too, and the rule can simply be replayed over it.

If the capture also keeps ~8% of the right tile's draws, the rule is right and
the empty right half comes from somewhere else entirely (and our idea of what
the bins mean is wrong). If the capture keeps most of them, the rule is wrong,
and the shape of the disagreement — which mask/select pairs differ — names where.

WHAT IT REPLAYS
---------------
The register writes that feed the comparison, and nothing else:
  * type-0 packets (a run of register writes) and type-1 (two registers),
  * SET_CONSTANT (0x2D) bank 4, which is the register bank,
  * SET_BIN_MASK/SELECT (0x50/0x51, one 64-bit value in two dwords) and the
    _LO/_HI forms (0x60..0x63), each writing half of a 64-bit register.
Everything else is skipped without decoding its body, because a 1.7 GB trace
walked dword by dword is a twenty-minute run rather than a two-minute one.

The window scissor is tracked alongside, because "which tile is this" is not
knowable from the bin select — that mapping is the very thing in doubt — and
PA_SC_WINDOW_SCISSOR states it directly in screen coordinates.

THE TRACE MODE
--------------
`--trace-window N` prints the same stream-order window the runtime's CZ_PM4_BIN_TRACE
prints — every bin mask/select write with its value, every draw with the pair the
ME compared — and `--trace-arm-mask` / `--trace-arm-select` hold that budget until
the register first takes a given value, exactly like CZ_PM4_BIN_TRACE_ARMMASK.
The symmetry is the point: the two windows are meant to be read side by side, and
a trace armed at the start of either stream proves nothing, because the first
~300,000 packets of a boot are packet-identical on both sides (part 10).

USAGE
    xtr_bin_predication.py <trace.xtr> [--limit-packets N] [--per-select]
                           [--trace-window N] [--trace-arm-mask HEX]
                           [--trace-arm-select HEX]
"""

import argparse
import collections
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402

# Xenos register indices. Same values as runtime/gpu/xenos.h; duplicated rather
# than shared because this tool must be able to disagree with the runtime.
PA_SC_WINDOW_SCISSOR_TL = 0x2081
PA_SC_WINDOW_SCISSOR_BR = 0x2082
PA_SC_WINDOW_OFFSET = 0x2080

DRAW_OPCODES = (0x22, 0x36)  # DRAW_INDX, DRAW_INDX_2

_BE = struct.Struct(">I")


def replay(path, limit_packets=None, per_select=False, trace=0,
           trace_arm_mask=None, trace_arm_select=None):
    data, hdr = xtr.open_trace(path)
    n = len(data)
    print(f"file   : {path}")
    print(f"header : version {hdr['version']}  title {hdr['title']}  "
          f"{hdr['size'] / 2**30:.2f} GiB")

    regs = {}
    bin_mask = (1 << 64) - 1
    bin_select = (1 << 64) - 1

    packets = 0
    draws = 0
    draws_predicated_bit = 0
    draws_skipped = 0
    other_skipped = 0

    # (mask, select) -> [offered, skipped]
    pair_counts = collections.defaultdict(lambda: [0, 0])
    # select -> [offered, skipped]
    select_counts = collections.defaultdict(lambda: [0, 0])
    # scissor -> [offered, skipped]
    scissor_counts = collections.defaultdict(lambda: [0, 0])
    # (scissor, select) -> offered — the join that says whether a bin select
    # corresponds to a screen-space tile at all.
    scissor_select = collections.Counter()

    mask_writes = collections.Counter()
    t0 = time.time()

    # The stream-order window. `armed` starts true only when no arm was asked for.
    trace_left = trace
    armed = trace_arm_mask is None and trace_arm_select is None

    trace_done = False

    def trace_line(text):
        nonlocal trace_left, trace_done
        trace_left -= 1
        if trace_left <= 0:
            trace_done = True
        print(text)

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
            body = count - 1
            for i in range(body):
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

        # The predication test, exactly as gpu/pm4.cpp performs it. Applied to
        # every type-3 opcode, because that is what the ME does — a skipped
        # SET_CONSTANT is as real as a skipped draw, and counting only draws
        # would hide a mask that is being set by a packet we skipped.
        predicated = (header & 1) and (bin_mask & bin_select) == 0

        if not armed and (
                (trace_arm_mask is not None
                 and (bin_mask & 0xFFFFFFFF) == trace_arm_mask)
                or (trace_arm_select is not None
                    and (bin_select & 0xFFFFFFFF) == trace_arm_select)):
            armed = True

        if armed and trace_left > 0:
            if opcode in DRAW_OPCODES:
                tl = regs.get(PA_SC_WINDOW_SCISSOR_TL, 0)
                br = regs.get(PA_SC_WINDOW_SCISSOR_BR, 0)
                trace_line(
                    f"[b1bin]     DRAW pred={header & 1} mask={bin_mask:016X} "
                    f"select={bin_select:016X} scissor={tl & 0x7FFF},"
                    f"{(tl >> 16) & 0x7FFF}..{br & 0x7FFF},{(br >> 16) & 0x7FFF}"
                    f" -> {'SKIP' if predicated else 'run'}")
            elif opcode in (0x50, 0x51, 0x60, 0x61, 0x62, 0x63):
                what = {0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT",
                        0x60: "MASK_LO", 0x61: "MASK_HI",
                        0x62: "SELECT_LO", 0x63: "SELECT_HI"}[opcode]
                trace_line(f"[b1bin] {what:<15} {word(off, 1):08X}"
                           f"{'   (SKIPPED)' if predicated else ''}")
            # A trace run stops at its budget rather than walking the remaining
            # gigabytes: a 1.7 GB replay is a twenty-minute wait for a window that
            # was complete after a hundred packets. The census below is then over a
            # PREFIX and says so, so that a partial table is never mistaken for the
            # whole-capture one the paragraph above quotes.
            if trace_done:
                break

        if opcode in DRAW_OPCODES:
            draws += 1
            if header & 1:
                draws_predicated_bit += 1
            pair_counts[(bin_mask, bin_select)][0] += 1
            select_counts[bin_select][0] += 1
            tl = regs.get(PA_SC_WINDOW_SCISSOR_TL, 0)
            br = regs.get(PA_SC_WINDOW_SCISSOR_BR, 0)
            sc = (tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF)
            scissor_counts[sc][0] += 1
            scissor_select[(sc, bin_select)] += 1
            if predicated:
                draws_skipped += 1
                pair_counts[(bin_mask, bin_select)][1] += 1
                select_counts[bin_select][1] += 1
                scissor_counts[sc][1] += 1

        if predicated:
            if opcode not in DRAW_OPCODES:
                other_skipped += 1
            # A skipped packet writes nothing — including the bin registers.
            continue

        if opcode == 0x50:  # SET_BIN_MASK (lo, hi)
            if count >= 3:
                bin_mask = word(off, 1) | (word(off, 2) << 32)
                mask_writes["SET_BIN_MASK"] += 1
        elif opcode == 0x51:  # SET_BIN_SELECT (lo, hi)
            if count >= 3:
                bin_select = word(off, 1) | (word(off, 2) << 32)
                mask_writes["SET_BIN_SELECT"] += 1
        elif opcode == 0x60:
            bin_mask = (bin_mask & 0xFFFFFFFF00000000) | word(off, 1)
            mask_writes["SET_BIN_MASK_LO"] += 1
        elif opcode == 0x61:
            bin_mask = (bin_mask & 0xFFFFFFFF) | (word(off, 1) << 32)
            mask_writes["SET_BIN_MASK_HI"] += 1
        elif opcode == 0x62:
            bin_select = (bin_select & 0xFFFFFFFF00000000) | word(off, 1)
            mask_writes["SET_BIN_SELECT_LO"] += 1
        elif opcode == 0x63:
            bin_select = (bin_select & 0xFFFFFFFF) | (word(off, 1) << 32)
            mask_writes["SET_BIN_SELECT_HI"] += 1
        elif opcode == 0x2D:  # SET_CONSTANT — bank 4 is the register file
            if count >= 2 and ((word(off, 1) >> 16) & 0xFF) == 4:
                index = (word(off, 1) & 0x7FF) + 0x2000
                for i in range(2, count):
                    regs[index + i - 2] = word(off, i)
        elif opcode in (0x55, 0x56):  # SET_CONSTANT2 — absolute index
            if count >= 2:
                index = word(off, 1) & 0xFFFF
                for i in range(2, count):
                    regs[index + i - 2] = word(off, i)

    dt = time.time() - t0
    print(f"walked : {packets:,} packets in {dt:.0f} s")
    if trace_done:
        print("NOTE   : stopped at the trace budget — every count below is over a "
              "PREFIX of the capture, not the whole of it")
    print()
    print(f"draw packets              : {draws:,}")
    print(f"  with predication bit set: {draws_predicated_bit:,} "
          f"({100.0 * draws_predicated_bit / max(draws, 1):.1f}%)")
    print(f"  SKIPPED by the bin rule : {draws_skipped:,} "
          f"({100.0 * draws_skipped / max(draws, 1):.1f}%)")
    print(f"non-draw type-3 skipped   : {other_skipped:,}")
    print()
    print("bin register writes:")
    for k, v in mask_writes.most_common():
        print(f"  {k:<20} {v:,}")

    print()
    print("by (mask, select) pair — the pair the ME compares:")
    print(f"  {'mask':>16}  {'select':>16}  {'offered':>12}  {'skipped':>12}   kept")
    for (m, s), (offered, skipped) in sorted(pair_counts.items(),
                                             key=lambda kv: -kv[1][0])[:20]:
        kept = offered - skipped
        print(f"  {m:016X}  {s:016X}  {offered:12,}  {skipped:12,}   "
              f"{100.0 * kept / max(offered, 1):5.1f}%")

    if per_select:
        print()
        print("by bin select alone:")
        for s, (offered, skipped) in sorted(select_counts.items(),
                                            key=lambda kv: -kv[1][0])[:20]:
            kept = offered - skipped
            print(f"  {s:016X}  offered {offered:12,}  kept {kept:12,}  "
                  f"{100.0 * kept / max(offered, 1):5.1f}%")

    print()
    print("by window scissor — the tile in SCREEN coordinates, which is the thing")
    print("the bin select is supposed to correspond to:")
    for sc, (offered, skipped) in sorted(scissor_counts.items(),
                                         key=lambda kv: -kv[1][0])[:16]:
        kept = offered - skipped
        print(f"  {sc[0]:5},{sc[1]:5} .. {sc[2]:5},{sc[3]:5}   offered {offered:12,}  "
              f"kept {kept:12,}  {100.0 * kept / max(offered, 1):5.1f}%")

    print()
    print("scissor x select — which selects each tile is rendered under:")
    for (sc, s), c in scissor_select.most_common(16):
        print(f"  {sc[0]:5},{sc[1]:5} .. {sc[2]:5},{sc[3]:5}   select {s:016X}  {c:12,}")

    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--limit-packets", type=int, default=None,
                    help="stop after N packets (a smoke test, not a measurement)")
    ap.add_argument("--per-select", action="store_true")
    # NB --trace-window, not --trace: the positional argument is already called
    # `trace` (the .xtr path), and argparse would silently give both the same dest.
    ap.add_argument("--trace-window", type=int, default=0, dest="trace_window",
                    help="print N bin-relevant packets in stream order, the same "
                         "window CZ_PM4_BIN_TRACE prints for our runtime")
    ap.add_argument("--trace-arm-mask", default=None,
                    help="hold the trace budget until the bin mask LO first equals "
                         "this hex value (mirrors CZ_PM4_BIN_TRACE_ARMMASK)")
    ap.add_argument("--trace-arm-select", default=None,
                    help="likewise for the bin select LO (CZ_PM4_BIN_TRACE_ARM)")
    args = ap.parse_args()
    return replay(args.trace, args.limit_packets, args.per_select, args.trace_window,
                  int(args.trace_arm_mask, 16) if args.trace_arm_mask else None,
                  int(args.trace_arm_select, 16) if args.trace_arm_select else None)


if __name__ == "__main__":
    sys.exit(main())
