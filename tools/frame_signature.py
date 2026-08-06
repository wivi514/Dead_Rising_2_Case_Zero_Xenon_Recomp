#!/usr/bin/env python3
"""Compare a rendered frame against a REFERENCE, and name the TRANSFORM if it is one.

WHY THIS EXISTS
---------------
Phase 5 rendered every frame VERTICALLY MIRRORED for its entire duration, and drew all
text as solid opaque blocks, and **not one number this project computes could see
either**. Both were found by an operator looking at the screen for a minute.

The flip is the sharper case. A vertical flip preserves coverage, mean luminance,
distinct-colour count and the whole histogram EXACTLY, so:

    tools/frame_compare.py       scores a flipped frame as identical  (aggregates)
    tools/ppm_stats.py           mean RGB, luma percentiles: unchanged (aggregates)
    tools/frame_matched_diff.py  compares us to US, so a flip in both arms cancels

Every one of those is an aggregate over pixel VALUES, and a transform rearranges pixel
POSITIONS. That is a structural blind spot, not a tuning problem, and no amount of
extra statistics closes it. Two things close it:

  1. compare against a REFERENCE that is known-correct — capture E's screenshots; and
  2. test the candidate TRANSFORMS explicitly, and say which one fits best.

(2) is the part that turns "these do not match" into "your frame is upside down".

WHAT IT COMPARES
----------------
A coarse spatial signature: the image is auto-cropped to its non-black bounding box,
resized to an NxN grid, and reduced to per-cell mean luminance normalised to [0,1].

Coarse and normalised on purpose. The reference is a windowed screen grab of an
emulator at a different size, with different post-processing and a different moment of
an animated scene, so absolute values and fine detail cannot match. The LAYOUT can —
where the bright regions are — and layout is exactly what a transform destroys.

USAGE
    frame_signature.py <image>                       # print one signature
    frame_signature.py --ref E2.png <ours.ppm> ...   # compare, and name the transform

    --grid N        grid size (default 32; 16 was too coarse to see a line of text)
    --show          print the ASCII grids side by side

READING IT
    best fit "identity"          -> layout agrees; any remaining error is content
    best fit anything else       -> THE FRAME IS TRANSFORMED, and it says which
    candidates close together    -> the image is too symmetric (or too empty) for this
                                    to discriminate; it says so rather than picking one
    best correlation below 0.70  -> not that reference at all (wrong moment or scene)

Exit status is 1 when a non-identity transform is named, so it can gate a script.
"""
import argparse
import sys

import numpy as np
from PIL import Image

# The transforms a renderer plausibly gets wrong. Rotations by 90 degrees are excluded
# deliberately: they change the aspect ratio, so they are not a mistake a viewport or a
# UV convention produces, and including them would only add noise to the ranking.
# How well the best orientation must correlate before the tool will claim anything, and
# by how much it must beat the runner-up. See the two-gate note at the use site: the
# negative control (title screen vs the ESRB card) is what these keep out.
MIN_CORRELATION = 0.70
MIN_GAP = 0.05

TRANSFORMS = {
    "identity": lambda a: a,
    "flip-vertical": lambda a: a[::-1, :],
    "flip-horizontal": lambda a: a[:, ::-1],
    "rotate-180": lambda a: a[::-1, ::-1],
}


def load_luma(path):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, dtype=np.float32)
    # Rec.601 luma; the exact weights do not matter at this coarseness, but using the
    # same ones as the runtime's own frame stats keeps the two comparable.
    return (a[:, :, 0] * 0.299 + a[:, :, 1] * 0.587 + a[:, :, 2] * 0.114)


def game_area(luma, aspect=16.0 / 9.0):
    """The largest CENTRED 16:9 region — the game area inside a windowed screen grab.

    Cropping to the non-black bounding box was the first attempt and it is wrong for
    this content. Capture E's shots are windowed grabs of assorted sizes (E1 1320x985,
    E2 1378x1125, E3 1401x1006, E4 1384x1189) and most of them are MOSTLY BLACK, so a
    content bbox finds the logo rather than the screen — and then a 1280x720 frame and a
    1264x1125 "logo" get squashed to the same grid by different amounts, which moves
    exactly the thing the comparison is trying to measure.
    
    The geometry is checkable on the one shot bright enough to reveal it: E4's content
    bbox is 1384x785, aspect 1.763, i.e. full width and letterboxed vertically about the
    centre. So the game area is the largest centred 16:9 rect, and that model reproduces
    E4's measured crop to within seven pixels.
    """
    h, w = luma.shape
    if w / h > aspect:          # pillarboxed: height is the limit
        cw, ch = int(round(h * aspect)), h
    else:                        # letterboxed: width is the limit
        cw, ch = w, int(round(w / aspect))
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    return luma[y0:y0 + ch, x0:x0 + cw]


