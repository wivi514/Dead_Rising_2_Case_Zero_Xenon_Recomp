#!/usr/bin/env python3
"""Compare two `CZ_FPS_LOG` arms, MATCHED ON DRAW COUNT.

WHY THIS EXISTS. The operator's objection, in their words: AutoChuck "isn't a
predetermined route and zombie spawns are not always the same, so it won't be 100%
accurate especially if in a run it stays in the military zone and one go on the main
street." A soak removes the route, but two soaks still land on slightly different crowds —
part 54's pair sat at ~6,700 draws in one arm and ~7,000 in the other, and comparing their
means would have credited the difference in crowd to the change.

So this bins `[fps]` windows by the DRAW COUNT the line now carries, and reports only bands
where both arms have windows. It also drops any window whose (min..max) draw spread is wide:
that window straddled two places and its median is not a place at all.

    tools/part54_fps_bins.py <armA.log> --arm <armB.log> [--band 250] [--spread 0.25]
"""
import re, sys, argparse, statistics

LINE = re.compile(
    r"\[fps\] ([\d.]+) fps mean \(([\d.]+) ms\) \| ([\d.]+) fps median \(([\d.]+) ms\) \| "
    r"(\d+) frames in ([\d.]+) s \| draws med (\d+) \((\d+)\.\.(\d+)\)")

def read(path, spread):
    out, dropped = [], 0
    with open(path, "rb") as f:
        for raw in f:
            m = LINE.search(raw.decode("utf-8", "replace"))
            if not m:
                continue
            medFps, medMs = float(m.group(3)), float(m.group(4))
            d, lo, hi = int(m.group(7)), int(m.group(8)), int(m.group(9))
            # A window whose draw count moved by more than `spread` of its median is not
            # one place. Counted, not silently skipped.
            if d == 0 or (hi - lo) / d > spread:
                dropped += 1
                continue
            out.append((d, medFps, medMs))
    return out, dropped

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("--arm", required=True)
    ap.add_argument("--band", type=int, default=250)
    ap.add_argument("--spread", type=float, default=0.25)
    a = ap.parse_args()
    A, dA = read(a.base, a.spread)
    B, dB = read(a.arm, a.spread)
    print(f"base {a.base}: {len(A)} windows kept, {dA} dropped as straddling two places")
    print(f"arm  {a.arm}: {len(B)} windows kept, {dB} dropped")
    if not A or not B:
        return
    print()
    print(f"{'draw band':>15} | {'base fps':>8} {'ms':>6} {'n':>3} | "
          f"{'arm fps':>8} {'ms':>6} {'n':>3} | {'d fps':>7} {'d ms':>7} {'delta':>8}")
    lo = min(x[0] for x in A + B) // a.band * a.band
    hi = max(x[0] for x in A + B)
    b = lo
    while b <= hi:
        sa = [x for x in A if b <= x[0] < b + a.band]
        sb = [x for x in B if b <= x[0] < b + a.band]
        if sa and sb:
            fa, fb = statistics.median(x[1] for x in sa), statistics.median(x[1] for x in sb)
            ma, mb = statistics.median(x[2] for x in sa), statistics.median(x[2] for x in sb)
            print(f"{b:>6}-{b+a.band-1:<8} | {fa:8.1f} {ma:6.2f} {len(sa):>3} | "
                  f"{fb:8.1f} {mb:6.2f} {len(sb):>3} | {fb-fa:+7.1f} {mb-ma:+7.2f} "
                  f"{100*(mb-ma)/ma:+7.1f}%")
        b += a.band

main()
