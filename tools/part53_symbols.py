#!/usr/bin/env python3
"""Per-THREAD, per-FUNCTION shares out of a `perf record` file, and the reason it exists.

Part 51 established that `perf record -F 999 -p <pid>` is the instrument that disagrees
with thirty parts of phase profiling (gotcha 340), and part 52 used it to find and price
`BindShader`. Both read it by hand. `perf report --tid=N` does NOT filter the percentage
column the way it looks like it does -- it prints each symbol's share of the WHOLE
profile while showing only that thread's rows -- so every hand reading of it has to
renormalise, and getting that wrong is how a symbol at 26% of one thread gets quoted as
8% of a process nobody is trying to speed up.

So this does three things `perf report` will not:

  1. buckets samples by TID and prints the threads in order of cost, so the pump does not
     have to be identified by guesswork (on this runtime the busiest thread is the
     GUEST's Draw Thread spinning on our ring pointer -- finding 38 -- and the pump is
     usually second);
  2. folds `symbol+0xNNN` rows into one function, because the offsets are an instruction
     histogram and the question here is which FUNCTION costs what;
  3. renormalises to the THREAD, which is the denominator every plan in this project
     prices items against.

`--diff` takes a second perf.data and prints the two arms side by side with the delta,
which is the whole shape of an item A/B: the control arm is the same binary with the
item's env switch flipped, run now (gotcha 51).

Usage:
    tools/part53_symbols.py A.perf.data [--diff B.perf.data] [--tid N] [--top N]
"""
import argparse
import collections
import re
import subprocess
import sys

SYM = re.compile(r"Pu:\s+[0-9a-f]+\s+(.*)$")


def read(path):
    """{tid: {function: cycles}} from one perf.data."""
    out = subprocess.run(["perf", "script", "-i", path],
                         capture_output=True, text=True, errors="replace")
    per = collections.defaultdict(collections.Counter)
    for line in out.stdout.splitlines():
        f = line.split()
        if len(f) < 6:
            continue
        try:
            tid = int(f[1])
            period = int(f[3])
        except ValueError:
            continue
        m = SYM.search(line)
        if not m:
            continue
        sym = m.group(1)
        # `name+0x1c (/path/to.so)` -> `name`. The offset is an instruction histogram and
        # the DSO is noise once the function name is unique enough to act on.
        sym = re.sub(r"\+0x[0-9a-f]+", "", sym)
        # The trailing ` (/path/to/dso)`. The whitespace before the paren is what
        # separates it from a C++ ARGUMENT LIST, which also ends in `)`: without it the
        # greedy match starts at the `(` of "(anonymous namespace)" and eats the entire
        # symbol, which is how the first version of this printed a 74% blank row for the
        # one thread it exists to read.
        sym = re.sub(r"\s+\([^()]*\)$", "", sym).strip()
        # The namespace prefix goes FIRST. Dropping the argument list before it would
        # cut at the `(` of "(anonymous namespace)" and erase the whole symbol -- which
        # is exactly what the first version of this did, and it silently emptied the one
        # thread the script exists to read.
        sym = sym.replace("(anonymous namespace)::", "")
        # C++ signatures are long enough to wrap a terminal; the arguments never
        # disambiguate anything here, so drop them.
        sym = re.sub(r"\(.*", "", sym)
        per[tid][sym] += period
    return per


def thread_order(per):
    return sorted(per, key=lambda t: -sum(per[t].values()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--diff")
    ap.add_argument("--tid", type=int, help="a specific thread; default is every thread "
                                            "holding at least 5%% of the process")
    ap.add_argument("--top", type=int, default=14)
    a = ap.parse_args()

    A = read(a.data)
    B = read(a.diff) if a.diff else None
    if not A:
        sys.exit("no samples parsed -- is that a perf.data with symbols?")

    totalA = sum(sum(c.values()) for c in A.values())
    tids = [a.tid] if a.tid else [t for t in thread_order(A)
                                  if sum(A[t].values()) >= 0.05 * totalA]

    for tid in tids:
        tot = sum(A[tid].values())
        print(f"=== tid {tid}: {100.0 * tot / totalA:.1f}% of the process's cycles")
        if B is None:
            for sym, v in A[tid].most_common(a.top):
                print(f"  {100.0 * v / tot:6.2f}%  {sym}")
            continue
        # The arms are two RUNS, so tids differ. Match the Nth-busiest thread instead,
        # which is stable on this runtime because the thread set is fixed at boot.
        rank = thread_order(A).index(tid)
        border = thread_order(B)
        if rank >= len(border):
            print("  (no matching thread in the diff arm)")
            continue
        btid = border[rank]
        btot = sum(B[btid].values())
        print(f"    vs tid {btid} ({100.0 * btot / sum(sum(c.values()) for c in B.values()):.1f}%)")
        keys = set(A[tid]) | set(B[btid])
        rows = []
        for s in keys:
            pa = 100.0 * A[tid][s] / tot
            pb = 100.0 * B[btid][s] / btot
            rows.append((max(pa, pb), pa, pb, s))
        rows.sort(reverse=True)
        print(f"  {'this':>7} {'other':>7} {'delta':>7}  symbol")
        for _, pa, pb, s in rows[:a.top]:
            print(f"  {pa:6.2f}% {pb:6.2f}% {pa - pb:+6.2f}   {s}")


if __name__ == "__main__":
    main()
