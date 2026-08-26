#!/usr/bin/env python3
"""Is the Xenos 2D tile swizzle separable into a 32x32 table plus a macro-tile base?

WHY THIS EXISTS. `Tiled2DOffset` in runtime/gpu/vk_renderer.cpp runs once per unit inside
the texture untile loop — 11.5 million times on a 60-second route — and it is about twenty
ALU operations of shifts and masks. Part 77's decomposition showed the untile is the largest
remaining column of the texture decode, so the obvious question is whether the address can
be table-driven instead of computed.

It can, but only if the function decomposes, and the decomposition is NOT obvious from the
source: the final expression bit-shuffles the SUM of the macro and micro terms
(`(offset & ~511) << 3` and friends), which in general does not distribute over an addition.
It happens to here, because the macro term is always a multiple of `128 << log2bpu` and the
micro term is always strictly below it, so the two never share a bit and never carry.

"Happens to" is exactly the kind of claim this project requires a census for rather than an
argument (and the reason is gotcha 5: a wrong texture address produces a plausible picture
of the wrong thing, not a failure). So this brute-forces the identity over every unit size
the renderer maps, six surface widths, 96 rows and every column:

    Tiled2DOffset(x, y, W, b) == micro(x & 31, y & 31, b) + macroBase(x >> 5, y >> 5, W, b)

where `micro` is Tiled2DOffset of the low five bits alone — independent of W, which is what
makes one table serve every surface — and `macroBase` is Tiled2DOffset of the tile origin.

It also prints the RUN LENGTHS of consecutive units within a row, because a run of units at
consecutive addresses can be copied and byte-swapped in one pass instead of one call each.

Exit 1 if the identity fails anywhere.
"""
import sys


def tiled2d(x, y, w, b):
    """Transcribed from Tiled2DOffset (which is itself XGAddress2DTiledOffset)."""
    macro = ((x >> 5) + (y >> 5) * (w >> 5)) << (b + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << b
    off = (macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + b))
           + ((y & 1) << 4))
    return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63)
             + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> b)


def main():
    bad = tot = 0
    for b in range(0, 5):                     # 1, 2, 4, 8, 16 bytes per unit
        for w in (32, 64, 128, 256, 512, 1024):
            for y in range(0, 96):            # three macro-tile rows
                for x in range(0, w):
                    tot += 1
                    lhs = tiled2d(x, y, w, b)
                    rhs = (tiled2d(x & 31, y & 31, w, b)
                           + tiled2d((x >> 5) << 5, (y >> 5) << 5, w, b)
                           - tiled2d(0, 0, w, b))
                    if lhs != rhs:
                        bad += 1
                        if bad < 5:
                            print(f'MISMATCH b={b} W={w} ({x},{y}): {lhs} != {rhs}')
    print(f'separability: {tot} combinations checked, {bad} mismatches')

    print('\ncontiguous-run lengths within a row (units), per log2(bytes per unit):')
    for b in range(0, 5):
        lens = set()
        for y in range(0, 32):
            row = [tiled2d(x, y, 1024, b) for x in range(32)]
            cur = 1
            for i in range(1, 32):
                if row[i] == row[i - 1] + 1:
                    cur += 1
                else:
                    lens.add(cur)
                    cur = 1
            lens.add(cur)
        runs = sorted(lens)
        print(f'  {1 << b:2d} B/unit: runs of {runs} units '
              f'= {[r * (1 << b) for r in runs]} bytes')
    return 1 if bad else 0


sys.exit(main())
