#!/usr/bin/env python3
"""Redo a crowd zombie's skinning offline from a CZ_CAPTURE_DUMP_VS draw dump.

WHY THIS EXISTS. The green "Las Vegas 46" highway sign lost straight-edged pieces while
the camera moved, and the draw-ID pass showed what was painting over it: one crowd
zombie draw (vs_b677dc3457f5b41a, a 392-index far-LOD mesh) covering an 830-pixel
horizon band. Its census line looked normal. Only the vertices can say which one
flew, and through which bone or weight, so this replays the translated shader's own
arithmetic on the inputs the draw actually had.

THE SHADER, as XenosRecomp translated it (see the .hlsl kept by CZ_KEEP_SYNTH):
    weights  = TEXCOORD2 (8_8_8_8 UNORM).yzw      bone idx = TEXCOORD3 (8_8_8_8 int).ywz
    a0 = round(idx);  rows c[8+a0], c[9+a0], c[10+a0], weighted and summed
    world = c4..c6 . (skinned, 1);   clip = c0..c3 . (world, 1)
so a bone index is pre-multiplied by 3, and the zombie's WORLD position lives in the
bone rows. A vertex whose weights do not sum to 1 is pulled toward the world origin.

The byte order: a stream dword is big-endian in guest memory, and an 8_8_8_8 element's
x is bits 0..7 of the swapped dword, i.e. the LAST byte in memory.

Usage:  tools/vsdump_skin.py <dir-with-vsdump_*.bin> [--top N] [--draw D]
"""
import argparse
import glob
import math
import os
import struct


def load(path):
    b = open(path, 'rb').read()
    hdr = struct.unpack('<16I', b[:64])
    if hdr[0] != 0x504D5544:
        raise SystemExit('%s: not a vsdump' % path)
    (_, _, draw, prim, icount, indexed, i32, iend, va, sizedw, vend, stride, ib) = hdr[:13]
    off = 64
    consts = struct.unpack('<1024f', b[off:off + 4096])
    off += 4096
    vbytes = b[off:off + sizedw * 4]
    off += sizedw * 4
    ibytes = b[off:off + ib]
    return dict(draw=draw, prim=prim, icount=icount, indexed=indexed, i32=i32, iend=iend,
                va=va, sizedw=sizedw, vend=vend, stride=stride, consts=consts,
                vbytes=vbytes, ibytes=ibytes)


def vc(c, k):
    if k < 0 or k > 255:
        return (0.0, 0.0, 0.0, 0.0)
    return c[k * 4:k * 4 + 4]


def dot4(r, v):
    return r[0] * v[0] + r[1] * v[1] + r[2] * v[2] + r[3] * v[3]


def indices(d):
    if not d['indexed']:
        return list(range(d['icount']))
    n = d['icount']
    ib = d['ibytes']
    if d['i32']:
        # Try both byte orders; the dump records the endian code, keep it simple.
        return list(struct.unpack('>%dI' % n, ib[:n * 4]))
    return list(struct.unpack('>%dH' % n, ib[:n * 2]))


def skin(d, vi):
    s = d['stride'] * 4
    o = vi * s
    raw = d['vbytes'][o:o + s]
    if len(raw) < s:
        return None
    px, py, pz = struct.unpack('>3f', raw[0:12])
    wb = raw[24:28]      # dword 6, memory order b0 b1 b2 b3
    ibb = raw[28:32]     # dword 7
    # 8_8_8_8 after the 8-in-32 swap: x = b3, y = b2, z = b1, w = b0
    w = (wb[3] / 255.0, wb[2] / 255.0, wb[1] / 255.0, wb[0] / 255.0)
    ix = (ibb[3], ibb[2], ibb[1], ibb[0])
    c = d['consts']
    # (weight component, index component) pairs exactly as the shader pairs them
    pairs = [(w[3], ix[3]), (w[2], ix[2]), (w[1], ix[1])]
    p = (px, py, pz, 1.0)
    sk = [0.0, 0.0, 0.0]
    for wt, idx in pairs:
        a0 = max(-256, min(255, int(math.floor(idx + 0.5))))
        for r in range(3):
            row = vc(c, 8 + r + a0)
            sk[r] += wt * dot4(row, p)
    world = [dot4(vc(c, 4 + r), (sk[0], sk[1], sk[2], 1.0)) for r in range(3)]
    clip = [dot4(vc(c, r), (world[0], world[1], world[2], 1.0)) for r in range(4)]
    wsum = pairs[0][0] + pairs[1][0] + pairs[2][0]
    return dict(vi=vi, pos=(px, py, pz), w=w, ix=ix, wsum=wsum, world=world, clip=clip)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--draw', type=int, default=None)
    a = ap.parse_args()
    rows = []
    for path in sorted(glob.glob(os.path.join(a.dir, 'vsdump_*.bin'))):
        d = load(path)
        if a.draw is not None and d['draw'] != a.draw:
            continue
        idx = indices(d)
        used = sorted(set(idx))
        vs = [skin(d, i) for i in used]
        vs = [v for v in vs if v]
        if not vs:
            continue
        ndc = [(v['clip'][0] / v['clip'][3], v['clip'][1] / v['clip'][3], v['clip'][2] / v['clip'][3])
               for v in vs if v['clip'][3] > 1e-6]
        behind = sum(1 for v in vs if v['clip'][3] <= 1e-6)
        xs = [n[0] for n in ndc] or [0]
        ys = [n[1] for n in ndc] or [0]
        wx = [v['world'][0] for v in vs]
        wz = [v['world'][2] for v in vs]
        badw = [v for v in vs if abs(v['wsum'] - 1.0) > 0.02]
        rows.append((max(xs) - min(xs), d, vs, xs, ys, behind, wx, wz, badw, os.path.basename(path)))
    rows.sort(key=lambda r: -r[0])
    print('%d draws dumped; widest NDC x-span first' % len(rows))
    for span, d, vs, xs, ys, behind, wx, wz, badw, name in rows[:a.top]:
        print('%s draw %d verts %d (used %d) ndc x %.3f..%.3f y %.3f..%.3f behind %d '
              'world x %.1f..%.1f z %.1f..%.1f  weight-sum!=1: %d  c8=%s'
              % (name, d['draw'], d['icount'], len(vs), min(xs), max(xs), min(ys), max(ys),
                 behind, min(wx), max(wx), min(wz), max(wz), len(badw),
                 '/'.join('%.3g' % x for x in vc(d['consts'], 8))))
        if a.draw is not None or span > 1.0:
            # the outliers: furthest from the median world position
            mx = sorted(wx)[len(wx) // 2]
            mz = sorted(wz)[len(wz) // 2]
            far = sorted(vs, key=lambda v: -abs(v['world'][0] - mx) - abs(v['world'][2] - mz))
            for v in far[:6]:
                print('    v%d pos %s w %s idx %s sum %.3f world %s clip %s'
                      % (v['vi'], tuple(round(x, 3) for x in v['pos']),
                         tuple(round(x, 3) for x in v['w']), v['ix'], v['wsum'],
                         tuple(round(x, 1) for x in v['world']),
                         tuple(round(x, 2) for x in v['clip'])))


if __name__ == '__main__':
    main()
