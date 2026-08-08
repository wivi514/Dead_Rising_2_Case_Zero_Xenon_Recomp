#!/usr/bin/env python3
"""Compare two CZ_VK_FRAME_STATS files as frame time BINNED BY DRAW COUNT.

Why this exists
---------------
The headless recipe that reaches Still Creek's crowds (`CLAUDE.md`, Commands) is 57
fixed 8-second steps against a boot whose depth in fixed wall time is a distribution
(gotcha 75). Two runs of the SAME binary therefore spend different amounts of time in
the safehouse, in the junkyard and facing different directions — so their whole-run
mean frame times differ for reasons that have nothing to do with the change under test,
and `docs/perf-cpu-plan.md`'s workload (a crowd) is a minority of either run.

A whole-run average is consequently the wrong statistic in both directions: it is
dominated by the ~1,900-draw safehouse era, where the title's own two-vblank pacing
pins the frame at 31 fps and a CPU saving measures as exactly zero (§3, "do not
optimise anything measured at ~1,930 draws"), and it is swamped by whichever arm
happened to linger there.

Binning by draw count fixes both. A frame with 6,000 draws is the same workload in
either run whatever second of the run it arrived in, so the comparison is made between
like frames rather than between like timestamps. The bins are also the honest place to
see the pacing cap: the low bins should be flat at ~32 ms in BOTH arms and only the
high bins should move, and an arm that "improves" the low bins is measuring noise or
has changed the draw set (which would make the whole comparison inadmissible — see
CLAUDE.md's A/B admissibility rule).

Usage
-----
    tools/frame_perf_bins.py A.txt B.txt [--bin 1000] [--min-frames 20]
    tools/frame_perf_bins.py A.txt                      # one arm, just the profile

The `msec` column is a CUMULATIVE timestamp, not a duration, so each frame's cost is
the difference from the previous frame's. The first frame of a file has no predecessor
and is dropped; so is any frame whose delta is non-positive or absurd (> 2 s), which is
what a logged pause or a clock step looks like.
"""

import argparse
import math
import sys


def read_frames(path):
    """-> list of (draws, ms) for every frame with a usable duration."""
    out = []
    prev_ms = None
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) < 18:
                continue
            try:
                draws = int(parts[1])
                ms = int(parts[17])
            except ValueError:
                continue
            if prev_ms is not None:
                dt = ms - prev_ms
                # A frame that took longer than two seconds is not a frame; it is a
                # load, a stall or a clock step, and averaging it in would let one
                # such event outweigh a hundred real frames.
                if 0 < dt <= 2000:
                    out.append((draws, dt))
            prev_ms = ms
    return out


def bin_frames(frames, width):
    bins = {}
    for draws, dt in frames:
        bins.setdefault((draws // width) * width, []).append(dt)
    return bins


def stats(values):
    n = len(values)
    mean = sum(values) / n
    if n < 2:
        return mean, 0.0, 0.0
    var = sum((v - mean) ** 2 for v in values) / (n - 1)
    sd = math.sqrt(var)
    return mean, sd, sd / math.sqrt(n)   # mean, sd, standard error of the mean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", help="frame stats file (the control arm)")
    ap.add_argument("b", nargs="?", help="frame stats file (the test arm)")
    ap.add_argument("--bin", type=int, default=1000, help="draw-count bin width")
    ap.add_argument("--min-frames", type=int, default=20,
                    help="skip bins with fewer frames than this in either arm")
    args = ap.parse_args()

    fa = read_frames(args.a)
    if not fa:
        print(f"{args.a}: no usable frames", file=sys.stderr)
        return 1
    ba = bin_frames(fa, args.bin)

    if not args.b:
        print(f"{args.a}: {len(fa)} frames")
        print(f"{'draws':>14}  {'frames':>7}  {'ms/frame':>9}  {'sd':>6}  {'fps':>6}")
        for lo in sorted(ba):
            m, sd, _ = stats(ba[lo])
            print(f"{lo:>6}-{lo+args.bin-1:<7}  {len(ba[lo]):>7}  {m:>9.2f}  "
                  f"{sd:>6.2f}  {1000.0/m:>6.1f}")
        return 0

    fb = read_frames(args.b)
    if not fb:
        print(f"{args.b}: no usable frames", file=sys.stderr)
        return 1
    bb = bin_frames(fb, args.bin)

    print(f"A = {args.a}  ({len(fa)} frames)")
    print(f"B = {args.b}  ({len(fb)} frames)")
    print()
    print(f"{'draws':>14}  {'A frames':>8} {'A ms':>8}  {'B frames':>8} {'B ms':>8}  "
          f"{'delta':>8}  {'sig':>5}")
    # "sig" is the difference in units of its own combined standard error. It is a
    # sanity flag, not a p-value: below ~2 the bin says nothing, and a change that only
    # shows in bins nobody visited twice is not a result (gotcha 50).
    for lo in sorted(set(ba) | set(bb)):
        va, vb = ba.get(lo, []), bb.get(lo, [])
        if len(va) < args.min_frames or len(vb) < args.min_frames:
            continue
        ma, _, sea = stats(va)
        mb, _, seb = stats(vb)
        se = math.sqrt(sea * sea + seb * seb)
        sig = (mb - ma) / se if se > 0 else 0.0
        pct = 100.0 * (mb - ma) / ma
        print(f"{lo:>6}-{lo+args.bin-1:<7}  {len(va):>8} {ma:>8.2f}  "
              f"{len(vb):>8} {mb:>8.2f}  {pct:>+7.1f}%  {sig:>+5.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
