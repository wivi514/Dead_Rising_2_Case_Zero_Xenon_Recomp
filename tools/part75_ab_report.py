#!/usr/bin/env python3
"""The part-75 A/B reader, written after three ways of reading it went wrong.

WHAT WENT WRONG, in order, because each is a trap the next version had to close:

  1. **A 2,000-draw bin is too coarse here.** 5,000-7,000 draws is a 40% range and the two
     arms populate different parts of it.
  2. **A LINE FIT is worse, not better.** One control run never left a 90-draw range, so its
     "slope" was noise and its intercept came out NEGATIVE. A fit needs the x to vary and
     nothing checks that for you.
  3. **The route gate must be read, and only on a FINISHED log.** `autoroute.sh` exits 3 and
     says "DID NOT REACH THE OUTDOOR WORLD" for a run that stayed on the menu; a run whose
     log is still being written looks exactly the same. Gate on completed logs.
  4. **The machine is not the same all session.** At the *identical, deterministic* DebugJump
     menu screen — same draw count to within 3% — runs early in a session read ~9.0 ms and
     later ones ~5.3 ms. That 1.7x is not a configuration difference, and it does NOT
     transfer to the crowd (scaling one run's crowd by it disagrees with a measured run by
     30%), so it cannot be divided out either. **Runs are only comparable to runs in the
     same state.**

So this tool reports the menu window as a STATE TAG beside every run, refuses runs that did
not reach the crowd, and compares crowd medians only between runs whose menu agrees. The
menu is a near-null control for this particular item and a state fingerprint for any item:
it is the same screen every run, so a difference there is the machine, not the change.
"""
import sys, re, glob, statistics, collections
LINE = re.compile(r"median \(([\d.]+) ms\).*?draws med (\d+)")
MENU_LO, MENU_HI = 2300, 2700     # the DebugJump screen, ~2,470-2,570 draws every run
CROWD_LO = 5000

def runs(pat, arm):
    out=[]
    for path in sorted(glob.glob(pat)):
        pts=[(int(m.group(2)), float(m.group(1))) for l in open(path, errors="replace")
             if l.startswith("[fps]") for m in [LINE.search(l)] if m]
        menu=[ms for d,ms in pts if MENU_LO<=d<=MENU_HI]
        crowd=[(d,ms) for d,ms in pts if d>=CROWD_LO]
        out.append({"name": path.rsplit('_',1)[-1][:-4], "arm": arm, "path": path,
                    "menu": statistics.median(menu) if menu else None,
                    "crowd": crowd,
                    "peak": max((d for d,_ in pts), default=0)})
    return out

allr=[]
for a in sys.argv[1:]:
    arm,pat=a.split('=',1)
    allr += runs(pat, arm)

print(f"{'run':>8} {'arm':>6} {'peak':>6} {'menu ms':>8} {'crowd n':>8} {'crowd med':>10}  gate")
for r in sorted(allr, key=lambda r: r["path"]):
    ok = r["peak"] >= CROWD_LO
    cm = statistics.median([ms for _,ms in r["crowd"]]) if r["crowd"] else float('nan')
    print(f"{r['name']:>8} {r['arm']:>6} {r['peak']:>6} "
          f"{r['menu'] if r['menu'] else float('nan'):>8.2f} {len(r['crowd']):>8} "
          f"{cm:>10.2f}  {'OK' if ok else '** NOT REPORTABLE (never reached the crowd)'}")

# Group by machine state, using the menu window as the fingerprint.
good=[r for r in allr if r["peak"]>=CROWD_LO and r["menu"]]
# Group by RATIO, not by a fixed bucket width. A width-2 ms bucket put 8.94 and 9.03 —
# obviously the same state — into different buckets, which is the classic boundary bug and
# it silently reported "no matched control" for a pair that matched to 1%.
states=collections.defaultdict(list)
for r in sorted(good, key=lambda r: r["menu"]):
    key = next((k for k in states if abs(r["menu"]/k - 1) <= 0.15), round(r["menu"], 1))
    states[key].append(r)
print("\ncomparisons WITHIN one machine state (menu window as the fingerprint):")
for st in sorted(states):
    rs=states[st]
    byarm=collections.defaultdict(list)
    for r in rs: byarm[r["arm"]].append(r)
    if len(byarm)<2:
        print(f"  menu~{st} ms: only arm(s) {list(byarm)} — NOT COMPARABLE, no matched control")
        continue
    print(f"  menu~{st} ms:")
    for arm,rs2 in byarm.items():
        pts=[p for r in rs2 for p in r["crowd"]]
        print(f"     {arm:>6}: {len(pts):3d} crowd windows, draws "
              f"{min(d for d,_ in pts)}..{max(d for d,_ in pts)}, "
              f"median {statistics.median([ms for _,ms in pts]):.2f} ms "
              f"({','.join(r['name'] for r in rs2)})")
    # matched narrow bins inside this state
    for b in range(CROWD_LO, 9000, 250):
        cells=[]
        meds=[]
        for arm in sorted(byarm):
            v=[ms for r in byarm[arm] for d,ms in r["crowd"] if b<=d<b+250]
            if len(v)<3: meds=[]; break
            meds.append(statistics.median(v)); cells.append(f"{arm} {meds[-1]:6.2f} n={len(v):<3}")
        if meds:
            print(f"       {b}-{b+250}: " + "  ".join(cells) +
                  f"   {100*(meds[1]-meds[0])/meds[0]:+.1f}%")
