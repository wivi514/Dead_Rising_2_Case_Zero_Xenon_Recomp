#!/usr/bin/env python3
"""Is a scene ADVANCING, LOOPING, or FROZEN? Two numbers off CZ_VK_FRAME_STATS.

WHY THIS EXISTS
---------------
Phase A/V wired audio and the prologue cinematic stopped being a frozen black screen.
The session then reported it as "the cinematic plays" — and it does not. It runs
forward about a second, backward about a second, and repeats forever. The operator saw
that in five seconds; the measurement that would have caught it was already sitting in
the frame-stats file nobody had asked the right question of.

The trap is that "frozen" and "advancing" are easy to tell apart and BOTH are easy to
confuse with "looping":

  * `runs`     — maximal stretches of one `cameraFingerprint`. Low means frozen.
  * `distinct` — how many different camera poses were ever visited.

A scene that advances visits each pose about once, so runs ~= distinct. A scene that
ping-pongs revisits the same poses over and over, so `runs` climbs while `distinct`
stays put. A FROZEN scene also scores runs/distinct ~= 1 — one enormous run and no
repeats — which is why the ratio alone is not enough and this script always prints
`runs/frames` beside it.

Measured reference points, all from this title (docs/open-items.md 00j):

    state              runs/distinct   runs/frames
    gameplay, healthy       1.09          0.65
    prologue ping-pong      6.13          0.58
    prologue frozen         1.01          0.08

So: ratio near 1 AND runs/frames well above 0.1 is a scene that is genuinely playing.
Either one alone will call one of the two failure modes healthy.

It is also the free regression gate for 00j — no operator, no new run, just the stats
file every renderer run already writes. And `distinct` turns out to be strikingly
stable for a given loop (1170/1170/1169/1173 across four runs on two binaries), which
makes it a sharp before/after: a fix that does not move `distinct` did not touch the
loop.

USAGE
    python3 tools/frame_loopiness.py <CZ_VK_FRAME_STATS file> [more files...]
"""

import sys


def loopiness(path):
    runs, prev, frames, seq = 0, None, 0, []
    with open(path) as fh:
        for i, line in enumerate(fh):
            if i == 0 or not line.strip():        # header / blank
                continue
            fields = line.split()
            if len(fields) < 5:                   # truncated final line of a killed run
                continue
            frames += 1
            cam = fields[4]                       # cameraFingerprint
            if cam != prev:
                runs += 1
                seq.append(cam)
            prev = cam
    distinct = len(set(seq))
    return frames, runs, distinct


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        frames, runs, distinct = loopiness(path)
        if not frames:
            print(f"{path}: no frame rows")
            continue
        # A verdict, because the whole point is that the raw numbers mislead.
        ratio = runs / max(distinct, 1)
        density = runs / max(frames, 1)
        if density < 0.15:
            verdict = "FROZEN"
        elif ratio > 1.5:
            verdict = "LOOPING"
        else:
            verdict = "advancing"
        print(f"{path.split('/')[-1]:16s} frames={frames:6d} runs={runs:6d} "
              f"distinct={distinct:5d} runs/distinct={ratio:5.2f} "
              f"runs/frames={density:5.2f}  {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
