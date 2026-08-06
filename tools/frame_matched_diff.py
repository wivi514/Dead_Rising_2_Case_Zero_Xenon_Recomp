#!/usr/bin/env python3
"""Cross-arm frame difference AGAINST THE WITHIN-ARM NOISE FLOOR.

IMPORTED FROM Fable2XenonRecomp/tools/frame_matched_diff.py, essentially unchanged.
Only the default --split differs (Case Zero has no world era; its interesting boundary
is the title screen, around present index 600).

It is imported rather than rewritten because its central idea is better than the one
this port reached independently. `tools/frame_compare.py` compares a run against a
baseline band MEASURED ONCE and quoted as a constant; this measures the within-arm
noise floor **in the same units, from the same runs, at the same time**. A quoted band
goes stale the moment anything about the machine or the binary changes, and nothing
announces that it has. Prefer this tool whenever there are >= 2 runs per arm.

The two are complementary rather than redundant: frame_compare.py reads the cheap
per-frame stats file (whole run, any surface, no pixel dumps), this reads actual pixel
dumps and answers "do the two arms render the same picture".

Why it exists. Every summary statistic Fable 2 tried on a world A/B
(mean luma, saturation, lit %, frame delta) has failed to separate a real change
from run-to-run noise -- six times, recorded in CLAUDE.md. The reason is always
the same: the world era is nondeterministic, so the spread WITHIN an arm is as
large as the gap BETWEEN arms, and a single number per arm cannot show that.

So measure the thing that actually answers a regression question -- "do the two
arms render the same picture?" -- and measure the noise floor in the same units
at the same time:

    within-A   mean |RGB difference| between two runs of arm A at matched indices
    within-B   the same for arm B
    cross      arm A run vs arm B run, matched indices

If cross ~= within, the arms are indistinguishable at this sample size: for a
change that is meant to be invisible (a present-source change) that is a PASS,
and for one that is meant to alter the picture it is a null result. If cross is
well above both withins, the arms genuinely differ -- and then the per-window
split says WHERE: the early window is boot/logos/menu/loading, which are scripted
and near-deterministic, so a difference there is a regression in something that
demonstrably worked.

Frames are matched by PRESENT INDEX parsed from the filename, the same
convention as frame_ab_report.py. Only indices present in both runs are used.

Usage:
  frame_matched_diff.py --a <run-dir> [<run-dir> ...] --b <run-dir> [...]
                        [--split N] [--max-pairs N] [--quiet]
"""
import argparse
import itertools
import os
import re
import sys

import numpy as np
from PIL import Image


def frame_dir(d):
    for cand in (os.path.join(d, 'frames'), os.path.join(d, 'dump'), d):
        if os.path.isdir(cand) and any(f.endswith('.ppm') for f in os.listdir(cand)):
            return cand
    return None


def index_of(name):
    m = re.search(r'(\d+)\.ppm$', name)
    return int(m.group(1)) if m else -1


def frames(d):
    """{present index: path} for one run dir."""
    fd = frame_dir(d)
    if not fd:
        return {}
    out = {}
    for f in os.listdir(fd):
        if f.endswith('.ppm'):
            i = index_of(f)
            if i >= 0:
                out[i] = os.path.join(fd, f)
    return out


_cache = {}


def load(path):
    if path not in _cache:
        # Keep the cache small: these are 2.7 MB each and a campaign has hundreds.
        if len(_cache) > 64:
            _cache.clear()
        _cache[path] = np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)
    return _cache[path]


def pair_diff(da, db, split):
    """Mean |RGB diff| per matched index, split into (early, late) lists."""
    fa, fb = frames(da), frames(db)
    early, late = [], []
    for i in sorted(set(fa) & set(fb)):
        a, b = load(fa[i]), load(fb[i])
        if a.shape != b.shape:
            continue
        d = float(np.abs(a - b).mean())
        (early if i < split else late).append(d)
    return early, late


def summarise(label, dirpairs, split, quiet):
    early_all, late_all = [], []
    for da, db in dirpairs:
        e, l = pair_diff(da, db, split)
        early_all += e
        late_all += l
        if not quiet:
            print(f'    {os.path.basename(da)} vs {os.path.basename(db)}: '
                  f'early n={len(e)} med={np.median(e) if e else float("nan"):.2f}  '
                  f'late n={len(l)} med={np.median(l) if l else float("nan"):.2f}')
    def med(x):
        return float(np.median(x)) if x else float('nan')
    print(f'  {label:10s} early med={med(early_all):7.2f} (n={len(early_all):4d})   '
          f'late med={med(late_all):7.2f} (n={len(late_all):4d})')
    return med(early_all), med(late_all)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', nargs='+', required=True)
    ap.add_argument('--b', nargs='+', required=True)
    ap.add_argument('--split', type=int, default=600,
                    help='present index splitting early/late (default 600: Case Zero '
                         'reaches its title screen around there)')
    ap.add_argument('--max-pairs', type=int, default=6,
                    help='cap on run pairs per category, to bound runtime')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    within_a = list(itertools.combinations(args.a, 2))[:args.max_pairs]
    within_b = list(itertools.combinations(args.b, 2))[:args.max_pairs]
    cross = [(x, y) for x in args.a for y in args.b][:args.max_pairs]

    print(f'matched-index mean |RGB diff|  (early = index < {args.split}, '
          f'late = >= {args.split})')
    if not within_a and not within_b:
        print('  NO WITHIN-ARM PAIRS: with one run per arm there is no noise floor,')
        print('  so a cross-arm number cannot be interpreted. Run more rounds.')
    ea, la = summarise('within-A', within_a, args.split, args.quiet)
    eb, lb = summarise('within-B', within_b, args.split, args.quiet)
    ec, lc = summarise('cross', cross, args.split, args.quiet)

    floor_e = np.nanmax([ea, eb])
    floor_l = np.nanmax([la, lb])
    print()
    for name, c, f in (('early', ec, floor_e), ('late', lc, floor_l)):
        if np.isnan(c) or np.isnan(f):
            print(f'  {name}: not enough data')
        elif c <= f * 1.25:
            print(f'  {name}: cross {c:.2f} vs noise floor {f:.2f} -> '
                  f'INDISTINGUISHABLE at this sample size')
        else:
            print(f'  {name}: cross {c:.2f} vs noise floor {f:.2f} -> '
                  f'ARMS DIFFER ({c / f:.2f}x the floor)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
