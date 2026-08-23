#!/usr/bin/env python3
"""DOES THE PALETTE BLEND PUT THE VERTICES WHERE HARDWARE PUT THEM?

WHY THIS EXISTS
---------------
`docs/rt-remix-plan.md` item 3 replaces the collector's "palette entry 0 with unit
weight" approximation with the real per-vertex blend, baked into the BLAS vertex data.
That is a change to WHERE 60% of the world's occluders sit, and the failure mode is
silent: a misplaced occluder is not a missing shadow, it is a shadow in the wrong place,
which is the class of defect this feature has already spent four parts failing to see.

So the blend is checked the same way part 67 checked the placement, against the same
oracle, on the same captures: project each vertex through the camera composite the draw
was ISSUED with and ask what fraction lands inside the frustum. Hardware drew these
meshes and they were visible, so a correct placement puts most of their vertices on
screen and a wrong one does not. Part 67 measured 0.0% against 61-98% per vertex that
way, which is the dynamic range this test has.

It reports three arms over the same draws, so the comparison is a fact about the blend
and not about which draws were readable:

    camera-only   no object transform at all — the part-66 renderer
    entry 0       the collector's approximation as shipped in parts 67-68
    per-vertex    the blend this tool exists to check

WHAT THE FRUSTUM TEST TURNED OUT NOT TO ANSWER — MEASURED, NOT ASSUMED
----------------------------------------------------------------------
It SATURATES on this population and therefore cannot separate the two arms: over the
gas-station trace, 1,063,570 palette vertices, entry 0 reads **96.55%** in frustum and the
per-vertex blend **96.38%**. That is not agreement, it is a statistic with no dynamic
range left (gotcha: a saturated count measures its emitter). Part 67's version of this
test worked because the defect put the whole town at the world ORIGIN, hundreds of units
away; collapsing a batch of props onto one of its own members moves a vertex by METRES,
and the frustum is a hundreds-of-metres test.

So the numbers this tool exists for are the DISPLACEMENT ones below, which have no such
ceiling: how far the blend moves each vertex away from the entry-0 approximation, and how
much wider the draw's world box gets. A batch of several props collapsed onto prop 0 has
a small box; blended, it spans the batch.

The end-to-end oracle is elsewhere and it is visual:
`tools/rt_placement_render.py --blend` projects the blended geometry through the camera
hardware itself used and puts it beside the frame-locked PNG.

USAGE
    tools/rt_bake_check.py                       # every world trace
    tools/rt_bake_check.py --trace FILE --top 8
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rt_tlas_census as C  # noqa: E402


def load_entries(path):
    """The RAW table entries, because this tool needs the blend field as well as the
    stages, and `rt_tlas_census.load_world_xform` returns only the parsed stage list."""
    import json
    try:
        return json.load(open(path))['shaders']
    except Exception as e:
        print('cannot read %s: %s' % (path, e), file=sys.stderr)
        return {}


def parse_blend(entry):
    """"slot:strideDw:weightOffDw:indexOffDw:bytes" -> a dict, or None."""
    b = (entry or {}).get('blend')
    if not b:
        return None
    f = b.split(':')
    if len(f) != 5:
        return None
    return {'slot': int(f[0]), 'stride': int(f[1]), 'w': int(f[2]), 'i': int(f[3]),
            'bytes': [int(c) for c in f[4]]}


def palette_base(entry):
    for tok in (entry or {}).get('stages', '').split(','):
        if tok.startswith('palette@'):
            return int(tok.split('@')[1])
    return None


def outer_stages(entry):
    """The stages that are NOT the palette — what the TLAS instance still carries."""
    out = []
    for tok in (entry or {}).get('stages', '').split(','):
        if '@' in tok and not tok.startswith('palette@'):
            out.append((int(tok.split('@')[1]), tok.split('@')[0]))
    return out


def in_clip(m, p):
    """Project a world point through the row-major 4x4 composite; inside the frustum?"""
    c = [m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]
         for r in range(4)]
    if c[3] <= 1e-6:
        return False
    return (abs(c[0]) <= c[3] and abs(c[1]) <= c[3] and -1e-4 <= c[2] <= c[3] + 1e-4)


def apply_affine(m, p):
    return [m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]
            for r in range(3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--cap', type=int, default=512, help='vertices sampled per draw')
    args = ap.parse_args()

    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in C.DEFAULT_TRACE_DIRS for p in (C.ROOT / d).rglob('*.xtr')))
    sidecars = C.load_sidecars(args.spv)
    xform = load_entries(str(C.WORLD_XFORM))
    if not traces or not sidecars or not xform:
        print('missing traces, sidecars or the transform table', file=sys.stderr)
        return 2

    tot = {'cam': 0, 'e0': 0, 'blend': 0}
    disp = []      # per draw, the MEDIAN distance the blend moves a vertex
    boxes = []     # per draw, (blended extent, entry-0 extent)
    verts = 0
    draws = 0
    oor = 0
    printed = 0
    for tp in traces:
        ds, mem = C.walk_and_classify(tp, sidecars, with_mem=True)
        for d in ds:
            if d.get('bucket') != 'ok' or not d.get('vs'):
                continue
            ent = xform.get(d['vs'])
            base = palette_base(ent)
            bl = parse_blend(ent)
            if base is None or bl is None:
                continue
            addr, size_dw, endian, stride_dw, off_dw = d['stream']
            if bl['stride'] != stride_dw:
                continue
            win = d['win']
            if any(v is None for v in win[:16]):
                continue
            cam = win[:16]
            # The outer stages, as the TLAS instance would carry them.
            outer = None
            for b, _k in outer_stages(ent):
                m = C.affine(win, b)
                if m is None:
                    continue
                outer = m if outer is None else C.compose(m, outer)

            nv = min(size_dw // stride_dw, args.cap)
            span = (nv - 1) * stride_dw * 4 + max(off_dw + 3, bl['w'], bl['i']) * 4 + 4
            blob = mem.read(addr, span)
            if blob is None or nv < 8:
                continue
            hit = {'cam': 0, 'e0': 0, 'blend': 0}
            n = 0
            moved = []
            bmn = [1e30] * 3
            bmx = [-1e30] * 3
            amn = [1e30] * 3
            amx = [-1e30] * 3
            e0 = C.affine(win, base)
            for v in range(nv):
                at = v * stride_dw * 4
                pc = blob[at + off_dw * 4: at + off_dw * 4 + 12]
                if len(pc) < 12:
                    break
                p = list(struct.unpack('>3f', pc))
                if any(abs(c) > 1e7 or c != c for c in p):
                    continue
                wd = struct.unpack('>I', blob[at + bl['w'] * 4: at + bl['w'] * 4 + 4])[0]
                idw = struct.unpack('>I', blob[at + bl['i'] * 4: at + bl['i'] * 4 + 4])[0]
                rows = [[0.0] * 4 for _ in range(3)]
                bad = False
                for byte in bl['bytes']:
                    wt = ((wd >> (byte * 8)) & 0xFF) / 255.0
                    if wt == 0.0:
                        continue
                    a0 = (idw >> (byte * 8)) & 0xFF
                    if base + a0 + 3 > C.WINDOW_ROWS:
                        bad = True
                        break
                    for r in range(3):
                        for c in range(4):
                            q = win[(base + a0 + r) * 4 + c]
                            if q is None:
                                bad = True
                                break
                            rows[r][c] += wt * q
                if bad:
                    oor += 1
                    continue
                blended = [rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2]
                           + rows[r][3] for r in range(3)]
                approx = apply_affine(e0, p) if e0 else list(p)
                if outer:
                    blended = apply_affine(outer, blended)
                    approx = apply_affine(outer, approx)
                n += 1
                hit['cam'] += 1 if in_clip(cam, p) else 0
                hit['e0'] += 1 if in_clip(cam, approx) else 0
                hit['blend'] += 1 if in_clip(cam, blended) else 0
                moved.append(max(abs(blended[k] - approx[k]) for k in range(3)))
                for k in range(3):
                    bmn[k] = min(bmn[k], blended[k])
                    bmx[k] = max(bmx[k], blended[k])
                    amn[k] = min(amn[k], approx[k])
                    amx[k] = max(amx[k], approx[k])
            if not n:
                continue
            draws += 1
            verts += n
            for k in tot:
                tot[k] += hit[k]
            moved.sort()
            disp.append(moved[len(moved) // 2])
            boxes.append((max(bmx[k] - bmn[k] for k in range(3)),
                          max(amx[k] - amn[k] for k in range(3))))
            if printed < args.top:
                printed += 1
                print('  %-22s %4d verts  cam %5.1f%%  entry0 %5.1f%%  blend %5.1f%%'
                      % (d['vs'], n, 100.0 * hit['cam'] / n, 100.0 * hit['e0'] / n,
                         100.0 * hit['blend'] / n))

    if not verts:
        print('no palette vertices readable', file=sys.stderr)
        return 1
    print('\n%d palette draws, %d vertices, over %d trace(s)' % (draws, verts, len(traces)))
    print('  vertices landing in the frustum the draw was issued into:')
    for k, label in (('cam', 'camera only (no object transform)'),
                     ('e0', 'palette ENTRY 0 (parts 67-68)'),
                     ('blend', 'per-vertex BLEND (item 3)')):
        print('    %-36s %8d  %5.2f%%' % (label, tot[k], 100.0 * tot[k] / verts))
    if oor:
        print('  %d vertices indexed past row %d of the window' % (oor, C.WINDOW_ROWS))
    print('  (this statistic SATURATES — see the docstring; the numbers below are the '
          'ones with dynamic range)')

    disp.sort()
    med = disp[len(disp) // 2]
    p90 = disp[int(len(disp) * 0.9)]
    far = sum(1 for x in disp if x > 1.0)
    print('\n  HOW FAR THE BLEND MOVES A VERTEX from the entry-0 approximation,')
    print('  per draw (median over its own vertices), in world units:')
    print('    median draw %8.2f      90th percentile %8.2f' % (med, p90))
    print('    %d of %d draws (%.1f%%) move by more than one world unit'
          % (far, len(disp), 100.0 * far / len(disp)))
    wider = sum(1 for b, a in boxes if b > a * 1.5)
    rb = sorted(b for b, _a in boxes)
    ra = sorted(a for _b, a in boxes)
    print('\n  HOW WIDE THE DRAW IS, blended against entry 0 (median extent):')
    print('    blended %8.2f   entry 0 %8.2f   %d of %d draws (%.1f%%) are >1.5x wider'
          % (rb[len(rb) // 2], ra[len(ra) // 2], wider, len(boxes),
             100.0 * wider / len(boxes)))
    # THE WIDTH IS THE DISCRIMINATOR, NOT THE DISPLACEMENT, and the first version of this
    # verdict read the wrong one. Entry 0 applies a RIGID transform, so it preserves the
    # mesh's shape and only its position can be wrong — and for a mesh whose vertices are
    # stored in BONE-LOCAL space (which is what a skinned mesh is) the position of the
    # collapsed cloud is roughly right while its size is a fifth of the truth. A median
    # displacement of 0.17 units next to a median extent going 1.51 -> 8.75 is not "the
    # blend barely moves this", it is "entry 0 was never producing this geometry at all".
    widened = wider > len(boxes) * 0.4
    print('\nVERDICT: %s' % (
        'ENTRY 0 IS NOT AN APPROXIMATION OF THIS GEOMETRY — IT IS A DIFFERENT MESH. The '
        'blend ASSEMBLES the draw: the median extent grows several-fold and a fifth of '
        'draws also move by metres. That is what a skinned mesh stored in bone-local '
        'space, or a batch of props each in its own frame, looks like when it is '
        'collapsed onto one matrix. Check the placement render for whether the assembled '
        'positions are the RIGHT ones.'
        if widened or far > len(disp) * 0.5 else
        'the blend neither moves nor widens this population — either these draws really '
        'do use one matrix here, or the descriptor is wrong.'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
