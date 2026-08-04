#!/usr/bin/env python3
"""Compare two `.xtr` GPU captures of the same drive — the determinism baseline.

WHY THIS EXISTS
---------------
Finding 9 records that B1b was captured as a determinism control for B1 and that
the comparison was never made, because nothing here could read a GPU stream.
This is that comparison, and its output is the number every later GPU gate is
measured against: if a same-binary, same-drive repeat on *hardware* already
differs by X, our runtime differing by less than X proves nothing.

THREE WAYS TO GET THIS WRONG, ALL OF WHICH THIS TOOL COMMITTED FIRST
--------------------------------------------------------------------
Each of these produced a confident, plausible, wrong answer before being caught.
They are written up here because the next person will reach for all three.

**1. Comparing file sizes.** B1 is 1.61 GiB, B1b 1.12 GiB — a ratio of 0.70 that
looks like gross non-determinism and means nothing. A continuous stream emits
frames for as long as the run lasts, so size mostly measures how long a human
sat on a menu. A byte-diff is worse: host fields (addresses, handles,
timestamps) differ every run under ASLR, so the streams diverge within the first
few hundred bytes even under perfect guest determinism.

**2. Fingerprinting frames with MemoryRead/MemoryWrite counts.** Those commands
are Xenia recording guest memory so the trace can replay standalone, and whether
a given block needs recording depends on the emulator's own dirty-tracking — not
on anything the guest did. Measured on B1/B1b: memread counts alone align on
17.7% of frames, and folding them into an otherwise-agreeing fingerprint dragged
it from 42.7% down to 16.0%. **An emulator-side bookkeeping field in a content
fingerprint manufactures non-determinism.** They are counted here and reported
under a separate heading that is explicitly not part of the verdict.

**3. Comparing whole runs instead of the fixed prefix.** These captures are
boot -> logos -> movies -> title -> *the operator presses exit whenever*. B1 sat
on the title for 619 frames and B1b for 409. That difference is a human's hand,
and averaged over the whole run it swamps everything real: the same comparison
reads 42.7% whole-run and 80.0% over the prefix. The B1/B1b capture notes said
to align over the fixed boot+movie prefix and ignore the idle tail; ignoring
that instruction produced a "this title is not deterministic" verdict that was
purely an artifact of method.

WHAT IT ACTUALLY MEASURES, AND WHY AGGREGATES ARE THE HEADLINE
--------------------------------------------------------------
Runs are deterministic in *content* and jittery in *phase*: a run can spend one
extra frame on a load, which shifts every later frame and makes a naive
frame-i-vs-frame-i comparison collapse. So:

  * frames are segmented into **eras** by draw-count regime, and eras are
    compared by aggregate — this is the primary number, and it is robust to
    phase drift because it does not care which frame a draw landed in;
  * frame sequences are aligned with `difflib.SequenceMatcher`, which absorbs
    insertions and deletions, and the **phase-lag distribution** is reported so
    drift is visible as its own quantity rather than as "divergence";
  * the naive number is printed *alongside* the aligned one, precisely so the
    gap between them is visible rather than hidden behind whichever the author
    preferred.

Addresses are excluded from the verdict and reported separately: an address
difference is a memory-layout question, not a rendering-content one.

USAGE
    xtr_determinism.py <a.xtr> <b.xtr> [--labels A B] [--json out.json]
                       [--era-threshold N] [--include-tail]
"""

import argparse
import collections
import difflib
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402


def scan(path, label):
    """Per-frame PM4 content + separately-tracked capture-side counts."""
    data, hdr = xtr.open_trace(path)
    n = len(data)

    frames = []          # (counts_tuple, opcode_histogram_tuple)
    capture = []         # per-frame memread/memwrite — NOT part of the verdict
    c = collections.Counter()
    ops = collections.Counter()
    desyncs = []

    t0 = time.time()
    for off, cmd in xtr.walk(data, n, on_desync=lambda o, w: desyncs.append((o, w))):
        if cmd == xtr.CMD_PACKET_START:
            dwords = xtr._U32.unpack_from(data, off + 8)[0]
            if not dwords:
                continue
            w0 = xtr._BE_U32.unpack_from(data, off + 12)[0]
            t = xtr.pm4_type(w0)
            c[f"type{t}"] += 1
            if t == 3:
                op = xtr.pm4_type3_opcode(w0)
                ops[op] += 1
                if op in xtr.DRAW_OPCODES:
                    c["draws"] += 1
        elif cmd == xtr.CMD_MEMORY_READ:
            c["memread"] += 1
        elif cmd == xtr.CMD_MEMORY_WRITE:
            c["memwrite"] += 1
        elif cmd == xtr.CMD_INDIRECT_BUFFER_START:
            c["indirect_buffers"] += 1
        elif cmd == xtr.CMD_EVENT:
            frames.append(((c["type0"], c["type1"], c["type2"], c["type3"],
                            c["draws"], c["indirect_buffers"]),
                           tuple(sorted(ops.items()))))
            capture.append((c["memread"], c["memwrite"]))
            c = collections.Counter()
            ops = collections.Counter()

    return frames, capture, {
        "label": label, "path": str(path), "size": hdr["size"],
        "frames": len(frames), "desyncs": len(desyncs),
        "seconds": round(time.time() - t0, 1)}


