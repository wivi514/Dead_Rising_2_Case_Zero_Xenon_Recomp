#!/usr/bin/env python3
"""Band an autoroute A/B by DRAW COUNT and print matched medians.

WHY THIS EXISTS ALONGSIDE `part75_ab_report.py`. That tool groups runs by a MENU-window
fingerprint, on the reasoning that the DebugJump screen is the same every run so a
difference there is the machine rather than the change. That is right for an item that
only affects the crowd — and **it silently refuses a perfectly good comparison for an item
that affects every presented frame**. Part 76's readback item moved the menu window by
-15.2%, exactly as it moved the crowd, so the two arms landed in different "machine
states" and the tool printed "NOT COMPARABLE, no matched control" for the cleanest A/B
this project has run.

So: a control channel is only a control for changes that cannot reach it. Say which kind
of change you have, and use the menu window as a STATE CHECK for the first kind and as a
SECOND MEASUREMENT for the second. This tool prints both and asserts neither.

The rest of the method is `docs/part76-kickoff.md` §5, unchanged: matched 250-draw bands
(a 2,000-draw bin is a 40% range), medians not means (a mean on this title measures the
vblank pacing floor, gotcha 237), and the per-arm draw median printed so a comparison
where one arm simply drew less is visible rather than inferred.
"""
import sys, re, glob, statistics
LINE = re.compile(r"median \(([\d.]+) ms\).*?draws med (\d+)")
MENU_LO, MENU_HI, CROWD = 2300, 2700, 5000

def pts(pat):
    o = []
    for p in sorted(glob.glob(pat)):
        for l in open(p, errors="replace"):
            if l.startswith("[fps]"):
                m = LINE.search(l)
                if m:
                    o.append((int(m.group(2)), float(m.group(1))))
    return o

arms = [(a.split('=', 1)[0], pts(a.split('=', 1)[1])) for a in sys.argv[1:]]
if len(arms) != 2:
    sys.exit("usage: part76_band.py <armA>=<glob> <armB>=<glob>")
(na, A), (nb, B) = arms
for n, v in arms:
    if not v:
        sys.exit(f"** arm {n} has no [fps] windows — did the runs finish?")

def med(v, lo, hi=10**9):
    x = [ms for d, ms in v if lo <= d < hi]
    return (statistics.median(x), len(x)) if x else (float('nan'), 0)

ma, ca = med(A, MENU_LO, MENU_HI + 1)
mb, cb = med(B, MENU_LO, MENU_HI + 1)
print(f"MENU window ({MENU_LO}-{MENU_HI} draws) — the same screen in every run")
print(f"  {na:>8}: {ma:6.2f} ms  n={ca}")
print(f"  {nb:>8}: {mb:6.2f} ms  n={cb}   delta {100*(ma-mb)/mb:+.1f}%")
print("  (a state check for a crowd-only item; a SECOND MEASUREMENT for a per-frame one)")

print(f"\nCROWD, matched 250-draw bands")
print(f"  {'band':>12} {'%s ms'%na:>10} {'n':>4} {'%s ms'%nb:>10} {'n':>4} {'delta':>8} {'ms':>7}")
for b in range(CROWD, 10000, 250):
    x, nx = med(A, b, b + 250)
    y, ny = med(B, b, b + 250)
    if nx < 3 or ny < 3:
        continue
    print(f"  {b}-{b+250:<6} {x:10.2f} {nx:4d} {y:10.2f} {ny:4d} {100*(x-y)/y:+7.1f}% {x-y:+7.2f}")

xa, nxa = med(A, CROWD)
xb, nxb = med(B, CROWD)
print(f"\nWHOLE CROWD (>= {CROWD} draws)")
print(f"  {na:>8}: {xa:6.2f} ms ({1000/xa:5.1f} fps)  n={nxa}  draw median "
      f"{statistics.median([d for d,_ in A if d>=CROWD]):.0f}")
print(f"  {nb:>8}: {xb:6.2f} ms ({1000/xb:5.1f} fps)  n={nxb}  draw median "
      f"{statistics.median([d for d,_ in B if d>=CROWD]):.0f}")
print(f"  delta {xa-xb:+.2f} ms  {100*(xa-xb)/xb:+.1f}%")
