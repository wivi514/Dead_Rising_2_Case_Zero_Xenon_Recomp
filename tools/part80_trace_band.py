#!/usr/bin/env python3
"""Compare two arms on the crowd route by pooling their FRAME TRACES into draw bands.

WHY NOT `part76_band.py`. That tool reads `[fps]` windows, which are five-second
aggregates. On the part-80 crowd route the two arms' window draw-medians rarely land in the
same 250-draw bucket, so the matched-band table comes out EMPTY and the only number left is
a whole-crowd median across populations whose draw medians differ by 300 — which is a
composition difference reported as a result. That is the same trap part 79's picture gate
fell into (§6dw §5): arms compared over populations that are not the same population.

A frame trace is one line per frame, so it can be banded directly and the bands can be made
narrow enough to be honest. It also carries the columns the regime turns on — GPU, fence
and CPUrec — so the same table says whether a saving could have converted at all.

WHY BANDING IS NOT OPTIONAL HERE. The crowd route does not land on the same spot twice, and
the operator diagnosed why: *"it is because the random zombie spawn placing zombies on the
way."* That is the title's own non-determinism, so frame N of one run is not frame N of
another and the draw distributions differ run to run. Banding is what makes two runs
comparable at all; matched frame indices are unavailable outdoors here for the same reason
they were unavailable in part 26 (gotcha 254).

TEXTURE-UPLOAD FRAMES ARE EXCLUDED FROM THE THROUGHPUT TABLE and counted separately. They
are 82-90% texture work and are the port's hitch item, not its throughput item; folding
them in moves a median for a reason that has nothing to do with what is being measured.

Usage:
    tools/part80_trace_band.py A=<glob> B=<glob> [--bin 500] [--min-n 200]
"""
import sys, glob, statistics, argparse

ap = argparse.ArgumentParser()
ap.add_argument("arms", nargs=2, metavar="NAME=GLOB")
ap.add_argument("--bin", type=int, default=500)
ap.add_argument("--min-n", type=int, default=200,
                help="bands with fewer frames than this in EITHER arm are not printed: a "
                     "median over a handful of frames on a non-deterministic route is not "
                     "a measurement")
a = ap.parse_args()


def load(pattern):
    """Every frame of every trace matching the glob, pooled.

    Pooled rather than averaged per run, deliberately: the runs are replicates of one
    configuration, and pooling weights each band by how many frames actually landed in it
    instead of by how many runs happened to reach it.
    """
    rows, files = [], sorted(glob.glob(pattern))
    for p in files:
        with open(p, errors="replace") as f:
            head = f.readline().split()
            if not head or head[0] != "frame":
                continue
            idx = {n: i for i, n in enumerate(head)}
            for line in f:
                v = line.split()
                if len(v) != len(head):
                    continue
                try:
                    rows.append({k: int(v[i]) for k, i in idx.items()})
                except ValueError:
                    continue
    return rows, files


out = []
for spec in a.arms:
    name, pat = spec.split("=", 1)
    rows, files = load(pat)
    if not rows:
        sys.exit(f"** arm {name}: no frame-trace rows matched {pat}")
    out.append((name, rows, files))

for name, rows, files in out:
    print(f"{name}: {len(rows)} frames from {len(files)} run(s)")
    for f in files:
        print(f"    {f}")

(na, A, _), (nb, B, _) = out


def band(rows, lo, hi, col, tex):
    v = [r[col] / 1000.0 for r in rows
         if lo <= r["draws"] < hi and ((r.get("texUploads", 0) > 0) == tex)]
    return v


lo_all = min(r["draws"] for r in A + B)
hi_all = max(r["draws"] for r in A + B)
print(f"\nTHROUGHPUT — frames with NO texture upload, {a.bin}-draw bands, medians in ms")
print(f"  {'band':>13} {na+' wall':>10} {'n':>6} {nb+' wall':>10} {'n':>6} "
      f"{'delta':>8} {'ms':>7} | {na+' GPU':>8} {nb+' GPU':>8} {'fence':>7}")
tot = []
for lo in range(0, hi_all + a.bin, a.bin):
    x = band(A, lo, lo + a.bin, "wallUs", False)
    y = band(B, lo, lo + a.bin, "wallUs", False)
    if len(x) < a.min_n or len(y) < a.min_n:
        continue
    mx, my = statistics.median(x), statistics.median(y)
    gx = statistics.median(band(A, lo, lo + a.bin, "gpuUs", False) or [0])
    gy = statistics.median(band(B, lo, lo + a.bin, "gpuUs", False) or [0])
    fy = statistics.median(band(B, lo, lo + a.bin, "fenceUs", False) or [0])
    print(f"  {lo:6d}-{lo+a.bin:6d} {mx:10.2f} {len(x):6d} {my:10.2f} {len(y):6d} "
          f"{100*(my-mx)/mx:+7.1f}% {my-mx:+7.2f} | {gx:8.2f} {gy:8.2f} {fy:7.2f}")
    tot.append((len(x) + len(y), 100 * (my - mx) / mx))

if tot:
    w = sum(n for n, _ in tot)
    print(f"\n  frame-weighted mean delta across printed bands: "
          f"{sum(n * d for n, d in tot) / w:+.1f}%")
    print(f"  bands: {len(tot)}   monotone: "
          f"{'yes' if all(d < 0 for _, d in tot) or all(d > 0 for _, d in tot) else 'NO'}"
          "   (a real change moves every band the same way; a mixed sign is noise or "
          "composition)")

# The hitch population, reported rather than dropped -- an item that moves throughput and
# hitches in opposite directions would otherwise look like a clean win.
for nm, rows in ((na, A), (nb, B)):
    t = [r["wallUs"] / 1000.0 for r in rows if r.get("texUploads", 0) > 0]
    if t:
        t.sort()
        print(f"  {nm} texture-upload frames: n={len(t)} median {statistics.median(t):.2f} "
              f"p99 {t[len(t) - 1 - (len(t) - 1)//100]:.2f} worst {t[-1]:.2f} ms")
