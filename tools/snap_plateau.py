#!/usr/bin/env python3
"""Is a surface's brightest population a PLATEAU or a distribution? — for CZ_VK_SNAP_DUMP.

WHY THIS EXISTS
---------------
The white-surface item (`docs/open-items.md` 00f) turns on one distinction that no
existing readout can make. `tools/snap_dump_stats.py` prints a surface's mean, max, lit%
and distinct-colour count, and those four numbers are identical for two completely
different pictures:

  * a surface where 15% of the pixels sit at ONE exact colour — a value pinned by a
    clamp somewhere upstream, which is what part 27 found at `rgb(180,180,180)`;
  * a surface whose bright pixels are spread over a dozen neighbouring values — an
    ordinary shaded surface that the tone curve happens to compress, which is what
    part 30 showed the same evidence is equally consistent with (gotcha 273: where
    `d(out)/dx` vanishes, a 10% spread in the input quantises to one 8-bit output).

Separating those is the whole of the open item, so it needs its own tool. This one
prints the top colours by population and, for a nominated value, how much of the
surface sits at EXACTLY that value versus within a few levels of it. A plateau shows a
single enormous bin with near-empty neighbours; a compressed distribution shows a hill.

WHY IT REPORTS NEIGHBOURS AND NOT JUST THE PEAK
-----------------------------------------------
A single peak count is not evidence on its own, because a compressed distribution ALSO
has a tallest bin. The discriminating statistic is the ratio of the peak to its
immediate neighbours: `rgb(180,180,180)` with `rgb(179,179,179)` and `rgb(181,181,181)`
essentially empty is a pin, and the same peak with neighbours at a third of its height
is a hill. Part 27's own number — "the next most common colour above luma 150 has TWO
pixels" — is exactly this statistic, computed by hand for one frame.

USAGE
    snap_plateau.py <snapdir> [--addr 0684B000] [--value 180] [--top 8]
    snap_plateau.py <dirA> <dirB> --addr 0684B000 --value 180      # two arms side by side
"""
import argparse
import sys
from collections import Counter
from pathlib import Path


def read_ppm(path):
    """P6 binary PPM -> (width, height, bytes). CZ_VK_SNAP_DUMP writes only P6."""
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        raise ValueError('%s is not a P6 PPM' % path)
    # Header fields are whitespace-separated and may carry # comments.
    fields, at = [], 2
    while len(fields) < 3:
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b'#':
            while at < len(data) and data[at] != 0x0A:
                at += 1
            continue
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        fields.append(int(data[start:at]))
    at += 1  # exactly one whitespace byte after maxval
    w, h, _maxval = fields
    return w, h, data[at:at + w * h * 3]


def surfaces(snapdir, addr):
    """Every dumped PPM in `snapdir` whose name carries `addr`, newest frame last.

    The dump names files `f%06u_snap_%08X_%ux%u%s.ppm`, so the address is field 3 and
    the frame number is in field 1. A directory can hold several frames when the
    trigger fired more than once; they are all returned rather than silently reduced
    to one, because "which frame" has been a load-bearing question in this project.
    """
    out = []
    for p in sorted(Path(snapdir).glob('*.ppm')):
        parts = p.name.split('_')
        if len(parts) >= 3 and parts[1] == 'snap' and parts[2].upper() == addr.upper():
            out.append(p)
    return out


def report(path, value, top):
    w, h, px = read_ppm(path)
    n = w * h
    if n == 0 or len(px) < n * 3:
        print('  %s: TRUNCATED (%u bytes for %ux%u)' % (path.name, len(px), w, h))
        return
    counts = Counter()
    for i in range(0, n * 3, 3):
        counts[px[i:i + 3]] += 1
    print('  %s  %ux%u = %u px' % (path.name, w, h, n))
    print('    top colours by population:')
    for rgb, c in counts.most_common(top):
        print('      rgb(%3u,%3u,%3u)  %9u  %6.2f%%'
              % (rgb[0], rgb[1], rgb[2], c, 100.0 * c / n))
    if value is None:
        return
    # The plateau test proper. Neighbours are the same grey one level either side —
    # that is where a compressed-but-real distribution would put its mass.
    print('    plateau test at rgb(%u,%u,%u):' % (value, value, value))
    peak = counts.get(bytes((value, value, value)), 0)
    for d in (-3, -2, -1, 0, 1, 2, 3):
        v = value + d
        if not 0 <= v <= 255:
            continue
        c = counts.get(bytes((v, v, v)), 0)
        ratio = ('peak' if d == 0
                 else ('%.4f' % (c / peak)) if peak else 'n/a')
        print('      %+d  rgb(%3u,%3u,%3u)  %9u  %6.3f%%   %s'
              % (d, v, v, v, c, 100.0 * c / n, ratio))
    # And the honest alternative reading: how much sits ANYWHERE near this luma. If the
    # peak is 15% of the frame and the +-3 band is 15.1%, it is a pin; if the band is
    # 40%, the peak is just the top of a hill.
    band = sum(counts.get(bytes((value + d,) * 3), 0) for d in range(-3, 4)
               if 0 <= value + d <= 255)
    print('    peak %.2f%% of surface; +-3 grey band %.2f%%; peak/band %.3f'
          % (100.0 * peak / n, 100.0 * band / n,
             (peak / band) if band else float('nan')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+', help='one or more CZ_VK_SNAP_DUMP directories')
    ap.add_argument('--addr', default='0684B000',
                    help='surface address (default 0684B000, the 1280x720 scene colour)')
    ap.add_argument('--value', type=int, default=180,
                    help='the grey level to test as a plateau (default 180)')
    ap.add_argument('--top', type=int, default=8)
    args = ap.parse_args()

    missing = 0
    for d in args.dirs:
        print('=== %s ===' % d)
        found = surfaces(d, args.addr)
        if not found:
            # Naming the absence rather than printing nothing: a snapshot directory with
            # no matching surface means the pass did not run in the dumped frame, which
            # is itself a finding and has been mistaken for "the arm made no difference".
            print('  no snapshot of %s in this directory (%u PPMs present)'
                  % (args.addr, len(list(Path(d).glob('*.ppm')))))
            missing += 1
            continue
        for p in found:
            report(p, args.value, args.top)
    return 1 if missing == len(args.dirs) else 0


if __name__ == '__main__':
    sys.exit(main())
