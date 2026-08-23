#!/usr/bin/env python3
"""IS THE PLACED RT GEOMETRY WHERE THE PICTURE SAYS IT IS? An offline silhouette check.

WHY THIS EXISTS
---------------
Part 67 gave every TLAS instance the draw's own object->world matrix, and part 67's own
operator session then showed the shadows still landing in the wrong places. The mode-20
factor dump is the reason why, and it is unambiguous: rendered from the player's camera,
the structure the rays see is **a flat plain with distant buildings** — no vans, no
wrecked cars, no fence, no Chuck. The primary ray sails past everything in the foreground
and lands on the ground far behind it, so the factor computed there is painted onto the
near object's pixels. That is exactly "misaligned" and "visible through walls".

Two explanations survive that dump and they need different fixes:

    (a) the foreground props are NOT IN the structure at all — some filter drops them
        (`dyn` is 41% of everything the collector sees);
    (b) the foreground props ARE in it and are PLACED WRONG — the palette approximation
        is 57% of all placements, and a skinned or multi-matrix mesh collapsed onto
        palette entry 0 lands somewhere else entirely.

The round-2/3/4 captures make this answerable with no operator and no runtime: each
`.xtr` is frame-locked to a PNG of what hardware drew that frame. So project the accepted
draws — placed by the same table the runtime uses — through the same camera matrix
hardware used, splat the vertices, and put the result beside the PNG. If the vans are in
the splat where they are in the photograph, the placement is right and the answer is (a).
If they are absent, (a) too. If they are present but somewhere else, it is (b).

WHY VERTEX SPLATS AND NOT A RASTERISER
---------------------------------------
The question is "is this mesh in the right PLACE", which a point cloud answers exactly as
well as filled triangles and in a tenth of the code. Filling would only matter for an
occlusion-accurate comparison, which is not what is being asked — and a rasteriser nobody
has validated would be a second untested thing in a comparison meant to test one.

The output is a depth splat: nearer vertices are brighter, so the image reads like a
photograph rather than like a fog of dots.

USAGE
    tools/rt_placement_render.py                                  # every world trace
    tools/rt_placement_render.py --trace <one.xtr> -o out.png
    tools/rt_placement_render.py --trace <one.xtr> --side-by-side  # with the hardware PNG
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    'rt_tlas_census', Path(__file__).resolve().parent / 'rt_tlas_census.py')
census = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(census)

ROOT = census.ROOT
W, H = 1280, 720          # the render target these captures were taken at


def parse_blend(entry):
    """The palette stage's vertex-buffer layout, or None. See rt_world_xform_census.py."""
    b = (entry or {}).get('blend')
    if not b:
        return None
    f = b.split(':')
    if len(f) != 5:
        return None
    return {'slot': int(f[0]), 'stride': int(f[1]), 'w': int(f[2]), 'i': int(f[3]),
            'bytes': [int(c) for c in f[4]]}


def blend_stage(entry):
    """(base, outer stages) for a palette shader, or (None, [])."""
    base, outer = None, []
    for tok in (entry or {}).get('stages', '').split(','):
        if '@' not in tok:
            continue
        kind, _, b = tok.partition('@')
        if kind == 'palette' and base is None:
            base = int(b)
        else:
            outer.append(int(b))
    return base, outer


