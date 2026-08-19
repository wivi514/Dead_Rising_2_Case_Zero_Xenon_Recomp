#!/usr/bin/env python3
"""Read an F8 burst and say WHETHER something flickers, WHERE, and WHICH mechanism.

WHY THIS EXISTS. The operator described a defect no single frame can show: "The decals how
it looks like is pretty much normal but it appears and disappear like flicker". A
screenshot of a flicker is a screenshot of one phase of it, and which phase you get is luck
(gotcha 133). F8 records every presented frame for about a second; this reads the result.

IT IS BUILT TO DISCRIMINATE, not to illustrate. Two mechanisms produce an identical still
image and need opposite fixes:

  * the draw is ISSUED every frame and loses a depth fight — z-fighting, which is what a
    decal does when the guest's polygon offset is not honoured;
  * the draw is DROPPED on some frames — by the guest, by predication, by a bin mask, or by
    one of our own declines.

Under the first, the burst's DRAW COUNT and `drawFingerprint` are identical frame to frame
while the pixels change. Under the second they move. The manifest carries both, so this
tool reports the pixel evidence and the draw-list evidence side by side and names which
mechanism they point at — rather than leaving that inference to whoever reads the images.

WHAT IT PRINTS
  * how many pixels changed at all, and how many changed REPEATEDLY (a pixel that changes
    once is the camera moving; a pixel that changes back and forth is a flicker);
  * the bounding box of the flickering region, which is what turns "there is a flicker"
    into "look here";
  * whether the draw list was stable across the burst;
  * and it writes `flicker_map.ppm` — a heat map of change counts — so the region can be
    seen at a glance rather than described.

A NOTE ON THE THRESHOLD. A pixel "changed" when any channel moves by more than
--tol (default 8/255). Below that is dither and film grain; this title's backdrop is
animated, so a tolerance of 0 reports the whole screen and says nothing.

Usage:
    tools/burst_read.py ~/DR2CZ-troubleshooting/burst [--seq 1] [--tol 8]
"""
import argparse
import glob
import os
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        if f.readline().strip() != b'P6':
            return None, 0, 0
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = (int(x) for x in line.split())
        f.readline()                      # maxval
        return f.read(w * h * 3), w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--seq", type=int, default=None, help="which burst; default the first")
    ap.add_argument("--tol", type=int, default=8, help="per-channel change threshold")
    a = ap.parse_args()

    pat = f"burst{a.seq:02d}_*.ppm" if a.seq else "burst*_*.ppm"
    files = sorted(glob.glob(os.path.join(a.dir, pat)))
    if not files:
        sys.exit(f"no burst frames matching {pat} in {a.dir}")
    # One burst at a time: two presses in one directory would otherwise be differenced
    # against each other, which is a comparison between two different moments and would
    # manufacture a flicker that is not there.
    seq = os.path.basename(files[0])[5:7]
    files = [f for f in files if os.path.basename(f)[5:7] == seq]
    print(f"burst {seq}: {len(files)} frames")

    frames = []
    w = h = 0
    for f in files:
        px, fw, fh = read_ppm(f)
        if px is None:
            print(f"  !! {os.path.basename(f)} is not a P6 PPM — skipped")
            continue
        if w and (fw, fh) != (w, h):
            print(f"  !! {os.path.basename(f)} is {fw}x{fh}, not {w}x{h} — skipped")
            continue
        w, h = fw, fh
        frames.append(px)
    if len(frames) < 3:
        sys.exit("need at least three readable frames to see a flicker")

    n = w * h
    changes = bytearray(n)              # how many times each pixel changed, capped at 255
    tol = a.tol
    for k in range(1, len(frames)):
        p, q = frames[k - 1], frames[k]
        for i in range(n):
            j = i * 3
            if (abs(p[j] - q[j]) > tol or abs(p[j + 1] - q[j + 1]) > tol
                    or abs(p[j + 2] - q[j + 2]) > tol):
                if changes[i] < 255:
                    changes[i] += 1

    moved = sum(1 for c in changes if c)
    # REPEATEDLY is the discriminator against a moving camera: a pixel the camera swept
    # past changes once or twice; a pixel that is flickering changes over and over.
    flicker = [i for i, c in enumerate(changes) if c >= max(3, len(frames) // 5)]
    print(f"  {w}x{h}, tolerance {tol}/255")
    print(f"  pixels that changed at all:      {moved:8d}  ({100.0*moved/n:5.2f}%)")
    print(f"  pixels that changed REPEATEDLY:  {len(flicker):8d}  ({100.0*len(flicker)/n:5.2f}%)"
          f"   <- this is the flicker")
    if flicker:
        xs = [i % w for i in flicker]
        ys = [i // w for i in flicker]
        print(f"  flickering region: x {min(xs)}..{max(xs)}, y {min(ys)}..{max(ys)}")

    out = os.path.join(a.dir, f"burst{seq}_flicker_map.ppm")
    hi = max(changes) or 1
    with open(out, 'wb') as f:
        f.write(f"P6\n{w} {h}\n255\n".encode())
        f.write(bytes(b for c in changes for b in (min(255, c * 255 // hi), 0,
                                                   255 - min(255, c * 255 // hi))))
    print(f"  wrote {out}  (red = changed often, blue = never)")

    man = os.path.join(a.dir, f"burst{seq}_manifest.txt")
    if not os.path.exists(man):
        print("  !! no manifest — the draw-list half of the answer is missing")
        return
    rows = [l.split() for l in open(man) if not l.startswith('#') and l.strip()]
    draws = {r[2] for r in rows if len(r) > 2}
    fps = {r[4] for r in rows if len(r) > 4}
    cams = {r[5] for r in rows if len(r) > 5}
    print(f"  draw counts across the burst: {len(draws)} distinct"
          f"{' (' + ', '.join(sorted(draws)[:6]) + ')' if len(draws) <= 6 else ''}")
    print(f"  drawFingerprints:             {len(fps)} distinct")
    print(f"  cameraFingerprints:           {len(cams)} distinct")
    print()
    if not flicker:
        print("  VERDICT: nothing flickered in this burst. Either the defect did not fire, "
              "or it is smaller than the tolerance — re-run with --tol 2 before believing "
              "the first reading.")
    elif len(fps) == 1:
        print("  VERDICT: the pixels flicker and the DRAW LIST DOES NOT CHANGE. The draws "
              "are being issued every frame and something after that is deciding whether "
              "they land — a depth fight is the first thing to check, and this renderer "
              "sets no depthBiasEnable at all while the guest has PA_SU_POLY_OFFSET.")
    elif len(cams) == 1:
        print("  VERDICT: the DRAW LIST ITSELF CHANGES while the CAMERA DID NOT MOVE. That "
              "is the strong form of the answer: the geometry is being dropped on some "
              "frames rather than losing a depth fight — look at the guest's own issuing, "
              "predication, the bin masks, and our own draw declines.")
    else:
        # The camera moving is not a flaw in the burst, it is a limit on what the DRAW-LIST
        # half can conclude: a moving camera changes the draw list legitimately, so a
        # changing list is no longer evidence of anything. The pixel half still stands.
        print(f"  VERDICT: the pixels flicker, but the camera moved during the burst "
              f"({len(cams)} distinct camera fingerprints), so a changing draw list proves "
              f"nothing on its own — a moving camera changes the draw list legitimately. "
              f"The pixel evidence stands; RE-TAKE THE BURST STANDING STILL to make the "
              f"draw-list half readable. That single distinction is what separates "
              f"'z-fighting' from 'dropped geometry'.")


if __name__ == "__main__":
    main()
