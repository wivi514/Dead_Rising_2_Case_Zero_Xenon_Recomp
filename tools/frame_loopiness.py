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

WHY IT ALSO PRINTS QUARTERS, ADDED IN PART 29 AFTER THE WHOLE-FILE NUMBER MISLED
--------------------------------------------------------------------------------
A whole-file score AVERAGES OVER ERAS, and on this title the eras are wildly unalike:
a prologue run spends ~1,870 frames in menus and intro before the cinematic starts.
Measured on the very run that established the mechanism:

    whole run              frames=12443  runs=7179  distinct=1170  runs/distinct= 6.14
    before the cinematic   frames= 1869  runs=1016  distinct=1010  runs/distinct= 1.01
    THE CINEMATIC ERA      frames=10573  runs=6162  distinct= 161  runs/distinct=38.27

The defect is a factor of **38**, and the number this project has been quoting for it
is **6.14** — because the 1,010 healthy menu poses inflate `distinct` while
contributing barely any of the runs. Every one of the four recorded 00j readings has
that dilution in it.

This matters in the direction that hurts: a partial fix that halved the loop would
move the whole-file ratio from 6.14 to ~3.6 and could be read as "nearly fixed", and a
fix that cleaned up the menus while leaving the cinematic alone would move it too. So
the quarters are printed unconditionally. They are a crude era split — no knowledge of
where the cinematic starts is needed, which is the point — and a run whose quarters
disagree is a run whose single number means nothing. `--from`/`--to` narrow it exactly
once you know the boundary.

USAGE
    python3 tools/frame_loopiness.py <CZ_VK_FRAME_STATS file> [more files...]
    python3 tools/frame_loopiness.py --from 1870 run.txt      # frame index window
    python3 tools/frame_loopiness.py --from 1870 --to 12000 run.txt
"""

import sys


def cameras(path, lo, hi):
    """The cameraFingerprint column, in order, over the [lo, hi) row window."""
    out = []
    with open(path) as fh:
        for i, line in enumerate(fh):
            if i == 0 or not line.strip():        # header / blank
                continue
            fields = line.split()
            if len(fields) < 5:                   # truncated final line of a killed run
                continue
            out.append(fields[4])                 # cameraFingerprint
    return out[lo:hi if hi is not None else len(out)]


def loopiness(cams):
    runs = sum(1 for a, b in zip([None] + cams, cams) if a != b)
    return len(cams), runs, len(set(cams))


def verdict(frames, runs, distinct):
    """FROZEN, LOOPING or advancing — and it takes BOTH numbers to tell them apart.

    A frozen camera scores runs/distinct ~= 1.01, the same as a healthy one; only
    runs/frames separates them. Reading either alone calls one failure mode healthy.
    """
    ratio = runs / max(distinct, 1)
    density = runs / max(frames, 1)
    if density < 0.15:
        return "FROZEN", ratio, density
    if ratio > 1.5:
        return "LOOPING", ratio, density
    return "advancing", ratio, density


def report(label, cams):
    frames, runs, distinct = loopiness(cams)
    if not frames:
        print(f"{label:22s} (no frame rows)")
        return
    name, ratio, density = verdict(frames, runs, distinct)
    print(f"{label:22s} frames={frames:6d} runs={runs:6d} distinct={distinct:5d} "
          f"runs/distinct={ratio:6.2f} runs/frames={density:5.2f}  {name}")


def main(argv):
    lo, hi, paths = 0, None, []
    it = iter(argv[1:])
    for a in it:
        if a == "--from":
            lo = int(next(it))
        elif a == "--to":
            hi = int(next(it))
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    for path in paths:
        cams = cameras(path, lo, hi)
        report(path.split("/")[-1], cams)
        # Unconditional era split. A single number over a run that changes character
        # halfway is an average of two different scenes, and on this title that
        # average has understated the live defect by 6x — see the header.
        n = len(cams)
        if n >= 400 and hi is None and lo == 0:
            q = n // 4
            for k in range(4):
                report(f"    quarter {k + 1}", cams[k * q:(k + 1) * q if k < 3 else n])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
