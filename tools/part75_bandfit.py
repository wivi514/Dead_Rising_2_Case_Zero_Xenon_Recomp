#!/usr/bin/env python3
"""Compare two arms by FITTING ms against draw count, instead of binning.

WHY. The banded comparison in `part75_ab.py` is the right default, but on this route the
bands are coarse (5,000-7,000 draws is a 40% range) and the DebugJump landing is bimodal,
so two arms can populate very different parts of the same bin and the bin median then
mixes a real difference with a placement difference.

The frame time on this workload is close to LINEAR in the draw count (§6do: 12.5 ms at
2,500 -> 33.8 at 9,000+, and the us/draw column is 3.65-5.05 across the whole range), so
a least-squares line per arm and an evaluation of both lines at the SAME draw count is a
matched comparison the bins cannot give. Quote the fit at a draw count both arms actually
cover; extrapolating past either arm's range is not a measurement.
"""
import sys, re, glob, statistics
LINE = re.compile(r"median \(([\d.]+) ms\).*?(\d+) frames in.*?draws med (\d+)")

def load(pat):
    pts=[]
    for path in sorted(glob.glob(pat)):
        for line in open(path, errors="replace"):
            if line.startswith("[fps]"):
                m=LINE.search(line)
                if m: pts.append((int(m.group(3)), float(m.group(1))))
    return pts

def fit(pts):
    n=len(pts); sx=sum(p[0] for p in pts); sy=sum(p[1] for p in pts)
    sxx=sum(p[0]*p[0] for p in pts); sxy=sum(p[0]*p[1] for p in pts)
    d=n*sxx-sx*sx
    if not d: return None
    b=(n*sxy-sx*sy)/d
    return (sy-b*sx)/n, b

arms=[]
for a in sys.argv[1:]:
    name,pat=a.split('=',1)
    pts=[p for p in load(pat) if p[0]>=1200]   # drop the menu/title windows
    arms.append((name,pts,fit(pts)))
    lo=min(p[0] for p in pts); hi=max(p[0] for p in pts)
    a0,b0=arms[-1][2]
    print(f"{name:>8}: {len(pts):3d} windows, draws {lo}..{hi}, "
          f"ms = {a0:.2f} + {b0*1000:.3f} per 1000 draws")

lo=max(min(p[0] for _,pts,_ in arms for p in pts if True) for _ in [0])
lo=max(min(p[0] for p in pts) for _,pts,_ in arms)
hi=min(max(p[0] for p in pts) for _,pts,_ in arms)
print(f"\nboth arms cover {lo}..{hi} draws; evaluated inside that range only:")
print(f"{'draws':>7} " + " ".join(f"{n:>9}" for n,_,_ in arms) + "     delta")
for d in range(int(lo//500*500), int(hi)+1, 500):
    if d < lo: continue
    vs=[a+b*d for _,_,(a,b) in arms]
    dd=f"  {100*(vs[1]-vs[0])/vs[0]:+6.1f}%  ({vs[1]-vs[0]:+.2f} ms)" if len(vs)>1 else ""
    print(f"{d:>7} " + " ".join(f"{v:9.2f}" for v in vs) + dd)
