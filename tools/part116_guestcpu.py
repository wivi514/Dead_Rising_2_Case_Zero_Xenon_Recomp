#!/usr/bin/env python3
"""The GUEST's CPU per frame (Main Thread, Draw Thread) and the wall, banded by draws.

The part-116 reader. `tools/part110_pumpcpu.py` bands the PUMP's CPU per frame; the
guest's own threads are the other floor of `wall ~ max(pump, guest, GPU)` and as of
part 116 the [fps] line carries them (`guest main N.NN draw N.NN ms/frame`, read from
each named thread's CPU clock over the same window). Same banding, same rule: medians
per 250-draw band, matched bands between arms, never a run-wide mean.

WHICH WALL. On a normal arm the wall is the pump (10-11 ms) and a guest-side saving is
INVISIBLE in it — that is part 110's whole finding. So a guest change is read here two
ways: its own columns on the normal arm, and the WALL on the `CZ_VK_NO_DODRAW=1` arm,
where the per-draw renderer is deleted and the wall IS the guest floor (8.8 ms in
part 110). Both arms of an A/B must be the same kind.

Usage:
    tools/part116_guestcpu.py <logs...>                # one arm
    tools/part116_guestcpu.py <A logs> -- <B logs>     # A/B, matched bands, B - A
"""
import collections, re, statistics, sys

BAND = 250
DRAW_FLOOR = 8000
PAT = re.compile(r"\[fps\][^|]*\|\s*([\d.]+) fps median \(([\d.]+) ms\)"
                 r".*?draws med (\d+)"
                 r".*?pump cpu ([\d.]+) ms/frame"
                 r".*?guest main ([-\d.]+) draw ([-\d.]+) ms/frame")

def load(paths):
    rows = []
    for p in paths:
        n = 0
        for line in open(p, errors="replace"):
            m = PAT.search(line)
            if m and int(m.group(3)) >= DRAW_FLOOR and float(m.group(5)) >= 0:
                rows.append((int(m.group(3)), float(m.group(2)), float(m.group(4)),
                             float(m.group(5)), float(m.group(6))))
                n += 1
        if n == 0:
            print(f"  !! no crowd windows with guest columns in {p}")
    return rows

COLS = ["wall", "pump", "main", "draw"]

def report(name, rows):
    if not rows:
        print(f"{name}: no crowd windows"); return {}
    b = collections.defaultdict(list)
    for r in rows:
        b[(r[0] // BAND) * BAND].append(r[1:])
    print(f"\n=== {name}: {len(rows)} windows >= {DRAW_FLOOR} draws")
    print(f"  {'band':>7} {'n':>3} " + " ".join(f"{c:>8}" for c in COLS))
    out = {}
    for k in sorted(b):
        med = [statistics.median(x[i] for x in b[k]) for i in range(4)]
        out[k] = (med, len(b[k]))
        print(f"  {k:>7} {len(b[k]):>3} " + " ".join(f"{m:8.2f}" for m in med))
    allm = [statistics.median(x[i + 1] for x in rows) for i in range(4)]
    print(f"  {'ALL':>7} {len(rows):>3} " + " ".join(f"{m:8.2f}" for m in allm))
    return out

def main():
    argv = sys.argv[1:]
    if not argv:
        sys.exit(__doc__)
    if "--" in argv:
        i = argv.index("--"); a, bl = argv[:i], argv[i+1:]
    else:
        a, bl = argv, []
    ra = report("A", load(a))
    if not bl:
        return
    rb = report("B", load(bl))
    shared = sorted(set(ra) & set(rb))
    print(f"\n=== MATCHED BANDS (B - A), ms/frame")
    if not shared:
        print("  !! no band in both arms — not comparable (gotcha 544)"); return
    print(f"  {'band':>7} {'nA':>3} {'nB':>3} " + " ".join(f"{c:>8}" for c in COLS))
    deltas = collections.defaultdict(list)
    for k in shared:
        ma, na = ra[k]; mb, nb = rb[k]
        d = [mb[i] - ma[i] for i in range(4)]
        for i in range(4): deltas[i].append(d[i])
        print(f"  {k:>7} {na:>3} {nb:>3} " + " ".join(f"{x:+8.2f}" for x in d))
    for i, c in enumerate(COLS):
        dl = deltas[i]
        mono = all(x < 0 for x in dl) or all(x > 0 for x in dl)
        print(f"  median {c:>5} delta over {len(shared)} bands: {statistics.median(dl):+.2f} ms"
              f"  ({'monotone' if mono else 'NOT monotone'})")

if __name__ == "__main__":
    main()
