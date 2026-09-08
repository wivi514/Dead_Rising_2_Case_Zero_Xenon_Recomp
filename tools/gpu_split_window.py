#!/usr/bin/env python3
"""Read CZ_VK_GPU_PASSES dumps out of a log and print the split for a WINDOW, not the run.

WHY THIS EXISTS. The per-region GPU split (`CZ_VK_GPU_PASSES=1`) prints CUMULATIVE
ms/frame over every frame since the instrument armed, and it prints at exit. Two things
break that on the Windows test box (czamd): the process there is ended by
`Stop-Process`, which is TerminateProcess and skips the exit dump entirely (part 102's
czamd logs carry none), and a cumulative mean over a run that spends its first 90-130 s
booting and walking menus is not the crowd's split — it is the crowd's split diluted by
several thousand 60-draw frames. `CZ_VK_STATS=N` re-prints the whole counter block every N
frames, and since the counters are cumulative, two consecutive dumps at frames F1 < F2
give the window's mean per class as (ms2*F2 - ms1*F1) / (F2 - F1). This script does that
subtraction for every consecutive pair and for the whole run, so a czamd log killed at
timeout still yields a crowd-window split — and so the czamd and 3070 numbers can be read
at the SAME draw band rather than as two runs of different shape.

Usage:
  python3 tools/gpu_split_window.py <log> [--all]     # last window (default) or every window
"""
import re
import sys

HDR = re.compile(r"GPU per-region split \(CZ_VK_GPU_PASSES\) over (\d+) frames — ([\d.]+) ms/frame measured, ([\d.]+) ms attributed")
ROW = re.compile(r"^\[vk\]\s+(pass: 0 draws|pass: 1 draw|pass: 2-255 draws|pass: >=256 draws|resolve copy|resolve clear|present blit|present readback|cube face refresh|snapshot views|pass-begin barriers|resolve barriers)\s+([\d.]+) ms/frame")
FPS = re.compile(r"^\[fps\].*draws med (\d+)")


def parse(path):
    dumps = []
    cur = None
    draws_since = []
    with open(path, errors="replace") as f:
        for line in f:
            m = FPS.match(line)
            if m:
                draws_since.append(int(m.group(1)))
                continue
            m = HDR.search(line)
            if m:
                cur = {"frames": int(m.group(1)), "total": float(m.group(2)),
                       "attr": float(m.group(3)), "cls": {}, "draws": draws_since}
                draws_since = []
                dumps.append(cur)
                continue
            if cur is not None:
                m = ROW.match(line)
                if m:
                    cur["cls"][m.group(1)] = float(m.group(2))
    return dumps


def window(a, b):
    """Per-class mean over the frames between dump a and dump b (a may be None = run start)."""
    fa = a["frames"] if a else 0
    fb = b["frames"]
    n = fb - fa
    if n <= 0:
        return None
    out = {"frames": n}
    for k in ("total", "attr"):
        va = a[k] * fa if a else 0.0
        out[k] = (b[k] * fb - va) / n
    out["cls"] = {}
    for c, vb in b["cls"].items():
        va = a["cls"].get(c, 0.0) * fa if a else 0.0
        out["cls"][c] = (vb * fb - va) / n
    ds = b["draws"]
    out["draws_med"] = sorted(ds)[len(ds) // 2] if ds else 0
    out["draws_min"] = min(ds) if ds else 0
    out["draws_max"] = max(ds) if ds else 0
    return out


def show(w, label):
    print(f"--- {label}: {w['frames']} frames, fps-window draws med {w['draws_med']} "
          f"({w['draws_min']}..{w['draws_max']})")
    print(f"    GPU {w['total']:.3f} ms/frame measured, {w['attr']:.3f} attributed, "
          f"residual {w['total'] - w['attr']:.3f}")
    for c, v in sorted(w["cls"].items(), key=lambda kv: -kv[1]):
        if v > 0.0005 or c.startswith("pass: >="):
            print(f"    {c:22s} {v:8.3f} ms/frame  {100.0 * v / w['total'] if w['total'] else 0:5.1f}%")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    dumps = parse(sys.argv[1])
    if not dumps:
        print("no GPU per-region split dump in", sys.argv[1], "(CZ_VK_GPU_PASSES=1 and either "
              "an exit dump or CZ_VK_STATS=N are needed)")
        sys.exit(1)
    show(window(None, dumps[-1]), f"whole run (cumulative, {len(dumps)} dumps)")
    if len(dumps) >= 2:
        if "--all" in sys.argv:
            for i in range(1, len(dumps)):
                w = window(dumps[i - 1], dumps[i])
                if w:
                    show(w, f"window {i}")
        else:
            show(window(dumps[-2], dumps[-1]), "last window")


if __name__ == "__main__":
    main()
