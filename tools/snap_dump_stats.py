#!/usr/bin/env python3
"""Summarise a CZ_VK_SNAP_DUMP directory: one line per resolve snapshot.

WHY THIS EXISTS
---------------
`CZ_VK_SNAP_DUMP` (and `CZ_VK_SNAP_ON_BLACK`, which triggers it on the frame the
picture died) writes EVERY resolve snapshot of a frame as a PPM. That is the whole
point of the instrument — a wrong final frame is consistent with every pass being wrong
and with exactly one being wrong, and only the per-pass pictures separate those — but a
single frame of this title dumps well over a hundred surfaces, and "open 132 images and
look" is not an analysis anyone repeats. Worse, it is exactly the failure mode gotcha
133 names: looking at one picture is ONE SAMPLE, and eyeballing a directory of them
invites a conclusion drawn from whichever three you happened to open.

So this prints the numbers instead, and the ordering is the analysis: surfaces are
grouped by frame and sorted by address, because this title's post chain is a REDUCTION —
a 1280x720 scene resolve, then a ladder of small surfaces down to a couple of pixels —
and a chain that breaks somewhere breaks at a specific link. Reading the `lit%` and
`mean` columns down the ladder says which link.

WHAT THE COLUMNS MEAN
---------------------
  lit%     fraction of pixels with any non-zero channel. The trigger's own metric.
  mean     mean luminance 0..255 over all pixels.
  max      the brightest single channel value in the surface. A saturated post surface
           (max 255 on a small reduction target) is the signature of a value that
           overflowed the 8-bit UNORM our EDRAM stand-in uses for every format,
           including the k_16_FLOAT surfaces this title's luminance chain wants.
  colours  distinct RGB triples. 1 means the surface is a flat fill, which for a
           reduction target is either a correct average or a dead pass, and the mean
           column is what tells those apart.

USAGE
    tools/snap_dump_stats.py <dir> [--frame N] [--min-lit PCT] [--sort addr|lit|size]
"""

import argparse
import collections
import os
import re
import sys

NAME = re.compile(r"^f(\d+)_snap_([0-9A-Fa-f]+)_(\d+)x(\d+)\.ppm$")


def read_ppm(path):
    """Return (width, height, bytes) for a binary P6 PPM, or None."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None
    # Header is three whitespace-separated integers after the magic, with '#' comments.
    fields, i = [], 2
    while len(fields) < 3 and i < len(data):
        c = data[i : i + 1]
        if c == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
        elif c.isspace():
            i += 1
        else:
            start = i
            while i < len(data) and not data[i : i + 1].isspace():
                i += 1
            fields.append(int(data[start:i]))
    i += 1  # the single whitespace byte after maxval
    w, h, _maxval = fields
    return w, h, data[i : i + w * h * 3]


def stats(pix):
    lit = 0
    total = 0
    peak = 0
    colours = set()
    n = len(pix) // 3
    for p in range(0, n * 3, 3):
        r, g, b = pix[p], pix[p + 1], pix[p + 2]
        if r or g or b:
            lit += 1
        # Rec.601 luma, integer, because the absolute scale does not matter here and a
        # float accumulator over 900k pixels is the slow part of this script.
        total += (77 * r + 150 * g + 29 * b) >> 8
        if r > peak:
            peak = r
        if g > peak:
            peak = g
        if b > peak:
            peak = b
        colours.add((r, g, b))
        if len(colours) > 100000:      # cap: past this the count is "many"
            colours.add(None)
    return (100.0 * lit / n if n else 0.0,
            total / n if n else 0.0,
            peak,
            len(colours))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--frame", type=int, help="only this frame number")
    ap.add_argument("--min-lit", type=float, default=None,
                    help="only surfaces at least this %% lit — the fast way to ask "
                         "'did ANY pass in this frame have a picture in it'")
    ap.add_argument("--sort", choices=["addr", "lit", "size"], default="addr")
    args = ap.parse_args()

    by_frame = collections.defaultdict(list)
    for name in sorted(os.listdir(args.directory)):
        m = NAME.match(name)
        if not m:
            continue
        frame, addr, w, h = int(m.group(1)), m.group(2).upper(), int(m.group(3)), int(m.group(4))
        if args.frame is not None and frame != args.frame:
            continue
        by_frame[frame].append((addr, w, h, os.path.join(args.directory, name)))

    if not by_frame:
        print("no snapshot PPMs matched (expected f<frame>_snap_<addr>_<w>x<h>.ppm)",
              file=sys.stderr)
        return 1

    for frame in sorted(by_frame):
        rows = []
        for addr, w, h, path in by_frame[frame]:
            got = read_ppm(path)
            if not got:
                continue
            pw, ph, pix = got
            lit, mean, peak, colours = stats(pix)
            if args.min_lit is not None and lit < args.min_lit:
                continue
            rows.append((addr, pw, ph, lit, mean, peak, colours))
        if args.sort == "lit":
            rows.sort(key=lambda r: -r[3])
        elif args.sort == "size":
            rows.sort(key=lambda r: -(r[1] * r[2]))
        print(f"\n=== frame {frame}: {len(rows)} snapshot(s) shown "
              f"of {len(by_frame[frame])} dumped ===")
        print(f"{'address':>10} {'extent':>11} {'lit%':>8} {'mean':>7} {'max':>4} "
              f"{'colours':>8}")
        for addr, w, h, lit, mean, peak, colours in rows:
            print(f"{addr:>10} {f'{w}x{h}':>11} {lit:8.3f} {mean:7.2f} {peak:4d} "
                  f"{colours:8d}")
        lit_any = [r for r in rows if r[3] >= 1.0]
        print(f"-- {len(lit_any)} of {len(rows)} surfaces are at least 1% lit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
