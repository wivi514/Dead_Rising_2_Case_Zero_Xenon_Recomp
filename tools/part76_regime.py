#!/usr/bin/env python3
"""Is the frame waiting on the GPU or on the CPU? Read a CZ_VK_FRAME_TRACE and say.

WHY THIS EXISTS. For 75 parts this port was CPU-bound and said so in every document. Part
76 took 4.15 ms of CPU and 1.66 ms of GPU out of every frame (the present readback was
both) and the autonomous route came out GPU-bound — GPU 10.55 ms of a 10.59 ms wall, fence
2.99. The immediate cost of not noticing was a correct, free CPU fix whose A/B measured
zero (gotcha 453). So the regime is now something to READ before pricing anything, and it
has to be as cheap to ask as a grep.

THE THREE COLUMNS THAT DECIDE IT, and none of them alone is enough:

  fence   the CPU BLOCKED waiting for the GPU. Large = the GPU is the limiter. This is the
          direct evidence and the one to lead with.
  GPU     from the frame's own command-buffer timestamps. It OVERLAPS the CPU work rather
          than adding to it, so `GPU/wall` near 100% means the device is busy the whole
          frame — not that the frame is GPU time.
  CPUrec  the CPU's own recording. `wall = CPUrec + fence + sleep + residual`, and those
          four DO sum, which is what makes an attribution possible at all (part 74).

BANDED BY DRAW COUNT, because that is the axis the two costs disagree on: GPU cost tracks
PIXELS and CPU cost tracks DRAWS, so a route can be GPU-bound at 6,000 draws and CPU-bound
at 10,000 in the same run at the same resolution. A single whole-run median hides exactly
the crossover this is looking for.

TEXTURE-UPLOAD FRAMES ARE SPLIT OUT, not dropped: they are the port's remaining HITCH item
and they are 82-90% texture work, so folding them into a throughput median moves it for a
reason that has nothing to do with throughput (§6dn).
"""
import sys, statistics

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    sys.exit("usage: part76_regime.py <trace file>")
with open(path) as f:
    head = f.readline().split()
    # `# MARK n frame N` lines share this file with the per-frame rows — an F7 press
    # writes one, and that is the point of the file. Skip any line that is not a full
    # row rather than any line that starts with a known prefix: a trace from a build
    # that adds a column must fail LOUDLY on the header check below, not silently drop
    # every row (gotcha 25 — a filter that cannot match is not a clean result).
    rows = [l.split() for l in f
            if l.strip() and not l.startswith("#") and not l.startswith("frame")]
    rows = [r for r in rows if len(r) == len(head)]
if not rows:
    sys.exit("** the trace is empty — the run wrote no frames")
i = {n: k for k, n in enumerate(head)}
need = ("draws", "wallUs", "gpuUs", "fenceUs", "recordUs", "texUploads")
missing = [n for n in need if n not in i]
if missing:
    sys.exit(f"** this trace has no {missing} column — it is from an older build")
c = lambda r, n: float(r[i[n]])

clean = [r for r in rows if c(r, "texUploads") == 0]
tex   = [r for r in rows if c(r, "texUploads") > 0]

print(f"  regime, from {len(rows)} traced frames "
      f"({len(clean)} with no texture upload, {len(tex)} with)")
if not clean:
    print("  ** every frame carried a texture upload — no throughput population to read")
    sys.exit(0)

bands = [(0, 3000), (3000, 5000), (5000, 7000), (7000, 9000), (9000, 12000), (12000, 10**9)]
print(f"  {'draws':>14} {'n':>6} {'wall':>7} {'GPU':>7} {'fence':>7} {'CPUrec':>7}   verdict")
for lo, hi in bands:
    v = [r for r in clean if lo <= c(r, "draws") < hi]
    if len(v) < 20:
        continue
    m = lambda n: statistics.median([c(r, n) for r in v]) / 1000.0
    wall, gpu, fence, rec = m("wallUs"), m("gpuUs"), m("fenceUs"), m("recordUs")
    # The verdict is on the FENCE, because that is the only column that says the CPU
    # actually stopped. A high GPU/wall with no fence wait means the device is busy and
    # keeping up, which is the good case and not a limit.
    share = 100.0 * fence / wall if wall else 0.0
    if share >= 20:      verdict = "GPU-BOUND — the CPU idles here"
    elif share >= 8:     verdict = "GPU-leaning, some CPU slack"
    elif share >= 2:     verdict = "balanced"
    else:                verdict = "CPU-BOUND — a CPU saving converts to frame time"
    hs = f"{lo}-{hi}" if hi < 10**9 else f"{lo}+"
    print(f"  {hs:>14} {len(v):>6} {wall:7.2f} {gpu:7.2f} {fence:7.2f} {rec:7.2f}"
          f"   {verdict}")

if tex:
    m = lambda n: statistics.median([c(r, n) for r in tex]) / 1000.0
    p99 = statistics.quantiles([c(r, "wallUs") for r in tex], n=100)[98] / 1000.0 \
          if len(tex) >= 100 else max(c(r, "wallUs") for r in tex) / 1000.0
    print(f"\n  texture-upload frames: median {m('wallUs'):.2f} ms, p99 {p99:.2f}, "
          f"worst {max(c(r,'wallUs') for r in tex)/1000.0:.1f} — the HITCH item, "
          f"part 77 item 1")
