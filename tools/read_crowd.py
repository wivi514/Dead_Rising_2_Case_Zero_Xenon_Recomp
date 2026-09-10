#!/usr/bin/env python3
"""Median frame time over the CROWD windows of a part80_crowdroute log.

Only windows whose median draw count is in the band are read: the route walks menus
(60 draws) into a crowd, so a run-wide median measures how long the menus lasted.
"""
import re, sys, statistics
BAND = 8000
pat = re.compile(r"\[fps\][^|]*\|\s*([\d.]+) fps median \(([\d.]+) ms\).*?p99 ([\d.]+) ms.*?draws med (\d+)")
for path in sys.argv[1:]:
    w = [(float(m.group(2)), float(m.group(3)), int(m.group(4)))
         for m in (pat.search(l) for l in open(path, errors="replace")) if m and int(m.group(4)) >= BAND]
    if not w:
        print(f"{path.split('/')[-1]:52s} no crowd windows"); continue
    med = statistics.median(x[0] for x in w)
    print(f"{path.split('/')[-1]:52s} windows={len(w):3d}  median {med:6.2f} ms ({1000/med:5.1f} fps)"
          f"  p99 {statistics.median(x[1] for x in w):6.2f}  draws {int(statistics.median(x[2] for x in w))}")
