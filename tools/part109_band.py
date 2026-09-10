#!/usr/bin/env python3
"""Compare two arms of a crowdroute A/B in MATCHED DRAW BANDS.

WHY THIS EXISTS, and it is not a convenience. `tools/read_crowd.py` reports each run's
median over its >= 8,000-draw windows, and part 109's item 1 A/B came back as 10.81 ms
(off) against 10.82 ms (on): a dead null. It was not a null. The ON arm had landed in
denser crowds — median 9,411 draws against 8,989, a 4.7% difference the route's own
header warns about, because the zombie spawn is random and identical inputs do not put
Chuck on the same spot twice — and this route's frame time rises about 0.0005 ms a draw.
Banding by draw count and comparing band by band recovered a −0.33 ms effect that was
monotone in all five matched bands.

So: a run median on this route is partly a statement about how many draws that run
happened to get, and two arms are only comparable where their draw counts are. Same
conclusion part 26 reached for picture A/Bs by a different road (gotcha 254) and the same
reader part 76 built for frame stats; this is the [fps]-window form of it.

Usage:
    tools/part109_band.py <A logs, comma-separated> <B logs, comma-separated>
    tools/part109_band.py "$(ls base_*.log | paste -sd,)" "$(ls arm_*.log | paste -sd,)"

BAND=250 draws by default; DRAW_FLOOR=8000 is the crowd gate (a run-wide median measures
how long the menus lasted, not the crowd).
"""
import re
import sys
import statistics
import collections

BAND = 250
DRAW_FLOOR = 8000
PAT = re.compile(r"\[fps\][^|]*\|\s*([\d.]+) fps median \(([\d.]+) ms\)"
                 r".*?p99 ([\d.]+) ms.*?draws med (\d+)")


def load(paths):
    """Every crowd [fps] window of every named log, as (draws, median ms, p99 ms)."""
    rows = []
    for p in paths:
        if not p:
            continue
        for line in open(p, errors="replace"):
            m = PAT.search(line)
            if m and int(m.group(4)) >= DRAW_FLOOR:
                rows.append((int(m.group(4)), float(m.group(2)), float(m.group(3))))
    return rows


def binned(rows):
    d = collections.defaultdict(list)
    for draws, ms, p99 in rows:
        d[draws // BAND].append((ms, p99))
    return d


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = load(sys.argv[1].split(',')), load(sys.argv[2].split(','))
    if not a or not b:
        sys.exit("one of the arms has no crowd windows at all — check the logs")
    A, B = binned(a), binned(b)
    print(f"arm A: {len(a)} crowd windows   arm B: {len(b)} crowd windows")
    print(f"{'band draws':>14}  {'A ms':>7} {'nA':>3}  {'B ms':>7} {'nB':>3}  "
          f"{'delta':>7} {'%':>6}")
    # Weight each band by the SMALLER of the two window counts: a band one arm barely
    # populated should not carry the summary. An unmatched band is printed nowhere,
    # because a band only one arm reached says nothing about the change.
    wa = wb = wn = 0.0
    for k in sorted(set(A) & set(B)):
        ma = statistics.median(x[0] for x in A[k])
        mb = statistics.median(x[0] for x in B[k])
        n = min(len(A[k]), len(B[k]))
        print(f"{k*BAND:6d}-{k*BAND+BAND-1:6d}  {ma:7.2f} {len(A[k]):3d}  "
              f"{mb:7.2f} {len(B[k]):3d}  {mb-ma:+7.2f} {100*(mb-ma)/ma:+6.1f}")
        wa += ma * n
        wb += mb * n
        wn += n
    if not wn:
        sys.exit("\nNO MATCHED BANDS — the arms never reached the same draw counts, so "
                 "this comparison cannot be made. Re-run, do not average.")
    print(f"\nweighted by matched windows: A {wa/wn:.2f} ms   B {wb/wn:.2f} ms   "
          f"delta {(wb-wa)/wn:+.2f} ms ({100*(wb-wa)/wa:+.1f}%)")


if __name__ == "__main__":
    main()
