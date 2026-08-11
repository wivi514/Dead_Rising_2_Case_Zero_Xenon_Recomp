#!/usr/bin/env python3
"""Turn an `XE_SHADER_TAG` painted frame back into a list of SHADER NAMES.

WHY THIS EXISTS
---------------
Part 27 established that Case Zero's white patches are exactly rgb(180,180,180) in the
scene buffer and that the material pixel shaders write that value themselves
(`XE_VALUE_PAINT`, docs/xenonrecomp-upstream-bugs.md). What that arm could not say is
WHICH shaders — it painted one colour, so a green patch on a slot machine and a green
patch on the ground were indistinguishable, and attributing them meant guessing from the
picture. This project has been wrong doing exactly that before (open item 00f's "props",
which the cube-poison overlay corrected to "characters").

So the paint now carries the low 16 bits of the shader's own hash, written as
`(hi, 255, lo)`. This selects the marker, groups the payload, and maps each tag back to
the name by hashing the cache's filenames the same way.

WHAT IT CHECKS RATHER THAN ASSUMES
-----------------------------------
* **Green 255 occurs naturally.** A bright green pixel is not a tag. Natural ones scatter
  across many (R,B) pairs with tiny counts; tags cluster hard on a pair that resolves to a
  real shader name. Both are reported, and pairs that resolve to NOTHING are listed
  separately as the noise floor rather than dropped — a decoder that silently discards
  what it cannot explain is how a wrong reading survives.
* **16 bits collide.** With 411 shaders the birthday expectation is ~1.3 collisions, so a
  tag that maps to more than one name is reported as AMBIGUOUS with every candidate
  named, not resolved by picking the first.

USAGE
    shader_tag_decode.py <scene.ppm> [more.ppm ...] [--spv assets/shader_spv] [--min N]
"""
import argparse
import collections
import sys
from pathlib import Path


def read_ppm(path):
    """P6 binary PPM -> (w, h, bytes). Written by hand: the renderer writes plain P6 and
    pulling in an imaging library for a three-line header is a dependency for nothing."""
    with open(path, 'rb') as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--min', type=int, default=8,
                    help='ignore (R,B) pairs with fewer pixels than this across all frames')
    args = ap.parse_args()

    # tag -> [names]. Built from the cache's own filenames, which are the hashes.
    tags = collections.defaultdict(list)
    for p in Path(args.spv).glob('ps_*.spv'):
        h = p.stem.split('_')[1]
        tags[int(h, 16) & 0xFFFF].append(p.stem)

    hits = collections.Counter()
    total = 0
    for f in args.frames:
        w, h, px = read_ppm(f)
        total += w * h
        for o in range(0, len(px) - 2, 3):
            if px[o + 1] == 255:
                hits[(px[o], px[o + 2])] += 1

    named, unknown = [], []
    for (r, b), n in hits.items():
        if n < args.min:
            continue
        who = tags.get((r << 8) | b)
        (named if who else unknown).append(((r << 8) | b, who, n))

    print('%d frames, %d pixels, %d distinct (R,B) pairs with green==255'
          % (len(args.frames), total, len(hits)))
    print('\nSHADERS THAT EMITTED THE PAINTED VALUE:')
    for tag, who, n in sorted(named, key=lambda x: -x[2]):
        flag = '   AMBIGUOUS — 16-bit tag collision' if len(who) > 1 else ''
        print('  %-9d px  tag %04X  %s%s' % (n, tag, ', '.join(who), flag))
    if not named:
        print('  none — no painted pixel resolved to a shader in the cache')
    # The noise floor, printed rather than dropped.
    if unknown:
        print('\n(%d (R,B) pairs above the threshold resolve to no shader — natural '
              'green-255 pixels, %d px total)'
              % (len(unknown), sum(n for _, _, n in unknown)))
        for tag, _, n in sorted(unknown, key=lambda x: -x[2])[:6]:
            print('   %-7d px  R=%3d B=%3d' % (n, tag >> 8, tag & 255))
    return 0


if __name__ == '__main__':
    sys.exit(main())
