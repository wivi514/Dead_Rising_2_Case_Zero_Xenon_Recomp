#!/usr/bin/env python3
"""Read a part-47 performance A/B: frame time by draw bin AND the profiler's phase shares.

WHY THIS EXISTS ON TOP OF `part46_perf_read.py`. That reader hard-codes its two arm
names, and part 47 has at least eight arms to run (`docs/perf-plan-part47.md` §7), so
the arms are arguments here. More importantly it reads ONLY frame time, and the plan's
own measurement rule says frame time is the wrong statistic for anything below ~10%:

    "`CZ_VK_PROFILE` phase shares, not frame time, for anything below ~10%. The
    frame-time A/B needed three runs an arm and still landed inside its floor in every
    bin; the phase share separates arms the frame time cannot."

The reason is structural, not statistical. This title paces itself to a two-vblank
floor, so a CPU saving converts to frame time ONLY where the frame is above one floor
and within reach of the next; below that it is absorbed and the frame time is flat
(gotchas 237/238). The phase share is measured inside the frame and is not absorbed --
if `textures` goes from 41% to 25% of the frame, that happened whether or not the frame
got shorter.

So this prints, per arm:
  * the MEDIAN phase share across every profile window of every run, per phase, with
    the within-arm spread -- the primary reading;
  * the median frame time and PINNED share by draw bin -- the secondary one, quoted
    against the within-arm spread that is its noise floor.

Usage: part47_perf_read.py <dir> [armA armB ...]      # default: base, and every other
                                                      # arm tag found in the directory
"""
import sys, os, re, glob, statistics

VBLANK_MS = 16.0
BINS = [(0, 500), (500, 1500), (1500, 3000), (3000, 5000), (5000, 8000), (8000, 10**9)]

# `[vkprof] 26.0 fps (38.5 ms/frame, 4565 draws/frame) | draw 64.5% [constants 2.3
#  streams 0.0 textures 40.7 record 14.6 other 6.8] submit 0.2% [call 0.2 gpu 0.0]
#  readback 1.0% outside 34.4%`
#
# Parsed with one generic "<name> <number>" sweep rather than a fixed field list,
# because the line has gained columns three times in this project's history and a
# reader that names them all is a reader that silently drops the newest one.
PROF = re.compile(r'^\[vkprof\] ([\d.]+) fps \(([\d.]+) ms/frame, (\d+) draws/frame\)(.*)$')
FIELD = re.compile(r'([a-z-]+) ([\d.]+)%?')


def load_stats(path):
    """(draws, frame_ms) per presented frame. The msec column is a CUMULATIVE
    timestamp, so a frame's cost is the difference from the previous one; a gap over
    2 s is a load or a clock step rather than a frame."""
    out, prev = [], None
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.split()
            if len(p) < 5:
                continue
            try:
                msec, draws = float(p[0]), int(p[1])
            except ValueError:
                continue
            if prev is not None:
                d = msec - prev
                if 0 < d < 2000:
                    out.append((draws, d))
            prev = msec
    return out


def load_prof(path):
    """One dict of phase -> percent per CZ_VK_PROFILE window, plus fps/ms/draws.

    Windows are kept individually rather than averaged, because a run walks through
    eras of wildly different draw counts and an average over the run is an average over
    the route (the same error gotcha 242 names one level up)."""
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            m = PROF.match(line.strip())
            if not m:
                continue
            row = {'fps': float(m.group(1)), 'ms': float(m.group(2)),
                   'draws': int(m.group(3))}
            for name, val in FIELD.findall(m.group(4)):
                row[name] = float(val)
            out.append(row)
    return out


def pinned(ms_list):
    if not ms_list:
        return 0.0
    n = sum(1 for m in ms_list
            if abs(m - round(m / VBLANK_MS) * VBLANK_MS) <= 1.0 and m >= VBLANK_MS - 1)
    return 100.0 * n / len(ms_list)


def summarise(runs, lo, hi):
    meds, pins, counts = [], [], []
    for r in runs:
        sel = [ms for d, ms in r if lo <= d < hi]
        if len(sel) < 20:
            continue
        meds.append(statistics.median(sel))
        pins.append(pinned(sel))
        counts.append(len(sel))
    return meds, pins, counts


