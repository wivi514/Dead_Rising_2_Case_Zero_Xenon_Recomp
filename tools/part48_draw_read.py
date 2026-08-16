#!/usr/bin/env python3
"""Read a part-48 A/B on the DRAW path, in ns per DRAW, in a band narrow enough that the
statistic is flat inside it — and against a NULL-CONTROL arm that must read zero.

WHY THIS EXISTS ON TOP OF `part47_perf_read.py`. That reader quotes phase MILLISECONDS
in a 3,000-8,000 draw band, which is gotcha 321's fix and was a real improvement over
pooling the whole route. It is not enough for a per-draw statistic, and part 48 measured
by how much:

    base arm, `record` ns per draw, by draw bin
        3,000-4,000   1,204
        4,000-5,000   1,138
        5,000-6,000   1,074
        6,000-7,000   1,047
        7,000-8,000   1,033

**17% across the band that was supposed to be the control.** Per-frame costs amortise
over more draws, so a run that reaches denser scenery has a lower ns/draw for reasons
that have nothing to do with its arm — and the four arms of part 48's campaign populated
that band at medians from 4,669 to 5,544 draws. An arm that merely wandered read 8%
"faster".

THE FIX IS TWO THINGS, AND THE SECOND MATTERS MORE.

1. A narrow band (default 4,000-6,000). Necessary, not sufficient — no band is provably
   narrow enough on its own.
2. **A NULL-CONTROL ARM: one whose change cannot possibly move the statistic.** In part
   48's campaign that is `CZ_PM4_ATOMIC_COUNTERS=1`, which touches only the PM4 command
   processor and so cannot move `record` by any mechanism. Whatever it reads IS the noise
   floor of this measurement, measured rather than assumed (gotcha 331). It read -5% in
   the wide band and 0.5-2.3% in the narrow one, which is how the band width was chosen
   and how the surviving 16.7% effect was known to be eight times the floor.

So: pass `--null <arm>` and every other arm is quoted against what that one reads. An arm
with no null control gets its numbers printed and a warning, because a percentage with no
floor beside it is not a result.

Usage:
  part48_draw_read.py <dir> [--null ARM] [--lo 4000] [--hi 6000]
"""
import sys, os, re, glob, statistics, argparse

PROF = re.compile(r'^\[vkprof\] ([\d.]+) fps \(([\d.]+) ms/frame, (\d+) draws/frame\)')
# `[vkprof] record 20.1% = state 2.2 + vertex 10.8 + index 4.1 + residual 3.0  |
#  per draw: 1274 ns = 138 + 684 + 261 + 191`
REC = re.compile(r'^\[vkprof\] record .*per draw: ([\d.]+) ns = ([\d.]+) \+ ([\d.]+) \+ '
                 r'([\d.]+) \+ ([\d.]+)')
# `[vkprof] other 571 ns/draw = shader 40 + key 37 + pipeline 116 + begin 12 + fetch 119
#  + tail 60 + residual 187 (9.4% of frame)`  -- part 48's split of `other`
OTH = re.compile(r'^\[vkprof\] other ([\d.]+) ns/draw = shader ([\d.]+) \+ key ([\d.]+) '
                 r'\+ pipeline ([\d.]+) \+ begin ([\d.]+) \+ fetch ([\d.]+) \+ tail '
                 r'([\d.]+) \+ residual ([\d.]+)')
RECORD_FIELDS = ['record', 'rec.state', 'rec.vertex', 'rec.index', 'rec.residual']
OTHER_FIELDS = ['other', 'oth.shader', 'oth.key', 'oth.pipeline', 'oth.begin',
                'oth.fetch', 'oth.tail', 'oth.residual']


def arm_logs(d, arm):
    """Every log belonging to `arm`. A headless campaign writes `<arm>_1.log` per run; an
    OPERATOR session writes a single `<arm>.log`. Both are real inputs to these readers,
    and a reader that globs only `<arm>_*.log` reports "no windows in band" for an
    operator session -- a zero from a check that could not have matched (gotcha 25)."""
    return sorted(glob.glob(os.path.join(d, f'{arm}_*.log')) +
                  glob.glob(os.path.join(d, f'{arm}.log')))


