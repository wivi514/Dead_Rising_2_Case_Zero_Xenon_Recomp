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

Multiple runs per arm are the point, not a convenience. Measured against a genuine
null arm — a commit that changed only the profiler's arithmetic — individual crowd bins
still moved 10-13% at ONE run a side, because the frames inside a bin are not
independent samples: consecutive frames share a camera, a location and a thermal state,
so the effective N is a small fraction of the frame count and the standard error
computed from the raw count is confidently wrong. Pool three runs an arm, alternated
(a, b, a, b, a, b) so that any drift in the machine is shared between the arms, and
read the per-run spread that this prints before reading the delta.

THE MEAN IS NOT THE STATISTIC TO READ (gotcha 237)
--------------------------------------------------
This tool reported means only, for its first two parts, and that nearly filed part 22's
cross-frame stream store as noise: it scored a change that removed 5.5 ms of measured
copying at **+1.7% against a +1.3% null**. The mechanism was real and the metric could
not see it.

The reason is that this title's frame time is QUANTISED BY A PACING FLOOR. The frame
sits on a multiple of the vblank interval — 32 ms at two, 48 ms at three — and a CPU
saving converts into frame rate only where the frame is above one floor and within reach
of the next. At ~6,500 draws both arms were already parked on 48 ms, and 5 ms cannot
reach 32, so the saving appeared as idle time and the mean barely moved. At ~3,700 draws
the SAME data reads 44 ms -> 32 ms.

So this prints three things per bin and they answer different questions:

  * **mean** — what it always printed. Sensitive to how long each arm lingered where,
    and pulled about by the floor. Kept for continuity with earlier sessions' numbers.
  * **median** — what the typical frame cost. This is the one that showed 44 -> 32.
  * **pinned%** — the share of frames within `--pin-tol` ms of a multiple of
    `--pin-ms`. **This is the most sensitive of the three**, because it turns "did the
    frame reach the next floor" into a yes/no per frame instead of averaging across a
    step function. Part 22's decisive number was this column going 10% -> 97% where the
    mean moved 1.7%. A high pinned share means the workload is no longer yours: the
    title's pacing is what is limiting the frame, and no further CPU saving in that bin
    will show up until the next floor is in reach.

Usage
-----
    tools/frame_perf_bins.py --a a1.txt a2.txt a3.txt --b b1.txt b2.txt b3.txt
    tools/frame_perf_bins.py A.txt B.txt                # one run an arm (see above)
    tools/frame_perf_bins.py A.txt                      # one arm, just the profile
    tools/frame_perf_bins.py --a ... --b ... --bin 500  # finer, to find the live band

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


