#!/usr/bin/env python3
"""The renderer A/B metric: per-era aggregates over a rendered surface.

WHY THIS EXISTS
---------------
Phase 5 spent a session A/B-ing renderer changes against "what percentage of the scene
surface is non-black at frame 600", and produced a confident, wrong result. The title
screen renders a live ANIMATED 3D background driven by guest time, and our frame rate
varies with host load — so frame 600 is a different camera angle every run. The same
binary measured 58.8%, 99.9% and 76.7% across three runs.

THE TWO THINGS THAT DID NOT WORK, both worth knowing before trying them again:

1. **Comparing the presented frame.** At the title screen the front buffer is the logo
   era — mostly UI, 2-36% covered. Disabling the 16-bit texcoord unswizzle, a change
   touching 476,858 draws a run, moved it by 0.1 percentage points. The defect lives on
   the SCENE surface; the presented frame is the overlay in front of it.

2. **Aligning frames by content, the way tools/xtr_determinism.py aligns the capture
   pair.** Matching on (draw fingerprint, camera fingerprint) does produce bit-exact
   agreement — 257 of 257 frames identical, zero delta — and it is worthless, because
   the only frames whose exact camera constants recur across two runs are the ones where
   the scene is EMPTY. All 257 had coverage 0.00% and a single distinct pixel hash: 257
   copies of a black image, reported as a 100% baseline. A metric that looks
   authoritative and tests nothing is worse than a noisy one.

   The general point: for a scene animated off wall-clock time, no post-hoc alignment
   can work. Exact alignment selects for stasis, which is exactly the content least able
   to reveal a rendering difference.

WHAT WORKS
----------
Aggregate over the era instead of aligning within it — which is what
`docs/phase5-kickoff.md` prescribed for the GPU gate all along (gotcha 38: gate on
per-era aggregates, NEVER on frame index).

The **median** over every frame that has scene content is stable, because several
hundred frames sample the whole animation cycle. The mean is not: it is pulled about by
how long a run happened to spend in each part of the cycle (measured means over five
runs of one binary spread 64.3 to 70.8; the medians spread 1.36). Measured:

    median surface coverage   64.34  64.44  64.45  64.56  65.70     band = 1.36 pp

and the metric has been shown capable of FAILING, which is the part that makes it a
metric (gotcha 30):

    CZ_VK_NO_TEXCOORD_SWAP=1   64.56   inside the band  -> no detectable effect
    CZ_VK_PRIM_RESTART=1       81.44   16 pp outside    -> detected decisively

So a shift of more than about 1.5 pp in the median is a real change; anything smaller
this metric cannot see, and the honest answer is then "no detectable effect" rather
than a number.

USAGE
    # every run needs the surface, and it must be the SAME surface
    CZ_VKDRAW=1 CZ_VK_FRAME_STATS_SURFACE=06BE4000 \\
        CZ_VK_FRAME_STATS=a.txt ./cz_runtime
    tools/frame_compare.py a.txt b.txt [c.txt ...]

    With one file it reports that run's aggregates. With several it reports each plus
    the spread, so a baseline band and an arm can be read off one table.

    The surface address is the scene's resolve destination — 06BE4000 at the title
    screen. CZ_VK_RESOLVE_TRACE=<frame> names it for any era.
"""
import statistics as st
import sys

# The measured baseline band: 1.36 pp over five runs of one binary, rounded up.
#
# Quoted rather than derived from the runs being compared. A band computed from the
# inputs widens to accommodate whatever difference is present, which is precisely how a
# metric stops being able to fail — and this file exists because that already happened
# once.
BASELINE_BAND_PP = 1.5


def load(path):
    """Frames that have scene content.

    Frames whose surface is absent or entirely black carry no signal and are excluded.
    Including them would turn the metric into a measurement of how long the run spent
    before the scene existed, which varies with load and is the original defect.
    """
    rows = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        if len(p) < 17 or p[16] == "0" * 16:
            continue
        coverage = float(p[13])
        if coverage <= 0.0:
            continue
        rows.append({"frame": int(p[0]), "draws": int(p[1]), "vertices": int(p[2]),
                     "w": int(p[11]), "h": int(p[12]), "coverage": coverage,
                     "luma": float(p[14]), "colours": int(p[15])})
    return rows


def aggregates(rows):
    return {
        "n": len(rows),
        "coverage": st.median(r["coverage"] for r in rows),
        "luma": st.median(r["luma"] for r in rows),
        "colours": st.median(r["colours"] for r in rows),
        "draws": st.median(r["draws"] for r in rows),
        "vertices": st.median(r["vertices"] for r in rows),
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    print(f"{'run':<26} {'frames':>6} {'med cov%':>9} {'med luma':>9} "
          f"{'med colours':>12} {'med draws':>10} {'med verts':>10}")
    results = []
    for path in sys.argv[1:]:
        rows = load(path)
        name = path.split("/")[-1]
        if not rows:
            print(f"{name:<26} NO SCENE CONTENT — was CZ_VK_FRAME_STATS_SURFACE set, "
                  f"and is it the right surface?")
            continue
        a = aggregates(rows)
        results.append((name, a))
        print(f"{name:<26} {a['n']:>6} {a['coverage']:>9.2f} {a['luma']:>9.3f} "
              f"{a['colours']:>12.0f} {a['draws']:>10.0f} {a['vertices']:>10.0f}")

    if len(results) < 2:
        return 0

    cov = [a["coverage"] for _, a in results]
    spread = max(cov) - min(cov)
    print()
    print(f"median-coverage spread: {spread:.2f} pp "
          f"({min(cov):.2f} .. {max(cov):.2f})")
    if spread <= BASELINE_BAND_PP:
        print(f"WITHIN the measured baseline band ({BASELINE_BAND_PP} pp): "
              f"NO DETECTABLE DIFFERENCE between these runs.")
    else:
        print(f"OUTSIDE the baseline band ({BASELINE_BAND_PP} pp): these runs render "
              f"measurably differently.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
