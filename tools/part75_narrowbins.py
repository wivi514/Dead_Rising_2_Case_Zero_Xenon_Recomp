#!/usr/bin/env python3
"""Compare two arms in NARROW matched draw bins, reporting which runs feed each bin.

WHY THIS REPLACED BOTH EARLIER FORMS. `part75_ab.py` bins at 2,000 draws wide and
`part75_bandfit.py` fits a line. Part 75's six-run A/B broke both, and the way it broke
them is the finding:

  * one control run (`arena2`) never left ~2,450 draws — 28 of its 28 windows are inside a
    90-draw range — so its "fit" is a slope through noise and its intercept came out
    NEGATIVE. A line fitted to a workload that never varied is not a measurement.
  * and at the SAME ~2,450 draws, different runs read **5.3 ms and 9.1 ms**. Draw count is
    therefore NOT a complete index of this workload: the early windows of a run include the
    arrival transient (texture streaming, problem A) while a parked later window at the same
    draw count is steady state.

So: bin narrowly, require BOTH arms to have samples in a bin before printing it, and NAME
the runs feeding each side — because a bin fed by one run per arm is a comparison of two
afternoons, not of two configurations.
"""
import sys, re, glob, statistics, collections
LINE = re.compile(r"median \(([\d.]+) ms\).*?draws med (\d+)")
WIDTH = 250

def load(pat):
    pts=[]
    for path in sorted(glob.glob(pat)):
        run=path.rsplit('_',1)[-1][:-4]
        for line in open(path, errors="replace"):
            if line.startswith("[fps]"):
                m=LINE.search(line)
                if m and int(m.group(2))>=1200:
                    pts.append((int(m.group(2)), float(m.group(1)), run))
    return pts

arms=[(a.split('=',1)[0], load(a.split('=',1)[1])) for a in sys.argv[1:]]
for n,p in arms:
    runs=sorted(set(r for _,_,r in p))
    print(f"{n:>8}: {len(p):3d} windows from {len(runs)} runs {runs}")

bins=collections.defaultdict(lambda: collections.defaultdict(list))
for n,p in arms:
    for d,ms,run in p:
        bins[d//WIDTH*WIDTH][n].append((ms,run))

print(f"\nbins of {WIDTH} draws where BOTH arms have >=3 windows:")
print(f"{'draws':>11} " + " ".join(f"{n:>26}" for n,_ in arms) + "     delta")
for b in sorted(bins):
    cols=[]
    ok=True
    meds=[]
    for n,_ in arms:
        v=bins[b][n]
        if len(v)<3: ok=False; break
        runs=sorted(set(r for _,r in v))
        m=statistics.median([x for x,_ in v])
        meds.append(m)
        cols.append(f"{m:7.2f} n={len(v):<3} {','.join(runs):<9}")
    if not ok: continue
    d=f"  {100*(meds[1]-meds[0])/meds[0]:+6.1f}%  ({meds[1]-meds[0]:+.2f} ms)" if len(meds)>1 else ""
    print(f"{b:>6}-{b+WIDTH:<4} " + " ".join(cols) + d)
