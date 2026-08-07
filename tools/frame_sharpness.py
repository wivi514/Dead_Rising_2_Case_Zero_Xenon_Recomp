#!/usr/bin/env python3
"""Measure how SHARP a set of dumped frames is, per arm, as a median over an era.

WHY THIS EXISTS
---------------
Phase C part 13 listed "the whole frame is uniformly out of focus at every depth" as
the largest visible difference from capture E3, and no instrument in this project could
see it. Every metric here aggregates over pixel VALUES — coverage, mean luminance,
distinct colours, a pixel hash — and a blur preserves all of them almost exactly:
gotcha 135's lesson about a vertical flip, in a second disguise. `frame_compare.py`
scored the blurred and the sharp arm at 99.61% and 99.62% coverage, 0.01 pp apart,
inside its own 1.5 pp band. The operator saw the difference instantly.

A blur is a statement about the SPATIAL DERIVATIVE, so measure that. Mean |gradient|
over the luminance channel separates part 14's two arms by a factor of six with no
overlap:

    CZ_VK_NO_DEPTH_RESOLVE=1 (blurred) : 1.242, 1.259
    depth resolves honoured  (sharp)   : 7.671, 7.622

It is also the metric that would have caught the defect the moment it appeared, which
is the real argument for keeping it: the depth-of-field pass had been compositing at
full strength over the entire frame since phase 5 because our resolve handed it the
colour buffer where it asked for depth, and five phases of renderer A/Bs went past it.

WHAT IT DOES NOT DO
-------------------
It cannot be compared frame-to-frame across runs. This title's title screen renders a
live animated 3D background (gotcha 127), so one frame is one sample of a moving
camera; the number to compare is the MEDIAN over every dumped frame of one era, the
same rule frame_compare.py follows for coverage (gotcha 38, gotcha 130). Pass the stats
file so the era can be selected by coverage rather than by frame index.

USAGE
    frame_sharpness.py <dumpdir> [<dumpdir> ...] [--stats a.txt b.txt]
                       [--min-coverage 90]

    frame_sharpness.py /tmp/ab_ctl1 /tmp/ab_new1 \
        --stats /tmp/ab_ctl1.txt /tmp/ab_new1.txt
"""

import argparse
import glob
import os
import statistics
import struct
import sys


def read_ppm_gray(path):
    """Read a binary P6 PPM and return (width, height, luminance bytes).

    Written by hand rather than through PIL because every other tool in this
    directory runs on a bare python3, and a metric that needs an install is a
    metric that gets skipped.
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM")
    # Three whitespace-separated fields after the magic, then exactly one
    # whitespace byte before the pixels.
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":                      # comments are legal
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _maxval = fields
    px = data[i : i + w * h * 3]
    # Rec.601 luma, integer, because the absolute scale does not matter — only the
    # ratio between two arms measured the same way.
    lum = bytearray(w * h)
    for k in range(w * h):
        r, g, b = px[3 * k], px[3 * k + 1], px[3 * k + 2]
        lum[k] = (77 * r + 150 * g + 29 * b) >> 8
    return w, h, lum


def sharpness(path):
    """Mean absolute first difference, horizontally and vertically, averaged.

    A blur is a low-pass filter, so it suppresses this and leaves every
    value-histogram statistic alone. Sampled on a stride rather than over every
    pixel: the quantity is an average over ~900,000 pixels and a 4-pixel stride
    changes the third decimal while making a pure-python pass tolerable.
    """
    w, h, lum = read_ppm_gray(path)
    stride = 4
    total, n = 0, 0
    for y in range(0, h - 1, stride):
        row = y * w
        nxt = row + w
        for x in range(0, w - 1, stride):
            c = lum[row + x]
            total += abs(lum[row + x + 1] - c) + abs(lum[nxt + x] - c)
            n += 2
    return total / max(n, 1)


def coverage_by_frame(stats_path):
    """frame index -> presented-frame coverage %, out of CZ_VK_FRAME_STATS."""
    out = {}
    with open(stats_path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) > 7:
                out[int(parts[0])] = float(parts[7])
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+", help="CZ_VK_FRAME_DUMP directories, one per arm")
    ap.add_argument("--stats", nargs="*", default=[],
                    help="the matching CZ_VK_FRAME_STATS files, in the same order")
    ap.add_argument("--min-coverage", type=float, default=90.0,
                    help="only frames whose presented coverage is at least this "
                         "(default 90 — the era with a scene in it)")
    args = ap.parse_args()

    if args.stats and len(args.stats) != len(args.dirs):
        sys.exit("--stats must name one file per directory, in the same order")

    print(f"{'arm':<28} {'frames':>7} {'median':>9} {'min':>9} {'max':>9}")
    medians = []
    for i, d in enumerate(args.dirs):
        cov = coverage_by_frame(args.stats[i]) if args.stats else None
        vals = []
        for p in sorted(glob.glob(os.path.join(d, "*.ppm"))):
            if cov is not None:
                try:
                    n = int(os.path.basename(p).split("_")[-1].split(".")[0])
                except ValueError:
                    continue
                if cov.get(n, 0.0) < args.min_coverage:
                    continue
            vals.append(sharpness(p))
        if not vals:
            print(f"{os.path.basename(d):<28} {0:>7}   no frames above the "
                  f"coverage floor — is --min-coverage right for this era?")
            continue
        med = statistics.median(vals)
        medians.append(med)
        print(f"{os.path.basename(d):<28} {len(vals):>7} {med:>9.3f} "
              f"{min(vals):>9.3f} {max(vals):>9.3f}")

    if len(medians) >= 2:
        lo, hi = min(medians), max(medians)
        print()
        print(f"spread: {lo:.3f} .. {hi:.3f}  ({hi / max(lo, 1e-9):.2f}x)")
        # No quoted band, deliberately (gotcha 132): the honest comparison is two
        # runs per arm, so the within-arm spread is measured from the same runs.
        print("Read it against the WITHIN-arm spread of your own two runs, not "
              "against a constant.")


if __name__ == "__main__":
    main()
