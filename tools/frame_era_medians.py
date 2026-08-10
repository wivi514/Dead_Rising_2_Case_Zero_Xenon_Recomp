#!/usr/bin/env python3
"""The OUTDOOR picture A/B: era medians, quoted against a null measured in the same block.

WHY THIS EXISTS
---------------
The matched-frame filter this project uses for picture A/Bs — two frames are comparable
only where `drawFingerprint` AND `cameraFingerprint` agree — cannot be satisfied in this
title's outdoor era at all. Part 26 measured the null: two runs of ONE configuration on
the DebugJump route share 422 of 13,056 frames, none above 141 draws, and ZERO of the
12,174 frames above 1,800 draws (tools/frame_determinism.py, gotcha 254). A crowd of
animated actors never renders the same draw list twice, so exact equality selects for
stasis and the filter's n is zero by construction.

What the same two runs DO reproduce is the era aggregate. Over the 12,000+ frames above
1,800 draws, medians of the per-frame statistics agree to ~1%, which is a noise floor, and
a noise floor is what makes a difference readable. That is the prescription this title has
had since phase 5 (gotcha 38: aggregate over the era, NEVER on frame index); what was
missing was a null for it outdoors.

HOW TO USE IT
-------------
Run every arm in ONE serial block, including two runs of the baseline, and pass them all:

    frame_era_medians.py --null base1.txt base2.txt --arm nocube.txt --arm nosnap.txt

The null pair's disagreement is printed first, per statistic, and every arm's difference
from the baseline is quoted as a MULTIPLE of it. An arm inside 1x the null has not been
shown to do anything; that is a null result, not an absence of effect (gotcha 249).

CAVEATS worth reading before quoting a number
---------------------------------------------
* One null pair gives ONE sample of the noise floor. Three runs an arm is better and the
  same rule applies as everywhere else: alternate the arms within the block.
* `coveragePct` saturates outdoors (99.67%) and cannot report anything. It is printed so
  that is visible rather than assumed.
* A blur preserves every statistic here exactly (gotcha 135). For sharpness use
  tools/frame_sharpness.py on pixel dumps; this tool cannot see it.
"""
import argparse
import statistics
import sys

# The frame-stats columns this reads, by name. Keep in step with the header
# CZ_VK_FRAME_STATS writes.
COLS = {'coveragePct': 7, 'meanLuma': 8, 'distinctColours': 9}


def era(path, min_draws):
    vals = {k: [] for k in COLS}
    n = 0
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        p = line.split()
        if len(p) < 10:
            continue
        try:
            if int(p[1]) < min_draws:
                continue
            for k, i in COLS.items():
                vals[k].append(float(p[i]))
        except ValueError:
            continue
        n += 1
    return n, {k: (statistics.median(v) if v else float('nan')) for k, v in vals.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--null', nargs=2, required=True,
                    metavar=('BASE1', 'BASE2'),
                    help='two runs of ONE configuration — the baseline and its own noise')
    ap.add_argument('--arm', action='append', default=[], metavar='FILE')
    ap.add_argument('--min-draws', type=int, default=1800,
                    help='the era floor (default 1800 = outdoors on this title)')
    args = ap.parse_args()

    runs = [(p, *era(p, args.min_draws)) for p in list(args.null) + args.arm]
    for path, n, _ in runs:
        print(f"{path}: {n} frames >= {args.min_draws} draws")
    if min(r[1] for r in runs) == 0:
        print("an arm has no frames in the era — nothing to compare", file=sys.stderr)
        return 1

    (_, _, b1), (_, _, b2) = runs[0][0:3], runs[1][0:3]
    print(f"\n{'statistic':<18} {'base1':>12} {'base2':>12} {'null':>8}")
    null = {}
    for k in COLS:
        base = (b1[k] + b2[k]) / 2.0
        null[k] = abs(b1[k] - b2[k]) / base if base else float('nan')
        print(f"{k:<18} {b1[k]:>12.4f} {b2[k]:>12.4f} {100 * null[k]:>7.2f}%")

    for path, n, v in runs[2:]:
        print(f"\n{path}  ({n} frames)")
        for k in COLS:
            base = (b1[k] + b2[k]) / 2.0
            d = abs(v[k] - base) / base if base else float('nan')
            # A null of exactly zero is two runs that happened to agree to the printed
            # precision, NOT a statistic with infinite resolving power. Dividing by it
            # would print a spectacular multiple of nothing — the denominator error this
            # project keeps meeting (gotcha 246), in the one place a tool can commit it
            # on your behalf.
            if null[k] <= 0.0:
                verdict = "no null to divide by — two base runs agreed exactly"
            else:
                mult = d / null[k]
                verdict = ("INSIDE the null" if mult <= 1.0 else f"{mult:.1f}x the null")
            print(f"  {k:<18} {v[k]:>12.4f}   {100 * d:>6.2f}% from base   {verdict}")

    # ONE null pair is ONE sample of the noise floor, and on this title the floor itself
    # moves: two independent pairs measured 0.94% and 0.55% on mean luma and 0.76% and
    # 0.12% on distinct colours. A multiple quoted against the smaller of those is six
    # times the multiple quoted against the larger, so an effect under ~3x is not a
    # result yet — run a third baseline before believing one (gotchas 50/51/86).
    print("\nNB one null pair is one sample of the floor; on this title independent pairs\n"
          "   have differed by 6x on distinct colours. Treat anything under ~3x as\n"
          "   unresolved and run a third baseline run.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
