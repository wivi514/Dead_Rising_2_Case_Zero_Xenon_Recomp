#!/usr/bin/env python3
"""Compare `CZ_VK_PROFILE` windows between two arms, BINNED BY DRAW COUNT.

WHY THIS EXISTS. The two arms of the swapchain A/B are two runs of a game driving itself
through a crowd, and they never see the same draw list: part 54's first round put one arm
at 1,806 draws a frame and the other at 4,039 in the same window. Reading the last line of
each, or their means, compares two different workloads and reports the difference as the
change (gotchas 237 and the whole of `frame_perf_bins.py`'s reason for existing).

`frame_perf_bins.py` already does this for `CZ_VK_FRAME_STATS` files — but frame stats
cost 1.9-3.3 ms a frame and, worse, they are one of the instruments that FORCES the
readback to keep running in the swapchain arm, which is precisely the cost being
measured. So this arm cannot be measured with that tool at all, and this reads the
profiler's own per-window lines instead: each is already an average over its window, so a
median of medians is the honest statistic and the draw count is right there on the line.

Reports, per draw band: the number of windows, the median ms/frame, and the median
`readback` share — the last being the column the change is supposed to zero.

    tools/part54_swap_bins.py <base.log> <arm.log> [--band 500]
"""
import re, sys, argparse, statistics

LINE = re.compile(
    r"\[vkprof\] ([\d.]+) fps \(([\d.]+) ms/frame, (\d+) draws/frame\).*?"
    r"readback ([\d.]+)%")

def read(path):
    out = []
    with open(path, "rb") as f:
        for raw in f:
            m = LINE.search(raw.decode("utf-8", "replace"))
            if m:
                out.append((int(m.group(3)), float(m.group(2)), float(m.group(4))))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", nargs="+")
    ap.add_argument("--arm", nargs="+", required=True)
    ap.add_argument("--band", type=int, default=1000)
    a = ap.parse_args()
    arms = {"base": [], "arm": []}
    for p in a.base: arms["base"] += read(p)
    for p in a.arm:  arms["arm"]  += read(p)
    for k, v in arms.items():
        print(f"{k}: {len(v)} profiler windows, "
              f"{min(x[0] for x in v)}-{max(x[0] for x in v)} draws")
    print()
    print(f"{'draw band':>16} | {'base ms':>9} {'n':>3} | {'arm ms':>9} {'n':>3} | "
          f"{'delta':>8} | {'base rb%':>8} {'arm rb%':>8}")
    lo = min(x[0] for x in arms["base"] + arms["arm"]) // a.band * a.band
    hi = max(x[0] for x in arms["base"] + arms["arm"])
    b = lo
    while b <= hi:
        sel = {k: [x for x in v if b <= x[0] < b + a.band] for k, v in arms.items()}
        # A band with fewer than three windows an arm is one sample wearing a median's
        # clothes; it is printed with its count so it can be discounted, not hidden.
        if not sel["base"] or not sel["arm"]:
            b += a.band; continue
        mb = statistics.median(x[1] for x in sel["base"])
        ma = statistics.median(x[1] for x in sel["arm"])
        rb = statistics.median(x[2] for x in sel["base"])
        ra = statistics.median(x[2] for x in sel["arm"])
        print(f"{b:>7}-{b+a.band-1:<8} | {mb:9.2f} {len(sel['base']):>3} | "
              f"{ma:9.2f} {len(sel['arm']):>3} | {100*(ma-mb)/mb:+7.1f}% | "
              f"{rb:8.1f} {ra:8.1f}")
        b += a.band

main()
