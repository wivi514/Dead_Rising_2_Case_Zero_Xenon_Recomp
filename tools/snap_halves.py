#!/usr/bin/env python3
"""Left/right asymmetry of every resolve snapshot of one captured frame.

WHY THIS EXISTS. Two operator reports of 2026-09-09 (open-items 0ab, 0ae) show a seam
at the screen's exact horizontal centre — near actors black from the centre rightward,
a neon's glow landing on the opposite side — and this title renders the scene in two
left/right tiles. The hypothesis is one screen-space pass sampling with the other
tile's window offset. An F9 capture already writes EVERY resolve snapshot of its frame,
so the pass can be named from disk: the snapshot whose right half does not belong with
its left half. For each snapshot this prints the mean luma of the two halves, their
ratio, and the correlation of the right half against the left half MIRRORED (a screen
centred subject is near-symmetric; a half copied from the other side correlates with
the UNMIRRORED left, which is the third column). Sorted by the ratio's distance from 1.
Small snapshots (< 64 px wide) are skipped: cube faces and shadow-map thumbnails.

Usage: tools/snap_halves.py <dir> <frame>   e.g. .../plan109/snaps 23003
"""
import sys, glob, os, re
import numpy as np
from PIL import Image

d, frame = sys.argv[1], int(sys.argv[2])
rows = []
for p in sorted(glob.glob(os.path.join(d, f"f{frame:06d}_snap_*.ppm"))):
    m = re.search(r"snap_([0-9A-F]+)_(\d+)x(\d+)", p)
    im = np.asarray(Image.open(p).convert("L"), dtype=np.float64)
    h, w = im.shape
    if w < 64:
        continue
    L, R = im[:, : w // 2], im[:, w - w // 2 :]
    ml, mr = L.mean(), R.mean()
    def corr(a, b):
        a = a - a.mean(); b = b - b.mean()
        n = np.sqrt((a * a).sum() * (b * b).sum())
        return float((a * b).sum() / n) if n else 0.0
    rows.append((abs(mr / ml - 1) if ml > 1 else 0, m[1], f"{w}x{h}", ml, mr, corr(R, L[:, ::-1]), corr(R, L)))
rows.sort(reverse=True)
print(f"frame {frame}: {len(rows)} snapshots >= 64 px wide (addr, extent, meanL, meanR, R/L, corr(R, mirrored L), corr(R, L))")
for dist, addr, ext, ml, mr, cm, cu in rows[:14]:
    print(f"  {addr:>8s} {ext:>10s}  L={ml:6.1f} R={mr:6.1f} R/L={mr/ml if ml>1 else 0:5.2f}  mirror={cm:+.3f} straight={cu:+.3f}")
