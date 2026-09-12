#!/usr/bin/env python3
"""INCLUSIVE shares and CALLER tables for one thread out of a DWARF call-graph capture.

WHY THIS EXISTS. `perf report --children` on the part-116 call-graph capture printed the
inclusive column equal to the self column for every row — it does not fold the DWARF
chains the way it folds frame-pointer ones here — while `perf script` shows the chains
are complete (Draw Thread: fence wait <- sub_82845160 <- ... <- the thread entry). So
this reads `perf script` directly and answers the two questions item 1 of
docs/perf-plan-part116.md asks of a hot guest function:

  1. INCLUSIVE share: what fraction of the thread's samples have this function anywhere
     on the stack — which is how a flat profile of 2,000 engine functions at <5% each
     becomes a tree with a handful of subsystems at 20-40% each;
  2. CALLERS of the top self-time functions: who calls memcpy, who calls the D3D
     constant copy — the guest function that consumes the result is the specification.

Frames are folded to the `sub_XXXXXXXX` name (the `__imp__` and the inlined alias are the
same function); recursion is counted once per sample.

Usage:
    tools/part116_callers.py <cg.perf.data> --tid N [--top 40] [--callers-of sub_X ...]
    tools/part116_callers.py <cg.perf.data> --tid N --depth 3     # top-of-stack table
"""
import argparse, collections, re, subprocess, sys

FRAME = re.compile(r"^\s+([0-9a-f]+)\s+(\S+?)(?:\+0x[0-9a-f]+)?\s+\(")
HEAD = re.compile(r"^\s*(.+?)\s+(\d+)\s+[\d.]+:\s+(\d+)\s+")

def fold(sym):
    sym = sym.replace("__imp__", "")
    return sym

def read(path, tid):
    out = subprocess.run(["perf", "script", "-i", path, "--tid", str(tid)],
                         capture_output=True, text=True, errors="replace").stdout
    samples = []
    cur = None
    for line in out.splitlines():
        if not line.strip():
            if cur is not None:
                samples.append(cur); cur = None
            continue
        h = HEAD.match(line)
        if h and not line.startswith("\t") and not line.startswith(" " * 8):
            cur = []
            continue
        m = FRAME.match(line)
        if m and cur is not None:
            cur.append(fold(m.group(2)))
    if cur:
        samples.append(cur)
    return samples

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("perf")
    ap.add_argument("--tid", type=int, required=True)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--callers-of", nargs="*", default=[])
    ap.add_argument("--below", nargs="*", default=[],
                    help="for each named function, the table of its DIRECT callees by "
                         "inclusive share (the subsystem split one level down)")
    ap.add_argument("--depth", type=int, default=0,
                    help="print the table of frames at this depth below the thread entry "
                         "(0 = the leaf), i.e. which top-level phase each sample belongs to")
    a = ap.parse_args()
    samples = read(a.perf, a.tid)
    n = len(samples)
    if not n:
        sys.exit(f"no samples for tid {a.tid}")
    print(f"tid {a.tid}: {n} samples with chains")
    incl = collections.Counter()
    for s in samples:
        for f in set(s):
            incl[f] += 1
    print(f"\n-- INCLUSIVE (share of samples with the function on the stack), top {a.top}")
    for f, c in incl.most_common(a.top):
        print(f"  {100.0*c/n:6.2f}%  {f}")
    if a.depth:
        # frames are leaf-first; depth d from the entry = s[-1-d]
        tbl = collections.Counter()
        for s in samples:
            if len(s) > a.depth:
                tbl[" <- ".join(reversed(s[-1-a.depth:]))] += 1
            else:
                tbl["(shallower)"] += 1
        print(f"\n-- FRAMES AT DEPTH {a.depth} FROM THE THREAD ENTRY")
        for f, c in tbl.most_common(a.top):
            print(f"  {100.0*c/n:6.2f}%  {f}")
    for parent in a.below:
        tbl = collections.Counter()
        tot = 0
        for s in samples:
            # leaf-first: the frame nearest the leaf whose caller is `parent`
            try:
                i = len(s) - 1 - s[::-1].index(parent)
            except ValueError:
                continue
            tot += 1
            tbl[s[i-1] if i > 0 else "(self)"] += 1
        print(f"\n-- DIRECT CALLEES of {parent} by inclusive share ({tot} samples, {100.0*tot/n:.2f}% of thread)")
        for f, c in tbl.most_common(25):
            print(f"  {100.0*c/n:6.2f}%  {f}")
    for target in a.callers_of:
        callers = collections.Counter()
        tot = 0
        for s in samples:
            if s and s[0] == target:
                tot += 1
                callers[s[1] if len(s) > 1 else "?"] += 1
        print(f"\n-- CALLERS of {target} when it is the LEAF ({tot} samples, {100.0*tot/n:.2f}% of thread)")
        for f, c in callers.most_common(15):
            print(f"  {100.0*c/max(tot,1):6.1f}%  {f}")

if __name__ == "__main__":
    main()
