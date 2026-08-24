#!/usr/bin/env python3
"""Read `[fps]` windows out of autoroute logs and compare two arms in a MATCHED DRAW BAND.

WHY IT IS BANDED. The crowd problem this part is about IS a function of draw count
(§6do: 12.5 ms at 2,500 draws, 33.8 at 9,000+), and the DebugJump landing is bimodal —
two runs of one configuration routinely differ by thousands of draws. A run mean over
that is a fact about where the camera happened to point, not about the change. Every
window is a `draws med` and a `ms median`, so bin the windows and compare bin to bin.

WHY MEDIANS OF WINDOW MEDIANS. Gotcha 237: a mean frame time on this title measures its
vblank pacing floor. The `[fps]` line already reports a per-window median; this takes the
median across a band's windows, which is robust to the parked-camera tail (gotcha 438).

Usage:  tools/part75_ab.py A=<glob> B=<glob> [...]
        tools/part75_ab.py base='...p75a_*.log' arena='...p75b_*.log'
"""
import sys, re, glob, statistics, collections

LINE = re.compile(r"median \(([\d.]+) ms\).*?(\d+) frames in.*?draws med (\d+)")
BANDS = [(1500,3000),(3000,5000),(5000,7000),(7000,9000),(9000,20000)]

def windows(pats):
    """(draws, ms, frames, logname) per [fps] window across every log matching the glob."""
    out=[]
    for pat in pats.split(','):
        for path in sorted(glob.glob(pat)):
            for line in open(path, errors="replace"):
                if not line.startswith("[fps]"): continue
                m=LINE.search(line)
                if m: out.append((int(m.group(3)), float(m.group(1)), int(m.group(2)),
                                  path.rsplit('/',1)[-1]))
    return out

arms=[]
for a in sys.argv[1:]:
    name,pat=a.split('=',1)
    arms.append((name, windows(pat)))
    print(f"{name}: {len(arms[-1][1])} windows from {len(glob.glob(pat.split(',')[0]))} log(s)")

print(f"\n{'band':>12} " + " ".join(f"{n:>18}" for n,_ in arms) + "   delta")
for lo,hi in BANDS:
    cells=[]; meds=[]
    for name,w in arms:
        b=[ms for d,ms,_,_ in w if lo<=d<hi]
        if len(b)<3: cells.append(f"{'-':>18}"); meds.append(None); continue
        m=statistics.median(b)
        cells.append(f"{m:8.2f} ms n={len(b):<4}")
        meds.append(m)
    if all(m is None for m in meds): continue
    d=""
    if len(meds)>=2 and meds[0] and meds[1]:
        d=f"  {100*(meds[1]-meds[0])/meds[0]:+6.1f}%  ({meds[1]-meds[0]:+.2f} ms)"
    print(f"{lo:>6}-{hi:<5} " + " ".join(cells) + d)

# PER-RUN, in the band with the most windows. Three runs an arm exist precisely so the
# WITHIN-arm spread can be read as this route's noise floor for the statistic — a
# between-arm difference smaller than the within-arm spread is not a result (gotcha 229).
best = max(BANDS, key=lambda b: sum(1 for _,w in arms for d,_,_,_ in w if b[0]<=d<b[1]))
print(f"\nper-run medians in {best[0]}-{best[1]} draws (the WITHIN-ARM spread is the floor):")
for name,w in arms:
    per=collections.defaultdict(list)
    for d,ms,_,log in w:
        if best[0]<=d<best[1]: per[log].append(ms)
    vals=[statistics.median(v) for v in per.values() if len(v)>=3]
    line=" ".join(f"{v:6.2f}" for v in sorted(vals))
    spread=f"  spread {max(vals)-min(vals):+.2f} ms ({100*(max(vals)-min(vals))/min(vals):.1f}%)" if len(vals)>1 else ""
    print(f"  {name:>8}: {line}{spread}")
