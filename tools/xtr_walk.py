#!/usr/bin/env python3
"""Walk a Xenia `.xtr` GPU trace: integrity check, command census, frame index.

WHY THIS EXISTS
---------------
The first question about any capture is "is it intact, and what is in it" — and
for a GPU stream that question had no answer in this repo at all. This is the
tool that answers it, and it is deliberately the *cheap* one: it counts trace
commands (how the capture was framed) and never looks inside a PM4 packet
(what the guest asked the GPU to do). `xtr_pm4_census.py` does the latter.

Format and design notes live in `tools/xtr.py`.

SUBCOMMANDS
    stats  <trace.xtr>              command census + integrity verdict
    index  <trace.xtr> <out.json>   byte offset of every frame (swap event)
    limits <trace.xtr>              largest value seen in each bounded field

WHY `limits` IS A SUBCOMMAND AND NOT A COMMENT
----------------------------------------------
`xtr.step()` decides "this is not a real command" from upper bounds on length
fields, because the format has no magic number or checksum to check instead.
Those bounds are guesses. A guess that is too tight reports a valid capture as
corrupt; too loose and a desync runs for megabytes before anything notices.
`limits` prints the largest value each field actually reaches, so the headroom
is a measured number rather than folklore inherited from another title.
"""

import argparse
import collections
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402


def _integrity(hdr, start, desyncs, tail_gap):
    """Print the verdict, and say what a non-clean result would mean."""
    print("\nintegrity:")
    ok = True
    if start != xtr.HEADER_SIZE:
        ok = False
        print(f"  !! walk began at {start:#x}, not {xtr.HEADER_SIZE:#x} — the file head "
              f"is unreadable.")
        print("     On a pre-fix Windows capture this was the 2 GiB wrapped-header bug "
              "clobbering\n     the head. That bug is fixed for this title, so this "
              "means a genuinely damaged file.")
    else:
        print(f"  head walkable from {xtr.HEADER_SIZE} (clean)")

    if desyncs:
        ok = False
        total = sum(w for _, w in desyncs)
        print(f"  !! {len(desyncs)} desync region(s), {total:,} bytes unreadable")
        for off, width in desyncs[:10]:
            print(f"       {off:#014x}  {width:,} bytes skipped")
        if len(desyncs) > 10:
            print(f"       ... and {len(desyncs) - 10} more")
        print("     A desync is NOT expected on this title: the 2 GiB trace_writer cliff\n"
              "     was fixed at source before these captures. Suspect either a damaged\n"
              "     file or a wrong belief about the format in tools/xtr.py.")
    else:
        print("  no desyncs — the whole stream parsed as a single sequence")

    if tail_gap:
        print(f"  note: {tail_gap:,} trailing byte(s) — the file stops mid-command.")
        print("     Expected here: the capture notes record that GPU-trace shutdown "
              "preempts\n     the final flush. Reported rather than discarded so a "
              "*large* tail, which\n     would mean a real truncation, is still visible.")
    print(f"  verdict: {'INTACT' if ok else 'DAMAGED'}")
    return ok


def stats(path):
    data, hdr = xtr.open_trace(path)
    n = len(data)
    print(f"file   : {path}")
    print(f"header : version {hdr['version']}  title {hdr['title']}  "
          f"build {hdr['sha'][:12]}...")
    print(f"size   : {hdr['size']:,} bytes ({hdr['size'] / 2**30:.2f} GiB)")

    counts = collections.Counter()
    desyncs = []
    tail = [0]
    frames = 0
    start = xtr.find_start(data, n)
    t0 = time.time()
    for off, cmd in xtr.walk(data, n, start=start,
                             on_desync=lambda o, w: desyncs.append((o, w)),
                             on_tail=lambda o, w: tail.__setitem__(0, w)):
        counts[cmd] += 1
        if cmd == xtr.CMD_EVENT:
            frames += 1
    tail_gap = tail[0]
    dt = time.time() - t0

    total = sum(counts.values())
    print(f"\ncommands: {total:,}  in {dt:.1f}s ({total / max(dt, 1e-9):,.0f}/s)")
    for cid in range(xtr.MAX_CMD + 1):
        if counts[cid]:
            print(f"  {counts[cid]:12,}  {xtr.CMD_NAMES[cid]}")

    print(f"\nframes (swap events): {frames:,}")
    if frames:
        print(f"  {hdr['size'] / frames / 1024:,.1f} KiB of stream per frame")

    _integrity(hdr, start, desyncs, tail_gap)


