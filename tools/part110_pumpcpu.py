#!/usr/bin/env python3
"""The PUMP THREAD's CPU per frame, banded by draw count — part 110's §3.1 quantity.

WHY A NEW READER RATHER THAN `tools/part109_band.py`. That one compares two arms' WALL
frame times in matched draw bands, and matched banding is not optional on this route
(part 109's texture memo read as a dead null at 10.81 vs 10.82 ms because the arms
landed at 8,989 and 9,411 draws; banded, it was −0.33 ms monotone in all five bands).

Part 110's ceiling arm cannot be read that way at all. `CZ_VK_NO_DODRAW=1` draws
nothing, so its GPU is empty and its wall time is meaningless twice over — an arm that
renders less is inadmissible for wall by this project's own A/B rule. The only
admissible quantity is the pump thread's own CPU milliseconds per presented frame, and
the runtime now prints exactly that on every `[fps]` window (one `clock_gettime` per
window, not per frame), so this bands THAT column instead of the frame time.

It also prints the wall column beside it, because the two together say something neither
says alone: on a normal arm the pump's CPU and the wall frame are within a few tenths of
a millisecond of each other, which is the statement "this frame is one thread long". If
that gap opens up, the frame stopped being CPU-bound on the pump and every conclusion
downstream of that changes.

Usage:
    tools/part110_pumpcpu.py <logs...>                    # one arm, banded
    tools/part110_pumpcpu.py <A logs> -- <B logs>         # two arms, banded, with deltas

BAND=250 draws; DRAW_FLOOR=8000 is the crowd gate (a run-wide median over every window
measures how long the menus lasted, not the crowd).
"""
import collections
import re
import statistics
import sys

BAND = 250
DRAW_FLOOR = 8000
PAT = re.compile(r"\[fps\][^|]*\|\s*([\d.]+) fps median \(([\d.]+) ms\)"
                 r".*?p99 ([\d.]+) ms.*?draws med (\d+)"
                 r".*?pump cpu ([\d.]+) ms/frame \((\d+)% of a core\)")


def load(paths):
    """Every crowd [fps] window: (draws, wall ms, pump cpu ms, duty %)."""
    rows = []
    for p in paths:
        n = 0
        for line in open(p, errors="replace"):
            m = PAT.search(line)
            if m and int(m.group(4)) >= DRAW_FLOOR:
                rows.append((int(m.group(4)), float(m.group(2)),
                             float(m.group(5)), float(m.group(6))))
                n += 1
        if n == 0:
            # Named, not skipped. A log with no matching windows is either a run that
            # never reached the crowd or a build without the `pump cpu` column, and
            # silently averaging the rest would hide both.
            print(f"  !! no crowd windows with a `pump cpu` column in {p}")
    return rows


def bands(rows):
    d = collections.defaultdict(list)
    for draws, wall, cpu, duty in rows:
        d[(draws // BAND) * BAND].append((wall, cpu, duty))
    return d


def report(name, rows):
    if not rows:
        print(f"{name}: no crowd windows at >= {DRAW_FLOOR} draws")
        return {}
    b = bands(rows)
    print(f"\n=== {name}: {len(rows)} windows >= {DRAW_FLOOR} draws")
    print(f"  {'band':>7} {'n':>3} {'wall ms':>9} {'pump cpu':>9} {'duty':>6} {'gap':>7}")
    out = {}
    for k in sorted(b):
        v = b[k]
        wall = statistics.median(x[0] for x in v)
        cpu = statistics.median(x[1] for x in v)
        duty = statistics.median(x[2] for x in v)
        out[k] = (wall, cpu, len(v))
        print(f"  {k:>7} {len(v):>3} {wall:9.2f} {cpu:9.2f} {duty:5.0f}% "
              f"{wall - cpu:7.2f}")
    allw = statistics.median(x[1] for x in rows)
    allc = statistics.median(x[2] for x in rows)
    print(f"  {'ALL':>7} {len(rows):>3} {allw:9.2f} {allc:9.2f}")
    return out


def main():
    argv = sys.argv[1:]
    if not argv:
        sys.exit(__doc__)
    if "--" in argv:
        i = argv.index("--")
        a, bl = argv[:i], argv[i + 1:]
    else:
        a, bl = argv, []
    ra = report("A", load(a))
    if not bl:
        return
    rb = report("B", load(bl))
    print(f"\n=== MATCHED BANDS (B - A)")
    print(f"  {'band':>7} {'nA':>3} {'nB':>3} {'wall d':>8} {'cpu d':>8} {'cpu %':>7}")
    shared = sorted(set(ra) & set(rb))
    if not shared:
        print("  !! no band has windows in BOTH arms — the arms did not land on the "
              "same load and nothing here is comparable (gotcha 544).")
        return
    dc = []
    for k in shared:
        wa, ca, na = ra[k]
        wb, cb, nb = rb[k]
        dc.append(cb - ca)
        print(f"  {k:>7} {na:>3} {nb:>3} {wb - wa:8.2f} {cb - ca:8.2f} "
              f"{100.0 * (cb - ca) / ca:6.1f}%")
    print(f"  median pump-cpu delta over {len(shared)} matched bands: "
          f"{statistics.median(dc):+.2f} ms/frame  "
          f"({'monotone' if all(x < 0 for x in dc) or all(x > 0 for x in dc) else 'NOT monotone — read the bands, not this line'})")


if __name__ == "__main__":
    main()
