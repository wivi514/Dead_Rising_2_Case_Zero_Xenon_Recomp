#!/usr/bin/env python3
"""Era medians for every run of every arm, then WITHIN-arm spread vs BETWEEN-arm spread.

WHY THIS EXISTS (part 39, gotcha 299)
-------------------------------------
`frame_era_medians.py` takes exactly TWO null runs and ONE arm run. At three runs an arm
that means somebody picks which pair supplies the noise floor — and on part 39's mip A/B
the pick decided the answer. With the mips-OFF pair as the null the mips-ON arm read
"9.0x the null" on mean luma; with the mips-ON pair as the null the mips-OFF arm read
"INSIDE the null" on everything. Same six runs, opposite conclusions, because the two
arms do not have the same noise floor (0.15% against 2.81% on mean luma).

So this does not choose. It prints every run's era median, the spread WITHIN each arm,
and the difference BETWEEN the arms' medians — and calls the comparison resolved only
when the between-arm difference exceeds twice the WORST within-arm spread. That is
deliberately conservative: on this title a picture A/B that cannot clear its own noisiest
arm is not a result (gotchas 50/51/86, 159, 229).

Read it beside the oracle, not alone: "better" is a direction toward hardware's own
frame statistics, not "more" or "less" (gotcha 298).

USAGE
    frame_arm_spread.py <armA_run1,run2,run3> <armB_run1,run2,run3>
where each argument is a comma-separated list of CZ_VK_FRAME_STATS files. The first
argument is labelled as the default/new arm and the second as the control.
"""
import statistics
import sys

# The outdoor era, as docs/measurement.md defines it: only frames above this draw count
# are the crowd/street workload the comparison is about.
MIN_DRAWS = 1800
COLS = {'meanLuma': 8, 'distinctColours': 9, 'coveragePct': 7}


def medians(path):
    vals = {k: [] for k in COLS}
    for line in open(path):
        if line.startswith('#'):
            continue
        f = line.split()
        if len(f) < 11:
            continue
        try:
            if int(f[1]) < MIN_DRAWS:
                continue
            for k, i in COLS.items():
                vals[k].append(float(f[i]))
        except ValueError:
            continue
    return {k: statistics.median(v) for k, v in vals.items() if v}, len(vals['meanLuma'])


ARM_A = 'arm A (the change)'
ARM_B = 'arm B (the control)'
arms = {ARM_A: sys.argv[1].split(','), ARM_B: sys.argv[2].split(',')}
res = {}
for name, paths in arms.items():
    res[name] = []
    for p in paths:
        m, n = medians(p)
        res[name].append(m)
        print('%-28s %-12s %6d frames  luma %8.4f  distinct %10.1f'
              % (name, p.split('/')[-1], n, m['meanLuma'], m['distinctColours']))

print()
for stat in ('meanLuma', 'distinctColours'):
    print(stat)
    for name, ms in res.items():
        v = [m[stat] for m in ms]
        span = (max(v) - min(v)) / statistics.median(v) * 100
        print('  within %-28s median %10.3f   spread %.2f%%  (%s)'
              % (name, statistics.median(v), span,
                 ' '.join('%.2f' % x for x in v)))
    a = [m[stat] for m in res[ARM_A]]
    b = [m[stat] for m in res[ARM_B]]
    diff = (statistics.median(a) - statistics.median(b)) / statistics.median(b) * 100
    worst = max((max(a) - min(a)) / statistics.median(a),
                (max(b) - min(b)) / statistics.median(b)) * 100
    print('  BETWEEN arms: %+.2f%%   worst within-arm spread %.2f%%   -> %s'
          % (diff, worst,
             'RESOLVED' if abs(diff) > 2 * worst else 'UNRESOLVED (inside the noise)'))
    print()