def signature(path, grid):
    luma = game_area(load_luma(path))
    # A 16:9 grid, not a square one. Squashing a 16:9 frame into NxN distorts it
    # anisotropically, which is harmless when both sides are distorted identically and
    # is not when the reference has a different source aspect.
    small = np.asarray(
        Image.fromarray(luma).resize((grid, max(1, round(grid * 9 / 16))), Image.BOX),
        dtype=np.float32)
    # Z-SCORE, not peak-normalise. Both images are ~90% black, and under a peak
    # normalisation the black cells agree with each other in every candidate transform —
    # so the matching background dominates the score and the real difference (a thin
    # line of text moving from the bottom to the top) is averaged into nothing. The
    # first version of this tool ranked both test cases CORRECTLY and still declared
    # them inconclusive, at a margin of 3%, for exactly that reason.
    #
    # Centring and scaling makes the signature about CONTRAST STRUCTURE — where this
    # frame is bright relative to its own average — which is the property a transform
    # moves and exposure does not.
    mean, std = float(small.mean()), float(small.std())
    return (small - mean) / std if std > 0 else small - mean


def ascii_grid(sig):
    ramp = " .:-=+*#%@"
    return "\n".join(
        "".join(ramp[min(int(v * (len(ramp) - 1) + 0.5), len(ramp) - 1)] for v in row)
        for row in sig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("images", nargs="+")
    ap.add_argument("--ref", help="reference image (e.g. a capture E screenshot)")
    ap.add_argument("--grid", type=int, default=32)
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    if not args.ref:
        for p in args.images:
            print(f"=== {p} ===")
            print(ascii_grid(signature(p, args.grid)))
        return 0

    ref = signature(args.ref, args.grid)
    if args.show:
        print(f"=== reference: {args.ref} ===")
        print(ascii_grid(ref))

    worst = 0
    for p in args.images:
        sig = signature(p, args.grid)
        # Correlation distance, not mean absolute difference. With z-scored
        # signatures, `1 - mean(a*b)` rewards cells that deviate from their own mean in
        # the SAME direction, so a bright region in the right place scores strongly and
        # two matching black regions contribute almost nothing. Mean-abs gives the
        # background an equal vote, which is what flattened the first version.
        # CORRELATION, reported directly, because a RATIO test on `1 - correlation` is
        # meaningless: those values cluster around 1.0, so correlations of +0.07 and
        # -0.02 — opposite signs, an unambiguous result — come out as 0.93 and 1.02 and
        # fail a 10% ratio margin. The first version of this tool did exactly that and
        # called both of its test cases inconclusive while ranking both correctly.
        scores = {name: float((fn(sig) * ref).mean())
                  for name, fn in TRANSFORMS.items()}
        order = sorted(scores.items(), key=lambda kv: -kv[1])
        best, bestv = order[0]
        second, secondv = order[1]
        spread = order[0][1] - order[-1][1]

        print(f"\n=== {p.split('/')[-1]}  vs  {args.ref.split('/')[-1]} ===")
        for name, v in order:
            print(f"    {name:<16} corr {v:+.4f}"
                  f"{'   <- best' if name == best else ''}")

        # TWO gates, and the second one exists because the first alone produced a
        # confident wrong answer on the negative control.
        #
        # Judging the margin as a FRACTION OF THE SPREAD is what failed: comparing the
        # title screen against the ESRB card (a completely different image) gives four
        # correlations of 0.538..0.576, and a 0.020 gap in a 0.038 spread is "52% of the
        # spread" — a confident "this frame is flipped" about two images that have
        # nothing to do with each other. A small spread is not evidence of a clear
        # winner; it is evidence that nothing discriminates.
        #
        # So: the best orientation must correlate well enough that this is plausibly the
        # same scene AT ALL, and it must beat the runner-up by an ABSOLUTE margin.
        # The thresholds are set with a wide margin around the measured cases rather
        # than fitted to them — the true positives sit at correlation 0.95 with gaps of
        # 0.12-0.17, and the negative control at 0.58 with a gap of 0.02.
        gap = bestv - secondv
        print(f"    (best correlates {bestv:+.3f}, beating the runner-up by {gap:.3f})")
        if bestv < MIN_CORRELATION:
            print(f"  NO MATCH: the best orientation correlates only {bestv:+.3f} "
                  f"(need {MIN_CORRELATION:+.2f}). This frame is not that reference — "
                  f"wrong moment, wrong scene, or wrong in a way this cannot describe.")
        elif gap < MIN_GAP:
            print(f"  INCONCLUSIVE: the orientations are within {gap:.3f} of each other "
                  f"(need {MIN_GAP:.2f}) — too symmetric or too empty to discriminate.")
        elif best == "identity":
            print("  LAYOUT AGREES with the reference (identity fits best). Any "
                  "remaining error is content, not orientation.")
        else:
            print(f"  *** THE FRAME IS TRANSFORMED: '{best}' fits best, "
                  f"beating the next candidate by {gap:.3f}. ***")
            worst = 1
        if args.show:
            print(ascii_grid(sig))
    return worst


if __name__ == "__main__":
    sys.exit(main())
