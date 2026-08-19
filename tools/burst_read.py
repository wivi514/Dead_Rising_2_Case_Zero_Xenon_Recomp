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
import re
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
    # Only real frame files (burstNN_IIII_fFFFFFF.ppm). Without this, a SECOND run of
    # this tool reads the flicker map its first run wrote as if it were a frame, and
    # reports 100% of pixels changed — the tool polluting its own input.
    files = [f for f in files
             if re.fullmatch(r'burst\d{2}_\d{4}_f\d+\.ppm', os.path.basename(f))]
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
              f"The pixel evidence stands. If this burst has no census file (pre-part-57 "
              f"binary), re-take it on a build that writes one — the census is what makes "
              f"a moving-camera burst readable at all.")

    # The census section runs on EVERY path, including "nothing flickered" — a burst
    # taken at a defect that did not fire still documents what was issued.
    read_census(a.dir, seq)


def read_census(dirname, seq):
    """The per-draw half (part 57): was each draw ISSUED on each burst frame?

    This is the question part 56 could not answer — the decal flicker appears ONLY
    under camera motion, so 'the draw list changed' proves nothing by itself. Keyed
    per DRAW, a fixed world-space decal separates cleanly from legitimate churn:
    its key either sits in every frame (issued, then discarded downstream — depth,
    stencil, alpha, or one of our own declines) or toggles in and out (never issued
    on the dark frames — guest visibility, predication, or a decline of ours before
    the census line is written... except the census is written for every draw that
    reaches DoDraw's bind path, so 'absent' means the draw never got that far).

    The key is (ps, v0, verts): v0 is the first vertex of the draw's STREAM, so
    draws sharing one batched buffer share a v0 — those are tracked by COUNT per
    frame instead, which still detects a drop (the count moves).
    """
    import collections
    cen = os.path.join(dirname, f"burst{seq}_census.txt")
    if not os.path.exists(cen):
        print("\n  (no census file — a pre-part-57 burst; issued-or-discarded cannot "
              "be answered from it)")
        return
    frames = {}          # frame -> Counter(key)
    meta = {}            # key -> the identifying fields, for the report
    for ln in open(cen):
        if ln.startswith('#'):
            continue
        m = re.match(r'f(\d+) draw \d+ verts=(\d+) prim=\d+ vs=([0-9a-f]+) '
                     r'ps=([0-9a-f]+) mask=(\S+) blend=(\S+) po=(\S+)', ln)
        if not m:
            continue
        frame = int(m.group(1))
        v0 = re.search(r' v0=(\S+)', ln)
        s0 = re.search(r' s0=([0-9A-F]+)', ln)
        key = (m.group(4), v0.group(1) if v0 else '-', m.group(2))
        frames.setdefault(frame, collections.Counter())[key] += 1
        if key not in meta:
            meta[key] = dict(verts=m.group(2), blend=m.group(6), po=m.group(7),
                             s0=s0.group(1) if s0 else '-')
    if not frames:
        print("\n  !! census file exists but parsed to nothing — its format and this "
              "reader have drifted; fix that before believing anything below")
        return
    nf = len(frames)
    order = sorted(frames)
    print(f"\n  census: {nf} frames, "
          f"{sum(sum(c.values()) for c in frames.values())} draws, "
          f"{len(meta)} distinct draw keys")

    # A key that toggles repeatedly is the candidate; one edge is the camera panning
    # it in or out. Count transitions of its per-frame count.
    toggles = []
    for key in meta:
        counts = [frames[f].get(key, 0) for f in order]
        trans = sum(1 for i in range(1, nf) if (counts[i] > 0) != (counts[i-1] > 0))
        cmoves = sum(1 for i in range(1, nf) if counts[i] != counts[i-1])
        if trans >= 2 or cmoves >= 4:
            toggles.append((trans, cmoves, key, counts))
    toggles.sort(reverse=True)
    decal = lambda k: meta[k]['blend'] == '07060706' and meta[k]['verts'] == '6'
    n_decal_keys = sum(1 for k in meta if decal(k))
    n_decal_toggling = sum(1 for t, c, k, _ in toggles if decal(k))
    print(f"  decal-shaped keys (verts=6 blend=07060706): {n_decal_keys}, "
          f"of which TOGGLING in and out: {n_decal_toggling}")
    if toggles:
        print(f"  keys present in SOME frames and absent in others (top 15 of "
              f"{len(toggles)} by transitions):")
        for trans, cmoves, key, counts in toggles[:15]:
            ps, v0, verts = key
            present = sum(1 for c in counts if c)
            print(f"    ps={ps} v0={v0} verts={verts} blend={meta[key]['blend']} "
                  f"po={meta[key]['po']} s0={meta[key]['s0']}  "
                  f"in {present}/{nf} frames, {trans} in/out transitions"
                  f"{'  <- DECAL-SHAPED' if decal(key) else ''}")
    if n_decal_keys and not n_decal_toggling:
        print("  CENSUS VERDICT: every decal-shaped draw was ISSUED on every census "
              "frame. A decal that blinks on screen is being discarded AFTER issue — "
              "depth, stencil, alpha or a render-state difference, not a missing draw.")
    elif n_decal_toggling:
        print("  CENSUS VERDICT: decal-shaped draws come and go across the burst's "
              "frames — the dark frames are frames the draw was NEVER ISSUED. Look at "
              "the guest's own issuing (visibility, predication, bin masks), not at "
              "depth or blend state.")


if __name__ == "__main__":
    main()
