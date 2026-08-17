#!/usr/bin/env python3
"""Read a part-51 tick A/B: does shortening the pump's sleep shorten the frame?

WHY A READER OF ITS OWN, next to `part47_perf_read.py` (frame time by draw bin and the
phase shares) and `part48_walk_read.py` (ns per packet). Neither can see the quantity
this A/B is about. The pump's sleep is reported only in the `[vkprof] pump` line, and
whether that sleep was ON THE CRITICAL PATH is reported only in the line part 51 added
underneath it. Those two are the MECHANISM: they move by construction if the arm engaged
at all, and an arm that cannot be shown to have engaged proves nothing whichever way the
frame time falls (gotcha 151).

So this prints, per arm, over the CROWD windows only (>= 3,000 draws/frame — a light
window and a heavy one are different workloads and pooling them measures the route,
gotcha 321):

  MECHANISM   ticks/frame, sleep % of wall clock, the share of ticks whose walk made
              progress, and the <= ms/frame latency bound. The arm changes the tick
              period, so ticks/frame and sleep% MUST move; if they do not, the run did
              not get the environment variable and nothing else in the table means
              anything.
  EFFECT      fps and ms/frame from the same windows, with the per-run spread, and the
              draws/frame those windows were drawing — because two arms are comparable
              only where they are drawing comparable amounts.

READ THE `slow` ARM FIRST. It is the positive control: a coarser tick must be WORSE if
the mechanism is real. Three arms that read the same across a 40x range of tick periods
would mean the sleep is not on the critical path, which is a result and not a failure.

Usage: part51_tick_read.py <dir>        # a MODE directory from part51_tick_campaign.sh
"""
import glob
import os
import re
import statistics
import sys

DRAW_FLOOR = 3000

# [vkprof] 38.5 fps (26.0 ms/frame, 6554 draws/frame) | draw 56.6% [...]
RE_MAIN = re.compile(
    r"\[vkprof\] ([\d.]+) fps \(([\d.]+) ms/frame, (\d+) draws/frame\)")
# [vkprof] pump 3465 ticks (3.00/frame) | sleep 12.2% walk 87.8% [pm4 29.6] ...
RE_PUMP = re.compile(
    r"\[vkprof\] pump (\d+) ticks \(([\d.]+)/frame\) \| sleep ([\d.]+)% walk ([\d.]+)%")
# [vkprof]   sleep on the critical path: 3465 of 3465 ticks made progress (100.0%),
#            sleep before them 3658.5 ms of 3658.5 ms | <= 3.17 ms/frame of latency
RE_CRIT = re.compile(
    r"sleep on the critical path: (\d+) of (\d+) ticks made progress \(([\d.]+)%\), "
    r"sleep before them ([\d.]+) ms of ([\d.]+) ms \| <= ([\d.]+) ms/frame")
# [vkprof]   CZ_VK_FRAME_STATS itself: 2.41 ms/frame (9.3% of this window)
RE_FS = re.compile(r"CZ_VK_FRAME_STATS itself: ([\d.]+) ms/frame \(([\d.]+)%")


def read_run(path):
    """Windows of one run, as dicts, keeping only the crowd ones.

    The four lines of a window are emitted consecutively, so a window is assembled by
    carrying the most recent main line forward. A window missing its pump line (the
    first of a run can be) is dropped rather than half-counted.
    """
    out, cur = [], None
    for line in open(path, errors="replace"):
        m = RE_MAIN.search(line)
        if m:
            if cur and "ticks" in cur:
                out.append(cur)
            cur = {"fps": float(m.group(1)), "ms": float(m.group(2)),
                   "draws": int(m.group(3))}
            continue
        if cur is None:
            continue
        m = RE_PUMP.search(line)
        if m:
            cur.update(ticks=int(m.group(1)), ticksPerFrame=float(m.group(2)),
                       sleepPct=float(m.group(3)), walkPct=float(m.group(4)))
            continue
        m = RE_CRIT.search(line)
        if m:
            cur.update(progPct=float(m.group(3)), boundMs=float(m.group(6)))
            continue
        m = RE_FS.search(line)
        if m:
            cur.update(fsMs=float(m.group(1)))
    if cur and "ticks" in cur:
        out.append(cur)
    return [w for w in out if w["draws"] >= DRAW_FLOOR]


def med(vals):
    return statistics.median(vals) if vals else float("nan")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    arms = {}
    for path in sorted(glob.glob(os.path.join(d, "*.log"))):
        tag = os.path.basename(path)[:-4]
        arm = tag.rsplit("_", 1)[0]
        # A run whose driver never wrote a completion marker may be TRUNCATED rather
        # than short, and nothing in the file distinguishes them (gotcha 339).
        if not os.path.exists(os.path.join(d, tag + ".done")):
            print(f"!! {tag}: no completion marker — SKIPPED (it may be truncated)")
            continue
        arms.setdefault(arm, []).append((tag, read_run(path)))

    if not arms:
        print("no completed runs found")
        return 1

    print(f"crowd windows only (>= {DRAW_FLOOR} draws/frame)\n")
    print("MECHANISM — did the arm engage? These must move, or nothing else counts.")
    print(f"{'arm':>6} {'runs':>5} {'windows':>8} {'ticks/frame':>12} {'sleep %':>8} "
          f"{'progress %':>11} {'<= ms/frame':>12}")
    for arm in sorted(arms):
        ws = [w for _, run in arms[arm] for w in run]
        print(f"{arm:>6} {len(arms[arm]):>5} {len(ws):>8} "
              f"{med([w['ticksPerFrame'] for w in ws]):>12.2f} "
              f"{med([w['sleepPct'] for w in ws]):>8.1f} "
              f"{med([w.get('progPct', float('nan')) for w in ws]):>11.1f} "
              f"{med([w.get('boundMs', float('nan')) for w in ws]):>12.2f}")

    print("\nEFFECT — frame time in the same windows. Per-run medians are the spread;")
    print("an arm difference inside the base spread is not a result.")
    print(f"{'arm':>6} {'ms/frame':>10} {'per-run medians':>34} {'fps':>7} "
          f"{'draws/frame':>12}")
    for arm in sorted(arms):
        ws = [w for _, run in arms[arm] for w in run]
        perrun = [med([w["ms"] for w in run]) for _, run in arms[arm] if run]
        print(f"{arm:>6} {med([w['ms'] for w in ws]):>10.2f} "
              f"{', '.join(f'{v:.2f}' for v in perrun):>34} "
              f"{med([w['fps'] for w in ws]):>7.1f} "
              f"{med([w['draws'] for w in ws]):>12.0f}")

    fs = [w["fsMs"] for arm in arms for _, run in arms[arm] for w in run if "fsMs" in w]
    if fs:
        print(f"\nAnd the instrument's own bill, present in these runs: "
              f"CZ_VK_FRAME_STATS = {med(fs):.2f} ms/frame. Every frame time above is "
              f"that much slower than the frame a player runs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