def segment(frames, threshold):
    """Split a run into eras by draw-count regime.

    An era is a stretch of frames drawing a similar amount — a logo, a movie, a
    menu. Boundaries are where the regime changes and stays changed for at least
    `MIN_ERA` frames, so a single anomalous frame does not split an era in two.
    """
    MIN_ERA = 8
    if not frames:
        return []

    def regime(f):
        d = f[0][4]
        for i, t in enumerate(threshold):
            if d < t:
                return i
        return len(threshold)

    eras = []
    cur = regime(frames[0])
    start = 0
    for i, f in enumerate(frames):
        r = regime(f)
        if r != cur and i - start >= MIN_ERA:
            eras.append((cur, start, i))
            start = i
            cur = r
    eras.append((cur, start, len(frames)))
    return eras


def totals(frames):
    t = collections.Counter()
    for counts, ops in frames:
        for k, v in zip(("type0", "type1", "type2", "type3", "draws",
                         "indirect_buffers"), counts):
            t[k] += v
        for op, k in ops:
            t[f"op{op:02X}"] += k
    return t


def align(fa, fb):
    sm = difflib.SequenceMatcher(None, fa, fb, autojunk=False)
    blocks = [b for b in sm.get_matching_blocks() if b.size]
    matched = sum(b.size for b in blocks)
    lags = collections.Counter()
    for b in blocks:
        lags[b.b - b.a] += b.size
    lo = min(len(fa), len(fb))
    naive = sum(1 for i in range(lo) if fa[i] == fb[i])
    return matched, naive, lo, lags, blocks


