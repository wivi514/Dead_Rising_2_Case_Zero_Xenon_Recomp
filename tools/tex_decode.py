#!/usr/bin/env python3
"""Decode a raw Xenos DXT1/DXT5 texture dump to a PPM so a human can LOOK at it.

WHY THIS EXISTS (part 36): part 35's junk-scorer flagged the odd-extent DXT5
"impostor sheets" as banded garbage, and the whole striped-material item (0s) was
framed around a writer composing junk into them. Decoding and LOOKING — this tool —
showed they are coherent billboard alpha-cutouts (white colour channels + the shape
in ALPHA), byte-identical to the bytes hardware sampled in the R3 tanker trace.
A greyscale-with-extremes SCORE is not a picture; this is the two-minute check that
should precede any "memory holds garbage" claim (gotcha 287).

The tiled layout is Tiled2DOffset transcribed from runtime/gpu/vk_renderer.cpp
(itself the XDK's XGAddress2DTiledOffset); pitch defaults to the fetch-constant rule
(width in blocks rounded up to 32) which is what a dump with no sidecar has to assume.
Xenos DXT payloads are big-endian 16-bit words, so --swap16 is normally wanted for
dumps of raw guest memory; the live_texdump .bin files are raw guest memory.

Usage:
  tex_decode.py <in.bin> <w> <h> <out.ppm> [--fmt 18|20] [--tiled] [--swap16]
                [--pitchblk N] [--alpha]
--fmt 18 = DXT1, 20 = DXT5 (default). --alpha writes the alpha plane as greyscale
(DXT5 only) — for a cutout sheet that is where the content is. --pitchblk is the
fetch constant's pitch field (blocks of 32 texels) if you have it, e.g. from a
CZ_CAPTURE census line's pitchBlk.
A dump shorter than the tiled footprint (w*h*bpp is SHORTER than pitch*rows for
odd extents) reports its missing blocks; they render magenta, not silently black.
"""
import sys, struct, collections


def tiled2d(x, y, wu, l2b):
    macro = ((x >> 5) + (y >> 5) * (wu >> 5)) << (l2b + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << l2b
    off = (macro + ((micro & ~15) << 1) + (micro & 15) +
           ((y & 8) << (3 + l2b)) + ((y & 1) << 4))
    return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) +
             ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> l2b)


def c565(c):
    return ((c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3)


def dxt1_block(b):
    c0, c1 = struct.unpack('<HH', b[0:4])
    bits = int.from_bytes(b[4:8], 'little')
    p = [c565(c0), c565(c1)]
    if c0 > c1:
        p.append(tuple((2 * p[0][i] + p[1][i]) // 3 for i in range(3)))
        p.append(tuple((p[0][i] + 2 * p[1][i]) // 3 for i in range(3)))
    else:
        p.append(tuple((p[0][i] + p[1][i]) // 2 for i in range(3)))
        p.append((0, 0, 0))
    return [(p[(bits >> (2 * t)) & 3], 255) for t in range(16)]


def dxt5_block(b):
    a0, a1 = b[0], b[1]
    abits = int.from_bytes(b[2:8], 'little')
    c0, c1 = struct.unpack('<HH', b[8:12])
    cbits = int.from_bytes(b[12:16], 'little')
    p = [c565(c0), c565(c1)]
    p.append(tuple((2 * p[0][i] + p[1][i]) // 3 for i in range(3)))
    p.append(tuple((p[0][i] + 2 * p[1][i]) // 3 for i in range(3)))
    al = [a0, a1]
    if a0 > a1:
        al += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        al += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [(p[(cbits >> (2 * t)) & 3], al[(abits >> (3 * t)) & 7])
            for t in range(16)]


def main():
    inp, w, h, outp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    fmt = int(sys.argv[sys.argv.index('--fmt') + 1]) if '--fmt' in sys.argv else 20
    tiled = '--tiled' in sys.argv
    swap = '--swap16' in sys.argv
    alpha = '--alpha' in sys.argv
    pitchblk = (int(sys.argv[sys.argv.index('--pitchblk') + 1])
                if '--pitchblk' in sys.argv else 0)
    block_bytes, decode = (8, dxt1_block) if fmt == 18 else (16, dxt5_block)
    l2b = 3 if fmt == 18 else 4

    data = open(inp, 'rb').read()
    if swap:
        d = bytearray(data)
        d[0::2], d[1::2] = d[1::2], d[0::2]
        data = bytes(d)

    bw, bh = (w + 3) // 4, (h + 3) // 4
    # pitchblk is the fetch constant's field: blocks of 32 TEXELS = 8 DXT blocks
    pitch = pitchblk * 8 if pitchblk else (((bw + 31) & ~31) if tiled else bw)
    img = [[(255, 0, 255)] * w for _ in range(h)]
    missing = 0
    for by in range(bh):
        for bx in range(bw):
            off = ((tiled2d(bx, by, pitch, l2b) * block_bytes) if tiled
                   else (by * pitch + bx) * block_bytes)
            if off + block_bytes > len(data):
                missing += 1
                continue
            for t, ((r, g, b), a) in enumerate(decode(data[off:off + block_bytes])):
                x, y = bx * 4 + t % 4, by * 4 + t // 4
                if x < w and y < h:
                    img[y][x] = (a, a, a) if alpha else (r, g, b)
    with open(outp, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        for row in img:
            f.write(bytes(v for px in row for v in px))
    lum = [(r * 3 + g * 6 + b) // 10 for row in img for (r, g, b) in row]
    hist = collections.Counter(lum)
    n = len(lum)
    print('%s: mean=%.1f distinct=%d black%%=%.1f white%%=%.1f blocks_missing=%d'
          % (outp, sum(lum) / n, len(hist),
             100 * sum(v for k, v in hist.items() if k < 8) / n,
             100 * sum(v for k, v in hist.items() if k > 247) / n, missing))


if __name__ == '__main__':
    main()
