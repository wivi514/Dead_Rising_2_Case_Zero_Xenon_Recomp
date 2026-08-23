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

# THE TAIL FIELDS ARE OPTIONAL ON PURPOSE. Part 71 added `p99 / worst / >2x med` to the
# `[fps]` line for the turn-stutter question (a median is the statistic least able to see
# a stutter — gotcha 237). Every log this project recorded before that lacks them, and a
# regex that required them would silently read ZERO windows out of parts 54-70's arms —
# which is gotcha 25's shape exactly, a grep that cannot match reading as a clean result.
LINE = re.compile(
    r"\[fps\] ([\d.]+) fps mean \(([\d.]+) ms\) \| ([\d.]+) fps median \(([\d.]+) ms\) \| "
    r"(?:p99 ([\d.]+) ms \| worst ([\d.]+) ms \| >2x med ([\d.]+)% \| )?"
    r"(\d+) frames in ([\d.]+) s \| draws med (\d+) \((\d+)\.\.(\d+)\)")

def read(path, spread):
    out, dropped = [], 0
    with open(path, "rb") as f:
        for raw in f:
            m = LINE.search(raw.decode("utf-8", "replace"))
            if not m:
                continue
            medFps, medMs = float(m.group(3)), float(m.group(4))
            p99 = float(m.group(5)) if m.group(5) else None
            over = float(m.group(7)) if m.group(7) else None
            d, lo, hi = int(m.group(10)), int(m.group(11)), int(m.group(12))
            # A window whose draw count moved by more than `spread` of its median is not
            # one place. Counted, not silently skipped.
            if d == 0 or (hi - lo) / d > spread:
                dropped += 1
                continue
            out.append((d, medFps, medMs, p99, over))
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
    haveTail = any(x[3] is not None for x in A) and any(x[3] is not None for x in B)
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

    # THE TAIL, in its own table because it answers a different question. The frame-time
    # bands above say how FAST the arm is; these say how EVEN it is, which is the only
    # thing that can speak to a reported stutter. A change that improves the median and
    # worsens p99 is a regression to the person holding the pad, and the table above
    # cannot say so.
    if haveTail:
        print()
        print("  the TAIL (turn stutter, part 71) — a median cannot see this")
        print(f"{'draw band':>15} | {'base p99':>9} {'>2x':>6} | "
              f"{'arm p99':>9} {'>2x':>6} | {'d p99':>8}")
        b = lo
        while b <= hi:
            sa = [x for x in A if b <= x[0] < b + a.band and x[3] is not None]
            sb = [x for x in B if b <= x[0] < b + a.band and x[3] is not None]
            if sa and sb:
                pa = statistics.median(x[3] for x in sa)
                pb = statistics.median(x[3] for x in sb)
                oa = statistics.median(x[4] for x in sa)
                ob = statistics.median(x[4] for x in sb)
                print(f"{b:>6}-{b+a.band-1:<8} | {pa:8.2f}m {oa:5.1f}% | "
                      f"{pb:8.2f}m {ob:5.1f}% | {pb-pa:+7.2f}m")
            b += a.band

main()