def median(values):
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def pinned_pct(values, period, tol):
    """Share of frames sitting within `tol` ms of a multiple of `period`.

    The distance to the NEAREST multiple, not the remainder: a frame at 31.6 ms with a
    16 ms period has remainder 15.6, which a naive `dt % period <= tol` test would call
    free-running when it is one third of a millisecond off the two-vblank floor.
    """
    if period <= 0:
        return float("nan")
    hit = 0
    for v in values:
        r = v % period
        if min(r, period - r) <= tol:
            hit += 1
    return 100.0 * hit / len(values)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pos", nargs="*", help="one or two frame stats files")
    ap.add_argument("--a", nargs="+", default=None,
                    help="control-arm frame stats files (pool several runs)")
    ap.add_argument("--b", nargs="+", default=None,
                    help="test-arm frame stats files (pool several runs)")
    ap.add_argument("--bin", type=int, default=1000, help="draw-count bin width")
    ap.add_argument("--min-frames", type=int, default=20,
                    help="skip bins with fewer frames than this in either arm")
    # 16 ms rather than 16.67: this is the number part 22 measured the pinning at, and
    # the frame stats' `msec` column is integer milliseconds, so a 16.67 period would
    # spend the tolerance on the quantisation of the timestamp rather than on the frame.
    ap.add_argument("--pin-ms", type=float, default=16.0,
                    help="the pacing period frames pin to (default 16)")
    ap.add_argument("--pin-tol", type=float, default=1.0,
                    help="how close to a multiple counts as pinned (default 1 ms)")
    args = ap.parse_args()

    a_files = args.a if args.a else args.pos[:1]
    b_files = args.b if args.b else args.pos[1:2]
    if not a_files:
        ap.error("give at least one frame stats file")

    fa = [f for p in a_files for f in read_frames(p)]
    if not fa:
        print(f"{a_files}: no usable frames", file=sys.stderr)
        return 1
    ba = bin_frames(fa, args.bin)

    if not b_files:
        print(f"{' '.join(a_files)}: {len(fa)} frames")
        print(f"{'draws':>14}  {'frames':>7}  {'mean':>7}  {'median':>7}  {'sd':>6}  "
              f"{'pinned':>7}  {'fps':>6}")
        for lo in sorted(ba):
            m, sd, _ = stats(ba[lo])
            md = median(ba[lo])
            pin = pinned_pct(ba[lo], args.pin_ms, args.pin_tol)
            print(f"{lo:>6}-{lo+args.bin-1:<7}  {len(ba[lo]):>7}  {m:>7.2f}  "
                  f"{md:>7.2f}  {sd:>6.2f}  {pin:>6.0f}%  {1000.0/md:>6.1f}")
        return 0

    fb = [f for p in b_files for f in read_frames(p)]
    if not fb:
        print(f"{b_files}: no usable frames", file=sys.stderr)
        return 1
    bb = bin_frames(fb, args.bin)

    print(f"A = {' '.join(a_files)}  ({len(fa)} frames)")
    print(f"B = {' '.join(b_files)}  ({len(fb)} frames)")
    print()

    # The per-RUN mean of the top bin, per arm. This is the number that says whether the
    # delta below is bigger than the spread between runs of one binary — which at one
    # run an arm it very often is not. Printing it beside the result is what stops a
    # within-arm spread being read as a between-arm effect.
    def per_run(files):
        out = []
        for p in files:
            fr = [dt for draws, dt in read_frames(p) if draws >= 6000]
            out.append(sum(fr) / len(fr) if fr else float("nan"))
        return out

    ra, rb = per_run(a_files), per_run(b_files)
    if len(a_files) > 1 or len(b_files) > 1:
        fmt = lambda xs: " ".join(f"{x:.2f}" for x in xs)
        print(f"per-run mean ms of frames with >= 6000 draws:")
        print(f"  A: {fmt(ra)}")
        print(f"  B: {fmt(rb)}")
        print()
    print(f"pinned% = frames within {args.pin_tol:g} ms of a multiple of "
          f"{args.pin_ms:g} ms — the title's pacing floor. READ THIS COLUMN FIRST "
          f"(gotcha 237).")
    print()
    print(f"{'draws':>14}  {'A n':>6} {'A mean':>7} {'A med':>6} {'A pin':>6}  "
          f"{'B n':>6} {'B mean':>7} {'B med':>6} {'B pin':>6}  "
          f"{'d mean':>7} {'d med':>7}  {'sig':>5}")
    # "sig" is the difference in units of its own combined standard error, and it is
    # NOT a significance test — do not read it as one. A null arm (same binary, two
    # runs) produced crowd bins at 10-13% and sig up to 22, because consecutive frames
    # in a bin share a camera and a location and are nowhere near independent. Treat it
    # as an ordering hint only, and let the per-run spread printed above decide.
    for lo in sorted(set(ba) | set(bb)):
        va, vb = ba.get(lo, []), bb.get(lo, [])
        if len(va) < args.min_frames or len(vb) < args.min_frames:
            continue
        ma, _, sea = stats(va)
        mb, _, seb = stats(vb)
        se = math.sqrt(sea * sea + seb * seb)
        sig = (mb - ma) / se if se > 0 else 0.0
        pct = 100.0 * (mb - ma) / ma
        da, db = median(va), median(vb)
        dpct = 100.0 * (db - da) / da if da else 0.0
        pa = pinned_pct(va, args.pin_ms, args.pin_tol)
        pb = pinned_pct(vb, args.pin_ms, args.pin_tol)
        print(f"{lo:>6}-{lo+args.bin-1:<7}  {len(va):>6} {ma:>7.2f} {da:>6.1f} "
              f"{pa:>5.0f}%  {len(vb):>6} {mb:>7.2f} {db:>6.1f} {pb:>5.0f}%  "
              f"{pct:>+6.1f}% {dpct:>+6.1f}%  {sig:>+5.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