def windows(path, lo, hi):
    """{field: value} per profile window whose draws/frame is in [lo, hi). The `record`
    and `other` lines are attributed to the most recent frame line — the profiler emits
    them as one block, and that is the window they were differenced over."""
    out, draws, cur = [], None, None
    for line in open(path, errors='replace'):
        m = PROF.match(line)
        if m:
            draws = int(m.group(3))
            cur = {'draws': draws} if lo <= draws < hi else None
            if cur is not None:
                out.append(cur)
            continue
        if cur is None:
            continue
        m = REC.match(line)
        if m:
            cur.update(zip(RECORD_FIELDS, (float(g) for g in m.groups())))
            continue
        m = OTH.match(line)
        if m:
            cur.update(zip(OTHER_FIELDS, (float(g) for g in m.groups())))
    return [w for w in out if len(w) > 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--null', default=None,
                    help='the arm that CANNOT move these numbers; its reading is the floor')
    ap.add_argument('--lo', type=int, default=4000)
    ap.add_argument('--hi', type=int, default=6000)
    a = ap.parse_args()

    arms = sorted({re.sub(r'(_\d+)?\.log$', '', os.path.basename(p))
                   for p in glob.glob(os.path.join(a.dir, '*.log'))} - {'base'})
    data = {}
    for arm in ['base'] + arms:
        runs = [windows(p, a.lo, a.hi) for p in arm_logs(a.dir, arm)]
        if any(runs):
            data[arm] = runs
    if 'base' not in data:
        sys.exit(f'no base_*.log in {a.dir}')

    fields = [f for f in RECORD_FIELDS + OTHER_FIELDS
              if any(f in w for r in data['base'] for w in r)]

    # The band check the docstring is about: if the statistic still varies across the
    # band, the band is too wide and everything below is suspect.
    flat = [w for r in data['base'] for w in r]
    if flat and 'record' in fields:
        half = (a.lo + a.hi) // 2
        lo_h = [w['record'] for w in flat if w['draws'] < half and 'record' in w]
        hi_h = [w['record'] for w in flat if w['draws'] >= half and 'record' in w]
        if lo_h and hi_h:
            drift = abs(statistics.median(lo_h) - statistics.median(hi_h))
            drift /= statistics.median(hi_h)
            print(f"band check: base `record` is {statistics.median(lo_h):.0f} ns/draw in "
                  f"{a.lo}-{half} and {statistics.median(hi_h):.0f} in {half}-{a.hi} "
                  f"({drift*100:.1f}% drift{'  <-- NARROW THE BAND' if drift > 0.05 else ''})")
    print()

    def med(runs, f):
        v = [w[f] for r in runs for w in r if f in w]
        return statistics.median(v) if v else None

    print(f"ns per DRAW, band {a.lo}-{a.hi}; each arm UNDOES a change, so an arm that is "
          f"SLOWER than base means the change is a win")
    hdr = f"{'field':<14} {'base':>7}"
    for arm in data:
        if arm != 'base':
            hdr += f" {arm[:13]:>14}"
    print(hdr)
    floor = {}
    for f in fields:
        b = med(data['base'], f)
        if b is None:
            continue
        row = f"{f:<14} {b:7.0f}"
        for arm, runs in data.items():
            if arm == 'base':
                continue
            v = med(runs, f)
            rel = (v - b) / b * 100 if v is not None and b else float('nan')
            if arm == a.null:
                floor[f] = abs(rel)
            row += f" {v:7.0f}{rel:+6.1f}%"
        print(row)

    print()
    if a.null:
        print(f"NULL CONTROL is '{a.null}' — it cannot move these numbers, so whatever it "
              f"reads is the floor.")
        for f in fields:
            b = med(data['base'], f)
            if b is None or f not in floor:
                continue
            for arm, runs in data.items():
                if arm in ('base', a.null):
                    continue
                v = med(runs, f)
                if v is None:
                    continue
                rel = abs((v - b) / b * 100)
                # A null control that reads EXACTLY zero does not mean the floor is zero;
                # it means this statistic is quantised (the profiler prints ns/draw as an
                # integer) and one arm happened to land on the same value. Dividing by it
                # produced "2702702702.7x, REAL" for a 2.7% difference on the first run of
                # this tool. The floor cannot be finer than the print resolution, so it is
                # clamped to one unit in the last place of the base value.
                ulp = 100.0 / b if b else 1.0
                fl = max(floor[f], ulp)
                if rel > 2 * fl:
                    print(f"  {f:<14} {arm}: {rel:.1f}% against a {fl:.1f}% floor "
                          f"— {rel/fl:.1f}x, REAL")
    else:
        print("!! no --null arm given. A percentage with no measured floor beside it is "
              "not a result (gotcha 331).")


if __name__ == '__main__':
    main()