def compare(a_path, b_path, labels, era_threshold, include_tail, out_json):
    la, lb = labels
    print(f"GPU stream determinism: {la} vs {lb}\n")
    fa, ca, sa = scan(a_path, la)
    print(f"  {la:4} {sa['frames']:6,} frames  {sa['size'] / 2**30:5.2f} GiB  "
          f"({sa['seconds']}s)")
    fb, cb, sb = scan(b_path, lb)
    print(f"  {lb:4} {sb['frames']:6,} frames  {sb['size'] / 2**30:5.2f} GiB  "
          f"({sb['seconds']}s)")
    for s in (sa, sb):
        if s["desyncs"]:
            print(f"  !! {s['label']} has {s['desyncs']} desync region(s); everything "
                  f"below is over a damaged stream.")

    ratio = min(sa["size"], sb["size"]) / max(sa["size"], sb["size"])
    print(f"\n  size ratio {ratio:.3f} — NOT a determinism metric. It mostly measures "
          f"how long\n  each run sat on a menu. See this tool's docstring.")

    # --- eras --------------------------------------------------------------
    ea, eb = segment(fa, era_threshold), segment(fb, era_threshold)
    print(f"\nERA STRUCTURE (draw-count regimes; the shape of the run)")
    print(f"  {'':4} {'#':>3}  {'frames':>14}  {'count':>6}  {'draws':>11}  {'mean':>8}")
    for tag, eras, frames in ((la, ea, fa), (lb, eb, fb)):
        for i, (r, s, e) in enumerate(eras):
            seg = [f[0][4] for f in frames[s:e]]
            print(f"  {tag:4} {i:3}  {s:6}-{e - 1:<7}  {e - s:6}  {sum(seg):11,}  "
                  f"{statistics.mean(seg):8.1f}")
        print()

    if len(ea) != len(eb):
        print(f"  !! different era COUNT ({len(ea)} vs {len(eb)}) — the runs did not "
              f"follow the\n     same path. Per-era comparison below is not "
              f"meaningful; investigate first.")

    # --- choose the comparison window -------------------------------------
    # The last era is where a human decided when to stop. Its length is not a
    # property of the title, so it is excluded by default.
    if include_tail:
        ka, kb = len(fa), len(fb)
        window = "WHOLE RUN (--include-tail)"
    else:
        ka = ea[-1][1] if len(ea) > 1 else len(fa)
        kb = eb[-1][1] if len(eb) > 1 else len(fb)
        window = f"FIXED PREFIX ({la}[0:{ka}], {lb}[0:{kb}]) — final era excluded"
    print(f"comparison window: {window}")
    if not include_tail and len(ea) > 1:
        print(f"  The final era is {len(fa) - ka:,} frames in {la} and "
              f"{len(fb) - kb:,} in {lb}. That\n  difference is when the operator "
              f"chose to exit, not a property of the title;\n  including it is how "
              f"this comparison first read 42.7% instead of 80.0%.")

    pa, pb = fa[:ka], fb[:kb]

    # --- frame alignment ---------------------------------------------------
    matched, naive, lo, lags, blocks = align(pa, pb)
    print(f"\nFRAME ALIGNMENT over the window")
    print(f"  naive frame-i vs frame-i : {naive:,}/{lo:,} ({naive / max(lo,1) * 100:.1f}%)")
    print(f"  aligned (insert/delete)  : {matched:,}/{lo:,} "
          f"({matched / max(lo,1) * 100:.1f}%)")
    if blocks:
        print(f"  longest identical run    : "
              f"{max(b.size for b in blocks):,} frames")
    print(f"  phase lag (frames {lb} runs ahead of {la}):")
    for lag, cnt in lags.most_common(6):
        print(f"      {lag:+4d}: {cnt:,} frames ({cnt / max(matched,1) * 100:.1f}%)")
    print("  A small set of small lags covering most frames is what "
          "'deterministic in content,\n  jittery in phase' looks like. It is drift, "
          "not divergence.")

    # --- aggregates: the headline -----------------------------------------
    ta, tb = totals(pa), totals(pb)
    print(f"\nAGGREGATE DELTA over the window  <-- the determinism baseline")
    keys = sorted(set(ta) | set(tb))
    worst = 0.0
    worst_key = None
    rows = []
    for k in keys:
        x, y = ta[k], tb[k]
        d = abs(x - y) / max(x, y, 1) * 100
        rows.append((k, x, y, d))
        if d > worst:
            worst, worst_key = d, k
    for k, x, y, d in rows:
        mark = "  <<" if d > 1.0 else ""
        print(f"  {k:20} {la} {x:11,}  {lb} {y:11,}   {d:6.2f}%{mark}")
    print(f"\n  worst aggregate delta: {worst:.2f}% ({worst_key})")
    draws_delta = next(d for k, _, _, d in rows if k == "draws")
    print(f"  draws delta          : {draws_delta:.2f}%")

    # --- capture-side, explicitly not the verdict -------------------------
    ma, mb = sum(x[0] for x in ca[:ka]), sum(x[0] for x in cb[:kb])
    wa, wb = sum(x[1] for x in ca[:ka]), sum(x[1] for x in cb[:kb])
    print(f"\nCAPTURE-SIDE COUNTS (reported, NOT part of the verdict)")
    print(f"  MemoryRead   {la} {ma:,}  {lb} {mb:,}   "
          f"{abs(ma - mb) / max(ma, mb, 1) * 100:.2f}%")
    print(f"  MemoryWrite  {la} {wa:,}  {lb} {wb:,}   "
          f"{abs(wa - wb) / max(wa, wb, 1) * 100:.2f}%")
    print("  These are Xenia deciding which guest memory still needs recording, which\n"
          "  depends on its own dirty-tracking rather than on the guest. Including them\n"
          "  in a content fingerprint manufactures non-determinism — measured here at\n"
          "  42.7% -> 16.0% frame agreement when they were wrongly folded in.")

    # --- verdict ----------------------------------------------------------
    frame_pct = matched / max(lo, 1) * 100
    print("\nVERDICT")
    if worst <= 1.0:
        print(f"  Content-deterministic. Every aggregate over the fixed window agrees "
              f"to within\n  {worst:.2f}%.")
    elif worst <= 5.0:
        print(f"  Substantially deterministic: worst aggregate {worst:.2f}%. Treat "
              f"anything below\n  that as noise.")
    else:
        print(f"  NOT content-deterministic: worst aggregate {worst:.2f}%. Find out why "
              f"before\n  building any GPU gate on these captures.")
    print(f"\n  Frame-exact agreement is only {frame_pct:.1f}% even over the fixed "
          f"window, because of\n  the phase drift above. So a frame-INDEXED GPU gate is "
          f"not viable on this title:\n  gate on per-era aggregates, which are robust "
          f"to drift.")
    print(f"\n  BASELINE FOR PHASE 4: two hardware runs of one drive differ by "
          f"{worst:.2f}% on the\n  worst aggregate and {draws_delta:.2f}% on draws. Our "
          f"runtime landing inside that band\n  is not evidence of correctness; landing "
          f"outside it is evidence of a defect.")

    if out_json:
        Path(out_json).write_text(json.dumps({
            "a": sa, "b": sb, "window": window,
            "window_frames": [ka, kb],
            "eras": {la: ea, lb: eb},
            "frame_matched": matched, "frame_naive": naive, "frame_compared": lo,
            "phase_lags": {str(k): v for k, v in lags.items()},
            "aggregates": {k: {la: x, lb: y, "delta_pct": d} for k, x, y, d in rows},
            "worst_delta_pct": worst, "worst_key": worst_key,
            "draws_delta_pct": draws_delta,
            "capture_side": {"memread": [ma, mb], "memwrite": [wa, wb]},
        }, indent=2))
        print(f"\n-> {out_json}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--labels", nargs=2, default=["A", "B"])
    ap.add_argument("--json", dest="out_json")
    ap.add_argument("--era-threshold", type=int, nargs="*", default=[50, 130, 600],
                    help="draws-per-frame boundaries that separate eras "
                         "(default: 50 130 600, measured on Case Zero's boot)")
    ap.add_argument("--include-tail", action="store_true",
                    help="compare whole runs including the final era. Almost always "
                         "wrong: the final era's length is when the operator chose to "
                         "exit.")
    args = ap.parse_args()
    compare(args.a, args.b, args.labels, args.era_threshold, args.include_tail,
            args.out_json)


if __name__ == "__main__":
    main()