def splat(draws, mem, xform, w=W, h=H, max_verts=3000, raw=None, blend=False):
    """Project every placed accepted draw's vertices and keep the nearest per pixel.

    `blend=True` applies the PER-VERTEX palette blend (the Remix plan's item 3) to
    palette-shaped draws instead of the entry-0 approximation, so the two renders can be
    put beside hardware's own frame and told apart by eye. The frustum test cannot tell
    them apart — both keep 96.5% of vertices on screen, because a batch of props is small
    next to the frustum and collapsing it onto one of its members still leaves it visible.
    That is a saturated statistic, not agreement.
    """
    depth = [0.0] * (w * h)      # 0 = empty; larger = nearer
    plotted = 0
    partial = 0
    done = 0
    for d in draws:
        if d['bucket'] != 'ok' or 'stream' not in d or not d.get('win'):
            continue
        cam = d['win'][:16]
        if any(v is None for v in cam):
            continue
        ent = xform.get(d['vs'] or '')
        if ent is None:
            continue
        m = census.world_xform(d['win'], ent)
        if m is None:
            continue
        # The per-vertex path, for palette draws only and only when asked.
        bl = pbase = None
        outer = None
        if blend and raw is not None:
            e = raw.get(d['vs'] or '')
            bl = parse_blend(e)
            pbase, oub = blend_stage(e)
            if bl is not None and pbase is not None:
                for b in oub:
                    a = census.affine(d['win'], b)
                    if a is None:
                        bl = None
                        break
                    outer = a if outer is None else census.compose(a, outer)
            else:
                bl = None
        done += 1
        if done % 500 == 0:
            print('    ... %d draws, %d vertices' % (done, plotted), flush=True)
        addr, size, endian, stride, offdw = d['stream']
        verts = size // stride
        step = max(1, verts // max_verts)
        # ONE read for the whole stream where the trace carries it contiguously. The
        # per-vertex path below is a linear scan over the trace's memory chunks, and
        # doing it a million times does not finish — the same cost that had to come out
        # of rt_tlas_census.py.
        span = ((verts - 1) * stride + offdw + 3) * 4 if verts else 0
        blob = mem.read(addr, span) if span else None
        if blob is None:
            # The trace carries this stream in pieces. `Memory.read` is a linear scan
            # over every chunk, so the per-vertex path below is capped HARD and the
            # shortfall is counted — an uncapped fallback is what turned a three-minute
            # job into a twenty-one-minute one, and a silently thinned splat would look
            # exactly like a mesh that is missing.
            step = max(step, max(1, verts // 96))
            partial += 1
        for i in range(0, verts, step):
            dw = i * stride + offdw
            if dw + 3 > size:
                break
            if blob is not None:
                chunk = blob[dw * 4:dw * 4 + 12]
            else:
                chunk = mem.read(addr + dw * 4, 12)
            if chunk is None or len(chunk) < 12:
                continue
            p = struct.unpack('>3f', chunk)
            # object -> world (the table's stages), then world -> clip (the camera the
            # draw was actually issued with).
            if bl is not None:
                base_at = i * stride
                wq = blob if blob is not None else None
                if wq is not None:
                    wb = wq[(base_at + bl['w']) * 4:(base_at + bl['w']) * 4 + 4]
                    ib = wq[(base_at + bl['i']) * 4:(base_at + bl['i']) * 4 + 4]
                else:
                    wb = mem.read(addr + (base_at + bl['w']) * 4, 4)
                    ib = mem.read(addr + (base_at + bl['i']) * 4, 4)
                if not wb or not ib or len(wb) < 4 or len(ib) < 4:
                    continue
                wd = struct.unpack('>I', wb)[0]
                idw = struct.unpack('>I', ib)[0]
                rows = [[0.0] * 4 for _ in range(3)]
                bad = False
                for byte in bl['bytes']:
                    wt = ((wd >> (byte * 8)) & 0xFF) / 255.0
                    if wt == 0.0:
                        continue
                    a0 = (idw >> (byte * 8)) & 0xFF
                    for r in range(3):
                        for c in range(4):
                            q = d['win'][(pbase + a0 + r) * 4 + c] \
                                if (pbase + a0 + r) * 4 + c < len(d['win']) else None
                            if q is None:
                                bad = True
                                break
                            rows[r][c] += wt * q
                        if bad:
                            break
                    if bad:
                        break
                if bad:
                    continue
                wv = [rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2]
                      + rows[r][3] for r in range(3)]
                if outer is not None:
                    wv = [outer[r * 4 + 0] * wv[0] + outer[r * 4 + 1] * wv[1]
                          + outer[r * 4 + 2] * wv[2] + outer[r * 4 + 3]
                          for r in range(3)]
                wp = wv + [1.0]
            else:
                wp = [m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2]
                      + m[r * 4 + 3] for r in range(3)] + [1.0]
            cl = [sum(cam[r * 4 + k] * wp[k] for k in range(4)) for r in range(4)]
            x, y, z, ww = cl
            if ww <= 1e-4 or z < 0 or z > ww:
                continue
            sx = int((x / ww * 0.5 + 0.5) * w)
            sy = int((0.5 - y / ww * 0.5) * h)
            if sx < 0 or sy < 0 or sx >= w or sy >= h:
                continue
            near = 1.0 / ww
            at = sy * w + sx
            if near > depth[at]:
                depth[at] = near
            plotted += 1
    return depth, plotted, partial


def write_pgm(depth, path, w=W, h=H):
    hi = max(depth) or 1.0
    # A log ramp: linear depth puts the whole town into two grey levels.
    import math
    px = bytearray(w * h)
    for i, v in enumerate(depth):
        if v <= 0.0:
            continue
        px[i] = 40 + int(215.0 * min(1.0, math.log1p(v / hi * 50.0) / math.log(51.0)))
    with open(path, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (w, h))
        f.write(bytes(px))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('-o', '--out')
    ap.add_argument('--side-by-side', action='store_true',
                    help='stack the hardware PNG above the splat (needs ImageMagick)')
    ap.add_argument('--xform', default=str(census.WORLD_XFORM))
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--blend', action='store_true',
                    help='apply the PER-VERTEX palette blend (the Remix plan item 3) '
                         'instead of the entry-0 approximation')
    args = ap.parse_args()

    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in census.DEFAULT_TRACE_DIRS
                     for p in (ROOT / d).rglob('*.xtr')))
    xform = census.load_world_xform(args.xform)
    raw = {}
    if args.blend:
        import json
        try:
            raw = json.load(open(args.xform))['shaders']
        except Exception as e:
            print('cannot read %s: %s' % (args.xform, e), file=sys.stderr)
            return 2
    sidecars = census.load_sidecars(args.spv)
    if not xform or not sidecars:
        print('need both the shader cache and config/rt_world_xform.json',
              file=sys.stderr)
        return 2

    for tp in traces:
        draws, mem = census.walk_and_classify(tp, sidecars, with_mem=True)
        depth, plotted, partial = splat(draws, mem, xform, raw=raw, blend=args.blend)
        covered = sum(1 for v in depth if v > 0.0)
        out = (Path(args.out) if args.out else
               tp.with_suffix('.blended.pgm' if args.blend else '.placed.pgm'))
        write_pgm(depth, out)
        print('%-28s %d vertices plotted, %d of %d pixels covered (%.1f%%), '
              '%d streams the trace carries in pieces (sampled) -> %s'
              % (tp.stem, plotted, covered, W * H, 100.0 * covered / (W * H), partial,
                 out))
        if args.side_by_side:
            png = tp.with_suffix('.png')
            pair = out.with_suffix('.pair.png')
            if png.exists():
                subprocess.run(['magick', str(png), str(out), '-append', str(pair)])
                print('    %s' % pair)
            else:
                print('    no hardware PNG beside %s' % tp.name)
    return 0


if __name__ == '__main__':
    sys.exit(main())
