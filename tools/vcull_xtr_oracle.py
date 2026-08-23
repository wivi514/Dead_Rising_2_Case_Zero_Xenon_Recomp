#!/usr/bin/env python3
"""Does the vertical-waste census place its boxes correctly? Ask hardware, offline.

WHY THIS EXISTS
---------------
Part 72 built a census (`CZ_VK_VCULL_CENSUS=1`) to price the 21:9 culling over-widen: it
projects each world draw's bounding box and counts the ones landing entirely outside the
clip volume. In its first operator session it read **98.1% of world draws as entirely
off-screen horizontally** — leaving ~142 on-screen draws to paint a scene submitting
9,750. Only a control added on a hunch caught it.

The cause was that the boxes were never PLACED: the census projected OBJECT-space
positions by the CAMERA matrix, because it was built on §6cs's conclusion that
composite-draw streams are world-space. Part 67 had already refuted that and measured the
consequence over the same traces this tool reads — boxes intersecting the frustum they
were drawn into go from **0.1% untransformed to 97.8% placed**.

That fix shipped as a PREDICTION, and the standing rule here is that all runtime
verification goes through the operator, so it would have sat unverified until the next
sitting. **It does not have to.** The `.xtr` captures carry hardware's own draws with
their own constant windows, so the census's arithmetic can be run against them with no
runtime and no operator at all — and the answer comes from a source this project did not
write, which is what makes it an oracle rather than two of our own components agreeing.

WHAT IT CHECKS, and both halves matter:

  * PLACED — the fix. The on-screen share should be a large majority, near part 67's
    97.8%. This is the census's own invariant (it refuses to report below 50%), computed
    on hardware's draws instead of ours.
  * UNPLACED — the bug, reproduced deliberately. Re-running with the placement removed
    must collapse to a few percent. A fix whose "before" case cannot be shown to fail has
    not been shown to fix anything (gotcha 30), and here the before case is a one-line
    switch rather than a rebuild.

WHAT IT CANNOT DO. These captures are hardware at 16:9 with the game's own fov, so they
do not contain our wide-mode substitution and **cannot price item 1**. They validate the
machinery, not the item. Do not quote a vertical-waste figure from this tool as the
over-widen's cost.

The clip test, the corner walk and the placement below are the same arithmetic as
`VerticalWasteCensus` in runtime/gpu/vk_renderer.cpp, and `tools/vcull_predicate_test.cpp`
gates the C++ side of it independently.

    python3 tools/vcull_xtr_oracle.py                 # every trace on disc
    python3 tools/vcull_xtr_oracle.py --trace X.xtr
    python3 tools/vcull_xtr_oracle.py --no-placement  # the deliberate breakage
"""
import argparse
import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
_spec = importlib.util.spec_from_file_location(
    'rt_tlas_census', Path(__file__).resolve().parent / 'rt_tlas_census.py')
census = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(census)
ROOT = census.ROOT


def classify(m, xf, mn, mx, scale=1.0):
    """The census's predicate. -1 near-plane, else bit0 = off-screen V, bit1 = off-screen H.

    Corners are placed and projected one at a time — never an AABB re-derived from a
    transformed box, which inflates and would read "on screen" for a box that is not
    (`project-the-points-not-the-box`).
    """
    all_above = all_below = all_left = all_right = True
    for c in range(8):
        ox = mx[0] if c & 1 else mn[0]
        oy = mx[1] if c & 2 else mn[1]
        oz = mx[2] if c & 4 else mn[2]
        x = xf[0] * ox + xf[1] * oy + xf[2] * oz + xf[3]
        y = xf[4] * ox + xf[5] * oy + xf[6] * oz + xf[7]
        z = xf[8] * ox + xf[9] * oy + xf[10] * oz + xf[11]
        cw = m[12] * x + m[13] * y + m[14] * z + m[15]
        if not cw > 0.0:
            return -1
        cx = m[0] * x + m[1] * y + m[2] * z + m[3]
        cy = m[4] * x + m[5] * y + m[6] * z + m[7]
        b = scale * cw
        if cy <= b:
            all_above = False
        if cy >= -b:
            all_below = False
        if cx >= -b:
            all_left = False
        if cx <= b:
            all_right = False
    return (1 if (all_above or all_below) else 0) | (2 if (all_left or all_right) else 0)


