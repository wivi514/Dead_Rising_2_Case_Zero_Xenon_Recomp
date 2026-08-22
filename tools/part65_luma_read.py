#!/usr/bin/env python3
"""Median outdoor/scene meanLuma per CZ_VK_FRAME_STATS file — WITH A COMPLETENESS GATE.

WHY THE GATE EXISTS, and why it is refusal rather than a warning: part 65 built three
separate conclusions on partial reads of these files and had to retract all three. The
worst was `CZ_VK_RT_FACTOR_DEBUG=6`, which read **13.49 at n=383** and **100.59 at
n=2,924** — the difference between "the whole chain works" and "nothing works at all",
from the same arm. A frame-stats file is appended to while the run continues, so a read
taken early is a measurement of a DIFFERENT PLACE in the boot, not a noisier version of
the same one (gotcha 384).

So this tool will not print a median for an arm whose frame count is below `--min-frac`
of the control's. It prints REFUSED and the counts instead, which is a result a reader
cannot mistake for a number.

USAGE
    part65_luma_read.py <dir> --control fs_off.txt [--min-draws 300] [--min-frac 0.85]
"""
import argparse
import os
import statistics
import sys


def read(path, li, di, min_draws):
    v = []
    with open(path) as f:
        for line in list(f)[1:]:
            p = line.split()
            if len(p) <= li:
                continue
            try:
                d, l = float(p[di]), float(p[li])
            except ValueError:
                continue
            if d >= min_draws:
                v.append(l)
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--control', default='fs_off.txt')
    ap.add_argument('--min-draws', type=int, default=300)
    ap.add_argument('--min-frac', type=float, default=0.85)
    a = ap.parse_args()

    ctl = os.path.join(a.dir, a.control)
    hdr = open(ctl).readline().split()[1:]      # the leading '#' is not a column
    li, di = hdr.index('meanLuma'), hdr.index('draws')
    base = read(ctl, li, di, a.min_draws)
    if not base:
        print('control has no frames above %d draws' % a.min_draws, file=sys.stderr)
        return 1
    bm = statistics.median(base)
    print('control %-18s n=%-6d median=%.2f' % (a.control, len(base), bm))
    bad = 0
    for name in sorted(os.listdir(a.dir)):
        if not name.startswith('fs_') or not name.endswith('.txt') or name == a.control:
            continue
        v = read(os.path.join(a.dir, name), li, di, a.min_draws)
        if len(v) < a.min_frac * len(base):
            print('  %-18s REFUSED — n=%d against the control\'s %d (%.0f%%); a partial '
                  'read measures a different PLACE, not a noisier same place'
                  % (name, len(v), len(base), 100.0 * len(v) / len(base)))
            bad += 1
            continue
        m = statistics.median(v)
        print('  %-18s n=%-6d median=%7.2f   %+6.2f vs control' % (name, len(v), m, m - bm))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
