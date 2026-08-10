#!/usr/bin/env python3
"""How many frames of two runs are COMPARABLE at all — the null arm of every picture A/B.

WHY THIS EXISTS
---------------
Two arms of a renderer A/B are only comparable where they rendered the same thing:
`drawFingerprint` (FNV over every draw's shader pair, primitive and index count) says
what the guest asked for, and `cameraFingerprint` says where it asked for it from. Two
frames that differ on either are two different pictures, and comparing them measures the
game, not the change (gotcha 247, docs/measurement.md).

Phase C part 25 measured that filter on this title's synthetic-input recipes and got
13-44 surviving frames out of ~300, EVERY ONE of them under 1,800 draws — so the outdoor
era, which is where the interesting picture defects are, had never been compared at all.
Part 26's DebugJump route reaches 7,300 draws; whether it is COMPARABLE is a different
question, and this tool is the one that answers it before anything is measured on top.

It is deliberately a NULL measurement: run it on two runs of ONE configuration. Whatever
it reports there is the ceiling for any two-arm comparison on the same recipe, because no
A/B can match better than the same binary matches itself. Quote its number before quoting
any effect (the mechanical form of gotchas 246/248/249: measure the arm against itself
first, in the same block, and quote ratios).

WHAT IT PRINTS
--------------
Two matchings, because they answer different questions:

  BY PRESENT INDEX   frame N of run A against frame N of run B. This is how
                     frame_matched_diff.py pairs pixel dumps, so its answer is the one
                     that bounds a pixel comparison.
  BY CONTENT         every (drawFingerprint, cameraFingerprint) pair that occurs in both
                     runs, wherever it occurs. This is the strictly more generous
                     matching — a run that lags by ten frames scores zero on the first and
                     well on the second — and it bounds any comparison that is allowed to
                     realign.

For each it reports how many frames survive and, crucially, THEIR DRAW COUNTS: a match
rate of 90% is worthless if every matched frame is a 40-draw loading screen. The draw
distribution is the difference between "these runs agree" and "these runs agree about
nothing".

USAGE
    frame_determinism.py <a.txt> <b.txt> [--min-draws N]
"""
import argparse
import statistics
import sys


def read(path):
    """(frame, draws, drawFp, cameraFp) per presented frame."""
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            p = line.split()
            if len(p) < 5:
                continue
            try:
                rows.append((int(p[0]), int(p[1]), p[3], p[4]))
            except ValueError:
                continue
    return rows


def describe(name, draws, total):
    if not draws:
        print(f"  {name:<22} 0 of {total} frames")
        return
    draws = sorted(draws)
    print(f"  {name:<22} {len(draws)} of {total} frames "
          f"({100.0 * len(draws) / total:.1f}%)  draws: "
          f"min {draws[0]}, median {int(statistics.median(draws))}, max {draws[-1]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--min-draws', type=int, default=1800,
                    help='the "interesting" threshold; part 25 measured that every '
                         'admissible frame on the old recipes was below 1800 (default)')
    args = ap.parse_args()

    A, B = read(args.a), read(args.b)
    if not A or not B:
        print("one of the runs has no frames", file=sys.stderr)
        return 1
    print(f"A: {args.a}  {len(A)} frames, peak {max(r[1] for r in A)} draws")
    print(f"B: {args.b}  {len(B)} frames, peak {max(r[1] for r in B)} draws")

    # --- by present index ---
    bi = {r[0]: r for r in B}
    common = [(a, bi[a[0]]) for a in A if a[0] in bi]
    print(f"\nBY PRESENT INDEX ({len(common)} indices in both runs)")
    both = [a[1] for a, b in common if a[2] == b[2] and a[3] == b[3]]
    draw_only = [a[1] for a, b in common if a[2] == b[2] and a[3] != b[3]]
    cam_only = [a[1] for a, b in common if a[2] != b[2] and a[3] == b[3]]
    describe("both fingerprints", both, len(common))
    describe("draw only", draw_only, len(common))
    describe("camera only", cam_only, len(common))
    describe(f"both, >= {args.min_draws} draws",
             [d for d in both if d >= args.min_draws], len(common))

    # --- by content, anywhere in the run ---
    keyB = {}
    for r in B:
        keyB.setdefault((r[2], r[3]), []).append(r)
    hit = [r for r in A if (r[2], r[3]) in keyB]
    print(f"\nBY CONTENT (any index, {len(set(keyB))} distinct pairs in B)")
    describe("matched", [r[1] for r in hit], len(A))
    describe(f"matched, >= {args.min_draws} draws",
             [r[1] for r in hit if r[1] >= args.min_draws], len(A))

    # Within-run distinctness, because a run whose own frames repeat can match by
    # accident: 300 identical black frames match 300 identical black frames perfectly
    # and say nothing (frame_compare.py's docstring records that exact failure).
    for name, rows in (("A", A), ("B", B)):
        pairs = {(r[2], r[3]) for r in rows}
        print(f"\n{name}: {len(pairs)} distinct (draw, camera) pairs in {len(rows)} frames "
              f"— {100.0 * len(pairs) / len(rows):.1f}% distinct")
    return 0


if __name__ == '__main__':
    sys.exit(main())