IDENT = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]


def run(trace, sidecars, xform, place=True, scale=1.0):
    draws, _mem = census.walk_and_classify(trace, sidecars, with_mem=True)
    n = on = offv = offh = near = noxf = palette = 0
    for d in draws:
        if d.get('bucket') != 'ok' or 'mn' not in d:
            continue
        stages = xform.get(d['vs'] or '')
        if stages is None:
            noxf += 1
            continue
        # The runtime declines palette draws: part 69 read 0 of 2,786 referencing a single
        # matrix, so entry 0 would pile a batch onto whichever prop is bone 0. Decline them
        # here too, or the two would be measuring different populations.
        if any(k == 'palette' for _b, k in stages):
            palette += 1
            continue
        xf = IDENT if not place else census.world_xform(d['win'], stages)
        if xf is None:
            noxf += 1
            continue
        r = classify(d['win'][:16], xf, d['mn'], d['mx'], scale)
        n += 1
        if r < 0:
            near += 1
        else:
            if r & 1:
                offv += 1
            if r & 2:
                offh += 1
            if r == 0:
                on += 1
    return dict(n=n, on=on, offv=offv, offh=offh, near=near, noxf=noxf, palette=palette)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('--xform', default=str(census.WORLD_XFORM))
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--no-placement', action='store_true',
                    help='reproduce the defect: project object space by the camera')
    ap.add_argument('--min-on', type=float, default=50.0,
                    help='placed on-screen share below which this exits non-zero')
    args = ap.parse_args()

    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in census.DEFAULT_TRACE_DIRS
                     for p in (ROOT / d).rglob('*.xtr')))
    xform = census.load_world_xform(args.xform)
    sidecars = census.load_sidecars(args.spv)
    if not xform or not sidecars:
        print('need both the shader cache and config/rt_world_xform.json', file=sys.stderr)
        return 2

    tot = dict(n=0, on=0, offv=0, offh=0, near=0, noxf=0, palette=0)
    print('%-26s %7s %8s %8s %8s %7s' % ('trace', 'draws', 'ON', 'off-V', 'off-H', 'near'))
    for tp in traces:
        try:
            r = run(tp, sidecars, xform, place=not args.no_placement, scale=args.scale)
        except Exception as e:                       # a trace we cannot walk is NAMED
            print('%-26s  unreadable: %s' % (tp.stem, e))
            continue
        if not r['n']:
            print('%-26s  no classifiable world draws' % tp.stem)
            continue
        for k in tot:
            tot[k] += r[k]
        print('%-26s %7d %7.1f%% %7.1f%% %7.1f%% %6.1f%%'
              % (tp.stem, r['n'], 100.0 * r['on'] / r['n'], 100.0 * r['offv'] / r['n'],
                 100.0 * r['offh'] / r['n'], 100.0 * r['near'] / r['n']))
    if not tot['n']:
        print('no classifiable world draws in any trace', file=sys.stderr)
        return 2
    share = 100.0 * tot['on'] / tot['n']
    print('%-26s %7d %7.1f%% %7.1f%% %7.1f%% %6.1f%%'
          % ('ALL', tot['n'], share, 100.0 * tot['offv'] / tot['n'],
             100.0 * tot['offh'] / tot['n'], 100.0 * tot['near'] / tot['n']))
    print('declined: %d no table entry, %d palette (the runtime declines these too)'
          % (tot['noxf'], tot['palette']))
    if args.no_placement:
        print('\nPLACEMENT OFF — this is the DEFECT reproduced. A low ON share here is the '
              'expected result and is what part 67 measured as 0.1%.')
        return 0
    print('\nplaced on-screen share %.1f%% (part 67 measured 97.8%% over 46,820 draws by '
          'the same route)' % share)
    if share < args.min_on:
        print('** FAIL: below %.0f%%, which is the census\'s own refusal threshold. The '
              'placement is not working.' % args.min_on)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