def index(path, out):
    data, hdr = xtr.open_trace(path)
    n = len(data)
    swaps = []
    desyncs = []
    start = xtr.find_start(data, n)
    for off, cmd in xtr.walk(data, n, start=start,
                             on_desync=lambda o, w: desyncs.append((o, w))):
        if cmd == xtr.CMD_EVENT:
            swaps.append(off)
    payload = {"header": hdr, "file": str(path), "start": start,
               "swaps": swaps, "desyncs": desyncs}
    Path(out).write_text(json.dumps(payload))
    print(f"{hdr['title']}  frames={len(swaps):,}  start={start:#x}  "
          f"desync_regions={len(desyncs)}  -> {out}")


def limits(path):
    """Measure how much headroom xtr.step()'s plausibility bounds actually have."""
    data, n = None, 0
    data, hdr = xtr.open_trace(path)
    n = len(data)
    peak = collections.Counter()
    start = xtr.find_start(data, n)
    for off, cmd in xtr.walk(data, n, start=start):
        if cmd == xtr.CMD_PACKET_START:
            v = xtr._U32.unpack_from(data, off + 8)[0]
            if v > peak["packet_dwords"]:
                peak["packet_dwords"] = v
        elif cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            v = xtr._U32.unpack_from(data, off + 12)[0]
            if v > peak["memory_bytes"]:
                peak["memory_bytes"] = v
        elif cmd == xtr.CMD_EDRAM_SNAPSHOT:
            v = xtr._U32.unpack_from(data, off + 8)[0]
            if v > peak["edram_bytes"]:
                peak["edram_bytes"] = v
        elif cmd == xtr.CMD_REGISTERS:
            first, count, _cb, _enc, elen = xtr._U32X5.unpack_from(data, off + 4)
            peak["register_first"] = max(peak["register_first"], first)
            peak["register_count"] = max(peak["register_count"], count)
            peak["register_bytes"] = max(peak["register_bytes"], elen)
        elif cmd == xtr.CMD_GAMMA_RAMP:
            _rw, _enc, elen = xtr._U32X3.unpack_from(data, off + 4)
            peak["gamma_bytes"] = max(peak["gamma_bytes"], elen)

    bounds = [("packet_dwords", xtr.LIMIT_PACKET_DWORDS),
              ("memory_bytes", xtr.LIMIT_MEMORY_BYTES),
              ("edram_bytes", xtr.LIMIT_EDRAM_BYTES),
              ("register_first", xtr.LIMIT_REGISTER_INDEX),
              ("register_count", xtr.LIMIT_REGISTER_INDEX),
              ("register_bytes", xtr.LIMIT_REGISTER_BYTES),
              ("gamma_bytes", xtr.LIMIT_GAMMA_BYTES)]
    print(f"{path}\n")
    print(f"{'field':18} {'observed max':>14} {'limit':>12}   headroom")
    for field, lim in bounds:
        obs = peak[field]
        ratio = f"{lim / obs:,.0f}x" if obs else "(never seen)"
        print(f"{field:18} {obs:>14,} {lim:>12,}   {ratio}")
    print("\nA field whose observed max approaches its limit is the one to raise before\n"
          "a longer capture reports a false desync.")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stats", help="command census + integrity verdict")
    p.add_argument("trace")
    p = sub.add_parser("index", help="byte offset of every frame")
    p.add_argument("trace")
    p.add_argument("out", nargs="?", default="trace_index.json")
    p = sub.add_parser("limits", help="measured headroom of the plausibility bounds")
    p.add_argument("trace")
    args = ap.parse_args()

    if args.cmd == "stats":
        stats(args.trace)
    elif args.cmd == "index":
        index(args.trace, args.out)
    else:
        limits(args.trace)


if __name__ == "__main__":
    main()
