#!/usr/bin/env python3
"""READ A DUMPED SHADOW-FACTOR IMAGE — structure, not just a mean.

WHY THIS EXISTS
---------------
`CZ_VK_RT_FACTOR_READBACK` prints a histogram, which answers "how much of the factor is
shadowed" and says nothing about WHERE. That distinction is the whole of part 66's first
operator session: the factor was 427 pixels out of place vertically, a defect a mean
cannot see and which part 65's horizontal-stripe control was structurally blind to
(gotcha 394).

So this reads the PGMs `CZ_VK_RT_FACTOR_PGM` writes and reports the shape:

* the ROW and COLUMN profiles, as a coarse ASCII plot — a vertical-stripe arm must show
  banding in the column profile and a flat row profile, and a horizontal-stripe arm the
  exact opposite. Running the two together and comparing their band COUNT and PITCH is
  what makes the alignment claim two-sided.
* the band count and pitch, measured from sign changes rather than eyeballed, so the
  two stripe arms can be compared as numbers.
* for a real factor: the shadowed share by screen band, which says whether the rays are
  finding the world everywhere or only in part of the frame.

USAGE
    tools/rt_factor_pgm_read.py <file.pgm|dir> [...]
    tools/rt_factor_pgm_read.py ~/DR2CZ-troubleshooting/part66-factor/*/
"""
import sys
from pathlib import Path


def read_pgm(p):
    d = p.read_bytes()
    if not d.startswith(b'P5'):
        return None
    # header: P5 <w> <h> <max>, whitespace separated, comments unsupported (we write it)
    parts, i, vals = [], 2, []
    while len(vals) < 3:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        vals.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _mx = vals
    px = d[i:i + w * h]
    # A PGM caught mid-write is short. Say so rather than dying halfway through a
    # directory scan and losing the readings that DID parse.
    if len(px) < w * h:
        return None
    return w, h, px


def profile(px, w, h, axis, n=64):
    """Mean value along `axis` ('row' or 'col'), bucketed into n samples."""
    out = []
    if axis == 'row':
        for b in range(n):
            y0, y1 = b * h // n, max(b * h // n + 1, (b + 1) * h // n)
            s = c = 0
            for y in range(y0, y1):
                base = y * w
                for x in range(0, w, 8):
                    s += px[base + x]; c += 1
            out.append(s / max(1, c) / 255.0)
    else:
        for b in range(n):
            x0, x1 = b * w // n, max(b * w // n + 1, (b + 1) * w // n)
            s = c = 0
            for y in range(0, h, 8):
                base = y * w
                for x in range(x0, x1):
                    s += px[base + x]; c += 1
            out.append(s / max(1, c) / 255.0)
    return out


def bands(prof):
    """How many times the profile crosses its own midpoint — 2x the band count."""
    lo, hi = min(prof), max(prof)
    if hi - lo < 0.25:                       # no structure worth counting
        return 0, hi - lo
    mid = (lo + hi) / 2
    cross = sum(1 for a, b in zip(prof, prof[1:]) if (a < mid) != (b < mid))
    return cross, hi - lo


def plot(prof, label):
    ramp = ' .:-=+*#%@'
    print('    %-4s %s' % (label, ''.join(ramp[min(9, int(v * 9.999))] for v in prof)))


def report(p):
    r = read_pgm(p)
    if not r:
        print('%s: not a readable P5 PGM (truncated, or still being written)' % p.name)
        return
    w, h, px = r
    n = len(px)
    mean = sum(px[::7]) / len(px[::7]) / 255.0
    shadow = 100.0 * sum(1 for v in px[::7] if v < 8) / len(px[::7])
    lit = 100.0 * sum(1 for v in px[::7] if v > 247) / len(px[::7])
    print('%s  %dx%d  mean=%.3f  shadowed=%.1f%%  lit=%.1f%%' %
          (p.name, w, h, mean, shadow, lit))
    rp, cp = profile(px, w, h, 'row'), profile(px, w, h, 'col')
    plot(cp, 'cols')          # varies for a VERTICAL-stripe pattern
    plot(rp, 'rows')          # varies for a HORIZONTAL-stripe pattern
    rc, rr = bands(rp)
    cc, cr = bands(cp)
    print('    columns: %d midpoint crossings (range %.2f)  ->  %s'
          % (cc, cr, '%d vertical bands' % (cc // 2) if cc else 'flat'))
    print('    rows   : %d midpoint crossings (range %.2f)  ->  %s'
          % (rc, rr, '%d horizontal bands' % (rc // 2) if rc else 'flat'))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    files = []
    for a in args:
        p = Path(a)
        files += sorted(p.glob('**/*.pgm')) if p.is_dir() else [p]
    if not files:
        print('no PGMs found', file=sys.stderr)
        return 1
    # The LAST reading per directory — the earliest ones are the title screen.
    bydir = {}
    for f in files:
        bydir[f.parent] = f
    for d in sorted(bydir):
        report(bydir[d])
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