def spread(vals):
    """The within-arm range as a percent of the median -- the noise floor this
    comparison has to beat. Zero runs and one run both mean 'unmeasured', and printing
    0.0% for those would manufacture confidence the run count cannot support."""
    if len(vals) < 2:
        return None
    med = statistics.median(vals)
    return (max(vals) - min(vals)) / med * 100 if med else None


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        '~/DR2CZ-troubleshooting/part47/perf')
    arms = sys.argv[2:]
    if not arms:
        tags = set()
        for p in glob.glob(os.path.join(d, '*.stats')):
            tags.add(os.path.basename(p).rsplit('_', 1)[0])
        arms = (['base'] if 'base' in tags else []) + sorted(tags - {'base'})
    if not arms:
        print('no .stats files in', d)
        return 2

    stats = {a: [load_stats(p) for p in sorted(glob.glob(os.path.join(d, a + '_*.stats')))]
             for a in arms}
    profs = {a: [load_prof(p) for p in sorted(glob.glob(os.path.join(d, a + '_*.log')))]
             for a in arms}
    for a in arms:
        peaks = [max((x[0] for x in r), default=0) for r in stats[a]]
        print('%-14s %d runs, frames %s, peak draws %s, profile windows %s'
              % (a, len(stats[a]), [len(r) for r in stats[a]], peaks,
                 [len(w) for w in profs[a]]))

    # --- the PRIMARY reading: phase shares -------------------------------------------
    # Restricted to windows above 1,500 draws: below that the frame is at the pacing
    # floor and every arm ties, so including the safehouse era dilutes the very
    # measurement it is being included for.
    print('\nCZ_VK_PROFILE phase shares, windows above 1,500 draws/frame')
    print('  (median over windows, one column per run; the spread across runs is the floor)')
    phases = ['draw', 'textures', 'record', 'streams', 'constants', 'other',
              'readback', 'outside', 'submit']
    print('  %-11s %-10s %-26s %-9s %s' % ('phase', 'arm', 'median % per run',
                                           'overall', 'within-arm spread'))
    for ph in phases:
        rows = []
        for a in arms:
            per_run = []
            for w in profs[a]:
                vals = [r[ph] for r in w if r.get('draws', 0) >= 1500 and ph in r]
                if vals:
                    per_run.append(statistics.median(vals))
            rows.append((a, per_run))
        if not any(r[1] for r in rows):
            continue
        label = ph
        for a, per_run in rows:
            if not per_run:
                print('  %-11s %-10s (no windows)' % (label, a))
                label = ''
                continue
            sp = spread(per_run)
            print('  %-11s %-10s %-26s %-9.2f %s'
                  % (label, a, ' '.join('%.2f' % v for v in per_run),
                     statistics.median(per_run),
                     'unmeasured (<2 runs)' if sp is None else '%.1f%%' % sp))
            label = ''
        base = rows[0]
        if base[1]:
            bmed = statistics.median(base[1])
            for a, per_run in rows[1:]:
                if not per_run:
                    continue
                amed = statistics.median(per_run)
                sb, sa = spread(base[1]), spread(per_run)
                floor = max([x for x in (sb, sa) if x is not None], default=None)
                delta = (amed - bmed) / bmed * 100 if bmed else 0.0
                if floor is None:
                    verdict = 'NO VERDICT — fewer than 2 runs an arm'
                else:
                    verdict = ('INSIDE the %.1f%% floor — not shown to do anything'
                               % floor if abs(delta) <= floor
                               else 'outside the %.1f%% floor' % floor)
                print('  %-11s %-10s %s vs %s: %+.1f%% -> %s'
                      % ('', '=>', a, base[0], delta, verdict))
        print()

    # --- the SECONDARY reading: frame time by draw bin --------------------------------
    print('frame time by draw bin (medians; the pacing floor absorbs small savings)')
    print('  %-13s %-10s %-26s %-18s' % ('draw bin', 'arm', 'MEDIAN ms per run',
                                         'PINNED % per run'))
    for lo, hi in BINS:
        rows = [(a,) + summarise(stats[a], lo, hi) for a in arms]
        if not any(r[1] for r in rows):
            continue
        label = '%d-%s' % (lo, '' if hi > 10**8 else hi)
        for a, meds, pins, counts in rows:
            if not meds:
                print('  %-13s %-10s (no bin)' % (label, a))
                label = ''
                continue
            sp = spread(meds)
            print('  %-13s %-10s %-26s %-18s n=%s  spread %s'
                  % (label, a, ' '.join('%.1f' % m for m in meds),
                     ' '.join('%.0f' % p for p in pins), counts,
                     'unmeasured' if sp is None else '%.1f%%' % sp))
            label = ''
        bmeds = rows[0][1]
        if bmeds:
            bmed = statistics.median(bmeds)
            for a, meds, _, _ in rows[1:]:
                if not meds:
                    continue
                amed = statistics.median(meds)
                floor = max([x for x in (spread(bmeds), spread(meds)) if x is not None],
                            default=None)
                delta = (amed - bmed) / bmed * 100
                if floor is None:
                    verdict = ('NO VERDICT — fewer than 2 runs an arm (the documented '
                               'one-run-a-side floor on this workload is 10-13%)')
                else:
                    verdict = ('INSIDE the %.1f%% floor' % floor if abs(delta) <= floor
                               else 'outside the %.1f%% floor' % floor)
                print('  %-13s %-10s %s vs %s: %+.1f%% median -> %s'
                      % ('', '=>', a, rows[0][0], delta, verdict))
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
