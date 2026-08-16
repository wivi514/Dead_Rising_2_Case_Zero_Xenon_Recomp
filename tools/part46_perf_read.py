#!/usr/bin/env python3
"""Read a part-46 shader-cache performance A/B the way this title has to be read.

WHY THIS EXISTS, and why `tools/frame_perf_bins.py` is not enough on its own: it
reports MEANS, and a mean on Dead Rising 2: Case Zero measures the title's own
two-vblank pacing floor rather than your change. Gotcha 237 has the worked example --
the cross-frame stream store scored +1.7% against a +1.3% null on means, while the same
data read as medians was 44 ms -> 32 ms, and the statistic that actually moved was the
share of frames pinned to a 16 ms multiple (10% -> 97%). So this reads:

  * the MEDIAN frame time, per draw-count bin (a frame's cost is dominated by its draw
    count, and the two arms do not visit the same places at the same times);
  * the PINNED share -- frames within 1 ms of a multiple of the 16 ms pacing period,
    i.e. frames the guest's pacing is holding rather than frames we are slow on;
  * and, first, the WITHIN-ARM spread across the three runs of each arm, which is the
    noise floor this comparison has to beat. Quoting an arm difference without it is
    the mistake gotcha 229 names: the floor here is 10-13% at one run a side.

Usage: part46_perf_read.py <dir>            # expects fixed_N.stats / pre45_N.stats
"""
import sys, os, glob, statistics

# 16 rather than 16.67: the msec column is integer milliseconds, so a 16.67
# period spends the tolerance on the timestamp's quantisation rather than on the
# frame. Same constant tools/frame_perf_bins.py defaults to.
VBLANK_MS = 16.0
BINS = [(0, 500), (500, 1500), (1500, 3000), (3000, 5000), (5000, 8000), (8000, 10**9)]


def load(path):
    """(draws, frame_ms) per presented frame.

    The `msec` column is a CUMULATIVE timestamp, not a duration, so a frame's cost is
    the difference from the previous presented frame's -- the same reading
    tools/frame_perf_bins.py uses, and getting it wrong the first time here produced
    median 'frame times' of 590,000 ms, which is the kind of number that says the
    column is not what you assumed rather than that the run was slow. The first row of
    a file has no predecessor, and a gap over 2 s is a load or a clock step rather than
    a frame."""
    out, prev = [], None
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.split()
            if len(p) < 18:
                continue
            try:
                draws, ms = int(p[1]), int(p[17])
            except ValueError:
                continue
            if prev is not None:
                dt = ms - prev
                if 0 < dt <= 2000:
                    out.append((draws, float(dt)))
            prev = ms
    return out


def pinned(ms_list):
    """Share of frames within 1 ms of a pacing multiple -- the decisive statistic."""
    if not ms_list:
        return float('nan')
    n = sum(1 for m in ms_list
            if abs(m - round(m / VBLANK_MS) * VBLANK_MS) <= 1.0 and m >= VBLANK_MS - 1.0)
    return 100.0 * n / len(ms_list)


def summarise(runs, lo, hi):
    """Per-run medians in a bin, so the spread ACROSS runs of one arm is visible."""
    meds, pins, counts = [], [], []
    for r in runs:
        sel = [m for d, m in r if lo <= d < hi]
        if len(sel) < 30:            # too few frames to median honestly
            continue
        meds.append(statistics.median(sel))
        pins.append(pinned(sel))
        counts.append(len(sel))
    return meds, pins, counts


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        '~/DR2CZ-troubleshooting/part46/perf')
    arms = {}
    for arm in ('fixed', 'pre45'):
        paths = sorted(glob.glob(os.path.join(d, arm + '_*.stats')))
        arms[arm] = [load(p) for p in paths]
        peaks = [max((x[0] for x in r), default=0) for r in arms[arm]]
        print('%-6s %d runs, frames %s, peak draws %s'
              % (arm, len(paths), [len(r) for r in arms[arm]], peaks))
    print()
    print('  %-14s %-34s %-34s' % ('draw bin', 'MEDIAN ms per run', 'PINNED %% per run'))
    for lo, hi in BINS:
        row = []
        for arm in ('fixed', 'pre45'):
            meds, pins, counts = summarise(arms[arm], lo, hi)
            row.append((arm, meds, pins, counts))
        if not any(r[1] for r in row):
            continue
        label = '%d-%s' % (lo, '' if hi > 10**8 else hi)
        for arm, meds, pins, counts in row:
            if not meds:
                print('  %-14s %-6s (no bin)' % (label, arm))
                continue
            spread = (max(meds) - min(meds)) / statistics.median(meds) * 100 if len(meds) > 1 else 0
            print('  %-14s %-6s %-34s %-28s  n=%s  within-arm spread %.1f%%'
                  % (label, arm,
                     ' '.join('%.1f' % m for m in meds),
                     ' '.join('%.0f' % p for p in pins),
                     counts, spread))
            label = ''
        # The comparison, quoted against the within-arm spread rather than on its own.
        fm = row[0][1]
        pm = row[1][1]
        if fm and pm:
            fmed, pmed = statistics.median(fm), statistics.median(pm)
            delta = (fmed - pmed) / pmed * 100
            if len(fm) < 2 or len(pm) < 2:
                # With one run an arm there IS no measured floor, and printing 0.0%
                # would turn every difference into "outside the floor" -- a tool
                # manufacturing the confidence the run count cannot support. The
                # documented floor on this workload is 10-13% at one run a side
                # (gotcha 229), which is larger than most differences worth having.
                print('  %-14s %-6s fixed vs pre45: %+.1f%% median -- NO VERDICT, '
                      'fewer than 2 runs an arm in this bin (floor unmeasured; '
                      'the documented one-run-a-side floor is 10-13%%)'
                      % ('', '=>', delta))
            else:
                floor = max((max(fm) - min(fm)) / fmed, (max(pm) - min(pm)) / pmed) * 100
                verdict = ('INSIDE the noise floor -- not shown to do anything'
                           if abs(delta) <= floor else 'outside the floor')
                print('  %-14s %-6s fixed vs pre45: %+.1f%% median, noise floor %.1f%%  -> %s'
                      % ('', '=>', delta, floor, verdict))
        print()


if __name__ == '__main__':
    main()
