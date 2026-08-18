#!/usr/bin/env python3
"""Split a hot SYMBOL by SOURCE LINE, at zero cost to the thing being measured.

WHY THIS EXISTS. Part 55's plan opens with item C: `UploadStream` is 12.84% of the pump
thread and it is in no performance plan this project has written, because part 22 closed
the stream cache on the strength of `ProfScope(streams)` reading 0.0% and the symbol says
that scope is not where the cost is. Splitting a phase has found three items in two parts
and reading the code has found none (gotcha 327), so the answer is another split.

BUT THIS ONE CANNOT BE SPLIT WITH ProfScope. `UploadStream`'s hot path is a hash-map
lookup taken ~33,000 times a crowd frame, and a `ProfScope` costs two clock reads at
~20 ns — 1.3 ms a frame, which is larger than several of the phases it would be
separating. An instrument that big does not measure the function, it replaces it
(gotcha 7). The whole point of part 51's move to `perf` was that a symbol profile needs no
instrument at all; this is the same move one level finer, using the DWARF line table the
RelWithDebInfo build already carries.

WHAT IT DOES. Reads a `perf record` flat profile, keeps only the samples inside one
symbol (and, by default, one thread), and folds them by source line — which for an -O2
build means INLINED callees are attributed to their own source lines rather than to the
container, so this splits `UploadStream` into its map lookups, its guard, its copy and
its bookkeeping without touching a line of the runtime.

READ THE DENOMINATOR. `perf` reports every share as a fraction of the WHOLE profile, and
that is not the number an item is priced in (gotcha, and `tools/part53_symbols.py` exists
for the same reason). This prints three columns: share of the whole process, share of the
chosen THREAD, and share of the SYMBOL, and it is the second that converts into
milliseconds of a frame.

Usage:
    tools/part55_srcline.py <perf.data> <symbol> [--tid N] [--top N]
    tools/part55_srcline.py p55_base.flat.perf.data UploadStream --tid 237990
"""
import argparse
import collections
import re
import subprocess
import sys


def read_samples(data, symbol, tid):
    """One row per sample bucket: (period, tid, symbol, srcline).

    `perf script` rather than `perf report`, because the report's own aggregation hides
    which THREAD a row came from once you sort by srcline, and the thread is the
    denominator that matters here.
    """
    # `ip` is not wanted in the output but IS required: without it perf refuses to look
    # up a source line at all ("sample IP is not selected"), which it says on stderr and
    # nowhere else.
    cmd = ["perf", "script", "-i", data, "-F", "tid,period,ip,sym,srcline"]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        # Older perf builds have no `srcline` field in `perf script`. Say so rather than
        # printing an empty table, which reads like "this symbol has no cost".
        sys.exit("perf script failed:\n" + out.stderr.strip())
    return out.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("symbol")
    ap.add_argument("--tid", type=int, default=None,
                    help="restrict to one thread; default is the busiest one in the symbol")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    text = read_samples(args.data, args.symbol, args.tid)

    # `perf script -F tid,period,ip,sym,srcline` emits, per sample, two lines:
    #     <tid> <period> <hex ip> <symbol>
    #       <file>:<line>
    # Parsing is done defensively — a line this tool cannot read is COUNTED and reported,
    # never dropped, because a missing attribution reads as an absent cost (gotcha 25).
    per_thread = collections.Counter()
    sym_lines = collections.Counter()
    sym_thread = collections.Counter()
    total = 0
    unparsed = 0
    cur = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"^(\d+)\s+(\d+)\s+([0-9a-f]+)\s+(.*)$", line)
        if m:
            tid, period, sym = int(m.group(1)), int(m.group(2)), m.group(4).strip()
            total += period
            per_thread[tid] += period
            cur = (tid, period, sym)
            continue
        if cur is not None:
            tid, period, sym = cur
            # SUBSTRING, not equality: perf prints the demangled signature, so
            # `UploadStream` arrives as
            # "(anonymous namespace)::UploadStream(unsigned char*, ...)". Asking for
            # equality here returned "no samples in symbol", which is the shape of
            # failure this project keeps writing down — a grep that cannot match is not
            # a clean result (gotcha 25).
            if args.symbol in sym:
                sym_thread[tid] += period
                sym_lines[(tid, line)] += period
            cur = None
        else:
            unparsed += 1

    if not sym_thread:
        sys.exit(f"no samples matched symbol substring {args.symbol!r} "
                 f"-- list what is there with: perf report -i {args.data} --stdio")

    tid = args.tid if args.tid is not None else sym_thread.most_common(1)[0][0]
    thread_total = per_thread[tid]
    sym_total = sym_thread[tid]

    print(f"{args.data}   symbol {args.symbol}   tid {tid}")
    print(f"  the symbol is {100.0*sym_total/thread_total:.2f}% of that thread "
          f"and {100.0*sym_total/total:.2f}% of the process")
    if unparsed:
        print(f"  !! {unparsed} lines this tool could not parse -- the table below is incomplete")
    print()
    print(f"  {'% of sym':>9} {'% of thr':>9}  source line")
    rows = [(v, k[1]) for k, v in sym_lines.items() if k[0] == tid]
    rows.sort(reverse=True)
    for v, src in rows[: args.top]:
        print(f"  {100.0*v/sym_total:9.2f} {100.0*v/thread_total:9.2f}  {src}")
    shown = sum(v for v, _ in rows[: args.top])
    if shown < sym_total:
        print(f"  {100.0*(sym_total-shown)/sym_total:9.2f} {100.0*(sym_total-shown)/thread_total:9.2f}"
              f"  ...{len(rows)-args.top} further lines")


if __name__ == "__main__":
    main()
