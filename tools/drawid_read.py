#!/usr/bin/env python3
"""WHICH DRAW PAINTED THAT PIXEL? Read a draw-ID map and answer it from the census.

WHY THIS EXISTS (part 39)
-------------------------
Every picture defect in this port has been attributed to a draw by INFERENCE — "the
shard tree is probably the shader binding a DXT5 and a DXN, because leaf cards usually
do". That inference was wrong twice in two sessions, and the second time it selected a
HAIR material and an entire investigation was built on top of it: a byte-identical
texture comparison, a matching blend split, matching vertex histograms, and a confident
conclusion about a material that was never on the tree (gotchas 291, 302).

The runtime could not answer the one question an operator's finger asks. Now it can:
`CZ_VK_DRAW_ID=1` plus F9 renders the frame after the captured one with every draw
painting its own index instead of its colour, and the scene resolve snapshot IS that
map. This turns the map plus the census into a lookup.

READING THE MAP
---------------
Each pixel holds `draw index + 1` as a little-endian 24-bit value over R,G,B (0 means no
draw touched it). The image is the SCENE surface, so it is usually smaller than the
presented picture — pass --picture-size if you want to give coordinates in the picture's
space and have them scaled.

USAGE
    drawid_read.py <drawid_snap.ppm> [--census capture_fNNNN.census]
                   [--at X,Y] [--rect X,Y,W,H] [--top N] [--picture-size WxH]

    --at        one pixel: print the draw that owns it, with its census line
    --rect      a region (e.g. the tree): rank the draws by how many pixels they own
    --top       how many to list for a region or for the whole frame (default 12)

With no --at or --rect it ranks the whole frame, which is a quick way to see what is
actually covering the screen.
"""
import argparse
import collections
import re
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6\n<w> <h>\n<max>\n — the header this project's dumps write, parsed strictly
    # rather than with a regex over the whole file, because a binary payload can contain
    # anything that looks like a header.
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    if parts[0] != b'P6':
        sys.exit('%s is not a binary PPM' % path)
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def census_lines(path):
    """draw index -> its census line, keyed exactly as the runtime numbers draws."""
    out = {}
    if not path:
        return out
    for line in open(path):
        m = re.match(r'draw (\d+) ', line)
        if m:
            out[int(m.group(1))] = line.rstrip()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--census')
    ap.add_argument('--at')
    ap.add_argument('--rect')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--picture-size')
    a = ap.parse_args()

    w, h, px = read_ppm(a.map)
    lines = census_lines(a.census)
    print('%s: %ux%u ID map%s' % (a.map, w, h,
                                  ', census %d draws' % len(lines) if lines else ''))

    sx = sy = 1.0
    if a.picture_size:
        pw, ph = (int(v) for v in a.picture_size.lower().split('x'))
        sx, sy = w / pw, h / ph
        print('  coordinates given in %ux%u picture space, scaled by %.3f x %.3f'
              % (pw, ph, sx, sy))

    def draw_at(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return None
        o = (y * w + x) * 3
        v = px[o] | (px[o + 1] << 8) | (px[o + 2] << 16)
        return None if v == 0 else v - 1

    def report(idx, extra=''):
        if idx is None:
            print('  no draw touched it (background)')
            return
        print('  draw %d%s' % (idx, extra))
        if idx in lines:
            print('    %s' % lines[idx])
        elif lines:
            print('    (not in the census — the ID frame is the one AFTER the captured '
                  'frame, so a draw list that changed between them will not line up)')

    if a.at:
        x, y = (int(v) for v in a.at.split(','))
        report(draw_at(int(x * sx), int(y * sy)))
        return 0

    if a.rect:
        x, y, rw, rh = (int(v) for v in a.rect.split(','))
        x, y = int(x * sx), int(y * sy)
        rw, rh = max(1, int(rw * sx)), max(1, int(rh * sy))
    else:
        x = y = 0
        rw, rh = w, h

    hist = collections.Counter()
    for yy in range(y, min(y + rh, h)):
        for xx in range(x, min(x + rw, w)):
            hist[draw_at(xx, yy)] += 1
    total = sum(hist.values())
    print('  region %d,%d %ux%u = %d pixels, %d distinct draws visible'
          % (x, y, rw, rh, total, len([k for k in hist if k is not None])))
    for idx, n in hist.most_common(a.top):
        if idx is None:
            print('  %6d px (%5.1f%%)  background' % (n, 100 * n / total))
            continue
        report(idx, '  — %d px (%.1f%% of the region)' % (n, 100 * n / total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
